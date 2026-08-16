# W5 DeepSeek V4 Flash official checkpoint reference oracle validation

Date: 2026-07-14

Scope: stage 2 of
`plans/2026-07-13-w5-deepseek-v4-flash-official-checkpoint-plan.md`.

## Result

`sim-models` now has an independent scalar CPU oracle that consumes the
official Safetensors payload directly. It does not build a full BF16 copy of
the checkpoint and does not call DS4 or a simpler production kernel. The
oracle provides stable CLI entry points for decoded tensor blocks, bounded
matrix rows, and a complete position-0 transformer-layer forward.

The reference implements:

- finite E4M3 decode and round-to-nearest-even encode;
- packed E2M1 decode, with the low nibble representing the first logical K
  element;
- UE8M0 scale decode and power-of-two dynamic activation scales;
- FP8 128-by-128 weight scaling and activation blocks of 128 K elements;
- FP4 per-output-row, per-32-K scaling with FP8 activations;
- F32 accumulation and explicit BF16 RNE at model dtype boundaries;
- mHC pre/Sinkhorn/post, RMSNorm, RoPE, sink attention, grouped output
  projection, router, shared expert, six routed experts, clamped SwiGLU, and
  final layer hidden output;
- pending attention/indexer compressor state checksums for compressed layers.

The loader schema gate was tightened at the same time. It now checks exact HC,
attention, compressor, indexer, shared-expert, and packed routed-expert shapes,
including the official checkpoint fact that compressor projection weights are
stored as BF16 and promoted to F32 for reference accumulation.

## Official asset identity

- model directory: `~/repos/models/ds4_flash`;
- revision: `Revision:master,CreatedAt:1782130416`;
- config checksum: `fnv1a64:1f21a2536706f3b8`;
- index checksum: `fnv1a64:9085917e69b68077`.

Every operator report binds the input, raw weight row slice, scale payload,
and output checksums. Every layer report binds the model identity, layer tensor
metadata, input hidden, attention output, KV state, routes, and final hidden.

## CLI evidence

Ordinary FP8 projection rows:

    target/release/deepseek_v4_flash_reference operator \
      --model ~/repos/models/ds4_flash \
      --tensor layers.0.attn.wkv.weight \
      --seed 7 --row-start 0 --row-count 8

Result:

- input: `fnv1a64:acc1ed86517be057`;
- weight rows: `fnv1a64:caf7f971b5d4e21d`;
- scale tensor: `fnv1a64:7391a00bd9374d39`;
- output: `fnv1a64:3f29ddf3d0033c55`;
- output prefix: `[0.14257812, -0.033447266, -0.0390625, 0.25976562,
  -0.08203125, -0.016479492, -0.23339844, 0.091796875]`.

Packed FP4 routed-expert rows:

    target/release/deepseek_v4_flash_reference operator \
      --model ~/repos/models/ds4_flash \
      --tensor layers.0.ffn.experts.17.w1.weight \
      --seed 7 --row-start 0 --row-count 8

Result:

- input: `fnv1a64:acc1ed86517be057`;
- packed weight rows: `fnv1a64:8d6e0fa4a5315fbe`;
- scale tensor: `fnv1a64:afc80391ac1f0e81`;
- output: `fnv1a64:8d1e67e5155c7a4c`;
- output prefix: `[0.11035156, -0.16015625, -0.004211426, -0.16992188,
  -0.125, -0.08300781, -0.08105469, 0.13769531]`.

Ratio-4 layer with hash routing, attention compressor, and indexer compressor:

    target/release/deepseek_v4_flash_reference layer \
      --model ~/repos/models/ds4_flash \
      --layer 2 --token 1 --position 0 --seed 7

Result:

- layer tensor metadata: `fnv1a64:b7628d3b7f6b382a`;
- input hidden: `fnv1a64:e0326dec7c7d9ea2`;
- attention output: `fnv1a64:a2b0cc4eba8a7035`;
- selected experts: `[217, 221, 240, 26, 247, 39]`;
- attention compressor pending state: `fnv1a64:91071f6747bbcd1d`;
- indexer compressor pending state: `fnv1a64:3b036aeceec73092`;
- final 16,384-element HC hidden: `fnv1a64:be12800e82919600`;
- tensor/expert cache high-water: 66,602,652 / 80,216,064 bytes.

Ratio-128 layer with learned routing:

    target/release/deepseek_v4_flash_reference layer \
      --model ~/repos/models/ds4_flash \
      --layer 3 --token 1 --position 0 --seed 7

Result:

- layer tensor metadata: `fnv1a64:e7c4b0eaff103a23`;
- attention output: `fnv1a64:05c37f8c2abb03c7`;
- selected experts: `[29, 27, 80, 14, 212, 99]`;
- attention compressor pending state: `fnv1a64:f0441aa7bc4cc81e`;
- final hidden: `fnv1a64:636a22e4d1a53230`.

The `tensor` command additionally decodes bounded logical element ranges. For
packed FP4, `--offset` and `--elements` are logical E2M1 elements rather than
storage bytes; the report includes both storage and logical shapes.

## Tests and fail-closed behavior

Focused tests cover fixed FP8/FP4/UE8M0 patterns, E4M3 RNE and saturation,
dynamic activation scaling, FP8 block-scale mapping, FP4 nibble order and
per-32-K scales, BF16 ties-to-even, strict CLI parsing, and finite/shape
rejection. Existing model reference tests cover mHC, dense/mixed attention,
compressor state, indexer QAT/selection, router bias semantics, shared/routed
SwiGLU, and output projection composition.

The official fixture is an explicitly ignored-by-default test because model
weights are external to the repository. It passed with:

    env SIM_DEEPSEEK_V4_FLASH_WEIGHTS_PATH=~/repos/models/ds4_flash \
      cargo test --release -p sim-models \
      official_tensor_and_ratio4_layer_fixture_is_stable -- \
      --ignored --nocapture

Result: 1 passed, 0 failed, 1.23 seconds. The test fixes the two operator
checksums and the complete ratio-4 layer routes, compressor/indexer state, and
final hidden checksum listed above.

The complete repository regressions also passed after the oracle and schema
changes:

- `cargo test -p sim-models`: 155 passed, 0 failed, 1 ignored; all three
  `deepseek_v4_flash_reference` CLI parser tests passed;
- `cargo test --workspace`: passed, including 135 `sim-uapi` tests with 3
  ignored external/native fixtures;
- `python3 -m unittest discover guest-linux/aarch64/tests`: 303 passed, 0
  failed, 1 skipped, 136.767 seconds;
- `cargo fmt --all -- --check` and `git diff --check`: passed.

## Boundary

This stage is a correctness oracle, not the production execution path. The
complete layer CLI currently accepts position 0; nonzero positions fail closed
until a KV fixture contract is added with continuous decode in a later stage.
The position-0 path still materializes the exact pending compressor state needed
for the next token. MTP, multi-token continuity, distributed W5 execution, and
1M context remain outside this stage. No DS4 build-time or runtime dependency
was added.
