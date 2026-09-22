# Lingqu shmem PTO guest adaptor

## Managed model-direct dispatch

`lingqu_model_direct_guest.h/.c` is the model-neutral W6 dispatch boundary.
Callers provide Memory Service mapping owners, bounded tensor descriptors,
scalars and a callable identity. The adapter acquires every owner through the
existing compute context, keeps its lease through the matching CQ completion
and releases only after all leases complete. Its public API carries no model
payload pointer. Completion reports `model_tensor_address_space=UB_GM`,
`pto_instruction_io=1` and `shared_payload_staging_bytes=0`.

`qwen3_managed_direct_guest.h/.c` maps Qwen first, middle and terminal range
geometry onto callable 3's six managed tensors and twelve scalars. It reuses
the caller's managed compute context, endpoint and object owners. Token IDs and
KV headers are bounded control content initialized by the caller before
prepare. Qwen W6 serving-loop integration is tracked in
`docs/plans/2026-09-22-w6-managed-model-direct-plan.md`.

## Managed W5 Rust/Simpler range adapter

`w5_managed_model_guest.h/.c` prepares callable 4 on the existing authorized
mapping/CQ path. It uses the current Rust/Simpler model-range implementation;
callables 1–3 and the separate Qwen PTO graph keep their existing contracts.
The model caller supplies SDK owners for a published read-only control view,
zero to two read-only V2 input views and a separate writable output allocation.
Inputs must have been acquired/mapped using those exact V2 references. The
adapter pins these existing owners; it does not resolve/grant new holders.
The W5 deployment opens its OBMM channel with O_SYNC mappings because strict
PTO registration requires an uncached page-granular VMA, including when the
managed output allocation is homed on the executing node.
Control is 8768 bytes: little-endian magic/version/count/op ID/request ID,
eight range coordinates, 8192 prompt-control bytes and two 256-byte V2 slots.
Unused slots are zero. The fingerprint CLI is
`sim-cli managed-model-provider-fingerprint`; it fingerprints the compiled
adapter/model dispatch source and selected profile, excluding model weights.

Prepare retains every memref. Submit begins SDK compute leases before enqueue;
a rejected enqueue consumes no endpoint ID and may be retried on the same
operation/endpoint without beginning pins twice. Poll retains all resources on
`-EAGAIN` or protocol failure. Only the original completion completes leases;
a consumed completion survives a lease-completion retry. Release preserves
failed cleanup handles. Callers retain context, endpoint and immutable queue
metadata until release, serialize endpoint operations, and publish output only
after a successful completion. A failed completion permits cleanup only.

The registered Rust entry reads and writes through request-bound callbacks,
checks permissions/ranges/full V2 payload checksums, rejects output aliasing,
and fences output before success. Temporary host bytes are part of the existing
Rust numerical path; this is not the zero-staging PTO numerical kernel contract.
`sim-cli managed-model-range-run --provider-callbacks ...` exercises the same
entry with explicitly labelled offline callbacks. That CLI cannot validate SDK
grants or physical leases. Native operation fixtures and offline model execution
do not establish W5 serving-loop integration or real multi-guest acceptance.

Managed serving output allocation keys use the model-side canonical form
`w5v1/<run:16hex>/<request:16hex>/<producer:8hex>/<step:16hex>` (lowercase,
nonzero run/request). The common llm_infer component produces these keys;
the Rust managed input boundary decodes them without changing the neutral
service's key semantics. A sealed KV reference must identify the previous
step and current range producer. Content version is independent of decode
step. Consumers still resolve the exact expected logical key and retain its
SDK holder; parsing this name cannot grant access or replace payload checks.
`sim-cli managed-model-object-key <run> <request> <producer> <step>` prints the
canonical allocation key for configuration and diagnosis.
The fixed two-guest managed model publication probe derives its output key
from the shared generation and configured producer node; both guests print
that exact key with `--managed-config ... --validate-only` before any network
or device access. Its step-zero probe remains separate from the W5 serving
loop and the multi-step PP acceptance matrix.

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
The raw region/memref lifetime API requires external serialization;
multiple in-flight dispatches remain part of the P4 concurrency gate. The
caller must keep the underlying mapping and OBMM registration active until
all in-flight objects have been finished and every memref has been destroyed.

### Endpoint timeout recovery contract

The endpoint retains one submitted operation until
its own valid CQ completion is consumed. A wait timeout keeps the queue pages,
metadata and memref leases alive. Recovery waits for that operation without
resubmitting its slot. A second submit and checked close return `-EBUSY` while
it is pending. An unexpected or malformed completion retains the operation
and reports a protocol error; it cannot authorize release. Callers continue
to serialize endpoint access; this increment does not establish the separate
multi-thread owner contract.

The existing managed-config CLI accepts an optional `submit_timeout_ms`
shorter than `timeout_ms`. Its timeout case must observe retained leases and
rejected close, then consume the original completion, validate both tensor
results and return the actual resources to baseline. Native endpoint tests
use queue/MMIO boundary fixtures; the guest case uses the existing QEMU
authorization-delay setting to exercise a real delayed completion. Neither
a timeout nor a cancellation request counts as completion.
The legacy direct consumer also checks endpoint close before releasing any
metadata or mapping; an unresolved operation keeps that guest process parked
with an explicit failure marker until the owning headless runner stops it.

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

清理失败时，lease 保留尚未释放的 region、registration 和 backend 归属，
`active_leases` 不减少，context close 继续返回 `-EBUSY`。调用方重试原 lease
的 release；已销毁的 memref 不可再次使用。acquire 的失败路径也遵守该约定：
若回滚未完成，返回非零错误和非空 `lease_out`，仅用于继续清理，不交付 memref
或本地输出 buffer。backend 在失败返回时通过 `backend_lease` 交回已有资源，
release 失败不得销毁该句柄。raw acquire/release 保持外部串行化约定；endpoint 的
原操作超时恢复遵循上节契约，并发 owned 接口遵循下文 access domain 约定。

受管理对象使用 `lingqu_shmem_mem_service_acquire_managed()`，输入为同一 SDK
返回并持有的 object mapping 与 active mapping lifecycle，保留现有 key/generation
空间；不将内容 version 当作分配 generation。只接受逻辑范围内的 tensor geometry
和原映射允许的权限。平台构建显式启用 `LINGQU_SHMEM_MANAGED_MAPPING`，并链接
OBMM provider；未启用的构建返回 `-EOPNOTSUPP`，旧 W5 acquire 接口保持兼容。
平台 adapter 先 pin 原 SDK mapping，再调用严格 V2 PTO 注册，只使用 driver
返回的真实 PA。注销失败保留 registration 和 provider pin；注销成功后才释放 pin。
调用方保留同一 holder/lifecycle，并串行化 mapping、compute 和清理操作。
AM2 的单线程双节点用例已验证四种实际数据组合和原 completion 超时恢复，详见
`docs/2026-09-11-gva-managed-service-implementation-progress.md` 的 r139–r142。
raw managed 接口保持串行化约定；并发 owned 接口的局部实测见下文，完整
权限/范围矩阵及 home 互换仍待验收。

无模型的受管理客户端通过 `lingqu_shmem_mem_service_open_managed()` 打开仅支持
managed mapping 的 compute context。该 context 不依赖旧 payload arena 或模型
runtime；legacy acquire 返回 `-EOPNOTSUPP`。部署构建链接启用 managed 功能的
平台 adapter，保留旧 `open()` 的行为，调用方无须选择传输。

并发接入使用 `lingqu_shmem_mem_service_acquire_owned()`，输入为 Memory Service
SDK 的同一 mapping owner 和有界 tensor spec。它先取得 compute pin，再在共享
access domain 锁内完成原映射的注册；退出时在同一锁内注销，最后释放 compute
pin。失败回滚仍遵守非空 cleanup-only lease 的约定。每个 lease 由单个调用方
使用；同一 compute context 的所有 owner 必须属于同一 access domain。
context 和 owner 的 destroy 必须在所有调用线程 join 后执行。
调用方在提交前用 `lingqu_shmem_mem_service_lease_begin()` 标记原 operation，
确认该 operation 的实际 completion 后才调用 `lease_complete()`。等待和 submit
在 domain 回调外执行，未完成的 pin 拒绝 release。旧 raw acquire 仍要求外部
串行化，不能与同一 channel 的 owner 操作并发混用。SDK 的八线程/双 owner
原生 fixture 和本适配层边界测试均已通过；真实 guest 证据及边界如下。
managed-config 新增 `access_threads=0..8`，默认 0；consumer 的工作线程分别持有
同一 owner 的只读 CPU pin，持续校验已发布且不再修改的输入 A，跨越两次 PTO
dispatch。第二次超时时检查 owner close 返回 busy 并停止新借用，已有线程继续
访问；原 completion 后停止并 join 工作线程，再释放主 CPU pin 和 holder。
普通等待模式在两个 dispatch 完成后检查 CPU 在用拒绝关闭，不声称此时计算在用。
独立 `MANAGED_OWNER` 日志记录实际 pin、线程校验和关闭结果；验收器按配置检查。
r146 延迟用例在第二次 PTO pending 时记录 CPU/compute/in-flight pins 为 5/3/3，
r147 普通用例在计算完成后记录 5/0/0；两者均拒绝 close 和新借用，4 个 CPU
线程在停止接纳后都继续完成原映射读取，最终全部 pins、mapping、holder 归零。
这仅证明双节点、单 home、单对象和两次顺序计算的并发 CPU 寿命子集，AM2 未完成。

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
