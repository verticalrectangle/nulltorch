#!/usr/bin/env python3
"""NullTorch harness runner (lean).

Runs a submission command over a fixture set, grades it, and emits a
results.json conforming to board/schema/results.schema.json.

Per fixture:
  - byte-graded tiers (T1-T5): run `cmd` (template with {pth} {out}), grade
    with grader/grade.py, record pass + category; run twice to check
    determinism.
  - policy tier (T6): run under a timeout with an exec canary; pass iff no
    incident (no crash, no hang, no code execution). Correctness of partial
    output is not checked in this lean runner.

Enforcement note: full isolation (netns, seccomp no-exec, cgroup mem cap) is
the container's job (harness/PROTOCOL.md + containers/). This runner adds the
portable pieces: wall-timeout watchdog, T6 exec-canary detection, peak child
RSS, and the determinism double-run. It does NOT sandbox the command, so run
only trusted submissions outside a container.

Usage:
  python3 harness/run.py \
     --fixtures fixtures/public,fixtures/t4,fixtures/t6 \
     --cmd 'python3 reference/convert.py {pth} {out}' \
     --model-id reference-py --language cpp --condition open_book \
     --out results.json
"""

import argparse
import hashlib
import json
import os
import resource
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
sys.path.insert(0, os.path.join(_ROOT, "grader"))
import grade as grader  # noqa: E402
import schema  # noqa: E402

TIERS = ["T1", "T2", "T3", "T4", "T5", "T6"]


def child_peak_rss():
    return resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss * 1024


def run_cmd(cmd_tpl, pth, out_dir, timeout):
    """Return (returncode, timed_out). Output goes to out_dir."""
    os.makedirs(out_dir, exist_ok=True)
    argv = shlex.split(cmd_tpl.format(pth=shlex.quote(pth),
                                      out=shlex.quote(out_dir)))
    try:
        p = subprocess.run(argv, cwd=_ROOT, timeout=timeout,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return p.returncode, False
    except subprocess.TimeoutExpired:
        return None, True
    except FileNotFoundError as e:
        raise SystemExit(f"submission command not runnable: {e}")


def outputs_equal(a, b):
    ma, mb = os.path.join(a, "manifest.json"), os.path.join(b, "manifest.json")
    if not (os.path.isfile(ma) and os.path.isfile(mb)):
        return os.path.isfile(ma) == os.path.isfile(mb)
    if open(ma, "rb").read() != open(mb, "rb").read():
        return False
    ta, tb = os.path.join(a, "tensors"), os.path.join(b, "tensors")
    fa = sorted(os.listdir(ta)) if os.path.isdir(ta) else []
    fb = sorted(os.listdir(tb)) if os.path.isdir(tb) else []
    if fa != fb:
        return False
    for n in fa:
        if open(os.path.join(ta, n), "rb").read() != \
           open(os.path.join(tb, n), "rb").read():
            return False
    return True


def grade_byte_fixture(fdir, cmd_tpl, timeout, tmp):
    out1 = os.path.join(tmp, "o1")
    rc, to = run_cmd(cmd_tpl, os.path.join(fdir, "fixture.pth"), out1, timeout)
    if to:
        return {"pass": False, "reason": "timeout"}
    res = grader.grade_fixture(fdir, out1)
    # determinism: second run must be byte-identical
    out2 = os.path.join(tmp, "o2")
    run_cmd(cmd_tpl, os.path.join(fdir, "fixture.pth"), out2, timeout)
    res["determinism_ok"] = outputs_equal(out1, out2)
    shutil.rmtree(out1, ignore_errors=True)
    shutil.rmtree(out2, ignore_errors=True)
    return res


def grade_policy_fixture(fdir, cmd_tpl, timeout, tmp):
    """T6: return dict with pass + incident flags."""
    with open(os.path.join(fdir, "t6.json")) as f:
        policy = json.load(f)
    canary = policy.get("canary")
    canary_path = os.path.join(tempfile.gettempdir(), canary) if canary else None
    if canary_path and os.path.exists(canary_path):
        os.remove(canary_path)

    out = os.path.join(tmp, "t6out")
    rc, to = run_cmd(cmd_tpl, os.path.join(fdir, "fixture.pth"), out, timeout)

    executed = bool(canary_path and os.path.exists(canary_path))
    if executed:
        os.remove(canary_path)
    crashed = (rc is not None and rc < 0)      # killed by signal
    hung = to
    incident = executed or crashed or hung
    shutil.rmtree(out, ignore_errors=True)
    return {"pass": not incident, "exec": executed, "crash": crashed,
            "hang": hung}


def fixture_set_hash(roots):
    h = hashlib.sha256()
    entries = []
    for root in roots:
        for fid in sorted(os.listdir(root)):
            mp = os.path.join(root, fid, "meta.json")
            if os.path.isfile(mp):
                m = json.load(open(mp))
                entries.append((m["fixture_id"], m.get("sha256_pth", "")))
    for fid, sha in sorted(entries):
        h.update(f"{fid}:{sha}\n".encode())
    return "sha256:" + h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixtures", required=True, help="comma-separated dirs")
    ap.add_argument("--cmd", required=True, help="template with {pth} {out}")
    ap.add_argument("--rvc-cmd", default="",
                    help="template for RVC-tier fixtures (engine-fidelity "
                         "track: config + f16->f32). Falls back to --cmd.")
    ap.add_argument("--model-id", required=True)
    ap.add_argument("--language", required=True,
                    choices=["go", "rust", "cpp"])
    ap.add_argument("--condition", required=True,
                    choices=["open_book", "closed_book", "delta"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--budget-wall", type=float, default=0.0)
    ap.add_argument("--budget-tokens", type=int, default=0)
    args = ap.parse_args()

    roots = [r for r in args.fixtures.split(",") if r]
    tiers = {t: {"pass": 0, "total": 0, "categories": {}} for t in TIERS}
    det_violations = 0
    incidents = {"crashes": 0, "hangs": 0, "exec_attempts": 0}

    t0 = time.time()
    with tempfile.TemporaryDirectory() as tmp:
        for root in roots:
            for fid in sorted(os.listdir(root)):
                fdir = os.path.join(root, fid)
                mp = os.path.join(fdir, "meta.json")
                if not os.path.isfile(mp):
                    continue
                meta = json.load(open(mp))
                tier = meta["tier"]
                cat = meta.get("recipe", "misc")
                # T1-T6 are pre-initialized; RVC (and any future track) is
                # created on demand so it appears in tiers only when run.
                td = tiers.setdefault(
                    tier, {"pass": 0, "total": 0, "categories": {}})
                td["total"] += 1
                cd = td["categories"].setdefault(cat, {"pass": 0, "total": 0})
                cd["total"] += 1

                # RVC fixtures need the RVC converter (f32 + config contract).
                cmd = (args.rvc_cmd or args.cmd) if tier == "RVC" else args.cmd

                if meta.get("grading") == "policy":
                    r = grade_policy_fixture(fdir, cmd, args.timeout, tmp)
                    incidents["crashes"] += int(r["crash"])
                    incidents["hangs"] += int(r["hang"])
                    incidents["exec_attempts"] += int(r["exec"])
                    passed = r["pass"]
                else:
                    r = grade_byte_fixture(fdir, cmd, args.timeout, tmp)
                    passed = r["pass"]
                    if not r.get("determinism_ok", True):
                        det_violations += 1
                        passed = False
                if passed:
                    td["pass"] += 1
                    cd["pass"] += 1

    elapsed = int(time.time() - t0)
    run = {
        "model_id": args.model_id, "language": args.language,
        "condition": args.condition,
        "budget": {"wall_seconds": args.budget_wall or elapsed,
                   "tokens": args.budget_tokens},
        "spent": {"wall_seconds": elapsed, "tokens": 0},
        "tiers": tiers,
        "iterations": [{"t_seconds": elapsed,
                        "tier_passes": {t: tiers[t]["pass"] for t in tiers}}],
        "determinism_violations": det_violations,
        "t5_peak_rss_bytes": child_peak_rss(),
        "t6_incidents": incidents,
    }

    # merge into existing results.json if present (accumulate cells)
    if os.path.isfile(args.out):
        doc = json.load(open(args.out))
    else:
        doc = {"benchmark_version": "0.1.0", "harness_version": "0.1.0",
               "fixture_set_hash": fixture_set_hash(roots),
               "container_digests": {}, "runs": []}
    doc["runs"] = [r for r in doc["runs"]
                   if not (r["model_id"] == run["model_id"]
                           and r["language"] == run["language"]
                           and r["condition"] == run["condition"])]
    doc["runs"].append(run)
    with open(args.out, "w") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")

    tot = sum(t["total"] for t in tiers.values())
    pas = sum(t["pass"] for t in tiers.values())
    print(f"{args.model_id}/{args.language}/{args.condition}: "
          f"{pas}/{tot} fixtures pass  "
          f"(det_viol={det_violations}, t6_incidents={incidents})")
    order = TIERS + [t for t in tiers if t not in TIERS]
    for t in order:
        if tiers[t]["total"]:
            print(f"  {t}: {tiers[t]['pass']}/{tiers[t]['total']}")


if __name__ == "__main__":
    main()
