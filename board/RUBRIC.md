# NullTorch-Board — human judging rubric

Six dimensions (SPEC.md §8), each scored on an anchored 1–5 scale. Anchors
are written for 1, 3, and 5; use 2 and 4 for submissions clearly between
anchors. Judges score every dimension independently; no overall gut score.

A submission is only judged at all if it has already passed the mechanical
gate (`GATE.md`). The gate is not part of the rubric score — it is a
precondition.

## Dimensions

### 1. Information hierarchy

Can a judge find the answer to a real analytical question fast, without
reading documentation?

- **1** — The judge cannot answer "which model is best at T3-Rust?" without
  hunting through multiple views or mentally recomputing numbers; the
  default view is a single collapsed score or an undifferentiated wall of
  numbers; tier structure is buried.
- **3** — The question is answerable in under a minute via sorting or
  filtering, but requires knowing where to look first; the leaderboard
  foregrounds tier profiles but secondary views (gap, drill-down) take
  trial and error to reach.
- **5** — A judge can answer "which model is best at T3-Rust?" in under
  10 seconds without documentation; the visual weight of every screen
  matches what matters (tier profiles > aggregates > metadata); any datum
  from the required views is at most two obvious interactions away.

### 2. Data-ink honesty

Do the visualizations tell the truth at a glance?

- **1** — Truncated or unlabeled axes flatter small differences; tier
  profiles are collapsed into a single score wherever comparison happens;
  languages are averaged across the Go/Rust-C++ stdlib boundary; missing
  cells (e.g., no delta run) are rendered as zeros.
- **3** — Axes are honest and labeled, but some views invite misreading:
  gap charts without sample context, scatter without units, aggregates
  shown more prominently than the profiles behind them.
- **5** — Every axis starts where it claims, every unit is labeled, missing
  data is visibly *missing* (not zero), per-fixture counts (n) are
  discoverable everywhere a rate is shown, and no view collapses the tier
  profile into one number without the profile being equally visible.

### 3. Interaction quality

- **1** — Sorting/filtering is absent, broken, or destroys other state
  (e.g., filtering resets the selected model); side-by-side comparison
  requires re-selecting everything; interactions lag noticeably on the
  many-models dataset.
- **3** — All required interactions (sort, filter by language/condition,
  model select, compare) work and are discoverable, but with friction:
  state occasionally resets, no feedback during selection, small click
  targets.
- **5** — Interactions are instant on 50-model data, state is coherent
  (filters persist across views where sensible), current selection is
  always visible, and the compare flow feels designed rather than bolted
  on; hover/focus reveals exact values wherever a mark is approximate.

### 4. Visual & aesthetic coherence (incl. dark/light)

- **1** — Inconsistent spacing/typography/colors between views; charts and
  tables look like different products; dark mode is absent, or broken
  (unreadable text, invisible chart marks) in one scheme.
- **3** — One coherent style; dark and light both usable, but with rough
  edges (chart colors not rebalanced for dark, hard-coded backgrounds
  bleeding through); category colors inconsistent between views.
- **5** — Reads as one system in both schemes: consistent palette with the
  same semantic colors for the same tiers/languages everywhere, deliberate
  typography and spacing, charts and tables visually unified; switching
  scheme degrades nothing.

### 5. Responsiveness

- **1** — Layout breaks at 375 px or 768 px (overlapping elements,
  unreadable charts, horizontal page scroll); usable only at desktop width.
- **3** — All views usable at all three checked widths, but small screens
  are an afterthought: charts shrink instead of reflowing, tap targets are
  small, tables are cramped though scrollable in-place.
- **5** — Deliberate layouts at phone/tablet/desktop: views reflow, wide
  content scrolls inside its own container, charts remain legible and
  interactive at 375 px, and nothing is lost — only rearranged.

### 6. Accessibility

- **1** — Contrast failures in either scheme; interactive elements are
  divs with click handlers and no roles/names; no visible focus; charts
  convey information by color alone with no text equivalent.
- **3** — Passes obvious checks: AA contrast for text, focus visible,
  controls are real buttons/inputs with labels; but chart data has no
  non-visual path and focus order has surprises.
- **5** — AA contrast in both schemes including chart marks; complete,
  logical focus order; ARIA roles/names/states on custom widgets; every
  chart's data reachable as text (table fallback, aria labels, or
  focusable marks with value readouts); works with a screen reader well
  enough to read the leaderboard.

## Procedure

1. **Mechanical gate first.** Run every check in `GATE.md`. A submission
   that fails any blocking gate item is recorded as "gate-failed" and is
   **not** human-judged. Gate results are reported alongside rubric scores.
2. **Anonymize.** Strip model/author identifiers from submissions (file
   names, code comments, meta tags, telltale strings). Assign opaque IDs.
3. **Randomize order.** Each judge receives the submissions in an
   independently randomized order, with the same three sample datasets
   preloaded/available for each.
4. **Blind, independent scoring.** Minimum **3 judges**; each scores all
   6 dimensions per submission without conferring. Judges record a short
   note per dimension citing what drove the score (notes are published
   with the results).
5. **Report agreement.** Compute and report **Krippendorff's alpha**
   (ordinal) per dimension alongside the scores. Publish per-dimension
   medians and the per-judge raw matrix — never a single collapsed design
   score without the profile.
6. **Conflict note (v1).** Benchmark operators double as judges in v1;
   this conflict is documented in the published results, and which
   submissions each operator judges is randomized (SPEC.md §11).
7. **Optional LLM judge.** An LLM judge may score the same rubric on the
   same anonymized submissions; human-vs-LLM correlation on design quality
   is published as a secondary finding, never substituted for human scores.
