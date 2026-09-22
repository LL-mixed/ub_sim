# W6 Managed UB_GM Model-Direct Plan

Date: 2026-09-22

Status: in implementation. The generic managed-direct dispatch layer and the
Qwen3 direct tensor contract are implemented and unit-validated. Neither Qwen
nor DeepSeek uses W6 as the serving default yet.

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

## 4. Remaining Implementation

### 4.1 Qwen managed serving integration

The current W5 managed round allocates one writer and opens managed readers for
the previous hidden and KV references. W6 must lay out token control, hidden
output, next KV and logits as disjoint views of that writer, then pass reader
and writer owners to `qwen3_managed_direct_prepare()`.

After CQ completion, the guest will publish the existing views in place. CPU
code may calculate checksums and sample terminal logits after visibility is
established. It must not copy a tensor into a second serving allocation.

This milestone also adds a stable model-direct selection to the existing W5
cluster entry. Qwen direct starts opt-in and becomes the W6 default after the
2/3/8-node matrix passes.

### 4.2 DeepSeek complete direct callable

DeepSeek currently executes complete ranges in the Rust/host runtime. Existing
Simpler-backed attention, quantized linear, router, routed expert, shared
expert and output-head implementations remain useful as semantics and oracle
code. W6 needs a PTO range graph that binds shared inputs and outputs as
`UB_GM` tensors and composes all 43 layers, including MLA/indexer and MoE.

Implementation work:

- define and freeze the DeepSeek tensor/scalar ABI;
- assign a callable ID only after that ABI is reviewed;
- implement the range orchestrator and PTO kernels;
- load weights as private immutable model data without mixing them with shared
  request tensors;
- bind hidden, KV and logits to the model-neutral managed-direct operation;
- compare range outputs and logits with the existing Rust/DS4-derived oracle;
- publish hidden, KV and terminal views in place through Memory Service.

No callable 4 execution is accepted as DeepSeek W6 evidence.

### 4.3 Serving selection and observability

The run configuration will expose one model-I/O choice with values `bulk` and
`direct`. Internal environment propagation stays limited to one functional
setting. Validation and report-only controls retain the `SIM_W5_TEST_*`
namespace.

Every direct range log must contain model, node, step, layer interval,
callable, tensor count, address space, PTO instruction path, staging bytes and
completion status. The host log must contain actual `TLOAD/TSTORE` counters for
the matching request ID.

### 4.4 Validation matrix

The required functional matrix is:

| Model | Nodes | Steps | Required comparison |
| --- | ---: | ---: | --- |
| Qwen3 | 2, 3, 8 | 4, 8 | callable 3 vs Qwen oracle |
| DeepSeek V4 Flash | 2, 3, 8 | 4, 8 | direct callable vs current oracle |

Each case runs through sim-console on both `n4-910c` and `n4-910c1` after the
focused unit and contract suites pass.

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

Wire the Qwen managed round writer/readers into
`qwen3_managed_direct_prepare()`, publish hidden/KV/logits views in place and
add a focused 2-node 1-step gate. This proves that current managed request
lifecycle and the already complete Qwen PTO range operate as one path before
the DeepSeek callable work starts.
