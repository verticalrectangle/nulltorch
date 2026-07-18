#!/usr/bin/env python3
"""Reference solution for the NullTorch LoRA-merge tier.

Stdlib only (no torch, no numpy). Reads a model checkpoint in safetensors
format plus a PEFT LoRA adapter, merges:

    W_merged = W + (alpha / rank) * (lora_B @ lora_A)

and writes the merged checkpoint as safetensors. A line-jsonl `warnings.jsonl`
is written with structured warnings for any skipped pairs.

The key mapping used here mirrors the real ACE-Step v1.5 scheme:

    adapter key  base_model.model.<layer>.lora_A.weight
                 base_model.model.<layer>.lora_B.weight
    target key   decoder.<layer>.weight
"""

import json
import os
import struct
import sys

DTYPE_INFO = {
    "F32": {"char": "f", "size": 4},
    "F16": {"char": "e", "size": 2},
    "F64": {"char": "d", "size": 8},
    "I32": {"char": "i", "size": 4},
    "I64": {"char": "q", "size": 8},
    "I16": {"char": "h", "size": 2},
    "I8": {"char": "b", "size": 1},
    "U8": {"char": "B", "size": 1},
    "BOOL": {"char": "?", "size": 1},
}


def _sf_format(n: int, char: str) -> str:
    if n == 0:
        return "<"
    return "<" + (char * n)


def dtype_token(s: str) -> str:
    return s.upper()


def read_sf(path: str):
    """Read safetensors file. Returns {name: {dtype, shape, data: list[floats]}}."""
    with open(path, "rb") as f:
        raw = f.read()
    if len(raw) < 8:
        raise ValueError("file too short for safetensors header length")
    header_len = struct.unpack("<Q", raw[:8])[0]
    header_bytes = raw[8:8 + header_len]
    if len(header_bytes) < header_len:
        raise ValueError("incomplete safetensors header")
    header = json.loads(header_bytes.decode("utf-8"))
    buffer = raw[8 + header_len:]
    tensors = {}
    for name, meta in header.items():
        if not isinstance(meta, dict):
            continue
        if name == "__metadata__":
            continue
        start, end = meta["data_offsets"]
        dtype = dtype_token(meta["dtype"])
        shape = meta["shape"]
        if dtype not in DTYPE_INFO:
            raise ValueError(f"unsupported dtype {dtype} for tensor {name}")
        info = DTYPE_INFO[dtype]
        n = 1
        for dim in shape:
            n *= dim
        data = buffer[start:end]
        if len(data) != n * info["size"]:
            raise ValueError(f"size mismatch for {name}: expected {n * info['size']}, got {len(data)}")
        nums = list(struct.unpack(_sf_format(n, info["char"]), data))
        tensors[name] = {"dtype": dtype, "shape": shape, "data": nums}
    return tensors


def write_sf(path: str, tensors: dict):
    """Write safetensors file. tensors: {name: {dtype, shape, data}}."""
    header = {}
    offset = 0
    payload = b""
    for name in sorted(tensors):
        t = tensors[name]
        dtype = dtype_token(t["dtype"])
        info = DTYPE_INFO[dtype]
        n = 1
        for dim in t["shape"]:
            n *= dim
        data = struct.pack(_sf_format(n, info["char"]), *t["data"])
        header[name] = {
            "dtype": dtype,
            "shape": t["shape"],
            "data_offsets": [offset, offset + len(data)],
        }
        payload += data
        offset += len(data)
    header_bytes = json.dumps(header, separators=(",", ":"), sort_keys=True).encode("utf-8")
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(header_bytes)))
        f.write(header_bytes)
        f.write(payload)


def matmul_f32(A_data, A_shape, B_data, B_shape):
    """A @ B, row-major f32. A_shape = [m, k], B_shape = [k, n]."""
    m, k = A_shape
    k2, n = B_shape
    if k != k2:
        raise ValueError(f"matmul shape mismatch: {A_shape} x {B_shape}")
    out = [0.0] * (m * n)
    for i in range(m):
        for j in range(n):
            s = 0.0
            a_row = i * k
            b_col0 = j
            for l in range(k):
                s += A_data[a_row + l] * B_data[l * n + b_col0]
            out[i * n + j] = s
    return out


def merge_lora(fixture_dir: str, out_dir: str) -> list:
    """Merge model + adapter. Returns list of warning dicts."""
    os.makedirs(out_dir, exist_ok=True)
    model = read_sf(os.path.join(fixture_dir, "model.safetensors"))
    adapter = read_sf(os.path.join(fixture_dir, "adapter_model.safetensors"))
    cfg_path = os.path.join(fixture_dir, "adapter_config.json")
    with open(cfg_path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    alpha = cfg.get("lora_alpha", cfg.get("alpha", 1))
    rank = cfg.get("r", 1)
    scale = alpha / rank

    pairs = {}
    for key in adapter:
        if key.endswith(".lora_A.weight"):
            base = key[: -len(".lora_A.weight")]
            pairs.setdefault(base, {})["A"] = adapter[key]
        elif key.endswith(".lora_B.weight"):
            base = key[: -len(".lora_B.weight")]
            pairs.setdefault(base, {})["B"] = adapter[key]

    merged = {}
    for name, t in model.items():
        merged[name] = {"dtype": t["dtype"], "shape": list(t["shape"]), "data": t["data"][:]}  # copy values

    warnings = []
    for base, pair in pairs.items():
        if "A" not in pair or "B" not in pair:
            warnings.append({"type": "incomplete_pair", "base": base})
            continue

        if base.startswith("base_model.model."):
            mid = base[len("base_model.model."):]
        else:
            mid = base
        target = "decoder." + mid + ".weight"

        if target not in merged:
            warnings.append({"type": "missing_target", "target": target})
            continue

        A = pair["A"]
        B = pair["B"]
        W = merged[target]

        r, in_f = A["shape"]
        out_f, r2 = B["shape"]
        if W["shape"] != [out_f, in_f]:
            warnings.append({"type": "shape_mismatch", "target": target, "expected": [out_f, in_f], "got": W["shape"]})
            continue

        # delta = B @ A
        delta = matmul_f32(B["data"], B["shape"], A["data"], A["shape"])
        n = out_f * in_f
        for idx in range(n):
            W["data"][idx] = W["data"][idx] + scale * delta[idx]

    write_sf(os.path.join(out_dir, "merged_model.safetensors"), merged)

    with open(os.path.join(out_dir, "warnings.jsonl"), "w", encoding="utf-8") as f:
        for w in warnings:
            f.write(json.dumps(w, sort_keys=True) + "\n")

    return warnings


def main():
    if len(sys.argv) != 3:
        print("usage: merge_reference.py <fixture_dir> <out_dir>")
        sys.exit(1)
    merge_lora(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
