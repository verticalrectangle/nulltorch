#!/usr/bin/env python3
"""NullTorch grader — stdlib only, no torch required at eval time.

Grade one fixture:
  python3 grade.py <fixture_dir> <submission_dir> [--json]

Grade a whole set (fixture dirs under <set_dir>, submissions under
<submissions_root>/<fixture_id>):
  python3 grade.py --set <set_dir> <submissions_root> [--json]

A submission directory contains:
  manifest.json
  tensors/<escaped_name>.bin

Verdict codes (stable identifiers, used by controls and the harness):
  PASS
  MANIFEST_INVALID    schema violation (types, internal consistency)
  MANIFEST_MISSING    manifest.json absent/unparseable
  TENSOR_MISSING      tensor in ground truth absent from submission manifest
  TENSOR_EXTRA        tensor in submission absent from ground truth
  FIELD_MISMATCH      dtype/shape/stride/storage_key/storage_offset/nbytes wrong
  ALIASING_MISMATCH   storage-sharing partition differs from ground truth
  BYTES_MISSING       .bin file absent
  BYTES_SIZE          .bin size differs
  BYTES_MISMATCH      .bin sha256 differs

Tier policy: T1 fixtures are graded on manifest only; T2+ grade bytes too.
A fixture passes iff it has zero failures under its tier policy.
"""

import argparse
import hashlib
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402

FIELDS = ("dtype", "shape", "stride", "storage_key", "storage_offset", "nbytes")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest(), h


def grade_fixture(fixture_dir, submission_dir):
    """Return dict: {fixture_id, tier, pass, failures: [{code, detail}]}."""
    failures = []

    with open(os.path.join(fixture_dir, "meta.json")) as f:
        meta = json.load(f)
    tier = meta["tier"]
    grade_bytes = tier != "T1"

    gt = schema.load_manifest(os.path.join(fixture_dir, "gt", "manifest.json"))
    with open(os.path.join(fixture_dir, "gt", "hashes.json")) as f:
        gt_hashes = json.load(f)
    # T5: ground truth stores no full bytes, only per-tensor sha256 + secret
    # sampled sub-range hashes (gt/samples.json). The full sha256 check below
    # is authoritative; sampled ranges are the belt-and-suspenders check that
    # works without materializing ground-truth bytes.
    samples_path = os.path.join(fixture_dir, "gt", "samples.json")
    gt_samples = json.load(open(samples_path)) if os.path.isfile(samples_path) \
        else {}

    sub_manifest_path = os.path.join(submission_dir, "manifest.json")
    try:
        sub = schema.load_manifest(sub_manifest_path)
    except (OSError, json.JSONDecodeError) as e:
        return {"fixture_id": meta["fixture_id"], "tier": tier, "pass": False,
                "failures": [{"code": "MANIFEST_MISSING", "detail": str(e)}]}

    errs = schema.validate_manifest(sub)
    for e in errs:
        failures.append({"code": "MANIFEST_INVALID", "detail": e})

    gt_t = gt["tensors"]
    sub_t = sub.get("tensors", {}) if isinstance(sub, dict) else {}
    if not isinstance(sub_t, dict):
        sub_t = {}

    for path in sorted(set(gt_t) - set(sub_t)):
        failures.append({"code": "TENSOR_MISSING", "detail": path})
    for path in sorted(set(sub_t) - set(gt_t)):
        failures.append({"code": "TENSOR_EXTRA", "detail": path})

    for path in sorted(set(gt_t) & set(sub_t)):
        # Compare only the fields the ground truth provides. General fixtures
        # provide all of FIELDS; an RVC f32 manifest provides just dtype/shape/
        # nbytes (contiguous output has no meaningful source stride/key/offset).
        for field in FIELDS:
            if field not in gt_t[path]:
                continue
            if gt_t[path].get(field) != sub_t[path].get(field):
                failures.append({
                    "code": "FIELD_MISMATCH",
                    "detail": f"{path}.{field}: expected "
                              f"{gt_t[path].get(field)!r}, "
                              f"got {sub_t[path].get(field)!r}"})

    # RVC config block (SynthesizerTrnMsNSFsid args + phone_dim), exact match.
    if "config" in gt:
        gc = gt["config"]
        sc = sub.get("config", {}) if isinstance(sub, dict) else {}
        if not isinstance(sc, dict):
            sc = {}
        for field in schema.RVC_CONFIG_FIELDS:
            if field in gc and gc[field] != sc.get(field):
                failures.append({
                    "code": "CONFIG_MISMATCH",
                    "detail": f"config.{field}: expected {gc[field]!r}, "
                              f"got {sc.get(field)!r}"})

    # Aliasing partition (redundant with exact storage_key equality, but
    # reported separately: it distinguishes "didn't understand sharing" from
    # "off-by-one key naming" in failure analyses).
    if schema.storage_partition(gt) != schema.storage_partition(sub):
        failures.append({"code": "ALIASING_MISMATCH",
                         "detail": "storage-sharing partition differs"})

    if grade_bytes:
        for path in sorted(set(gt_t) & set(sub_t)):
            bin_name = schema.tensor_bin_name(path)
            sub_bin = os.path.join(submission_dir, "tensors", bin_name)
            want = gt_hashes[bin_name]
            if not os.path.isfile(sub_bin):
                failures.append({"code": "BYTES_MISSING", "detail": bin_name})
                continue
            size = os.path.getsize(sub_bin)
            if size != want["size"]:
                failures.append({
                    "code": "BYTES_SIZE",
                    "detail": f"{bin_name}: expected {want['size']}, got {size}"})
                continue
            digest, _ = sha256_file(sub_bin)
            if digest != want["sha256"]:
                failures.append({"code": "BYTES_MISMATCH", "detail": bin_name})
                continue
            for rng in gt_samples.get(bin_name, []):
                with open(sub_bin, "rb") as bf:
                    bf.seek(rng["off"])
                    seg = bf.read(rng["len"])
                if hashlib.sha256(seg).hexdigest() != rng["sha256"]:
                    failures.append({"code": "SAMPLE_MISMATCH",
                                     "detail": f"{bin_name}@{rng['off']}"})

    return {"fixture_id": meta["fixture_id"], "tier": tier,
            "pass": not failures, "failures": failures}


def grade_set(set_dir, submissions_root):
    results = []
    for fid in sorted(os.listdir(set_dir)):
        fdir = os.path.join(set_dir, fid)
        if not os.path.isfile(os.path.join(fdir, "meta.json")):
            continue
        sub = os.path.join(submissions_root, fid)
        if os.path.isdir(sub):
            results.append(grade_fixture(fdir, sub))
        else:
            results.append({"fixture_id": fid, "tier": "?", "pass": False,
                            "failures": [{"code": "MANIFEST_MISSING",
                                          "detail": "no submission dir"}]})
    tiers = {}
    for r in results:
        t = tiers.setdefault(r["tier"], {"pass": 0, "total": 0})
        t["total"] += 1
        t["pass"] += 1 if r["pass"] else 0
    return {"fixtures": results, "by_tier": tiers,
            "pass": sum(1 for r in results if r["pass"]),
            "total": len(results)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a", help="fixture_dir, or set_dir with --set")
    ap.add_argument("b", help="submission_dir, or submissions_root with --set")
    ap.add_argument("--set", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if args.set:
        report = grade_set(args.a, args.b)
        if args.json:
            print(json.dumps(report, indent=1))
        else:
            for r in report["fixtures"]:
                mark = "PASS" if r["pass"] else "FAIL"
                print(f"{mark}  {r['tier']:3s} {r['fixture_id']}")
                for fl in r["failures"][:5]:
                    print(f"      {fl['code']}: {fl['detail']}")
                if len(r["failures"]) > 5:
                    print(f"      … {len(r['failures']) - 5} more")
            print(f"\n{report['pass']}/{report['total']} fixtures pass")
            for tier, t in sorted(report["by_tier"].items()):
                print(f"  {tier}: {t['pass']}/{t['total']}")
        sys.exit(0 if report["pass"] == report["total"] else 1)
    else:
        r = grade_fixture(args.a, args.b)
        if args.json:
            print(json.dumps(r, indent=1))
        else:
            print("PASS" if r["pass"] else "FAIL")
            for fl in r["failures"]:
                print(f"  {fl['code']}: {fl['detail']}")
        sys.exit(0 if r["pass"] else 1)


if __name__ == "__main__":
    main()
