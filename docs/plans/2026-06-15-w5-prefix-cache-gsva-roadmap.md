# W5 Prefix Cache and GSVA Roadmap

Date: 2026-06-15

## Purpose

This document records the next execution direction for W5 after the current
Qwen3-14B W5 inference, Memory Service shortpath, and GVA/GSVA work.

The goal is not to describe every W5 subsystem. The goal is to track two
specific questions:

1. Make prefix cache a real W5 main-path optimization with measurable benefit.
2. Decide how GVA/GSVA should be used by W5, and prove the benefit with
   end-to-end runs instead of architectural assumption.

## Current Baseline

W5 already has a working 8-node Qwen3 inference path on top of the W4
guest/QEMU runtime:

- Qwen3-14B 8-node layer-range pipeline has completed 16-step seed and reuse
  runs.
- Memory Service exact shortpath reuse has produced measurable speedup by
  hitting `jump-to-terminal`.
- Runtime async commit writes boundary observations, KV artifacts, and
  terminal logits artifacts during decode.
- Sampler and Engram policy can run serially after terminal logits selection.
- GVA/GSVA has a separate validated V1 base: GVA non-identity route, GSVA
  identity route, GSVA coherence, token/epoch/retire/TLB flush, and UB NPU/SSD
  device access through GSVA.

The important boundary:

- The measured W5 speedup today comes from exact Memory Service shortpath.
- Prefix cache service has infrastructure, but current W5 main-path evidence
  reports `prefix_cache_ids=none`.
- GVA/GSVA is validated as a multi-node shared-address/device base, but W5
  inference has not yet shown a quantified benefit from using GSVA-backed
  hidden/KV/cache objects.

## Working Definitions

### Exact Shortpath

Exact shortpath is a range-exit decision:

```text
range exit hidden
  -> exact boundary lookup
  -> verified terminal logits artifact
  -> sampler
  -> optional Engram policy
  -> terminal token publish
```

This can skip downstream range-forward work items. It is already proven in W5
E2E runs.

### Prefix Cache

Prefix cache is a prefix/KV reuse decision:

```text
prompt or prefix identity
  -> prefix-cache lookup
  -> verified KV/cache artifact refs
  -> materialize or map cached state
  -> avoid recomputing the prefix/cache span
```

It should not be reported as proven until W5 summary shows non-empty
`prefix_cache_ids`, prefix-cache service evidence, output guard pass, and
timing improvement versus a no-prefix-cache baseline.

### GSVA-backed W5 Object

A GSVA-backed W5 object is a hidden/KV/cache/logits payload whose lifetime and
access are expressed through a GSVA segment rather than only through an object
registry entry or stream ref.

The expected metadata is:

- model/profile/tokenizer binding;
- producing node and layer range;
- decode step or prefix span;
- GSVA segment descriptor;
- token, epoch, retire state, and checksum;
- object ref compatibility for existing Memory Service records.

## P0: Make Prefix Cache a Real W5 Main-Path Optimization

### P0.1 Define the Prefix Cache Contract

Define the exact reuse unit before changing the runner:

- prompt-level prefix cache;
- prefix token span;
- per-node layer-range KV cache;
- position/rope binding;
- model/profile/tokenizer binding;
- cache checksum and shape;
- miss behavior and stale behavior.

Acceptance:

- A design note or code-level schema names the prefix cache key fields.
- A lookup with mismatched model, tokenizer, prompt token ids, position, or
  layer range must fail closed.

### P0.2 Generate Prefix Cache Reuse Plans in Seed Runs

Seed runs must register cacheable artifacts into Memory Service and produce a
reuse plan.

Required evidence:

- Memory store contains a prefix-cache manifest.
- The plan records artifact ids, checksums, layer range, prefix span, and
  source run id.
- The plan can be listed by CLI without reading raw payload bytes.

Candidate CLI flow:

```text
sim-cli lingqu-memory register-prefix-cache ...
sim-cli lingqu-memory list-prefix-cache-reuse ...
```

### P0.3 Consume Prefix Cache in Reuse Runs

Reuse runs must use the generated plan through the W5 runner, not by ad hoc
manual env injection.

Required runtime evidence:

- `SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR` is non-empty.
- `w5_prefix_cache_service_ready.<run_id>.txt` exists.
- W5 summary reports non-empty `prefix_cache_ids`.
- W5 summary distinguishes prefix-cache hit, miss, and stale rejection.
- Output guard passes.

Candidate run shape:

```text
round1: 16-step Qwen3-14B seed run, prefix cache registration enabled
round2: same prompt, prefix cache lookup enabled
round3: different prompt or altered prefix, expected miss
```

### P0.4 Measure Prefix Cache Benefit

The first benefit claim must be conservative. Do not compare a prefix-cache run
against a shortpath jump-to-terminal run unless the claim is explicitly about
combined optimizations.

Measure at least:

- round sum;
- post-step0 average round;
- prefix materialization time;
- KV load/materialize time;
- range-forward count;
- prefix-cache hit count and miss count;
- object store and memory store growth.

Acceptance:

- Same-output run pair: no-prefix-cache versus prefix-cache, same prompt.
- Negative control: prefix-cache enabled but intentionally mismatched prompt
  produces a miss and correct runtime recompute.
- Summary/report tooling surfaces the difference without manually reading logs.

## P1: Connect GSVA to W5 Through KV/Prefix Cache First

### P1.1 GSVA-backed KV Segment Prototype

Start with one narrow object type: per-node KV cache. Do not move every W5
payload to GSVA at once.

Prototype flow:

```text
range worker publishes KV
  -> allocate/register GSVA segment
  -> write KV payload into segment
  -> publish Memory Service object ref with GSVA metadata
  -> downstream/reuse path maps or reads segment
```

Acceptance:

- One W5 run publishes KV refs with `backend=gsva`.
- Consumer resolves the same KV through GSVA metadata.
- Output guard passes.
- Summary reports `gsva_kv_refs`, `gsva_reads`, and `gsva_writebacks`.

Progress as of 2026-06-15:

- Implemented a narrow `ExecutionArtifactObject.gsva_segment_ref` metadata path
  for KV artifacts.
- `SIM_W5_MEMORY_GSVA_KV=1` makes runtime KV artifacts publish
  `backend=gsva` segment metadata while leaving default runs unchanged.
- Prefix-cache KV stream now carries GSVA segment metadata as an extended
  18-field line while preserving the old 9-field OBMM format.
- Guest prefix-cache KV consumption validates GSVA token, epoch, retire state,
  bytes, and checksum before accepting the object ref.
- W5 summary/report now surfaces `gsva_kv_refs`, `gsva_reads`, and
  `gsva_writebacks`; summary scans both guest serial logs and per-node QEMU
  logs so host-side GSVA writebacks are auditable.
- Evidence run:
  - seed: `2026-06-15_w5_p1_gsva_seed`
  - reuse: `2026-06-15_w5_p1_gsva_reuse3`
  - summary:
    `guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.2026-06-15_w5_p1_gsva_reuse3.txt`
  - report status: `pass`
  - output tokens: `[264, 8453, 67926, 5440]`
  - prefix cache: `action=reuse`, `prefix_cache_kv_hits=8`
  - GSVA: `gsva_kv_refs=8`, `gsva_reads=8`, `gsva_writebacks=0`
  - KV stream evidence:
    `guest-linux/aarch64/out/w5_memory_registry.2026-06-15_w5_p1_gsva_reuse3/w5_memory_prefix_cache_kv_stream.txt`

Follow-up:

- The current evidence proves the GSVA-backed KV audit/use path, not a
  performance benefit.

### P1.2 GSVA Token/Epoch/Retire Guard for W5 Cache

GSVA is valuable to W5 only if it prevents stale or unauthorized cache reuse.

Required negative tests:

- token revoked before reuse -> prefix/KV reuse rejected;
- epoch mismatch -> reuse rejected;
- retired segment -> reuse rejected;
- checksum mismatch -> reuse rejected;
- cache rejection records `cache_reject_then_recompute` and completes decode.

Acceptance:

- W5 summary reports the rejection reason.
- Health check fails if stale GSVA cache is silently accepted.

Progress as of 2026-06-15:

- Schema/unit tests reject zero token, zero epoch, retired segment, and checksum
  mismatch.
- Guest parser rejects stale GSVA prefix-cache stream entries fail-closed but
  no longer aborts the decode for cache-staleness cases. It records
  `qwen3_w5_memory_prefix_cache_gsva_rejected`, skips the stale KV entry, and
  forces the normal range-forward runtime recompute path to complete the decode.
- Summary/report surface `prefix_cache_gsva_rejections` and
  `prefix_cache_gsva_rejection_reasons`.
- Summary/report also require the stale-cache recompute triplet:
  `prefix_cache_reject_policy=cache_reject_then_recompute`,
  `prefix_cache_recompute_range_forwards>0`, and
  `prefix_cache_reject_then_recompute=1`.
- `SIM_W5_MEMORY_GSVA_EXPECTED_EPOCH` provides an explicit negative-test gate
  for epoch mismatch without changing default runs.
- Full Qwen3-14B stale-GSVA negative control:
  - run: `2026-06-15_w5_p1_gsva_stale_epoch`
  - summary:
    `guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.2026-06-15_w5_p1_gsva_stale_epoch.txt`
  - report status: `pass`
  - output tokens: `[264, 8453, 67926, 5440]`
  - prefix cache: `action=reuse`, `prefix_cache_kv_hits=0`
  - rejection: `prefix_cache_gsva_rejections=64`,
    `prefix_cache_gsva_rejection_reasons=epoch_mismatch`
  - reject/recompute evidence:
    `prefix_cache_reject_policy=cache_reject_then_recompute`,
    `prefix_cache_recompute_range_forwards=32`,
    `prefix_cache_reject_then_recompute=1`, `gsva_reads=0`

### P1.3 GSVA-backed Prefix Cache Plan

After KV segment prototype passes, prefix-cache plans can return GSVA segment
refs instead of only object-registry refs.

Plan record should include:

- prefix span;
- layer range;
- GSVA segment descriptor;
- token and epoch;
- producer run id;
- checksum and payload shape;
- compatibility object ref if old consumers still need object-registry lookup.

Acceptance:

- Prefix-cache hit uses `backend=gsva`.
- Output guard passes.
- Token/epoch/retire negative cases fail closed.
- Timing report separates lookup time, GSVA map/read time, and avoided compute.

Progress as of 2026-06-15:

- Prefix-cache hit can now consume KV refs with `backend=gsva` metadata.
- Handoff timing now emits `kv_backend`, `gsva_lookup_ms`,
  `gsva_map_read_ms`, and `prefix_cache_avoided_compute_ms`.
- Summary/report emit `gsva_timing`.
- Current `prefix_cache_avoided_compute_ms` remains conservative and may be
  zero until the prefix-cache planner can attribute skipped prefix compute
  directly.

## P2: Device-backed W5 Payloads

### P2.1 UB SSD as Durable Cache Backend

Current UB SSD GSVA tests validate block-object semantics through memory
backend. W5 should not claim durable SSD benefit until a durable backend exists.

Work items:

- add host-file or AIO-backed SSD mode;
- bind W5 prefix/KV artifacts to SSD object semantics;
- define crash/restart boundary;
- run W5 prefix-cache restore from SSD-backed store.

Acceptance:

- Cache survives process restart according to the documented simulator
  boundary.
- Corrupt snapshot/import is rejected.
- Store size and restore time are reported.

Progress as of 2026-06-15:

- Host-file SSD mode is represented by the durable store JSON plus external
  block sidecar (`<store>.bin`) produced by
  `save_lingqu_memory_durable_store_to_path` when block payloads exceed the
  externalization threshold.
- Simulator crash/restart boundary: prefix-cache manifest metadata lives in
  durable DFS JSON; W5 KV/prefix payload bytes live in durable block records
  and may be hydrated from the host-file sidecar after a new process loads the
  same store path. Removing or corrupting either file is treated as import
  failure, not a cache miss.
- Restore/report CLI:

```text
sim-cli lingqu-memory restore-prefix-cache-ssd --store <store.json> --report <report.json>
```

- Restore reports `backend=ub_ssd_host_file`, artifact counts, durable payload
  ref count, durable payload bytes, store JSON bytes, sidecar bytes, restore
  time, and a payload proof checksum.

### P2.2 UB NPU as GSVA Tensor Consumer

Use NPU first for a small W5 tensor operation, not a full performance model.

Work items:

- NPU reads GSVA hidden/KV/input tensor;
- NPU writes GSVA output tensor;
- W5 verifies checksum and shape;
- token/epoch/retire guards apply to device access.

Acceptance:

- Device path appears in W5 summary.
- CPU reference produces the same output checksum.
- Rejected token/epoch/retire cases are tested.

Progress as of 2026-06-15:

- `npu_gsva_test` emits W5 device tensor records for a GSVA-backed
  `NPU_OP_VECTOR_ADD_U32` consumer:
  - NPU reads GSVA input tensors A/B;
  - NPU writes GSVA output tensor C;
  - guest validates elementwise CPU reference parity and emits CPU/device checksum
    parity plus output shape.
- `npu_gsva_test` emits W5 device rejection records for token, stale epoch,
  and retired segment guards.
- `w4_guest_run_summary.py` emits `w5_device_summary` with device/backend/op,
  checksum parity, shape verification, rejection guard/reason, and status.
- `w5_inference_run_report.py --require-device-gsva` fails unless a W5 summary
  contains NPU+GSVA tensor consumer evidence, checksum/shape parity, and
  token/epoch/retire rejection evidence.

## Acceptance Gates

### Gate A: Prefix Cache Main Path

Status target: required before claiming prefix cache benefit.

Required:

- 16-step Qwen3-14B seed run passes.
- 16-step Qwen3-14B prefix-cache reuse run passes.
- `prefix_cache_ids` is non-empty.
- prefix-cache service ready log exists.
- output guard passes.
- no-prefix-cache versus prefix-cache timing comparison is emitted.
- mismatched-prefix run misses and falls back.

### Gate B: GSVA-backed KV

Status target: required before claiming GSVA benefits W5.

Required:

- W5 run emits `backend=gsva` for at least one KV cache object class.
- Consumer resolves and uses GSVA-backed KV.
- output guard passes.
- token/epoch/retire negative cases fail closed.
- timing report separates GSVA overhead from avoided compute/copy.

### Gate C: GSVA-backed Prefix Cache

Status target: required before claiming GSVA accelerates prefix cache.

Required:

- prefix-cache hit returns GSVA segment refs.
- W5 maps/reads the segment successfully.
- output guard passes.
- stale GSVA cache is rejected.
- no-GSVA prefix cache versus GSVA prefix cache comparison is emitted.

## Tracking Table

| Area | Current status | Next milestone | Done when |
| --- | --- | --- | --- |
| Prefix cache schema | Partial infrastructure | Define key and proof fields | Mismatch tests fail closed |
| Prefix cache seed registration | Partial | Seed emits reuse plan | Plan can be listed and validated |
| Prefix cache reuse | Not proven in W5 main path | Reuse run has non-empty `prefix_cache_ids` | Output guard and timing pass |
| Prefix cache service | Implemented path, not main evidence | Runner starts and injects service addr | Ready log and guest env prove it |
| GSVA W5 KV | Not connected | One KV object uses GSVA backend | Consumer uses it and output passes |
| GSVA stale guards | Not connected to W5 | Token/epoch/retire negative tests | Silent stale reuse impossible |
| GSVA prefix cache | Not started | Prefix cache plan returns GSVA refs | Hit path uses GSVA and passes |
| UB SSD W5 cache | Not started | Durable backend plan | Restart/restore evidence exists |
| UB NPU W5 tensor op | Not started | Small tensor op via GSVA | CPU/device checksum match |

## Non-goals For The Next Phase

- Do not replace the entire W5 data plane with GSVA in one step.
- Do not claim prefix cache benefit from shortpath `jump-to-terminal` speedup.
- Do not treat GVA direct as equivalent to GSVA identity coherence.
- Do not claim real NPU/SSD performance from functional simulator tests.
- Do not enable approximate terminal commits by default.

## Immediate Execution Order

1. Add a hard W5 prefix-cache acceptance config.
2. Make seed runs publish a validated prefix-cache reuse plan.
3. Make reuse runs start prefix-cache service and prove non-empty
   `prefix_cache_ids`.
4. Add same-prompt and mismatched-prompt A/B timing reports.
5. Prototype GSVA-backed per-node KV segment.
6. Add GSVA token/epoch/retire stale-cache negative tests.
7. Upgrade prefix-cache plans to optionally carry GSVA segment refs.

The expected first real milestone is not a new large feature. It is a report
showing:

```text
no-prefix-cache baseline: pass
prefix-cache reuse: pass, prefix_cache_ids != none
prefix-cache mismatch: pass with miss/recompute
timing: emitted and attributable to prefix cache, not shortpath
```
