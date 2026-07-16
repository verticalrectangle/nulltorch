# NullTorch open-book documentation bundle

This directory is the **open-book docs bundle**: the reference material
mounted into the evaluation container in the *open-book* condition
(SPEC.md §4, condition 1). In the *closed-book* condition the container gets
none of it; in the *delta* condition it is provided together with the
one-page deviation spec. The bundle is identical for every language and
every model — it is part of the frozen benchmark surface, not a hint channel.

## Contents

| File | Layer | What it covers |
|---|---|---|
| `pickle_opcodes.md` | pickle | Complete opcode table (all protocols 0–5), generated from CPython's `pickletools`; stack machine, memo, and protocol-marker primer |
| `zip_format.md` | container | Local/central/EOCD record layouts with byte offsets, zip64, data descriptors, general-purpose flag bits, STORED + DEFLATE, CRC-32 |
| `float_formats.md` | dtypes | Bit-level layouts of binary16, bfloat16, float8 e4m3fn and e5m2, with special-value encodings and worked bit patterns |

## The deliberate boundary

Everything in this bundle documents formats with **public, official
specifications**: the pickle virtual machine is specified by CPython itself
(the table here is machine-generated from `pickletools`, not transcribed),
zip by PKWARE's APPNOTE and DEFLATE by RFC 1951, and the float formats by
IEEE 754 and the published FP8 conventions. Providing these tests whether a
model can *use* a spec, not whether it happens to have memorized one.

What is **never documented, in any condition**, is the PyTorch serialization
conventions layer: the persistent-ID tuple layout, the storage class names
and their dtype/width correspondence, the `_rebuild_tensor_v2` (and relatives')
argument signatures, and the archive-internal directory conventions. That
layer has **no official specification** — its only definition is the torch
source, which is banned from the container in every condition. Recovering it
from priors or from fixture archaeology (hexdumps, `pickletools`-style manual
disassembly of the embedded pickle, differential experiments across fixtures)
is part of what the benchmark measures. Do not "fix" this by adding a torch
conventions doc here; that would collapse the open-book condition into a
different, easier benchmark and destroy comparability across releases.

## Regenerating

`pickle_opcodes.md` must be regenerated (never hand-edited) from the
CPython version pinned for the benchmark release, so that the table is
authoritative for that interpreter. The other two files are hand-maintained
prose; changes to any file in this bundle are a benchmark-surface change and
require a fixture-set version bump.
