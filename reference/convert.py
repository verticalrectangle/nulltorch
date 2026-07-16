#!/usr/bin/env python3
"""NullTorch REFERENCE converter — validation-only.

Reads a .pth and emits the NullTorch output contract:
  <out>/manifest.json
  <out>/tensors/<escaped>.bin   (contiguous, row-major, little-endian)

It exists to (a) prove the fixtures are solvable within the "no torch" rule
and (b) give the harness a real submission to run end-to-end. It uses Python's
stdlib (zipfile + a stubbed pickle.Unpickler) — so it is NOT a valid benchmark
submission (candidates get Go/Rust/C++ with no Python). Language-specific
reference solutions are a separate, future artifact.

Scope: STORED and DEFLATE zip containers, pickle protocol 2 and 4, typed
storages. Unknown user globals are skipped with a warning (module graceful
degradation). fp8/qtensor/legacy-stream are out of scope (see
fixtures/t4/DEFERRED.md).

Usage:
  python3 convert.py <fixture.pth> <out_dir>

Emits a JSONL warning stream on stderr. Exit 0 on success (possibly partial),
2 on a container it refuses to read.
"""

import collections
import io
import json
import os
import pickle
import sys
import zipfile

# dtype token -> itemsize (independent copy; the reference does not import the
# grader's schema, to keep it an honest separate implementation)
DTYPE_ITEMSIZE = {
    "f64": 8, "f32": 4, "f16": 2, "bf16": 2, "f8_e4m3": 1, "f8_e5m2": 1,
    "i64": 8, "i32": 4, "i16": 2, "i8": 1, "u8": 1, "bool": 1,
}
STORAGE_CLASS_DTYPE = {
    "DoubleStorage": "f64", "FloatStorage": "f32", "HalfStorage": "f16",
    "BFloat16Storage": "bf16", "LongStorage": "i64", "IntStorage": "i32",
    "ShortStorage": "i16", "CharStorage": "i8", "ByteStorage": "u8",
    "BoolStorage": "bool",
}


def warn(code, detail):
    sys.stderr.write(json.dumps({"warn": code, "detail": detail}) + "\n")


# ── pickle interpretation (stubs; no torch) ──────────────────────────────────

class StorageRef:
    def __init__(self, cls_name, key, numel):
        self.cls_name, self.key, self.numel = cls_name, key, numel


class TensorStub:
    def __init__(self, storage, offset, size, stride, *_):
        self.storage, self.offset = storage, offset
        self.size, self.stride = tuple(size), tuple(stride)


class _StorageCls:
    def __init__(self, name):
        self.name = name


class _Skip:
    pass


class RefUnpickler(pickle.Unpickler):
    def find_class(self, module, name):
        if module == "torch._utils" and name in (
                "_rebuild_tensor_v2", "_rebuild_tensor_v3"):
            return TensorStub
        if module == "torch._utils" and name == "_rebuild_parameter":
            return lambda data, *a: data
        if module == "collections" and name == "OrderedDict":
            return collections.OrderedDict
        if module == "torch" and name in STORAGE_CLASS_DTYPE:
            return _StorageCls(name)
        warn("UNKNOWN_GLOBAL", f"{module}.{name} -> skipped")
        return lambda *a, **k: _Skip()

    def persistent_load(self, pid):
        if not (isinstance(pid, tuple) and pid and pid[0] == "storage"):
            warn("UNKNOWN_PERSID", repr(pid))
            return _Skip()
        cls, key, _loc, numel = pid[1], pid[2], pid[3], pid[4]
        if not isinstance(cls, _StorageCls):
            warn("UNKNOWN_STORAGE_CLASS", repr(cls))
            return _Skip()
        return StorageRef(cls.name, str(key), numel)


def walk(obj, prefix=""):
    if isinstance(obj, dict):
        for k, v in obj.items():
            if isinstance(k, str) and "/" not in k:
                yield from walk(v, f"{prefix}{k}/")
    elif isinstance(obj, (list, tuple)):
        for i, v in enumerate(obj):
            yield from walk(v, f"{prefix}{i}/")
    else:
        yield (prefix[:-1] if prefix else ""), obj


# ── contiguous materialization (stride-aware gather) ─────────────────────────

def materialize(raw, itemsize, offset, shape, stride):
    n = 1
    for d in shape:
        n *= d
    if n == 0:
        return b""
    # fast path: already C-contiguous with matching offset
    exp, contig = 1, []
    for d in reversed(shape):
        contig.append(exp)
        exp *= d
    contig.reverse()
    if list(stride) == contig and offset == 0 and len(raw) == n * itemsize:
        return raw
    # general gather in row-major order
    out = bytearray(n * itemsize)
    idx = [0] * len(shape)
    for lin in range(n):
        src = offset
        for i, ix in enumerate(idx):
            src += ix * stride[i]
        sb = src * itemsize
        out[lin * itemsize:(lin + 1) * itemsize] = raw[sb:sb + itemsize]
        for i in range(len(shape) - 1, -1, -1):
            idx[i] += 1
            if idx[i] < shape[i]:
                break
            idx[i] = 0
    return bytes(out)


# ── container ────────────────────────────────────────────────────────────────

def read_pth(path):
    try:
        with zipfile.ZipFile(path) as zf:
            names = zf.namelist()
            pkls = [n for n in names if n.endswith("/data.pkl")]
            if len(pkls) != 1:
                warn("NO_DATA_PKL", str(pkls))
                return None
            graph = RefUnpickler(io.BytesIO(zf.read(pkls[0]))).load()
            storages = {n.split("/data/")[1]: zf.read(n)
                        for n in names if "/data/" in n}
        return graph, storages
    except zipfile.BadZipFile as e:
        warn("BAD_ZIP", str(e))
        return None
    except Exception as e:                       # never crash on hostile input
        warn("READ_ERROR", f"{type(e).__name__}: {e}")
        return None


def convert(path, out_dir):
    got = read_pth(path)
    if got is None:
        return 2
    graph, storages = got

    os.makedirs(os.path.join(out_dir, "tensors"), exist_ok=True)
    tensors = {}
    for tpath, leaf in walk(graph):
        if not isinstance(leaf, TensorStub):
            continue
        token = STORAGE_CLASS_DTYPE.get(leaf.storage.cls_name)
        if token is None:
            warn("UNKNOWN_DTYPE", leaf.storage.cls_name)
            continue
        itemsize = DTYPE_ITEMSIZE[token]
        raw = storages.get(leaf.storage.key, b"")
        data = materialize(raw, itemsize, leaf.offset,
                           list(leaf.size), list(leaf.stride))
        n = 1
        for d in leaf.size:
            n *= d
        if len(data) != n * itemsize:
            warn("SIZE_MISMATCH", f"{tpath}: {len(data)} != {n * itemsize}")
            continue
        bin_name = tpath.replace("/", "__") + ".bin"
        with open(os.path.join(out_dir, "tensors", bin_name), "wb") as f:
            f.write(data)
        tensors[tpath] = {
            "dtype": token, "shape": list(leaf.size),
            "stride": list(leaf.stride), "storage_key": leaf.storage.key,
            "storage_offset": leaf.offset, "nbytes": n * itemsize,
        }

    manifest = {"nulltorch_manifest": 1, "byteorder": "little",
                "tensors": tensors}
    with open(os.path.join(out_dir, "manifest.json"), "w") as f:
        json.dump(manifest, f, sort_keys=True, indent=1)
        f.write("\n")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.stderr.write("usage: convert.py <fixture.pth> <out_dir>\n")
        sys.exit(2)
    sys.exit(convert(sys.argv[1], sys.argv[2]))
