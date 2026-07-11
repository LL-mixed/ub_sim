//! Runtime traits and orchestration glue.

use std::collections::{HashMap, HashSet, VecDeque};
use std::ffi::OsString;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Mutex, OnceLock};
use std::time::Instant;

use sim_chipbackend_simpler as simpler_capi;
use sim_config::ScenarioConfig;
use sim_core::{
    BackendDispatchOperation, BackendExecutionRequest, BinaryArtifactRef, BlockHash,
    BlockPlacement, CompletionEvent, CompletionSource, CompletionStatus, CopyDirection,
    CopyRequest, DispatchBackendProfile, DispatchBackendSpec, DispatchBufferBinding,
    DispatchHandle, DispatchRequest, DispatchRuntimeVariant, ExecutionContextCommand,
    MemoryEndpoint, NodeId, OpId, PlLevel, RouteDecision, RouteReason, ServiceOpHandle, SimEvent,
    SimTimestamp, SimplerRuntimeArg, TaskKey, TransferHandle,
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

fn qwen3_dispatch_detail_timing_enabled() -> bool {
    std::env::var("SIM_QWEN3_DISPATCH_DETAIL_TIMING")
        .map(|value| {
            matches!(
                value.as_str(),
                "1" | "true" | "TRUE" | "yes" | "YES" | "on" | "ON"
            )
        })
        .unwrap_or(false)
}

fn qwen3_dispatch_detail_log_line(line: &str) {
    let path = std::env::var_os("SIM_QWEN3_DISPATCH_DETAIL_TIMING_FILE")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp/qwen3-dispatch-detail.log"));
    if let Ok(mut file) = fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(file, "{line}");
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
    pub backend_spec: Option<DispatchBackendSpec>,
    pub request: Option<BackendExecutionRequest>,
    pub copy_req: Option<CopyRequest>,
    pub task: TaskKey,
    pub function_name: Option<String>,
    pub request_id: Option<String>,
    pub trace_id: Option<String>,
    pub plan_id: Option<String>,
    pub step_id: Option<String>,
    pub step_kind: Option<String>,
    pub device_context_id: Option<String>,
    pub runtime_context_id: Option<String>,
    pub target_level: Option<PlLevel>,
    pub target_node: Option<NodeId>,
    pub input_segment_count: usize,
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
    backend_mode: ChipBackendMode,
    dispatch_latency_us: SimTimestamp,
    copy_latency_us: SimTimestamp,
    timeout_us: SimTimestamp,
    max_inflight: usize,
    submission_queue: SharedRuntimeExecutor<RuntimeWorkItem<()>>,
    inflight: Vec<RuntimeOpRecord>,
    completed: VecDeque<CompletionEvent>,
    simpler_capi: SimplerCapiBackendState,
    host_payloads: HostPayloadRegistry,
    execution_contexts: ExecutionContextRegistry,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ChipBackendMode {
    LocalRuntime,
    SimplerProcess,
    SimplerCapi,
}

#[derive(Debug, Clone)]
struct SimplerProcessRunner {
    adapter_script: PathBuf,
}

#[derive(Debug, Clone, Copy)]
struct SimplerDeviceAlloc {
    #[allow(dead_code)]
    ptr: simpler_capi::DevicePtr,
    bytes: u64,
}

#[derive(Debug, Default)]
struct SimplerCapiBackendState {
    runtime_library: Option<SimplerLoadedRuntimeLibrary>,
    device_allocs: HashMap<(NodeId, sim_core::SegmentHandle), SimplerDeviceAlloc>,
}

impl Drop for SimplerCapiBackendState {
    fn drop(&mut self) {
        if self.runtime_library.is_none() || self.device_allocs.is_empty() {
            return;
        }
        let allocations = self
            .device_allocs
            .drain()
            .map(|(_, allocation)| allocation.ptr)
            .collect::<Vec<_>>();
        let _ = with_simpler_device_context(self, 0, |api, worker| {
            for allocation in allocations {
                api.free_device(&worker.context, allocation);
            }
            Ok(())
        });
    }
}

#[derive(Debug, Default)]
struct HostPayloadRegistry {
    segments: HashMap<(NodeId, sim_core::SegmentHandle), Vec<u8>>,
}

#[derive(Debug, Clone)]
struct DeviceContextRecord {
    id: String,
    state: ContextState,
    generation: u64,
    warm: bool,
    reusable: bool,
    created_at: SimTimestamp,
    last_used_at: SimTimestamp,
    dispatch_count: u64,
    reset_count: u64,
    teardown_count: u64,
}

#[derive(Debug, Clone)]
struct RuntimeContextRecord {
    id: String,
    device_context_id: String,
    state: ContextState,
    generation: u64,
    warm: bool,
    reusable: bool,
    created_at: SimTimestamp,
    last_used_at: SimTimestamp,
    dispatch_count: u64,
    reset_count: u64,
    teardown_count: u64,
    resident_bindings: HashMap<String, DispatchBufferBinding>,
}

#[derive(Debug, Default)]
struct ExecutionContextRegistry {
    device_contexts: HashMap<String, DeviceContextRecord>,
    runtime_contexts: HashMap<String, RuntimeContextRecord>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ContextState {
    Active,
    Closed,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeviceContextSnapshot {
    pub id: String,
    pub state: ContextState,
    pub generation: u64,
    pub warm: bool,
    pub reusable: bool,
    pub created_at: SimTimestamp,
    pub last_used_at: SimTimestamp,
    pub dispatch_count: u64,
    pub reset_count: u64,
    pub teardown_count: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RuntimeContextSnapshot {
    pub id: String,
    pub device_context_id: String,
    pub state: ContextState,
    pub generation: u64,
    pub warm: bool,
    pub reusable: bool,
    pub created_at: SimTimestamp,
    pub last_used_at: SimTimestamp,
    pub dispatch_count: u64,
    pub reset_count: u64,
    pub teardown_count: u64,
    pub resident_binding_count: usize,
}

#[derive(Debug)]
struct SimplerLoadedRuntimeLibrary {
    path: PathBuf,
    api: &'static simpler_capi::RuntimeLibrary,
}

static SIMPLER_RUNTIME_LIBRARY_CACHE: OnceLock<
    Mutex<HashMap<PathBuf, &'static simpler_capi::RuntimeLibrary>>,
> = OnceLock::new();
static SIMPLER_DEVICE_CONTEXT_CACHE: OnceLock<
    Mutex<HashMap<(usize, i32), SharedSimplerDeviceContext>>,
> = OnceLock::new();

struct SharedSimplerDeviceContext {
    context: simpler_capi::DeviceContext<'static>,
    callable_ids: HashMap<String, i32>,
    next_callable_id: i32,
}

struct EnvGuard {
    saved: Vec<(OsString, Option<OsString>)>,
}

impl EnvGuard {
    fn apply(overrides: &std::collections::BTreeMap<String, String>) -> Self {
        let mut saved = Vec::with_capacity(overrides.len());
        for (key, value) in overrides {
            let key_os = OsString::from(key);
            saved.push((key_os.clone(), std::env::var_os(key)));
            unsafe {
                std::env::set_var(key_os, value);
            }
        }
        Self { saved }
    }
}

impl Drop for EnvGuard {
    fn drop(&mut self) {
        for (key, previous) in self.saved.drain(..).rev() {
            match previous {
                Some(value) => unsafe {
                    std::env::set_var(&key, value);
                },
                None => unsafe {
                    std::env::remove_var(&key);
                },
            }
        }
    }
}

#[derive(Debug, Clone)]
struct SimplerDispatchManifest {
    version: u32,
    op_id: OpId,
    task: String,
    request_id: Option<String>,
    trace_id: Option<String>,
    plan_id: Option<String>,
    step_id: Option<String>,
    step_kind: Option<String>,
    device_context_id: Option<String>,
    runtime_context_id: Option<String>,
    function_name: Option<String>,
    target_level: Option<String>,
    target_node: Option<NodeId>,
    input_segment_count: usize,
    binding_count: usize,
    profile: Option<String>,
    platform: String,
    runtime_variant: Option<String>,
    callable_hint: Option<String>,
    block_hash: Option<String>,
    request_index: Option<u64>,
    block_index: Option<u64>,
    request_blocks_total: Option<u64>,
    blocks_remaining_in_request: Option<u64>,
    is_first_block_in_request: bool,
    is_last_block_in_request: bool,
    request_control_phase: Option<String>,
    request_control_epoch: Option<u64>,
    request_control_result_kind: Option<String>,
    request_control_result_value: Option<u64>,
    request_control_view_kind: Option<String>,
    kvcache_resolution_kind: Option<String>,
    kvcache_view_kind: Option<String>,
    logical_system_id: Option<u32>,
    scope_depth: Option<u32>,
    prefix_group: Option<u64>,
    route_from_level: Option<String>,
    route_to_level: Option<String>,
    route_selected_node: Option<NodeId>,
    route_reason: Option<String>,
    placement_level: Option<String>,
    placement_node: Option<NodeId>,
    capacity_pressure_active: bool,
    evictions_seen: u64,
    block_writebacks_seen: u64,
    promoted_this_access: bool,
    reloaded_after_eviction: bool,
    uses_dfs_fallback: bool,
    includes_request_control: bool,
    includes_prefix_shared: bool,
    hot_segment: Option<u64>,
    request_segment: Option<u64>,
    control_segment: Option<u64>,
    prefix_segment: Option<u64>,
}

impl SimplerProcessRunner {
    fn from_env() -> Self {
        Self {
            adapter_script: default_simpler_dispatch_script(),
        }
    }

    fn run_dispatch_example(
        &self,
        op_id: OpId,
        op: &RuntimeOpRecord,
        backend_spec: Option<&DispatchBackendSpec>,
    ) -> Result<(), String> {
        validate_simpler_dispatch_spec(backend_spec)?;
        validate_simpler_capi_dispatch_spec(backend_spec, op.request.as_ref())?;
        let manifest = simpler_dispatch_manifest(op_id, op, backend_spec);
        let manifest_path = write_simpler_dispatch_manifest(op_id, &manifest)?;
        let mut command = Command::new(&self.adapter_script);
        if let Ok(python_bin) = std::env::var("SIMPLER_PYTHON") {
            command.env("SIMPLER_PYTHON", python_bin);
        }
        if let Ok(simpler_root) = std::env::var("SIMPLER_PROJECT_ROOT") {
            command.env("SIMPLER_PROJECT_ROOT", simpler_root);
        }
        let status = command
            .arg("--manifest")
            .arg(&manifest_path)
            .status()
            .map_err(|err| format!("spawn_failed:{err}"))?;
        let _ = fs::remove_file(&manifest_path);

        if status.success() {
            Ok(())
        } else {
            Err(format!("runner_exit:{status}"))
        }
    }
}

fn default_simpler_dispatch_script() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(3)
        .unwrap_or_else(|| Path::new("."))
        .join("simulator")
        .join("scripts")
        .join("run_simpler_dispatch.sh")
}

fn load_binary_artifact(artifact: &BinaryArtifactRef) -> Result<Vec<u8>, String> {
    fs::read(&artifact.source)
        .map_err(|err| format!("artifact_read_failed:{}:{err}", artifact.source))
}

fn simpler_binary_fingerprint(binary: &[u8]) -> u64 {
    let mut hash = 0xcbf29ce484222325u64;
    for byte in binary {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash ^ binary.len() as u64
}

fn ensure_simpler_runtime_library(
    state: &mut SimplerCapiBackendState,
    artifact: &BinaryArtifactRef,
) -> Result<&'static simpler_capi::RuntimeLibrary, String> {
    let expected = PathBuf::from(&artifact.source);
    let path_changed = match state.runtime_library.as_ref() {
        Some(loaded) => loaded.path != expected,
        None => false,
    };
    if path_changed {
        state.device_allocs.clear();
        state.runtime_library = None;
    }
    if state.runtime_library.is_none() {
        let api = cached_simpler_runtime_library(&expected)?;
        state.runtime_library = Some(SimplerLoadedRuntimeLibrary {
            path: expected,
            api,
        });
    }
    Ok(&state.runtime_library.as_ref().expect("runtime library").api)
}

fn cached_simpler_runtime_library(
    expected: &Path,
) -> Result<&'static simpler_capi::RuntimeLibrary, String> {
    let cache = SIMPLER_RUNTIME_LIBRARY_CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    let mut cache = cache
        .lock()
        .map_err(|_| "simpler_capi_runtime_library_cache_poisoned".to_string())?;
    if let Some(api) = cache.get(expected).copied() {
        return Ok(api);
    }
    let api = simpler_capi::RuntimeLibrary::load(expected)
        .map_err(|err| format!("simpler_capi_load_runtime_library_failed:{err}"))?;
    let api = Box::leak(Box::new(api));
    cache.insert(expected.to_path_buf(), api);
    Ok(api)
}

fn with_simpler_device_context<T>(
    state: &mut SimplerCapiBackendState,
    device_id: i32,
    operation: impl FnOnce(
        &'static simpler_capi::RuntimeLibrary,
        &mut SharedSimplerDeviceContext,
    ) -> Result<T, String>,
) -> Result<T, String> {
    let api = state
        .runtime_library
        .as_ref()
        .ok_or_else(|| "simpler_capi_missing_runtime_library".to_string())?
        .api;
    let key = (
        api as *const simpler_capi::RuntimeLibrary as usize,
        device_id,
    );
    let cache = SIMPLER_DEVICE_CONTEXT_CACHE.get_or_init(|| Mutex::new(HashMap::new()));
    let mut cache = cache
        .lock()
        .map_err(|_| "simpler_capi_device_context_cache_poisoned".to_string())?;
    if !cache.contains_key(&key) {
        cache.insert(
            key,
            SharedSimplerDeviceContext {
                context: api
                    .create_context()
                    .map_err(|err| format!("simpler_capi_create_device_context_failed:{err}"))?,
                callable_ids: HashMap::new(),
                next_callable_id: 0,
            },
        );
    }
    operation(api, cache.get_mut(&key).expect("device context"))
}

#[derive(Debug)]
struct PreparedSimplerCapiArgs {
    task_args: simpler_capi::ChipStorageTaskArgs,
    signature: Vec<simpler_capi::ArgDirection>,
}

fn prepare_simpler_capi_args(
    profile: DispatchBackendProfile,
    runtime_args: &[SimplerRuntimeArg],
    host_payloads: &mut HostPayloadRegistry,
) -> Result<PreparedSimplerCapiArgs, String> {
    for arg in runtime_args {
        match arg {
            SimplerRuntimeArg::OutputSegment { endpoint, bytes }
            | SimplerRuntimeArg::InoutSegment { endpoint, bytes } => {
                let key = (endpoint.node, endpoint.segment);
                let payload = host_payloads
                    .segments
                    .entry(key)
                    .or_insert_with(|| vec![0u8; (endpoint.offset + *bytes) as usize]);
                let end = endpoint.offset as usize + *bytes as usize;
                if end > payload.len() {
                    payload.resize(end, 0);
                }
            }
            SimplerRuntimeArg::InputSegment { endpoint, bytes } => {
                let key = (endpoint.node, endpoint.segment);
                let payload = host_payloads
                    .segments
                    .get(&key)
                    .ok_or_else(|| "simpler_capi_missing_input_payload".to_string())?;
                let end = endpoint.offset as usize + *bytes as usize;
                if end > payload.len() {
                    return Err("simpler_capi_input_payload_too_short".to_string());
                }
            }
            SimplerRuntimeArg::ScalarU64(_) => {}
        }
    }

    let mut tensors = Vec::new();
    let mut scalars = Vec::new();
    let mut signature = Vec::new();
    for arg in runtime_args {
        match arg {
            SimplerRuntimeArg::ScalarU64(value) => {
                scalars.push(*value);
            }
            SimplerRuntimeArg::InputSegment { endpoint, bytes } => {
                let tensor_index = tensors.len();
                let payload = host_payloads
                    .segments
                    .get(&(endpoint.node, endpoint.segment))
                    .ok_or_else(|| "simpler_capi_missing_input_payload".to_string())?;
                let start = endpoint.offset as usize;
                let end = start.saturating_add(*bytes as usize);
                tensors.push(
                    simpler_capi::Tensor::new(
                        payload[start..end].as_ptr() as u64,
                        *bytes,
                        simpler_tensor_dtype(profile, tensor_index),
                    )
                    .map_err(|err| format!("simpler_capi_tensor_arg_failed:{err}"))?,
                );
                signature.push(simpler_capi::ArgDirection::In);
            }
            SimplerRuntimeArg::OutputSegment { endpoint, bytes } => {
                let tensor_index = tensors.len();
                let payload = host_payloads
                    .segments
                    .get_mut(&(endpoint.node, endpoint.segment))
                    .ok_or_else(|| "simpler_capi_missing_output_payload".to_string())?;
                let start = endpoint.offset as usize;
                let end = start.saturating_add(*bytes as usize);
                tensors.push(
                    simpler_capi::Tensor::new(
                        payload[start..end].as_mut_ptr() as u64,
                        *bytes,
                        simpler_tensor_dtype(profile, tensor_index),
                    )
                    .map_err(|err| format!("simpler_capi_tensor_arg_failed:{err}"))?,
                );
                signature.push(simpler_capi::ArgDirection::Out);
            }
            SimplerRuntimeArg::InoutSegment { endpoint, bytes } => {
                let tensor_index = tensors.len();
                let payload = host_payloads
                    .segments
                    .get_mut(&(endpoint.node, endpoint.segment))
                    .ok_or_else(|| "simpler_capi_missing_inout_payload".to_string())?;
                let start = endpoint.offset as usize;
                let end = start.saturating_add(*bytes as usize);
                tensors.push(
                    simpler_capi::Tensor::new(
                        payload[start..end].as_mut_ptr() as u64,
                        *bytes,
                        simpler_tensor_dtype(profile, tensor_index),
                    )
                    .map_err(|err| format!("simpler_capi_tensor_arg_failed:{err}"))?,
                );
                signature.push(simpler_capi::ArgDirection::Inout);
            }
        }
    }
    signature.extend(std::iter::repeat(simpler_capi::ArgDirection::Scalar).take(scalars.len()));
    let task_args = simpler_capi::ChipStorageTaskArgs::new(&tensors, &scalars)
        .map_err(|err| format!("simpler_capi_task_args_failed:{err}"))?;
    Ok(PreparedSimplerCapiArgs {
        task_args,
        signature,
    })
}

fn simpler_tensor_dtype(
    profile: DispatchBackendProfile,
    tensor_index: usize,
) -> simpler_capi::DataType {
    match profile {
        DispatchBackendProfile::HostMatmul => {
            if tensor_index < 3 {
                simpler_capi::DataType::Float16
            } else {
                simpler_capi::DataType::Float32
            }
        }
        DispatchBackendProfile::HostGemm => {
            if tensor_index < 2 {
                simpler_capi::DataType::Bfloat16
            } else {
                simpler_capi::DataType::Float32
            }
        }
        DispatchBackendProfile::HostQuantizedGemm => {
            if tensor_index < 2 {
                simpler_capi::DataType::Int8
            } else {
                simpler_capi::DataType::Int32
            }
        }
        DispatchBackendProfile::HostEngramContext => {
            if tensor_index == 1 {
                simpler_capi::DataType::Int32
            } else {
                simpler_capi::DataType::Float32
            }
        }
        DispatchBackendProfile::HostVector | DispatchBackendProfile::TmrbVector => {
            simpler_capi::DataType::Float32
        }
    }
}

fn validate_simpler_capi_dispatch_spec(
    backend_spec: Option<&DispatchBackendSpec>,
    request: Option<&BackendExecutionRequest>,
) -> Result<(), String> {
    let backend_spec = backend_spec.ok_or_else(|| "missing_backend_spec".to_string())?;
    let runtime = backend_spec
        .simpler_runtime
        .as_ref()
        .ok_or_else(|| "missing_simpler_runtime_artifacts".to_string())?;
    if runtime.args.is_empty() {
        return Err("missing_simpler_runtime_args".to_string());
    }
    if let Some(request) = request {
        let request_bindings: Vec<_> = request
            .bindings
            .iter()
            .filter(|binding| !binding.resident)
            .collect();
        if request_bindings.is_empty() {
            return Err("missing_request_bindings".to_string());
        }
        let runtime_buffer_args = runtime
            .args
            .iter()
            .filter(|arg| {
                matches!(
                    arg,
                    SimplerRuntimeArg::InputSegment { .. }
                        | SimplerRuntimeArg::OutputSegment { .. }
                        | SimplerRuntimeArg::InoutSegment { .. }
                )
            })
            .count();
        if request_bindings.len() != runtime_buffer_args {
            return Err(format!(
                "binding_arg_count_mismatch:{}:{runtime_buffer_args}",
                request_bindings.len()
            ));
        }
        for (binding, arg) in request_bindings
            .into_iter()
            .zip(runtime.args.iter().filter(|arg| {
                matches!(
                    arg,
                    SimplerRuntimeArg::InputSegment { .. }
                        | SimplerRuntimeArg::OutputSegment { .. }
                        | SimplerRuntimeArg::InoutSegment { .. }
                )
            }))
        {
            let (endpoint, bytes, usage_ok) = match arg {
                SimplerRuntimeArg::InputSegment { endpoint, bytes } => (
                    endpoint,
                    *bytes,
                    matches!(binding.usage, sim_core::BufferUsage::Input),
                ),
                SimplerRuntimeArg::OutputSegment { endpoint, bytes } => (
                    endpoint,
                    *bytes,
                    matches!(binding.usage, sim_core::BufferUsage::Output),
                ),
                SimplerRuntimeArg::InoutSegment { endpoint, bytes } => (
                    endpoint,
                    *bytes,
                    matches!(binding.usage, sim_core::BufferUsage::Inout),
                ),
                SimplerRuntimeArg::ScalarU64(_) => unreachable!(),
            };
            if !usage_ok {
                return Err(format!("binding_usage_mismatch:{}", binding.name));
            }
            if binding.endpoint != *endpoint {
                return Err(format!("binding_endpoint_mismatch:{}", binding.name));
            }
            if binding.bytes != bytes {
                return Err(format!("binding_size_mismatch:{}", binding.name));
            }
        }
    }
    Ok(())
}

fn simpler_dispatch_manifest(
    op_id: OpId,
    op: &RuntimeOpRecord,
    backend_spec: Option<&DispatchBackendSpec>,
) -> SimplerDispatchManifest {
    SimplerDispatchManifest {
        version: 1,
        op_id,
        task: op.task.task_id.to_string(),
        request_id: op.request_id.clone(),
        trace_id: op.trace_id.clone(),
        plan_id: op.plan_id.clone(),
        step_id: op.step_id.clone(),
        step_kind: op.step_kind.clone(),
        device_context_id: op.device_context_id.clone(),
        runtime_context_id: op.runtime_context_id.clone(),
        function_name: op.function_name.clone(),
        target_level: op.target_level.map(|level| format!("{level:?}")),
        target_node: op.target_node,
        input_segment_count: op.input_segment_count,
        binding_count: op
            .request
            .as_ref()
            .map(|request| request.bindings.len())
            .unwrap_or(0),
        profile: backend_spec
            .map(|dispatch_spec| backend_profile_name(dispatch_spec.profile).into()),
        platform: backend_spec
            .map(|dispatch_spec| dispatch_spec.platform.clone())
            .unwrap_or_else(|| "a2a3sim".to_string()),
        runtime_variant: backend_spec
            .map(|dispatch_spec| runtime_variant_name(dispatch_spec.runtime_variant).into()),
        callable_hint: backend_spec.and_then(|dispatch_spec| dispatch_spec.callable_hint.clone()),
        block_hash: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.block_hash.clone())
        }),
        request_index: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.request_index)
        }),
        block_index: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.block_index)
        }),
        request_blocks_total: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.request_blocks_total)
        }),
        blocks_remaining_in_request: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.blocks_remaining_in_request)
        }),
        is_first_block_in_request: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.is_first_block_in_request)
            })
            .unwrap_or(false),
        is_last_block_in_request: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.is_last_block_in_request)
            })
            .unwrap_or(false),
        request_control_phase: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.request_control_phase.clone())
        }),
        request_control_epoch: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.request_control_epoch)
        }),
        request_control_result_kind: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.request_control_result_kind.clone())
        }),
        request_control_result_value: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.request_control_result_value)
        }),
        request_control_view_kind: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.request_control_view_kind.clone())
        }),
        kvcache_resolution_kind: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.kvcache_resolution_kind.clone())
        }),
        kvcache_view_kind: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.kvcache_view_kind.clone())
        }),
        logical_system_id: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.logical_system_id)
        }),
        scope_depth: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.scope_depth)
        }),
        prefix_group: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.prefix_group)
        }),
        route_from_level: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.route_from_level.clone())
        }),
        route_to_level: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.route_to_level.clone())
        }),
        route_selected_node: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.route_selected_node)
        }),
        route_reason: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.route_reason.clone())
        }),
        placement_level: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.placement_level.clone())
        }),
        placement_node: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.placement_node)
        }),
        capacity_pressure_active: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.capacity_pressure_active)
            })
            .unwrap_or(false),
        evictions_seen: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.evictions_seen)
            })
            .unwrap_or(0),
        block_writebacks_seen: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.block_writebacks_seen)
            })
            .unwrap_or(0),
        promoted_this_access: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.promoted_this_access)
            })
            .unwrap_or(false),
        reloaded_after_eviction: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.reloaded_after_eviction)
            })
            .unwrap_or(false),
        uses_dfs_fallback: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.uses_dfs_fallback)
            })
            .unwrap_or(false),
        includes_request_control: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.includes_request_control)
            })
            .unwrap_or(false),
        includes_prefix_shared: backend_spec
            .and_then(|dispatch_spec| {
                dispatch_spec
                    .context
                    .as_ref()
                    .map(|context| context.includes_prefix_shared)
            })
            .unwrap_or(false),
        hot_segment: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.hot_segment)
        }),
        request_segment: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.request_segment)
        }),
        control_segment: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.control_segment)
        }),
        prefix_segment: backend_spec.and_then(|dispatch_spec| {
            dispatch_spec
                .context
                .as_ref()
                .and_then(|context| context.prefix_segment)
        }),
    }
}

fn write_simpler_dispatch_manifest(
    op_id: OpId,
    manifest: &SimplerDispatchManifest,
) -> Result<PathBuf, String> {
    let path = std::env::temp_dir().join(format!("simpler-dispatch-{op_id}.env"));
    let mut content = String::new();
    content.push_str("MANIFEST_VERSION=");
    content.push_str(&manifest.version.to_string());
    content.push('\n');
    content.push_str("OP_ID=");
    content.push_str(&manifest.op_id.to_string());
    content.push('\n');
    content.push_str("TASK_ID=");
    content.push_str(&manifest.task);
    content.push('\n');
    if let Some(request_id) = manifest.request_id.as_deref() {
        content.push_str("REQUEST_ID=");
        content.push_str(request_id);
        content.push('\n');
    }
    if let Some(trace_id) = manifest.trace_id.as_deref() {
        content.push_str("TRACE_ID=");
        content.push_str(trace_id);
        content.push('\n');
    }
    if let Some(plan_id) = manifest.plan_id.as_deref() {
        content.push_str("PLAN_ID=");
        content.push_str(plan_id);
        content.push('\n');
    }
    if let Some(step_id) = manifest.step_id.as_deref() {
        content.push_str("STEP_ID=");
        content.push_str(step_id);
        content.push('\n');
    }
    if let Some(step_kind) = manifest.step_kind.as_deref() {
        content.push_str("STEP_KIND=");
        content.push_str(step_kind);
        content.push('\n');
    }
    if let Some(device_context_id) = manifest.device_context_id.as_deref() {
        content.push_str("DEVICE_CONTEXT_ID=");
        content.push_str(device_context_id);
        content.push('\n');
    }
    if let Some(runtime_context_id) = manifest.runtime_context_id.as_deref() {
        content.push_str("RUNTIME_CONTEXT_ID=");
        content.push_str(runtime_context_id);
        content.push('\n');
    }
    if let Some(function_name) = manifest.function_name.as_deref() {
        content.push_str("FUNCTION_NAME=");
        content.push_str(function_name);
        content.push('\n');
    }
    if let Some(target_level) = manifest.target_level.as_deref() {
        content.push_str("TARGET_LEVEL=");
        content.push_str(target_level);
        content.push('\n');
    }
    if let Some(target_node) = manifest.target_node {
        content.push_str("TARGET_NODE=");
        content.push_str(&target_node.to_string());
        content.push('\n');
    }
    content.push_str("INPUT_SEGMENT_COUNT=");
    content.push_str(&manifest.input_segment_count.to_string());
    content.push('\n');
    content.push_str("BINDING_COUNT=");
    content.push_str(&manifest.binding_count.to_string());
    content.push('\n');
    if let Some(profile) = manifest.profile.as_deref() {
        content.push_str("PROFILE=");
        content.push_str(profile);
        content.push('\n');
    }
    content.push_str("PLATFORM=");
    content.push_str(&manifest.platform);
    content.push('\n');
    if let Some(runtime_variant) = manifest.runtime_variant.as_deref() {
        content.push_str("RUNTIME_VARIANT=");
        content.push_str(runtime_variant);
        content.push('\n');
    }
    if let Some(callable_hint) = manifest.callable_hint.as_deref() {
        content.push_str("CALLABLE_HINT=");
        content.push_str(callable_hint);
        content.push('\n');
    }
    if let Some(block_hash) = manifest.block_hash.as_deref() {
        content.push_str("BLOCK_HASH=");
        content.push_str(block_hash);
        content.push('\n');
    }
    if let Some(request_index) = manifest.request_index {
        content.push_str("REQUEST_INDEX=");
        content.push_str(&request_index.to_string());
        content.push('\n');
    }
    if let Some(block_index) = manifest.block_index {
        content.push_str("BLOCK_INDEX=");
        content.push_str(&block_index.to_string());
        content.push('\n');
    }
    if let Some(request_blocks_total) = manifest.request_blocks_total {
        content.push_str("REQUEST_BLOCKS_TOTAL=");
        content.push_str(&request_blocks_total.to_string());
        content.push('\n');
    }
    if let Some(blocks_remaining_in_request) = manifest.blocks_remaining_in_request {
        content.push_str("BLOCKS_REMAINING_IN_REQUEST=");
        content.push_str(&blocks_remaining_in_request.to_string());
        content.push('\n');
    }
    content.push_str("IS_FIRST_BLOCK_IN_REQUEST=");
    content.push_str(if manifest.is_first_block_in_request {
        "1"
    } else {
        "0"
    });
    content.push('\n');
    content.push_str("IS_LAST_BLOCK_IN_REQUEST=");
    content.push_str(if manifest.is_last_block_in_request {
        "1"
    } else {
        "0"
    });
    content.push('\n');
    if let Some(request_control_phase) = manifest.request_control_phase.as_deref() {
        content.push_str("REQUEST_CONTROL_PHASE=");
        content.push_str(request_control_phase);
        content.push('\n');
    }
    if let Some(request_control_epoch) = manifest.request_control_epoch {
        content.push_str("REQUEST_CONTROL_EPOCH=");
        content.push_str(&request_control_epoch.to_string());
        content.push('\n');
    }
    if let Some(request_control_result_kind) = manifest.request_control_result_kind.as_deref() {
        content.push_str("REQUEST_CONTROL_RESULT_KIND=");
        content.push_str(request_control_result_kind);
        content.push('\n');
    }
    if let Some(request_control_result_value) = manifest.request_control_result_value {
        content.push_str("REQUEST_CONTROL_RESULT_VALUE=");
        content.push_str(&request_control_result_value.to_string());
        content.push('\n');
    }
    if let Some(request_control_view_kind) = manifest.request_control_view_kind.as_deref() {
        content.push_str("REQUEST_CONTROL_VIEW_KIND=");
        content.push_str(request_control_view_kind);
        content.push('\n');
    }
    if let Some(kvcache_resolution_kind) = manifest.kvcache_resolution_kind.as_deref() {
        content.push_str("KVCACHE_RESOLUTION_KIND=");
        content.push_str(kvcache_resolution_kind);
        content.push('\n');
    }
    if let Some(kvcache_view_kind) = manifest.kvcache_view_kind.as_deref() {
        content.push_str("KVCACHE_VIEW_KIND=");
        content.push_str(kvcache_view_kind);
        content.push('\n');
    }
    if let Some(logical_system_id) = manifest.logical_system_id {
        content.push_str("LOGICAL_SYSTEM_ID=");
        content.push_str(&logical_system_id.to_string());
        content.push('\n');
    }
    if let Some(scope_depth) = manifest.scope_depth {
        content.push_str("SCOPE_DEPTH=");
        content.push_str(&scope_depth.to_string());
        content.push('\n');
    }
    if let Some(prefix_group) = manifest.prefix_group {
        content.push_str("PREFIX_GROUP=");
        content.push_str(&prefix_group.to_string());
        content.push('\n');
    }
    if let Some(route_from_level) = manifest.route_from_level.as_deref() {
        content.push_str("ROUTE_FROM_LEVEL=");
        content.push_str(route_from_level);
        content.push('\n');
    }
    if let Some(route_to_level) = manifest.route_to_level.as_deref() {
        content.push_str("ROUTE_TO_LEVEL=");
        content.push_str(route_to_level);
        content.push('\n');
    }
    if let Some(route_selected_node) = manifest.route_selected_node {
        content.push_str("ROUTE_SELECTED_NODE=");
        content.push_str(&route_selected_node.to_string());
        content.push('\n');
    }
    if let Some(route_reason) = manifest.route_reason.as_deref() {
        content.push_str("ROUTE_REASON=");
        content.push_str(route_reason);
        content.push('\n');
    }
    if let Some(placement_level) = manifest.placement_level.as_deref() {
        content.push_str("PLACEMENT_LEVEL=");
        content.push_str(placement_level);
        content.push('\n');
    }
    if let Some(placement_node) = manifest.placement_node {
        content.push_str("PLACEMENT_NODE=");
        content.push_str(&placement_node.to_string());
        content.push('\n');
    }
    content.push_str("CAPACITY_PRESSURE_ACTIVE=");
    content.push_str(if manifest.capacity_pressure_active {
        "1"
    } else {
        "0"
    });
    content.push('\n');
    content.push_str("EVICTIONS_SEEN=");
    content.push_str(&manifest.evictions_seen.to_string());
    content.push('\n');
    content.push_str("BLOCK_WRITEBACKS_SEEN=");
    content.push_str(&manifest.block_writebacks_seen.to_string());
    content.push('\n');
    content.push_str("PROMOTED_THIS_ACCESS=");
    content.push_str(if manifest.promoted_this_access {
        "1"
    } else {
        "0"
    });
    content.push('\n');
    content.push_str("RELOADED_AFTER_EVICTION=");
    content.push_str(if manifest.reloaded_after_eviction {
        "1"
    } else {
        "0"
    });
    content.push('\n');
    content.push_str("USES_DFS_FALLBACK=");
    content.push_str(if manifest.uses_dfs_fallback { "1" } else { "0" });
    content.push('\n');
    content.push_str("INCLUDES_REQUEST_CONTROL=");
    content.push_str(if manifest.includes_request_control {
        "1"
    } else {
        "0"
    });
    content.push('\n');
    content.push_str("INCLUDES_PREFIX_SHARED=");
    content.push_str(if manifest.includes_prefix_shared {
        "1"
    } else {
        "0"
    });
    content.push('\n');
    if let Some(hot_segment) = manifest.hot_segment {
        content.push_str("HOT_SEGMENT=");
        content.push_str(&hot_segment.to_string());
        content.push('\n');
    }
    if let Some(request_segment) = manifest.request_segment {
        content.push_str("REQUEST_SEGMENT=");
        content.push_str(&request_segment.to_string());
        content.push('\n');
    }
    if let Some(control_segment) = manifest.control_segment {
        content.push_str("CONTROL_SEGMENT=");
        content.push_str(&control_segment.to_string());
        content.push('\n');
    }
    if let Some(prefix_segment) = manifest.prefix_segment {
        content.push_str("PREFIX_SEGMENT=");
        content.push_str(&prefix_segment.to_string());
        content.push('\n');
    }
    fs::write(&path, content).map_err(|err| format!("manifest_write_failed:{err}"))?;
    Ok(path)
}

fn backend_profile_name(profile: DispatchBackendProfile) -> &'static str {
    match profile {
        DispatchBackendProfile::HostVector => "host_vector",
        DispatchBackendProfile::TmrbVector => "tmrb_vector",
        DispatchBackendProfile::HostMatmul => "host_matmul",
        DispatchBackendProfile::HostGemm => "host_gemm",
        DispatchBackendProfile::HostQuantizedGemm => "host_quantized_gemm",
        DispatchBackendProfile::HostEngramContext => "host_engram_context",
    }
}

fn runtime_variant_name(runtime_variant: DispatchRuntimeVariant) -> &'static str {
    match runtime_variant {
        DispatchRuntimeVariant::HostBuildGraph => "host_build_graph",
        DispatchRuntimeVariant::TensormapAndRingbuffer => "tensormap_and_ringbuffer",
    }
}

fn validate_simpler_dispatch_spec(
    backend_spec: Option<&DispatchBackendSpec>,
) -> Result<(), String> {
    if let Some(spec) = backend_spec {
        if spec.platform != "a2a3sim" {
            return Err(format!("unsupported_platform:{}", spec.platform));
        }
    }

    if let Some(dispatch_spec) = backend_spec {
        match (&dispatch_spec.profile, &dispatch_spec.runtime_variant) {
            (
                DispatchBackendProfile::TmrbVector,
                DispatchRuntimeVariant::TensormapAndRingbuffer,
            )
            | (DispatchBackendProfile::HostMatmul, DispatchRuntimeVariant::HostBuildGraph)
            | (DispatchBackendProfile::HostGemm, DispatchRuntimeVariant::HostBuildGraph)
            | (DispatchBackendProfile::HostQuantizedGemm, DispatchRuntimeVariant::HostBuildGraph)
            | (DispatchBackendProfile::HostEngramContext, DispatchRuntimeVariant::HostBuildGraph)
            | (DispatchBackendProfile::HostVector, DispatchRuntimeVariant::HostBuildGraph) => {}
            (profile, runtime_variant) => {
                return Err(format!(
                    "unsupported_runtime_variant:{profile:?}:{runtime_variant:?}"
                ));
            }
        }
    }

    Ok(())
}

impl LocalRuntimeEngine {
    pub fn from_config(config: &ScenarioConfig) -> Self {
        let mut engine = Self::with_policy(
            config
                .pypto
                .simpler_boundary
                .dispatch_latency_us
                .unwrap_or(1),
            config.levels.l3_host_tier.fetch_latency_us.unwrap_or(1),
            config.levels.l4_domain_tier.fetch_latency_us.unwrap_or(80),
            (config.topology.hosts * config.topology.ubpus_per_host) as usize,
            1,
        );
        engine.backend_mode = match config.pypto.simpler_boundary.chip_backend_mode.as_str() {
            "simpler_process" => ChipBackendMode::SimplerProcess,
            "simpler_capi" => ChipBackendMode::SimplerCapi,
            _ => ChipBackendMode::LocalRuntime,
        };
        engine
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
            backend_mode: ChipBackendMode::LocalRuntime,
            dispatch_latency_us,
            copy_latency_us,
            timeout_us,
            max_inflight,
            submission_queue: SharedRuntimeExecutor::with_policy(0, 0, max_inflight, max_retries),
            inflight: Vec::new(),
            completed: VecDeque::new(),
            simpler_capi: SimplerCapiBackendState::default(),
            host_payloads: HostPayloadRegistry::default(),
            execution_contexts: ExecutionContextRegistry::default(),
        }
    }

    pub fn now(&self) -> SimTimestamp {
        self.now
    }

    pub fn seed_host_segment(
        &mut self,
        node: NodeId,
        segment: sim_core::SegmentHandle,
        bytes: Vec<u8>,
    ) {
        self.host_payloads.segments.insert((node, segment), bytes);
    }

    pub fn host_segment_payload(
        &self,
        node: NodeId,
        segment: sim_core::SegmentHandle,
    ) -> Option<&[u8]> {
        self.host_payloads
            .segments
            .get(&(node, segment))
            .map(Vec::as_slice)
    }

    pub fn device_context_snapshot(&self, id: &str) -> Option<DeviceContextSnapshot> {
        self.execution_contexts
            .device_contexts
            .get(id)
            .map(|record| DeviceContextSnapshot {
                id: record.id.clone(),
                state: record.state,
                generation: record.generation,
                warm: record.warm,
                reusable: record.reusable,
                created_at: record.created_at,
                last_used_at: record.last_used_at,
                dispatch_count: record.dispatch_count,
                reset_count: record.reset_count,
                teardown_count: record.teardown_count,
            })
    }

    pub fn runtime_context_snapshot(&self, id: &str) -> Option<RuntimeContextSnapshot> {
        self.execution_contexts
            .runtime_contexts
            .get(id)
            .map(|record| RuntimeContextSnapshot {
                id: record.id.clone(),
                device_context_id: record.device_context_id.clone(),
                state: record.state,
                generation: record.generation,
                warm: record.warm,
                reusable: record.reusable,
                created_at: record.created_at,
                last_used_at: record.last_used_at,
                dispatch_count: record.dispatch_count,
                reset_count: record.reset_count,
                teardown_count: record.teardown_count,
                resident_binding_count: record.resident_bindings.len(),
            })
    }

    pub fn apply_execution_context_command(
        &mut self,
        cmd: ExecutionContextCommand,
    ) -> Result<(), sim_core::SimError> {
        let request = BackendExecutionRequest {
            correlation: sim_core::RequestCorrelation {
                request_id: format!("context-cmd-{}", self.next_op_id.saturating_add(1)),
                trace_id: None,
                op_name: Some("execution_context_command".into()),
                step_index: None,
                sequence_no: None,
            },
            plan: None,
            context: Some(sim_core::ExecutionContextRef {
                device_context_id: cmd.device_context_id,
                runtime_context_id: cmd.runtime_context_id,
                lifecycle: cmd.lifecycle,
                warm: cmd.warm,
                reusable: cmd.reusable,
            }),
            bindings: Vec::new(),
        };
        self.touch_execution_contexts(Some(&request))
    }

    fn update_resident_bindings(
        &mut self,
        request: &BackendExecutionRequest,
    ) -> Result<(), sim_core::SimError> {
        let Some(context) = request.context.as_ref() else {
            return Ok(());
        };
        let Some(runtime_context_id) = context.runtime_context_id.as_ref() else {
            return Ok(());
        };
        let Some(runtime_context) = self
            .execution_contexts
            .runtime_contexts
            .get_mut(runtime_context_id)
        else {
            return Ok(());
        };

        match context.lifecycle {
            sim_core::ExecutionLifecycle::Reset | sim_core::ExecutionLifecycle::Teardown => {
                runtime_context.resident_bindings.clear();
                return Ok(());
            }
            _ => {}
        }

        for binding in request.bindings.iter().filter(|binding| binding.resident) {
            if let Some(existing) = runtime_context.resident_bindings.get(&binding.name) {
                if existing.endpoint != binding.endpoint
                    || existing.bytes != binding.bytes
                    || existing.dtype != binding.dtype
                    || existing.shape != binding.shape
                    || existing.layout != binding.layout
                    || existing.strides != binding.strides
                {
                    return Err(sim_core::SimError::InvalidInput(
                        "resident_binding_mismatch",
                    ));
                }
            } else {
                runtime_context
                    .resident_bindings
                    .insert(binding.name.clone(), binding.clone());
            }
        }

        Ok(())
    }

    fn touch_execution_contexts(
        &mut self,
        request: Option<&BackendExecutionRequest>,
    ) -> Result<(), sim_core::SimError> {
        let Some(request) = request else {
            return Ok(());
        };
        let Some(context) = request.context.as_ref() else {
            return Ok(());
        };

        let device_context = self
            .execution_contexts
            .device_contexts
            .entry(context.device_context_id.clone())
            .or_insert_with(|| DeviceContextRecord {
                id: context.device_context_id.clone(),
                state: ContextState::Active,
                generation: 0,
                warm: false,
                reusable: context.reusable,
                created_at: self.now,
                last_used_at: self.now,
                dispatch_count: 0,
                reset_count: 0,
                teardown_count: 0,
            });

        match context.lifecycle {
            sim_core::ExecutionLifecycle::Init => {
                if device_context.state == ContextState::Closed {
                    device_context.state = ContextState::Active;
                    device_context.generation = device_context.generation.saturating_add(1);
                }
                device_context.warm = context.warm;
            }
            sim_core::ExecutionLifecycle::Warmup => {
                if device_context.state != ContextState::Active {
                    return Err(sim_core::SimError::InvalidInput(
                        "device_context_not_active_for_warmup",
                    ));
                }
                device_context.warm = true;
            }
            sim_core::ExecutionLifecycle::Reuse => {
                if device_context.state != ContextState::Active {
                    return Err(sim_core::SimError::InvalidInput(
                        "device_context_not_active_for_reuse",
                    ));
                }
            }
            sim_core::ExecutionLifecycle::Reset => {
                if device_context.state != ContextState::Active {
                    return Err(sim_core::SimError::InvalidInput(
                        "device_context_not_active_for_reset",
                    ));
                }
                device_context.generation = device_context.generation.saturating_add(1);
                device_context.reset_count = device_context.reset_count.saturating_add(1);
                device_context.warm = false;
            }
            sim_core::ExecutionLifecycle::Teardown => {
                if device_context.state != ContextState::Active {
                    return Err(sim_core::SimError::InvalidInput(
                        "device_context_not_active_for_teardown",
                    ));
                }
                device_context.state = ContextState::Closed;
                device_context.teardown_count = device_context.teardown_count.saturating_add(1);
                device_context.warm = false;
            }
        }
        device_context.reusable = context.reusable;
        device_context.last_used_at = self.now;
        device_context.dispatch_count = device_context.dispatch_count.saturating_add(1);

        if let Some(runtime_context_id) = context.runtime_context_id.as_ref() {
            let runtime_context = self
                .execution_contexts
                .runtime_contexts
                .entry(runtime_context_id.clone())
                .or_insert_with(|| RuntimeContextRecord {
                    id: runtime_context_id.clone(),
                    device_context_id: context.device_context_id.clone(),
                    state: ContextState::Active,
                    generation: 0,
                    warm: false,
                    reusable: context.reusable,
                    created_at: self.now,
                    last_used_at: self.now,
                    dispatch_count: 0,
                    reset_count: 0,
                    teardown_count: 0,
                    resident_bindings: HashMap::new(),
                });
            if runtime_context.device_context_id != context.device_context_id {
                return Err(sim_core::SimError::InvalidInput(
                    "runtime_context_device_mismatch",
                ));
            }
            match context.lifecycle {
                sim_core::ExecutionLifecycle::Init => {
                    if runtime_context.state == ContextState::Closed {
                        runtime_context.state = ContextState::Active;
                        runtime_context.generation = runtime_context.generation.saturating_add(1);
                    }
                    runtime_context.warm = context.warm;
                }
                sim_core::ExecutionLifecycle::Warmup => {
                    if runtime_context.state != ContextState::Active {
                        return Err(sim_core::SimError::InvalidInput(
                            "runtime_context_not_active_for_warmup",
                        ));
                    }
                    runtime_context.warm = true;
                }
                sim_core::ExecutionLifecycle::Reuse => {
                    if runtime_context.state != ContextState::Active {
                        return Err(sim_core::SimError::InvalidInput(
                            "runtime_context_not_active_for_reuse",
                        ));
                    }
                }
                sim_core::ExecutionLifecycle::Reset => {
                    if runtime_context.state != ContextState::Active {
                        return Err(sim_core::SimError::InvalidInput(
                            "runtime_context_not_active_for_reset",
                        ));
                    }
                    runtime_context.generation = runtime_context.generation.saturating_add(1);
                    runtime_context.reset_count = runtime_context.reset_count.saturating_add(1);
                    runtime_context.warm = false;
                    runtime_context.resident_bindings.clear();
                }
                sim_core::ExecutionLifecycle::Teardown => {
                    if runtime_context.state != ContextState::Active {
                        return Err(sim_core::SimError::InvalidInput(
                            "runtime_context_not_active_for_teardown",
                        ));
                    }
                    runtime_context.state = ContextState::Closed;
                    runtime_context.teardown_count =
                        runtime_context.teardown_count.saturating_add(1);
                    runtime_context.warm = false;
                    runtime_context.resident_bindings.clear();
                }
            }
            runtime_context.reusable = context.reusable;
            runtime_context.last_used_at = self.now;
            runtime_context.dispatch_count = runtime_context.dispatch_count.saturating_add(1);
        }

        Ok(())
    }

    fn validate_execution_request(
        request: &BackendExecutionRequest,
    ) -> Result<(), sim_core::SimError> {
        if request.bindings.is_empty() {
            return Err(sim_core::SimError::InvalidInput("missing_request_bindings"));
        }

        let mut binding_names = HashSet::new();
        for binding in &request.bindings {
            if binding.bytes == 0 {
                return Err(sim_core::SimError::InvalidInput("binding_zero_bytes"));
            }
            if !binding_names.insert(binding.name.as_str()) {
                return Err(sim_core::SimError::InvalidInput(
                    "duplicate_request_binding_name",
                ));
            }

            match binding.layout {
                sim_core::TensorLayout::Contiguous => {
                    if binding.strides.is_some() {
                        return Err(sim_core::SimError::InvalidInput(
                            "contiguous_binding_has_strides",
                        ));
                    }
                    if let Some(byte_width) = binding.dtype.byte_width() {
                        let elem_count = binding
                            .shape
                            .iter()
                            .try_fold(1u64, |acc, dim| acc.checked_mul(*dim))
                            .ok_or(sim_core::SimError::InvalidInput("binding_shape_overflow"))?;
                        let expected_bytes = elem_count
                            .checked_mul(byte_width)
                            .ok_or(sim_core::SimError::InvalidInput("binding_size_overflow"))?;
                        if expected_bytes != binding.bytes {
                            return Err(sim_core::SimError::InvalidInput(
                                "binding_bytes_shape_mismatch",
                            ));
                        }
                    }
                }
                sim_core::TensorLayout::Strided => {
                    let Some(strides) = binding.strides.as_ref() else {
                        return Err(sim_core::SimError::InvalidInput(
                            "strided_binding_missing_strides",
                        ));
                    };
                    if strides.len() != binding.shape.len() {
                        return Err(sim_core::SimError::InvalidInput(
                            "strided_binding_rank_mismatch",
                        ));
                    }
                    if binding.dtype.byte_width().is_none() {
                        return Err(sim_core::SimError::InvalidInput(
                            "strided_binding_requires_typed_dtype",
                        ));
                    }
                }
                sim_core::TensorLayout::Opaque => {}
            }
        }

        Ok(())
    }

    fn infer_input_segments_from_request(
        request: &BackendExecutionRequest,
    ) -> Vec<sim_core::SegmentHandle> {
        let mut seen = HashSet::new();
        let mut segments = Vec::new();
        for binding in &request.bindings {
            if matches!(
                binding.usage,
                sim_core::BufferUsage::Input | sim_core::BufferUsage::Inout
            ) && !binding.resident
                && seen.insert(binding.endpoint.segment)
            {
                segments.push(binding.endpoint.segment);
            }
        }
        segments
    }

    pub fn submit_backend_dispatch(
        &mut self,
        op: BackendDispatchOperation,
        sink: &mut dyn EventSink,
    ) -> Result<DispatchHandle, sim_core::SimError> {
        let mut input_segments = Self::infer_input_segments_from_request(&op.request);
        if input_segments.is_empty() {
            input_segments = op.legacy_input_segments;
        }
        self.submit_dispatch(
            DispatchRequest {
                task: op.task,
                function: op.function,
                backend_spec: Some(op.backend_spec),
                request: Some(op.request),
                target_level: op.target_level,
                target_node: op.target_node,
                input_segments,
            },
            sink,
        )
    }

    fn simpler_alloc_for_segment(
        simpler_capi: &mut SimplerCapiBackendState,
        endpoint: MemoryEndpoint,
        bytes: u64,
    ) -> Result<SimplerDeviceAlloc, String> {
        let key = (endpoint.node, endpoint.segment);
        if let Some(existing) = simpler_capi.device_allocs.get(&key).copied() {
            if existing.bytes < bytes {
                return Err("simpler_capi_segment_too_small".to_string());
            }
            return Ok(existing);
        }
        let ptr = with_simpler_device_context(simpler_capi, 0, |api, worker| {
            api.alloc_device(&worker.context, bytes as usize)
                .map_err(|err| format!("simpler_capi_alloc_failed:{err}"))
        })?;
        let alloc = SimplerDeviceAlloc { ptr, bytes };
        simpler_capi.device_allocs.insert(key, alloc);
        Ok(alloc)
    }

    fn simpler_copy_completion(
        simpler_capi: &mut SimplerCapiBackendState,
        host_payloads: &mut HostPayloadRegistry,
        op: &RuntimeOpRecord,
        now: SimTimestamp,
    ) -> CompletionEvent {
        let Some(copy_req) = op.copy_req.as_ref() else {
            return CompletionEvent {
                op_id: op.op_id,
                task: Some(op.task.clone()),
                source: CompletionSource::ChipBackend,
                status: CompletionStatus::FatalFailure {
                    code: "simpler_capi_missing_copy_request".to_string(),
                },
                finished_at: now,
            };
        };

        let result = match copy_req.direction {
            CopyDirection::HostToDevice => {
                let alloc = match Self::simpler_alloc_for_segment(
                    simpler_capi,
                    copy_req.dst.clone(),
                    copy_req.bytes,
                ) {
                    Ok(alloc) => alloc,
                    Err(code) => {
                        return CompletionEvent {
                            op_id: op.op_id,
                            task: Some(op.task.clone()),
                            source: CompletionSource::ChipBackend,
                            status: CompletionStatus::FatalFailure { code },
                            finished_at: now,
                        }
                    }
                };
                let key = (copy_req.src.node, copy_req.src.segment);
                match host_payloads.segments.get(&key) {
                    None => Err("simpler_capi_missing_host_payload".to_string()),
                    Some(payload) => {
                        let start = copy_req.src.offset as usize;
                        let end = start.saturating_add(copy_req.bytes as usize);
                        if end > payload.len() {
                            Err("simpler_capi_host_payload_too_short".to_string())
                        } else {
                            with_simpler_device_context(simpler_capi, 0, |api, worker| {
                                api.host_to_device(
                                    &worker.context,
                                    alloc.ptr,
                                    payload[start..end].as_ptr() as *const _,
                                    copy_req.bytes as usize,
                                )
                                .map_err(|err| format!("simpler_capi_h2d_failed:{err}"))
                            })
                        }
                    }
                }
            }
            CopyDirection::DeviceToHost => {
                let key = (copy_req.src.node, copy_req.src.segment);
                let Some(alloc) = simpler_capi.device_allocs.get(&key).copied() else {
                    return CompletionEvent {
                        op_id: op.op_id,
                        task: Some(op.task.clone()),
                        source: CompletionSource::ChipBackend,
                        status: CompletionStatus::FatalFailure {
                            code: "simpler_capi_missing_device_allocation".to_string(),
                        },
                        finished_at: now,
                    };
                };
                let dst_key = (copy_req.dst.node, copy_req.dst.segment);
                let payload = host_payloads
                    .segments
                    .entry(dst_key)
                    .or_insert_with(|| vec![0u8; (copy_req.dst.offset + copy_req.bytes) as usize]);
                let start = copy_req.dst.offset as usize;
                let end = start.saturating_add(copy_req.bytes as usize);
                if end > payload.len() {
                    payload.resize(end, 0);
                }
                with_simpler_device_context(simpler_capi, 0, |api, worker| {
                    api.device_to_host(
                        &worker.context,
                        payload[start..end].as_mut_ptr() as *mut _,
                        alloc.ptr,
                        copy_req.bytes as usize,
                    )
                    .map_err(|err| format!("simpler_capi_d2h_failed:{err}"))
                })
            }
        };

        match result {
            Ok(()) => CompletionEvent {
                op_id: op.op_id,
                task: Some(op.task.clone()),
                source: CompletionSource::ChipBackend,
                status: CompletionStatus::Success,
                finished_at: op.ready_at,
            },
            Err(code) => CompletionEvent {
                op_id: op.op_id,
                task: Some(op.task.clone()),
                source: CompletionSource::ChipBackend,
                status: CompletionStatus::FatalFailure { code },
                finished_at: now,
            },
        }
    }

    fn simpler_dispatch_completion(
        simpler_capi: &mut SimplerCapiBackendState,
        host_payloads: &mut HostPayloadRegistry,
        op: &RuntimeOpRecord,
        now: SimTimestamp,
    ) -> CompletionEvent {
        let backend_spec = match op.backend_spec.as_ref() {
            Some(spec) => spec,
            None => {
                return CompletionEvent {
                    op_id: op.op_id,
                    task: Some(op.task.clone()),
                    source: CompletionSource::ChipBackend,
                    status: CompletionStatus::FatalFailure {
                        code: "missing_backend_spec".to_string(),
                    },
                    finished_at: now,
                }
            }
        };
        let runtime_artifacts = match backend_spec.simpler_runtime.as_ref() {
            Some(runtime) => runtime,
            None => {
                return CompletionEvent {
                    op_id: op.op_id,
                    task: Some(op.task.clone()),
                    source: CompletionSource::ChipBackend,
                    status: CompletionStatus::FatalFailure {
                        code: "missing_simpler_runtime_artifacts".to_string(),
                    },
                    finished_at: now,
                }
            }
        };

        let detail_timing = qwen3_dispatch_detail_timing_enabled();
        let detail_total_started = Instant::now();
        let mut detail_env_ms = 0u128;
        let mut detail_load_runtime_ms = 0u128;
        let detail_create_context_ms = 0u128;
        let mut detail_runtime_alloc_ms = 0u128;
        let mut detail_load_binary_ms = 0u128;
        let mut detail_prepare_args_ms = 0u128;
        let mut detail_make_callable_ms = 0u128;
        let mut detail_run_runtime_ms = 0u128;

        let result: Result<(), String> = (|| {
            let detail_started = Instant::now();
            let _runtime_env = EnvGuard::apply(&runtime_artifacts.runtime_env);
            detail_env_ms = detail_started.elapsed().as_millis();

            let detail_started = Instant::now();
            let api = ensure_simpler_runtime_library(
                simpler_capi,
                &runtime_artifacts.host_runtime_library,
            )?;
            detail_load_runtime_ms = detail_started.elapsed().as_millis();
            let detail_started = Instant::now();
            let runtime = simpler_capi::RuntimeBuffer::allocate(api)
                .map_err(|err| format!("simpler_capi_runtime_alloc_failed:{err}"))?;
            let runtime_handle = runtime.handle();
            detail_runtime_alloc_ms = detail_started.elapsed().as_millis();

            let detail_started = Instant::now();
            let orch_binary = load_binary_artifact(&runtime_artifacts.orch_shared_object)?;
            let aicpu_binary = match runtime_artifacts.aicpu_binary.as_ref() {
                Some(artifact) => load_binary_artifact(artifact)?,
                None => Vec::new(),
            };
            let aicore_binary = match runtime_artifacts.aicore_binary.as_ref() {
                Some(artifact) => load_binary_artifact(artifact)?,
                None => Vec::new(),
            };
            let mut kernel_binaries = Vec::with_capacity(runtime_artifacts.kernels.len());
            for kernel in &runtime_artifacts.kernels {
                kernel_binaries.push(load_binary_artifact(&kernel.binary)?);
            }
            detail_load_binary_ms = detail_started.elapsed().as_millis();

            let detail_started = Instant::now();
            let prepared = prepare_simpler_capi_args(
                backend_spec.profile,
                &runtime_artifacts.args,
                host_payloads,
            )?;
            detail_prepare_args_ms = detail_started.elapsed().as_millis();
            let kernel_inputs: Vec<simpler_capi::KernelCallableInput<'_>> = runtime_artifacts
                .kernels
                .iter()
                .zip(kernel_binaries.iter())
                .map(|(kernel, binary)| simpler_capi::KernelCallableInput {
                    func_id: kernel.func_id,
                    binary: binary.as_slice(),
                })
                .collect();
            let detail_started = Instant::now();
            let callable = simpler_capi::make_chip_callable(
                &runtime_artifacts.orch_function_name,
                &orch_binary,
                &kernel_inputs,
                &prepared.signature,
            )
            .map_err(|err| format!("simpler_capi_make_callable_failed:{err}"))?;
            detail_make_callable_ms = detail_started.elapsed().as_millis();
            let mut callable_key = runtime_artifacts.orch_shared_object.source.clone();
            callable_key.push('|');
            callable_key.push_str(&runtime_artifacts.orch_function_name);
            callable_key.push_str(&format!(
                ":{}:{:016x}",
                orch_binary.len(),
                simpler_binary_fingerprint(&orch_binary)
            ));
            for (kernel, binary) in runtime_artifacts.kernels.iter().zip(&kernel_binaries) {
                callable_key.push('|');
                callable_key.push_str(&kernel.func_id.to_string());
                callable_key.push(':');
                callable_key.push_str(&kernel.binary.source);
                callable_key.push_str(&format!(
                    ":{}:{:016x}",
                    binary.len(),
                    simpler_binary_fingerprint(binary)
                ));
            }

            let detail_started = Instant::now();
            with_simpler_device_context(simpler_capi, 0, |api, worker| {
                let (callable_id, prepare) = match worker.callable_ids.get(&callable_key) {
                    Some(callable_id) => (*callable_id, false),
                    None => {
                        if worker.next_callable_id >= 64 {
                            return Err("simpler_capi_callable_cache_full".to_string());
                        }
                        (worker.next_callable_id, true)
                    }
                };
                api.run_prepared_callable(
                    &worker.context,
                    runtime_handle,
                    &callable,
                    &prepared.task_args,
                    callable_id,
                    prepare,
                    runtime_artifacts.launch.block_dim as i32,
                    runtime_artifacts.launch.aicpu_thread_num as i32,
                    runtime_artifacts.launch.device_id as i32,
                    if aicpu_binary.is_empty() {
                        std::ptr::null()
                    } else {
                        aicpu_binary.as_ptr()
                    },
                    aicpu_binary.len(),
                    if aicore_binary.is_empty() {
                        std::ptr::null()
                    } else {
                        aicore_binary.as_ptr()
                    },
                    aicore_binary.len(),
                )
                .map_err(|err| format!("simpler_capi_run_callable_failed:{err}"))?;
                if prepare {
                    worker
                        .callable_ids
                        .insert(callable_key.clone(), callable_id);
                    worker.next_callable_id += 1;
                }
                Ok(())
            })?;
            detail_run_runtime_ms = detail_started.elapsed().as_millis();

            Ok(())
        })();

        if detail_timing {
            qwen3_dispatch_detail_log_line(&format!(
                "qwen3-runtime-detail: op_id={} task_id={} function={} request={} total_ms={} env_ms={} load_runtime_ms={} create_context_ms={} runtime_alloc_ms={} load_binary_ms={} prepare_args_ms={} make_callable_ms={} run_runtime_ms={}",
                op.op_id,
                op.task.task_id,
                op.function_name.as_deref().unwrap_or("-"),
                op.request_id.as_deref().unwrap_or("-"),
                detail_total_started.elapsed().as_millis(),
                detail_env_ms,
                detail_load_runtime_ms,
                detail_create_context_ms,
                detail_runtime_alloc_ms,
                detail_load_binary_ms,
                detail_prepare_args_ms,
                detail_make_callable_ms,
                detail_run_runtime_ms
            ));
        }

        match result {
            Ok(()) => CompletionEvent {
                op_id: op.op_id,
                task: Some(op.task.clone()),
                source: CompletionSource::ChipBackend,
                status: CompletionStatus::Success,
                finished_at: op.ready_at,
            },
            Err(code) => CompletionEvent {
                op_id: op.op_id,
                task: Some(op.task.clone()),
                source: CompletionSource::ChipBackend,
                status: CompletionStatus::FatalFailure { code },
                finished_at: now,
            },
        }
    }

    pub fn submit_dispatch(
        &mut self,
        req: DispatchRequest,
        sink: &mut dyn EventSink,
    ) -> Result<DispatchHandle, sim_core::SimError> {
        self.ensure_capacity()?;
        if let Some(request) = req.request.as_ref() {
            Self::validate_execution_request(request)?;
        }
        self.touch_execution_contexts(req.request.as_ref())?;
        if let Some(request) = req.request.as_ref() {
            self.update_resident_bindings(request)?;
        }
        let op_id = self.next_op();
        self.inflight.push(RuntimeOpRecord {
            op_id,
            kind: RuntimeOpKind::Dispatch,
            backend_spec: req.backend_spec.clone(),
            request: req.request.clone(),
            copy_req: None,
            task: req.task.clone(),
            function_name: Some(req.function.name.clone()),
            request_id: req
                .request
                .as_ref()
                .map(|request| request.correlation.request_id.clone()),
            trace_id: req
                .request
                .as_ref()
                .and_then(|request| request.correlation.trace_id.clone()),
            plan_id: req
                .request
                .as_ref()
                .and_then(|request| request.plan.as_ref())
                .map(|plan| plan.plan_id.clone()),
            step_id: req
                .request
                .as_ref()
                .and_then(|request| request.plan.as_ref())
                .map(|plan| plan.step_id.clone()),
            step_kind: req
                .request
                .as_ref()
                .and_then(|request| request.plan.as_ref())
                .map(|plan| format!("{:?}", plan.step_kind)),
            device_context_id: req
                .request
                .as_ref()
                .and_then(|request| request.context.as_ref())
                .map(|context| context.device_context_id.clone()),
            runtime_context_id: req
                .request
                .as_ref()
                .and_then(|request| request.context.as_ref())
                .and_then(|context| context.runtime_context_id.clone()),
            target_level: Some(req.target_level),
            target_node: Some(req.target_node),
            input_segment_count: req
                .request
                .as_ref()
                .map(|request| {
                    request
                        .bindings
                        .iter()
                        .filter(|binding| {
                            matches!(
                                binding.usage,
                                sim_core::BufferUsage::Input | sim_core::BufferUsage::Inout
                            ) && !binding.resident
                        })
                        .count()
                })
                .unwrap_or(req.input_segments.len()),
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
            backend_spec: None,
            request: None,
            copy_req: Some(req.clone()),
            task,
            function_name: None,
            request_id: None,
            trace_id: None,
            plan_id: None,
            step_id: None,
            step_kind: None,
            device_context_id: None,
            runtime_context_id: None,
            target_level: None,
            target_node: None,
            input_segment_count: 0,
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
        let backend_mode = self.backend_mode;
        let dispatch_latency_us = self.dispatch_latency_us;
        let copy_latency_us = self.copy_latency_us;
        let timeout_us = self.timeout_us;

        let _ =
            self.submission_queue.drive_ready(now, |ready| {
                if let Some(op) = self.inflight.iter_mut().find(|op| {
                    op.op_id == ready.payload.op_id && op.state == RuntimeOpState::Queued
                }) {
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
                let completion = match (backend_mode, op.kind) {
                    (
                        ChipBackendMode::SimplerCapi,
                        RuntimeOpKind::HostToDeviceCopy | RuntimeOpKind::DeviceToHostCopy,
                    ) => Self::simpler_copy_completion(
                        &mut self.simpler_capi,
                        &mut self.host_payloads,
                        op,
                        now,
                    ),
                    (ChipBackendMode::SimplerCapi, RuntimeOpKind::Dispatch) => {
                        let completion = match validate_simpler_capi_dispatch_spec(
                            op.backend_spec.as_ref(),
                            op.request.as_ref(),
                        ) {
                            Ok(()) => Self::simpler_dispatch_completion(
                                &mut self.simpler_capi,
                                &mut self.host_payloads,
                                op,
                                now,
                            ),
                            Err(code) => CompletionEvent {
                                op_id: op.op_id,
                                task: Some(op.task.clone()),
                                source: CompletionSource::ChipBackend,
                                status: CompletionStatus::FatalFailure { code },
                                finished_at: now,
                            },
                        };
                        if let CompletionStatus::RetryableFailure { code }
                        | CompletionStatus::FatalFailure { code } = &completion.status
                        {
                            sink.emit(SimEvent::RuntimeFailed {
                                at: now,
                                op_id: op.op_id,
                                reason: code.clone(),
                            });
                        }
                        completion
                    }
                    (ChipBackendMode::SimplerProcess, RuntimeOpKind::Dispatch) => {
                        let runner = SimplerProcessRunner::from_env();
                        match runner.run_dispatch_example(op.op_id, &op, op.backend_spec.as_ref()) {
                            Ok(()) => CompletionEvent {
                                op_id: op.op_id,
                                task: Some(op.task.clone()),
                                source: CompletionSource::ChipBackend,
                                status: CompletionStatus::Success,
                                finished_at: op.ready_at,
                            },
                            Err(code) => {
                                sink.emit(SimEvent::RuntimeFailed {
                                    at: now,
                                    op_id: op.op_id,
                                    reason: code.clone(),
                                });
                                CompletionEvent {
                                    op_id: op.op_id,
                                    task: Some(op.task.clone()),
                                    source: CompletionSource::ChipBackend,
                                    status: CompletionStatus::FatalFailure { code },
                                    finished_at: now,
                                }
                            }
                        }
                    }
                    _ => CompletionEvent {
                        op_id: op.op_id,
                        task: Some(op.task.clone()),
                        source: CompletionSource::ChipBackend,
                        status: CompletionStatus::Success,
                        finished_at: op.ready_at,
                    },
                };
                op.state = match &completion.status {
                    CompletionStatus::Success => RuntimeOpState::Completed,
                    CompletionStatus::RetryableFailure { .. }
                    | CompletionStatus::FatalFailure { .. } => RuntimeOpState::Failed,
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
        simpler_binary_fingerprint, ContextState, EvictionPlan, InMemoryBlockStore,
        LocalRuntimeEngine, PromotionPlan, RecursiveRoutePlanner, RoutePlanner, RouteRequest,
        RuntimeCompletionTracker, RuntimeOpKind, RuntimeOpState, SharedRuntimeQueue, SimBlockStore,
        VecEventSink,
    };
    use sim_config::ScenarioConfig;
    use sim_core::{
        BackendDispatchOperation, BackendExecutionRequest, BlockHash, BufferUsage, CompletionEvent,
        CompletionSource, CompletionStatus, CopyDirection, CopyRequest, DispatchBackendSpec,
        DispatchBufferBinding, DispatchRequest, ExecutionContextCommand, ExecutionContextRef,
        ExecutionLifecycle, ExecutionPlanRef, ExecutionStepKind, FunctionLabel, HierarchyCoord,
        LogicalSystemId, MemoryEndpoint, PlLevel, RequestCorrelation, SegmentHandle, SimEvent,
        TaskKey, TensorDType, TensorLayout,
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

    #[test]
    fn simpler_binary_fingerprint_changes_with_artifact_content() {
        assert_ne!(
            simpler_binary_fingerprint(b"same-path-geometry-a"),
            simpler_binary_fingerprint(b"same-path-geometry-b")
        );
    }

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
                    backend_spec: None,
                    request: None,
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
    fn runtime_engine_reuses_execution_contexts_across_dispatches() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let request = |lifecycle| BackendExecutionRequest {
            correlation: RequestCorrelation {
                request_id: "req-1".into(),
                trace_id: Some("trace-1".into()),
                op_name: Some("decode_step".into()),
                step_index: Some(0),
                sequence_no: Some(1),
            },
            plan: None,
            context: Some(ExecutionContextRef {
                device_context_id: "device-ctx-0".into(),
                runtime_context_id: Some("runtime-ctx-0".into()),
                lifecycle,
                warm: true,
                reusable: true,
            }),
            bindings: vec![DispatchBufferBinding {
                name: "input".into(),
                usage: BufferUsage::Input,
                endpoint: MemoryEndpoint {
                    node: 0,
                    segment: SegmentHandle(1),
                    offset: 0,
                },
                bytes: 4096,
                dtype: TensorDType::Opaque,
                shape: vec![4096],
                layout: TensorLayout::Opaque,
                strides: None,
                resident: false,
            }],
        };

        for (task_id, lifecycle) in [
            (71u64, ExecutionLifecycle::Init),
            (72u64, ExecutionLifecycle::Reuse),
        ] {
            runtime
                .submit_dispatch(
                    DispatchRequest {
                        task: TaskKey {
                            logical_system: LogicalSystemId(1),
                            coord: HierarchyCoord { levels: [0; 8] },
                            scope_depth: 0,
                            task_id,
                        },
                        function: FunctionLabel {
                            name: "decode_step".into(),
                            level: PlLevel::L4,
                        },
                        backend_spec: None,
                        request: Some(request(lifecycle)),
                        target_level: PlLevel::L4,
                        target_node: 19,
                        input_segments: vec![SegmentHandle(1)],
                    },
                    &mut sink,
                )
                .expect("dispatch");
        }

        let device = runtime
            .device_context_snapshot("device-ctx-0")
            .expect("device context");
        let runtime_ctx = runtime
            .runtime_context_snapshot("runtime-ctx-0")
            .expect("runtime context");

        assert_eq!(device.dispatch_count, 2);
        assert_eq!(runtime_ctx.dispatch_count, 2);
        assert_eq!(runtime_ctx.device_context_id, "device-ctx-0");
        assert!(device.warm);
        assert!(runtime_ctx.reusable);
        assert_eq!(device.state, ContextState::Active);
        assert_eq!(runtime_ctx.state, ContextState::Active);
    }

    #[test]
    fn runtime_engine_submit_backend_dispatch_infers_input_segments() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let handle = runtime
            .submit_backend_dispatch(
                BackendDispatchOperation {
                    task: TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 73,
                    },
                    function: FunctionLabel {
                        name: "decode_step".into(),
                        level: PlLevel::L4,
                    },
                    backend_spec: DispatchBackendSpec {
                        profile: sim_core::DispatchBackendProfile::HostVector,
                        platform: "test".into(),
                        runtime_variant: sim_core::DispatchRuntimeVariant::HostBuildGraph,
                        callable_hint: None,
                        context: None,
                        simpler_runtime: None,
                    },
                    request: BackendExecutionRequest {
                        correlation: RequestCorrelation {
                            request_id: "req-backend".into(),
                            trace_id: None,
                            op_name: Some("decode_step".into()),
                            step_index: Some(0),
                            sequence_no: Some(1),
                        },
                        plan: None,
                        context: Some(ExecutionContextRef {
                            device_context_id: "device-ctx-backend".into(),
                            runtime_context_id: Some("runtime-ctx-backend".into()),
                            lifecycle: ExecutionLifecycle::Init,
                            warm: true,
                            reusable: true,
                        }),
                        bindings: vec![
                            DispatchBufferBinding {
                                name: "kv-cache".into(),
                                usage: BufferUsage::Inout,
                                endpoint: MemoryEndpoint {
                                    node: 0,
                                    segment: SegmentHandle(99),
                                    offset: 0,
                                },
                                bytes: 4096,
                                dtype: TensorDType::Opaque,
                                shape: vec![4096],
                                layout: TensorLayout::Opaque,
                                strides: None,
                                resident: true,
                            },
                            DispatchBufferBinding {
                                name: "input".into(),
                                usage: BufferUsage::Input,
                                endpoint: MemoryEndpoint {
                                    node: 0,
                                    segment: SegmentHandle(10),
                                    offset: 0,
                                },
                                bytes: 4096,
                                dtype: TensorDType::Opaque,
                                shape: vec![4096],
                                layout: TensorLayout::Opaque,
                                strides: None,
                                resident: false,
                            },
                            DispatchBufferBinding {
                                name: "state".into(),
                                usage: BufferUsage::Inout,
                                endpoint: MemoryEndpoint {
                                    node: 0,
                                    segment: SegmentHandle(11),
                                    offset: 0,
                                },
                                bytes: 4096,
                                dtype: TensorDType::Opaque,
                                shape: vec![4096],
                                layout: TensorLayout::Opaque,
                                strides: None,
                                resident: false,
                            },
                            DispatchBufferBinding {
                                name: "output".into(),
                                usage: BufferUsage::Output,
                                endpoint: MemoryEndpoint {
                                    node: 0,
                                    segment: SegmentHandle(12),
                                    offset: 0,
                                },
                                bytes: 4096,
                                dtype: TensorDType::Opaque,
                                shape: vec![4096],
                                layout: TensorLayout::Opaque,
                                strides: None,
                                resident: false,
                            },
                        ],
                    },
                    target_level: PlLevel::L4,
                    target_node: 19,
                    legacy_input_segments: vec![],
                },
                &mut sink,
            )
            .expect("dispatch");

        assert_eq!(handle.0, 1);
        assert_eq!(runtime.inflight()[0].input_segment_count, 2);
    }

    #[test]
    fn runtime_engine_records_execution_plan_metadata() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let handle = runtime
            .submit_backend_dispatch(
                BackendDispatchOperation {
                    task: TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 74,
                    },
                    function: FunctionLabel {
                        name: "decode_step".into(),
                        level: PlLevel::L4,
                    },
                    backend_spec: DispatchBackendSpec {
                        profile: sim_core::DispatchBackendProfile::HostVector,
                        platform: "test".into(),
                        runtime_variant: sim_core::DispatchRuntimeVariant::HostBuildGraph,
                        callable_hint: None,
                        context: None,
                        simpler_runtime: None,
                    },
                    request: BackendExecutionRequest {
                        correlation: RequestCorrelation {
                            request_id: "req-plan".into(),
                            trace_id: Some("trace-plan".into()),
                            op_name: Some("decode_step".into()),
                            step_index: Some(3),
                            sequence_no: Some(9),
                        },
                        plan: Some(ExecutionPlanRef {
                            plan_id: "plan-1".into(),
                            step_id: "step-3".into(),
                            step_kind: ExecutionStepKind::Compute,
                        }),
                        context: None,
                        bindings: vec![DispatchBufferBinding {
                            name: "input".into(),
                            usage: BufferUsage::Input,
                            endpoint: MemoryEndpoint {
                                node: 0,
                                segment: SegmentHandle(13),
                                offset: 0,
                            },
                            bytes: 4096,
                            dtype: TensorDType::Opaque,
                            shape: vec![4096],
                            layout: TensorLayout::Opaque,
                            strides: None,
                            resident: false,
                        }],
                    },
                    target_level: PlLevel::L4,
                    target_node: 19,
                    legacy_input_segments: vec![],
                },
                &mut sink,
            )
            .expect("dispatch");

        assert_eq!(handle.0, 1);
        assert_eq!(runtime.inflight()[0].plan_id.as_deref(), Some("plan-1"));
        assert_eq!(runtime.inflight()[0].step_id.as_deref(), Some("step-3"));
        assert_eq!(runtime.inflight()[0].step_kind.as_deref(), Some("Compute"));
    }

    #[test]
    fn runtime_engine_supports_explicit_context_commands() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);

        runtime
            .apply_execution_context_command(ExecutionContextCommand {
                device_context_id: "device-ctx-cmd".into(),
                runtime_context_id: Some("runtime-ctx-cmd".into()),
                lifecycle: ExecutionLifecycle::Init,
                warm: false,
                reusable: true,
            })
            .expect("init context");

        runtime
            .apply_execution_context_command(ExecutionContextCommand {
                device_context_id: "device-ctx-cmd".into(),
                runtime_context_id: Some("runtime-ctx-cmd".into()),
                lifecycle: ExecutionLifecycle::Warmup,
                warm: true,
                reusable: true,
            })
            .expect("warmup context");

        let runtime_ctx = runtime
            .runtime_context_snapshot("runtime-ctx-cmd")
            .expect("runtime context");
        assert_eq!(runtime_ctx.state, ContextState::Active);
        assert!(runtime_ctx.warm);

        runtime
            .apply_execution_context_command(ExecutionContextCommand {
                device_context_id: "device-ctx-cmd".into(),
                runtime_context_id: Some("runtime-ctx-cmd".into()),
                lifecycle: ExecutionLifecycle::Teardown,
                warm: false,
                reusable: true,
            })
            .expect("teardown context");

        let runtime_ctx = runtime
            .runtime_context_snapshot("runtime-ctx-cmd")
            .expect("runtime context after teardown");
        assert_eq!(runtime_ctx.state, ContextState::Closed);
        assert_eq!(runtime_ctx.teardown_count, 1);
    }

    #[test]
    fn runtime_engine_applies_context_lifecycle_transitions() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let make_request = |task_id: u64, lifecycle: ExecutionLifecycle| BackendExecutionRequest {
            correlation: RequestCorrelation {
                request_id: format!("req-{task_id}"),
                trace_id: None,
                op_name: Some("decode_step".into()),
                step_index: Some(task_id as u32),
                sequence_no: Some(task_id),
            },
            plan: None,
            context: Some(ExecutionContextRef {
                device_context_id: "device-ctx-1".into(),
                runtime_context_id: Some("runtime-ctx-1".into()),
                lifecycle,
                warm: true,
                reusable: true,
            }),
            bindings: vec![DispatchBufferBinding {
                name: "input".into(),
                usage: BufferUsage::Input,
                endpoint: MemoryEndpoint {
                    node: 0,
                    segment: SegmentHandle(9),
                    offset: 0,
                },
                bytes: 4096,
                dtype: TensorDType::Opaque,
                shape: vec![4096],
                layout: TensorLayout::Opaque,
                strides: None,
                resident: false,
            }],
        };

        for (task_id, lifecycle) in [
            (1u64, ExecutionLifecycle::Init),
            (2u64, ExecutionLifecycle::Reset),
            (3u64, ExecutionLifecycle::Teardown),
        ] {
            runtime
                .submit_dispatch(
                    DispatchRequest {
                        task: TaskKey {
                            logical_system: LogicalSystemId(1),
                            coord: HierarchyCoord { levels: [0; 8] },
                            scope_depth: 0,
                            task_id,
                        },
                        function: FunctionLabel {
                            name: "decode_step".into(),
                            level: PlLevel::L4,
                        },
                        backend_spec: None,
                        request: Some(make_request(task_id, lifecycle)),
                        target_level: PlLevel::L4,
                        target_node: 19,
                        input_segments: vec![SegmentHandle(9)],
                    },
                    &mut sink,
                )
                .expect("dispatch");
        }

        let runtime_ctx = runtime
            .runtime_context_snapshot("runtime-ctx-1")
            .expect("runtime context");
        assert_eq!(runtime_ctx.generation, 1);
        assert_eq!(runtime_ctx.reset_count, 1);
        assert_eq!(runtime_ctx.teardown_count, 1);
        assert_eq!(runtime_ctx.state, ContextState::Closed);

        let err = runtime.submit_dispatch(
            DispatchRequest {
                task: TaskKey {
                    logical_system: LogicalSystemId(1),
                    coord: HierarchyCoord { levels: [0; 8] },
                    scope_depth: 0,
                    task_id: 4,
                },
                function: FunctionLabel {
                    name: "decode_step".into(),
                    level: PlLevel::L4,
                },
                backend_spec: None,
                request: Some(make_request(4, ExecutionLifecycle::Reuse)),
                target_level: PlLevel::L4,
                target_node: 19,
                input_segments: vec![SegmentHandle(9)],
            },
            &mut sink,
        );
        assert!(matches!(
            err,
            Err(sim_core::SimError::InvalidInput(
                "device_context_not_active_for_reuse"
            ))
        ));
    }

    #[test]
    fn runtime_engine_rejects_duplicate_binding_names() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let err = runtime.submit_dispatch(
            DispatchRequest {
                task: TaskKey {
                    logical_system: LogicalSystemId(1),
                    coord: HierarchyCoord { levels: [0; 8] },
                    scope_depth: 0,
                    task_id: 88,
                },
                function: FunctionLabel {
                    name: "decode_step".into(),
                    level: PlLevel::L4,
                },
                backend_spec: None,
                request: Some(BackendExecutionRequest {
                    correlation: RequestCorrelation {
                        request_id: "req-dup".into(),
                        trace_id: None,
                        op_name: Some("decode_step".into()),
                        step_index: Some(0),
                        sequence_no: Some(88),
                    },
                    plan: None,
                    context: None,
                    bindings: vec![
                        DispatchBufferBinding {
                            name: "input".into(),
                            usage: BufferUsage::Input,
                            endpoint: MemoryEndpoint {
                                node: 0,
                                segment: SegmentHandle(1),
                                offset: 0,
                            },
                            bytes: 4096,
                            dtype: TensorDType::Opaque,
                            shape: vec![4096],
                            layout: TensorLayout::Opaque,
                            strides: None,
                            resident: false,
                        },
                        DispatchBufferBinding {
                            name: "input".into(),
                            usage: BufferUsage::Output,
                            endpoint: MemoryEndpoint {
                                node: 0,
                                segment: SegmentHandle(2),
                                offset: 0,
                            },
                            bytes: 4096,
                            dtype: TensorDType::Opaque,
                            shape: vec![4096],
                            layout: TensorLayout::Opaque,
                            strides: None,
                            resident: false,
                        },
                    ],
                }),
                target_level: PlLevel::L4,
                target_node: 19,
                input_segments: vec![SegmentHandle(1)],
            },
            &mut sink,
        );

        assert!(matches!(
            err,
            Err(sim_core::SimError::InvalidInput(
                "duplicate_request_binding_name"
            ))
        ));
    }

    #[test]
    fn runtime_engine_rejects_contiguous_binding_size_mismatch() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let err = runtime.submit_dispatch(
            DispatchRequest {
                task: TaskKey {
                    logical_system: LogicalSystemId(1),
                    coord: HierarchyCoord { levels: [0; 8] },
                    scope_depth: 0,
                    task_id: 89,
                },
                function: FunctionLabel {
                    name: "decode_step".into(),
                    level: PlLevel::L4,
                },
                backend_spec: None,
                request: Some(BackendExecutionRequest {
                    correlation: RequestCorrelation {
                        request_id: "req-size".into(),
                        trace_id: None,
                        op_name: Some("decode_step".into()),
                        step_index: Some(0),
                        sequence_no: Some(89),
                    },
                    plan: None,
                    context: None,
                    bindings: vec![DispatchBufferBinding {
                        name: "input".into(),
                        usage: BufferUsage::Input,
                        endpoint: MemoryEndpoint {
                            node: 0,
                            segment: SegmentHandle(1),
                            offset: 0,
                        },
                        bytes: 8,
                        dtype: TensorDType::F32,
                        shape: vec![4],
                        layout: TensorLayout::Contiguous,
                        strides: None,
                        resident: false,
                    }],
                }),
                target_level: PlLevel::L4,
                target_node: 19,
                input_segments: vec![SegmentHandle(1)],
            },
            &mut sink,
        );

        assert!(matches!(
            err,
            Err(sim_core::SimError::InvalidInput(
                "binding_bytes_shape_mismatch"
            ))
        ));
    }

    #[test]
    fn runtime_engine_tracks_and_clears_resident_bindings() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let make_request =
            |task_id: u64, lifecycle: ExecutionLifecycle, resident: bool| BackendExecutionRequest {
                correlation: RequestCorrelation {
                    request_id: format!("req-res-{task_id}"),
                    trace_id: None,
                    op_name: Some("decode_step".into()),
                    step_index: Some(task_id as u32),
                    sequence_no: Some(task_id),
                },
                plan: None,
                context: Some(ExecutionContextRef {
                    device_context_id: "device-ctx-res".into(),
                    runtime_context_id: Some("runtime-ctx-res".into()),
                    lifecycle,
                    warm: true,
                    reusable: true,
                }),
                bindings: vec![DispatchBufferBinding {
                    name: "kv-cache".into(),
                    usage: BufferUsage::Inout,
                    endpoint: MemoryEndpoint {
                        node: 0,
                        segment: SegmentHandle(21),
                        offset: 0,
                    },
                    bytes: 4096,
                    dtype: TensorDType::Opaque,
                    shape: vec![4096],
                    layout: TensorLayout::Opaque,
                    strides: None,
                    resident,
                }],
            };

        runtime
            .submit_dispatch(
                DispatchRequest {
                    task: TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 91,
                    },
                    function: FunctionLabel {
                        name: "decode_step".into(),
                        level: PlLevel::L4,
                    },
                    backend_spec: None,
                    request: Some(make_request(91, ExecutionLifecycle::Init, true)),
                    target_level: PlLevel::L4,
                    target_node: 19,
                    input_segments: vec![SegmentHandle(21)],
                },
                &mut sink,
            )
            .expect("init dispatch");

        let runtime_ctx = runtime
            .runtime_context_snapshot("runtime-ctx-res")
            .expect("runtime context after init");
        assert_eq!(runtime_ctx.resident_binding_count, 1);

        runtime
            .apply_execution_context_command(ExecutionContextCommand {
                device_context_id: "device-ctx-res".into(),
                runtime_context_id: Some("runtime-ctx-res".into()),
                lifecycle: ExecutionLifecycle::Reset,
                warm: false,
                reusable: true,
            })
            .expect("reset context");

        let runtime_ctx = runtime
            .runtime_context_snapshot("runtime-ctx-res")
            .expect("runtime context after reset");
        assert_eq!(runtime_ctx.resident_binding_count, 0);
    }

    #[test]
    fn runtime_engine_rejects_resident_binding_mismatch() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let make_request =
            |task_id: u64, lifecycle: ExecutionLifecycle, segment: u64| BackendExecutionRequest {
                correlation: RequestCorrelation {
                    request_id: format!("req-res-mismatch-{task_id}"),
                    trace_id: None,
                    op_name: Some("decode_step".into()),
                    step_index: Some(task_id as u32),
                    sequence_no: Some(task_id),
                },
                plan: None,
                context: Some(ExecutionContextRef {
                    device_context_id: "device-ctx-res-mismatch".into(),
                    runtime_context_id: Some("runtime-ctx-res-mismatch".into()),
                    lifecycle,
                    warm: true,
                    reusable: true,
                }),
                bindings: vec![DispatchBufferBinding {
                    name: "kv-cache".into(),
                    usage: BufferUsage::Inout,
                    endpoint: MemoryEndpoint {
                        node: 0,
                        segment: SegmentHandle(segment),
                        offset: 0,
                    },
                    bytes: 4096,
                    dtype: TensorDType::Opaque,
                    shape: vec![4096],
                    layout: TensorLayout::Opaque,
                    strides: None,
                    resident: true,
                }],
            };

        runtime
            .submit_dispatch(
                DispatchRequest {
                    task: TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 92,
                    },
                    function: FunctionLabel {
                        name: "decode_step".into(),
                        level: PlLevel::L4,
                    },
                    backend_spec: None,
                    request: Some(make_request(92, ExecutionLifecycle::Init, 30)),
                    target_level: PlLevel::L4,
                    target_node: 19,
                    input_segments: vec![SegmentHandle(30)],
                },
                &mut sink,
            )
            .expect("init dispatch");

        let err = runtime.submit_dispatch(
            DispatchRequest {
                task: TaskKey {
                    logical_system: LogicalSystemId(1),
                    coord: HierarchyCoord { levels: [0; 8] },
                    scope_depth: 0,
                    task_id: 93,
                },
                function: FunctionLabel {
                    name: "decode_step".into(),
                    level: PlLevel::L4,
                },
                backend_spec: None,
                request: Some(make_request(93, ExecutionLifecycle::Reuse, 31)),
                target_level: PlLevel::L4,
                target_node: 19,
                input_segments: vec![SegmentHandle(31)],
            },
            &mut sink,
        );

        assert!(matches!(
            err,
            Err(sim_core::SimError::InvalidInput(
                "resident_binding_mismatch"
            ))
        ));
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
                    backend_spec: None,
                    request: None,
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
                    backend_spec: None,
                    request: None,
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
        assert!(events
            .iter()
            .any(|event| matches!(event, SimEvent::RuntimeFailed { op_id: 1, .. })));
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
                    backend_spec: None,
                    request: None,
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
