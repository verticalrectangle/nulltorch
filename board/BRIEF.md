# NullTorch-Board — design brief

You are building the results dashboard for **NullTorch**, a clean-room
format-reimplementation benchmark for LLMs (models write torchless `.pth`
converters in Go/Rust/C++; runs are graded per fixture across six difficulty
tiers, three languages, and three conditions). This dashboard is itself a
benchmark task — the human-judged member of the suite. Your submission will
first pass a mechanical gate (`GATE.md`), then be scored blind by human
judges against a rubric (`RUBRIC.md`).

## The task

Build a **static, self-contained, offline results explorer**: a single HTML
file or a small static bundle. No backend. No external requests. It loads
`results.json` from disk and lets a judge explore benchmark results.

## Hard constraint: zero external dependencies — all raw

This is a blocking gate item, checked by static scan **before any human
judging**. Any violation is an automatic failure.

- **No CDN links.** No `<script src>` or `<link href>` to any external URL,
  no web fonts, no external images.
- **No frameworks or libraries of any kind.** No React/Vue/Svelte, no
  D3/Chart.js/Plotly, no Tailwind/Bootstrap. Vanilla HTML + CSS + JS only.
- **No build step.** No npm, no bundler, no imports from a package registry.
  The submission runs exactly as submitted.
- **Charts are hand-drawn.** The heatmap, scatter, iteration curves, and
  memorization-gap visualization must be rendered with inline SVG or canvas
  you generate yourself.
- **Everything is inline or in sibling local files** loaded by relative
  path. The only runtime data input is the local `results.json`. The page
  must run by opening the file offline (`file://`) with no network
  whatsoever.

## Input data

`results.json` conforms to `schema/results.schema.json`. Shape summary:

- Top level: `benchmark_version`, `fixture_set_hash`, `harness_version`,
  `container_digests` (one per language×condition container), `runs[]`.
- Each run is one (model × language × condition) cell:
  - `language`: `go | rust | cpp`; `condition`: `open_book | closed_book | delta`
  - `budget` / `spent`: `{wall_seconds, tokens}`
  - `tiers.T1..T6`: `{pass, total, categories: {<name>: {pass, total}}}`
  - `tiers.RVC` (**optional**): same shape as a tier, but it is the
    **engine-fidelity track**, not a difficulty tier — see below. Present
    only on `open_book` / `closed_book` runs; **absent on `delta` runs**.
  - `iterations[]`: `{t_seconds, tier_passes: {T1..T6[, RVC]}}` — time series
    for iterations-to-green curves; `RVC` appears here exactly when it appears
    in `tiers`.
  - `determinism_violations`, `t5_peak_rss_bytes`,
    `t6_incidents: {crashes, hangs, exec_attempts}`

### The RVC engine-fidelity track (docs/RVC_TRACK.md)

`RVC` is a **separate output-contract track**, not a T1–T6 difficulty tier.
It mirrors what pop-maker-studio's `pth_reader.cpp` actually does end-to-end
on a voice model: **config extraction** (18-element positional config →
named fields) and **f16 → f32 widening** (bit-exact). It shares the tier
shape (`pass`/`total`/`categories`) and is graded by its own converter,
reported alongside T1–T6.

Treat it as engine-fidelity, distinct from the reading tiers:

- It is **not** part of the memorization-gap visualization. The gap is
  stock − delta over the **T1–T6 reading tiers only**; RVC has no delta
  variant, so it never enters the gap. Do not fabricate an RVC gap.
- It may be **absent** (always on `delta`, and on any cell that did not run
  it). Render its absence gracefully — a clearly empty/"n/a" slot, never a
  zero, never a crash.

**The memorization gap is not stored — you derive it.** Gap = stock pass
rate − delta pass rate for the same (model, language), where stock is the
`open_book` (or `closed_book`) run and delta is the `delta` run. Cells
missing a delta run have no gap; show that honestly, don't invent zeros.

Sample datasets in `sample_data/`: `empty.json` (no runs),
`one_model.json` (1 model × 3 languages × 3 conditions),
`many_models.json` (12 models, 2–3 conditions each). The gate exercises
all three; assume real data may also carry ~50 models.

Because `file://` pages cannot `fetch()` local files in most browsers, you
must provide a file picker (`<input type="file">`) and/or drag-and-drop to
load `results.json`; you may *additionally* try `fetch('./results.json')`
for the case where the bundle is served from a local static server.

## Required analytical views

Verbatim from the benchmark spec (SPEC.md §8) — all five are required and
mechanically checked:

1. **Leaderboard with tier profiles** (sortable/filterable by language and
   condition — never one collapsed score as the only view). The tier profile
   includes an **additional RVC column** for the engine-fidelity track,
   visually distinguished from the T1–T6 difficulty tiers (e.g. a divider,
   different header treatment, or its own labeled group) and shown as
   "n/a" where a cell has no RVC data (all delta cells).
2. **Model detail:** tier × language heatmap **with RVC as an extra row/column**
   labeled as the engine-fidelity track (config + f16→f32) and set apart from
   T1–T6; **memorization-gap** visualization (stock vs. delta, **T1–T6 reading
   tiers only — RVC is excluded**); iterations-to-green curves; cost-vs-score
   scatter
3. **Fixture drill-down:** failing categories, linked warning streams
4. **Two-model side-by-side comparison**
5. **Reproducibility strip:** harness version, fixture-set hash, container
   digests

Notes:

- "Never one collapsed score as the only view": you may show aggregates,
  but the per-tier profile must always be reachable in one step and the
  default leaderboard must expose tier structure, not a single number.
- Per-language scores are never silently averaged across languages: Go has
  stdlib zip/DEFLATE, Rust/C++ hand-roll them, so the numbers are not
  comparable across that boundary.
- The v1 schema carries category pass/fail only; per-fixture warning
  streams are a later schema addition. The drill-down must render failing
  categories now and degrade gracefully (e.g., a disabled link slot) where
  warning-stream data is absent.
- Cost-vs-score: use `spent` (wall seconds and/or tokens) against a clearly
  labeled score; label which axis is which unit.

## Behavioral requirements

- **Degenerate datasets:** handles empty results (meaningful empty state,
  no errors), a single model (comparisons and scatter still render sanely),
  and 50 models (leaderboard stays usable — no unbounded page growth).
- **Keyboard navigable:** every interactive control reachable and operable
  by keyboard, visible focus, sensible focus order.
- **No horizontal page scroll** at common widths (checked at 375, 768,
  1440 px). Wide tables/charts scroll inside their own containers.
- **Dark and light** appearance are both judged (see RUBRIC.md); honor
  `prefers-color-scheme` and/or provide a toggle.
- No console errors on any provided dataset.

## What happens next

1. Mechanical gate (`GATE.md`) — including the zero-external-dependency
   static scan. Fail any blocking item and the submission is not judged.
2. Blind human judging (`RUBRIC.md`) — 6 anchored dimensions, ≥3 judges,
   inter-rater agreement reported.
