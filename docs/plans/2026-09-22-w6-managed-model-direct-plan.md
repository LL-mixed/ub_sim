# W6 Managed UB_GM Model-Direct Plan

Date: 2026-09-22. Last verified: 2026-09-23.

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
| 3 | 4 | `sim-console-1790162823449-0` | 12/12 ranges |
| 8 | 8 | `sim-console-1790163015394-0` | 64/64 ranges |

Each counted range used callable 3, had nonzero provider reads and writes,
reported zero shared payload staging, and completed successfully. The 8-step
run produced token IDs `[264, 3644, 7653, 304, 279, 2070, 315, 1995]`.

On `n4-910c1`, sim-console 2-node/1-step, 3-node/4-step and 8-node/8-step
cases passed as `sim-console-1790163569079-0`,
`sim-console-1790163821727-0` and `sim-console-1790163981682-0`. The last
case retained 64/64 host access records. The remaining two-host matrix is
still pending at this update.

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
The terminal token table must be staged in a path that survives this mount;
the sim-console DeepSeek gate remains failed until that correction is tested.

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
For every case, compare terminal tokens and logits with the established model
oracle, match guest request IDs to host access counts, and confirm zero
publication copy bytes. Record the exact run ID and source revision.

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
8. Terminal logits and selected tokens match the established model oracle.
9. The 2/3/8-node, 4/8-step matrix passes on both test-farm hosts.
10. W6 becomes the default only after Qwen and DeepSeek both satisfy items
    1 through 9. W5 callable 4 remains explicitly selectable for comparison.

## 6. Immediate Next Change

Move the openEuler direct token table to a guest path that survives boot,
rerun the DeepSeek sim-console gate, then continue the two-host matrix and
oracle comparisons. Keep W6 acceptance open until every condition in
Section 5 has direct evidence.
