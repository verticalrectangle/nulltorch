# Elegance rubric (advisory — not part of the mechanical score)

NullTorch's leaderboard is mechanical: correctness, robustness, memorization
gap. **Elegance is a separate, qualitative dimension** — it does not change a
model's benchmark score. There is no byte oracle for "beautiful code"; it takes
a judge (human or LLM). This rubric is what the judge scores, so the call is
consistent and defensible.

`scripts/elegance.py` reports objective *hygiene* metrics (warnings,
portability, golfing, comments) as supporting signal — read them, but the score
below comes from reading the code.

## Procedure
- **Blind.** Strip model names; label submissions A, B, C… in randomized order.
- Judge each stock `submissions/<model>/convert.cpp` on the six dimensions
  below (1–5 each). Report per-dimension scores + a one-line justification, not
  just a total.
- If using an LLM judge, run ≥2 independent passes and report agreement; a
  single pass is advisory only.
- The reader's *correctness* is already known from the leaderboard — judge how
  it's written, not whether it works.

## Dimensions (1 = poor, 3 = adequate, 5 = exemplary)

1. **Readability** — can a competent C++ reader follow it without a decoder
   ring? 5: clear names, sensible structure, skimmable. 1: single-letter
   everything, 2000-char lines, no separation of concerns.
2. **Structure** — is the zip / pickle-VM / materialization split into coherent
   units with clean interfaces? 5: each layer isolated and named. 1: one
   monolith `main`.
3. **Idiom** — modern, idiomatic C++ (RAII, `std::` containers, no needless
   raw pointers/macros/UB), not C-in-C++ or code-golf tricks. 5: idiomatic and
   restrained. 1: `<bits/stdc++.h>`, macro soup, aliasing hacks.
4. **Error handling as written** — how clearly are malformed inputs and edge
   cases handled *in the source* (not just whether T6 passed)? 5: explicit,
   localized, obvious. 1: bare `at()`/asserts scattered, silent catches.
5. **Conciseness without golfing** — says what it needs, no more, but not
   compressed past readability. 5: tight and clear. 1: either bloated OR
   minified into unreadable one-liners (both score low — brevity ≠ elegance).
6. **Comments & intent** — does it explain the *why* at the tricky spots
   (persistent-id order, stride gather, zip64 saturation)? 5: sparse, high-value
   comments. 1: none, or noise.

## Reporting
A per-submission table of the six scores + total (out of 30), the blind→model
mapping revealed only after scoring, and inter-judge agreement if multiple
passes were run. Present it **beside** the mechanical leaderboard, never merged
into it — elegance informs, it does not rank the benchmark.
