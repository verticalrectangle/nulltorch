# T5 — resource envelope (design; fixtures generated at calibration time)

T5 tests that a converter streams instead of slurping: convert a very large
checkpoint under a memory cap and a wall-clock ceiling. Fixtures are NOT
committed (multi-GB); they are generated on calibration hardware from this
spec, and only their sampled-hash ground truth (small) is stored.

## Fixtures
1. `t5_big_flat` — ~10 GB, many medium fp16 tensors, 1:1 storages. Baseline
   throughput; extract-then-read (temp-dir explode) blows the disk/RAM budget.
2. `t5_giant_transposed` — one >4 GB tensor stored transposed (non-contiguous
   stride). Contiguous materialization requires a strided gather; a reader
   that mmaps the storage and memcpys blindly produces wrong bytes, and one
   that loads the whole storage to transpose blows the RAM cap. Forces a
   chunked, stride-aware gather.
3. `t5_many_tied` — heavy storage sharing at scale: materializing each alias
   independently is correct but a reader that also re-reads the shared storage
   per alias thrashes I/O; tests that sharing is exploited, not just handled.

## Caps (recorded per fixture in meta.json)
- `mem_cap_bytes` ≈ 1.2 × largest single tensor's contiguous size.
- `wall_ceiling_s` set from a reference streaming implementation × 3.
- Harness runs under a cgroup memory limit; peak RSS sampled and reported
  (`t5_peak_rss_bytes`). OOM-kill = fixture fail.

## Ground truth (small, committed)
Per tensor: dtype/shape/stride/storage_key/offset/nbytes (as normal), plus
`sha256` of the full contiguous materialization. The generator computes this
by STREAMING the tensor in chunks (never holding it whole), so ground truth
stays tiny regardless of tensor size — an 11 MiB fixture's gt is ~12 KiB.
The grader streams the submission's tensor output and compares the full
sha256; that is the authoritative, byte-exact check.

`sample_ranges` (secret [offset,len) windows with sub-hashes, seed-derived
per release) are ALSO emitted and checked, but they are belt-and-suspenders,
not load-bearing: a full streamed sha256 already covers every byte, so there
are no "unsampled regions" to special-case. Sampling would only become the
primary signal if a full hash were deliberately withheld (not the case here).
Kept because it is cheap and makes partial-verification tooling possible.

## Generation procedure (calibration host)
1. Build graphs per recipe with a fixed seed; `torch.save` (stored zip).
2. Compute full sha256 + sampled sub-hashes by streaming each tensor's
   contiguous bytes (never hold >1 tensor in RAM in the generator either).
3. Emit meta + gt manifest + hashes; discard the multi-GB `.pth` after
   hashing, or host it out-of-band (HF) referenced by sha256 in meta.

## Why not committed
Repo stays small and clonable; the expensive artifacts live on calibration
hardware / object storage, reproducible from this spec + the recorded seed.
