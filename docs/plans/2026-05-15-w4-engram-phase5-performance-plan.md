# W4 Engram Phase 5 Performance Plan

## Status

W4 guest decode engram support is functionally present enough to treat Phase 5
as a performance project, not a correctness bring-up project.

Current completed capabilities:

- CPU/guest-side engram policy selects a token from bounded top-K candidate
  metadata.
- OBMM/object-service transport publishes and resolves candidate, selected
  token, history, and engram state objects.
- Terminal token writeback is validated against engram selection.
- Existing unit tests cover policy behavior and OBMM transport report parsing.
- A 24-step eight-node run exists as a performance baseline:
  `docs/2026-05-13-eight-node-w4-timing-report.md`.
- 2026-05-16: P5.0 profiling gate is implemented. W4 guest logs now emit
  `qwen3_engram_timing`, and `w4_guest_run_summary.py` emits
  `engram_timing_step` plus `engram_bottleneck`.
- 2026-05-16: P5.1 CPU/reference `EngramContextOp` is implemented in
  `sim-models::engram_context`, with a standalone
  `engram_context_reference` CLI and deterministic checksum tests.
- 2026-05-16: P5.2/P5.5 host-side fused-SIMT adapter discovery is
  implemented. `sim-models::engram_simt_adapter` validates artifact layout,
  selects `runEngram_fused_E{D}_B{B}`, and `sim-cli` accepts
  `--engram-mode=fused-simt` as an opt-in mode with host-side artifact checks.
- 2026-05-16: P5.3 external contract scaffolding is implemented:
  `--engram-context-op=disabled|cpu-reference|fused-simt` parses, exports
  `SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP`, and guest decode fails fast for context
  ops that are not actually wired.
- 2026-05-16: P5.3 CPU-reference range-runtime integration is implemented.
  With `SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=cpu-reference`, the terminal range
  forward path augments the final hidden vector before full-vocab logits, so
  the selected token is driven by the augmented hidden rather than a side
  report. `fused-simt` remains fail-fast until the A5/CANN runtime launch path
  is connected.
- 2026-05-16: W4 run summaries now parse `qwen3-engram-context` records from
  QEMU logs and emit `engram_context_summary` plus per-step checksum/latency
  records. This makes context-op execution observable without manual log grep.

Current gaps:

- `--engram-mode` supports `cpu` and opt-in `fused-simt` parsing/artifact
  discovery, but W4 guest decode still rejects `fused-simt` until P5.3 wires a
  context-op execution path.
- The decode-time token policy is intentionally still CPU/guest-side because
  P5.0 shows it is not a throughput bottleneck.
- The hidden/context engram augmentation now has a CPU/reference operator and a
  W4 terminal range-runtime integration path for Qwen3-0.6B hidden size
  (`D=1024`).
- The vendor fused Engram SIMT kernel is not connected to the W4 guest decode
  path.
- P5.0 now isolates engram policy cost from object transport, range handoff,
  candidate wait, selected-token wait, and publish latency. The remaining
  timing gap is fused-context-op delta reporting after P5.1-P5.3 exists.

## Goal

Phase 5 must answer one question before moving code into the hot path:

> Which part of engram decode is actually limiting user-visible throughput?

The user-visible target is not "use the fused kernel". The target is lower
token latency for W4 guest decode with engram enabled, while preserving the
current correctness contract:

- stop token handling has priority over engram policy;
- no-repeat/repetition behavior remains deterministic;
- selected token equals terminal token writeback;
- Qwen forward is still executed through the existing W4 range/simpler path;
- all new fast paths have CPU/reference parity tests.

## Existing Vendor Asset

The relevant vendor exploration is:

```text
vendor/pto-isa/kernels/manual/a5/engram_simt/
```

It provides:

- a fused GM-SIMT kernel for DeepSeek-style Engram lookup/aggregate/gated
  residual add;
- baseline and fused launch wrappers;
- compile-time `D` and `B` variants through `FusedEngramImpl<D, B>`;
- simulator/NPU run scripts and generated test cases.

Important mismatch:

- Current W4 engram means decode-time token policy over history and top-K
  candidates.
- The vendor kernel means a hidden-state memory/context layer:
  `hidden + sigmoid(dot(hidden, gate_weight) + bias) * mean(table[indices])`.

Therefore the vendor kernel is reusable as a Phase 5 acceleration building
block, but it cannot directly replace the existing token policy.

## Phase 5 Workstreams

### P5.0 Profiling Gate

Status: complete as of 2026-05-16.

Purpose: prove where latency is going before adding a fused operator.

Add explicit timings for:

- `engram_candidate_publish_ms`
- `engram_candidate_wait_ms`
- `engram_policy_select_ms`
- `engram_decision_publish_ms`
- `engram_selected_wait_ms`
- `engram_selected_writeback_ms`
- `engram_history_state_wait_ms`
- `qwen3_range_publish_ms`
- `qwen3_range_input_wait_ms`

Required output:

```text
[w4_guest] stage qwen3_engram_timing step=N
  candidate_publish_ms=...
  candidate_wait_ms=...
  policy_select_ms=...
  decision_publish_ms=...
  selected_wait_ms=...
  selected_writeback_ms=...
  history_state_wait_ms=...
  status=ok
```

Acceptance:

- `cargo test -p sim-cli qwen3_engram` passes.
- Eight-node W4 engram run prints an engram timing summary.
- The summary identifies whether CPU policy, OBMM object transport, or range
  pipeline wait dominates token latency.

Validation run:

```text
RUN_ID=w4_engram_p5_timing_0_6b_8step_20260516
SIM_UAPI_W4_CHIPBACKEND_PROFILE=qwen3_dense
SIM_QWEN3_DENSE_WEIGHTS_PATH=/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B
SIM_QWEN3_GUEST_DECODE_STEPS=8
SIM_QWEN3_GUEST_ENGRAM=1
SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE=8
SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE=3
SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW=64
./guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh
```

Summary:

```text
guest-linux/aarch64/out/eight_node_w4_guest_summary.w4_engram_p5_timing_0_6b_8step_20260516.txt
```

Observed result:

- PASS.
- Output text: `, I'm a bit confused about the`.
- `engram_timing_records=64`.
- `engram_bottleneck: dominant=range_pipeline dominant_ms=19557 cpu_policy_ms=0 object_transport_ms=1584 range_pipeline_ms=19557`.
- Max object transport record: step6 / nodeH, `object_transport_ms=155`.
- Max range record: step0 / nodeH, `qwen3_range_input_wait_ms=4258`, `qwen3_range_publish_ms=9`.

Interpretation:

- CPU token policy is below current millisecond resolution and is not the
  throughput limiter.
- OBMM engram object transport is visible but much smaller than the range
  pipeline wait.
- P5.4 token-policy micro-kernel is not justified by current data.
- P5.1-P5.3 can still proceed as an optional context augmentation path, but
  expected end-to-end gain is bounded unless range pipeline wait is reduced.

### P5.1 Reference Operator Boundary

Status: complete as of 2026-05-16.

Purpose: define a stable operator contract before wiring vendor code.

Add a simulator-side reference op with this logical shape:

```text
EngramContextOp {
  table: f32[R, D],
  indices: i32[B, 8],
  hidden: f32[B, D],
  gate_weight: f32[B, D],
  output: f32[B, D],
}
```

Initial supported dimensions:

- `D = 1024`, because Qwen3-0.6B hidden size is 1024;
- `B = 1`, `4`, `16`, `64`, matching vendor kernel instantiations;
- `R` runtime-configurable, starting with `65536`.

Acceptance:

- CPU/reference implementation has deterministic checksum tests.
- The op can be called without changing the existing token policy.
- The op is represented in reports as context augmentation, not token
  selection.

Implementation:

- Module: `crates/sim-models/src/engram_context.rs`.
- CLI: `cargo run -p sim-models --bin engram_context_reference -- --batch=4 --rows=16`.
- Report kind: `context_augmentation`.
- Current formula:

```text
gate = sigmoid(dot(hidden[b], gate_weight[b]))
output[b, d] = hidden[b, d] + gate * mean(table[indices[b, 0..8], d])
```

Validation:

- `cargo test -p sim-models engram_context`
- `cargo run -p sim-models --bin engram_context_reference -- --batch=4 --rows=16`

### P5.2 Vendor Kernel Adapter

Status: host-side discovery and opt-in plumbing complete as of 2026-05-16;
runtime launch and fused golden parity still require an A5/CANN environment.

Purpose: reuse the vendor exploration without modifying vendor source first.

Add an adapter layer that can:

- build or locate the `engram_simt` fused kernel artifact;
- expose a narrow host-side launch API;
- select `runEngram_fused_E{D}_B{B}` by `D` and `B`;
- fall back with an actionable error when CANN/A5 runtime is unavailable.

The adapter must not make W4 decode depend on local Ascend tooling by default.
The feature must be opt-in.

Proposed mode names:

```text
--engram-mode cpu
--engram-mode fused-simt
```

Environment:

```text
SIM_QWEN3_GUEST_ENGRAM_MODE=cpu|fused-simt
SIM_ENGRAM_SIMT_ARTIFACT_DIR=/path/to/engram_simt/build
```

Acceptance:

- Parser tests cover `fused-simt`.
- Missing artifact/runtime produces a clear error.
- CPU/reference and fused outputs match within tolerance for golden cases.
- Existing `cpu` mode behavior is unchanged.

Implementation:

- Module: `crates/sim-models/src/engram_simt_adapter.rs`.
- CLI discovery path:

```text
cargo run -p sim-models --bin engram_context_reference -- \
  --mode=fused-simt \
  --batch=4 \
  --rows=65536 \
  --artifact-dir=vendor/pto-isa/kernels/manual/a5/engram_simt/build
```

- `sim-cli qwen3-guest-decode-loop --engram-mode=fused-simt` now parses and
  validates `SIM_ENGRAM_SIMT_ARTIFACT_DIR` before launching QEMU.
- Guest C rejects non-`cpu` `SIM_QWEN3_GUEST_ENGRAM_MODE` with an explicit
  P5.3-required error, so direct shell runs cannot silently fall back to CPU.

Validation:

- `cargo test -p sim-models engram_simt`
- `cargo test -p sim-models cli_args`
- `cargo test -p sim-cli qwen3_guest_decode_loop_args_accept_fused_simt_engram_mode`
- `cargo test -p sim-cli qwen3_guest_engram_env_vars_include_policy_knobs`
- Missing artifact smoke emits
  `engram_simt_artifact_dir_missing:path=...:hint=build with ... run.sh`.

### P5.3 W4 Decode Integration

Status: CPU-reference terminal hidden augmentation is wired as of 2026-05-16;
fused execution is still pending.

Purpose: introduce the fused op without violating the current decode contract.

Integration rule:

- fused SIMT may augment hidden/context state before logits or candidate
  generation;
- fused SIMT must not replace stop-token priority or token policy decisions;
- final terminal token must still be checked against engram selected token.

Initial integration should be behind a separate flag:

```text
--engram-context-op fused-simt
```

This avoids overloading `--engram-mode`, which currently describes token policy
placement.

Current contract:

- `sim-cli qwen3-guest-decode-loop` accepts
  `--engram-context-op=disabled|cpu-reference|fused-simt`.
- `fused-simt` context op validates the same `SIM_ENGRAM_SIMT_ARTIFACT_DIR`
  discovery path used by P5.2 before QEMU launch.
- `cpu-reference` is allowed in guest decode and is consumed by `sim-uapi`
  during terminal range forward. The op mutates the final hidden vector before
  logits are computed.
- `fused-simt` still fails fast in guest decode with a clear runtime-launch
  integration-required error, preventing a silent CPU fallback.

Implementation:

- `crates/sim-uapi/src/lib.rs` reads
  `SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=cpu-reference` in the Qwen3 range-forward
  path.
- The CPU-reference op is applied only on the terminal range
  (`layer_end == total_layers`), after the true transformer range has produced
  hidden state and before full-vocab logits are derived.
- Default context table rows for the runtime path are controlled by
  `SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_ROWS` and default to `16` to keep W4
  guest verification cheap; the standalone operator still supports larger
  golden cases such as `65536`.
- The runtime emits `qwen3-engram-context: ...` with mode, table rows,
  checksum fields, and latency when the op runs.

Validation:

- `cargo test -p sim-uapi qwen3_engram_context_cpu_reference_mutates_terminal_hidden`
- `cargo test -p sim-cli qwen3_engram`
- `python3 -m unittest guest-linux/aarch64/tests/test_w4_guest_run_summary.py guest-linux/aarch64/tests/test_qwen3_dense_env.py`
- `cargo test --workspace`

Acceptance:

- Eight-node W4 decode passes with `--engram --engram-pool obmm`.
- Eight-node W4 decode passes with `cpu-reference` context op enabled.
- Eight-node W4 decode passes with the fused context op enabled where runtime
  support is available.
- Report includes:
  - context op enabled/disabled;
  - table rows;
  - context op checksums;
  - context op latency;
  - total token latency delta versus CPU/reference.

CPU-reference W4 validation:

```text
RUN_ID=w4_engram_p5_context_cpu_0_6b_4step_runtime2_20260516
SIM_UAPI_W4_CHIPBACKEND_PROFILE=qwen3_dense
SIM_QWEN3_DENSE_WEIGHTS_PATH=/Volumes/repos/qwen3_mlx_run/Qwen3-0.6B
SIM_QWEN3_GUEST_DECODE_STEPS=4
SIM_QWEN3_GUEST_ENGRAM=1
SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE=8
SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE=3
SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW=64
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=cpu-reference
./guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh
```

Observed result:

- PASS.
- `engram_context_records=4`.
- Output token IDs changed to `[11, 108386, 6313, 112169]`, proving the
  terminal logits used the augmented hidden state rather than the plain decode
  path.
- `engram_context_summary` reported `records=4 steps=4/4 modes=cpu-reference`
  with per-step output/gate/index checksums.

### P5.4 Token Policy Micro-Kernel Decision

Purpose: only accelerate top-K token policy if profiling proves it matters.

Candidate NPU-side token-policy work:

- top-K candidate filtering;
- rolling history checksum update;
- repeated-token score adjustment;
- no-repeat-ngram lookup for small windows.

Do not implement this before P5.0 proves `engram_policy_select_ms` is a
meaningful share of token latency. Current evidence points more strongly at
range input wait and publish/object transport than at CPU policy math.

Acceptance:

- A design note states why policy micro-kernel is or is not worth doing.
- If implemented, CPU policy remains the reference and every NPU decision has
  parity tests against CPU results.

## Execution Order

1. [x] Add P5.0 timing instrumentation and summaries.
2. [x] Run one short eight-node engram decode and compare against the 2026-05-13
   timing report.
3. [x] Add the CPU/reference `EngramContextOp`.
4. [x] Add the vendor fused kernel adapter behind an opt-in feature.
5. [x] Add CLI/env plumbing for `fused-simt` and artifact discovery.
6. [ ] Wire the fused context op runtime launch into W4 decode behind
   `--engram-context-op`.
7. [x] Run CPU/reference parity tests.
8. [ ] Run fused golden tests where A5/CANN runtime is available.
9. [x] Run eight-node W4 engram decode with CPU-reference context op.
10. [ ] Run eight-node W4 engram decode with fused context op where A5/CANN
    runtime is available.
11. [x] Decide whether a separate token-policy micro-kernel is justified:
    current P5.0 data says no.

## Validation Matrix

| Scope | Command | Required Result |
| --- | --- | --- |
| Rust policy tests | `cargo test -p sim-cli qwen3_engram` | pass |
| Guest env tests | `python3 -m unittest guest-linux/aarch64/tests/test_qwen3_dense_env.py` | pass |
| Workspace tests | `cargo test --workspace` | pass before merge |
| Vendor fused sim | `vendor/pto-isa/kernels/manual/a5/engram_simt/run.sh -r sim -v Ascend910_9599 -c <case>` | pass when runtime exists |
| W4 eight-node CPU mode | `qwen3-guest-decode-loop --engram --engram-pool obmm` | pass |
| W4 eight-node fused mode | CPU mode plus fused context op | pass and emit timing delta |

Any QEMU/W4 guest command must run outside the Codex sandbox per `CLAUDE.md`.

## Risks

- The vendor fused kernel accelerates a hidden/context layer, not the current
  top-K token policy. Treating it as a drop-in replacement would break semantics.
- The 2026-05-13 baseline shows large `input_wait_ms` and `publish_ms`; fused
  compute may not improve end-to-end latency until transport/pipeline waits are
  reduced.
- A5/CANN availability cannot be assumed on every development machine.
- Adding a fused path without CPU/reference parity will make future token
  regressions hard to diagnose.

## Done Definition

Phase 5 is done when:

- engram timing is broken down enough to identify the dominant bottleneck;
- `cpu` mode remains behaviorally unchanged;
- fused context op has CPU/reference parity tests;
- vendor fused implementation is callable through a narrow adapter;
- W4 eight-node engram run can compare CPU/reference and fused modes;
- the final report states whether fused SIMT improves token latency and what
  bottleneck remains.
