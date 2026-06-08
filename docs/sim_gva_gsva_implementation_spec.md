# GVA-GSVA Implementation Spec

## 1) 目标与适用范围

本文是 `docs/sim_gva_gsva_final_architecture_execution_plan.md` 的实现规格。

目标是把该目标转换成可执行实现：

```text
default guest access path
  -> ARM MMU / page-table-visible GSVA metadata
  -> QEMU GVA/GSVA route lookup
  -> GSVA-keyed coherence
  -> UB Link / OBMM data backend
```

本文定义以下内容：

- 协议对象与 ABI 字段。
- QEMU 与 guest Linux 的代码落点。
- GSVA coherence 状态机。
- segment retire/reuse 的事务语义。
- 从 legacy SIM_DEC/SIM_GVA_TCG 到 ARM MMU 主路径的迁移开关。
- 2/4/8-node 验收命令和日志判据。

## 2) 非目标

- 不在第一版精确模拟真实 ARM cache microarchitecture。
- 不改变已稳定的 OBMM directory MESI 数据面语义。
- 不删除 legacy `SIM_DEC` 控制面。
- 不要求一次性把所有 GVA demo 迁移为 ARM MMU 主路径。
- 不允许为了通过测试禁用 GSVA key 校验、epoch 校验或 coherence ACK。

## 3) 架构分层

实现必须分成三层，避免把 GSVA 语义继续混在 PA-MESI 或 SIM_DEC 私有字段里。

```text
GSVA semantic layer
  key: segment_id/home_va/vmid/asid/pte_offset/p_tag/cache_policy/epoch
  events: map/update/unmap/retire/reuse/token/cache_policy
  coherence: GSVA object ownership and stale rejection

GVA route layer
  ma_table: {vmid, asid, uba_range} -> {dcna, tid, upi, p_tag, token}
  mp_table: {p_tag} -> {ubc_port, link, lane}
  data route: GSVA UBA -> UB Link target

OBMM/PA data layer
  backing memory
  OBMM import/export lifecycle
  directory MESI line cache
  persistent point writeback/fence
```

Rule:

- GSVA layer decides whether an access is valid.
- GVA route layer decides where the access goes.
- OBMM/PA layer moves bytes and enforces line-level data coherence.

## 4) GSVA key protocol

### 4.1 Key definition

`gsva_key` is a stable protocol object. 

```c
struct gsva_key_v1 {
    uint32_t version;       /* must be 1 */
    uint32_t flags;
    uint64_t segment_id;
    uint64_t home_va;
    uint64_t size;
    uint64_t vmid;
    uint64_t asid;
    uint64_t pte_offset;
    uint32_t p_tag;
    uint32_t cache_policy;
    uint64_t epoch;
};
```

### 4.2 Equality rule

Two mappings refer to the same GSVA coherence object only if all fields below match:

```text
segment_id
home_va
vmid
asid
pte_offset
p_tag
cache_policy
epoch
```

`size` is not part of equality, but must pass containment validation:

```text
access_va in [home_va, home_va + size)
```

### 4.3 Field semantics

- `segment_id`: allocation identity from GVA Manager.
- `home_va`: GSVA base. In strict GSVA, `user_va == uba == home_va`.
- `size`: segment byte size.
- `vmid`: guest VM context. Use `0` until multi-VM support is implemented.
- `asid`: guest address-space context. Use process ASID when available; use `0` for kernel/global mappings.
- `pte_offset`: offset encoded by page-table metadata. Strict GSVA default is `0`.
- `p_tag`: route tag used by NoC `mp_table`.
- `cache_policy`: must match import/map policy. `DIRECTORY_MESI` remains value `4`.
- `epoch`: monotonic generation for segment lifecycle.

### 4.4 Token rule

`token_id` and `token_value` are permissions, not key identity.

Token validation happens after key lookup:

```text
lookup gsva_key
  -> validate epoch
  -> validate token
  -> validate access permissions
```

Changing token does not create a new key. It emits `GSVA_EVENT_TOKEN_CHANGE`.

### 4.5 Cache policy change

Changing `cache_policy` creates a new key identity.

Required sequence:

```text
old key: revoke + invalidate + drain + unmap
new key: map with new cache_policy
```

In-place cache policy mutation is forbidden.

## 5) Guest/QEMU ABI

### 5.1 Legacy ABI rule

Existing `SIM_DEC_OP_MAP` v1 must remain wire-compatible.

Do not append GSVA fields to legacy packed map payload.

### 5.2 New metadata opcode

Add a versioned GSVA metadata operation:

```text
SIM_DEC_OP_GSVA_MAP_V1
SIM_DEC_OP_GSVA_UNMAP_V1
SIM_DEC_OP_GSVA_EVENT_V1
SIM_DEC_OP_GSVA_QUERY_V1
```

Minimal payload:

```c
struct sim_dec_gsva_map_v1 {
    uint32_t version;
    uint32_t flags;
    struct gsva_key_v1 key;
    uint64_t local_pa;
    uint64_t local_va;
    uint64_t remote_uba;
    uint64_t token_id;
    uint64_t token_value;
    uint32_t source;
    uint32_t address_profile;
};
```

`local_va` must equal `remote_uba` and `key.home_va` for strict GSVA.

### 5.3 Event payload

```c
enum gsva_event_type {
    GSVA_EVENT_MAP = 1,
    GSVA_EVENT_MAP_UPDATE = 2,
    GSVA_EVENT_UNMAP = 3,
    GSVA_EVENT_SEGMENT_RETIRE = 4,
    GSVA_EVENT_SEGMENT_REUSE = 5,
    GSVA_EVENT_TOKEN_CHANGE = 6,
    GSVA_EVENT_CACHE_POLICY_CHANGE = 7,
    GSVA_EVENT_TLB_FLUSH = 8,
};

struct sim_dec_gsva_event_v1 {
    uint32_t version;
    uint32_t type;
    struct gsva_key_v1 key;
    uint64_t new_epoch;
    uint64_t flags;
};
```

### 5.4 Error codes

Use stable error names in logs and query output.

```text
GSVA_OK
GSVA_ERR_BAD_VERSION
GSVA_ERR_KEY_MISMATCH
GSVA_ERR_STALE_EPOCH
GSVA_ERR_TOKEN_DENIED
GSVA_ERR_ROUTE_MISSING
GSVA_ERR_COH_PENDING
GSVA_ERR_COH_TIMEOUT
GSVA_ERR_TLB_STALE
GSVA_ERR_SEGMENT_RETIRED
GSVA_ERR_UNSUPPORTED_POLICY
```

## 6) QEMU implementation touchpoints

### 6.1 New modules

Add these QEMU modules:

```text
vendor/qemu_8.2.0_ub/hw/ub/gsva_key.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_key.h
vendor/qemu_8.2.0_ub/hw/ub/gsva_route.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_route.h
vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.h
vendor/qemu_8.2.0_ub/hw/ub/gsva_stats.c
vendor/qemu_8.2.0_ub/hw/ub/gsva_stats.h
```

Keep existing:

```text
vendor/qemu_8.2.0_ub/hw/ub/obmm_coherence.c
vendor/qemu_8.2.0_ub/hw/ub/obmm_coherence.h
```

### 6.2 `ub_ubc.c` responsibilities

`ub_ubc.c` remains the integration point:

- Parse `SIM_DEC_OP_GSVA_*`.
- Register GSVA map into `gsva_route`.
- Register coherence object into `gsva_coherence`.
- Dispatch CPU window and DMA hits through GSVA validation before OBMM PA-MESI data access.
- Emit route/coherence stats.

`ub_ubc.c` must not own GSVA state-machine logic.

### 6.3 ARM MMU hook migration point

Phase 1 and Phase 2 can still enter through imported PA windows.

The final ARM MMU path must add an entry point equivalent to:

```c
bool gsva_mmu_lookup(uint64_t va,
                     uint64_t vmid,
                     uint64_t asid,
                     GSVALookupResult *out);
```

The lookup result must include:

```c
struct GSVALookupResult {
    struct gsva_key_v1 key;
    uint64_t uba;
    uint64_t offset;
    uint32_t access_flags;
    uint32_t p_tag;
};
```

The ARM MMU hook must call the same route and coherence code used by imported PA validation.

## 7) Guest Linux implementation touchpoints

### 7.1 UAPI

Add GSVA metadata structures under:

```text
guest-linux/kernel_ub/include/uapi/ub/
```

Required ABI:

```text
OBMM_CMD_GSVA_REGISTER_APERTURE
OBMM_CMD_GSVA_ALLOC_SEGMENT
OBMM_CMD_GSVA_RETIRE_SEGMENT
OBMM_CMD_GSVA_QUERY_SEGMENT
```

Do not remove existing OBMM import/export commands.

### 7.2 OBMM import/export path

Update:

```text
guest-linux/kernel_ub/drivers/ub/obmm/obmm_import.c
guest-linux/kernel_ub/drivers/ub/obmm/obmm_export.c
guest-linux/kernel_ub/drivers/ub/obmm/obmm_shm_dev.c
```

Required behavior:

- `MAP_GSVA` mmap must use fixed GSVA address.
- Strict GSVA must reject `mmap(NULL, ...)`.
- `obmm_import` must build `gsva_key_v1`.
- `obmm_shm_dev` must reject mapping outside registered GSVA aperture.
- unimport must emit `GSVA_EVENT_UNMAP`.

### 7.3 sim decoder backend

Update:

```text
guest-linux/kernel_ub/drivers/ub/ubus/sim/
```

Required behavior:

- Preserve legacy `SIM_DEC_OP_MAP`.
- Add `SIM_DEC_OP_GSVA_*`.
- Include run-time feature query for QEMU GSVA support.
- Fail strict GSVA if QEMU does not advertise GSVA support.

### 7.4 CLI and tests

Every GSVA feature must have a CLI.

Required tools:

```text
guest-linux/aarch64/apps/gva_manager
guest-linux/aarch64/apps/gsva_lifecycle_test
guest-linux/aarch64/apps/gsva_coh_test
```

Required modes:

```text
gva_manager --bootstrap --node-id N --node-count M
gva_manager --alloc --size BYTES --cache-policy directory-mesi
gva_manager --retire --segment-id ID
gsva_lifecycle_test --mode retire_reuse
gsva_lifecycle_test --mode stale_epoch
gsva_coh_test --mode write_read
gsva_coh_test --mode writer_inv
gsva_coh_test --mode retire_while_shared
```

## 8) GSVA coherence state machine

### 8.1 Object state

```c
enum gsva_coh_state {
    GSVA_COH_I = 0,
    GSVA_COH_S = 1,
    GSVA_COH_E = 2,
    GSVA_COH_M = 3,
    GSVA_COH_RETIRED = 4,
};

struct gsva_coh_object {
    struct gsva_key_v1 key;
    enum gsva_coh_state state;
    uint32_t home_cna;
    uint32_t owner_cna;
    uint64_t sharer_bitmap;
    uint64_t epoch;
    bool pending;
    uint64_t pending_seq;
};
```

### 8.2 Events

```text
MapShared
MapExclusive
ReadAcquire
WriteAcquire
Invalidate
InvalidateAck
Downgrade
DowngradeAck
Writeback
WritebackAck
Fence
FenceAck
Unmap
Retire
RetireAck
Reuse
TokenChange
TLBFlush
```

### 8.3 Transition table

```text
I + ReadAcquire        -> S, add requester sharer
I + WriteAcquire       -> M, owner=requester
S + ReadAcquire        -> S, add requester sharer
S + WriteAcquire       -> pending invalidate sharers except requester
S + all InvalidateAck  -> M, owner=requester, sharers=0
E + ReadAcquire        -> S, owner becomes sharer, requester sharer
E + WriteAcquire owner -> M, owner unchanged
E + WriteAcquire other -> pending downgrade/invalidate owner
M + ReadAcquire other  -> pending writeback or data-forward, then S
M + WriteAcquire other -> pending writeback/invalidate owner, then M owner=other
any + Retire           -> pending revoke all holders
pending + all ACK      -> requested terminal state
any + stale epoch      -> reject GSVA_ERR_STALE_EPOCH
RETIRED + any access   -> reject GSVA_ERR_SEGMENT_RETIRED
```

### 8.4 Ordering rule

All GSVA coherence operations are ordered by `(key, pending_seq)`.

For each key:

```text
new operation cannot commit while pending=true
retry with same pending_seq is idempotent
retry with older epoch is stale
```

### 8.5 PA-MESI relationship

GSVA coherence calls PA-MESI only after GSVA validation passes.

For a write:

```text
GSVA WriteAcquire
  -> owner permission granted
  -> obmm_coh_cpu_write / obmm_coh_dma_write
```

For a read:

```text
GSVA ReadAcquire
  -> shared permission granted
  -> obmm_coh_cpu_read / obmm_coh_dma_read
```

For retire:

```text
GSVA Retire
  -> revoke GSVA holders
  -> PA-MESI fence/writeback
  -> unmap route
  -> mark RETIRED
```

## 9) Segment lifecycle transaction

### 9.1 Coordinator

The segment home manager is the coordinator.

Coordinator identity:

```text
home_cna from export/bootstrap metadata
```

### 9.2 Retire sequence

```text
1. coordinator emits GSVA_EVENT_SEGMENT_RETIRE(key, epoch)
2. QEMU marks key pending retire
3. QEMU sends revoke/invalidate to all holders
4. holders drop local GSVA state and flush TLB for range
5. holders ACK retire
6. coordinator issues PA-MESI fence/writeback
7. route entry is removed
8. key enters GSVA_COH_RETIRED
9. guest manager receives retire committed
```

### 9.3 Reuse sequence

```text
1. old key must be RETIRED
2. new segment gets new segment_id or higher epoch
3. route is installed with new key
4. old epoch requests are rejected
5. new map requests must carry new epoch
```

### 9.4 Timeout behavior

Timeout does not silently commit.

Allowed terminal outcomes:

```text
RETIRE_COMMITTED
RETIRE_ABORTED
RETIRE_PENDING_TIMEOUT
```

If timeout occurs:

- New maps for the same segment are rejected.
- Existing stale holders are treated as invalid.
- Query must report `GSVA_ERR_COH_TIMEOUT`.

## 10) Migration switches

Add explicit mode selection.

```text
gsva.mode=legacy_sim_dec
gsva.mode=sim_gva_tcg
gsva.mode=arm_mmu
```

Default during development:

```text
gsva.mode=sim_gva_tcg
```

Final default:

```text
gsva.mode=arm_mmu
```

Strict GSVA flag:

```text
gsva.strict=1
```

Strict mode requirements:

- `user_va == uba == home_va`.
- No fallback to relocated mmap.
- No fallback to legacy map if QEMU lacks GSVA support.
- Missing GSVA metadata is a hard failure.

## 11) Stats and diagnostics

Add stats grouped by layer.

GSVA stats:

```text
gsva_map_total
gsva_unmap_total
gsva_key_mismatch_total
gsva_stale_epoch_total
gsva_retire_total
gsva_retire_timeout_total
gsva_tlb_flush_total
gsva_coh_gets_total
gsva_coh_getm_total
gsva_coh_inv_total
gsva_coh_wb_total
```

Route stats:

```text
gva_ma_lookup_total
gva_ma_miss_total
gva_mp_lookup_total
gva_mp_miss_total
```

PA-MESI stats remain separate:

```text
obmm_coh_gets_total
obmm_coh_getm_total
obmm_coh_inv_total
obmm_coh_wb_total
obmm_coh_fence_total
```

Required log tags:

```text
GSVA_MAP
GSVA_UNMAP
GSVA_KEY
GSVA_COH
GSVA_RETIRE
GSVA_ROUTE
GSVA_TLB
```

Every acceptance run must print:

```text
run_id=<id>
mode=<legacy_sim_dec|sim_gva_tcg|arm_mmu>
node_count=<2|4|8>
verdict=<PASS|FAIL>
failure_reason=<stable error name>
```

## 12) Validation matrix

### 12.1 Existing OBMM coherence baseline

Must continue to pass:

```bash
COH_TEST_MODE=all ./guest-linux/aarch64/scripts/run_ub_dual_node_obmm_coh_test.sh
COH_TEST_MODE=all ./guest-linux/aarch64/scripts/run_ub_four_node_obmm_coh_test.sh
COH_TEST_MODE=all RUN_SECS=360 ./guest-linux/aarch64/scripts/run_ub_eight_node_obmm_coh_test.sh
```

### 12.2 GSVA address identity

Required command:

```bash
./guest-linux/aarch64/scripts/run_ub_four_node_gsva_identity_test.sh
```

Required assertions:

```text
user_va == uba == home_va
GSVA_MAP appears in QEMU logs
no relocated mmap fallback appears
```

### 12.3 GSVA lifecycle

Required command:

```bash
GSVA_TEST_MODE=retire_reuse ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
```

Required assertions:

```text
GSVA_RETIRE committed
old epoch request rejected with GSVA_ERR_STALE_EPOCH
new epoch map succeeds
```

### 12.4 GSVA coherence

Required commands:

```bash
GSVA_TEST_MODE=write_read ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_TEST_MODE=writer_inv ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_TEST_MODE=retire_while_shared ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_TEST_MODE=write_read RUN_SECS=360 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_coh_test.sh
```

Required assertions:

```text
GSVA_COH_GETS present
GSVA_COH_GETM present for writer tests
GSVA_COH_INV present for conflicting writer tests
OBMM_COH_* present only as data-layer evidence
SIM_GVA_TCG absent when gsva.mode=arm_mmu
```

### 12.5 ARM MMU default path

Required command:

```bash
GSVA_MODE=arm_mmu ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_arm_mmu_acceptance.sh
```

Required assertions:

```text
GSVA_TLB lookup present
GSVA_ROUTE lookup present
SIM_GVA_TCG data-path hit absent
legacy SIM_DEC map may appear only for bootstrap/fallback diagnostics
```

## 13) Milestones

### Milestone 1: Protocol freeze

Deliverables:

- `gsva_key_v1` in guest UAPI and QEMU header.
- `SIM_DEC_OP_GSVA_*` protocol implemented as query-only or map dry-run.
- Logs show `GSVA_KEY`.

Acceptance:

```bash
./guest-linux/aarch64/scripts/build_guest_artifacts.sh
./guest-linux/aarch64/scripts/build_qemu_binary.sh
```

### Milestone 2: GSVA map/unmap route

Deliverables:

- Strict GSVA map validates `user_va == uba == home_va`.
- QEMU route table contains `gsva_route_entry`.
- unmap emits `GSVA_UNMAP`.

Acceptance:

```bash
./guest-linux/aarch64/scripts/run_ub_four_node_gsva_identity_test.sh
```

### Milestone 3: GSVA coherence over existing PA-MESI

Deliverables:

- GSVA ReadAcquire/WriteAcquire implemented.
- PA-MESI called only after GSVA validation.
- writer invalidation test passes.

Acceptance:

```bash
GSVA_TEST_MODE=writer_inv ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
```

### Milestone 4: lifecycle transaction

Deliverables:

- retire/reuse coordinator.
- epoch stale rejection.
- timeout diagnostics.

Acceptance:

```bash
GSVA_TEST_MODE=retire_reuse ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
```

### Milestone 5: ARM MMU default path

Deliverables:

- `gsva.mode=arm_mmu` enters route/coherence through MMU/TLB metadata.
- `SIM_GVA_TCG` no longer appears in default data path.
- 2/4/8-node matrix passes.

Acceptance:

```bash
GSVA_MODE=arm_mmu ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_arm_mmu_acceptance.sh
GSVA_MODE=arm_mmu RUN_SECS=360 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_arm_mmu_acceptance.sh
```

## 14) Implementation rules

- Do not modify legacy `SIM_DEC_OP_MAP` layout.
- Do not use PA as GSVA coherence identity.
- Do not treat token as key identity.
- Do not allow strict GSVA to relocate mmap address.
- Do not commit segment reuse before retire ACK/fence completion.
- Do not merge GSVA stats into OBMM PA-MESI stats.
- Do not accept `cache_policy` mutation in place.

## 15) Open decisions to close before coding

These must be resolved before Milestone 1 is considered complete:

- Exact numeric opcode values for `SIM_DEC_OP_GSVA_*`.
- Exact UAPI header file names.
- Whether `asid` is real guest ASID or process-derived software ASID in first version.
- Whether `segment_id` is globally allocated by GVA Manager or derived from `{home_cna, generation, local_id}`.
- Whether ARM MMU metadata is encoded in PTE bits or stored in a side table for the first `arm_mmu` implementation.
