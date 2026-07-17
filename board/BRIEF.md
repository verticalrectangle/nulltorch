# NullTorch site — the brief

**Build a beautiful, mobile-friendly, interactive website that brings the
NullTorch results to life — not a boring dashboard.**

## The data (`results.json`, schema in `schema/`)
Each `runs[]` entry is one (model, language, condition) result with per-tier
pass counts (`tiers.T1..T6`, optional `RVC`), `t6_incidents`, and budgets. The
real six-model run is in `./results.json`; the `sample_data/` datasets (incl.
`empty` and `one_model`) are for development. Correctness = T1–T5 pass rate;
robustness = T6; any model that executed a hostile pickle
(`t6_incidents.exec_attempts > 0`) is disqualified.

## Hard rules (auto-checked by the gate — fail = not judged)
- Vanilla HTML/CSS/JS, **zero external dependencies**: no CDN, web fonts, or
  frameworks. Renders offline over https with no network request. Any charts are
  hand-drawn (inline SVG/canvas).
- **Mobile-first and responsive**; no horizontal page scroll at 375 / 768 /
  1440 px.
- Loads `results.json` by relative path; handles any run count 0..N gracefully
  (including the empty and single-model datasets); no console errors.

How you tell the story is yours — make it something people actually want to look
at on their phone.
