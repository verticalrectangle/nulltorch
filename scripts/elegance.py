#!/usr/bin/env python3
"""Report code-HYGIENE metrics for the submissions (supporting signal only).

Elegance is qualitative — there is no byte oracle for it, so this is NOT the
elegance verdict. It reports objective, reproducible metrics that *correlate*
with clean code, to sit alongside a judge's rubric score (docs/ELEGANCE.md):

  warnings   count under `-Wall -Wextra -Wpedantic` (fewer = cleaner)
  portable   uses explicit headers, not <bits/stdc++.h> (a non-portable smell)
  sloc       non-blank lines
  max_line   longest line (a golf/readability smell — a "500-line" reader on
             11 physical lines is concise on paper but unreadable in practice)
  comment%   comment lines / sloc
  size       source bytes

The `hygiene` column is a transparent 0-100 index over those objective smells
ONLY (see weights below). Do not mistake it for elegance — read the code and
score it against docs/ELEGANCE.md. A model can be terse and warning-free yet
unreadable, or verbose yet beautifully structured.

Usage: python3 scripts/elegance.py [submissions_dir]
"""
import os
import re
import subprocess
import sys
import tempfile

CXX = "g++"
FLAGS = ["-std=c++20", "-O2", "-Wall", "-Wextra", "-Wpedantic", "-c"]


def metrics(cpp):
    src = open(cpp, "r", errors="replace").read()
    lines = src.split("\n")
    nonblank = [ln for ln in lines if ln.strip()]
    max_line = max((len(ln) for ln in lines), default=0)
    comments = sum(1 for ln in nonblank
                   if ln.lstrip().startswith(("//", "*", "/*")))
    portable = "<bits/stdc++.h>" not in src

    warn = None
    with tempfile.TemporaryDirectory() as td:
        try:
            r = subprocess.run([CXX, *FLAGS, cpp, "-o", os.path.join(td, "o")],
                               capture_output=True, text=True, timeout=120)
            warn = len(re.findall(r": warning:", r.stderr))
            if r.returncode != 0 and warn == 0:
                warn = -1  # did not compile with these flags
        except Exception:
            warn = -1

    return {
        "sloc": len(nonblank), "bytes": len(src), "max_line": max_line,
        "comment_pct": round(100 * comments / max(1, len(nonblank))),
        "warnings": warn, "portable": portable,
    }


def hygiene(m):
    """Transparent 0-100 index over OBJECTIVE smells only (not elegance)."""
    if m["warnings"] is None or m["warnings"] < 0:
        return 0  # didn't compile clean-flagged
    s = 100.0
    s -= min(40, 5 * m["warnings"])              # -5 per warning, cap -40
    if not m["portable"]:
        s -= 15                                  # <bits/stdc++.h>
    if m["max_line"] > 200:                      # golfing / readability smell
        s -= min(25, (m["max_line"] - 200) / 40)
    if m["comment_pct"] == 0:                    # no comments at all
        s -= 5
    return max(0, round(s))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "submissions")
    subs = sorted(d for d in os.listdir(root)
                  if os.path.isfile(os.path.join(root, d, "convert.cpp")))
    print("code hygiene — OBJECTIVE proxies, NOT the elegance verdict "
          "(judge against docs/ELEGANCE.md)\n")
    hdr = (f"{'submission':16} {'sloc':>5} {'bytes':>6} {'max_ln':>6} "
           f"{'cmt%':>5} {'warns':>5} {'portable':>8} {'hygiene':>7}")
    print(hdr); print("-" * len(hdr))
    rows = []
    for d in subs:
        m = metrics(os.path.join(root, d, "convert.cpp"))
        h = hygiene(m)
        rows.append((d, m, h))
    for d, m, h in sorted(rows, key=lambda x: -x[2]):
        w = "DID NOT COMPILE" if m["warnings"] is not None and m["warnings"] < 0 \
            else str(m["warnings"])
        print(f"{d:16} {m['sloc']:>5} {m['bytes']:>6} {m['max_line']:>6} "
              f"{m['comment_pct']:>5} {w:>5} {'yes' if m['portable'] else 'NO':>8} "
              f"{h:>7}")
    print("\nnote: max_ln flags golfing (unreadable one-liners); a low hygiene "
          "score means objective smells, a high one does NOT mean elegant — "
          "that is the judge's call.")


if __name__ == "__main__":
    main()
