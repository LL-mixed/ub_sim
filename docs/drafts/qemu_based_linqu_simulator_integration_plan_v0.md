# QEMU-based Linqu Simulator Integration Plan (v0)

> 目标：把 `qemu_based_linqu_simulator_architecture_spec.md` 落成可执行的集成计划，明确 QEMU machine profile、device model、host-side co-sim service、launch 方式、trace/metrics 桥接，以及当前阶段的实现顺序。

---

## 1. 目标与范围

本计划回答的问题是：

1. QEMU 在这个平台里具体扮演什么角色
2. 哪些能力做成 QEMU machine / device
3. 哪些能力做成 host-side service
4. guest、host service、Linqu semantic layer 如何通信
5. v0 应该先集成什么，后集成什么

本计划不定义 workload 细节。workload 挂接由后续 `rust_llm_server_v8_mvp workload spec` 负责。

---

## 2. 集成总图

```text
+------------------------------------------------------+
| Workload Harness                                     |
| - rust_llm_server_v8_mvp                             |
+-------------------------------+----------------------+
                                |
                                v
+------------------------------------------------------+
| Linqu Semantic Runtime                               |
| - hierarchy / TaskKey / pl.Level                     |
| - ring lifecycle / dispatch / trace                  |
+-------------------------------+----------------------+
                                |
             +------------------+------------------+
             |                                     |
             v                                     v
+-------------------------------+   +----------------------------------+
| QEMU Guest Nodes              |   | Host-side Co-sim Services        |
| - host runtime                |   | - shmem manager                  |
| - paravirt clients            |   | - block service                  |
| - device drivers / agents     |   | - dfs service                    |
+-------------------------------+   | - db service                     |
             |                       | - coordinator / registry         |
             v                       +----------------+-----------------+
+------------------------------------------------------+ 
| QEMU Platform                                         |
| - machine profiles                                    |
| - virtual buses / devices                             |
| - memory regions / doorbells / queues                 |
+------------------------------------------------------+
```

---

## 3. QEMU 集成原则

### 3.1 最小侵入

v0 优先使用：

- QEMU 启动参数
- 标准虚拟机配置
- 用户态可扩展设备或较薄的自定义设备
- 宿主侧协同服务

避免一开始做重度 QEMU 内核修改。

### 3.2 设备边界显式化

只有真正需要 guest-visible 边界语义的对象，才优先做 QEMU device：

- chip backend boundary
- shared memory region exposure
- block queue / completion queue
- control doorbell / interrupt endpoint

### 3.3 服务与设备分层

高层分布式服务尽量在宿主侧实现，通过 guest client 接入：

- DFS
- DB
- coordinator / registry / membership

### 3.4 先跑通单 host，再扩展多 host

v0 实施顺序应是：

1. 单 guest，单 host service
2. 单 guest，多 device / 多 service
3. 双 guest，单 switch-domain
4. 多 guest，分层拓扑

---

## 4. Machine Profile 设计

### 4.1 `linqu-host-v0`

v0 的默认 machine profile 定义一个 Linqu Host。

建议组成：

- 1 个 guest OS 实例
- N 个虚拟 CPU
- host DRAM 主内存
- 一个或多个 guest-visible chip backend endpoint
- 一个共享内存 region 映射入口
- 一个 block queue device
- 一个 control/trace doorbell device

### 4.2 Profile 配置字段

建议字段：

```yaml
platform:
  backend: qemu
  machine_profile: linqu-host-v0
  guest_count: 2
  vcpus_per_guest: 8
  memory_mb_per_guest: 16384
  chip_endpoints_per_guest: 4
  shmem_region_mb: 1024
  block_queue_depth: 128
  trace_channel: virtio-serial
```

### 4.3 多 host 拓扑

多个 guest 组成 L4 时：

- 每个 guest 是一个 L3 host
- 宿主侧 launch manager 负责把这些 guest 分组到同一个 switch-domain
- semantic layer 同步维护 L3/L4/L5/L6/L7 编号信息

---

## 5. Device Model 计划

### 5.1 `linqu-chip-backend-dev`

用途：

- 表达 guest host runtime 到 L2 chip backend 的边界

建议能力：

- command queue
- completion queue
- doorbell
- h2d / d2h request descriptor
- dispatch trace tagging

v0 不执行真实 kernel，只返回：

- accepted
- completed
- failed
- latency metadata

### 5.2 `linqu-shmem-dev`

用途：

- 暴露 guest-visible shared region 映射能力

建议能力：

- region register
- region map
- put/get request
- barrier token

实现方式：

- 优先用共享内存 backing + guest 可见控制寄存器

### 5.3 `linqu-block-dev`

用途：

- 暴露 `(UB_ADDRESS, LBA, length, flags)` 异步 block I/O

建议能力：

- submission queue
- completion queue
- read/write descriptors
- completion correlation id

### 5.4 `linqu-control-dev`

用途：

- 用于 platform trace、time sync、control plane event injection

建议能力：

- inject fault
- inject topology change
- query runtime identity
- emit structured trace record

---

## 6. Host-side Co-sim Services

### 6.1 `shmem-manager`

职责：

- 维护 PE registry
- 管理共享 region backing
- 协助 barrier / sync

### 6.2 `block-service`

职责：

- 管理虚拟 UB block devices
- 处理异步 read/write
- 生成 completion

### 6.3 `dfs-service`

职责：

- 提供全局 namespace
- 分离 metadata path 和 data path
- 暴露 POSIX-like API 子集

### 6.4 `db-service`

职责：

- 实现 KV / Hash / pipeline / pubsub 最小能力
- 提供 correlation id 和 batch handling

### 6.5 `linqu-coordinator`

职责：

- 维护 L7 / logical system 视图
- 管理 guest 拓扑分组
- 提供 registry / membership / launch metadata

---

## 7. Guest 内部组件

### 7.1 `linqu-agent`

这是 guest 内的系统代理进程。

职责：

- 和 QEMU device 通信
- 与宿主侧 service 建立会话
- 把 guest runtime 事件桥接到 semantic layer

### 7.2 `host-runtime`

职责：

- 表达 Linqu L3 host runtime
- 管理 `pl.Level` dispatch、TaskKey、scope/ring、service 调用

### 7.3 `paravirt clients`

分别对应：

- shmem client
- block client
- dfs client
- db client

这些 client 不需要一开始做成完整 kernel driver，v0 优先走用户态代理即可。

---

## 8. 通信与协议桥接

### 8.1 guest ↔ device

优先模式：

- MMIO control registers
- shared queue memory
- virtio-serial / socket trace channel

### 8.2 guest ↔ host services

优先模式：

- Unix domain socket
- vsock
- virtio-serial

v0 推荐：

- 控制面走 `virtio-serial` 或 `vsock`
- 数据面大块传输用 shared backing 或 host-side indirection

### 8.3 semantic layer ↔ launch manager

优先模式：

- structured JSON events
- scenario-driven injection
- deterministic seed + timeline replay

---

## 9. Tracing 与 Metrics 桥接

### 9.1 Trace 目标

需要统一看到三层事件：

1. platform events
- VM start/stop
- device queue event
- interrupt / completion

2. Linqu semantic events
- TaskKey creation
- `pl.Level` dispatch
- `pl.free`
- ring retire

3. service events
- shmem put/get/barrier
- block read/write
- dfs open/read/write
- db get/set/publish

### 9.2 Trace 关联键

至少保留：

- `logical_system`
- `guest_id`
- `task_key`
- `request_id`
- `correlation_id`
- `device_id`
- `service_op_id`

### 9.3 Metrics

建议分四组：

- platform metrics
- runtime semantics metrics
- data service metrics
- workload metrics

---

## 10. Launch 方案

### 10.1 `linqu-sim-launch`

建议提供统一 launch 工具：

```bash
linqu-sim-launch \
  --scenario scenarios/llm_mvp_2host.yaml \
  --platform qemu \
  --guests 2
```

职责：

- 启动 host-side services
- 启动 QEMU guests
- 注入 topology 和 logical system 配置
- 挂接 trace sink
- 启动 workload harness

### 10.2 启动顺序

1. 启动 `linqu-coordinator`
2. 启动 `shmem/block/dfs/db` services
3. 启动 QEMU guests
4. 启动 guest agents
5. 注入 topology / logical system
6. 启动 workload harness

---

## 11. 实施顺序

### Phase A

- 单 guest `linqu-host-v0`
- `linqu-control-dev`
- guest `linqu-agent`
- trace 通路打通

### Phase B

- `linqu-chip-backend-dev`
- `linqu-shmem-dev`
- `shmem-manager`
- `simpler` 边界 stub trace

### Phase C

- `linqu-block-dev`
- `block-service`
- async completion 归因

### Phase D

- `dfs-service`
- `db-service`
- guest-side paravirt clients

### Phase E

- 双 guest 启动
- L3/L4 拓扑分组
- logical system / TaskKey / service tracing 联动

### Phase F

- workload harness 挂接
- `rust_llm_server_v8_mvp` 首个 workload 跑通

---

## 12. v0 风险与控制

### 12.1 风险：QEMU 接入过重

控制：

- v0 限制自定义 device 数量
- 优先 host-side services

### 12.2 风险：平台语义和 workload 逻辑耦合

控制：

- 单独维护 workload harness 层
- 平台 API 不引用 LLM 特有概念

### 12.3 风险：trace 不可关联

控制：

- 统一 correlation key
- 平台层和语义层共用 trace schema

### 12.4 风险：`simpler` 边界被误实现

控制：

- 只允许通过 adapter stub 接入
- 不在平台中重做 L0-L2 语义

---

## 13. 交付标准

v0 集成计划成立的标志是：

1. 能用统一 launch 工具启动最小 QEMU Linqu 平台
2. 能看到 platform + runtime + service 三层 trace
3. 至少有 `chip-backend`、`shmem`、`block` 三类边界可运行
4. `dfs` 和 `db` 至少能以 host-side service 形式接入
5. 平台对 workload 暴露稳定挂载入口
