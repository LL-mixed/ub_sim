# W6 Managed UB_GM Model-Direct Plan

Date: 2026-09-22. Last verified: 2026-09-24.

Status: Qwen callable 3 and DeepSeek callable 5 are integrated with managed
`UB_GM` serving, and the stable W5 config entry selects `direct` by default
for these two profiles. Functional matrices passed through the cluster runner
on `n4-910c`. The sim-console two-host matrix and the oracle acceptance gate
remain open; the configured default is not a claim of full W6 acceptance.

## 1. Goal

W6 extends the working W5 Memory Service, OBMM, GVA/GSVA and pipeline-parallel
serving path so that a complete Qwen or DeepSeek model range receives its
shared tensors as Memory Service managed `UB_GM` memrefs.

The following tensors belong to the direct contract:

- hidden input and hidden output;
- previous and next KV state;
- token IDs or equivalent bounded control tensor;
- terminal logits;
- any shared model-runtime tensor that crosses a range or request boundary.

The numerical callable must issue PTO `TLOAD/TSTORE` against those memrefs.
The simulator runtime may create private constants and private scratch, but it
may not copy a shared model tensor into a Rust `Vec`, host tensor, inline UAPI
payload or legacy arena before executing the range.

Small control metadata remains CPU-visible: request identity, shape, stride,
layer range, scalar geometry, object references, checksums and completion
records. This exception does not include hidden, KV or logits payload bytes.

## 2. W5 Baseline and W6 Boundary

W5 callable 4 remains a stable comparison and recovery baseline. It uses the
Memory Service managed OBMM provider and preserves service leases correctly,
then reads shared input payloads into Rust-owned bytes, executes the existing
host model runtime and writes result bytes back through provider callbacks.

W6 selects a direct callable per model:

| Path | Tensor address space | Numerical execution | Shared staging |
| --- | --- | --- | --- |
| W5 callable 4 | Host at model boundary | Rust/Simpler host runtime | Nonzero |
| W6 Qwen callable 3 | `UB_GM` | complete PTO decoder range | Zero |
| W6 DeepSeek callable | `UB_GM` | complete PTO DeepSeek range | Zero |

Callable 4 cannot satisfy W6 acceptance. A W6 run fails if it dispatches
callable 4, invokes bulk provider read/write for a shared tensor, or reports
nonzero shared payload staging.

## 3. Implemented Foundation

### 3.1 Model-neutral managed-direct operation

`lingqu_model_direct_guest.h/.c` accepts only:

- Memory Service mapping owners;
- tensor offset, length, dtype, shape, stride and access;
- scalar arguments;
- callable identity and artifact fingerprint;
- the existing compute context, PTO endpoint and private dispatch metadata.

It acquires each tensor through
`lingqu_shmem_mem_service_acquire_owned()`, begins every owner lease before
enqueue, waits for the original CQ completion, completes those same leases and
only then permits release. Timeout, mismatched completion, partial acquire,
enqueue retry and cleanup retry retain the required ownership state.

The public API has no hidden/KV/logits payload pointer. Successful completion
emits these mandatory provenance fields:

```text
model_tensor_address_space=UB_GM
pto_instruction_io=1
shared_payload_staging_bytes=0
```

### 3.2 Qwen direct tensor adapter

`qwen3_managed_direct_guest.h/.c` converts one Qwen range geometry into the
existing callable 3 contract:

1. hidden input;
2. previous packed KV;
3. token IDs;
4. hidden output;
5. next packed KV;
6. terminal logits;
7. twelve scalar geometry arguments.

Every tensor is backed by a caller-owned Memory Service mapping owner. The
adapter does not open another Memory Service context and does not allocate a
parallel local arena. It supports first, middle and terminal ranges.

### 3.3 Current validation

The source passes AArch64 cross-compiler `-Wall -Wextra -Werror` syntax checks.
Native Linux golden tests ran on `n4-910c1`:

```text
model-direct: op_id=101 request_id=202 callable=41 tensors=3 \
model_tensor_address_space=UB_GM pto_instruction_io=1 \
shared_payload_staging_bytes=0 completion_status=1
model_direct_guest=pass cases=5 staging_bytes=0
qwen3_managed_direct_guest=pass cases=4 callable=3 staging_bytes=0
```

The generic test callable ID is fixture data and does not reserve a production
DeepSeek callable ID. The test matrix covers partial acquire, begin retry,
enqueue retry, poll retry, completion failure, release retry and exact owner
retention. The Qwen matrix covers first, middle and terminal range geometry.

## 4. Current Implementation and Remaining Work

### 4.1 Qwen managed serving integration

`qwen3_managed_serving_guest.c` passes the existing hidden/KV reader owners
and writer owner to `qwen3_managed_direct_prepare()`. The writer has disjoint
token, hidden output, next KV and logits views. Completion waits for the
original operation, then stages and seals references to those views in place.
The terminal token record is small control data written into its reserved
writer view. The published hidden, KV and logits remain in the writer backing.

Qwen direct passed 2/3/8-node, 4/8-step cluster-runner cases on `n4-910c`.
The committed default is exercised through sim-console on `n4-910c` by:

| Nodes | Steps | Run ID | Host access evidence |
| ---: | ---: | --- | --- |
| 2 | 1 | `sim-console-1790161824664-0` | 2/2 ranges |
| 2 | 4 | `sim-console-1790162601003-0` | 8/8 ranges |
| 2 | 8 | `sim-console-1790220092981-0` | 16/16 ranges |
| 3 | 4 | `sim-console-1790162823449-0` | 12/12 ranges |
| 3 | 8 | `sim-console-1790219814717-0` | 24/24 ranges |
| 8 | 4 | `sim-console-1790220274030-0` | 32/32 ranges |
| 8 | 8 | `sim-console-1790163015394-0` | 64/64 ranges |

Each counted range used callable 3, had nonzero provider reads and writes,
reported zero shared payload staging, and completed successfully. The 8-step
run produced token IDs `[264, 3644, 7653, 304, 279, 2070, 315, 1995]`.
The new 2-node/8-step, 3-node/8-step and 8-node/4-step runs used revision
`a9b11b6a` and passed with 16, 24 and 32 zero-staging host access records,
respectively. Every guest cleaned up with zero live or quarantined objects,
and no QEMU process remained on the target after each run. This completes
the Qwen sim-console 2/3/8-node by 4/8-step configuration coverage on
`n4-910c` across the recorded source revisions.

On `n4-910c1`, the sim-console matrix also passed:

| Nodes | Steps | Run ID | Host access evidence |
| ---: | ---: | --- | --- |
| 2 | 1 | `sim-console-1790163569079-0` | 2/2 ranges |
| 2 | 4 | `sim-console-1790218873505-0` | 8/8 ranges |
| 2 | 8 | `sim-console-1790220589352-0` | 16/16 ranges |
| 3 | 4 | `sim-console-1790163821727-0` | 12/12 ranges |
| 3 | 8 | `sim-console-1790220794399-0` | 24/24 ranges |
| 8 | 4 | `sim-console-1790221022099-0` | 32/32 ranges |
| 8 | 8 | `sim-console-1790163981682-0` | 64/64 ranges |

The new 2-node/8-step, 3-node/8-step and 8-node/4-step runs used revision
`a9b11b6a`. Their terminal token sequences matched the corresponding
`n4-910c` runs. All counted host access records used callable 3, had nonzero
reads/writes and zero shared staging. Every guest reported zero live and
quarantined objects after cleanup. The target retained 28 unrelated QEMU
processes that predated these runs by over three days; none of the new runs
left another QEMU process. Qwen's two-host sim-console functional matrix is
covered across source revisions. A same-revision rerun of all 12 cases and
independent logits-reference acceptance remain open.

### 4.2 DeepSeek complete direct callable

DeepSeek has a model-specific tensor/scalar ABI, callable 5 range
orchestrator and PTO kernels for attention, MLA/indexer, MoE, shared expert,
KV and output head. Shared hidden/KV/logits use managed `UB_GM` views; weights
remain private immutable model data. The cluster-runner 2/3/8-node, 4/8-step
matrix passed on `n4-910c`. A later 2-node/1-step gate logged callable 5
provider reads/writes of `308693/4446` and `339479/133858`, with a terminal
token and no reported staging.

DeepSeek still needs the sim-console openEuler gate, the second-host matrix,
and explicit range/logits oracle comparison evidence before full acceptance.
No callable 4 execution counts as DeepSeek W6 evidence.

The first sim-console openEuler attempt, `sim-console-1790164420881-0`,
failed before QEMU launch because its managed checkout reused a stale
`sim-cli` that did not recognize callable 5. The stable config entry now asks
Cargo to incrementally verify the default `sim-cli` before fingerprinting.
An isolated remote reproduction copied that old binary into the validation
tree and then passed DeepSeek `--print-env` with `SIM_W5_MODEL_IO=direct` and
a nonzero callable artifact fingerprint.

The next sim-console attempt, `sim-console-1790165525235-0`, reached both
openEuler guests. Node A completed callable 5; node B completed host PTO I/O
(`reads=26`, `writes=132487`, `shared_payload_staging_bytes=0`) but failed
while preparing the terminal token with `rc=-2` (`ENOENT`). The token table
was generated in the per-run initramfs and copied into the root overlay at
`/tmp/deepseek_v4_flash_pto_token_table.bin`. The guest boot log shows that
openEuler mounted a fresh `/tmp` over that overlay path before `run_app`.
The guest runner now stages direct token tables under `/opt/ub_sim` for
openEuler and points `run_app` there; busybox guests keep `/tmp`. An isolated
validation checkout on `n4-910c` passed the 2-node/1-step direct run
`2026-09-23_20-32-34_w5_deepseek_v4_flash_decode_13449` with this patch.
Node A logged callable 5 `reads=23/writes=3164`; node B logged callable 5
`reads=26/writes=132487`. Both reported
`shared_payload_staging_bytes=0 status=pass`. Node B published terminal
token 65 from `managed_callable5_logits`, and the cluster reported 2/2
passed nodes and one decoded token. This proves the table-path correction in
that isolated checkout.

The committed correction, revision `36f664fd1f7d22e55403348a501ed045319bc393`,
also passed the DeepSeek sim-console openEuler 2-node/1-step gate on both
registered targets:

| Target | Sim-console run | Result | Terminal token/logits checksum |
| --- | --- | --- | --- |
| `n4-910c` | `sim-console-1790170966071-0` | 2/2 nodes passed | `65` / `0xee06ac1d0ff28301` |
| `n4-910c1` | `sim-console-1790171546173-0` | 2/2 nodes passed | `65` / `0xee06ac1d0ff28301` |

Both runs logged callable 5 host access on node A (`reads=23`,
`writes=3164`) and node B (`reads=26`, `writes=132487`) with
`shared_payload_staging_bytes=0`. Each node A produced hidden/KV checksums
`0x3b97679d3d3ccf4a`/`0x3a36fce46f940764`; each node B produced
`0x181e729cfff848cb`/`0x383faf8e18adb5bc`. Node B published the terminal
result from `managed_callable5_logits`. These runs close the specific
openEuler token-table gate and show repeatable direct output on two hosts;
they cover only one step and do not establish numerical oracle parity.

An additional isolated 2-node/1-step comparison used the same GGUF and
prompt tokens. Direct callable 5 and explicit bulk callable 4 both passed
and selected token 65. Their terminal logits checksums differed:
`0xee06ac1d0ff28301` for direct and `0xd9ae438eff27b75a` for bulk.
With range output checksums enabled, node A's `[0,22)` hidden/KV checksums
were `0x3b97679d3d3ccf4a`/`0x3a36fce46f940764` in direct run
`2026-09-23_21-09-37_w5_deepseek_v4_flash_decode_29162`, versus
`0x0c16f40f1b014a08`/`0x89574f23358e08e8` in bulk run
`2026-09-23_21-17-46_w5_deepseek_v4_flash_decode_13501`. The bytewise
difference begins by the first range output; token agreement alone does not
establish numerical parity. A full-tensor comparison against the model oracle
with an explicit tolerance is still required before acceptance.

A focused layer-0 comparison on `n4-910c` used token 42 at position 0 and
the same GGUF for direct PTO and bulk execution. The encoded KV segment
(512 `f32` values beginning at byte 48) matched bitwise, 512/512. The full
16,384-value hidden output differed: maximum absolute error `0.0040334910`,
mean absolute error `0.0002459195`, relative L2 error `0.0042583656`, and
cosine similarity `0.9999909384`. At diagnostic tolerances `atol=1e-4` and
`rtol=1e-3`, 3,635 values were outside tolerance. These thresholds are not
model acceptance criteria. The matching KV segment narrows the investigation;
the stage comparison below localizes the first measurable difference.

Stage isolation on the same host, model, token and position confirms that
the bulk stage runner reproduces its previous 16,384-value final hidden
output bitwise. The direct probe also checks that replaying layer 0 as
embedding, attention and FFN reproduces its original range output bitwise.
The two attention outputs already differ before the FFN:

| Stage | Values | Max absolute error | Relative L2 error | Outside diagnostic tolerance |
| --- | ---: | ---: | ---: | ---: |
| Raw attention, before HC post | 4,096 | `0.0210111141` | `0.0021458036` | 2,917 |
| Attention HC, after HC post | 16,384 | `0.0006673019` | `0.0016561342` | 1,401 |

Both rows use diagnostic `atol=1e-4`, `rtol=1e-3`; these are not acceptance
limits. A same-GGUF scalar calculation using `sim-models` F16/Q8_0 decoders
and projections, with the validated layer-0 KV row supplied as input, further
localized this case:

| Comparison | Result |
| --- | --- |
| Query after RoPE, scalar vs PTO | 32,768/32,768 bitwise equal |
| Attention heads after inverse RoPE, scalar vs PTO | max absolute error `4.7683716e-7`; 20,957/32,768 bitwise equal |
| Grouped `output_a`, independently computed heads | max absolute error `0.0036337376` |
| Grouped `output_a`, identical PTO heads supplied to both paths | 8,192/8,192 bitwise equal |
| Raw attention output, identical PTO heads supplied to both paths | 4,096/4,096 bitwise equal |

The two head tensors produce 156 different Q8 activation codes out of 32,768
and 360 different scale bit patterns out of 1,024 blocks. The tiny
sink-attention floating-point difference is amplified at the Q8 activation
boundary. This case gives no evidence of a grouped Q8 or output-B kernel
mismatch when those kernels receive identical inputs. The scalar calculation
reuses a KV row produced by the direct run; it is a stage differential, not
a full independent model oracle.

A follow-up PTO change uses the DS4 normalization order for sink attention:
calculate one FP32 reciprocal of the softmax denominator, then multiply each
output value by it. On `n4-910c`, the same layer-0/token-42 GGUF probe increased
bitwise-equal heads from 20,957 to 26,863 of 32,768. Changed Q8 activation
codes fell from 156 to 98; changed scale bit patterns fell from 360 to 188.
Maximum raw-attention error against the scalar reference fell from `0.0210111`
to `0.0111675`, and maximum final-hidden error against the bulk layer stage
fell from `0.00403349` to `0.00335476`. The 2,096-byte layer-0 KV state stayed
byte-identical to the previous direct probe. The AArch64 arithmetic fixture
passed. This is a measured improvement on one layer and token; full-model
numerical parity remains open.

A full 43-layer, position-0, token-42 same-GGUF differential on `n4-910c`
compared the direct PTO probe with the existing Simpler bulk range runner. The
probe now writes all terminal logits via `--logits-out`. Both paths selected
token `65`, but the full tensors differed:

| Output | Values/bytes | Relative L2 error | Max absolute error | Outside diagnostic tolerance |
| --- | ---: | ---: | ---: | ---: |
| Hidden | 16,384 f32 | `0.00567159` | `6.55566` | 14,946 |
| Logits | 129,280 f32 | `0.00837669` | `0.211663` | 118,524 |
| Encoded KV state | 12,297,344 bytes | not applicable | first mismatch at byte 2,147 | not applicable |

The diagnostic tolerance was `atol=1e-4`, `rtol=1e-3`; it is not an acceptance
limit. The direct top logit was approximately `17.3029`, versus `17.3396` for
bulk. The KV files shared the first 2,146 bytes, including the layer-0 state;
the first mismatch occurs in the next layer's state. Remote outputs are under
`/home/ll/sim-console/ub_sim/out/w6-range-parity/` as
`{hidden,state,logits}-full-{direct,bulk}.*`. This establishes a complete
same-GGUF numerical mismatch despite top-token agreement. The existing bulk
runner is a differential baseline, not an independent DS4 or official-checkpoint
oracle. The direct probe's logits-output CLI change has passed focused tests;
the multi-step guest and oracle gates remain open.

An independent DS4 layer-slice oracle was attempted against clean DS4 source
revision `931b98e` on `n4-910c`. The adapter's engine-options layout and
layer-slice call signature had drifted from that header; both are corrected
and the engine now opens. DS4's CPU backend rejects layer-slice evaluation
with `layer slices require the graph backend`, so this CPU-only test cannot
produce a DS4 slice oracle. The unsupported CPU slice CLI option was removed.
The independent same-GGUF oracle remains open and requires a supported DS4
graph backend or a separate full-session reference path.

The current run uses an IQ2 GGUF. The official checkpoint plan explicitly
prohibits treating DS4 GGUF logits as the FP8/FP4 official-checkpoint golden.
W6 must keep two numerical claims separate: same-GGUF direct-path parity for
the deployed quantized model, and official-checkpoint parity against the
independent Safetensors CPU reference. Neither full-model claim is proven by
this one-layer stage comparison.

### 4.3 Serving selection and observability

The stable W5 entry selects `direct` for managed Qwen3-0.6B and DeepSeek V4
Flash. `--model-io bulk` keeps callable 4 explicitly available for comparison;
other profiles retain their prior default. The host wraps PTO provider
callbacks and counts successful reads/writes by request ID. A model range
fails closed on zero reads, zero writes or staged shared payload.

Sim-console mirrors short QEMU log tails during a run. At completion it also
extracts every model-direct host access line from the complete remote QEMU
logs into per-node summaries and the Process log. This retains all per-request
counters without repeatedly transferring large QEMU logs.

### 4.4 Remaining validation matrix

The acceptance matrix remains 2/3/8 nodes times 4/8 steps for each model,
through sim-console on both `n4-910c` and `n4-910c1`. The cluster-runner
matrix on one host is useful evidence and does not replace that requirement.
The `n4-910c` DeepSeek 2-node/4-step sim-console run
`sim-console-1790214179061-0` failed at node B, step 2 after two terminal
tokens. Callable 5 completed that range with direct UB_GM reads and writes;
the OBMM worker then stopped on a TCP connection reset during
`provider-refresh`, quarantining the new allocation before terminal
publication. The bounded reconnect change in `mem_service` has passed focused
provider tests and compilation, but has not yet been pinned or verified by a
multi-step guest rerun. The cause of the connection reset remains unproven.
For every case, compare terminal tokens and logits with the corresponding
same-format model reference, match guest request IDs to host access counts,
and confirm zero publication copy bytes. Record the exact run ID and source
revision. The deployed GGUF path still needs a complete same-GGUF reference;
the official Safetensors reference covers a different weight format.

## 5. Acceptance Criteria

W6 model direct is complete only when all conditions hold:

1. Qwen and DeepSeek complete model ranges execute with managed `UB_GM`
   hidden, KV and logits tensors.
2. The range callable executes PTO `TLOAD/TSTORE`; matching host counters are
   greater than zero for every request.
3. `shared_payload_staging_bytes=0` is measured and enforced for every range.
4. No shared tensor enters Rust `Vec<u8>`, host tensor materialization, inline
   UAPI payload or a second serving allocation.
5. Mapping owner, object version, access permission and lease checks fail
   closed for stale, out-of-range and cross-request inputs.
6. The same operation remains pinned through its original CQ completion,
   including timeout and retry cases.
7. Hidden and KV handoff publication is in place and reports zero publication
   copy bytes.
8. Terminal logits and selected tokens match a reference for the same model
   asset and weight format. GGUF differential results cannot establish
   official FP8/FP4 checkpoint parity.
9. The 2/3/8-node, 4/8-step matrix passes on both test-farm hosts.
10. W6 becomes the default only after Qwen and DeepSeek both satisfy items
    1 through 9. W5 callable 4 remains explicitly selectable for comparison.

## 6. Immediate Next Change

Extend the same-GGUF scalar comparison beyond the first dense layer and
position-0 token, with a justified tolerance for Q8 activation-boundary
effects. Separately compare the official-checkpoint path with the existing
independent Safetensors CPU reference. Complete the 2/3/8-node, 4/8-step
sim-console matrix on both hosts. Keep W6 acceptance open until every
condition in Section 5 has direct evidence.
