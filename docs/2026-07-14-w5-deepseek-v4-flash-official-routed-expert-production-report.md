# W5 DeepSeek V4 Flash official routed-expert production validation

Date: 2026-07-14

Scope: stage 4 of
`plans/2026-07-13-w5-deepseek-v4-flash-official-checkpoint-plan.md`.

## Result

The official Safetensors checkpoint now has a production simpler path for
packed FP4 routed experts. It consumes the official packed E2M1 weights and
per-32-K UE8M0 scales, performs dynamic E4M3 activation quantization, executes
gate/up/down projections, applies the model's clamped SwiGLU and route weight,
and combines exactly six selected experts.

The A5 MX instruction does not accept an E4M3 activation with an E2M1 weight
in one mixed-dtype operation. The production path therefore lowers each E2M1
nibble to its exactly equivalent E4M3 code inside the requested output tile.
The official UE8M0 scale remains unchanged. This preserves every official FP4
value and scale while avoiding a persistent BF16/F32 checkpoint copy.

The artifact uses M=128, N=128 and a 128-aligned full K. K=4096 serves gate
and up; K=2048 serves down. One artifact dispatch produces 128 output rows and
accumulates all K tiles inside the A5 kernel. A full expert therefore performs
16 gate, 16 up and 32 down dispatches. Each projection reads only its selected
expert's requested weight and scale slices through the bounded expert cache.

## Production contracts

The `host_fp4_gemm` profile is fixed to:

- platform: `a5sim`;
- geometry: M=128, N=128, K positive and 128-aligned;
- activation: dynamically quantized E4M3 with UE8M0 per-128 scale;
- weight: official packed E2M1, tile-locally lowered to exact E4M3 codes;
- weight scale: official UE8M0 per 32 K elements;
- accumulator and artifact output: F32;
- projection boundary: BF16 RNE, matching the independent CPU oracle.

The runtime fails closed on wrong dtype/rank/shape, missing or mismatched scale,
row-range overflow, malformed packed payload, UE8M0 `0xff`, non-finite input,
invalid layer/expert/route weight, duplicate or non-top-6 selection, and
non-finite aggregation. The native asymmetric shifted-permutation test fixes
both low-nibble-first decoding and the A5 DN weight packing direction.

## Router and loading behavior

`deepseek_v4_flash_official_expert` exposes three explicit commands:

    cargo run --release -p sim-uapi \
      --bin deepseek_v4_flash_official_expert -- \
      route --model ~/repos/models/ds4_flash --layer 2 --token 1

    cargo run --release -p sim-uapi \
      --bin deepseek_v4_flash_official_expert -- \
      expert --model ~/repos/models/ds4_flash --layer 0 \
      --expert 17 --expert-weight 0.25

    cargo run --release -p sim-uapi \
      --bin deepseek_v4_flash_official_expert -- \
      selected --model ~/repos/models/ds4_flash --layer 2 --token 1

The CLI reports compact samples, checksums, dispatch counts and cache evidence;
it does not print every 4096-element intermediate vector. The library API
retains complete vectors for reference comparison.

For the seed-7 hidden fixture and token 1, the official hash-routed layer 2
selected:

- experts: `[217, 221, 240, 26, 247, 39]`;
- weights: `[0.24876454, 0.25466543, 0.25782603, 0.23966464,
  0.24883921, 0.25024012]`.

The complete selected path performed 384 simpler dispatches and produced
`fnv1a64:fb2a1cc223bef0f6`. It read six experts only: each cold expert consumed
13,369,344 weight/scale bytes, or 80,216,064 bytes total. The router-only
execution leaves every expert-cache counter unchanged, so selection cannot
silently preload unselected expert payloads.

The learned-router layer 3 is also compared with the CPU oracle using the same
seed-7 input. Its selection bias participates only in top-k ordering. Route
weights are normalized from the original, unbiased probabilities; a dedicated
unit assertion rejects the biased-weight alternative.

## Reference and cache evidence

The official expert-17 fixture compares production with an independently
opened CPU-reference checkpoint for all three projections and the clamped
SwiGLU boundary:

- gate: `fnv1a64:8d0f431e15aae1d7`;
- up: `fnv1a64:bd487944a1d88b52`;
- activated after route weight 0.25: `fnv1a64:572c1efd3b9bd116`;
- dispatches: 64;
- cold-cache reads: 13,369,344 bytes in six weight/scale misses.

Gate, up, activation and down output match the independent oracle exactly at
their defined BF16 boundaries.

Separate official rows from experts 17 and 99 match the independent packed-FP4
oracle exactly. A 5 MiB expert cache fixture forces LRU eviction between those
expert payloads, proves repeated resident reads become hits, enforces resident
bytes at or below capacity, and rejects an out-of-range expert slice. Structural
loader tests reject missing or malformed scale associations, while the FP4
production contract rejects the reserved `0xff` scale byte.

## Validation

- official hash and learned router fixture: 1 passed, 0 failed, 3.18 seconds;
- official full expert/reference/cache fixture: 1 passed, 0 failed, 20.25
  seconds;
- official representative linear fixture, now including experts 17 and 99: 1
  passed, 0 failed, 28.64 seconds;
- official top-6 selected execution: completed 384 dispatches with the checksum
  above;
- focused Rust FP4 and CLI tests: 5 passed, 0 failed;
- host artifact contract tests: 9 passed, 0 failed;
- `cargo test --workspace`: passed, including native simpler execution;
- `python3 -m unittest discover guest-linux/aarch64/tests`: 305 passed, 1
  skipped, 0 failed, 160.395 seconds;
- `cargo fmt --all -- --check` and `git diff --check`: passed.

## Boundary

This stage validates the official router and packed-FP4 routed-expert operator
path, including top-6 combination and bounded on-demand loading. It does not
claim a complete official transformer layer, first-token logits, continuous
decode, distributed W5 execution, or MTP. Those remain stages 5 through 7. No
DS4 build-time or runtime dependency was added.
