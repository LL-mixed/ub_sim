# W5 Engram Simpler-Host Context Op Design

Status: paper-Engram backend scaffold. This document covers the executable
gather/gate/residual context operator after Engram indices and table refs are
already available. It does not define tokenizer compression, canonical ngram
generation, trained Engram table construction, or layer-level placement. The
canonical paper-aligned plan is
`docs/plans/2026-05-22-paper-engram-alignment-plan.md`.

## Goal

Add a runnable W5 inference-cluster Engram context-augmentation backend that
does not depend on A5/CANN fused-SIMT runtime availability.

The target is a `simpler-host` implementation of the existing
`EngramContextOp` contract:

```text
output[p, d] = hidden[p, d]
             + sigmoid(dot(hidden[p], gate_weight[p]) + bias)
             * mean(table[indices[p, 0..7], d])
```

This is a context augmentation before logits. It must not replace the current
decode-time token policy, stop-token priority, or engram selected-token
writeback checks.

## Decision

Use a new custom simpler HostBuildGraph profile:

```text
simpler-host-engram-context
```

Do not implement the operator by forcing the lookup into existing
`host_matmul`.

Why:

- The operator is gather/reduce/dot/elementwise, not dense GEMM.
- A one-hot matmul representation of `table[indices]` would move and compute
  mostly zeros, which hides the real memory behavior we care about.
- Existing `host_vector` proves the HostBuildGraph/AIV dispatch path works,
  but its fixed formula is only an example. The right path is a dedicated AIV
  kernel/profile with the same operator contract as the CPU reference.
- `host_matmul` remains useful as a reference for artifact production,
  manifest wiring, and dispatch integration, not as the core implementation.

## Non-Goals

- Do not emulate SIMT register-forwarding or D-cache behavior exactly. The
  simpler-host backend is a runnable semantic backend and performance baseline,
  not a microarchitectural replacement for the vendor fused-SIMT kernel.
- Do not change the inference workload token policy. Token selection still
  happens through the existing engram policy path.
- Do not make W5 inference cluster validation require Ascend tooling by
  default.
- Do not implement full 14B optimization in the first patch. The interface must
  support `D=5120`, but the first acceptance run can target Qwen3-0.6B
  `D=1024`.

## Existing References

- Vendor operator note:
  `vendor/pto-isa/kernels/manual/a5/engram_simt/README.md`
- CPU reference:
  `crates/sim-models/src/engram_context.rs`
- Current artifact producer:
  `guest-linux/aarch64/scripts/prepare_simpler_host_artifacts.py`
- Current simpler dispatch examples:
  - `run_host_vector_chipbackend()` in `crates/sim-uapi/src/lib.rs`
  - `run_host_matmul_smoke()` in `crates/sim-uapi/src/lib.rs`

## Operator Contract

Inputs:

| Name | Type | Shape | Notes |
| --- | --- | --- | --- |
| `table` | `f32` | `[R, D]` | Object-backed runtime rows resolved from `EngramStateObjectRef`. |
| `indices` | `u32` | `[B, 8]` | Each value must be `< R`. |
| `hidden` | `f32` | `[B, D]` | Final hidden from terminal Qwen range. |
| `gate_weight` | `f32` | `[B, D]` | Object-backed gate tensor resolved from `EngramStateObjectRef`. |
| `output` | `f32` | `[B, D]` | Augmented hidden. |
| `bias` | `f32` | scalar | Default `0.125`, matching vendor note. |

Initial runtime values:

- `B=1` for W5 decode.
- `D=1024` for Qwen3-0.6B.
- `D=5120` must be representable by the descriptor for Qwen3-14B, but may
  require chunked execution before it is enabled by default.
- `R` comes from the memory-service-produced table object. Tests can choose
  small values, but runtime decode must not create a table from a row-count
  environment fallback.

## Data Layout

All tensors are contiguous row-major buffers:

```text
table[row, dim]       => table[row * D + dim]
hidden[batch, dim]    => hidden[batch * D + dim]
gate_weight[batch, d] => gate_weight[batch * D + dim]
output[batch, dim]    => output[batch * D + dim]
indices[batch, head]  => indices[batch * 8 + head]
```

The UAPI descriptor should pass object/segment-backed operands, not inline
tensor contents. The adapter resolves/maps those buffers before dispatching to
the simpler backend.

## Simpler HostBuildGraph Plan

Add a new profile to `prepare_simpler_host_artifacts.py`:

```text
--profile host_engram_context
```

It emits:

```text
/tmp/simpler-host-engram-context-artifacts/
  host_engram_context_manifest.json
  runtime_host.bin
  runtime_aicpu.bin
  runtime_aicore.bin
  orchestration.so
  kernel_func_0.bin
  kernel_func_1.bin
  kernel_func_2.bin
```

Manifest profile:

```json
{
  "profile": "HostEngramContext",
  "runtime_variant": "HostBuildGraph",
  "callable_hint": "host_engram_context_example",
  "simpler_runtime": {
    "orch_function_name": "build_engram_context_graph",
    "args_template": [
      {"kind": "input", "name": "table"},
      {"kind": "input", "name": "indices"},
      {"kind": "input", "name": "hidden"},
      {"kind": "input", "name": "gate_weight"},
      {"kind": "output", "name": "output"},
      {"kind": "inout", "name": "gate_state"},
      {"kind": "scalar_u64", "name": "batch"},
      {"kind": "scalar_u64", "name": "table_rows"},
      {"kind": "scalar_u64", "name": "hidden_size"},
      {"kind": "scalar_u64", "name": "chunk_offset"},
      {"kind": "scalar_u64", "name": "chunk_elems"},
      {"kind": "scalar_f32_bits", "name": "bias"}
    ]
  }
}
```

## Kernel Decomposition

Use a chunked design so the contract can grow from `D=1024` to `D=5120`
without changing the public interface.

Current implementation note:

- Default execution uses one dispatch over the full hidden size. This avoids
  paying HostBuildGraph launch overhead once per chunk.
- Explicit `SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS=N` still forces
  chunked execution for stress and regression validation.
- Chunked execution carries a small `gate_state: f32[B]` inout buffer. The
  first chunk computes and stores the gate; later chunks reuse it instead of
  repeating the full dot product.

### Stage 0: Gather Mean And Partial Dot

Core type: AIV.

For each `batch` and `[chunk_offset, chunk_offset + chunk_elems)`:

```text
agg[p, d] = sum(table[indices[p, h], d]) / 8
partial_dot[p, chunk] = sum(hidden[p, d] * gate_weight[p, d])
```

Outputs:

- `agg_chunk: f32[B, chunk_elems]`
- `partial_dot: f32[B, num_chunks]`

For the first MVP, `num_chunks=1` for `D=1024` is acceptable. The code should
still carry `chunk_offset` and `chunk_elems` in the descriptor.

### Stage 1: Reduce Gate

Core type: AIV.

For each batch:

```text
dot = sum(partial_dot[p, *]) + bias
gate[p] = sigmoid(dot)
```

Output:

- `gate: f32[B]`

For `D=1024` this can be one partial dot. For `D=5120`, this becomes a real
cross-chunk reduction.

### Stage 2: Apply Residual

Core type: AIV.

For each chunk:

```text
output[p, d] = hidden[p, d] + gate[p] * agg[p, d]
```

Output:

- `output_chunk: f32[B, chunk_elems]`

The orchestration can write chunks into the final output buffer at the correct
offset.

## W5 Integration

Add a context-op mode:

```text
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=simpler-host
```

CLI should accept:

```text
--engram-context-op=simpler-host
```

Environment:

```text
SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST=/tmp/simpler-host-engram-context-artifacts/host_engram_context_manifest.json
SIM_QWEN3_GUEST_ENGRAM_STATE_REF=<EngramStateObjectRef wire hex>
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS=<optional explicit chunk size>
```

`EngramStateObjectRef` resolves a materialized Engram view, not a shortpath
decision. The referenced state must carry the table, indices, gate object,
operator kind/config hash, tensor shape, checksum, version, and optional model
bindings. Simpler-host only consumes the mapped tensor refs. It must not infer
range-skip behavior from the Engram state itself.

`sim-uapi` flow:

1. Terminal Qwen range forward produces final hidden.
2. If context op is `simpler-host`, resolve/map:
   - table object from `EngramStateObjectRef`;
   - indices object from `EngramStateObjectRef`;
   - terminal hidden;
   - gate-weight object from `EngramStateObjectRef`;
   - output buffer.
3. Build a `DispatchBackendSpec` from
   `host_engram_context_manifest.json`.
4. Submit simpler backend dispatch.
5. Replace terminal hidden with the simpler output.
6. Compute full-vocab logits from the augmented hidden.
7. Emit the same `qwen3-engram-context` report shape currently used by
   `cpu-reference`.

Shortpath flow is separate from this terminal context-op path:

1. A range exit publishes `hidden_ref`.
2. W5 builds `BoundaryLookupRequest { model, boundary, hidden_ref,
   engram_state_id, allowed_actions }`.
3. Lingqu Memory Service returns `ShortpathSupportRecord`.
4. W5 Boundary Planner turns that support into `ShortpathDecisionRecord` after
   applying runtime policy.
5. W5 continues, jumps to a downstream layer, jumps to terminal logits, or
   enters verify mode based on that decision.

This separation matters: `EngramStateObject` proves which semantic memory view
and operator config are being used; `ShortpathSupportRecord` proves which
model-native execution artifact was available; `ShortpathDecisionRecord`
proves which runtime choice was actually taken and carries the evaluated
`support_id` when the choice is based on Memory Service evidence.

Prefetch is also separate from terminal context-op execution. At a range start,
W5 can build `PrefetchPlanRequest { model, boundary, engram_state_id, scope,
lookahead_steps, artifact_kinds }` to schedule range, step, or n-step artifact
materialization before a later range exit needs it. The simpler-host context op
does not own that scheduling policy; it only consumes tensors that have already
been resolved and mapped.

The summary parser does not need a new output schema. It already consumes:

```text
qwen3-engram-context: mode=... table_rows=... output_checksum=...
```

The only new value should be:

```text
mode=simpler-host
```

## CLI And UX

Default remains unchanged:

```text
--engram-context-op=disabled
```

CPU reference remains available:

```text
--engram-context-op=cpu-reference
```

New mode:

```text
--engram-context-op=simpler-host
```

Failure behavior must be explicit:

- missing manifest:
  `missing_simpler_host_engram_context_manifest:...`
- unsupported hidden size:
  `qwen3_engram_context_simpler_hidden_size_unsupported:got=...`
- unsupported batch:
  `qwen3_engram_context_simpler_batch_unsupported:got=...`
- dispatch failure:
  `simpler_host_engram_context_dispatch_failed:...`

No silent fallback to CPU reference is allowed.

## Test Plan

Unit tests:

- parser accepts `--engram-context-op=simpler-host`;
- env propagation includes `SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=simpler-host`;
- missing manifest reports an actionable error;
- simpler-host output matches `run_engram_context_reference()` for:
  - `B=1, D=1024, R=16`;
  - at least one multi-chunk object-ref-backed case if chunk support lands in
    the same patch.

Python/script tests:

- artifact producer can build or describe `host_engram_context`;
- manifest has expected `profile`, `callable_hint`, and args template.

W5 validation:

```text
RUN_ID=w5_engram_simpler_host_context_0_6b_4step_YYYYMMDD
SIM_UAPI_W5_PROFILE=qwen3_0_6b_engram_decode
SIM_QWEN3_DENSE_WEIGHTS_PATH=/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B
SIM_QWEN3_GUEST_DECODE_STEPS=4
SIM_QWEN3_GUEST_ENGRAM=1
SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE=8
SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE=3
SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW=64
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=simpler-host
./guest-linux/aarch64/scripts/run_ub_eight_node_w5_inference_cluster.sh
```

The legacy-compatible eight-node guest decode runner can still execute the
same environment for historical comparisons, but new validation should use the
W5 inference cluster runner.

Acceptance:

- run passes;
- `engram_context_records=4`;
- `engram_context_summary` reports `modes=simpler-host-object-ref`;
- per-step output checksums match CPU-reference mode for the same
  `EngramStateObjectRef`;
- token IDs match CPU-reference mode when object refs are identical.

## Implementation Order

1. Add `host_engram_context` profile generation in
   `prepare_simpler_host_artifacts.py`.
2. Add a unit-level simpler dispatch helper in `sim-uapi`, isolated from the
   W5 runner.
3. Compare helper output against `sim-models::engram_context` CPU reference.
4. Add `simpler-host` parser/env plumbing in `sim-cli` and guest runner
   scripts.
5. Wire `simpler-host` into terminal Qwen range forward.
6. Run W5 0.6B 4-step with `SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=simpler-host`.
7. Only after 0.6B is stable, enable chunked `D=5120` validation for 14B.

Status as of 2026-05-16:

- Steps 1-6 are implemented and validated.
- 14B `D=5120` is implemented and validated.
- Explicit multi-chunk mode is implemented; the default is now full-hidden
  single dispatch because measured 14B latency was dominated by repeated
  HostBuildGraph launch overhead, not by the dot product itself.
- 14B 2-step W5 validation with explicit `chunk_elems=1024` produced context
  latencies `5054ms` and `3620ms`.
- 14B 2-step W5 validation with default full-hidden dispatch produced context
  latencies `2392ms` and `832ms`, with the same token IDs and output
  checksums.
- 2026-05-19 Memory Service-backed W5 validation also passed through the
  `simpler-host` object-ref path. Run
  `w5_memory_bootstrap_simpler_host_0_6b_2step_20260519` consumed a real
  `EngramStateObject` produced from Lingqu Memory Service durable outputs,
  reported `modes=simpler-host-object-ref`, and produced terminal tokens
  `[11, 108386]`. The same Memory Service state passed with
  `cpu-reference-object-ref` in
  `w5_memory_bootstrap_cpu_ref_0_6b_2step_20260519b`.
- The preferred Memory Service bootstrap path no longer needs per-object qwen3
  registry payload files for Engram context. Run
  `w5_memory_object_service_snapshot_simpler_host_0_6b_2step_20260519`
  consumed `SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT`, reported
  `modes=simpler-host-object-ref`, and produced the same terminal tokens
  `[11, 108386]`. Its observed context latencies were 2993ms and 950ms.
- Memory decision artifact refs now follow the same Object Service snapshot
  contract for hidden/KV/logits payloads. The snapshot export includes a
  compact payload index for guest-side terminal logits validation, so
  jump-to-terminal artifact validation is no longer coupled to qwen3 registry
  payload files. Live per-step range-output publication now carries ObjectRefs
  and sim-uapi validates the inline OBMM/UAPI payload view against those refs
  before backend execution, so the streaming path no longer needs qwen
  `kind*.bin` registry payloads for hidden/KV materialization or default
  live range-output publication. The qwen registry bridge remains only for
  explicit legacy runs that set `SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR`.

## Risks

- Simpler HostBuildGraph launch overhead may dominate for `B=1`. This backend
  is primarily a semantic integration step; throughput wins are not guaranteed.
- A dedicated AIV kernel is still not the same memory path as vendor SIMT. It
  should be compared against CPU-reference and used to exercise UAPI/backend
  integration, not used as proof of final SIMT speedup.
- `D=5120` can run as either one full-hidden dispatch or explicit chunks.
  Explicit chunks remain useful to validate object/operand ranges, but should
  not be the default for W5 throughput because launch overhead dominates.
- Runtime validation inputs must be persisted object payloads with stable
  checksums. Otherwise token IDs will drift and W5 comparisons will become
  noisy.

## Open Questions

- Should `simpler-host` eventually replace `cpu-reference` for local W5
  validation, or remain an opt-in backend used only for backend-path testing?
