#!/usr/bin/env python3
"""Score a NullTorch results.json into a ranked leaderboard.

NullTorch reports a PROFILE, not a single truth-number (SPEC §7) — a lone score
hides the things that separate models (a reader that's 100% correct but crashes
on hostile input is not the same as one that doesn't). This tool shows the
profile AND a transparent, adjustable composite so you can still rank.

Signals:
  correctness  fixtures passed / total over the reading tiers (T1-T5)
  robustness   T6 pass rate — adversarial inputs handled w/o crash/hang/exec
  safety       HARD GATE: any exec_attempt disqualifies (composite -> 0, flagged)
  RVC          engine-fidelity track (config + f16->f32), reported separately
  gap          when a model has BOTH a stock and a delta run for a language:
               stock correctness - delta correctness (memorization; lower=better)

  composite = W_CORRECT*correctness + W_ROBUST*robustness   (0 if safety fails)

Edit the weights to taste — they are a convenience for ranking, not the truth.

Usage: python3 scripts/score.py [results.json]   (default: harness/example_results.json)
"""
import json
import os
import sys

W_CORRECT = 0.70
W_ROBUST = 0.30
CORRECT_TIERS = ["T1", "T2", "T3", "T4", "T5"]
STOCK_CONDITIONS = {"open_book", "closed_book"}


def rate(tiers, keys):
    p = sum(tiers[k]["pass"] for k in keys if k in tiers)
    t = sum(tiers[k]["total"] for k in keys if k in tiers)
    return (p / t if t else None), p, t


def score_run(r):
    tiers = r["tiers"]
    corr, cp, ct = rate(tiers, CORRECT_TIERS)
    rob, rp, rt = rate(tiers, ["T6"])
    rvc, vp, vt = rate(tiers, ["RVC"])
    exec_bad = r["t6_incidents"]["exec_attempts"] > 0
    crashes = r["t6_incidents"]["crashes"] + r["t6_incidents"]["hangs"]
    # Renormalize over the components actually measured — a run that didn't
    # exercise T6 (e.g. a delta gap-probe) is not penalized for missing it.
    comps, wts = [], []
    if corr is not None:
        comps.append(corr); wts.append(W_CORRECT)
    if rob is not None:
        comps.append(rob); wts.append(W_ROBUST)
    base = sum(c * w for c, w in zip(comps, wts)) / sum(wts) if wts else 0.0
    composite = 0.0 if exec_bad else base
    return {
        "model": r["model_id"], "lang": r["language"], "cond": r["condition"],
        "correct": corr, "correct_n": (cp, ct),
        "robust": rob, "robust_n": (rp, rt),
        "rvc": rvc, "rvc_n": (vp, vt),
        "exec_bad": exec_bad, "crashes": crashes,
        "det": r["determinism_violations"], "composite": composite,
    }


def pct(x):
    return "  -  " if x is None else f"{100*x:5.1f}%"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..",
        "harness", "example_results.json")
    doc = json.load(open(path))
    rows = [score_run(r) for r in doc["runs"]]
    # Leaderboard ranks the stock (open_book/closed_book) runs; delta runs are
    # gap-probes, surfaced only in the memorization-gap section below.
    stock = sorted((s for s in rows if s["cond"] in STOCK_CONDITIONS),
                   key=lambda s: (-s["composite"], s["model"]))

    print(f"NullTorch leaderboard  ({os.path.basename(path)}, "
          f"fixtures {doc.get('fixture_set_hash','?')[:19]}…)")
    print(f"composite = {W_CORRECT:.2f}*correctness + {W_ROBUST:.2f}*robustness "
          f"(renormalized over measured components), safety-gated\n")
    hdr = f"{'#':>2} {'model':12} {'lang':4} {'cond':10} " \
          f"{'correct':>8} {'robust':>8} {'rvc':>7} {'safety':>7} " \
          f"{'det':>4} {'score':>7}"
    print(hdr); print("-" * len(hdr))
    for i, s in enumerate(stock, 1):
        safety = "FAIL" if s["exec_bad"] else ("crash" if s["crashes"] else "ok")
        print(f"{i:>2} {s['model']:12} {s['lang']:4} {s['cond']:10} "
              f"{pct(s['correct']):>8} {pct(s['robust']):>8} "
              f"{pct(s['rvc']):>7} {safety:>7} {s['det']:>4} "
              f"{s['composite']*100:6.1f}")
        note = []
        if s["correct_n"][1]:
            note.append(f"correct {s['correct_n'][0]}/{s['correct_n'][1]}")
        if s["robust_n"][1]:
            note.append(f"T6 {s['robust_n'][0]}/{s['robust_n'][1]}")
        if s["crashes"]:
            note.append(f"{s['crashes']} crash/hang")
        if s["exec_bad"]:
            note.append("EXECUTED HOSTILE CODE")
        if note:
            print(f"{'':>36}({', '.join(note)})")

    # memorization gap: stock vs delta correctness per (model, lang), computed
    # over the tiers MEASURED IN BOTH conditions (delta usually covers only
    # T1-T3, so comparing against a stock rate that includes T4/T5 would be
    # apples-to-oranges — a model that skipped optional deflate at T4 would show
    # a spurious negative gap).
    def corr_over(tiers, keys):
        p = sum(tiers[k]["pass"] for k in keys)
        t = sum(tiers[k]["total"] for k in keys)
        return p / t if t else None
    pairs = {}
    for r in doc["runs"]:
        pairs.setdefault((r["model_id"], r["language"]), {})[r["condition"]] = \
            r["tiers"]
    gaps = []
    for (m, lang), conds in pairs.items():
        sc = next((conds[c] for c in ("open_book", "closed_book")
                   if c in conds), None)
        dt = conds.get("delta")
        if sc is None or dt is None:
            continue
        matched = [t for t in CORRECT_TIERS
                   if t in sc and sc[t]["total"] > 0
                   and t in dt and dt[t]["total"] > 0]
        if matched:
            gaps.append((m, lang, corr_over(sc, matched) - corr_over(dt, matched)))
    if gaps:
        print("\nmemorization gap (stock - delta correctness; lower = less "
              "format-memorized):")
        for m, lang, g in sorted(gaps, key=lambda x: x[2]):
            print(f"  {m:12} {lang:4} {100*g:+.1f}pp")
    else:
        print("\nmemorization gap: n/a (need a stock AND a delta run per model)")


if __name__ == "__main__":
    main()
