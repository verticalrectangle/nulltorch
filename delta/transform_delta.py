#!/usr/bin/env python3
"""Transform stock NullTorch fixtures into the delta variant (DELTA_SPEC.md).

Approach: semantic re-encode, not byte surgery. Each stock fixture.pth is
read with the generator's stub unpickler (structure preserved exactly:
storage keys, offsets, shapes, strides), then re-pickled with a delta
encoder (swapped pid tuple, class-as-string, Vault names) and re-containered
as a STORED zip whose structural signatures are patched PK->DZ at parsed
offsets (never a blind byte replace — tensor payloads could contain 'PK').

Ground truth is UNCHANGED by the delta (same tensors, same keys, same
bytes), so gt/ is copied from the stock fixture. Verification: an inverse
delta reader re-parses the emitted file and its manifest must equal the
stock ground truth exactly; storage record payloads must be byte-identical.

Usage:
  python3 transform_delta.py <stock_set_dir> <delta_set_dir>
"""

import collections
import io
import json
import os
import pickle
import shutil
import struct
import sys
import zipfile

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "fixtures", "gen"))
sys.path.insert(0, os.path.join(_HERE, "..", "grader"))

import torch  # noqa: E402  (generator-side only; eval env never runs this)
import schema  # noqa: E402
from generate import (OracleUnpickler, StorageRef, TensorStub,  # noqa: E402
                      oracle_read, walk)


# ── delta pickling ────────────────────────────────────────────────────────────

def _vault_name(storage_cls_name: str) -> str:
    assert storage_cls_name.endswith("Storage"), storage_cls_name
    return storage_cls_name[: -len("Storage")] + "Vault"


def _unvault_name(vault: str) -> str:
    assert vault.endswith("Vault"), vault
    return vault[: -len("Vault")] + "Storage"


class DeltaPickler(pickle.Pickler):
    def persistent_id(self, obj):
        if isinstance(obj, StorageRef):
            # DELTA: ('storage', key, class-as-string Vault, location, numel)
            return ("storage", obj.key, _vault_name(obj.cls_name),
                    obj.location, obj.numel)
        return None


def _reduce_tensor_stub(stub: TensorStub):
    return (torch._utils._rebuild_tensor_v2,
            (stub.storage, stub.offset, tuple(stub.size), tuple(stub.stride),
             False, collections.OrderedDict()))


def delta_pickle_bytes(graph) -> bytes:
    buf = io.BytesIO()
    p = DeltaPickler(buf, protocol=2)
    p.dispatch_table = {TensorStub: _reduce_tensor_stub}
    p.dump(graph)
    return buf.getvalue()


# ── delta container ───────────────────────────────────────────────────────────

SIG_LOCAL, SIG_CENTRAL, SIG_EOCD = b"PK\x03\x04", b"PK\x01\x02", b"PK\x05\x06"


def patch_signatures(raw: bytearray) -> bytearray:
    """PK->DZ at structural offsets only, found by parsing the archive we
    just wrote (no comment, no zip64 at these sizes)."""
    assert raw[-22:-18] == SIG_EOCD, "EOCD not at expected position"
    eocd_off = len(raw) - 22
    n_records, cd_size, cd_off = struct.unpack("<H", raw[eocd_off + 10:eocd_off + 12])[0], \
        struct.unpack("<I", raw[eocd_off + 12:eocd_off + 16])[0], \
        struct.unpack("<I", raw[eocd_off + 16:eocd_off + 20])[0]

    local_offsets = []
    pos = cd_off
    for _ in range(n_records):
        assert raw[pos:pos + 4] == SIG_CENTRAL, f"bad CD sig at {pos}"
        fn_len, ex_len, cm_len = struct.unpack("<HHH", raw[pos + 28:pos + 34])
        local_offsets.append(struct.unpack("<I", raw[pos + 42:pos + 46])[0])
        raw[pos:pos + 2] = b"DZ"
        pos += 46 + fn_len + ex_len + cm_len
    assert pos == cd_off + cd_size, "central directory walk mismatch"

    for off in local_offsets:
        assert raw[off:off + 4] == SIG_LOCAL, f"bad local sig at {off}"
        raw[off:off + 2] = b"DZ"
    raw[eocd_off:eocd_off + 2] = b"DZ"
    return raw


def unpatch_signatures(raw: bytes) -> bytes:
    """Inverse of patch_signatures for the verifying reader."""
    b = bytearray(raw)
    assert b[-22:-18] == b"DZ\x05\x06", "delta EOCD missing"
    eocd_off = len(b) - 22
    b[eocd_off:eocd_off + 2] = b"PK"
    n_records = struct.unpack("<H", b[eocd_off + 10:eocd_off + 12])[0]
    cd_off = struct.unpack("<I", b[eocd_off + 16:eocd_off + 20])[0]
    pos = cd_off
    locals_ = []
    for _ in range(n_records):
        assert b[pos:pos + 4] == b"DZ\x01\x02", f"bad delta CD sig at {pos}"
        b[pos:pos + 2] = b"PK"
        fn_len, ex_len, cm_len = struct.unpack("<HHH", b[pos + 28:pos + 34])
        locals_.append(struct.unpack("<I", b[pos + 42:pos + 46])[0])
        pos += 46 + fn_len + ex_len + cm_len
    for off in locals_:
        assert b[off:off + 4] == b"DZ\x03\x04", f"bad delta local sig at {off}"
        b[off:off + 2] = b"PK"
    return bytes(b)


# ── verifying delta reader (proves DELTA_SPEC is implementable) ───────────────

class DeltaVerifyUnpickler(OracleUnpickler):
    def persistent_load(self, pid):
        assert isinstance(pid, tuple) and pid[0] == "storage", pid
        key, vault, location, numel = pid[1], pid[2], pid[3], pid[4]
        assert isinstance(vault, str), "delta pid class must be a string"
        return StorageRef(_unvault_name(vault), key, location, numel)


def delta_read_manifest(delta_pth: str):
    raw = unpatch_signatures(open(delta_pth, "rb").read())
    with zipfile.ZipFile(io.BytesIO(raw)) as zf:
        pkl = [n for n in zf.namelist() if n.endswith("/data.pkl")]
        assert len(pkl) == 1
        graph = DeltaVerifyUnpickler(io.BytesIO(zf.read(pkl[0]))).load()
        storages = {n.split("/data/")[1]: zf.read(n)
                    for n in zf.namelist() if "/data/" in n}
    tensors = {}
    for path, leaf in walk(graph):
        if isinstance(leaf, TensorStub):
            token = schema.STORAGE_CLASS_DTYPE[leaf.storage.cls_name]
            n = 1
            for d in leaf.size:
                n *= d
            tensors[path] = {
                "dtype": token, "shape": list(leaf.size),
                "stride": list(leaf.stride),
                "storage_key": leaf.storage.key,
                "storage_offset": leaf.offset,
                "nbytes": n * schema.DTYPE_TOKENS[token],
            }
    return tensors, storages


# ── transform one fixture ─────────────────────────────────────────────────────

def transform_fixture(stock_dir: str, delta_dir: str):
    stock_pth = os.path.join(stock_dir, "fixture.pth")

    # 1. structure via the stock oracle, storage payloads via zipfile
    with zipfile.ZipFile(stock_pth) as zf:
        pkl_name = [n for n in zf.namelist() if n.endswith("/data.pkl")][0]
        prefix = pkl_name[: -len("/data.pkl")]
        graph = OracleUnpickler(io.BytesIO(zf.read(pkl_name))).load()
        stock_storages = {n.split("/data/")[1]: zf.read(n)
                          for n in zf.namelist() if "/data/" in n}

    # 2. delta re-encode
    dpkl = delta_pickle_bytes(graph)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_STORED) as zf:
        zf.writestr(f"{prefix}/data.pkl", dpkl)
        for key in sorted(stock_storages, key=lambda k: (len(k), k)):
            zf.writestr(f"{prefix}/data/{key}", stock_storages[key])
    delta_raw = patch_signatures(bytearray(buf.getvalue()))

    os.makedirs(delta_dir, exist_ok=True)
    delta_pth = os.path.join(delta_dir, "fixture.pth")
    with open(delta_pth, "wb") as f:
        f.write(delta_raw)

    # 3. verify: independent delta read == stock ground truth, payloads equal
    got_tensors, got_storages = delta_read_manifest(delta_pth)
    gt = schema.load_manifest(os.path.join(stock_dir, "gt", "manifest.json"))
    assert got_tensors == gt["tensors"], "delta read != stock ground truth"
    assert got_storages == stock_storages, "storage payloads changed"

    # sanity: torch itself must NOT be able to read the delta file
    try:
        torch.load(delta_pth, weights_only=True)
        raise AssertionError("torch.load unexpectedly read a delta file")
    except AssertionError:
        raise
    except Exception:
        pass  # expected

    # 4. gt is unchanged: copy; meta marks the variant
    shutil.copytree(os.path.join(stock_dir, "gt"),
                    os.path.join(delta_dir, "gt"), dirs_exist_ok=True)
    with open(os.path.join(stock_dir, "meta.json")) as f:
        meta = json.load(f)
    meta["container"] = "zip_stored_delta"
    meta["variant"] = "delta"
    meta["sha256_pth"] = __import__("hashlib").sha256(
        bytes(delta_raw)).hexdigest()
    with open(os.path.join(delta_dir, "meta.json"), "w") as f:
        f.write(schema.canonical_dumps(meta))


def main():
    stock_set, delta_set = sys.argv[1], sys.argv[2]
    n = 0
    for fid in sorted(os.listdir(stock_set)):
        sdir = os.path.join(stock_set, fid)
        if not os.path.isfile(os.path.join(sdir, "meta.json")):
            continue
        transform_fixture(sdir, os.path.join(delta_set, fid))
        n += 1
    print(f"transformed {n} fixtures -> {delta_set}")


if __name__ == "__main__":
    main()
