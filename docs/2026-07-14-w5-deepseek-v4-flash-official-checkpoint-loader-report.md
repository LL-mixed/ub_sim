# W5 DeepSeek V4 Flash official checkpoint loader validation

Date: 2026-07-14

Scope: stage 1 of
plans/2026-07-13-w5-deepseek-v4-flash-official-checkpoint-plan.md.

## Result

The sim-models loader now opens the official DeepSeek V4 Flash checkpoint
directly from config.json, model.safetensors.index.json, and all 46
Safetensors shards. It validates the complete index/header mapping before any
payload is exposed and reads payload slices with positioned I/O. It does not
load a shard or the full checkpoint into memory.

Validated official asset:

- model directory: ~/repos/models/ds4_flash;
- revision metadata: Revision:master,CreatedAt:1782130416;
- config FNV-1a checksum: 0x1f21a2536706f3b8;
- index FNV-1a checksum: 0x9085917e69b68077;
- shards: 46;
- tensors: 69,187;
- indexed tensor payload: 159,609,485,896 bytes;
- header/config/index input bytes retained during open: 13,035,906;
- observed process peak resident bytes during validation: 65,208,320.

The dtype inventory is:

| Safetensors dtype | Tensor count |
| --- | ---: |
| BF16 | 433 |
| F32 | 417 |
| F8_E4M3 | 375 |
| F8_E8M0 | 34,167 |
| I64 | 3 |
| I8 packed FP4 | 33,792 |

All 34,167 quantized weights have a validated F8_E8M0 scale association.
FP8 scales use the configured 128-by-128 block mapping. Packed FP4 routed
expert scales use one scale per 32 logical K elements.

## CLI evidence

Full config/index/shard/schema gate:

    cargo run -p sim-models --bin deepseek_v4_flash_checkpoint -- \
      validate \
      --model ~/repos/models/ds4_flash \
      --tensor-cache-bytes 1048576 \
      --expert-cache-bytes 1048576

Result: schema_valid=true, 46 shards, 69,187 tensors, and exact agreement
between the index total_size and validated header payload bytes.

Ordinary FP8 tensor slice:

    target/debug/deepseek_v4_flash_checkpoint slice \
      --model ~/repos/models/ds4_flash \
      --tensor layers.0.attn.wkv.weight \
      --offset 0 \
      --bytes 4096 \
      --tensor-cache-bytes 8192 \
      --expert-cache-bytes 8192

Result:

- dtype/shape: F8_E4M3 [512,4096];
- scale: layers.0.attn.wkv.scale, F8_E8M0 [4,32];
- payload checksum: fnv1a64:a804ca4072a1431b;
- tensor cache resident/high-water: 4,096/4,096 bytes;
- expert cache resident/high-water: 0/0 bytes.

Routed expert FP4 slice:

    target/debug/deepseek_v4_flash_checkpoint slice \
      --model ~/repos/models/ds4_flash \
      --tensor layers.0.ffn.experts.17.w1.weight \
      --offset 64 \
      --bytes 4096 \
      --tensor-cache-bytes 8192 \
      --expert-cache-bytes 8192

Result:

- storage dtype/shape: I8 [2048,2048], representing packed E2M1 values;
- scale: layers.0.ffn.experts.17.w1.scale, F8_E8M0 [2048,128];
- payload checksum: fnv1a64:d2ffdc67f294a27c;
- expert cache resident/high-water: 4,096/4,096 bytes;
- tensor cache resident/high-water: 0/0 bytes.

## Fail-closed and memory gates

Focused tests cover:

- missing official shard;
- unsupported dtype;
- empty/bad offset range;
- zero dimension and dtype/shape payload-size mismatch;
- missing scale and wrong scale shape;
- independent tensor/expert cache capacity and eviction;
- positioned reads returning only the requested range;
- malformed layer names and out-of-range layer admission.

Opening the 149 GiB checkpoint retained about 13 MiB of source metadata and
peaked at about 65 MiB process RSS. A 4 KiB payload request increased only the
matching bounded cache by 4 KiB. Neither metadata validation nor single-slice
reads scale resident memory with checkpoint payload size.

Full repository regression after the implementation:

- `cargo test --workspace`: passed, including 135 `sim-uapi` tests; 3 native
  runtime tests remain explicitly ignored by the existing suite;
- `python3 -m unittest discover guest-linux/aarch64/tests`: 303 passed, 1
  skipped, 0 failed.

## Boundary

This stage provides asset loading and schema admission only. It does not claim
FP8/FP4 operator correctness, first-token correctness, W5 distributed official
checkpoint inference, or MTP execution. The loader retains
max_position_embeddings=1048576; 1M context remains explicitly unvalidated.
There is no DS4 build-time or runtime dependency in this loader.
