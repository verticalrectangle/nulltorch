# RVC track — fidelity to pop-maker-studio's engine

The T1–T6 tiers test general `.pth` *reading* (dtype-preserving, bit-exact).
The **RVC track** additionally tests the two things
`pop-maker-studio/src/pth_reader.cpp` actually does end-to-end when it loads an
RVC voice model, so the benchmark matches the real engine's job:

1. **Config extraction.** `cpt['config']` is an 18-element positional argument
   list to `SynthesizerTrnMsNSFsid`. The reader maps it to named fields
   (spec_channels … sr) and infers `phone_dim` from
   `enc_p.emb_phone.weight.shape[1]`. Graded exactly (`CONFIG_MISMATCH`).
2. **f16 → f32 conversion.** RVC weights are `HalfStorage`; the engine feeds
   ONNX float32. The RVC output contract is contiguous **f32** tensors (exact
   IEEE widening), graded bit-exact by sha256.

## Output contract (RVC manifest)
```
manifest.json = {
  nulltorch_manifest: 1, byteorder: "little",
  config:  { spec_channels, segment_size, inter_channels, hidden_channels,
             filter_channels, n_heads, n_layers, kernel_size, p_dropout,
             resblock, resblock_kernel_sizes, resblock_dilation_sizes,
             upsample_rates, upsample_initial_channel, upsample_kernel_sizes,
             n_speakers, gin_channels, sr, phone_dim },
  tensors: { "<weight-name>": { dtype: "f32", shape, nbytes } }   # no source
}                                                                 # stride/key
tensors/<name>.bin = contiguous f32 little-endian
```
Tensors are scoped to the `weight` dict (fallbacks: `model`, or a bare
state_dict) exactly like the engine. Source-layout fields (stride /
storage_key / storage_offset) are omitted — the output is freshly contiguous.

## Three-way validation (real HuggingFace checkpoint)
Run on `InductiveGrub/DollyParton` (55 MB, 457 f16 tensors):

| leg | result |
|---|---|
| torch oracle (`gen_rvc.py --from-pth`) | 457 tensors, 19 config fields |
| stdlib reference (`rvc_convert.py`) vs oracle | **PASS** — 457 f32 tensors bit-exact + all config fields |
| engine `pth_reader.cpp` (`test_pth_reader`) vs oracle | **all config fields match**, 457 tensors, phone_dim=768, n_speakers=109 |

So "correct RVC conversion" is the same value across the torch oracle, the
benchmark, and the shipping engine — the stdlib `struct` f16→f32 widening is
bit-identical to torch's `.float()`.

## Reproduce
```
# synthetic committed fixture (full gt, small)
python3 fixtures/gen/gen_rvc.py --synth --out fixtures/rvc --seed 7001
python3 reference/rvc_convert.py fixtures/rvc/rvc_synth__s7001/fixture.pth /tmp/out
python3 grader/grade.py fixtures/rvc/rvc_synth__s7001 /tmp/out
python3 grader/selftest.py fixtures/rvc

# real checkpoint (hashes-only gt; needs torch + a local .pth)
python3 fixtures/gen/gen_rvc.py --from-pth <model.pth> --out fixtures/rvc_real
python3 reference/rvc_convert.py <model.pth> /tmp/real
python3 grader/grade.py fixtures/rvc_real/<id> /tmp/real
```

## Board integration
RVC is wired into the board as an **optional tier** alongside T1–T6:
- `results.schema.json` allows an optional `RVC` key in `tiers` and
  `iterations[].tier_passes`.
- The harness runs it with `--rvc-cmd` (RVC fixtures use the RVC converter);
  `run.py` emits an `RVC` tier only for cells that ran it.
- It appears as its own leaderboard/heatmap column, visually distinct from the
  difficulty tiers, and is **excluded from the memorization gap** (gap =
  stock − delta over the T1–T6 reading tiers). RVC has no delta variant, so
  delta cells carry no `RVC` tier — dashboards handle its absence.

## Notes / limits
- Grading `p_dropout` as a float mirrors `pth_reader`'s `(float)` cast; real
  configs carry `0`.
- Per-fixture pass = all config fields AND all f32 tensors correct; category
  breakdown (config_extraction / f32_tensors) surfaces which half failed.
