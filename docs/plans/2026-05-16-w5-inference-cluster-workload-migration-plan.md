# W5 Inference Cluster Workload Migration Plan

## Goal

Move the current guest decode validation line from the legacy W4 demo framing
to a W5 inference cluster workload framing.

W5 is the standard workload family for validating UB cluster behavior with real
inference execution:

- Qwen decode/prefill execution;
- object-backed handoff;
- persistent KV cache;
- UAPI/backend dispatch;
- multi-node timing and bottleneck analysis;
- optional engram policy and context augmentation.

The name should communicate the user-facing goal: validate the UB cluster with
real inference workloads, not keep extending a historical demo.

## Current State

The legacy W4 guest decode path has grown beyond its original scope:

- eight-node guest decode works;
- Qwen3-0.6B and Qwen3-14B real weights are supported;
- tokenizer output is real, with fallback removed for real-weight paths;
- object-backed handoff is in place;
- real numeric KV cache is persisted;
- engram policy and CPU-reference context augmentation are integrated;
- timing summaries expose node, edge, handoff, engram, and context-op records.

That means W4 has served its bring-up role. Keeping the mainline name as W4 now
obscures what the workload actually validates.

## Naming Decision

Use W5 as the mainline name:

```text
W5: UB Inference Cluster Workload
```

Short names:

```text
w5
w5_inference_cluster
inference_cluster
```

Recommended display name in docs and reports:

```text
W5 inference cluster
```

The old W4 name remains only as a compatibility label:

```text
legacy W4 guest decode
```

## Workload Definition

W5 is not one fixed demo. It is a family of inference cluster validation
profiles.

Initial profiles:

| Profile | Model | Mode | Purpose |
| --- | --- | --- | --- |
| `qwen3_0_6b_decode` | Qwen3-0.6B | decode | Fast correctness and timing loop. |
| `qwen3_14b_decode` | Qwen3-14B | decode | Large-model handoff/KV/tokenizer validation. |
| `qwen3_0_6b_engram_decode` | Qwen3-0.6B | decode + engram | Policy/context-op validation. |
| `qwen3_14b_engram_decode` | Qwen3-14B | decode + engram | Large-model engram validation. |

Future profiles can add long-context, batching, MoE, and other models without
changing the workload family name. `qwen3_prefill_decode` is a reserved future
profile name and must not be accepted by the runner until prefill+decode
behavior is actually implemented.

## User-Facing Command Shape

Introduce new entrypoints:

```text
guest-linux/aarch64/scripts/run_ub_eight_node_w5_inference_cluster.sh
guest-linux/aarch64/scripts/run_ub_w5_inference_cluster.sh
```

The eight-node script can be the first implementation. A generic node-count
wrapper can come later.

Example:

```text
RUN_ID=w5_qwen3_0_6b_decode_8step_YYYYMMDD
SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode
SIM_QWEN3_DENSE_WEIGHTS_PATH=/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B
SIM_QWEN3_GUEST_DECODE_STEPS=8
./guest-linux/aarch64/scripts/run_ub_eight_node_w5_inference_cluster.sh
```

Engram example:

```text
RUN_ID=w5_qwen3_0_6b_engram_context_4step_YYYYMMDD
SIM_UAPI_W5_PROFILE=qwen3_0_6b_engram_decode
SIM_QWEN3_DENSE_WEIGHTS_PATH=/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B
SIM_QWEN3_GUEST_DECODE_STEPS=4
SIM_QWEN3_GUEST_ENGRAM=1
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=simpler-host
./guest-linux/aarch64/scripts/run_ub_eight_node_w5_inference_cluster.sh
```

Compatibility:

- Existing scripts keep working during migration.
- New scripts initially delegate to the existing implementation.
- New docs and new validation reports should use W5 names.

## Environment Variables

Keep existing model-specific variables where they are accurate:

```text
SIM_QWEN3_DENSE_WEIGHTS_PATH
SIM_QWEN3_GUEST_DECODE_STEPS
SIM_QWEN3_GUEST_ENGRAM
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP
```

Add a W5 profile selector:

```text
SIM_UAPI_W5_PROFILE=qwen3_0_6b_decode
```

During compatibility, map W5 profile names to existing backend knobs:

```text
SIM_UAPI_W4_CHIPBACKEND_PROFILE=qwen3_dense
```

New docs should describe the W5 profile first and treat the W4 backend profile
variable as an implementation detail until it is renamed.

## Log And Summary Naming

Add W5 output aliases:

```text
guest-linux/aarch64/logs/<RUN_ID>_headless8/
guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.<RUN_ID>.txt
```

Keep the summary content schema stable:

- `decode_output`
- `timing_step`
- `handoff_step`
- `edge_step`
- `engram_timing_step`
- `engram_context_summary`
- `obmm_pool`

Reason: the schema already describes inference cluster behavior. The problem is
the workload family name, not the timing fields.

## Code Migration Strategy

Phase 1: Alias, no behavior change. Status: implemented.

- Add W5 runner script that delegates to the existing eight-node implementation.
- Add W5 summary output filename while keeping existing summary generation.
- Add docs using W5 names.
- Keep existing tests unchanged except for any new W5 wrapper tests.

Phase 2: Internal naming cleanup. Status: implemented for user-facing entry
points; wire/log compatibility names remain intentionally unchanged.

- Add W5 profile parsing in `sim-cli`.
- Rename user-facing report labels from "w4 guest" to "w5 inference cluster"
  where those labels are not part of a wire/log compatibility contract.
- Keep legacy script names as wrappers.

Phase 3: Workload profile schema. Status: documented only.

- Replace ad hoc environment groups with a profile schema:

```text
profile: qwen3_0_6b_engram_decode
model: qwen3_0_6b
mode: decode
nodes: 8
backend: qwen3_dense
engram:
  enabled: true
  context_op: simpler-host
```

The profile schema can live in docs first, then become a config file when the
runner is ready.

## Documentation Migration

New documents should use W5 names.

Existing documents should be updated opportunistically:

- keep historical run IDs unchanged;
- rewrite forward-looking headings from W4 to W5;
- refer to old scripts as "legacy W4-compatible runners";
- move new engram/context-op design under W5.

Do not bulk-rewrite old timing reports. Those reports are historical evidence
and their run IDs should remain stable.

## Acceptance

Migration is complete when:

| Item | Status |
| --- | --- |
| A new W5 runner exists and can execute the same 8-node decode path. | Implemented: `run_ub_eight_node_w5_inference_cluster.sh`. |
| Summary output has a W5 filename alias. | Implemented: `eight_node_w5_inference_cluster_summary.<RUN_ID>.txt`. |
| Docs describe the mainline workload as W5 inference cluster. | Implemented for forward-looking W5 plans. |
| Legacy W4 commands still work. | Preserved by keeping the legacy runner and compatibility env mapping. |
| A W5 Qwen3-0.6B decode run passes. | Verified: `w5_migration_0_6b_decode_1step_20260516_170746`. |
| A W5 Qwen3-14B decode run passes. | Verified: `w5_migration_14b_decode_1step_20260516_170832`. |
| A W5 engram context-op run emits `engram_context_summary`. | Verified: `w5_migration_0_6b_engram_context_1step_20260516_171046`, `modes=cpu-reference`. |

Latest post-migration validation summaries:

```text
guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.w5_migration_0_6b_decode_1step_20260516_170746.txt
guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.w5_migration_14b_decode_1step_20260516_170832.txt
guest-linux/aarch64/out/eight_node_w5_inference_cluster_summary.w5_migration_0_6b_engram_context_1step_20260516_171046.txt
```

## Risks

- A broad rename can break scripts and historical comparisons. Use aliases
  first.
- The code still contains many W4 identifiers tied to wire formats, tests, and
  log parsing. Rename those only when a compatibility plan exists.
- W5 should not become a catch-all label for unrelated demos. It specifically
  means inference cluster validation.

## Immediate Next Step

Treat the simpler-host Engram context-op design as a W5 feature:

```text
W5 Engram Simpler-Host Context Op
```

The implementation should target the W5 inference cluster runner. The
legacy-compatible W4 runner remains available for historical comparisons and
bisecting old reports.
