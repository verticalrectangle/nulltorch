#!/usr/bin/env python3
"""Generate LoRA-merge fixtures for the NullTorch benchmark.

Uses only stdlib + merge_reference.py (which is also stdlib only). The
ground-truth merged checkpoint is produced by the reference solution itself,
so there is a single source of truth for the merge math.
"""

import hashlib
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import merge_reference as ref

FIXTURES_ROOT = os.path.dirname(os.path.abspath(__file__))


def make_f32(shape):
    n = 1
    for d in shape:
        n *= d
    return [0.0] * n, shape


def make_random_f32(rng: random.Random, shape, lo=-1.0, hi=1.0):
    n = 1
    for d in shape:
        n *= d
    return [rng.uniform(lo, hi) for _ in range(n)]


def build_fixture(fixture_id: str, seed: int, include_distractor: bool):
    rng = random.Random(seed)
    out_dir = os.path.join(FIXTURES_ROOT, fixture_id)
    gt_dir = os.path.join(out_dir, "gt")
    os.makedirs(gt_dir, exist_ok=True)

    # Model checkpoint (decoder-prefixed DiT-like weights)
    model = {}
    adapter = {}
    expected_targets = []
    layer_specs = [
        ("decoder.0.attn.to_q.weight", [4, 8]),
        ("decoder.0.attn.to_k.weight", [4, 8]),
        ("decoder.0.ff.0.weight", [16, 8]),
        ("decoder.1.attn.to_q.weight", [6, 12]),
    ]
    rank = 2
    alpha = 4

    for target_name, shape in layer_specs:
        model[target_name] = {"dtype": "F32", "shape": list(shape), "data": make_random_f32(rng, shape)}
        expected_targets.append(target_name)
        # derive adapter base key: strip "decoder." prefix and ".weight" suffix
        assert target_name.startswith("decoder.") and target_name.endswith(".weight")
        mid = target_name[len("decoder."):-len(".weight")]
        base = "base_model.model." + mid
        A_shape = [rank, shape[1]]
        B_shape = [shape[0], rank]
        adapter[base + ".lora_A.weight"] = {"dtype": "F32", "shape": A_shape, "data": make_random_f32(rng, A_shape)}
        adapter[base + ".lora_B.weight"] = {"dtype": "F32", "shape": B_shape, "data": make_random_f32(rng, B_shape)}

    expected_warnings = []
    if include_distractor:
        # a pair whose target does not exist in the base model
        base = "base_model.model.1.missing"
        adapter[base + ".lora_A.weight"] = {"dtype": "F32", "shape": [rank, 8], "data": make_random_f32(rng, [rank, 8])}
        adapter[base + ".lora_B.weight"] = {"dtype": "F32", "shape": [4, rank], "data": make_random_f32(rng, [4, rank])}
        expected_warnings.append({"type": "missing_target", "target": "decoder.1.missing.weight"})

    adapter_config = {"lora_alpha": alpha, "r": rank, "target_modules": ["to_q", "to_k"], "lora_dropout": 0.0, "bias": "none"}

    ref.write_sf(os.path.join(out_dir, "model.safetensors"), model)
    ref.write_sf(os.path.join(out_dir, "adapter_model.safetensors"), adapter)
    with open(os.path.join(out_dir, "adapter_config.json"), "w", encoding="utf-8") as f:
        json.dump(adapter_config, f, indent=2, sort_keys=True)

    # ground truth from reference
    tmp_out = os.path.join(out_dir, "_gt_tmp")
    if os.path.isdir(tmp_out):
        for fn in os.listdir(tmp_out):
            os.remove(os.path.join(tmp_out, fn))
        os.rmdir(tmp_out)
    warnings = ref.merge_lora(out_dir, tmp_out)
    merged_path = os.path.join(tmp_out, "merged_model.safetensors")
    warnings_path = os.path.join(tmp_out, "warnings.jsonl")
    # move to gt/
    os.replace(merged_path, os.path.join(gt_dir, "merged_model.safetensors"))
    os.replace(warnings_path, os.path.join(gt_dir, "warnings.jsonl"))
    os.rmdir(tmp_out)

    # hashes for gt
    hashes = {}
    for fn in ["merged_model.safetensors", "warnings.jsonl"]:
        p = os.path.join(gt_dir, fn)
        with open(p, "rb") as f:
            h = hashlib.sha256(f.read()).hexdigest()
        hashes[fn] = {"sha256": h, "size": os.path.getsize(p)}
    with open(os.path.join(gt_dir, "hashes.json"), "w", encoding="utf-8") as f:
        json.dump(hashes, f, indent=2, sort_keys=True)

    meta = {
        "fixture_id": fixture_id,
        "tier": "L1",
        "recipe": "lora_merge",
        "seed": seed,
        "include_distractor": include_distractor,
        "rank": rank,
        "lora_alpha": alpha,
        "n_model_tensors": len(model),
        "n_adapter_pairs": len(layer_specs),
        "expected_targets": expected_targets,
        "expected_warnings": expected_warnings,
    }
    with open(os.path.join(out_dir, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=2, sort_keys=True)

    return meta


def main():
    build_fixture("lora_fixture_000", seed=6000, include_distractor=False)
    build_fixture("lora_fixture_001", seed=6001, include_distractor=True)
    print("fixtures/lora fixtures generated.")


if __name__ == "__main__":
    main()
