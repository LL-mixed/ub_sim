# W5 DeepSeek V4 Flash official linear production validation

Date: 2026-07-14

Scope: stage 3 of
`plans/2026-07-13-w5-deepseek-v4-flash-official-checkpoint-plan.md`.

## Result

The official Safetensors checkpoint now has a production simpler path for
ordinary FP8 linear weights. The path performs dynamic E4M3 activation
quantization, consumes the official E4M3 weights and UE8M0 scales directly,
maps official 128-by-128 scale blocks to the A5 MX scale layout, accumulates in
F32, and returns either F32 or explicitly rounded BF16 output.

The implementation dispatches one 128-by-128 A5 MX tile for each input K block
and requested output-row tile. It reads only requested weight rows, retains no
full F32 weight copy, and reports a 99,328-byte peak payload for each FP8 tile.
The official checkpoint's native 128-element scale is repeated across the four
32-element A5 MX scale lanes; no scale interpolation or recomputation occurs.

The official `head.weight` is BF16 rather than FP8. The production CLI inspects
checkpoint metadata and routes that tensor through the existing BF16 simpler
GEMM instead of misclassifying it as FP8. The BF16 output-head path transposes
only one requested 128-row tile into GEMM layout, rounds activation input with
ties-to-even, and supports F32 or BF16 output. Its 4,096-wide tile peak payload
is 2,162,688 bytes.

## Production contracts

The new `host_fp8_gemm` artifact contract is fixed to:

- platform: `a5sim`;
- geometry: `128x128x128`;
- activation/weight: E4M3;
- activation/weight scale: UE8M0;
- scale lane width: 32, with exact mapping from official 128-wide blocks;
- accumulator and artifact output: F32;
- Rust boundary output: F32 or BF16 RNE.

The runtime fails closed on wrong rank, dtype, scale dtype, missing scale
association, row-range overflow, non-128-aligned K, weight/scale length
mismatch, FP8 NaN encodings, and non-finite activation input. The artifact
manifest is admitted only when its platform, geometry, dtype, tile, runtime,
and version contract match.

The A5 kernel uses `float8_e4m3_t`, `float8_e8m0_t`, `TileLeftScale`,
`TileRightScale`, and `TMATMUL_MX`. An asymmetric shifted-permutation native
test fixes the B-matrix packing direction; this prevents a symmetric identity
fixture from hiding an accidental transpose.

## CLI evidence

The CLI is dtype-directed so one command covers official FP8 linears and the
BF16 output head:

    cargo run --release -p sim-uapi \
      --bin deepseek_v4_flash_official_linear -- \
      --model ~/repos/models/ds4_flash \
      --tensor layers.0.attn.wkv.weight \
      --row-count 8 --seed 7 --output-dtype bf16

Result:

- geometry: `[512,4096]`, rows 0 through 7;
- simpler dispatches: 32;
- activation values: `fnv1a64:58cb8d9385c5d556`;
- activation scales: `fnv1a64:b30ccdd940aa1625`;
- output: `fnv1a64:3f29ddf3d0033c55`;
- output values: `[0.14257812, -0.033447266, -0.0390625, 0.25976562,
  -0.08203125, -0.016479492, -0.23339844, 0.091796875]`.

This output and checksum exactly match the independent stage-2 CPU oracle.
Rows 127 and 128 additionally cross an official output scale-block boundary;
production and reference both return `[-0.27539062, -0.05126953]` with
`fnv1a64:6910580ec7e8c58f`.

Output-head command:

    cargo run --release -p sim-uapi \
      --bin deepseek_v4_flash_official_linear -- \
      --model ~/repos/models/ds4_flash \
      --tensor head.weight \
      --row-count 2 --seed 7 --output-dtype bf16

Result:

- official dtype/geometry: BF16 `[129280,4096]`;
- simpler dispatches: 1;
- rounded input: `fnv1a64:4b006ff502a141d7`;
- output: `[-1.9609375, 1.1875]`;
- output checksum: `fnv1a64:7c7c5bd6857dea6e`.

## Coverage and validation

The external official-checkpoint fixture compares production simpler output
with the independent CPU reference for:

- `layers.0.attn.wkv.weight`, including a full scale block and the 127/128
  output scale boundary;
- `layers.0.attn.wo_a.weight`, representing grouped attention output;
- `layers.0.ffn.shared_experts.w1.weight`, representing shared experts;
- `head.weight`, using the checkpoint's actual BF16 dtype.

All remaining official E4M3 matrix tensors use the same metadata-driven
production function; unsupported dtypes fail closed. The fixture passed with:

    env SIM_DEEPSEEK_V4_FLASH_WEIGHTS_PATH=~/repos/models/ds4_flash \
      cargo test --release -p sim-uapi \
      official_linear_production_matches_reference_across_fp8_roles_and_bf16_head \
      -- --ignored --nocapture

Result: 1 passed, 0 failed, 28.22 seconds. It performed 128 FP8 MX dispatches
and one BF16 output-head dispatch against the official checkpoint.

Additional evidence:

- the native asymmetric E4M3 permutation test passed for both BF16 and F32
  output boundaries;
- FP8 quantization tests cover zero, extrema, ties-to-even, saturation, and
  exact agreement with the independent oracle bytes and scales;
- negative tests cover NaN/Inf, shape, scale, range, dtype, and malformed
  manifest rejection;
- `python3 -m unittest
  guest-linux/aarch64/tests/test_simpler_host_gemm_artifacts.py`: 8 passed;
- `cargo test --workspace`: passed, including native simpler execution;
- `python3 -m unittest discover guest-linux/aarch64/tests`: 304 passed, 1
  skipped, 0 failed, 153.388 seconds;
- `cargo fmt --all -- --check` and `git diff --check`: passed.

## Boundary

This stage validates official ordinary FP8 linear operators and the BF16
output-head linear boundary. It does not claim packed FP4 routed-expert
production, a complete transformer layer through production kernels,
continuous decode, distributed W5 execution, or MTP. Those remain subsequent
plan stages. No DS4 build-time or runtime dependency was added.
