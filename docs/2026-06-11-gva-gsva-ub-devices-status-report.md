# GVA / GSVA / UB NPU / UB SSD 当前状态报告

仓库：`/Volumes/repos/ub_sim`

依据：

- 当前代码实现。
- `guest-linux/aarch64/logs` 下 2026-06-11 的 NPU/SSD GSVA 2/4/8 节点运行日志。
- `docs/sim_gva_gsva_run_report.md` 中归档的 2026-06-05 GVA/GSVA 两节点与四节点运行日志。
- `docs/sim_gva_gsva_obmm_mesi_stage_status_summary.md` 中归档的 2026-06-09 GSVA、OBMM MESI、ARM MMU default GSVA path 和 active UB Link coherence 运行日志。

## 1. 结论

当前状态可以判断为：GVA/GSVA 地址与一致性底座已经进入多节点可运行 V1；`ub_npu` 和 `ub_ssd` 已经接入这个底座，并通过 2/4/8 节点端到端日志验证。

具体判断：

1. GVA 已验证普通 non-identity route：`local_va/home_va/uba` 可以不相等，QEMU 侧通过 `pte_offset` 完成地址转换，读写进入 `GVA_PATH` 和 `SIM_DEC_STATS/GVA_STATS`。
2. GSVA 已验证 identity route：`user_va == home_va == uba`、`pte_offset=0`、`address_profile=2` 在 2/4/8 节点场景成立；后续 coherence、token、epoch、retire、timeout、TLB flush 已有独立运行日志闭环。
3. `ub_npu` 不是纯 guest mock：guest 通过 `UB_NPU_SUBMIT/UB_NPU_WAIT` 提交命令，QEMU 设备侧执行 NOOP、MEMCOPY、FILL、VECTOR_ADD_U32、CHECKSUM64，并通过 `UB_DEV_GSVA` 访问 GSVA segment。
4. `ub_ssd` 不是普通内存 memcpy demo：guest 通过 `UB_SSD_SUBMIT/UB_SSD_WAIT` 和 snapshot ioctl 操作块对象，QEMU 设备侧维护 block record、version/seal/tombstone/quarantine 状态，并通过 `UB_DEV_GSVA` 访问 GSVA payload。
5. 最新 2026-06-11 运行矩阵中，NPU 和 SSD 在 2/4/8 节点全部 `verdict=PASS`。这是当前报告最直接的验收依据。

用户影响：

- 对上层使用者来说，当前已经可以把 GVA/GSVA 当作多 guest OS 之间的共享虚拟地址与一致性实验底座使用。
- NPU/SSD 设备已经能作为“挂在 UB fabric 上、使用 GSVA 读写内存”的真实模拟设备参与测试。
- 但这仍是 V1 simulator target，不是生产级远端设备服务：设备命令提交目前仍是每节点本地 device instance，跨节点通过 GSVA data/coherence 发生，不是把设备命令本身远程投递到另一个节点执行。

## 2. 最新运行矩阵

| 场景 | 日志目录 | 结果 |
| --- | --- | --- |
| 2-node NPU GSVA | `guest-linux/aarch64/logs/2026-06-11_12-57-36_npu_gsva_test_24744` | nodeA/nodeB：`14/14 passed`，`verdict=PASS` |
| 2-node SSD GSVA | `guest-linux/aarch64/logs/2026-06-11_12-57-56_ssd_gsva_test_5423` | nodeA/nodeB：`15/15 passed`，`verdict=PASS` |
| 4-node NPU GSVA | `guest-linux/aarch64/logs/2026-06-11_12-58-37_npu_gsva_test_4_2419` | nodeA-D：`40/40 passed`，`verdict=PASS` |
| 4-node SSD GSVA | `guest-linux/aarch64/logs/2026-06-11_12-59-15_ssd_gsva_test_4_22099` | nodeA-D：`41/41 passed`，`verdict=PASS` |
| 8-node NPU GSVA | `guest-linux/aarch64/logs/2026-06-11_13-00-34_npu_gsva_test_8_9114` | nodeA-H：`92/92 passed`，`verdict=PASS` |
| 8-node SSD GSVA | `guest-linux/aarch64/logs/2026-06-11_13-02-20_ssd_gsva_test_8_23532` | nodeA-H：`93/93 passed`，`verdict=PASS` |

这些数字的含义不是“跑了一个 happy path”。8 节点下，NPU 每个节点对 7 个 peer 执行 13 类核心/异常路径，合计 92 项；SSD 每个节点对 7 个 peer 执行 13 类核心/异常路径加 2 个本地 backend/stat 路径，合计 93 项。

## 3. GVA 状态

已实现并验证：

- Guest OBMM import/export 能进入 QEMU SIM_DEC/GVA route，形成 `GVA_S3_MAP` 和 `GVA_ROUTE_DUMP`。
- 普通 GVA direct profile 已验证 non-identity mapping：peer 侧 `local_va`、home 侧 `home_va`、OBMM `uba` 可以不同，QEMU 通过非零 `pte_offset` 转换地址。
- CPU window read/write 命中 `GVA_PATH`，并进入 `SIM_DEC_STATS` 与 `GVA_STATS` 计数。
- 2026-06-05 两节点 GVA direct 日志中，peer `address_profile=1`、`pte_offset=0x8effffc00000`，最终读写值一致。

当前边界：

- GVA 当前主要承担普通 route 与 CPU/device data path 的地址转换底座。
- 完整 cache-coherence 语义主要在 GSVA route/coherence 层推进；不要把普通 GVA direct 等价理解为 GSVA identity coherence。

用户影响：

- 需要 non-identity virtual address 转换的实验可以继续用 GVA direct。
- 如果需求是“多个节点看到完全同一个虚拟地址并带 token/epoch/retire 语义”，应优先使用 GSVA。

## 4. GSVA 状态

已实现并验证：

- GSVA manager 能 bootstrap OBMM manager queue，协商统一 aperture，并注册到 guest kernel/OBMM。
- GSVA identity profile 已验证 `user_va == home_va == uba`、`pte_offset=0`、`address_profile=2`。
- Guest kernel 已接入 GSVA aperture registry、`MAP_GSVA` 保护、segment descriptor ABI、`OBMM_CMD_GSVA_EVENT_V1`。
- QEMU 已接入 GSVA route/coherence：ReadAcquire、WriteAcquire、TokenChange、Retire、InvAck、Retry、Query、TLB flush。
- ARM MMU default GSVA path 已通过 2/4/8 节点验收；验收日志要求出现 `GSVA_TLB: lookup` 和 `GSVA_COH:`，并拒绝 `GVA_TCG_TRANSLATE` 回退。
- Active UB Link GSVA remote coherence 已验证 invalidate/writeback/downgrade/token/fence/retire ACK。
- 最新 NPU/SSD 日志证明设备侧 GSVA 也进入同一 coherence path：QEMU 可见 `UB_DEV_GSVA: ReadAcquire ok`、`write ok`、`fence ok`，并带设备 CNA。

关键实现入口：

- Guest 测试通过 `OBMM_CMD_GSVA_APERTURE_REGISTER`、`OBMM_CMD_GSVA_ALLOC_SEGMENT`、`OBMM_CMD_GSVA_QUERY_SEGMENT` 建立 segment。
- NPU/SSD 测试通过 `OBMM_CMD_GSVA_EVENT_V1` 注入 token rotate、retire、timeout 等事件。
- QEMU 侧通过 GSVA route/coherence 模块执行 device CNA 参与的 ReadAcquire/WriteAcquire/Fence。

当前边界：

- GSVA V1 已覆盖地址、route、coherence、token、epoch、retire、timeout、TLB flush。
- 更高层的统一 lifecycle coordinator 仍然分散在测试工具、manager 和设备测试程序中；后续如果要给产品层使用，应收敛成更稳定的管理面。

用户影响：

- 对模拟器用户来说，GSVA 已经能支撑“多节点同虚拟地址 + coherence + attached device DMA-like access”的实验。
- 对上层产品来说，仍需要封装生命周期，否则用户要理解过多 segment/token/epoch 细节。

## 5. UB NPU 状态

已实现能力：

- UAPI 定义了 `UB_NPU_SUBMIT`、`UB_NPU_WAIT`、命令描述符和 completion 描述符。
- QEMU `ub_npu` 支持 NOOP、MEMCOPY、FILL、VECTOR_ADD_U32、CHECKSUM64。
- QEMU 设备执行路径通过 GSVA ReadAcquire/WriteAcquire、device read/write、fence 访问 guest segment。
- completion 日志 `UB_NPU_CPL` 记录 opcode、status、bytes_read、bytes_written、token_denied、stale_epoch、retired_segment、coh_timeout。
- guest 测试覆盖 NOOP 控制路径、MEMCOPY、FILL、VECTOR_ADD_U32、CHECKSUM64、descriptor rejection、truncate、token denied、stale epoch、token rotate、coherence timeout injection、segment retired。
- guest 测试还输出 Lingqu 风格执行产物：`LINGQU_BLOCK_WRITE payload_kind=npu-output` 和 `/lingqu/npu/execution-artifacts/...` manifest。

最新运行状态：

- 2-node：每节点 `14/14 passed`。
- 4-node：每节点 `40/40 passed`。
- 8-node：每节点 `92/92 passed`。
- 8-node QEMU 日志中可见 `UB_NPU: realized cna=0x1c4c1000`，后续 `UB_DEV_GSVA` 日志使用同一 NPU device CNA 进入 GSVA coherence。

当前边界：

- NPU 当前是功能模拟设备，不是性能模型；没有真实调度队列、算子图编译器或异步多队列执行语义。
- 跨节点能力来自 GSVA data/coherence，而不是远程提交 NPU 命令到 peer 节点的 NPU。

用户影响：

- 可以用它验证“设备读取 GSVA input、写回 GSVA output、受 token/epoch/retire 限制”的行为。
- 不能用它评估真实 NPU 性能、队列争用或跨节点设备调度策略。

## 6. UB SSD 状态

已实现能力：

- UAPI 定义了 `UB_SSD_SUBMIT`、`UB_SSD_WAIT`、`UB_SSD_EXPORT_SNAPSHOT`、`UB_SSD_IMPORT_SNAPSHOT`。
- QEMU `ub_ssd` 支持 BLOCK_WRITE、BLOCK_READ、BLOCK_SEAL、BLOCK_TOMBSTONE、FLUSH、STAT、EXPORT_SNAPSHOT、IMPORT_SNAPSHOT。
- QEMU 设备通过 GSVA ReadAcquire/WriteAcquire、device read/write、fence 访问 payload。
- SSD block record 维护 committed、sealed、tombstoned、quarantined 等状态。
- 版本冲突、seal 后写入、tombstone 后读写、checksum mismatch、missing block、corrupted snapshot import 等错误路径已纳入 guest 测试。
- stats/backend MMIO 已拆分：stats 从 `SSD_STATS_OFF=0x520` 开始，backend profile/status 保持独立，避免 ABI 混叠。
- guest 测试输出 Lingqu 风格块对象与 manifest：`LINGQU_BLOCK_WRITE`、`LINGQU_BLOCK_READ`、`/lingqu/block/objects/...`。

最新运行状态：

- 2-node：每节点 `15/15 passed`。
- 4-node：每节点 `41/41 passed`。
- 8-node：每节点 `93/93 passed`。
- 8-node QEMU 日志中可见 `UB_SSD: realized cna=0x1c4c2000 ... backend=memory`，后续 `UB_DEV_GSVA` 日志使用同一 SSD device CNA 进入 GSVA coherence。
- 8-node SSD 日志末尾 `SIM_DEC_STATS/GVA_STATS` 中 remote read/write 计数非零，`read_timeouts=0`、`read_errors=0`、`write_errors=0`。

当前边界：

- 当前 backend profile 是 `memory`。这能验证块语义、版本语义和 GSVA data path，但不是 host-file/AIO/durable storage。
- 当前 SSD 是 Lingqu block-object 语义，不是裸 LBA 盘；这对对象存储/DFS 实验是合理的，但不应宣传成通用 NVMe/SSD 仿真。
- Snapshot 已有 export/import 和 corrupted import rejection，但还不是持久化崩溃恢复模型。

用户影响：

- 可以用它验证“块对象通过 GSVA payload 读写，并受 version/seal/tombstone/token/epoch/coherence 约束”的行为。
- 不能用它评估真实 SSD 延迟、磨损、队列深度、持久化崩溃一致性或 NVMe 协议兼容性。

## 7. 关键证据链

实现证据：

- NPU guest app：`guest-linux/aarch64/apps/npu_gsva_test/npu_gsva_test.c`
- NPU QEMU device：`vendor/qemu_8.2.0_ub/hw/ub/ub_npu.c`
- NPU UAPI：`guest-linux/kernel_ub/include/uapi/ub/ub_npu.h`
- SSD guest app：`guest-linux/aarch64/apps/ssd_gsva_test/ssd_gsva_test.c`
- SSD QEMU device：`vendor/qemu_8.2.0_ub/hw/ub/ub_ssd.c`
- SSD UAPI：`guest-linux/kernel_ub/include/uapi/ub/ub_ssd.h`
- GVA/GSVA 历史运行报告：`docs/sim_gva_gsva_run_report.md`
- GSVA/OBMM MESI 阶段总结：`docs/sim_gva_gsva_obmm_mesi_stage_status_summary.md`

运行日志证据：

- NPU 8-node：`guest-linux/aarch64/logs/2026-06-11_13-00-34_npu_gsva_test_8_9114`
- SSD 8-node：`guest-linux/aarch64/logs/2026-06-11_13-02-20_ssd_gsva_test_8_23532`
- NPU/SSD 2-node 与 4-node：见第 2 节矩阵。

代表性日志信号：

- `UB_NPU: realized cna=0x1c4c1000`
- `UB_SSD: realized cna=0x1c4c2000 ... backend=memory`
- `UB_DEV_GSVA: ReadAcquire ok`
- `UB_DEV_GSVA: WriteAcquire ok`
- `UB_DEV_GSVA: fence ok`
- `GSVA_COH: ReadAcquire`
- `GSVA_TLB: lookup`
- `UB_NPU_CPL: ... status=0`
- `UB_SSD_CPL: ... status=0`
- `SIM_DEC_STATS ... read_timeouts=0 read_errors=0 write_errors=0`
- `GVA_STATS ... read_timeouts=0 read_errors=0 write_errors=0`

## 8. 下一步

如果目标是把当前 V1 simulator 往产品化接口推进，优先级应该是：

1. 收敛 GSVA segment lifecycle：把 alloc/import/token/retire/cleanup 从测试程序里的流程沉淀成统一 CLI 和库接口。
2. 定义远程 device command 语义：明确是否需要“nodeA 提交命令到 nodeB 的 NPU/SSD 执行”，如果需要，应设计 UB device command carrier，而不是继续只依赖 GSVA data path。
3. SSD durable backend：在 memory backend 之外增加 host-file 或 AIO backend，并定义 snapshot 与 crash consistency 的真实边界。
4. NPU execution model：如果要服务推理/算子实验，应增加队列、异步 completion、batch/graph 描述符；否则保持当前功能模拟定位，不引入伪性能指标。
5. 把 2/4/8 NPU/SSD 矩阵纳入常规回归门禁，避免后续 GVA/GSVA 改动破坏 device CNA coherence path。
