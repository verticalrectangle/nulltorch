#!/usr/bin/env python3
"""NullTorch REFERENCE RVC converter — validation-only.

Mirrors what pop-maker-studio's src/pth_reader.cpp does for RVC checkpoints:
  - scope tensors to the 'weight' dict (fallbacks: 'model', or a bare
    state_dict),
  - parse cpt['config'] (18 positional SynthesizerTrnMsNSFsid args) into a
    named config, plus phone_dim inferred from enc_p.emb_phone.weight shape[1],
  - load every weight tensor as float32 (exact IEEE f16->f32 widening), the
    dtype the engine feeds to ONNX.

Output contract (RVC manifest):
  <out>/manifest.json  = {nulltorch_manifest, byteorder, config:{...},
                          tensors:{name:{dtype:"f32", shape, nbytes}}}
  <out>/tensors/<escaped>.bin  = contiguous f32 little-endian bytes

Stdlib only (struct does f16<->f32). Like convert.py it is NOT a valid
benchmark submission (Python); it validates the RVC track and cross-checks
against the engine.

Usage: python3 rvc_convert.py <model.pth> <out_dir>
"""

import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from convert import (DTYPE_ITEMSIZE, STORAGE_CLASS_DTYPE,  # noqa: E402
                     TensorStub, materialize, read_pth, warn)

CHUNK = 1 << 20  # elements per struct batch (bounds memory on big tensors)


def to_f32_bytes(raw, src_token):
    """Convert contiguous source bytes to contiguous little-endian f32 bytes.
    RVC weights are f16; f32 passes through; other floats handled for safety."""
    if src_token == "f32":
        return raw
    code = {"f16": "e", "f64": "d", "bf16": None}.get(src_token)
    if code is None:
        # integer/bool or bf16: element-wise via Python int/float
        isz = DTYPE_ITEMSIZE[src_token]
        n = len(raw) // isz
        out = bytearray(4 * n)
        for i in range(n):
            seg = raw[i * isz:(i + 1) * isz]
            if src_token == "bf16":                    # bf16 -> f32: high 16 bits
                val = struct.unpack("<f", b"\x00\x00" + seg)[0]
            elif src_token in ("i64", "i32", "i16", "i8"):
                val = float(int.from_bytes(seg, "little", signed=True))
            else:                                       # u8 / bool
                val = float(seg[0])
            struct.pack_into("<f", out, 4 * i, val)
        return bytes(out)
    isz = DTYPE_ITEMSIZE[src_token]
    n = len(raw) // isz
    out = bytearray(4 * n)
    o = 0
    for base in range(0, n, CHUNK):
        m = min(CHUNK, n - base)
        vals = struct.unpack(f"<{m}{code}", raw[base * isz:(base + m) * isz])
        struct.pack_into(f"<{m}f", out, o, *vals)
        o += 4 * m
    return bytes(out)


def parse_config(cfg):
    """18 positional args -> named RVC config (mirrors pth_reader parse_config)."""
    def ig(i):
        return int(cfg[i]) if i < len(cfg) and isinstance(cfg[i], (int, float)) else 0

    def sg(i):
        return str(cfg[i]) if i < len(cfg) else ""

    def lg(i):
        return [int(x) for x in cfg[i]] if i < len(cfg) and isinstance(
            cfg[i], (list, tuple)) else []

    if not isinstance(cfg, (list, tuple)) or len(cfg) < 18:
        return {}
    ddl = []
    if isinstance(cfg[11], (list, tuple)):
        ddl = [[int(y) for y in sub] for sub in cfg[11]
               if isinstance(sub, (list, tuple))]
    return {
        "spec_channels": ig(0), "segment_size": ig(1),
        "inter_channels": ig(2), "hidden_channels": ig(3),
        "filter_channels": ig(4), "n_heads": ig(5), "n_layers": ig(6),
        "kernel_size": ig(7), "p_dropout": float(ig(8)), "resblock": sg(9),
        "resblock_kernel_sizes": lg(10), "resblock_dilation_sizes": ddl,
        "upsample_rates": lg(12), "upsample_initial_channel": ig(13),
        "upsample_kernel_sizes": lg(14), "n_speakers": ig(15),
        "gin_channels": ig(16), "sr": ig(17),
    }


def find_weight_and_config(graph):
    weight = config = None
    if isinstance(graph, dict):
        weight = graph.get("weight")
        config = graph.get("config")
        if weight is None:
            weight = graph.get("model")                 # fairseq/HuBERT
        if weight is None and any(isinstance(v, TensorStub)
                                  for v in graph.values()):
            weight = graph                              # bare state_dict
    return weight, config


def convert(path, out_dir):
    got = read_pth(path)
    if got is None:
        return 2
    graph, storages = got
    weight, config_list = find_weight_and_config(graph)
    if not isinstance(weight, dict):
        warn("NO_WEIGHT_DICT", "no 'weight'/'model'/bare state_dict found")
        return 2

    os.makedirs(os.path.join(out_dir, "tensors"), exist_ok=True)
    tensors = {}
    for name, leaf in weight.items():
        if not (isinstance(name, str) and isinstance(leaf, TensorStub)):
            continue
        token = STORAGE_CLASS_DTYPE.get(leaf.storage.cls_name)
        if token is None:
            warn("UNKNOWN_DTYPE", f"{name}: {leaf.storage.cls_name}")
            continue
        isz = DTYPE_ITEMSIZE[token]
        src = materialize(storages.get(leaf.storage.key, b""), isz,
                          leaf.offset, list(leaf.size), list(leaf.stride))
        f32 = to_f32_bytes(src, token)
        n = 1
        for d in leaf.size:
            n *= d
        if len(f32) != n * 4:
            warn("SIZE_MISMATCH", name)
            continue
        with open(os.path.join(out_dir, "tensors",
                               name.replace("/", "__") + ".bin"), "wb") as f:
            f.write(f32)
        tensors[name] = {"dtype": "f32", "shape": list(leaf.size),
                         "nbytes": n * 4}

    config = parse_config(config_list) if config_list is not None else {}
    if config:
        emb = weight.get("enc_p.emb_phone.weight")
        if isinstance(emb, TensorStub) and len(emb.size) >= 2:
            config["phone_dim"] = int(emb.size[1])

    manifest = {"nulltorch_manifest": 1, "byteorder": "little",
                "tensors": tensors}
    if config:
        manifest["config"] = config
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, sort_keys=True, indent=1)
        f.write("\n")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.stderr.write("usage: rvc_convert.py <model.pth> <out_dir>\n")
        sys.exit(2)
    sys.exit(convert(sys.argv[1], sys.argv[2]))
