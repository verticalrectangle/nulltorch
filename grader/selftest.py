#!/usr/bin/env python3
"""Grader self-test: positive + negative controls.

For a generated fixture set, verify:
  - ground truth submitted as-is PASSES every fixture (positive control)
  - each targeted corruption FAILS with exactly the expected verdict code

Usage: python3 selftest.py <fixture_set_dir>
"""

import copy
import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402
from grade import grade_fixture  # noqa: E402


def has_gt_bytes(fixture_dir):
    return os.path.isdir(os.path.join(fixture_dir, "gt", "tensors"))


def make_submission(fixture_dir, dst):
    """Copy ground truth into submission layout. Hashes-only fixtures (T5 and
    other large-fixture tiers) have no gt bytes to copy — only the manifest."""
    os.makedirs(os.path.join(dst, "tensors"), exist_ok=True)
    shutil.copy(os.path.join(fixture_dir, "gt", "manifest.json"),
                os.path.join(dst, "manifest.json"))
    src_t = os.path.join(fixture_dir, "gt", "tensors")
    if not os.path.isdir(src_t):
        return
    for name in os.listdir(src_t):
        shutil.copy(os.path.join(src_t, name),
                    os.path.join(dst, "tensors", name))


def load_manifest(sub):
    with open(os.path.join(sub, "manifest.json")) as f:
        return json.load(f)


def write_manifest(sub, m):
    with open(os.path.join(sub, "manifest.json"), "w") as f:
        f.write(schema.canonical_dumps(m))


def expect(result, code, label):
    codes = {f["code"] for f in result["failures"]}
    if result["pass"]:
        print(f"  FAIL(control) {label}: unexpectedly passed")
        return False
    if code not in codes:
        print(f"  FAIL(control) {label}: wanted {code}, got {sorted(codes)}")
        return False
    print(f"  ok {label} -> {code}")
    return True


def pick_fixture(set_dir, need_alias=False, need_bytes=False):
    for fid in sorted(os.listdir(set_dir)):
        fdir = os.path.join(set_dir, fid)
        meta_p = os.path.join(fdir, "meta.json")
        if not os.path.isfile(meta_p):
            continue
        with open(meta_p) as f:
            meta = json.load(f)
        if need_bytes and (meta["tier"] == "T1" or not has_gt_bytes(fdir)):
            continue
        m = schema.load_manifest(os.path.join(fdir, "gt", "manifest.json"))
        part = schema.storage_partition(m)
        if need_alias and not any(len(v) > 1 for v in part.values()):
            continue
        return fdir, m
    raise SystemExit("no suitable fixture found")


def main():
    set_dir = sys.argv[1]
    ok = True

    # ── positive control: every fixture, ground truth as submission ──────────
    # (skips hashes-only fixtures — no gt bytes exist to resubmit; those are
    #  validated via the reference converter through the harness instead.)
    n = skipped = 0
    for fid in sorted(os.listdir(set_dir)):
        fdir = os.path.join(set_dir, fid)
        if not os.path.isfile(os.path.join(fdir, "meta.json")):
            continue
        if not has_gt_bytes(fdir):
            skipped += 1
            continue
        with tempfile.TemporaryDirectory() as sub:
            make_submission(fdir, sub)
            r = grade_fixture(fdir, sub)
            if not r["pass"]:
                print(f"  FAIL(positive) {fid}: {r['failures'][:3]}")
                ok = False
            n += 1
    note = f" ({skipped} hashes-only skipped)" if skipped else ""
    print(f"  ok positive control: {n} fixtures pass as ground truth{note}")
    if n == 0:
        print("  (no byte-graded fixtures in set; skipping byte controls)")
        print("SELFTEST " + ("PASS" if ok else "FAIL"))
        sys.exit(0 if ok else 1)

    # ── negative controls ─────────────────────────────────────────────────────
    fdir, _ = pick_fixture(set_dir, need_bytes=True)

    with tempfile.TemporaryDirectory() as sub:   # corrupt one byte
        make_submission(fdir, sub)
        tdir = os.path.join(sub, "tensors")
        target = sorted(os.listdir(tdir))[0]
        p = os.path.join(tdir, target)
        raw = bytearray(open(p, "rb").read())
        if not raw:
            raw = bytearray(b"\x00")  # extend: also a failure (size)
        else:
            raw[len(raw) // 2] ^= 0xFF
        open(p, "wb").write(bytes(raw))
        ok &= expect(grade_fixture(fdir, sub), "BYTES_MISMATCH"
                     if len(raw) == os.path.getsize(
                         os.path.join(fdir, "gt", "tensors", target))
                     else "BYTES_SIZE", "flipped byte")

    with tempfile.TemporaryDirectory() as sub:   # wrong field (stride or shape)
        make_submission(fdir, sub)
        m = load_manifest(sub)
        path = sorted(m["tensors"])[0]
        # RVC manifests omit stride; mutate whichever list field is present.
        field = "stride" if "stride" in m["tensors"][path] else "shape"
        v = m["tensors"][path][field]
        m["tensors"][path][field] = [x + 1 for x in v] or [7]
        write_manifest(sub, m)
        ok &= expect(grade_fixture(fdir, sub), "FIELD_MISMATCH",
                     f"wrong {field}")

    with tempfile.TemporaryDirectory() as sub:   # missing tensor
        make_submission(fdir, sub)
        m = load_manifest(sub)
        path = sorted(m["tensors"])[0]
        del m["tensors"][path]
        write_manifest(sub, m)
        ok &= expect(grade_fixture(fdir, sub), "TENSOR_MISSING",
                     "dropped tensor")

    with tempfile.TemporaryDirectory() as sub:   # extra tensor
        make_submission(fdir, sub)
        m = load_manifest(sub)
        m["tensors"]["phantom"] = copy.deepcopy(
            m["tensors"][sorted(m["tensors"])[0]])
        write_manifest(sub, m)
        ok &= expect(grade_fixture(fdir, sub), "TENSOR_EXTRA", "extra tensor")

    with tempfile.TemporaryDirectory() as sub:   # float where int required
        make_submission(fdir, sub)
        m = load_manifest(sub)
        path = sorted(m["tensors"])[0]
        m["tensors"][path]["storage_offset"] = 0.0
        write_manifest(sub, m)
        ok &= expect(grade_fixture(fdir, sub), "MANIFEST_INVALID",
                     "float offset")

    # broken aliasing on a fixture that has tied storages
    fdir_a, m_a = pick_fixture(set_dir, need_alias=True, need_bytes=True)
    with tempfile.TemporaryDirectory() as sub:
        make_submission(fdir_a, sub)
        m = load_manifest(sub)
        part = schema.storage_partition(m)
        shared = next(v for v in part.values() if len(v) > 1)
        # give one member of a shared group a fresh, unused key
        used = set(part)
        fresh = next(str(i) for i in range(10000) if str(i) not in used)
        m["tensors"][shared[0]]["storage_key"] = fresh
        write_manifest(sub, m)
        ok &= expect(grade_fixture(fdir_a, sub), "ALIASING_MISMATCH",
                     "broken tie")

    with tempfile.TemporaryDirectory() as sub:   # missing bin file
        make_submission(fdir, sub)
        tdir = os.path.join(sub, "tensors")
        os.remove(os.path.join(tdir, sorted(os.listdir(tdir))[0]))
        ok &= expect(grade_fixture(fdir, sub), "BYTES_MISSING", "missing bin")

    print("SELFTEST " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
