# W5 DeepSeek V4 Flash official first-token validation

Date: 2026-07-16

Scope: stage 5 of
`plans/2026-07-13-w5-deepseek-v4-flash-official-checkpoint-plan.md`.

## Result

The official DeepSeek V4 Flash Safetensors checkpoint now executes a complete
position-0 first-token forward in `ub_sim`: embedding, all 43 transformer
layers, output norm/head, full-vocabulary logits, top-k and top-1. The recorded
raw prompt is exactly token ID `[1]`; no BOS, chat template, thinking marker or
other token is inserted.

Both matrix and vector production operations traverse the required path:

```text
UAPI dispatch
-> sim-uapi
-> sim-chipbackend-simpler
-> simpler C API
-> A5 kernel
```

The production path does not call the scalar CPU oracle, DS4, GGUF or a
synthetic/checksum-derived fallback. The independent CPU oracle and production
path share only the checked Safetensors slice reader and are compared at every
layer boundary.

## Checkpoint identity and terminal result

The successful report is bound to:

- model revision: `Revision:master,CreatedAt:1782130416`;
- config checksum: `fnv1a64:1f21a2536706f3b8`;
- index checksum: `fnv1a64:9085917e69b68077`;
- prompt token IDs: `[1]`;
- position: `0`;
- layer IDs: every integer in `[0,43)`, exactly once;
- reference and production embedding checksum:
  `fnv1a64:aab63bc484278535`;
- reference and production logits checksum:
  `fnv1a64:3056e8a03cf73db1`;
- logits maximum absolute difference: `0`;
- top-1 token: `294`.

Reference and production returned the same ordered top-5 logits:

| Rank | Token | Logit |
| ---: | ---: | ---: |
| 1 | 294 | 22.271175 |
| 2 | 339 | 18.179247 |
| 3 | 1 | 16.768436 |
| 4 | 223 | 16.685265 |
| 5 | 995 | 16.417803 |

## Layer alignment

Every layer report records input/output hidden checksums, selected experts,
route weights, attention kind and compression ratio, raw/compressed KV state,
production dispatch count and checkpoint read/cache evidence. Ratio-128 and
ratio-4 attention layers both execute their real compressor paths; ratio-4
layers additionally execute and compare the sparse indexer query, weights and
compressor state.

The validator uses zero tolerance for attention output, raw KV, attention and
indexer compressor state, indexer query/weights and terminal logits. The only
non-zero tolerances are route weights at `1e-7` and output hidden at `1e-5`.
These bounds cover cross-compiler scalar-versus-A5 floating-point evaluation,
not checkpoint quantization or skipped work. The complete 43-layer run observed:

- attention, raw KV, compressor/indexer state and logits: maximum difference
  `0`;
- route weights: maximum difference `3.7252903e-8`;
- output hidden: maximum difference `3.8146973e-6`.

Checksums remain audit evidence, but a checksum mismatch does not override an
explicit non-zero numeric tolerance. Fail-closed tests prove values within the
two tolerances pass while values above either tolerance fail.

## A5 execution and resource evidence

The first-token run issued 41,248 simpler dispatches. Official FP8/FP4 matrix
tiles use A5 MX kernels. The `host_deepseek_vector` A5 artifact covers RMS norm,
mHC split/head weights/weighted sum/post, RoPE, FP8 KV round-trip, sink
attention, indexer QAT, scale, clamped SwiGLU, add, router and top-k. A native
smoke test dispatches all vector operation kinds with non-trivial fixtures.

The successful run recorded:

- elapsed execution time: 11,161.061 seconds;
- process peak resident bytes: 4,792,549,376 (4.46 GiB);
- peak single tile payload: 16,842,752 bytes (16.06 MiB);
- tensor cache: 32 MiB capacity and peak resident, 301 hits, 3,311 misses and
  3,279 evictions;
- expert cache: 64 MiB capacity and peak resident, 1,548 selected-expert misses
  and 1,517 evictions;
- tensor disk reads: 7,762,709,612 bytes;
- selected-expert disk reads: 3,449,290,752 bytes.

The caches remain at their configured bounds, selected experts are loaded on
demand, and peak RSS is far below the 128 GiB host capacity. No complete
checkpoint, BF16 model copy or F32 model copy is resident.

## CLI and validation

The stable first-token entry point is:

```text
cargo run --release -p sim-uapi \
  --bin deepseek_v4_flash_official_first_token -- \
  --model /Users/liliang/repos/models/ds4_flash \
  --tokens 1 \
  --artifact-dir /tmp/deepseek-v4-stage5-first-token-artifacts \
  --top-k 5 \
  --report /tmp/deepseek-v4-stage5-first-token.json
```

Validation completed with:

- official 43-layer first-token/reference comparison and terminal validator:
  passed;
- official layer-0, ratio-4 layer, router and full output-head focused
  comparisons: passed;
- native `host_deepseek_vector` A5 operation smoke: passed as part of the Rust
  workspace regression;
- host artifact generator and contract tests: passed as part of the guest
  Python regression;
- `cargo test --workspace -- --test-threads=1`: passed;
- `python3 -m unittest discover guest-linux/aarch64/tests`: 306 tests passed,
  1 skipped, 0 failed, 141.311 seconds;
- `cargo fmt --all -- --check` and `git diff --check`: passed.

## Boundary

This stage proves one official-checkpoint token at position 0 through all 43
base-model layers and the full output head. It does not prove continuous decode,
W5 2/3/8-node layer pipelines, MTP, or 1M-token context. Those boundaries remain
stages 6 and 7 plus the explicit stage-8 non-claim. DS4 remains read-only and is
not a build-time or runtime dependency.
