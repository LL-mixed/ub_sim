//! Guest-visible UAPI surface placeholders.

use std::collections::{HashMap, VecDeque};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

use sim_config::ScenarioConfig;
use sim_core::{
    BackendDispatchOperation, BackendExecutionRequest, BinaryArtifactRef, BlockHash, BufferUsage,
    CmdQueueHandle, CompletionEvent, CompletionSource, CompletionStatus, CqHandle,
    DispatchBackendProfile, DispatchBackendSpec, DispatchBufferBinding, DispatchLaunchParams,
    DispatchRuntimeVariant, ExecutionContextRef, ExecutionLifecycle, FunctionLabel,
    EntityId, HealthStatus, HierarchyCoord, IoOpcode, IoSubmitReq, LogicalSystemId,
    MemoryEndpoint, PlLevel, RequestCorrelation, SegmentHandle, SimError, SimplerKernelArtifact,
    SimplerRuntimeArg, SimplerRuntimeArtifacts, TaskKey, TensorDType, TensorLayout,
};
use sim_models::qwen3_dense_0_6b::{
    self, Qwen3Dense06bProfile, Qwen3Dense06bShard, QWEN3_DENSE_0_6B_PROFILE,
};
use sim_runtime::{
    LocalRuntimeEngine, RuntimeCompletionTracker, RuntimeDriveAction, RuntimeQueueRecord,
    RuntimeWorkItem, RuntimeWorkKind, SharedRuntimeExecutor, VecEventSink,
};
use sim_services::{
    block::{BlockServiceProfile, BlockServiceStub},
    db::{DbGetReq, DbPutReq, DbServiceProfile, DbServiceStub},
    dfs::{DfsReadReq, DfsServiceProfile, DfsServiceStub, DfsWriteReq},
    shmem::{ShmemGetReq, ShmemPutReq, ShmemServiceProfile, ShmemServiceStub},
    weights::{
        ServiceObjectKind, ServiceObjectMetadataPut, ServiceObjectPayloadWrite,
        ServiceObjectPublishReq, ServiceObjectResolveReq, WeightStorageKind, WeightsServiceStub,
    },
};
use sim_topology::{SimTopology, TopologySnapshot};

#[derive(Debug, Clone)]
pub enum UapiDescriptor {
    Io(IoSubmitReq),
    BlockWriteback {
        block: BlockHash,
        task: Option<TaskKey>,
    },
    ShmemPut(ShmemPutReq),
    ShmemGet(ShmemGetReq),
    DfsRead(DfsReadReq),
    DfsWrite(DfsWriteReq),
    DbPut(DbPutReq),
    DbGet(DbGetReq),
}

#[derive(Debug, Clone)]
pub enum UapiCommand {
    QueryTopology,
    CreateSegment {
        bytes: u64,
    },
    RegisterCq {
        owner: EntityId,
    },
    CreateCmdQueue {
        cq: CqHandle,
        owner: EntityId,
        depth: usize,
    },
    EnqueueCmd {
        cmdq: CmdQueueHandle,
        owner: EntityId,
        desc: UapiDescriptor,
    },
    RingDoorbell {
        cmdq: CmdQueueHandle,
        owner: EntityId,
        max_batch: Option<usize>,
    },
    SubmitIo {
        req: IoSubmitReq,
    },
    SubmitBlockWriteback {
        block: BlockHash,
        task: Option<TaskKey>,
    },
    SubmitShmemPut {
        req: ShmemPutReq,
    },
    SubmitShmemGet {
        req: ShmemGetReq,
    },
    SubmitDfsRead {
        req: DfsReadReq,
    },
    SubmitDfsWrite {
        req: DfsWriteReq,
    },
    SubmitDbPut {
        req: DbPutReq,
    },
    SubmitDbGet {
        req: DbGetReq,
    },
    PollCq {
        cq: CqHandle,
        owner: EntityId,
        max_entries: Option<usize>,
    },
    DrainCq {
        cq: CqHandle,
        owner: EntityId,
    },
    GetHealth {
        entity: EntityId,
    },
}

#[derive(Debug, Clone)]
pub enum UapiResponse {
    TopologySnapshot(TopologySnapshot),
    SegmentCreated(SegmentHandle),
    CqRegistered(CqHandle),
    CmdQueueCreated(CmdQueueHandle),
    IoSubmitted(u64),
    CommandEnqueued {
        depth: usize,
        remaining_capacity: usize,
    },
    DoorbellRung {
        submitted: usize,
        pending: usize,
    },
    Completions {
        events: Vec<CompletionEvent>,
        remaining: usize,
    },
    HealthStatus(HealthStatus),
}

pub trait GuestUapiSurface {
    fn query_topology(&self) -> TopologySnapshot;
    fn create_segment(&mut self, bytes: u64) -> Result<SegmentHandle, SimError>;
    fn register_cq(&mut self) -> Result<CqHandle, SimError>;
    fn submit_io(&mut self, req: IoSubmitReq) -> Result<u64, SimError>;
    fn poll_cq(&self, cq: CqHandle) -> Vec<CompletionEvent>;
    fn get_health(&self, entity: EntityId) -> Result<HealthStatus, SimError>;
}

#[derive(Debug)]
pub struct LocalGuestUapiSurface {
    topology: SimTopology,
    block_service: BlockServiceStub,
    shmem_service: ShmemServiceStub,
    dfs_service: DfsServiceStub,
    db_service: DbServiceStub,
    segment_payloads: HashMap<SegmentHandle, Vec<u8>>,
    block_payloads: HashMap<BlockHash, Vec<u8>>,
    next_segment_id: u64,
    next_cq_id: u32,
    next_cmdq_id: u32,
    service_clock: u64,
    runtime_issue_latency_us: u64,
    runtime_retry_delay_us: u64,
    runtime_queue_depth: usize,
    runtime_max_retries: u32,
    cq_events: HashMap<CqHandle, CompletionQueueState>,
    cmd_queues: HashMap<CmdQueueHandle, CommandQueueState>,
    runtime_queue: SharedRuntimeExecutor<RuntimeWorkItem<UapiRuntimePayload>>,
    completion_routes: RuntimeCompletionTracker<CqHandle>,
}

#[derive(Debug)]
struct CompletionQueueState {
    owner: EntityId,
    events: VecDeque<CompletionEvent>,
}

#[derive(Debug)]
struct CommandQueueState {
    cq: CqHandle,
    owner: EntityId,
    depth: usize,
    pending: VecDeque<UapiDescriptor>,
}

#[derive(Debug, Clone)]
struct UapiRuntimePayload {
    cq: CqHandle,
    desc: UapiDescriptor,
}

#[derive(Debug, Clone, Copy)]
struct UapiRuntimeFailure {
    cq: CqHandle,
    code: &'static str,
}

#[derive(Debug, Clone, serde::Deserialize)]
struct SimplerRuntimeManifestEnvelope {
    simpler_runtime: SimplerRuntimeManifest,
}

#[derive(Debug, Clone, serde::Deserialize)]
struct SimplerRuntimeManifest {
    host_runtime_library: BinaryArtifactRef,
    orch_shared_object: BinaryArtifactRef,
    orch_function_name: String,
    aicpu_binary: Option<BinaryArtifactRef>,
    aicore_binary: Option<BinaryArtifactRef>,
    kernels: Vec<SimplerKernelArtifact>,
    launch: DispatchLaunchParams,
    #[serde(default)]
    runtime_env: BTreeMap<String, String>,
}

impl LocalGuestUapiSurface {
    pub fn new(topology: SimTopology) -> Self {
        Self::with_profiles_and_runtime_policy(
            topology,
            BlockServiceProfile::default(),
            ShmemServiceProfile::default(),
            DfsServiceProfile::default(),
            DbServiceProfile::default(),
            4,
            5,
            32,
            1,
        )
    }

    pub fn with_profiles_and_runtime_policy(
        topology: SimTopology,
        block_profile: BlockServiceProfile,
        shmem_profile: ShmemServiceProfile,
        dfs_profile: DfsServiceProfile,
        db_profile: DbServiceProfile,
        runtime_issue_latency_us: u64,
        runtime_retry_delay_us: u64,
        runtime_queue_depth: usize,
        runtime_max_retries: u32,
    ) -> Self {
        Self::with_service_profiles(
            topology,
            block_profile,
            shmem_profile,
            dfs_profile,
            db_profile,
        )
        .with_runtime_policy(
            runtime_issue_latency_us,
            runtime_retry_delay_us,
            runtime_queue_depth,
            runtime_max_retries,
        )
    }

    pub fn with_block_profile(topology: SimTopology, profile: BlockServiceProfile) -> Self {
        Self::with_service_profiles(
            topology,
            profile,
            ShmemServiceProfile::default(),
            DfsServiceProfile::default(),
            DbServiceProfile::default(),
        )
    }

    pub fn with_service_profiles(
        topology: SimTopology,
        block_profile: BlockServiceProfile,
        shmem_profile: ShmemServiceProfile,
        dfs_profile: DfsServiceProfile,
        db_profile: DbServiceProfile,
    ) -> Self {
        Self {
            topology,
            block_service: BlockServiceStub::with_profile(block_profile),
            shmem_service: ShmemServiceStub::new(shmem_profile),
            dfs_service: DfsServiceStub::new(dfs_profile),
            db_service: DbServiceStub::new(db_profile),
            segment_payloads: HashMap::new(),
            block_payloads: HashMap::new(),
            next_segment_id: 0,
            next_cq_id: 0,
            next_cmdq_id: 0,
            service_clock: 0,
            runtime_issue_latency_us: 4,
            runtime_retry_delay_us: 5,
            runtime_queue_depth: 32,
            runtime_max_retries: 1,
            cq_events: HashMap::new(),
            cmd_queues: HashMap::new(),
            runtime_queue: SharedRuntimeExecutor::with_policy(4, 5, 32, 1),
            completion_routes: RuntimeCompletionTracker::default(),
        }
    }

    pub fn with_runtime_policy(
        mut self,
        runtime_issue_latency_us: u64,
        runtime_retry_delay_us: u64,
        runtime_queue_depth: usize,
        runtime_max_retries: u32,
    ) -> Self {
        self.runtime_issue_latency_us = runtime_issue_latency_us;
        self.runtime_retry_delay_us = runtime_retry_delay_us;
        self.runtime_queue_depth = runtime_queue_depth;
        self.runtime_max_retries = runtime_max_retries;
        self.runtime_queue = SharedRuntimeExecutor::with_policy(
            runtime_issue_latency_us,
            runtime_retry_delay_us,
            runtime_queue_depth,
            runtime_max_retries,
        );
        self
    }

    pub fn write_segment_payload(
        &mut self,
        segment: SegmentHandle,
        offset: usize,
        bytes: &[u8],
    ) -> Result<(), SimError> {
        let payload = self
            .segment_payloads
            .get_mut(&segment)
            .ok_or(SimError::NotFound("segment payload"))?;
        let end = offset
            .checked_add(bytes.len())
            .ok_or(SimError::InvalidInput("segment payload range overflow"))?;
        if end > payload.len() {
            return Err(SimError::InvalidInput("segment payload range out of bounds"));
        }
        payload[offset..end].copy_from_slice(bytes);
        Ok(())
    }

    pub fn read_segment_payload(
        &self,
        segment: SegmentHandle,
        offset: usize,
        out: &mut [u8],
    ) -> Result<(), SimError> {
        let payload = self
            .segment_payloads
            .get(&segment)
            .ok_or(SimError::NotFound("segment payload"))?;
        let end = offset
            .checked_add(out.len())
            .ok_or(SimError::InvalidInput("segment payload range overflow"))?;
        if end > payload.len() {
            return Err(SimError::InvalidInput("segment payload range out of bounds"));
        }
        out.copy_from_slice(&payload[offset..end]);
        Ok(())
    }

    fn default_cq(&self) -> Result<CqHandle, SimError> {
        self.cq_events
            .keys()
            .next()
            .copied()
            .ok_or(SimError::NotFound("completion queue"))
    }

    fn enqueue_to_cq(&mut self, cq: CqHandle, event: CompletionEvent) -> Result<(), SimError> {
        let queue = self
            .cq_events
            .get_mut(&cq)
            .ok_or(SimError::NotFound("completion queue"))?;
        queue.events.push_back(event);
        Ok(())
    }

    pub fn drain_cq(&mut self, cq: CqHandle) -> Vec<CompletionEvent> {
        self.poll_cq_with_owner(cq, 0, None)
            .map(|(events, _)| events)
            .unwrap_or_default()
    }

    fn stage_runtime_descriptor(
        &mut self,
        cq: CqHandle,
        desc: UapiDescriptor,
    ) -> Result<(), SimError> {
        let now = self.next_service_time();
        let kind = runtime_kind_for_descriptor(&desc);
        let task = runtime_task_for_descriptor(&desc);
        self.runtime_queue
            .enqueue(
                RuntimeWorkItem {
                    op_id: now,
                    kind,
                    task,
                    payload: UapiRuntimePayload { cq, desc },
                },
                now,
            )
            .map_err(|err| match err {
                SimError::InvalidInput("runtime queue full") => {
                    SimError::InvalidInput("uapi runtime queue full")
                }
                other => other,
            })
    }

    fn flush_services(&mut self, now: u64) -> Result<(), SimError> {
        for event in self.block_service.poll_ready(now) {
            self.route_completion_to_cq(event)?;
        }
        for event in self.shmem_service.poll_ready(now) {
            self.route_completion_to_cq(event)?;
        }
        for event in self.dfs_service.poll_ready(now) {
            self.route_completion_to_cq(event)?;
        }
        for event in self.db_service.poll_ready(now) {
            self.route_completion_to_cq(event)?;
        }
        Ok(())
    }

    fn flush_runtime(&mut self, now: u64) -> Result<(), SimError> {
        let mut runtime_queue = std::mem::replace(
            &mut self.runtime_queue,
            SharedRuntimeExecutor::with_policy(
                self.runtime_issue_latency_us,
                self.runtime_retry_delay_us,
                self.runtime_queue_depth,
                self.runtime_max_retries,
            ),
        );

        let (failures, force_flush) = runtime_queue.drive_ready(now, |entry| {
            let RuntimeQueueRecord {
                payload:
                    RuntimeWorkItem {
                        payload: UapiRuntimePayload { cq, desc },
                        ..
                    },
                ..
            } = entry;
            match self.submit_descriptor_to_cq(desc.clone(), *cq) {
                Ok(_) => RuntimeDriveAction::Complete,
                Err(SimError::InvalidInput(code))
                    if matches!(
                        code.as_ref(),
                        "block queue full"
                            | "shmem queue full"
                            | "dfs queue full"
                            | "db queue full"
                    ) =>
                {
                    if now == u64::MAX {
                        let _ = self.flush_services(now);
                    }
                    RuntimeDriveAction::Retry(UapiRuntimeFailure { cq: *cq, code })
                }
                Err(err) => RuntimeDriveAction::Fail(UapiRuntimeFailure {
                    cq: *cq,
                    code: match err {
                        SimError::InvalidInput(code) => code,
                        _ => "runtime_issue_failed",
                    },
                }),
            }
        });
        self.runtime_queue = runtime_queue;

        for failure in failures {
            let op_id = self.next_service_time();
            self.enqueue_to_cq(
                failure.cq,
                CompletionEvent {
                    op_id,
                    task: None,
                    source: CompletionSource::GuestUapi,
                    status: CompletionStatus::RetryableFailure {
                        code: format!("runtime_exhausted_{}", failure.code),
                    },
                    finished_at: now,
                },
            )?;
        }

        if force_flush && !self.runtime_queue.is_empty() {
            return self.flush_runtime(now);
        }
        Ok(())
    }

    fn route_completion_to_cq(&mut self, event: CompletionEvent) -> Result<(), SimError> {
        let cq = self
            .completion_routes
            .complete(&event)
            .or_else(|| self.default_cq().ok())
            .ok_or(SimError::NotFound("completion queue"))?;
        self.enqueue_to_cq(cq, event)
    }

    fn bind_completion_route(&mut self, source: CompletionSource, op_id: u64, cq: CqHandle) {
        self.completion_routes.issue(source, op_id, cq);
    }

    fn create_cmd_queue(
        &mut self,
        cq: CqHandle,
        owner: EntityId,
        depth: usize,
    ) -> Result<CmdQueueHandle, SimError> {
        if depth == 0 {
            return Err(SimError::InvalidInput(
                "command queue depth must be positive",
            ));
        }
        let cq_state = self
            .cq_events
            .get(&cq)
            .ok_or(SimError::NotFound("completion queue"))?;
        if cq_state.owner != owner {
            return Err(SimError::InvalidInput("command queue owner mismatch"));
        }
        self.next_cmdq_id += 1;
        let cmdq = CmdQueueHandle(self.next_cmdq_id);
        self.cmd_queues.insert(
            cmdq,
            CommandQueueState {
                cq,
                owner,
                depth,
                pending: VecDeque::new(),
            },
        );
        Ok(cmdq)
    }

    fn enqueue_cmd(
        &mut self,
        cmdq: CmdQueueHandle,
        owner: EntityId,
        desc: UapiDescriptor,
    ) -> Result<(usize, usize), SimError> {
        let queue = self
            .cmd_queues
            .get_mut(&cmdq)
            .ok_or(SimError::NotFound("command queue"))?;
        if queue.owner != owner {
            return Err(SimError::InvalidInput("command queue owner mismatch"));
        }
        if queue.pending.len() >= queue.depth {
            return Err(SimError::InvalidInput("command queue full"));
        }
        queue.pending.push_back(desc);
        Ok((queue.pending.len(), queue.depth - queue.pending.len()))
    }

    fn ring_doorbell(
        &mut self,
        cmdq: CmdQueueHandle,
        owner: EntityId,
        max_batch: Option<usize>,
    ) -> Result<(usize, usize), SimError> {
        let (cq, pending_after_ring, mut staged) = {
            let queue = self
                .cmd_queues
                .get_mut(&cmdq)
                .ok_or(SimError::NotFound("command queue"))?;
            if queue.owner != owner {
                return Err(SimError::InvalidInput("command queue owner mismatch"));
            }
            let batch = max_batch
                .unwrap_or(queue.pending.len())
                .min(queue.pending.len());
            let mut staged = Vec::with_capacity(batch);
            for _ in 0..batch {
                if let Some(desc) = queue.pending.pop_front() {
                    staged.push(desc);
                }
            }
            (queue.cq, queue.pending.len(), staged)
        };

        let submitted = staged.len();
        for desc in staged.drain(..) {
            self.stage_runtime_descriptor(cq, desc)?;
        }
        Ok((submitted, pending_after_ring))
    }

    fn poll_cq_with_owner(
        &mut self,
        cq: CqHandle,
        owner: EntityId,
        max_entries: Option<usize>,
    ) -> Result<(Vec<CompletionEvent>, usize), SimError> {
        self.flush_runtime(u64::MAX)?;
        self.flush_services(u64::MAX)?;
        let queue = self
            .cq_events
            .get_mut(&cq)
            .ok_or(SimError::NotFound("completion queue"))?;
        if queue.owner != owner {
            return Err(SimError::InvalidInput("completion queue owner mismatch"));
        }
        let limit = max_entries
            .unwrap_or(queue.events.len())
            .min(queue.events.len());
        let mut events = Vec::with_capacity(limit);
        for _ in 0..limit {
            if let Some(event) = queue.events.pop_front() {
                events.push(event);
            }
        }
        Ok((events, queue.events.len()))
    }

    fn submit_descriptor_to_cq(
        &mut self,
        desc: UapiDescriptor,
        cq: CqHandle,
    ) -> Result<u64, SimError> {
        match desc {
            UapiDescriptor::Io(req) => self.submit_io_to_cq(req, cq),
            UapiDescriptor::BlockWriteback { block, task } => {
                let now = self.next_service_time();
                let handle = self.block_service.submit_writeback(block, task, now)?;
                self.bind_completion_route(CompletionSource::BlockService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::ShmemPut(req) => {
                let now = self.next_service_time();
                let handle = self.shmem_service.submit_put(req, now)?;
                self.bind_completion_route(CompletionSource::ShmemService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::ShmemGet(req) => {
                let now = self.next_service_time();
                let handle = self.shmem_service.submit_get(req, now)?;
                self.bind_completion_route(CompletionSource::ShmemService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::DfsRead(req) => {
                let now = self.next_service_time();
                let handle = self.dfs_service.submit_read(req, now)?;
                self.bind_completion_route(CompletionSource::DfsService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::DfsWrite(req) => {
                let now = self.next_service_time();
                let handle = self.dfs_service.submit_write(req, now)?;
                self.bind_completion_route(CompletionSource::DfsService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::DbPut(req) => {
                let now = self.next_service_time();
                let handle = self.db_service.submit_put(req, now)?;
                self.bind_completion_route(CompletionSource::DbService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::DbGet(req) => {
                let now = self.next_service_time();
                let handle = self.db_service.submit_get(req, now)?;
                self.bind_completion_route(CompletionSource::DbService, handle.0, cq);
                Ok(handle.0)
            }
        }
    }

    pub fn execute(&mut self, cmd: UapiCommand) -> Result<UapiResponse, SimError> {
        match cmd {
            UapiCommand::QueryTopology => Ok(UapiResponse::TopologySnapshot(self.query_topology())),
            UapiCommand::CreateSegment { bytes } => {
                self.create_segment(bytes).map(UapiResponse::SegmentCreated)
            }
            UapiCommand::RegisterCq { owner } => self
                .register_cq_with_owner(owner)
                .map(UapiResponse::CqRegistered),
            UapiCommand::CreateCmdQueue { cq, owner, depth } => self
                .create_cmd_queue(cq, owner, depth)
                .map(UapiResponse::CmdQueueCreated),
            UapiCommand::EnqueueCmd { cmdq, owner, desc } => self
                .enqueue_cmd(cmdq, owner, desc)
                .map(
                    |(depth, remaining_capacity)| UapiResponse::CommandEnqueued {
                        depth,
                        remaining_capacity,
                    },
                ),
            UapiCommand::RingDoorbell {
                cmdq,
                owner,
                max_batch,
            } => self
                .ring_doorbell(cmdq, owner, max_batch)
                .map(|(submitted, pending)| UapiResponse::DoorbellRung { submitted, pending }),
            UapiCommand::SubmitIo { req } => self.submit_io(req).map(UapiResponse::IoSubmitted),
            UapiCommand::SubmitBlockWriteback { block, task } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::BlockWriteback { block, task }, cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitShmemPut { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::ShmemPut(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitShmemGet { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::ShmemGet(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitDfsRead { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::DfsRead(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitDfsWrite { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::DfsWrite(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitDbPut { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::DbPut(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitDbGet { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::DbGet(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::PollCq {
                cq,
                owner,
                max_entries,
            } => self
                .poll_cq_with_owner(cq, owner, max_entries)
                .map(|(events, remaining)| UapiResponse::Completions { events, remaining }),
            UapiCommand::DrainCq { cq, owner } => self
                .poll_cq_with_owner(cq, owner, None)
                .map(|(events, remaining)| UapiResponse::Completions { events, remaining }),
            UapiCommand::GetHealth { entity } => {
                self.get_health(entity).map(UapiResponse::HealthStatus)
            }
        }
    }

    fn next_service_time(&mut self) -> u64 {
        self.service_clock += 1;
        self.service_clock
    }
}

impl GuestUapiSurface for LocalGuestUapiSurface {
    fn query_topology(&self) -> TopologySnapshot {
        self.topology.snapshot()
    }

    fn create_segment(&mut self, bytes: u64) -> Result<SegmentHandle, SimError> {
        if bytes == 0 {
            return Err(SimError::InvalidInput("segment bytes must be positive"));
        }
        self.next_segment_id += 1;
        let segment = SegmentHandle(self.next_segment_id);
        self.shmem_service.register_segment(segment, 0, bytes)?;
        self.segment_payloads.insert(segment, vec![0; bytes as usize]);
        Ok(segment)
    }

    fn register_cq(&mut self) -> Result<CqHandle, SimError> {
        self.register_cq_with_owner(0)
    }

    fn submit_io(&mut self, req: IoSubmitReq) -> Result<u64, SimError> {
        let cq = self.default_cq()?;
        self.submit_io_to_cq(req, cq)
    }

    fn poll_cq(&self, cq: CqHandle) -> Vec<CompletionEvent> {
        self.cq_events
            .get(&cq)
            .map(|queue| queue.events.iter().cloned().collect())
            .unwrap_or_default()
    }

    fn get_health(&self, entity: EntityId) -> Result<HealthStatus, SimError> {
        self.topology
            .entities
            .iter()
            .find(|e| e.id == entity)
            .map(|e| e.health)
            .ok_or(SimError::NotFound("entity"))
    }
}

impl LocalGuestUapiSurface {
    fn register_cq_with_owner(&mut self, owner: EntityId) -> Result<CqHandle, SimError> {
        self.next_cq_id += 1;
        let cq = CqHandle(self.next_cq_id);
        self.cq_events.insert(
            cq,
            CompletionQueueState {
                owner,
                events: VecDeque::new(),
            },
        );
        Ok(cq)
    }
    fn submit_io_to_cq(&mut self, req: IoSubmitReq, cq: CqHandle) -> Result<u64, SimError> {
        match req.opcode {
            IoOpcode::ReadBlock => {
                let block = req
                    .block
                    .ok_or(SimError::InvalidInput("missing block hash"))?;
                let segment = req
                    .segment
                    .ok_or(SimError::InvalidInput("missing segment handle"))?;
                self.copy_block_to_segment(&block, segment)?;
                let now = self.next_service_time();
                let handle = self.block_service.submit_read(
                    sim_runtime::BlockReadReq {
                        task: req.task,
                        block,
                    },
                    now,
                )?;
                self.bind_completion_route(CompletionSource::BlockService, handle.0, cq);
                Ok(handle.0)
            }
            IoOpcode::WriteBlock => {
                let block = req
                    .block
                    .ok_or(SimError::InvalidInput("missing block hash"))?;
                let segment = req
                    .segment
                    .ok_or(SimError::InvalidInput("missing segment handle"))?;
                self.copy_segment_to_block(segment, block.clone())?;
                let now = self.next_service_time();
                let handle = self.block_service.submit_write(
                    sim_runtime::BlockWriteReq {
                        task: req.task,
                        block,
                    },
                    now,
                )?;
                self.bind_completion_route(CompletionSource::BlockService, handle.0, cq);
                Ok(handle.0)
            }
            IoOpcode::Dispatch => {
                let event = self.run_chipbackend_dispatch(req)?;
                let op_id = event.op_id;
                self.enqueue_to_cq(cq, event)?;
                Ok(op_id)
            }
            IoOpcode::RemoteFetch | IoOpcode::RemoteStore => {
                /* Phase 2: remote block transport — stub for now */
                let event = CompletionEvent {
                    op_id: req.op_id,
                    task: req.task,
                    source: CompletionSource::RemoteNode,
                    status: CompletionStatus::RetryableFailure {
                        code: "remote_transport_not_available".to_string(),
                    },
                    finished_at: self.next_service_time(),
                };
                self.enqueue_to_cq(cq, event)?;
                Ok(req.op_id)
            }
        }
    }

    fn run_chipbackend_dispatch(
        &mut self,
        req: IoSubmitReq,
    ) -> Result<CompletionEvent, SimError> {
        let now = self.next_service_time();
        let output_segment = req.segment;
        let task = req.task.unwrap_or(TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: req.op_id,
        });
        let result = output_segment
            .ok_or_else(|| "missing_dispatch_segment".to_string())
            .and_then(|segment| {
                let input = self
                    .segment_payloads
                    .get(&segment)
                    .ok_or_else(|| "missing_dispatch_segment_payload".to_string())?
                    .clone();
                run_w4_chipbackend(&self.topology, &task, &input)
                    .map(|output| (segment, output))
            });
        if let Ok((segment, output)) = &result {
            self.write_dispatch_result_to_segment(*segment, output)?;
        }
        Ok(CompletionEvent {
            op_id: req.op_id,
            task: Some(task),
            source: CompletionSource::ChipBackend,
            status: match result {
                Ok(_) => CompletionStatus::Success,
                Err(code) => CompletionStatus::FatalFailure { code },
            },
            finished_at: now,
        })
    }

    fn copy_segment_to_block(
        &mut self,
        segment: SegmentHandle,
        block: BlockHash,
    ) -> Result<(), SimError> {
        let payload = self
            .segment_payloads
            .get(&segment)
            .ok_or(SimError::NotFound("segment payload"))?
            .clone();
        self.block_payloads.insert(block, payload);
        Ok(())
    }

    fn copy_block_to_segment(
        &mut self,
        block: &BlockHash,
        segment: SegmentHandle,
    ) -> Result<(), SimError> {
        let payload = self
            .block_payloads
            .get(block)
            .ok_or(SimError::NotFound("block payload"))?
            .clone();
        let segment_payload = self
            .segment_payloads
            .get_mut(&segment)
            .ok_or(SimError::NotFound("segment payload"))?;
        let copy_len = segment_payload.len().min(payload.len());
        segment_payload[..copy_len].copy_from_slice(&payload[..copy_len]);
        if segment_payload.len() > copy_len {
            segment_payload[copy_len..].fill(0);
        }
        Ok(())
    }

fn write_dispatch_result_to_segment(
        &mut self,
        segment: SegmentHandle,
        output: &[u8],
    ) -> Result<(), SimError> {
        let payload = self
            .segment_payloads
            .get_mut(&segment)
            .ok_or(SimError::NotFound("segment payload"))?;
        let copy_len = payload.len().min(output.len());
        payload[..copy_len].copy_from_slice(&output[..copy_len]);
        if payload.len() > copy_len {
            payload[copy_len..].fill(0);
        }
        Ok(())
    }
}

fn run_w4_chipbackend(
    topology: &SimTopology,
    task: &TaskKey,
    guest_input: &[u8],
) -> Result<Vec<u8>, String> {
    match std::env::var("SIM_UAPI_W4_CHIPBACKEND_PROFILE")
        .unwrap_or_else(|_| "host_vector".to_string())
        .as_str()
    {
        "qwen3_dense_0_6b" => run_qwen3_dense_0_6b_prefill_smoke(topology, task, guest_input),
        "host_matmul" => run_host_matmul_smoke(topology, task),
        "host_vector" | "" => run_host_vector_chipbackend(topology, task, guest_input),
        other => Err(format!("unsupported_w4_chipbackend_profile:{other}")),
    }
}

fn run_host_vector_chipbackend(
    topology: &SimTopology,
    task: &TaskKey,
    guest_input: &[u8],
) -> Result<Vec<u8>, String> {
    let manifest_path = simpler_manifest_path()?;
    let scenario_config = scenario_config_for_chipbackend()?;
    let elems = W4_KVCACHE_PAYLOAD_BYTES / std::mem::size_of::<f32>();
    let size_bytes = elems * std::mem::size_of::<f32>();
    let kvcache_layout = KvCachePayloadLayout::new(elems, size_bytes)?;
    let segment_base = 10_000 + task.task_id.saturating_mul(10);
    let host_node = topology
        .hosts
        .first()
        .map(|host| host.node_id)
        .ok_or_else(|| "missing_host_node".to_string())?;
    let ubpu_node = topology
        .ubpus
        .first()
        .map(|ubpu| ubpu.node_id)
        .ok_or_else(|| "missing_ubpu_node".to_string())?;
    let _dispatch_lock = host_vector_dispatch_lock_guard()?;
    let input_a_bytes = kvcache_input_a_payload(guest_input, size_bytes);
    let input_b_bytes = kvcache_input_b_payload(&kvcache_layout);
    let mut runtime = LocalRuntimeEngine::from_config(&scenario_config);
    let mut sink = VecEventSink::default();
    let complete_at = scenario_config
        .pypto
        .simpler_boundary
        .dispatch_latency_us
        .unwrap_or(15);
    let mut output_segments = Vec::new();
    let chunk_bytes = W4_HOST_VECTOR_CHUNK_BYTES;
    for (chunk_index, chunk_offset) in (0..size_bytes).step_by(chunk_bytes).enumerate() {
        let chunk_end = (chunk_offset + chunk_bytes).min(size_bytes);
        let chunk_len = chunk_end - chunk_offset;
        let chunk_elems = chunk_len / std::mem::size_of::<f32>();
        let input_a = SegmentHandle(segment_base + 1 + chunk_index as u64 * 3);
        let input_b = SegmentHandle(segment_base + 2 + chunk_index as u64 * 3);
        let output = SegmentHandle(segment_base + 3 + chunk_index as u64 * 3);
        runtime.seed_host_segment(host_node, input_a, input_a_bytes[chunk_offset..chunk_end].to_vec());
        runtime.seed_host_segment(host_node, input_b, input_b_bytes[chunk_offset..chunk_end].to_vec());
        runtime.seed_host_segment(host_node, output, vec![0u8; chunk_len]);
        output_segments.push(output);

        let backend_spec = host_vector_backend_spec_from_manifest(
            &manifest_path,
            MemoryEndpoint {
                node: host_node,
                segment: input_a,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: input_b,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: output,
                offset: 0,
            },
            chunk_len as u64,
            chunk_elems as u64,
        )?;
        let mut bindings = kvcache_layout_bindings_for_chunk(
            &kvcache_layout,
            host_node,
            input_a,
            chunk_offset,
            chunk_end,
        );
        bindings.extend([
                dense_f32_binding(
                    "input_a",
                    BufferUsage::Input,
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_a,
                        offset: 0,
                    },
                    chunk_elems as u64,
                ),
                dense_f32_binding(
                    "input_b",
                    BufferUsage::Input,
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_b,
                        offset: 0,
                    },
                    chunk_elems as u64,
                ),
                dense_f32_binding(
                    "output_f",
                    BufferUsage::Output,
                    MemoryEndpoint {
                        node: host_node,
                        segment: output,
                        offset: 0,
                    },
                    chunk_elems as u64,
                ),
            ]);
        let request = kvcache_host_vector_request(
            task.task_id,
            &kvcache_layout,
            chunk_index,
            chunk_offset,
            bindings,
        );
        let dispatch = BackendDispatchOperation {
            task: task.clone(),
            function: FunctionLabel {
                name: "host_vector_example".into(),
                level: PlLevel::L2,
            },
            backend_spec,
            request,
            target_level: PlLevel::L2,
            target_node: ubpu_node,
            legacy_input_segments: vec![input_a, input_b],
        };
        with_suppressed_stdio(|| {
            runtime
                .submit_backend_dispatch(dispatch, &mut sink)
                .map_err(|err| err.to_string())
        })?;
    }
    let completions = with_suppressed_stdio(|| {
        runtime.advance_to(complete_at, &mut sink);
        Ok(runtime.poll_completions(complete_at, &mut sink))
    })?;
    if completions.len() != output_segments.len() {
        return Err(format!(
            "simpler_capi_dispatch_completion_count_mismatch:got={}:expected={}",
            completions.len(),
            output_segments.len()
        ));
    }
    for completion in completions {
        match completion.status {
            CompletionStatus::Success => {}
            other => return Err(format!("simpler_capi_dispatch_failed:{other:?}")),
        }
    }
    let mut produced = Vec::with_capacity(size_bytes);
    for output in output_segments {
        let chunk_output = runtime
            .host_segment_payload(host_node, output)
            .ok_or_else(|| "missing_host_vector_output_payload".to_string())?;
        produced.extend_from_slice(chunk_output);
    }
    let output_values = bytes_to_f32s(&produced);
    let input_values = bytes_to_f32s(&input_a_bytes);
    let input_b_values = bytes_to_f32s(&input_b_bytes);
    if output_values.len() != elems || input_values.len() != elems || input_b_values.len() != elems {
        return Err(format!(
            "simpler_capi_output_len_mismatch:first={:?}:output_len={}:input_a_len={}:input_b_len={}",
            output_values.first(),
            output_values.len(),
            input_values.len(),
            input_b_values.len()
        ));
    }
    for (elem_index, ((output, input_a), input_b)) in output_values
        .iter()
        .zip(input_values.iter())
        .zip(input_b_values.iter())
        .enumerate()
    {
        let Some((block, tile, row_group)) = kvcache_layout.tile_row_group_for_elem(elem_index) else {
            return Err(format!("kvcache_layout_missing_tile_row_group:elem={elem_index}"));
        };
        let sum = *input_a + *input_b;
        let expected = (sum + 1.0) * (sum + 2.0);
        if (*output - expected).abs() > 1e-5 {
            return Err(format!(
                "simpler_capi_output_mismatch:elem={elem_index}:block={}:prefix={}:tile={}:row_group={}:input_a={}:input_b={}:output={}:expected={}",
                block.block_id,
                block.prefix_group_id,
                tile.tile_id,
                row_group.row_group_id,
                input_a,
                input_b,
                output,
                expected
            ));
        }
    }
    Ok(produced)
}

const W4_KVCACHE_PAYLOAD_BYTES: usize = 8192;
const W4_KVCACHE_BLOCKS: usize = 4;
const W4_KVCACHE_PREFIX_GROUPS: usize = 2;
const W4_KVCACHE_TILE_ROWS: usize = 16;
const W4_KVCACHE_TILE_COLS: usize = 16;
const W4_KVCACHE_TILE_BYTES: usize =
    W4_KVCACHE_TILE_ROWS * W4_KVCACHE_TILE_COLS * std::mem::size_of::<f32>();
const W4_KVCACHE_ROW_GROUP_ROWS: usize = 4;
const W4_KVCACHE_ROW_GROUP_BYTES: usize =
    W4_KVCACHE_ROW_GROUP_ROWS * W4_KVCACHE_TILE_COLS * std::mem::size_of::<f32>();
const W4_HOST_VECTOR_CHUNK_BYTES: usize = 4096;

#[derive(Debug, Clone)]
struct KvCachePayloadLayout {
    bytes: usize,
    elems: usize,
    blocks: Vec<KvCachePayloadBlock>,
}

#[derive(Debug, Clone)]
struct KvCachePayloadBlock {
    block_id: usize,
    prefix_group_id: usize,
    offset: usize,
    bytes: usize,
    elems: usize,
    tiles: Vec<KvCachePayloadTile>,
}

#[derive(Debug, Clone)]
struct KvCachePayloadTile {
    tile_id: usize,
    offset: usize,
    bytes: usize,
    elems: usize,
    rows: usize,
    cols: usize,
    row_groups: Vec<KvCachePayloadRowGroup>,
}

#[derive(Debug, Clone)]
struct KvCachePayloadRowGroup {
    row_group_id: usize,
    offset: usize,
    bytes: usize,
    elems: usize,
    rows: usize,
    cols: usize,
}

impl KvCachePayloadLayout {
    fn new(elems: usize, size_bytes: usize) -> Result<Self, String> {
        if size_bytes != W4_KVCACHE_PAYLOAD_BYTES {
            return Err(format!("invalid_kvcache_payload_bytes:{size_bytes}"));
        }
        if size_bytes % std::mem::size_of::<f32>() != 0
            || elems * std::mem::size_of::<f32>() != size_bytes
        {
            return Err("invalid_kvcache_payload_elem_shape".to_string());
        }
        if size_bytes % W4_KVCACHE_BLOCKS != 0
            || W4_KVCACHE_TILE_BYTES % W4_KVCACHE_ROW_GROUP_BYTES != 0
        {
            return Err("invalid_kvcache_layout_divisibility".to_string());
        }
        let bytes_per_block = size_bytes / W4_KVCACHE_BLOCKS;
        if bytes_per_block % W4_KVCACHE_TILE_BYTES != 0 {
            return Err("invalid_kvcache_block_tile_shape".to_string());
        }
        let mut blocks = Vec::with_capacity(W4_KVCACHE_BLOCKS);
        for block_id in 0..W4_KVCACHE_BLOCKS {
            let block_offset = block_id * bytes_per_block;
            let tiles_per_block = bytes_per_block / W4_KVCACHE_TILE_BYTES;
            let mut tiles = Vec::with_capacity(tiles_per_block);
            for tile_index in 0..tiles_per_block {
                let tile_offset = block_offset + tile_index * W4_KVCACHE_TILE_BYTES;
                let row_groups_per_tile = W4_KVCACHE_TILE_BYTES / W4_KVCACHE_ROW_GROUP_BYTES;
                let mut row_groups = Vec::with_capacity(row_groups_per_tile);
                for row_group_index in 0..row_groups_per_tile {
                    let row_group_offset =
                        tile_offset + row_group_index * W4_KVCACHE_ROW_GROUP_BYTES;
                    row_groups.push(KvCachePayloadRowGroup {
                        row_group_id: row_group_index,
                        offset: row_group_offset,
                        bytes: W4_KVCACHE_ROW_GROUP_BYTES,
                        elems: W4_KVCACHE_ROW_GROUP_BYTES / std::mem::size_of::<f32>(),
                        rows: W4_KVCACHE_ROW_GROUP_ROWS,
                        cols: W4_KVCACHE_TILE_COLS,
                    });
                }
                tiles.push(KvCachePayloadTile {
                    tile_id: tile_index,
                    offset: tile_offset,
                    bytes: W4_KVCACHE_TILE_BYTES,
                    elems: W4_KVCACHE_TILE_BYTES / std::mem::size_of::<f32>(),
                    rows: W4_KVCACHE_TILE_ROWS,
                    cols: W4_KVCACHE_TILE_COLS,
                    row_groups,
                });
            }
            blocks.push(KvCachePayloadBlock {
                block_id,
                prefix_group_id: block_id % W4_KVCACHE_PREFIX_GROUPS,
                offset: block_offset,
                bytes: bytes_per_block,
                elems: bytes_per_block / std::mem::size_of::<f32>(),
                tiles,
            });
        }
        Ok(Self {
            bytes: size_bytes,
            elems,
            blocks,
        })
    }

    fn tile_row_group_for_elem(
        &self,
        elem_index: usize,
    ) -> Option<(
        &KvCachePayloadBlock,
        &KvCachePayloadTile,
        &KvCachePayloadRowGroup,
    )> {
        let byte_offset = elem_index.checked_mul(std::mem::size_of::<f32>())?;
        self.blocks.iter().find_map(|block| {
            if byte_offset < block.offset || byte_offset >= block.offset + block.bytes {
                return None;
            }
            block.tiles.iter().find_map(|tile| {
                if byte_offset < tile.offset || byte_offset >= tile.offset + tile.bytes {
                    return None;
                }
                tile.row_groups.iter().find_map(|row_group| {
                    if byte_offset >= row_group.offset
                        && byte_offset < row_group.offset + row_group.bytes
                    {
                        Some((block, tile, row_group))
                    } else {
                        None
                    }
                })
            })
        })
    }
}

fn kvcache_input_a_payload(guest_input: &[u8], size_bytes: usize) -> Vec<u8> {
    let mut input = vec![0u8; size_bytes];
    let copy_len = input.len().min(guest_input.len());
    input[..copy_len].copy_from_slice(&guest_input[..copy_len]);
    input
}

fn kvcache_input_b_payload(layout: &KvCachePayloadLayout) -> Vec<u8> {
    let mut values = vec![3.0f32; layout.elems];
    for block in &layout.blocks {
        let block_elem_count: usize = block.tiles.iter().map(|tile| tile.elems).sum();
        debug_assert_eq!(block_elem_count, block.elems);
        for tile in &block.tiles {
            debug_assert_eq!(tile.rows * tile.cols, tile.elems);
            for row_group in &tile.row_groups {
                debug_assert_eq!(row_group.rows * row_group.cols, row_group.elems);
                let row_group_begin = row_group.offset / std::mem::size_of::<f32>();
                let row_group_end = row_group_begin + row_group.elems;
                let layout_bias = kvcache_layout_bias(block, tile, row_group);
                for value in &mut values[row_group_begin..row_group_end] {
                    *value += layout_bias;
                }
            }
        }
    }
    f32s_to_bytes(&values)
}

fn kvcache_layout_bias(
    block: &KvCachePayloadBlock,
    tile: &KvCachePayloadTile,
    row_group: &KvCachePayloadRowGroup,
) -> f32 {
    block.prefix_group_id as f32
        + block.block_id as f32 * 0.125
        + tile.tile_id as f32 * 0.03125
        + row_group.row_group_id as f32 * 0.0078125
}

fn kvcache_layout_bindings_for_chunk(
    layout: &KvCachePayloadLayout,
    host_node: u64,
    input_segment: SegmentHandle,
    chunk_begin: usize,
    chunk_end: usize,
) -> Vec<DispatchBufferBinding> {
    let mut bindings = Vec::new();
    for block in &layout.blocks {
        if !range_contains(chunk_begin, chunk_end, block.offset, block.bytes) {
            continue;
        }
        bindings.push(opaque_resident_binding(
            format!(
                "kvcache_prefix{}_block{}_state",
                block.prefix_group_id, block.block_id
            ),
            BufferUsage::Inout,
            MemoryEndpoint {
                node: host_node,
                segment: input_segment,
                offset: (block.offset - chunk_begin) as u64,
            },
            block.bytes as u64,
        ));
        for tile in &block.tiles {
            if !range_contains(chunk_begin, chunk_end, tile.offset, tile.bytes) {
                continue;
            }
            bindings.push(opaque_resident_binding(
                format!(
                    "kvcache_block{}_prefix{}_tile{}_state",
                    block.block_id, block.prefix_group_id, tile.tile_id
                ),
                BufferUsage::Inout,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_segment,
                    offset: (tile.offset - chunk_begin) as u64,
                },
                tile.bytes as u64,
            ));
            for row_group in &tile.row_groups {
                if !range_contains(chunk_begin, chunk_end, row_group.offset, row_group.bytes) {
                    continue;
                }
                bindings.push(opaque_resident_binding(
                    format!(
                        "kvcache_block{}_prefix{}_tile{}_rowgroup{}",
                        block.block_id,
                        block.prefix_group_id,
                        tile.tile_id,
                        row_group.row_group_id
                    ),
                    BufferUsage::Inout,
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_segment,
                        offset: (row_group.offset - chunk_begin) as u64,
                    },
                    row_group.bytes as u64,
                ));
            }
        }
    }
    bindings
}

fn range_contains(container_begin: usize, container_end: usize, offset: usize, bytes: usize) -> bool {
    offset >= container_begin && offset + bytes <= container_end
}

#[cfg(unix)]
struct HostVectorDispatchLock {
    fd: libc::c_int,
    _thread_guard: std::sync::MutexGuard<'static, ()>,
}

#[cfg(unix)]
impl Drop for HostVectorDispatchLock {
    fn drop(&mut self) {
        unsafe {
            libc::flock(self.fd, libc::LOCK_UN);
            libc::close(self.fd);
        }
    }
}

#[cfg(unix)]
fn host_vector_dispatch_lock_guard() -> Result<HostVectorDispatchLock, String> {
    static HOST_VECTOR_DISPATCH_MUTEX: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let thread_guard = HOST_VECTOR_DISPATCH_MUTEX
        .lock()
        .map_err(|_| "simpler_capi_dispatch_lock_poisoned".to_string())?;
    unsafe {
        let path = b"/tmp/linqu_simpler_host_vector.lock\0";
        let fd = libc::open(path.as_ptr().cast(), libc::O_CREAT | libc::O_RDWR, 0o666);
        if fd < 0 {
            return Err("simpler_capi_dispatch_lock_open_failed".to_string());
        }
        if libc::flock(fd, libc::LOCK_EX) < 0 {
            libc::close(fd);
            return Err("simpler_capi_dispatch_lock_failed".to_string());
        }

        Ok(HostVectorDispatchLock {
            fd,
            _thread_guard: thread_guard,
        })
    }
}

#[cfg(not(unix))]
struct HostVectorDispatchLock {
    _thread_guard: std::sync::MutexGuard<'static, ()>,
}

#[cfg(not(unix))]
fn host_vector_dispatch_lock_guard() -> Result<HostVectorDispatchLock, String> {
    static HOST_VECTOR_DISPATCH_MUTEX: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let thread_guard = HOST_VECTOR_DISPATCH_MUTEX
        .lock()
        .map_err(|_| "simpler_capi_dispatch_lock_poisoned".to_string())?;
    Ok(HostVectorDispatchLock {
        _thread_guard: thread_guard,
    })
}

#[cfg(all(unix, not(test)))]
fn with_suppressed_stdio<T>(f: impl FnOnce() -> Result<T, String>) -> Result<T, String> {
    f()
}

#[cfg(any(not(unix), test))]
fn with_suppressed_stdio<T>(f: impl FnOnce() -> Result<T, String>) -> Result<T, String> {
    f()
}

fn simpler_manifest_path() -> Result<PathBuf, String> {
    let path = std::env::var("SIMPLER_HOST_VECTOR_MANIFEST").unwrap_or_else(|_| {
        "/private/tmp/simpler-host-vector-artifacts/host_vector_manifest.json".to_string()
    });
    let path = PathBuf::from(path);
    if !path.exists() {
        return Err(format!(
            "missing_simpler_host_vector_manifest:{}",
            path.display()
        ));
    }
    Ok(path)
}

fn simpler_matmul_manifest_path() -> Result<PathBuf, String> {
    let path = std::env::var("SIMPLER_HOST_MATMUL_MANIFEST").unwrap_or_else(|_| {
        "/private/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json".to_string()
    });
    let path = PathBuf::from(path);
    if !path.exists() {
        return Err(format!(
            "missing_simpler_host_matmul_manifest:{}",
            path.display()
        ));
    }
    Ok(path)
}

fn scenario_config_for_chipbackend() -> Result<ScenarioConfig, String> {
    let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
    let default_path = manifest_dir
        .join("../../scenarios/mvp_2host_single_domain.yaml")
        .canonicalize()
        .unwrap_or_else(|_| PathBuf::from("scenarios/mvp_2host_single_domain.yaml"));
    let path = std::env::var("SIM_UAPI_SCENARIO_CONFIG")
        .map(PathBuf::from)
        .unwrap_or(default_path);
    let yaml = std::fs::read_to_string(&path)
        .map_err(|err| format!("read_scenario_config_failed:{}:{err}", path.display()))?;
    let yaml = yaml.replace("chip_backend_mode: stub", "chip_backend_mode: simpler_capi");
    ScenarioConfig::from_yaml_str(&yaml)
        .map_err(|err| format!("parse_scenario_config_failed:{}:{err}", path.display()))
}

fn host_vector_backend_spec_from_manifest(
    manifest_path: &Path,
    input_a: MemoryEndpoint,
    input_b: MemoryEndpoint,
    output_f: MemoryEndpoint,
    size_bytes: u64,
    elems: u64,
) -> Result<DispatchBackendSpec, String> {
    let manifest_text = std::fs::read_to_string(manifest_path).map_err(|err| {
        format!(
            "read_simpler_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let manifest: SimplerRuntimeManifestEnvelope =
        serde_json::from_str(&manifest_text).map_err(|err| {
            format!(
                "parse_simpler_manifest_failed:{}:{err}",
                manifest_path.display()
            )
        })?;
    let args = vec![
        SimplerRuntimeArg::InputSegment {
            endpoint: input_a,
            bytes: size_bytes,
        },
        SimplerRuntimeArg::InputSegment {
            endpoint: input_b,
            bytes: size_bytes,
        },
        SimplerRuntimeArg::OutputSegment {
            endpoint: output_f,
            bytes: size_bytes,
        },
        SimplerRuntimeArg::ScalarU64(size_bytes),
        SimplerRuntimeArg::ScalarU64(size_bytes),
        SimplerRuntimeArg::ScalarU64(size_bytes),
        SimplerRuntimeArg::ScalarU64(elems),
    ];
    let runtime = SimplerRuntimeArtifacts {
        host_runtime_library: manifest.simpler_runtime.host_runtime_library,
        orch_shared_object: manifest.simpler_runtime.orch_shared_object,
        orch_function_name: manifest.simpler_runtime.orch_function_name,
        aicpu_binary: manifest.simpler_runtime.aicpu_binary,
        aicore_binary: manifest.simpler_runtime.aicore_binary,
        kernels: manifest.simpler_runtime.kernels,
        launch: manifest.simpler_runtime.launch,
        runtime_env: manifest.simpler_runtime.runtime_env,
        args,
    };
    Ok(DispatchBackendSpec {
        profile: DispatchBackendProfile::HostVector,
        platform: "a2a3sim".to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_vector_example".to_string()),
        simpler_runtime: Some(runtime),
        context: None,
    })
}

fn host_matmul_backend_spec_from_manifest(
    manifest_path: &Path,
    input_a: MemoryEndpoint,
    input_w1: MemoryEndpoint,
    input_w2: MemoryEndpoint,
    output_f: MemoryEndpoint,
    input_bytes: u64,
    output_bytes: u64,
    elems: u64,
) -> Result<DispatchBackendSpec, String> {
    let manifest_text = std::fs::read_to_string(manifest_path).map_err(|err| {
        format!(
            "read_simpler_matmul_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let manifest: SimplerRuntimeManifestEnvelope =
        serde_json::from_str(&manifest_text).map_err(|err| {
            format!(
                "parse_simpler_matmul_manifest_failed:{}:{err}",
                manifest_path.display()
            )
        })?;
    let args = vec![
        SimplerRuntimeArg::InputSegment {
            endpoint: input_a,
            bytes: input_bytes,
        },
        SimplerRuntimeArg::InputSegment {
            endpoint: input_w1,
            bytes: input_bytes,
        },
        SimplerRuntimeArg::InputSegment {
            endpoint: input_w2,
            bytes: input_bytes,
        },
        SimplerRuntimeArg::OutputSegment {
            endpoint: output_f,
            bytes: output_bytes,
        },
        SimplerRuntimeArg::ScalarU64(input_bytes),
        SimplerRuntimeArg::ScalarU64(input_bytes),
        SimplerRuntimeArg::ScalarU64(input_bytes),
        SimplerRuntimeArg::ScalarU64(output_bytes),
        SimplerRuntimeArg::ScalarU64(elems),
    ];
    let runtime = SimplerRuntimeArtifacts {
        host_runtime_library: manifest.simpler_runtime.host_runtime_library,
        orch_shared_object: manifest.simpler_runtime.orch_shared_object,
        orch_function_name: manifest.simpler_runtime.orch_function_name,
        aicpu_binary: manifest.simpler_runtime.aicpu_binary,
        aicore_binary: manifest.simpler_runtime.aicore_binary,
        kernels: manifest.simpler_runtime.kernels,
        launch: manifest.simpler_runtime.launch,
        runtime_env: manifest.simpler_runtime.runtime_env,
        args,
    };
    Ok(DispatchBackendSpec {
        profile: DispatchBackendProfile::HostMatmul,
        platform: "a2a3sim".to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_matmul_example".to_string()),
        simpler_runtime: Some(runtime),
        context: None,
    })
}

fn kvcache_host_vector_request(
    task_id: u64,
    layout: &KvCachePayloadLayout,
    chunk_index: usize,
    chunk_offset: usize,
    bindings: Vec<DispatchBufferBinding>,
) -> BackendExecutionRequest {
    BackendExecutionRequest {
        correlation: RequestCorrelation {
            request_id: format!(
                "uapi-kvcache-host-vector-dispatch-{task_id}-chunk{chunk_index}-offset{chunk_offset}-blocks{}-groups{}-tile{}x{}-rowgroup{}x{}",
                layout.blocks.len(),
                W4_KVCACHE_PREFIX_GROUPS,
                W4_KVCACHE_TILE_ROWS,
                W4_KVCACHE_TILE_COLS,
                W4_KVCACHE_ROW_GROUP_ROWS,
                W4_KVCACHE_TILE_COLS
            ),
            trace_id: Some(format!(
                "w4-kvcache-layout-bytes{}-elems{}",
                layout.bytes, layout.elems
            )),
            op_name: Some("w4_kvcache_host_vector_example".to_string()),
            step_index: Some(0),
            sequence_no: Some(task_id),
        },
        plan: None,
        context: Some(ExecutionContextRef {
            device_context_id: "device-ctx-uapi-kvcache-host-vector".to_string(),
            runtime_context_id: Some(format!("runtime-ctx-uapi-kvcache-{task_id}")),
            lifecycle: ExecutionLifecycle::Init,
            warm: true,
            reusable: true,
        }),
        bindings,
    }
}

fn kvcache_host_matmul_request(
    task_id: u64,
    bindings: Vec<DispatchBufferBinding>,
) -> BackendExecutionRequest {
    BackendExecutionRequest {
        correlation: RequestCorrelation {
            request_id: format!("uapi-kvcache-host-matmul-dispatch-{task_id}"),
            trace_id: Some("w4-kvcache-host-matmul-128x128".to_string()),
            op_name: Some("w4_kvcache_host_matmul_example".to_string()),
            step_index: Some(0),
            sequence_no: Some(task_id),
        },
        plan: None,
        context: Some(ExecutionContextRef {
            device_context_id: "device-ctx-uapi-kvcache-host-matmul".to_string(),
            runtime_context_id: Some(format!("runtime-ctx-uapi-kvcache-matmul-{task_id}")),
            lifecycle: ExecutionLifecycle::Init,
            warm: true,
            reusable: true,
        }),
        bindings,
    }
}

#[derive(Clone, Copy, Debug)]
enum Qwen3ProjectionKind {
    Q,
    Kv,
    V,
}

fn qwen3_dense_0_6b_request(
    task_id: u64,
    profile: Qwen3Dense06bProfile,
    shard: Qwen3Dense06bShard,
    bindings: Vec<DispatchBufferBinding>,
) -> BackendExecutionRequest {
    BackendExecutionRequest {
        correlation: RequestCorrelation {
            request_id: format!(
                "uapi-qwen3-dense-0-6b-prefill-dispatch-{task_id}-shard{}-node{}-heads{}-{}",
                shard.shard_id, shard.owner_node, shard.head_start, shard.head_end
            ),
            trace_id: Some(format!(
                "w4-qwen3-dense-0-6b-layers{}-hidden{}-heads{}-kvheads{}-headdim{}-prefill{}-decode{}-tp{}-shard{}-kvblocks{}-{}",
                profile.num_hidden_layers,
                profile.hidden_size,
                profile.num_attention_heads,
                profile.num_key_value_heads,
                profile.head_dim,
                profile.prefill_tokens,
                profile.decode_tokens,
                profile.tp_nodes,
                shard.shard_id,
                shard.kv_block_start,
                shard.kv_block_end
            )),
            op_name: Some("w4_qwen3_dense_0_6b_prefill_matmul".to_string()),
            step_index: Some(shard.shard_id as u32),
            sequence_no: Some(task_id + shard.shard_id),
        },
        plan: None,
        context: Some(ExecutionContextRef {
            device_context_id: format!(
                "device-ctx-uapi-qwen3-dense-0-6b-node{}-shard{}",
                shard.owner_node, shard.shard_id
            ),
            runtime_context_id: Some(format!(
                "runtime-ctx-uapi-qwen3-dense-0-6b-{task_id}-shard{}",
                shard.shard_id
            )),
            lifecycle: ExecutionLifecycle::Init,
            warm: true,
            reusable: true,
        }),
        bindings,
    }
}

fn run_host_matmul_smoke(topology: &SimTopology, task: &TaskKey) -> Result<Vec<u8>, String> {
    const MATMUL_DIM: usize = 128;
    const MATMUL_ELEMS: usize = MATMUL_DIM * MATMUL_DIM;
    const HALF_ONE: u16 = 0x3c00;

    let manifest_path = simpler_matmul_manifest_path()?;
    let scenario_config = scenario_config_for_chipbackend()?;
    let segment_base = 20_000 + task.task_id.saturating_mul(10);
    let input_a = SegmentHandle(segment_base + 1);
    let input_w1 = SegmentHandle(segment_base + 2);
    let input_w2 = SegmentHandle(segment_base + 3);
    let output_f = SegmentHandle(segment_base + 4);
    let input_bytes = MATMUL_ELEMS * std::mem::size_of::<u16>();
    let output_bytes = MATMUL_ELEMS * std::mem::size_of::<f32>();
    let host_node = topology
        .hosts
        .first()
        .map(|host| host.node_id)
        .ok_or_else(|| "missing_host_node".to_string())?;
    let ubpu_node = topology
        .ubpus
        .first()
        .map(|ubpu| ubpu.node_id)
        .ok_or_else(|| "missing_ubpu_node".to_string())?;
    let _dispatch_lock = host_vector_dispatch_lock_guard()?;
    let mut runtime = LocalRuntimeEngine::from_config(&scenario_config);
    runtime.seed_host_segment(host_node, input_a, repeated_u16_le_bytes(HALF_ONE, MATMUL_ELEMS));
    runtime.seed_host_segment(host_node, input_w1, repeated_u16_le_bytes(HALF_ONE, MATMUL_ELEMS));
    runtime.seed_host_segment(host_node, input_w2, repeated_u16_le_bytes(HALF_ONE, MATMUL_ELEMS));
    runtime.seed_host_segment(host_node, output_f, vec![0u8; output_bytes]);

    let backend_spec = host_matmul_backend_spec_from_manifest(
        &manifest_path,
        MemoryEndpoint {
            node: host_node,
            segment: input_a,
            offset: 0,
        },
        MemoryEndpoint {
            node: host_node,
            segment: input_w1,
            offset: 0,
        },
        MemoryEndpoint {
            node: host_node,
            segment: input_w2,
            offset: 0,
        },
        MemoryEndpoint {
            node: host_node,
            segment: output_f,
            offset: 0,
        },
        input_bytes as u64,
        output_bytes as u64,
        MATMUL_ELEMS as u64,
    )?;
    let request = kvcache_host_matmul_request(
        task.task_id,
        vec![
            opaque_binding(
                "kvcache_matmul_a_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_a,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            opaque_binding(
                "kvcache_matmul_w1_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_w1,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            opaque_binding(
                "kvcache_matmul_w2_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_w2,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            dense_f32_binding(
                "kvcache_matmul_output_f",
                BufferUsage::Output,
                MemoryEndpoint {
                    node: host_node,
                    segment: output_f,
                    offset: 0,
                },
                MATMUL_ELEMS as u64,
            ),
        ],
    );
    let dispatch = BackendDispatchOperation {
        task: task.clone(),
        function: FunctionLabel {
            name: "host_matmul_example".into(),
            level: PlLevel::L2,
        },
        backend_spec,
        request,
        target_level: PlLevel::L2,
        target_node: ubpu_node,
        legacy_input_segments: vec![input_a, input_w1, input_w2],
    };
    let mut sink = VecEventSink::default();
    let complete_at = scenario_config
        .pypto
        .simpler_boundary
        .dispatch_latency_us
        .unwrap_or(15);
    let completion = with_suppressed_stdio(|| {
        runtime
            .submit_backend_dispatch(dispatch, &mut sink)
            .map_err(|err| err.to_string())?;
        runtime.advance_to(complete_at, &mut sink);
        runtime
            .poll_completions(complete_at, &mut sink)
            .into_iter()
            .next()
            .ok_or_else(|| "simpler_capi_matmul_dispatch_did_not_complete".to_string())
    })?;
    match completion.status {
        CompletionStatus::Success => {}
        other => return Err(format!("simpler_capi_matmul_dispatch_failed:{other:?}")),
    }
    let produced = runtime
        .host_segment_payload(host_node, output_f)
        .ok_or_else(|| "missing_host_matmul_output_payload".to_string())?;
    let output_values = bytes_to_f32s(produced);
    if output_values.len() != MATMUL_ELEMS {
        return Err(format!(
            "simpler_capi_matmul_output_len_mismatch:got={}:expected={}",
            output_values.len(),
            MATMUL_ELEMS
        ));
    }
    if output_values
        .iter()
        .any(|value| !value.is_finite() || *value <= 0.0)
    {
        return Err(format!(
            "simpler_capi_matmul_output_invalid:first={:?}",
            output_values.first()
        ));
    }
    Ok(produced.to_vec())
}

fn run_qwen3_dense_0_6b_prefill_smoke(
    topology: &SimTopology,
    task: &TaskKey,
    guest_input: &[u8],
) -> Result<Vec<u8>, String> {
    const MATMUL_DIM: usize = 128;
    const MATMUL_ELEMS: usize = MATMUL_DIM * MATMUL_DIM;

    let profile = QWEN3_DENSE_0_6B_PROFILE;
    let manifest_path = simpler_matmul_manifest_path()?;
    let scenario_config = scenario_config_for_chipbackend()?;
    let tp_plan = qwen3_dense_0_6b::tensor_parallel_plan(topology, profile)?;
    let shard_plan: Vec<Qwen3Dense06bShard> = tp_plan
        .iter()
        .map(|tp_shard| Qwen3Dense06bShard {
            shard_id: tp_shard.shard_id,
            owner_node: tp_shard.owner_node,
            target_node: tp_shard.target_node,
            head_start: tp_shard.q_head_start,
            head_end: tp_shard.q_head_end,
            kv_block_start: tp_shard.shard_id * 2,
            kv_block_end: tp_shard.shard_id * 2 + 2,
        })
        .collect();
    let segment_base = 30_000 + task.task_id.saturating_mul(100);
    let model_meta = SegmentHandle(segment_base + 80);
    let kv_layout = SegmentHandle(segment_base + 81);
    let guest_kvcache_payload = SegmentHandle(segment_base + 82);
    let input_bytes = MATMUL_ELEMS * std::mem::size_of::<u16>();
    let output_bytes = MATMUL_ELEMS * std::mem::size_of::<f32>();
    let host_node = topology
        .hosts
        .first()
        .map(|host| host.node_id)
        .ok_or_else(|| "missing_host_node".to_string())?;
    let _dispatch_lock = host_vector_dispatch_lock_guard()?;
    let mut runtime = LocalRuntimeEngine::from_config(&scenario_config);
    runtime.seed_host_segment(host_node, model_meta, qwen3_dense_0_6b_model_meta_payload(profile));
    runtime.seed_host_segment(host_node, kv_layout, qwen3_dense_0_6b_kv_layout_payload(profile));
    runtime.seed_host_segment(
        host_node,
        guest_kvcache_payload,
        qwen3_dense_0_6b_guest_kvcache_payload(guest_input),
    );

    let mut sink = VecEventSink::default();
    let dispatch_latency = scenario_config
        .pypto
        .simpler_boundary
        .dispatch_latency_us
        .unwrap_or(15);
    let mut runtime_time = 0;
    let mut produced = Vec::with_capacity(output_bytes * shard_plan.len());
    let mut shard_checksums = Vec::with_capacity(shard_plan.len());
    let mut weights_service = WeightsServiceStub::new();
    let mut round0_outputs = Vec::with_capacity(shard_plan.len());
    for shard in shard_plan.iter().copied() {
        let shard_base = segment_base + shard.shard_id.saturating_mul(10);
        let input_a = SegmentHandle(shard_base + 1);
        let input_q = SegmentHandle(shard_base + 2);
        let input_kv = SegmentHandle(shard_base + 3);
        let input_v = SegmentHandle(shard_base + 4);
        let output_f = SegmentHandle(shard_base + 5);

        runtime.seed_host_segment(
            host_node,
            input_a,
            qwen3_dense_0_6b_prefill_hidden_from_guest_payload(
                guest_input,
                MATMUL_ELEMS,
                shard,
            ),
        );
        runtime.seed_host_segment(
            host_node,
            input_q,
            qwen3_dense_0_6b_projection_tile_from_guest_payload(
                guest_input,
                MATMUL_ELEMS,
                Qwen3ProjectionKind::Q,
                shard,
            ),
        );
        runtime.seed_host_segment(
            host_node,
            input_kv,
            qwen3_dense_0_6b_projection_tile_from_guest_payload(
                guest_input,
                MATMUL_ELEMS,
                Qwen3ProjectionKind::Kv,
                shard,
            ),
        );
        runtime.seed_host_segment(
            host_node,
            input_v,
            qwen3_dense_0_6b_projection_tile_from_guest_payload(
                guest_input,
                MATMUL_ELEMS,
                Qwen3ProjectionKind::V,
                shard,
            ),
        );
        runtime.seed_host_segment(host_node, output_f, vec![0u8; output_bytes]);

        let backend_spec = host_matmul_backend_spec_from_manifest(
            &manifest_path,
            MemoryEndpoint {
                node: host_node,
                segment: input_a,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: input_q,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: input_kv,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: output_f,
                offset: 0,
            },
            input_bytes as u64,
            output_bytes as u64,
            MATMUL_ELEMS as u64,
        )?;
        let request = qwen3_dense_0_6b_request(
            task.task_id,
            profile,
            shard,
            vec![
            opaque_binding(
                "qwen3_dense_0_6b_layer0_prefill_hidden_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_a,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            opaque_binding(
                "qwen3_dense_0_6b_layer0_q_proj_tile_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_q,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            opaque_binding(
                "qwen3_dense_0_6b_layer0_kv_proj_tile_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_kv,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            opaque_resident_binding(
                "qwen3_dense_0_6b_layer0_v_proj_tile_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_v,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            dense_f32_binding(
                "qwen3_dense_0_6b_layer0_prefill_projection_f",
                BufferUsage::Output,
                MemoryEndpoint {
                    node: host_node,
                    segment: output_f,
                    offset: 0,
                },
                MATMUL_ELEMS as u64,
            ),
            opaque_resident_binding(
                "qwen3_dense_0_6b_model_config",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: model_meta,
                    offset: 0,
                },
                96,
            ),
            opaque_resident_binding(
                "qwen3_dense_0_6b_kvcache_layout",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: kv_layout,
                    offset: 0,
                },
                96,
            ),
            opaque_resident_binding(
                "qwen3_dense_0_6b_guest_kvcache_payload",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: guest_kvcache_payload,
                    offset: 0,
                },
                W4_KVCACHE_PAYLOAD_BYTES as u64,
            ),
            ],
        );
        let dispatch = BackendDispatchOperation {
            task: TaskKey {
                task_id: task.task_id + shard.shard_id,
                ..task.clone()
            },
            function: FunctionLabel {
                name: format!(
                    "qwen3_dense_0_6b_prefill_matmul_shard{}",
                    shard.shard_id
                ),
                level: PlLevel::L2,
            },
            backend_spec,
            request,
            target_level: PlLevel::L2,
            target_node: shard.target_node,
            legacy_input_segments: vec![input_a, input_q, input_kv, input_v],
        };
        with_suppressed_stdio(|| {
            runtime
                .submit_backend_dispatch(dispatch, &mut sink)
                .map_err(|err| err.to_string())?;
            runtime_time += dispatch_latency;
            runtime.advance_to(runtime_time, &mut sink);
            let completions = runtime.poll_completions(runtime_time, &mut sink);
            if completions.len() != 1 {
                return Err(format!(
                    "simpler_capi_qwen3_dense_0_6b_shard_completion_count_mismatch:shard={}:got={}:expected=1",
                    shard.shard_id,
                    completions.len()
                ));
            }
            match &completions[0].status {
                CompletionStatus::Success => Ok(()),
                other => Err(format!(
                    "simpler_capi_qwen3_dense_0_6b_shard_dispatch_failed:shard={}:status={other:?}",
                    shard.shard_id
                )),
            }
        })?;

        let shard_output = runtime
            .host_segment_payload(host_node, output_f)
            .ok_or_else(|| {
                format!(
                    "missing_qwen3_dense_0_6b_shard_output_payload:{}",
                    shard.shard_id
                )
            })?;
        let output_values = bytes_to_f32s(shard_output);
        if output_values.len() != MATMUL_ELEMS {
            return Err(format!(
                "qwen3_dense_0_6b_shard_output_len_mismatch:shard={}:got={}:expected={}",
                shard.shard_id,
                output_values.len(),
                MATMUL_ELEMS
            ));
        }
        if output_values
            .iter()
            .any(|value| !value.is_finite() || *value <= 0.0)
        {
            return Err(format!(
                "qwen3_dense_0_6b_shard_output_invalid:shard={}:first={:?}",
                shard.shard_id,
                output_values.first()
            ));
        }
        let checksum = qwen3_dense_0_6b_shard_output_checksum(shard_output);
        let publish_stats = weights_service
            .submit_publish_object(
                ServiceObjectPublishReq {
                    task: Some(TaskKey {
                        task_id: task.task_id + shard.shard_id,
                        ..task.clone()
                    }),
                    requester_entity: shard.shard_id as u32,
                    metadata_puts: vec![ServiceObjectMetadataPut {
                        key: qwen3_dense_0_6b_partial_result_key(0, shard.shard_id),
                        object_kind: ServiceObjectKind::PartialResultTile,
                        bytes: 192,
                    }],
                    payload_writes: vec![ServiceObjectPayloadWrite {
                        storage_ref: qwen3_dense_0_6b_partial_result_storage_ref(0, shard.shard_id),
                        object_kind: ServiceObjectKind::PartialResultTile,
                        storage_kind: WeightStorageKind::Block,
                        segment: output_f,
                        offset: 0,
                        bytes: output_bytes as u64,
                        checksum,
                        producer_entity: shard.shard_id as u32,
                    }],
                },
                runtime_time,
            )
            .map_err(|err| format!("qwen3_dense_0_6b_partial_result_publish_failed:{err:?}"))?;
        if publish_stats.metadata_puts != 1
            || publish_stats.block_writes != 1
            || publish_stats.payload_bytes != output_bytes as u64
        {
            return Err(format!(
                "qwen3_dense_0_6b_partial_result_publish_stats_invalid:shard={}:stats={publish_stats:?}",
                shard.shard_id
            ));
        }
        shard_checksums.push(checksum);
        round0_outputs.push((shard, shard_output.to_vec(), output_f, checksum));
        produced.extend_from_slice(shard_output);
    }
    if shard_checksums.len() != shard_plan.len() {
        return Err(format!(
            "qwen3_dense_0_6b_shard_summary_count_mismatch:got={}:expected={}",
            shard_checksums.len(),
            shard_plan.len()
        ));
    }
    if !shard_checksums.windows(2).any(|pair| pair[0] != pair[1]) {
        return Err(format!(
            "qwen3_dense_0_6b_shard_outputs_not_distinct:shards={}:checksum={:#x}",
            shard_checksums.len(),
            shard_checksums.first().copied().unwrap_or(0)
        ));
    }
    let mut round1_checksums = Vec::with_capacity(round0_outputs.len());
    for (index, (shard, _round0_output, _round0_segment, _round0_checksum)) in
        round0_outputs.iter().enumerate()
    {
        let remote_index = (index + round0_outputs.len() - 1) % round0_outputs.len();
        let (remote_shard, remote_output, _remote_segment, _remote_checksum) =
            &round0_outputs[remote_index];
        let resolve_stats = weights_service
            .submit_resolve_object(
                ServiceObjectResolveReq {
                    task: Some(TaskKey {
                        task_id: task.task_id + 10_000 + shard.shard_id,
                        ..task.clone()
                    }),
                    requester_entity: shard.shard_id as u32,
                    metadata_key: qwen3_dense_0_6b_partial_result_key(0, remote_shard.shard_id),
                    object_kind: ServiceObjectKind::PartialResultTile,
                    storage_ref: qwen3_dense_0_6b_partial_result_storage_ref(
                        0,
                        remote_shard.shard_id,
                    ),
                    storage_kind: WeightStorageKind::Block,
                    segment: SegmentHandle(segment_base + 900 + remote_shard.shard_id),
                    bytes: output_bytes as u64,
                },
                runtime_time,
            )
            .map_err(|err| format!("qwen3_dense_0_6b_partial_result_resolve_failed:{err:?}"))?;
        if resolve_stats.metadata_gets != 1 || resolve_stats.block_reads != 1 {
            return Err(format!(
                "qwen3_dense_0_6b_partial_result_resolve_stats_invalid:shard={}:remote_shard={}:stats={resolve_stats:?}",
                shard.shard_id, remote_shard.shard_id
            ));
        }

        let shard_base = segment_base + 2_000 + shard.shard_id.saturating_mul(10);
        let input_a = SegmentHandle(shard_base + 1);
        let input_q = SegmentHandle(shard_base + 2);
        let input_kv = SegmentHandle(shard_base + 3);
        let output_f = SegmentHandle(shard_base + 4);
        runtime.seed_host_segment(
            host_node,
            input_a,
            qwen3_dense_0_6b_remote_partial_to_half_input(remote_output, MATMUL_ELEMS),
        );
        runtime.seed_host_segment(
            host_node,
            input_q,
            qwen3_dense_0_6b_projection_tile_from_guest_payload(
                guest_input,
                MATMUL_ELEMS,
                Qwen3ProjectionKind::Q,
                *shard,
            ),
        );
        runtime.seed_host_segment(
            host_node,
            input_kv,
            qwen3_dense_0_6b_projection_tile_from_guest_payload(
                guest_input,
                MATMUL_ELEMS,
                Qwen3ProjectionKind::Kv,
                *shard,
            ),
        );
        runtime.seed_host_segment(host_node, output_f, vec![0u8; output_bytes]);
        let backend_spec = host_matmul_backend_spec_from_manifest(
            &manifest_path,
            MemoryEndpoint {
                node: host_node,
                segment: input_a,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: input_q,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: input_kv,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: output_f,
                offset: 0,
            },
            input_bytes as u64,
            output_bytes as u64,
            MATMUL_ELEMS as u64,
        )?;
        let request = qwen3_dense_0_6b_request(
            task.task_id + 20_000,
            profile,
            *shard,
            vec![
                opaque_binding(
                    format!(
                        "qwen3_dense_0_6b_round1_remote_partial_from_shard{}",
                        remote_shard.shard_id
                    ),
                    BufferUsage::Input,
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_a,
                        offset: 0,
                    },
                    input_bytes as u64,
                ),
                opaque_binding(
                    "qwen3_dense_0_6b_round1_q_proj_tile_half",
                    BufferUsage::Input,
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_q,
                        offset: 0,
                    },
                    input_bytes as u64,
                ),
                opaque_binding(
                    "qwen3_dense_0_6b_round1_kv_proj_tile_half",
                    BufferUsage::Input,
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_kv,
                        offset: 0,
                    },
                    input_bytes as u64,
                ),
                dense_f32_binding(
                    "qwen3_dense_0_6b_round1_remote_dependent_output_f",
                    BufferUsage::Output,
                    MemoryEndpoint {
                        node: host_node,
                        segment: output_f,
                        offset: 0,
                    },
                    MATMUL_ELEMS as u64,
                ),
            ],
        );
        let dispatch = BackendDispatchOperation {
            task: TaskKey {
                task_id: task.task_id + 20_000 + shard.shard_id,
                ..task.clone()
            },
            function: FunctionLabel {
                name: format!(
                    "qwen3_dense_0_6b_round1_remote_dependent_matmul_shard{}",
                    shard.shard_id
                ),
                level: PlLevel::L2,
            },
            backend_spec,
            request,
            target_level: PlLevel::L2,
            target_node: shard.target_node,
            legacy_input_segments: vec![input_a, input_q, input_kv],
        };
        with_suppressed_stdio(|| {
            runtime
                .submit_backend_dispatch(dispatch, &mut sink)
                .map_err(|err| err.to_string())?;
            runtime_time += dispatch_latency;
            runtime.advance_to(runtime_time, &mut sink);
            let completions = runtime.poll_completions(runtime_time, &mut sink);
            if completions.len() != 1 {
                return Err(format!(
                    "simpler_capi_qwen3_dense_0_6b_round1_completion_count_mismatch:shard={}:got={}:expected=1",
                    shard.shard_id,
                    completions.len()
                ));
            }
            match &completions[0].status {
                CompletionStatus::Success => Ok(()),
                other => Err(format!(
                    "simpler_capi_qwen3_dense_0_6b_round1_dispatch_failed:shard={}:status={other:?}",
                    shard.shard_id
                )),
            }
        })?;
        let round1_output = runtime
            .host_segment_payload(host_node, output_f)
            .ok_or_else(|| {
                format!(
                    "missing_qwen3_dense_0_6b_round1_output_payload:{}",
                    shard.shard_id
                )
            })?;
        round1_checksums.push(qwen3_dense_0_6b_shard_output_checksum(round1_output));
    }
    if round1_checksums.len() != shard_plan.len() {
        return Err(format!(
            "qwen3_dense_0_6b_round1_summary_count_mismatch:got={}:expected={}",
            round1_checksums.len(),
            shard_plan.len()
        ));
    }
    if !round1_checksums.windows(2).any(|pair| pair[0] != pair[1]) {
        return Err(format!(
            "qwen3_dense_0_6b_round1_outputs_not_distinct:shards={}:checksum={:#x}",
            round1_checksums.len(),
            round1_checksums.first().copied().unwrap_or(0)
        ));
    }
    qwen3_dense_0_6b_write_service_flow_markers(
        &mut produced,
        shard_plan.len() as u64,
        round0_outputs.len() as u64,
        round1_checksums.len() as u64,
    );
    Ok(produced)
}

fn qwen3_dense_0_6b_shard_output_checksum(bytes: &[u8]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for chunk in bytes.chunks(std::mem::size_of::<u64>()) {
        let mut word = [0u8; 8];
        word[..chunk.len()].copy_from_slice(chunk);
        acc ^= u64::from_le_bytes(word);
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn qwen3_dense_0_6b_partial_result_key(layer_id: u64, shard_id: u64) -> String {
    format!("qwen3_dense_0_6b/layer/{layer_id}/shard/{shard_id}/partial_result_tile")
}

fn qwen3_dense_0_6b_partial_result_storage_ref(layer_id: u64, shard_id: u64) -> String {
    format!("qwen3_dense_0_6b/runtime/layer/{layer_id}/shard/{shard_id}/partial_result_tile")
}

fn qwen3_dense_0_6b_remote_partial_to_half_input(bytes: &[u8], elems: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let byte_index = (elem_index * std::mem::size_of::<f32>()) % bytes.len().max(1);
        let source = bytes.get(byte_index).copied().unwrap_or(0);
        let half_bits = 0x3c00u16 + ((source & 0x07) as u16);
        out.extend_from_slice(&half_bits.to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_write_service_flow_markers(
    output: &mut [u8],
    round0_publish_count: u64,
    round1_resolve_count: u64,
    round1_compute_count: u64,
) {
    const MARKER_PUBLISH: u64 = 0x7133773470756230;
    const MARKER_RESOLVE: u64 = 0x7133773472657331;
    const MARKER_COMPUTE: u64 = 0x71337734636d7031;
    write_u64_le_at(output, 8, MARKER_PUBLISH);
    write_u64_le_at(output, 16, MARKER_RESOLVE);
    write_u64_le_at(output, 24, MARKER_COMPUTE);
    write_u64_le_at(output, 32, round0_publish_count);
    write_u64_le_at(output, 40, round1_resolve_count);
    write_u64_le_at(output, 48, round1_compute_count);
}

fn write_u64_le_at(output: &mut [u8], offset: usize, value: u64) {
    let end = offset + std::mem::size_of::<u64>();
    if end <= output.len() {
        output[offset..end].copy_from_slice(&value.to_le_bytes());
    }
}

fn qwen3_dense_0_6b_model_meta_payload(profile: Qwen3Dense06bProfile) -> Vec<u8> {
    [
        profile.vocab_size,
        profile.hidden_size,
        profile.intermediate_size,
        profile.num_hidden_layers,
        profile.num_attention_heads,
        profile.num_key_value_heads,
        profile.head_dim,
        profile.max_position_embeddings,
        profile.rope_theta,
        profile.tp_nodes,
        0,
        0,
    ]
    .into_iter()
    .flat_map(u64::to_le_bytes)
    .collect()
}

fn qwen3_dense_0_6b_kv_layout_payload(profile: Qwen3Dense06bProfile) -> Vec<u8> {
    let kv_elem_bytes = 2u64;
    let kv_bytes_per_token_per_layer = profile.num_key_value_heads * profile.head_dim * kv_elem_bytes * 2;
    [
        profile.prefill_tokens,
        profile.decode_tokens,
        profile.num_hidden_layers,
        profile.num_key_value_heads,
        profile.head_dim,
        kv_elem_bytes,
        kv_bytes_per_token_per_layer,
        kv_bytes_per_token_per_layer * profile.prefill_tokens,
        kv_bytes_per_token_per_layer * profile.prefill_tokens * profile.num_hidden_layers,
        W4_KVCACHE_PAYLOAD_BYTES as u64,
        W4_KVCACHE_BLOCKS as u64,
        W4_KVCACHE_PREFIX_GROUPS as u64,
    ]
    .into_iter()
    .flat_map(u64::to_le_bytes)
    .collect()
}

fn qwen3_dense_0_6b_guest_kvcache_payload(guest_input: &[u8]) -> Vec<u8> {
    let mut payload = vec![0u8; W4_KVCACHE_PAYLOAD_BYTES];
    let copy_len = payload.len().min(guest_input.len());
    payload[..copy_len].copy_from_slice(&guest_input[..copy_len]);
    payload
}

fn qwen3_dense_0_6b_prefill_hidden_from_guest_payload(
    guest_input: &[u8],
    elems: usize,
    shard: Qwen3Dense06bShard,
) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let source_index =
            (elem_index + shard.head_start as usize + shard.kv_block_start as usize)
                % guest_input.len().max(1);
        let source = guest_input.get(source_index).copied().unwrap_or(0);
        let half_bits = 0x3c00u16 + ((source & 0x03) as u16) + ((shard.shard_id & 0x01) as u16);
        bytes.extend_from_slice(&half_bits.to_le_bytes());
    }
    bytes
}

fn qwen3_dense_0_6b_projection_tile_from_guest_payload(
    guest_input: &[u8],
    elems: usize,
    kind: Qwen3ProjectionKind,
    shard: Qwen3Dense06bShard,
) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    let stride = match kind {
        Qwen3ProjectionKind::Q => 17usize,
        Qwen3ProjectionKind::Kv => 31usize,
        Qwen3ProjectionKind::V => 47usize,
    };
    let bias = match kind {
        Qwen3ProjectionKind::Q => 0u16,
        Qwen3ProjectionKind::Kv => 4u16,
        Qwen3ProjectionKind::V => 8u16,
    };
    for elem_index in 0..elems {
        let source_index = (elem_index
            .wrapping_mul(stride)
            .wrapping_add(shard.head_start as usize)
            .wrapping_add((shard.kv_block_start * 13) as usize))
            % guest_input.len().max(1);
        let source = guest_input.get(source_index).copied().unwrap_or(0);
        let half_bits = 0x3c00u16 + bias + ((source & 0x03) as u16) + ((shard.shard_id & 0x01) as u16);
        bytes.extend_from_slice(&half_bits.to_le_bytes());
    }
    bytes
}

fn repeated_u16_le_bytes(value: u16, count: usize) -> Vec<u8> {
    let bytes = value.to_le_bytes();
    let mut out = Vec::with_capacity(count * bytes.len());
    for _ in 0..count {
        out.extend_from_slice(&bytes);
    }
    out
}

fn dense_f32_binding(
    name: impl Into<String>,
    usage: BufferUsage,
    endpoint: MemoryEndpoint,
    elems: u64,
) -> DispatchBufferBinding {
    DispatchBufferBinding {
        name: name.into(),
        usage,
        endpoint,
        bytes: elems * std::mem::size_of::<f32>() as u64,
        dtype: TensorDType::F32,
        shape: vec![elems],
        layout: TensorLayout::Contiguous,
        strides: None,
        resident: false,
    }
}

fn opaque_resident_binding(
    name: impl Into<String>,
    usage: BufferUsage,
    endpoint: MemoryEndpoint,
    bytes: u64,
) -> DispatchBufferBinding {
    DispatchBufferBinding {
        name: name.into(),
        usage,
        endpoint,
        bytes,
        dtype: TensorDType::Opaque,
        shape: vec![bytes],
        layout: TensorLayout::Opaque,
        strides: None,
        resident: true,
    }
}

fn opaque_binding(
    name: impl Into<String>,
    usage: BufferUsage,
    endpoint: MemoryEndpoint,
    bytes: u64,
) -> DispatchBufferBinding {
    DispatchBufferBinding {
        name: name.into(),
        usage,
        endpoint,
        bytes,
        dtype: TensorDType::Opaque,
        shape: vec![bytes],
        layout: TensorLayout::Opaque,
        strides: None,
        resident: false,
    }
}

fn f32s_to_bytes(values: &[f32]) -> Vec<u8> {
    values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect()
}

fn bytes_to_f32s(bytes: &[u8]) -> Vec<f32> {
    bytes
        .chunks_exact(std::mem::size_of::<f32>())
        .map(|chunk| f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect()
}

fn runtime_kind_for_descriptor(desc: &UapiDescriptor) -> RuntimeWorkKind {
    match desc {
        UapiDescriptor::Io(_) => RuntimeWorkKind::GuestIo,
        UapiDescriptor::BlockWriteback { .. } => RuntimeWorkKind::BlockWriteback,
        UapiDescriptor::ShmemPut(_) => RuntimeWorkKind::ShmemPut,
        UapiDescriptor::ShmemGet(_) => RuntimeWorkKind::ShmemGet,
        UapiDescriptor::DfsRead(_) => RuntimeWorkKind::DfsRead,
        UapiDescriptor::DfsWrite(_) => RuntimeWorkKind::DfsWrite,
        UapiDescriptor::DbPut(_) => RuntimeWorkKind::DbPut,
        UapiDescriptor::DbGet(_) => RuntimeWorkKind::DbGet,
    }
}

fn runtime_task_for_descriptor(desc: &UapiDescriptor) -> Option<TaskKey> {
    match desc {
        UapiDescriptor::Io(req) => req.task.clone(),
        UapiDescriptor::BlockWriteback { task, .. } => task.clone(),
        UapiDescriptor::ShmemPut(req) => req.task.clone(),
        UapiDescriptor::ShmemGet(req) => req.task.clone(),
        UapiDescriptor::DfsRead(req) => req.task.clone(),
        UapiDescriptor::DfsWrite(req) => req.task.clone(),
        UapiDescriptor::DbPut(req) => req.task.clone(),
        UapiDescriptor::DbGet(req) => req.task.clone(),
    }
}

#[cfg(test)]
mod tests {
    use super::{
        bytes_to_f32s, kvcache_input_b_payload,
        run_host_matmul_smoke, run_qwen3_dense_0_6b_prefill_smoke, GuestUapiSurface,
        KvCachePayloadLayout, QWEN3_DENSE_0_6B_PROFILE,
        LocalGuestUapiSurface, UapiCommand, UapiDescriptor, UapiResponse, W4_KVCACHE_BLOCKS,
        W4_KVCACHE_PAYLOAD_BYTES, W4_KVCACHE_PREFIX_GROUPS,
    };
    use sim_config::ScenarioConfig;
    use sim_core::{
        BlockHash, CompletionStatus, HierarchyCoord, IoOpcode, IoSubmitReq, LogicalSystemId,
        SimError, TaskKey,
    };
    use sim_services::block::BlockServiceProfile;
    use sim_services::{
        db::{DbGetReq, DbPutReq, DbServiceProfile},
        dfs::{DfsReadReq, DfsServiceProfile, DfsWriteReq},
        shmem::{ShmemGetReq, ShmemPutReq, ShmemServiceProfile},
    };
    use sim_topology::SimTopology;

    #[test]
    fn kvcache_payload_layout_explicitly_maps_blocks_tiles_and_row_groups() {
        let elems = W4_KVCACHE_PAYLOAD_BYTES / std::mem::size_of::<f32>();
        let layout = KvCachePayloadLayout::new(elems, W4_KVCACHE_PAYLOAD_BYTES).unwrap();

        assert_eq!(layout.blocks.len(), W4_KVCACHE_BLOCKS);
        assert_eq!(layout.blocks[0].prefix_group_id, 0);
        assert_eq!(layout.blocks[1].prefix_group_id, 1);
        assert_eq!(layout.blocks[2].prefix_group_id, 0);
        assert_eq!(layout.blocks[3].prefix_group_id, 1);
        assert_eq!(W4_KVCACHE_PREFIX_GROUPS, 2);
        assert_eq!(layout.blocks[0].tiles.len(), 2);
        assert_eq!(layout.blocks[0].tiles[0].rows, 16);
        assert_eq!(layout.blocks[0].tiles[0].cols, 16);
        assert_eq!(layout.blocks[0].tiles[0].row_groups.len(), 4);
        assert_eq!(layout.blocks[0].tiles[0].row_groups[0].rows, 4);
        assert_eq!(layout.blocks[0].tiles[0].row_groups[0].cols, 16);
        assert!(layout.tile_row_group_for_elem(0).is_some());
        assert!(layout.tile_row_group_for_elem(elems - 1).is_some());
        assert!(layout.tile_row_group_for_elem(elems).is_none());
    }

    #[test]
    fn kvcache_input_b_encodes_prefix_block_tile_and_row_group_bias() {
        let elems = W4_KVCACHE_PAYLOAD_BYTES / std::mem::size_of::<f32>();
        let layout = KvCachePayloadLayout::new(elems, W4_KVCACHE_PAYLOAD_BYTES).unwrap();
        let values = bytes_to_f32s(&kvcache_input_b_payload(&layout));

        assert_eq!(values.len(), elems);
        assert!(values[0] < values[64]);
        assert!(values[64] < values[256]);
        assert!(values[512] > values[0]);
        assert!(values[1024] > values[0]);
        assert!(values[1536] > values[1024]);
    }

    #[test]
    fn host_matmul_dispatch_accepts_manifest_artifact() {
        let topology = test_topology();
        let output = run_host_matmul_smoke(
            &topology,
            &TaskKey {
                logical_system: LogicalSystemId(1),
                coord: HierarchyCoord { levels: [0; 8] },
                scope_depth: 0,
                task_id: 99,
            },
        )
        .expect("host matmul dispatch");
        assert_eq!(output.len(), 128 * 128 * std::mem::size_of::<f32>());
    }

    #[test]
    fn qwen3_dense_0_6b_prefill_profile_uses_host_matmul_artifact() {
        let topology = test_topology();
        let output = run_qwen3_dense_0_6b_prefill_smoke(
            &topology,
            &TaskKey {
                logical_system: LogicalSystemId(1),
                coord: HierarchyCoord { levels: [0; 8] },
                scope_depth: 0,
                task_id: 100,
            },
            &[0xa5; W4_KVCACHE_PAYLOAD_BYTES],
        )
        .expect("qwen3 dense 0.6b shard-aware prefill dispatch");
        let values = bytes_to_f32s(&output);
        assert_eq!(
            values.len(),
            QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize * 128 * 128
        );
        let shard_elems = 128 * 128;
        let mut shard_firsts = Vec::new();
        for shard in 0..QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize {
            let first = values[shard * shard_elems];
            assert!(first.is_finite(), "shard {shard} first output is not finite");
            assert!(first > 1.0, "shard {shard} first output is not positive enough");
            shard_firsts.push(first.to_bits());
        }
        assert!(
            shard_firsts.windows(2).any(|pair| pair[0] != pair[1]),
            "qwen3 shard-aware dispatch produced identical first outputs for all shards"
        );
        assert_eq!(
            u64::from_le_bytes(output[8..16].try_into().expect("publish marker")),
            0x7133773470756230
        );
        assert_eq!(
            u64::from_le_bytes(output[16..24].try_into().expect("resolve marker")),
            0x7133773472657331
        );
        assert_eq!(
            u64::from_le_bytes(output[24..32].try_into().expect("compute marker")),
            0x71337734636d7031
        );
        assert_eq!(
            u64::from_le_bytes(output[32..40].try_into().expect("publish count")),
            8
        );
        assert_eq!(
            u64::from_le_bytes(output[40..48].try_into().expect("resolve count")),
            8
        );
        assert_eq!(
            u64::from_le_bytes(output[48..56].try_into().expect("compute count")),
            8
        );
    }

    const VALID_YAML: &str = r#"
scenario:
  name: mvp_2host_single_domain
  group: M
  variant: m_single_domain_mvp
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
  ubpus_per_host: 2
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
      - uba: ssu0
        blocks: 1048576
        block_size: 4096
  dfs:
    enabled: true
    namespace_root: /
    metadata_latency_us: 20
    data_latency_us: 80
  db:
    enabled: true
    inline_value_limit: 64
    pipeline_batch_limit: 16
levels:
  l2_ubpu_tier:
    capacity_blocks: 1024
    high_watermark: 0.9
    low_watermark: 0.7
    hit_latency_us: 5
  l3_host_tier:
    capacity_blocks: 8192
    high_watermark: 0.9
    low_watermark: 0.7
    fetch_latency_us: 30
  l4_domain_tier:
    capacity_blocks: 65536
    high_watermark: 0.95
    low_watermark: 0.8
    fetch_latency_us: 80
routing:
  mode: recursive
  hit_weight: 10.0
  load_weight: 2.0
  capacity_weight: 1.0
workload:
  type: rust_llm_server_mvp
  profile: single_domain_basic
  qps: 2000
  unique_prefixes: 256
  blocks_per_request: 4
  function_label_mode: host_orchestration
faults:
  - type: host_degraded
    at_us: 300000
    host_id: 0
outputs:
  trace: true
  metrics_csv: true
  summary_json: true
  emit_task_coord_trace: true
  emit_data_service_trace: true
  emit_qemu_platform_trace: true
"#;

    fn test_topology() -> SimTopology {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        SimTopology::from_config(&config).expect("topology")
    }

    fn test_surface() -> LocalGuestUapiSurface {
        LocalGuestUapiSurface::new(test_topology())
    }

    #[test]
    fn local_guest_uapi_can_submit_write_and_drain_completion() {
        let mut surface = test_surface();
        let cq = surface.register_cq().expect("register cq");

        surface
            .submit_io(IoSubmitReq {
                op_id: 11,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: None,
                block: Some(BlockHash("block-1".into())),
            })
            .expect("submit write");

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].status, CompletionStatus::Success);
    }

    #[test]
    fn local_guest_uapi_command_model_executes_round_trip() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };

        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        match surface
            .execute(UapiCommand::SubmitIo {
                req: IoSubmitReq {
                    op_id: 12,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("block-2".into())),
                },
            })
            .expect("submit io")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::DrainCq { cq, owner: 0 })
            .expect("drain cq")
        {
            UapiResponse::Completions { events, remaining } => {
                assert_eq!(events.len(), 1);
                assert_eq!(remaining, 0);
                assert_eq!(events[0].status, CompletionStatus::Success);
            }
            other => panic!("unexpected response: {other:?}"),
        }
    }

    #[test]
    fn local_guest_uapi_supports_shmem_dfs_and_db_commands() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        match surface
            .execute(UapiCommand::SubmitShmemPut {
                req: ShmemPutReq {
                    task: None,
                    requester_entity: 0,
                    segment,
                    bytes: 4096,
                },
            })
            .expect("shmem put")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitDfsWrite {
                req: DfsWriteReq {
                    task: None,
                    path: "/weights/l0.bin".into(),
                    bytes: 8192,
                },
            })
            .expect("dfs write")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitDfsRead {
                req: DfsReadReq {
                    task: None,
                    path: "/weights/l0.bin".into(),
                },
            })
            .expect("dfs read")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitShmemGet {
                req: ShmemGetReq {
                    task: None,
                    requester_entity: 0,
                    segment,
                    bytes: 4096,
                },
            })
            .expect("shmem get")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitDbPut {
                req: DbPutReq {
                    task: None,
                    key: "weights:layer0".into(),
                    bytes: 128,
                },
            })
            .expect("db put")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitDbGet {
                req: DbGetReq {
                    task: None,
                    key: "weights:layer0".into(),
                },
            })
            .expect("db get")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 6);
        assert!(events
            .iter()
            .any(|event| event.source == sim_core::CompletionSource::ShmemService));
        assert!(events
            .iter()
            .any(|event| event.source == sim_core::CompletionSource::DfsService));
        assert!(events
            .iter()
            .any(|event| event.source == sim_core::CompletionSource::DbService));
    }

    #[test]
    fn local_guest_uapi_supports_block_writeback_command() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };

        surface
            .execute(UapiCommand::SubmitIo {
                req: IoSubmitReq {
                    op_id: 13,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: None,
                    block: Some(BlockHash("wb-block".into())),
                },
            })
            .expect("submit write");
        surface
            .execute(UapiCommand::SubmitBlockWriteback {
                block: BlockHash("wb-block".into()),
                task: None,
            })
            .expect("submit writeback");

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 2);
        assert!(events
            .iter()
            .all(|event| event.status == CompletionStatus::Success));
    }

    #[test]
    fn local_guest_uapi_can_surface_block_queue_pressure() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut surface = LocalGuestUapiSurface::with_block_profile(
            topology,
            BlockServiceProfile {
                queue_depth: 1,
                ..BlockServiceProfile::default()
            },
        );
        let _cq = surface.register_cq().expect("register cq");

        surface
            .submit_io(IoSubmitReq {
                op_id: 21,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: None,
                block: Some(BlockHash("queue-0".into())),
            })
            .expect("first write should succeed");

        let err = surface
            .submit_io(IoSubmitReq {
                op_id: 22,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: None,
                block: Some(BlockHash("queue-1".into())),
            })
            .expect_err("second write should hit queue pressure");
        assert!(matches!(err, SimError::InvalidInput("block queue full")));
    }

    #[test]
    fn local_guest_uapi_supports_command_queue_and_doorbell() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 4,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };

        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        match surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::Io(IoSubmitReq {
                    op_id: 30,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("cmdq-block".into())),
                }),
            })
            .expect("enqueue write")
        {
            UapiResponse::CommandEnqueued { depth: 1, .. } => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::RingDoorbell {
                cmdq,
                owner: 0,
                max_batch: Some(1),
            })
            .expect("doorbell")
        {
            UapiResponse::DoorbellRung {
                submitted: 1,
                pending: 0,
            } => {}
            other => panic!("unexpected response: {other:?}"),
        }

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].source, sim_core::CompletionSource::BlockService);
    }

    #[test]
    fn local_guest_uapi_dispatch_completes_as_chipbackend() {
        let mut surface = test_surface();
        let cq = surface.register_cq().expect("register cq");
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 4,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };
        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        let _ = surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::Io(IoSubmitReq {
                    op_id: 31,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::Dispatch,
                    segment: Some(segment),
                    block: None,
                }),
            })
            .expect("enqueue dispatch");
        let _ = surface
            .execute(UapiCommand::RingDoorbell {
                cmdq,
                owner: 0,
                max_batch: Some(1),
            })
            .expect("doorbell");

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].source, sim_core::CompletionSource::ChipBackend);
    }

    #[test]
    fn host_vector_dispatch_accepts_w4_seed_payload() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let guest_input = vec![0u8; 4096];
        let task = TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 31,
        };
        let output =
            super::run_host_vector_chipbackend(&topology, &task, &guest_input).expect("dispatch");
        assert_eq!(&output[..8], &0x41a0000041a00000u64.to_le_bytes());
    }

    #[test]
    fn local_guest_uapi_command_queue_enforces_depth() {
        let mut surface = test_surface();
        let cq = surface.register_cq().expect("register cq");
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 1,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };

        surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::DbPut(DbPutReq {
                    task: None,
                    key: "one".into(),
                    bytes: 16,
                }),
            })
            .expect("first enqueue");

        let err = surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::DbPut(DbPutReq {
                    task: None,
                    key: "two".into(),
                    bytes: 16,
                }),
            })
            .expect_err("depth should be enforced");
        assert!(matches!(err, SimError::InvalidInput("command queue full")));
    }

    #[test]
    fn local_guest_uapi_partial_poll_preserves_remaining_entries() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 4,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };
        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        for (op_id, opcode) in [(40, IoOpcode::WriteBlock), (41, IoOpcode::ReadBlock)] {
            match surface
                .execute(UapiCommand::EnqueueCmd {
                    cmdq,
                    owner: 0,
                    desc: UapiDescriptor::Io(IoSubmitReq {
                        op_id,
                        task: None,
                        entity: 0,
                        opcode,
                        segment: Some(segment),
                        block: Some(BlockHash("partial-poll-block".into())),
                    }),
                })
                .expect("enqueue io")
            {
                UapiResponse::CommandEnqueued { .. } => {}
                other => panic!("unexpected response: {other:?}"),
            }
        }

        match surface
            .execute(UapiCommand::RingDoorbell {
                cmdq,
                owner: 0,
                max_batch: None,
            })
            .expect("ring doorbell")
        {
            UapiResponse::DoorbellRung {
                submitted: 2,
                pending: 0,
            } => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::PollCq {
                cq,
                owner: 0,
                max_entries: Some(1),
            })
            .expect("partial poll")
        {
            UapiResponse::Completions { events, remaining } => {
                assert_eq!(events.len(), 1);
                assert_eq!(remaining, 1);
            }
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::DrainCq { cq, owner: 0 })
            .expect("drain cq")
        {
            UapiResponse::Completions { events, remaining } => {
                assert_eq!(events.len(), 1);
                assert_eq!(remaining, 0);
            }
            other => panic!("unexpected response: {other:?}"),
        }
    }

    #[test]
    fn local_guest_uapi_enforces_cq_and_cmdq_ownership() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 7 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 7,
                depth: 2,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };

        let enqueue_err = surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::DbPut(DbPutReq {
                    task: None,
                    key: "owner-mismatch".into(),
                    bytes: 32,
                }),
            })
            .expect_err("owner mismatch should fail");
        assert!(matches!(
            enqueue_err,
            SimError::InvalidInput("command queue owner mismatch")
        ));

        let poll_err = surface
            .execute(UapiCommand::PollCq {
                cq,
                owner: 0,
                max_entries: Some(1),
            })
            .expect_err("cq owner mismatch should fail");
        assert!(matches!(
            poll_err,
            SimError::InvalidInput("completion queue owner mismatch")
        ));
    }

    #[test]
    fn local_guest_uapi_retries_service_submission_after_runtime_backpressure() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut surface = LocalGuestUapiSurface::with_service_profiles(
            topology,
            BlockServiceProfile::default(),
            ShmemServiceProfile {
                queue_depth: 1,
                ..ShmemServiceProfile::default()
            },
            DfsServiceProfile::default(),
            DbServiceProfile::default(),
        );

        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 4,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };
        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        for _ in 0..2 {
            match surface
                .execute(UapiCommand::EnqueueCmd {
                    cmdq,
                    owner: 0,
                    desc: UapiDescriptor::ShmemPut(ShmemPutReq {
                        task: None,
                        requester_entity: 0,
                        segment,
                        bytes: 1024,
                    }),
                })
                .expect("enqueue put")
            {
                UapiResponse::CommandEnqueued { .. } => {}
                other => panic!("unexpected response: {other:?}"),
            }
        }

        match surface
            .execute(UapiCommand::RingDoorbell {
                cmdq,
                owner: 0,
                max_batch: None,
            })
            .expect("ring doorbell")
        {
            UapiResponse::DoorbellRung {
                submitted: 2,
                pending: 0,
            } => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::DrainCq { cq, owner: 0 })
            .expect("drain cq")
        {
            UapiResponse::Completions { events, remaining } => {
                assert_eq!(remaining, 0);
                assert_eq!(events.len(), 2);
                assert!(events
                    .iter()
                    .all(|event| event.source == sim_core::CompletionSource::ShmemService));
            }
            other => panic!("unexpected response: {other:?}"),
        }
    }
}
