# NullTorch — clean-room format-reimplementation benchmark for LLMs

**Working title.** "NullTorch" = build a reader for format X with zero dependence on
the library that defines X. Flagship task: the torchless `.pth` converter
(**PTH-Read**), generalized from `pop-maker-studio/src/pth_reader.cpp`.
Companion human-judged task: the results dashboard (**NullTorch-Board**).

Status: design spec, v1. No code exists yet.

---

## 1. Concept and claims

The benchmark measures an LLM's ability to **reimplement a layered real-world
binary format from evidence, under a negative dependency constraint** — "read
PyTorch checkpoints without torch or Python." Three properties motivate it:

1. **Mechanical oracle.** Fixtures are generated *with* torch at build time;
   ground truth is frozen as raw bytes + manifests. Grading is byte comparison.
   No LLM judge, no float tolerances (extraction is byte-copying — demand
   bit-exactness).
2. **Layered difficulty.** Three stacked formats — zip container → pickle
   object graph → torch tensor/storage semantics — each with a long tail of
   edge cases. Difficulty dials without changing the task statement.
3. **Reference implementation defines the format but is banned at runtime.**
   Tests "understand the thing" vs. "call the thing."

The task is inherently **agentic**: first implementations never work; the
measured skill includes the hexdump-diff-fix debug loop.

---

## 2. PTH-Read: task statement (as given to the model)

> You are given a directory of PyTorch `.pth` checkpoint files. Produce a
> converter in **{Go | Rust | C++}** using **only the language's standard
> library and toolchain** that, for each input file, emits:
>
> 1. `manifest.json` — every tensor's name, shape, stride, dtype, storage key,
>    storage offset, and byte length (canonical schema below).
> 2. `tensors/<name>.bin` — the tensor's elements, materialized contiguous
>    row-major, little-endian, **in the original dtype** (no upconversion),
>    bit-exact.
> 3. A JSONL warning stream for anything skipped or degraded.
>
> Constraints: no network; no Python; no subprocesses at runtime (your
> converter is one process); deterministic output. A grader CLI is available
> for the public fixture set.

Notes vs. the progenitor implementation (`pth_reader.cpp`):
- The container layer is **part of the task** (the progenitor shells out to
  `unzip`; here that is prohibited — runtime subprocess spawns fail the run).
- Dtype is preserved, not converted to f32 (bit-exactness would otherwise be
  destroyed for bf16/fp8, and conversion invites tolerance disputes).

### 2.1 Output canonicalization
- `manifest.json`: UTF-8, keys sorted lexicographically, integers only for
  numeric fields. Quantization parameters (T4 qtensors) encoded as IEEE-754
  hex strings, never decimal floats.
- Tensor files named by tensor name with `/` → `__` escaping; escaping table
  fixed in the schema doc.
- Tied weights (two names, one storage) are materialized twice — one file per
  name. Aliasing is reported in the manifest (`storage_key` equality), which is
  how T3 grading detects that the model *understood* sharing.

---

## 3. Language rotation

**Languages:** Go, Rust, C++ (C++20). Pinned toolchains per release
(e.g., Go 1.24.x, Rust stable 1.8x, gcc 14/clang 18 — exact pins in the
harness lockfile).

**"Stdlib only"** per language — with one deliberate asymmetry:

| layer | Go | Rust | C++ |
|---|---|---|---|
| zip parsing | `archive/zip` free | hand-rolled | hand-rolled |
| DEFLATE | `compress/flate` free | hand-rolled | hand-rolled |

Torch writes zip entries **STORED (uncompressed)**, so T1–T3 need no inflate
in any language. DEFLATE appears only in T4 (`rezipped` fixtures, simulating a
user re-zipping a checkpoint), where hand-rolling inflate is intended
archaeology for Rust/C++ and a stdlib call for Go. Scores are **reported
per-language and never silently averaged across this boundary**; the T4
notes call out the asymmetry.

**Rotation protocol:** full cross product — every (model × language ×
condition × fixture-set) cell runs. Per-language variance is a finding
(transferable skill vs. memorized-C++), not noise to be averaged away.

---

## 4. Conditions

1. **Open-book (default).** In-container docs: CPython pickle opcode reference
   (pickletools-derived table), PKWARE APPNOTE (zip), IEEE-754
   binary16/bfloat16/fp8 tables. **Never torch source or serialization.py** —
   the torch conventions layer (persistent-ID tuple shape, storage classes,
   `_rebuild_tensor_v2` signature) has no official spec and stays closed-book
   in all conditions. That layer must come from priors or fixture archaeology.
2. **Closed-book.** No docs. Isolates recall + inference from evidence.
3. **Delta ("pth-prime").** A one-page spec of deviations from stock pth
   (changed zip magic, reordered persistent-ID tuple fields, one renamed
   storage class, one new dtype tag). Fixtures regenerated accordingly by a
   patched writer. Verbatim-recalled readers fail; spec-grounded ones pass.
   **Memorization gap** = stock score − delta score, reported per model. This
   is a headline metric, not a footnote.

---

## 5. Tiers

| tier | name | contents | pass criterion |
|---|---|---|---|
| T1 | Metadata | names/shapes/dtypes/strides from flat f32/f16 state_dicts | manifest exact |
| T2 | Extraction | bit-exact bytes, contiguous tensors, stored zip, protocol 2 | manifest + bytes exact |
| T3 | Memory semantics | shared storages (tied weights), views with nonzero offsets, transposed/sliced strides, 0-dim scalars, zero-size tensors | manifest + bytes exact, aliasing correctly reported |
| T4 | Archaeology | bf16/f64/int family/bool/fp8; quantized tensors (`_rebuild_qtensor`, scales/zero-points); pickle protocol 4 (FRAME/MEMOIZE/STACK_GLOBAL); zip64 (>4 GiB archives); DEFLATE-rezipped; legacy pre-1.6 inline-storage stream; multi-torch-version fixtures; whole-module pickles referencing unavailable user classes → **graceful skip with structured warning, extract the extractable** | per-category; graceful-degradation behavior scored |
| T5 | Resource envelope | 10 GB checkpoint, cgroup RAM cap ≈ 1.2 × largest tensor, wall-clock ceiling; includes a huge *transposed* view (forces chunked gather, defeats extract-then-read and full-file mmap-materialize) | exact output within caps |
| T6 | Adversarial | truncated at every layer boundary; memo cycles; deep nesting; pickle bombs; opcodes that trigger exec in naive unpicklers (GLOBAL/REDUCE to os.system etc.); garbage central directories | never crash, never hang (watchdog), never spawn/exec; correct structured refusal |

Scoring granularity: pass/fail **per fixture**; partial credit only via
category composition within a tier, never within a fixture (no near-miss
inflation).

---

## 6. Fixture generator

Procedural, seeded, deterministic: fixture = f(axes tuple, seed).

**Axes:**
1. Container: stored zip | zip64 | deflate-rezipped | legacy inline stream | truncated/corrupt (T6 only)
2. Pickle protocol: 2 (torch default) | 4. (5 reserved for a future delta.)
3. Object graph: flat state_dict | nested dicts | OrderedDict + `_metadata` |
   lists/tuples of tensors | mixed config dicts with python scalars
   (RVC-style; feeds the semantic-extraction category) | whole-module w/
   custom classes | memo-heavy | cyclic (T6)
4. Storage topology: 1:1 | shared storage | offset views | transposed strides |
   zero-size | 0-dim | single >4 GiB tensor
5. Dtypes: f32 f16 bf16 f64 i64 i32 i16 i8 u8 bool | fp8 e4m3fn/e5m2 | qint8
6. Scale: tiny (KB, dev loop) | medium (~200 MB) | huge (10 GB, T5)
7. Torch provenance: pinned generator containers (e.g., 1.13.x, 2.x latest,
   plus one pre-1.6 for legacy) — version drift in pickle layout is a fixture
   axis, not noise.

Byte order: little-endian only (torch's storage format); declared in
manifests; big-endian out of scope v1.

**Ground truth:** per-tensor sha256 + raw bytes for tiny/medium; for huge
fixtures, sha256 + deterministic sampled byte ranges (grader streams and
hashes, keeping the eval bundle small). Generators run only at
benchmark-build time; the eval harness never needs torch.

**Public/hidden split:** public dev set spans every axis value; hidden eval
set = same generator distribution, fresh seeds, plus hidden-only handmade
rarities. Hidden set rotates per release.

---

## 7. Harness

- One container per (language, condition): toolchain, public fixtures, docs
  (per condition), grader CLI (`nulltorch grade <dir>`) invocable freely.
- No network. **No Python binary anywhere in the image** (enforcement of
  "torchless" by absence). seccomp/audit: build phase may spawn (compilers);
  run phase must be a single process — any spawn fails the fixture.
- Determinism check: every eval fixture converted twice; outputs must be
  byte-identical.
- Budgeted agentic protocol: the model gets a wall-clock + token budget per
  (language, condition) cell; unlimited self-grading iterations on public
  fixtures within budget. Primary score: hidden-set state at budget
  exhaustion. Logged curve: time-to-first-green per tier.

**Metrics:** per-tier × per-language × per-condition pass rates; macro
profile (never a single collapsed number in the headline table);
memorization gap (stock − delta); iterations-to-green; wall/token cost; T5
peak RSS; T6 crash/hang/exec count (target: zero); determinism violations.

---

## 8. NullTorch-Board: the dashboard benchmark (human-judged companion)

The results website is itself a benchmark task — the suite's deliberately
human-judged member, dogfooding real NullTorch output.

**Task:** given `results.json` (schema: models × languages × tiers ×
conditions × fixtures; iteration time-series; costs) and a design brief,
build a **static, self-contained, offline** results explorer (single HTML
file or small static bundle; no backend; no external requests; loads
`results.json` from disk).

**Required analytical views** (functional, mechanically checkable):
1. Leaderboard with **tier profiles** (sortable/filterable by language and
   condition — never one collapsed score as the only view)
2. Model detail: tier × language heatmap; **memorization-gap** visualization
   (stock vs. delta); iterations-to-green curves; cost-vs-score scatter
3. Fixture drill-down: failing categories, linked warning streams
4. Two-model side-by-side comparison
5. Reproducibility strip: harness version, fixture-set hash, container digests

**Scoring — hybrid gate + rubric:**
1. **Mechanical gate (must pass before judging):** renders offline with no
   console errors; handles provided datasets including degenerate ones (empty
   results, 1 model, 50 models); DOM assertions on a known dataset verify the
   displayed numbers match the JSON; keyboard navigable; no horizontal page
   scroll at common widths.
2. **Human rubric:** 6 anchored 1–5 dimensions — information hierarchy (can a
   judge answer "which model is best at T3-Rust?" in <10 s), data-ink honesty
   (no truncated-axis flattery; profiles not misleadingly collapsed),
   interaction quality, visual/aesthetic coherence (incl. dark/light),
   responsiveness, accessibility (contrast, ARIA, focus order).
   Judging is **blind** (anonymized submissions, randomized order), ≥3
   judges, inter-rater agreement (Krippendorff's α) reported alongside scores.
3. Optional research byproduct: an LLM judge scores the same rubric;
   human-vs-LLM judge correlation on *design* quality is published as a
   secondary finding.

Contamination note: dashboards are memorization-friendly; the anchor is the
NullTorch-specific schema and required views (memorization-gap plots and tier
profiles have no public template to recall).

---

## 9. Suite extensions (post-v1)

Same shape — "read X without the library that defines X," mechanical oracle:
inflate without zlib · `.npy`/`.npz` without numpy · git packfiles without
git · SQLite file reader · PNG decoder without libpng. PTH-Read stays the
flagship: only member with three stacked layers **and** a security dimension
(T6 is a safe-unpickler test by construction).

---

## 10. Build order

"Everything" in dependency order, each phase shippable:

1. Fixture generator + manifest schema + grader; T1–T3 stock fixtures;
   three language containers; open-book docs bundle.
2. Delta writer + delta fixtures; closed-book condition; memorization-gap
   reporting.
3. T4 (archaeology) → T6 (adversarial) → T5 (resource; needs cgroup
   plumbing and huge-fixture streaming ground truth).
4. Board: results schema freeze, sample datasets (incl. degenerate), design
   brief, mechanical gate tests, rubric + blind-judging procedure.
5. Calibration: baseline runs across 3–4 models; tune tier composition so the
   profile discriminates (target: T1 near-saturated, T3/T4 spread, T5/T6
   headroom); then freeze fixture-set v1 and publish.

## 11. Known risks / open items

- **fp8/qtensor fixture stability** across torch versions — pin generators
  hard; drop a dtype from v1 rather than ship flaky ground truth.
- **T5 grading at 10 GB** — sampled-hash ground truth must be designed so
  streaming converters can't skip unsampled regions (secret sample offsets,
  rotate per release).
- **C++ "stdlib only" edge**: no `<zip>`, no hashing lib — sha256 for
  self-checks isn't needed by the contract (grader hashes), keep it that way.
- **Judge pool** for Board — operators double as judges v1; document the
  conflict and randomize which runs they see.
- Name is a working title.
