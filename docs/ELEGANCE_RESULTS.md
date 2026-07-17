# Elegance judgment — results (advisory; NOT part of the benchmark score)

Blind, two-pass judgment against `ELEGANCE.md`. The six stock readers had their
names stripped and order scrambled (A–F); two independent LLM judges scored each
1–5 on the six rubric dimensions without knowing which model wrote which. The
A–F → model mapping was revealed only after scoring.

## Aggregate (total /30)

| # | model | judge 1 | judge 2 | avg | Δ | benchmark score |
|---|-------|:------:|:------:|:---:|:--:|:--:|
| 1 | swe-1.7   | 26 | 27 | **26.5** | ±1 | 100.0 |
| 2 | glm-5.2   | 25 | 26 | **25.5** | ±1 | 100.0 |
| 3 | kimi-3    | 25 | 25 | **25.0** | ±0 | 100.0 |
| 4 | gpt-sol   | 25 | 24 | **24.5** | ±1 | 95.7 |
| 5 | fable-cpp | 22 | 18 | **20.0** | ±4 | 91.4 |
| 6 | gpt-luna  |  9 |  9 | **9.0**  | ±0 | 97.3 |

- judge 1 rank: swe-1.7 > gpt-sol > kimi-3 > glm-5.2 > fable-cpp > gpt-luna
- judge 2 rank: swe-1.7 > glm-5.2 > kimi-3 > gpt-sol > fable-cpp > gpt-luna

**Inter-judge agreement is high:** same #1 (swe-1.7), identical last place
(gpt-luna, 9/9), the same tight top cluster (24.5–26.5, only internal shuffling),
and fable-cpp pinned at #5 by both. The one real disagreement is fable-cpp's
magnitude (±4).

## What the judges saw (blind, condensed)
- **swe-1.7** — cleanest, most conventional modern C++: classed layers, `make_*`
  factories, uniform `fail()`→exception→catch, high-value comments. Dinged only
  for verbosity (a fat all-in-one value struct, repeated f32-convert blocks).
- **glm-5.2** — well-separated `Reader`/`Inflater`/`ZipFile`/`Pickler`/`walk`,
  `string_view` reads, documented RVC config; dinged for C `FILE` I/O and some
  silent leniency (zero-filling missing storage, a quiet visit cap).
- **kimi-3** — best comments (section banners + why-notes) and the most complete
  /robust, but the longest file, with C-style `FILE` I/O and non-portable
  `unsigned __int128` hurting idiom/conciseness.
- **gpt-sol** — tight and idiomatic with the most rigorous bounds/overflow
  guards, but literally zero comments and a very dense core.
- **fable-cpp** — adequate and decently commented, but inconsistent error
  handling (`die()`/`exit()` mixed with throws `main` never catches) and missing
  validations. *(Note: the objective hygiene index rated this file the CLEANEST
  — 0 warnings, portable, short — yet the judges ranked it 5th. Warnings don't
  catch a muddled error model. This is why elegance needs a judge.)*
- **gpt-luna** — textbook code-golf: `using namespace std`, single-letter types,
  whole subsystems on ~2000–3000-char single lines, no comments. Both judges'
  worked example of a 1.

## Elegance vs the benchmark (the interesting cross-cut)
- **swe-1.7 wins both** — tied #1 on the mechanical benchmark AND #1 on
  elegance. The genuine all-rounder: correct, robust, and cleanly written.
- **gpt-luna is the sharpest split** — respectable #4 on the benchmark (97.3)
  but dead last on elegance (9). It reached correctness + robustness through
  extreme code-golf. The mechanical benchmark rewards *behavior*; it is blind to
  the fact that the code is unmaintainable. Elegance is the dimension that sees
  it.
- **fable-cpp inverts the hygiene proxy** — metric-cleanest (hygiene 99), judged
  5th (20). Objective smells ≠ elegance.

Elegance informs; it does not rank the benchmark. Reproduce the objective half
with `python3 scripts/elegance.py`; the qualitative half is a judged pass
against `ELEGANCE.md`.
