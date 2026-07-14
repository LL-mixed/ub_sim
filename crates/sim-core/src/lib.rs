//! Core shared types for the simulator workspace.

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};

pub type SimTimestamp = u64;
pub type NodeId = u64;
pub type HostId = u32;
pub type UbpuId = u32;
pub type UbcId = u32;
pub type UmmuId = u32;
pub type DecoderId = u32;
pub type RouteId = u32;
pub type EntityId = u32;
pub type Eid = u32;
pub type DomainId = u32;
pub type RequestId = u64;
pub type TaskId = u64;
pub type OpId = u64;
pub type SegmentId = u64;
pub type CqId = u32;
pub type CmdQueueId = u32;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct LogicalSystemId(pub u32);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum DecoderKind {
    PlToNode,
    EidToEntity,
    SegmentToDomain,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum RouteScope {
    UbLocal,
    HostLocal,
    DomainShared,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
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

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct HierarchyCoord {
    pub levels: [u32; 8],
}

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct TaskKey {
    pub logical_system: LogicalSystemId,
    pub coord: HierarchyCoord,
    pub scope_depth: u32,
    pub task_id: TaskId,
}

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct BlockHash(pub String);

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct FunctionLabel {
    pub name: String,
    pub level: PlLevel,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct DispatchHandle(pub OpId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct TransferHandle(pub OpId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct ServiceOpHandle(pub OpId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct SegmentHandle(pub SegmentId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct CqHandle(pub CqId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct CmdQueueHandle(pub CmdQueueId);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum HealthStatus {
    Healthy,
    Degraded,
    Failed,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum IntegrityState {
    Valid,
    Suspect,
    Corrupted,
    Quarantined,
}

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub struct BlockPlacement {
    pub block: BlockHash,
    pub level: PlLevel,
    pub node: NodeId,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct RouteBinding {
    pub id: RouteId,
    pub scope: RouteScope,
    pub from_node: NodeId,
    pub to_node: NodeId,
    pub level: PlLevel,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum RouteReason {
    LocalHit,
    CapacityPreferred,
    HealthPreferred,
    RecursiveFallback,
    FlatFallback,
    ExplicitReroute,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct RouteDecision {
    pub from_level: PlLevel,
    pub to_level: PlLevel,
    pub selected_node: NodeId,
    pub reason: RouteReason,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum CopyDirection {
    HostToDevice,
    DeviceToHost,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemoryEndpoint {
    pub node: NodeId,
    pub segment: SegmentHandle,
    pub offset: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum TensorDType {
    U8,
    U32,
    U64,
    F32,
    Opaque,
}

impl TensorDType {
    pub fn byte_width(self) -> Option<u64> {
        match self {
            Self::U8 => Some(1),
            Self::U32 | Self::F32 => Some(4),
            Self::U64 => Some(8),
            Self::Opaque => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum TensorLayout {
    Contiguous,
    Strided,
    Opaque,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum BufferUsage {
    Input,
    Output,
    Inout,
    Workspace,
    Cache,
    Control,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DispatchBufferBinding {
    pub name: String,
    pub usage: BufferUsage,
    pub endpoint: MemoryEndpoint,
    pub bytes: u64,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
    pub layout: TensorLayout,
    pub strides: Option<Vec<u64>>,
    pub resident: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct RequestCorrelation {
    pub request_id: String,
    pub trace_id: Option<String>,
    pub op_name: Option<String>,
    pub step_index: Option<u32>,
    pub sequence_no: Option<u64>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ExecutionLifecycle {
    Init,
    Warmup,
    Reuse,
    Reset,
    Teardown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum ExecutionStepKind {
    RequestControl,
    CacheResolve,
    CacheFill,
    Compute,
    Finalize,
    Generic,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExecutionPlanRef {
    pub plan_id: String,
    pub step_id: String,
    pub step_kind: ExecutionStepKind,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExecutionContextRef {
    pub device_context_id: String,
    pub runtime_context_id: Option<String>,
    pub lifecycle: ExecutionLifecycle,
    pub warm: bool,
    pub reusable: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExecutionContextCommand {
    pub device_context_id: String,
    pub runtime_context_id: Option<String>,
    pub lifecycle: ExecutionLifecycle,
    pub warm: bool,
    pub reusable: bool,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackendExecutionRequest {
    pub correlation: RequestCorrelation,
    pub plan: Option<ExecutionPlanRef>,
    pub context: Option<ExecutionContextRef>,
    pub bindings: Vec<DispatchBufferBinding>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum DispatchBackendProfile {
    HostVector,
    TmrbVector,
    HostMatmul,
    HostGemm,
    HostQuantizedGemm,
    HostFp8Gemm,
    HostEngramContext,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum DispatchRuntimeVariant {
    HostBuildGraph,
    TensormapAndRingbuffer,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DispatchExecutionContext {
    pub block_hash: Option<String>,
    pub request_index: Option<u64>,
    pub block_index: Option<u64>,
    pub request_blocks_total: Option<u64>,
    pub blocks_remaining_in_request: Option<u64>,
    pub is_first_block_in_request: bool,
    pub is_last_block_in_request: bool,
    pub request_control_phase: Option<String>,
    pub request_control_epoch: Option<u64>,
    pub request_control_result_kind: Option<String>,
    pub request_control_result_value: Option<u64>,
    pub request_control_view_kind: Option<String>,
    pub kvcache_resolution_kind: Option<String>,
    pub kvcache_view_kind: Option<String>,
    pub kvcache_transition_kind: Option<String>,
    pub logical_system_id: Option<u32>,
    pub scope_depth: Option<u32>,
    pub prefix_group: Option<u64>,
    pub route_from_level: Option<String>,
    pub route_to_level: Option<String>,
    pub route_selected_node: Option<NodeId>,
    pub route_reason: Option<String>,
    pub placement_level: Option<String>,
    pub placement_node: Option<NodeId>,
    pub capacity_pressure_active: bool,
    pub evictions_seen: u64,
    pub block_writebacks_seen: u64,
    pub promoted_this_access: bool,
    pub reloaded_after_eviction: bool,
    pub uses_dfs_fallback: bool,
    pub includes_request_control: bool,
    pub includes_prefix_shared: bool,
    pub hot_segment: Option<u64>,
    pub request_segment: Option<u64>,
    pub control_segment: Option<u64>,
    pub prefix_segment: Option<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BinaryArtifactRef {
    pub id: String,
    pub format: String,
    pub source: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SimplerKernelArtifact {
    pub func_id: i32,
    pub binary: BinaryArtifactRef,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DispatchLaunchParams {
    pub aicpu_thread_num: u32,
    pub block_dim: u32,
    pub device_id: u32,
    pub orch_thread_num: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum SimplerRuntimeArg {
    ScalarU64(u64),
    InputSegment {
        endpoint: MemoryEndpoint,
        bytes: u64,
    },
    OutputSegment {
        endpoint: MemoryEndpoint,
        bytes: u64,
    },
    InoutSegment {
        endpoint: MemoryEndpoint,
        bytes: u64,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct SimplerRuntimeArtifacts {
    pub host_runtime_library: BinaryArtifactRef,
    pub orch_shared_object: BinaryArtifactRef,
    pub orch_function_name: String,
    pub aicpu_binary: Option<BinaryArtifactRef>,
    pub aicore_binary: Option<BinaryArtifactRef>,
    pub kernels: Vec<SimplerKernelArtifact>,
    pub launch: DispatchLaunchParams,
    pub runtime_env: BTreeMap<String, String>,
    pub args: Vec<SimplerRuntimeArg>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DispatchBackendSpec {
    pub profile: DispatchBackendProfile,
    pub platform: String,
    pub runtime_variant: DispatchRuntimeVariant,
    pub callable_hint: Option<String>,
    pub context: Option<DispatchExecutionContext>,
    pub simpler_runtime: Option<SimplerRuntimeArtifacts>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DispatchRequest {
    pub task: TaskKey,
    pub function: FunctionLabel,
    pub backend_spec: Option<DispatchBackendSpec>,
    pub request: Option<BackendExecutionRequest>,
    pub target_level: PlLevel,
    pub target_node: NodeId,
    pub input_segments: Vec<SegmentHandle>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BackendDispatchOperation {
    pub task: TaskKey,
    pub function: FunctionLabel,
    pub backend_spec: DispatchBackendSpec,
    pub request: BackendExecutionRequest,
    pub target_level: PlLevel,
    pub target_node: NodeId,
    pub legacy_input_segments: Vec<SegmentHandle>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CopyRequest {
    pub task: TaskKey,
    pub direction: CopyDirection,
    pub bytes: u64,
    pub src: MemoryEndpoint,
    pub dst: MemoryEndpoint,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum IoOpcode {
    ReadBlock,
    WriteBlock,
    Dispatch,
    RemoteFetch,
    RemoteStore,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct IoSubmitReq {
    pub op_id: OpId,
    pub task: Option<TaskKey>,
    pub entity: EntityId,
    pub opcode: IoOpcode,
    pub segment: Option<SegmentHandle>,
    pub block: Option<BlockHash>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum CompletionSource {
    ChipBackend,
    BlockService,
    ShmemService,
    DfsService,
    DbService,
    GuestUapi,
    RemoteNode,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum CompletionStatus {
    Success,
    RetryableFailure { code: String },
    FatalFailure { code: String },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompletionEvent {
    pub op_id: OpId,
    pub task: Option<TaskKey>,
    pub source: CompletionSource,
    pub status: CompletionStatus,
    pub finished_at: SimTimestamp,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub enum SimEvent {
    TaskCreated {
        at: SimTimestamp,
        task: TaskKey,
    },
    RoutePlanned {
        at: SimTimestamp,
        task: TaskKey,
        decision: RouteDecision,
    },
    BlockPromoted {
        at: SimTimestamp,
        block: BlockHash,
        placement: BlockPlacement,
    },
    BlockEvicted {
        at: SimTimestamp,
        block: BlockHash,
        from: BlockPlacement,
    },
    DispatchSubmitted {
        at: SimTimestamp,
        req: DispatchRequest,
    },
    CompletionObserved {
        at: SimTimestamp,
        completion: CompletionEvent,
    },
    RuntimeRetried {
        at: SimTimestamp,
        op_id: OpId,
        reason: String,
        attempt: u32,
    },
    RuntimeFailed {
        at: SimTimestamp,
        op_id: OpId,
        reason: String,
    },
    W4ResultHandled {
        at: SimTimestamp,
        task: TaskKey,
        function_name: String,
        block_hash: Option<BlockHash>,
        request_index: Option<u64>,
        block_index: Option<u64>,
        result_segment: SegmentHandle,
        payload_validated: bool,
        request_control_phase: Option<String>,
        request_control_result_kind: Option<String>,
        request_control_view_kind: Option<String>,
        kvcache_resolution_kind: Option<String>,
        kvcache_view_kind: Option<String>,
        kvcache_transition_kind: Option<String>,
    },
    W4ServiceResultApplied {
        at: SimTimestamp,
        task: TaskKey,
        service_kind: String,
        action_kind: String,
        block_hash: Option<BlockHash>,
        target_segment: SegmentHandle,
        result_segment: SegmentHandle,
    },
    FaultInjected {
        at: SimTimestamp,
        fault: String,
    },
}

#[derive(Debug, thiserror::Error)]
pub enum SimError {
    #[error("not implemented")]
    NotImplemented,
    #[error("not found: {0}")]
    NotFound(&'static str),
    #[error("invalid input: {0}")]
    InvalidInput(&'static str),
}
