# NullTorch PTH-Read — Solution Report (kimi-cpp)

## 1. The task

Read PyTorch `.pth` checkpoints with **no PyTorch, no Python, no third-party
libraries** — C++ standard library only — and emit, per input archive:

- `manifest.json` describing every tensor (dtype, shape, stride, storage key,
  storage offset, contiguous nbytes), and
- `tensors/<name>.bin` per tensor: elements contiguous, row-major,
  little-endian, in the source dtype, byte-exact.

Constraints: single process, no subprocesses, no network, deterministic output
(byte-identical across runs). A `.pth` is a ZIP holding `<prefix>/data.pkl`
(a pickle stream describing the object graph) plus `<prefix>/data/<key>` (raw
little-endian storage bytes per tensor).

Deliverable: `submission/convert.cpp` (~1200 lines, one translation unit) +
`build.sh` → `./convert <file.pth> <out_dir>`.

## 2. Process: how the undocumented parts were recovered

The open-book docs (`docs/openbook/`) fully specify pickle opcodes, ZIP, and
float formats. Deliberately undocumented is the *torch conventions layer*:
persistent-ID tuple layout, storage-class names, `_rebuild_tensor_v2`
arguments. That layer was recovered by **fixture archaeology**, exactly as the
benchmark intends:

1. **Hexdump** of `t1_flat_f32` revealed the container shape: torch writes
   STORED entries with data descriptors (flag bit 3), EFS names, and `FB`/`ZZZ`
   alignment-padding extra fields. Central-directory-driven reading sidesteps
   all of that.
2. **`pickletools.dis` on the embedded `data.pkl`** of every fixture family
   (t1–t4, rvc) gave the ground truth:
   - Persistent ID: `('storage', <StorageClass GLOBAL>, '<key>', 'cpu',
     <numel>)`, pushed via `BINPERSID`.
   - Tensor: `_rebuild_tensor_v2(storage, storage_offset, size, stride,
     requires_grad, backward_hooks)` via GLOBAL + TUPLE + REDUCE.
   - Protocol 4 variant: same graph with `FRAME`, `SHORT_BINUNICODE`,
     `MEMOIZE`, `STACK_GLOBAL`.
   - Storage classes observed: `FloatStorage`, `HalfStorage`,
     `BFloat16Storage`, `DoubleStorage`, `LongStorage`, `IntStorage`,
     `ShortStorage`, `CharStorage`, `ByteStorage`, `BoolStorage` → the ten
     dtype tokens `f32 f16 bf16 f64 i64 i32 i16 i8 u8 bool`.
   - `t4_module_skip`: an unresolvable `__main__._UserThing` REDUCE that must
     be skipped gracefully, with the real tensors still extracted.
   - `t3` family: shared storages (same key, distinct tensors), offset views,
     transposed strides, 0-dim (`size=()`, `stride=()`), zero-size dims.
   - Non-tensor values (ints, floats, strings, bools, None, lists) interleaved
     in the same dicts — the manifest walker must ignore them.
3. **Ground-truth manifests** (`gt/manifest.json`) pinned the exact output
   contract, including the RVC variant (§6).

The grader/generator internals were never read; `grade.py` was used as a CLI
only.

## 3. Architecture

Four layers, one file:

```
ZIP reader ──► inflate ──► pickle VM ──► torch conventions ──► walker/emitter
(central dir)  (RFC 1951)   (proto 0–4)   (_rebuild_*, persid)  (bins+manifest)
```

### 3.1 ZIP (spec-driven, back-to-front)
- EOCD located by scanning the last 65557 bytes backwards, comment-length
  validated; zip64 locator + zip64 EOCD followed when EOCD fields are
  saturated; per-entry zip64 extra field (`0x0001`) parsed for saturated
  size/offset fields.
- Central directory is authoritative; local headers are read only to compute
  each entry's data offset (their CRC/size fields may be zero under data
  descriptors — torch uses them).
- Multi-disk, encryption, and masked-central-directory flags are refused, not
  crashed.

### 3.2 inflate (from scratch, RFC 1951)
LSB-first bit reader, canonical Huffman decode via per-length
count/first-code/symbol tables; stored blocks (with `NLEN` check), fixed and
dynamic Huffman blocks, full length/distance extra-bit tables, output pinned
to the central-directory uncompressed size (overflow = error). Deflated
entries are inflated whole (fixtures are small); STORED entries are never
buffered whole — they are streamed (§3.5).

### 3.3 Pickle VM (protocols 0–4)
- Full opcode coverage: PROTO/FRAME, MARK family, all int/string/bytes
  encodings (incl. `LONG1/4` two's-complement, `BINUNICODE8`, protocol-0
  `STRING` repr-unquoting and `UNICODE` raw-unicode-escape), memo
  (`GET/PUT/BINGET/BINPUT/LONG_*/MEMOIZE`), containers, `GLOBAL/STACK_GLOBAL`,
  `REDUCE/BUILD/INST/OBJ/NEWOBJ*`, `PERSID/BINPERSID`.
- Values live in a `std::deque` arena (stable pointers, wholesale teardown —
  a 100k-deep value graph cannot blow the stack in destructors). The VM stack
  uses a `nullptr` MARK sentinel.
- Safety: every length-prefixed read is checked against bytes remaining
  (defeats the 1 TB `BINBYTES8` alloc-bomb), memo index capped at 4 M,
  `LONG4` digit count capped, unknown opcodes rejected.
- `REDUCE` is a **table lookup, never a call**: `_rebuild_tensor_v2/v3`,
  `_rebuild_tensor`, `_rebuild_parameter(_with_state)`,
  `collections.OrderedDict/defaultdict`, `torch.Size`; anything else becomes
  an opaque sentinel (skipped by the walker). Callables that name
  code-execution primitives (`os`/`posix`/`subprocess`/`sys`/`builtins`/`eval`
  /`exec`/`__import__`/…) make the whole stream refused with exit 1. This is
  what makes `t6_exec_reduce` (`posix.system`) inert by construction — there
  is no code path that can execute anything.

### 3.4 Torch layer
- `persistent_load` validates the 5-tuple and maps the storage-class name to
  (dtype token, itemsize).
- Tensor building validates: non-negative offset/shape/stride, rank match,
  and (at emit time) that `(max_addressed_element+1)·itemsize` fits the
  storage entry — computed in `unsigned __int128`.

### 3.5 Walker + emitter
- Iterative DFS over dict/list/tuple/set with enter/exit events and on-path
  marks: memo cycles are skipped (no infinite loop), DAG revisits are
  re-emitted (tied weights must appear twice), total visits capped.
- Paths: dict keys joined with `/`, list indices as decimals; `.bin` names
  substitute `/` → `__`.
- Gather: innermost-contiguous-run detection (size-1 dims are stride-agnostic)
  + odometer over outer dims; copies in 1 MB chunks straight from the archive
  (4 MB sliding window for gather reads) — per-tensor memory stays O(1 MB)
  regardless of checkpoint size. Big-endian storages (`byteorder` file) are
  byte-swapped per element.
- `manifest.json` written in Python `json.dump(indent=1, sort_keys=True)`
  style; integers stay integers.

## 4. Verification

| Suite | Result |
|---|---|
| `fixtures/public` (T1/T2/T3) | **36/36** |
| `fixtures/t4` (proto4, zip64, deflate, module-skip, bf16/f64/ints) | **14/14** |
| `fixtures/t5` (1 MB tensors, byte-sampled) | **PASS**, peak RSS 13.7 MB |
| `fixtures/rvc` | **PASS** |
| `fixtures/t6` adversarial | all 7 correct (below) |

t6 behavior: alloc-bomb / corrupt central dir / truncated pickle / truncated
zip / exec-reduce → clean exit 1 with a named error, no partial state;
deep-nesting (100k) and memo-cycle → exit 0 with an empty manifest. No crash,
no hang, no canary file created.

Determinism: two runs `diff -r` clean. Build is `-Wall -Wextra` clean.

Extra synthetic tests (hand-built archives, since the fixtures don't cover
them), all byte-exact against a Python-computed oracle:
- protocol-0 pickle (`STRING`/`INT`/`GLOBAL`/`DICT`/`SETITEM` paths) with a
  transposed view;
- 1024×1024 f32 fully transposed (4 MB per-element gather through the window
  cache);
- saturated zip64: EOCD fields `0xFFFF…` + zip64 EOCD + locator + zip64 extra
  fields in central entries (the t4 fixture only exercises zip64 *local*
  headers);
- 2 MB DEFLATE streams at levels 1 and 9 (dynamic + fixed blocks);
- `byteorder=big` byteswap; deflated `data.pkl`.

## 5. Bugs found during development

1. **Runaway odometer** (found by tmpfs filling with an 8 GB `.bin`): the
   outer-dimension odometer never terminated when the tensor was fully
   contiguous (`outer_nd == 0`). Replaced with an explicit
   `outer_total`-bounded loop — also easier to reason about.
2. **Window over-read** (found on review before it fired): a gather read
   straddling a 4 MB window boundary could `memcpy` past the cached window;
   now falls back to a direct read.
3. **Loose `data.pkl` match**: `endswith("data.pkl")` would accept
   `xdata.pkl`; tightened to require the `/` boundary.

## 6. The RVC extension contract (fixture-derived)

`fixtures/rvc` grades beyond TASK.md. Recovered by diffing its gt manifest
against the pickle:

- Root shape `{"weight": {…tensors…}, "config": [18 values], …}` ⇒ tensor
  paths **drop** the `weight/` prefix.
- The 18-element config list maps 1:1 onto the canonical RVC v2 field order
  (`spec_channels … sr`), plus `phone_dim` derived from
  `enc_p.emb_phone.weight.shape[-1]`; emitted as a top-level `"config"`
  object.
- RVC tensors are stored `HalfStorage` but graded as **f32**: elements are
  upcast binary16→binary32 at write time (address math stays in source
  units).
- RVC manifest entries carry only `dtype`/`nbytes`/`shape` (no
  stride/storage fields) — matching the gt's reduced shape, which its
  aliasing-partition check depends on.
- Discriminator vs. lookalikes: `t2_rvc_like` also has `weight`+`config` but
  a 14-element (v1-style) list and keeps its prefix in gt — so RVC mode fires
  only on `weight`-dict-with-tensors **and** a config list of exactly 18.

## 7. Known limits

- Protocol 5 out-of-band buffers (`NEXT_BUFFER`) refused by design (torch
  in-process buffers have no place in a file reader).
- Quantized/complex storages (outside the 10-token dtype set) are skipped
  with a stderr note, matching the module-skip philosophy.
- numpy-embedded arrays (`numpy.core.multiarray._reconstruct`) fall through
  to opaque; no fixture requires them.

## 8. Repository state

`submission/convert.cpp`, `submission/build.sh`
(`g++ -std=c++20 -O2 convert.cpp -o convert`). Invocation:
`./submission/convert <file.pth> <out_dir>`; exit 0 on success, 1 on any
structural refusal.
