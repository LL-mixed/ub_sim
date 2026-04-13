//! Runtime traits and orchestration glue.

use std::collections::{HashMap, HashSet, VecDeque};

use sim_config::ScenarioConfig;
use sim_core::{
    BlockHash, BlockPlacement, CompletionEvent, CompletionSource, CompletionStatus, CopyDirection,
    CopyRequest, DispatchHandle, DispatchRequest, OpId, PlLevel, RouteDecision, RouteReason,
    ServiceOpHandle, SimEvent, SimTimestamp, TaskKey, TransferHandle,
};
use sim_topology::SimTopology;

pub trait EventSink {
    fn emit(&mut self, event: SimEvent);
}

#[derive(Debug, Default)]
pub struct VecEventSink {
    events: Vec<SimEvent>,
}

impl VecEventSink {
    pub fn into_events(self) -> Vec<SimEvent> {
        self.events
    }
}

impl EventSink for VecEventSink {
    fn emit(&mut self, event: SimEvent) {
        self.events.push(event);
    }
}

#[derive(Debug, Clone)]
pub struct BlockReadReq {
    pub task: Option<TaskKey>,
    pub block: BlockHash,
}

#[derive(Debug, Clone)]
pub struct BlockWriteReq {
    pub task: Option<TaskKey>,
    pub block: BlockHash,
}

#[derive(Debug, Clone)]
pub struct RouteRequest {
    pub task: TaskKey,
    pub current_level: sim_core::PlLevel,
    pub block: BlockHash,
}

#[derive(Debug, Clone)]
pub struct LookupResult {
    pub found: bool,
    pub placement: Option<BlockPlacement>,
}

#[derive(Debug, Clone)]
pub struct PromotionPlan {
    pub block: BlockHash,
}

#[derive(Debug, Clone)]
pub struct EvictionPlan {
    pub max_blocks: usize,
}

pub trait ChipBackend {
    fn dispatch(&self, req: DispatchRequest) -> Result<DispatchHandle, sim_core::SimError>;
    fn h2d_copy(&self, req: CopyRequest) -> Result<TransferHandle, sim_core::SimError>;
    fn d2h_copy(&self, req: CopyRequest) -> Result<TransferHandle, sim_core::SimError>;
    fn poll_completion(&self, now: SimTimestamp) -> Vec<CompletionEvent>;
}

pub trait BlockService {
    fn read(&self, req: BlockReadReq) -> Result<ServiceOpHandle, sim_core::SimError>;
    fn write(&self, req: BlockWriteReq) -> Result<ServiceOpHandle, sim_core::SimError>;
    fn poll_completion(&self, now: SimTimestamp) -> Vec<CompletionEvent>;
}

pub trait RoutePlanner {
    fn plan(
        &self,
        req: RouteRequest,
        topo: &SimTopology,
    ) -> Result<RouteDecision, sim_core::SimError>;
}

pub trait SimBlockStore {
    fn lookup(&self, block: &BlockHash) -> LookupResult;
    fn stage_insert(&mut self, plan: PromotionPlan) -> Result<(), sim_core::SimError>;
    fn evict(&mut self, plan: EvictionPlan) -> Result<Vec<BlockHash>, sim_core::SimError>;
}

pub trait RingRuntime {
    fn on_scope_enter(&mut self, task: &TaskKey);
    fn on_scope_exit(&mut self, task: &TaskKey);
    fn on_pl_free(&mut self, task: &TaskKey, block: &BlockHash);
}

#[derive(Debug, Clone)]
pub struct RuntimeQueueRecord<T> {
    pub payload: T,
    pub ready_at: SimTimestamp,
    pub attempts: u32,
}

#[derive(Debug)]
pub struct SharedRuntimeQueue<T> {
    issue_latency_us: SimTimestamp,
    retry_delay_us: SimTimestamp,
    queue_depth: usize,
    max_retries: u32,
    pending: VecDeque<RuntimeQueueRecord<T>>,
}

impl<T> SharedRuntimeQueue<T> {
    pub fn with_policy(
        issue_latency_us: SimTimestamp,
        retry_delay_us: SimTimestamp,
        queue_depth: usize,
        max_retries: u32,
    ) -> Self {
        Self {
            issue_latency_us,
            retry_delay_us,
            queue_depth,
            max_retries,
            pending: VecDeque::new(),
        }
    }

    pub fn enqueue(&mut self, payload: T, now: SimTimestamp) -> Result<(), sim_core::SimError> {
        if self.pending.len() >= self.queue_depth {
            return Err(sim_core::SimError::InvalidInput("runtime queue full"));
        }
        self.pending.push_back(RuntimeQueueRecord {
            payload,
            ready_at: now.saturating_add(self.issue_latency_us),
            attempts: 0,
        });
        Ok(())
    }

    pub fn drain_ready(&mut self, now: SimTimestamp) -> (Vec<RuntimeQueueRecord<T>>, bool) {
        let mut ready = Vec::new();
        let mut deferred = VecDeque::new();
        let force_flush = now == u64::MAX;

        while let Some(entry) = self.pending.pop_front() {
            if !force_flush && entry.ready_at > now {
                deferred.push_back(entry);
                continue;
            }
            ready.push(entry);
        }

        self.pending = deferred;
        (ready, force_flush)
    }

    pub fn retry(&mut self, mut entry: RuntimeQueueRecord<T>, now: SimTimestamp) -> bool {
        if entry.attempts >= self.max_retries {
            return false;
        }
        entry.attempts += 1;
        entry.ready_at = if now == u64::MAX {
            now
        } else {
            now.saturating_add(self.retry_delay_us)
        };
        self.pending.push_back(entry);
        true
    }

    pub fn len(&self) -> usize {
        self.pending.len()
    }

    pub fn is_empty(&self) -> bool {
        self.pending.is_empty()
    }
}

#[derive(Debug)]
pub struct SharedRuntimeExecutor<T> {
    queue: SharedRuntimeQueue<T>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct RuntimeCompletionKey {
    pub source: CompletionSource,
    pub op_id: OpId,
}

#[derive(Debug)]
pub struct RuntimeCompletionTracker<T> {
    issued: HashMap<RuntimeCompletionKey, T>,
}

impl<T> RuntimeCompletionTracker<T> {
    pub fn issue(&mut self, source: CompletionSource, op_id: OpId, payload: T) {
        self.issued
            .insert(RuntimeCompletionKey { source, op_id }, payload);
    }

    pub fn complete(&mut self, event: &CompletionEvent) -> Option<T> {
        self.issued.remove(&RuntimeCompletionKey {
            source: event.source,
            op_id: event.op_id,
        })
    }
}

impl<T> Default for RuntimeCompletionTracker<T> {
    fn default() -> Self {
        Self {
            issued: HashMap::new(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RuntimeDriveAction<E> {
    Complete,
    Retry(E),
    Fail(E),
}

impl<T> SharedRuntimeExecutor<T> {
    pub fn with_policy(
        issue_latency_us: SimTimestamp,
        retry_delay_us: SimTimestamp,
        queue_depth: usize,
        max_retries: u32,
    ) -> Self {
        Self {
            queue: SharedRuntimeQueue::with_policy(
                issue_latency_us,
                retry_delay_us,
                queue_depth,
                max_retries,
            ),
        }
    }

    pub fn enqueue(&mut self, payload: T, now: SimTimestamp) -> Result<(), sim_core::SimError> {
        self.queue.enqueue(payload, now)
    }

    pub fn drain_ready(&mut self, now: SimTimestamp) -> (Vec<RuntimeQueueRecord<T>>, bool) {
        self.queue.drain_ready(now)
    }

    pub fn retry(&mut self, entry: RuntimeQueueRecord<T>, now: SimTimestamp) -> bool {
        self.queue.retry(entry, now)
    }

    pub fn drive_ready<E, F>(&mut self, now: SimTimestamp, mut issue: F) -> (Vec<E>, bool)
    where
        T: Clone,
        F: FnMut(&RuntimeQueueRecord<T>) -> RuntimeDriveAction<E>,
    {
        let (ready, force_flush) = self.queue.drain_ready(now);
        let mut failures = Vec::new();

        for entry in ready {
            match issue(&entry) {
                RuntimeDriveAction::Complete => {}
                RuntimeDriveAction::Retry(err) => {
                    if !self.queue.retry(entry, now) {
                        failures.push(err);
                    }
                }
                RuntimeDriveAction::Fail(err) => failures.push(err),
            }
        }

        (failures, force_flush)
    }

    pub fn len(&self) -> usize {
        self.queue.len()
    }

    pub fn is_empty(&self) -> bool {
        self.queue.is_empty()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RuntimeWorkKind {
    Dispatch,
    HostToDeviceCopy,
    DeviceToHostCopy,
    GuestIo,
    BlockWriteback,
    ShmemPut,
    ShmemGet,
    DfsRead,
    DfsWrite,
    DbPut,
    DbGet,
}

pub type RuntimeOpKind = RuntimeWorkKind;

#[derive(Debug, Clone)]
pub struct RuntimeWorkItem<T> {
    pub op_id: OpId,
    pub kind: RuntimeWorkKind,
    pub task: Option<TaskKey>,
    pub payload: T,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RuntimeOpState {
    Queued,
    Issued,
    Completed,
    Failed,
}

#[derive(Debug, Clone)]
pub struct RuntimeOpRecord {
    pub op_id: OpId,
    pub kind: RuntimeOpKind,
    pub task: TaskKey,
    pub state: RuntimeOpState,
    pub submitted_at: SimTimestamp,
    pub issued_at: Option<SimTimestamp>,
    pub ready_at: SimTimestamp,
    pub timeout_at: SimTimestamp,
    pub attempts: u32,
}

#[derive(Debug)]
pub struct LocalRuntimeEngine {
    now: SimTimestamp,
    next_op_id: OpId,
    dispatch_latency_us: SimTimestamp,
    copy_latency_us: SimTimestamp,
    timeout_us: SimTimestamp,
    max_inflight: usize,
    submission_queue: SharedRuntimeExecutor<RuntimeWorkItem<()>>,
    inflight: Vec<RuntimeOpRecord>,
    completed: VecDeque<CompletionEvent>,
}

impl LocalRuntimeEngine {
    pub fn from_config(config: &ScenarioConfig) -> Self {
        Self::with_policy(
            config.pypto.simpler_boundary.dispatch_latency_us.unwrap_or(1),
            config.levels.l3_host_tier.fetch_latency_us.unwrap_or(1),
            config.levels.l4_domain_tier.fetch_latency_us.unwrap_or(80),
            (config.topology.hosts * config.topology.ubpus_per_host) as usize,
            1,
        )
    }

    pub fn with_policy(
        dispatch_latency_us: SimTimestamp,
        copy_latency_us: SimTimestamp,
        timeout_us: SimTimestamp,
        max_inflight: usize,
        max_retries: u32,
    ) -> Self {
        Self {
            now: 0,
            next_op_id: 0,
            dispatch_latency_us,
            copy_latency_us,
            timeout_us,
            max_inflight,
            submission_queue: SharedRuntimeExecutor::with_policy(0, 0, max_inflight, max_retries),
            inflight: Vec::new(),
            completed: VecDeque::new(),
        }
    }

    pub fn now(&self) -> SimTimestamp {
        self.now
    }

    pub fn submit_dispatch(
        &mut self,
        req: DispatchRequest,
        sink: &mut dyn EventSink,
    ) -> Result<DispatchHandle, sim_core::SimError> {
        self.ensure_capacity()?;
        let op_id = self.next_op();
        self.inflight.push(RuntimeOpRecord {
            op_id,
            kind: RuntimeOpKind::Dispatch,
            task: req.task.clone(),
            state: RuntimeOpState::Queued,
            submitted_at: self.now,
            issued_at: None,
            ready_at: 0,
            timeout_at: 0,
            attempts: 0,
        });
        self.submission_queue.enqueue(
            RuntimeWorkItem {
                op_id,
                kind: RuntimeWorkKind::Dispatch,
                task: Some(req.task.clone()),
                payload: (),
            },
            self.now,
        )?;
        sink.emit(SimEvent::DispatchSubmitted { at: self.now, req });
        Ok(DispatchHandle(op_id))
    }

    pub fn submit_copy(&mut self, req: CopyRequest) -> Result<TransferHandle, sim_core::SimError> {
        self.ensure_capacity()?;
        let op_id = self.next_op();
        let task = req.task.clone();
        let kind = match req.direction {
            CopyDirection::HostToDevice => RuntimeOpKind::HostToDeviceCopy,
            CopyDirection::DeviceToHost => RuntimeOpKind::DeviceToHostCopy,
        };
        self.inflight.push(RuntimeOpRecord {
            op_id,
            kind,
            task,
            state: RuntimeOpState::Queued,
            submitted_at: self.now,
            issued_at: None,
            ready_at: 0,
            timeout_at: 0,
            attempts: 0,
        });
        self.submission_queue.enqueue(
            RuntimeWorkItem {
                op_id,
                kind,
                task: Some(req.task),
                payload: (),
            },
            self.now,
        )?;
        Ok(TransferHandle(op_id))
    }

    pub fn advance_to(&mut self, now: SimTimestamp, sink: &mut dyn EventSink) {
        self.now = now;
        let dispatch_latency_us = self.dispatch_latency_us;
        let copy_latency_us = self.copy_latency_us;
        let timeout_us = self.timeout_us;

        let _ = self.submission_queue.drive_ready(now, |ready| {
            if let Some(op) = self
                .inflight
                .iter_mut()
                .find(|op| op.op_id == ready.payload.op_id && op.state == RuntimeOpState::Queued)
            {
                op.state = RuntimeOpState::Issued;
                op.issued_at = Some(now);
                let latency = match op.kind {
                    RuntimeOpKind::Dispatch => dispatch_latency_us,
                    RuntimeOpKind::HostToDeviceCopy | RuntimeOpKind::DeviceToHostCopy => {
                        copy_latency_us
                    }
                    _ => 0,
                };
                op.ready_at = op.submitted_at + latency;
                op.timeout_at = op.submitted_at + timeout_us;
            }
            RuntimeDriveAction::<()>::Complete
        });

        for op in &mut self.inflight {
            if op.state == RuntimeOpState::Issued && op.ready_at <= now {
                op.state = RuntimeOpState::Completed;
                let completion = CompletionEvent {
                    op_id: op.op_id,
                    task: Some(op.task.clone()),
                    source: CompletionSource::ChipBackend,
                    status: CompletionStatus::Success,
                    finished_at: op.ready_at,
                };
                sink.emit(SimEvent::CompletionObserved {
                    at: completion.finished_at,
                    completion: completion.clone(),
                });
                self.completed.push_back(completion);
                continue;
            }

            if op.state == RuntimeOpState::Issued && op.timeout_at <= now {
                let retry_entry = RuntimeQueueRecord {
                    payload: RuntimeWorkItem {
                        op_id: op.op_id,
                        kind: op.kind,
                        task: Some(op.task.clone()),
                        payload: (),
                    },
                    ready_at: now,
                    attempts: op.attempts,
                };
                if self.submission_queue.retry(retry_entry, now) {
                    op.attempts += 1;
                    op.state = RuntimeOpState::Queued;
                    op.submitted_at = now;
                    op.issued_at = None;
                    op.ready_at = 0;
                    op.timeout_at = 0;
                    sink.emit(SimEvent::RuntimeRetried {
                        at: now,
                        op_id: op.op_id,
                        reason: "timeout".to_string(),
                        attempt: op.attempts,
                    });
                } else {
                    op.state = RuntimeOpState::Failed;
                    let completion = CompletionEvent {
                        op_id: op.op_id,
                        task: Some(op.task.clone()),
                        source: CompletionSource::ChipBackend,
                        status: CompletionStatus::FatalFailure {
                            code: "timeout_exhausted".to_string(),
                        },
                        finished_at: now,
                    };
                    sink.emit(SimEvent::RuntimeFailed {
                        at: now,
                        op_id: op.op_id,
                        reason: "timeout_exhausted".to_string(),
                    });
                    sink.emit(SimEvent::CompletionObserved {
                        at: completion.finished_at,
                        completion: completion.clone(),
                    });
                    self.completed.push_back(completion);
                }
            }
        }

        self.inflight
            .retain(|op| !matches!(op.state, RuntimeOpState::Completed | RuntimeOpState::Failed));
    }

    pub fn poll_completions(
        &mut self,
        now: SimTimestamp,
        sink: &mut dyn EventSink,
    ) -> Vec<CompletionEvent> {
        self.advance_to(now, sink);
        self.completed.drain(..).collect()
    }

    pub fn inflight(&self) -> &[RuntimeOpRecord] {
        &self.inflight
    }

    fn next_op(&mut self) -> OpId {
        self.next_op_id += 1;
        self.next_op_id
    }

    fn ensure_capacity(&self) -> Result<(), sim_core::SimError> {
        if self.inflight.len() >= self.max_inflight {
            return Err(sim_core::SimError::InvalidInput("runtime queue full"));
        }
        Ok(())
    }
}

#[derive(Debug)]
pub struct RecursiveRoutePlanner {
    pub hit_weight: f64,
    pub load_weight: f64,
    pub capacity_weight: f64,
}

impl RecursiveRoutePlanner {
    pub fn from_config(config: &ScenarioConfig) -> Self {
        Self {
            hit_weight: config.routing.hit_weight,
            load_weight: config.routing.load_weight,
            capacity_weight: config.routing.capacity_weight,
        }
    }

    fn choose_reason(&self, level: PlLevel) -> RouteReason {
        match level {
            PlLevel::L2 if self.hit_weight >= self.capacity_weight => RouteReason::LocalHit,
            PlLevel::L3 if self.capacity_weight >= self.load_weight => {
                RouteReason::CapacityPreferred
            }
            PlLevel::L4 if self.load_weight > self.capacity_weight => RouteReason::HealthPreferred,
            _ => RouteReason::RecursiveFallback,
        }
    }
}

impl Default for RecursiveRoutePlanner {
    fn default() -> Self {
        Self {
            hit_weight: 1.0,
            load_weight: 1.0,
            capacity_weight: 1.0,
        }
    }
}

impl RoutePlanner for RecursiveRoutePlanner {
    fn plan(
        &self,
        req: RouteRequest,
        topo: &SimTopology,
    ) -> Result<RouteDecision, sim_core::SimError> {
        let selected = match req.current_level {
            PlLevel::L2 => topo
                .ubpus
                .iter()
                .find(|ubpu| ubpu.health == sim_core::HealthStatus::Healthy)
                .map(|ubpu| (PlLevel::L2, ubpu.node_id, self.choose_reason(PlLevel::L2))),
            PlLevel::L3 => topo
                .hosts
                .iter()
                .find(|host| host.health == sim_core::HealthStatus::Healthy)
                .map(|host| (PlLevel::L3, host.node_id, self.choose_reason(PlLevel::L3))),
            _ => topo
                .domains
                .iter()
                .find(|domain| domain.health == sim_core::HealthStatus::Healthy)
                .map(|domain| (PlLevel::L4, domain.node_id, self.choose_reason(PlLevel::L4))),
        };

        match selected {
            Some((to_level, selected_node, reason)) => Ok(RouteDecision {
                from_level: req.current_level,
                to_level,
                selected_node,
                reason,
            }),
            None => Err(sim_core::SimError::NotImplemented),
        }
    }
}

#[derive(Debug, Clone)]
pub struct InMemoryBlockStore {
    placements: HashSet<BlockPlacement>,
    insertion_order: VecDeque<BlockHash>,
    capacity_blocks: usize,
    default_level: PlLevel,
    default_node: u64,
}

impl InMemoryBlockStore {
    pub fn new() -> Self {
        Self {
            placements: HashSet::new(),
            insertion_order: VecDeque::new(),
            capacity_blocks: usize::MAX,
            default_level: PlLevel::L2,
            default_node: 0,
        }
    }

    pub fn from_config(config: &ScenarioConfig) -> Self {
        Self {
            placements: HashSet::new(),
            insertion_order: VecDeque::new(),
            capacity_blocks: config.levels.l2_ubpu_tier.capacity_blocks as usize,
            default_level: PlLevel::L2,
            default_node: 0,
        }
    }

    pub fn capacity_blocks(&self) -> usize {
        self.capacity_blocks
    }
}

impl Default for InMemoryBlockStore {
    fn default() -> Self {
        Self::new()
    }
}

impl SimBlockStore for InMemoryBlockStore {
    fn lookup(&self, block: &BlockHash) -> LookupResult {
        let placement = self
            .placements
            .iter()
            .find(|placement| placement.block == *block)
            .cloned();

        LookupResult {
            found: placement.is_some(),
            placement,
        }
    }

    fn stage_insert(&mut self, plan: PromotionPlan) -> Result<(), sim_core::SimError> {
        let placement = BlockPlacement {
            block: plan.block.clone(),
            level: self.default_level,
            node: self.default_node,
        };

        if self.placements.insert(placement) {
            self.insertion_order.push_back(plan.block);
        }
        while self.placements.len() > self.capacity_blocks {
            let _ = self.evict(EvictionPlan { max_blocks: 1 })?;
        }
        Ok(())
    }

    fn evict(&mut self, plan: EvictionPlan) -> Result<Vec<BlockHash>, sim_core::SimError> {
        let mut evicted = Vec::new();

        for _ in 0..plan.max_blocks {
            let Some(block) = self.insertion_order.pop_front() else {
                break;
            };

            if let Some(placement) = self
                .placements
                .iter()
                .find(|placement| placement.block == block)
                .cloned()
            {
                self.placements.remove(&placement);
                evicted.push(block);
            }
        }

        Ok(evicted)
    }
}

#[cfg(test)]
mod tests {
    use super::{
        EvictionPlan, InMemoryBlockStore, LocalRuntimeEngine, PromotionPlan, RecursiveRoutePlanner,
        RoutePlanner, RouteRequest, RuntimeCompletionTracker, RuntimeOpKind, RuntimeOpState,
        SharedRuntimeQueue, SimBlockStore, VecEventSink,
    };
    use sim_config::ScenarioConfig;
    use sim_core::{
        BlockHash, CompletionEvent, CompletionSource, CompletionStatus, CopyDirection,
        CopyRequest, DispatchRequest, FunctionLabel, HierarchyCoord, LogicalSystemId,
        MemoryEndpoint, PlLevel, SegmentHandle, SimEvent, TaskKey,
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

    fn test_task() -> TaskKey {
        TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 7,
        }
    }

    fn test_topology() -> SimTopology {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        SimTopology::from_config(&config).expect("topology build")
    }

    #[test]
    fn recursive_route_planner_picks_domain_for_l4_request() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let planner = RecursiveRoutePlanner::from_config(&config);
        let topo = test_topology();

        let decision = planner
            .plan(
                RouteRequest {
                    task: test_task(),
                    current_level: PlLevel::L4,
                    block: BlockHash("block-a".into()),
                },
                &topo,
            )
            .expect("route decision");

        assert_eq!(decision.from_level, PlLevel::L4);
        assert_eq!(decision.to_level, PlLevel::L4);
        assert_eq!(decision.selected_node, topo.domains[0].node_id);
    }

    #[test]
    fn in_memory_block_store_supports_lookup_insert_and_evict() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut store = InMemoryBlockStore::from_config(&config);
        let block = BlockHash("block-a".into());

        assert!(!store.lookup(&block).found);

        store
            .stage_insert(PromotionPlan {
                block: block.clone(),
            })
            .expect("insert");

        let lookup = store.lookup(&block);
        assert!(lookup.found);
        assert!(lookup.placement.is_some());

        let evicted = store.evict(EvictionPlan { max_blocks: 1 }).expect("evict");
        assert_eq!(evicted, vec![block.clone()]);
        assert!(!store.lookup(&block).found);
    }

    #[test]
    fn in_memory_block_store_uses_capacity_from_config() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let store = InMemoryBlockStore::from_config(&config);
        assert_eq!(store.capacity_blocks(), 1024);
    }

    #[test]
    fn runtime_engine_advances_dispatch_to_completion() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let handle = runtime
            .submit_dispatch(
                DispatchRequest {
                    task: test_task(),
                    function: FunctionLabel {
                        name: "decode_step".into(),
                        level: PlLevel::L4,
                    },
                    target_level: PlLevel::L4,
                    target_node: 19,
                    input_segments: vec![SegmentHandle(1)],
                },
                &mut sink,
            )
            .expect("dispatch submit");

        assert_eq!(handle.0, 1);
        assert_eq!(runtime.inflight().len(), 1);
        assert_eq!(runtime.inflight()[0].kind, RuntimeOpKind::Dispatch);
        assert_eq!(runtime.inflight()[0].state, RuntimeOpState::Queued);
        assert_eq!(runtime.inflight()[0].attempts, 0);

        runtime.advance_to(1, &mut sink);
        assert_eq!(runtime.inflight()[0].state, RuntimeOpState::Issued);

        let completions = runtime.poll_completions(15, &mut sink);
        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].op_id, 1);
        assert!(runtime.inflight().is_empty());
        assert!(!sink.into_events().is_empty());
    }

    #[test]
    fn runtime_engine_uses_copy_latency_from_config() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let handle = runtime
            .submit_copy(CopyRequest {
                task: test_task(),
                direction: CopyDirection::HostToDevice,
                bytes: 4096,
                src: MemoryEndpoint {
                    node: 1,
                    segment: SegmentHandle(1),
                    offset: 0,
                },
                dst: MemoryEndpoint {
                    node: 3,
                    segment: SegmentHandle(2),
                    offset: 0,
                },
            })
            .expect("copy submit");

        assert_eq!(handle.0, 1);
        runtime.advance_to(29, &mut sink);
        assert_eq!(runtime.inflight()[0].state, RuntimeOpState::Issued);

        let completions = runtime.poll_completions(30, &mut sink);
        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].finished_at, 30);
    }

    #[test]
    fn runtime_engine_retries_on_timeout_before_success() {
        let mut runtime = LocalRuntimeEngine::with_policy(15, 30, 5, 4, 3);
        let mut sink = VecEventSink::default();

        runtime
            .submit_dispatch(
                DispatchRequest {
                    task: test_task(),
                    function: FunctionLabel {
                        name: "decode_step".into(),
                        level: PlLevel::L4,
                    },
                    target_level: PlLevel::L4,
                    target_node: 19,
                    input_segments: vec![SegmentHandle(1)],
                },
                &mut sink,
            )
            .expect("dispatch submit");

        runtime.advance_to(5, &mut sink);
        assert_eq!(runtime.inflight()[0].state, RuntimeOpState::Queued);
        assert_eq!(runtime.inflight()[0].attempts, 1);

        let events = sink.into_events();
        assert!(events.iter().any(|event| matches!(
            event,
            SimEvent::RuntimeRetried {
                op_id: 1,
                attempt: 1,
                ..
            }
        )));
    }

    #[test]
    fn runtime_engine_fails_after_retry_budget_exhausted() {
        let mut runtime = LocalRuntimeEngine::with_policy(15, 30, 5, 4, 0);
        let mut sink = VecEventSink::default();

        runtime
            .submit_dispatch(
                DispatchRequest {
                    task: test_task(),
                    function: FunctionLabel {
                        name: "decode_step".into(),
                        level: PlLevel::L4,
                    },
                    target_level: PlLevel::L4,
                    target_node: 19,
                    input_segments: vec![SegmentHandle(1)],
                },
                &mut sink,
            )
            .expect("dispatch submit");

        let completions = runtime.poll_completions(5, &mut sink);
        assert_eq!(completions.len(), 1);
        assert_eq!(
            completions[0].status,
            CompletionStatus::FatalFailure {
                code: "timeout_exhausted".into()
            }
        );

        let events = sink.into_events();
        assert!(events.iter().any(|event| matches!(
            event,
            SimEvent::RuntimeFailed { op_id: 1, .. }
        )));
    }

    #[test]
    fn runtime_engine_rejects_submit_when_queue_is_full() {
        let mut runtime = LocalRuntimeEngine::with_policy(15, 30, 80, 1, 1);
        let mut sink = VecEventSink::default();

        runtime
            .submit_dispatch(
                DispatchRequest {
                    task: test_task(),
                    function: FunctionLabel {
                        name: "decode_step".into(),
                        level: PlLevel::L4,
                    },
                    target_level: PlLevel::L4,
                    target_node: 19,
                    input_segments: vec![SegmentHandle(1)],
                },
                &mut sink,
            )
            .expect("dispatch submit");

        let err = runtime
            .submit_copy(CopyRequest {
                task: test_task(),
                direction: CopyDirection::HostToDevice,
                bytes: 4096,
                src: MemoryEndpoint {
                    node: 1,
                    segment: SegmentHandle(1),
                    offset: 0,
                },
                dst: MemoryEndpoint {
                    node: 3,
                    segment: SegmentHandle(2),
                    offset: 0,
                },
            })
            .expect_err("queue full");

        assert!(matches!(
            err,
            sim_core::SimError::InvalidInput("runtime queue full")
        ));
    }

    #[test]
    fn shared_runtime_queue_retries_then_exhausts() {
        let mut queue = SharedRuntimeQueue::with_policy(2, 3, 4, 1);
        queue.enqueue("job", 10).expect("enqueue");

        let (ready, force_flush) = queue.drain_ready(11);
        assert!(!force_flush);
        assert!(ready.is_empty());

        let (ready, _) = queue.drain_ready(12);
        let entry = ready.into_iter().next().expect("ready entry");
        assert!(queue.retry(entry, 12));

        let (ready, _) = queue.drain_ready(14);
        assert!(ready.is_empty());

        let (ready, _) = queue.drain_ready(15);
        let entry = ready.into_iter().next().expect("retried entry");
        assert!(!queue.retry(entry, 15));
    }

    #[test]
    fn completion_tracker_round_trips_payload() {
        let mut tracker = RuntimeCompletionTracker::default();
        tracker.issue(CompletionSource::ChipBackend, 7, "payload");
        let event = CompletionEvent {
            op_id: 7,
            task: None,
            source: CompletionSource::ChipBackend,
            status: CompletionStatus::Success,
            finished_at: 10,
        };
        assert_eq!(tracker.complete(&event), Some("payload"));
        assert_eq!(tracker.complete(&event), None);
    }
}
