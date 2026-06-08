# GVA / GSVA / OBMM MESI 阶段性状态总结

日期：2026-06-09

仓库：`/Volumes/repos/ub_sim`

依据：

- 当前代码实现。
- `docs/sim_gva_gsva_run_report.md` 中归档的 2026-06-05 两节点与四节点 GVA/GSVA 运行日志。
- `guest-linux/aarch64/logs` 下 2026-06-07 的 GVA direct matrix、GSVA 8-node manager/matrix、OBMM coherence 4/8-node 日志。
- 2026-06-08 在远端 `cf:/sd_data/repo/ub_sim` 上重新构建 guest artifacts 并通过的 GSVA segment ABI lifecycle 验证日志：
  `guest-linux/aarch64/logs/2026-06-08_23-57-24_gsva_lc_11516`。
- 2026-06-09 在远端 `cf:/sd_data/repo/ub_sim` 上重新构建 guest artifacts 与 QEMU，并通过的 GSVA token v1 acquire 验证日志：
  `guest-linux/aarch64/logs/2026-06-09_00-09-33_gsva_coh_30739`。
- 2026-06-09 在远端 `cf:/sd_data/repo/ub_sim` 上重新构建 guest artifacts 与 QEMU，并通过的 GSVA token v1 rotation 验证日志：
  `guest-linux/aarch64/logs/2026-06-09_00-15-27_gsva_coh_10949`。
- 2026-06-09 在远端 `cf:/sd_data/repo/ub_sim` 上重新构建 guest artifacts 与 QEMU，并通过的 GSVA event retire tombstone 验证日志：
  `guest-linux/aarch64/logs/2026-06-09_00-21-31_gsva_coh_12940`。
- 2026-06-09 在远端 `cf:/sd_data/repo/ub_sim` 上重新构建 guest artifacts，并通过的四节点 GSVA writer invalidation 验证日志：
  `guest-linux/aarch64/logs/2026-06-09_00-25-17_gsva_coh4_15946`。
- 2026-06-09 在远端 `cf:/sd_data/repo/ub_sim` 上重新构建 guest artifacts，并通过的 GSVA stale epoch remap rejection 验证日志：
  `guest-linux/aarch64/logs/2026-06-09_00-28-44_gsva_coh_25343`。
- `docs/qemu_obmm_directory_mesi_coherence_design.md` 中的 OBMM directory MESI 当前实现状态记录。

除明确列出的 2026-06-08 segment ABI 验证、2026-06-09 token acquire 验证、2026-06-09 token rotation 验证、2026-06-09 event retire 验证、2026-06-09 四节点 writer invalidation 验证和 2026-06-09 stale remap 验证外，其余结论基于已经存在的代码和日志证据。

## 1. 总体结论

当前 `ub_sim` 已经完成了一个可运行、可验证的 GVA / GSVA / OBMM MESI 阶段闭环：

1. GVA 路由控制面已经能从 guest OBMM import/export 进入 QEMU SIM_DEC/GVA route，并形成 `ma_table/mp_table` 元数据。
2. GSVA identity profile 已经能在 2/4/8 节点场景下证明 `user_va == uba == home_va` 与 `pte_offset=0`。
3. 普通 GVA direct profile 已经能证明 `local_va/home_va/uba` 非 identity 时的 `pte_offset` 路由转换。
4. Guest kernel 已经具备 GSVA aperture 注册、`MAP_GSVA` flag 消费、OBMM shmdev mmap 校验和普通 mmap 防重叠保护。
5. QEMU ARM TCG slow path 已经有 `SIM_GVA_TCG=1` 控制的 GVA route probe，证明把 GVA 入口前移到 ARM MMU/TLB fill path 是可行的。
6. OBMM directory MESI 已经实现并通过 4/8 节点运行日志验证，覆盖 `GETS/GETM/DATA/INV/INV_ACK/WB/FENCE` 等关键消息与 dirty owner writeback。
7. `OBMM_CMD_GSVA_ALLOC_SEGMENT/QUERY_SEGMENT/RETIRE_SEGMENT` v1 ABI 已经进入 guest kernel ioctl path，并通过两节点 lifecycle `segment_abi` 验证。
8. `OBMM_CMD_GSVA_EVENT_V1` 已经进入 guest kernel ioctl path，可触发 QEMU GSVA ReadAcquire/WriteAcquire/TokenChange/Retire；active route token v1 精确匹配校验已通过两节点 `token_denied` 验证，basic route-local token rotation 已通过两节点 `token_rotate` 验证，event Retire route tombstone 已通过两节点 `retire_event` 验证。
9. 四节点 `writer_inv` 已验证 GSVA ReadAcquire shared state 到 WriteAcquire invalidating writer 的状态机转换，QEMU 日志可见 `WriteAcquire S->M pending inv` 和 `WriteAcquire S->M`。
10. Stale epoch remap rejection 已验证：event Retire 后同一 `{segment_id, home_va, epoch=1}` 再次 map 会被 QEMU tombstone 拒绝为 `GSVA_ERR_STALE_EPOCH`。
11. 最终目标中的 GSVA-specific coherence 仍未完整完成；当前已经有 GSVA route/coherence 模块、acquire token 校验、basic token rotation、event retire tombstone、四节点 writer invalidation 和 stale remap rejection，但跨节点 retire ACK/timeout、epoch reuse commit、manager token revoke ACK、ARM MMU 默认路径还未形成最终事务闭环。

一句话判断：

```text
当前阶段已经从“设计概念”进入“多节点可运行实现”。
GVA/GSVA 地址语义、QEMU route、guest kernel GSVA aperture、OBMM directory MESI 已经有真实日志闭环。
segment descriptor ABI、token acquire/rotation、event retire tombstone、四节点 writer invalidation 和 stale epoch remap rejection 已有独立验证；下一阶段的核心不是再证明能跑，而是把 GSVA-specific coherence、token revoke ACK、epoch reuse 与 route/coherence/TLB 的事务绑定、ARM MMU 默认路径产品化。
```

## 2. 当前实现分层状态

### 2.1 Guest kernel / OBMM mmap 层

已实现能力：

- `MAP_GSVA` 已定义并进入 generic mmap 入口。
- `mmap_consume_gsva_flag()` 对 `MAP_GSVA` 做前置校验：
  - 必须 file-backed。
  - 必须 shared mapping。
  - 文件必须声明 `mmap_supported_flags & MAP_GSVA`。
  - 通过 `OBMM_MMAP_FLAG_GSVA` 编码到 `pgoff`，再交给 OBMM shmdev mmap。
- `MAP_GSVA | MAP_ANONYMOUS` 被拒绝。
- guest kernel 中存在 GSVA reserved aperture registry：
  - register。
  - query。
  - clear。
  - overlap 检测。
- `obmm_shm_dev` 中已经对 GSVA mmap 做 lease/address 校验：
  - `OBMM_MMAP_FLAG_GSVA` 只允许映射 GSVA segment。
  - `region_gsva_segment(reg)` 必须成立。
  - `expected_start = reg->gsva_base + offset` 必须等于 `vma->vm_start`。
  - 映射范围必须落在 active GSVA aperture 内。
  - GSVA segment 不允许不用 `MAP_GSVA` 映射。
  - 普通 OBMM mmap 不允许重叠 active GSVA aperture。

用户影响：

- 用户态不能无意中把普通 mapping 放进 GSVA aperture。
- GSVA segment 必须在协议声明的地址上 mmap，避免“日志看起来相等但实际用户 VA 被内核重定位”的假成功。

当前边界：

- guest kernel 已有 aperture、`MAP_GSVA` mmap 保护，以及 `OBMM_CMD_GSVA_ALLOC_SEGMENT` / `QUERY_SEGMENT` / `RETIRE_SEGMENT` v1 descriptor ABI。
- `segment_abi` 验证已经证明 descriptor 字段来源可用：`segment_id` 来自 `home_cna << 48 | local_counter`，`epoch=1`，`p_tag=home_cna & 0x00ffffff`，`token_id/token_value` 由 kernel 分配。
- `OBMM_CMD_GSVA_EVENT_V1` 已暴露 guest ioctl request/response，可从 guest 侧发起 ReadAcquire/WriteAcquire/Retire/InvAck/Retry 语义事件，并获得 `GSVA_OK` 或 `GSVA_ERR_*`。
- Event Retire 已接到 QEMU GSVA unmap handler，能触发 coherence retire、PA-MESI fence/invalidate best-effort、CPU window remove、route tombstone。
- 当前 segment lifecycle 仍未和跨节点 retire ACK、reuse epoch、TLB flush 形成完整原子事务；这仍是后续工作。

## 2.2 GVA Manager / GSVA address management 层

已实现能力：

- `gva_manager` 可以通过 OBMM bootstrap 建立 manager 间通信。
- manager 能协商同一个 GSVA aperture。
- manager 能把 aperture 注册给 guest kernel/OBMM。
- `/proc/obmm/gsva_aperture` 可作为 kernel registry 可见性的诊断点。
- 两节点、四节点、八节点日志均显示 manager 能完成 bootstrap 与 kernel aperture registration。

日志证据：

- 2-node：`guest-linux/aarch64/logs/2026-06-05_13-49-52_gsva_mgr_4165`
- 4-node：`guest-linux/aarch64/logs/2026-06-05_10-12-55_gsva_mgr4_5846`
- 8-node：`guest-linux/aarch64/logs/2026-06-07_22-39-37_gsva_mgr8_13812`

8-node 最新证据示例：

```text
[gva_manager] kernel aperture registry -> ok base=0x700000000000 size=0x4000000 generation=0x475356410008
[gva_manager] result=done generation=0x475356410008 aperture_base=0x700000000000 aperture_size=0x4000000 registry=kernel-obmm
```

用户影响：

- GSVA aperture 已不是 demo 进程私有假设，而是每个 guest OS 的 kernel/OBMM 都可见的 reserved range。
- 多节点场景中，不同节点对 `base/size/generation` 的理解一致。

当前边界：

- Manager 已能 bootstrap aperture，但还未成为最终 segment lifecycle coordinator。
- kernel segment ABI 已有 alloc/query/retire 验证；manager 还没有把该 ABI 作为默认 alloc/retire 协调路径。
- basic route-local token rotation 已按 canonical implementation plan 的 `old token denied / new token accepted / key identity unchanged` 规则落地；manager 分发、revoke ACK 和 TLB flush 仍未完成。

## 2.3 QEMU SIM_DEC / GVA route 层

已实现能力：

- 当前 SIM_DEC opcode 已包括：

```c
SIM_DEC_OP_MAP              0x01
SIM_DEC_OP_UNMAP            0x02
SIM_DEC_OP_SYNC             0x03
SIM_DEC_OP_QUERY            0x04
SIM_DEC_OP_OBMM_BOOTSTRAP_PUBLISH 0x05
SIM_DEC_OP_OBMM_BOOTSTRAP_LOOKUP  0x06
SIM_DEC_OP_GVA_MAP          0x07
SIM_DEC_OP_COH_FENCE        0x08
SIM_DEC_OP_GSVA_MAP_V1      0x09
SIM_DEC_OP_GSVA_UNMAP_V1    0x0a
SIM_DEC_OP_GSVA_EVENT_V1    0x0b
SIM_DEC_OP_GSVA_QUERY_V1    0x0c
```

- `SIM_DEC_OP_GVA_MAP` 已能建立 GVA route。
- QEMU route entry 保存：
  - `local_pa`
  - `remote_uba`
  - `local_va`
  - `home_va`
  - `pte_offset`
  - `vmid/asid`
  - `token_id/token_value`
  - `p_tag`
  - `cache_policy`
  - `access_flags`
  - `mp_table` 解析结果。
- GSVA identity profile 校验：
  - `local_va != 0`
  - `home_va == local_va`
  - `home_va == remote_uba`
  - `pte_offset == 0`
- QEMU route dump 可以输出完整 `ma_table/mp_table` 视图。
- GVA path 读写统计分别由 `SIM_DEC_STATS` 与 `GVA_STATS` 导出。
- `SIM_DEC_OP_GSVA_QUERY_V1` capability query 已有初步 handler，能输出 `GSVA_QUERY_CAPS`。
- `SIM_DEC_OP_GSVA_EVENT_V1` 已接入 guest ioctl/backend，可转发 ReadAcquire/WriteAcquire，并把 QEMU 的 `GSVA_ERR_TOKEN_DENIED` 作为语义错误返回给 guest。
- QEMU `gsva_route_validate_token()` 已按 token v1 定义收紧：
  - protected route 必须有非零 `token_id/token_value`。
  - supplied `token_id/token_value` 必须和 route lease 精确匹配。
  - strict GSVA 不再允许 `token_value == 0` 时只校验 token id。

日志证据：

```text
GVA_S3_MAP ...
GVA_ROUTE_DUMP state=active ...
GVA_PATH gva_path=cpu_window op=read ...
GVA_PATH gva_path=cpu_window op=write ...
SIM_DEC_STATS ... read_errors=0 write_errors=0
GVA_STATS ... read_errors=0 write_errors=0
```

用户影响：

- 现在可以通过日志明确区分“guest demo 成功”和“QEMU route 真正安装并被访问命中”。
- 失败场景可以通过 route dump、stats 和 fault marker 定位到 `dcna/p_tag/upi/token/access_flags/cache_policy` 等字段。

当前边界：

- `SIM_DEC_OP_GSVA_MAP_V1/UNMAP_V1/EVENT_V1/QUERY_V1` 已有实现路径，但 query 仍主要覆盖 caps，route/coherence 细粒度查询还未完整产品化。
- 当前 GSVA identity 路径仍部分依赖现有 GVA/OBMM helper 元数据；`obmm_import` 的 `segment_id/epoch/p_tag/token` 还未强制全部来自 kernel segment descriptor。
- basic route-local token rotation 和 `lease_epoch++` 已完成；manager 侧 token 分发、revoke ACK、holder token cache/TLB flush 仍未完成。

## 2.4 ARM MMU / TCG hook 层

已实现能力：

- QEMU ARM TCG slow path 中，`arm_cpu_tlb_fill()` 已在 `get_phys_addr()` 成功后调用 `sim_dec_gva_tcg_translate()`。
- `SIM_GVA_TCG=1` 时会按 VA 查 GVA route，并把 TLB fill 的 `res.f.phys_addr` 改写到 route 对应的 local CPU-window PA。
- `sim_dec_gva_tcg_translate()` 会验证：
  - VA 命中 active GVA route。
  - 写访问不能打到 read-only route。
  - `VA + pte_offset` 能回查到同一条 `ma_table` entry。
- `GVA_TCG_TLB_FLUSH` 用于 map/unmap 的 TLB flush 证据。
- `GVA_TCG_TRANSLATE` 用于证明 ARM TLB fill 确实命中 GVA route。

日志/脚本证据：

- `run_ub_dual_node_gva_direct_test.sh` 在 `SIM_GVA_TCG=1` 时要求：
  - `GVA_TCG_TLB_FLUSH reason=gva_map`
  - `GVA_TCG_TRANSLATE`
- 在 `SIM_GVA_TCG=0` 时要求不能出现 `GVA_TCG_TRANSLATE`，证明默认 legacy path 不被侵入。

用户影响：

- 这证明 ARM MMU 主路径不是纯设计，当前已经有可运行的 TCG hook 骨架。
- 但它仍是显式开关的过渡路径，不是默认生产路径。

当前边界：

- 还没有把 `GSVA_MODE=arm_mmu` 作为默认。
- 还没有 `gsva_arm_mmu_translate()`、GSVA TLB metadata side table、stale epoch fault、token lease 校验等最终实现。

## 2.5 OBMM directory MESI 数据层

已实现能力：

- `DIRECTORY_MESI` cache policy 值为 `4`，已接入 GVA map 校验、CPU window read/write 和 strict DMA read/write。
- QEMU `obmm_coherence.c` 维护：
  - per-node local coherent cache。
  - home directory。
  - pending request table。
- 支持的 coherence 消息包括：
  - `GETS`
  - `GETM`
  - `DATA`
  - `INV`
  - `INV_ACK`
  - `DOWNGRADE`
  - `DOWNGRADE_ACK`
  - `WB`
  - `WB_ACK`
  - `FENCE`
  - `FENCE_ACK`
- `GETS` 支持 shared/read grant。
- `GETM` 会 invalidate owner/sharers，等待 ACK 后授予 writer。
- dirty owner invalidate 会先撤销本地权限，再 writeback；writeback 失败会恢复原 local line，避免丢失唯一 dirty copy。
- `COH_FENCE` 会 drain home range；unmap/shutdown 前会先 fence，再 invalidate 本地 range。
- pending response 按 `(ubc_dev, req_id, peer_cna, msg_type)` 匹配，避免单全局 wait slot 互相覆盖。

4-node 日志证据：

- `guest-linux/aarch64/logs/2026-06-07_22-43-59_coh4_2116`
- `guest-linux/aarch64/logs/2026-06-07_22-44-51_coh4_6891`
- `guest-linux/aarch64/logs/2026-06-07_22-45-09_coh4_17738`

这些日志中可见：

```text
obmm_coh_test: PASS
cache_policy=4
GVA_S3_MAP ... cache_policy=4
OBMM_COH_INV
OBMM_COH_INV_ACK ... status=0
OBMM_COH_WB
OBMM_COH_FENCE_ACK ... status=0
GVA_ROUTE_DUMP state=retired ... cache_policy=4
```

8-node 日志证据：

- `guest-linux/aarch64/logs/2026-06-07_21-46-17_coh8_30276`
- `guest-linux/aarch64/logs/2026-06-07_21-52-21_coh8_28740`

这些日志中可见：

```text
mode=1 size=2097152 cache_policy=4 iterations=2
mode=5 size=2097152 cache_policy=4 iterations=2
obmm_coh_test: PASS
GVA_S3_MAP ... cache_policy=4
OBMM_COH_DATA ... status=0 len=64 grant=1
OBMM_COH_INV
OBMM_COH_INV_ACK ... status=0
OBMM_COH_FENCE_ACK ... status=0
```

用户影响：

- 当前数据层已经不是简单 write-through stub，而是有真实 directory owner/sharer 状态、remote invalidation、writeback 和 fence 的 MESI 数据层。
- GVA/GSVA 的 higher-level semantic coherence 可以复用它作为 PA/data backend，但不能把它等同于最终 GSVA-specific coherence。

当前边界：

- OBMM MESI 的 coherence identity 仍是 data-layer line/range + home/token 语义，不是最终的 GSVA `{segment_id, home_va, epoch, ...}` key。
- GSVA-specific coherence 还需要在 OBMM MESI 上方新增语义状态机。

## 3. 已通过日志按能力归档

## 3.1 两节点 GSVA manager bootstrap

日志目录：

```text
guest-linux/aarch64/logs/2026-06-05_13-49-52_gsva_mgr_4165
```

关键证据：

```text
obmm bootstrap -> ok count=2
manager queues -> ok
bootstrap hello -> ok peers=1
kernel aperture registry -> ok base=0x700000000000 size=0x1000000 generation=0x475356410001
result=done ... registry=kernel-obmm
```

证明点：

- 两节点 manager 间 OBMM bootstrap 成功。
- 双方达成同一 aperture。
- aperture 被注册到 guest kernel/OBMM。

## 3.2 两节点 GSVA identity

日志目录：

```text
guest-linux/aarch64/logs/2026-06-05_13-50-05_gsva_demo_31826
```

关键证据：

```text
fixed export -> ok mem_id=0x1 uba=0x700000000000 token=96
result=done mode=identity role=home ptr=0x700000000000 home_va=0x700000000000 uba=0x700000000000
result=done mode=identity role=peer ptr=0x700000000000 user_va=0x700000000000 uba=0x700000000000
GVA_S3_MAP ... local_va=700000000000 home_va=700000000000 pte_offset=0 uba=700000000000 ... address_profile=2
GVA_ROUTE_DUMP state=active ... address_profile=2
GVA_PATH ... op=read
GVA_PATH ... op=write
SIM_DEC_STATS ... read_timeouts=0 read_errors=0 write_errors=0
GVA_STATS ... read_timeouts=0 read_errors=0 write_errors=0
```

证明点：

- `user_va == uba == home_va` 成立。
- `pte_offset=0` 成立。
- QEMU route 已安装且被 CPU window read/write 命中。
- peer 写回能被 home 观测到。

## 3.3 两节点 generic GVA direct

日志目录：

```text
guest-linux/aarch64/logs/2026-06-05_13-50-15_gva_direct_32219
```

关键证据：

```text
result=done mode=write-read role=home local_va=0x710000000000 home_va=0x720000000000 uba=0xffffffc00000 pte_offset=remote-local
result=done mode=write-read role=peer local_va=0x710000000000 home_va=0x720000000000 uba=0xffffffc00000 pte_offset=0x8effffc00000
SIM_DEC: GVA_MAP success ...
GVA_ROUTE_DUMP state=active ... address_profile=1 pte_offset=8effffc00000
GVA_PATH ... remote_uba=ffffffc00008
SIM_DEC_STATS ... read_errors=0 write_errors=0
GVA_STATS ... read_errors=0 write_errors=0
```

证明点：

- generic GVA 不是 GSVA identity。
- `pte_offset = remote_uba - local_va` 的 side-table route 语义可用。
- peer 访问 `local_va + offset` 时 QEMU 转换到 remote UBA。

## 3.4 四节点 GSVA manager bootstrap

日志目录：

```text
guest-linux/aarch64/logs/2026-06-05_10-12-55_gsva_mgr4_5846
```

关键证据：

```text
obmm bootstrap -> ok count=4
manager queues -> ok
bootstrap hello -> ok peers=3
kernel aperture registry -> ok base=0x700000000000 size=0x1000000 generation=0x475356410004
result=done ... registry=kernel-obmm
```

证明点：

- 四节点 full-mesh manager bootstrap 成功。
- 每个节点都看到 3 个 peer。
- 所有节点使用同一 `base/size/generation`。

## 3.5 四节点 GSVA matrix

日志目录：

```text
guest-linux/aarch64/logs/2026-06-05_10-20-01_gsva_matrix4_25263
```

关键证据：

```text
result=done mode=matrix node=<0..3> node_count=4
GVA_S3_MAP ... address_profile=2 pte_offset=0
GVA_ROUTE_DUMP state=active ... address_profile=2
value_from_node0=...
value_from_last=...
```

证明点：

- 每个节点都能映射其他 `node_count - 1` 个 GSVA slice。
- 每个 remote owner 的 slice 都以 `local_va == home_va == uba == slot_base` 建立 route。
- full-mesh 写入结果能被 owner 观测到。

## 3.6 四节点 GSVA aperture conflict

日志目录：

```text
guest-linux/aarch64/logs/2026-06-05_10-10-04_gsva_mgr4_29378
```

关键证据：

```text
aperture reserve failed errno=17
result=fail
```

证明点：

- GSVA aperture reservation 失败路径可观测。
- 冲突不会被误判为成功。

## 3.7 八节点 GSVA manager / matrix

日志目录：

```text
guest-linux/aarch64/logs/2026-06-07_22-39-37_gsva_mgr8_13812
guest-linux/aarch64/logs/2026-06-07_22-39-16_gsva_matrix8_31247
```

关键证据：

```text
kernel aperture registry -> ok base=0x700000000000 size=0x4000000 generation=0x475356410008
result=done generation=0x475356410008 aperture_base=0x700000000000 aperture_size=0x4000000 registry=kernel-obmm
result=done mode=matrix node=<0..7> node_count=8
GVA_S3_MAP ... address_profile=2 pte_offset=0
GVA_STATS ... remote_reads=165 remote_writes=7 ... read_errors=0 write_errors=0
```

证明点：

- GSVA manager aperture bootstrap 已扩展到 8 节点。
- GSVA matrix 已扩展到 8 节点。
- 节点上的 GVA stats 显示 remote reads/writes 非零且错误计数为 0。

## 3.8 GVA direct matrix

日志目录：

```text
guest-linux/aarch64/logs/2026-06-07_22-40-26_gva_direct_matrix_7594_*
```

已覆盖模式包括：

- `write_read_tcg0`
- `write_read_tcg1`
- `sync_tcg0`
- `unmap_fault_tcg0`
- `unmap_fault_tcg1`
- `dump_tcg0`
- `invalid_cache_tcg0`
- `invalid_dcna_tcg0`
- `invalid_ptag_tcg0`
- `invalid_upi_tcg0`
- `token_mismatch_tcg0`
- `overlap_tcg0`
- `route_overlap_tcg0`
- `read_cache_write_fault_tcg0`
- `write_back_no_sync_tcg0`
- `write_back_sync_tcg0`
- `mrsw_conflict_tcg0`
- `mrsw_read_share_tcg0`
- `mrsw_writer_conflict_tcg0`

关键证据示例：

```text
result=done mode=sync role=home ... sync_done=1
result=done mode=sync role=peer local_va=0x710000000000 home_va=0x720000000000 uba=0xffffffc00000 pte_offset=0x8effffc00000
result=done mode=mrsw-read-share role=peer reader1_import_pa=... reader2_import_pa=...
result=done mode=mrsw-conflict role=peer ... errno=16
result=done mode=mrsw-writer-conflict role=peer ... errno=16
result=done mode=invalid-cache role=peer bad_cache_policy=0xffffffff errno=22
result=done mode=token-mismatch role=peer fault_injected=1
GVA_S3_MAP ... address_profile=1 pte_offset=...
GVA_ROUTE_DUMP state=active ...
```

证明点：

- generic GVA happy path、sync、dump、unmap fault、TCG hook、invalid metadata、token mismatch、route overlap、MRSW reader sharing 和 writer conflict 都有脚本级验证。
- TCG path 通过 `SIM_GVA_TCG=1` 单独开启，不影响默认 `SIM_GVA_TCG=0` 回归。

## 3.9 OBMM directory MESI 4/8-node

日志目录：

```text
guest-linux/aarch64/logs/2026-06-07_22-43-59_coh4_2116
guest-linux/aarch64/logs/2026-06-07_22-44-51_coh4_6891
guest-linux/aarch64/logs/2026-06-07_22-45-09_coh4_17738
guest-linux/aarch64/logs/2026-06-07_21-46-17_coh8_30276
guest-linux/aarch64/logs/2026-06-07_21-52-21_coh8_28740
```

关键证据：

```text
mode=1 size=2097152 cache_policy=4 iterations=2
mode=5 size=2097152 cache_policy=4 iterations=2
obmm_coh_test: PASS
GVA_S3_MAP ... cache_policy=4
GVA_ROUTE_DUMP state=active ... cache_policy=4
OBMM_COH_DATA ... status=0 len=64 grant=1
OBMM_COH_INV
OBMM_COH_INV_ACK ... status=0
OBMM_COH_WB
OBMM_COH_FENCE_ACK ... status=0
GVA_ROUTE_DUMP state=retired ... cache_policy=4
```

证明点：

- `DIRECTORY_MESI` 不是仅控制面字段，QEMU 数据层确实进入 OBMM coherence API。
- 读共享、写获取、失效、writeback 和 fence 都有 QEMU 日志证据。
- 4/8 节点都能跑到 `obmm_coh_test: PASS`。

## 4. 当前阶段能力矩阵

| 能力 | 当前状态 | 证据 |
| --- | --- | --- |
| GSVA aperture bootstrap | 已实现并通过 2/4/8 节点日志 | `gva_manager result=done registry=kernel-obmm` |
| Kernel aperture registry | 已实现 | `kernel aperture registry -> ok`、`/proc/obmm/gsva_aperture` |
| `MAP_GSVA` flag 消费 | 已实现 | `mmap_consume_gsva_flag()`、`OBMM_MMAP_FLAG_GSVA` |
| 普通 mmap 防重叠 | 已实现 | `overlaps active GSVA aperture` rejection |
| GSVA identity route | 已实现并通过 2/4/8 节点日志 | `address_profile=2 pte_offset=0` |
| Generic GVA route | 已实现并通过 direct/matrix 日志 | `address_profile=1 pte_offset!=0` |
| GVA route stats | 已实现 | `SIM_DEC_STATS`、`GVA_STATS` |
| GVA metadata fault injection | 已实现并通过 matrix 日志 | invalid cache/dcna/ptag/upi/token/overlap modes |
| GVA MRSW ownership registry | 已实现为当前阶段的 conflict/reject 语义 | `mrsw-read-share`、`mrsw-conflict`、`mrsw-writer-conflict` |
| ARM TCG GVA probe | 已实现为显式开关 | `SIM_GVA_TCG=1`、`GVA_TCG_TRANSLATE` |
| OBMM directory MESI | 已实现并通过 4/8 节点日志 | `cache_policy=4`、`OBMM_COH_*`、`obmm_coh_test: PASS` |
| GSVA-specific coherence | 部分实现 | `gsva_route/gsva_coherence` 已接入 map/event，ReadAcquire/WriteAcquire token validation 已验证；retire/reuse 和 ACK 恢复仍未完整 |
| GSVA writer invalidation | 已实现并通过四节点日志 | `writer_inv`：`ReadAcquire I->S`、`ReadAcquire S->S`、`WriteAcquire S->M pending inv`、`WriteAcquire S->M` |
| Event retire route tombstone | 已实现并通过两节点日志 | `retire_event`：retire 后 `GSVA_UNMAP ... tombstone=yes`，post-retire ReadAcquire 返回 retired |
| Stale epoch remap rejection | 已实现并通过两节点日志 | `stale_remap`：retire 后 epoch=1 remap 被 `GSVA_ERR_STALE_EPOCH` 拒绝 |
| Segment reuse / distributed retire transaction | 未完成 | 仍需 new epoch reuse commit、ACK/timeout/TLB flush 事务化 |
| Token lease v1 acquire validation | 已实现并通过两节点日志 | `token_denied`：valid ReadAcquire PASS，bad ReadAcquire/WriteAcquire 返回 `GSVA_ERR_TOKEN_DENIED` |
| Basic token rotation | 已实现并通过两节点日志 | `token_rotate`：TokenChange 后 `lease_epoch=2`，old token denied，新 token 使用同一 key 通过 |
| Token revoke/ACK 产品化 | 未完成 | 仍需 manager 分发、revoke ACK、holder token cache/TLB flush |
| ARM MMU 默认路径 | 未完成 | 当前是 `SIM_GVA_TCG` transition hook，默认仍非最终 `arm_mmu` |

## 5. 与最终架构目标的差距

当前实现已经证明：

- GSVA address identity 可行。
- 多节点 GSVA manager bootstrap 可行。
- GVA route side table 可驱动 QEMU 访问路径。
- ARM TLB fill 中探测 GVA route 可行。
- OBMM directory MESI 可作为数据层 coherence backend。

当前实现尚未证明：

- 以 `gsva_key_v1` 为唯一 semantic identity 的 coherence。
- segment retire/reuse 与 route/coherence/TLB flush 的原子事务。
- `GSVA_MODE=arm_mmu` 作为默认路径。
- stale epoch、retired tombstone、token revoke ACK、cache policy change 的完整 ACK/timeout/recovery。

当前实现已经新增证明：

- ReadAcquire/WriteAcquire 前的 active route GSVA token lease 校验。
- `token_value` 错误时 QEMU 返回 `GSVA_ERR_TOKEN_DENIED`，guest ioctl 能收到该语义错误。
- TokenChange 可在同一 `gsva_key_v1` 上提交新的 `token_value` 和 `lease_epoch`；旧 token 随后返回 `GSVA_ERR_TOKEN_DENIED`，新 token 通过。
- Event Retire 可把 active route 迁移到 tombstone，删除 CPU window，并让后续同一 key 的 acquire 返回 retired。
- 四节点 writer invalidation 可从 shared reader 状态进入 writer invalidation，再授予 modified writer。
- Retired tombstone 会拒绝同一 epoch 的 stale remap，避免旧 key 静默复活。

2026-06-09 token acquire 验证证据：

```text
run_id=guest-linux/aarch64/logs/2026-06-09_00-09-33_gsva_coh_30739
nodeA_guest.log: [gsva_coh_test] TEST: GSVA ReadAcquire/WriteAcquire token v1 validation
nodeA_guest.log: [gsva_coh_test] verdict=PASS
nodeA_qemu.log:  GSVA_COH: ReadAcquire I->S cna=50370 segment_id=0x1
nodeA_qemu.log:  GSVA_COH: ReadAcquire token denied: cna=50370 token_id=96 rc=-4
nodeA_qemu.log:  GSVA_COH: WriteAcquire token denied: cna=50370 token_id=96 rc=-4
nodeB_guest.log: [gsva_coh_test] verdict=PASS
nodeB_qemu.log:  GSVA_COH: ReadAcquire I->S cna=50386 segment_id=0x1
nodeB_qemu.log:  GSVA_COH: ReadAcquire token denied: cna=50386 token_id=96 rc=-4
nodeB_qemu.log:  GSVA_COH: WriteAcquire token denied: cna=50386 token_id=96 rc=-4
```

2026-06-09 token rotation 验证证据：

```text
run_id=guest-linux/aarch64/logs/2026-06-09_00-15-27_gsva_coh_10949
nodeA_guest.log: [gsva_coh_test] TEST: GSVA token rotation preserves key identity
nodeA_guest.log: [gsva_coh_test] verdict=PASS
nodeA_qemu.log:  GSVA_COH: ReadAcquire I->S cna=50370 segment_id=0x1
nodeA_qemu.log:  GSVA_ROUTE: token rotated segment_id=0x1 token_id=96 lease_epoch=2
nodeA_qemu.log:  GSVA_COH: ReadAcquire token denied: cna=50370 token_id=96 rc=-4
nodeA_qemu.log:  GSVA_COH: ReadAcquire S->S cna=50370 segment_id=0x1
nodeB_guest.log: [gsva_coh_test] verdict=PASS
nodeB_qemu.log:  GSVA_COH: ReadAcquire I->S cna=50386 segment_id=0x1
nodeB_qemu.log:  GSVA_ROUTE: token rotated segment_id=0x1 token_id=96 lease_epoch=2
nodeB_qemu.log:  GSVA_COH: ReadAcquire token denied: cna=50386 token_id=96 rc=-4
nodeB_qemu.log:  GSVA_COH: ReadAcquire S->S cna=50386 segment_id=0x1
```

2026-06-09 event retire 验证证据：

```text
run_id=guest-linux/aarch64/logs/2026-06-09_00-21-31_gsva_coh_12940
nodeA_guest.log: [gsva_coh_test] TEST: GSVA event retire tombstones route
nodeA_guest.log: [gsva_coh_test] verdict=PASS
nodeA_qemu.log:  GSVA_COH: ReadAcquire I->S cna=50370 segment_id=0x1
nodeA_qemu.log:  GSVA_RETIRE: segment_id=0x1 home_va=0x700002400000 epoch=1 RETIRED
nodeA_qemu.log:  GSVA_UNMAP: cpu_window removed from pa=60000000000
nodeA_qemu.log:  GSVA_UNMAP: map_id=1 segment_id=0x1 home_va=0x700002400000 epoch=1 tombstone=yes
nodeA_qemu.log:  GSVA_COH: ReadAcquire retired segment_id=0x1 cna=50370
nodeB_guest.log: [gsva_coh_test] verdict=PASS
nodeB_qemu.log:  GSVA_COH: ReadAcquire I->S cna=50386 segment_id=0x1
nodeB_qemu.log:  GSVA_RETIRE: segment_id=0x1 home_va=0x700002000000 epoch=1 RETIRED
nodeB_qemu.log:  GSVA_UNMAP: cpu_window removed from pa=60000000000
nodeB_qemu.log:  GSVA_UNMAP: map_id=1 segment_id=0x1 home_va=0x700002000000 epoch=1 tombstone=yes
nodeB_qemu.log:  GSVA_COH: ReadAcquire retired segment_id=0x1 cna=50386
```

2026-06-09 四节点 writer invalidation 验证证据：

```text
run_id=guest-linux/aarch64/logs/2026-06-09_00-25-17_gsva_coh4_15946
nodeA_guest.log: [gsva_coh_test] TEST: GSVA writer invalidates shared readers
nodeA_guest.log: [gsva_coh_test] verdict=PASS
nodeA_qemu.log:  GSVA_COH: ReadAcquire I->S cna=50370 segment_id=0x1
nodeA_qemu.log:  GSVA_COH: ReadAcquire S->S cna=50402 segment_id=0x1
nodeA_qemu.log:  GSVA_COH: WriteAcquire S->M pending inv cna=50370 waiting_for=0x400000000 seq=2
nodeA_qemu.log:  GSVA_COH: WriteAcquire S->M cna=50370 segment_id=0x1
nodeB_guest.log: [gsva_coh_test] verdict=PASS
nodeC_guest.log: [gsva_coh_test] verdict=PASS
nodeD_guest.log: [gsva_coh_test] verdict=PASS
```

2026-06-09 stale remap 验证证据：

```text
run_id=guest-linux/aarch64/logs/2026-06-09_00-28-44_gsva_coh_25343
nodeA_guest.log: [gsva_coh_test] TEST: GSVA stale epoch remap rejected after retire
nodeA_guest.log: OBMM: sim decoder map callback failed: -EIO.
nodeA_guest.log: [gsva_coh_test] verdict=PASS
nodeA_qemu.log:  GSVA_UNMAP: map_id=1 segment_id=0x1 home_va=0x700003400000 epoch=1 tombstone=yes
nodeA_qemu.log:  GSVA_ROUTE: stale epoch on tombstone: new epoch=1 <= old epoch=1
nodeA_qemu.log:  GSVA_MAP: failed: GSVA_ERR_STALE_EPOCH
nodeB_guest.log: [gsva_coh_test] verdict=PASS
nodeB_qemu.log:  GSVA_UNMAP: map_id=1 segment_id=0x1 home_va=0x700003000000 epoch=1 tombstone=yes
nodeB_qemu.log:  GSVA_ROUTE: stale epoch on tombstone: new epoch=1 <= old epoch=1
nodeB_qemu.log:  GSVA_MAP: failed: GSVA_ERR_STALE_EPOCH
```

关键区别：

```text
已完成：
  GSVA identity address + GVA route + OBMM MESI data-layer coherence

未完成：
  GSVA-keyed semantic coherence + lifecycle transaction + default ARM MMU mode
```

## 6. 对用户/产品层面的影响

当前阶段已经让用户可以做的事：

- 在 2/4/8 节点 guest 中使用同一 GSVA aperture。
- 在 GSVA identity demo/matrix 中用同一个 virtual address 访问 remote OBMM-backed segment。
- 在 generic GVA 中验证非 identity VA 到 remote UBA 的 offset 映射。
- 用日志定位 GVA route 的 `dcna/token/p_tag/link/lane/access_flags/cache_policy`。
- 用 `DIRECTORY_MESI` 路径验证跨节点 read/write/fence/writeback。
- 用 negative tests 验证 invalid route metadata 不会被静默接受。

当前阶段还不能承诺给用户的事：

- “默认所有 GSVA 访问都经过 ARM MMU 主路径”。
- “GSVA coherence 已经按 segment/epoch/token 全事务语义完成”。
- “segment reuse 后 stale mapping 一定被 route/coherence/TLB 事务化拒绝”。
- “token revoke ACK / holder token cache / TLB flush 已经产品化”。
- “cache_policy change 已经按 old-key revoke + new-key map 完整处理”。

## 7. 下一阶段建议

推荐按以下顺序推进，不建议直接跳到大规模重构：

1. Protocol freeze implementation
   - `guest-linux/kernel_ub/include/uapi/ub/gsva.h` 已落地。
   - `OBMM_CMD_GSVA_ALLOC_SEGMENT/QUERY_SEGMENT/RETIRE_SEGMENT` descriptor ABI 已有两节点 `segment_abi` 验证。
   - 下一步是让 `obmm_import` 的 `gsva_key_v1` 字段完全来自 descriptor，而不是 demo metadata 拼装。

2. GSVA route/token v1
   - `gsva_route.c/h` 已存在并接入 `SIM_DEC_OP_GSVA_MAP_V1`。
   - active route lease 的 ReadAcquire/WriteAcquire token validation 已验证。
   - basic token rotation、`lease_epoch` commit、旧 token negative test 已验证。
   - 下一步是实现 manager token 分发、revoke ACK、holder token cache/TLB flush。
   - `obmm_import` 仍需强制从 segment descriptor 生成 `gsva_key_v1`，避免 demo metadata 自行拼装。

3. GSVA-specific coherence
   - 新建 `gsva_coherence.c/h`。
   - 复用现有 UBC msgq / UB Link transport。
   - 新增 `UBC_MSG_SUB_GSVA_COH_*`，不要复用 OBMM line-level payload。
   - 先实现 single-key read/write/invalidate，再扩展 retire/reuse。

4. ARM MMU default path
   - 在 `arm_cpu_tlb_fill()` 中从 `sim_dec_gva_tcg_translate()` 过渡到 `gsva_arm_mmu_translate()`。
   - 引入 TLB metadata side table。
   - `GSVA_MODE=arm_mmu` 下禁止 `GVA_TCG_TRANSLATE` 出现在 data path。

5. Lifecycle transaction
   - event retire 已绑定 route removal、coherence retire、PA-MESI fence/invalidate best-effort、CPU window remove、tombstone。
   - 下一步是 segment reuse epoch、跨节点 ACK/timeout、TLB flush。
   - timeout 必须暴露为 stable error，不允许静默提交。

## 8. 推荐保留的回归矩阵

必须保留：

```bash
./guest-linux/aarch64/scripts/run_ub_dual_node_gsva_manager_bootstrap.sh
./guest-linux/aarch64/scripts/run_ub_dual_node_gsva_demo.sh
./guest-linux/aarch64/scripts/run_ub_dual_node_gva_direct_matrix.sh
GSVA_TEST_MODE=segment_abi ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_lifecycle_test.sh
GSVA_TEST_MODE=token_denied ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_coh_test.sh
GSVA_TEST_MODE=token_rotate ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_coh_test.sh
GSVA_TEST_MODE=retire_event ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_coh_test.sh
GSVA_TEST_MODE=stale_remap ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_coh_test.sh
GSVA_TEST_MODE=writer_inv ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
./guest-linux/aarch64/scripts/run_ub_four_node_gsva_manager_bootstrap.sh
./guest-linux/aarch64/scripts/run_ub_four_node_gsva_matrix_demo.sh
COH_TEST_MODE=multi_reader ./guest-linux/aarch64/scripts/run_ub_four_node_obmm_coh_test.sh
COH_TEST_MODE=writer_inv ./guest-linux/aarch64/scripts/run_ub_four_node_obmm_coh_test.sh
COH_TEST_MODE=multi_reader ./guest-linux/aarch64/scripts/run_ub_eight_node_obmm_coh_test.sh
COH_TEST_MODE=writer_inv ./guest-linux/aarch64/scripts/run_ub_eight_node_obmm_coh_test.sh
```

新增 GSVA-specific coherence 后，必须再补：

```bash
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_two_node_gsva_identity_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_identity_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 ./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_identity_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=writer_inv ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_coh_test.sh
GSVA_MODE=arm_mmu GSVA_STRICT=1 GSVA_TEST_MODE=retire_reuse ./guest-linux/aarch64/scripts/run_ub_four_node_gsva_lifecycle_test.sh
```

## 9. 最终阶段判定

当前阶段可标记为：

```text
GVA route simulation: PASS
GSVA address identity: PASS
GSVA aperture/kernel protection: PASS
Generic GVA fault/ownership matrix: PASS
ARM TCG route probe: PASS as transition path
OBMM directory MESI data layer: PASS
GSVA segment descriptor ABI: PASS
GSVA token acquire validation: PASS
GSVA basic token rotation: PASS
GSVA event retire tombstone: PASS
GSVA four-node writer invalidation: PASS
GSVA stale epoch remap rejection: PASS
GSVA-specific coherence: PARTIAL
ARM MMU default GSVA path: NOT YET
Segment new-epoch reuse route/coherence/TLB transaction binding: NOT YET
Token revoke/ACK productization: NOT YET
```

因此，当前最准确的项目状态是：

```text
已完成一个稳定的 GVA/GSVA 地址、GSVA segment descriptor ABI、GSVA token acquire/rotation、event retire tombstone、四节点 writer invalidation、stale epoch remap rejection 与 OBMM MESI 数据层阶段。
下一阶段应从“能跑”转向“语义收敛”：把 GSVA key、token revoke ACK、new-epoch reuse、route/coherence/TLB 事务和 ARM MMU 默认路径合成最终架构。
```
