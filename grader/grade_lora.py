#!/usr/bin/env python3
"""NullTorch grader for the LoRA-merge tier.

Compares a submission's output directory against the fixture's ground truth:
  - merged_model.safetensors must be byte-identical to gt/merged_model.safetensors
  - warnings.jsonl must contain the expected distractor warning(s)

Usage (per fixture):
  python3 grader/grade_lora.py <fixture_dir> <submission_dir>

Usage (set):
  python3 grader/grade_lora.py --set <fixtures_root> <submissions_root>
"""

import argparse
import json
import os
import sys


def grade_fixture(fixture_dir: str, submission_dir: str):
    meta_path = os.path.join(fixture_dir, "meta.json")
    with open(meta_path, "r", encoding="utf-8") as f:
        meta = json.load(f)
    gt_merged = os.path.join(fixture_dir, "gt", "merged_model.safetensors")
    sub_merged = os.path.join(submission_dir, "merged_model.safetensors")
    failures = []

    if not os.path.isfile(sub_merged):
        failures.append({"code": "OUTPUT_MISSING", "detail": "merged_model.safetensors not found in submission"})
    elif not os.path.isfile(gt_merged):
        failures.append({"code": "GT_MISSING", "detail": "ground-truth merged_model.safetensors missing"})
    else:
        want_size = os.path.getsize(gt_merged)
        got_size = os.path.getsize(sub_merged)
        if want_size != got_size:
            failures.append({"code": "SIZE_MISMATCH", "detail": f"merged size: expected {want_size}, got {got_size}"})
        else:
            with open(gt_merged, "rb") as f:
                want = f.read()
            with open(sub_merged, "rb") as f:
                got = f.read()
            if want != got:
                failures.append({"code": "BYTES_MISMATCH", "detail": "merged_model.safetensors differs from ground truth"})

    # warnings.jsonl checks
    expected = meta.get("expected_warnings", [])
    sub_warn = os.path.join(submission_dir, "warnings.jsonl")
    if expected:
        if not os.path.isfile(sub_warn):
            failures.append({"code": "WARNINGS_MISSING", "detail": "warnings.jsonl expected but not found"})
        else:
            with open(sub_warn, "r", encoding="utf-8") as f:
                text = f.read()
            for w in expected:
                if w.get("target") and w["target"] not in text:
                    failures.append({"code": "WARNING_MISSING", "detail": f"missing warning for target {w['target']}"})

    return {"fixture_id": meta["fixture_id"], "tier": meta["tier"], "pass": not failures, "failures": failures}


def grade_set(fixtures_root: str, submissions_root: str):
    results = []
    for fid in sorted(os.listdir(fixtures_root)):
        fdir = os.path.join(fixtures_root, fid)
        if not os.path.isfile(os.path.join(fdir, "meta.json")):
            continue
        sdir = os.path.join(submissions_root, fid)
        if os.path.isdir(sdir):
            results.append(grade_fixture(fdir, sdir))
        else:
            results.append({"fixture_id": fid, "tier": "L1", "pass": False, "failures": [{"code": "NO_SUBMISSION", "detail": "submission dir missing"}]})

    tiers = {}
    for r in results:
        t = tiers.setdefault(r["tier"], {"pass": 0, "total": 0})
        t["total"] += 1
        t["pass"] += 1 if r["pass"] else 0
    return {"fixtures": results, "by_tier": tiers, "pass": sum(1 for r in results if r["pass"]), "total": len(results)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a", help="fixture_dir or fixtures_root with --set")
    ap.add_argument("b", help="submission_dir or submissions_root with --set")
    ap.add_argument("--set", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if args.set:
        report = grade_set(args.a, args.b)
        if args.json:
            print(json.dumps(report, indent=2))
        else:
            for r in report["fixtures"]:
                mark = "PASS" if r["pass"] else "FAIL"
                print(f"{mark}  {r['tier']:3s} {r['fixture_id']}")
                for fl in r["failures"]:
                    print(f"      {fl['code']}: {fl['detail']}")
            print(f"\n{report['pass']}/{report['total']} fixtures pass")
            for tier, t in sorted(report["by_tier"].items()):
                print(f"  {tier}: {t['pass']}/{t['total']}")
        sys.exit(0 if report["pass"] == report["total"] else 1)
    else:
        r = grade_fixture(args.a, args.b)
        if args.json:
            print(json.dumps(r, indent=2))
        else:
            print("PASS" if r["pass"] else "FAIL")
            for fl in r["failures"]:
                print(f"  {fl['code']}: {fl['detail']}")
        sys.exit(0 if r["pass"] else 1)


if __name__ == "__main__":
    main()
