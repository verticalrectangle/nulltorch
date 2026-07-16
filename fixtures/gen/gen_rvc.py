#!/usr/bin/env python3
"""NullTorch RVC fixture / ground-truth generator (torch oracle).

Ground truth for the RVC track mirrors pop-maker-studio's engine job: the
'weight' tensors loaded AS float32 (exact f16->f32 widening via torch.float())
plus the named config parsed positionally from cpt['config'] (+ phone_dim from
enc_p.emb_phone.weight). torch is the oracle for the f32 bytes.

Modes:
  --synth   build a tiny synthetic RVC checkpoint (real 18-arg config, small
            f16 weights incl. enc_p.emb_phone.weight); FULL gt bytes stored.
  --from-pth <model.pth>   ground truth for a real (large) checkpoint;
            HASHES-ONLY gt (f32 output is ~2x the f16 file).

Usage:
  python3 gen_rvc.py --synth --out ../rvc --seed 7001
  python3 gen_rvc.py --from-pth /path/model.pth --out ../rvc_real
"""

import argparse
import hashlib
import os
import sys

import torch

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "..", "grader"))
sys.path.insert(0, os.path.join(_HERE, "..", "..", "reference"))
import schema  # noqa: E402
from rvc_convert import parse_config  # noqa: E402  (positional config mapping)


def f32_bytes(t):
    # numpy buffer copy — bytes(untyped_storage()) falls back to a per-byte
    # Python loop (minutes for 100+ MB). numpy is a torch dep; oracle-side only.
    return t.detach().float().contiguous().numpy().tobytes()


def config_ground_truth(cfg_list, weight):
    cfg = parse_config(cfg_list) if cfg_list is not None else {}
    if cfg and "enc_p.emb_phone.weight" in weight:
        emb = weight["enc_p.emb_phone.weight"]
        if emb.ndim >= 2:
            cfg["phone_dim"] = int(emb.shape[1])
    return cfg


def emit(out, fid, cpt, store_bytes):
    weight = cpt.get("weight")
    if not isinstance(weight, dict):
        raise SystemExit("checkpoint has no 'weight' dict")
    fdir = os.path.join(out, fid)
    os.makedirs(os.path.join(fdir, "gt", "tensors"), exist_ok=True)

    tensors, hashes = {}, {}
    for name, t in weight.items():
        if not (isinstance(name, str) and torch.is_tensor(t)):
            continue
        raw = f32_bytes(t)
        n = t.numel()
        tensors[name] = {"dtype": "f32", "shape": list(t.shape),
                         "nbytes": n * 4}
        bn = name.replace("/", "__") + ".bin"
        hashes[bn] = {"sha256": hashlib.sha256(raw).hexdigest(), "size": len(raw)}
        if store_bytes:
            with open(os.path.join(fdir, "gt", "tensors", bn), "wb") as f:
                f.write(raw)

    manifest = {"nulltorch_manifest": schema.SCHEMA_VERSION,
                "byteorder": "little", "tensors": tensors,
                "config": config_ground_truth(cpt.get("config"), weight)}
    errs = schema.validate_manifest(manifest)
    assert not errs, errs

    with open(os.path.join(fdir, "gt", "manifest.json"), "w") as f:
        f.write(schema.canonical_dumps(manifest))
    with open(os.path.join(fdir, "gt", "hashes.json"), "w") as f:
        f.write(schema.canonical_dumps(hashes))
    with open(os.path.join(fdir, "meta.json"), "w") as f:
        f.write(schema.canonical_dumps({
            "fixture_id": fid, "tier": "RVC", "recipe": "rvc",
            "grading": "byte", "n_tensors": len(tensors),
            "n_config_fields": len(manifest["config"]),
            "gt_bytes_stored": bool(store_bytes)}))
    return len(tensors), len(manifest["config"])


def build_synth(seed):
    torch.manual_seed(seed)
    def h(*s):
        return torch.randn(*s).half()
    # Small tensors — enough to exercise weight-scoping, config, phone_dim
    # (from emb_phone.shape[1]=768), and f16->f32. Kept tiny so the committed
    # fixture is small; real fidelity is validated against a full HF checkpoint.
    weight = {
        "enc_p.emb_phone.weight": h(8, 768),   # shape[1] drives phone_dim=768
        "enc_p.emb_phone.bias": h(8),
        "enc_p.emb_pitch.weight": h(8, 16),
        "dec.conv_pre.weight": h(16, 8, 3),
        "dec.conv_pre.bias": h(16),
        "dec.ups.0.weight": h(16, 8, 4),
        "flow.flows.0.enc.cond_layer.weight": h(12, 8, 1),
        "emb_g.weight": h(1, 32),
    }
    config = [1025, 32, 192, 192, 768, 2, 6, 3, 0, "1",
              [3, 7, 11], [[1, 3, 5], [1, 3, 5], [1, 3, 5]],
              [10, 10, 2, 2], 512, [16, 16, 4, 4], 1, 256, 40000]
    return {"weight": weight, "config": config, "sr": "40k", "f0": 1,
            "version": "v2", "info": "synthetic"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--synth", action="store_true")
    ap.add_argument("--from-pth")
    ap.add_argument("--seed", type=int, default=7001)
    args = ap.parse_args()

    if args.synth:
        cpt = build_synth(args.seed)
        fid = f"rvc_synth__s{args.seed}"
        # also write the actual .pth so the reference converter can be run on it
        fdir = os.path.join(args.out, fid)
        os.makedirs(fdir, exist_ok=True)
        torch.save(cpt, os.path.join(fdir, "fixture.pth"))
        nt, nc = emit(args.out, fid, cpt, store_bytes=True)
        print(f"synth {fid}: {nt} tensors, {nc} config fields, full gt")
    elif args.from_pth:
        cpt = torch.load(args.from_pth, map_location="cpu", weights_only=False)
        base = os.path.basename(os.path.dirname(args.from_pth)) or "model"
        fid = f"rvc_real__{base}"
        nt, nc = emit(args.out, fid, cpt, store_bytes=False)
        print(f"real {fid}: {nt} tensors, {nc} config fields, hashes-only gt")
    else:
        raise SystemExit("pass --synth or --from-pth")


if __name__ == "__main__":
    main()
