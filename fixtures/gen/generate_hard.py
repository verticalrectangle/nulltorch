#!/usr/bin/env python3
"""NullTorch hard-tier fixture generator — T4 (archaeology) and T6 (adversarial).

T4 fixtures are real checkpoints with unusual-but-valid encodings; they carry
full ground truth and are graded by grade.py exactly like T1-T3.

T6 fixtures are DELIBERATELY malformed or hostile containers. They are NOT
byte-graded; each carries a `t6.json` policy describing the required behavior
(never crash, never hang, never execute/spawn; refuse or degrade gracefully).
Grading is harness-side (watchdog + exec canary). The malicious payloads here
are only ever pickle.dumps()'d (safe) and written to disk — this generator
NEVER pickle.loads()/torch.load()s them. The exec-canary command is benign
(writes a sentinel file the harness looks for); a correct reader never runs it.

Usage:
  python3 generate_hard.py --out-t4 ../t4 --out-t6 ../t6 --seeds 1001,1002
"""

import argparse
import collections
import hashlib
import io
import json
import os
import pickle
import struct
import sys
import zipfile

import torch

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "..", "grader"))
import schema  # noqa: E402
from generate import (RECIPES, TORCH_DTYPE_TOKEN, OracleUnpickler,  # noqa: E402
                      StorageRef, TensorStub, _r, emit_fixture, oracle_read,
                      sha256_file, tensor_bytes, walk)


# ── T4: dtype archaeology (reuse the standard emitter; valid zip/proto2) ───────

def t4_bf16(_):
    return {"w": _r([8, 8], torch.bfloat16), "b": _r([8], torch.bfloat16)}


def t4_f64(_):
    return {"w": _r([6, 6], torch.float64), "s": torch.tensor(2.0,
                                                              dtype=torch.float64)}


def t4_int_family(_):
    return {"i64": _r([4], torch.int64), "i32": _r([4], torch.int32),
            "i16": _r([4], torch.int16), "i8": _r([4], torch.int8),
            "u8": _r([4], torch.uint8), "bool": _r([8], torch.bool)}


def t4_fp8(_):
    if not hasattr(torch, "float8_e4m3fn"):
        return None
    return {"e4m3": _r([16], torch.float32).to(torch.float8_e4m3fn),
            "e5m2": _r([16], torch.float32).to(torch.float8_e5m2)}


T4_DTYPE_RECIPES = [
    ("t4_bf16", t4_bf16), ("t4_f64", t4_f64),
    ("t4_int_family", t4_int_family),
    # DEFERRED: fp8 serializes via torch.storage.UntypedStorage (no legacy
    # typed storage class), so the dtype must be read from the rebuild args,
    # not the storage-class name. That needs a distinct oracle path — tracked
    # in ../t4/DEFERRED.md alongside the legacy pre-1.6 stream. ("t4_fp8", …)
]


# ── T4: container/protocol variants (custom emitters) ─────────────────────────

class _Skip:
    """Sentinel for an unpicklable-by-the-reader user object (module pickle)."""


class LenientOracleUnpickler(OracleUnpickler):
    """Ground-truth oracle for the module-skip fixture: resolves torch
    storage/tensor constructs, turns every other GLOBAL into a skip factory so
    the tensor structure is still recoverable (mirrors a correct reader that
    skips unknown user classes and keeps extracting tensors)."""

    def find_class(self, module, name):
        try:
            return super().find_class(module, name)
        except pickle.UnpicklingError:
            return lambda *a, **k: _Skip()


def _tensor_meta_from_graph(graph):
    tensors = {}
    for path, leaf in walk(graph):
        if isinstance(leaf, torch.Tensor):
            token = TORCH_DTYPE_TOKEN[leaf.dtype]
            n = 1
            for d in leaf.shape:
                n *= d
            tensors[path] = {
                "dtype": token, "shape": list(leaf.shape),
                "stride": list(leaf.stride()),
                "storage_offset": leaf.storage_offset(),
                "_cdata": leaf.untyped_storage()._cdata,
                "nbytes": n * schema.DTYPE_TOKENS[token],
            }
    # assign storage_key per storage identity in first-seen order
    key_of = {}
    for path in tensors:
        cd = tensors[path].pop("_cdata")
        key_of.setdefault(cd, str(len(key_of)))
        tensors[path]["storage_key"] = key_of[cd]
    return tensors


def _emit_from_pth_bytes(out_root, tier, name, seed, graph, raw_bytes,
                         container, protocol, gt_tensors=None):
    fid = f"{name}__s{seed}"
    fdir = os.path.join(out_root, fid)
    os.makedirs(os.path.join(fdir, "gt", "tensors"), exist_ok=True)
    pth = os.path.join(fdir, "fixture.pth")
    with open(pth, "wb") as f:
        f.write(raw_bytes)

    tensors = gt_tensors if gt_tensors is not None \
        else _tensor_meta_from_graph(graph)
    manifest = {"nulltorch_manifest": schema.SCHEMA_VERSION,
                "byteorder": "little", "tensors": tensors}
    assert not schema.validate_manifest(manifest), \
        schema.validate_manifest(manifest)

    hashes = {}
    for path, leaf in walk(graph):
        if not isinstance(leaf, torch.Tensor):
            continue
        raw = tensor_bytes(leaf)
        assert len(raw) == tensors[path]["nbytes"], path
        bn = schema.tensor_bin_name(path)
        with open(os.path.join(fdir, "gt", "tensors", bn), "wb") as f:
            f.write(raw)
        hashes[bn] = {"sha256": hashlib.sha256(raw).hexdigest(),
                      "size": len(raw)}

    with open(os.path.join(fdir, "gt", "manifest.json"), "w") as f:
        f.write(schema.canonical_dumps(manifest))
    with open(os.path.join(fdir, "gt", "hashes.json"), "w") as f:
        f.write(schema.canonical_dumps(hashes))
    with open(os.path.join(fdir, "meta.json"), "w") as f:
        f.write(schema.canonical_dumps({
            "fixture_id": fid, "tier": tier, "recipe": name, "seed": seed,
            "torch_version": torch.__version__, "container": container,
            "pickle_protocol": protocol, "sha256_pth": sha256_file(pth)[0],
            "n_tensors": len(tensors)}))
    return fid, len(tensors)


def _rezip(stock_pth, method):
    with zipfile.ZipFile(stock_pth) as zf:
        names = zf.namelist()
        prefix = [n for n in names if n.endswith("/data.pkl")][0][:-len("/data.pkl")]
        payload = {n: zf.read(n) for n in names}
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method, allowZip64=True) as zf:
        for n in names:
            zf.writestr(n, payload[n])
    return buf.getvalue(), prefix


def _rezip_zip64(stock_pth):
    with zipfile.ZipFile(stock_pth) as zf:
        names = zf.namelist()
        payload = {n: zf.read(n) for n in names}
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED, allowZip64=True) as zf:
        for n in names:                       # force zip64 even for small files
            with zf.open(n, mode="w", force_zip64=True) as f:
                f.write(payload[n])
    return buf.getvalue()


def emit_t4_variants(out_root, seed):
    from generate import t2_nested, t3_tied
    made = []

    # protocol 4 (FRAME/MEMOIZE/STACK_GLOBAL in the stream)
    torch.manual_seed(seed)
    g = t2_nested(seed)
    tmp = os.path.join(out_root, "_tmp_proto4.pth")
    torch.save(g, tmp, pickle_protocol=4)
    raw = open(tmp, "rb").read()
    os.remove(tmp)
    made.append(_emit_from_pth_bytes(out_root, "T4", "t4_proto4", seed, g,
                                     raw, "zip_stored", 4))

    # deflate-rezipped (compression method 8 — Go stdlib free; Rust/C++ hand-roll)
    torch.manual_seed(seed)
    g = t3_tied(seed)
    tmp = os.path.join(out_root, "_tmp_defl.pth")
    torch.save(g, tmp)
    raw, _ = _rezip(tmp, zipfile.ZIP_DEFLATED)
    os.remove(tmp)
    made.append(_emit_from_pth_bytes(out_root, "T4", "t4_rezip_deflate", seed,
                                     g, raw, "zip_deflate", 2))

    # zip64 (small payload, forced 64-bit records)
    torch.manual_seed(seed)
    g = t2_nested(seed)
    tmp = os.path.join(out_root, "_tmp_z64.pth")
    torch.save(g, tmp)
    raw = _rezip_zip64(tmp)
    os.remove(tmp)
    made.append(_emit_from_pth_bytes(out_root, "T4", "t4_zip64", seed, g, raw,
                                     "zip64_stored", 2))

    # module pickle: tensors + an unresolvable user object → graceful skip
    torch.manual_seed(seed)
    g = {"w": _r([5, 5]), "b": _r([5], torch.float16),
         "opt": _UserThing(step=7)}
    tmp = os.path.join(out_root, "_tmp_mod.pth")
    torch.save(g, tmp)
    raw = open(tmp, "rb").read()
    os.remove(tmp)
    with zipfile.ZipFile(io.BytesIO(raw)) as zf:
        pkl = [n for n in zf.namelist() if n.endswith("/data.pkl")][0]
        graph = LenientOracleUnpickler(io.BytesIO(zf.read(pkl))).load()
    gt = {p: v for p, v in _tensor_meta_from_graph_lenient(graph).items()}
    made.append(_emit_from_pth_bytes(out_root, "T4", "t4_module_skip", seed, g,
                                     raw, "zip_stored", 2, gt_tensors=gt))
    return made


class _UserThing:
    """A user optimizer-state-like object; its class won't exist for the
    reader. Uses __reduce__ so only GLOBAL+REDUCE appear (no BUILD state)."""
    def __init__(self, step=0):
        self.step = step

    def __reduce__(self):
        return (_UserThing, (self.step,))


def _tensor_meta_from_graph_lenient(graph):
    tensors = {}
    key_of = {}
    for path, leaf in walk(graph):
        if isinstance(leaf, TensorStub):
            token = schema.STORAGE_CLASS_DTYPE[leaf.storage.cls_name]
            n = 1
            for d in leaf.size:
                n *= d
            key_of.setdefault(leaf.storage.key, leaf.storage.key)
            tensors[path] = {
                "dtype": token, "shape": list(leaf.size),
                "stride": list(leaf.stride), "storage_key": leaf.storage.key,
                "storage_offset": leaf.offset,
                "nbytes": n * schema.DTYPE_TOKENS[token]}
    return tensors


# ── T6: adversarial containers (crafted bytes; never loaded here) ─────────────

def _wrap_pickle_in_zip(pickle_bytes, prefix="fixture"):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:
        zf.writestr(f"{prefix}/data.pkl", pickle_bytes)
    return buf.getvalue()


def _t6_write(out_root, name, raw, policy):
    fdir = os.path.join(out_root, name)
    os.makedirs(fdir, exist_ok=True)
    with open(os.path.join(fdir, "fixture.pth"), "wb") as f:
        f.write(raw)
    with open(os.path.join(fdir, "t6.json"), "w") as f:
        f.write(schema.canonical_dumps(policy))
    with open(os.path.join(fdir, "meta.json"), "w") as f:
        f.write(schema.canonical_dumps({
            "fixture_id": name, "tier": "T6", "recipe": name, "seed": 0,
            "grading": "policy", "container": "adversarial",
            "sha256_pth": hashlib.sha256(raw).hexdigest()}))


def emit_t6(out_root, stock_pth):
    P = {"must_not": ["crash", "hang", "exec", "spawn"]}

    # 1. truncated zip (container cut mid-stream)
    raw = open(stock_pth, "rb").read()
    _t6_write(out_root, "t6_truncated_zip", raw[: len(raw) // 2],
              {**P, "expectation": "refuse_or_partial",
               "note": "central directory is absent; reader must not crash"})

    # 2. valid zip, truncated data.pkl
    with zipfile.ZipFile(stock_pth) as zf:
        names = zf.namelist()
        prefix = [n for n in names if n.endswith("/data.pkl")][0][:-len("/data.pkl")]
        pkl = zf.read([n for n in names if n.endswith("/data.pkl")][0])
    _t6_write(out_root, "t6_truncated_pickle",
              _wrap_pickle_in_zip(pkl[: len(pkl) // 2], prefix),
              {**P, "expectation": "refuse_or_partial",
               "note": "pickle stream ends early"})

    # 3. memo cycle: a list containing itself (EMPTY_LIST, MEMOIZE, GET, APPEND)
    cyclic = b"\x80\x02]\x94h\x00a."
    _t6_write(out_root, "t6_memo_cycle", _wrap_pickle_in_zip(cyclic),
              {**P, "expectation": "refuse_or_empty",
               "note": "self-referential object; walker must not loop forever"})

    # 4. deep nesting: MARK*d + LIST*d (parser must not stack-overflow)
    d = 100000
    deep = b"\x80\x02" + b"(" * d + b"l" * d + b"."
    _t6_write(out_root, "t6_deep_nesting", _wrap_pickle_in_zip(deep),
              {**P, "expectation": "refuse_or_empty", "depth": d,
               "note": "requires iterative or depth-bounded parsing"})

    # 5. allocation bomb: BINBYTES8 claiming 2**40 bytes with no payload
    bomb = b"\x80\x04\x8e" + struct.pack("<Q", 1 << 40) + b"."
    _t6_write(out_root, "t6_alloc_bomb", _wrap_pickle_in_zip(bomb),
              {**P, "expectation": "refuse",
               "note": "declared length must be validated against bytes remaining"})

    # 6. exec attempt: __reduce__ -> os.system(<benign canary>). Constructed by
    #    pickling a reduce object (SAFE: dumps never executes). A correct reader
    #    resolves no such global and never runs it; the canary file must NOT
    #    appear. The command only writes a sentinel the harness checks for.
    canary = "nulltorch_exec_canary"
    exec_stream = pickle.dumps(_ExecCanary(canary), protocol=2)
    assert b"system" in exec_stream  # sanity: the dangerous GLOBAL is present
    _t6_write(out_root, "t6_exec_reduce", _wrap_pickle_in_zip(exec_stream),
              {**P, "expectation": "refuse", "canary": canary,
               "note": "GLOBAL os.system + REDUCE; safe reader must not execute"})

    # 7. corrupt central directory (valid local data, mangled CD signature)
    raw = bytearray(open(stock_pth, "rb").read())
    idx = raw.rfind(b"PK\x01\x02")
    if idx >= 0:
        raw[idx + 1] = 0x00
    _t6_write(out_root, "t6_corrupt_central_dir", bytes(raw),
              {**P, "expectation": "refuse_or_partial",
               "note": "central directory record signature is broken"})

    return 7


class _ExecCanary:
    """Its __reduce__ targets os.system with a BENIGN canary command. We only
    pickle.dumps() it (safe). A safe reader must never resolve/run this."""
    def __init__(self, canary):
        self.canary = canary

    def __reduce__(self):
        import os as _os
        # benign: create a sentinel file if (and only if) a reader executes it
        return (_os.system, (f"touch /tmp/{self.canary} 2>/dev/null || true",))


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-t4", required=True)
    ap.add_argument("--out-t6", required=True)
    ap.add_argument("--seeds", required=True)
    ap.add_argument("--stock", default=os.path.join(
        _HERE, "..", "public", "t3_tied__s1001", "fixture.pth"),
        help="a valid stock .pth used as the basis for T6 corruption")
    args = ap.parse_args()
    seeds = [int(s) for s in args.seeds.split(",")]

    n4 = 0
    for name, builder in T4_DTYPE_RECIPES:
        for seed in seeds:
            torch.manual_seed(seed)
            g = builder(seed)
            if g is None:
                print(f"  skip {name} (dtype unavailable)")
                continue
            fid, nt = emit_fixture(args.out_t4, "T4", name, builder, seed)
            print(f"  T4 {fid}: {nt} tensors")
            n4 += 1
    for seed in seeds:
        for fid, nt in emit_t4_variants(args.out_t4, seed):
            print(f"  T4 {fid}: {nt} tensors")
            n4 += 1

    n6 = emit_t6(args.out_t6, args.stock)
    print(f"emitted {n4} T4 fixtures -> {args.out_t4}")
    print(f"emitted {n6} T6 fixtures -> {args.out_t6}")


if __name__ == "__main__":
    main()
