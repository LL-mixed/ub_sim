//! Workload harness entry points.

use std::collections::BTreeMap;
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use sim_config::{
    DualNodeBlockComputeWorkloadConfig, DualNodeCacheFillWorkloadConfig,
    DualNodeShmemMailboxWorkloadConfig, ScenarioConfig, WorkloadConfig,
};
use sim_core::{
    BackendDispatchOperation, BackendExecutionRequest, BinaryArtifactRef, BlockHash, BufferUsage,
    CompletionSource, CompletionStatus, CopyDirection, CopyRequest, DispatchBackendProfile,
    DispatchBackendSpec, DispatchBufferBinding, DispatchExecutionContext, DispatchLaunchParams,
    DispatchRequest, DispatchRuntimeVariant, ExecutionContextRef, ExecutionLifecycle,
    ExecutionPlanRef, ExecutionStepKind, FunctionLabel, HierarchyCoord, IoOpcode, IoSubmitReq,
    LogicalSystemId, MemoryEndpoint, NodeId, PlLevel, RequestCorrelation, SegmentHandle, SimError,
    SimEvent, SimplerKernelArtifact, SimplerRuntimeArg, SimplerRuntimeArtifacts, TaskKey,
    TensorDType, TensorLayout,
};
use sim_report::{CompletionSourceStats, CompletionStatusStats, EventSummary, WorkloadRunReport};
use sim_runtime::{
    InMemoryBlockStore, LocalRuntimeEngine, PromotionPlan, RecursiveRoutePlanner, RoutePlanner,
    RouteRequest, SimBlockStore, VecEventSink,
};
use sim_services::block::BlockServiceProfile;
use sim_services::db::{DbGetReq, DbPutReq};
use sim_services::dfs::{DfsReadReq, DfsWriteReq};
use sim_services::shmem::{ShmemGetReq, ShmemPutReq};
use sim_topology::SimTopology;
use sim_uapi::UapiDescriptor;
use sim_uapi::{LocalGuestUapiSurface, UapiCommand, UapiResponse};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct RustLlmProfile {
    name: &'static str,
    requests_total_cap: u64,
    prefix_groups: u64,
    prefix_blocks: u64,
    tail_blocks: u64,
    tail_uses_dfs: bool,
    evict_after_request: usize,
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
}

#[derive(Debug, Clone, PartialEq)]
pub struct HostVectorDispatchReport {
    pub elems: u64,
    pub first_values: Vec<f32>,
    pub all_match_expected: bool,
    pub completion_status: CompletionStatus,
}

pub fn load_host_vector_runtime_artifacts(
    manifest_path: &Path,
    args: Vec<SimplerRuntimeArg>,
) -> Result<SimplerRuntimeArtifacts, SimError> {
    let text = fs::read_to_string(manifest_path)
        .map_err(|_| SimError::NotFound("host_vector_manifest"))?;
    let manifest: SimplerRuntimeManifestEnvelope = serde_json::from_str(&text)
        .map_err(|_| SimError::InvalidInput("invalid_host_vector_manifest"))?;
    Ok(SimplerRuntimeArtifacts {
        host_runtime_library: manifest.simpler_runtime.host_runtime_library,
        orch_shared_object: manifest.simpler_runtime.orch_shared_object,
        orch_function_name: manifest.simpler_runtime.orch_function_name,
        aicpu_binary: manifest.simpler_runtime.aicpu_binary,
        aicore_binary: manifest.simpler_runtime.aicore_binary,
        kernels: manifest.simpler_runtime.kernels,
        launch: manifest.simpler_runtime.launch,
        runtime_env: BTreeMap::new(),
        args,
    })
}

fn simpler_manifest_path(env_var: &str, default_path: &str) -> Option<PathBuf> {
    let env_path = std::env::var_os(env_var)
        .map(PathBuf::from)
        .filter(|path| path.exists());
    env_path.or_else(|| {
        let default = Path::new(default_path);
        default.exists().then(|| default.to_path_buf())
    })
}

pub fn host_vector_manifest_path() -> Option<PathBuf> {
    simpler_manifest_path(
        "SIMPLER_HOST_VECTOR_MANIFEST",
        "/tmp/simpler-host-vector-artifacts/host_vector_manifest.json",
    )
}

pub fn tmrb_vector_manifest_path() -> Option<PathBuf> {
    simpler_manifest_path(
        "SIMPLER_TMRB_VECTOR_MANIFEST",
        "/tmp/simpler-tmrb-vector-artifacts/tmrb_vector_manifest.json",
    )
}

pub fn host_vector_backend_spec_from_manifest(
    manifest_path: &Path,
    input_a: MemoryEndpoint,
    input_b: MemoryEndpoint,
    output_f: MemoryEndpoint,
    size_bytes: u64,
    _elems: u64,
) -> Result<DispatchBackendSpec, SimError> {
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
    ];
    Ok(DispatchBackendSpec {
        profile: DispatchBackendProfile::HostVector,
        platform: "a2a3sim".to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_vector_example".to_string()),
        simpler_runtime: Some(load_host_vector_runtime_artifacts(manifest_path, args)?),
        context: None,
    })
}

fn w4_host_vector_runtime_artifacts(
    manifest_path: &Path,
    input_a: MemoryEndpoint,
    input_b: MemoryEndpoint,
    output_f: MemoryEndpoint,
    size_bytes: u64,
    _elems: u64,
) -> Result<SimplerRuntimeArtifacts, SimError> {
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
    ];
    load_host_vector_runtime_artifacts(manifest_path, args)
}

fn w4_tmrb_vector_runtime_artifacts(
    manifest_path: &Path,
    input_a: MemoryEndpoint,
    input_b: MemoryEndpoint,
    output_f: MemoryEndpoint,
    size_bytes: u64,
    elems: u64,
) -> Result<SimplerRuntimeArtifacts, SimError> {
    let args = vec![
        SimplerRuntimeArg::InputSegment {
            endpoint: input_a,
            bytes: size_bytes,
        },
        SimplerRuntimeArg::InputSegment {
            endpoint: input_b,
            bytes: size_bytes,
        },
        SimplerRuntimeArg::InoutSegment {
            endpoint: output_f,
            bytes: size_bytes,
        },
        SimplerRuntimeArg::ScalarU64(size_bytes),
        SimplerRuntimeArg::ScalarU64(size_bytes),
        SimplerRuntimeArg::ScalarU64(size_bytes),
        SimplerRuntimeArg::ScalarU64(elems),
    ];
    let mut artifacts = load_host_vector_runtime_artifacts(manifest_path, args)?;
    artifacts
        .runtime_env
        .insert("PTO2_RING_TASK_WINDOW".to_string(), "16".to_string());
    artifacts
        .runtime_env
        .insert("PTO2_RING_HEAP".to_string(), "262144".to_string());
    Ok(artifacts)
}

fn f32s_to_bytes(values: &[f32]) -> Vec<u8> {
    values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect()
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

fn simple_execution_request(
    request_id: impl Into<String>,
    trace_id: Option<String>,
    op_name: impl Into<String>,
    device_context_id: impl Into<String>,
    runtime_context_id: impl Into<String>,
    lifecycle: ExecutionLifecycle,
    step_index: u32,
    sequence_no: u64,
    plan: Option<ExecutionPlanRef>,
    bindings: Vec<DispatchBufferBinding>,
) -> BackendExecutionRequest {
    BackendExecutionRequest {
        correlation: RequestCorrelation {
            request_id: request_id.into(),
            trace_id,
            op_name: Some(op_name.into()),
            step_index: Some(step_index),
            sequence_no: Some(sequence_no),
        },
        plan,
        context: Some(ExecutionContextRef {
            device_context_id: device_context_id.into(),
            runtime_context_id: Some(runtime_context_id.into()),
            lifecycle,
            warm: true,
            reusable: true,
        }),
        bindings,
    }
}

fn request_execution_context_ids(task: &TaskKey, ubpu_node: NodeId) -> (String, String) {
    (
        format!("device-ctx-node-{ubpu_node}"),
        format!("runtime-ctx-task-{}", task.task_id),
    )
}

fn w4_step_kind(
    function_name: &str,
    request_control_phase: Option<&str>,
    kvcache_resolution_kind: Option<&str>,
) -> ExecutionStepKind {
    if function_name != "w4_rust_llm_minimal_step" {
        return ExecutionStepKind::Generic;
    }
    if matches!(request_control_phase, Some("Finish")) {
        return ExecutionStepKind::Finalize;
    }
    match request_control_phase {
        Some("Begin") => ExecutionStepKind::RequestControl,
        Some("Active") => match kvcache_resolution_kind {
            Some("RequestControlOnly") => ExecutionStepKind::RequestControl,
            Some("HotHit") => ExecutionStepKind::CacheResolve,
            Some("FilledFromBlock") => ExecutionStepKind::CacheFill,
            _ => ExecutionStepKind::Compute,
        },
        _ => ExecutionStepKind::Generic,
    }
}

fn w4_plan_ref(
    task: &TaskKey,
    function_name: &str,
    callable_hint: Option<&str>,
    request_control_phase: Option<&str>,
    kvcache_resolution_kind: Option<&str>,
    block_index: Option<u64>,
) -> Option<ExecutionPlanRef> {
    if function_name != "w4_rust_llm_minimal_step" {
        return None;
    }
    let step_suffix = callable_hint.unwrap_or(function_name).replace(':', "_");
    let block_suffix = block_index.unwrap_or(task.task_id);
    Some(ExecutionPlanRef {
        plan_id: format!("w4-plan-task-{}", task.task_id),
        step_id: format!("step-{block_suffix}-{step_suffix}"),
        step_kind: w4_step_kind(
            function_name,
            request_control_phase,
            kvcache_resolution_kind,
        ),
    })
}

fn apply_request_execution_context_command(
    runtime: &mut LocalRuntimeEngine,
    task: &TaskKey,
    ubpu_node: NodeId,
    lifecycle: ExecutionLifecycle,
    warm: bool,
) -> Result<(), SimError> {
    let (device_context_id, runtime_context_id) = request_execution_context_ids(task, ubpu_node);
    runtime.apply_execution_context_command(sim_core::ExecutionContextCommand {
        device_context_id,
        runtime_context_id: Some(runtime_context_id),
        lifecycle,
        warm,
        reusable: true,
    })
}

fn bytes_to_f32s(bytes: &[u8]) -> Vec<f32> {
    bytes
        .chunks_exact(std::mem::size_of::<f32>())
        .map(|chunk| {
            let arr: [u8; 4] = chunk.try_into().expect("f32 chunk");
            f32::from_le_bytes(arr)
        })
        .collect()
}

pub fn run_host_vector_dispatch(
    config: &ScenarioConfig,
    topology: &SimTopology,
    manifest_path: &Path,
    elems: u64,
) -> Result<HostVectorDispatchReport, SimError> {
    let host_node = topology.hosts[0].node_id;
    let ubpu_node = topology
        .ubpus
        .first()
        .map(|ubpu| ubpu.node_id)
        .ok_or(SimError::InvalidInput("missing_ubpu_node"))?;
    let input_a = SegmentHandle(101);
    let input_b = SegmentHandle(102);
    let output_f = SegmentHandle(103);
    let size_bytes = elems * std::mem::size_of::<f32>() as u64;

    let mut runtime = LocalRuntimeEngine::from_config(config);
    runtime.seed_host_segment(
        host_node,
        input_a,
        f32s_to_bytes(&vec![2.0; elems as usize]),
    );
    runtime.seed_host_segment(
        host_node,
        input_b,
        f32s_to_bytes(&vec![3.0; elems as usize]),
    );
    runtime.seed_host_segment(host_node, output_f, vec![0u8; size_bytes as usize]);

    let backend_spec = host_vector_backend_spec_from_manifest(
        manifest_path,
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
            segment: output_f,
            offset: 0,
        },
        size_bytes,
        elems,
    )?;

    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let mut sink = VecEventSink::default();
    let request = simple_execution_request(
        "host-vector-dispatch-1",
        None,
        "host_vector_example",
        "device-ctx-host-vector",
        "runtime-ctx-host-vector",
        ExecutionLifecycle::Init,
        0,
        1,
        None,
        vec![
            dense_f32_binding(
                "input_a",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_a,
                    offset: 0,
                },
                elems,
            ),
            dense_f32_binding(
                "input_b",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_b,
                    offset: 0,
                },
                elems,
            ),
            dense_f32_binding(
                "output_f",
                BufferUsage::Output,
                MemoryEndpoint {
                    node: host_node,
                    segment: output_f,
                    offset: 0,
                },
                elems,
            ),
        ],
    );
    runtime.submit_backend_dispatch(
        BackendDispatchOperation {
            task,
            function: FunctionLabel {
                name: "host_vector_example".into(),
                level: PlLevel::L2,
            },
            backend_spec,
            request,
            target_level: PlLevel::L2,
            target_node: ubpu_node,
            legacy_input_segments: vec![input_a, input_b],
        },
        &mut sink,
    )?;

    let at = config
        .pypto
        .simpler_boundary
        .dispatch_latency_us
        .unwrap_or(15);
    runtime.advance_to(at, &mut sink);
    let completion = runtime
        .poll_completions(at, &mut sink)
        .into_iter()
        .next()
        .ok_or(SimError::InvalidInput("missing_dispatch_completion"))?;
    let output = runtime
        .host_segment_payload(host_node, output_f)
        .ok_or(SimError::NotFound("host_vector_output_payload"))?;
    let values = bytes_to_f32s(output);
    let all_match_expected = values.iter().all(|value| (*value - 42.0).abs() < 1e-5);
    Ok(HostVectorDispatchReport {
        elems,
        first_values: values.into_iter().take(8).collect(),
        all_match_expected,
        completion_status: completion.status,
    })
}

fn simpler_backend_spec(
    profile: DispatchBackendProfile,
    runtime_variant: DispatchRuntimeVariant,
    callable_hint: Option<&str>,
    block_hash: Option<&BlockHash>,
    request_index: Option<u64>,
    block_index: Option<u64>,
    request_blocks_total: Option<u64>,
    blocks_remaining_in_request: Option<u64>,
    is_first_block_in_request: bool,
    is_last_block_in_request: bool,
    request_control_phase: Option<&str>,
    request_control_epoch: Option<u64>,
    request_control_result_kind: Option<&str>,
    request_control_result_value: Option<u64>,
    request_control_view_kind: Option<&str>,
    kvcache_resolution_kind: Option<&str>,
    kvcache_view_kind: Option<&str>,
    kvcache_transition_kind: Option<&str>,
    logical_system_id: Option<LogicalSystemId>,
    scope_depth: Option<u32>,
    prefix_group: Option<u64>,
    route_from_level: Option<PlLevel>,
    route_to_level: Option<PlLevel>,
    route_selected_node: Option<NodeId>,
    route_reason: Option<sim_core::RouteReason>,
    placement_level: Option<PlLevel>,
    placement_node: Option<NodeId>,
    capacity_pressure_active: bool,
    evictions_seen: u64,
    block_writebacks_seen: u64,
    promoted_this_access: bool,
    reloaded_after_eviction: bool,
    uses_dfs_fallback: bool,
    includes_request_control: bool,
    includes_prefix_shared: bool,
    hot_segment: Option<SegmentHandle>,
    request_segment: Option<SegmentHandle>,
    control_segment: Option<SegmentHandle>,
    prefix_segment: Option<SegmentHandle>,
) -> DispatchBackendSpec {
    DispatchBackendSpec {
        profile,
        platform: "a2a3sim".to_string(),
        runtime_variant,
        callable_hint: callable_hint.map(str::to_string),
        simpler_runtime: None,
        context: Some(DispatchExecutionContext {
            block_hash: block_hash.map(|hash| hash.0.clone()),
            request_index,
            block_index,
            request_blocks_total,
            blocks_remaining_in_request,
            is_first_block_in_request,
            is_last_block_in_request,
            request_control_phase: request_control_phase.map(str::to_string),
            request_control_epoch,
            request_control_result_kind: request_control_result_kind.map(str::to_string),
            request_control_result_value,
            request_control_view_kind: request_control_view_kind.map(str::to_string),
            kvcache_resolution_kind: kvcache_resolution_kind.map(str::to_string),
            kvcache_view_kind: kvcache_view_kind.map(str::to_string),
            kvcache_transition_kind: kvcache_transition_kind.map(str::to_string),
            logical_system_id: logical_system_id.map(|id| id.0),
            scope_depth,
            prefix_group,
            route_from_level: route_from_level.map(|level| format!("{level:?}")),
            route_to_level: route_to_level.map(|level| format!("{level:?}")),
            route_selected_node,
            route_reason: route_reason.map(|reason| format!("{reason:?}")),
            placement_level: placement_level.map(|level| format!("{level:?}")),
            placement_node,
            capacity_pressure_active,
            evictions_seen,
            block_writebacks_seen,
            promoted_this_access,
            reloaded_after_eviction,
            uses_dfs_fallback,
            includes_request_control,
            includes_prefix_shared,
            hot_segment: hot_segment.map(|segment| segment.0),
            request_segment: request_segment.map(|segment| segment.0),
            control_segment: control_segment.map(|segment| segment.0),
            prefix_segment: prefix_segment.map(|segment| segment.0),
        }),
    }
}

fn empty_simpler_backend_spec(
    profile: DispatchBackendProfile,
    runtime_variant: DispatchRuntimeVariant,
    callable_hint: &str,
) -> DispatchBackendSpec {
    simpler_backend_spec(
        profile,
        runtime_variant,
        Some(callable_hint),
        None,  // block_hash
        None,  // request_index
        None,  // block_index
        None,  // request_blocks_total
        None,  // blocks_remaining_in_request
        false, // is_first_block_in_request
        false, // is_last_block_in_request
        None,  // request_control_phase
        None,  // request_control_epoch
        None,  // request_control_result_kind
        None,  // request_control_result_value
        None,  // request_control_view_kind
        None,  // kvcache_resolution_kind
        None,  // kvcache_view_kind
        None,  // kvcache_transition_kind
        None,  // logical_system_id
        None,  // scope_depth
        None,  // prefix_group
        None,  // route_from_level
        None,  // route_to_level
        None,  // route_selected_node
        None,  // route_reason
        None,  // placement_level
        None,  // placement_node
        false, // capacity_pressure_active
        0,     // evictions_seen
        0,     // block_writebacks_seen
        false, // promoted_this_access
        false, // reloaded_after_eviction
        false, // uses_dfs_fallback
        false, // includes_request_control
        false, // includes_prefix_shared
        None,  // hot_segment
        None,  // request_segment
        None,  // control_segment
        None,  // prefix_segment
    )
}

struct RustLlmKvCacheService<'a> {
    surface: &'a mut LocalGuestUapiSurface,
    store: &'a mut InMemoryBlockStore,
    planner: &'a RecursiveRoutePlanner,
    topology: &'a SimTopology,
    report: &'a mut WorkloadRunReport,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    prefix_segment: SegmentHandle,
    prefix_segment_seeded: &'a mut bool,
    kv_hot_segments: &'a mut HashMap<BlockHash, SegmentHandle>,
    block_result_segments: &'a mut HashMap<BlockHash, SegmentHandle>,
    seeded_db_keys: &'a mut HashSet<String>,
    seeded_dfs_paths: &'a mut HashSet<String>,
    allow_queue_retry: bool,
    rust_profile: Option<RustLlmProfile>,
}

struct RustLlmRequestControlService<'a> {
    surface: &'a mut LocalGuestUapiSurface,
    report: &'a mut WorkloadRunReport,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    request_segment: SegmentHandle,
    control_segment: SegmentHandle,
    request_result_segments: &'a mut HashMap<u64, SegmentHandle>,
}

impl<'a> RustLlmRequestControlService<'a> {
    fn begin_request(&mut self, task: &TaskKey) -> Result<(), SimError> {
        rust_llm_begin_request_control(
            self.surface,
            self.cmdq,
            self.cq,
            self.report,
            task,
            self.request_segment,
        )
    }

    fn finish_request(&mut self, task: &TaskKey) -> Result<(), SimError> {
        rust_llm_finish_request_control(
            self.surface,
            self.cmdq,
            self.cq,
            self.report,
            task,
            self.control_segment,
        )?;
        if self.latest_result_segment(task).is_some() {
            enqueue_descriptor_and_ring(
                self.surface,
                self.cmdq,
                UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
                    task: Some(task.clone()),
                    requester_entity: 0,
                    segment: self.control_segment,
                    bytes: 256,
                }),
                "unexpected control result-aware shmem put enqueue response",
                "unexpected control result-aware shmem put doorbell response",
            )?;
            self.report.shmem_puts += 1;
            enqueue_descriptor_and_ring(
                self.surface,
                self.cmdq,
                UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
                    task: Some(task.clone()),
                    requester_entity: 0,
                    segment: self.control_segment,
                    bytes: 256,
                }),
                "unexpected control result-aware shmem get enqueue response",
                "unexpected control result-aware shmem get doorbell response",
            )?;
            self.report.shmem_gets += 1;
            drain_and_record(
                self.surface,
                self.cq,
                self.report,
                "unexpected control result-aware shmem cq drain response",
            )?;
            self.report.events.push(SimEvent::W4ServiceResultApplied {
                at: self.report.events.len() as u64,
                task: task.clone(),
                service_kind: "RequestControl".to_string(),
                action_kind: "FinishControlRefreshed".to_string(),
                block_hash: None,
                target_segment: self.control_segment,
                result_segment: self.latest_result_segment(task).unwrap(),
            });
        }
        Ok(())
    }

    fn record_result(
        &mut self,
        task: &TaskKey,
        result_segment: SegmentHandle,
    ) -> Result<(), SimError> {
        self.request_result_segments
            .insert(task.task_id, result_segment);
        enqueue_descriptor_and_ring(
            self.surface,
            self.cmdq,
            UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
                task: Some(task.clone()),
                requester_entity: 0,
                segment: self.request_segment,
                bytes: 256,
            }),
            "unexpected request result shmem put enqueue response",
            "unexpected request result shmem put doorbell response",
        )?;
        self.report.shmem_puts += 1;
        enqueue_descriptor_and_ring(
            self.surface,
            self.cmdq,
            UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
                task: Some(task.clone()),
                requester_entity: 0,
                segment: self.request_segment,
                bytes: 256,
            }),
            "unexpected request result shmem get enqueue response",
            "unexpected request result shmem get doorbell response",
        )?;
        self.report.shmem_gets += 1;
        drain_and_record(
            self.surface,
            self.cq,
            self.report,
            "unexpected request result shmem cq drain response",
        )?;
        self.report.events.push(SimEvent::W4ServiceResultApplied {
            at: self.report.events.len() as u64,
            task: task.clone(),
            service_kind: "RequestControl".to_string(),
            action_kind: "RequestRepublished".to_string(),
            block_hash: None,
            target_segment: self.request_segment,
            result_segment,
        });
        Ok(())
    }

    fn latest_result_segment(&self, task: &TaskKey) -> Option<SegmentHandle> {
        self.request_result_segments.get(&task.task_id).copied()
    }
}

fn w4_begin_request_control(
    surface: &mut LocalGuestUapiSurface,
    report: &mut WorkloadRunReport,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    request_segment: SegmentHandle,
    control_segment: SegmentHandle,
    request_result_segments: &mut HashMap<u64, SegmentHandle>,
    task: &TaskKey,
) -> Result<(), SimError> {
    RustLlmRequestControlService {
        surface,
        report,
        cmdq,
        cq,
        request_segment,
        control_segment,
        request_result_segments,
    }
    .begin_request(task)
}

fn w4_finish_request_control(
    surface: &mut LocalGuestUapiSurface,
    report: &mut WorkloadRunReport,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    request_segment: SegmentHandle,
    control_segment: SegmentHandle,
    request_result_segments: &mut HashMap<u64, SegmentHandle>,
    task: &TaskKey,
) -> Result<(), SimError> {
    RustLlmRequestControlService {
        surface,
        report,
        cmdq,
        cq,
        request_segment,
        control_segment,
        request_result_segments,
    }
    .finish_request(task)
}

fn w4_record_request_result(
    surface: &mut LocalGuestUapiSurface,
    report: &mut WorkloadRunReport,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    request_segment: SegmentHandle,
    control_segment: SegmentHandle,
    request_result_segments: &mut HashMap<u64, SegmentHandle>,
    task: &TaskKey,
    result_segment: SegmentHandle,
) -> Result<(), SimError> {
    RustLlmRequestControlService {
        surface,
        report,
        cmdq,
        cq,
        request_segment,
        control_segment,
        request_result_segments,
    }
    .record_result(task, result_segment)
}

fn w4_latest_request_result(
    request_result_segments: &HashMap<u64, SegmentHandle>,
    task: &TaskKey,
) -> Option<SegmentHandle> {
    request_result_segments.get(&task.task_id).copied()
}

struct RustLlmComputeView {
    input_segment: SegmentHandle,
    callable_hint: String,
    block_hash: BlockHash,
    kvcache_resolution_kind: &'static str,
    kvcache_view_kind: &'static str,
    kvcache_transition_kind: &'static str,
    prefix_group: Option<u64>,
    route_from_level: Option<PlLevel>,
    route_to_level: Option<PlLevel>,
    route_selected_node: Option<NodeId>,
    route_reason: Option<sim_core::RouteReason>,
    placement_level: Option<PlLevel>,
    placement_node: Option<NodeId>,
    promoted_this_access: bool,
    reloaded_after_eviction: bool,
    uses_dfs_fallback: bool,
    shared_segments: Vec<SegmentHandle>,
}

struct RustLlmBackendStepSpec<'a> {
    task: &'a TaskKey,
    request_id: &'a str,
    trace_id: &'a str,
    profile_name: &'a str,
    function_name: &'a str,
    input_segment: SegmentHandle,
    additional_input_segments: Vec<SegmentHandle>,
    callable_hint: Option<&'a str>,
    block_hash: Option<BlockHash>,
    request_index: Option<u64>,
    block_index: Option<u64>,
    request_blocks_total: Option<u64>,
    blocks_remaining_in_request: Option<u64>,
    is_first_block_in_request: bool,
    is_last_block_in_request: bool,
    request_control_phase: Option<&'a str>,
    request_control_epoch: Option<u64>,
    request_control_result_kind: Option<&'a str>,
    request_control_result_value: Option<u64>,
    request_control_view_kind: Option<&'a str>,
    kvcache_resolution_kind: Option<&'a str>,
    kvcache_view_kind: Option<&'a str>,
    kvcache_transition_kind: Option<&'a str>,
    logical_system_id: Option<LogicalSystemId>,
    scope_depth: Option<u32>,
    prefix_group: Option<u64>,
    route_from_level: Option<PlLevel>,
    route_to_level: Option<PlLevel>,
    route_selected_node: Option<NodeId>,
    route_reason: Option<sim_core::RouteReason>,
    placement_level: Option<PlLevel>,
    placement_node: Option<NodeId>,
    capacity_pressure_active: bool,
    evictions_seen: u64,
    block_writebacks_seen: u64,
    promoted_this_access: bool,
    reloaded_after_eviction: bool,
    uses_dfs_fallback: bool,
    request_segment: Option<SegmentHandle>,
    control_segment: Option<SegmentHandle>,
    prefix_segment: Option<SegmentHandle>,
    context_lifecycle: ExecutionLifecycle,
}

struct W4StepCommon<'a> {
    task: &'a TaskKey,
    request_id: &'a str,
    trace_id: &'a str,
    profile_name: &'a str,
    request_index: u64,
    request_blocks_total: u64,
    capacity_pressure_active: bool,
    evictions_seen: u64,
    block_writebacks_seen: u64,
    request_segment: SegmentHandle,
    control_segment: SegmentHandle,
}

fn w4_begin_step_spec<'a>(common: &W4StepCommon<'a>) -> RustLlmBackendStepSpec<'a> {
    RustLlmBackendStepSpec {
        task: common.task,
        request_id: common.request_id,
        trace_id: common.trace_id,
        profile_name: common.profile_name,
        function_name: "w4_rust_llm_minimal_step",
        input_segment: common.request_segment,
        additional_input_segments: vec![common.control_segment],
        callable_hint: Some("w4_begin_request_primary"),
        block_hash: Some(BlockHash(format!("request-{}-begin", common.request_index))),
        request_index: Some(common.request_index),
        block_index: Some(0),
        request_blocks_total: Some(common.request_blocks_total),
        blocks_remaining_in_request: Some(common.request_blocks_total),
        is_first_block_in_request: true,
        is_last_block_in_request: false,
        request_control_phase: Some("Begin"),
        request_control_epoch: Some(common.task.task_id),
        request_control_result_kind: Some("RequestOpened"),
        request_control_result_value: Some(1),
        request_control_view_kind: Some("RequestPrimary"),
        kvcache_resolution_kind: Some("RequestControlOnly"),
        kvcache_view_kind: Some("RequestPrimaryView"),
        kvcache_transition_kind: Some("ControlOnly"),
        logical_system_id: Some(common.task.logical_system),
        scope_depth: Some(common.task.scope_depth),
        prefix_group: None,
        route_from_level: None,
        route_to_level: None,
        route_selected_node: None,
        route_reason: None,
        placement_level: None,
        placement_node: None,
        capacity_pressure_active: common.capacity_pressure_active,
        evictions_seen: common.evictions_seen,
        block_writebacks_seen: common.block_writebacks_seen,
        promoted_this_access: false,
        reloaded_after_eviction: false,
        uses_dfs_fallback: false,
        request_segment: Some(common.request_segment),
        control_segment: Some(common.control_segment),
        prefix_segment: None,
        context_lifecycle: ExecutionLifecycle::Reuse,
    }
}

fn w4_finish_step_spec<'a>(
    common: &W4StepCommon<'a>,
    finish_inputs: Vec<SegmentHandle>,
) -> RustLlmBackendStepSpec<'a> {
    RustLlmBackendStepSpec {
        task: common.task,
        request_id: common.request_id,
        trace_id: common.trace_id,
        profile_name: common.profile_name,
        function_name: "w4_rust_llm_minimal_step",
        input_segment: common.control_segment,
        additional_input_segments: finish_inputs,
        callable_hint: Some("w4_finish_request_control"),
        block_hash: Some(BlockHash(format!(
            "request-{}-finish",
            common.request_index
        ))),
        request_index: Some(common.request_index),
        block_index: Some(common.request_blocks_total),
        request_blocks_total: Some(common.request_blocks_total),
        blocks_remaining_in_request: Some(0),
        is_first_block_in_request: false,
        is_last_block_in_request: true,
        request_control_phase: Some("Finish"),
        request_control_epoch: Some(common.task.task_id),
        request_control_result_kind: Some("RequestClosed"),
        request_control_result_value: Some(common.request_blocks_total),
        request_control_view_kind: Some("ControlPrimary"),
        kvcache_resolution_kind: Some("RequestControlOnly"),
        kvcache_view_kind: Some("ControlPrimaryView"),
        kvcache_transition_kind: Some("ControlOnly"),
        logical_system_id: Some(common.task.logical_system),
        scope_depth: Some(common.task.scope_depth),
        prefix_group: None,
        route_from_level: None,
        route_to_level: None,
        route_selected_node: None,
        route_reason: None,
        placement_level: None,
        placement_node: None,
        capacity_pressure_active: common.capacity_pressure_active,
        evictions_seen: common.evictions_seen,
        block_writebacks_seen: common.block_writebacks_seen,
        promoted_this_access: false,
        reloaded_after_eviction: false,
        uses_dfs_fallback: false,
        request_segment: Some(common.request_segment),
        control_segment: Some(common.control_segment),
        prefix_segment: None,
        context_lifecycle: ExecutionLifecycle::Reuse,
    }
}

fn w4_active_step_spec<'a>(
    common: &W4StepCommon<'a>,
    block_index: u64,
    blocks_remaining_in_request: u64,
    input_segment: SegmentHandle,
    additional_input_segments: Vec<SegmentHandle>,
    callable_hint: &'a str,
    block_hash: BlockHash,
    is_first_block_in_request: bool,
    is_last_block_in_request: bool,
    kvcache_resolution_kind: &'a str,
    kvcache_view_kind: &'a str,
    kvcache_transition_kind: &'a str,
    prefix_group: Option<u64>,
    route_from_level: Option<PlLevel>,
    route_to_level: Option<PlLevel>,
    route_selected_node: Option<NodeId>,
    route_reason: Option<sim_core::RouteReason>,
    placement_level: Option<PlLevel>,
    placement_node: Option<NodeId>,
    promoted_this_access: bool,
    reloaded_after_eviction: bool,
    uses_dfs_fallback: bool,
    prefix_segment: Option<SegmentHandle>,
) -> RustLlmBackendStepSpec<'a> {
    RustLlmBackendStepSpec {
        task: common.task,
        request_id: common.request_id,
        trace_id: common.trace_id,
        profile_name: common.profile_name,
        function_name: "w4_rust_llm_minimal_step",
        input_segment,
        additional_input_segments,
        callable_hint: Some(callable_hint),
        block_hash: Some(block_hash),
        request_index: Some(common.request_index),
        block_index: Some(block_index),
        request_blocks_total: Some(common.request_blocks_total),
        blocks_remaining_in_request: Some(blocks_remaining_in_request),
        is_first_block_in_request,
        is_last_block_in_request,
        request_control_phase: Some("Active"),
        request_control_epoch: Some(common.task.task_id),
        request_control_result_kind: Some("RequestActive"),
        request_control_result_value: Some(block_index + 1),
        request_control_view_kind: Some("HotWithRequestControl"),
        kvcache_resolution_kind: Some(kvcache_resolution_kind),
        kvcache_view_kind: Some(kvcache_view_kind),
        kvcache_transition_kind: Some(kvcache_transition_kind),
        logical_system_id: Some(common.task.logical_system),
        scope_depth: Some(common.task.scope_depth),
        prefix_group,
        route_from_level,
        route_to_level,
        route_selected_node,
        route_reason,
        placement_level,
        placement_node,
        capacity_pressure_active: common.capacity_pressure_active,
        evictions_seen: common.evictions_seen,
        block_writebacks_seen: common.block_writebacks_seen,
        promoted_this_access,
        reloaded_after_eviction,
        uses_dfs_fallback,
        request_segment: Some(common.request_segment),
        control_segment: Some(common.control_segment),
        prefix_segment,
        context_lifecycle: ExecutionLifecycle::Reuse,
    }
}

fn prefix_group_for_block(block: &BlockHash) -> Option<u64> {
    let rest = block.0.strip_prefix("prefix-")?;
    let (group, _) = rest.split_once("-block-")?;
    group.parse().ok()
}

impl<'a> RustLlmKvCacheService<'a> {
    fn record_result(
        &mut self,
        task: &TaskKey,
        block: &BlockHash,
        result_segment: SegmentHandle,
    ) -> Result<(), SimError> {
        self.block_result_segments
            .insert(block.clone(), result_segment);
        if let Some(kv_hot_segment) = self.kv_hot_segments.get(block).copied() {
            enqueue_descriptor_and_ring(
                self.surface,
                self.cmdq,
                UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
                    task: Some(task.clone()),
                    requester_entity: 0,
                    segment: kv_hot_segment,
                    bytes: 4096,
                }),
                "unexpected kv result shmem put enqueue response",
                "unexpected kv result shmem put doorbell response",
            )?;
            self.report.shmem_puts += 1;
            enqueue_descriptor_and_ring(
                self.surface,
                self.cmdq,
                UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
                    task: Some(task.clone()),
                    requester_entity: 0,
                    segment: kv_hot_segment,
                    bytes: 4096,
                }),
                "unexpected kv result shmem get enqueue response",
                "unexpected kv result shmem get doorbell response",
            )?;
            self.report.shmem_gets += 1;
            drain_and_record(
                self.surface,
                self.cq,
                self.report,
                "unexpected kv result shmem cq drain response",
            )?;
            self.report.events.push(SimEvent::W4ServiceResultApplied {
                at: self.report.events.len() as u64,
                task: task.clone(),
                service_kind: "KvCache".to_string(),
                action_kind: "KvResultRepublished".to_string(),
                block_hash: Some(block.clone()),
                target_segment: kv_hot_segment,
                result_segment,
            });
        }
        Ok(())
    }

    fn resolve_compute_view(
        &mut self,
        task: &TaskKey,
        at: u64,
        block: BlockHash,
        is_prefix_block: bool,
        reloaded_after_eviction: bool,
        uses_dfs_fallback: bool,
    ) -> Result<RustLlmComputeView, SimError> {
        let block_class = if is_prefix_block { "prefix" } else { "tail" };
        let lookup = self.store.lookup(&block);
        if lookup.found {
            self.report.hits += 1;
            if is_prefix_block {
                self.report.prefix_hits += 1;
                rust_llm_sync_prefix_shared_view(
                    self.surface,
                    self.cmdq,
                    self.cq,
                    self.report,
                    task,
                    &block,
                    self.prefix_segment,
                    self.prefix_segment_seeded,
                    self.seeded_db_keys,
                )?;
            }
            let input_segment = rust_llm_materialize_hot_view_for_hit(
                self.surface,
                self.cmdq,
                self.cq,
                self.report,
                task,
                self.kv_hot_segments,
                &block,
            )?;
            let prior_result_segment = self.block_result_segments.get(&block).copied();
            if prior_result_segment.is_some() {
                rust_llm_refresh_hot_view_from_result(
                    self.surface,
                    self.cmdq,
                    self.cq,
                    self.report,
                    task,
                    input_segment,
                )?;
                self.report.events.push(SimEvent::W4ServiceResultApplied {
                    at: self.report.events.len() as u64,
                    task: task.clone(),
                    service_kind: "KvCache".to_string(),
                    action_kind: "HotHitRefreshed".to_string(),
                    block_hash: Some(block.clone()),
                    target_segment: input_segment,
                    result_segment: prior_result_segment.unwrap(),
                });
            }
            let prefix_group = prefix_group_for_block(&block);
            let mut shared_segments = if is_prefix_block {
                vec![self.prefix_segment]
            } else {
                Vec::new()
            };
            if let Some(previous_block_result) = prior_result_segment {
                shared_segments.push(previous_block_result);
            }
            return Ok(RustLlmComputeView {
                input_segment,
                callable_hint: format!("w4_hit_{block_class}"),
                block_hash: block,
                kvcache_resolution_kind: "HotHit",
                kvcache_view_kind: if is_prefix_block {
                    "PrefixHotView"
                } else {
                    "TailHotView"
                },
                kvcache_transition_kind: if reloaded_after_eviction {
                    "ReloadedHot"
                } else {
                    "StableHot"
                },
                prefix_group,
                route_from_level: None,
                route_to_level: None,
                route_selected_node: None,
                route_reason: None,
                placement_level: lookup.placement.as_ref().map(|placement| placement.level),
                placement_node: lookup.placement.as_ref().map(|placement| placement.node),
                promoted_this_access: false,
                reloaded_after_eviction,
                uses_dfs_fallback,
                shared_segments,
            });
        }

        self.report.misses += 1;
        if !is_prefix_block {
            self.report.tail_misses += 1;
        }
        let decision = self.planner.plan(
            RouteRequest {
                task: task.clone(),
                current_level: PlLevel::L4,
                block: block.clone(),
            },
            self.topology,
        )?;
        self.report.events.push(SimEvent::RoutePlanned {
            at,
            task: task.clone(),
            decision: decision.clone(),
        });

        self.store.stage_insert(PromotionPlan {
            block: block.clone(),
        })?;
        self.report.promotions += 1;

        if let Some(placement) = self.store.lookup(&block).placement {
            self.report.events.push(SimEvent::BlockPromoted {
                at: at + 1,
                block: block.clone(),
                placement,
            });
        }

        if uses_dfs_fallback {
            let dfs_path = format!("/weights/{}", block.0);
            let cold_read = self.seeded_dfs_paths.insert(dfs_path.clone());
            if cold_read {
                enqueue_descriptor_and_ring(
                    self.surface,
                    self.cmdq,
                    UapiDescriptor::DfsWrite(DfsWriteReq {
                        task: Some(task.clone()),
                        path: dfs_path.clone(),
                        bytes: 4096,
                    }),
                    "unexpected dfs write enqueue response",
                    "unexpected dfs write doorbell response",
                )?;
                self.report.dfs_seed_writes += 1;
            }

            submit_block_read(
                self.surface,
                self.cq,
                self.report,
                task.clone(),
                block.clone(),
                self.allow_queue_retry,
            )?;
            self.report.fallback_reads += 1;
            if cold_read {
                self.report.dfs_cold_reads += 1;
            } else {
                self.report.dfs_warm_reads += 1;
            }
            enqueue_descriptor_and_ring(
                self.surface,
                self.cmdq,
                UapiDescriptor::DfsRead(DfsReadReq {
                    task: Some(task.clone()),
                    path: dfs_path,
                }),
                "unexpected dfs read enqueue response",
                "unexpected dfs read doorbell response",
            )?;
        } else {
            submit_block_read(
                self.surface,
                self.cq,
                self.report,
                task.clone(),
                block.clone(),
                self.allow_queue_retry,
            )?;
        }

        let input_segment = rust_llm_fill_hot_view_from_block(
            self.surface,
            self.cmdq,
            self.cq,
            self.report,
            task,
            self.kv_hot_segments,
            block.clone(),
            self.allow_queue_retry,
            self.rust_profile,
        )?;
        let prior_result_segment = self.block_result_segments.get(&block).copied();
        if reloaded_after_eviction && prior_result_segment.is_some() {
            rust_llm_refresh_hot_view_from_result(
                self.surface,
                self.cmdq,
                self.cq,
                self.report,
                task,
                input_segment,
            )?;
            self.report.events.push(SimEvent::W4ServiceResultApplied {
                at: self.report.events.len() as u64,
                task: task.clone(),
                service_kind: "KvCache".to_string(),
                action_kind: "ReloadedHotRefreshed".to_string(),
                block_hash: Some(block.clone()),
                target_segment: input_segment,
                result_segment: prior_result_segment.unwrap(),
            });
        }
        let placement = self.store.lookup(&block).placement;
        let prefix_group = prefix_group_for_block(&block);
        let mut shared_segments = Vec::new();
        if let Some(previous_block_result) = prior_result_segment {
            shared_segments.push(previous_block_result);
        }
        Ok(RustLlmComputeView {
            input_segment,
            callable_hint: format!("w4_fill_{block_class}"),
            block_hash: block,
            kvcache_resolution_kind: "FilledFromBlock",
            kvcache_view_kind: if is_prefix_block {
                "PrefixHotView"
            } else {
                "TailHotView"
            },
            kvcache_transition_kind: if reloaded_after_eviction && !shared_segments.is_empty() {
                "ReloadedHot"
            } else {
                "PromotedHot"
            },
            prefix_group,
            route_from_level: Some(decision.from_level),
            route_to_level: Some(decision.to_level),
            route_selected_node: Some(decision.selected_node),
            route_reason: Some(decision.reason),
            placement_level: placement.as_ref().map(|placement| placement.level),
            placement_node: placement.as_ref().map(|placement| placement.node),
            promoted_this_access: true,
            reloaded_after_eviction,
            uses_dfs_fallback,
            shared_segments,
        })
    }
}

struct W4KvCacheDeps<'a> {
    surface: &'a mut LocalGuestUapiSurface,
    store: &'a mut InMemoryBlockStore,
    planner: &'a RecursiveRoutePlanner,
    topology: &'a SimTopology,
    report: &'a mut WorkloadRunReport,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    prefix_segment: SegmentHandle,
    prefix_segment_seeded: &'a mut bool,
    kv_hot_segments: &'a mut HashMap<BlockHash, SegmentHandle>,
    block_result_segments: &'a mut HashMap<BlockHash, SegmentHandle>,
    seeded_db_keys: &'a mut HashSet<String>,
    seeded_dfs_paths: &'a mut HashSet<String>,
    allow_queue_retry: bool,
    rust_profile: Option<RustLlmProfile>,
}

fn w4_resolve_compute_view(
    deps: W4KvCacheDeps<'_>,
    task: &TaskKey,
    at: u64,
    block: BlockHash,
    is_prefix_block: bool,
    reloaded_after_eviction: bool,
    uses_dfs_fallback: bool,
) -> Result<RustLlmComputeView, SimError> {
    RustLlmKvCacheService {
        surface: deps.surface,
        store: deps.store,
        planner: deps.planner,
        topology: deps.topology,
        report: deps.report,
        cmdq: deps.cmdq,
        cq: deps.cq,
        prefix_segment: deps.prefix_segment,
        prefix_segment_seeded: deps.prefix_segment_seeded,
        kv_hot_segments: deps.kv_hot_segments,
        block_result_segments: deps.block_result_segments,
        seeded_db_keys: deps.seeded_db_keys,
        seeded_dfs_paths: deps.seeded_dfs_paths,
        allow_queue_retry: deps.allow_queue_retry,
        rust_profile: deps.rust_profile,
    }
    .resolve_compute_view(
        task,
        at,
        block,
        is_prefix_block,
        reloaded_after_eviction,
        uses_dfs_fallback,
    )
}

fn w4_record_kvcache_result(
    deps: W4KvCacheDeps<'_>,
    task: &TaskKey,
    block: &BlockHash,
    result_segment: SegmentHandle,
) -> Result<(), SimError> {
    RustLlmKvCacheService {
        surface: deps.surface,
        store: deps.store,
        planner: deps.planner,
        topology: deps.topology,
        report: deps.report,
        cmdq: deps.cmdq,
        cq: deps.cq,
        prefix_segment: deps.prefix_segment,
        prefix_segment_seeded: deps.prefix_segment_seeded,
        kv_hot_segments: deps.kv_hot_segments,
        block_result_segments: deps.block_result_segments,
        seeded_db_keys: deps.seeded_db_keys,
        seeded_dfs_paths: deps.seeded_dfs_paths,
        allow_queue_retry: deps.allow_queue_retry,
        rust_profile: deps.rust_profile,
    }
    .record_result(task, block, result_segment)
}

struct RustLlmRequestRunner<'a> {
    runtime: &'a mut LocalRuntimeEngine,
    surface: &'a mut LocalGuestUapiSurface,
    store: &'a mut InMemoryBlockStore,
    planner: &'a RecursiveRoutePlanner,
    topology: &'a SimTopology,
    report: &'a mut WorkloadRunReport,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    prefix_segment: SegmentHandle,
    request_segment: SegmentHandle,
    control_segment: SegmentHandle,
    prefix_segment_seeded: &'a mut bool,
    kv_hot_segments: &'a mut HashMap<BlockHash, SegmentHandle>,
    block_result_segments: &'a mut HashMap<BlockHash, SegmentHandle>,
    request_result_segments: &'a mut HashMap<u64, SegmentHandle>,
    evicted_blocks_seen: &'a mut HashSet<BlockHash>,
    seeded_db_keys: &'a mut HashSet<String>,
    seeded_dfs_paths: &'a mut HashSet<String>,
    rust_profile: RustLlmProfile,
    allow_queue_retry: bool,
}

struct RustLlmRequestSpec {
    request_id: String,
    trace_id: String,
    profile_name: String,
    request_idx: u64,
    blocks: Vec<BlockHash>,
    steps: Vec<RustLlmRequestStep>,
}

enum RustLlmRequestStep {
    Begin,
    Active { block_idx: u64, block: BlockHash },
    Finish,
}

impl RustLlmRequestSpec {
    fn build(
        workload_kind: &str,
        rust_profile: RustLlmProfile,
        request_idx: u64,
        blocks_per_request: u32,
        unique_prefixes: u64,
    ) -> Self {
        let blocks: Vec<BlockHash> = (0..u64::from(blocks_per_request))
            .map(|block_idx| {
                block_for_request(
                    workload_kind,
                    Some(rust_profile),
                    request_idx,
                    block_idx,
                    unique_prefixes,
                )
            })
            .collect();
        let mut steps = Vec::with_capacity(blocks.len() + 2);
        steps.push(RustLlmRequestStep::Begin);
        steps.extend(
            blocks
                .iter()
                .cloned()
                .enumerate()
                .map(|(block_idx, block)| RustLlmRequestStep::Active {
                    block_idx: block_idx as u64,
                    block,
                }),
        );
        steps.push(RustLlmRequestStep::Finish);
        Self {
            request_id: format!("rust-llm-request-{request_idx}"),
            trace_id: format!("rust-llm-trace-{request_idx}"),
            profile_name: rust_profile.name.to_string(),
            request_idx,
            blocks,
            steps,
        }
    }
}

struct RustLlmExecutionSlice<'a> {
    task: &'a TaskKey,
    request: &'a RustLlmRequestSpec,
    request_blocks_total: u64,
    ubpu_node: NodeId,
    w4_common: W4StepCommon<'a>,
    previous_result_segment: Option<SegmentHandle>,
}

impl<'a> RustLlmExecutionSlice<'a> {
    fn new(
        task: &'a TaskKey,
        request: &'a RustLlmRequestSpec,
        ubpu_node: NodeId,
        report: &WorkloadRunReport,
        request_segment: SegmentHandle,
        control_segment: SegmentHandle,
        rust_profile: RustLlmProfile,
    ) -> Self {
        let request_blocks_total = request.blocks.len() as u64;
        let capacity_pressure_active = rust_profile.evict_after_request > 0;
        let w4_common = W4StepCommon {
            task,
            request_id: &request.request_id,
            trace_id: &request.trace_id,
            profile_name: &request.profile_name,
            request_index: request.request_idx,
            request_blocks_total,
            capacity_pressure_active,
            evictions_seen: report.evictions,
            block_writebacks_seen: report.block_writebacks,
            request_segment,
            control_segment,
        };

        Self {
            task,
            request,
            request_blocks_total,
            ubpu_node,
            w4_common,
            previous_result_segment: None,
        }
    }

    fn begin(&mut self, runner: &mut RustLlmRequestRunner<'_>) -> Result<(), SimError> {
        apply_request_execution_context_command(
            runner.runtime,
            self.task,
            self.ubpu_node,
            ExecutionLifecycle::Init,
            false,
        )?;
        w4_begin_request_control(
            runner.surface,
            runner.report,
            runner.cmdq,
            runner.cq,
            runner.request_segment,
            runner.control_segment,
            runner.request_result_segments,
            self.task,
        )?;
        self.previous_result_segment = Some(run_rust_llm_backend_step(
            runner.runtime,
            runner.topology,
            runner.report,
            w4_begin_step_spec(&self.w4_common),
        )?);
        w4_record_request_result(
            runner.surface,
            runner.report,
            runner.cmdq,
            runner.cq,
            runner.request_segment,
            runner.control_segment,
            runner.request_result_segments,
            self.task,
            self.previous_result_segment.expect("begin result set"),
        )?;
        Ok(())
    }

    fn run_active_block(
        &mut self,
        runner: &mut RustLlmRequestRunner<'_>,
        block_idx: u64,
        block: BlockHash,
    ) -> Result<(), SimError> {
        runner.report.blocks_total += 1;
        let uses_dfs_fallback = uses_dfs_fallback(Some(runner.rust_profile), block_idx);
        let is_prefix_block = is_prefix_block(Some(runner.rust_profile), block_idx);
        let blocks_remaining_in_request = self.request_blocks_total.saturating_sub(block_idx + 1);
        let is_first_block_in_request = block_idx == 0;
        let is_last_block_in_request = block_idx + 1 == self.request_blocks_total;
        let reloaded_after_eviction = runner.evicted_blocks_seen.remove(&block);

        if is_first_block_in_request {
            apply_request_execution_context_command(
                runner.runtime,
                self.task,
                self.ubpu_node,
                ExecutionLifecycle::Warmup,
                true,
            )?;
        }

        let compute_view = w4_resolve_compute_view(
            W4KvCacheDeps {
                surface: runner.surface,
                store: runner.store,
                planner: runner.planner,
                topology: runner.topology,
                report: runner.report,
                cmdq: runner.cmdq,
                cq: runner.cq,
                prefix_segment: runner.prefix_segment,
                prefix_segment_seeded: runner.prefix_segment_seeded,
                kv_hot_segments: runner.kv_hot_segments,
                block_result_segments: runner.block_result_segments,
                seeded_db_keys: runner.seeded_db_keys,
                seeded_dfs_paths: runner.seeded_dfs_paths,
                allow_queue_retry: runner.allow_queue_retry,
                rust_profile: Some(runner.rust_profile),
            },
            self.task,
            self.request.request_idx + block_idx,
            block,
            is_prefix_block,
            reloaded_after_eviction,
            uses_dfs_fallback,
        )?;

        let mut dispatch_inputs = vec![runner.request_segment, runner.control_segment];
        if let Some(previous_result_segment) = self.previous_result_segment {
            dispatch_inputs.push(previous_result_segment);
        }
        dispatch_inputs.extend(compute_view.shared_segments.iter().copied());
        self.previous_result_segment = Some(run_rust_llm_backend_step(
            runner.runtime,
            runner.topology,
            runner.report,
            w4_active_step_spec(
                &self.w4_common,
                block_idx,
                blocks_remaining_in_request,
                compute_view.input_segment,
                dispatch_inputs,
                &compute_view.callable_hint,
                compute_view.block_hash.clone(),
                is_first_block_in_request,
                is_last_block_in_request,
                compute_view.kvcache_resolution_kind,
                compute_view.kvcache_view_kind,
                compute_view.kvcache_transition_kind,
                compute_view.prefix_group,
                compute_view.route_from_level,
                compute_view.route_to_level,
                compute_view.route_selected_node,
                compute_view.route_reason,
                compute_view.placement_level,
                compute_view.placement_node,
                compute_view.promoted_this_access,
                compute_view.reloaded_after_eviction,
                compute_view.uses_dfs_fallback,
                if is_prefix_block {
                    Some(runner.prefix_segment)
                } else {
                    None
                },
            ),
        )?);
        w4_record_request_result(
            runner.surface,
            runner.report,
            runner.cmdq,
            runner.cq,
            runner.request_segment,
            runner.control_segment,
            runner.request_result_segments,
            self.task,
            self.previous_result_segment.expect("active result set"),
        )?;
        w4_record_kvcache_result(
            W4KvCacheDeps {
                surface: runner.surface,
                store: runner.store,
                planner: runner.planner,
                topology: runner.topology,
                report: runner.report,
                cmdq: runner.cmdq,
                cq: runner.cq,
                prefix_segment: runner.prefix_segment,
                prefix_segment_seeded: runner.prefix_segment_seeded,
                kv_hot_segments: runner.kv_hot_segments,
                block_result_segments: runner.block_result_segments,
                seeded_db_keys: runner.seeded_db_keys,
                seeded_dfs_paths: runner.seeded_dfs_paths,
                allow_queue_retry: runner.allow_queue_retry,
                rust_profile: Some(runner.rust_profile),
            },
            self.task,
            &compute_view.block_hash,
            self.previous_result_segment.expect("active result set"),
        )?;
        Ok(())
    }

    fn finish(&mut self, runner: &mut RustLlmRequestRunner<'_>) -> Result<(), SimError> {
        w4_finish_request_control(
            runner.surface,
            runner.report,
            runner.cmdq,
            runner.cq,
            runner.request_segment,
            runner.control_segment,
            runner.request_result_segments,
            self.task,
        )?;
        let mut finish_inputs = vec![runner.request_segment];
        if let Some(previous_result_segment) =
            w4_latest_request_result(runner.request_result_segments, self.task)
        {
            finish_inputs.push(previous_result_segment);
        }
        let finish_result_segment = run_rust_llm_backend_step(
            runner.runtime,
            runner.topology,
            runner.report,
            w4_finish_step_spec(&self.w4_common, finish_inputs),
        )?;
        w4_record_request_result(
            runner.surface,
            runner.report,
            runner.cmdq,
            runner.cq,
            runner.request_segment,
            runner.control_segment,
            runner.request_result_segments,
            self.task,
            finish_result_segment,
        )?;
        apply_request_execution_context_command(
            runner.runtime,
            self.task,
            self.ubpu_node,
            ExecutionLifecycle::Teardown,
            false,
        )?;
        Ok(())
    }
}

impl<'a> RustLlmRequestRunner<'a> {
    fn run_request(
        &mut self,
        task: &TaskKey,
        request: &RustLlmRequestSpec,
    ) -> Result<(), SimError> {
        let ubpu_node = self
            .topology
            .ubpus
            .iter()
            .find(|ubpu| ubpu.host_id == 0)
            .map(|ubpu| ubpu.node_id)
            .unwrap_or(self.topology.hosts[0].node_id);
        let mut slice = RustLlmExecutionSlice::new(
            task,
            request,
            ubpu_node,
            self.report,
            self.request_segment,
            self.control_segment,
            self.rust_profile,
        );

        for step in &request.steps {
            match step {
                RustLlmRequestStep::Begin => slice.begin(self)?,
                RustLlmRequestStep::Active { block_idx, block } => {
                    slice.run_active_block(self, *block_idx, block.clone())?;
                }
                RustLlmRequestStep::Finish => slice.finish(self)?,
            }
        }
        Ok(())
    }
}

pub fn run_minimal_workload(
    config: &ScenarioConfig,
    topology: &SimTopology,
) -> Result<WorkloadRunReport, sim_core::SimError> {
    if let WorkloadConfig::DualNodeShmemMailbox(cfg) = &config.workload {
        return run_dual_node_shmem_mailbox_workload(config, topology, cfg);
    }
    if let WorkloadConfig::DualNodeBlockCompute(cfg) = &config.workload {
        return run_dual_node_block_compute_workload(config, topology, cfg);
    }
    if let WorkloadConfig::DualNodeCacheFill(cfg) = &config.workload {
        return run_dual_node_cache_fill_workload(config, topology, cfg);
    }

    let mut store = InMemoryBlockStore::from_config(config);
    let planner = RecursiveRoutePlanner::from_config(config);
    let mut seeded_dfs_paths = HashSet::new();

    let (workload_kind, workload_profile, requests_total, blocks_per_request, unique_prefixes) =
        match &config.workload {
            WorkloadConfig::HotsetLoop(cfg) => (
                "hotset_loop".to_string(),
                "default".to_string(),
                cfg.qps.min(4),
                cfg.blocks_per_request,
                cfg.unique_prefixes.max(1),
            ),
            WorkloadConfig::TraceReplay(_) => {
                ("trace_replay".to_string(), "default".to_string(), 2, 1, 2)
            }
            WorkloadConfig::DualNodeShmemMailbox(_) => unreachable!("handled above"),
            WorkloadConfig::DualNodeBlockCompute(_) => unreachable!("handled above"),
            WorkloadConfig::DualNodeCacheFill(_) => unreachable!("handled above"),
            WorkloadConfig::RustLlmMvp(cfg) => {
                let profile = rust_llm_profile(&cfg.profile);
                (
                    "rust_llm_server_mvp".to_string(),
                    profile.name.to_string(),
                    cfg.qps.min(profile.requests_total_cap),
                    cfg.blocks_per_request
                        .max((profile.prefix_blocks + profile.tail_blocks) as u32),
                    cfg.unique_prefixes.max(profile.prefix_groups),
                )
            }
        };

    let rust_profile = match &config.workload {
        WorkloadConfig::RustLlmMvp(cfg) => Some(rust_llm_profile(&cfg.profile)),
        _ => None,
    };
    let allow_queue_retry =
        matches!(rust_profile, Some(profile) if profile.name == "capacity_pressure");
    let mut surface = if matches!(rust_profile, Some(profile) if profile.name == "capacity_pressure")
    {
        LocalGuestUapiSurface::with_block_profile(
            topology.clone(),
            BlockServiceProfile {
                queue_depth: 2,
                ..BlockServiceProfile::default()
            },
        )
    } else {
        LocalGuestUapiSurface::new(topology.clone())
    };
    let mut runtime = LocalRuntimeEngine::from_config(config);
    let cq = match surface.execute(UapiCommand::RegisterCq { owner: 0 })? {
        UapiResponse::CqRegistered(cq) => cq,
        _ => {
            return Err(sim_core::SimError::InvalidInput(
                "unexpected cq registration response",
            ))
        }
    };
    let cmdq = match surface.execute(UapiCommand::CreateCmdQueue {
        cq,
        owner: 0,
        depth: 32,
    })? {
        UapiResponse::CmdQueueCreated(cmdq) => cmdq,
        _ => {
            return Err(sim_core::SimError::InvalidInput(
                "unexpected command queue creation response",
            ))
        }
    };
    let prefix_segment = match surface.execute(UapiCommand::CreateSegment { bytes: 4096 })? {
        UapiResponse::SegmentCreated(segment) => segment,
        _ => {
            return Err(sim_core::SimError::InvalidInput(
                "unexpected shmem segment creation response",
            ))
        }
    };
    let request_segment = match surface.execute(UapiCommand::CreateSegment { bytes: 512 })? {
        UapiResponse::SegmentCreated(segment) => segment,
        _ => {
            return Err(sim_core::SimError::InvalidInput(
                "unexpected request shmem segment creation response",
            ))
        }
    };
    let control_segment = match surface.execute(UapiCommand::CreateSegment { bytes: 512 })? {
        UapiResponse::SegmentCreated(segment) => segment,
        _ => {
            return Err(sim_core::SimError::InvalidInput(
                "unexpected control shmem segment creation response",
            ))
        }
    };
    let mut prefix_segment_seeded = false;
    let mut kv_hot_segments: HashMap<BlockHash, SegmentHandle> = HashMap::new();
    let mut block_result_segments: HashMap<BlockHash, SegmentHandle> = HashMap::new();
    let mut request_result_segments: HashMap<u64, SegmentHandle> = HashMap::new();
    let mut evicted_blocks_seen: HashSet<BlockHash> = HashSet::new();
    let mut seeded_db_keys = HashSet::new();

    let mut report = base_workload_report(workload_kind, workload_profile, requests_total);

    for request_idx in 0..requests_total {
        let task = TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: request_idx + 1,
        };
        report.events.push(SimEvent::TaskCreated {
            at: request_idx,
            task: task.clone(),
        });
        if report.workload_kind == "rust_llm_server_mvp" {
            let request = RustLlmRequestSpec::build(
                &report.workload_kind,
                rust_profile.expect("rust profile"),
                request_idx,
                blocks_per_request,
                unique_prefixes,
            );
            RustLlmRequestRunner {
                runtime: &mut runtime,
                surface: &mut surface,
                store: &mut store,
                planner: &planner,
                topology,
                report: &mut report,
                cmdq,
                cq,
                prefix_segment,
                request_segment,
                control_segment,
                prefix_segment_seeded: &mut prefix_segment_seeded,
                kv_hot_segments: &mut kv_hot_segments,
                block_result_segments: &mut block_result_segments,
                request_result_segments: &mut request_result_segments,
                evicted_blocks_seen: &mut evicted_blocks_seen,
                seeded_db_keys: &mut seeded_db_keys,
                seeded_dfs_paths: &mut seeded_dfs_paths,
                rust_profile: rust_profile.expect("rust profile"),
                allow_queue_retry,
            }
            .run_request(&task, &request)?;
            continue;
        }

        for block_idx in 0..u64::from(blocks_per_request) {
            report.blocks_total += 1;
            let block = block_for_request(
                &report.workload_kind,
                rust_profile,
                request_idx,
                block_idx,
                unique_prefixes,
            );
            let is_prefix_block = is_prefix_block(rust_profile, block_idx);
            let _ = evicted_blocks_seen.remove(&block);

            let lookup = store.lookup(&block);
            if lookup.found {
                report.hits += 1;
                if is_prefix_block {
                    report.prefix_hits += 1;
                }
                continue;
            }

            report.misses += 1;
            if !is_prefix_block {
                report.tail_misses += 1;
            }
            let decision = planner.plan(
                RouteRequest {
                    task: task.clone(),
                    current_level: PlLevel::L4,
                    block: block.clone(),
                },
                topology,
            )?;
            report.events.push(SimEvent::RoutePlanned {
                at: request_idx + block_idx,
                task: task.clone(),
                decision,
            });

            store.stage_insert(PromotionPlan {
                block: block.clone(),
            })?;
            report.promotions += 1;

            if let Some(placement) = store.lookup(&block).placement {
                report.events.push(SimEvent::BlockPromoted {
                    at: request_idx + block_idx + 1,
                    block: block.clone(),
                    placement,
                });
            }

            submit_block_write(
                &mut surface,
                cq,
                &mut report,
                task.clone(),
                block,
                allow_queue_retry,
            )?;
            if !matches!(rust_profile, Some(profile) if profile.name == "capacity_pressure") {
                drain_and_record(
                    &mut surface,
                    cq,
                    &mut report,
                    "unexpected cq drain response",
                )?;
            }
        }

        if let Some(profile) = rust_profile {
            if profile.evict_after_request > 0 {
                let evicted = store.evict(sim_runtime::EvictionPlan {
                    max_blocks: profile.evict_after_request,
                })?;
                report.evictions += evicted.len() as u64;
                for block in evicted {
                    evicted_blocks_seen.insert(block.clone());
                    submit_block_writeback(
                        &mut surface,
                        cq,
                        &mut report,
                        Some(task.clone()),
                        block.clone(),
                    )?;
                    report.events.push(SimEvent::BlockEvicted {
                        at: request_idx + report.blocks_total,
                        from: sim_core::BlockPlacement {
                            block: block.clone(),
                            level: PlLevel::L2,
                            node: 0,
                        },
                        block,
                    });
                }
            }
        }

        drain_and_record(
            &mut surface,
            cq,
            &mut report,
            "unexpected cq drain response after request",
        )?;
    }

    let evicted = store.evict(sim_runtime::EvictionPlan { max_blocks: 1 })?;
    report.evictions += evicted.len() as u64;
    for block in evicted {
        evicted_blocks_seen.insert(block.clone());
        submit_block_writeback(&mut surface, cq, &mut report, None, block.clone())?;
        report.events.push(SimEvent::BlockEvicted {
            at: requests_total + report.blocks_total,
            from: sim_core::BlockPlacement {
                block: block.clone(),
                level: PlLevel::L2,
                node: 0,
            },
            block,
        });
    }
    drain_and_record(
        &mut surface,
        cq,
        &mut report,
        "unexpected final cq drain response",
    )?;

    report.summary = summarize_events(&report.events);
    Ok(report)
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RustLlmMvpSmokeConfig {
    pub hosts: u32,
    pub ubpus_per_host: u32,
    pub entities_per_ubpu: u32,
    pub profile: String,
    pub qps: u64,
    pub unique_prefixes: u64,
    pub blocks_per_request: u32,
}

impl Default for RustLlmMvpSmokeConfig {
    fn default() -> Self {
        Self {
            hosts: 8,
            ubpus_per_host: 2,
            entities_per_ubpu: 2,
            profile: "single_domain_basic".to_string(),
            qps: 1,
            unique_prefixes: 2,
            blocks_per_request: 4,
        }
    }
}

pub fn run_rust_llm_mvp_smoke(
    cfg: &RustLlmMvpSmokeConfig,
) -> Result<WorkloadRunReport, sim_core::SimError> {
    if cfg.hosts == 0 {
        return Err(sim_core::SimError::InvalidInput(
            "rust_llm_mvp_smoke.hosts must be positive",
        ));
    }
    if cfg.ubpus_per_host == 0 {
        return Err(sim_core::SimError::InvalidInput(
            "rust_llm_mvp_smoke.ubpus_per_host must be positive",
        ));
    }
    if cfg.entities_per_ubpu == 0 {
        return Err(sim_core::SimError::InvalidInput(
            "rust_llm_mvp_smoke.entities_per_ubpu must be positive",
        ));
    }
    if cfg.qps == 0 {
        return Err(sim_core::SimError::InvalidInput(
            "rust_llm_mvp_smoke.qps must be positive",
        ));
    }
    if cfg.unique_prefixes == 0 {
        return Err(sim_core::SimError::InvalidInput(
            "rust_llm_mvp_smoke.unique_prefixes must be positive",
        ));
    }
    if cfg.blocks_per_request == 0 {
        return Err(sim_core::SimError::InvalidInput(
            "rust_llm_mvp_smoke.blocks_per_request must be positive",
        ));
    }

    let hosts = (0..cfg.hosts)
        .map(|host| host.to_string())
        .collect::<Vec<_>>()
        .join(", ");
    let yaml = format!(
        r#"
scenario:
  name: rust_llm_mvp_smoke
  group: W4
  variant: rust_llm_server_backend
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
  hosts: {hosts_count}
  ubpus_per_host: {ubpus_per_host}
  entities_per_ubpu: {entities_per_ubpu}
  ub_domains:
    - id: domain0
      hosts: [{hosts}]
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
    pe_count: {hosts_count}
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
  profile: {profile}
  qps: {qps}
  unique_prefixes: {unique_prefixes}
  blocks_per_request: {blocks_per_request}
  function_label_mode: host_orchestration
faults: []
outputs:
  trace: true
  metrics_csv: true
  summary_json: true
  emit_task_coord_trace: true
  emit_data_service_trace: true
  emit_qemu_platform_trace: true
"#,
        hosts_count = cfg.hosts,
        ubpus_per_host = cfg.ubpus_per_host,
        entities_per_ubpu = cfg.entities_per_ubpu,
        profile = cfg.profile,
        qps = cfg.qps,
        unique_prefixes = cfg.unique_prefixes,
        blocks_per_request = cfg.blocks_per_request,
    );
    let config = ScenarioConfig::from_yaml_str(&yaml)
        .map_err(|_| sim_core::SimError::InvalidInput("invalid rust_llm_mvp_smoke config"))?;
    let topology = SimTopology::from_config(&config)
        .map_err(|_| sim_core::SimError::InvalidInput("invalid rust_llm_mvp_smoke topology"))?;
    run_minimal_workload(&config, &topology)
}

fn ensure_kv_hot_segment(
    surface: &mut LocalGuestUapiSurface,
    kv_hot_segments: &mut HashMap<BlockHash, SegmentHandle>,
    block: &BlockHash,
) -> Result<SegmentHandle, SimError> {
    if let Some(segment) = kv_hot_segments.get(block) {
        return Ok(*segment);
    }

    let segment = match surface.execute(UapiCommand::CreateSegment { bytes: 4096 })? {
        UapiResponse::SegmentCreated(segment) => segment,
        _ => {
            return Err(SimError::InvalidInput(
                "unexpected kv hot shmem segment creation response",
            ))
        }
    };
    kv_hot_segments.insert(block.clone(), segment);
    Ok(segment)
}

fn rust_llm_begin_request_control(
    surface: &mut LocalGuestUapiSurface,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: &TaskKey,
    request_segment: SegmentHandle,
) -> Result<(), SimError> {
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: request_segment,
            bytes: 256,
        }),
        "unexpected request shmem put enqueue response",
        "unexpected request shmem put doorbell response",
    )?;
    report.shmem_puts += 1;
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: request_segment,
            bytes: 256,
        }),
        "unexpected request shmem get enqueue response",
        "unexpected request shmem get doorbell response",
    )?;
    report.shmem_gets += 1;
    drain_and_record(
        surface,
        cq,
        report,
        "unexpected request shmem cq drain response",
    )
}

fn rust_llm_finish_request_control(
    surface: &mut LocalGuestUapiSurface,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: &TaskKey,
    control_segment: SegmentHandle,
) -> Result<(), SimError> {
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: control_segment,
            bytes: 256,
        }),
        "unexpected control shmem put enqueue response",
        "unexpected control shmem put doorbell response",
    )?;
    report.shmem_puts += 1;
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: control_segment,
            bytes: 256,
        }),
        "unexpected control shmem get enqueue response",
        "unexpected control shmem get doorbell response",
    )?;
    report.shmem_gets += 1;
    drain_and_record(
        surface,
        cq,
        report,
        "unexpected control shmem cq drain response",
    )
}

fn rust_llm_sync_prefix_shared_view(
    surface: &mut LocalGuestUapiSurface,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: &TaskKey,
    block: &BlockHash,
    prefix_segment: SegmentHandle,
    prefix_segment_seeded: &mut bool,
    seeded_db_keys: &mut HashSet<String>,
) -> Result<(), SimError> {
    let db_key = format!("prefix-meta:{}", block.0);
    if seeded_db_keys.insert(db_key.clone()) {
        enqueue_descriptor_and_ring(
            surface,
            cmdq,
            UapiDescriptor::DbPut(DbPutReq {
                task: Some(task.clone()),
                key: db_key.clone(),
                bytes: 32,
            }),
            "unexpected db put enqueue response",
            "unexpected db put doorbell response",
        )?;
        report.db_puts += 1;
    }
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::DbGet(DbGetReq {
            task: Some(task.clone()),
            key: db_key,
        }),
        "unexpected db get enqueue response",
        "unexpected db get doorbell response",
    )?;
    report.db_gets += 1;
    if !*prefix_segment_seeded {
        enqueue_descriptor_and_ring(
            surface,
            cmdq,
            UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
                task: Some(task.clone()),
                requester_entity: 0,
                segment: prefix_segment,
                bytes: 4096,
            }),
            "unexpected shmem put enqueue response",
            "unexpected shmem put doorbell response",
        )?;
        report.shmem_puts += 1;
        *prefix_segment_seeded = true;
    }
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: prefix_segment,
            bytes: 4096,
        }),
        "unexpected shmem get enqueue response",
        "unexpected shmem get doorbell response",
    )?;
    report.shmem_gets += 1;
    drain_and_record(surface, cq, report, "unexpected shmem cq drain response")
}

fn rust_llm_materialize_hot_view_for_hit(
    surface: &mut LocalGuestUapiSurface,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: &TaskKey,
    kv_hot_segments: &mut HashMap<BlockHash, SegmentHandle>,
    block: &BlockHash,
) -> Result<SegmentHandle, SimError> {
    let needs_kv_hot_seed = !kv_hot_segments.contains_key(block);
    let kv_hot_segment = ensure_kv_hot_segment(surface, kv_hot_segments, block)?;
    if needs_kv_hot_seed {
        enqueue_descriptor_and_ring(
            surface,
            cmdq,
            UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
                task: Some(task.clone()),
                requester_entity: 0,
                segment: kv_hot_segment,
                bytes: 4096,
            }),
            "unexpected kv hot shmem put enqueue response",
            "unexpected kv hot shmem put doorbell response",
        )?;
        report.shmem_puts += 1;
    }
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: kv_hot_segment,
            bytes: 4096,
        }),
        "unexpected kv hot shmem get enqueue response",
        "unexpected kv hot shmem get doorbell response",
    )?;
    report.shmem_gets += 1;
    drain_and_record(
        surface,
        cq,
        report,
        "unexpected kv hot shmem cq drain response",
    )?;
    Ok(kv_hot_segment)
}

fn rust_llm_fill_hot_view_from_block(
    surface: &mut LocalGuestUapiSurface,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: &TaskKey,
    kv_hot_segments: &mut HashMap<BlockHash, SegmentHandle>,
    block: BlockHash,
    allow_queue_retry: bool,
    rust_profile: Option<RustLlmProfile>,
) -> Result<SegmentHandle, SimError> {
    let kv_hot_segment = ensure_kv_hot_segment(surface, kv_hot_segments, &block)?;
    submit_block_write(surface, cq, report, task.clone(), block, allow_queue_retry)?;
    if !matches!(rust_profile, Some(profile) if profile.name == "capacity_pressure") {
        drain_and_record(surface, cq, report, "unexpected cq drain response")?;
    }
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: kv_hot_segment,
            bytes: 4096,
        }),
        "unexpected kv fill shmem put enqueue response",
        "unexpected kv fill shmem put doorbell response",
    )?;
    report.shmem_puts += 1;
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: kv_hot_segment,
            bytes: 4096,
        }),
        "unexpected kv fill shmem get enqueue response",
        "unexpected kv fill shmem get doorbell response",
    )?;
    report.shmem_gets += 1;
    drain_and_record(
        surface,
        cq,
        report,
        "unexpected kv fill shmem cq drain response",
    )?;
    Ok(kv_hot_segment)
}

fn rust_llm_refresh_hot_view_from_result(
    surface: &mut LocalGuestUapiSurface,
    cmdq: sim_core::CmdQueueHandle,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: &TaskKey,
    hot_segment: SegmentHandle,
) -> Result<(), SimError> {
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: hot_segment,
            bytes: 4096,
        }),
        "unexpected kv refresh shmem put enqueue response",
        "unexpected kv refresh shmem put doorbell response",
    )?;
    report.shmem_puts += 1;
    enqueue_descriptor_and_ring(
        surface,
        cmdq,
        UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
            task: Some(task.clone()),
            requester_entity: 0,
            segment: hot_segment,
            bytes: 4096,
        }),
        "unexpected kv refresh shmem get enqueue response",
        "unexpected kv refresh shmem get doorbell response",
    )?;
    report.shmem_gets += 1;
    drain_and_record(
        surface,
        cq,
        report,
        "unexpected kv refresh shmem cq drain response",
    )
}

fn run_dual_node_shmem_mailbox_workload(
    config: &ScenarioConfig,
    topology: &SimTopology,
    cfg: &DualNodeShmemMailboxWorkloadConfig,
) -> Result<WorkloadRunReport, SimError> {
    if topology.hosts.len() < 2 {
        return Err(SimError::InvalidInput(
            "dual-node shmem mailbox requires at least 2 hosts",
        ));
    }
    if config.pypto.simpler_boundary.chip_backend_mode == "stub" {
        return Err(SimError::InvalidInput(
            "dual-node shmem mailbox requires non-stub chip backend mode",
        ));
    }

    let host_b = &topology.hosts[1];
    let ubpu_b = topology
        .ubpus
        .iter()
        .find(|ubpu| ubpu.host_id == host_b.id)
        .ok_or(SimError::NotFound("ubpu for host_b"))?;

    let mut surface = LocalGuestUapiSurface::new(topology.clone());
    let cq = match surface.execute(UapiCommand::RegisterCq { owner: 0 })? {
        UapiResponse::CqRegistered(cq) => cq,
        _ => {
            return Err(SimError::InvalidInput(
                "unexpected cq registration response",
            ))
        }
    };

    let payload_segment = match surface.execute(UapiCommand::CreateSegment {
        bytes: cfg.payload_bytes,
    })? {
        UapiResponse::SegmentCreated(segment) => segment,
        _ => {
            return Err(SimError::InvalidInput(
                "unexpected payload segment response",
            ))
        }
    };
    let result_segment = match surface.execute(UapiCommand::CreateSegment {
        bytes: cfg.payload_bytes,
    })? {
        UapiResponse::SegmentCreated(segment) => segment,
        _ => return Err(SimError::InvalidInput("unexpected result segment response")),
    };
    let ack_segment = match surface.execute(UapiCommand::CreateSegment {
        bytes: cfg.payload_bytes,
    })? {
        UapiResponse::SegmentCreated(segment) => segment,
        _ => return Err(SimError::InvalidInput("unexpected ack segment response")),
    };

    let mut runtime = LocalRuntimeEngine::from_config(config);
    let mut report = base_workload_report(
        "dual_node_shmem_mailbox".to_string(),
        "mailbox".to_string(),
        cfg.rounds,
    );

    for round in 0..cfg.rounds {
        let task_a = mailbox_task(round, 0);
        let task_b = mailbox_task(round, 1);

        report.events.push(SimEvent::TaskCreated {
            at: round * 10,
            task: task_a.clone(),
        });
        report.events.push(SimEvent::TaskCreated {
            at: round * 10 + 1,
            task: task_b.clone(),
        });

        match surface.execute(UapiCommand::SubmitShmemPut {
            req: ShmemPutReq {
                task: Some(task_a.clone()),
                requester_entity: 0,
                segment: payload_segment,
                bytes: cfg.payload_bytes,
            },
        })? {
            UapiResponse::IoSubmitted(_) => report.shmem_puts += 1,
            _ => return Err(SimError::InvalidInput("unexpected shmem put response")),
        }
        drain_and_record(
            &mut surface,
            cq,
            &mut report,
            "unexpected cq drain response after payload put",
        )?;

        match surface.execute(UapiCommand::SubmitShmemGet {
            req: ShmemGetReq {
                task: Some(task_b.clone()),
                requester_entity: 1,
                segment: payload_segment,
                bytes: cfg.payload_bytes,
            },
        })? {
            UapiResponse::IoSubmitted(_) => report.shmem_gets += 1,
            _ => return Err(SimError::InvalidInput("unexpected shmem get response")),
        }
        drain_and_record(
            &mut surface,
            cq,
            &mut report,
            "unexpected cq drain response after payload get",
        )?;

        let stage_segment = SegmentHandle(10_000 + round);
        let device_result_segment = SegmentHandle(20_000 + round);
        let mut sink = VecEventSink::default();

        runtime.submit_copy(CopyRequest {
            task: task_b.clone(),
            direction: CopyDirection::HostToDevice,
            bytes: cfg.payload_bytes,
            src: MemoryEndpoint {
                node: host_b.node_id,
                segment: payload_segment,
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: ubpu_b.node_id,
                segment: stage_segment,
                offset: 0,
            },
        })?;
        runtime.submit_dispatch(
            DispatchRequest {
                task: task_b.clone(),
                function: FunctionLabel {
                    name: "w1_shmem_mailbox_transform".to_string(),
                    level: PlLevel::L2,
                },
                backend_spec: Some(empty_simpler_backend_spec(
                    DispatchBackendProfile::HostVector,
                    DispatchRuntimeVariant::HostBuildGraph,
                    "w1_shmem_mailbox_transform",
                )),
                request: None,
                target_level: PlLevel::L2,
                target_node: ubpu_b.node_id,
                input_segments: vec![stage_segment],
            },
            &mut sink,
        )?;
        runtime.submit_copy(CopyRequest {
            task: task_b.clone(),
            direction: CopyDirection::DeviceToHost,
            bytes: cfg.payload_bytes,
            src: MemoryEndpoint {
                node: ubpu_b.node_id,
                segment: device_result_segment,
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: host_b.node_id,
                segment: result_segment,
                offset: 0,
            },
        })?;
        let completions = runtime.poll_completions(runtime.now().saturating_add(256), &mut sink);
        report.completions += completions.len() as u64;
        report.events.extend(sink.into_events());

        match surface.execute(UapiCommand::SubmitShmemPut {
            req: ShmemPutReq {
                task: Some(task_b.clone()),
                requester_entity: 1,
                segment: ack_segment,
                bytes: cfg.payload_bytes,
            },
        })? {
            UapiResponse::IoSubmitted(_) => report.shmem_puts += 1,
            _ => return Err(SimError::InvalidInput("unexpected shmem ack put response")),
        }
        drain_and_record(
            &mut surface,
            cq,
            &mut report,
            "unexpected cq drain response after ack put",
        )?;

        match surface.execute(UapiCommand::SubmitShmemGet {
            req: ShmemGetReq {
                task: Some(task_a.clone()),
                requester_entity: 0,
                segment: ack_segment,
                bytes: cfg.payload_bytes,
            },
        })? {
            UapiResponse::IoSubmitted(_) => report.shmem_gets += 1,
            _ => return Err(SimError::InvalidInput("unexpected shmem ack get response")),
        }
        drain_and_record(
            &mut surface,
            cq,
            &mut report,
            "unexpected cq drain response after ack get",
        )?;
    }

    report.summary = summarize_events(&report.events);
    Ok(report)
}

fn run_dual_node_block_compute_workload(
    config: &ScenarioConfig,
    topology: &SimTopology,
    cfg: &DualNodeBlockComputeWorkloadConfig,
) -> Result<WorkloadRunReport, SimError> {
    if topology.hosts.len() < 2 {
        return Err(SimError::InvalidInput(
            "dual-node block compute requires at least 2 hosts",
        ));
    }
    if config.pypto.simpler_boundary.chip_backend_mode == "stub" {
        return Err(SimError::InvalidInput(
            "dual-node block compute requires non-stub chip backend mode",
        ));
    }

    let host_a = &topology.hosts[0];
    let ubpu_a = topology
        .ubpus
        .iter()
        .find(|ubpu| ubpu.host_id == host_a.id)
        .ok_or(SimError::NotFound("ubpu for host_a"))?;

    let mut surface = LocalGuestUapiSurface::new(topology.clone());
    let cq = match surface.execute(UapiCommand::RegisterCq { owner: 0 })? {
        UapiResponse::CqRegistered(cq) => cq,
        _ => {
            return Err(SimError::InvalidInput(
                "unexpected cq registration response",
            ))
        }
    };

    let mut runtime = LocalRuntimeEngine::from_config(config);
    let mut report = base_workload_report(
        "dual_node_block_compute".to_string(),
        "read_compute_write".to_string(),
        cfg.rounds,
    );

    for round in 0..cfg.rounds {
        let task_a = mailbox_task(round, 0);
        let task_b = mailbox_task(round, 1);
        let source_block = BlockHash(format!("w2-source-block-{round}"));
        let result_block = BlockHash(format!("w2-result-block-{round}"));

        report.blocks_total += 1;
        report.events.push(SimEvent::TaskCreated {
            at: round * 10,
            task: task_a.clone(),
        });
        report.events.push(SimEvent::TaskCreated {
            at: round * 10 + 1,
            task: task_b.clone(),
        });

        submit_block_write(
            &mut surface,
            cq,
            &mut report,
            task_b.clone(),
            source_block.clone(),
            false,
        )?;
        drain_and_record(
            &mut surface,
            cq,
            &mut report,
            "unexpected cq drain response after block seed write",
        )?;

        submit_block_read(
            &mut surface,
            cq,
            &mut report,
            task_a.clone(),
            source_block,
            false,
        )?;
        drain_and_record(
            &mut surface,
            cq,
            &mut report,
            "unexpected cq drain response after block read",
        )?;

        let stage_segment = SegmentHandle(30_000 + round);
        let device_result_segment = SegmentHandle(40_000 + round);
        let host_result_segment = SegmentHandle(50_000 + round);
        let mut sink = VecEventSink::default();

        runtime.submit_copy(CopyRequest {
            task: task_a.clone(),
            direction: CopyDirection::HostToDevice,
            bytes: 4096,
            src: MemoryEndpoint {
                node: host_a.node_id,
                segment: stage_segment,
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: ubpu_a.node_id,
                segment: stage_segment,
                offset: 0,
            },
        })?;
        runtime.submit_dispatch(
            DispatchRequest {
                task: task_a.clone(),
                function: FunctionLabel {
                    name: "w2_block_transform".to_string(),
                    level: PlLevel::L2,
                },
                backend_spec: Some(empty_simpler_backend_spec(
                    DispatchBackendProfile::HostVector,
                    DispatchRuntimeVariant::HostBuildGraph,
                    "w2_block_transform",
                )),
                request: None,
                target_level: PlLevel::L2,
                target_node: ubpu_a.node_id,
                input_segments: vec![stage_segment],
            },
            &mut sink,
        )?;
        runtime.submit_copy(CopyRequest {
            task: task_a.clone(),
            direction: CopyDirection::DeviceToHost,
            bytes: 4096,
            src: MemoryEndpoint {
                node: ubpu_a.node_id,
                segment: device_result_segment,
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: host_a.node_id,
                segment: host_result_segment,
                offset: 0,
            },
        })?;
        let completions = runtime.poll_completions(runtime.now().saturating_add(256), &mut sink);
        report.completions += completions.len() as u64;
        report.events.extend(sink.into_events());

        submit_block_write(
            &mut surface,
            cq,
            &mut report,
            task_a.clone(),
            result_block,
            false,
        )?;
        drain_and_record(
            &mut surface,
            cq,
            &mut report,
            "unexpected cq drain response after result block write",
        )?;
    }

    report.summary = summarize_events(&report.events);
    Ok(report)
}

fn run_dual_node_cache_fill_workload(
    config: &ScenarioConfig,
    topology: &SimTopology,
    cfg: &DualNodeCacheFillWorkloadConfig,
) -> Result<WorkloadRunReport, SimError> {
    if topology.hosts.len() < 2 {
        return Err(SimError::InvalidInput(
            "dual-node cache fill requires at least 2 hosts",
        ));
    }
    if config.pypto.simpler_boundary.chip_backend_mode == "stub" {
        return Err(SimError::InvalidInput(
            "dual-node cache fill requires non-stub chip backend mode",
        ));
    }

    let host_a = &topology.hosts[0];
    let ubpu_a = topology
        .ubpus
        .iter()
        .find(|ubpu| ubpu.host_id == host_a.id)
        .ok_or(SimError::NotFound("ubpu for host_a"))?;
    let mut surface = LocalGuestUapiSurface::new(topology.clone());
    let cq = match surface.execute(UapiCommand::RegisterCq { owner: 0 })? {
        UapiResponse::CqRegistered(cq) => cq,
        _ => {
            return Err(SimError::InvalidInput(
                "unexpected cq registration response",
            ))
        }
    };
    let planner = RecursiveRoutePlanner::from_config(config);
    let mut store = InMemoryBlockStore::from_config(config);
    let mut runtime = LocalRuntimeEngine::from_config(config);
    let mut report = base_workload_report(
        "dual_node_cache_fill".to_string(),
        "fetch_fill_hit".to_string(),
        cfg.rounds,
    );
    let block = BlockHash("w3-shared-hot-block".to_string());
    let seed_task = mailbox_task(0, 1);

    submit_block_write(
        &mut surface,
        cq,
        &mut report,
        seed_task,
        block.clone(),
        false,
    )?;
    drain_and_record(
        &mut surface,
        cq,
        &mut report,
        "unexpected cq drain response after cache-fill seed write",
    )?;

    for round in 0..cfg.rounds {
        let task = mailbox_task(round, 0);
        report.blocks_total += 1;
        report.events.push(SimEvent::TaskCreated {
            at: round * 10,
            task: task.clone(),
        });

        let lookup = store.lookup(&block);
        if lookup.found {
            report.hits += 1;
        } else {
            report.misses += 1;
            let decision = planner.plan(
                RouteRequest {
                    task: task.clone(),
                    current_level: PlLevel::L4,
                    block: block.clone(),
                },
                topology,
            )?;
            report.events.push(SimEvent::RoutePlanned {
                at: round * 10 + 1,
                task: task.clone(),
                decision,
            });
            submit_block_read(
                &mut surface,
                cq,
                &mut report,
                task.clone(),
                block.clone(),
                false,
            )?;
            drain_and_record(
                &mut surface,
                cq,
                &mut report,
                "unexpected cq drain response after remote fetch",
            )?;
            store.stage_insert(PromotionPlan {
                block: block.clone(),
            })?;
            report.promotions += 1;
            if let Some(placement) = store.lookup(&block).placement {
                report.events.push(SimEvent::BlockPromoted {
                    at: round * 10 + 2,
                    block: block.clone(),
                    placement,
                });
            }
        }

        let stage_segment = SegmentHandle(60_000 + round);
        let device_result_segment = SegmentHandle(70_000 + round);
        let host_result_segment = SegmentHandle(80_000 + round);
        let mut sink = VecEventSink::default();

        runtime.submit_copy(CopyRequest {
            task: task.clone(),
            direction: CopyDirection::HostToDevice,
            bytes: 4096,
            src: MemoryEndpoint {
                node: host_a.node_id,
                segment: stage_segment,
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: ubpu_a.node_id,
                segment: stage_segment,
                offset: 0,
            },
        })?;
        runtime.submit_dispatch(
            DispatchRequest {
                task: task.clone(),
                function: FunctionLabel {
                    name: "w3_cache_fill_transform".to_string(),
                    level: PlLevel::L2,
                },
                backend_spec: Some(empty_simpler_backend_spec(
                    DispatchBackendProfile::TmrbVector,
                    DispatchRuntimeVariant::TensormapAndRingbuffer,
                    "w3_cache_fill_transform",
                )),
                request: None,
                target_level: PlLevel::L2,
                target_node: ubpu_a.node_id,
                input_segments: vec![stage_segment],
            },
            &mut sink,
        )?;
        runtime.submit_copy(CopyRequest {
            task: task.clone(),
            direction: CopyDirection::DeviceToHost,
            bytes: 4096,
            src: MemoryEndpoint {
                node: ubpu_a.node_id,
                segment: device_result_segment,
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: host_a.node_id,
                segment: host_result_segment,
                offset: 0,
            },
        })?;
        let completions = runtime.poll_completions(runtime.now().saturating_add(256), &mut sink);
        report.completions += completions.len() as u64;
        report.events.extend(sink.into_events());
    }

    report.summary = summarize_events(&report.events);
    Ok(report)
}

fn submit_block_read(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: TaskKey,
    block: BlockHash,
    allow_retry_after_queue_full: bool,
) -> Result<(), SimError> {
    let segment = create_block_io_segment(surface)?;
    submit_block_io(
        surface,
        cq,
        report,
        IoSubmitReq {
            op_id: 10_000 + report.blocks_total,
            task: Some(task),
            entity: 0,
            opcode: IoOpcode::ReadBlock,
            segment: Some(segment),
            block: Some(block),
        },
        allow_retry_after_queue_full,
    )
}

fn enqueue_descriptor_and_ring(
    surface: &mut LocalGuestUapiSurface,
    cmdq: sim_core::CmdQueueHandle,
    desc: UapiDescriptor,
    enqueue_err: &'static str,
    doorbell_err: &'static str,
) -> Result<(), SimError> {
    match surface.execute(UapiCommand::EnqueueCmd {
        cmdq,
        owner: 0,
        desc,
    })? {
        UapiResponse::CommandEnqueued { .. } => {}
        _ => return Err(SimError::InvalidInput(enqueue_err)),
    }

    match surface.execute(UapiCommand::RingDoorbell {
        cmdq,
        owner: 0,
        max_batch: Some(1),
    })? {
        UapiResponse::DoorbellRung { submitted: 1, .. } => Ok(()),
        _ => Err(SimError::InvalidInput(doorbell_err)),
    }
}

fn submit_block_write(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: TaskKey,
    block: BlockHash,
    allow_retry_after_queue_full: bool,
) -> Result<(), SimError> {
    report.block_writes += 1;
    let segment = create_block_io_segment(surface)?;
    submit_block_io(
        surface,
        cq,
        report,
        IoSubmitReq {
            op_id: 20_000 + report.blocks_total,
            task: Some(task),
            entity: 0,
            opcode: IoOpcode::WriteBlock,
            segment: Some(segment),
            block: Some(block),
        },
        allow_retry_after_queue_full,
    )
}

fn create_block_io_segment(surface: &mut LocalGuestUapiSurface) -> Result<SegmentHandle, SimError> {
    match surface.execute(UapiCommand::CreateSegment { bytes: 4096 })? {
        UapiResponse::SegmentCreated(segment) => Ok(segment),
        _ => Err(SimError::InvalidInput(
            "unexpected block io segment creation response",
        )),
    }
}

fn submit_block_io(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    req: IoSubmitReq,
    allow_retry_after_queue_full: bool,
) -> Result<(), SimError> {
    match surface.execute(UapiCommand::SubmitIo { req: req.clone() }) {
        Ok(UapiResponse::IoSubmitted(_)) => Ok(()),
        Ok(_) => Err(SimError::InvalidInput("unexpected io submit response")),
        Err(SimError::InvalidInput("block queue full")) if allow_retry_after_queue_full => {
            report.block_queue_rejections += 1;
            drain_and_record(
                surface,
                cq,
                report,
                "unexpected cq drain response while clearing block queue pressure",
            )?;
            match surface.execute(UapiCommand::SubmitIo { req })? {
                UapiResponse::IoSubmitted(_) => Ok(()),
                _ => Err(SimError::InvalidInput(
                    "unexpected io submit retry response",
                )),
            }
        }
        Err(err) => Err(err),
    }
}

fn submit_block_writeback(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: Option<TaskKey>,
    block: BlockHash,
) -> Result<(), SimError> {
    report.block_writebacks += 1;
    match surface.execute(UapiCommand::SubmitBlockWriteback {
        block: block.clone(),
        task: task.clone(),
    }) {
        Ok(UapiResponse::IoSubmitted(_)) => Ok(()),
        Ok(_) => Err(SimError::InvalidInput(
            "unexpected block writeback response",
        )),
        Err(SimError::InvalidInput("block queue full")) => {
            report.block_queue_rejections += 1;
            drain_and_record(
                surface,
                cq,
                report,
                "unexpected cq drain response while clearing writeback queue pressure",
            )?;
            match surface.execute(UapiCommand::SubmitBlockWriteback { block, task })? {
                UapiResponse::IoSubmitted(_) => Ok(()),
                _ => Err(SimError::InvalidInput(
                    "unexpected block writeback retry response",
                )),
            }
        }
        Err(err) => Err(err),
    }
}

fn drain_and_record(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    response_err: &'static str,
) -> Result<(), SimError> {
    let completions = match surface.execute(UapiCommand::DrainCq { cq, owner: 0 })? {
        UapiResponse::Completions { events, .. } => events,
        _ => return Err(SimError::InvalidInput(response_err)),
    };

    report.completions += completions.len() as u64;
    for completion in completions {
        match completion.source {
            CompletionSource::BlockService => match &completion.status {
                CompletionStatus::RetryableFailure { code } => {
                    report.block_retryable_failures += 1;
                    if code == "block_miss" {
                        report.block_read_misses += 1;
                    }
                }
                CompletionStatus::FatalFailure { .. } | CompletionStatus::Success => {}
            },
            CompletionSource::ShmemService => match &completion.status {
                CompletionStatus::FatalFailure { code } if code == "shmem_access_denied" => {
                    report.shmem_denied += 1;
                }
                CompletionStatus::RetryableFailure { .. }
                | CompletionStatus::FatalFailure { .. }
                | CompletionStatus::Success => {}
            },
            CompletionSource::DbService => {
                if matches!(
                    &completion.status,
                    CompletionStatus::RetryableFailure { .. }
                ) {
                    report.db_retryable_failures += 1;
                }
            }
            CompletionSource::ChipBackend
            | CompletionSource::DfsService
            | CompletionSource::GuestUapi
            | CompletionSource::RemoteNode => {}
        }
        report.events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    Ok(())
}

fn block_for_request(
    workload_kind: &str,
    profile: Option<RustLlmProfile>,
    request_idx: u64,
    block_idx: u64,
    unique_prefixes: u64,
) -> BlockHash {
    let stable_prefixes = unique_prefixes.max(1).min(8);
    let prefix_group =
        request_idx % stable_prefixes.min(profile.map(|p| p.prefix_groups).unwrap_or(2).max(1));

    match workload_kind {
        "rust_llm_server_mvp" => {
            if is_prefix_block(profile, block_idx) {
                BlockHash(format!("prefix-{prefix_group}-block-{block_idx}"))
            } else {
                BlockHash(format!("tail-req-{request_idx}-block-{block_idx}"))
            }
        }
        "hotset_loop" => BlockHash(format!(
            "hotset-prefix-{}",
            (request_idx + block_idx) % stable_prefixes
        )),
        _ => BlockHash(format!("trace-block-{request_idx}-{block_idx}")),
    }
}

fn mailbox_task(round: u64, host_idx: u32) -> TaskKey {
    let mut levels = [0; 8];
    levels[3] = host_idx;
    TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels },
        scope_depth: 0,
        task_id: round * 10 + u64::from(host_idx) + 1,
    }
}

fn run_rust_llm_backend_step(
    runtime: &mut LocalRuntimeEngine,
    topology: &SimTopology,
    report: &mut WorkloadRunReport,
    spec: RustLlmBackendStepSpec<'_>,
) -> Result<SegmentHandle, SimError> {
    let RustLlmBackendStepSpec {
        task,
        request_id,
        trace_id,
        profile_name: _profile_name,
        function_name,
        input_segment,
        additional_input_segments,
        callable_hint,
        block_hash,
        request_index,
        block_index,
        request_blocks_total,
        blocks_remaining_in_request,
        is_first_block_in_request,
        is_last_block_in_request,
        request_control_phase,
        request_control_epoch,
        request_control_result_kind,
        request_control_result_value,
        request_control_view_kind,
        kvcache_resolution_kind,
        kvcache_view_kind,
        kvcache_transition_kind,
        logical_system_id,
        scope_depth,
        prefix_group,
        route_from_level,
        route_to_level,
        route_selected_node,
        route_reason,
        placement_level,
        placement_node,
        capacity_pressure_active,
        evictions_seen,
        block_writebacks_seen,
        promoted_this_access,
        reloaded_after_eviction,
        uses_dfs_fallback,
        request_segment,
        control_segment,
        prefix_segment,
        context_lifecycle,
    } = spec;
    let host_node = topology.hosts[0].node_id;
    let ubpu_node = topology
        .ubpus
        .iter()
        .find(|ubpu| ubpu.host_id == 0)
        .map(|ubpu| ubpu.node_id)
        .unwrap_or(host_node);
    let segment_seed = 90_000 + report.blocks_total * 10 + task.task_id;
    let device_result_segment = SegmentHandle(segment_seed + 1);
    let host_result_segment = SegmentHandle(segment_seed + 2);
    let host_vector_input_b_segment = SegmentHandle(segment_seed + 3);
    let mut sink = VecEventSink::default();
    let includes_request_control = function_name == "w4_rust_llm_minimal_step";
    let includes_prefix_shared = prefix_segment.is_some();
    let w4_vector_size_bytes = 16_384u64 * std::mem::size_of::<f32>() as u64;
    let w4_vector_elems = 16_384u64;
    let w4_vector_runtime_manifest = if function_name == "w4_rust_llm_minimal_step" {
        match callable_hint {
            Some(hint) if hint.contains("_tail") => tmrb_vector_manifest_path(),
            _ => host_vector_manifest_path(),
        }
    } else {
        None
    };
    let uses_w4_vector_runtime = w4_vector_runtime_manifest.is_some();

    if uses_w4_vector_runtime {
        runtime.seed_host_segment(
            host_node,
            input_segment,
            f32s_to_bytes(&vec![2.0; w4_vector_elems as usize]),
        );
        runtime.seed_host_segment(
            host_node,
            host_vector_input_b_segment,
            f32s_to_bytes(&vec![3.0; w4_vector_elems as usize]),
        );
        runtime.seed_host_segment(
            host_node,
            host_result_segment,
            vec![0u8; w4_vector_size_bytes as usize],
        );
    }

    if !uses_w4_vector_runtime {
        runtime.submit_copy(CopyRequest {
            task: task.clone(),
            direction: CopyDirection::HostToDevice,
            bytes: 4096,
            src: MemoryEndpoint {
                node: host_node,
                segment: input_segment,
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: ubpu_node,
                segment: input_segment,
                offset: 0,
            },
        })?;
    }
    let request_bindings = if uses_w4_vector_runtime {
        let mut bindings = Vec::new();
        if let Some(segment) = request_segment {
            bindings.push(opaque_resident_binding(
                "request_state",
                BufferUsage::Inout,
                MemoryEndpoint {
                    node: host_node,
                    segment,
                    offset: 0,
                },
                512,
            ));
        }
        if let Some(segment) = control_segment {
            bindings.push(opaque_resident_binding(
                "control_state",
                BufferUsage::Inout,
                MemoryEndpoint {
                    node: host_node,
                    segment,
                    offset: 0,
                },
                512,
            ));
        }
        if let Some(segment) = prefix_segment {
            bindings.push(opaque_resident_binding(
                "prefix_state",
                BufferUsage::Inout,
                MemoryEndpoint {
                    node: host_node,
                    segment,
                    offset: 0,
                },
                4096,
            ));
        }
        if function_name == "w4_rust_llm_minimal_step" && request_control_phase == Some("Active") {
            bindings.push(opaque_resident_binding(
                format!("kv_hot_state_{}", input_segment.0),
                BufferUsage::Inout,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_segment,
                    offset: 0,
                },
                w4_vector_size_bytes,
            ));
        }
        bindings.extend([
            dense_f32_binding(
                "input_a",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_segment,
                    offset: 0,
                },
                w4_vector_elems,
            ),
            dense_f32_binding(
                "input_b",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: host_vector_input_b_segment,
                    offset: 0,
                },
                w4_vector_elems,
            ),
            dense_f32_binding(
                "output_f",
                if matches!(
                    callable_hint,
                    Some(hint) if hint.contains("_tail")
                ) {
                    BufferUsage::Inout
                } else {
                    BufferUsage::Output
                },
                MemoryEndpoint {
                    node: host_node,
                    segment: host_result_segment,
                    offset: 0,
                },
                w4_vector_elems,
            ),
        ]);
        bindings
    } else {
        let mut bindings = vec![DispatchBufferBinding {
            name: "stage_input".to_string(),
            usage: BufferUsage::Input,
            endpoint: MemoryEndpoint {
                node: host_node,
                segment: input_segment,
                offset: 0,
            },
            bytes: 4096,
            dtype: TensorDType::Opaque,
            shape: vec![4096],
            layout: TensorLayout::Opaque,
            strides: None,
            resident: false,
        }];
        bindings.extend(
            additional_input_segments
                .iter()
                .enumerate()
                .map(|(idx, segment)| DispatchBufferBinding {
                    name: format!("additional_input_{idx}"),
                    usage: BufferUsage::Input,
                    endpoint: MemoryEndpoint {
                        node: host_node,
                        segment: *segment,
                        offset: 0,
                    },
                    bytes: 4096,
                    dtype: TensorDType::Opaque,
                    shape: vec![4096],
                    layout: TensorLayout::Opaque,
                    strides: None,
                    resident: false,
                }),
        );
        bindings
    };
    let request = simple_execution_request(
        request_id,
        Some(trace_id.to_string()),
        function_name,
        format!("device-ctx-node-{ubpu_node}"),
        format!("runtime-ctx-task-{}", task.task_id),
        context_lifecycle,
        scope_depth.unwrap_or(0),
        block_index.unwrap_or(task.task_id),
        w4_plan_ref(
            task,
            function_name,
            callable_hint,
            request_control_phase,
            kvcache_resolution_kind,
            block_index,
        ),
        request_bindings,
    );
    let dispatch_handle = runtime.submit_backend_dispatch(
        BackendDispatchOperation {
            task: task.clone(),
            function: FunctionLabel {
                name: function_name.to_string(),
                level: PlLevel::L2,
            },
            backend_spec: if function_name == "w4_rust_llm_minimal_step" {
                let (profile, runtime_variant) = match callable_hint {
                    Some(hint) if hint.contains("_tail") => (
                        DispatchBackendProfile::TmrbVector,
                        DispatchRuntimeVariant::TensormapAndRingbuffer,
                    ),
                    _ => (
                        DispatchBackendProfile::HostVector,
                        DispatchRuntimeVariant::HostBuildGraph,
                    ),
                };
                let mut backend_spec = simpler_backend_spec(
                    profile,
                    runtime_variant,
                    callable_hint.or(Some(function_name)),
                    block_hash.as_ref(),
                    request_index,
                    block_index,
                    request_blocks_total,
                    blocks_remaining_in_request,
                    is_first_block_in_request,
                    is_last_block_in_request,
                    request_control_phase,
                    request_control_epoch,
                    request_control_result_kind,
                    request_control_result_value,
                    request_control_view_kind,
                    kvcache_resolution_kind,
                    kvcache_view_kind,
                    kvcache_transition_kind,
                    logical_system_id,
                    scope_depth,
                    prefix_group,
                    route_from_level,
                    route_to_level,
                    route_selected_node,
                    route_reason,
                    placement_level,
                    placement_node,
                    capacity_pressure_active,
                    evictions_seen,
                    block_writebacks_seen,
                    promoted_this_access,
                    reloaded_after_eviction,
                    uses_dfs_fallback,
                    includes_request_control,
                    includes_prefix_shared,
                    Some(input_segment),
                    request_segment,
                    control_segment,
                    prefix_segment,
                );
                if let Some(manifest_path) = w4_vector_runtime_manifest.as_ref() {
                    let input_a = MemoryEndpoint {
                        node: host_node,
                        segment: input_segment,
                        offset: 0,
                    };
                    let input_b = MemoryEndpoint {
                        node: host_node,
                        segment: host_vector_input_b_segment,
                        offset: 0,
                    };
                    let output_f = MemoryEndpoint {
                        node: host_node,
                        segment: host_result_segment,
                        offset: 0,
                    };
                    backend_spec.simpler_runtime = Some(match runtime_variant {
                        DispatchRuntimeVariant::HostBuildGraph => w4_host_vector_runtime_artifacts(
                            manifest_path,
                            input_a,
                            input_b,
                            output_f,
                            w4_vector_size_bytes,
                            w4_vector_elems,
                        )?,
                        DispatchRuntimeVariant::TensormapAndRingbuffer => {
                            w4_tmrb_vector_runtime_artifacts(
                                manifest_path,
                                input_a,
                                input_b,
                                output_f,
                                w4_vector_size_bytes,
                                w4_vector_elems,
                            )?
                        }
                    });
                }
                backend_spec
            } else {
                empty_simpler_backend_spec(
                    DispatchBackendProfile::HostVector,
                    DispatchRuntimeVariant::HostBuildGraph,
                    function_name,
                )
            },
            request,
            target_level: PlLevel::L2,
            target_node: ubpu_node,
            legacy_input_segments: {
                let mut segments = vec![input_segment];
                segments.extend(additional_input_segments);
                segments
            },
        },
        &mut sink,
    )?;
    if !uses_w4_vector_runtime {
        runtime.submit_copy(CopyRequest {
            task: task.clone(),
            direction: CopyDirection::DeviceToHost,
            bytes: 4096,
            src: MemoryEndpoint {
                node: ubpu_node,
                segment: device_result_segment,
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: host_node,
                segment: host_result_segment,
                offset: 0,
            },
        })?;
    }
    let completions = runtime.poll_completions(runtime.now().saturating_add(256), &mut sink);
    report.completions += completions.len() as u64;
    report.events.extend(sink.into_events());
    let handled_success = completions.iter().any(|completion| {
        completion.op_id == dispatch_handle.0
            && completion.source == CompletionSource::ChipBackend
            && matches!(completion.status, CompletionStatus::Success)
    });
    if handled_success && function_name.starts_with("w4_") {
        report.events.push(SimEvent::W4ResultHandled {
            at: runtime.now(),
            task: task.clone(),
            function_name: function_name.to_string(),
            block_hash,
            request_index,
            block_index,
            result_segment: host_result_segment,
            payload_validated: true,
            request_control_phase: request_control_phase.map(str::to_string),
            request_control_result_kind: request_control_result_kind.map(str::to_string),
            request_control_view_kind: request_control_view_kind.map(str::to_string),
            kvcache_resolution_kind: kvcache_resolution_kind.map(str::to_string),
            kvcache_view_kind: kvcache_view_kind.map(str::to_string),
            kvcache_transition_kind: kvcache_transition_kind.map(str::to_string),
        });
    }
    Ok(host_result_segment)
}

fn base_workload_report(
    workload_kind: String,
    workload_profile: String,
    requests_total: u64,
) -> WorkloadRunReport {
    WorkloadRunReport {
        workload_kind,
        workload_profile,
        requests_total,
        blocks_total: 0,
        hits: 0,
        misses: 0,
        prefix_hits: 0,
        tail_misses: 0,
        fallback_reads: 0,
        shmem_puts: 0,
        shmem_gets: 0,
        shmem_denied: 0,
        dfs_cold_reads: 0,
        dfs_warm_reads: 0,
        block_read_misses: 0,
        block_writes: 0,
        block_writebacks: 0,
        block_retryable_failures: 0,
        block_queue_rejections: 0,
        dfs_seed_writes: 0,
        db_puts: 0,
        db_gets: 0,
        db_retryable_failures: 0,
        promotions: 0,
        evictions: 0,
        completions: 0,
        summary: EventSummary {
            total_events: 0,
            tasks_created: 0,
            routes_planned: 0,
            blocks_promoted: 0,
            blocks_evicted: 0,
            dispatch_submitted: 0,
            completions_total: 0,
            runtime_retried: 0,
            runtime_failed: 0,
            faults_injected: 0,
            completions_by_source: CompletionSourceStats {
                chip_backend: 0,
                block_service: 0,
                shmem_service: 0,
                dfs_service: 0,
                db_service: 0,
                guest_uapi: 0,
                remote_node: 0,
            },
            completions_by_status: CompletionStatusStats {
                success: 0,
                retryable_failure: 0,
                fatal_failure: 0,
            },
            w4_results_handled: Default::default(),
            w4_service_results: Default::default(),
        },
        events: Vec::new(),
    }
}

fn rust_llm_profile(profile: &str) -> RustLlmProfile {
    match profile {
        "dual_node_minimal" => RustLlmProfile {
            name: "dual_node_minimal",
            requests_total_cap: 2,
            prefix_groups: 1,
            prefix_blocks: 1,
            tail_blocks: 1,
            tail_uses_dfs: false,
            evict_after_request: 0,
        },
        "high_reuse" => RustLlmProfile {
            name: "high_reuse",
            requests_total_cap: 6,
            prefix_groups: 1,
            prefix_blocks: 3,
            tail_blocks: 1,
            tail_uses_dfs: false,
            evict_after_request: 0,
        },
        "capacity_pressure" => RustLlmProfile {
            name: "capacity_pressure",
            requests_total_cap: 6,
            prefix_groups: 2,
            prefix_blocks: 1,
            tail_blocks: 3,
            tail_uses_dfs: true,
            evict_after_request: 2,
        },
        "dfs_heavy_fallback" => RustLlmProfile {
            name: "dfs_heavy_fallback",
            requests_total_cap: 5,
            prefix_groups: 2,
            prefix_blocks: 1,
            tail_blocks: 4,
            tail_uses_dfs: true,
            evict_after_request: 1,
        },
        _ => RustLlmProfile {
            name: "single_domain_basic",
            requests_total_cap: 4,
            prefix_groups: 2,
            prefix_blocks: 2,
            tail_blocks: 2,
            tail_uses_dfs: true,
            evict_after_request: 0,
        },
    }
}

fn is_prefix_block(profile: Option<RustLlmProfile>, block_idx: u64) -> bool {
    profile
        .map(|profile| block_idx < profile.prefix_blocks)
        .unwrap_or(false)
}

fn uses_dfs_fallback(profile: Option<RustLlmProfile>, block_idx: u64) -> bool {
    profile
        .map(|profile| profile.tail_uses_dfs && block_idx >= profile.prefix_blocks)
        .unwrap_or(false)
}

fn summarize_events(events: &[SimEvent]) -> EventSummary {
    let mut summary = EventSummary {
        total_events: events.len() as u64,
        tasks_created: 0,
        routes_planned: 0,
        blocks_promoted: 0,
        blocks_evicted: 0,
        dispatch_submitted: 0,
        completions_total: 0,
        runtime_retried: 0,
        runtime_failed: 0,
        faults_injected: 0,
        completions_by_source: CompletionSourceStats {
            chip_backend: 0,
            block_service: 0,
            shmem_service: 0,
            dfs_service: 0,
            db_service: 0,
            guest_uapi: 0,
            remote_node: 0,
        },
        completions_by_status: CompletionStatusStats {
            success: 0,
            retryable_failure: 0,
            fatal_failure: 0,
        },
        w4_results_handled: Default::default(),
        w4_service_results: Default::default(),
    };

    for event in events {
        match event {
            SimEvent::TaskCreated { .. } => summary.tasks_created += 1,
            SimEvent::RoutePlanned { .. } => summary.routes_planned += 1,
            SimEvent::BlockPromoted { .. } => summary.blocks_promoted += 1,
            SimEvent::BlockEvicted { .. } => summary.blocks_evicted += 1,
            SimEvent::DispatchSubmitted { .. } => summary.dispatch_submitted += 1,
            SimEvent::CompletionObserved { completion, .. } => {
                summary.completions_total += 1;
                match completion.source {
                    CompletionSource::ChipBackend => {
                        summary.completions_by_source.chip_backend += 1
                    }
                    CompletionSource::BlockService => {
                        summary.completions_by_source.block_service += 1
                    }
                    CompletionSource::ShmemService => {
                        summary.completions_by_source.shmem_service += 1
                    }
                    CompletionSource::DfsService => summary.completions_by_source.dfs_service += 1,
                    CompletionSource::DbService => summary.completions_by_source.db_service += 1,
                    CompletionSource::GuestUapi => summary.completions_by_source.guest_uapi += 1,
                    CompletionSource::RemoteNode => summary.completions_by_source.remote_node += 1,
                }
                match &completion.status {
                    CompletionStatus::Success => summary.completions_by_status.success += 1,
                    CompletionStatus::RetryableFailure { .. } => {
                        summary.completions_by_status.retryable_failure += 1
                    }
                    CompletionStatus::FatalFailure { .. } => {
                        summary.completions_by_status.fatal_failure += 1
                    }
                }
            }
            SimEvent::RuntimeRetried { .. } => summary.runtime_retried += 1,
            SimEvent::RuntimeFailed { .. } => summary.runtime_failed += 1,
            SimEvent::W4ResultHandled {
                payload_validated,
                request_control_phase,
                kvcache_resolution_kind,
                kvcache_transition_kind,
                ..
            } => {
                summary.w4_results_handled.total += 1;
                if *payload_validated {
                    summary.w4_results_handled.payload_validated += 1;
                }
                match request_control_phase.as_deref() {
                    Some("Begin") => summary.w4_results_handled.begin += 1,
                    Some("Active") => summary.w4_results_handled.active += 1,
                    Some("Finish") => summary.w4_results_handled.finish += 1,
                    _ => {}
                }
                match kvcache_resolution_kind.as_deref() {
                    Some("RequestControlOnly") => {
                        summary.w4_results_handled.request_control_only += 1
                    }
                    Some("HotHit") => summary.w4_results_handled.hot_hit += 1,
                    Some("FilledFromBlock") => summary.w4_results_handled.filled_from_block += 1,
                    _ => {}
                }
                match kvcache_transition_kind.as_deref() {
                    Some("StableHot") => summary.w4_results_handled.stable_hot += 1,
                    Some("PromotedHot") => summary.w4_results_handled.promoted_hot += 1,
                    Some("ReloadedHot") => summary.w4_results_handled.reloaded_hot += 1,
                    Some("ControlOnly") => summary.w4_results_handled.control_only += 1,
                    _ => {}
                }
            }
            SimEvent::W4ServiceResultApplied {
                service_kind,
                action_kind,
                ..
            } => {
                summary.w4_service_results.total += 1;
                match service_kind.as_str() {
                    "RequestControl" => summary.w4_service_results.request_control += 1,
                    "KvCache" => summary.w4_service_results.kvcache += 1,
                    _ => {}
                }
                match action_kind.as_str() {
                    "RequestRepublished" => summary.w4_service_results.request_republished += 1,
                    "FinishControlRefreshed" => {
                        summary.w4_service_results.finish_control_refresh += 1
                    }
                    "KvResultRepublished" => summary.w4_service_results.kv_republished += 1,
                    "HotHitRefreshed" => summary.w4_service_results.hot_hit_refresh += 1,
                    "ReloadedHotRefreshed" => summary.w4_service_results.reload_refresh += 1,
                    _ => {}
                }
            }
            SimEvent::FaultInjected { .. } => summary.faults_injected += 1,
        }
    }

    summary
}

#[cfg(test)]
mod tests {
    use super::{
        block_for_request, host_vector_backend_spec_from_manifest, host_vector_manifest_path,
        run_minimal_workload, run_rust_llm_mvp_smoke, rust_llm_profile, tmrb_vector_manifest_path,
        w4_step_kind, w4_tmrb_vector_runtime_artifacts, RustLlmMvpSmokeConfig,
    };
    use sim_config::ScenarioConfig;
    use sim_core::{
        CompletionSource, CompletionStatus, DispatchBackendProfile, DispatchBackendSpec,
        DispatchRequest, DispatchRuntimeVariant, ExecutionStepKind, FunctionLabel, HierarchyCoord,
        LogicalSystemId, MemoryEndpoint, PlLevel, SegmentHandle, TaskKey,
    };
    use sim_runtime::{LocalRuntimeEngine, VecEventSink};
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
  qps: 3
  unique_prefixes: 2
  blocks_per_request: 4
  function_label_mode: host_orchestration
faults: []
outputs:
  trace: true
  metrics_csv: true
  summary_json: true
  emit_task_coord_trace: true
  emit_data_service_trace: true
  emit_qemu_platform_trace: true
"#;

    #[test]
    fn minimal_workload_runs_and_emits_events() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let report = run_minimal_workload(&config, &topology).expect("workload");

        assert_eq!(report.requests_total, 3);
        assert_eq!(report.workload_profile, "single_domain_basic");
        assert_eq!(report.blocks_total, 12);
        assert!(report.promotions > 0);
        assert!(report.completions > 0);
        assert!(report.tail_misses > 0);
        assert!(report.fallback_reads > 0);
        assert!(report.summary.completions_by_source.shmem_service > 0);
        assert!(!report.events.is_empty());
        assert!(report.events.iter().any(|event| matches!(
            event,
            sim_core::SimEvent::CompletionObserved {
                completion: sim_core::CompletionEvent {
                    source: CompletionSource::DfsService,
                    ..
                },
                ..
            }
        )));
    }

    #[test]
    fn rust_llm_mvp_smoke_api_runs_eight_host_workload() {
        let report =
            run_rust_llm_mvp_smoke(&RustLlmMvpSmokeConfig::default()).expect("rust llm mvp smoke");

        assert_eq!(report.workload_kind, "rust_llm_server_mvp");
        assert_eq!(report.workload_profile, "single_domain_basic");
        assert_eq!(report.requests_total, 1);
        assert_eq!(report.blocks_total, 4);
        assert!(report.completions > 0);
        assert!(
            !report.events.iter().any(|event| matches!(
                event,
                sim_core::SimEvent::CompletionObserved {
                    completion: sim_core::CompletionEvent {
                        status: CompletionStatus::FatalFailure { .. },
                        ..
                    },
                    ..
                }
            )),
            "rust llm mvp smoke must not emit fatal completions"
        );
    }

    #[test]
    fn rust_llm_workload_reuses_prefix_blocks() {
        use sim_core::BlockHash;
        let profile = Some(rust_llm_profile("single_domain_basic"));

        assert_eq!(
            block_for_request("rust_llm_server_mvp", profile, 0, 0, 16),
            BlockHash("prefix-0-block-0".into())
        );
        assert_eq!(
            block_for_request("rust_llm_server_mvp", profile, 0, 1, 16),
            BlockHash("prefix-0-block-1".into())
        );
        assert_eq!(
            block_for_request("rust_llm_server_mvp", profile, 0, 3, 16),
            BlockHash("tail-req-0-block-3".into())
        );
    }

    #[test]
    fn w4_step_kind_classifies_request_control_and_finalize() {
        assert_eq!(
            w4_step_kind(
                "w4_rust_llm_minimal_step",
                Some("Begin"),
                Some("RequestControlOnly")
            ),
            ExecutionStepKind::RequestControl
        );
        assert_eq!(
            w4_step_kind("w4_rust_llm_minimal_step", Some("Finish"), None),
            ExecutionStepKind::Finalize
        );
    }

    #[test]
    fn w4_step_kind_classifies_cache_paths() {
        assert_eq!(
            w4_step_kind("w4_rust_llm_minimal_step", Some("Active"), Some("HotHit")),
            ExecutionStepKind::CacheResolve
        );
        assert_eq!(
            w4_step_kind(
                "w4_rust_llm_minimal_step",
                Some("Active"),
                Some("FilledFromBlock")
            ),
            ExecutionStepKind::CacheFill
        );
    }

    #[test]
    fn high_reuse_profile_increases_prefix_hits() {
        let config = ScenarioConfig::from_yaml_str(
            &VALID_YAML.replace("profile: single_domain_basic", "profile: high_reuse"),
        )
        .expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let report = run_minimal_workload(&config, &topology).expect("workload");

        assert_eq!(report.workload_profile, "high_reuse");
        assert!(report.prefix_hits > 0);
        assert_eq!(report.fallback_reads, 0);
    }

    #[test]
    fn dfs_heavy_profile_increases_fallback_reads() {
        let config = ScenarioConfig::from_yaml_str(&VALID_YAML.replace(
            "profile: single_domain_basic",
            "profile: dfs_heavy_fallback",
        ))
        .expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let report = run_minimal_workload(&config, &topology).expect("workload");

        assert_eq!(report.workload_profile, "dfs_heavy_fallback");
        assert!(report.fallback_reads >= report.requests_total);
        assert!(report.summary.completions_by_source.dfs_service > 0);
    }

    #[test]
    fn capacity_pressure_profile_forces_extra_evictions() {
        let config = ScenarioConfig::from_yaml_str(
            &VALID_YAML.replace("profile: single_domain_basic", "profile: capacity_pressure"),
        )
        .expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let report = run_minimal_workload(&config, &topology).expect("workload");

        assert_eq!(report.workload_profile, "capacity_pressure");
        assert!(report.evictions > 0);
        assert!(report.block_writebacks > 0);
    }

    fn simpler_capi_yaml() -> String {
        VALID_YAML.replace("chip_backend_mode: stub", "chip_backend_mode: simpler_capi")
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
            .map(|chunk| {
                let arr: [u8; 4] = chunk.try_into().expect("f32 chunk");
                f32::from_le_bytes(arr)
            })
            .collect()
    }

    #[test]
    #[ignore = "requires prebuilt simpler host_vector manifest"]
    fn simpler_capi_host_vector_dispatch_smoke() {
        let manifest_path = host_vector_manifest_path()
            .expect("set SIMPLER_HOST_VECTOR_MANIFEST or build default manifest");
        let config = ScenarioConfig::from_yaml_str(&simpler_capi_yaml()).expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let host_node = topology.hosts[0].node_id;
        let ubpu_node = topology.ubpus[0].node_id;
        let input_a = SegmentHandle(101);
        let input_b = SegmentHandle(102);
        let output_f = SegmentHandle(103);
        let elems = 16_384u64;
        let size_bytes = elems * std::mem::size_of::<f32>() as u64;

        let mut runtime = LocalRuntimeEngine::from_config(&config);
        runtime.seed_host_segment(
            host_node,
            input_a,
            f32s_to_bytes(&vec![2.0; elems as usize]),
        );
        runtime.seed_host_segment(
            host_node,
            input_b,
            f32s_to_bytes(&vec![3.0; elems as usize]),
        );
        runtime.seed_host_segment(host_node, output_f, vec![0u8; size_bytes as usize]);

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
                segment: output_f,
                offset: 0,
            },
            size_bytes,
            elems,
        )
        .expect("backend spec");

        let task = TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        };
        let mut sink = VecEventSink::default();
        runtime
            .submit_dispatch(
                DispatchRequest {
                    task,
                    function: FunctionLabel {
                        name: "host_vector_example".into(),
                        level: PlLevel::L2,
                    },
                    backend_spec: Some(backend_spec),
                    request: None,
                    target_level: PlLevel::L2,
                    target_node: ubpu_node,
                    input_segments: vec![input_a, input_b],
                },
                &mut sink,
            )
            .expect("dispatch submit");

        runtime.advance_to(15, &mut sink);
        let completions = runtime.poll_completions(15, &mut sink);
        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].status, CompletionStatus::Success);

        let output = runtime
            .host_segment_payload(host_node, output_f)
            .expect("output payload");
        let values = bytes_to_f32s(output);
        assert_eq!(values.len(), elems as usize);
        assert!(values.iter().all(|value| (*value - 42.0).abs() < 1e-5));
    }

    #[test]
    #[ignore = "requires prebuilt simpler host_vector and tmrb manifests"]
    fn simpler_capi_w4_minimal_dual_node_smoke() {
        assert!(
            host_vector_manifest_path().is_some(),
            "set SIMPLER_HOST_VECTOR_MANIFEST or build default manifest"
        );
        assert!(
            tmrb_vector_manifest_path().is_some(),
            "set SIMPLER_TMRB_VECTOR_MANIFEST or build default manifest"
        );

        let yaml = simpler_capi_yaml()
            .replace("profile: single_domain_basic", "profile: dual_node_minimal")
            .replace("qps: 3", "qps: 1")
            .replace("blocks_per_request: 4", "blocks_per_request: 2");
        let config = ScenarioConfig::from_yaml_str(&yaml).expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let report = run_minimal_workload(&config, &topology).expect("workload");

        assert_eq!(report.workload_profile, "dual_node_minimal");
        assert_eq!(report.requests_total, 1);
        assert_eq!(report.blocks_total, 2);
        assert!(report.completions > 0);
        assert!(
            !report.events.iter().any(|event| matches!(
                event,
                sim_core::SimEvent::CompletionObserved {
                    completion: sim_core::CompletionEvent {
                        status: CompletionStatus::FatalFailure { .. },
                        ..
                    },
                    ..
                }
            )),
            "unexpected fatal completion in W4 minimal simpler_capi path"
        );
    }

    #[test]
    #[ignore = "requires prebuilt simpler tmrb manifest"]
    fn simpler_capi_tmrb_vector_dispatch_smoke() {
        let manifest_path = tmrb_vector_manifest_path()
            .expect("set SIMPLER_TMRB_VECTOR_MANIFEST or build default manifest");
        let config = ScenarioConfig::from_yaml_str(&simpler_capi_yaml()).expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let host_node = topology.hosts[0].node_id;
        let ubpu_node = topology.ubpus[0].node_id;
        let input_a = SegmentHandle(201);
        let input_b = SegmentHandle(202);
        let output_f = SegmentHandle(203);
        let elems = 16_384u64;
        let size_bytes = elems * std::mem::size_of::<f32>() as u64;

        let mut runtime = LocalRuntimeEngine::from_config(&config);
        runtime.seed_host_segment(
            host_node,
            input_a,
            f32s_to_bytes(&vec![2.0; elems as usize]),
        );
        runtime.seed_host_segment(
            host_node,
            input_b,
            f32s_to_bytes(&vec![3.0; elems as usize]),
        );
        runtime.seed_host_segment(host_node, output_f, vec![0u8; size_bytes as usize]);

        let backend_spec = DispatchBackendSpec {
            profile: DispatchBackendProfile::TmrbVector,
            platform: "a2a3sim".to_string(),
            runtime_variant: DispatchRuntimeVariant::TensormapAndRingbuffer,
            callable_hint: Some("tmrb_vector_example".to_string()),
            simpler_runtime: Some(
                w4_tmrb_vector_runtime_artifacts(
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
                        segment: output_f,
                        offset: 0,
                    },
                    size_bytes,
                    elems,
                )
                .expect("tmrb backend spec"),
            ),
            context: None,
        };

        let task = TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 2,
        };
        let mut sink = VecEventSink::default();
        runtime
            .submit_dispatch(
                DispatchRequest {
                    task,
                    function: FunctionLabel {
                        name: "tmrb_vector_example".into(),
                        level: PlLevel::L2,
                    },
                    backend_spec: Some(backend_spec),
                    request: None,
                    target_level: PlLevel::L2,
                    target_node: ubpu_node,
                    input_segments: vec![input_a, input_b],
                },
                &mut sink,
            )
            .expect("dispatch submit");

        runtime.advance_to(15, &mut sink);
        let completions = runtime.poll_completions(15, &mut sink);
        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].status, CompletionStatus::Success);

        let output = runtime
            .host_segment_payload(host_node, output_f)
            .expect("output payload");
        let values = bytes_to_f32s(output);
        assert_eq!(values.len(), elems as usize);
        assert!(values.iter().all(|value| (*value - 47.0).abs() < 1e-5));
    }

    #[test]
    #[ignore = "requires prebuilt simpler manifests"]
    fn simpler_capi_host_then_tmrb_sequence_smoke() {
        let host_manifest = host_vector_manifest_path()
            .expect("set SIMPLER_HOST_VECTOR_MANIFEST or build default manifest");
        let tmrb_manifest = tmrb_vector_manifest_path()
            .expect("set SIMPLER_TMRB_VECTOR_MANIFEST or build default manifest");
        let config = ScenarioConfig::from_yaml_str(&simpler_capi_yaml()).expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let host_node = topology.hosts[0].node_id;
        let ubpu_node = topology.ubpus[0].node_id;
        let elems = 16_384u64;
        let size_bytes = elems * std::mem::size_of::<f32>() as u64;

        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        for seq in 0..3u64 {
            let input_a = SegmentHandle(300 + seq * 10);
            let input_b = SegmentHandle(301 + seq * 10);
            let output_f = SegmentHandle(302 + seq * 10);
            runtime.seed_host_segment(
                host_node,
                input_a,
                f32s_to_bytes(&vec![2.0; elems as usize]),
            );
            runtime.seed_host_segment(
                host_node,
                input_b,
                f32s_to_bytes(&vec![3.0; elems as usize]),
            );
            runtime.seed_host_segment(host_node, output_f, vec![0u8; size_bytes as usize]);

            let backend_spec = host_vector_backend_spec_from_manifest(
                &host_manifest,
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
                    segment: output_f,
                    offset: 0,
                },
                size_bytes,
                elems,
            )
            .expect("host backend spec");
            runtime
                .submit_dispatch(
                    DispatchRequest {
                        task: TaskKey {
                            logical_system: LogicalSystemId(1),
                            coord: HierarchyCoord { levels: [0; 8] },
                            scope_depth: 0,
                            task_id: 10 + seq,
                        },
                        function: FunctionLabel {
                            name: "host_vector_example".into(),
                            level: PlLevel::L2,
                        },
                        backend_spec: Some(backend_spec),
                        request: None,
                        target_level: PlLevel::L2,
                        target_node: ubpu_node,
                        input_segments: vec![input_a, input_b],
                    },
                    &mut sink,
                )
                .expect("host dispatch submit");
            let now = 100 + seq * 100;
            runtime.advance_to(now, &mut sink);
            let completions = runtime.poll_completions(now, &mut sink);
            assert_eq!(completions.len(), 1);
            assert_eq!(completions[0].status, CompletionStatus::Success);
        }

        let input_a = SegmentHandle(400);
        let input_b = SegmentHandle(401);
        let output_f = SegmentHandle(402);
        runtime.seed_host_segment(
            host_node,
            input_a,
            f32s_to_bytes(&vec![2.0; elems as usize]),
        );
        runtime.seed_host_segment(
            host_node,
            input_b,
            f32s_to_bytes(&vec![3.0; elems as usize]),
        );
        runtime.seed_host_segment(host_node, output_f, vec![0u8; size_bytes as usize]);
        let backend_spec = DispatchBackendSpec {
            profile: DispatchBackendProfile::TmrbVector,
            platform: "a2a3sim".to_string(),
            runtime_variant: DispatchRuntimeVariant::TensormapAndRingbuffer,
            callable_hint: Some("tmrb_vector_example".to_string()),
            simpler_runtime: Some(
                w4_tmrb_vector_runtime_artifacts(
                    &tmrb_manifest,
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
                        segment: output_f,
                        offset: 0,
                    },
                    size_bytes,
                    elems,
                )
                .expect("tmrb backend spec"),
            ),
            context: None,
        };
        runtime
            .submit_dispatch(
                DispatchRequest {
                    task: TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 99,
                    },
                    function: FunctionLabel {
                        name: "tmrb_vector_example".into(),
                        level: PlLevel::L2,
                    },
                    backend_spec: Some(backend_spec),
                    request: None,
                    target_level: PlLevel::L2,
                    target_node: ubpu_node,
                    input_segments: vec![input_a, input_b],
                },
                &mut sink,
            )
            .expect("tmrb dispatch submit");
        runtime.advance_to(1_000, &mut sink);
        let completions = runtime.poll_completions(1_000, &mut sink);
        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].status, CompletionStatus::Success);
    }

    #[test]
    #[ignore = "requires prebuilt simpler manifests"]
    fn simpler_capi_host_tmrb_host_sequence_smoke() {
        let host_manifest = host_vector_manifest_path()
            .expect("set SIMPLER_HOST_VECTOR_MANIFEST or build default manifest");
        let tmrb_manifest = tmrb_vector_manifest_path()
            .expect("set SIMPLER_TMRB_VECTOR_MANIFEST or build default manifest");
        let config = ScenarioConfig::from_yaml_str(&simpler_capi_yaml()).expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let host_node = topology.hosts[0].node_id;
        let ubpu_node = topology.ubpus[0].node_id;
        let elems = 16_384u64;
        let size_bytes = elems * std::mem::size_of::<f32>() as u64;

        let mut runtime = LocalRuntimeEngine::from_config(&config);
        let mut sink = VecEventSink::default();

        let run_host = |runtime: &mut LocalRuntimeEngine,
                        sink: &mut VecEventSink,
                        seq: u64,
                        input_a: SegmentHandle,
                        input_b: SegmentHandle,
                        output_f: SegmentHandle| {
            runtime.seed_host_segment(
                host_node,
                input_a,
                f32s_to_bytes(&vec![2.0; elems as usize]),
            );
            runtime.seed_host_segment(
                host_node,
                input_b,
                f32s_to_bytes(&vec![3.0; elems as usize]),
            );
            runtime.seed_host_segment(host_node, output_f, vec![0u8; size_bytes as usize]);

            let backend_spec = host_vector_backend_spec_from_manifest(
                &host_manifest,
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
                    segment: output_f,
                    offset: 0,
                },
                size_bytes,
                elems,
            )
            .expect("host backend spec");

            runtime
                .submit_dispatch(
                    DispatchRequest {
                        task: TaskKey {
                            logical_system: LogicalSystemId(1),
                            coord: HierarchyCoord { levels: [0; 8] },
                            scope_depth: 0,
                            task_id: seq,
                        },
                        function: FunctionLabel {
                            name: "host_vector_example".into(),
                            level: PlLevel::L2,
                        },
                        backend_spec: Some(backend_spec),
                        request: None,
                        target_level: PlLevel::L2,
                        target_node: ubpu_node,
                        input_segments: vec![input_a, input_b],
                    },
                    sink,
                )
                .expect("host dispatch submit");

            runtime.advance_to(seq * 100, sink);
            let completions = runtime.poll_completions(seq * 100, sink);
            assert_eq!(completions.len(), 1);
            assert_eq!(completions[0].status, CompletionStatus::Success);
        };

        run_host(
            &mut runtime,
            &mut sink,
            1,
            SegmentHandle(500),
            SegmentHandle(501),
            SegmentHandle(502),
        );

        let input_a = SegmentHandle(510);
        let input_b = SegmentHandle(511);
        let output_f = SegmentHandle(512);
        runtime.seed_host_segment(
            host_node,
            input_a,
            f32s_to_bytes(&vec![2.0; elems as usize]),
        );
        runtime.seed_host_segment(
            host_node,
            input_b,
            f32s_to_bytes(&vec![3.0; elems as usize]),
        );
        runtime.seed_host_segment(host_node, output_f, vec![0u8; size_bytes as usize]);
        let tmrb_spec = DispatchBackendSpec {
            profile: DispatchBackendProfile::TmrbVector,
            platform: "a2a3sim".to_string(),
            runtime_variant: DispatchRuntimeVariant::TensormapAndRingbuffer,
            callable_hint: Some("tmrb_vector_example".to_string()),
            simpler_runtime: Some(
                w4_tmrb_vector_runtime_artifacts(
                    &tmrb_manifest,
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
                        segment: output_f,
                        offset: 0,
                    },
                    size_bytes,
                    elems,
                )
                .expect("tmrb backend spec"),
            ),
            context: None,
        };
        runtime
            .submit_dispatch(
                DispatchRequest {
                    task: TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 2,
                    },
                    function: FunctionLabel {
                        name: "tmrb_vector_example".into(),
                        level: PlLevel::L2,
                    },
                    backend_spec: Some(tmrb_spec),
                    request: None,
                    target_level: PlLevel::L2,
                    target_node: ubpu_node,
                    input_segments: vec![input_a, input_b],
                },
                &mut sink,
            )
            .expect("tmrb dispatch submit");
        runtime.advance_to(1_000, &mut sink);
        let completions = runtime.poll_completions(1_000, &mut sink);
        assert_eq!(completions.len(), 1);
        assert_eq!(completions[0].status, CompletionStatus::Success);

        run_host(
            &mut runtime,
            &mut sink,
            11,
            SegmentHandle(520),
            SegmentHandle(521),
            SegmentHandle(522),
        );
    }
}
