# Rust LLM Server MVP 仿真系统 High Level Design (v1)

> 目标：构建一个 **基于 QEMU、符合 UB/Linqu spec 的系统级仿真平台**，用于验证 UB/PyPTO 层级运行时设计。Linqu 和 UB 为同等的概念，在本文档中不加区分。`draft/rust_llm_server_design_v8.md` 的 MVP 是这个平台上的第一个重点验证目标。
>
> 定位：这不是一个仅面向 LLM serving 的专用行为模拟器，而是一个 **UB/Linqu-compliant simulator**。它以 QEMU 为底层系统仿真基座，在其上承载 UB 机器对象、PyPTO 运行时语义、Lingqu 数据系统服务和具体 workload。

## Revision

| Revision | Time | Brief Changelog |
|---|---|---|
| v2 | 2026-03-22 CST | 补充术语与出处、四层仿真栈、PyPTO device/UB 关系、模块关系、场景组细化和场景配置模板。 |
| v1 | 2026-03-21 CST | 收紧目标为“基于 QEMU、符合 UB/Linqu spec 的系统级仿真平台”，补入 `UB` 管控面与基础设施仿真，明确 Rust LLM MVP 只是首个 workload。 |
| v0 | 2026-03-20 20:14 CST | 初始版本。定义了面向 MVP 的仿真系统目标、范围、模块划分、执行模型、场景、配置、里程碑与验收标准。 |

---

## 1. 背景与目标

在 `rust_llm_server_design_v8.md` 中定义了生产系统的统一层级抽象：

- `HierarchyLevel`
- `BlockStore`
- `LevelNode`
- `LevelAllocator`
- `IntegrityVerifier`
- 递归树构建
- 递归路由
- 级联 eviction / promotion

但在真正接入 GPU、UB/Linqu 和远端存储之前，需要一个中间层系统来回答如下工程问题：

1. 这些抽象是否足够稳定，能否支撑 MVP 的主流程？
2. 树结构和 level collapsing 是否真的简化了路由和容量管理？
3. 路由、promotion、eviction、故障绕行是否能在统一接口下跑通？
4. 指标、日志、事件流是否足够支撑调试和演示？
5. 在不同拓扑和负载下，递归方案相对 flat routing 的收益和成本是什么？

因此，本设计提出一个 **QEMU-based UB/Linqu System Simulator**：

- 基于 QEMU 扩展出机器、设备、内存域、互连和虚拟节点的系统级仿真底座；
- 实现基于 UB 规范定义的系统对象、枚举/路由/资源管理边界、内存与通信语义的管控面仿真框架；
- PyPTO/Lingqu runtime 文档负责定义 hierarchy、task identity、scope/ring、dispatch 和 data service 的上层编程语义；
- 符合 `rust_llm_server_design_v8.md` 设计实现的 MVP 作为第一个 workload，通过LLM serving 场景验证这套平台；

### 1.1 Linqu / PyPTO 结构约束

如果当前设计只停留在 “KV cache simulator”，会遗漏 v8 的真正结构来源。根据 `ub_docs` 下的 `UB` 文档，仿真平台首先要尊重的是 `UBPU`、`Entity/EID`、`UB domain`、`UB Fabric`、`UMMU`、`UB Decoder`、`UBM`、`UB OS Component`、`UB Service Core` 这些系统对象和分层边界；PyPTO/Lingqu 的 hierarchy、task、scope/ring 等只是建立在这些接口之上的编程与执行视图，不是平台本体的替代物。

根据 `rust_llm_server_design_v8.md`、`linqu_runtime_design.md`、`machine_hierarchy_and_function_hierarchy.md`、`multi_level_runtime_ring_and_pypto_free_api.md` 和 `linqu_data_system.md`，这个仿真系统还必须满足四条上位约束：

- **Linqu 的层级对称性**：运行时结构必须镜像物理层级，不能只把 hierarchy 当文档说明
- **PyPTO 的函数层级语义**：`pl.Level`、`pl.at(level=...)`、`@pl.function(level=...)` 这些 hierarchy label 必须能在仿真中保留下来
- **scope-driven ring stack 语义**：任务身份、scope depth、`pl.free`、layer-local retire 不能被简化丢失
- **`simpler` 的边界约束**：L0-L2 的既有执行能力不应被重做，仿真器需要明确建模与 `simpler` 的适配边界

另外，`linqu_data_system.md` 说明在更大的 Lingqu 体系里，运行时并不是孤立工作的，而是建立在统一的数据平面服务之上：

- `lingqu_shmem`
- `lingqu_block`
- `lingqu_dfs`
- `lingqu_db`

当前最小可验证范围不需要完整实现这些数据服务，但必须在设计中为它们保留对接位置。

因此，当前设计的正确定位不是一个孤立的缓存仿真器，而是一个：

**基于 QEMU 的 UB/Linqu / PyPTO 兼容系统仿真器，其中 LLM serving MVP 是首个验证目标。**

### 1.2 UB 规范基线

本 HLD 后续所有 `Linqu` 表述，默认都应与 `UB` 规范对象对齐，并至少参考以下文档：

- [UB-Base-Specification-2.0-en.pdf](../ub_docs/UB-Base-Specification-2.0-en.pdf)
- [UB-Firmware-Specification-2.0-en.pdf](../ub_docs/UB-Firmware-Specification-2.0-en.pdf)
- [UB-Software-Reference-Design-for-OS-2.0-en.pdf](../ub_docs/UB-Software-Reference-Design-for-OS-2.0-en.pdf)
- [UB-Service-Core-SW-Arch-RD-2.0-en.pdf](../ub_docs/UB-Service-Core-SW-Arch-RD-2.0-en.pdf)
- [UB-Mgmt-OM-SW-Arch-and-IF-RD-2.0-en.pdf](../ub_docs/UB-Mgmt-OM-SW-Arch-and-IF-RD-2.0-en.pdf)

对仿真平台而言，最关键的 `UB` 基线如下：

- `UBPU` 是处理单元，`Entity` 是资源与事务通信的基本分配对象
- `UB domain` 和 `UB Fabric` 定义互连域与交换/链路集合
- `UMMU` 与 `UB Decoder` 定义内存寻址、权限校验和资源空间访问边界
- `UBM` 负责枚举、CNA/EID 地址管理、路由、资源管理与运维
- `UB OS Component` 负责 OS 侧的设备管理、内存管理、通信和虚拟化扩展
- `UB Service Core` 提供集群级系统服务，包括 `UBS Engine`、`UBS Mem`、`UBS Comm`、`UBS IO`、`UBS Virt`

后续所有 QEMU 映射、task 语义和 workload 验证，都应建立在这些对象之上，而不是绕开它们单独定义另一套“Linqu 平台对象”。

### 1.3 当前 Linux 内核实现基线

除规范文档外，本 HLD 还参考：

- [UB-Implementation-Summary.md](../ub_docs/UB-Implementation-Summary.md)

这份实现总结说明，当前 Linux 内核里的 `UB` 主线落点更接近一个“内核设备栈 + URMA 通信栈”，而不是直接等同于 `UBM` 或 `UB Service Core`。其主链路可以概括为：

- `ubfi`
  - 解析 `UBRT` / `ACPI` / `DTS`，创建 `UBC`、`UMMU` 等固件可见对象
- `ubus`
  - 管理 `Entity`、`Port`、`Route`、`Decoder`、热插拔、RAS、sysfs、ioctl
- `ubase`
  - 管理控制队列、事件队列、邮箱、QoS、reset 和基础能力
- `ubcore` / `uburma`
  - 提供 `Jetty`、`Segment`、`EID` 等 `URMA` 通信对象，以及 `/dev/ubcoreX`、ioctl、mmap、netlink 等用户接口
- `ummu`
  - 提供 token、IOVA、页表、SVA 等内存管理能力
- `obmm` / `cdma` / `sentry` / `vfio-ub`
  - 提供跨机内存、DMA、安全监控和设备直通能力

这对本仿真平台的直接影响是：

- 基于 Qemu 的单节点模拟 `UBRT`/`ACPI`/`DTS` 启动入口和 `ubfi` 能看到的设备对象
- 设备模型优先应贴近 `ubus` / `ummu` / `ubcore` / `vfio-ub` 的对象边界
- `UBM`、`UB` 管控面基础设施、PoD 级管理和更高层资源调度，是平台本体的一部分；在当前实现路线里，其中一部分可以作为宿主侧控制面服务先行落地，另一部分则用全新设计来补足

也就是说，先把 Linux 能识别和使用的 `UB` 对象链路模拟对，然后需要“模拟完整 SuperPoD 管理基础”，然后需要缺失的软件组件。

### 1.4 术语与出处

本节把正文中高频出现、且容易在多份设计文档之间漂移的术语统一收口。若后文未特别声明，均按下表理解。

| 术语 | 本文中的含义 | 主要出处 |
|---|---|---|
| `L0` | Core / Core-group。最细粒度执行单元，对应 AIC / AIV / core-group 调度位。当前平台仅保留标签与边界，真实执行由 `simpler` 负责。 | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md), [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md) |
| `L1` | Chip die。可选层级；当前单 die 模型中可省略。本文把它作为保留层级位，不在 MVP 中激活。 | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md), [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md) |
| `L2` | Chip。一个 chip-level execution boundary；在本文中优先映射为单个 `UBPU` 或一组紧耦合 `Entity` 暴露出的 guest-visible endpoint。 | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md), [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md), [UB-Base-Specification-2.0-en.pdf](../ub_docs/UB-Base-Specification-2.0-en.pdf) |
| `L3` | Host。一个 OS instance；负责 host-side orchestration、Tier-2 边界和本地 cache / control-plane 视图。 | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md), [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md) |
| `L4` | Cluster-level-0。局部池化与高带宽局部互连域；在本文中优先对齐 `UB domain`。 | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md), [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md), [UB-Base-Specification-2.0-en.pdf](../ub_docs/UB-Base-Specification-2.0-en.pdf) |
| `L5` | Cluster-level-1。多个 `L4` 单元之上的上层汇聚域；当前正文中默认 collapsed/stub，但在扩展章节中保留为 fabric cell 语义。 | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md), [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md) |
| `L6` | Cluster-level-2。跨域/跨 rack 的更大规模互连与编排域；当前正文中默认 collapsed/stub。 | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md), [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md) |
| `L7` | Global Coordinator。顶层入口、全局编排与 northbound 观察面。本文保留该编号位和控制面位置，不在 MVP 中激活。 | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md) |
| `Linqu` / `Lingqu` | 本文中若指系统级对象与互连边界，默认与 `UB` 规范体系对齐；若指运行时/数据平面语义，则指建立在这些 `UB` 对象之上的 PyPTO/Linqu runtime 与 data service 视图。 | [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md), [linqu_data_system.md](../docs/pypto_top_level_design_documents/linqu_data_system.md), `ub_docs/*` |
| `UBPU` | 支持 `UB` 协议栈并实现设备特定功能的处理单元。本文中它是 L2 guest-visible endpoint 的首选对象。 | [UB-Base-Specification-2.0-en.pdf](../ub_docs/UB-Base-Specification-2.0-en.pdf) |
| `Entity` / `EID` | `UB` 中资源分配和事务通信的基本对象及其标识。本文把它作为 device / route / resource-space / UAPI 设计的基础对象。 | [UB-Base-Specification-2.0-en.pdf](../ub_docs/UB-Base-Specification-2.0-en.pdf), [UB-Implementation-Summary.md](../ub_docs/UB-Implementation-Summary.md) |
| `UB domain` / `UB Fabric` | `UB` 互连域与交换/链路集合。本文中 `L4` 优先映射 `UB domain`，`L5/L6` 逐步映射更大的 `UB Fabric` 管理域。 | [UB-Base-Specification-2.0-en.pdf](../ub_docs/UB-Base-Specification-2.0-en.pdf) |
| `UMMU` / `UB Decoder` | `UB` 体系中的地址映射、权限校验和资源空间访问边界。本文中它们属于 guest-visible device model 的核心对象。 | [UB-Software-Reference-Design-for-OS-2.0-en.pdf](../ub_docs/UB-Software-Reference-Design-for-OS-2.0-en.pdf), [UB-Implementation-Summary.md](../ub_docs/UB-Implementation-Summary.md) |
| `UBM` | `UB` 管理与运维平面。本文中默认作为控制面对象存在，可先落在宿主侧 control-plane service。 | [UB-Mgmt-OM-SW-Arch-and-IF-RD-2.0-en.pdf](../ub_docs/UB-Mgmt-OM-SW-Arch-and-IF-RD-2.0-en.pdf) |
| `UB OS Component` | `UB` 在 OS 侧的设备、内存、通信与虚拟化扩展。本文中 `L3` host 视图与 guest-visible object 设计需要优先贴近该对象边界。 | [UB-Software-Reference-Design-for-OS-2.0-en.pdf](../ub_docs/UB-Software-Reference-Design-for-OS-2.0-en.pdf) |
| `UB Service Core` | 集群级系统服务层，包括 `UBS Engine`、`UBS Mem`、`UBS Comm`、`UBS IO`、`UBS Virt`。本文把它视为平台本体的一部分，而不是可选 helper。 | [UB-Service-Core-SW-Arch-RD-2.0-en.pdf](../ub_docs/UB-Service-Core-SW-Arch-RD-2.0-en.pdf) |
| `TaskKey` | 完整层级坐标形式的任务身份，写作 `(logical_system, L7..L0, scope_depth, task_id)`。本文中任何跨层任务/trace/retire 语义都应最终可还原到该标识。 | [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md) |
| `pl.Level` | PyPTO 的层级标签枚举，用于 `pl.at(level=...)` 和 `@pl.function(level=...)`。本文要求这些标签在 simulator 中不丢失。 | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md) |
| `pl.free` | 对某个输出提前施加 scope token 的语义，不绕过 fanout safety。本文要求该语义至少在 trace 和场景判定中可表达。 | [multi_level_runtime_ring_and_pypto_free_api.md](../docs/pypto_top_level_design_documents/multi_level_runtime_ring_and_pypto_free_api.md) |
| `task_ring` / `buffer_ring` | 按层级、按 scope depth 划分的 runtime ring 结构。本文在 L3+ 负责其仿真语义；L0-L2 的真实实现归 `simpler`。 | [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md), [multi_level_runtime_ring_and_pypto_free_api.md](../docs/pypto_top_level_design_documents/multi_level_runtime_ring_and_pypto_free_api.md) |
| `simpler` | 已有的 L0-L2 runtime；负责 chip/core 侧 ring、scope、task 和执行语义。本文强调“适配，不重做”。 | [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md) |
| `ChipBackend` | host-side runtime 与 chip/device runtime 之间的 Tier-2 适配边界；负责 `dispatch`、`h2d_copy`、`d2h_copy`、句柄映射与完成事件。 | [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md) |
| `BlockStore` / `LevelNode` / `LevelAllocator` | `rust_llm_server_design_v8` 中用于递归 cache / route / allocation 的核心统一抽象。本文把它们视为首个 workload 消费的平台能力。 | [rust_llm_server_design_v8_zh.md](rust_llm_server_design_v8_zh.md) |

---

## 2. 设计目标

### 2.1 必须达到

- 基于 QEMU 构建系统级仿真底座
- 仿真底座要能承载 Linqu 机器层级、设备模型、互连和节点实例
- 能模拟 L2/L3/L4 三层层级：
  - L2: `UBPU` / `Entity` 执行边界与 chip-local memory tier
  - L3: host OS 视图下的本地内存层，例如 host DRAM / CXL-local
  - L4: `UB domain` 局部池化层，例如 pooled memory / shared fabric tier
- 能保留 Linqu / Lingqu L0-L7 的完整层级编号语义，即使当前范围只激活 L2-L4
- 能构建层级树，并支持 level collapsing
- 能执行请求流的递归路由
- 能模拟 block hit / miss / fetch / store / remove
- 能模拟 promotion 和 cascading eviction
- 能模拟健康状态变化和完整性失败
- 能输出统一 metrics、事件日志和场景报告
- 能通过配置驱动不同拓扑、容量、延迟、故障和 workload
- 能表达 PyPTO 风格的 function label、task coordinate 和 scope 语义
- 能表达 scope-driven multi-layer ring stack、`pl.free`、layer-local retire 的调测语义
- 能提供 `simpler` L2 backend 适配边界的仿真抽象
- 能为 `lingqu_shmem` / `lingqu_block` / `lingqu_dfs` / `lingqu_db` 提供最小但可验证的仿真功能
- 能把 `rust_llm_server_design_v8.md` 的 MVP 作为第一个验证 workload 挂到平台上

### 2.2 暂时不做

- 不做真实 CUDA/NPU kernel 执行
- 不做真实 CXL mmap / device IO
- 不做真实网络协议栈
- 不做真实 tokenizer / detokenizer / batch scheduler
- 不重实现 `simpler` 内部 ring buffer、scope-exit 和 AIC/AIV 执行机制
- 不做 TB 级 WAL 持久化和 Bloom gossip 优化
- 不做 layer-wise prefetch 和 compute-IO overlap
- 不要求当前范围就具备 cycle-accurate 精度

上述不做的部分属于生产版或后续仿真增强版，暂时不划归到最小可验证范围。

---

## 3. 仿真系统的角色

该系统承担四个角色：

### 3.1 架构验证器

验证 v8 MVP 的核心接口和行为是否闭环。

### 3.2 演示系统

用固定场景展示：

- 请求如何沿树递归选路
- block 如何从 L4 提升到 L2
- L2 满时如何级联逐层淘汰
- 节点故障时如何绕行

### 3.3 评估平台

用统一 workload 和拓扑参数对比：

- flat routing vs recursive routing
- 不同容量配比下的 hit rate
- 不同高低水位下的 eviction 压力
- 不同故障注入下的恢复行为

### 3.4 调测工具

为后续真实实现提供：

- trait 边界验证
- 配置结构验证
- 指标 schema 验证
- debug event 语义验证

### 3.5 Linqu / PyPTO 语义桥接器

它还要承担一个额外角色：

- 对 Linqu，验证 hierarchy symmetry 和 recursive enclosure 是否真正体现在结构里
- 对 PyPTO，验证 function hierarchy label 和 runtime dispatch 边界能否落到 serving 系统
- 对 `simpler`，验证 host-level orchestration 与 chip-level backend 的边界是否清晰

---

## 4. 总体设计

### 4.1 核心思想

仿真系统以 QEMU 为单节点仿真基座，构建系统仿真框架，在其上扩展出 `UB/Linqu` 规范一致的设备、控制面、基础设施和 workload 验证能力。

也就是说：

- 用 QEMU 模拟机器、节点、CPU、内存域、虚拟设备和互连拓扑
- 用 `UB/Linqu` 仿真核心定义规范接口、设备对象、控制面能力，以及 PyPTO 所依赖的 hierarchy、task、scope/ring、dispatch 和 data services
- 用参数化模型补足当前范围中尚无真实硬件支持的设备特性
- 用具体 workload 去验证 Linqu 平台语义是否能支撑上层系统

系统的首要目标不是 cycle-accurate，而是：

- 行为可解释
- 场景可复现
- 参数可调
- 输出可分析

### 4.2 高层架构

```text
                +-------------------------------+
                | Scenario / Topology Loader    |
                +---------------+---------------+
                                |
                                v
                +-------------------------------+
                | QEMU Platform Layer           |
                | - machine models              |
                | - virtual hosts/chips         |
                | - memory domains              |
                | - virtual UB/CXL devices      |
                +---------------+---------------+
                                |
                                v
                +-------------------------------+
                | UB/Linqu Simulation Core      |
                | - UB core interfaces          |
                | - control plane + infra       |
                | - hierarchy / TaskKey         |
                | - scope/ring lifecycle        |
                | - function dispatch           |
                | - data service simulation     |
                +---------------+---------------+
                                |
                                v
                +-------------------------------+
                | Workload Validation Layer     |
                | - rust_llm_server MVP         |
                | - Lingqu service tests        |
                | - synthetic PyPTO traces      |
                +---------------+---------------+
                                |
                                v
                +-------------------------------+
                | Metrics / Trace / Report      |
                +---------------+---------------+
                                |
                                v
                +-------------------------------+
                | CLI / Demo Runner             |
                +-------------------------------+
```

### 4.2.1 四层仿真栈

为了避免把 `QEMU`、`UB`、`PyPTO device` 和 host runtime 混成同一层，建议在实现和验收中显式采用以下四层栈：

```text
+---------------------------------------------------+
| Layer 4: Host-Side Linqu Runtime / Workload       |
| - host orchestration                              |
| - TaskKey / hierarchy routing                     |
| - scenario driver / workload harness              |
+--------------------------+------------------------+
                           |
                           v
+---------------------------------------------------+
| Layer 3: PyPTO / simpler Device Runtime           |
| - task_ring / buffer_ring / scope token           |
| - pl.free / retire / function-group scheduling    |
| - device-side dispatch semantics for L0/L1/L2     |
+--------------------------+------------------------+
                           |
                           v
+---------------------------------------------------+
| Layer 2: UB Guest-Visible Device Model            |
| - UBPU / Entity / UMMU / Decoder                  |
| - queue / doorbell / completion / DMA / RAS       |
| - guest-visible resource space and UAPI surface   |
+--------------------------+------------------------+
                           |
                           v
+---------------------------------------------------+
| Layer 1: QEMU Machine / Board / Bus               |
| - CPU / memory / interrupt / timer / fabric       |
| - VM / board / virtual bus / multi-node platform  |
+---------------------------------------------------+
```

这四层的关系应固定为：

- Layer 1 提供系统级仿真底座
- Layer 2 提供 `UB` 设备对象与 guest-visible 边界
- Layer 3 在这些边界之上实现 `PyPTO` / `simpler` 的 device runtime 语义
- Layer 4 消费前三层能力，承载 Linqu host runtime、场景驱动和 `rust_llm_server` workload

### 4.3 Linqu / PyPTO 双视图

当前仿真系统需要同时维护四套互相映射的视图：

1. `UB` 对象视图
- `UBPU`
- `Entity/EID`
- `UB domain` / `UB Fabric`
- `UMMU` / `UB Decoder`
- `UBM`
- `UB OS Component`
- `UB Service Core`

2. 运行时层级视图
- Linqu / Lingqu L0-L7 层级
- 当前范围激活 L2-L4
- L0/L1/L5/L6/L7 仍保留编号、标签和 collapsed/stub 位置

3. 运行时语义视图
- PyPTO 的 `pl.Level` 函数标签
- `simpler` 负责的 L0-L2 执行边界
- 本系统负责的 L2-L6 routing / cache / health / failure / metrics

4. QEMU 平台视图
- 哪些层级被映射成 QEMU machine / board / device
- 哪些互连被映射成虚拟 bus / fabric
- 哪些 data service 被映射成 QEMU device model 或 host service

建议在实现中显式维护下表：

| Linqu Level | 运行时角色 | 优先对应的 UB 对象 | 当前范围状态 | 说明 |
|---|---|---|---|---|
| L0 | AIC/AIV/Core-group | UBPU 内执行上下文 | stub | 只保留标签和 dispatch 语义 |
| L1 | Chip die | UBPU 内子拓扑 | stub | 保留层级位，不激活 |
| L2 | Chip-level execution boundary | 单个 UBPU 或一组紧耦合 Entities | active | `simpler` 适配边界，优先落到 guest-visible UB endpoint |
| L3 | Host | 运行 UB OS Component 的 host OS 视图 | active | host orchestration 与本地 cache |
| L4 | Cluster-level-0 | UB domain 内局部池化单元 | active | local fabric / pooled memory / shared service view |
| L5 | Cluster-level-1 | 更大范围的 UB Fabric 管理域 | collapsed/stub | 预留 fabric 语义 |
| L6 | Cluster-level-2 | 跨域控制或更大规模互连域 | collapsed/stub | 预留 global 语义 |
| L7 | Global Coordinator | UBM northbound 或外部编排平面 | collapsed/stub | 顶层入口与全局编排位 |

### 4.4 补充设计原则

当前范围还必须满足以下附加原则：

- **结构优先于表格映射**：Linqu hierarchy 要在代码对象中体现，而不是只在说明文字里体现
- **标签优先于功能完整性**：未激活的 level 也要保留 label 和接口位置
- **边界优先于重做**：`simpler` 以内不重做，`simpler` 以外由仿真器建模
- **数据系统前向兼容**：对接 Lingqu 数据平面的接口位置必须提前保留

---

## 5. 关键设计决策

### 5.1 使用离散事件仿真，而不是多线程实时仿真

原因：

- 更容易复现
- 更容易做 deterministic replay
- 更容易插入故障和观测点
- 不会把并发调度噪音误当成架构问题

当前范围中推荐采用 **QEMU 主循环 + 上层事件调度** 的组合方式：

- QEMU 提供设备/中断/时钟推进底座
- `UB/Linqu` 仿真核心维护自己的高层事件、控制面事件和 trace
- 不需要在当前阶段追求硬件级精确时序

### 5.2 以 QEMU 为底座，而不是另起一套系统模拟框架

原因：

- 目标本质上是系统仿真，不只是算法行为仿真
- `UB` 规范涉及 `UBPU`、`Entity`、内存域、路由、资源管理、固件启动信息和 OS/Service Core 边界，不适合只用纯业务对象建模
- 后续考虑接真实 ISA、虚拟设备和更细粒度 DMA/interrupt 行为，可复用 QEMU 组件，扩展方便
- 可以把 Lingqu 数据服务逐步落成 host service、virtio-like device 或自定义 QEMU device model
- 当前 Linux 实现主链明确存在 `ubfi -> ubus -> ubase -> ubcore/uburma -> ummu/vfio-ub`，QEMU 更适合承载 链路

### 5.3 仿真内核语言与生产代码风格保持一致

建议仿真核心使用 Rust 实现，而不是 Python。原因：

- 最终生产系统也会使用 Rust
- 可以直接复用或镜像 v8 中的 traits 和类型
- 后续更容易把 simulated `BlockStore` 替换成真实实现
- 配置、错误类型、指标模型都能提前稳定下来

### 5.4 数据面只模拟 block，不模拟真实 tensor

每个 block 在仿真里只需要：

- `BlockHash`
- 大小/尺寸/size
- 所在 level / node
- 最近访问时间
- 引用计数或热度
- 完整性状态

不需要真实 KV payload。

### 5.5 路由和迁移采用“逻辑成功 + 参数化耗时”模型

例如：

- L2 hit: 1 个时间单位
- L3 fetch + promote to L2: 10 个时间单位
- L4 fetch + promote chain: 30 个时间单位
- integrity failure: 额外产生 quarantine 和 retry

这样既能比较方案，也避免被硬件细节绑死。

### 5.6 保留完整任务坐标

从 Linqu / PyPTO 角度看，仅有 `request_id` 不够。

仿真系统应当显式支持：

`TaskCoord = (logical_system, l6, l5, l4, l3, l2, scope_depth, task_id)`

在当前范围中：

- serving request 可以映射到一个或多个 `TaskCoord`
- 即使没有真实 kernel 执行，也要保留这个坐标骨架
- 这样后续接入真实 PyPTO runtime dispatch 时，不需要推翻标识系统

### 5.7 保留 scope-driven ring stack 语义

根据 `multi_level_runtime_ring_and_pypto_free_api.md`，PyPTO 运行时的关键不是普通 task queue，而是：

- `task_ring[d]`
- `buffer_ring[d]`
- `last_task_alive[d]`
- `scope_depth`
- `pl.free`
- `fanout_count` / `ref_count`

因此当前仿真系统即使不实现真实 ring buffer，也必须建模以下语义：

- task identity 至少是 `(scope_level, task_id)`，而不是单个 `task_id`
- 每个 scope depth 都有独立 layer-local retire 视图
- `pl.free` 是提前应用 scope token，而不是绕过 fanout 安全
- inner scope 的 retire 不应被 outer scope 结构性阻塞

### 5.8 把 `pl.Level` 视为一等输入

仿真器不仅接受拓扑和缓存参数，也接受 hierarchy label。

至少在配置、trace 和 workload 中，要能表达：

- `@pl.function(level=...)`
- `pl.at(level=...)`
- default runtime level for a request or scenario step

### 5.9 把 Lingqu Data System 作为可验证子系统建模

根据 `linqu_data_system.md`，上层 runtime 未来会依赖四类底层数据服务：

- `lingqu_shmem`
- `lingqu_block`
- `lingqu_dfs`
- `lingqu_db`

当前最小可验证范围不需要实现真实协议或完整生产语义，但必须提供可验证的最小仿真功能：

- `lingqu_shmem`
  - 优先对应 `UBS Mem` / `UMMU` / shared region
  - 若对齐当前 Linux 实现，优先走 `OBMM` 路径，即 `obmm_region` / export-import / `obmm_shm_dev`
  - 验证共享区域建立、PE 可见性、one-sided put/get、同步点语义
- `lingqu_block`
  - 优先对应 `UBS IO` / `Entity` resource space / block gateway
  - 若对齐当前 Linux 实现，优先走 `ubcore Segment + JFS/JFC completion` 路径
  - 验证 `(UBA, LBA)` 寻址、异步读写、producer/consumer completion 语义
- `lingqu_dfs`
  - 优先作为平台控制面/基础设施侧服务，位于 `UB Service Core` 之上
  - 验证全局 namespace、路径访问、pread/pwrite、元数据/数据路径延迟模型
- `lingqu_db`
  - 优先作为平台控制面/基础设施侧服务，必要时通过 RPC gateway 接入 guest
  - 验证 Redis-like KV/Hash 基本命令、pipeline/batch、pub/sub 基本通知语义

也就是说，serving cache hierarchy 不是整个数据平面，但当前仿真器要能对这些数据平面服务给出最小可运行、可观测、可验证的模型。

---

## 6. 系统边界

### 6.1 输入

- 拓扑配置
- 层级配置
- workload 配置
- 故障注入配置
- 仿真参数
- 可选的 `pl.Level` / function label 输入
- 可选的 synthetic function graph 或 dispatch trace

### 6.2 输出

- 终态 metrics
- 每请求路由路径
- block 生命周期轨迹
- promotion / eviction 统计
- 故障与恢复事件
- 对比报告
- task coordinate / function label trace

### 6.3 不纳入系统边界

- 真实模型执行
- 真实硬件资源分配
- 真实网络消息序列化兼容性
- `simpler` 内部执行引擎本身

### 6.4 仿真对象到 Linux 内核对象的对照

结合 `UB-Implementation-Summary.md`，建议在实现中显式维护以下对照，而不是自造一套与内核无关的对象名：

| 仿真对象 | Linux 内核优先对应对象 | 说明 |
|---|---|---|
| UB controller object | `struct ub_bus_controller` | 控制器、解码器、资源列表、控制器编号 |
| UB entity object | `struct ub_entity` | `GUID`、`EID`、`CNA`、resource space、driver 绑定点 |
| UB port object | `struct ub_port` | 端口、远端实体、链路状态、domain boundary |
| route table / decoder object | `ub_route_*` / `ub_decoder` 相关对象 | 路由、地址解码、资源空间映射 |
| URMA jetty and queue object | `ubcore_jetty` / `ubcore_jfs` / `ubcore_jfr` / `ubcore_jfc` | `URMA` 通信对象 |
| URMA segment object | `struct ubcore_seg` | `Segment`、`ubva`、`token_id` |
| shared memory region object | `struct obmm_region` / `obmm_export_region` / `obmm_import_region` | 跨机共享内存区域 |
| guest 用户态设备面 | `/dev/ubcoreX` + ioctl/mmap/netlink/sysfs | 最小 guest 用户接口 |
| passthrough device edge | `vfio-ub` | 设备直通与用户态直接访问边界 |

这个对照不是要求当前范围完整实现所有 Linux 代码路径，而是要求 simulator 的对象名字、边界和可观测事件尽量与这些对象同构。

---

## 7. UB 管控面与基础设施仿真

符合 `UB/Linqu` spec 的仿真系统不能只有 guest-visible 设备和上层 workload；它还必须显式覆盖最小可用的管控面与基础设施仿真。

### 7.1 管控面范围

当前范围至少应仿真以下能力：

- 拓扑发现与枚举
- `CNA` / `EID` 分配与回收
- 路由生成、下发与更新
- 资源编排与容量摘要
- 健康、告警、遥测和故障事件
- 最小 northbound 管控接口

### 7.2 与平台本体的关系

这些能力不应被表述为“host-side helper”或“可选 co-sim 附件”，而应被视为平台本体的一部分。当前实现中，某些控制面逻辑可以先运行在宿主侧，但在设计语义上它们属于：

- `UB/Linqu` 规范接口的一部分
- `UB` 系统可用性的必要条件
- Rust LLM MVP 只消费、不定义的平台能力

### 7.3 最小控制面对象

建议至少显式建模：

- `TopologyManager`
- `AddressManager`
- `RouteManager`
- `ResourceManager`
- `HealthManager`
- `TelemetryManager`

它们分别负责：

- `TopologyManager`
  - 设备发现、域边界、链路状态、成员变化
- `AddressManager`
  - `CNA` / `EID` 生命周期
- `RouteManager`
  - 路由计算、下发、重路由
- `ResourceManager`
  - 容量摘要、借用/回收、资源视图
- `HealthManager`
  - degraded/failed/quarantined 状态和故障升级
- `TelemetryManager`
  - 指标、事件和 northbound 查询/推送

### 7.4 最小 northbound 面

当前范围不要求完整复制 `RESTCONF/NETCONF/SNMP/Telemetry` 协议，但应至少保留以下平台级查询/订阅能力：

- 查询拓扑
- 查询地址映射
- 查询路由状态
- 查询健康状态
- 查询资源摘要
- 订阅关键事件

### 7.5 与 guest 和 workload 的边界

guest 设备面、guest UAPI、control plane、workload 的关系应明确为：

- guest 设备面
  - 暴露 `UBC`、`Entity`、`Port`、`UMMU`、`URMA` 端点
- guest UAPI
  - 暴露 `/dev/ubcoreX`、ioctl、mmap、sysfs、netlink 的最小可见面
- control plane
  - 负责枚举、地址、路由、资源、健康和遥测
- workload
  - 消费上述平台能力，但不决定这些对象模型

---

## 8. 模块设计

本章中的模块名默认表示**建议的职责边界**，不是仓库中已经存在的 Rust 文件名；是否最终落成单独 `.rs` 文件、子模块目录或 crate，留待实现阶段决定。

### 8.1 模块分层与依赖方向

当前章节若只罗列模块，容易把“能调用什么”和“谁拥有状态”混在一起。实现时建议显式采用以下依赖方向：

```text
Workload Target Module
        |
        v
Program Model Module -----> Ring Lifecycle Module
        |                           |
        v                           v
Routing Module ------------> Cache Lifecycle Module
        |                           |
        v                           v
Hierarchy Module ----------> Backend Adapter Module
        |                           |
        v                           v
Guest UAPI Module ---------> Data Service Module
        |                           |
        +-------------> Topology Builder
                              |
                              v
                        QEMU Integration Module
                              |
                              v
                           Core Types

Event Engine 横切所有层，负责驱动、调度、trace 和 replay。
```

依赖约束应明确为：

- `Core Types` 是最底层公共定义，不依赖其他业务模块
- `QEMU Integration Module` 只负责平台对象、设备和 host/QEMU bridge，不直接决定 routing 或 cache policy
- `Topology Builder` 负责把配置变成对象图；它产出拓扑，但不执行请求
- `Hierarchy Module` 持有运行时主对象，例如 simulated `LevelNode` / `BlockStore` / allocator / integrity verifier
- `Routing Module` 只做选路，不拥有 block 生命周期状态
- `Cache Lifecycle Module` 只做 hit/miss/fetch/promote/evict/quarantine 的状态迁移，不直接生成 topology
- `Program Model Module` 和 `Ring Lifecycle Module` 负责 `PyPTO` / `simpler` 语义，不直接修改 `UB` platform object
- `Backend Adapter Module` 是 L3 host runtime 与 L2 chip backend 的唯一边界，不应把 `simpler` 语义散落到其他层
- `Guest UAPI Module` 与 `Data Service Module` 都是“能力暴露面”，它们消费 platform/runtime 对象，但不反向拥有核心拓扑
- `Workload Target Module` 只消费平台能力，不能回写平台对象模型
- `Event Engine` 是横切引擎，不应该承载具体业务规则

### 8.2 Core Types

定义仿真共享类型：

- `BlockHash`
- `BlockMeta`
- `BlockHandle`
- `RequestId`
- `NodeId`
- `LevelId`
- `SimTimestamp`
- `LatencyModel`
- `HealthStatus`
- `IntegrityState`
- `PlLevel`
- `FunctionLabel`
- `LogicalSystemId`
- `TaskCoord`
- `ScopeDepth`
- `TaskKey`
- `ScopeTokenState`
- `RingLayerId`

要求：

- 尽量与 v8 设计中的类型命名一致
- 能直接映射到生产 traits
- 能映射到 Linqu / PyPTO 的 hierarchy label 和 task key
- 能覆盖 `(scope_level, task_id)` 到 `(logical_system, L7..L0, scope_depth, task_id)` 的演进

### 8.3 QEMU Integration Module

这是新的底层基座模块。

职责：

- 定义 QEMU machine / board / device 的抽象映射
- 生成虚拟 host、chip、memory region、bus/fabric
- 挂载 Lingqu 数据服务所需的虚拟设备或 host service 入口
- 为上层 `UB/Linqu` 规范接口和 PyPTO 编程视图暴露稳定的平台接口

当前范围中不要求完整修改 QEMU 主线代码，但 HLD 上要明确以下三类接入点：

- QEMU machine model
- QEMU device model
- QEMU 外部协同服务接口

### 8.4 Topology Builder

负责根据配置构建逻辑拓扑：

- Host 数
- 每 Host 下映射的 `UBPU` / `Entity` 数
- `UB domain` 局部划分与 fabric 分组
- 可选的 level collapsing

输出：

- `SimTopology`
- `HierarchyTree`
- `LevelMap`

额外要求：

- 保留 L0-L6 的层级编号语义
- 保留 L0-L7 的层级编号语义
- 支持 active / collapsed / stubbed level
- 拓扑树不仅服务缓存路由，也服务 dispatch trace

### 8.5 Hierarchy Module

这是仿真核心，提供生产 trait 的仿真实现：

- simulated `BlockStore`
- simulated `LevelNode`
- simulated allocator
- simulated integrity verifier

各 level 的行为差异由参数控制，而不是派生大量完全独立的实现。

建议保留三个更贴近 `UB` 的对象壳：

- UBPU endpoint object
- host OS node object
- UB domain node object

但其内部共享绝大部分逻辑。

这个模块还应同时暴露两组能力：

- serving hierarchy：`BlockStore` / `LevelNode` / `LevelAllocator`
- runtime hierarchy：function label、task coordinate、scope 和 dispatch 边界

### 8.6 Routing Module

实现两套路由器：

- recursive router
- flat routing baseline

这样仿真系统不仅能证明 MVP 能跑，还能拿到基线对比。

主要职责：

- 计算 child score
- 生成 route path
- 记录 route reason
- 记录 route path 与 level path 的映射关系

### 8.7 Cache Lifecycle Module

负责 block 生命周期管理：

- hit
- miss
- fetch
- promote
- demote
- evict
- discard
- quarantine

这里应当把“主流程”显式化，避免逻辑散落在 store/node/router 中。

此外需要保留两条生命周期线：

- block lifecycle
- task lifecycle

当前范围里 task lifecycle 可以简化，但不能缺失。

### 8.8 Program Model Module

职责：

- 表达 PyPTO `pl.Level` 标签
- 表达 `@pl.function(level=...)` 和 `pl.at(level=...)` 的最小运行时语义
- 维护 `FunctionLabel`、`TaskCoord`、`ScopeDepth`
- 维护局部 `TaskKey(scope_level, task_id)` 与 full `TaskCoord`
- 把 serving request 映射成 runtime-level trace

当前范围不做真实 PyPTO 编译，只做层级标签与 dispatch 语义建模。

### 8.9 Ring Lifecycle Module

职责：

- 建模 `task_ring[d]` / `buffer_ring[d]` / `last_task_alive[d]`
- 建模 `pl.free` 的 scope token 语义
- 建模 layer-local retire，而不是单一全局 retire
- 输出 ring pressure / block reason / retire trace

注意：

- 当前范围可以是逻辑模型，不必实现真实 lock-free ring
- 但语义必须与 `multi_level_runtime_ring_and_pypto_free_api.md` 对齐

### 8.10 Backend Adapter Module

职责：

- 建模本系统与 `simpler` 的边界
- 提供 chip backend boundary object / simpler boundary adapter
- 模拟 host 到 chip backend 的 dispatch 成本和返回语义

当前范围不执行真实设备逻辑，但必须能在 trace 中表现出这个边界。

### 8.11 Guest UAPI Module

职责：

- 建模 guest 用户态可见接口，而不只建模内核内部对象
- 暴露 `/dev/ubcoreX` 风格的设备面
- 暴露最小 ioctl / mmap / netlink / sysfs 语义
- 为后续 workload 或测试脚本提供 Linux 风格的交互入口

当前范围不要求完整复制所有 UAPI，但至少要能表达：

- 设备查询
- `Jetty` / queue / `Segment` 的最小资源创建
- 事件通知
- `UBUS` / `URMA` sysfs 可观测面

同时，Lingqu data system 不应只是空壳 adapter。当前范围建议提供四类最小可运行服务：

- shmem service adapter
- block service adapter
- dfs service adapter
- db service adapter

这些模块在当前范围中至少应提供：

- 明确的 API 面
- 逻辑状态模型
- 延迟模型
- 失败模型
- 可验证的 trace / metrics

### 8.12 Data Service Module

职责：

- 聚合 `lingqu_shmem` / `lingqu_block` / `lingqu_dfs` / `lingqu_db` 的仿真实现
- 为 runtime / serving 路径提供可调用的数据平面能力
- 统一暴露 completion、failure、latency 和 metrics

若最终拆分到源码目录，建议子模块可包括：

- `data_services/shmem`
- `data_services/block`
- `data_services/dfs`
- `data_services/db`

每类服务的最小能力如下：

- `lingqu_shmem`
  - shared region create/map
  - PE discovery / visibility model
  - put/get
  - barrier / sync

- `lingqu_block`
  - `(uba, lba, length, flags)` 描述符
  - async read / async write
  - completion queue
  - producer / consumer completion 归因

- `lingqu_dfs`
  - global path namespace
  - open / read / write / pread / pwrite / stat
  - metadata path 与 data path 分离延迟

- `lingqu_db`
  - `GET/SET/MGET/MSET/DEL`
  - `HGET/HSET`
  - batched pipeline
- `PUBLISH/SUBSCRIBE` 最小通知模型

### 8.13 Workload Target Module

职责：

- 挂载具体验证目标
- 第一个目标是 `rust_llm_server_design_v8.md` 的 MVP
- 其他目标可以是 Lingqu data service 自检场景、PyPTO trace 回放、未来的 compiler/runtime integration case

这层的原则是：

- Linqu simulator 是平台
- LLM server MVP 是 workload target，不是平台本身

### 8.14 Event Engine

负责事件驱动执行：

- `RequestArrive`
- `LookupStart`
- `FetchComplete`
- `PromoteComplete`
- `EvictionStart`
- `EvictionComplete`
- `IntegrityFailureDetected`
- `NodeHealthChanged`
- `ScenarioCheckpoint`
- `FunctionDispatchStart`
- `FunctionDispatchComplete`
- `ScopeEnter`
- `ScopeExit`
- `PlFree`
- `RingRetireScan`
- `RingRetireBlocked`
- `ShmemOpComplete`
- `BlockIoComplete`
- `DfsOpComplete`
- `DbOpComplete`

事件引擎要支持：

- 定时触发
- 因果链追踪
- 固定随机种子重放

### 8.15 Workload Generator

用于生成请求流。当前范围只需要支持三类 workload：

- `HotsetLoop`: 小热集重复访问
- `SkewedZipf`: 长尾分布访问
- `BurstFailover`: 运行中叠加节点故障

每个请求最少包含：

- 请求时间
- 前缀 hash
- block 数量或 token span
- 优先级
- 可选 function label
- 可选 logical system / task coordinate seed

workload 输入应支持两类来源：

- serving-native 访问流
- PyPTO-shaped hierarchy-labeled trace

### 8.16 Fault Injection Module

用于故障注入：

- 节点 down
- 节点 degraded
- block corruption
- store capacity exhaustion
- allocator unavailable
- ring layer pressure
- Lingqu data service unavailable
- shmem visibility / sync failure
- block device timeout
- dfs metadata unavailable
- db replica / pubsub drop

故障必须是可配置、可重放、可统计的。

### 8.17 Metrics and Reporting Module

输出统一指标：

- 请求数
- 路由次数
- 各 level hit/miss
- fetch/promote/demote/evict 次数
- 平均/分位延迟
- 故障数
- quarantine 数
- 各 level occupancy
- 各 `pl.Level` dispatch 次数
- scope enter / exit 次数
- `pl_free_total`
- `ring_retire_blocked_total`
- `ring_layer_occupancy{depth=...}`
- `shmem_ops_total{op=...}`
- `block_io_total{op=read|write}`
- `dfs_ops_total{op=...}`
- `db_ops_total{op=...}`
- `qemu_device_events_total{device=...}`
- `qemu_vm_exit_total{reason=...}`

应同时支持：

- 终态 summary
- 时间序列采样
- 每事件 trace

### 8.18 CLI and Runner Surface

提供最小命令行接口：

```bash
sim-run --scenario scenarios/mvp_2host_single_domain.yaml
sim-run --scenario scenarios/eviction_pressure.yaml --router recursive
sim-run --scenario scenarios/failover.yaml --compare flat,recursive
```

### 8.19 主执行流

为了避免模块关系只停留在静态依赖，建议把“一个请求如何穿过模块”固定成如下主执行流：

```text
Workload Generator / Workload Target
        |
        v
Event Engine: RequestArrive
        |
        v
Program Model Module
  - 生成 FunctionLabel / TaskCoord / ScopeDepth
        |
        v
Routing Module
  - 基于 Hierarchy Module 提供的节点视图选路
        |
        v
Cache Lifecycle Module
  - 判断 hit/miss/fetch/promote/evict/quarantine
        |
        +------ miss/fetch ------> Data Service Module
        |                              |
        |                              v
        |                       Guest UAPI / QEMU Integration
        |
        +------ dispatch ------> Backend Adapter Module
                                       |
                                       v
                                 L2 chip boundary
```

这条执行流中的 ownership 应明确为：

- `Workload Generator` 只产生请求，不拥有平台状态
- `Program Model Module` 拥有 runtime-level label 和 task-coordinate 视图
- `Routing Module` 只产出 route decision，不修改 block state
- `Cache Lifecycle Module` 拥有 block 生命周期状态迁移
- `Hierarchy Module` 提供节点/容量/健康/完整性等主状态查询接口
- `Backend Adapter Module` 只负责 L3→L2 边界
- `Guest UAPI` / `Data Service` / `QEMU Integration` 负责把动作映射到底层对象和设备面
- `Event Engine` 只负责时序推进与因果串接

### 8.20 主控制流与可观测性流

除请求执行流外，还存在一条持续运行的控制/观测流。建议显式表达如下：

```text
Scenario / CLI
    |
    v
Topology Builder -----> Hierarchy Module -----> Routing / Cache / Backend
    |                     |                          |
    |                     v                          v
    +--------------> Fault Injection Module ----> Event Engine
                          |
                          v
                 Metrics and Reporting Module
                          |
                          v
                    summary / trace / replay
```

这条流回答的是：

- 谁创建对象图：`Topology Builder`
- 谁维护主运行态：`Hierarchy Module`
- 谁注入扰动：`Fault Injection Module`
- 谁把扰动和行为变成事件：`Event Engine`
- 谁沉淀证据：`Metrics and Reporting Module`

因此模块边界应进一步约束为：

- `Topology Builder` 只能在初始化或重配置点修改对象图
- 日常运行时的健康、容量、完整性和 route-related state 由 `Hierarchy Module` 持有
- `Fault Injection Module` 不直接修改 report，而是通过事件驱动状态变化
- `Metrics and Reporting Module` 只读核心状态和事件，不反向驱动业务逻辑

### 8.21 模块关系到源码组织的映射建议

如果后续落到实际代码目录，建议优先按“稳定依赖方向”组织，而不是按“功能名堆文件夹”组织：

- `core/`
  - 放 `Core Types`
- `platform/`
  - 放 `QEMU Integration`、`Topology Builder`
- `runtime/`
  - 放 `Hierarchy`、`Routing`、`Cache Lifecycle`、`Program Model`、`Ring Lifecycle`、`Backend Adapter`
- `services/`
  - 放 `Guest UAPI`、`Data Service`
- `engine/`
  - 放 `Event Engine`、`Fault Injection`、`Metrics and Reporting`
- `workloads/`
  - 放 `Workload Target`、`Workload Generator`
- `cli/`
  - 放 `CLI and Runner`

这样拆分的好处是：

- `platform` 不需要依赖 `workloads`
- `runtime` 不需要依赖 `cli`
- `engine` 可以横切 `platform` 与 `runtime`，但不拥有业务规则
- `workloads` 只消费 `runtime/services/engine` 暴露的稳定接口

---

## 9. 执行模型

### 9.1 仿真时钟

系统维护一个逻辑时钟 `now`。

所有行为都通过事件推进：

1. 取出最早事件
2. 执行状态变更
3. 生成后续事件
4. 更新 metrics 和 trace
5. 推进到下一事件

对 QEMU-based 方案，建议理解为：

- QEMU 提供底层时间推进和设备事件源
- `UB/Linqu` 仿真核心在其上维护高层运行时事件、控制面事件和数据服务事件
- 当前范围可以通过宿主调度器把两者桥接，而不必一开始就深度侵入 QEMU 内核

### 9.2 双重执行视图

当前执行模型应同时覆盖两条路径：

1. serving path
- 请求如何路由
- block 在哪一层命中
- 是否发生 promotion / eviction / fallback

2. programming/runtime view
- 请求带什么 function label
- 运行在哪个 `pl.Level`
- 是否经过 host runtime 到 chip backend 的 dispatch
- scope 何时 enter / exit
- ring layer 如何分配与 retire

### 9.3 请求处理主流程

对一个请求，当前仿真系统的标准流程如下：

1. 请求到达
2. 按配置生成目标 block hashes
3. 从 root 节点开始递归路由
4. 在目标 leaf 或中间层执行 `contains`
5. 命中则记录 hit latency
6. 未命中则向父层或上层 store 查找
7. 找到后执行 promotion 链
8. 若目标层容量不足，先触发 cascading eviction
9. promotion 完成后返回成功
10. 若发生完整性失败或节点故障，则重试或绕行

对于带 hierarchy label 的请求，还应补充：

0. 解析或生成 `FunctionLabel` / `TaskCoord`
11. 完成 task lifecycle 收尾和 trace 归档

### 9.4 scope / ring 生命周期

当前范围需要显式模拟下列运行时行为：

1. `ScopeEnter`
- `current_scope_depth += 1`
- 当前任务/输出绑定到 ring layer `d`

2. task creation
- 获得局部 `TaskKey(scope_level, task_id)`
- 初始化 `fanout_count`
- 初始化 scope token 状态

3. `pl.free`
- 提前应用 scope token
- 不改变 fanout 安全条件
- 必须幂等

4. `ScopeExit`
- 对当前 scope 内尚未 `free` 的任务应用 scope token
- 触发 layer-local retire scan

5. retire
- 条件为 scope token 已应用且 `ref_count == fanout_count`
- retire 是 layer-local，不应依赖外层 ring head 先前进

### 9.5 与 `simpler` 的边界流

当一个请求需要进入 chip backend 边界时，仿真器执行：

1. `FunctionDispatchStart`
2. chip backend boundary object 接收一个 L2 dispatch
3. 计算边界延迟和结果
4. `FunctionDispatchComplete`

这里不模拟真实 AIC/AIV 执行，但必须保留“host runtime 发起 dispatch，chip backend 响应”的边界语义。

在 QEMU 视角下，这一边界可映射为：

- host side runtime process / thread
- chip backend virtual device or host-side adapter
- guest-visible `UBPU` / `Entity` endpoint
- h2d / d2h 作为显式边界事件

如果进一步贴近当前 Linux 内核实现，这个 guest-visible endpoint 最好还能被解释成：

- `ubfi` 可发现的固件对象
- `ubus` 可枚举的 `Entity` / `Port` / `Decoder`
- `ubcore` / `uburma` 可绑定的通信端点

### 9.5.1 是否需要 PyPTO device 仿真

对当前 `rust_llm_server_design_v8.md` 的 MVP 来说，答案是：**暂时不需要完整的 `pypto` device 仿真。**

MVP 关注的是：

- 递归层级树能否承载 `BlockStore` / `LevelNode` / `LevelAllocator`
- L2/L3/L4 的 route / fetch / store / evict / promote / quarantine 是否闭环
- host orchestration 与 chip backend 的接口是否稳定
- latency / capacity / fault / integrity 模型是否足够支撑设计验证

因此，L2 侧当前只需要：

- 一个 chip backend boundary object
- 一个 guest-visible `UBPU` / `Entity` endpoint
- `dispatch` / `completion` / `h2d` / `d2h` / `fault` 的边界事件
- 可配置的 device-facing latency / bandwidth / queue-depth / failure 行为模型

当前 **不要求** simulator 去实现：

- `pypto` / `simpler` 的 device-side task ring
- scope token、`pl.free`、layer-local retire 在 device 内部的真实执行
- AIC/AIV kernel、DMA engine、device scheduler 的内部状态机

换句话说，当前 MVP 需要的是 **chip backend 边界仿真**，而不是 **完整 device runtime 仿真**。

只有在后续目标变成以下内容时，`pypto device` 仿真才应进入必做范围：

- 验证 `pl.at(level=L0/L1/L2)` 的真实 device-side dispatch 语义
- 验证 scope-driven ring stack 与 device 执行的耦合正确性
- 评估 device runtime 内部调度、背压、并发和 kernel overlap
- 让 simulator 直接承载 `simpler` 以内的运行时回归测试

### 9.5.2 升级到真实 device-side runtime 验证时的新增组件

当目标升级到“验证真实 `pl.at(level=L0/L1/L2)` 语义”或“直接运行 `simpler` 内回归测试”时，`pypto device` 仿真本身还不够，至少还需要以下组件一起到位：

- `ChipBackend` 真实适配层
  - 把 host/L3 runtime 的 `dispatch`、`completion`、错误传播与 task identity 映射真正接到 L2
  - 不再允许只用 stub latency 表示 device 响应
- Tier-2 内存边界与 DMA 模型
  - host DRAM 与 device GM 必须是显式分离的内存域
  - 必须支持 `h2d_copy` / `d2h_copy`、buffer handle 映射、完成队列、超时和 fault injection
- L0-L2 runtime 状态机
  - `task_ring[L][d]`、`buffer_ring[L][d]`、`last_task_alive`
  - `scope.enter` / `scope.exit`
  - `pl.free`、`task_freed`
  - `fanout_count` / `ref_count` / layer-local retire
- L0/L1/L2 拓扑与调度模型
  - `AIC` / `AIV` / `core-group`
  - optional `L1` die topology
  - `InCoreFunctionGroup`、TPUSH/TPOP 或等价的共调度约束
- compiler metadata 消费链路
  - runtime 必须真正消费 hierarchy label、outlined function metadata、function group 信息
  - 否则只能验证外层 workload，不能验证 `pl.at(level=...)` 本身
- profiling / trace / replay
  - 必须能检查 ring occupancy、retire ordering、dispatch/completion ordering、DMA latency、missing/late `pl.free`
  - 这既是调试手段，也是回归测试判定依据

因此，升级后的目标应被视为一个完整能力包：

- 更细粒度的 `UB` 设备/内存边界仿真
- 真正接入的 `PyPTO` / `simpler` device runtime 语义
- compiler label → runtime dispatch → device execution → trace/profiling 的验证闭环

### 9.5.3 PyPTO device 仿真与 UB 仿真的关系

二者应明确建模为上下分层关系，而不是两个互斥选项：

- `UB` 仿真是下层机器/设备/互连对象模型
- `PyPTO device` 仿真是上层 device runtime 执行语义模型

分工如下：

- `UB` 仿真负责提供 guest-visible `UBPU` / `Entity` / `UMMU` / resource space / queue / doorbell / completion / DMA / memory region / RAS fault
- `PyPTO device` 仿真负责消费这些对象和边界，在其上实现 `simpler`/L0-L2 的 `task_ring`、`buffer_ring`、scope token、`pl.free`、retire 和 function-group 调度语义

推荐的分层栈为：

1. QEMU machine / board / bus
2. `UB` guest-visible device model
3. `PyPTO` / `simpler` device runtime model
4. host-side Linqu runtime / workload harness

这样定义后，两者的职责边界就很清楚：

- 只有 `UB` 仿真，没有 `PyPTO device` 仿真：只能证明设备对象和队列边界存在，不能证明 `pl.at(level=L0/L1/L2)` 的真实运行时语义
- 只有 `PyPTO device` 仿真，没有 `UB` 仿真：runtime 语义失去 guest-visible 设备落点，很难与平台模型、Linux 设备栈和后续真实化路径对齐

因此，要支持 `simpler` 内回归或真实 device-side runtime 验证，最终需要的是一套组合栈，而不是单独一个 `device simulator`。

### 9.6 与 Lingqu Data System 的关系

在更完整的 Lingqu 体系中，runtime 会通过以下服务与数据平面交互：

- `lingqu_shmem`
- `lingqu_block`
- `lingqu_dfs`
- `lingqu_db`

对当前范围来说，这些服务不是纯占位，而是要提供最小可验证能力：

- `lingqu_shmem`
  - 可验证共享内存 region、put/get、barrier
- `lingqu_block`
  - 可验证异步 block read/write 与 completion
- `lingqu_dfs`
  - 可验证 namespace、文件读写和 metadata/data path
- `lingqu_db`
  - 可验证 KV/Hash/pipeline/pubsub 基本语义

不在当前范围中实现真实协议，但必须能被 scenario 驱动并产出可检查结果。

对 QEMU-based 平台，建议按两类方式落地：

- QEMU 设备模型
  - 适合 `UBPU` endpoint、`Entity` resource space、`UMMU`/`UB Decoder` 最小边界、block / shmem / doorbell / completion
- 宿主侧控制面与基础设施服务
  - 适合 `UBM`、`UB` 管控面基础设施、`UB Service Core` 外壳、dfs / db 这类平台能力，再通过虚拟设备或 RPC gateway 接到客体

对照当前 Linux 实现，当前范围更具体的优先级应当是：

- 先保证 `UBRT` / `ACPI` / `DTS` 到 `ubfi`
- 再保证 `ubfi` 到 `ubus` / `ummu`
- 再保证 `ubcore` / `uburma` / `vfio-ub` 所依赖的最小对象边界
- 最后再把 `UBM` / `UB Service Core` 的更高层能力叠上去

### 9.7 容量与淘汰

每个 level store 维护：

- `capacity_blocks`
- `used_blocks`
- `high_watermark`
- `low_watermark`
- `eviction_policy`

当 promotion 或 store 导致超过高水位时：

1. 触发当前层 eviction
2. 被淘汰 block 尝试下沉到上一层可容纳位置
3. 若上一层也高水位，则继续级联
4. 顶层无法容纳时丢弃

### 9.8 完整性与健康状态

当前范围只模拟统一语义，不模拟真实 checksum 算法：

- `Healthy`
- `Degraded`
- `Failed`

完整性状态：

- `Valid`
- `Corrupted`
- `Quarantined`

当 block 被标记为 `Corrupted`：

1. 当前 fetch 失败
2. 记录 integrity failure
3. block 进入 quarantine
4. 路由器可尝试其他副本或上层回源

---

## 10. 场景设计

当前范围的场景不再按离散小项验收，而是收束为三个场景组。每个场景组内部可以包含多个 topology / load / failure 子变体，但对外按场景组闭环。

### 10.1 场景组 U：UB/Linqu 平台与基础设施验证

该场景组收束原先的前 5 个场景以及所有面向 `UB` 平台对象、基础设施和 guest 可见性的场景，统一验证平台本体是否成立。

覆盖原场景：

- 场景 A：2 Host / 单 Switch / 基本 promotion
- 场景 B：GPU 容量紧张 / cascading eviction
- 场景 C：Host 故障绕行
- 场景 D：Block corruption
- 场景 E：热集与长尾混合负载
- 场景 H：Linqu 平台自举验证
- 场景 I：`lingqu_shmem` 基本验证
- 场景 J：`lingqu_block` 异步 IO 验证
- 场景 K：`lingqu_dfs` namespace 验证
- 场景 L：`lingqu_db` KV 与 pub/sub 验证
- 场景 N：guest UAPI 最小可见性验证

核心目的：

- 验证 QEMU 平台下的 host / `UBPU` / `Entity` / memory tier / `UB domain` 映射
- 验证 guest-visible `UB` 对象、queue、doorbell、completion、DMA、resource space 和最小 UAPI 边界
- 验证 topology、routing、promotion、eviction、integrity、quarantine、failover 等平台基础行为
- 验证 `lingqu_shmem` / `lingqu_block` / `lingqu_dfs` / `lingqu_db` 的最小可验证落点
- 验证后续真实 Linux `ubfi` / `ubus` / `ummu` / `ubcore` 接入路径不是伪接口

建议子变体：

- `u_topology_bootstrap`
- `u_promotion_eviction`
- `u_failover_integrity`
- `u_data_services`
- `u_guest_uapi_visibility`

各子变体建议说明如下。

#### `u_topology_bootstrap`

输入/拓扑：

- 最小 `2-host / single-switch / single-domain`
- 每个 host 含一个 guest、一个或多个 `UBPU` endpoint、最小 `Entity` / `UMMU` / resource-space

驱动动作：

- 启动 QEMU machine 和宿主侧控制面
- 完成 topology 枚举、地址分配、route 初始化
- 加载最小 block/shmem backing，发起一轮基础 lookup / fetch / store 请求

关键观察点：

- guest 内是否能看到预期 `UB` 对象集合
- `UBPU` / `Entity` / host / `UB domain` / memory tier 映射是否一致
- route trace、dispatch trace 和 topology report 是否彼此对齐

验收要点：

- 平台能稳定自举并输出统一 topology report
- guest-visible 对象与控制面对象一一对应
- 至少一条基本请求路径可从 L4 走到 L2 并回传 completion

#### `u_promotion_eviction`

输入/拓扑：

- 与 `u_topology_bootstrap` 相同或稍大规模 topology
- 刻意压低 L2/L3 容量，制造高水位

驱动动作：

- 先预热一批 blocks 形成局部命中
- 再注入超出 L2 容量的请求流，触发 promotion 与 eviction
- 重放一轮热点请求，观察是否出现预期回升

关键观察点：

- L4 命中后提升到 L2 的路径
- L2 高水位时是否触发 L2→L3→L4 级联下沉
- 各层 `used_blocks`、eviction 次数、promotion 次数和 hit/miss 变化

验收要点：

- promotion / cascading eviction 顺序可解释
- 不出现“块丢失但无告警”的静默错误
- 同一 workload 在不同容量配比下呈现可预期的行为差异

#### `u_failover_integrity`

输入/拓扑：

- 至少 `2-host`，每 host 至少一个可选 route target
- 开启 fault injection 和 integrity injection

驱动动作：

- 运行正常请求流后注入 host down、endpoint unreachable 或 route degraded
- 对部分 block 注入 corruption / checksum failure / quarantine
- 继续发起请求，观察 fallback、绕行或失败语义

关键观察点：

- recursive routing 是否绕开失败 host
- integrity failure 是否被记录并触发 quarantine
- fallback 到其他副本或上层回源的路径是否完整

验收要点：

- flat vs recursive 的行为差异可观测
- corruption 不会被当成正常命中吞掉
- 故障后的 route/health/resource 摘要会及时收敛

#### `u_data_services`

输入/拓扑：

- 最小 `lingqu_shmem` / `lingqu_block` / `lingqu_dfs` / `lingqu_db` 服务桩
- 可选择 QEMU device model 或 host service gateway 作为接入方式

驱动动作：

- `shmem`：建立 region、执行 put/get、barrier
- `block`：发起异步 `(UBA, LBA)` 读写并等待 completion
- `dfs`：执行 namespace、metadata path、data path 的最小读写
- `db`：执行 KV/Hash、pipeline 和一条 publish/subscribe 链路

关键观察点：

- guest 请求如何映射到虚拟设备或宿主侧服务
- completion、barrier、namespace、pub/sub 等事件是否能出现在统一 trace 中
- 数据服务对象与 `UB`/platform object 的映射是否明确

验收要点：

- 四类服务都具备最小可运行、可观测、可验证能力
- service trace 能与 topology / health / tenant 视图关联
- 不要求真实协议完整实现，但必须能被 scenario 精确驱动

#### `u_guest_uapi_visibility`

输入/拓扑：

- guest 内暴露最小 `/dev/ubcoreX` 风格设备面
- 暴露至少一条 ioctl / mmap / netlink / sysfs 观察路径

驱动动作：

- 在 guest 内枚举对象
- 发起最小控制操作、状态读取或映射动作
- 将 guest 侧看到的对象与宿主侧控制面对象做交叉校验

关键观察点：

- `ubfi -> ubus -> ubase -> ubcore/uburma -> ummu/vfio-ub` 的对象边界是否能闭环表达
- guest 用户态是否能观察和操作 `UBUS` / `URMA` 最小对象
- UAPI 是否对应真实后续 Linux 驱动接入路径

验收要点：

- 至少一条完整 guest UAPI 观测/操作链跑通
- guest-visible object 与宿主侧仿真对象保持一致
- 不使用只为 demo 存在的伪接口

### 10.2 场景组 P：PyPTO / simpler 语义验证

该场景组专门收束 `PyPTO` 语义相关场景，用来验证 hierarchy label、scope/ring 和 device dispatch 语义不会在平台化过程中丢失。

覆盖原场景：

- 场景 F：PyPTO 层级标签透传
- 场景 G：`pl.free` 与 ring-layer retire

核心目的：

- 验证 `pl.Level` / hierarchy label 在 trace、dispatch、scenario report 中不丢失
- 验证 host runtime → chip backend → device runtime 的语义链路可观测
- 验证 `pl.free`、scope token、`fanout_count` / `ref_count`、layer-local retire 的关键不变量
- 为后续接入真实 `PyPTO device` 仿真和 `simpler` 回归测试保留兼容场景骨架

建议子变体：

- `p_level_label_passthrough`
- `p_scope_ring_retire`

各子变体建议说明如下。

#### `p_level_label_passthrough`

输入/拓扑：

- 一个包含 `pl.at(level=...)` 标签的 synthetic PyPTO trace 或最小 outlined function 集
- 至少覆盖 L2、L3，以及一个保留级别标签

驱动动作：

- 从 host runtime 发起 dispatch
- 经 `ChipBackend` 边界投递到 chip backend boundary object
- 输出 dispatch trace、function metadata report、scenario summary

关键观察点：

- `pl.Level` / hierarchy label 是否从 compiler metadata 透传到 runtime trace
- dispatch 过程中是否保留 level、task coordinate、function identity
- host path 与 device-facing path 是否能在同一 report 中对齐

验收要点：

- label 不丢失、不被 workload 私有字段替代
- 同一 dispatch 在 topology 视图和 runtime 视图中可相互定位
- 为后续真实 `PyPTO device` 仿真保留兼容 metadata contract

#### `p_scope_ring_retire`

输入/拓扑：

- synthetic scope trace 或最小 `pl.free` 示例程序
- 至少包含 outer scope、inner scope、fanout pending、repeated free 几种情况

驱动动作：

- 构造若干 task/output，记录 `fanout_count`、`ref_count`
- 在 inner scope 内执行 `pl.free`
- 触发 scope exit、retire scan 和 blocked/unblocked 过程

关键观察点：

- `pl.free` 是否只提前应用 scope token，而不绕过 fanout safety
- inner scope 的 retire 是否不再被 outer scope head 阻塞
- repeated `pl.free` 是否保持幂等

验收要点：

- `task_freed`、`ref_count`、`fanout_count` 的变化可解释
- ring pressure 指标和 blocked trace 与 retire 顺序一致
- 为未来 `simpler` 回归测试保留一致的语义判定标准

### 10.3 场景组 M：`rust_llm_server` MVP workload 验证

该场景组专门承载 `rust_llm_server_design_v8.md` 的 MVP，作为首个 workload 验证目标，与平台/语义基础场景分开验收。

覆盖原场景：

- 场景 M：`rust_llm_server_design_v8.md` MVP 作为首个 workload

核心目的：

- 在 QEMU-based Linqu simulator 上挂载 LLM serving MVP
- 验证 `BlockStore` / `LevelNode` / `LevelAllocator` 在 workload 中真正闭环
- 验证 recursive hierarchy、routing、promotion/eviction、integrity、data services 与平台运行时/控制面能力的组合行为
- 证明该平台不是为某一 workload 特化，而是能承载第一个真实上层系统

建议子变体：

- `m_single_domain_mvp`
- `m_eviction_pressure`
- `m_failover_compare_flat_vs_recursive`

各子变体建议说明如下。

#### `m_single_domain_mvp`

输入/拓扑：

- 最小 `2-host / single-domain` serving topology
- 一个 MVP workload profile，覆盖 lookup、fetch、promotion 和基础回源

驱动动作：

- 启动 `rust_llm_server` MVP workload harness
- 构建 `BlockStore` / `LevelNode` / `LevelAllocator` 树
- 执行一轮稳定请求流并导出 metrics/report

关键观察点：

- workload 是否真正消费平台提供的 `BlockStore` / `LevelNode`
- recursive hierarchy 和 serving path 是否闭环
- 平台 trace、serving trace、resource summary 是否能相互映射

验收要点：

- MVP 可在 simulator 上稳定跑通
- 不需要依赖平台外的专用 mock 才能成立
- 报告中能清楚区分平台行为和 workload 行为

#### `m_eviction_pressure`

输入/拓扑：

- 与 `m_single_domain_mvp` 相同，但显著压低缓存容量
- 构造热集与长尾混合 workload

驱动动作：

- 先预热热集，再注入长尾请求
- 持续运行直到触发多轮 promotion / eviction / 回源
- 导出 workload 级 hit rate、latency 和资源占用变化

关键观察点：

- serving path 中的 eviction 是否与平台基础场景一致
- hit rate、tail latency 与各层容量/高低水位之间的关系
- data service 和 integrity 机制是否在 workload 中保持正确交互

验收要点：

- workload 行为能解释平台级 promotion/eviction 事件
- 指标变化具有可重复性，不依赖偶然调度
- 可作为后续参数调优和性能对比基线

#### `m_failover_compare_flat_vs_recursive`

输入/拓扑：

- 至少两个可用 route target
- 同时开启 `flat` 与 `recursive` 两种 router 配置
- 开启 host failure 或 degraded domain 注入

驱动动作：

- 在正常条件下分别运行两套路由策略
- 注入故障后重复相同 workload
- 对比 route decision、回源次数、latency 和失败率

关键观察点：

- recursive routing 是否优先保留局部性并减少无效候选评估
- flat routing 与 recursive routing 的恢复行为是否不同
- failover 后 metrics / trace / report 是否仍然可解释

验收要点：

- 路由差异不仅体现在日志文字上，也体现在可量化指标上
- 故障后的 serving 行为与平台健康/路由视图一致
- 该子变体可作为对外演示 recursive hierarchy 价值的主场景

---

## 11. 配置设计

### 11.1 配置原则

- YAML 驱动
- 单文件可描述一个完整 scenario
- 结构尽量贴近未来生产配置
- 显式保留 Linqu / PyPTO hierarchy 语义
- `UBPU` / `Entity` / `UB domain` 等 `UB` 对象应作为一等配置对象出现

### 11.2 建议配置骨架

```yaml
scenario:
  name: mvp_2host_single_domain
  seed: 42
  duration_us: 1000000
  logical_system: llm-serving-mvp

platform:
  backend: qemu
  machine_profile: ub-host-minimal
  cpu_model: host
  memory_model: numa-sim
  device_model_mode: mixed

topology:
  hosts: 2
  ubpus_per_host: 4
  entities_per_ubpu: 2
  ub_domains:
    - id: domain0
      hosts: [0, 1]
  collapse:
    fabric: true
    global: true

ub_runtime:
  active_levels: [2, 3, 4]
  reserved_levels: [0, 1, 5, 6, 7]
  preserve_full_task_coord: true

pypto:
  enable_function_labels: true
  default_level: HOST
  allow_levels: [CHIP, HOST, CLUSTER_0]
  simpler_boundary:
    enabled: true
    chip_backend_mode: stub
    dispatch_latency_us: 15
  scope_runtime:
    enable_multi_layer_ring: true
    enable_pl_free: true
    max_scope_depth: 8

lingqu_data:
  shmem:
    enabled: true
    pe_count: 2
    default_latency_us: 3
  block:
    enabled: true
    devices:
      - { uba: "ssu0", blocks: 1048576, block_size: 4096 }
  dfs:
    enabled: true
    namespace_root: "/"
    metadata_latency_us: 20
    data_latency_us: 80
  db:
    enabled: true
    inline_value_limit: 64
    pipeline_batch_limit: 16

levels:
  l2_ubpu_tier:
    capacity_blocks: 1024
    high_watermark: 0.90
    low_watermark: 0.70
    hit_latency_us: 5
  l3_host_tier:
    capacity_blocks: 8192
    high_watermark: 0.90
    low_watermark: 0.70
    fetch_latency_us: 30
  l4_domain_tier:
    capacity_blocks: 65536
    high_watermark: 0.95
    low_watermark: 0.80
    fetch_latency_us: 80

routing:
  mode: recursive
  hit_weight: 10.0
  load_weight: 2.0
  capacity_weight: 1.0

workload:
  type: hotset_loop
  qps: 2000
  unique_prefixes: 256
  blocks_per_request: 4
  function_label_mode: host_orchestration

faults:
  - at_us: 300000
    type: host_degraded
    host_id: 0
  - at_us: 600000
    type: block_corruption
    level: ub_domain
    node_id: domain0
    block_hash: random_hot

outputs:
  trace: true
  metrics_csv: true
  summary_json: true
  emit_task_coord_trace: true
  emit_data_service_trace: true
  emit_qemu_platform_trace: true
```

### 11.3 场景组配置模板

为避免场景组定义停留在文字层，建议在配置中把 `U/P/M` 三组作为一等对象显式表达。

#### 11.3.1 场景组 U 模板

```yaml
scenario:
  group: U
  variant: u_topology_bootstrap

topology:
  hosts: 2
  ubpus_per_host: 2
  entities_per_ubpu: 2
  ub_domains:
    - id: domain0
      hosts: [0, 1]

guest_uapi:
  expose_ubcore_dev: true
  expose_sysfs: true
  expose_netlink: false
  expose_mmap: true

lingqu_data:
  shmem: { enabled: true }
  block: { enabled: true }
  dfs:   { enabled: false }
  db:    { enabled: false }

faults: []
```

适用说明：

- `variant=u_topology_bootstrap` 重点打开 topology / object visibility
- `variant=u_promotion_eviction` 重点压低 `levels.l2/l3` 容量并配置热点 workload
- `variant=u_failover_integrity` 重点开启 host fault / corruption fault
- `variant=u_data_services` 重点开启四类 `lingqu_data` 服务
- `variant=u_guest_uapi_visibility` 重点打开 `guest_uapi.*` 字段并最小化 workload

#### 11.3.2 场景组 P 模板

```yaml
scenario:
  group: P
  variant: p_scope_ring_retire

pypto:
  enable_function_labels: true
  default_level: HOST
  allow_levels: [CHIP, HOST, CLUSTER_0]
  simpler_boundary:
    enabled: true
    chip_backend_mode: stub
  scope_runtime:
    enable_multi_layer_ring: true
    enable_pl_free: true
    max_scope_depth: 8

runtime_semantics:
  emit_scope_events: true
  emit_ring_state: true
  emit_function_labels: true
  verify_pl_free_idempotent: true
  verify_layer_local_retire: true

synthetic_trace:
  enabled: true
  program: nested_scope_pl_free_demo
  include_levels: [2, 3, 4]
```

适用说明：

- `variant=p_level_label_passthrough` 重点打开 `emit_function_labels`
- `variant=p_scope_ring_retire` 重点打开 `emit_scope_events`、`emit_ring_state` 和语义校验开关
- 后续接入真实 `PyPTO device` 仿真时，优先扩展 `synthetic_trace.program` 与 `chip_backend_mode`

#### 11.3.3 场景组 M 模板

```yaml
scenario:
  group: M
  variant: m_single_domain_mvp

workload:
  type: rust_llm_server_mvp
  profile: single_domain_basic
  qps: 2000
  unique_prefixes: 256
  blocks_per_request: 4
  function_label_mode: host_orchestration

routing:
  mode: recursive

levels:
  l2_ubpu_tier:
    capacity_blocks: 1024
  l3_host_tier:
    capacity_blocks: 8192
  l4_domain_tier:
    capacity_blocks: 65536
```

适用说明：

- `variant=m_single_domain_mvp` 采用最小 `2-host / single-domain`
- `variant=m_eviction_pressure` 重点压低容量并提高 `unique_prefixes`
- `variant=m_failover_compare_flat_vs_recursive` 重点切换 `routing.mode` 并加入故障注入

### 11.4 关键字段约束与来源

为避免配置字段失去设计依据，建议对关键字段建立“字段名 -> 语义 -> 来源”的最小约束表。

| 字段 | 语义 | 主要来源 |
|---|---|---|
| `scenario.group` | 指定场景组 `U/P/M`，决定验收分组 | 本文第 10 节 |
| `scenario.variant` | 指定场景组中的具体子变体 | 本文第 10 节 |
| `topology.ub_domains` | `L4` / `UB domain` 级局部池化与路由单元 | `UB` 规范，本 HLD 第 4、9、10 节 |
| `ub_runtime.active_levels` | 当前激活层级，本文默认 `[2,3,4]` | [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md), 本文第 4 节 |
| `ub_runtime.reserved_levels` | 当前保留但不激活的层级编号 | 同上 |
| `pypto.enable_function_labels` | 保留 `pl.Level` / hierarchy label | [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md) |
| `pypto.scope_runtime.enable_pl_free` | 打开 `pl.free` 语义与相关 trace | [multi_level_runtime_ring_and_pypto_free_api.md](../docs/pypto_top_level_design_documents/multi_level_runtime_ring_and_pypto_free_api.md) |
| `pypto.simpler_boundary.*` | 定义 `ChipBackend`/L2 边界适配方式 | [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md) |
| `lingqu_data.*` | 打开最小数据服务落点 | [linqu_data_system.md](../docs/pypto_top_level_design_documents/linqu_data_system.md), 本文第 9、10 节 |
| `levels.l2_* / l3_* / l4_*` | 各层容量、水位、延迟模型 | [rust_llm_server_design_v8_zh.md](rust_llm_server_design_v8_zh.md), 本文第 9、10 节 |
| `routing.mode` | `flat` 或 `recursive` 路由策略 | [rust_llm_server_design_v8_zh.md](rust_llm_server_design_v8_zh.md) |
| `workload.type=rust_llm_server_mvp` | 首个 workload 的显式绑定位 | 本文第 10 节 |
| `faults[]` | host degraded / corruption / route failure 等故障注入 | 本文第 9、10 节 |
| `outputs.emit_*` | 指定 trace / metrics / report 输出面 | 本文第 12 节 |

---

## 12. 输出与可观测性

### 12.1 Metrics

最小指标集合：

- `requests_total`
- `requests_succeeded`
- `requests_failed`
- `route_decisions_total`
- `level_hits_total{level=...}`
- `level_misses_total{level=...}`
- `promotions_total{from,to}`
- `evictions_total{level=...}`
- `quarantined_blocks{level=...}`
- `occupancy_ratio{level,node}`
- `request_latency_us_p50/p95/p99`
- `function_dispatch_total{pl_level=...}`
- `scope_events_total{type=enter|exit}`
- `requests_total{pl_level=...}`
- `pl_free_total`
- `ring_retire_blocked_total`
- `ring_layer_occupancy{depth=...}`

### 12.2 事件 Trace

每个请求输出可选 trace：

- request id
- task coord
- function label
- route path
- scope / ring events
- qemu platform events
- hit/miss chain
- promotion chain
- eviction chain
- fault encountered
- final latency
- dispatch chain

### 12.3 场景报告

每次运行结束后输出：

- 场景摘要
- 核心指标
- 与基线对比
- 关键异常事件
- 建议观察点

---

## 13. 与生产系统的映射关系

仿真系统不是一次性工具，设计上必须为生产实现让路。

这里的“生产实现”不是孤立的 LLM server，而是更大的 Linqu / PyPTO 运行时体系中的 serving specialization。

并且这个更大的体系还包括 Lingqu data system 的外部依赖面。因此本文档需要同时和三组文档对齐：

- hierarchy / function grammar
- runtime / ring / `pl.free`
- data system service layering

同时，仿真平台实现路径需要和“QEMU 作为系统级底座”这一前提对齐，而不是独立发展出另一套与 QEMU 无关的模拟框架。

### 13.1 可直接复用的部分

- 层级枚举与公共类型
- 配置 schema 的主体结构
- routing 参数模型
- metrics 名称和维度
- 事件语义
- `pl.Level` / `TaskCoord` / function label 模型
- scope/ring lifecycle 语义模型
- 四类 Lingqu data service 的最小 API/事件/指标模型
- workload target 挂载模型
- `UBPU` / `Entity` / `UB domain` / `UMMU` / `UB Decoder` 的对象抽象

### 13.2 后续可替换的部分

- simulated `BlockStore` → `GpuBlockStore` / `HostBlockStore` / `CxlBlockStore`
- simulated allocator → 真实 allocator client
- `LatencyModel` → 真实 IO/transport
- `FaultInjector` → 真实故障检测输入
- chip backend boundary object → 真实 `simpler` 适配层
- synthetic function trace → 编译器或 runtime 实际 dispatch trace
- shmem/block/dfs/db service adapters → 真实 Lingqu data services
- QEMU integration layer → 实际 QEMU machine/device integration

### 13.3 明确不会复用的部分

- 事件调度引擎本身
- mock workload 生成器
- 纯逻辑 payload 占位符

---

## 14. 代码组织建议

以下目录树只是**一种可能的 Rust workspace 布局**，用于帮助讨论边界；它不代表这些文件当前已经存在于仓库中。

```text
simulator/
├── Cargo.toml
├── src/
│   ├── entry/
│   ├── config/
│   ├── types/
│   ├── topology/
│   ├── qemu_integration/
│   ├── program_model/
│   ├── ring_lifecycle/
│   ├── backend_adapter/
│   ├── guest_uapi/
│   ├── data_services/
│   │   ├── shmem/
│   │   ├── block/
│   │   ├── dfs/
│   │   └── db/
│   ├── hierarchy/
│   │   ├── nodes/
│   │   ├── store/
│   │   ├── allocator/
│   │   └── integrity/
│   ├── routing/
│   │   ├── recursive/
│   │   └── flat/
│   ├── lifecycle/
│   ├── events/
│   ├── workload/
│   ├── faults/
│   ├── metrics/
│   ├── trace/
│   └── report/
├── scenarios/
│   ├── mvp_2host_single_domain.yaml
│   ├── eviction_pressure.yaml
│   ├── failover.yaml
│   └── corruption.yaml
└── docs/
    └── simulator_hld_v0.md
```

如果后续要与生产代码库合并，可以把 `simulator` 变成 workspace 下的独立 crate。

---

## 15. 里程碑

### Milestone 0

- 配置加载
- 拓扑构建
- L2/L3/L4 树生成
- 静态打印树结构
- 完成 `UBPU` / `Entity` / `UB domain` 最小对象图

### Milestone 1

- block store 仿真
- 递归路由
- 单请求 lookup / fetch / promote
- `UBA` / `Entity` / level tier 映射打通

### Milestone 2

- eviction / demotion
- metrics 和 trace
- CLI 场景运行

### Milestone 3

- 故障注入
- integrity/quarantine
- flat vs recursive 对比

### Milestone 4

- 稳定场景集
- summary report
- 演示脚本和文档

---

## 16. 验收标准

满足以下条件即可认为本文档对应的系统设计成立：

1. 能在单机和 `2-host/single-domain` 场景下稳定运行
2. 能输出可解释的 route / promotion / eviction trace
3. 能通过配置复现至少 4 类关键场景
4. 能比较 flat routing 与 recursive routing 的行为差异
5. 能支持故障注入并观察到正确的绕行或失败语义
6. 能保留 Linqu / PyPTO 的 hierarchy label、task coordinate 和 dispatch 边界语义
7. 能对 `lingqu_shmem` / `lingqu_block` / `lingqu_dfs` / `lingqu_db` 提供最小可运行、可观测、可验证能力
8. 能在 QEMU 平台视图、`UB` 对象视图、Linqu 语义视图和 workload 视图之间建立一致映射
9. 能把 `rust_llm_server_design_v8.md` 的 MVP 作为首个 workload 跑在该平台上
10. 能为后续生产实现提供清晰的 trait、配置、`UB` 对象边界、适配边界和指标边界
11. 能把 guest 侧最小对象链路明确收敛为 `UBRT/ACPI/DTS -> ubfi -> ubus -> ubase -> ubcore/uburma -> ummu/vfio-ub`
12. 能暴露最小 guest UAPI，可覆盖 `/dev/ubcoreX`、ioctl、mmap、netlink、sysfs 中至少一条完整观测/操作链

---

## 17. L5/L6、Gossip 与 Allocator HA 扩展设计

当前正文已经把 `L5/L6`、gossip 和 allocator 主备切换当成保留位提出，但如果它们始终停留在路线图里，前面的 `L0-L7` 编号和控制面位置仍然是不完整的。因此，本节开始把这三个扩展点落成具体设计。

### 17.1 扩展目标

这一层扩展的目标不是把平台做成完整生产集群，而是补齐三个当前缺口：

- 让 `L5/L6` 不再只是“编号保留位”，而成为可观测、可调度、可故障注入的对象层
- 让跨 `L4` 的拓扑、健康、容量和路由摘要能够传播，而不是完全依赖中心化静态配置
- 让 allocator 不再是单点对象，而具备最小主备切换能力

### 17.2 L5/L6 对象语义

在当前文档里，`L2-L4` 已经有较清晰的 `UBPU`、host 和 `UB domain` 映射。向上扩展时，建议采用以下对象语义：

| Level | 建议对象 | 作用 |
|---|---|---|
| L5 | fabric cell | 聚合多个 `L4 UB domain` 的局部管理和资源汇总单元 |
| L6 | federation domain | 聚合多个 `L5 fabric cell` 的跨域编排与容错单元 |
| L7 | global coordinator | 顶层入口、策略下发和全局观察面 |

其中：

- `L5` 更偏向“同一大拓扑内的上层汇聚”
- `L6` 更偏向“跨域联合和故障隔离”

当前设计建议引入以下扩展对象：

- `FabricCellId`
- `FederationId`
- `CellMembership`
- `FederationMembership`
- `CapacitySummary`
- `RouteSummary`
- `HealthSummary`

并把 `TaskCoord` 从：

`(logical_system, l6, l5, l4, l3, l2, scope_depth, task_id)`

继续解释为真实可用的跨域路径坐标，而不是仅保留字段。

### 17.3 L5/L6 拓扑与控制面

在扩展后的控制面里，建议形成如下层次：

1. `L4 domain controller`
- 汇总本域内 host / `UBPU` / `Entity` 的容量、健康、路由可达性

2. `L5 cell controller`
- 聚合多个 `L4` 摘要
- 做 cell 内部的路径选择、容量再分配和局部故障隔离

3. `L6 federation controller`
- 聚合多个 `L5` 摘要
- 做跨 cell 的任务放置、回源路径选择和跨域降级

在 QEMU 平台中，这三个控制器不必一开始都实现为独立进程，但在语义上应区分：

- 本地状态
- 聚合摘要
- 上行传播
- 下行决策

### 17.4 最小 Gossip 聚合协议

为避免 `L5/L6` 完全依赖强中心化轮询，建议引入最小 gossip 聚合协议。该协议不是为生产网络优化的最终协议，而是一个用于仿真和验证的状态传播模型。

最小 gossip 消息建议包括：

- `membership_digest`
  - 节点/域成员、epoch、状态位
- `capacity_digest`
  - free blocks、watermark、导出/导入内存容量
- `health_digest`
  - degraded/failed/quarantined 摘要
- `route_digest`
  - 可达性、首选出口、备用路径数量
- `allocator_digest`
  - allocator leader、lease epoch、last applied index

传播规则建议为：

1. `L4 -> L5`
- 周期性上报本域摘要

2. `L5 -> L5`
- 同层 anti-entropy，同步 cell 级视图

3. `L5 -> L6`
- 上报聚合后的 cell 摘要

4. `L6 -> L5/L4`
- 回传策略性决策，例如限流、降级、重路由或迁移建议

最小一致性要求：

- gossip 最终一致，不要求强一致
- 每条摘要带 `epoch`
- 新 epoch 覆盖旧 epoch
- 明确允许短时间摘要过期，但必须可观测

建议引入以下事件：

- `MembershipDigestSent`
- `MembershipDigestMerged`
- `CapacityDigestUpdated`
- `HealthDigestEscalated`
- `RouteDigestApplied`
- `GossipDigestExpired`

### 17.5 Allocator 主备切换

当前文档中的 allocator 还是单对象视角。向上扩展后，allocator 至少要具备：

- leader / follower 角色区分
- lease-based 主角色授予
- 状态复制
- 故障切换
- 恢复后追赶

建议的最小角色模型：

| 角色 | 职责 |
|---|---|
| allocator leader | 接受分配请求、写入意图、发布结果 |
| allocator follower | 跟随复制、提供热备 |
| allocator observer | 只读观察，不参与提交 |

最小状态机建议为：

1. `Follower`
- 接收 leader 状态更新

2. `Candidate`
- 在 lease 超时后尝试提升

3. `Leader`
- 持有有效 lease，负责分配

4. `Recovering`
- 故障恢复后追赶缺失日志

最小复制数据建议包括：

- `allocation_intent_log`
- `watermark_state`
- `ownership_map`
- `lease_epoch`
- `applied_index`

切换规则建议为：

- 只有持有最新 `lease_epoch` 的节点可成为 leader
- 新 leader 在接管前必须完成最小状态追平
- 旧 leader 恢复后必须先进入 `Recovering`
- 任何双主都必须通过 fencing 事件暴露出来

建议引入以下事件：

- `AllocatorLeaseGranted`
- `AllocatorLeaseExpired`
- `AllocatorPromotionStarted`
- `AllocatorPromotionCompleted`
- `AllocatorFencingTriggered`
- `AllocatorRecoveryStarted`
- `AllocatorRecoveryCompleted`

### 17.6 对现有模块的影响

这三个扩展点会直接影响现有模块边界：

- `Topology Builder`
  - 需要从 `L4` 扩展到 `L6`
- `Event Engine`
  - 需要支持 gossip、lease、切换和恢复事件
- `Metrics and Reporting Module`
  - 需要支持 cell/federation/allocator 级指标
- `QEMU Integration Module`
  - 需要支持更多控制面协同服务，而不只是局部 domain

建议新增的概念模块包括：

- `cell_control`
- `federation_control`
- `gossip`
- `allocator_ha`

这些仍然是职责边界，不预设为具体文件名。

### 17.7 新增指标与验收点

建议新增以下指标：

- `gossip_messages_total{type=...}`
- `gossip_merge_latency_us`
- `cell_membership_changes_total`
- `federation_route_changes_total`
- `allocator_role_changes_total`
- `allocator_lease_epoch`
- `allocator_failover_duration_us`
- `allocator_fencing_total`

建议新增以下验收点：

- `L5/L6` 对象能在拓扑、trace 和配置中显式出现
- gossip 摘要能够跨 `L4` 传播并收敛
- allocator leader 故障后，follower 能在可配置时间内接管
- 主备切换期间 allocation 语义可解释，且不会出现静默双主

### 17.8 后续更远期增强

在这三个扩展点落稳之后，再考虑更后面的增强方向：

- 持久化 index 仿真
- Bloom/delta gossip 成本模型
- 更真实的容量碎片和恢复逻辑
- layer-wise block 与 prefetch 仿真
- compute-IO overlap 模型
- 连续批处理和更真实的请求调度

---

## 18. 结论

这个 High Level Design 选择先构建一个 **基于 QEMU、符合 UB/Linqu spec、以 Rust 承载 `UB` 对象层、`UB/Linqu` 仿真核心、管控面与基础设施仿真以及 workload 挂载层** 的系统仿真平台。目标不是先做一个只服务于 LLM 的专用模拟器，而是先把平台本身搭对，再用 `rust_llm_server_design_v8.md` 的 MVP 作为第一个真实 workload 去验证它。

如果这个仿真平台能跑通，那么后续工作就不再是“为单个业务不断添加仿真逻辑”，而是：

- 在稳定的平台语义上增加新的 workload
- 把当前仿真实现逐步替换成更真实的 QEMU 集成、设备模型和管控面组件
- 让 LLM serving 成为平台验证样例，而不只是平台定义本身
