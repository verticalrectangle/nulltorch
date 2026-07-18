# NullTorch task — LoRA-Merge (safetensors)

Write a program that merges a PEFT LoRA adapter into a `safetensors` model
checkpoint **without PyTorch, without Python (at runtime), and without any
third-party library** — standard library and toolchain only.

## What you are given

For each fixture directory `<fixture>`:

- `<fixture>/model.safetensors` — the base model checkpoint. Tensor names look
  like `decoder.<layer>.weight`. All tensors are **F32**, contiguous, row-major.
- `<fixture>/adapter_model.safetensors` — the LoRA adapter. Tensor names look
  like `base_model.model.<layer>.lora_A.weight` and
  `base_model.model.<layer>.lora_B.weight`. All F32.
- `<fixture>/adapter_config.json` — contains at least `lora_alpha` and `r`.

## What you must produce

Given an output directory `<out>`, write:

1. `<out>/merged_model.safetensors` — the base model with every matching LoRA
   pair merged in, using:

   ```
   scale = lora_alpha / r
   W_merged = W + scale * (lora_B @ lora_A)
   ```

   where `lora_A` has shape `[r, in_features]`, `lora_B` has shape
   `[out_features, r]`, and `W` has shape `[out_features, in_features]`.
   The multiplication is ordinary matrix multiplication over f32.

2. `<out>/warnings.jsonl` — one JSON object per line for every skipped pair.
   At minimum, report a `missing_target` warning when an adapter pair has no
   corresponding `decoder.<layer>.weight` in the base model.

## Invocation

```
./convert <fixture_dir> <out_dir>
```

Exit 0 on success. Read nothing from the network; spawn no subprocesses.

## safetensors format

A `.safetensors` file is:

1. Header length: unsigned 64-bit little-endian integer.
2. Header: that many UTF-8 bytes containing a JSON object. Each tensor maps to
   a value with `dtype` (string, e.g. `"F32"`), `shape` (list of ints), and
   `data_offsets` (`[start, end]` byte offsets into the data region).
3. Data region: concatenated raw tensor bytes, little-endian, row-major.

Your output must be a valid safetensors file. For this tier every tensor is
F32. You may keep the original `__metadata__` block if present, but it is not
required and will not be graded.

## Key mapping

```
base_model.model.<layer>.lora_A.weight
base_model.model.<layer>.lora_B.weight
                     ->
decoder.<layer>.weight
```

That is, strip the `base_model.model.` prefix and the `.lora_A.weight` or
`.lora_B.weight` suffix, then add `decoder.` prefix and `.weight` suffix.

## Rules

- Standard library only. No `torch`, no `numpy`, no `safetensors`.
- No network, no subprocesses at runtime.
- Deterministic output: two runs on the same fixture must produce byte-identical
  `merged_model.safetensors` and `warnings.jsonl`.
- Self-grade with `python3 grader/grade_lora.py <fixture_dir> <out_dir>`.

## Tier

This is tier **L1**. It tests format parsing, matrix multiplication, key
rewriting, and graceful handling of adapter pairs that do not match the base
model.
