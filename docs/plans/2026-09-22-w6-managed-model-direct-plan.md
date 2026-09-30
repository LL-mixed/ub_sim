# W6 Managed UB_GM Model-Direct Plan

Date: 2026-09-22. Status reconciled: 2026-09-30.

Status: Qwen callable 3 and DeepSeek callable 5 execute complete model
ranges with managed `UB_GM` tensors. The stable W5 config entry selects
`direct` for the validated Qwen3-0.6B and DeepSeek V4 Flash profiles.
Both test-farm hosts passed the 2/3/8-node by 4/8-step functional matrix
for both models, 24 combinations in total, with the model source recorded
in the local checkpoint below. The subsequent PTO indexer change has its
own operator and real-weight boundary evidence in section 6.
The checkpoint guest regression passed 709/709 on 2026-09-29; the Rust
workspace suite also passed. The indexer continuation passed 710/710 guest
tests on 2026-09-30. Individual run records below identify logging-only
source differences. DeepSeek full-logits precision alignment is deferred
by user decision. Clean-checkout Qwen and DeepSeek 3-node/4-step
reproduction passed on 2026-09-30. Managed history beyond 512 compressed
rows remains open and must originate from real model execution. An unchanged
DS4 CPU reference has now generated all 2051 preceding positions and saved
the complete checkpoint; importing it and executing the managed continuation
are still unverified.

The 2026-09-30 checkpoint is local-only by user instruction. It pins
`mem_service` at `546985e26bce60798fa3f88329c3fbed8350c798` and
`kernel_ub` at `c2def7c931e6451d89727dec0c6f892abe28ba00`, matching the
validated checkouts. Before publishing a root commit that contains these
gitlinks, publish those commits to each submodule's `origin/ub_sim` and
verify fetchability. The offline-bundle reproduction below does not establish
that a GitHub-only clone can fetch the unpublished commits.

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

On 2026-09-26, the generic direct release path gained pre-enqueue lease
rollback. A failed lease begin or endpoint enqueue can now release already
begun leases without leaving the operation permanently busy; the golden
fixture covers both cases and a transient rollback failure. The updated
fixture reports `model_direct_guest=pass cases=7 staging_bytes=0`.
The current guest-source patch passed all 701 Python guest tests on
`n4-910c` in 316.057 seconds. Validation used a temporary patch on the
dedicated detached `8be893ca` checkout; the checkout was restored clean
afterward. This is a guest test-suite result, not a DeepSeek full-logits or
official-checkpoint oracle result.

On 2026-09-27, an isolated checkout with root HEAD `0577b38bd` and the
validation patch (SHA-256
`0dcca11d1e259506e7c2a26724afb60460b4c12692b76694b6dafd6b4fc07b61`)
passed all 703 guest Python tests on `n4-910c` in 345.693 seconds when
`mem_service` was at the root gitlink and lock revision `002343d`.
An initial run with local `mem_service` HEAD `546985e` reported 11
failures/errors because the bootstrap source check requires `002343d`;
the affected Qwen config and bootstrap modules then passed 60/60 and
11/11 after the isolated checkout returned to the pinned revision.
The 703-test result covers the pinned root configuration. The local
`mem_service` branch is one commit ahead of the root gitlink and two
commits ahead of `origin/ub_sim`. Its new commit must be published and
pinned with the lock before that combination can pass the normal W6
bootstrap check.
The same isolated source snapshot passed `cargo test --workspace
--offline --quiet` on `n4-910c` with exit code 0; at least one Rust test
was reported as ignored. This validates the Rust workspace without a model
oracle or a new multi-node guest run.
The unpinned `mem_service` commit `546985e` passed its 13 focused OBMM
provider tests locally on 2026-09-27. A full `mem_service` unittest
discovery in an isolated `n4-910c` checkout reached its 600-second
limit without a result; it is not a passing full-suite gate. The
isolated checkout was restored to the root-pinned `002343d`, and no
matching unittest process remained.

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

The earlier Qwen callable-3 R9 numerical audit retained raw full-vocabulary
logits snapshots for prompt token IDs `81378,37585,374` and eight output
steps. Parsing its `QPTOTR1` records and applying the guest's indexed
64-bit payload checksum gave a direct comparison with the W6 managed-direct
terminal logs. Both hosts' 2-node/8-step runs matched the R9 full-vocabulary
checksum at all eight steps; each also matched the reference top four IDs
and FP32 logit bits. The reference snapshots, not merely the R9 report's
pass flag, were used for this check. Their source revision and callable
artifact differ from the W6 runs, so this is a cross-version numerical
comparison, not a same-binary identity test.

| W6 topology | Hosts | R9 full-vocab checksum matches | R9 top-four ranks | Largest top-four absolute error |
| --- | --- | ---: | ---: | ---: |
| 2 nodes, 8 steps | both | 8/8 each | 8/8 each | 0 |
| 3 nodes, 8 steps | both | 0/8 each | 8/8 each | `0.00238609314` |
| 8 nodes, 8 steps | both | 0/8 each | 8/8 each | `0.00306034088` |

At each topology and step, the two hosts reported the same full-vocabulary
checksum. The 3/8-node checksum differences are compatible with their
different layer handoff boundaries but do not establish that boundary
rounding is the sole cause. A matching non-cryptographic 64-bit checksum
is strong evidence, not a mathematical proof of bytewise identity. The
3/8-node logs expose top-four values and whole-vocabulary checksums, not
the raw logits arrays, so their full-vocabulary numerical error versus R9
remains unmeasured. These observations strengthen Qwen direct numerical
evidence without closing the entire reference acceptance condition.

The stable cluster entry now accepts `--numerical-trace` for a single-request
managed-direct Qwen run. It creates a run-scoped, test-only `QPTOTR1` trace
directory under `guest-linux/aarch64/out/`; the default remains trace-off.
On `n4-910c`, the two-node, one-step run
`2026-09-26_15-30-35_w5_qwen3_0_6b_decode_30885` passed and produced both
range traces. Its terminal range trace was byte-identical to the earlier R9
trace for prompt tokens `81378,37585,374`. The two-node, eight-step run
`2026-09-26_15-34-56_w5_qwen3_0_6b_decode_31077` passed with tokens
`[264, 3644, 7653, 304, 279, 2070, 315, 1995]`. Both nodes recorded
eight callable-3 host accesses with positive reads/writes and
`shared_payload_staging_bytes=0`. The independent
`sim-cli qwen3-pto-trace-check` consumed all 16 new traces and reported
`status=pass`, zero violations and no missing records across eight steps.
Its full-vocabulary logits maximum absolute error per step ranged from
`2.8610e-5` to `5.1737e-5`, below the checker's existing `0.02` limit;
hidden and KV checks also passed their existing scaled limits. This closes
the same-asset Qwen two-node, eight-step independent numerical gate for the
tested worktree. The run used temporary guest-source changes on a dedicated
test-farm checkout and has not been committed. Tracing adds diagnostic PTO
reads and filesystem writes, so these runs are excluded from performance
claims.

The checker now follows the PP layer partitions for 2/3/8 nodes and accepts
`QPTOTR2` range-input snapshots while retaining `QPTOTR1` read compatibility.
On `n4-910c`, the 3-node, 8-step run
`2026-09-26_16-18-34_w5_qwen3_0_6b_decode_18699` passed 3/3 nodes and
8/8 steps. Its 24 range traces and all eight full-vocabulary logits vectors
matched the independent reference exactly after the checker modeled PTO's
`TCVT(CAST_RINT)` FP16 handoff. The first checker attempt had reported 12 KV
threshold failures at steps 6-7. A captured nodeB pre-handoff value was the
exact FP16 midpoint `2.7080078125`; nodeC read `2.70703125`, while the old
checker expected `2.708984375`. The old helper rounded ties away from zero;
`CAST_RINT` rounds them to even. The corrected independent wire conversion
removed all 12 failures, with zero input, hidden, KV, or logits differences.
Only the audit oracle changed; the PTO model kernel did not.

The 8-node, 8-step run
`2026-09-26_16-31-19_w5_qwen3_0_6b_decode_28986` passed 8/8 nodes,
8/8 steps and produced the same eight token IDs. All eight QEMU nodes logged
eight `QWEN3_PTO_OPERATOR_COUNTS ... kernel_complete=1` records. Its 64
range traces and 64 input snapshots passed the same independent checker with
zero elementwise differences in inputs, layer hidden/KV and full-vocabulary
logits. Host access logs contain 62 intact callable-3 records with positive
reads/writes and `shared_payload_staging_bytes=0`; two records were split by
concurrent QEMU logging, so their full counter lines are not claimed. The
earlier 2-node, 8-step `QPTOTR1` trace was rechecked under the corrected
oracle and also passed with zero elementwise differences. These runs close the
same-asset, one-host Qwen 2/3/8-node eight-step numerical matrix for the
tested worktree. They do not establish second-host repeatability or
performance. The isolated checkout used temporary source patches and remains
uncommitted. QEMU was relinked during the 8-node startup because its binary
predated the refreshed `sim-qemu` static library; build time is excluded from
all inference measurements. The structured 2/3/8-node checker reports remain
in the test farm's ignored `out/w6-qwen-{2,3,8}node-rint-trace-check.json`;
the corresponding `QPTOTR1`/`QPTOTR2` snapshots remain under
`guest-linux/aarch64/out/w6-pto-numerical-trace/` by run ID.
The earlier R9 checksum table compares 3/8-node outputs with a two-node
partition's snapshots across source revisions. Each new audit instead uses
the matching PP partition and PTO FP16 boundary for its independent oracle;
the two comparisons answer different numerical questions.

### 4.2 DeepSeek direct callable and remaining PTO arithmetic

DeepSeek has a model-specific tensor/scalar ABI, callable 5 range
orchestrator and PTO kernels for attention, MLA/indexer, MoE, shared expert,
KV and output head. Shared hidden/KV/logits use managed `UB_GM` views; weights
remain private immutable model data. The cluster-runner 2/3/8-node, 4/8-step
matrix passed on `n4-910c`. A later 2-node/1-step gate logged callable 5
provider reads/writes of `308693/4446` and `339479/133858`, with a terminal
token and no reported staging.

On 2026-09-27, the DeepSeek token control path stopped copying the complete
managed token tensor through PTO `TSTORE` into a host `std::vector`. Each
token now enters through PTO `TLOAD`, and the tile yields only the scalar
embedding/router control index. The focused fixture checks two valid token
positions, invalid IDs, bounds, two loads and zero token-side stores.
The updated source passed all 703 guest Python tests and the Rust workspace
tests on `n4-910c` with the root-pinned `mem_service`. A one-step, all-43-layer
PTO probe for raw token `42` produced token `201`; its complete 129,280-value
FP32 logits were byte-identical to the saved pre-change step-0 PTO output
(`cmp` exit 0). The 2/3/8-node, 4/8-step matrix has not been rerun with
this change.
The changed source then passed a 2-node, 1-step openEuler guest run on
`n4-910c`, run ID
`2026-09-27_18-02-03_w5_deepseek_v4_flash_decode_9549`.
Both nodes completed their callable-5 ranges, and nodeB published token
`201`. Their QEMU model-access records reported `reads/writes=23/3164`
and `26/132487`, each with `shared_payload_staging_bytes=0` and
`status=pass`. The run reused a QEMU binary, Image, and modules from the
test farm's matching QEMU/kernel gitlinks, then rebuilt guest content in
an isolated checkout. It exited 0 with no remaining QEMU process.
The same source passed a 2-node, 4-step openEuler guest run on `n4-910c`,
run ID `2026-09-27_18-10-57_w5_deepseek_v4_flash_decode_5693`.
Both nodes completed 4/4 ranges, and the streamed token IDs were
`[201, 41, 201, 35]`, matching the saved DS4 CPU greedy reference.
All eight QEMU callable-5 model-access records reported positive reads
and writes, `shared_payload_staging_bytes=0`, and `status=pass`. The
runner exited 0. This confirms four-step cross-node functionality of the
new token path. The same isolated source then passed a 2-node, 8-step
openEuler guest run on `n4-910c`, run ID
`2026-09-27_18-32-43_w5_deepseek_v4_flash_decode_4535`. The summary
reported `decode_steps_observed=8`, `passed_nodes=2/2`, and token IDs
`[201, 41, 201, 35, 201, 35, 201, 35]`, matching the saved DS4 CPU
greedy sequence. NodeA and nodeB each recorded eight callable-5 accesses
with positive reads/writes, `shared_payload_staging_bytes=0`, and
`status=pass`; no QEMU process remained. This run passed the second
ratio-4 compressed-row boundary at step 7. The same source also passed
a 3-node, 4-step openEuler guest run on `n4-910c`, run ID
`2026-09-27_19-12-46_w5_deepseek_v4_flash_decode_635`. Its summary
reported `decode_steps_observed=4`, `passed_nodes=3/3`, and token IDs
`[201, 41, 201, 35]`. Nodes A, B, and C each recorded four callable-5
accesses with positive reads/writes, `shared_payload_staging_bytes=0`,
and `status=pass`; no QEMU process remained. The same source passed an
8-node, 4-step openEuler guest run on `n4-910c`, run ID
`2026-09-27_19-34-40_w5_deepseek_v4_flash_decode_18728`.
Its summary reported `decode_steps_observed=4`, `passed_nodes=8/8`,
32 worker and handoff records, and token IDs `[201, 41, 201, 35]`.
Each of the eight QEMU logs recorded four callable-5 accesses with
`shared_payload_staging_bytes=0` and `status=pass`; no QEMU process
remained. At that point, the current-source 3-node eight-step gate and
complete-logits acceptance remained open.

The current-source 8-node, 8-step openEuler attempt on `n4-910c`, run ID
`2026-09-27_20-11-13_w5_deepseek_v4_flash_decode_9912`, failed at
nodeA's step-4 range before producing that range's hidden output:
`rc=-116` (`-ESTALE`). NodeA had loaded the previous terminal token
`35` and published its new OBMM allocation. Immediately before enqueue,
`lingqu_shmem_pto_endpoint` reported a changed CMDQ page physical address
(`0x1bfe1f000` to `0x10d122000`) and a changed CQ page physical address
(`0x1bfe20000` to `0x10ca8b000`). The endpoint correctly failed closed;
that 8-step attempt failed, regardless of the earlier 4-step PASS.
The exact mechanism of the page move (for example migration or copy-on-write)
has not been established. That queue setup used anonymous mappings,
`mlock`, and `/proc/self/pagemap`; it had no device-lifetime DMA pin.
The kernel's `Documentation/core-api/pin_user_pages.rst` specifies
`pin_user_pages*()` for DMA-pinned user pages. A durable repair must give
CMDQ/CQ stable device-visible backing for the entire queue lifetime,
release it only after device quiescence, and retain the stale-page guard.
Rebinding an unpinned PFN or removing that guard would leave an in-flight
move unsafe. This is a queue-memory lifetime failure; the run does not
measure a DeepSeek logits or compressor defect at step 4.

The selected simulator repair is a dedicated queue allocation in
`ub-sim-decoder`: one kernel-owned, nonmovable two-page allocation per
endpoint open. Its device exposes a shared CMDQ/CQ mapping and the two
guest-physical page addresses through a versioned UAPI. The guest PTO
endpoint must use that allocation exclusively, retain MMIO-base and
queue-identity checks, clear the device queue bases before unmapping, and
free the pages only after the last file mapping is gone. Missing queue
device support is an explicit error, not an anonymous-page fallback.
Acceptance requires a kernel build, endpoint contract tests, and a fresh
8-node 8-step run that passes without a stale-page event.

The repair passed those gates on `n4-910c`. The new
`/dev/ub_sim_pto_queue` miscdevice allocates the page pair with
`alloc_pages(GFP_KERNEL | __GFP_ZERO, 1)` per open, maps it shared with
`remap_pfn_range`, and returns the guest physical addresses through
`UB_SIM_PTO_QUEUE_GET_INFO`. The endpoint has no anonymous-page fallback.
The isolated kernel `Image` compiled successfully; its SHA-256 matched the
image copied into the guest-artifact output (`c5deaa1e7baf8b3a0...`).
Run ID `2026-09-27_20-58-55_w5_deepseek_v4_flash_decode_14746` completed
8/8 steps on all eight nodes with `PASS`, exit code 0, and terminal IDs
`[201, 41, 201, 35, 201, 35, 201, 35]`. NodeA's previously failing
step-4 range reported `status=ok`; no guest log contained `queue page stale`
and no QEMU process remained. The target ran 703 guest contract tests
(`OK`) and `cargo test --workspace --offline --quiet` (exit 0).
This closes the observed queue-lifetime failure for that run. It does not
close complete-logits numerical acceptance or cross-host repeatability.
The same fixed kernel also passed a Qwen 2-node, 1-step openEuler run on
`n4-910c`, run ID
`2026-09-27_22-00-56_w5_qwen3_0_6b_decode_13978`. Both node ranges
reported `callable=3`, `model_io=direct`, and `status=ok`; nodeB
published terminal token `264` from managed callable-3 logits. No guest
log contained `queue page stale`, no QEMU process remained, and the
runner exited 0 with `PASS`. This is a shared-endpoint smoke gate for
the queue change, not a new Qwen numerical matrix.

On 2026-09-28, the fixed-kernel isolated checkout on `n4-910c` passed
the current-source DeepSeek 3-node/8-step openEuler gate, run ID
`2026-09-28_09-43-30_w5_deepseek_v4_flash_decode_17771`. Its summary
reported 8/8 steps, 3/3 passed nodes, 24 worker and handoff records,
and terminal IDs `[201, 41, 201, 35, 201, 35, 201, 35]`. Each node's
QEMU log contained eight `callable=5` completions with positive reads
and writes, `shared_payload_staging_bytes=0`, and `status=pass`.
The guest and QEMU logs contained no queue-page or queue-base stale
marker. The `Image` SHA-256 was
`c5deaa1e7baf8b3a03fd41ed474d7ccc62102f38c827446e6a3a9125c73f36be`,
matching the earlier fixed-kernel 8-node run. The runner exited 0, and
no QEMU process remained. An earlier attempt used an insufficient
25-minute outer timeout and was stopped after two tokens; it is not a
model failure or an acceptance result. The passing rerun used a
60-minute bound and reused the prepared guest and QEMU artifacts.
This closes the 3-node/8-step functional gate on this host and source.
Full-logits parity and a same-revision second-host matrix remain open.
The kernel queue repair is committed locally as `c2def7c931e6`; its
root gitlink still points to `ac09d36aa221` until the kernel commit is
published and fetchable.

The direct callable still performs some private model arithmetic in C++.
The router softplus and top-k selection now execute on PTO tiles; only the
selected expert indices reach CPU control state. Q8_K activation quantization
now uses PTO finite-mask, absolute argmax, conversion and 16 group reductions
without copying its 256 FP32 inputs to a CPU array. Its focused golden passed
under both `-O2` and the deployed `-O3 -ffast-math
-fno-finite-math-only` options, including NaN/Inf rejection at the first
and last block positions. On `n4-910c1`, the same changed source passed all
704 guest Python tests and `cargo test --workspace --offline --quiet` with
exit code 0. The default-direct DeepSeek IQ2XXS GGUF completed 2-node
1-step and 4-step openEuler runs with terminal IDs `201` and
`[201, 41, 201, 35]`; all 10 callable-5 host access records reported
positive reads/writes, zero shared staging and `status=pass`. Both runs
cleaned up with zero live/quarantined objects and no run-owned QEMU process.
These results cover the tested path, not the full topology matrix after the
Q8_K change.

The indexer FP4 roundtrip now performs its 32-element magnitude, eight-level
tie-to-even selection, sign and rescale on PTO tiles. The FP8 KV roundtrip
likewise performs the 64-element maximum reduction and E4M3FN bucket and
mantissa selection on PTO tiles. Both paths retain a CPU-visible scalar
maximum and compute a power-of-two scale on the CPU; complete PTO-only model
arithmetic is therefore still open. Independent scalar-reference goldens
cover all seven FP4 boundaries and all 126 E4M3FN boundaries at three scales
under both regular and production compiler options. Direct PTO FP32-to-E4M3
`TCVT` cannot replace the latter path in this CPU simulator: a minimal
probe loaded nonzero FP32 values but encoded all four sample lanes as zero.
`vendor/pto-isa` was left unchanged.

FP4 and FP8 each store one reduced maximum before deriving a block scale.
Q8_0 and Q8_K also store quantized activation blocks into local host arrays
for their dot-product paths; Q8_K additionally stores a finite flag, argmax
index, selected scalar and group sums. These local operator temporaries do
not replace the managed hidden/KV/logits views. The tested model ranges
report zero shared tensor staging, which establishes their managed-direct
tensor boundary. It does not establish PTO-only model arithmetic.
Before the indexer-capacity correction, retained compressed history and
sparse-indexer `top_k` were both capped at 512. `sparse_indexer()` therefore
returned all retained rows before its score-and-select branch. The
correction separates these quantities: the guest C descriptor and PTO state
retain the actual compressed row count, Rust's attention plan reports the
same count, and `top_k=512` controls only per-query selection. At 2052
positions the ratio-4 state has 513 compressed rows; C and C++ layout
goldens check its size and shape, and the operator golden selects rows
512 through 1 from a 513-row input. The 4/8-step cluster runs still cannot
exercise this larger-history branch. A managed model run that enters it
remains necessary. 1M-context runtime validation remains excluded by the
earlier user decision.

An intermediate real-weight range check on `n4-910c` used the existing
DeepSeek GGUF index and the tracked PTO probe at `past=2051`, seeding the
previous state in the production binary layout. It executed layers 0-2
for token `42` and exited 0 with
`seeded_indexer=pass past=2051 candidates=513 topk_calls=1` and
`direct_range=pass ... state_bytes=2181792`. All 16,384 resulting FP32
hidden values were finite. The new counter increments only after the
`selection_count == compressed_kv.rows` early return, so this run reached
the actual score-and-select branch with real weights. The previous state
was synthetic, and this standalone probe did not use mem_service; the
managed long-history runtime gate remains open.

After the seeded probe and top-k execution counter changes, the complete
guest Python suite passed 707/707 on `n4-910c1`. The first offline Rust
workspace run had one unrelated Engram SIMT unit-test failure while
executing a just-written stub (`Text file busy`); the failed test passed
alone and the complete workspace rerun exited 0 without a source change.
The retained test logs are `out/w6-seeded-indexer-guest-tests.log` and
`out/w6-seeded-indexer-cargo-tests-rerun.log` in the isolated target
checkout. The synthetic-state probe log is
`out/w6-indexer-seeded-run.log` on `n4-910c`.

The changed source passed all 706 guest Python tests on `n4-910c1` in
414.199 seconds. The first concurrent model run reached the guests but node B
rejected callable 5 before execution because its expected artifact fingerprint
did not match the guest request. A serial rerun, after the guest suite had
finished, passed DeepSeek 2-node/4-step at run ID
`2026-09-28_17-56-53_w5_deepseek_v4_flash_decode_3250` with token IDs
`[201, 41, 201, 35]`. All eight callable-5 host access records had positive
reads and writes, zero shared staging and `status=pass`. Both guests reported
zero live and quarantined objects after cleanup; no run-owned QEMU process
remained. The local SSH client lost contact after the remote run had already
passed, so the local wrapper returned 255; the retained remote completion and
guest/QEMU logs establish the result. Concurrent artifact mutation is a
plausible cause of the first fingerprint mismatch, but has not been proven.
The stable cluster entry now builds each Qwen or DeepSeek direct artifact in
a distinct `guest-linux/aarch64/out/w6-...` directory. A focused test on
`n4-910c` prepared two manifests and confirmed that the first persisted
after the second preparation. The updated entry passed all 707
guest Python tests on `n4-910c1` in 440.315 seconds and the offline Rust
workspace suite with exit code 0. A Qwen 2-node/1-step smoke run
`2026-09-28_20-21-35_w5_qwen3_0_6b_decode_16676` passed with token ID
`264`, two callable-3 host records with positive I/O and zero shared staging,
and zero live/quarantined objects after cleanup. During the DeepSeek
3-node/4-step run below, a separate `--print-env` preparation completed in
its own `w6-deepseek-v4-flash-direct-artifacts.VSgtIsZ5` directory. This is
one controlled overlap, not an exhaustive concurrency or retention test.
The fixed output-directory mutation mechanism has been removed.

The same FP8/FP4 source passed a DeepSeek 8-node/8-step run on `n4-910c1`:
`2026-09-28_19-15-02_w5_deepseek_v4_flash_decode_30115` emitted
`[201, 41, 201, 35, 201, 35, 201, 35]`. All 64 callable-5 records had
positive reads and writes, zero shared staging and `status=pass`; all eight
guests cleaned up with zero live/quarantined objects and no run-owned QEMU
process. On `n4-910c`, the source-matched 2-node/4-step run
`2026-09-28_20-13-16_w5_deepseek_v4_flash_decode_15858` passed with
`[201, 41, 201, 35]`, eight equivalent callable-5 records, clean guest
resources and no run-owned QEMU process. Its kernel queue-fix file hashes
matched the local `c2def7c` commit, although that isolated checkout still
held the patch as uncommitted files. These runs do not replace the complete
same-source 2/3/8-node by 4/8-step, two-host matrix.

The changed source also passed DeepSeek 3-node/4-step on `n4-910c1` in run
`2026-09-28_20-39-11_w5_deepseek_v4_flash_decode_18197`, concurrent with
the separate artifact preparation above. All three guests completed four
ranges and emitted `[201, 41, 201, 35]`. The 12 callable-5 host access
records had positive reads and writes, `shared_payload_staging_bytes=0`,
and `status=pass`. All three guest cleanup records reported zero live and
quarantined objects. The QEMU logs contained zero callable fingerprint
rejections, and no QEMU process remained from the isolated checkout. This
verifies the observed overlap; it does not cover arbitrary concurrent builds.

The same isolated checkout passed Qwen 8-node/8-step on `n4-910c1` in run
`2026-09-28_21-25-30_w5_qwen3_0_6b_decode_28893`. All eight guests
completed eight ranges, with terminal IDs
`[264, 3644, 7653, 304, 279, 2070, 315, 1995]`. QEMU logs contain 64
callable-3 access markers; 60 intact records have positive reads/writes,
zero shared staging and `status=pass`. The other four marker lines were
interleaved with CQ logging, so their complete counters are not claimed.
All eight guest cleanup records report zero live and quarantined objects,
and no QEMU process remained from the isolated checkout. This is a
current-source eight-node regression gate, not a same-source two-host matrix.

On 2026-09-29, the same current Qwen production source passed all six
2/3/8-node by 4/8-step cases in the isolated `n4-910c` checkout:

| Nodes | Steps | Run ID | Callable-3 access records |
| ---: | ---: | --- | ---: |
| 2 | 4 | `2026-09-29_11-53-04_w5_qwen3_0_6b_decode_17974` | 8/8 |
| 2 | 8 | `2026-09-29_11-42-36_w5_qwen3_0_6b_decode_25262` | 16/16 |
| 3 | 4 | `2026-09-29_11-47-56_w5_qwen3_0_6b_decode_21372` | 12/12 |
| 3 | 8 | `2026-09-29_11-15-32_w5_qwen3_0_6b_decode_13291` | 24/24 |
| 8 | 4 | `2026-09-29_11-21-29_w5_qwen3_0_6b_decode_9400` | 32/32 |
| 8 | 8 | `2026-09-29_11-28-35_w5_qwen3_0_6b_decode_13429` | 64/64 |

Each run reported all expected worker/handoff records and the matching
prefix of `[264, 3644, 7653, 304, 279, 2070, 315, 1995]`. All access
records have positive reads/writes, zero shared staging and `status=pass`;
one 8-node/4-step node-C record had its fields split by concurrent CQ log
output and was reconstructed from adjacent fragments. Every guest cleanup
reported zero live and quarantined objects. No run-owned QEMU process
remained between cases. This closes the current-source Qwen matrix on
`n4-910c`; the separate `n4-910c1` matrix above predates these source
changes.

After the compressed-history correction, `n4-910c1` passed all 707 guest
Python tests with the established test-farm environment and the offline
Rust workspace suite with exit code 0. An earlier guest-suite invocation
omitted that environment; it ended with 17 compiler/toolchain-related test
failures and is not counted as a code regression. The corrected source then
passed DeepSeek 2-node/4-step in run
`2026-09-29_08-53-14_w5_deepseek_v4_flash_decode_1840`, emitting
`[201, 41, 201, 35]`. All eight callable-5 host access records reported
positive reads and writes, zero shared staging and `status=pass`. Both
guests reported zero live and quarantined objects after cleanup, with no
QEMU process left from the isolated checkout. This validates the current
short-sequence path after the state-layout change; it does not validate a
managed range with more than 512 compressed history rows.

The same current DeepSeek source completed 8-node/8-step on `n4-910c` in
run `2026-09-29_10-10-07_w5_deepseek_v4_flash_decode_25886`. The runner
reported `decode_steps_observed=8`, `passed_nodes=8/8`, 64 worker/handoff
records, and terminal IDs `[201, 41, 201, 35, 201, 35, 201, 35]`. Each
QEMU node logged eight callable-5 access records. Sixty-three intact
records show positive reads/writes, zero shared staging and `status=pass`;
node H's remaining record is interleaved with CQ messages, but its adjacent
fragments identify request `11654077420883633702`, `reads=682`,
`writes=129972`, zero staging and `status=pass`. All eight guests reported
`live_objects=0` and `quarantined_objects=0` after 64 allocations each;
`pgrep` found no remaining QEMU process. The initiating SSH client exited
255 after step 6, while the remote launcher continued to the recorded
`PASS`. The retained log is `out/w6-indexer-capacity-deepseek-8node8-n4-run.log`
in the isolated target checkout. This run covers eight-step managed-direct
functionality on the second host, not the 513-row managed-history branch.

The user accepted deferring DeepSeek full-logits precision alignment on
2026-09-28. This is a temporary scope decision for functional validation;
the independent oracle criterion in Section 5 remains recorded as unmet.

The current-source DeepSeek 3-node/8-step attempt on `n4-910c1`, run
`2026-09-29_11-12-16_w5_deepseek_v4_flash_decode_8725`, reached all eight
steps in all three guests, including the terminal token at step 7. Every
guest reported zero live/quarantined objects after cleanup. The runner
returned 1 because one node-B `deepseek-v4-flash-pto-range` line was split
by concurrent CQ text; its exact fields continue across adjacent lines,
while all eight callable-5 access lines were intact and passed. The
sim-uapi logger now emits each model-access and range-completion marker in
one `write` call. Its focused unit test, offline Rust workspace suite and
707/707 guest tests passed on `n4-910c1`. The repaired run
`2026-09-29_12-21-15_w5_deepseek_v4_flash_decode_6772` then passed
3-node/8-step: 24/24 worker/handoff records and token IDs
`[201, 41, 201, 35, 201, 35, 201, 35]`. Every node logged eight intact
callable-5 access lines and eight intact range-completion lines, all with
positive reads/writes where applicable, zero shared staging and
`status=pass`. Three guest cleanup records reported zero live/quarantined
objects, and no run-owned QEMU process remained. The failed attempt remains
recorded as evidence of the log-interleaving defect; the repaired rerun
closes that runner gate.

With the same atomic logger, `n4-910c` passed DeepSeek 2-node/8-step in
run `2026-09-29_12-23-44_w5_deepseek_v4_flash_decode_19045`, emitting the
same eight token IDs. Both nodes logged eight intact callable-5 access
lines and eight intact range-completion lines with positive reads/writes,
zero shared staging and `status=pass`. Both guests cleaned up with zero
live/quarantined objects; no run-owned QEMU process remained. Two further
same-source eight-step cases passed with the same token IDs and validation
conditions:

| Host | Nodes | Run ID | Complete access/range records |
| --- | ---: | --- | ---: |
| `n4-910c` | 3 | `2026-09-29_13-10-43_w5_deepseek_v4_flash_decode_23447` | 24/24 each |
| `n4-910c1` | 8 | `2026-09-29_13-10-04_w5_deepseek_v4_flash_decode_16594` | 64/64 each |

Both reported all expected worker/handoff records, zero shared staging,
positive provider reads/writes and `status=pass`. Every guest cleaned up
with zero live/quarantined objects, and neither host retained a run-owned
QEMU process. The full same-source DeepSeek matrix and the managed 513-row
history gate remain open.

The atomic-logger source also passed two 4-step cases on 2026-09-29:

| Host | Nodes | Run ID | Complete access/range records |
| --- | ---: | --- | ---: |
| `n4-910c1` | 2 | `2026-09-29_14-04-38_w5_deepseek_v4_flash_decode_305` | 8/8 each |
| `n4-910c` | 8 | `2026-09-29_14-05-21_w5_deepseek_v4_flash_decode_11662` | 32/32 each |

Both emitted `[201, 41, 201, 35]`, completed every expected worker and
handoff record, and had positive provider reads/writes with zero shared
staging and `status=pass`. The 2-node and 8-node runs reported 2/2 and 8/8
clean guest resources, respectively; neither left a run-owned QEMU process.
These observations do not exercise the 513-row managed-history branch.

Two more same-source 4-step cases passed after explicitly selecting the
existing test-farm GGUF and openEuler disk paths:

| Host | Nodes | Run ID | Complete access/range records |
| --- | ---: | --- | ---: |
| `n4-910c` | 2 | `2026-09-29_14-53-48_w5_deepseek_v4_flash_decode_28542` | 8/8 each |
| `n4-910c1` | 3 | `2026-09-29_14-53-21_w5_deepseek_v4_flash_decode_3378` | 12/12 each |

Both emitted `[201, 41, 201, 35]`, had positive provider reads/writes,
zero shared staging and `status=pass`, and completed all expected worker
and handoff records. All five guest cleanup records reported zero live and
quarantined objects; neither run left a QEMU process. Two preliminary
launches on each host exited before guest startup because the container
defaults `/models/...` were absent in these bare-metal test checkouts.
The successful runs used the entrypoint's `--model` and
`--open-euler-disk-image` options to select the already present files.

The atomic-logger source then passed two additional cells:

| Host | Nodes | Steps | Run ID | Complete access/range records |
| --- | ---: | ---: | --- | ---: |
| `n4-910c` | 3 | 4 | `2026-09-29_20-14-21_w5_deepseek_v4_flash_decode_2187` | 12/12 each |
| `n4-910c1` | 2 | 8 | `2026-09-29_20-14-38_w5_deepseek_v4_flash_decode_7833` | 16/16 each |

The 4-step and 8-step terminal sequences matched the respective prefixes
of `[201, 41, 201, 35, 201, 35, 201, 35]`. All records show positive
reads/writes, zero shared staging and `status=pass`; all five guests
reported zero live/quarantined objects, and no run-owned QEMU remained.

The remaining `n4-910c1` 8-node/4-step cell passed in run
`2026-09-29_20-59-17_w5_deepseek_v4_flash_decode_1961` with
`[201, 41, 201, 35]`, 32/32 worker/handoff records, 32/32 intact
callable-5 access markers and 32/32 intact range markers. Every access
had positive reads/writes, zero shared staging and `status=pass`.
All eight guest cleanups reported zero live/quarantined objects, and no
run-owned QEMU process remained. Thus the 2/3/8-node by 4/8-step
DeepSeek functional matrix passed on both test-farm hosts with the same
model production source. The `n4-910c` 8-node/8-step run predates only
the atomic log-emission change, so it does not prove byte-identical
logging-source repeatability. None of the short runs reaches 513
compressed history rows.

After the W5 entry made `--open-euler-disk-image` select the openEuler
guest engine, the complete guest Python suite passed 708/708 on
`n4-910c` using the PATH-only test-farm environment. The first suite
attempt loaded the DeepSeek workload environment into the test process
and failed six unrelated Qwen expectations; one golden fixture in that
checkout also lagged the current header. The fixture and entry test
expectations were synced before the clean passing rerun. The retained
passing log is `out/w6-openeuler-cli-guest-tests-clean.log` in the
isolated checkout.

The current Qwen production source also passed the 2/3/8-node by
4/8-step matrix on `n4-910c1`. The five 2/3/8-node cases through
8-node/4-step had run IDs `2026-09-29_21-28-05_w5_qwen3_0_6b_decode_6145`,
`2026-09-29_21-33-31_w5_qwen3_0_6b_decode_26249`,
`2026-09-29_21-36-40_w5_qwen3_0_6b_decode_19671`,
`2026-09-29_21-43-32_w5_qwen3_0_6b_decode_27765`, and
`2026-09-29_21-50-41_w5_qwen3_0_6b_decode_4939`. Their QEMU logs contain
the expected 8, 16, 12, 24, and 32 callable-3 access records, all with
positive reads/writes, zero shared staging, and `status=pass`. The first
8-node/8-step attempt produced all eight expected tokens and 64/64
successful access records, but the launcher could not count one pass marker
concatenated with OBMM serial output; it is not counted as a passing run.
After the log matcher and progress parser accepted this observed serial
interleaving, run `2026-09-29_22-59-28_w5_qwen3_0_6b_decode_8535` passed:
8/8 steps, 8/8 nodes, token IDs
`[264, 3644, 7653, 304, 279, 2070, 315, 1995]`, and eight intact
callable-3 access records per QEMU node with positive reads/writes, zero
shared staging, and `status=pass`. The runner checked each node's managed
cleanup and found no run-owned QEMU process after completion. The complete
guest Python suite after this change passed 709/709 on `n4-910c`; its log is
`out/w6-pass-log-fix-guest-tests.log` in the isolated checkout. The offline
Rust workspace suite had already passed after the preceding Rust source
change; this follow-up changed only shell, Python tests, and documentation.

The sim-console openEuler and second-host functional gates are recorded
below. Independent range/logits oracle comparison and repeat-run numerical
stability remain open before full acceptance.
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

A later current-source audit on `n4-910c` rechecked the position-1
layer-1 result. A four-layer trace built against the deployed PTO headers
and an independent one-layer `decoder_range` probe produced the same
16,384-value layer-1 hidden SHA-256
`572ba6aa9f14dc749f94993b03ce62658a94c56c6c7155da5783c39857b20164`.
Against the existing DS4 trace, its relative L2 was `0.004623975`;
layers 2 through 4 rose to `0.008498810`, `0.009735931`, and
`0.012326177`. The older fast-math binary still reproduces its saved
layer-1 output exactly and its `1.018e-6` DS4-relative error, but
rebuilding the old diagnostic source against current headers reproduced
the current hash. Rebuilding with only the pre-`a8cde067` pool header,
under GCC 15.3 and separately under GCC 15.2, also reproduced that
current hash. The old binary's exact source/header/flag provenance is
unavailable. Its favorable layer-1 measurement is therefore not a
current-source acceptance result or evidence that the pool-order change
caused a layer-1 regression. No production code was changed by this audit.

A second-token (`[42, 201]`) stage trace then narrowed the current-source
error on `n4-910c`. The new DS4 trace was compiled with the reference's
GCC 10.3 CPU flags; its complete logits and layer-1 hidden are byte-identical
to the saved independent DS4 oracle. PTO used the deployed headers with
`-O3 -ffast-math -fno-finite-math-only` under `__CPU_SIM`. Relative L2
errors against the GCC 10.3 DS4 trace were:

| Position-1 layer-1 boundary | PTO/DS4 relative L2 |
| --- | ---: |
| Query after RoPE | `2.4251e-7` |
| Attention heads after inverse RoPE | `1.5510e-7` |
| Grouped Q8 output-A | `1.1638e-4` |
| HC state after attention | `3.4500e-4` |
| HC state after FFN | `4.62398e-3` |

Replaying the exact DS4 heads through the current PTO grouped projection
gave output-A relative L2 `1.6925e-7`. Feeding the exact DS4 post-attention
HC state into the current PTO FFN gave complete layer-1 relative L2
`7.7139e-7`. Thus the first measured amplification is at grouped Q8 input;
FFN magnifies the incoming state error but agrees closely on common input.
The diagnostic outputs are in
the target's ignored `out/w6-range-parity/` directory under
`ds4-step1-stageq.*`, `pto-step1-stageq.*`,
`pto-output-a-from-ds4-step1.f32`, and
`pto-ffn-from-ds4-step1.layer01.f32`.

The position-1 grouped-Q8 boundary now has a causal one-lane test. Applying
the current PTO `quantize_q8_0` to both native heads tensors changed only
one of 32,768 codes: block 851, lane 13 was `42` for DS4 heads and `41`
for PTO heads. The lane-13 input itself was byte-identical
(`0.3107659221`). Block 851's max-absolute-value lane 31 was
`-0.9510185719` for DS4 and `-0.9510186315` for PTO, producing scales
`0.007488335017` and `0.007488335483`. The lane-13 quotient crossed the
`41.5` rounding boundary. Across all 1,024 blocks, 561 scales had small
differences; the maximum relative difference was `7.59e-7`. Replacing
only PTO's block-851 lane-31 input with the DS4 value made every packed
code agree and reduced complete grouped output-A relative L2 error from
`1.1638e-4` to `2.7201e-7`. This verifies that this single scale/code
flip causes almost all of the measured position-1 output-A difference.

The replay path was checked against the original PTO heads byte-for-byte.
Replaying the exact DS4 query with the same two raw KV rows through PTO
attention made the critical max lane, its scale and every Q8 code agree
with DS4. Feeding DS4's exact attention-norm tensor to the current PTO
query projection also made every resulting heads code agree, while its
query relative L2 error decreased from `2.4251e-7` to `1.6216e-7`.
The token-201 HC-mixed tensor differed from DS4 by relative L2
`2.2190e-8`, rising to `5.5146e-7` after attention RMSNorm. These
interventions establish the causal chain for this one activation boundary:
HC/normalization arithmetic changes Q, which changes one heads block
scale and code, then grouped projection amplifies that change. They do
not establish that a global RMSNorm rewrite improves full-model logits;
the earlier global and attention-only reduction experiments did not.

A saved token-201, layer-1 Q-stage replay on `n4-910c` compared the native
PTO path and a PTO path fed the exact DS4 attention-norm tensor against
the same DS4 stage outputs. All vectors had the expected dimensions and
finite values. The same-input errors start at `attn_q_a`, before Q LoRA
normalization, the second Q8 projection, head normalization or RoPE:

| Stage | Values | Native PTO relative L2 | Same-input PTO relative L2 |
| --- | ---: | ---: | ---: |
| `attn_q_a` output | 1,024 | `5.9575e-7` | `1.1149e-7` |
| Q LoRA RMSNorm output | 1,024 | `2.4135e-7` | `1.4569e-7` |
| Final Q after RoPE | 32,768 | `2.4251e-7` | `1.6216e-7` |

This separates upstream normalization error from residual same-input Q8
projection arithmetic. DS4 uses integer Q8 dots with two fused FP32
accumulators on the AArch64 path; PTO converts the Q8 operands to FP32
tiles and accumulates their scaled dot results. The source difference is
consistent with the measured residual but is not yet a causal proof for
full-model logits drift. The saved stage tensors and replay output are
under ignored `out/w6-range-parity/` on the test bed; no production
operator changed. A source diff verified that the test-bed production
operator header and this worktree differ only in the optional logits
trace output and its `TSTORE` count; the Q projection implementation is
identical.

A GCC 10.3 DS4 norm replay self-check reproduced the native DS4
attention-norm tensor byte-for-byte. Applying that same DS4
double-accumulation RMSNorm to the PTO token-201 HC-mixed tensor reduced
attention-norm relative L2 from `5.5146e-7` to `2.5170e-8`; 3,700 of
4,096 normalized values then matched DS4 exactly. The resulting PTO Q
and two-row attention replay matched all 32,768 DS4 heads Q8 codes.
This confirms the reduction as the trigger for this local code flip.
The diagnostic tensors are `ds4-norm-from-pto-mixed-201.f32` and
`pto-heads-from-ds4-reduction-201.*` in the ignored target output.

A separate full-range diagnostic copied the committed PTO headers into an
ignored directory and changed only `rms_norm` to accumulate squared inputs
in C++ `double`; this is a CPU-simulator experiment, not a PTO-hardware
implementation. The copied-source baseline reproduced both saved PTO
step-0 and step-1 complete-logits files byte-for-byte. With the double-sum
variant, the step-0 logits relative L2 against the fixed GCC 10.3 DS4
reference moved from `0.003836918` to `0.003825395`, while maximum
absolute error worsened from `0.081278` to `0.097858`. Both selected
token `201`. The relative-L2 improvement is about 0.3%, with a larger
worst-case error; this candidate did not meet the predeclared requirement
to improve the complete logits and was stopped before the eight-step run.
It was not promoted to production. The baseline and candidate outputs
are under the target's ignored `out/w6-range-parity/exact-rms/` directory.

The DS4 reference build is material: compiling the same DS4 diagnostic with
GCC 15.3 selected token `361` rather than GCC 10.3's `41`; the complete
step-1 logits differ by relative L2 `0.031354`. The saved PTO step-1
logits differ from those GCC 10.3 and GCC 15.3 DS4 builds by `0.033570`
and `0.045010`, respectively. Compiler variability does not establish PTO
parity; all future differential gates must record and hold the reference
compiler and flags fixed. No production source changed in this stage trace.

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

A saved-tensor Q8 differential check on `n4-910c` further bounds this
amplification. The token-42 DS4 and production-flag PTO first-layer heads
each contain 32,768 FP32 values; 18,987 match bitwise, their relative L2
error is `8.483157e-8`, and their maximum absolute difference is
`9.536743e-7`. Applying the existing PTO `quantize_q8_0` probe to both
files yields 198 differing Q8 codes across 1,024 blocks and 429 differing
block scales. The first code difference is block 19, lane 5 (`64` versus
`63`). As a control, the same probe reproduces the documented token-201
result: one differing code at block 851, lane 13 and 561 differing scales.
These are diagnostic CPU-simulator results on saved heads, not a new guest
run. They show that the token-201 single-lane correction cannot explain
the token-42 output-A or complete-logits mismatch; a candidate upstream
arithmetic change must be checked against both positions and the full
eight-step logits sequence. The ignored input tensors are
`out/w6-range-parity/ds4-first-stage-42.heads.f32` and
`out/w6-range-parity/pto-fastmath-stage-attention.f32.raw.f32.heads.f32`.

The tracked `deepseek_v4_flash_pto_range_probe.cpp` now has an
`--attention-heads-from-qkv` mode that replays PTO first-layer attention
from saved Q, KV and sink tensors without loading model weights. Its
model-free fixture passed locally. On `n4-910c`, the probe was compiled
with GCC 15.3 and the production DeepSeek arithmetic flags against
production range/ops headers whose SHA-256 matched the local source.
Using the DS4 token-42, position-0 Q/KV/sink tensors produced heads with
SHA-256 `4e55ae34649288b0c7b1fe133c40a2adaf33352d97ea6d64688f32bb59224212`,
identical to two earlier ignored same-input outputs. The current-source
PTO Q8 probe found 74 differing codes and 295 differing block scales
against DS4, compared with 198 codes and 429 scales for native PTO
heads. DS4's saved pre- and post-inverse-RoPE heads are byte-identical
at position 0. This establishes an attention-arithmetic residual under
identical Q/KV/sink inputs and shows that upstream input differences also
matter at this boundary. The net code-count reduction does not assign
each individual code flip to one stage. No production kernel was changed,
and full-range logits acceptance remains open.

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

A narrower token-42 first-HC probe on `n4-910c` compared the same IQ2 GGUF
through DS4 CPU and the production-flag PTO CPU simulator. Both diagnostic
stage replays matched their respective original HC preparation paths. The
existing FP32 comparison tool produced these zero-tolerance results:

| Stage | Values | Exact values | Relative L2 error |
| --- | ---: | ---: | ---: |
| HC token embedding | 16,384 | 16,384 | 0 |
| HC control RMSNorm output | 16,384 | 0 | `1.228742e-6` |
| HC control projection | 24 | 0 | `9.796435e-7` |
| HC split | 24 | 5 | `5.093556e-8` |
| HC weighted mix | 4,096 | 3,646 | `2.297681e-8` |
| Attention RMSNorm output | 4,096 | 0 | `4.352559e-7` |

The embedding is byte-identical; the first observed difference is therefore
the HC control RMSNorm, before the HC projection, weighted mix, attention,
or compressed pooling. The common embedding's double-precision mean square
is `0.00143036300916`. Fitting one scale to all 16,384 outputs gives
`26.4316997026` for DS4 and `26.4317321667` for PTO; the DS4 formula
with the same `1e-6` epsilon gives `26.4316997528`. DS4 accumulates its
squared sum in `double`; PTO's `TROWSUM` and cross-chunk total are FP32.
The matching input, scale measurements, and implementations identify
reduction arithmetic as the first-layer control-norm discrepancy. They do
not establish that it is the sole source of the full-model logits drift.

An isolated PTO-only compensation experiment used 32-value `TROWSUM`
chunks and scalar Kahan accumulation under the deployed GCC 15.3
`-O3 -ffast-math -fno-finite-math-only` flags. The same-GGUF token-42
first-layer comparison against DS4 produced:

| PTO reduction scope | HC control exact values | Attention norm relative L2 | Complete layer-1 hidden relative L2 |
| --- | ---: | ---: | ---: |
| Committed baseline | 0 / 16,384 | `4.352559e-7` | `0.004371485` |
| All RMSNorm calls | 16,384 / 16,384 | `5.604918e-8` | `0.004486001` |
| HC control only | 16,384 / 16,384 | `4.352559e-7` | `0.004371411` |
| Attention RMSNorm only | 0 / 16,384 | `5.604918e-8` | `0.003683477` |

The control-only variant's attention norm output was byte-identical to
the committed baseline (4,096 of 4,096 values), even though its HC
control norm matched DS4 exactly. The all-RMSNorm variant improved the
attention norm but worsened the complete layer. These controlled results
reject both variants as production fixes. They show why locating the
earliest numerical difference alone cannot explain the much larger
complete-layer error. The attention-only variant reduced the complete
first-layer error by about 15.7%, so it was also run through all 43 layers
and the output head. Its step-0 full-logits relative L2 error increased
from the committed baseline's `0.003836918` to `0.007350699`, and maximum
absolute error increased from `0.081278` to `0.159392`. Under the
diagnostic `1e-4 + 1e-3 * abs(ref)` threshold, values outside tolerance
rose from 108,972 to 118,762 of 129,280. Both selected token 201.
Attention-only compensation therefore also fails the full-logits gate.
None of these RMSNorm variants was applied to production code.

The same token-42, position-0 probe dumped all 43 post-layer hidden
vectors for DS4, baseline PTO, and attention-only compensation. Baseline
relative L2 error was `0.004371485` after layer 1, `0.002421645`
after layer 36, `0.006062818` after layer 37, and `0.002307412`
after layer 43. The attention-only variant measured `0.003683477`,
`0.004053881`, `0.017845502`, and `0.006605937` at those layers.
Both traces amplify the error at layer 37; the attention-only variant
amplifies it more and ends with worse full logits. This trace locates
where error grows, without identifying the responsible operator. No
compressed row can be emitted at position 0, so compressor pooling
cannot explain the first-layer or step-0 logits discrepancy.

A production-header, token-42 position-0 probe on `n4-910c` localized the
layer-37 spike further. DS4 and PTO selected the same six routed experts
`[104, 100, 198, 160, 214, 147]`. Their 16,384-value hidden tensors had
relative L2 error `0.002421645` before layer 37, `0.002423162` after
attention, and `0.006062818` after FFN. A DS4 FFN replay on its captured
post-attention tensor reproduced its original post-layer tensor byte-for-byte.
Feeding that identical DS4 post-attention tensor to the production PTO FFN
reduced post-layer error to `6.740788e-7` relative L2 (maximum absolute
error `0.00134277344`). Thus this layer's FFN amplifies incoming numerical
drift; its expert IDs do not diverge, and same-input FFN arithmetic agrees
closely. This identifies where the step-0 hidden error grows, not the first
upstream operator responsible for it. The ignored diagnostic traces are in
`out/w6-range-parity/` locally and on `n4-910c`; the PTO probe explicitly
used the production header, since an older experimental header in the
diagnostic directory otherwise changes the measured arithmetic.

A focused comparison of the existing first-layer token-42 DS4 and PTO
diagnostic dumps found Q8 activation sensitivity. Their query
tensors have relative L2 error `2.45110827e-7`; their post-attention,
inverse-RoPE heads have relative L2 error `8.84045342e-8`.
An initial strict-FP32 replay predicted 217 differing Q8 codes and put
the first difference at block 1, lane 27. A local `-O2` PTO CPU-sim
fixture also returned codes 63 and 64 there. These results do not match
the deployed arithmetic flags. On `n4-910c`, GCC 15.3 with the DeepSeek
kernel's `-O3 -ffast-math -fno-finite-math-only` flags compiled the PTO
`quantize_q8_0` operator. Applied to the same two 32,768-value heads
dumps, it found 212 differing codes and 449 differing scale bit patterns.
Its first code difference was block 0, lane 21: the DS4-dump input
`-0.693499148` produced code `-64` with scale `0.0109212464`, while
the PTO-dump input `-0.693499207` produced code `-63` with scale
`0.0109212473`. At block 1, lane 27, both inputs produced code 64.
The earlier strict-FP32 first-difference claim is therefore invalid for
the production compiler. This probe applies the *PTO* operator to both
float dumps.

An independent wrapper around DS4 revision `931b98e` then called its
`quantize_q8_0_activation` on each of the same heads dumps. DS4 used
its Makefile-equivalent `-O3 -ffast-math -fno-finite-math-only`
arithmetic; both probes ran with GCC 15.3. The SHA-256 results compare
the complete 32,768-byte code stream and 1,024-scale FP32 stream:

| Heads input | Code SHA-256, DS4 and PTO | Scale SHA-256, DS4 and PTO |
| --- | --- | --- |
| DS4 dump | `e192af232cfddc3f6cd4e6ee21ed8b81527f388ae64497e7b5f5014efc40d16d` | `d9bac9d70ac9dc8e9ec27f9cb0deee9a90bacdb713fbf75294d3a38f4e22f679` |
| PTO dump | `3a27c234330287bbdb0155330df4357f0b025794b879ccc620affa376b08bff7` | `b3c194eccf5dbf91dc06ec3cd870b0ac41203c384c16d46bcf3565df309f9e89` |

For either input, the two implementations were byte-identical across
all codes and scales. The 212 code and 449 scale differences arise from
the differing heads inputs. At the first code difference, both
implementations return `-64` for the DS4 heads and `-63` for the PTO
heads. This isolates the grouped Q8 activation mismatch upstream of
quantization for this token and layer. The query already differs before
sink attention; trace the earliest HC/query-stage value that changes
under common input. Full-model logits parity remains unproven.

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

On `n4-910c`, a diagnostic-only router candidate replaced the private
`std::log1p` softplus loop with PTO `TMAXS/TABS/TNEG/TEXP/TADDS/TLOG/TADD/TSQRT`
on tiles. It retained the existing top-k selection and used the same IQ2
GGUF, token sequence `[42, 201, 41, 201, 35, 201, 35, 201]`, GCC 15.3
`-O3 -ffast-math -fno-finite-math-only`, and fixed GCC 10.3 DS4 reference.
All 701 guest tests passed with the current guest-source patch. The direct
probe completed eight steps and selected the same eight tokens as DS4.
Complete-vocabulary relative L2 logits errors were:

| Step | Existing PTO | PTO tile softplus |
| ---: | ---: | ---: |
| 0 | `0.003837` | `0.002665` |
| 1 | `0.033570` | `0.014962` |
| 2 | `0.072947` | `0.062466` |
| 3 | `0.054313` | `0.066389` |
| 4 | `0.109596` | `0.059164` |
| 5 | `0.091544` | `0.060934` |
| 6 | `0.095986` | `0.089205` |
| 7 | `0.107647` | `0.170240` |

The independent token-43 first-step holdout improved from `0.018360` to
`0.011734`, but step 3 worsened and step 7 worsened by about 58%. The
step-7 maximum absolute logits error also rose from about `2.189` to
`2.428`. The candidate was therefore removed from production source; the
isolated target checkout was restored clean. Its ignored diagnostic logits
remain under `out/w6-range-parity/pto-router-tile-*.logits.f32` on
`n4-910c`. This experiment neither validates a PTO-only router nor
identifies router softplus as the cause of the DS4/PTO discrepancy. Any
replacement must retain the `log1p` numerical behavior well enough to
pass all eight steps and independent inputs, not only the first token.

A second, diagnostic-only PTO router candidate compensated the rounding
of `1+u` with `log(1+u) - (((1+u)-1)-u)/(1+u)`, where
`u=exp(-abs(logit))`. The existing router golden covered logits from
`-20` through `20`, and all 701 guest tests passed on `n4-910c` with
GCC 15.3. The same-GGUF token-42 full-range probe selected token `201`,
but its 129,280-value step-0 logits relative L2 error against the fixed
DS4 reference rose from `0.003836918` to `0.005142310`; maximum absolute
error rose from `0.081278` to `0.109780`. The predeclared first-step gate
failed, so no multi-step run was attempted. This candidate was also
removed from production source, and the isolated checkout was restored
clean. Its diagnostic output is
`out/w6-range-parity/pto-router-compensated-42.logits.f32` on `n4-910c`.
PTO `TLOG` compositions have now failed model-level acceptance in both
naive and compensated forms. Further router work needs an explicit
`log1p`-equivalent operator contract and common-input numerical evidence
before another full-model trial.

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

FP64 softmax over the saved complete logits quantifies that gap: DS4 and
PTO have the same top token in all eight steps, but their largest
total-variation distance is `0.120286` (step 2) and largest
`KL(DS4 || PTO)` is `0.030552` (step 7). At step 2 the selected token's
probability is `0.582298` for DS4 versus `0.466182` for PTO; top-five
overlap is 4/5 at steps 4 and 5 and 5/5 elsewhere. These values describe
one seed and cannot be promoted into a post-hoc acceptance threshold.
On 2026-09-26, an independent DS4 CPU eight-step reference for raw seed
token `43` completed with outputs
`[42498, 51, 18, 48, 18, 70, 58, 102447]`. The matching PTO CPU
probe was stopped after step 0 because its prior eight-step token-42
artifacts took about 29 minutes from first to last logits snapshot; the
new run's 15-minute bound was insufficient. Its completed step selected
`42498`, with top-five overlap 5/5, total variation `0.023523`,
KL `0.001700`, and relative L2 logits error `0.018360`. Multi-step
token-43 PTO or guest parity remains unverified.

The DS4 reference itself has a measured compiler-arithmetic sensitivity.
An existing eight-step DS4 CPU probe built with the reference
`-O3 -ffast-math -march=native` options and a strict-FP variant built with
`-O3 -fno-fast-math -ffp-contract=off` used the same GGUF and token `42`.
The strict-FP versus fast-math complete-logits relative L2 error was
`0.005978825` at step 0 and `0.090513015` at step 7. PTO versus the
strict-FP DS4 step-7 logits had relative L2 error `0.105410341`; PTO
versus fast-math DS4 had `0.107647`. All three eight-step outputs selected
`[201, 41, 201, 35, 201, 35, 201, 35]`, though top-1 margins changed.
This establishes substantial arithmetic sensitivity even within DS4.
It does not establish an acceptable W6 tolerance or rule out an operator
bug; acceptance still needs a threshold justified by independent
operator/state checks and token-ranking behavior.

A full eight-step, 129,280-logit comparison of those existing files
quantifies where compiler variation stops being a sufficient explanation.
All three sequences were greedy replays with the same eight input token
IDs. The ratio below divides PTO-versus-fast-math DS4 relative L2 by
strict-versus-fast-math DS4 relative L2:

| Step | DS4 strict/fast relative L2 | PTO/fast relative L2 | Ratio | PTO values outside DS4 strict/fast interval |
| ---: | ---: | ---: | ---: | ---: |
| 0 | `0.005979` | `0.003837` | `0.642` | 68,348/129,280 |
| 1 | `0.016608` | `0.033570` | `2.021` | 114,168/129,280 |
| 2 | `0.052739` | `0.072947` | `1.383` | 98,932/129,280 |
| 3 | `0.059339` | `0.054313` | `0.915` | 79,949/129,280 |
| 4 | `0.082405` | `0.109596` | `1.330` | 88,101/129,280 |
| 5 | `0.069086` | `0.091544` | `1.325` | 88,305/129,280 |
| 6 | `0.065724` | `0.095986` | `1.460` | 101,730/129,280 |
| 7 | `0.090513` | `0.107647` | `1.189` | 93,399/129,280 |

The DS4 interval is a two-build diagnostic, not a correctness envelope.
At step 1, PTO differs by about twice the DS4 compiler-variant distance,
and most vocabulary values fall outside the two DS4 outputs. The top
one and runner-up IDs remain the same in all three replays, but that
ranking agreement cannot dismiss the broader discrepancy. The state
substitution below narrows the next investigation to the step-1 compute
path; do not assign a numerical acceptance threshold yet.

A two-step causal probe on `n4-910c` then replaced only the 43 serialized
step-0 raw-KV rows in PTO's next-state buffer with DS4's corresponding
rows before token `201`. It used the production PTO header and the same
GGUF index. The diagnostic DS4 step-0 logits matched the earlier DS4
reference byte-for-byte; the PTO step-0 logits matched the existing PTO
baseline byte-for-byte (SHA-256
`57f38f2c2e2d6c6743e3214666384f978c136611344333815306fb342e9bb4ec`).
Against DS4, the full 129,280-logit step-1 relative L2 error rose from
`0.033569570` to `0.035850543` (about 6.8%); the top token remained
`41`. This intervention changes the result, but correcting raw KV alone
does not remove the step-1 drift or identify the first responsible operator.
The diagnostic source and outputs remain in ignored
`out/w6-fma-candidate/` on the test farm; no production code changed.

A follow-up DS4 one-step CPU probe exported every layer's step-0 raw KV
and, for compressed layers, attention/indexer compressor KV and scores.
Its full step-0 logits had the same SHA-256 as the earlier independent
DS4 reference (`830522ba1ea5b561c9a35258f78b7b4f7fe6dd6999afa9470ed9d1b7081f1dc8`).
The production-header PTO diagnostic then replaced all these nonempty
serialized state fields before step 1; step-0 PTO logits again matched
the deployed baseline byte-for-byte. The all-state-injected step-1
129,280-value logits file had the same SHA-256 as the earlier raw-KV-only
injection (`f477f177508919b3bb3b842f96d2a9c65803440f90586d2a7043cd56083abcb7`),
with relative L2 `0.035850543` against DS4 and top token `41`. The
compressor fields therefore have no observed effect at step 1, before
their first emitted compressed row. Correcting the full saved state
still does not reduce the step-1 drift; investigate current-token
arithmetic and the attention/Q8 boundary before more state changes.
Only ignored diagnostic files were modified on the test farm.

A target-local DS4 CPU probe then exported layer-2 ratio-4 compressor
state after steps 6 and 7, using the same eight-token greedy sequence.
Its step-7 full logits matched the earlier DS4 reference byte-for-byte.
The committed-order PTO probe's seven-token state and existing eight-token
state were parsed with validated header, layer and tensor dimensions.
At step 6, before the second pool, DS4 versus PTO relative L2 errors
were `0.011901` for attention state KV, `0.003943` for attention scores,
`0.007710` for indexer state KV, and `0.004766` for indexer scores.
Their first compressed attention row already differed by `0.011534`.
After step 7, the newly emitted second attention and indexer rows
differed by `0.020515` and `0.017863`, respectively. This establishes
upstream state divergence before the second pool; it does not identify
the first responsible operator or prove pool arithmetic is bitwise equal.
The outputs remain in ignored `out/w6-range-parity/` on `n4-910c`.

A small same-input ratio-4 probe reconstructed the layer-2 step-7 pool
inputs from DS4's saved step-6 previous lane and step-7 current lane.
With those identical raw KV and score values, the production-header PTO
pool output versus a source-aligned DS4 pool formula differed by relative
L2 `8.8312e-8` for the 512-value attention lane and `7.8929e-8` for
the 128-value indexer lane; maximum absolute error was `2.98e-8` in
each. The saved DS4 compressed rows also pass through RMSNorm, RoPE and
FP8/QAT, so they are not raw-pool oracle outputs. This focused comparison
supports the corrected accumulation order and makes raw pooling a weak
candidate for the larger step-7 discrepancy. It does not establish
bitwise equality or post-pool parity. The probe source and results are
in ignored `out/w6-range-parity/` on `n4-910c`.

A controlled layer-2 projection probe used the same DS4 GGUF and verified
that both DS4 compressor KV/gate weight pairs are F16, as in the PTO
path. With DS4's exact normalized input, PTO attention KV/score output
relative L2 errors were `5.19e-7`/`1.78e-7`; indexer KV/score errors
were `3.94e-7`/`2.59e-7`. Given the same DS4 layer-1 hidden tensor,
PTO's layer-2 HC/attention normalized output differed by `1.69e-7`
and its attention KV projection by `5.49e-7`. With PTO's native layer-1
hidden instead, those errors rose to `0.003359` and `0.003231`; indexer
KV projection error was `0.002138`. The native layer-1 hidden itself
differs from DS4 by `0.004371485`. Thus the measured large layer-2
compressor-input drift follows the upstream hidden difference. The
common-input projection retains small FP32 differences and is not
bitwise equal. No production operator was changed by these probes.

A follow-up first-layer common-input test on `n4-910c` fed the exact DS4
query and KV dumps into the production-header PTO `sink_attention` path.
The 64 sink weights were byte-identical between DS4 and PTO (SHA-256
`7aa4f19ba809780629a5f9b4188221931740c2a1bf7cf5391d776bfcb16e8e4b`).
The DS4 diagnostic separately replayed sink attention and inverse RoPE;
its replayed heads matched the original first-layer trace byte-for-byte.
Against those DS4 heads, the common-input PTO heads had relative L2
`5.7962e-8`, maximum absolute error `9.5367e-7`, and 23,430 exact
values out of 32,768. The error was identical before and after inverse
RoPE at position 0. Quantizing the two heads tensors with the same PTO
Q8 operator changed 74 codes out of 32,768 and 295 scales out of 1,024.
The resulting grouped output-A relative L2 error was `4.9851e-4`.
This isolates a numerically consequential difference within the
sink-attention computation on identical Q/KV and weights. It does not
yet identify whether the first difference is in dot-product reduction,
exponentiation, denominator accumulation, or division, and it does not
prove that changing any one operation will improve full-model logits.
The probes and raw tensors remain in ignored `out/w6-range-parity/` on
`n4-910c`; product code was unchanged.

A scalar-stage follow-up replayed that same one-row attention input.
DS4 and PTO dot scores differed for 38 of 64 heads (relative L2
`1.3518e-7`, maximum absolute error `1.9073e-6`). The sink exponential
weight, KV exponential weight, denominator and inverse differed for
20, 14, 13 and 11 heads, respectively. For all 26 heads whose dot
scores were bitwise equal, every downstream scalar was bitwise equal,
as were all 13,312 values of their pre-inverse-RoPE output heads.
Thus the Q/KV dot score is the first measured divergent operation in
this common-input, single-KV-row case. The result does not establish
which PTO-compatible reduction should replace it or predict the effect
on complete logits and later multi-row attention. Diagnostic sources
and outputs remain in ignored `out/w6-range-parity/` on `n4-910c`.

A PTO `TMUL`/`TROWSUM` chunk sweep then tested widths 1 through 512 on
the same DS4 Q/KV. Replaying the width-512 scores reproduced the current
PTO heads byte-for-byte; replaying exact DS4 scores produced DS4 heads
byte-for-byte. Widths 32, 128 and 256 reduced score relative L2 from
the width-512 baseline `1.3518e-7` to `9.5690e-8`, `9.8381e-8` and
`9.2045e-8`, respectively. They increased Q8 code differences from 74
to 157, 125 and 183. Lower score error alone is therefore an invalid
selection rule for this quantized boundary; none of these three chunk
variants is a validated production correction.

On the AArch64 test bed, DS4's `dot_f32` uses two four-lane NEON fused
multiply-add accumulators. A common-Q/KV diagnostic reproduced all 64
DS4 sink-attention scores bitwise with that exact order. Keeping the
same two-accumulator reduction but separating multiply and add matched
39/64 scores; the deployed PTO `TMUL`/`TROWSUM` path matched 26/64.
Both fusion and reduction order therefore contribute to the local score
difference. An isolated, diagnostic-only full-model probe substituted
the DS4 NEON dot in sink attention while leaving the remaining PTO
operators unchanged. With token `42`, its first-layer hidden relative
L2 error against DS4 fell from `0.004371485` to `0.004326369`; its
complete 129,280-value step-0 logits error fell from `0.003836918` to
`0.003591703`. The top token remained `201`. This is a modest local
improvement, not full-model parity or a PTO-compatible production fix.
The same probe's first-layer serialized raw KV row matched the DS4 KV
row bitwise in all 512 values. The production-flag PTO query dump
matched the DS4 query in only 3,247 of 32,768 values (relative L2
`2.561124e-7`). For this token and layer, the divergent attention
input is Q; the raw KV input is exact. Trace Q normalization,
projections, head norm and RoPE before changing another dot operator.

A same-input Q-stage replay reproduced DS4's original 32,768-value Q
bitwise. Feeding its exact attention-normalized vector into the deployed
PTO query path reduced Q relative L2 from `2.561124e-7` to
`1.434267e-7`, but still matched only 6,012/32,768 values bitwise.
The first difference was `attn_q_a`: 292/1,024 exact values. DS4 and
PTO quantized that same input to identical 4,096 Q8 codes and 128
scales (matching SHA-256 for each stream). The AArch64 test compiler
did not define `__ARM_FEATURE_DOTPROD`, so DS4's Q8 matvec used its
scalar-block accumulation branch. An isolated PTO `TAXPY` accumulator
matched DS4 `attn_q_a` 1,024/1,024 bitwise. Its next LoRA RMSNorm
matched only 264/1,024 values, and final Q matched 5,927/32,768.

Global substitution of that `TAXPY` accumulator across Q8 matvecs is
rejected by the full-model check. On the same four-token GGUF replay,
the top IDs remained `[201, 41, 201, 35]`, but logits relative L2
against DS4 changed as follows:

| Step | Deployed PTO | Global `TAXPY` diagnostic |
| ---: | ---: | ---: |
| 0 | `0.003836918` | `0.003308125` |
| 1 | `0.033569570` | `0.038141183` |
| 2 | `0.072947333` | `0.078911147` |
| 3 | `0.054312597` | `0.082762942` |

The first-layer hidden error improved from `0.004371485` to
`0.003921277`, but three of four complete-logits steps regressed.
These are same-GGUF diagnostics in ignored `out/w6-fma-candidate/` on
`n4-910c`; no production operator was changed.

A second isolated build enabled `TAXPY` only for `attn_q_a` and kept the
other Q8 matvecs on the deployed accumulator. With the same DS4-normalized
input and production compiler flags, its `attn_q_a` output matched DS4 in
362/1,024 values, with relative L2 error `8.872752e-8`. The earlier
all-Q8 `TAXPY` build matched 1,024/1,024; the two diagnostic binaries also
matched each other in only 362/1,024 values at this stage. Thus the earlier
exact result depends on the compiled surrounding code, even at the first
projection. The scoped build then completed one 43-layer step with token
`42` and selected token `201`. Against the same DS4 GGUF reference, its
complete 129,280-value logits relative L2 error was `0.009149992`, worse
than the deployed PTO path's `0.003836918`. The scoped candidate is
rejected without a four-step run or production change. The next candidate
must make its arithmetic semantics explicit and improve complete logits
before multi-step validation.

A separate first-layer intervention replayed the same PTO projection
and FFN with substituted attention heads. Re-injecting PTO's own heads
reproduced its original final hidden byte-for-byte. Relative to DS4's
16,384-value layer-1 hidden, the baseline PTO error was `0.004371485`;
using heads computed from exact DS4 Q/KV with PTO attention gave
`0.003897843`, and substituting exact DS4 heads gave `0.001756476`.
This establishes substantial downstream sensitivity to the heads
boundary. The latter intervention also removes upstream Q/KV error, so
the result does not assign the entire improvement to dot reduction.
It is a one-token, one-layer causal probe, not a full-logits result.

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
On 2026-09-26, the isolated `8be893ca` checkout on `n4-910c` plus the
uncommitted guest lease-release fix passed a separate 2-node/8-step
DeepSeek run, `2026-09-26_11-15-22_w5_deepseek_v4_flash_decode_31268`.
Both nodes completed 8/8 ranges, and the terminal IDs were
`[201, 41, 201, 35, 201, 35, 201, 35]`. Each node had eight callable-5
host records with positive reads and writes,
`shared_payload_staging_bytes=0`, and `status=pass`. The process exited 0;
no QEMU process remained. The temporary patch was reversed afterward and
the validation checkout was clean. This is one regression pass for the
lease fix; it does not establish full-logits parity or a repeat-run gate.
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

`sim-console prepare-target n4-910c1` materialized the exact committed
submodules for isolated source revision `8be893ca` and reported 19 ready
demos with zero readiness blockers. The pre-run check found 28 QEMU
processes from older runs. A sampled process was parented by PID 1;
their ownership was not established, so none was stopped. With 320 CPUs
and about 1.8 TiB of available memory, the host had capacity for a
separate two-node gate. Run `sim-console-1790302387279-0` then passed
DeepSeek 2-node/4-step on that revision: 4/4 steps, 2/2 passed nodes,
terminal IDs `[201, 41, 201, 35]` matching the same-GGUF DS4 greedy
reference. All eight callable-5 host access records had positive reads
and writes, `shared_payload_staging_bytes=0`, and `status=pass`.
The post-run process list still contained the same 28 older QEMU
processes and no process from this run. This closes one second-host
functional gate. The remaining second-host topology/step matrix,
repeat-run stability, and full-logits parity stay open.

The same isolated checkout passed DeepSeek 3-node/4-step on `n4-910c1`
in run `sim-console-1790304017814-0`, exit code 0. All three nodes
completed 4/4 ranges, with layer splits `[0,15)`, `[15,29)`, and
`[29,43)`. Terminal IDs `[201, 41, 201, 35]` matched the independent
same-GGUF DS4 greedy reference. All 12 callable-5 host access records
had positive reads and writes, `shared_payload_staging_bytes=0`, and
`status=pass`. The post-run process list still had the original 28 QEMU
processes and none from this run. This closes the second-host
three-node four-step functional gate; eight-step and eight-node gates
remain open there.

The same revision passed DeepSeek 2-node/8-step on `n4-910c1` in
`sim-console-1790305607975-0`, exit code 0. Both nodes completed all
eight ranges. Terminal IDs `[201, 41, 201, 35, 201, 35, 201, 35]`
matched the independent same-GGUF DS4 greedy reference, including
step 7. All 16 callable-5 host access records had positive reads and
writes, `shared_payload_staging_bytes=0`, and `status=pass`. A post-run
process check found no process with this run ID; the 28 pre-existing
QEMU processes remained. This closes the second-host two-node eight-step
functional gate, but does not establish full-logits or compressed-state
parity. At this point, the second-host 3-node/8-step and 8-node/4/8-step
gates remained.

The same revision passed DeepSeek 8-node/4-step on `n4-910c1` in
`sim-console-1790308262561-0`, exit code 0. The SSH launcher stream
disconnected near the end; the first status query timed out during SSH
banner exchange, and sim-console recovered the target's `passed` state.
All eight fetched guest logs contain four range-complete records, and
nodeH published terminal IDs `[201, 41, 201, 35]`, matching the same-GGUF
DS4 greedy reference. All 32 callable-5 host access records had positive
reads and writes, `shared_payload_staging_bytes=0`, and `status=pass`.
No process with this run ID remained, and the host still had the 28
pre-existing QEMU processes. This is a real SSH-disconnect recovery case;
the target result and fetched logs establish the functional gate despite
the interrupted launcher output. It leaves full-logits parity and the
second-host 3-node/8-step and 8-node/8-step gates open.

The same isolated checkout passed DeepSeek 3-node/8-step on `n4-910c1`
in run `sim-console-1790312839485-0`, exit code 0. All three nodes
completed eight ranges. Terminal IDs
`[201, 41, 201, 35, 201, 35, 201, 35]` matched the same-GGUF DS4
greedy reference, including selection after the second ratio-4 pool.
All 24 callable-5 host access records had positive reads and writes,
`shared_payload_staging_bytes=0`, and `status=pass`. This closes the
second-host three-node eight-step functional gate. It does not establish
compressed-state or full-logits parity; the second-host eight-node
eight-step gate remained open at that point.

The same isolated checkout passed DeepSeek 8-node/8-step on `n4-910c1`
in run `sim-console-1790315528053-0`, exit code 0. All eight nodes
completed eight ranges. Terminal IDs
`[201, 41, 201, 35, 201, 35, 201, 35]` matched the same-GGUF DS4
greedy reference. All 64 callable-5 host access records reported
positive reads and writes, `shared_payload_staging_bytes=0`, and
`status=pass`. This completes the recorded second-host 2/3/8-node,
4/8-step DeepSeek functional matrix. It does not establish full-logits
or compressed-state parity, same-revision repeatability, or official
checkpoint acceptance.

Existing guest logs now show the same 129,280-value full-vocabulary
logits checksum sequence for all six DeepSeek 2/3/8-node, eight-step
runs on the two test-farm hosts:

| Target | 2 nodes | 3 nodes | 8 nodes |
| --- | --- | --- | --- |
| `n4-910c` | `sim-console-1790265385881-0` | `sim-console-1790294940097-0` | `sim-console-1790297485314-0` |
| `n4-910c1` | `sim-console-1790305607975-0` | `sim-console-1790312839485-0` | `sim-console-1790315528053-0` |

The first `n4-910c` two-node run used committed source `10cd07c9`;
the other five runs used the `8be893ca` validation checkout. The
per-step guest checksums match across those recorded runs despite the
different range partitions. Recomputing the guest's indexed FNV
checksum over the independent PTO probe's complete
`pto-neworder-42-8.step*.logits.f32` files matched all eight values:
`f84d1dee3e293398`, `c0eb22f0d2a2534c`, `de41ab355e5d42ce`,
`96cab884bc370e72`, `b5bbcdb5bef8a9f9`, `1a4d2c114aa3ec87`,
`0d4e691272974df0`, and `a5c5ae4d07196bc4`. This establishes
matching complete-vector guest and PTO probe checksums for the six
eight-step runs, including the second ratio-4 pool at step 7. The
probe's recorded step-7 relative L2 logits error against the independent
DS4 same-GGUF reference is `0.107647`; DS4 full-logits parity remains
open. Matching non-cryptographic checksums is strong output evidence,
not a mathematical proof of bytewise identity. These runs do not
establish repeated-run stability for every topology or compressed-state
parity.

An additional input-diversity gate used the clean `8be893ca` checkout
on `n4-910c` with an ignored env-file copy setting the raw prompt token
to `43` instead of `42`. The stable config entry validated with the
target-local GGUF and openEuler disk paths. Run
`2026-09-25_22-05-15_w5_deepseek_v4_flash_decode_6105` generated a
summary of 1/1 step and 2/2 passed nodes, selecting terminal token
`42498` with runner-up `6288`. Both guest logs report one prompt token
seeded from the guest environment and zero live/quarantined objects at
cleanup. Host access records are `callable=5`, reads/writes `23/3164`
and `26/132487`, `shared_payload_staging_bytes=0`, `status=pass`.
There were no QEMU or launcher processes left after the run. The guest
logs do not print the raw prompt ID, so the env-file value is the
available input identity evidence. The remote summary proves run
completion; the local SSH client was interrupted after it stayed open
with no remote launcher, so its exit code does not prove launcher
success.

The same clean `8be893ca` checkout on `n4-910c` repeated that raw-token-43
2-node/1-step gate as
`2026-09-26_09-35-57_w5_deepseek_v4_flash_decode_10759`. This run exited
0 with 2/2 nodes and 1/1 step passed. It again selected token `42498`
with runner-up `6288`, and the complete-vocabulary logits checksum was
`0xd364ebb350229280`, matching the first run and the independent PTO
probe. Both guest operations logged callable 5, `UB_GM` and zero shared
staging; host access records were `23/3164` and `26/132487` reads/writes,
also with zero staging. No QEMU process remained. This establishes a
same-revision repeat for this single input and topology; it does not
establish repeatability of the full topology/step matrix or DS4 logits
parity.

The existing DS4 CPU probe, whose binary metadata identifies GCC
10.3.1, then evaluated the same GGUF and raw token `43` for one step.
Its complete 129,280-value logits selected token `42498` at
`16.3442612`. DS4's top four IDs were `[42498, 6288, 11060, 90998]`;
the guest observation's top four were `[42498, 6288, 11060, 666]`.
The guest's first three logits were `16.2899132`, `15.2698441` and
`15.1032295`, versus DS4's `16.3442612`, `15.3210135` and
`15.1273947`. Thus the selected token and top-three ordering agree
for this additional input, while the fourth-ranked ID and values
already differ. The existing PTO range probe then
executed all 43 layers for raw token `43` using a weight index that
embeds the same target-local GGUF path. Its 517,120-byte logits output
has the guest's indexed FNV checksum `0xd364ebb350229280` exactly,
and its top-four values match the guest observation. Compared with the
GCC 10.3.1 DS4 complete logits, the PTO probe's relative L2 error is
`0.018359898`, maximum absolute error `0.63835907`, and
`124369/129280` values exceed the diagnostic
`1e-4 + 1e-3 * abs(reference)` tolerance. This tolerance is not a
W6 acceptance limit. The matching 64-bit checksum strongly links the
probe vector to the guest output but is not a mathematical proof of
bytewise identity; the guest did not persist its raw logits array.
The derived DS4 and PTO logits remain in ignored `out/` on the test
farm, with ignored local comparison copies. No model weights were
transferred.

On 2026-09-26, a 2-node/1-step managed-direct guest run on `n4-910c`
(`2026-09-26_18-19-11_w5_deepseek_v4_flash_decode_19223`) persisted its
terminal raw logits for prompt token IDs `42,5207,24063,344`. The file has
517,120 bytes, all 129,280 FP32 values are finite, and its indexed FNV
checksum `0x14ff6fa749ac4d37` exactly matches the guest's independent
`model_terminal_logits_observation` checksum. Both select token `260`; the
top four IDs are `260,270,13792,411`. Both nodes completed their ranges,
and nodeB's host log reports `callable=5`, `status=pass`, and
`shared_payload_staging_bytes=0`. An initial diagnostic attempt failed with
`pto_ub_gm_access_denied` because it tried to `TLOAD` the write-only managed
logits binding. The corrected diagnostic mirrors output-head scalars into a
private buffer using PTO `TSTORE`, then writes that buffer after the range.
The successful run verifies guest raw-logits capture and managed-direct
completion; it does not compare these logits with a same-prompt DS4 vector.
The matching DeepSeek range test passed 6/6 on `n4-910c`, and the local
range/ops/state tests passed 8/8. A broader target `unittest discover` run
reported 701 tests with 6 failures and 7 errors while only part of this
working tree was staged there; it is not a clean full-suite gate. The target
checkout was restored after validation, with no QEMU process left running.

The same four token IDs were then evaluated sequentially by an independent
DS4 CPU session against the same target-local IQ2XXS GGUF. The reference
`ds4.c` SHA-256 was
`6be99cdae47b9bdf241cde9619f5674545a21e10e32540f03c68442fae7912cd`,
matching local DS4 revision `931b98e`. The DS4 reference and managed-direct
guest both ranked tokens `[260,270,13792,411]` highest. Their complete
129,280-value logits were not byte-identical: relative L2 error
`0.03519603275`, maximum absolute error `1.33355999`, and
`126581/129280` values outside the diagnostic
`1e-4 + 1e-3 * abs(DS4)` threshold. The DS4 and guest SHA-256 values were
`bd2c3fe40a1b3ec77a447fc63372c19a51e453117ed2c7c039b09f42ebe74076`
and
`310a85bae8a7c9a150dacccc4f2b1c799cf09ca4ecd35dcacd98c0ff7c2ca05b`,
respectively. The DS4 reference vector and comparison copy of the guest
vector remain in ignored `out/w6-range-parity/`; no model weights moved.
The threshold is diagnostic only and has not been accepted as a W6 limit.
The DS4 public-session diagnostic was cross-checked on raw token `42`:
its complete logits matched the pre-existing
`ds4-greedy-42-4.step0.logits.f32` reference byte-for-byte (`cmp` exit 0),
and both selected token `201` with logit `19.0194111`.

A controlled compiler comparison on `n4-910c` rebuilt the unchanged DS4
public-session oracle from the same `ds4.c` and companion sources with
identical `-O3 -ffast-math -fno-finite-math-only -march=native` flags. Only
the compiler changed: system GCC 10 versus GCC 15. Both evaluated the
same IQ2XXS GGUF and token IDs `42,5207,24063,344`. The new GCC 10
logits matched the earlier DS4 reference byte-for-byte. GCC 15 still
selected token `260`, but its top-four IDs were `[260,270,13792,834]`
instead of GCC 10 and PTO's `[260,270,13792,411]`. Across all 129,280
logits, relative L2 was `0.061274788` for GCC 10 versus GCC 15,
`0.035196033` for GCC 10 versus the managed-direct guest, and
`0.065981722` for GCC 15 versus that same guest. At token `260`, the
three logits were `23.535549`, `23.786224`, and `23.589329`, respectively.
The PTO value lay outside the interval formed by the two DS4 builds for
81,410 of 129,280 entries. This compiler-controlled result makes a
single-build, near-bitwise DS4 threshold unsuitable as an unqualified
cross-compiler acceptance criterion. It also rules out compiler choice
as a complete explanation for the PTO discrepancy. Preserve the exact
reference build identity in future comparisons, continue tracing the
model's first divergent operators, and justify any eventual tolerance
against independent model behavior rather than this compiler range alone.
The generated vectors remain in ignored `out/`; no weights were moved.
An eight-step, same-sequence comparison then held the IQ2XXS GGUF and
input IDs `[42,201,41,201,35,201,35,201]` fixed. The independent DS4
core with `ds4.c` SHA-256 `6be99cdae47b9bdf241cde9619f5674545a21e10e32540f03c68442fae7912cd`
was compiled with GCC 15.3 and
`-O3 -ffast-math -fno-finite-math-only -march=native`; its existing
non-model helper objects were reused. One CPU session saved all eight
129,280-value logits arrays. The GCC 10.3 DS4 and deployed PTO arrays
were the previously recorded fixed-sequence outputs. KL below is
`KL(DS4 GCC 10.3 || other)` after whole-vocabulary softmax; TV is total
variation distance between those distributions. All three runs selected
the same top token at every step.

| Step | DS4 GCC10/15 KL | DS4 GCC10/PTO KL | DS4 GCC10/15 TV | DS4 GCC10/PTO TV |
| ---: | ---: | ---: | ---: | ---: |
| 0 | `0.00005` | `0.00002` | `0.00365` | `0.00162` |
| 1 | `0.00837` | `0.00400` | `0.05080` | `0.03504` |
| 2 | `0.00352` | `0.03015` | `0.02086` | `0.12029` |
| 3 | `0.01295` | `0.00687` | `0.06071` | `0.04638` |
| 4 | `0.00014` | `0.00013` | `0.00030` | `0.00036` |
| 5 | `0.01142` | `0.01511` | `0.05930` | `0.07202` |
| 6 | `0.00093` | `0.00141` | `0.00192` | `0.00295` |
| 7 | `0.02624` | `0.03055` | `0.08729` | `0.10106` |

The step-2 PTO distribution error is several times the DS4 compiler
variation on the same input and cannot be waived as compiler noise.
Steps 5 and 7 also exceed that comparison, although by less. Raw-logits
relative L2 alone exaggerates some confident-token steps: at step 4,
PTO/GCC10 raw-logits relative L2 is `0.10960` while TV is `0.00036`.
The next numerical investigation should use the actual position-2 Q/KV
state and find the first operator or Q8 activation block whose output
diverges from both DS4 builds. Preserve the eight-step top-token gate
and whole-vocabulary distribution comparison; do not declare acceptance
from top-token agreement or a global tolerance inferred from another step.
The GCC 15 vectors and one-session diagnostic source are in ignored
`out/w6-range-parity/`; no product or DS4 source was changed.

On 2026-09-27, a syntax-only build of the current DeepSeek PTO kernel
with the target's GCC 10.3.1 failed in the pinned PTO headers because
that compiler does not recognize the required `_Float16` type. GCC 15.3
is the supported PTO build on this test bed. A same-GCC-10 DS4/PTO
comparison is therefore unavailable without changing the toolchain or
PTO implementation; no numerical conclusion follows from the failed
build.

To isolate pipeline effects on the four-token prompt, the existing
corrected-order standalone PTO range probe evaluated all 43 layers from
the same GGUF index and token IDs. It selected token `260` with logit
`23.5893`. Its complete raw logits file was byte-identical (`cmp` exit 0)
to the 2-node managed-direct guest trace above. For this input, the
cross-node handoff and managed tensor mapping introduce no additional
logits difference; the measured DS4 discrepancy is reproducible in the
standalone PTO model range. This does not prove every topology or later
decode step is numerically identical to the standalone probe.

An existing first-layer trace for raw token `43` compared the same
DS4 GCC 10.3.1 and PTO inputs at successive model stages. Relative
L2 errors were `1.7254e-7` for Q, `8.8677e-8` for attention heads,
`0.00114175` for grouped Q8 output-A, `0.00262137` for the raw
attention output, `0.00206771` after attention HC, and `0.00490121`
at the end of layer 1. Quantizing the two heads tensors with the
existing Q8 diff probe changed 216 of 32,768 codes and 449 of 1,024
block scales; the first changed code was block 24, lane 10, from 64
to 63. This reproduces the early attention-to-Q8 amplification seen
with token `42` on an independent input. It does not assign all
43-layer logits drift to this one boundary or validate a production
arithmetic change.

A common-input intervention then fed the exact DS4 token-`43`
first-layer heads to the existing PTO grouped output-A operator.
Relative to DS4 output-A, the original PTO heads produced `0.00114175`
relative L2 and `0.00410748` maximum absolute error; the DS4-heads
input produced `1.5717e-7` and `5.9605e-7`, respectively. The local
relative error fell by about 7,260 times. This supports grouped Q8
projection parity on a common input and places the large local
amplification at its quantized input boundary. It does not by itself
prove full-model parity or select a PTO-compatible attention correction.

For every case, compare terminal tokens and logits with the corresponding
same-format model reference, match guest request IDs to host access counts,
and confirm zero publication copy bytes. Record the exact run ID and source
revision. The deployed GGUF path has four- and eight-step references, but
full-logits guest-to-DS4 parity remains open. The eight-step guest
full-logits checksum comparison to the PTO probe passes for all 2/3/8-node
topologies on both hosts. The official
Safetensors reference covers a different weight format.

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

The current functional validation scope temporarily defers item 8 by user
decision. Passing items 1-7 and 9 under that exception can establish a
functional W6 direct path; it cannot satisfy the full acceptance condition
above or establish official-checkpoint numerical parity.

## 6. Next Functional Gates

Execution order confirmed on 2026-09-30: first reproduce the local
checkpoint from a clean checkout, then validate managed compressed history.
The clean-checkout run must pin every required submodule and match the
Memory Service lock. Use a separate target directory with fresh QEMU,
guest and PTO build outputs; existing Git objects, model weights and the
openEuler base image may be reused as explicitly recorded inputs.
Run Qwen and DeepSeek 3-node/4-step through the normal config entry and
check direct I/O, zero shared staging, cleanup and source cleanliness.
Transfer unpublished commits by offline bundles; no push is authorized.

### Clean-Checkout Reproduction: Passed

On `n4-910c1`, the separate checkout
`/home/ll/w6-clean-20260930/ub_sim` pins root commit
`32a0c7d97a5cf76620fc721ff46cd4f7daf4c0dd`. All eight top-level
submodules and `mem_service/vendor/obmm` match their gitlinks; the Memory
Service lock matches `546985e`. Existing Git objects, target-local model
weights, the openEuler base image, toolchains and dependency caches were
reused. Simulator, QEMU, kernel, guest/initramfs, BusyBox and PTO build
outputs were generated in this fresh checkout. No old build outputs were
copied. Unused optional nested submodules were not initialized.

Both cases used `run_w5_cluster_config.sh --nodes 3 --steps 4` with the
recorded config files in this checkout's ignored `out/` directory:

| Model | Run ID | Range completions | Output token IDs |
| --- | --- | ---: | --- |
| Qwen3-0.6B | `2026-09-30_14-19-08_w5_qwen3_0_6b_decode_28390` | 12/12 | `[264,3644,7653,304]` |
| DeepSeek V4 Flash IQ2 GGUF | `2026-09-30_14-28-03_w5_deepseek_v4_flash_decode_31756` | 12/12 | `[201,41,201,35]` |

Every range reported its expected callable (Qwen 3, DeepSeek 5), positive
managed reads and writes, `shared_payload_staging_bytes=0`, and
`status=pass`. Each node recorded 24 `QEMU_UB_GM_GSVA_BIND` events per run.
Each node's final cleanup reported zero live objects, backing/address bytes,
export/import mappings, live references, in-flight operations and
quarantined objects/bytes, with `status=ok`. Both launches exited 0. The
root and initialized submodule sources remained clean after both runs;
no run-owned QEMU remained. Other users' pre-existing processes were left
untouched.

Evidence is preserved in the target parent directory as `checkout.json`,
`qwen-clean-run.log`, `deepseek-clean-run.log`, `qwen-evidence.json` and
`deepseek-evidence.json`; per-node logs are under
`guest-linux/aarch64/logs/<run-id>_headless8/`. The checkout and evidence
checkers are task-local helpers outside production source. This verifies
fresh-build reproduction of the local checkpoint using offline bundles;
publishing the child pins and a GitHub-only clone remain separate work.

### Real-Model History Input: Generated

The test-only CLI `tests/deepseek_v4_flash_history_reference.c` links against
the unchanged DS4 public CPU API. Its interface is:

```text
history-reference MODEL POSITIONS INITIAL_TOKEN PAYLOAD TRACE
```

Each position calls `ds4_session_eval`, checks the session position, reads
the complete logits and selects the next token by argmax. The trace records
every input/next-token pair. The public `ds4_session_save_payload` API writes
the final complete native checkpoint. The helper rejects nonfinite logits,
never overwrites an existing output, and removes the checkpoint on a
reported evaluation or save failure. Its four model-free CLI unit tests use
stubs solely to test error handling and token chaining; those stubs do not
produce the integration checkpoint.

On `n4-910c`, the real reference used the existing
`DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf`
asset, initial raw token `42`, a 2052-position context and 12 CPU threads.
The DS4 source SHA-256 is
`6be99cdae47b9bdf241cde9619f5674545a21e10e32540f03c68442fae7912cd`.
The helper and all linked DS4 object digests are preserved in the build
receipt beside the checkpoint. The DS4 CPU object identifies GCC 10.3.1.
DS4 source and W6 production execution were not modified.

The reference executed all 43 layers at each of 2051 positions, exiting 0
after 1030.414 seconds. Its native payload is 52,184,472 bytes. Verification
checked all 2051 trace records against the payload's token sequence, chained
every next token to the following input, checked the final logits argmax,
validated every layer's state geometry, and required exact end-of-file.
All layers retain 128 live raw rows. The 21 ratio-4 layers each contain
512 compressed rows and 512 indexer rows; the 20 ratio-128 layers each
contain 16 compressed rows. Compressor buffers are included. The next
input token is `201`.

Payload SHA-256:
`06fe1e0365b8b0447ad07dc3a66ae1a6fa95cb568e23eae7b6984804825b870a`.
Trace SHA-256:
`03c0a33b22b9ab4814b7808d175b7de3f6a670081ef7c5dc9b53f817cb07e84e`.
Evidence lives under `n4-910c:/home/ll/sim-console/ub_sim/out/w6-range-parity/`:
`real-history-2051.payload`, `.jsonl`, `.log`,
`real-history-2051-evidence.json`, `real-history-2051-build.sha256` and
`real-history-2051-compiler.txt`. The task-local checker is
`w6_verify_real_history.py`. No weights or full checkpoint were transferred
to the development machine.

This completes authentic history generation only. The checkpoint is in DS4's
native session format; it cannot be passed directly as W6 range-state bytes.
Next, preserve and validate its complete state when converting to the
existing range encoding, publish it through the real Memory Service, and
run callable 5 at position 2051. That continuation must produce the 513th
ratio-4 row, exercise the PTO top-k branch, report positive managed reads
and writes with zero shared staging, and drain its objects and leases.
These continuation checks remain open. The DS4 reference remains a test
input producer; W6 continues to execute through its PTO model implementation.

The full guest suite passed 714/714 tests in 513.194 seconds on the separate
`n4-910c1` checkout after adding the four CLI tests. Its log is
`out/real-history-guest-tests-configured.log`. The offline Rust workspace
suite exited 0; its log is `out/real-history-cargo-tests.log`. An initial
guest-suite invocation omitted the build environment and failed with
missing Cargo, system-GCC PTO compilation and missing sanitizer libraries.
The rerun loaded the existing GCC 15/Cargo paths and sanitizer runtime from
`out/w6-history-tests.env`; no test was skipped or weakened. These are
regression results, independent of the still-open managed-history gate.

### Remaining Functional Work

1. The compressed-history and `top_k=512` limits are now separate. Validate
   a managed model range with at least 513 ratio-4 compressed rows so the
   score-and-select branch is exercised through the guest/service boundary;
   check state/lease budget and zero shared tensor staging there. The C/C++
   513-row goldens and the real-weight synthetic-state PTO range probe above
   do not prove a serving-produced history or the managed service path.
   The user explicitly rejected synthetic checkpoints on 2026-09-30:
   every preceding history position must be evaluated by the real model.
   Zero-filled history, repeated/copied KV rows, fabricated compressor
   state and a reduced threshold cannot satisfy this gate. A checkpoint
   produced by an existing real-model runtime is only useful after recording
   its exact model asset, source/build identity, input token sequence,
   positions and complete raw/compressed KV plus compressor state. Its
   conversion and managed continuation must be checked independently;
   generating that checkpoint alone does not close the service gate.
   The 2026-09-30 continuation replaced the CPU score scan with two-level
   PTO `TROWARGMAX`: first within each tile, then across tile winners.
   Selected rows are masked through PTO stores; first-index tie ordering
   is preserved. The five operator tests passed on `n4-910c`, including
   stable-reference comparisons for 513 rows, tile boundaries, duplicate
   scores and invalid inputs under both `-O2` and production math flags.
   The real-weight, synthetic-history probe also passed with
   `past=2051 candidates=513 topk_calls=1`, executing layers `[0,3)`.
   Its complete hidden and serialized state outputs are byte-identical
   to the previous CPU-selection probe. Logs:
   `out/w6-indexer-pto-focused.log` and `out/w6-indexer-pto-run.log` in
   the isolated `n4-910c` checkout. This closes the operator-selection
   implementation gap; it does not close the managed-history gate.
   The complete guest suite then passed 710/710 tests in 463.636 seconds;
   its log is `out/w6-indexer-pto-guest-tests.log` in the same checkout.
   `cargo test --workspace --offline` also completed with exit code 0;
   its log is `out/w6-indexer-pto-cargo-tests.log` in that checkout.
2. The two-host short-sequence functional matrix is complete. Preserve its
   per-range direct I/O, zero staging, lease and process cleanup evidence.
   The controlled 3-node DeepSeek/artifact-preparation overlap above passed;
   broader concurrency and generated-artifact retention remain unverified.
3. Publish tested `mem_service` and `kernel_ub` commits only when authorized,
   then update their root gitlinks and the `mem_service` lock together.
   Revalidate from a clean clone before claiming reproducibility.

Private constants and scratch are permitted by Section 1. A requirement to
execute CPU-visible quantization scales and private Q8_0/Q8_K activation
blocks exclusively through PTO would extend that scope; it is not an
additional prerequisite for the current managed shared-tensor goal.

### Deferred Precision Investigation

The position-1 first-layer Q8 flip is now measured and reproduced by a
single heads max-lane intervention. DS4's exact attention norm fed through
the current PTO query and attention path removes that flip. HC mixing is
already within `2.2190e-8` relative L2, while its attention RMSNorm
output differs by `5.5146e-7`. A verified DS4 double-accumulation
replay on PTO's token-201 HC-mixed input removes this code flip. Evaluate
any PTO-expressible numerical correction against complete step-0-through-7
logits using the fixed GCC 10.3 DS4 reference. Do not change the
production quantizer or grouped projection:
both agree closely on common input. Earlier global and attention-only
32-chunk compensated RMSNorm variants worsened full logits, so a local
code flip alone cannot justify promotion. A global double-sum
CPU-simulator variant was also insufficient at step 0: relative L2 moved
only 0.3% while maximum absolute error worsened by about 20%. Do not
repeat global reduction changes without a concrete multi-layer error
prediction and a PTO-hardware-expressible implementation.

PTO A5 `TADD` and `TMUL` reject FP64 tile operands in their type checks.
DS4's RMSNorm uses a `double` squared-sum, so direct FP64 tile replay is
not a device-executable correction. An FP32 compensated candidate must
prove its complete-model effect; local RMSNorm agreement does not suffice.

The position-0 heads comparison has 198 Q8 code differences, while the
position-1 comparison has one. Measure whether a candidate reduces both
counts and the corresponding output-A errors before paying for a full
range run. This is a diagnostic filter, not a substitute for complete
logits or the managed-direct guest gate.
The source-controlled position-0 same-Q/KV/sink replay leaves 74 code
differences, so a normalization-only candidate cannot be assumed to close
the first-token boundary. Separate the remaining attention score,
softmax-denominator and weighted-value arithmetic under that fixed input
before selecting a PTO-expressible production change.

The current PTO sink-attention score uses `TMUL` followed by `TROWSUM`.
DS4's bitwise-matching diagnostic uses two four-lane NEON fused
multiply-add accumulators. PTO `TAXPY` accepts a C++ scalar coefficient,
so feeding it a key element from a managed tile would move model payload
into host control state. PTO `TMULADDDST` can express an eight-lane
tile-by-tile fused accumulation, although its CPU simulator did not
guarantee FP32 fusion at this revision. `TGEMV` is not an
equivalent shortcut: its CPU simulator accumulates floating products in
`double`, while its device arithmetic is target-dependent. Do not promote
a CPU-only DS4 dot or a `TGEMV` simulation result as evidence of device
parity. A production numerical change must preserve direct `UB_GM` access
and pass complete-logits gates on the selected PTO target.

An isolated `n4-910c` candidate combined eight-lane `TMULADDDST`
sink scoring with FP32 `std::fma` in the PTO CPU simulator. On saved
same-input token-42 Q/KV data it matched all 64 DS4 raw scores bitwise.
The candidate then ran all 43 layers from the existing IQ2XXS GGUF
index with GCC 15.3 and `-O3 -ffast-math -fno-finite-math-only`, using
the fixed token sequence `[42,201,41,201]`. Complete-vocabulary
logits against the same GCC 10.3 DS4 reference were:

| Step | Baseline relative L2 | Candidate relative L2 | DS4 / baseline / candidate top token |
| ---: | ---: | ---: | --- |
| 0 | 0.003836918 | 0.003591703 | 201 / 201 / 201 |
| 1 | 0.033569570 | 0.035528108 | 41 / 41 / 41 |
| 2 | 0.072947333 | 0.071527423 | 201 / 201 / 201 |
| 3 | 0.054312597 | 0.047444571 | 35 / 35 / 18 |

Maximum absolute error also worsened at steps 1 through 3. The
step-3 token divergence rejects the candidate despite its step-0 score
and logits improvement. The production sink code and vendor CPU-sim
overlay were reverted; ignored diagnostic outputs remain in
`out/w6-fma-probe/` on `n4-910c`. A correct local dot score is therefore
insufficient to establish multi-step model parity. Continue from the
earliest full-attention/Q8 boundary with complete-logits and token gates.

A bounded multi-row check then parsed layer-0's four real raw-KV rows
from the saved PTO four-step state. It combined those rows with saved
DS4 token-42 or token-201 Q and the same learned sink values. The PTO
path and a DS4-order downstream replay received identical PTO dot
scores; the replay used sequential softmax-denominator accumulation
and FP32 fused weighted-value accumulation. GCC 15.3 used the deployed
DeepSeek arithmetic flags. Results were:

| Saved Q | Different denominators | Heads relative L2 | Q8 code differences | Q8 scale differences |
| --- | ---: | ---: | ---: | ---: |
| token 42 | 14/64 | `6.7484e-8` | 0/32,768 | 285/1,024 |
| token 201 | 9/64 | `5.9009e-8` | 0/32,768 | 262/1,024 |

Holding PTO's denominator fixed while changing only the weighted sum
to fused accumulation gave relative L2 `3.0558e-8` and `3.8873e-8`,
respectively, with zero Q8 code differences in both cases. The Q and
four-row KV came from saved but different positions, so this is an
arithmetic sensitivity check, not an actual step-3 state oracle. It
does not eliminate multi-row rounding effects in other states. It gives
no measured Q8-code benefit for changing the downstream accumulation
now. The ignored probe and tensors are in `out/w6-range-parity/` on
`n4-910c`; no production source was changed.

The all-state step-0 substitution did not improve step-1 logits; stop
substituting compressor state for that question. The separate step-7
compressed-row difference remains open. Keep model-functional and oracle
gates distinct, and require same-revision repeatability before closing W6.
The two-host 2-node/1-step and recorded 2/3/8-node, 4/8-step DeepSeek
functional gates have passed. The 8-node/4-step run exercised sim-console
SSH recovery against a real disconnect. Confirm same-revision repeatability
without conflating it with the recorded cross-revision matrix.
Set a justified tolerance for Q8 activation-boundary effects only after
the first model divergence is understood. Separately
compare the official-checkpoint path with the independent Safetensors CPU
reference. Keep W6 acceptance open until every condition in Section 5 has
direct evidence.
