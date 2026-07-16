#!/usr/bin/env python3
"""NullTorch fixture generator (T1-T3).

Builds seeded state_dict-like graphs, saves them with torch.save (stock zip
container, pickle protocol 2), then extracts ground truth WITHOUT torch
internals: a stdlib pickle.Unpickler with persistent_load/find_class stubs
walks data.pkl to recover exactly what a torchless reader must recover
(storage keys, offsets, shapes, strides, dtypes).  Tensor bytes come from
torch (the oracle for content); structure comes from the pickle stream (the
oracle for layout).  The two are cross-asserted before a fixture is emitted.

Usage:
  python3 generate.py --out ../public --seeds 1001,1002,1003
  python3 generate.py --out ../hidden --seeds 2001,2002,2003
"""

import argparse
import collections
import hashlib
import io
import json
import os
import pickle
import sys
import zipfile

import torch

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "grader"))
import schema  # noqa: E402

# ── dtype maps (torch side; the grader never sees these) ─────────────────────

TORCH_DTYPE_TOKEN = {
    torch.float64: "f64", torch.float32: "f32", torch.float16: "f16",
    torch.bfloat16: "bf16",
    torch.int64: "i64", torch.int32: "i32", torch.int16: "i16",
    torch.int8: "i8", torch.uint8: "u8", torch.bool: "bool",
}
if hasattr(torch, "float8_e4m3fn"):
    TORCH_DTYPE_TOKEN[torch.float8_e4m3fn] = "f8_e4m3"
    TORCH_DTYPE_TOKEN[torch.float8_e5m2] = "f8_e5m2"


# ── graph walk (shared traversal rules: dict keys / seq indices, '/' joined) ──

def walk(obj, prefix=""):
    """Yield (path, leaf) for every leaf in a dict/list/tuple graph."""
    if isinstance(obj, dict):
        for k, v in obj.items():
            assert isinstance(k, str) and "/" not in k, f"bad key {k!r}"
            yield from walk(v, f"{prefix}{k}/")
    elif isinstance(obj, (list, tuple)):
        for i, v in enumerate(obj):
            yield from walk(v, f"{prefix}{i}/")
    else:
        yield prefix[:-1] if prefix else "", obj


# ── stdlib-pickle oracle ──────────────────────────────────────────────────────

class StorageRef:
    def __init__(self, cls_name, key, location, numel):
        self.cls_name, self.key, self.location, self.numel = \
            cls_name, key, location, numel


class TensorStub:
    def __init__(self, storage, offset, size, stride, *_):
        assert isinstance(storage, StorageRef), type(storage)
        self.storage, self.offset = storage, offset
        self.size, self.stride = tuple(size), tuple(stride)


class _StorageCls:
    def __init__(self, name):
        self.name = name


class OracleUnpickler(pickle.Unpickler):
    """Interprets torch's pickle stream with stubs; no torch import needed
    conceptually (we run it in the generator for convenience only)."""

    def find_class(self, module, name):
        if module == "torch._utils" and name in (
                "_rebuild_tensor_v2", "_rebuild_tensor_v3"):
            return TensorStub
        if module == "torch._utils" and name == "_rebuild_parameter":
            return lambda data, *a: data
        if module == "collections" and name == "OrderedDict":
            return collections.OrderedDict
        if module == "torch" and name in schema.STORAGE_CLASS_DTYPE:
            return _StorageCls(name)
        raise pickle.UnpicklingError(
            f"oracle: unexpected global {module}.{name} "
            f"(fixture uses an unsupported construct)")

    def persistent_load(self, pid):
        assert isinstance(pid, tuple) and pid[0] == "storage", pid
        cls, key, location, numel = pid[1], pid[2], pid[3], pid[4]
        assert isinstance(cls, _StorageCls), (
            f"oracle: unexpected storage class repr {cls!r} — "
            f"torch serialization scheme changed; update the oracle")
        return StorageRef(cls.name, key, location, numel)


def oracle_read(pth_path):
    """Return (tensors: {path: dict}, prefix) from the .pth zip via stdlib only."""
    with zipfile.ZipFile(pth_path) as zf:
        names = zf.namelist()
        pkl = [n for n in names if n.endswith("/data.pkl")]
        assert len(pkl) == 1, f"expected one data.pkl, got {pkl}"
        prefix = pkl[0][: -len("/data.pkl")]
        graph = OracleUnpickler(io.BytesIO(zf.read(pkl[0]))).load()

    tensors = {}
    for path, leaf in walk(graph):
        if isinstance(leaf, TensorStub):
            token = schema.STORAGE_CLASS_DTYPE[leaf.storage.cls_name]
            n = 1
            for d in leaf.size:
                n *= d
            tensors[path] = {
                "dtype": token,
                "shape": list(leaf.size),
                "stride": list(leaf.stride),
                "storage_key": leaf.storage.key,
                "storage_offset": leaf.offset,
                "nbytes": n * schema.DTYPE_TOKENS[token],
            }
    return tensors, prefix


# ── ground-truth bytes from torch ─────────────────────────────────────────────

def tensor_bytes(t: torch.Tensor) -> bytes:
    c = t.detach().clone(memory_format=torch.contiguous_format)
    return bytes(c.untyped_storage())


# ── recipes ───────────────────────────────────────────────────────────────────

def _r(shape, dtype=torch.float32):
    if dtype.is_floating_point:
        return torch.randn(shape).to(dtype)
    if dtype == torch.bool:
        return torch.randint(0, 2, shape, dtype=torch.uint8).bool()
    info = torch.iinfo(dtype)
    lo, hi = max(info.min, -1000), min(info.max, 1000)
    return torch.randint(lo, hi + 1, shape, dtype=dtype)


def t1_flat_f32(_):
    return {f"layer{i}.weight": _r([3 + i, 4]) for i in range(5)}


def t1_flat_f16(_):
    d = {f"blk.{i}.w": _r([8, 2 + i], torch.float16) for i in range(4)}
    d["scale"] = _r([1], torch.float16)
    return d


def t2_nested(_):
    return {
        "enc": {"conv.weight": _r([4, 3, 3, 3]), "conv.bias": _r([4])},
        "dec": {"lin.weight": _r([6, 4], torch.float16),
                "lin.bias": _r([6], torch.float16)},
        "step": 1234,                    # non-tensor noise: must be ignored
        "note": "not a tensor",
        "lr": 0.001,
        "flags": [True, None, "x"],
    }


def t2_ordered_meta(_):
    od = collections.OrderedDict()
    od["w1"] = _r([10, 10])
    od["w2"] = _r([10], torch.int64)
    od["_metadata"] = collections.OrderedDict(
        {"": {"version": 1}, "w1": {"version": 2}})
    return od


def t2_rvc_like(_):
    # Echo of the RVC checkpoints the progenitor reader targets:
    # tensors under 'weight', config as a plain python list, scalars alongside.
    return {
        "weight": {
            "enc_p.emb.weight": _r([16, 8], torch.float16),
            "dec.cond.weight": _r([8, 4, 1], torch.float16),
            "dec.cond.bias": _r([8], torch.float16),
        },
        "config": [1025, 32, 192, 192, 768, 2, 6, 3, 0.0,
                   "1", [3, 7, 11], [10, 10, 2, 2], 512, 1],
        "sr": 40000,
        "f0": 1,
        "version": "v2",
        "info": "epoch=100",
    }


def t2_seq_of_tensors(_):
    return {"stack": [_r([2, 2]), _r([3], torch.int32), _r([2, 2])],
            "pair": (_r([5]), _r([5], torch.float16))}


def t3_tied(_):
    emb = _r([12, 6])
    return {"emb.weight": emb, "head.weight": emb, "bias": _r([12])}


def t3_overlap_views(_):
    base = _r([40])
    return {"a": base[0:16], "b": base[8:32], "whole": base}


def t3_transposed(_):
    x = _r([6, 9])
    return {"x_t": x.t(), "x": x, "strided": _r([12, 10])[::3, ::2]}


def t3_offset_reshape(_):
    base = _r([120])
    return {"win": base[37:97].reshape(6, 10),
            "tail": base[100:], "head2d": base[:24].reshape(2, 3, 4)}


def t3_scalar_zero(_):
    return {"scalar": torch.tensor(3.5), "zero_row": _r([0, 7]),
            "empty": _r([0]), "one": _r([1], torch.int64)}


def t3_mixed(_):
    base = _r([64], torch.float16)
    x = _r([8, 8])
    return {
        "tied.a": base, "tied.b": base,
        "view.mid": base[16:48], "trans": x.t(),
        "nested": {"deep": {"w": _r([4, 4], torch.int64)}},
        "cfg": {"n": 4, "name": "mixed"},
    }


RECIPES = [
    ("T1", "t1_flat_f32", t1_flat_f32),
    ("T1", "t1_flat_f16", t1_flat_f16),
    ("T2", "t2_nested", t2_nested),
    ("T2", "t2_ordered_meta", t2_ordered_meta),
    ("T2", "t2_rvc_like", t2_rvc_like),
    ("T2", "t2_seq_of_tensors", t2_seq_of_tensors),
    ("T3", "t3_tied", t3_tied),
    ("T3", "t3_overlap_views", t3_overlap_views),
    ("T3", "t3_transposed", t3_transposed),
    ("T3", "t3_offset_reshape", t3_offset_reshape),
    ("T3", "t3_scalar_zero", t3_scalar_zero),
    ("T3", "t3_mixed", t3_mixed),
]


# ── cross-assertions (oracle vs torch) ────────────────────────────────────────

def cross_check(graph, oracle_tensors, pth_path):
    torch_tensors = {p: v for p, v in walk(graph)
                     if isinstance(v, torch.Tensor)}
    assert set(torch_tensors) == set(oracle_tensors), (
        set(torch_tensors) ^ set(oracle_tensors))

    # per-tensor structural agreement
    ptr_to_paths = {}
    for path, t in torch_tensors.items():
        o = oracle_tensors[path]
        assert o["dtype"] == TORCH_DTYPE_TOKEN[t.dtype], path
        assert o["shape"] == list(t.shape), path
        assert o["stride"] == list(t.stride()), path
        assert o["storage_offset"] == t.storage_offset(), path
        # dedup key: StorageImpl identity (_cdata), same as torch.serialization
        # uses — data_ptr() is NULL for all empty storages and would falsely
        # tie distinct zero-size tensors together.
        ptr_to_paths.setdefault(t.untyped_storage()._cdata,
                                set()).add(path)

    # aliasing partition agreement
    key_to_paths = {}
    for path, o in oracle_tensors.items():
        key_to_paths.setdefault(o["storage_key"], set()).add(path)
    assert (sorted(map(sorted, ptr_to_paths.values()))
            == sorted(map(sorted, key_to_paths.values()))), "aliasing mismatch"

    # round-trip: reload and compare bytes bit-exactly
    loaded = torch.load(pth_path, weights_only=True)
    loaded_tensors = {p: v for p, v in walk(loaded)
                      if isinstance(v, torch.Tensor)}
    assert set(loaded_tensors) == set(torch_tensors)
    for path, t in torch_tensors.items():
        lt = loaded_tensors[path]
        assert lt.dtype == t.dtype and lt.shape == t.shape, path
        assert tensor_bytes(lt) == tensor_bytes(t), f"round-trip bytes: {path}"


# ── emission ──────────────────────────────────────────────────────────────────

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def emit_fixture(out_root, tier, recipe_name, builder, seed):
    fid = f"{recipe_name}__s{seed}"
    fdir = os.path.join(out_root, fid)
    os.makedirs(os.path.join(fdir, "gt", "tensors"), exist_ok=True)

    torch.manual_seed(seed)
    graph = builder(seed)
    pth = os.path.join(fdir, "fixture.pth")
    torch.save(graph, pth)

    oracle_tensors, _prefix = oracle_read(pth)
    cross_check(graph, oracle_tensors, pth)

    manifest = {"nulltorch_manifest": schema.SCHEMA_VERSION,
                "byteorder": "little", "tensors": oracle_tensors}
    errs = schema.validate_manifest(manifest)
    assert not errs, errs

    hashes = {}
    torch_tensors = {p: v for p, v in walk(graph)
                     if isinstance(v, torch.Tensor)}
    for path, t in torch_tensors.items():
        raw = tensor_bytes(t)
        assert len(raw) == oracle_tensors[path]["nbytes"], path
        bin_name = schema.tensor_bin_name(path)
        with open(os.path.join(fdir, "gt", "tensors", bin_name), "wb") as f:
            f.write(raw)
        hashes[bin_name] = {"sha256": hashlib.sha256(raw).hexdigest(),
                            "size": len(raw)}

    with open(os.path.join(fdir, "gt", "manifest.json"), "w") as f:
        f.write(schema.canonical_dumps(manifest))
    with open(os.path.join(fdir, "gt", "hashes.json"), "w") as f:
        f.write(schema.canonical_dumps(hashes))
    with open(os.path.join(fdir, "meta.json"), "w") as f:
        f.write(schema.canonical_dumps({
            "fixture_id": fid, "tier": tier, "recipe": recipe_name,
            "seed": seed, "torch_version": torch.__version__,
            "container": "zip_stored", "pickle_protocol": 2,
            "sha256_pth": sha256_file(pth),
            "n_tensors": len(oracle_tensors),
        }))
    return fid, len(oracle_tensors)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--seeds", required=True,
                    help="comma-separated, e.g. 1001,1002,1003")
    args = ap.parse_args()
    seeds = [int(s) for s in args.seeds.split(",")]

    total = 0
    for tier, name, builder in RECIPES:
        for seed in seeds:
            fid, n = emit_fixture(args.out, tier, name, builder, seed)
            print(f"  {tier} {fid}: {n} tensors")
            total += 1
    print(f"emitted {total} fixtures to {args.out} (torch {torch.__version__})")


if __name__ == "__main__":
    main()
