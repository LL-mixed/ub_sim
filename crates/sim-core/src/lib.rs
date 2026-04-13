//! Core shared types for the simulator workspace.

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

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct DispatchRequest {
    pub task: TaskKey,
    pub function: FunctionLabel,
    pub target_level: PlLevel,
    pub target_node: NodeId,
    pub input_segments: Vec<SegmentHandle>,
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
