# NullTorch task — PTH-Read (C++ cell)

Write a program that reads PyTorch `.pth` checkpoint files **without PyTorch,
without Python, and without any third-party library** — C++ standard library
and the C++ toolchain only.

## What a `.pth` is
A `.pth` is a ZIP archive. Inside, under a single top-level directory, are:
- `<prefix>/data.pkl` — a Python **pickle** stream describing the object graph
  (usually a dict of tensor-name → tensor).
- `<prefix>/data/<key>` — raw little-endian storage bytes for each tensor
  (`<key>` is a decimal string).

torch writes ZIP entries **STORED (uncompressed)**, so you do not need an
inflate implementation for this cell. A tensor references a storage by key, a
storage offset (in elements), a shape, and a stride — tensors can be
non-contiguous views (transposed / sliced) and several tensors can share one
storage. You must materialize each tensor **contiguous, row-major** from its
storage using its offset+shape+stride.

## Output contract
For each input `<file.pth>`, given an output directory `<out>`, emit:

1. `<out>/manifest.json`:
```
{
  "nulltorch_manifest": 1,
  "byteorder": "little",
  "tensors": {
    "<path>": {
      "dtype": "<token>",        // f32 f16 bf16 f64 i64 i32 i16 i8 u8 bool
      "shape": [ints],           // [] for 0-dim
      "stride": [ints],          // element strides as stored (torch order)
      "storage_key": "<key>",    // the data/<key> this tensor reads from
      "storage_offset": <int>,   // in elements
      "nbytes": <int>            // prod(shape) * itemsize (contiguous size)
    }, ...
  }
}
```
- `<path>` = dict keys / list indices joined with `/` (e.g. `enc.0.weight`,
  `stack/0`). Keys never contain `/`.
- Preserve the **source dtype** (do NOT convert). `nbytes` is the contiguous
  materialized size. JSON numeric fields must be integers.

2. `<out>/tensors/<name>.bin` for each tensor: its elements **contiguous,
   row-major, little-endian, in the source dtype**, byte-exact. `<name>` is the
   tensor path with `/` replaced by `__`.

Two tensors that share a storage are each materialized to their own `.bin`;
report the shared `storage_key` for both (aliasing is graded).

## Invocation
`./your_converter <file.pth> <out_dir>` — exit 0 on success. Read nothing from
the network; spawn no subprocesses at runtime (single process).

## Dtype ↔ storage class
The pickle references a storage class (e.g. `FloatStorage`, `HalfStorage`,
`LongStorage`). Map it to the dtype token above. The pickle protocol
(opcode-level) and ZIP container are documented in `docs/openbook/`. The torch
serialization conventions (the persistent-id tuple, storage class names, the
`_rebuild_tensor_v2` argument order) are **not** documented — recover them from
the docs' pickle model + the fixtures themselves.

## Self-grading
Public fixtures live in `fixtures/public/` (also `fixtures/t4/` for the harder
container/dtype cases). Each has `gt/manifest.json` and `gt/hashes.json` you may
read to check your work. Grade yourself:
```
python3 grader/grade.py <fixture_dir> <your_out_dir>     # one fixture
python3 grader/grade.py --set fixtures/public <subs_root> # whole set
```
`--set` expects `<subs_root>/<fixture_id>/manifest.json` per fixture.

Tiers, easiest → hardest: **T1** (metadata: names/shapes/dtypes/strides),
**T2** (bit-exact contiguous tensors), **T3** (shared storages, offset views,
transposed strides, 0-dim, zero-size). T4 (protocol-4 pickles, zip64, module
graceful-skip; deflate needs inflate — out of scope unless you implement it).

## Rules
- Standard library only. No third-party packages, no `unzip`/`python`/`torch`
  at runtime.
- Do not read the reference solution, the fixture generator/oracle, or the
  grader's internals to copy the algorithm — build it from this spec, the
  open-book docs, and the fixtures. (`grade.py` as a CLI is fine.)
- Deterministic output: two runs on the same input must be byte-identical.
