#!/usr/bin/env python3
"""NullTorch orchestrator (submission driver).

Turns a built submission into a scored results.json cell. This is the thin,
reproducible entry point behind "point an agent at it and go eval":

  agent (model under test) --writes--> submissions/<id>/  (convert.cpp + build)
  orchestrate.py           --builds--> a converter binary
                           --grades--> run.py over public (dev) + hidden (eval)
                           --emits---> results.json cell (board schema)

The AGENTIC iteration time-series (PROTOCOL.md §4) is produced when the
orchestrator mediates the agent's self-grade calls live; a submission graded
after the fact gets a single terminal iteration (its final state). Both are
valid board rows.

A submission dir contains either:
  - build.sh   (compiles to ./convert; run from the submission dir), and/or
  - convert    (a prebuilt binary),
invoked as  ./convert <file.pth> <out_dir>.

Usage:
  python3 harness/orchestrate.py \
      --submission submissions/claude-cpp \
      --model-id claude-cpp --language cpp --condition open_book \
      --public fixtures/public,fixtures/t4 \
      --hidden fixtures/hidden \
      --out results.json
"""

import argparse
import os
import shlex
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_ROOT = os.path.dirname(_HERE)
RUN = os.path.join(_HERE, "run.py")


def build(subdir):
    sh = os.path.join(subdir, "build.sh")
    if os.path.isfile(sh):
        print(f"[orchestrate] building via {sh}")
        r = subprocess.run(["bash", "build.sh"], cwd=subdir)
        if r.returncode != 0:
            raise SystemExit("[orchestrate] build failed")
    binary = os.path.join(subdir, "convert")
    if not (os.path.isfile(binary) and os.access(binary, os.X_OK)):
        raise SystemExit(f"[orchestrate] no runnable ./convert in {subdir}")
    return os.path.abspath(binary)


def run_cells(binary, model_id, language, condition, public, hidden, out):
    cmd_tpl = f"{shlex.quote(binary)} {{pth}} {{out}}"
    # dev pass over public fixtures (informational; not the authoritative score)
    if public:
        print("[orchestrate] --- dev grade (public fixtures) ---")
        subprocess.run([sys.executable, RUN, "--fixtures", public,
                        "--cmd", cmd_tpl, "--model-id", model_id + "-dev",
                        "--language", language, "--condition", condition,
                        "--out", os.path.join(_ROOT, "scratch_dev.json")],
                       cwd=_ROOT)
    # authoritative pass over the hidden set -> the real results cell
    print("[orchestrate] --- eval grade (hidden fixtures) ---")
    subprocess.run([sys.executable, RUN, "--fixtures", hidden,
                    "--cmd", cmd_tpl, "--model-id", model_id,
                    "--language", language, "--condition", condition,
                    "--out", out], cwd=_ROOT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--submission", required=True)
    ap.add_argument("--model-id", required=True)
    ap.add_argument("--language", required=True, choices=["go", "rust", "cpp"])
    ap.add_argument("--condition", required=True,
                    choices=["open_book", "closed_book", "delta"])
    ap.add_argument("--public", default="")
    ap.add_argument("--hidden", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    binary = build(os.path.abspath(args.submission))
    run_cells(binary, args.model_id, args.language, args.condition,
              args.public, args.hidden, os.path.abspath(args.out))
    print(f"[orchestrate] wrote cell {args.model_id}/{args.language}/"
          f"{args.condition} -> {args.out}")


if __name__ == "__main__":
    main()
