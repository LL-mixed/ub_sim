# Simulator Rust Interface Sketch (v0)

> 目标：把 [qemu_based_linqu_simulator_architecture_spec_v2.md](qemu_based_linqu_simulator_architecture_spec_v2.md) 和 [rust_llm_server_mvp_simulator_hld_v2.md](rust_llm_server_mvp_simulator_hld_v2.md) 中已经出现的对象、配置和边界，收敛成一版**可开始实现的 Rust 接口草案**。
>
> 定位：这不是最终 API 定稿，也不是完整 crate 设计；它是一份“可以开始写代码”的最小接口包，用来减少后续在命名、边界和事件模型上的返工。

## Revision

| Revision | Time | Brief Changelog |
|---|---|---|
| v0 | 2026-03-23 CST | 初始版本。定义 workspace 切分建议、核心类型、配置 schema、事件模型、trait 草案、guest UAPI 草案与首批实现顺序。 |

---

## 1. 设计目标

这份接口草案优先解决五个问题：

1. Rust workspace 应该先怎么切
2. 哪些 type 必须先冻结
3. 哪些 trait 应该作为第一批实现边界
4. 配置 YAML 应如何映射到 `serde` struct
5. 事件、completion 和 guest UAPI 应如何共用同一套 envelope

当前版本明确不追求：

- 一次性覆盖完整 `ubcore` / `uburma` 兼容层
- 一次性覆盖真实 `simpler` device runtime
- 一次性覆盖所有 `L0-L7` 运行时细节

当前版本只覆盖真正开始实现 P0/P1 所需的最小接口。

---

## 2. Workspace 切分建议

建议第一阶段直接使用多 crate workspace，而不是单 crate 大包。

```text
simulator/
├── Cargo.toml
├── crates/
│   ├── sim-core/
│   ├── sim-config/
│   ├── sim-topology/
│   ├── sim-runtime/
│   ├── sim-services/
│   ├── sim-qemu/
│   ├── sim-uapi/
│   ├── sim-workloads/
│   └── sim-cli/
└── scenarios/
```

### 2.1 crate 职责

- `sim-core`
  - 基础 ID、时间、错误、事件 envelope、共享 enum
- `sim-config`
  - 全部 `serde` 配置类型和配置校验
- `sim-topology`
  - `UBPU` / `Entity` / `UB domain` / hierarchy tree / route input snapshot
- `sim-runtime`
  - routing、block lifecycle、ring lifecycle、task/runtime semantics、backend adapter trait
- `sim-services`
  - `lingqu_shmem` / `lingqu_block` / `lingqu_dfs` / `lingqu_db`
- `sim-qemu`
  - machine profile、device endpoint、bootstrap、QEMU bridge
- `sim-uapi`
  - guest-visible endpoint object 和 ioctl-like surface
- `sim-workloads`
  - `rust_llm_server` MVP harness 和 synthetic trace replay
- `sim-cli`
  - scenario loader、runner、report 导出

### 2.2 依赖方向

建议固定以下依赖方向：

```text
sim-cli -----------> sim-config
sim-cli -----------> sim-workloads
sim-workloads -----> sim-runtime
sim-workloads -----> sim-services
sim-workloads -----> sim-topology
sim-runtime -------> sim-core
sim-runtime -------> sim-topology
sim-runtime -------> sim-services (trait only or stable interface)
sim-services ------> sim-core
sim-services ------> sim-topology
sim-qemu ----------> sim-core
sim-qemu ----------> sim-topology
sim-uapi ----------> sim-core
sim-uapi ----------> sim-topology
sim-uapi ----------> sim-runtime
sim-config --------> sim-core
```

约束：

- `sim-core` 不依赖其他业务 crate
- `sim-qemu` 不拥有 routing / block lifecycle 规则
- `sim-workloads` 不定义平台基础对象

---

## 3. 首批冻结的核心类型

### 3.1 基础 ID 与坐标

```rust
pub type NodeId = u64;
pub type HostId = u32;
pub type UbpuId = u32;
pub type EntityId = u32;
pub type Eid = u32;
pub type DomainId = u32;
pub type RequestId = u64;
pub type TaskId = u64;
pub type OpId = u64;
pub type SegmentId = u64;
pub type CqId = u32;
pub type SimTimestamp = u64;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct LogicalSystemId(pub u32);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum PlLevel {
    L0,
    L1,
    L2,
    L3,
    L4,
    L5,
    L6,
    L7,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct HierarchyCoord {
    pub levels: [u32; 8], // L7..L0
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct TaskKey {
    pub logical_system: LogicalSystemId,
    pub coord: HierarchyCoord,
    pub scope_depth: u32,
    pub task_id: TaskId,
}
```

### 3.2 block / 路由 / 放置

```rust
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct BlockHash(pub String);

#[derive(Debug, Clone)]
pub struct BlockPlacement {
    pub block: BlockHash,
    pub level: PlLevel,
    pub node: NodeId,
}

#[derive(Debug, Clone)]
pub enum RouteReason {
    LocalHit,
    CapacityPreferred,
    HealthPreferred,
    RecursiveFallback,
    FlatFallback,
    ExplicitReroute,
}

#[derive(Debug, Clone)]
pub struct RouteDecision {
    pub from_level: PlLevel,
    pub to_level: PlLevel,
    pub selected_node: NodeId,
    pub reason: RouteReason,
}
```

### 3.3 function / runtime label

```rust
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct FunctionLabel {
    pub name: String,
    pub level: PlLevel,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HealthStatus {
    Healthy,
    Degraded,
    Failed,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IntegrityState {
    Valid,
    Suspect,
    Corrupted,
    Quarantined,
}
```

---

## 4. 请求、句柄与完成事件

这部分必须统一，否则 `ChipBackend`、`lingqu_block`、guest UAPI 和 trace/replay 会各自定义一套对象。

### 4.1 句柄

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct DispatchHandle(pub OpId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct TransferHandle(pub OpId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct ServiceOpHandle(pub OpId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct SegmentHandle(pub SegmentId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct CqHandle(pub CqId);
```

### 4.2 请求

```rust
#[derive(Debug, Clone)]
pub struct DispatchRequest {
    pub task: TaskKey,
    pub function: FunctionLabel,
    pub target_level: PlLevel,
    pub target_node: NodeId,
    pub input_segments: Vec<SegmentHandle>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CopyDirection {
    HostToDevice,
    DeviceToHost,
}

#[derive(Debug, Clone)]
pub struct MemoryEndpoint {
    pub node: NodeId,
    pub segment: SegmentHandle,
    pub offset: u64,
}

#[derive(Debug, Clone)]
pub struct CopyRequest {
    pub task: TaskKey,
    pub direction: CopyDirection,
    pub bytes: u64,
    pub src: MemoryEndpoint,
    pub dst: MemoryEndpoint,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IoOpcode {
    ReadBlock,
    WriteBlock,
    Dispatch,
}

#[derive(Debug, Clone)]
pub struct IoSubmitReq {
    pub op_id: OpId,
    pub task: Option<TaskKey>,
    pub entity: EntityId,
    pub opcode: IoOpcode,
    pub segment: Option<SegmentHandle>,
    pub block: Option<BlockHash>,
}
```

### 4.3 completion

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompletionSource {
    ChipBackend,
    BlockService,
    ShmemService,
    GuestUapi,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CompletionStatus {
    Success,
    RetryableFailure { code: String },
    FatalFailure { code: String },
}

#[derive(Debug, Clone)]
pub struct CompletionEvent {
    pub op_id: OpId,
    pub task: Option<TaskKey>,
    pub source: CompletionSource,
    pub status: CompletionStatus,
    pub finished_at: SimTimestamp,
}
```

---

## 5. 拓扑与快照对象

### 5.1 最小拓扑对象

```rust
#[derive(Debug, Clone)]
pub struct SimHost {
    pub id: HostId,
    pub node_id: NodeId,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimUbpu {
    pub id: UbpuId,
    pub node_id: NodeId,
    pub host_id: HostId,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimEntity {
    pub id: EntityId,
    pub eid: Eid,
    pub ubpu_id: UbpuId,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimDomain {
    pub id: DomainId,
    pub node_id: NodeId,
    pub hosts: Vec<HostId>,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimTopology {
    pub hosts: Vec<SimHost>,
    pub ubpus: Vec<SimUbpu>,
    pub entities: Vec<SimEntity>,
    pub domains: Vec<SimDomain>,
}
```

### 5.2 查询快照

```rust
#[derive(Debug, Clone)]
pub struct TopologySnapshot {
    pub hosts: usize,
    pub ubpus: usize,
    pub entities: usize,
    pub domains: usize,
}
```

---

## 6. 配置 schema 草案

建议所有场景文件都进入同一个 `ScenarioConfig`。

### 6.1 顶层配置

```rust
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ScenarioConfig {
    pub scenario: ScenarioMetaConfig,
    pub platform: PlatformConfig,
    pub topology: TopologyConfig,
    pub ub_runtime: UbRuntimeConfig,
    pub pypto: PyptoConfig,
    pub lingqu_data: LingquDataConfig,
    pub levels: LevelsConfig,
    pub routing: RoutingConfig,
    pub workload: WorkloadConfig,
    pub faults: Vec<FaultConfig>,
    pub outputs: OutputConfig,
}
```

### 6.2 元信息与平台配置

```rust
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ScenarioMetaConfig {
    pub name: String,
    pub group: Option<String>,
    pub variant: Option<String>,
    pub seed: u64,
    pub duration_us: u64,
    pub logical_system: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub enum PlatformBackend {
    Qemu,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub enum DeviceModelMode {
    Stub,
    Mixed,
    GuestVisible,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct PlatformConfig {
    pub backend: PlatformBackend,
    pub machine_profile: String,
    pub cpu_model: String,
    pub memory_model: String,
    pub device_model_mode: DeviceModelMode,
}
```

### 6.3 topology / runtime / workload

```rust
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct UbDomainConfig {
    pub id: String,
    pub hosts: Vec<u32>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct CollapseConfig {
    pub fabric: bool,
    pub global: bool,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct TopologyConfig {
    pub hosts: u32,
    pub ubpus_per_host: u32,
    pub entities_per_ubpu: u32,
    pub ub_domains: Vec<UbDomainConfig>,
    pub collapse: CollapseConfig,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct UbRuntimeConfig {
    pub active_levels: Vec<u8>,
    pub reserved_levels: Vec<u8>,
    pub preserve_full_task_coord: bool,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub enum RuntimeDefaultLevel {
    CHIP,
    HOST,
    CLUSTER_0,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct SimplerBoundaryConfig {
    pub enabled: bool,
    pub chip_backend_mode: String,
    pub dispatch_latency_us: Option<u64>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ScopeRuntimeConfig {
    pub enable_multi_layer_ring: bool,
    pub enable_pl_free: bool,
    pub max_scope_depth: u32,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct PyptoConfig {
    pub enable_function_labels: bool,
    pub default_level: RuntimeDefaultLevel,
    pub allow_levels: Vec<RuntimeDefaultLevel>,
    pub simpler_boundary: SimplerBoundaryConfig,
    pub scope_runtime: ScopeRuntimeConfig,
}
```

### 6.4 levels / routing / outputs

```rust
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct TierConfig {
    pub capacity_blocks: u64,
    pub high_watermark: f64,
    pub low_watermark: f64,
    pub hit_latency_us: Option<u64>,
    pub fetch_latency_us: Option<u64>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct LevelsConfig {
    pub l2_ubpu_tier: TierConfig,
    pub l3_host_tier: TierConfig,
    pub l4_domain_tier: TierConfig,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub enum RoutingMode {
    Recursive,
    Flat,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct RoutingConfig {
    pub mode: RoutingMode,
    pub hit_weight: f64,
    pub load_weight: f64,
    pub capacity_weight: f64,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct OutputConfig {
    pub trace: bool,
    pub metrics_csv: bool,
    pub summary_json: bool,
    pub emit_task_coord_trace: bool,
    pub emit_data_service_trace: bool,
    pub emit_qemu_platform_trace: bool,
}
```

### 6.5 workload / fault

```rust
#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct HotsetLoopWorkloadConfig {
    pub qps: u64,
    pub unique_prefixes: u64,
    pub blocks_per_request: u32,
    pub function_label_mode: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct TraceReplayWorkloadConfig {
    pub trace_path: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct RustLlmMvpWorkloadConfig {
    pub profile: String,
    pub qps: u64,
    pub unique_prefixes: u64,
    pub blocks_per_request: u32,
    pub function_label_mode: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(tag = "type")]
pub enum WorkloadConfig {
    #[serde(rename = "hotset_loop")]
    HotsetLoop(HotsetLoopWorkloadConfig),
    #[serde(rename = "trace_replay")]
    TraceReplay(TraceReplayWorkloadConfig),
    #[serde(rename = "rust_llm_server_mvp")]
    RustLlmMvp(RustLlmMvpWorkloadConfig),
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(tag = "type")]
pub enum FaultConfig {
    #[serde(rename = "host_degraded")]
    HostDegraded { at_us: u64, host_id: u32 },
    #[serde(rename = "block_corruption")]
    BlockCorruption { at_us: u64, level: String, node_id: String, block_hash: String },
}
```

---

## 7. 核心 trait 草案

### 7.1 backend / service / route

```rust
pub trait ChipBackend {
    fn dispatch(&self, req: DispatchRequest) -> Result<DispatchHandle, BackendError>;
    fn h2d_copy(&self, req: CopyRequest) -> Result<TransferHandle, BackendError>;
    fn d2h_copy(&self, req: CopyRequest) -> Result<TransferHandle, BackendError>;
    fn poll_completion(&self, now: SimTimestamp) -> Vec<CompletionEvent>;
}

pub trait BlockService {
    fn read(&self, req: BlockReadReq) -> Result<ServiceOpHandle, ServiceError>;
    fn write(&self, req: BlockWriteReq) -> Result<ServiceOpHandle, ServiceError>;
    fn poll_completion(&self, now: SimTimestamp) -> Vec<CompletionEvent>;
}

pub trait RoutePlanner {
    fn plan(&self, req: RouteRequest, topo: &SimTopology) -> Result<RouteDecision, RouteError>;
}
```

### 7.2 block store / runtime

```rust
pub trait SimBlockStore {
    fn lookup(&self, block: &BlockHash) -> LookupResult;
    fn stage_insert(&mut self, plan: PromotionPlan) -> Result<(), StoreError>;
    fn evict(&mut self, plan: EvictionPlan) -> Result<Vec<BlockHash>, StoreError>;
}

pub trait RingRuntime {
    fn on_scope_enter(&mut self, task: &TaskKey);
    fn on_scope_exit(&mut self, task: &TaskKey);
    fn on_pl_free(&mut self, task: &TaskKey, block: &BlockHash);
}

pub trait EventSink {
    fn emit(&mut self, event: SimEvent);
}
```

### 7.3 guest UAPI

```rust
pub trait GuestUapiSurface {
    fn query_topology(&self) -> TopologySnapshot;
    fn create_segment(&self, req: SegmentCreateReq) -> Result<SegmentHandle, UapiError>;
    fn register_cq(&self, req: RegisterCqReq) -> Result<CqHandle, UapiError>;
    fn submit_io(&self, req: IoSubmitReq) -> Result<OpId, UapiError>;
    fn poll_cq(&self, cq: CqHandle) -> Vec<CompletionEvent>;
    fn get_health(&self, entity: EntityId) -> Result<HealthStatus, UapiError>;
}
```

---

## 8. 事件模型草案

建议从第一天开始就把事件做成统一 envelope，而不是先打散日志。

```rust
#[derive(Debug, Clone)]
pub enum SimEvent {
    TaskCreated { at: SimTimestamp, task: TaskKey },
    RoutePlanned { at: SimTimestamp, task: TaskKey, decision: RouteDecision },
    BlockPromoted { at: SimTimestamp, block: BlockHash, placement: BlockPlacement },
    BlockEvicted { at: SimTimestamp, block: BlockHash, from: BlockPlacement },
    DispatchSubmitted { at: SimTimestamp, req: DispatchRequest },
    CompletionObserved { at: SimTimestamp, completion: CompletionEvent },
    FaultInjected { at: SimTimestamp, fault: String },
}
```

要求：

- 所有事件至少带 `at`
- 与 task 强相关的事件尽量带 `TaskKey`
- 与 block 强相关的事件尽量带 `BlockHash`
- completion 相关事件统一复用 `CompletionEvent`

---

## 9. guest UAPI 草案

### 9.1 最小对象

第一阶段建议只做这些对象：

- `ubc0`
- `entityX`
- `segmentY`
- `cqZ`
- `queue pair`

### 9.2 最小操作

```text
QUERY_TOPOLOGY
CREATE_SEGMENT
REGISTER_CQ
SUBMIT_IO
POLL_CQ
GET_HEALTH
```

### 9.3 Linux 风格落点

第一阶段建议优先提供：

- `/dev/ubcore0`
- 只读 sysfs 观测面
- 极小 ioctl 命令集

不建议第一阶段就做：

- 完整 mmap 协议
- 完整 netlink family
- 全量 `uburma` 资源类型

---

## 10. 首批占位错误类型

第一阶段允许错误类型比较薄，但名字应先固定。

```rust
pub struct BackendError;
pub struct ServiceError;
pub struct RouteError;
pub struct StoreError;
pub struct UapiError;
```

后续再逐步演进为：

- typed error code
- retryability 标记
- source category
- attachable context

---

## 11. 首批实现顺序

建议按下面顺序实现，而不是并行把所有模块都铺开。

### 11.1 Step 1: `sim-core`

先实现：

- 基础 ID
- `PlLevel`
- `HierarchyCoord`
- `TaskKey`
- `CompletionEvent`
- `SimEvent`

### 11.2 Step 2: `sim-config`

先实现：

- `ScenarioConfig`
- `PlatformConfig`
- `TopologyConfig`
- `WorkloadConfig`
- `FaultConfig`
- YAML 加载与基本校验

### 11.3 Step 3: `sim-topology`

先实现：

- `SimTopology`
- host / `UBPU` / entity / domain 构建
- `TopologySnapshot`

### 11.4 Step 4: `sim-runtime`

先实现：

- `RoutePlanner`
- `SimBlockStore`
- `RingRuntime`
- `EventSink`

### 11.5 Step 5: `sim-services` + `sim-uapi`

先实现：

- `BlockService` stub
- `GuestUapiSurface` stub
- `CompletionEvent` 贯通

### 11.6 Step 6: `sim-workloads`

最后挂：

- `hotset_loop`
- `trace_replay`
- `rust_llm_server_mvp`

---

## 12. 当前明确不定稿的部分

以下内容在 `v0` 不应过早定稿：

- 真正的 MMIO 寄存器布局
- QEMU device model 内部结构
- `vfio-ub` 兼容细节
- allocator HA 持久化格式
- `L5/L6` gossip 消息格式

这些内容应在 P0/P1 跑通后再进入细化。

---

## 13. 结论

这份接口草案的目的不是“把架构文档再写一遍”，而是把后续实现真正需要先冻结的边界提前收拢：

- 强类型配置入口
- 统一 task/route/completion/event 对象
- 清晰的 `ChipBackend` / `BlockService` / `GuestUapiSurface` 边界
- 明确的 workspace 切分和实现顺序

如果下一步开始写代码，建议直接按本文件第 11 节顺序推进，而不要先从 QEMU 细节或 workload harness 开始倒推平台对象。
