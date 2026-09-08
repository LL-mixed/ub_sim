# GSVA 设计：统一地址、分配管理与生命周期

更新：2026-09-08；版本基线与总体职责见 [GVA 设计](sim_gva_simulation_design.md)。
本文描述代码已具备的机制、服务化尚未闭合的部分以及需要保持的约束。

## 1. 定义

严格 GSVA profile 要求 `user_va = uba = home_va`，`pte_offset = 0`。
该约束适用于被管理 segment 的地址范围。segment 内某个对象的地址
还需要加上经过边界检查的 payload offset 和 object backing offset。

GSVA feature 包括共同 aperture、地址分配、固定映射、对象身份、
访问权限、epoch、coherence 和撤销/回收。
仅让几个进程打印相同 VA，不能证明已建立上述全部语义。
token 权限与地址相等是两个独立条件。

## 2. SVG 与生成图

总图中的 02 区域表示 segment 生命周期；01 区域表示资源与管理的职责。

![GSVA 地址管理、并列 backing 与生命周期原图](gva_gsva_current_architecture.svg)

![基于架构逻辑重新构图的空间设备插画](gva_gsva_current_architecture.generated.png)

[SVG 原图](gva_gsva_current_architecture.svg) ·
[1536×1024 PNG](gva_gsva_current_architecture.generated.png)。
生成版本的限制及维护规则见 [图像说明](sim_gva_simulation_design.md#2-架构图)。

## 3. 三类 allocator 不能混为一谈

| 对象 | 所需管理 | 当前代码 |
| --- | --- | --- |
| 物理内存 | 容量、对齐、NUMA、实际 backing 的分配与释放 | OBMM mempool allocator、export-from-pool |
| 全局虚拟地址 | aperture reservation、segment ID、home VA、epoch、固定地址映射 | gva_manager、OBMM GSVA segment UAPI、kernel registry |
| 服务对象 payload | 对象大小、offset、版本、引用、backend 绑定 | mem_service record 与线性 payload arena |

服务级 memory manager 应向客户端提供统一的分配和访问能力，
GSVA 是其可选择的地址/共享语义。当前上述功能分布于多个组件；
文档不再把“统一 memory manager 已完整存在”作为事实。

物理池有空闲空间不保证所有进程都能在同一 VA 成功映射。
地址 aperture 有空闲范围也不保证 OBMM 或 UB-SSD 分配一定成功。
两类分配失败需要分别报告，失败过程必须回收本次已获得的资源。

## 4. 当前实现

### 4.1 管理端与 bootstrap

[gva_manager.c](../guest-linux/aarch64/apps/gva_manager/gva_manager.c)
包含 per-node bootstrap、共享队列消息、aperture 协商以及 segment CLI。
其控制通信复用 OBMM 共享内存和 MPMC bus，先建立基础通信再协商地址范围。
这个管理端属于地址基础设施，生命周期不能依附某次 infer 请求。

当前代码具有 `--bootstrap`、`--alloc`、`--query`、`--retire`、
`--allocate-segment` 和 `--dump-routes` 入口。
`--allocate-segment` 还承载分布式描述符导入、token rotation、
coherence recovery、retire/reuse 等验收流程。
这些入口能验证管理机制，但不足以证明常驻、多租户、持久化的分配服务已完成。

### 4.2 Kernel 和 libobmm

[gsva.h](../guest-linux/kernel_ub/include/uapi/ub/gsva.h)
定义 `obmm_gsva_segment_desc_v1` 与 alloc/query/retire/event ABI。
[obmm_core.c](../guest-linux/kernel_ub/drivers/ub/obmm/obmm_core.c)
实现 segment 请求处理；
[obmm_shm_dev.c](../guest-linux/kernel_ub/drivers/ub/obmm/obmm_shm_dev.c)
检查严格映射地址与 aperture 冲突。

[obmm_common.h](../guest-linux/aarch64/common/obmm_common.h)
提供 `obmm_do_export_fixed_uba()`、`obmm_map_gsva_region_at()`、
`obmm_do_import_gsva_desc_v1()` 等仿真适配帮助函数。
export/import 的通用用户态接口复用 vendor/obmm 中的 libobmm；
GSVA 及仿真专用扩展仍可见于本地 adapter。
固定地址失败时必须返回错误，禁止自动换址后继续宣称严格同址。

### 4.3 对象描述符与 W5

[mem_service_gsva_access.h](../mem_service/components/mem_service/mem_service_gsva_access.h)
把服务记录与各节点 region 元数据转换为 GSVA buffer descriptor。
它检查 active、owner、region 范围和整数溢出。
地址计算为：

`gsva_base = home_va + payload_offset + object_backing_offset`。

当前 helper 有明确的 profile 限制：最多 8 个节点，key version/epoch 为 1，
VMID/ASID 由清零初始化，p_tag 从 home CNA 派生，
cache policy 固定为 directory MESI，token value 从 token ID 填充。
这些是这一适配路径的实现限制，不能当作所有 GSVA manager 的协议上限，
也不能宣称动态租约和 epoch 已从管理端贯穿到全部 W5 请求。

## 5. 生命周期与失效顺序

1. **Reserve：**各节点确认同一 aperture；kernel 保护该地址范围。
2. **Allocate：**选定 home、segment ID、大小、对齐、地址、epoch 和权限，
   再建立相应 backing 与 descriptor。
3. **Map / acquire：**home 和 importer 遵守固定地址与范围要求，安装路由；
   接受访问前检查 key、token、epoch 和 coherence 条件。
4. **Revoke / retire：**阻止旧身份继续取得访问权，发出相应失效事务。
5. **ACK 与清理：**跟踪目标 holder 的 ACK，处理映射清理和关联 TLB 失效。
   无法确认完成时保留 pending/timeout 状态，不把它当作可安全复用。
6. **Reuse：**完成回收后以更高 epoch 重新使用地址或 segment，
   旧描述符应被拒绝。

token rotation、writer invalidation 和 segment retire 各有状态机与 ACK，
图中生命周期是共同的管理约束，不能把所有操作简化为同一条 wire 消息。
实际事件、状态和错误码以 [实现规格](sim_gva_gsva_implementation_spec.md) 为准。

## 6. OBMM 与 UB-SSD

OBMM pool 管理可映射内存，UB-SSD 管理块对象。
GSVA 可用于描述 I/O 传输 buffer，不意味着 UB-SSD 的 block ID 就是 GSVA，
也不意味着 SSD 具有 CPU cache 的全部语义。

[mem_service_ub_ssd_gsva_backend.h](../mem_service/components/mem_service/mem_service_ub_ssd_gsva_backend.h)
定义 block ref 的身份、版本、offset、bytes、checksum。
记录同时带有 backend node/device CNA 等归属信息，
并通过 `make_primary_payload` 区分附加 backend 引用与主 payload backing。
远端 UB-SSD 访问必须按 backend 归属路由，不能用本地设备读冒充远端读。
两种 backing 的选择、迁移和传输 buffer 都应显式表达。

## 7. 一致性的不同层面

| 机制 | 保证范围 | 不能据此推出的结论 |
| --- | --- | --- |
| GSVA key/token/epoch | 身份、权限、代际与访问资格 | 每个 reader 的导入字节自动是最新值 |
| GSVA coherence 事务 | 模拟对象 ownership、失效与 ACK 协调 | 已经等价真实 CPU 全系统 cache 一致性 |
| OBMM directory MESI | 仿真 backing 的 line-level 数据协调 | 所有设备、存储块都使用相同 cache 协议 |
| mem_service metadata/payload refresh | 指定导入映射范围更新及内容检查 | 获得通用分布式事务或故障接管保证 |
| 队列 notification / ACK | 描述符通知与相应消费协议 | 独立对象读取接口自动看见新 payload |

W5 的 terminal-token reader 修复属于第四行。
[调查报告](2026-09-07-w5-terminal-token-visibility-investigation.md)
记录了未刷新映射时第二个 token 已发布但 nodeA 读不到的失败，
以及刷新 metadata、确认序列、刷新 payload 后的修复验证。
不得通过恢复旧数据、跳过 checksum 或增加等待时间来替代正确的可见性处理。

## 8. 尚需补齐

实施顺序与功能、集成、验证工作的区分见
[服务能力补齐实施计划](plans/2026-09-08-gva-gsva-service-completion-plan.md)。

- 将地址管理接口接入稳定的服务级 allocator，明确多客户端并发与 ownership。
- 让 W5 描述符消费管理端真实的 epoch、租约和上下文，消除固定 profile 假设。
- 建立配额、回收、碎片、重启重建和节点失效接管的独立验收。
- 联合检查地址回收、coherence、TLB 与 tokenless replay 的映射生命周期，
  保证 voided 旧 payload 被丢弃，旧上下文不能在同址重用后访问新对象。
- 将 OBMM 与 UB-SSD 的跨 backend 生命周期和失败回滚纳入统一对象语义，
  保持 backing 资源并列，保留不同设备的数据访问机制。

已实现机制的历史证据见
[阶段状态报告](sim_gva_gsva_obmm_mesi_stage_status_summary.md)；
当前可调用的 CLI 和回归入口见 [实现与验证](sim_gva_gsva_implementation_spec.md)。
