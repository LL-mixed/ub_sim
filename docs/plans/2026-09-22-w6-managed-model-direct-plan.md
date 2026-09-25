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

An independent same-GGUF full-session oracle now runs from clean DS4 source
revision `931b98e` on `n4-910c`. For raw token ID `42` at position zero,
DS4 CPU `session_sync` and `session_eval` produced byte-identical full
129,280-value logits. Both selected token `201` with top logit `19.019411`.
The DS4 CPU layer-slice API still requires a graph backend, so a separate
CPU-only diagnostic compilation unit records DS4's per-layer hidden. Its
43-layer final hidden reproduces the public DS4 session logits bytewise.
DS4's output head applied to the old PTO final hidden agreed with PTO's
own logits to relative L2 `2.44e-7`, isolating the major mismatch upstream.

The first large per-layer error appeared at layer 4, the first learned
top-k MoE layer. DS4 reads `blk.3.exp_probs_b.bias`, while the PTO range
and Rust GGUF paths had looked for `blk.3.ffn_exp_probs_b.weight` and
silently omitted the selection bias. Correcting the tensor name made the
PTO bias dump byte-identical to DS4's. With the same input hidden, both
selected experts `[247, 27, 67, 69, 143, 118]`; expert weights differed
by at most about `1e-6`. The layer-4 hidden relative L2 error fell from
`0.033144` to `0.004511`.

The corrected PTO 43-layer trace completed on `n4-910c` with the same GGUF
and raw token. Final hidden relative L2 error versus DS4 fell from
`0.985387` to `0.001489`, with cosine similarity `0.999999560`. Applying
the DS4 output head to that corrected hidden selected token `201` with
top logit `19.001715`. Full logits versus the independent DS4 session had
relative L2 error `0.003281`, cosine similarity `0.999994619`, and maximum
absolute error `0.069979`. At diagnostic `atol=1e-4`, `rtol=1e-3`, 105,450
of 129,280 logits remained outside tolerance. These are diagnostic
thresholds, not final acceptance criteria. This proves the routing fix and
same top token for one raw-token case. A separate four-layer
`decoder_range()` probe passed and produced a byte-identical hidden tensor
to the corrected per-layer PTO trace. Exact numerical parity remains open.

A controlled production guest check then staged only the corrected PTO
header into the otherwise clean `n4-910c` test checkout at `a9b11b6a`.
The default-direct openEuler 2-node/1-step run
`2026-09-24_13-44-30_w5_deepseek_v4_flash_decode_2664` passed 2/2 nodes
and generated token `201` for the configured raw prompt token `42`.
Node A recorded callable 5 `reads=23/writes=3164`; node B recorded
callable 5 `reads=26/writes=132487`. Both host records reported
`shared_payload_staging_bytes=0 status=pass`. Node B published the token
from `managed_callable5_logits`; its top logit was `19.001728`, close to
the corrected standalone PTO probe's `19.001715`. The independent DS4
CPU session selected the same token with top logit `19.019411`. Both
guests cleaned up with zero live and quarantined objects, and no QEMU
process remained. The temporary header was restored, leaving the remote
tracked checkout clean. This proves the fix reaches a real callable-5
guest path for one token; it preceded the committed-source gate.

Revision `a4d7c520` then passed the official sim-console DeepSeek
2-node/1-step gate on both registered targets, using the same GGUF and
configured raw input token `42`:

| Target | Sim-console run | Result | Token/logits checksum |
| --- | --- | --- | --- |
| `n4-910c` | `sim-console-1790229922646-0` | 2/2 nodes passed | `201` / `0xf70760778103bfe3` |
| `n4-910c1` | `sim-console-1790230564818-0` | 2/2 nodes passed | `201` / `0xf70760778103bfe3` |

On each host, node A logged callable 5 `reads=23/writes=3164` and node B
logged callable 5 `reads=26/writes=132487`; all four records reported
`shared_payload_staging_bytes=0 status=pass`. Node B published token `201`
from `managed_callable5_logits`. Both guests on each host reported zero
live and quarantined objects at cleanup. The two remote Git checkouts
remained clean. No QEMU process from either new run remained; `n4-910c1`
still has unrelated, older QEMU processes. This closes the committed-source
two-host 1-step gate.

The same committed revision also passed a `n4-910c` 2-node/4-step run,
`sim-console-1790231281752-0`: the process exited 0, the cluster reported
4/4 steps and 2/2 guest passes, and the tokens were
`[201, 361, 1823, 11006]`. All eight callable-5 host-access records had
nonzero reads and writes with zero shared payload staging; both guests
reported zero live and quarantined objects at cleanup. The archived
sim-console record incorrectly marks node A failed: an unrelated openEuler
`[FAILED] Failed to start dnf makecache` line matched its broad `] fail`
log classifier even though node A ended with `[w4_guest] pass`. The
classifier now distinguishes `fail` from `failed`; the archived record
retains the original misclassification. This is one functional 4-step
result, not multi-position DS4 numerical parity or the full 2/3/8-node
matrix.

Revision `44226cb8` passed the `n4-910c` 3-node/4-step sim-console gate,
`sim-console-1790233366506-0`. The process exited 0, its summary reported
4/4 steps and 3/3 nodes passed, and the token sequence was again
`[201, 361, 1823, 11006]`. Its four full-vocabulary logits checksums,
`[0xf70760778103bfe3, 0x656efc28b2db5f75, 0x176ff7bc2eaad88c,
0xb552aa498178d08f]`, match the 2-node/4-step run at every position.
The original target QEMU logs contain four
callable-5 host-access records per node, all with nonzero reads/writes,
`shared_payload_staging_bytes=0` and `status=pass`. The original guest
logs end with `w5_managed_request_cleanup status=ok` on all three nodes:
zero live objects, mappings, in-flight references and quarantined objects.
No QEMU process remained on `n4-910c`. Sim-console's local node-log mirror
for this run omitted the final step's tail and model-access extracts, so
the original target logs were checked directly. The mirror limitation
remains open; it does not change the process and original-guest results.
This closes one 3-node/4-step functional gate, not the 3-node/8-step,
8-node or independent numerical-oracle gates.

Revision `74929857` completed the `n4-910c` 8-node/4-step model run
`sim-console-1790236011324-0` on the target. The target-side summary
reported 4/4 steps, 32 worker timing records and 8/8 guests passed.
It produced `[201, 361, 1823, 11006]`, with the same four logits
checksums as the 2-node and 3-node runs above. Each of the eight original
QEMU logs contains four callable-5 access records with nonzero reads and
writes, zero shared payload staging and `status=pass`. All eight guest
cleanup records report zero live objects, mappings, in-flight references
and quarantined objects. No QEMU process remained afterward.

The local sim-console record for this run is **failed**: its long-lived
SSH launcher disconnected with status 255 during step 1. The remote
runner remained alive and completed all four steps after that disconnect.
The local final log sync also failed during a temporary target hostname
resolution outage, and recorded both the full-sync and lightweight-sync
errors. Thus the remote DeepSeek 8-node/4-step **model-function gate**
passed, while the sim-console end-to-end **session gate** did not. The
control plane needs to recover a detached target run's final status
after SSH reconnection; the local failed record must not be reported as a
sim-console pass. This run does not establish independent DS4 numerical
oracle parity.

Revision `27cb20ba` now writes an atomic target-side per-run exit status and
reconnects to read it after SSH status 255. A committed-source
`memory-service-smoke` run, `sim-console-1790238569482-0`, passed 2/2 nodes
on `n4-910c`; its target-side exit-status file contains `0`. This verifies
status-file publication in a real remote run. An actual disconnect followed
by successful recovery has not yet been exercised end to end.

A same-GGUF DS4 CPU session independently evaluated raw tokens `[42, 201]`
and selected token `41` at position 1. Its logits for tokens `41` and `361`
were `18.572752` and `18.229273`. The committed W6 2/3/8-node runs all
selected `361` at that position; the 2-node guest recorded `18.256815` for
`41` and `18.282267` for `361`. A separate two-token PTO range probe,
using the same GGUF weight index and carrying its encoded KV state from the
first token into the second, reproduced the guest's token `361` and logits
exactly. Its complete 129,280-value logits versus DS4 had relative L2
error `0.036051`, maximum absolute error `0.962967`, and 127,019 values
outside the diagnostic `atol=1e-4, rtol=1e-3` bounds. The probe's second
state was 12,385,408 bytes, matching the production state geometry.
This localizes the token-selection mismatch to direct PTO model arithmetic
or its retained KV state, before guest handoff or cluster scheduling. The
diagnostic bounds are not acceptance criteria. DeepSeek numerical parity is
contradicted at position 1, so additional functional steps cannot close
the W6 model acceptance gate.

A DS4 CPU layer trace for the same `[42, 201]` sequence was rebuilt with
the reference project's own compiler flags. Its complete logits are
byte-for-byte identical to the independent DS4 CPU oracle, so the trace is
a valid comparison point. Against that trace, the `-O2` PTO probe's
position-1 hidden state has relative L2 error `0.004624` after layer 1,
`0.007949` after layer 2, and `0.012019` after layer 4. Layer-1 raw KV for
both positions is bitwise identical (`512/512` values per position); the
layer-2 raw KV errors are `0.011833` and `0.015572`, respectively. The
first material difference therefore occurs in layer-1 hidden arithmetic,
before layer-2 KV is written.

A diagnostic probe compiled with `-O3 -ffast-math -fno-finite-math-only`,
matching the DS4 CPU arithmetic flags more closely,
reduced the position-1 layer-1 hidden error to `1.018e-6`. Its full
two-token range selected DS4's token `41`, but complete-logits relative L2
error remained `0.015672` and maximum absolute error `0.367785`.
Position-0 layer-1 hidden error remained about `0.004461`, and position-0
layer-2 raw KV error was `0.013264`; position-1 layer-2 raw KV became
bitwise identical. This diagnostic measurement preceded the production
artifact change and did not establish numerical parity.

An independent DS4 position-0 layer-1 stage trace then localized the
amplification: the fast-math PTO query and attention heads had relative L2
errors `2.56e-7` and `8.48e-8`, respectively. The grouped Q8 output-A error
jumped to `9.29e-4`, the attention output to `2.17e-3`, and the HC state
after attention to `1.67e-3`. Replaying the exact DS4 heads through the PTO
grouped Q8 operator reduced output-A error to `1.43e-7`. The grouped
operator agrees closely for identical input; the preceding tiny heads
differences cross Q8 activation-rounding boundaries. The remaining work is
to trace and align the upstream query/attention arithmetic, then recheck
full-vocabulary logits rather than relax the numerical gate.

Revision `774ec91a` applies `-O3 -ffast-math -fno-finite-math-only` only to
the simulated DeepSeek range kernel. An isolated artifact build on
`n4-910c` succeeded and produced a kernel with a different SHA-256 from
the previous `-O2` artifact. The committed-source sim-console run
`sim-console-1790249800360-0` then passed on `n4-910c` with 2 nodes and
2 steps. Its streamed terminal tokens were `[201, 41]`, matching the
same-GGUF DS4 CPU reference at both positions; the old guest path selected
`361` at position 1. The four callable-5 ranges reported positive direct
reads/writes and `shared_payload_staging_bytes=0`, for example:

```text
pto-ub-gm-model-access: request=11756923580248608108 callable=5 reads=23 writes=3164 shared_payload_staging_bytes=0 status=pass
pto-ub-gm-model-access: request=11756923584543575407 callable=5 reads=3186 writes=132508 shared_payload_staging_bytes=0 status=pass
```

This run proves the two-token guest path selects the expected tokens with
the new artifact. The earlier `0.015672` relative L2 measurement came from
a locally cross-compiled, statically linked diagnostic probe. That probe did
not reproduce the deployed kernel's later decode positions and must not be
used as the production numerical result.

A production-equivalent diagnostic probe was compiled on `n4-910c` with
GCC 15.3.0 and the simulated kernel's C++23, CPU-sim and DeepSeek arithmetic
flags. It replayed tokens `[42, 201, 41, 201]` against the same GGUF weight
index. Its complete 129,280-value logits checksum matched the 2-node W6
guest at each step: `0xf84d1dee3e293398`, `0xc0eb22f0d2a2534c`,
`0xde41ab355e5d42ce`, and `0x96cab884bc370e72`. Splitting its final
43-layer state at the guest's `[0,22)` / `[22,43)` boundary and applying
the Memory Service provider checksum reproduced both guest KV checksums,
`0x5e3294b72ed7451b` and `0x92b241f2c338cfdb`. Its final hidden checksum
also matched the guest, `0xed280939670b0b94`. This validates the
diagnostic probe as a byte-for-byte reproduction of this deployed 4-step
run, including state handoff. The old GCC 15.2 static probe matched the
guest's complete logits through step 2, then selected token `18` at step 3;
the production-compiled probe and guest selected `35`.

The production-equivalent probe still differs materially from the
independent DS4 CPU oracle on the same IQ2 GGUF:

| Step | DS4 / W6 top token | Logits relative L2 | Max absolute error | Values outside diagnostic `1e-4 + 1e-3 * abs(ref)` |
| --- | --- | ---: | ---: | ---: |
| 0 | 201 / 201 | 0.003836918 | 0.081278 | 108,972 / 129,280 |
| 1 | 41 / 41 | 0.033569570 | 0.727366 | 127,230 / 129,280 |
| 2 | 201 / 201 | 0.072947333 | 1.661165 | 128,404 / 129,280 |
| 3 | 35 / 35 | 0.054312597 | 1.316248 | 128,023 / 129,280 |

These bounds are diagnostic, not acceptance criteria. Matching greedy
tokens does not close the full-logits gate. The first-token upstream
query/attention and Q8 activation-boundary differences remain, and the
first ratio-4 compressed KV row is reached at step 3.

The ratio-4 compressor originally accumulated all previous-lane terms
before all current-lane terms. DS4 accumulates a previous/current pair
for each row. Commit `a8cde067` matches that order and adds a cancellation
fixture that distinguishes the two algorithms. A GCC 15.3 probe using the
deployed kernel flags replayed `[42, 201, 41, 201]` before and after the
change. The complete 129,280-value logits files were byte-identical at
each of steps 0 through 3 (matching SHA-256 for each pair). The initial
previous lane is empty; the first ratio-4 pool at step 3 is therefore
unaffected by this order change. The first possible full-model effect is
the second ratio-4 pool at step 7. This correction cannot explain the
existing first-token or four-step logits differences against DS4.

A first-layer common-input probe narrowed the earlier discrepancy. The
DS4 and PTO query tensors differed by relative L2 `2.45e-7`, and the
attention-head tensors by `8.84e-8`. Their separately computed grouped
Q8 output-A tensors differed by `9.26e-4`. Feeding the exact DS4 heads
into the PTO grouped projection reduced output-A error to `1.50e-7`.
Thus the projection agrees on common input; small differences upstream
cross activation-quantization boundaries. DS4 uses a double-precision
squared-sum in RMSNorm while the current PTO path uses float reduction.
The following audit verifies that as one first-layer rounding source;
it does not establish a sole cause of full-model logits drift.

A same-GGUF, token-42, first-layer probe on `n4-910c` subsequently captured
the HC-mixed tensor and its attention RMSNorm output from both programs.
The unmodified PTO path differed from DS4 by relative L2 `2.30e-8` at
HC mixing and `5.10e-7` after RMSNorm. Replaying DS4's double-accumulation
RMSNorm on the PTO HC-mixed tensor reduced the normalized-output error to
`2.62e-8`; 3,740 of 4,096 values then matched DS4 bitwise. The DS4
replay on DS4's own input matched all 4,096 values bitwise. This isolates
RMSNorm reduction as a measured first-layer error source.

An ignored, diagnostic-only PTO copy changed the RMSNorm reduction from
one 4,096-element `TROWSUM` to 32-element chunks, keeping PTO tile
operations throughout. The table compares both PTO probes against the same
DS4 first-layer output (relative L2 error):

| Tensor | Original PTO | 32-element diagnostic |
| --- | ---: | ---: |
| Attention RMSNorm output | `5.10e-7` | `5.60e-8` |
| Q projection | `2.28e-7` | `1.69e-7` |
| Grouped Q8 output-A | `6.80e-4` | `9.00e-4` |
| Layer-1 final hidden | `3.90e-3` | `4.24e-3` |

The local RMSNorm improvement did not improve grouped Q8 output-A or the
final layer output. It is therefore not a validated production fix.
The ratio-4 compressor cannot produce its first pooled row before position
3, and the ratio-128 compressor cannot produce one before position 127.
The measured step-1 logits drift predates both. DS4 and PTO also use the
same previous/current pair accumulation order in the ratio-4 pool loop;
the output may still differ because its input values and transcendental
arithmetic differ. Next numerical work should locate the first Q8 activation
block whose quantized code changes under otherwise common layer input,
then trace the contributing pre-quantization values. Do not infer a full
model fix from a reduction-order change alone.

These tests use an IQ2 GGUF. The official checkpoint plan explicitly
prohibits treating DS4 GGUF logits as the FP8/FP4 official-checkpoint golden.
W6 must keep two numerical claims separate: same-GGUF direct-path parity for
the deployed quantized model, and official-checkpoint parity against the
independent Safetensors CPU reference. Neither full-model claim is proven by
the two-token functional run.

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
multi-step guest rerun. A newer committed-source run on the same pinned
`mem_service` revision, `sim-console-1790250799430-0` at root revision
`774ec91a`, passed 2 nodes and 4 steps on `n4-910c`. Its streamed tokens
`[201, 41, 201, 35]` matched an independent four-step DS4 CPU greedy
reference on the same IQ2 GGUF. The DS4 source copy used for this oracle
has the same `ds4.c` SHA-256 as local DS4 revision `931b98e5`; its four
129,280-value logits arrays are stored as
`out/w6-range-parity/ds4-greedy-42-4.step*.logits.f32` on `n4-910c`.
The reference's winning logits were `19.0194111`, `18.572752`,
`24.3486137`, and `21.1338158`, respectively. All eight callable-5 ranges
reported positive reads/writes with `shared_payload_staging_bytes=0`, and
both guest nodes passed. That single earlier run did not prove that the
connection reset was fixed or that the unpinned reconnect commit was ready.
The cause of the connection reset remains unproven.
An independent same-GGUF DS4 CPU eight-step greedy reference is also stored
on `n4-910c` as `out/w6-range-parity/ds4-greedy-42-8.step*.logits.f32`.
Its token sequence is `[201, 41, 201, 35, 201, 35, 201, 35]`.
An independent PTO range probe using the same GGUF completed eight greedy
steps with the same token sequence. Its logits are stored as
`out/w6-range-parity/pto-neworder-42-8.step*.logits.f32` on `n4-910c`.
Against the DS4 reference, relative L2 logits error was 0.003837 at step 0,
0.033570 at step 1, 0.095986 at step 6, and 0.107647 at step 7; the
respective maximum absolute errors were 0.081278, 0.727366, 2.745873,
and 2.188519. The ratio-4 compressor's second row first participates at
step 7. The corrected sum order is a real local parity fix, but it does not
explain the earlier numerical drift or the managed-reference TCP failures.
Matching selected tokens does not establish full-logits parity. This probe
exercises the standalone PTO range, not the multi-node guest path.
Two 2-node, eight-step sim-console attempts stopped before step 7. Baseline
`774ec91a` on `n4-910c` (`sim-console-1790256637013-0`) produced the first
three DS4 tokens, then nodeB failed to open the previous-token managed
reference at step 3 after repeated `mem_service client: tcp connect timeout`.
Revision `a8cde067` on `n4-910c1` (`sim-console-1790257453045-0`) produced
the first five DS4 tokens, then nodeB failed to close that reference at
step 5 with `rc=-2`, again after TCP connect timeouts. Both failures are
in managed-reference access before the affected model step. They provide
no evidence about the numerical effect of the step-7 compressor change.
The QEMU bridge now enables a bounded asynchronous PTO `UB_GM` dispatch
worker. CQ polling returns while model computation runs, and device reset
joins the worker before releasing UB_GM mappings. This allows QEMU to keep
servicing guest memory and network activity during a long model range.
Focused `sim-uapi` tests cover nonblocking polling and ordered completion;
the `sim-qemu` FFI test and QEMU C object compile also pass.
On `n4-910c`, committed source `10cd07c9` with QEMU pin `f8e44ee7`
passed both 2-node/4-step run `sim-console-1790264001765-0` and
2-node/8-step run `sim-console-1790265385881-0`. Their token sequences
were `[201, 41, 201, 35]` and
`[201, 41, 201, 35, 201, 35, 201, 35]`, respectively. The eight-step
summary reports 8/8 steps, 2/2 passed nodes and 16 worker timing records.
All 16 callable-5 host access records report positive reads and writes,
`shared_payload_staging_bytes=0`, and `status=pass`; mirrored guest logs
contain no TCP connect timeout. No QEMU processes remained after either run.
This is evidence that the earlier step-3 and step-5 liveness failures do not
reproduce with the asynchronous bridge on this host. It does not establish
full-logits parity, other topology gates, or repeat-run stability.
The same committed source `10cd07c9` passed the DeepSeek 3-node/4-step
sim-console gate on `n4-910c`: run `sim-console-1790270198285-0` exited 0,
all three guest nodes passed, and the layer ranges were `[0,15)`,
`[15,29)`, and `[29,43)`. The streamed token IDs
`[201, 41, 201, 35]` matched the independent four-step DS4 CPU greedy
reference. All 12 callable-5 model-direct access records reported positive
reads and writes, `shared_payload_staging_bytes=0`, and `status=pass`.
The remote process list had no remaining `qemu-system-aarch64` after the
run. This proves one three-node, four-step managed direct functional gate;
its evidence alone does not prove full-logits parity or eight-step behavior.

The same isolated validation checkout at `8be893ca` passed the DeepSeek
3-node/8-step sim-console run `sim-console-1790294940097-0` on `n4-910c`.
It exited 0 with all three guest nodes at 8/8 ranges. The terminal IDs
`[201, 41, 201, 35, 201, 35, 201, 35]` match the independent same-GGUF
eight-step DS4 greedy reference and include the second ratio-4 pool at
step 7. All 24 callable-5 host access records report positive reads and
writes, `shared_payload_staging_bytes=0`, and `status=pass`. The remote
process list had no remaining QEMU process after the run. This closes the
three-node eight-step functional gate on one host; a matching token at
step 7 does not prove that its full logits or compressed state match DS4.

The first committed-source 8-node/4-step sim-console run on `n4-910c`,
`sim-console-1790271676343-0` at `10cd07c9`, failed at step 0. NodeB
rang the PTO doorbell, but QEMU read `opcode=0, op_id=0` from its command
queue and rejected the slot. NodeA completed its range; downstream nodes
never received the required hidden output. This is a command-queue
visibility/lifetime failure, independent of DS4/PTO numerical parity.
Commits `2505c1a0`, `a87de85c`, and `43bdda4f` lock both queue pages,
verify their physical addresses and MMIO bases before submission, and
read the MMIO high-word mirror correctly for addresses above 4 GiB. The
endpoint test suite passed all five tests, including a stale-base guard.
The original empty-slot mechanism was not isolated to one kernel event;
the new checks fail closed if queue addresses change.

The isolated validation checkout at `8be893ca` (the same endpoint source
as those three `master` commits) passed the 8-node/4-step sim-console run
`sim-console-1790293399754-0` on `n4-910c` with exit code 0. All eight
nodes completed four ranges, the terminal IDs were `[201, 41, 201, 35]`,
and all 32 callable-5 host access records had positive reads and writes,
`shared_payload_staging_bytes=0`, and `status=pass`. QEMU read nodeB's
first command as `opcode=10` with the same op ID that the guest logged.
No QEMU process remained after the run. This closes the 8-node/4-step
functional gate on one host. It does not establish repeat-run stability,
the 8-node/8-step gate, second-host coverage, or full-logits parity.

The same isolated checkout passed the DeepSeek 8-node/8-step sim-console
run `sim-console-1790297485314-0` on `n4-910c` with exit code 0. All
eight nodes completed 8/8 ranges; the terminal IDs
`[201, 41, 201, 35, 201, 35, 201, 35]` match the independent same-GGUF
DS4 greedy reference. All 64 callable-5 host access records had positive
reads and writes, `shared_payload_staging_bytes=0`, and `status=pass`.
The post-run process check found no remaining QEMU process. This closes
the eight-node eight-step functional gate on one host, including token
selection at the second ratio-4 pool in step 7. It does not establish
compressed-state or full-logits parity, repeat-run stability, or
second-host coverage.

For every case, compare terminal tokens and logits with the corresponding
same-format model reference, match guest request IDs to host access counts,
and confirm zero publication copy bytes. Record the exact run ID and source
revision. The deployed GGUF path has four- and eight-step references, but
full-logits guest comparison remains open. The eight-step guest token
comparison passes. The official Safetensors reference covers a different
weight format.

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

Trace the first token's pre-projection rounding, including RMSNorm
accumulation, then rerun the common-input and full-logits comparisons with
the production compiler. Locate the step-1 state difference that expands
the standalone range's relative L2 logits error to 0.033570. Verify the
second ratio-4 compressed KV row at step 7 against DS4 state, beyond the
already-matching guest token sequence.
Exercise the new sim-console SSH recovery path with a real disconnect, and run
committed-source callable 5 through the remaining 2/3/8-node,
4/8-step sim-console matrix on both hosts. The two-host 2-node/1-step gate
and `n4-910c` 2-node/4-step, 2-node/8-step, 3-node/4-step,
3-node/8-step, and 8-node/4-step sim-console gates have passed. The next
functional gate is the full second-host matrix; the `n4-910c` 8-node/8-step
gate has also passed. Set a justified tolerance for Q8 activation-boundary
effects only after the first model divergence is understood. Separately
compare the official-checkpoint path with the independent Safetensors CPU
reference. Keep W6 acceptance open until every condition in Section 5 has
direct evidence.
