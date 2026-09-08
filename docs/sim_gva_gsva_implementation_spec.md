# GVA / GSVA 实现规格与状态

更新：2026-09-08。本文件说明当前代码的落点、协议边界、验证依据和剩余缺口。
总体架构见 [GVA 设计](sim_gva_simulation_design.md)，
allocator 与生命周期见 [GSVA 设计](sim_gsva_shared_virtual_address_design.md)。

## 1. 审计基线

| 仓库 | 本次核对的提交 |
| --- | --- |
| ub_sim | `39576eb` |
| kernel_ub | `85fb35d` |
| QEMU | `f201ebe` |
| mem_service | `e4a8a58` |
| libobmm | `53011ee` |

这些是代码核对的基线。历史试验所用的提交、机器和配置另有记录。
2026-09-07 W5 回归使用的具体组合见原调查报告，不能把那次测试
改写成上述全部最新 gitlink 的端到端验收。

## 2. 图形规格

![架构、生命周期、导入视图刷新 SVG](gva_gsva_current_architecture.svg)

![基于 SVG 架构关系生成的空间设备插画](gva_gsva_current_architecture.generated.png)

[SVG 原图](gva_gsva_current_architecture.svg) ·
[PNG 展示图](gva_gsva_current_architecture.generated.png)

SVG 中 01 对应职责与数据路径，02 对应 segment 生命周期，03 对应
mem_service reader。PNG 重新构图展示 01 的空间职责关系，省略后两项时序。
图像生成工具、原生尺寸和模型版本核验限制见
[GVA 图像说明](sim_gva_simulation_design.md#2-架构图)。
图形表达使用 SVG；代码块只放可执行命令或数据定义，不使用 ASCII 流程图。

## 3. 已实现能力与落点

| 能力 | 当前实现 | 状态与限制 |
| --- | --- | --- |
| 物理内存池分配 | kernel `ubmempool_allocator.c`、`obmm_export_from_pool.c` | 有实现；与全局 VA 分配分开 |
| aperture 与 segment 管理 | `gva_manager.c`、`obmm_core.c`、GSVA UAPI | bootstrap、alloc/query/retire、descriptor 分发已有代码 |
| 严格同址映射 | `obmm_shm_dev.c`、`obmm_common.h` | fixed UBA、aperture 范围与固定映射检查 |
| GVA 路由 | QEMU `ub_ubc.c` 与 GSVA route 模块 | 保留 SIM_DEC backend 辅助机制 |
| ARM MMU 入口 | `target/arm/tcg/tlb_helper.c` | 受 feature/mode 控制，不能推定所有 W5 默认启用 |
| key 身份和范围 | QEMU `gsva_key.c` | version、identity、epoch、containment 与错误码 |
| token 与 route 生命周期 | QEMU `gsva_route.c` | active/revoking/revoked、lease epoch 与撤销事务 |
| GSVA coherence | QEMU `gsva_coherence.c` | acquire、invalidate/downgrade/writeback/fence/retire/token ACK |
| 服务对象 GSVA 描述符 | mem_service `mem_service_gsva_access.h` | owner/范围/溢出检查；当前 helper 保留固定 profile 值 |
| UB-SSD block backend | mem_service backend ref、I/O 适配与 guest device | storage identity 与 GSVA buffer descriptor 分离 |
| terminal token 可见性 | mem_service `e4a8a58` | 与 range reader 共用 metadata/payload refresh；已有行为与 W5 回归 |
| 通用常驻 memory manager | 分散于上列组件 | 尚未形成统一生产级 allocator、持久化恢复与失效接管验收 |

### 源码导航

- [manager CLI](../guest-linux/aarch64/apps/gva_manager/gva_manager.c)
- [kernel segment API 实现](../guest-linux/kernel_ub/drivers/ub/obmm/obmm_core.c)
- [kernel mmap 检查](../guest-linux/kernel_ub/drivers/ub/obmm/obmm_shm_dev.c)
- [物理 allocator](../guest-linux/kernel_ub/drivers/ub/obmm/ubmempool_allocator.c)
- [GVA/GSVA UBC 实现](../vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c)
- [ARM TLB fill](../vendor/qemu_8.2.0_ub/target/arm/tcg/tlb_helper.c)
- [GSVA key](../vendor/qemu_8.2.0_ub/hw/ub/gsva_key.h)
- [route/token](../vendor/qemu_8.2.0_ub/hw/ub/gsva_route.h)
- [coherence](../vendor/qemu_8.2.0_ub/hw/ub/gsva_coherence.h)
- [对象 arena](../mem_service/components/mem_service/mem_service_obmm_objects.c)
- [GSVA buffer descriptor](../mem_service/components/mem_service/mem_service_gsva_access.h)
- [UB-SSD block ref](../mem_service/components/mem_service/mem_service_ub_ssd_gsva_backend.h)
- [共享 reader](../mem_service/components/mem_service/mem_service_cluster_read.c)

## 4. 协议与状态

### 4.1 GSVA key 与 descriptor

[guest UAPI](../guest-linux/kernel_ub/include/uapi/ub/gsva.h)
和 QEMU `GsvaKeyV1` 保持相应 wire 布局。不要通过更改已有 opcode 的
payload 来偷偷引入新字段；变更需要同时处理两端版本与兼容检查。

| 字段 | 含义 |
| --- | --- |
| `version / flags` | ABI 版本和标志 |
| `segment_id / home_va / size` | segment 身份、home 地址和范围 |
| `vmid / asid / pte_offset` | 地址上下文及翻译偏移 |
| `p_tag / cache_policy` | 路由相关标签与缓存策略 |
| `epoch` | segment/映射代际，旧 epoch 访问应被拒绝 |

token ID/value、权限、holder 和 lease epoch 位于关联的 descriptor、
route/token 状态中；不能把它们与 key 中的 segment epoch 混用。

`mem_service_make_gsva_buffer_desc_from_source()` 当前限制为 8 节点、
key version 1、epoch 1、directory MESI，VMID/ASID 为零初始化值，
并把 token ID 填入 token value。后续接入动态管理端时必须逐项消除这些假设。

### 4.2 控制命令

| 命令 | opcode |
| --- | --- |
| `SIM_DEC_OP_GSVA_MAP_V1` | `0x09` |
| `SIM_DEC_OP_GSVA_UNMAP_V1` | `0x0a` |
| `SIM_DEC_OP_GSVA_EVENT_V1` | `0x0b` |
| `SIM_DEC_OP_GSVA_QUERY_V1` | `0x0c` |

query 区分 CAPS、ROUTE、COHERENCE、SEGMENT。
使用查询结果判断能力，不能只凭存在设备节点或成功打印配置认定路径已生效。
address profile、cache policy 和操作类型属于不同枚举，数值不能跨命名空间混用。

### 4.3 Coherence 与错误

对象状态为 I、S、E、M、RETIRED、TIMEOUT。
pending sequence、目标 holder 集合、ACK 集合和开始时间单独记录。
存在 invalidate、downgrade、writeback、fence、retire、token revoke
六类请求及其 ACK；完成某一类 ACK 不代表其他事务也完成。

稳定错误包括 BAD_VERSION、KEY_MISMATCH、STALE_EPOCH、TOKEN_DENIED、
ROUTE_MISSING、COH_PENDING、COH_TIMEOUT、TLB_STALE、
SEGMENT_RETIRED、UNSUPPORTED_POLICY、STRICT_ADDRESS、FEATURE_MISSING。

消费方需要区分尚在等待的事务与终态失败。
地址或权限错误必须拒绝访问；retire/reuse 需要旧映射与关联 TLB 的清理。
QEMU 状态机属于模拟机制，不能据此声称所有真实设备已经具备硬件级一致性。

## 5. W5 reader 修复的具体保证

`mem_service_wait_terminal_token_result_for_model()` 现在在对象查找前调用
共享 metadata refresh，复制有效 records 后再次确认发布序列稳定。
找到 token record 后，按范围刷新 64 字节 payload，再检查 step 和 checksum。
本地 slot 不需要远端同步。

共享帮助函数拒绝越界和地址加法溢出；同步失败、元数据发布变化、
checksum 错误均不能产生成功返回。
这保持了既有对象等待语义，没有靠关闭 shortpath 或增加 timeout 绕过缺陷。

“token 的 notification 已送达”与“当前独立 reader 能看到新 token”
是两个不同事实。GSVA 的 epoch/token 检查同样不能代替 reader 的数据可见性处理。

正式行为测试使用真实 terminal reader 和 cluster reader，仅把外部 provider
建模为互相独立的 exporter/importer 字节区。包含 10 个场景：
stale import、local、checksum、metadata sync failure、payload sync failure、
torn publication、changed publication、record bounds、metadata bounds、
address overflow。

## 6. 命令入口与使用条件

以下 guest 命令要求节点已完成基础设施 bootstrap，`/dev/obmm` 和相关驱动可用。
先查询现有 segment，再操作实际分配获得的 ID；不要编造可用 segment ID。

```sh
/bin/linqu_gva_manager --dump-routes
/bin/linqu_gva_manager --query
```

以下是已有的远端仿真验收入口，在准备好的目标机仓库根目录执行。
配置应来自该目标机的配置文件；本次文档刷新没有运行这些命令。

```sh
./guest-linux/aarch64/scripts/run_ub_two_node_gsva_arm_mmu_acceptance.sh
./guest-linux/aarch64/scripts/run_ub_four_node_gsva_arm_mmu_acceptance.sh
./guest-linux/aarch64/scripts/run_ub_eight_node_gsva_arm_mmu_acceptance.sh
./guest-linux/aarch64/scripts/run_ub_two_node_gva_manager_segment_cli_test.sh
./guest-linux/aarch64/scripts/run_ub_two_node_ssd_gsva_test.sh
```

服务 reader 回归可在 mem_service 根目录独立运行，无需模型权重：

```sh
python3 -m unittest discover -s tests -p test_mem_service_terminal_token_visibility.py
```

W5 手动运行入口与参数维护在 [W5 手动运行文档](w5_manual_serving_run.md)。
地址机制测试、服务 reader 测试与模型端到端验证分别保留，避免一次 PASS 覆盖多个未经验证的层次。

## 7. 证据分级

| 证据 | 可以得出的结论 | 范围限制 |
| --- | --- | --- |
| 当前代码与 UAPI 核对 | 对应机制和入口确实存在 | 无法单凭源码判定完整运行成功 |
| [GVA/GSVA 阶段报告](sim_gva_gsva_obmm_mesi_stage_status_summary.md) | 报告列出的历史拓扑、模式和生命周期测试通过 | 当前更新没有重跑这些历史矩阵 |
| [2026-09-07 W5 调查](2026-09-07-w5-terminal-token-visibility-investigation.md) | 修复后的 4/8-node、4-step token 可见性路径通过 | 使用报告列出的代码与 runner 组合；不覆盖全部最新子模块组合 |
| 同报告的正式服务测试 | 136 项中 131 通过、5 条件跳过；guest 304 项通过 | 跳过项和构建环境见报告 |
| [历史数据面 microbenchmark](2026-06-24-w5-gva-gsva-dataplane-benefit-report.md) | 各 baseline 实现的观测差异 | 无法直接外推真实硬件或完整 LLM serving 收益 |
| 本次文档检查 | SVG/XML、图片、源码引用、入口和 Markdown 链接一致 | 文档检查不增加运行功能的验收覆盖率 |

## 8. 剩余工作与优先级

以下混合了实现缺口、集成和验证工作。P1/P2 是建议优先级，详细实施顺序见
[服务能力补齐实施计划](plans/2026-09-08-gva-gsva-service-completion-plan.md)。
缺少验证证据不能直接推出相应功能完全不存在。mem_service 已有 journal、
compaction 和恢复逻辑；此处恢复范围限定为 GSVA 分配与映射状态的端到端一致性。

| 优先级 | 工作 | 性质 | 完成判据 |
| --- | --- | --- | --- |
| P1 | 统一服务级 allocator 与描述符 ownership | 服务整合与功能补齐 | 多客户端 alloc/map/release；地址与 backing 分配失败可回滚 |
| P1 | 动态 key/epoch/lease 贯穿 W5 adapter | 明确的适配实现缺口 | rotate/revoke/reuse 后旧引用拒绝，新引用可用 |
| P1 | GSVA 分配与映射状态恢复 | 整合与验证，按失败结果补实现 | 不重复分配仍被使用的地址；无法证明状态时拒绝访问 |
| P2 | tokenless replay 与 GSVA 联合失效 | 跨机制集成与验证 | voided 旧 payload 丢弃，replay 与 mapping pin/generation、key/token/epoch 一致；旧访问不可命中新对象 |
| P2 | 回收与资源压力矩阵 | 验证工作，可能暴露实现问题 | 多 prefix、KV、hidden、block 混用时计数和容量正确 |
| P2 | 分层性能验收 | 性能评估 | 控制同 prompt/steps/拓扑，并分别报告解析、传输、一致性与端到端时间 |

当前 async-load 与 PTO/UB_GM 有各自实现和证据，但不能自动继承 GSVA 的全部保证。
普通 LDR 联调采用 [09-05 source-owned tokenless 方案](2026-09-05-source-owned-void-response-validation-report.md)，
不新增 software-visible wait_key。PTO 显式异步 dispatch 单独验收。
[硬件机制分解](2026-08-20-gva-gsva-upcall-coroutine-hardware-mechanisms.md)
作为历史背景；具体执行基线与恢复缺口见上述实施计划。
