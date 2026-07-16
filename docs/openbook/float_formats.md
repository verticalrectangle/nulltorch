# Floating-point format reference: binary16, bfloat16, float8 e4m3fn / e5m2

All four formats follow the IEEE-754 scheme: a value is
`(-1)^sign × 2^(E − bias) × 1.mantissa` for normal numbers (stored exponent
`E` neither all-zeros nor all-ones), and
`(-1)^sign × 2^(1 − bias) × 0.mantissa` for subnormals (stored exponent
all-zeros). Stored exponent all-ones encodes infinity (mantissa = 0) and NaN
(mantissa ≠ 0) — **except float8 e4m3fn**, which deviates as described below.
Zero is sign + all other bits zero; −0 exists in every format.

On disk (in this benchmark's fixtures, and in practice on every mainstream
platform) multi-byte values are stored **little-endian**: a binary16 `1.0`
(`0x3C00`) appears as the byte sequence `00 3C`. The float8 formats are
single bytes, so endianness does not apply.

Note for the converter task: extraction is **byte-copying** — you never need
to decode these formats arithmetically to pass. This reference exists so you
can recognize values in hexdumps while debugging (e.g. spotting a plausible
run of `1.0`s vs. garbage) and sanity-check dtype/element-size bookkeeping.

## Format summary

| Format | Total bits | Sign | Exp bits | Mantissa bits | Bias | Inf? | NaN encodings |
|---|---:|---:|---:|---:|---:|---|---|
| binary16 (f16, IEEE half) | 16 | 1 | 5 | 10 | 15 | yes | E=11111, M≠0 (many patterns) |
| bfloat16 (bf16) | 16 | 1 | 8 | 7 | 127 | yes | E=11111111, M≠0 (many patterns) |
| float8 e4m3fn | 8 | 1 | 4 | 3 | 7 | **no** | **only** `S.1111.111` (`0x7F`/`0xFF`) |
| float8 e5m2 | 8 | 1 | 5 | 2 | 15 | yes | E=11111, M≠0 (`0x7D`,`0x7E`,`0x7F` ± sign) |

| Format | Max finite | Min normal | Min subnormal |
|---|---|---|---|
| binary16 | 65504 (`0x7BFF`) | 2⁻¹⁴ ≈ 6.104e-05 (`0x0400`) | 2⁻²⁴ ≈ 5.960e-08 (`0x0001`) |
| bfloat16 | ≈ 3.3895e38 (`0x7F7F`) | 2⁻¹²⁶ ≈ 1.1755e-38 (`0x0080`) | 2⁻¹³³ ≈ 9.18e-41 (`0x0001`) |
| float8 e4m3fn | 448 (`0x7E`) | 2⁻⁶ = 0.015625 (`0x08`) | 2⁻⁹ = 0.001953125 (`0x01`) |
| float8 e5m2 | 57344 (`0x7B`) | 2⁻¹⁴ ≈ 6.104e-05 (`0x04`) | 2⁻¹⁶ ≈ 1.526e-05 (`0x01`) |

## binary16 (IEEE-754 half precision, "f16")

Layout (bit 15 = MSB): `S EEEEE MMMMMMMMMM` — 1 sign, 5 exponent (bias 15),
10 mantissa.

- Normal: `E` in 1..30, value `±2^(E−15) × 1.M`.
- Subnormal: `E = 0, M ≠ 0`, value `±2^−14 × (M / 1024)`.
- `E = 31`: `M = 0` → ±infinity (`0x7C00`, `0xFC00`); `M ≠ 0` → NaN.

## bfloat16 ("bf16")

Layout: `S EEEEEEEE MMMMMMM` — 1 sign, 8 exponent (bias 127), 7 mantissa.

bfloat16 is exactly the **top 16 bits of an IEEE-754 binary32** (same sign
and exponent field, mantissa truncated from 23 to 7 bits). To widen a bf16
to f32 losslessly: place the 16 bits in the high half of a 32-bit word and
zero the low half. Same special-value rules as f16: `E = 255, M = 0` →
±infinity (`0x7F80`, `0xFF80`); `E = 255, M ≠ 0` → NaN. Subnormals
(`E = 0, M ≠ 0`, value `±2^−126 × M/128`) are defined by the format, though
much hardware flushes them to zero.

## float8 e4m3fn

Layout: `S EEEE MMM` — 1 sign, 4 exponent (bias 7), 3 mantissa. The `fn`
suffix means **finite + NaN only**; this format deviates from the IEEE
pattern to reclaim encodings:

- There is **no infinity**.
- NaN has a **single pattern per sign**: `S 1111 111` (`0x7F` and `0xFF`).
  These are the only two non-finite encodings in the whole format.
- `E = 1111` with `M ≠ 111` encodes **ordinary normal numbers** — this is
  where the deviation pays off: the max finite value is
  `0 1111 110` = 1.75 × 2⁸ = **448** (`0x7E`).
- Normal: `E` in 1..15 (with the `E=15, M=7` NaN carve-out),
  value `±2^(E−7) × 1.M`.
- Subnormal: `E = 0, M ≠ 0`, value `±2^−6 × (M / 8)`.

## float8 e5m2

Layout: `S EEEEE MM` — 1 sign, 5 exponent (bias 15), 2 mantissa. Fully
IEEE-style (it is binary16 with the mantissa truncated to 2 bits):

- Normal: `E` in 1..30, value `±2^(E−15) × 1.M`.
- Subnormal: `E = 0, M ≠ 0`, value `±2^−14 × (M / 4)`.
- `E = 31, M = 0` → ±infinity (`0x7C`, `0xFC`); `E = 31, M ≠ 0` → NaN
  (`0x7D`, `0x7E`, `0x7F` and their sign-bit variants).
- Max finite: `0 11110 11` = 1.75 × 2¹⁵ = **57344** (`0x7B`).

## Worked examples

`1.0` = `+ 2^0 × 1.0`: sign 0, stored exponent = bias, mantissa 0.

| Format | 1.0 bits | Hex | On-disk bytes (LE) |
|---|---|---|---|
| binary16 | `0 01111 0000000000` | `0x3C00` | `00 3C` |
| bfloat16 | `0 01111111 0000000` | `0x3F80` | `80 3F` |
| e4m3fn | `0 0111 000` | `0x38` | `38` |
| e5m2 | `0 01111 00` | `0x3C` | `3C` |

`-2.5` = `− 2^1 × 1.25`: sign 1, stored exponent = bias + 1, mantissa = `01`
followed by zeros (0.25 = first fractional bit 0, second bit 1).

| Format | −2.5 bits | Hex | On-disk bytes (LE) |
|---|---|---|---|
| binary16 | `1 10000 0100000000` | `0xC100` | `00 C1` |
| bfloat16 | `1 10000000 0100000` | `0xC020` | `20 C0` |
| e4m3fn | `1 1000 010` | `0xC2` | `C2` |
| e5m2 | `1 10000 01` | `0xC1` | `C1` |

A few more useful anchors:

| Value | f16 | bf16 | e4m3fn | e5m2 |
|---|---|---|---|---|
| +0.0 | `0x0000` | `0x0000` | `0x00` | `0x00` |
| −0.0 | `0x8000` | `0x8000` | `0x80` | `0x80` |
| +inf | `0x7C00` | `0x7F80` | — (none) | `0x7C` |
| a NaN | `0x7E00` (typical quiet) | `0x7FC0` (typical quiet) | `0x7F` (the only one) | `0x7E` (typical) |
| 0.5 | `0x3800` | `0x3F00` | `0x30` | `0x38` |
| −1.0 | `0xBC00` | `0xBF80` | `0xB8` | `0xBC` |
