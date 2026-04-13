# QEMU-based Linqu Simulator Architecture Spec (v1)

> 目标：定义一个 **基于 QEMU、符合 UB/Linqu spec 的系统级仿真平台**。这里的 `Linqu` 采用与你给出的说明一致的含义，即 `Linqu === UB === UnifiedBus`。该平台用于承载 UB/PyPTO 运行时语义、Lingqu 数据系统服务，以及上层 workload。`draft/rust_llm_server_design_v8.md` 的 MVP 是该平台上的第一个重点验证 workload。

## Revision

| Revision | Time | Brief Changelog |
|---|---|---|
| v2 | 2026-03-22 CST | 补充 PyPTO device 仿真与 UB 仿真的关系、四层仿真栈，以及面向真实 device-side runtime 的新增组件要求。 |
| v1 | 2026-03-21 CST | 去除文件名之外的版本耦合表达，收紧为基于 QEMU 的 UB/Linqu 规范一致仿真平台，并补入 `UB` 管控面与基础设施仿真边界。 |
| v0 | 2026-03-20 CST | 初始版本。建立 QEMU-based UB/Linqu simulator 的架构、对象映射、QEMU 集成策略和验证矩阵。 |

---

## 1. 范围与定位

这份文档描述的平台不是一个“为 LLM serving 定制的模拟器”，而是一个更通用的 **UB/Linqu-compliant simulator**。

它的三层目标是：

1. 用 QEMU 提供系统级仿真底座
2. 用 UB Base/Firmware/OS/Service Core/UBM 相关规范定义系统对象、控制面、数据面与运行时边界
3. 用具体 workload 验证平台，其中第一个 workload 是 `rust_llm_server_design_v8.md` MVP

这个平台的产出不是“一个能跑 demo 的脚本”，而是：

- 一个可扩展的 UB/Linqu 机器模型
- 一个符合 `UB/Linqu` 规范、可承载 PyPTO hierarchy label 的仿真核心
- 一个可验证 Lingqu 数据系统的仿真环境
- 一个能挂载真实上层系统设计的 workload harness

---

## 2. 对齐的上位规范

本平台必须同时对齐以下文档：

- [linqu_runtime_design.md](../docs/pypto_top_level_design_documents/linqu_runtime_design.md)
- [machine_hierarchy_and_function_hierarchy.md](../docs/pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md)
- [multi_level_runtime_ring_and_pypto_free_api.md](../docs/pypto_top_level_design_documents/multi_level_runtime_ring_and_pypto_free_api.md)
- [linqu_data_system.md](../docs/pypto_top_level_design_documents/linqu_data_system.md)

同时，第一个 workload 必须对齐：

- [rust_llm_server_design_v8.md](rust_llm_server_design_v8.md)

此外，`Linqu` 的系统级底座需要首先对齐以下 `UB` 规范文档，而不能只从本仓库的 runtime 文档反推：

- [UB-Base-Specification-2.0-en.pdf](../ub_docs/UB-Base-Specification-2.0-en.pdf)
- [UB-Firmware-Specification-2.0-en.pdf](../ub_docs/UB-Firmware-Specification-2.0-en.pdf)
- [UB-Software-Reference-Design-for-OS-2.0-en.pdf](../ub_docs/UB-Software-Reference-Design-for-OS-2.0-en.pdf)
- [UB-Service-Core-SW-Arch-RD-2.0-en.pdf](../ub_docs/UB-Service-Core-SW-Arch-RD-2.0-en.pdf)
- [UB-Mgmt-OM-SW-Arch-and-IF-RD-2.0-en.pdf](../ub_docs/UB-Mgmt-OM-SW-Arch-and-IF-RD-2.0-en.pdf)

### 2.1 术语基线

后续章节中出现的 `Linqu`，若无特别声明，均按 `UB` 规范体系理解，而不是一套独立于 `UB` 的新总线协议。

平台建模时应优先使用以下 `UB` 规范对象：

- `UBPU`：支持 UB 协议栈并实现设备特定功能的处理单元
- `Entity` / `EID`：资源分配和事务通信的基本对象及其标识
- `UB domain` / `UB Fabric`：由 `UB` 链路、交换与 `UBPU` 组成的互连域
- `UMMU` / `UB Decoder`：负责地址映射、权限校验和资源空间访问
- `UBM`：管理与控制平面，负责枚举、CNA/EID 管理、路由与运维
- `UB OS Component`：OS 侧的设备管理、内存管理、通信与虚拟化扩展
- `UB Service Core`：集群级服务层，包括 `UBS Engine`、`UBS Mem`、`UBS Comm`、`UBS IO`、`UBS Virt`

因此，本平台中的 PyPTO/Lingqu 语义，不应替代这些 `UB` 对象，而应显式映射到这些对象之上。

### 2.2 当前 Linux 内核实现基线

除 `UB` 规范文档外，本平台还应参考当前内核实现总结：

- [UB-Implementation-Summary.md](../ub_docs/UB-Implementation-Summary.md)

该总结反映的现实非常重要：当前 Linux 内核中的 `UB` 落点，主线并不是直接从 `UBM` 或 `UB Service Core` 起步，而是围绕以下链路展开：

- `ubfi`
  - 负责 `UBRT` / `ACPI` / `DTS` 解析，以及 `UMMU` 等设备创建
- `ubus`
  - 负责 `Entity` / `Port` / `Route` / `Decoder` / hotplug / RAS / sysfs / ioctl
- `ubase`
  - 负责控制队列、事件队列、邮箱、QoS、reset、资源能力等基础硬件抽象
- `ubcore` / `uburma`
  - 负责 `Jetty` / `JFS` / `JFR` / `JFC` / `Segment` / `EID` 等 URMA 通信模型，以及 `/dev/ubcoreX`、ioctl、mmap、netlink 等用户态接口
- `ummu`
  - 负责 token、IOVA、页表、SVA 等内存管理能力
- `obmm` / `cdma` / `sentry` / `vfio-ub`
  - 负责跨机内存、DMA、安全监控和设备直通等能力

因此，当前仿真平台在 guest 侧如果要贴近当前 Linux 实现，优先应该模拟和暴露的是：

- `UBRT` / `ACPI` / `DTS` 风格的启动信息入口
- `ubfi` 能创建出的 `UBC` / `UMMU` / `Entity` / `Port` 可见对象
- `ubus` 风格的实体、端口、路由、资源空间和热插拔模型
- `ubcore` / `uburma` 风格的 `URMA` 资源和字符设备接口
- `vfio-ub` 可利用的 guest-visible 设备边界

而 `UBM`、`UB` 管控面基础设施、PoD 级管理和 `UB Service Core` 的更高层能力，不能被降格为可有可无的 helper；它们应作为平台本体的一部分被仿真，并在实现上可先落在宿主侧控制面服务中。

---

## 3. 设计原则

### 3.1 平台优先

先定义 Linqu 平台，再挂 workload。

LLM serving MVP 是第一个 workload target，不是平台定义来源。

### 3.2 QEMU 作为底座

QEMU 负责：

- 虚拟 machine / board
- CPU / memory / interrupt / bus 基础设施
- 虚拟设备模型
- 多节点系统实例

`UB/Linqu` 仿真核心不另造一个独立系统模拟框架，而是在 QEMU 之上实现。

### 3.3 语义忠实于 UB 与上层 runtime

平台必须同时保留两类约束：

- `UB` 的规范对象和边界：
  - `UBPU` / `Entity` / `EID`
  - `UB domain` / `UB Fabric`
  - `UBM` / `UBIOS` / `UBRT`
  - `UMMU` / `UB Decoder`
  - `URMA` / `URPC`
  - `UB Service Core` 与 `UB OS Component` 的分层位置
- 上层 PyPTO/Lingqu runtime 语义：
  - L0-L7 hierarchy
  - `TaskKey(logical_system, L7..L0, scope_depth, task_id)` 语义
  - `pl.Level` hierarchy label
  - scope-driven ring stack
  - `pl.free` 和 scope token 语义
  - `simpler` 的 L0-L2 边界

### 3.4 分层实现

平台分层必须清晰：

- 平台层：QEMU machine/device/bus
- 规范一致性仿真层：`UB/Linqu` 核心接口、PyPTO grammar、Lingqu services、控制面与基础设施
- workload 层：LLM MVP 或其他上层系统

### 3.5 当前范围不追求 cycle-accurate

当前范围的目标是：

- 结构正确
- 语义可验证
- 行为可观测
- workload 可挂载

不是把所有设备时序都精确复刻。

---

## 4. 平台总架构

```text
+---------------------------------------------------+
| Workload Targets                                  |
| - rust_llm_server v8 MVP                          |
| - Lingqu service tests                            |
| - synthetic PyPTO traces                          |
+--------------------------+------------------------+
                           |
                           v
+---------------------------------------------------+
| UB/Linqu Spec-Conformant Simulation Core         |
| - UB/Linqu core interfaces and device contracts  |
| - hierarchy / TaskKey / pl.Level                 |
| - scope/ring lifecycle                           |
| - function dispatch                              |
| - Lingqu data service simulation                 |
| - control plane and infra simulation             |
| - simpler boundary adapter                       |
+--------------------------+------------------------+
                           |
                           v
+---------------------------------------------------+
| QEMU Integration Layer                            |
| - machine profiles                                |
| - device models                                   |
| - control-plane / infra simulation services       |
| - tracing bridge                                  |
+--------------------------+------------------------+
                           |
                           v
+---------------------------------------------------+
| QEMU Platform                                     |
| - CPU / memory / interrupts / timers / buses      |
| - virtual host/chip/device instances              |
+---------------------------------------------------+
```

### 4.1 四层仿真栈

为了避免把 `QEMU`、`UB`、`PyPTO device` 和 host runtime 混成一个抽象层，建议在平台实现和文档表达里显式采用以下四层栈：

```text
+---------------------------------------------------+
| Layer 4: Host-Side Linqu Runtime / Workload       |
| - host orchestration                              |
| - hierarchy routing / TaskKey / workload harness  |
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
- Layer 4 消费前三层能力，承载 Linqu host runtime、scenario driver 和 workload harness

---

## 5. UB 对象与 Linqu 层级如何映射到 QEMU

### 5.1 映射原则

QEMU 映射不能直接从 `L0-L7` 跳到设备实例，而应分两步：

1. 先映射 `UB` 规范对象：
- `UBPU`
- `Entity/EID`
- `UB domain`
- `UB Fabric`
- `UMMU` / `UB Decoder`
- `UBM` / `UBIOS` / `UBRT`

2. 再把 PyPTO/Lingqu 的 `L0-L7` hierarchy label 叠加到这些对象之上。

也就是说，`L2/L3/L4` 是本平台的运行时视图，不是对 `UB` 规范对象的替代命名。

### 5.2 层级总表

| Linqu Level | 运行时含义 | 优先对应的 UB 对象 | QEMU 映射建议 | 当前范围状态 |
|---|---|---|---|---|
| L0 | Core / Core-group | UBPU 内执行上下文 | guest 内逻辑执行单元，必要时用虚拟核组抽象 | 标签保留，执行委托给 `simpler` 或 stub |
| L1 | Chip die | UBPU 内子拓扑 | guest 内 chip-local 子拓扑或子设备树 | 预留 |
| L2 | Chip-level execution boundary | 单个 UBPU 或一组紧耦合 Entities | QEMU guest-visible UB endpoint、虚拟加速器端点、URMA/doorbell/resource-space 边界 | 激活 |
| L3 | Host-level orchestration | 运行 UB OS Component 的主机 OS 视图 | 一个 QEMU VM / guest OS 实例 | 激活 |
| L4 | Cluster-level-0 | UB domain 内多个 UBPUs/hosts 形成的局部池化单元 | 多 VM + QEMU fabric extension + control-plane simulation | 激活 |
| L5 | Cluster-level-1 | 更大范围的 UB Fabric / supernode 管理域 | 多 L4 组的上层 pooling / routing 域 | 预留 |
| L6 | Cluster-level-2 | 跨域控制或更大规模互连域 | cross-domain / cross-rack management domain | 预留 |
| L7 | Global Coordinator | 顶层管理与编排平面 | UBM northbound / external coordinator / launcher service | 预留 |

### 5.3 当前最小激活集

当前范围激活：

- L2
- L3
- L4

同时保留：

- L0/L1 的标签与边界
- L5/L6/L7 的编号与控制面位置

但在对象层面，当前范围至少要显式保留：

- `UBPU`
- `Entity/EID`
- `UB domain`
- `UB Fabric`
- `UMMU`
- `UB Decoder`

### 5.4 主机与节点模型

建议把一个 **L3 Host** 映射成一个 QEMU guest 实例，但 guest 内部对象应按 `UB` 规范展开，而不是退化成旧式的 `HostNode/ChipNode` 纯抽象。

每个 guest 内部包含：

- `UB OS Component` 视图下的 host runtime
- 一个或多个 `UBPU` 的 guest-visible 端点
- `Entity` resource space / MMIO / doorbell / queue 暴露
- `UMMU` / `UB Decoder` 的最小可观测抽象
- `UBIOS` / `UBRT` 风格的启动信息输入

多个 guest 通过 QEMU 扩展出的 `UB` 设备、fabric 和控制面仿真组成 `UB domain`/`UB Fabric`，而 L4 及以上层级语义是在这些对象上进一步叠加出来的。

---

## 6. QEMU 集成策略

### 6.1 三类接入方式

当前范围建议同时支持三类接入方式：

1. **QEMU machine profile**
- 用于定义承载 `UB OS Component` 的 host 形态和启动信息入口

2. **QEMU device model**
- 用于实现 guest-visible 的 `UB` 设备边界和 resource space

3. **Host-side co-simulation service**
- 用于实现当前暂不适合直接塞进 guest 设备模型的 `UB` 管控面、基础设施能力以及更高层服务

从当前 Linux 实现看，当前范围的关键不是先做一个“大而全”的管理平面，而是先把 guest 可见链路做通：

- firmware/bootstrap entry：`UBRT` / `ACPI` / `DTS`
- kernel-visible object chain：`ubfi -> ubus -> ubase -> ubcore/uburma`
- memory/device path：`ummu` / `vfio-ub`

### 6.2 哪些适合做 QEMU device model

优先建议做成 device model 的对象：

- `ubfi` 可枚举/创建的 `UBC` / `UMMU` / `Entity` 基础对象
- `ubus` 可见的 `Entity` resource space、`Port`、`Route`、`Decoder` 边界
- `UBPU` guest-visible endpoint
- `Entity` resource space
- `UMMU` / `UB Decoder` 的最小配置与观测边界
- `ubcore` / `URMA` / queue / doorbell / completion 端点
- shared-memory region exposure
- block service gateway

### 6.3 哪些适合先落在宿主侧控制面与基础设施服务

优先建议先落在宿主侧控制面与基础设施服务的对象：

- `UBM` control-plane 能力
- `UB` 管控面基础设施，例如枚举、CNA/EID 分配、路由下发、遥测和故障管理
- `UB Service Core` 组件外壳
- `UBS Engine` 风格的资源池化与调度逻辑
- `UBS Mem` / `UBS Comm` / `UBS IO` / `UBS Virt` 的高层服务抽象
- 尚未直接落到内核主链的管理/O&M 逻辑
- `lingqu_dfs`
- `lingqu_db`
- L7 coordinator
- gossip / membership / registry

这些能力是平台本体的一部分，只是在当前实现路线里，部分内容更适合先落在宿主侧控制面服务，而不是都塞进 guest 内核级设备模型。

### 6.4 当前范围的 QEMU 接入深度

当前范围不要求：

- 修改 QEMU 主线复杂调度逻辑
- 自定义完整 CPU ISA 扩展
- 实现复杂 DMA 引擎模型

当前范围需要：

- 能启动多 VM 拓扑
- 能向 guest 注入 `UBIOS` / `UBRT` 风格的最小启动信息
- 能注册若干 `UB` 风格的虚拟设备或 guest-visible endpoint
- 能让 guest 内核侧对象链至少可抽象为 `ubfi -> ubus -> ubase -> ubcore/uburma`
- 能把 `UB/Linqu` 仿真核心事件与 guest 侧 trace 对接

### 6.5 仿真对象到 Linux 内核对象的优先映射

为避免 simulator 内部对象继续偏离当前 Linux 实现，当前范围建议优先按下表组织：

| Simulator / QEMU 对象 | Linux 内核优先对应对象 | 说明 |
|---|---|---|
| guest-visible UB controller device | `struct ub_bus_controller` | 对应 `UBC`、控制器编号、解码器、资源列表等 |
| guest-visible UB entity device | `struct ub_entity` | 对应 `GUID`、`EID`、`CNA`、resource space、driver 绑定点 |
| guest-visible port/link object | `struct ub_port` | 对应端口类型、远端实体、链路状态、domain boundary |
| route / decoder state | `ub_route_*` + `ub_decoder` 相关对象 | 对应 `Route` / `Decoder` / resource space 配置边界 |
| URMA endpoint / queue / completion | `ubcore_jetty` / `ubcore_jfs` / `ubcore_jfr` / `ubcore_jfc` | 对应 Jetty、发送/接收/完成队列 |
| URMA memory object | `struct ubcore_seg` | 对应 Segment、`ubva`、`token_id` |
| shared memory export/import object | `struct obmm_region` / `obmm_export_region` / `obmm_import_region` | 对应跨机共享内存与 `UBA` 导出/导入 |
| guest userspace device surface | `/dev/ubcoreX` + ioctl/mmap/netlink/sysfs | 对应最小用户态可见接口 |
| pass-through capable device boundary | `vfio-ub` | 对应用户态直接访问和设备直通边界 |

对 Lingqu data services，当前范围建议优先挂接如下：

- `lingqu_shmem`
  - 优先映射到 `OBMM` 路径，即 `obmm_region` / `obmm_shm_dev` / export-import 模型
- `lingqu_block`
  - 优先映射到 `ubcore_seg` + `ubcore_jfs_wr` + `JFC completion` 这条 `URMA`/Segment 路径
- `lingqu_dfs`
  - 优先保持为平台控制面/基础设施侧服务，再通过 guest-side client 或 gateway 接入
- `lingqu_db`
  - 优先保持为平台控制面/基础设施侧服务，必要时通过 RPC gateway 或 paravirt channel 接入

---

## 7. UB/Linqu 规范一致性仿真核心

### 7.1 Task Identity

平台必须支持完整任务坐标：

`TaskKey = (logical_system, L7, L6, L5, L4, L3, L2, L1, L0, scope_depth, task_id)`

当前范围中允许多数高层字段为 0，但字段必须存在。

### 7.2 `pl.Level` 与 function label

平台必须支持：

- `@pl.function(level=...)`
- `pl.at(level=...)`
- cluster scope / incore scope / orchestration scope

即使当前范围不编译真实 PyPTO 程序，也必须能在 trace、scenario 和 dispatch 里表达这些标签。

### 7.3 scope / ring lifecycle

平台必须按 `multi_level_runtime_ring_and_pypto_free_api.md` 建模：

- `task_ring[d]`
- `buffer_ring[d]`
- `last_task_alive[d]`
- scope token
- `pl.free`
- layer-local retire

这不是“额外调试信息”，而是平台语义的一部分。

### 7.4 `simpler` 边界

根据 Linqu runtime spec：

- `simpler` 负责 L0-L2 既有能力
- 本平台不修改 `simpler`
- 本平台通过 `ChipBackend` adapter 在 L2 边界接入

因此在仿真平台中，L2 不应被误建模为“纯内部对象”，而应被当成：

- 一个可调用 backend 边界
- 一个具有 h2d/d2h 边界语义的执行域

### 7.5 是否需要 PyPTO device 仿真

为了达成 `rust_llm_server_design_v8.md` 当前定义的 MVP，**不需要**把 `pypto` 的 device 侧执行模型完整仿真出来。

原因是当前 MVP 的验证目标集中在：

- `BlockStore` / `LevelNode` / `LevelAllocator` 的分层抽象是否闭环
- L2/L3/L4 层级树、递归路由、promotion、cascading eviction 是否成立
- 容量、延迟、故障、完整性和指标语义是否可观测
- host runtime 与 chip backend 之间的 dispatch / fetch / completion 边界是否清晰

这些目标要求平台提供的是：

- `L2` 作为一个可调用的 chip backend boundary object
- guest-visible `UBPU` / `Entity` endpoint
- `h2d` / `d2h` / completion / fault injection 的可观测边界事件
- 可参数化的 latency / bandwidth / capacity / failure model

而不是：

- 重新实现 `pypto`/`simpler` 的 device-side task ring
- 重新实现 AIC/AIV kernel 执行、scope-exit、layer-local retire
- 在 simulator 内重放完整的 device instruction / kernel runtime 语义

因此，当前平台对 `pypto device` 的正确要求是 **边界仿真**，不是 **设备内部执行仿真**。

只有在目标升级为以下任一项时，才应把 `pypto` 的 device 仿真提升为必需能力：

- 需要验证 `pl.at(level=L0/L1/L2)` 在 device 侧的真实运行时语义
- 需要验证 scope-driven ring stack 在 chip 内部的行为正确性
- 需要验证 `pl.free`、scope token、layer-local retire 与 device 执行的耦合
- 需要评估 kernel/device runtime 级别的调度、背压和并发行为

### 7.6 升级到真实 device-side runtime 验证时的新增组件

一旦目标从“L2 边界仿真”升级为“真实 `pl.at(level=L0/L1/L2)` 语义验证”或“直接运行 `simpler` 内回归测试”，除了 `pypto device` 仿真本身，还需要同时补齐以下组件：

- `ChipBackend` 真实适配层
  - 不能再停留在 stub dispatch
  - 必须把 host/L3 runtime 的 `dispatch`、`completion`、错误传播和 task identity 映射真正接到 L2
- Tier-2 memory boundary 与 DMA 模型
  - 必须显式建模 host DRAM 与 device GM 的分离
  - 必须支持 `h2d_copy` / `d2h_copy`、buffer handle 映射、完成队列、超时和故障注入
- L0-L2 runtime state machine
  - 必须覆盖 `task_ring[L][d]`、`buffer_ring[L][d]`、`last_task_alive`
  - 必须覆盖 `scope.enter` / `scope.exit`、`pl.free`、`task_freed`
  - 必须覆盖 `fanout_count` / `ref_count` / layer-local retire
- L0/L1/L2 拓扑与调度模型
  - 必须区分 `AIC` / `AIV` / `core-group`
  - 若目标包含多 die，还必须显式建模 `L1`
  - 必须保留 `InCoreFunctionGroup`、TPUSH/TPOP 或其等价调度约束
- 编译产物与 hierarchy label 的消费链路
  - runtime 必须真正消费 `pl.at(level=...)` 对应的 hierarchy label
  - 必须识别 outlined function metadata、function group 信息和 level-specific dispatch 输入
- profiling / trace / replay 能力
  - 必须能观测 ring occupancy、block count、retire ordering、DMA latency、dispatch/completion ordering
  - 否则无法把“设计不一致”与“参数配置不同”区分开

因此，升级后的能力包不应被理解为“只多做一个 device simulator”，而应被理解为：

- `UB` 设备与内存边界仿真更细化
- `PyPTO` / `simpler` device runtime 语义被真正接入
- compiler label → runtime dispatch → device execution → trace/profiling 形成闭环

### 7.7 PyPTO device 仿真与 UB 仿真的关系

两者不是替代关系，而是明确的上下分层关系：

- `UB` 仿真负责机器/设备/互连对象模型
- `PyPTO device` 仿真负责设备之上的运行时执行语义模型

更具体地说：

- `UB` 仿真回答“guest 看到的这台 chip / UBPU / Entity / UMMU / queue / doorbell / completion / DMA / memory region 长什么样”
- `PyPTO device` 仿真回答“建立在这些设备对象之上的 `simpler`/L0-L2 runtime 如何执行 `pl.at(level=L0/L1/L2)`、scope/ring、retire 和 function-group 调度”

因此架构上应采用：

1. QEMU machine / board / bus 提供系统底座
2. `UB` 仿真提供 guest-visible `UBPU` / `Entity` / `UMMU` / resource-space / DMA / completion 边界
3. `PyPTO device` 仿真消费这些边界，实现 `task_ring` / `buffer_ring` / `scope token` / `pl.free` / `dispatch`
4. host-side Linqu runtime 再通过 `ChipBackend` 与 device runtime 对接

这意味着：

- 没有 `UB` 仿真时，`PyPTO device` 仿真缺少可靠的 guest-visible 设备落点
- 没有 `PyPTO device` 仿真时，`UB` 仿真只能证明设备对象存在，不能证明 `pl.at(level=L0/L1/L2)` 的真实运行时语义

所以在范围表达上，应明确写成：

- `UB sim` 是下层平台对象与设备边界
- `PyPTO device sim` 是上层 runtime 语义
- 二者共同构成 “可验证 `simpler`/chip runtime 语义” 的完整 L2 仿真栈

---

## 8. Lingqu Data System 在平台中的落点

### 8.1 `lingqu_shmem`

建议落点：

- QEMU device-backed shared region
- 或 host-side region manager + guest mapping
- 若对齐当前 Linux 实现，优先贴近 `OBMM` export/import + `obmm_shm_dev`

最小可验证语义：

- PE discovery
- region create/map
- put/get
- barrier/sync

### 8.2 `lingqu_block`

建议落点：

- QEMU virtual block gateway device
- 宿主侧 block service queue
- 若对齐当前 Linux 实现，优先贴近 `ubcore_seg` + `ubcore_jfs_wr` + `JFC completion`

最小可验证语义：

- `(UBA, LBA, length, flags)`
- async read/write
- completion queue
- read 作为 producer、write 作为 consumer 的 runtime 归因

### 8.3 `lingqu_dfs`

建议落点：

- host-side DFS co-sim service
- guest 内通过 RPC gateway 或 paravirt client 接入

最小可验证语义：

- global namespace
- open/read/write/pread/pwrite/stat
- metadata path 与 data path 分离

### 8.4 `lingqu_db`

建议落点：

- host-side DB co-sim service
- guest-side thin client / gateway

最小可验证语义：

- `GET/SET/MGET/MSET/DEL`
- `HGET/HSET`
- pipelining
- `PUBLISH/SUBSCRIBE`

---

## 9. UB 管控面与基础设施仿真

`UB/Linqu` 平台不是只有设备面和数据面。符合 spec 的仿真系统还必须覆盖最小可用的管控面与基础设施仿真，否则 guest 只能看到零散设备，而看不到真正的 `UB` 系统。

### 9.1 范围

当前范围至少应仿真以下管控面能力：

- 拓扑发现与枚举
- `CNA` / `EID` 分配与回收
- 路由生成、下发与更新
- 资源编排与分配摘要
- 健康、告警、遥测和故障事件
- 最小 northbound 管控接口

### 9.2 与 guest 设备面的关系

这部分能力不应被理解成“可选的 host-side helper”，而应被理解成：

- 一部分通过 guest-visible `UB` 对象体现
- 一部分通过管理消息和配置空间体现
- 一部分通过宿主侧控制面服务体现

也就是说，当前实现可以把部分控制面逻辑先落在宿主侧，但设计语义上它们属于 `UB/Linqu` 平台本体。

### 9.3 最小控制面对象

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

### 9.4 最小 northbound 面

当前范围不要求完整实现 `RESTCONF/NETCONF/SNMP/Telemetry` 全接口，但应至少保留：

- 查询拓扑
- 查询地址映射
- 查询路由状态
- 查询健康状态
- 查询资源摘要
- 订阅关键事件

### 9.5 与 workload 的边界

Rust LLM MVP 只消费这些平台能力，而不定义这些能力。也就是说：

- workload 可以读取拓扑、容量、健康和数据服务状态
- workload 可以触发标准平台请求
- workload 不应决定 `UB` 管控面的对象模型

---

## 10. 第一个 Workload: rust_llm_server v8 MVP

### 9.1 平台与 workload 的关系

`rust_llm_server_design_v8.md` 的 MVP 是：

- 平台的第一个重点验证对象
- 不是平台本身

工作方式应是：

1. 平台先提供 `UB` 对象层、`UB/Linqu` 仿真核心、管控面与基础设施仿真、data services 和 QEMU nodes
2. workload harness 在其上实例化 LLM-serving-specific 组件
3. 通过场景驱动验证 v8 设计是否能成立

### 9.2 需要挂接的 v8 能力

第一个 workload 至少需要使用：

- recursive hierarchy tree
- `BlockStore` / `LevelNode`
- recursive routing
- promotion / cascading eviction
- integrity / quarantine
- basic metrics / failure handling

### 9.3 在平台中的映射

建议映射如下：

- v8 `ChipNode` → L2 `UBPU` / `Entity` execution boundary
- v8 `HostNode` → L3 guest-side `UB OS Component` host view
- v8 `SwitchDomainNode` → L4 `UB domain` local pooling/fabric view
- v8 remote / future levels → 保留映射位

---

## 11. 模块划分建议

```text
linqu-sim/
├── qemu/
│   ├── machine_profiles/
│   ├── device_models/
│   └── launch/
├── host_services/
│   ├── shmem/
│   ├── block/
│   ├── dfs/
│   └── db/
├── semantics/
│   ├── hierarchy/
│   ├── task_model/
│   ├── ring_lifecycle/
│   ├── dispatch/
│   └── tracing/
├── adapters/
│   ├── simpler/
│   └── qemu_bridge/
├── workloads/
│   ├── rust_llm_server_v8_mvp/
│   ├── lingqu_service_tests/
│   └── pypto_trace_replay/
└── scenarios/
```

### 11.1 模块关系

上面的目录只表达“放在哪里”，不表达“谁依赖谁”。当前平台建议固定以下关系：

```text
workloads
   |
   v
semantics
   | \
   |  \----> adapters
   |          |
   v          v
host_services  qemu
       \       /
        \     /
         v   v
        platform objects / device endpoints
```

更具体地说：

- `qemu/` 提供 machine、device、bus、launch 和 guest-visible endpoint
- `host_services/` 提供 `shmem` / `block` / `dfs` / `db` 的宿主侧服务能力
- `semantics/` 持有 hierarchy、task、ring、dispatch、trace 的主语义
- `adapters/` 负责把 `semantics/` 对接到 `simpler` 边界和 QEMU/device 边界
- `workloads/` 只消费前述能力，不反向定义平台对象
- `scenarios/` 只提供配置与驱动，不持有运行时主状态

因此依赖方向应限制为：

- `workloads -> semantics/adapters/host_services`
- `semantics -> qemu/host_services` 通过稳定接口消费平台能力
- `adapters -> qemu` 或 `adapters -> simpler boundary`
- `qemu`、`host_services` 不能依赖 `workloads`

---

## 12. 验证矩阵

### 11.1 平台自举

- 能启动 1-host 和 2-host QEMU 拓扑
- 能建立 L0-L7 编号空间
- 能把 `pl.Level`、TaskKey、`UBPU`/`Entity`/`UB domain` 拓扑节点映射到 platform trace
- 能说明 guest 侧最小对象链如何对应 `UBRT/ACPI/DTS -> ubfi -> ubus -> ubase -> ubcore/uburma -> ummu/vfio-ub`
- 能说明最小 guest UAPI 如何对应 `/dev/ubcoreX`、ioctl、mmap、sysfs、netlink

### 11.2 Runtime 语义

- `pl.free` 幂等
- layer-local retire 正确
- full TaskKey 可传播
- `simpler` 边界 trace 完整

### 11.3 Lingqu 数据服务

- `lingqu_shmem`：put/get/barrier
- `lingqu_block`：async read/write + completion
- `lingqu_dfs`：namespace + pread/pwrite
- `lingqu_db`：KV + pipeline + pub/sub

### 11.4 LLM workload

- 能挂载 `rust_llm_server_design_v8.md` MVP
- 能完成树构建、递归路由、promotion/eviction、故障绕行的验证

---

## 13. 非目标

- 不做完整 QEMU 深度设备时序建模
- 不做真实 GPU/CXL/RDMA 内核执行
- 不做完整 PyPTO compiler integration
- 不做完整多租户安全隔离实现
- 不做 TB-scale durability、layer-wise prefetch 等 v8 后续扩展

---

## 14. 结论

这个平台的正确定义是：

**一个基于 QEMU、符合 UB/Linqu spec、能承载 Lingqu 数据系统和上层 workload 的系统级仿真平台。**

`rust_llm_server_design_v8.md` MVP 不是平台本身，而是平台上的第一个强验证目标。只有先把平台边界、Linqu 语义和 QEMU 落点定义清楚，后续的 LLM serving、PyPTO runtime 和更广泛的数据服务验证才不会不断返工。
