# NullTorch

[![CI](https://github.com/verticalrectangle/nulltorch/actions/workflows/ci.yml/badge.svg)](https://github.com/verticalrectangle/nulltorch/actions/workflows/ci.yml)

An LLM benchmark: **reimplement a layered real-world binary format from
evidence, under a negative dependency constraint.** Flagship task — read
PyTorch `.pth` checkpoints (zip → pickle → torch tensor/storage semantics)
in Go / Rust / C++ with **no torch, no Python, stdlib only.** Generalized
from the torchless `pth_reader.cpp` in pop-maker-studio.

Grading is a mechanical byte oracle: fixtures are generated *with* torch,
ground truth frozen as raw bytes + manifests, submissions graded by exact
comparison. No LLM judge on the core task.

See `SPEC.md` for the full design. This tree is a working v1 implementation.

## Layout

```
SPEC.md                     full benchmark design
grader/
  schema.py                 manifest schema — single source of truth
  grade.py                  stdlib-only grader (no torch at eval time)
  selftest.py               positive + negative controls
fixtures/
  gen/generate.py           T1–T3 generator + stdlib-pickle oracle
  gen/generate_hard.py      T4 (archaeology) + T6 (adversarial) generator
  public/  hidden/          T1–T3 stock fixture sets (seeds 1xxx / 2xxx)
  public_delta/ hidden_delta/  delta-variant sets (see delta/)
  t4/  t6/                  hard-tier fixtures  (+ t4/DEFERRED.md)
  T5_DESIGN.md              resource-envelope tier (generated at calibration)
delta/
  DELTA_SPEC.md             one-page deviation spec (delta condition)
  transform_delta.py        stock → delta transformer (verified invertible)
docs/openbook/              open-book condition docs (pickle/zip/floats only —
                            NOT torch; that layer is closed-book always)
harness/
  PROTOCOL.md               cells, enforcement, budgets, hidden-set handling
  containers/*.Containerfile  pinned Go/Rust/C++ runtime images (no Python)
board/                      NullTorch-Board: the human-judged dashboard task
  schema/results.schema.json, BRIEF.md, RUBRIC.md, GATE.md, sample_data/
```

## Reproduce the checks

```
# generate fixtures (needs torch, generation-side only)
cd fixtures/gen
python3 generate.py       --out ../public --seeds 1001,1002,1003
python3 generate.py       --out ../hidden --seeds 2001,2002,2003
python3 generate_hard.py  --out-t4 ../t4 --out-t6 ../t6 --seeds 1001,1002
cd ../../delta
python3 transform_delta.py ../fixtures/public ../fixtures/public_delta

# grade / self-test (stdlib only, no torch)
cd ../grader
python3 selftest.py ../fixtures/public      # positive + negative controls
python3 grade.py --set ../fixtures/public <submissions_root>
```

## Conditions & rotation
- Languages: Go, Rust, C++ — full cross product, reported per-language
  (Go's stdlib has zip/DEFLATE; Rust/C++ hand-roll — surfaces only at T4).
- Open-book (default) / closed-book / **delta** (memorization gap =
  stock − delta).

## Running the benchmark

Needs Python 3 (stdlib only) and, for the C++ example, `g++`. **No torch is
needed to run or grade** — the fixtures are committed. `pip install jsonschema`
is only for board-schema validation.

**1. Verify the whole suite** (exactly what CI runs):
```
bash scripts/ci.sh
```

**2. Evaluate a converter** — build it and score it on the hidden set:
```
python3 harness/orchestrate.py \
  --submission submissions/claude-cpp \
  --model-id my-model --language cpp --condition open_book \
  --public fixtures/public,fixtures/t4 --hidden fixtures/hidden \
  --out results.json
```
A submission dir needs a `build.sh` that compiles `./convert` (see
`submissions/claude-cpp/`), invoked as `./convert <file.pth> <out_dir>`. The
authoritative score is the **hidden**-set pass; the public pass is a dev signal.
For the RVC track add `--rvc-cmd` via `harness/run.py` (see docs/RVC_TRACK.md).

**3. Grade output you already produced**:
```
python3 grader/grade.py <fixture_dir> <your_out_dir>       # one fixture
python3 grader/grade.py --set fixtures/public <subs_root>  # a whole set
```
where `<subs_root>/<fixture_id>/` holds `manifest.json` + `tensors/*.bin`.

**4. Run a model as an agent.** First make an **isolated workspace** — in the
full repo an agent will just find `reference/` or `submissions/` and grade the
committed answer instead of solving:
```
scripts/new_eval.sh kimi-cpp cpp     # -> eval/kimi-cpp/ (no reference, no hidden set)
```
Open your agent tool (opencode, Claude Code, …) **inside `eval/kimi-cpp/`** and
say *"Read `harness/TASK.md` and `START.md`, then do it."* It writes its
converter in `submission/` and self-grades on the public fixtures. When it's
done, score its artifact on the **hidden** set from the real repo:
```
python3 harness/orchestrate.py --submission eval/kimi-cpp/submission \
  --model-id kimi-cpp --language cpp --condition open_book \
  --public eval/kimi-cpp/fixtures/public --hidden fixtures/hidden --out results.json
```
`submissions/claude-cpp/` was produced this way (via Claude as the agent).

**5. Score / rank the results**:
```
python3 scripts/score.py [results.json]   # default: harness/example_results.json
```
A leaderboard with per-model correctness, T6 robustness, a safety gate (any
executed hostile pickle → disqualified), the RVC track, and the memorization
gap (stock − delta, when both conditions are present). NullTorch reports a
*profile*, not one number — the composite is a convenience for ranking, with
adjustable weights at the top of `scripts/score.py`.

**Regenerate fixtures** (optional; the only step that needs torch):
```
python3 fixtures/gen/generate.py      --out fixtures/public --seeds 1001,1002,1003
python3 fixtures/gen/generate_hard.py --out-t4 fixtures/t4 --out-t6 fixtures/t6 --seeds 1001,1002
python3 delta/transform_delta.py      fixtures/public fixtures/public_delta
```

## RVC track — engine fidelity (implemented)
Beyond general reading, NullTorch mirrors what `pop-maker-studio/src/pth_reader.cpp`
actually does for RVC voice models: **config extraction** (18-arg
SynthesizerTrnMsNSFsid list + phone_dim) and **f16→f32 conversion**, graded
exactly. Validated three-way on a real HuggingFace checkpoint
(`InductiveGrub/DollyParton`, 457 tensors): torch oracle == stdlib reference
(config + all f32 tensors bit-exact) == the shipping `pth_reader.cpp` engine
(every config field). See `docs/RVC_TRACK.md`. Converters: `reference/rvc_convert.py`,
oracle: `fixtures/gen/gen_rvc.py`.

## Agent eval — live, end-to-end (implemented)
`harness/TASK.md` is the agent-facing task; `harness/orchestrate.py` builds a
submission and grades it on public (dev) + hidden (eval) into a results cell.
Proven by running it for real: a coding agent built a from-scratch **stdlib
C++** `.pth` reader (`submissions/claude-cpp/`, 586 lines, one file — ZIP
central-dir parse incl. zip64, a full RFC-1951 inflate, a protocol-0/1/2/4
pickle VM, stride-aware materialization) and was scored on the **hidden** set
it never saw:

- Correctness: **hidden 36/36** (T1–T3, disjoint seeds) + dev 50/50 incl. T4 —
  generalizes, 0 determinism violations.
- Safety: exec-canary never fired (0 exec attempts) — it skips the `os.system`
  global instead of running it.
- Robustness: **T6 5/7 — two SIGSEGV** (`deep_nesting`, `memo_cycle`:
  unbounded recursion). The correctness fixtures never showed this; the
  adversarial tier did. That gap *is* the benchmark working.

Results cell in `harness/example_results.json` (now 2 real models:
`reference-py`, `claude-cpp`).

## End-to-end loop (implemented)
`reference/convert.py` (stdlib, validation-only) → `harness/run.py` grades it
over any fixture set → board-schema `results.json` (`harness/example_results.json`).
Verified: **57/57** on stock (T1–T4 + T6, zero T6 incidents), **1/1** on T5
(streaming, hashes-only gt), and **0/36** on the delta variant — the
format-knowledge reader collapses under the delta spec exactly as intended
(the memorization-gap signal, live).

## Status (v1)
Green: T1–T3 (public+hidden), delta variant (both sets), T4 (bf16/f64/int
family/proto4/deflate/zip64/module-skip), T5 (streaming, sampled-hash gt),
T6 (7 adversarial), grader with controls, reference converter, harness runner
→ results.json, open-book docs, board suite, orchestrator protocol +
Containerfiles.
Deferred (documented): fp8/qtensor/legacy T4 (`fixtures/t4/DEFERRED.md`),
T5 multi-GB calibration fixtures (`fixtures/T5_DESIGN.md`), container digest
pins, and the agent-driver `orchestrate.py` (specified in
`harness/PROTOCOL.md`; runtime-SDK-specific).
