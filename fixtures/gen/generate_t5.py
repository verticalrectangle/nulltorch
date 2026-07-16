#!/usr/bin/env python3
"""NullTorch T5 (resource envelope) generator — modest-size runnable instance.

Ground truth for T5 is deliberately SMALL: per-tensor sha256 + secret sampled
byte-ranges (with sub-hashes), never the full bytes. The generator streams
each tensor's contiguous bytes in chunks so it never holds a whole tensor —
mirroring the streaming a compliant converter must do. Real releases use
multi-GB fixtures on calibration hardware (see fixtures/T5_DESIGN.md); this
script defaults to a few MB so the streaming + sampled-hash path is testable
here.

The full sha256 already forces byte-exact output; the sampled ranges are the
belt-and-suspenders check the grader can run without storing full bytes, and
their offsets are seed-derived so they cannot be special-cased.

Usage:
  python3 generate_t5.py --out ../t5 --seed 5001 --mb 8
"""

import argparse
import hashlib
import io
import json
import os
import sys
import zipfile

import torch

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(_HERE, "..", "..", "grader"))
import schema  # noqa: E402
from generate import (TORCH_DTYPE_TOKEN, oracle_read, sha256_file,  # noqa: E402
                      tensor_bytes, walk)

CHUNK = 1 << 20  # 1 MiB streaming chunks


def stream_contiguous(t: torch.Tensor):
    """Yield contiguous little-endian byte chunks without materializing the
    whole tensor at once (chunk over the outermost dim).

    NOTE: a contiguous slice's .untyped_storage() is the PARENT storage, so we
    must .clone() each block (tensor_bytes does) to get slice-sized bytes; and
    the row step is derived from shape, not from storage size."""
    c = t.detach()
    if c.ndim == 0 or c.numel() == 0:
        yield tensor_bytes(c)
        return
    rows = c.shape[0]
    per_row = (c.numel() // rows) * c.element_size()
    step = max(1, CHUNK // max(1, per_row))
    for i in range(0, rows, step):
        yield tensor_bytes(c[i:i + step])


def hash_and_sample(t: torch.Tensor, seed_words, itemsize, nbytes):
    """Single pass: full sha256 + sampled sub-range hashes."""
    # derive up to 4 secret ranges from a seed (offset,len) within nbytes
    ranges = []
    if nbytes > 0:
        rng = int.from_bytes(hashlib.sha256(
            b"|".join(seed_words)).digest()[:8], "little")
        n_ranges = min(4, max(1, nbytes // (64 * 1024) + 1))
        for i in range(n_ranges):
            rng = (rng * 6364136223846793005 + 1442695040888963407) & (2**64 - 1)
            ln = min(nbytes, itemsize * (1 + (rng >> 20) % 4096))
            off = (rng % max(1, nbytes - ln + 1))
            off -= off % itemsize                      # align to element
            ranges.append([off, ln])
    ranges = sorted({tuple(r) for r in ranges})

    full = hashlib.sha256()
    subs = [hashlib.sha256() for _ in ranges]
    pos = 0
    for chunk in stream_contiguous(t):
        full.update(chunk)
        c0, c1 = pos, pos + len(chunk)
        for k, (off, ln) in enumerate(ranges):
            a, b = max(off, c0), min(off + ln, c1)
            if a < b:
                subs[k].update(chunk[a - c0:b - c0])
        pos = c1
    assert pos == nbytes, (pos, nbytes)
    return full.hexdigest(), [
        {"off": off, "len": ln, "sha256": subs[k].hexdigest()}
        for k, (off, ln) in enumerate(ranges)]


def build(seed, mb):
    # Runnable T5 = big CONTIGUOUS streaming (the memory-envelope case): a
    # compliant reader streams these under a mem cap instead of slurping. The
    # giant-transposed strided-gather-at-size case is t5_giant_transposed in
    # fixtures/T5_DESIGN.md (calibration hardware) — kept out of the runnable
    # instance so a naive O(n) Python gather doesn't dominate the smoke.
    torch.manual_seed(seed)
    n = (mb * (1 << 20)) // 4 // 2          # split budget across two tensors
    return {
        "big.flat": torch.randn(n),                      # f32 contiguous
        "big.2d": torch.randn(n // 256, 256),            # f32 contiguous 2-D
        "half.vec": torch.randn(n).to(torch.float16),    # f16 contiguous
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=5001)
    ap.add_argument("--mb", type=int, default=8)
    args = ap.parse_args()

    fid = f"t5_stream__s{args.seed}"
    fdir = os.path.join(args.out, fid)
    os.makedirs(os.path.join(fdir, "gt"), exist_ok=True)
    graph = build(args.seed, args.mb)
    pth = os.path.join(fdir, "fixture.pth")
    torch.save(graph, pth)

    tensors, _ = oracle_read(pth)
    manifest = {"nulltorch_manifest": schema.SCHEMA_VERSION,
                "byteorder": "little", "tensors": tensors}
    assert not schema.validate_manifest(manifest), \
        schema.validate_manifest(manifest)

    hashes, samples = {}, {}
    for path, leaf in walk(graph):
        if not isinstance(leaf, torch.Tensor):
            continue
        token = TORCH_DTYPE_TOKEN[leaf.dtype]
        itemsize = schema.DTYPE_TOKENS[token]
        nbytes = tensors[path]["nbytes"]
        full, ranges = hash_and_sample(
            leaf, [path.encode(), str(args.seed).encode()], itemsize, nbytes)
        bn = schema.tensor_bin_name(path)
        hashes[bn] = {"sha256": full, "size": nbytes}   # NO gt bytes stored
        samples[bn] = ranges

    with open(os.path.join(fdir, "gt", "manifest.json"), "w") as f:
        f.write(schema.canonical_dumps(manifest))
    with open(os.path.join(fdir, "gt", "hashes.json"), "w") as f:
        f.write(schema.canonical_dumps(hashes))
    with open(os.path.join(fdir, "gt", "samples.json"), "w") as f:
        f.write(schema.canonical_dumps(samples))
    total = sum(t["nbytes"] for t in tensors.values())
    caps = {"mem_cap_bytes": int(max(t["nbytes"]
                                     for t in tensors.values()) * 1.2)}
    with open(os.path.join(fdir, "meta.json"), "w") as f:
        f.write(schema.canonical_dumps({
            "fixture_id": fid, "tier": "T5", "recipe": "t5_stream",
            "seed": args.seed, "torch_version": torch.__version__,
            "container": "zip_stored", "pickle_protocol": 2,
            "grading": "byte_sampled", "sha256_pth": sha256_file(pth)[0],
            "n_tensors": len(tensors), "total_tensor_bytes": total, **caps}))
    print(f"emitted {fid}: {len(tensors)} tensors, "
          f"{total / (1<<20):.1f} MiB of tensor data, gt is hashes-only")


if __name__ == "__main__":
    main()
