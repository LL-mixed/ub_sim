//! Guest-visible UAPI surface placeholders.

use std::collections::{HashMap, VecDeque};

use sim_core::{
    BlockHash, CmdQueueHandle, CompletionEvent, CompletionSource, CompletionStatus, CqHandle,
    EntityId, HealthStatus, IoOpcode, IoSubmitReq, SegmentHandle, SimError, TaskKey,
};
use sim_services::{
    block::{BlockServiceProfile, BlockServiceStub},
    db::{DbGetReq, DbServiceProfile, DbServiceStub, DbPutReq},
    dfs::{DfsReadReq, DfsServiceProfile, DfsServiceStub, DfsWriteReq},
    shmem::{ShmemGetReq, ShmemPutReq, ShmemServiceProfile, ShmemServiceStub},
};
use sim_runtime::{
    RuntimeCompletionTracker, RuntimeDriveAction, RuntimeQueueRecord, RuntimeWorkItem,
    RuntimeWorkKind, SharedRuntimeExecutor,
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
    CreateSegment { bytes: u64 },
    RegisterCq { owner: EntityId },
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
    SubmitIo { req: IoSubmitReq },
    SubmitBlockWriteback {
        block: BlockHash,
        task: Option<TaskKey>,
    },
    SubmitShmemPut { req: ShmemPutReq },
    SubmitShmemGet { req: ShmemGetReq },
    SubmitDfsRead { req: DfsReadReq },
    SubmitDfsWrite { req: DfsWriteReq },
    SubmitDbPut { req: DbPutReq },
    SubmitDbGet { req: DbGetReq },
    PollCq {
        cq: CqHandle,
        owner: EntityId,
        max_entries: Option<usize>,
    },
    DrainCq { cq: CqHandle, owner: EntityId },
    GetHealth { entity: EntityId },
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
                        "block queue full" | "shmem queue full" | "dfs queue full" | "db queue full"
                    ) =>
                {
                    if now == u64::MAX {
                        let _ = self.flush_services(now);
                    }
                    RuntimeDriveAction::Retry(UapiRuntimeFailure {
                        cq: *cq,
                        code,
                    })
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
            return Err(SimError::InvalidInput("command queue depth must be positive"));
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
            let batch = max_batch.unwrap_or(queue.pending.len()).min(queue.pending.len());
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
        let limit = max_entries.unwrap_or(queue.events.len()).min(queue.events.len());
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
            UapiCommand::RegisterCq { owner } => {
                self.register_cq_with_owner(owner).map(UapiResponse::CqRegistered)
            }
            UapiCommand::CreateCmdQueue { cq, owner, depth } => self
                .create_cmd_queue(cq, owner, depth)
                .map(UapiResponse::CmdQueueCreated),
            UapiCommand::EnqueueCmd { cmdq, owner, desc } => self
                .enqueue_cmd(cmdq, owner, desc)
                .map(|(depth, remaining_capacity)| UapiResponse::CommandEnqueued {
                    depth,
                    remaining_capacity,
                }),
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
                let block = req.block.ok_or(SimError::InvalidInput("missing block hash"))?;
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
                let block = req.block.ok_or(SimError::InvalidInput("missing block hash"))?;
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
                let event = CompletionEvent {
                    op_id: req.op_id,
                    task: req.task,
                    source: CompletionSource::GuestUapi,
                    status: CompletionStatus::Success,
                    finished_at: req.op_id,
                };
                self.enqueue_to_cq(cq, event)?;
                Ok(req.op_id)
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
        GuestUapiSurface, LocalGuestUapiSurface, UapiCommand, UapiDescriptor, UapiResponse,
    };
    use sim_config::ScenarioConfig;
    use sim_core::{BlockHash, CompletionStatus, IoOpcode, IoSubmitReq, SimError};
    use sim_services::block::BlockServiceProfile;
    use sim_services::{
        db::{DbGetReq, DbPutReq, DbServiceProfile},
        dfs::{DfsReadReq, DfsServiceProfile, DfsWriteReq},
        shmem::{ShmemGetReq, ShmemPutReq, ShmemServiceProfile},
    };
    use sim_topology::SimTopology;

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

    fn test_surface() -> LocalGuestUapiSurface {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        LocalGuestUapiSurface::new(topology)
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
        assert!(events.iter().any(|event| event.source == sim_core::CompletionSource::ShmemService));
        assert!(events.iter().any(|event| event.source == sim_core::CompletionSource::DfsService));
        assert!(events.iter().any(|event| event.source == sim_core::CompletionSource::DbService));
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
        assert!(events.iter().all(|event| event.status == CompletionStatus::Success));
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
            UapiResponse::DoorbellRung { submitted: 1, pending: 0 } => {}
            other => panic!("unexpected response: {other:?}"),
        }

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].source, sim_core::CompletionSource::BlockService);
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
            UapiResponse::DoorbellRung { submitted: 2, pending: 0 } => {}
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
                assert!(events.iter().all(|event| event.source == sim_core::CompletionSource::ShmemService));
            }
            other => panic!("unexpected response: {other:?}"),
        }
    }
}
