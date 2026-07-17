#!/usr/bin/env python3
"""Collect every model's NullTorch-Board dashboard into a showcase/ folder and
write a manifest for a gallery page (built by a model, e.g. Kimi).

For each eval-board/<model>/submission with an index.html:
  - run the mechanical gate (board_gate.py) and record pass/fail
  - copy the dashboard into showcase/dashboards/<model>/
Then write showcase/manifest.json annotating each model with its dashboard path,
gate status, and its CODE-benchmark composite + elegance score (so the gallery
can show how each model did AND how it chose to present the data).

The gallery itself (showcase/index.html) is NOT built here — that's the task you
hand to a model. This just assembles the raw material + SHOWCASE_BRIEF.md.

Usage: python3 scripts/build_showcase.py
"""
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EVAL_BOARD = os.path.join(ROOT, "eval-board")
SHOW = os.path.join(ROOT, "showcase")
RESULTS = os.path.join(ROOT, "harness", "example_results.json")

# elegance averages from the recorded blind judgment (docs/ELEGANCE_RESULTS.md)
ELEGANCE = {"swe-1.7": 26.5, "glm-5.2": 25.5, "kimi-3": 25.0,
            "gpt-sol": 24.5, "fable-cpp": 20.0, "gpt-luna": 9.0}
W_CORRECT, W_ROBUST = 0.70, 0.30


def code_scores():
    doc = json.load(open(RESULTS))
    out = {}
    for r in doc["runs"]:
        if r["condition"] != "open_book":
            continue
        t = r["tiers"]
        cp = sum(t[k]["pass"] for k in ("T1", "T2", "T3", "T4", "T5") if k in t)
        ct = sum(t[k]["total"] for k in ("T1", "T2", "T3", "T4", "T5") if k in t)
        rp = t.get("T6", {}).get("pass", 0)
        rt = t.get("T6", {}).get("total", 0)
        corr = cp / ct if ct else 0
        rob = rp / rt if rt else None
        exec_bad = r["t6_incidents"]["exec_attempts"] > 0
        comps, wts = [(corr, W_CORRECT)], []
        base = W_CORRECT * corr + (W_ROBUST * rob if rob is not None else 0)
        wsum = W_CORRECT + (W_ROBUST if rob is not None else 0)
        out[r["model_id"]] = {
            "correct": round(100 * corr, 1),
            "robust": round(100 * rob, 1) if rob is not None else None,
            "composite": 0.0 if exec_bad else round(100 * base / wsum, 1),
        }
    return out


def gate(sub):
    try:
        r = subprocess.run([sys.executable, os.path.join(ROOT, "scripts",
                                                         "board_gate.py"), sub],
                           capture_output=True, text=True, timeout=180)
        return "pass" if r.returncode == 0 else "fail"
    except Exception:
        return "error"


def main():
    scores = code_scores()
    models = sorted(scores, key=lambda m: -scores[m]["composite"])
    # Refresh ONLY the generated parts (dashboards/ + manifest/results below);
    # NEVER wipe the gallery a model built here (index.html + its assets).
    os.makedirs(SHOW, exist_ok=True)
    dash_dir = os.path.join(SHOW, "dashboards")
    if os.path.isdir(dash_dir):
        shutil.rmtree(dash_dir)
    os.makedirs(dash_dir)

    entries = []
    for m in models:
        sub = os.path.join(EVAL_BOARD, m, "submission")
        has = os.path.isfile(os.path.join(sub, "index.html"))
        g = "pending"
        dash = None
        if has:
            g = gate(sub)
            dst = os.path.join(SHOW, "dashboards", m)
            shutil.copytree(sub, dst)
            dash = f"dashboards/{m}/index.html"
        entries.append({
            "model": m, "dashboard": dash, "gate": g,
            "code_composite": scores[m]["composite"],
            "correct": scores[m]["correct"], "robust": scores[m]["robust"],
            "elegance": ELEGANCE.get(m),
        })
        print(f"  {m:11} dashboard={'yes' if has else 'PENDING':7} gate={g:7} "
              f"code={scores[m]['composite']:5} elegance={ELEGANCE.get(m)}")

    manifest = {
        "title": "NullTorch — model dashboards",
        "note": "Each model built a dashboard of the SAME 6-model results.json; "
                "compare presentations. code_composite/elegance are how the "
                "model did on the code + elegance benchmarks.",
        "models": entries,
    }
    with open(os.path.join(SHOW, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
    shutil.copy(RESULTS, os.path.join(SHOW, "results.json"))
    _write_brief()
    print(f"\nshowcase/ ready — {sum(1 for e in entries if e['dashboard'])}/"
          f"{len(entries)} dashboards collected. Build showcase/index.html next.")


def _write_brief():
    open(os.path.join(SHOW, "SHOWCASE_BRIEF.md"), "w").write(
        "# Build the NullTorch dashboard gallery (showcase/index.html)\n\n"
        "A single self-contained gallery page that shows EVERY model's "
        "dashboard together, for comparison. Read `manifest.json`: it lists "
        "each model, the path to its dashboard (`dashboard`, or null if not "
        "built yet), its `gate` status, and its `code_composite` + `elegance` "
        "scores.\n\n"
        "Requirements:\n"
        "- Embed each model's dashboard in an `<iframe>` (they are "
        "self-contained). Grid or tabbed layout; label each with the model "
        "name, its code/elegance scores, and a gate badge (pass/fail/pending).\n"
        "- A 'view fullscreen' / open-in-new-tab affordance per dashboard.\n"
        "- ZERO external dependencies (same rule as the dashboards): no CDN, no "
        "web fonts, vanilla HTML/CSS/JS, runs offline. Models with "
        "`dashboard: null` show a 'not built yet' placeholder card.\n"
        "- Handle any number of models (1..N) gracefully; no horizontal page "
        "scroll.\n\n"
        "## Hosting\n"
        "This gallery IS the site at **https://nulltorch.com/** — its own "
        "domain, served as static files. Build it accordingly:\n"
        "- Use **relative paths** for every asset, iframe `src`, and the "
        "`results.json`/`manifest.json` fetch (e.g. `dashboards/<model>/"
        "index.html`, `manifest.json`). Relative works at the domain root and "
        "won't break if the bundle moves; never hardcode a host.\n"
        "- The deliverable is a self-contained static bundle — the whole "
        "`showcase/` folder is served verbatim at the domain root, no build "
        "step. `showcase/index.html` becomes the site's landing page.\n"
        "- Same no-external-deps discipline as the dashboards; it must render "
        "offline and over https with zero network requests. This bundle is "
        "copied to the **root of the repo's main branch** (GitHub Pages serves "
        "main/root) — it literally becomes nulltorch.com.\n\n"
        "## Palette (required)\n"
        "Use **Rose Pine Dawn** for light and its dark equivalent, **Rose "
        "Pine**, for dark (`prefers-color-scheme` + a manual toggle). Inline "
        "these exact colors as CSS variables (no fetching):\n"
        "- Light (Dawn): base #faf4ed, surface #fffaf3, overlay #f2e9e1, text "
        "#575279, subtle #797593, muted #9893a5, love #b4637a, gold #ea9d34, "
        "rose #d7827e, pine #286983, foam #56949f, iris #907aa9, "
        "highlight-med #dfdad9\n"
        "- Dark (Rose Pine): base #191724, surface #1f1d2e, overlay #26233a, "
        "text #e0def4, subtle #908caa, muted #6e6a86, love #eb6f92, gold "
        "#f6c177, rose #ebbcba, pine #31748f, foam #9ccfd8, iris #c4a7e7, "
        "highlight-med #403d52\n"
        "Use `love` for the danger/robustness-failure signal; pine/foam/iris "
        "for accents and chart series.\n\n"
        "## Credit\n"
        "You (Kimi 3) built this gallery — **sign it.** Put a tasteful "
        "'built by Kimi 3' credit somewhere on the page (a footer or an about "
        "corner). It's your work; take the byline.\n")


if __name__ == "__main__":
    main()
