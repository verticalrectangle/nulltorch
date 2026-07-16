# NullTorch harness protocol

Two layers:

- **Orchestrator** — spawns a model *agent* inside a cell, drives its agentic
  debug loop within a budget, records the iteration/token time-series, and
  captures the final artifact. (`orchestrator` below; reference driver:
  `harness/orchestrate.py`, spec in this doc.)
- **Runner** — language-agnostic; executes a *submission command* over a
  fixture set, grades it, and emits board-schema `results.json`. Already
  implemented: `harness/run.py`. The orchestrator calls the runner for the
  final hidden-set grade.

## Cell = (model × language × condition)

Every cell runs in its own container built from the language's Containerfile
(`containers/`). One agent, one budget, one artifact, one hidden-set score.

### Container contents
- Pinned toolchain (digests recorded in `results.json.container_digests`)
- `/fixtures/public/` — public fixtures WITH ground truth (self-grading is
  allowed and expected)
- `/task/TASK.md` — the task statement (SPEC §2) rendered for the cell's
  language
- `/docs/openbook/` — **open-book condition only**
- `/delta/DELTA_SPEC.md` — **delta condition only**
- `nulltorch-grade` — the self-grade RPC (below). Grading logic lives in the
  harness sidecar; no Python exists in the model's runtime image.

## Orchestrator: the agent loop

The orchestrator spawns the model as an autonomous agent whose job is to
produce a converter that passes the public fixtures, then submit it. It is a
thin driver — the intelligence is the model's; the orchestrator only mediates
tools, budget, and telemetry.

### 1. Spawn
Instantiate the model agent with:
- **System prompt** = `/task/TASK.md` + the cell's condition docs list + the
  tool contract below. No hidden fixtures, no ground-truth internals of the
  oracle, no torch.
- **Working dir** = `/work` (writable scratch inside the container).
- **Model params** = the cell's model id and decoding config (pinned per
  release).

### 2. Tool surface (what the agent may call)
- `shell(cmd)` — run a build/test command in the container. Build-phase
  process spawns are allowed (compilers). Subject to per-call and cumulative
  wall limits.
- `read_file` / `write_file` — edit its converter sources under `/work`.
- `nulltorch_grade(dir)` — **self-grade RPC**. Runs the sidecar grader over
  `/fixtures/public` against the artifact the agent points at, returns the
  same per-fixture verdicts + per-tier tallies a real grade produces. This is
  how the agent gets its hexdump→fix→regrade loop. Every call is a telemetry
  checkpoint (see §4). Rate-limited to prevent grade-spamming as a
  search oracle over hidden behavior (it only ever sees public fixtures).
- No network tool. No package-install tool. (Stdlib-only is also enforced at
  build time — see Enforcement.)

### 3. Agentic loop
The agent iterates freely: write converter → `shell` build → `nulltorch_grade`
→ inspect failures → edit → repeat. The orchestrator does not steer content;
it only:
- meters each tool call's wall-time and the agent's token usage,
- enforces the budget (§5),
- snapshots telemetry on each `nulltorch_grade` call (§4),
- persists every artifact the agent marks as a submission (last one wins).

Termination when the FIRST of these trips: budget exhausted; the agent calls
`submit()` and then stops; or the agent yields with no further tool calls.
The **submission artifact** = the converter sources + build command from the
last `submit()` (or the last state if none). 

### 4. Telemetry → board schema
Each `nulltorch_grade` call appends one entry to the run's `iterations[]`:
`{ t_seconds: wall-since-spawn, tier_passes: {T1..T6: <public pass count>} }`
— giving the iterations-to-green curves the board plots. Token spend is
sampled continuously into `spent.tokens`; `spent.wall_seconds` is total cell
wall time. `budget` holds the cell's caps. (Public-set pass counts drive the
curve; the authoritative score is the hidden re-grade in §6.)

### 5. Budget & enforcement
- **Budget** per cell: wall-clock + tokens (both recorded). Reaching either
  cap ends the agent loop immediately; the last artifact is submitted as-is.
- **No network**: container netns has no egress.
- **No Python in the runtime image**: the model cannot shell out to torch or
  a stock unpickler; `nulltorch-grade` is an RPC to the sidecar, not an
  in-container interpreter.
- **Stdlib-only**: enforced at build time per language — Go `go build` with a
  vendored-empty module cache; Rust `cargo build --offline` with an empty
  registry (any `[dependencies]` fails); C++ `g++ -std=c++20` with no `-I/-l`
  beyond the toolchain. A build that pulls a third-party dep fails the cell.
- **Runtime is single-process**: the submitted converter runs under seccomp
  notify; any `fork`/`execve` at convert time fails the fixture
  (`RUNTIME_SPAWN`). (Distinct from build phase, which may spawn.)

### 6. Final scoring — hidden set, no agent
The agent NEVER sees the hidden set. After the loop ends, the orchestrator
hands the submission artifact to the **runner** (`run.py`), which:
1. builds it once in a fresh container instance (build phase),
2. runs it over `/hidden-fixtures` (mounted read-only; disjoint seeds + rare
   hand-made cases) with the runtime enforcement above,
3. grades byte-tiers exactly, policy-grades T6, and
4. writes the run's authoritative `tiers`, `determinism_violations`,
   `t5_peak_rss_bytes`, and `t6_incidents` into `results.json`.

The public iteration curve (§4) and the hidden final score are both reported;
a large gap between them flags overfitting to the public fixtures.

## Runner specifics (implemented in run.py)
- **Determinism**: every byte-graded fixture is converted twice; outputs must
  be byte-identical or the fixture fails and `determinism_violations`
  increments.
- **T5**: run under a cgroup memory cap (`meta.mem_cap_bytes`) and wall
  ceiling; peak child RSS sampled into `t5_peak_rss_bytes`. Ground truth is a
  streamed full sha256 (+ optional sampled ranges); OOM-kill = fail.
- **T6**: run under a watchdog with an exec canary. `pass` iff no incident —
  no crash (killed by signal), no hang (timeout), no execution (canary file
  appears). Incidents tallied into `t6_incidents {crashes, hangs,
  exec_attempts}`. Partial-output correctness is not scored in v1.
- **Accumulation**: `run.py` merges each cell into a shared `results.json`,
  replacing any prior run with the same (model, language, condition).

## Language rotation
Full cross product (Go, Rust, C++). Go's stdlib includes zip + DEFLATE
(`archive/zip`, `compress/flate`); Rust and C++ do not — the T4
`rezip_deflate` fixtures are where that asymmetry bites, by design. Scores are
reported per-language and never averaged across languages in headline tables
(SPEC §3).

## Conditions
`open_book` (docs bundle mounted) · `closed_book` (no docs) · `delta`
(`DELTA_SPEC.md` mounted, delta fixtures). Memorization gap = stock − delta,
derived per (model, language) at report time — never stored (board §8).

## Reference driver status
`run.py` (runner) is implemented and validated end-to-end against the stdlib
reference converter (T1–T6 + T5). `orchestrate.py` (agent driver) is specified
here but not implemented — it is model-runtime-specific (which agent SDK
spawns the model, how tokens are metered). The seam is deliberately narrow:
the orchestrator only has to produce a submission artifact + an `iterations[]`
series; `run.py` already turns an artifact into an authoritative scored run.
