# Lingqu shmem PTO guest adaptor

This directory contains the guest-side, simulator-facing adaptor for PTO
dispatches whose tensor payload remains in `lingqu_shmem`.

- `lingqu_shmem_pto_guest.h` defines the bounded metadata input model.
- `lingqu_shmem_pto_guest.c` materializes the frozen tag-10 ABI v2 wire
  objects and computes their metadata CRC.
- `lingqu_shmem_pto_endpoint.h` exposes one opaque UBC CMDQ/CQ endpoint.
- `lingqu_shmem_pto_endpoint.c` owns queue pages, submits one tag-10 slot at a
  time, and decodes the existing completion prefix.
- `lingqu_shmem.h` is the public opaque region/memref programming model.
- `lingqu_shmem_sim.h` is the simulator-only region attachment seam.
- `lingqu_shmem_pto.h` converts public memrefs into one in-flight dispatch and
  keeps each region alive until completion.

The metadata builder and endpoint never copy tensor payload bytes. They do not
expose QEMU, SIM_DEC, GVA, or GSVA identities through the public
`lingqu_shmem_memref` programming model. OBMM registration and public memref
lifetime ownership belong to the dedicated guest adaptor and workload layer.
The current lifetime API is single-threaded; concurrent create/destroy and
multiple in-flight dispatches remain part of the P4 concurrency gate. The
caller must keep the underlying mapping and OBMM registration active until
all in-flight objects have been finished and every memref has been destroyed.

The P3 two-node acceptance workload contract is documented under
`apps/lingqu_shmem_pto_direct/`. Its implementation consumes the public memref
API and keeps simulator identities inside `lingqu_shmem_sim_region_desc`.

## Memory Service compute adapter

`lingqu_shmem_mem_service.h` is the model-facing bridge from a resolved Memory
Service object view to a dispatch-bound `lingqu_shmem_memref`. Callers provide
only the object view and tensor geometry. They do not select a transport or
interpret an OBMM mapping identifier.

The implementation is split into two translation units:

- `lingqu_shmem_mem_service.c` owns provider-neutral validation, memref
  construction, lease accounting, and release ordering;
- `lingqu_shmem_mem_service_obmm.c` owns the QEMU guest OBMM mapping
  registration used by the current eight-node deployment.

Every acquired memref has one lease. The lease pins the provider mapping until
the caller has observed PTO completion and calls
`lingqu_shmem_mem_service_release()`. Context close fails with `-EBUSY` while a
lease is active. Model code keeps passing Memory Service ObjectRefs for
identity, placement, version, and commit semantics; the compute adapter only
materializes the temporary data-plane view.

Local output follows a two-address rule. `lingqu_shmem_memref` carries the
self-import alias address registered for Simpler/PTO `TSTORE`. The returned
`lingqu_shmem_mem_service_local_buffer.data` points at the original local
Memory Service OBMM payload arena. After dispatch completion, the caller may
release the alias lease and publish the original arena range in place with its
`backing_offset`; tensor payload bytes stay in the same OBMM allocation.

Qwen3 next-KV uses `lingqu_shmem_mem_service_acquire_local_kv()`. The backend
reserves the final tiered KV block span through Memory Service's model
allocator, then binds only its payload bytes as a read/write memref. Padding
remains outside the tensor view. After successful completion, the guest sets
`publish_kv_in_place` and the returned `backing_offset`; publication validates
the reservation and reports `publication_copy_bytes=0`. A backend lacking
this reservation operation returns `-EOPNOTSUPP` without a generic-allocation
fallback. The `run_w5_qwen3_pto.py` CLI exercises this path, and
`audit_w5_qwen3_pto.py` requires matching output/publication offsets and zero
publication copies for every step. Failed or pending dispatches cannot publish.

The current in-place publish contract is limited to the trusted in-process
model runtime. Memory Service checks arena bounds, length, and exact pointer
identity. A separately minted allocation token and concurrent allocation owner
check are deferred to the concurrency and recovery gate.

## Qwen3 PTO numerical operators

`qwen3_pto_ops.hpp` contains the model numerical operators compiled into
Simpler kernels. It is separate from the guest C adapter and does not manage
object placement or leases. It uses PTO instructions for numerical work and
`TLOAD/TSTORE` for GM access, including synthetic UB_GM addresses. CPU oracle
tests live in `tests/qwen3_pto_ops_golden.cpp`; generated binaries and reports
belong under `out/`. Kernel wrappers must validate the operation contract
before dispatch and preserve the region leases until completion.

`qwen3_pto_kernel.cpp` adapts four two-dimensional `ChipTensor` descriptors
and three scalars (operation, row/position parameter, FP32 scale bits) to the
operators. `qwen3_pto_orch.cpp` submits the kernel through HostBuildGraph.
Build with `prepare_simpler_host_artifacts.py --profile host_qwen3_operator`.
Operation IDs 1..8 are Copy, RMSNorm, Linear, RoPE, Attention, Residual,
SwiGLU and Softmax. Copy also implements embedding row selection, KV copy
and F16/F32 conversion. Unused input descriptors remain valid dummy matrices.

`qwen3_pto_layer.hpp` composes these primitives into a complete decoder layer.
Weights and RoPE factors are explicit views. Previous and next KV views name
separate allocations; the next KV binding requires read/write permission,
since attention consumes the appended cache in the same invocation. Private
intermediates use local F32 GM. Shared hidden and KV remain bound views and
are accessed through PTO instructions. Layer oracle tests reuse the operator
test entrypoint; they do not constitute W5 guest end-to-end acceptance.

The model-range callable uses `qwen3_pto_range.hpp`, its kernel wrapper and
orchestrator. Six guest UB_GM arguments are hidden input, previous packed KV,
token IDs (exact F32 integers, control metadata), hidden output, next packed
KV, and last-token logits. A seventh, private host-GM tensor holds initialized
weights followed by RoPE constants. Twelve scalars describe start/end/total
layers, past/current tokens, hidden/intermediate/Q-head/KV-head/head/vocab
dimensions, and attention-scale bits. No shared payload is copied into the
private constants tensor. The first range packs the embedding table; the
terminal range packs final norm and an independent LM-head view. An explicit
`lm_head.weight` takes precedence, with the embedding table used when absent.
KV headers keep the existing five-u64 per-layer
layout; guest control initializes headers and validates previous headers.
The numerical kernel touches only the K/V subranges. Next KV is read/write;
hidden and terminal logits outputs are write-only. Unused guest arguments
are read-only 1x1 dummies, including logits on nonterminal ranges.

`qwen3_pto_guest.h/.c` owns the W5 model-range guest operation: acquire six
Memory Service leases, initialize token metadata/KV headers, prepare callable
3, submit, and return original arena buffers after completion. The opaque
operation keeps all mappings and metadata alive. A timed-out submit retains
the operation and refuses release; a subsequent completion-recovery API is
still pending. Callers must not publish failed or pending buffers. The W5
build includes this helper and the W5 inference loop consumes its output.
Run `scripts/run_w5_qwen3_pto.py` for the two-node model campaign. This entry
disables the optional UB-SSD/GSVA materialization path so that downstream
hidden and previous KV are acquired from OBMM object views.

W5's model mode is selected with `SIM_W5_QWEN3_PTO=1`, separate from the
callable-1 probe. Token-text control metadata is prepared by
`scripts/prepare_qwen3_pto_token_table.py --weights DIR --output FILE`.
The little-endian table contains four u64 header words (magic
`0x515750544f545854`, version 1, configured vocabulary size, entry size 32),
then one four-u64 row per token (piece checksum, raw UTF-8 length, first two
eight-byte words). Missing/padded vocabulary entries are zero and rejected
if selected. This preserves the existing W5 tokenizer-piece wire encoding;
it contains no logits or model results. Set `SIM_W5_QWEN3_PTO_TOKEN_TABLE` to
the staged guest file. Sampling consumes PTO-produced logits.

`scripts/audit_w5_qwen3_pto.py` compares retained candidate/reference logs,
checks each node/step, UB_GM completion accounting, KV reuse and top-four
logits with a fixed 0.02 absolute tolerance. Its JSON output and log hashes
belong under the campaign's `out/` directory. This log audit does not claim
full-vocabulary elementwise or per-layer hidden-tensor comparison.

Each successful model kernel emits `QWEN3_PTO_OPERATOR_COUNTS` to stderr.
Counters are thread-local, reset at range entry, and incremented at actual
operator entry points (including nested attention linear/softmax calls).
A count record requires a matching successful dispatch completion before it
can establish coverage. The audit rejects missing records or count/geometry
mismatches; these counters measure operator invocations, not PTO instructions.

Optional numerical evidence uses `SIM_QWEN3_PTO_TRACE_DIR`, set by
`run_w5_qwen3_pto.py --numerical-trace` to a new campaign-local directory.
`qwen3_pto_trace.hpp` writes one exclusive, little-endian binary file per
range/past position. It snapshots layer hidden and K/V through PTO loads;
logits are observed from the tile after the ordinary TSTORE, without reading
a write-only binding or changing the destination. These extra diagnostic
reads are excluded from performance claims. Default runs produce no snapshots.
The file has magic `QPTOTR1\0`, seven u32 geometry words (first, end, past,
tokens, hidden, KV width, vocab), followed by records: six u32 words
(kind, layer, row, col, rows, cols), then rows*cols F32 bits. Kinds are 1 hidden,
2 key, 3 value, 4 logits; kind 0 with all other words zero terminates a
successfully written kernel snapshot. Logits use consecutive 64-column
records. An end record alone cannot prove dispatch completion: the separate
guest/QEMU audit must also pass. Missing, repeated, truncated or unexpected
records must fail numerical validation. Artifacts stay under `out/`.

RMSNorm uses FP32 `TSQRT` followed by `TDIV(1, root)` to reproduce the W5
reference's two rounding steps. The CPU `TRSQRT` implementation computes a
double reciprocal square root with a single final cast; it can differ by an
FP32 ULP before the subsequent FP16 handoff. A dedicated exact-FP32 golden
test covers the chosen arithmetic sequence, in addition to the independent
double-oracle tolerance tests. All arithmetic still uses PTO instructions.

The FP32 exponential precision check compares CPU `TEXP` against the host's
FP32 `exp` overload on a deterministic finite grid, including negative inputs
used by Softmax and SwiGLU. It reports a mismatch before any model campaign
is accepted. This isolates the CPU simulation arithmetic contract; it does
not claim bit-identical NPU transcendental results or change model tolerances.
