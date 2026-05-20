use anyhow::Context;
use serde::{Deserialize, Serialize};
use sim_config::ScenarioConfig;
use sim_core::{
    BlockHash, CompletionSource, CompletionStatus, CopyDirection, CopyRequest, DispatchRequest,
    FunctionLabel, HierarchyCoord, IoOpcode, IoSubmitReq, LogicalSystemId, MemoryEndpoint, PlLevel,
    SegmentHandle, SimEvent, TaskKey,
};
use sim_memory::{
    BoundaryLookupRequest, EmbeddingRow, EmbeddingSegment, EngramStateMaterializeFromBlockReq,
    EngramStateObject, ExecutionArtifactObject, HotMemoryMaterializeFromQueryReq,
    HotMemoryMaterializeReq, HotMemoryStateObject, LingquBlockPayloadRef, LingquDfsPath,
    LingquMemoryDurableStore, LingquMemoryDurableStoreSnapshot, LingquMemoryService,
    MemoryCatalogSnapshot, MemoryChunk, MemoryContentType, MemoryCorpusCatalog, MemoryPiiState,
    MemoryQuery, MemoryRecord, MemoryRecordState, MemoryRetentionPolicy, MemoryScope,
    MemorySecurityLabel, MemorySourceKind, MemoryTrustLevel, MemoryVisibility, PrefetchPlanRequest,
    PrefixCacheArtifact, PrefixCacheLookupRequest, QueryResult, VectorIndexKind, VectorIndexObject,
};
use sim_models::qwen3_dense_reference::{
    token_piece_bytes_from_tokenizer_path, token_piece_decode_bytes,
    tokenize_prompt_from_tokenizer_path,
};
use sim_models::{
    engram_simt_adapter::{
        artifact_config_from_env, discover_engram_simt_artifact, EngramSimtLaunchSpec,
    },
    qwen3_dense::{
        decode_hidden_bytes, hidden_range_bytes, kv_state_bytes_for_layer_count,
        model_key as qwen3_dense_model_key, profile_from_weights_dir, Qwen3DenseProfile,
        QWEN3_DENSE_DEFAULT_DECODE_TOKENS, QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
        QWEN3_DENSE_DEFAULT_TP_NODES,
    },
};
use sim_qemu::{
    GuestDescriptor, GuestIoDescriptor, GuestServiceDescriptor, LinquDeviceModel, QemuMmioHandler,
};
use sim_report::{
    AuxiliaryDebugReport, CliReport, CompletionSourceStats, CompletionStatusStats, DecoderReport,
    DomainReport, EntityReport, EventSummary, HostReport, QemuBackendDemoReport, RouteReport,
    TopologyReport, UapiDemoReport, UbcReport, UbpuReport, UmmuReport,
};
use sim_runtime::{
    EventSink, EvictionPlan, InMemoryBlockStore, LocalRuntimeEngine, PromotionPlan,
    RecursiveRoutePlanner, RoutePlanner, RouteRequest, SimBlockStore, VecEventSink,
};
use sim_services::{
    db::{DbGetReq, DbPutReq},
    dfs::{DfsReadReq, DfsWriteReq},
    durable::{
        LingquBlockWriteOptions, LingquDfsAppendOptions, LingquDfsListOptions,
        LingquDfsWriteOptions, LingquDurableBatchOp, LingquDurableSim, LingquDurableSimSnapshot,
    },
    object::{
        LingquObjectKind, LingquObjectLocality, LingquObjectMetadata, LingquObjectPublishReq,
        LingquObjectResolveReq, LingquObjectServiceProfile, LingquObjectServiceSnapshot,
        LingquObjectServiceStub, LingquObjectState, LingquObjectVersionSelector,
        LingquObmmObjectRefWire, LingquPayloadBackend, LingquPayloadPlacement,
    },
    shmem::{ShmemGetReq, ShmemPutReq},
};
use sim_topology::SimTopology;
use sim_uapi::{
    qwen3_dense_reference_decode_loop_report, qwen3_dense_reference_decode_loop_report_with_prompt,
    qwen3_dense_reference_default_guest_input, qwen3_dense_reference_prefill_text_output_report,
    qwen3_dense_reference_range_forward_report_with_prompt, qwen3_obmm_object_ref_for_payload,
    qwen3_obmm_object_ref_wire_to_hex, qwen3_publish_engram_state_registry_payload,
    qwen3_publish_object_registry_payload, qwen3_validate_engram_state_object_service_payload,
    qwen3_validate_engram_state_registry_payload, LocalGuestUapiSurface,
    Qwen3EngramStateRegistryValidation, UapiCommand, UapiDescriptor, UapiResponse,
    QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_GATE_WEIGHT,
    QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_INDICES,
    QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_TABLE, QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_STATE,
    QWEN3_DENSE_PROFILE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
    QWEN3_DENSE_PROFILE_OBMM_KIND_QWEN3_KV_STATE, QWEN3_DENSE_PROFILE_OBMM_KIND_TERMINAL_LOGITS,
    SIM_QWEN3_GUEST_ENGRAM_STATE_REF, SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR,
    SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT,
};
use sim_workloads::{run_host_vector_dispatch, run_minimal_workload};
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

mod qwen3_simpler;

fn main() -> anyhow::Result<()> {
    if lingqu_durable_args() {
        return run_lingqu_durable_cli();
    }
    if lingqu_memory_args() {
        return run_lingqu_memory_cli();
    }
    if lingqu_object_service_args() {
        return run_lingqu_object_service_cli();
    }
    if let Some(args) = qwen3_simpler::args()? {
        return run_qwen3_simpler_generate_cli(args);
    }
    if let Some(args) = qwen3_decode_loop_args()? {
        return run_qwen3_decode_loop_cli(&args);
    }
    if let Some(args) = qwen3_guest_decode_loop_args()? {
        return run_qwen3_guest_decode_loop_cli(&args);
    }
    if let Some(args) = qwen3_range_forward_args()? {
        return run_qwen3_range_forward_cli(&args);
    }
    if let Some(scenario_path) = qwen3_text_output_scenario_from_args() {
        return run_qwen3_text_output_cli(&scenario_path);
    }
    if let Some(manifest_path) = host_vector_manifest_from_args() {
        return run_host_vector_cli(&manifest_path);
    }
    let scenario_path = scenario_path_from_args();
    let config = ScenarioConfig::from_yaml_file(&scenario_path).with_context(|| {
        format!(
            "failed to load scenario config from {}",
            scenario_path.display()
        )
    })?;
    let topology = SimTopology::from_config(&config).context("failed to build topology")?;
    let mut workload_report =
        run_minimal_workload(&config, &topology).context("failed to run minimal workload")?;
    workload_report.summary = summarize_events(&workload_report.events);
    let auxiliary = if include_auxiliary_debug() {
        let runtime_events =
            run_demo(&config, &topology).context("failed to run route/store/event demo")?;
        let uapi_report = run_uapi_demo(&topology).context("failed to run local uapi demo")?;
        let qemu_backend_report =
            run_qemu_backend_demo(&topology).context("failed to run qemu backend demo")?;
        Some(AuxiliaryDebugReport {
            runtime_summary: summarize_events(&runtime_events),
            runtime_events,
            uapi_report,
            qemu_backend_report,
        })
    } else {
        None
    };

    let report = CliReport {
        scenario_name: config.scenario.name,
        group: config.scenario.group,
        variant: config.scenario.variant,
        logical_system: config.scenario.logical_system,
        scenario_file: scenario_path.display().to_string(),
        topology: topology_report(&topology),
        workload_report,
        auxiliary,
    };

    print_report(&report);
    if let Some(assessment) = compute_w4_assessment(&report.workload_report.summary) {
        if !assessment.complete {
            anyhow::bail!("w4 assessment incomplete: {}", assessment.missing.join(","));
        }
    }

    Ok(())
}

fn run_qwen3_simpler_generate_cli(
    args: qwen3_simpler::Qwen3SimplerGenerateArgs,
) -> anyhow::Result<()> {
    let runtime_name = qwen3_simpler::runtime_name(&args)
        .context("failed to inspect Qwen3 simpler build_output runtime")?;
    let manifest_path = default_simpler_qwen3_runtime_manifest_path(&runtime_name, &args.platform);
    ensure_simpler_qwen3_runtime_manifest(&manifest_path, &runtime_name, &args.platform)
        .context("failed to prepare reusable Qwen3 simpler runtime artifacts")?;
    let result = qwen3_simpler::run(args, &manifest_path)?;
    println!("text: {}", result.text);
    println!("token_ids: {:?}", result.token_ids);
    println!("finish_reason: {}", result.finish_reason);
    Ok(())
}

fn lingqu_object_service_args() -> bool {
    lingqu_object_service_args_from(env::args_os().skip(1))
}

fn lingqu_object_service_args_from<I, S>(args: I) -> bool
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    matches!(
        args.into_iter().next().map(Into::into),
        Some(mode) if mode == "lingqu-object-service"
    )
}

fn lingqu_durable_args() -> bool {
    lingqu_durable_args_from(env::args_os().skip(1))
}

fn lingqu_durable_args_from<I, S>(args: I) -> bool
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    matches!(
        args.into_iter().next().map(Into::into),
        Some(mode) if mode == "lingqu-durable"
    )
}

fn lingqu_memory_args() -> bool {
    lingqu_memory_args_from(env::args_os().skip(1))
}

fn lingqu_memory_args_from<I, S>(args: I) -> bool
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    matches!(
        args.into_iter().next().map(Into::into),
        Some(mode) if mode == "lingqu-memory"
    )
}

fn qwen3_text_output_scenario_from_args() -> Option<PathBuf> {
    let mut args = env::args_os().skip(1);
    match args.next() {
        Some(mode) if mode == "qwen3-text-output" => Some(
            args.next()
                .map(PathBuf::from)
                .unwrap_or_else(default_scenario_path),
        ),
        _ => None,
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3DecodeLoopCliArgs {
    scenario_path: PathBuf,
    step_count: usize,
    prompt: Option<String>,
    matmul_batch: Option<usize>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3GuestDecodeLoopCliArgs {
    step_count: usize,
    prompt: Option<String>,
    prompt_token_ids: Option<String>,
    script_path: PathBuf,
    matmul_batch: Option<usize>,
    model: Option<String>,
    weights_path: Option<PathBuf>,
    w5_profile: Option<String>,
    engram: Qwen3EngramConfig,
    memory_bootstrap: Option<W5MemoryBootstrapConfig>,
    memory_decisions: Option<W5MemoryDecisionConfig>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct W5MemoryBootstrapConfig {
    store_path: PathBuf,
    object_store_path: PathBuf,
    engram_state_path: PathBuf,
    registry_dir: PathBuf,
    owner_entity: u32,
    producer_entity: u32,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct W5MemoryDecisionConfig {
    store_path: PathBuf,
    boundary_request_path: Option<PathBuf>,
    boundary_observation_id: Option<String>,
    shortpath_decision_id: Option<String>,
    shortpath_execute: bool,
    prefetch_plan_id: Option<String>,
    prefix_cache_reuse_plan_id: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3DenseGuestRuntime {
    profile: Qwen3DenseProfile,
    model_key: String,
    weights_path: PathBuf,
    chipbackend_profile: &'static str,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct Qwen3RangeForwardCliArgs {
    scenario_path: PathBuf,
    prompt: String,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Qwen3EngramMode {
    Cpu,
    FusedSimt,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Qwen3EngramPool {
    Inline,
    Object,
    Obmm,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Qwen3EngramContextOp {
    Disabled,
    CpuReference,
    FusedSimt,
    SimplerHost,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Qwen3EngramReport {
    None,
    Summary,
    Steps,
    Verbose,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3EngramConfig {
    enabled: bool,
    mode: Qwen3EngramMode,
    pool: Qwen3EngramPool,
    owner_node: usize,
    no_repeat_ngram_size: usize,
    repetition_penalty_milli: u32,
    history_window: usize,
    blocked_token_ids: Vec<u64>,
    context_op: Qwen3EngramContextOp,
    report: Qwen3EngramReport,
    state_ref: Option<String>,
    object_registry_dir: Option<PathBuf>,
    object_service_snapshot_path: Option<PathBuf>,
}

impl Default for Qwen3EngramConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            mode: Qwen3EngramMode::Cpu,
            pool: Qwen3EngramPool::Inline,
            owner_node: 8,
            no_repeat_ngram_size: 0,
            repetition_penalty_milli: 1000,
            history_window: 0,
            blocked_token_ids: Vec::new(),
            context_op: Qwen3EngramContextOp::Disabled,
            report: Qwen3EngramReport::Summary,
            state_ref: None,
            object_registry_dir: None,
            object_service_snapshot_path: None,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3CandidateRecord {
    step_index: u64,
    rank: u64,
    token_id: u64,
    logit_milli: i32,
    adjusted_score_milli: i32,
    token_piece_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3EngramState {
    session_id: u64,
    step_index: u64,
    token_count: u64,
    rolling_hash: u64,
    ngram_window: u8,
    repetition_penalty_milli: u32,
    blocked_token_count: u32,
    fallback_used: bool,
    raw_sampled_token: u64,
    runner_up_token: u64,
    top_score_milli: i32,
    runner_up_score_milli: i32,
    history_window: u64,
    logits_checksum: u64,
    text_checksum: u64,
    selected_token: u64,
    state_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3EngramStepDecision {
    step_index: u64,
    candidates: Vec<Qwen3CandidateRecord>,
    selected_token: u64,
    blocked_token_count: u32,
    fallback_used: bool,
    state: Qwen3EngramState,
}

fn qwen3_decode_loop_args() -> anyhow::Result<Option<Qwen3DecodeLoopCliArgs>> {
    qwen3_decode_loop_args_from(env::args_os().skip(1))
}

fn qwen3_range_forward_args() -> anyhow::Result<Option<Qwen3RangeForwardCliArgs>> {
    qwen3_range_forward_args_from(env::args_os().skip(1))
}

fn qwen3_guest_decode_loop_args() -> anyhow::Result<Option<Qwen3GuestDecodeLoopCliArgs>> {
    qwen3_guest_decode_loop_args_from(env::args_os().skip(1))
}

fn qwen3_decode_loop_args_from<I, S>(args: I) -> anyhow::Result<Option<Qwen3DecodeLoopCliArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    match args.next() {
        Some(mode) if mode == "qwen3-decode-loop" => {
            let mut scenario_path = None;
            let mut step_count = None;
            let mut prompt = None;
            let mut matmul_batch = None;
            let mut positionals = Vec::new();
            let mut pending = args.peekable();

            while let Some(value) = pending.next() {
                let text = value.to_string_lossy();
                if text == "--scenario" || text == "--nodes" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("{text} requires a value"))?;
                    scenario_path = Some(qwen3_scenario_path_from_value(&next.to_string_lossy()));
                } else if let Some(value) = text.strip_prefix("--scenario=") {
                    scenario_path = Some(qwen3_scenario_path_from_value(value));
                } else if let Some(value) = text.strip_prefix("--nodes=") {
                    scenario_path = Some(qwen3_scenario_path_from_value(value));
                } else if text == "--steps" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--steps requires a value"))?;
                    step_count = Some(parse_positive_usize("--steps", &next.to_string_lossy())?);
                } else if let Some(value) = text.strip_prefix("--steps=") {
                    step_count = Some(parse_positive_usize("--steps", value)?);
                } else if text == "--prompt" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--prompt requires a value"))?;
                    prompt = Some(next.to_string_lossy().to_string());
                } else if let Some(value) = text.strip_prefix("--prompt=") {
                    prompt = Some(value.to_string());
                } else if text == "--matmul-batch" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--matmul-batch requires a value"))?;
                    matmul_batch = Some(parse_positive_usize(
                        "--matmul-batch",
                        &next.to_string_lossy(),
                    )?);
                } else if let Some(value) = text.strip_prefix("--matmul-batch=") {
                    matmul_batch = Some(parse_positive_usize("--matmul-batch", value)?);
                } else if text.starts_with("--") {
                    anyhow::bail!("unknown qwen3-decode-loop option: {text}");
                } else {
                    positionals.push(value);
                }
            }

            let mut positional_index = 0usize;
            if scenario_path.is_none() {
                if let Some(value) = positionals.get(positional_index) {
                    scenario_path = Some(qwen3_scenario_path_from_value(&value.to_string_lossy()));
                    positional_index += 1;
                }
            }
            if step_count.is_none() {
                if let Some(value) = positionals.get(positional_index) {
                    let value = value.to_string_lossy();
                    if let Ok(parsed) = value.parse::<usize>() {
                        if parsed == 0 {
                            anyhow::bail!("step count must be > 0");
                        }
                        step_count = Some(parsed);
                        positional_index += 1;
                    }
                }
            }
            if prompt.is_none() {
                if let Some(value) = positionals.get(positional_index) {
                    prompt = Some(value.to_string_lossy().to_string());
                }
            }

            Ok(Some(Qwen3DecodeLoopCliArgs {
                scenario_path: scenario_path.unwrap_or_else(default_scenario_path),
                step_count: step_count.unwrap_or(2),
                prompt,
                matmul_batch,
            }))
        }
        _ => Ok(None),
    }
}

fn qwen3_guest_decode_loop_args_from<I, S>(
    args: I,
) -> anyhow::Result<Option<Qwen3GuestDecodeLoopCliArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    match args.next() {
        Some(mode) if mode == "qwen3-guest-decode-loop" || mode == "w5-inference-cluster" => {
            let mut step_count = None;
            let mut prompt = None;
            let mut prompt_token_ids = None;
            let mut script_path = None;
            let mut matmul_batch = None;
            let mut model = None;
            let mut weights_path = None;
            let mut w5_profile = None;
            let mut engram = Qwen3EngramConfig::default();
            let mut memory_store_path = None;
            let mut memory_object_store_path = None;
            let mut memory_engram_state_path = None;
            let mut memory_registry_dir = None;
            let mut memory_owner_entity = None;
            let mut memory_producer_entity = None;
            let mut memory_decision_store_path = None;
            let mut memory_boundary_request_path = None;
            let mut memory_boundary_observation_id = None;
            let mut memory_shortpath_decision_id = None;
            let mut memory_shortpath_execute = false;
            let mut memory_prefetch_plan_id = None;
            let mut memory_prefix_cache_reuse_plan_id = None;
            let mut positionals = Vec::new();
            let mut pending = args.peekable();

            while let Some(value) = pending.next() {
                let text = value.to_string_lossy();
                if text == "--steps" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--steps requires a value"))?;
                    step_count = Some(parse_positive_usize("--steps", &next.to_string_lossy())?);
                } else if let Some(value) = text.strip_prefix("--steps=") {
                    step_count = Some(parse_positive_usize("--steps", value)?);
                } else if text == "--prompt" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--prompt requires a value"))?;
                    prompt = Some(next.to_string_lossy().to_string());
                } else if let Some(value) = text.strip_prefix("--prompt=") {
                    prompt = Some(value.to_string());
                } else if text == "--prompt-token-ids" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--prompt-token-ids requires a value"))?;
                    let value = next.to_string_lossy().to_string();
                    qwen3_parse_token_id_csv(&value)?;
                    prompt_token_ids = Some(value);
                } else if let Some(value) = text.strip_prefix("--prompt-token-ids=") {
                    qwen3_parse_token_id_csv(value)?;
                    prompt_token_ids = Some(value.to_string());
                } else if text == "--script" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--script requires a value"))?;
                    script_path = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--script=") {
                    script_path = Some(PathBuf::from(value));
                } else if text == "--matmul-batch" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--matmul-batch requires a value"))?;
                    matmul_batch = Some(parse_positive_usize(
                        "--matmul-batch",
                        &next.to_string_lossy(),
                    )?);
                } else if let Some(value) = text.strip_prefix("--matmul-batch=") {
                    matmul_batch = Some(parse_positive_usize("--matmul-batch", value)?);
                } else if text == "--model" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--model requires a value"))?;
                    model = Some(next.to_string_lossy().to_string());
                } else if let Some(value) = text.strip_prefix("--model=") {
                    model = Some(value.to_string());
                } else if text == "--weights-path" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--weights-path requires a value"))?;
                    weights_path = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--weights-path=") {
                    weights_path = Some(PathBuf::from(value));
                } else if text == "--w5-profile" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--w5-profile requires a value"))?;
                    w5_profile = Some(validate_w5_inference_profile(&next.to_string_lossy())?);
                } else if let Some(value) = text.strip_prefix("--w5-profile=") {
                    w5_profile = Some(validate_w5_inference_profile(value)?);
                } else if text == "--engram" {
                    engram.enabled = true;
                } else if text == "--engram-mode" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--engram-mode requires a value"))?;
                    engram.mode = parse_qwen3_engram_mode(&next.to_string_lossy())?;
                } else if let Some(value) = text.strip_prefix("--engram-mode=") {
                    engram.mode = parse_qwen3_engram_mode(value)?;
                } else if text == "--engram-pool" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--engram-pool requires a value"))?;
                    engram.pool = parse_qwen3_engram_pool(&next.to_string_lossy())?;
                } else if let Some(value) = text.strip_prefix("--engram-pool=") {
                    engram.pool = parse_qwen3_engram_pool(value)?;
                } else if text == "--engram-owner-node" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--engram-owner-node requires a value"))?;
                    engram.owner_node = parse_qwen3_engram_owner_node(&next.to_string_lossy())?;
                } else if let Some(value) = text.strip_prefix("--engram-owner-node=") {
                    engram.owner_node = parse_qwen3_engram_owner_node(value)?;
                } else if text == "--no-repeat-ngram-size" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--no-repeat-ngram-size requires a value")
                    })?;
                    engram.no_repeat_ngram_size =
                        parse_nonnegative_usize("--no-repeat-ngram-size", &next.to_string_lossy())?;
                } else if let Some(value) = text.strip_prefix("--no-repeat-ngram-size=") {
                    engram.no_repeat_ngram_size =
                        parse_nonnegative_usize("--no-repeat-ngram-size", value)?;
                } else if text == "--repetition-penalty" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--repetition-penalty requires a value"))?;
                    engram.repetition_penalty_milli =
                        parse_repetition_penalty_milli(&next.to_string_lossy())?;
                } else if let Some(value) = text.strip_prefix("--repetition-penalty=") {
                    engram.repetition_penalty_milli = parse_repetition_penalty_milli(value)?;
                } else if text == "--engram-block-token-id" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--engram-block-token-id requires a value")
                    })?;
                    engram.blocked_token_ids.push(parse_nonnegative_u64(
                        "--engram-block-token-id",
                        &next.to_string_lossy(),
                    )?);
                } else if let Some(value) = text.strip_prefix("--engram-block-token-id=") {
                    engram
                        .blocked_token_ids
                        .push(parse_nonnegative_u64("--engram-block-token-id", value)?);
                } else if text == "--engram-block-token-ids" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--engram-block-token-ids requires a value")
                    })?;
                    engram
                        .blocked_token_ids
                        .extend(qwen3_parse_token_id_csv(&next.to_string_lossy())?);
                } else if let Some(value) = text.strip_prefix("--engram-block-token-ids=") {
                    engram
                        .blocked_token_ids
                        .extend(qwen3_parse_token_id_csv(value)?);
                } else if text == "--engram-context-op" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--engram-context-op requires a value"))?;
                    engram.context_op = parse_qwen3_engram_context_op(&next.to_string_lossy())?;
                } else if let Some(value) = text.strip_prefix("--engram-context-op=") {
                    engram.context_op = parse_qwen3_engram_context_op(value)?;
                } else if text == "--engram-history-window" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--engram-history-window requires a value")
                    })?;
                    engram.history_window = parse_nonnegative_usize(
                        "--engram-history-window",
                        &next.to_string_lossy(),
                    )?;
                } else if let Some(value) = text.strip_prefix("--engram-history-window=") {
                    engram.history_window =
                        parse_nonnegative_usize("--engram-history-window", value)?;
                } else if text == "--engram-report" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--engram-report requires a value"))?;
                    engram.report = parse_qwen3_engram_report(&next.to_string_lossy())?;
                } else if let Some(value) = text.strip_prefix("--engram-report=") {
                    engram.report = parse_qwen3_engram_report(value)?;
                } else if text == "--engram-state-ref" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--engram-state-ref requires a value"))?;
                    engram.state_ref =
                        Some(validate_qwen3_engram_state_ref(&next.to_string_lossy())?);
                } else if let Some(value) = text.strip_prefix("--engram-state-ref=") {
                    engram.state_ref = Some(validate_qwen3_engram_state_ref(value)?);
                } else if text == "--object-registry-dir" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--object-registry-dir requires a value"))?;
                    engram.object_registry_dir = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--object-registry-dir=") {
                    engram.object_registry_dir = Some(PathBuf::from(value));
                } else if text == "--object-service-snapshot" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--object-service-snapshot requires a value")
                    })?;
                    engram.object_service_snapshot_path = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--object-service-snapshot=") {
                    engram.object_service_snapshot_path = Some(PathBuf::from(value));
                } else if text == "--memory-store" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--memory-store requires a value"))?;
                    memory_store_path = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--memory-store=") {
                    memory_store_path = Some(PathBuf::from(value));
                } else if text == "--memory-object-store" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--memory-object-store requires a value"))?;
                    memory_object_store_path = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--memory-object-store=") {
                    memory_object_store_path = Some(PathBuf::from(value));
                } else if text == "--memory-engram-state" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--memory-engram-state requires a value"))?;
                    memory_engram_state_path = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--memory-engram-state=") {
                    memory_engram_state_path = Some(PathBuf::from(value));
                } else if text == "--memory-registry-dir" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--memory-registry-dir requires a value"))?;
                    memory_registry_dir = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--memory-registry-dir=") {
                    memory_registry_dir = Some(PathBuf::from(value));
                } else if text == "--memory-owner-entity" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--memory-owner-entity requires a value"))?;
                    memory_owner_entity = Some(parse_cli_u32(
                        "--memory-owner-entity",
                        &next.to_string_lossy(),
                    )?);
                } else if let Some(value) = text.strip_prefix("--memory-owner-entity=") {
                    memory_owner_entity = Some(parse_cli_u32("--memory-owner-entity", value)?);
                } else if text == "--memory-producer-entity" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--memory-producer-entity requires a value")
                    })?;
                    memory_producer_entity = Some(parse_cli_u32(
                        "--memory-producer-entity",
                        &next.to_string_lossy(),
                    )?);
                } else if let Some(value) = text.strip_prefix("--memory-producer-entity=") {
                    memory_producer_entity =
                        Some(parse_cli_u32("--memory-producer-entity", value)?);
                } else if text == "--memory-decision-store" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--memory-decision-store requires a value")
                    })?;
                    memory_decision_store_path = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--memory-decision-store=") {
                    memory_decision_store_path = Some(PathBuf::from(value));
                } else if text == "--memory-boundary-request" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--memory-boundary-request requires a value")
                    })?;
                    memory_boundary_request_path = Some(PathBuf::from(next));
                } else if let Some(value) = text.strip_prefix("--memory-boundary-request=") {
                    memory_boundary_request_path = Some(PathBuf::from(value));
                } else if text == "--memory-boundary-observation-id" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--memory-boundary-observation-id requires a value")
                    })?;
                    memory_boundary_observation_id = Some(next.to_string_lossy().to_string());
                } else if let Some(value) = text.strip_prefix("--memory-boundary-observation-id=") {
                    memory_boundary_observation_id = Some(value.to_string());
                } else if text == "--memory-shortpath-decision-id" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--memory-shortpath-decision-id requires a value")
                    })?;
                    memory_shortpath_decision_id = Some(next.to_string_lossy().to_string());
                } else if let Some(value) = text.strip_prefix("--memory-shortpath-decision-id=") {
                    memory_shortpath_decision_id = Some(value.to_string());
                } else if text == "--memory-shortpath-execute" {
                    memory_shortpath_execute = true;
                } else if let Some(value) = text.strip_prefix("--memory-shortpath-execute=") {
                    memory_shortpath_execute = parse_cli_bool("--memory-shortpath-execute", value)?;
                } else if text == "--memory-prefetch-plan-id" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--memory-prefetch-plan-id requires a value")
                    })?;
                    memory_prefetch_plan_id = Some(next.to_string_lossy().to_string());
                } else if let Some(value) = text.strip_prefix("--memory-prefetch-plan-id=") {
                    memory_prefetch_plan_id = Some(value.to_string());
                } else if text == "--memory-prefix-cache-reuse-plan-id" {
                    let next = pending.next().ok_or_else(|| {
                        anyhow::anyhow!("--memory-prefix-cache-reuse-plan-id requires a value")
                    })?;
                    memory_prefix_cache_reuse_plan_id = Some(next.to_string_lossy().to_string());
                } else if let Some(value) =
                    text.strip_prefix("--memory-prefix-cache-reuse-plan-id=")
                {
                    memory_prefix_cache_reuse_plan_id = Some(value.to_string());
                } else if text.starts_with("--") {
                    anyhow::bail!("unknown qwen3-guest-decode-loop option: {text}");
                } else {
                    positionals.push(value);
                }
            }

            let mut positional_index = 0usize;
            if step_count.is_none() {
                if let Some(value) = positionals.get(positional_index) {
                    let value = value.to_string_lossy();
                    if let Ok(parsed) = value.parse::<usize>() {
                        if parsed == 0 {
                            anyhow::bail!("step count must be > 0");
                        }
                        step_count = Some(parsed);
                        positional_index += 1;
                    }
                }
            }
            if prompt.is_none() {
                if let Some(value) = positionals.get(positional_index) {
                    prompt = Some(value.to_string_lossy().to_string());
                }
            }
            if engram.state_ref.is_some()
                && engram.object_registry_dir.is_none()
                && engram.object_service_snapshot_path.is_none()
            {
                anyhow::bail!(
                    "--engram-state-ref requires --object-registry-dir or --object-service-snapshot"
                );
            }
            let memory_bootstrap = match (
                memory_store_path,
                memory_object_store_path,
                memory_engram_state_path,
                memory_registry_dir,
            ) {
                (None, None, None, None) => None,
                (
                    Some(store_path),
                    Some(object_store_path),
                    Some(engram_state_path),
                    Some(registry_dir),
                ) => {
                    if engram.state_ref.is_some()
                        || engram.object_registry_dir.is_some()
                        || engram.object_service_snapshot_path.is_some()
                    {
                        anyhow::bail!(
                            "--memory-* bootstrap cannot be combined with explicit --engram-state-ref/--object-registry-dir/--object-service-snapshot"
                        );
                    }
                    Some(W5MemoryBootstrapConfig {
                        store_path,
                        object_store_path,
                        engram_state_path,
                        registry_dir,
                        owner_entity: memory_owner_entity.unwrap_or(0),
                        producer_entity: memory_producer_entity.unwrap_or(0),
                    })
                }
                _ => anyhow::bail!(
                    "--memory-store, --memory-object-store, --memory-engram-state, and --memory-registry-dir must be provided together"
                ),
            };
            let memory_has_decision_id = memory_shortpath_decision_id.is_some()
                || memory_prefetch_plan_id.is_some()
                || memory_prefix_cache_reuse_plan_id.is_some();
            let shortpath_source_count = usize::from(memory_boundary_request_path.is_some())
                + usize::from(memory_boundary_observation_id.is_some())
                + usize::from(memory_shortpath_decision_id.is_some());
            if shortpath_source_count > 1 {
                anyhow::bail!(
                    "--memory-boundary-request, --memory-boundary-observation-id, and --memory-shortpath-decision-id are mutually exclusive"
                );
            }
            let memory_has_decision_input = memory_has_decision_id
                || memory_boundary_request_path.is_some()
                || memory_boundary_observation_id.is_some();
            let memory_decisions = if let Some(store_path) = memory_decision_store_path {
                if !memory_has_decision_input {
                    anyhow::bail!(
                        "--memory-decision-store requires at least one of --memory-boundary-request, --memory-boundary-observation-id, --memory-shortpath-decision-id, --memory-prefetch-plan-id, or --memory-prefix-cache-reuse-plan-id"
                    );
                }
                Some(W5MemoryDecisionConfig {
                    store_path,
                    boundary_request_path: memory_boundary_request_path,
                    boundary_observation_id: memory_boundary_observation_id,
                    shortpath_decision_id: memory_shortpath_decision_id,
                    shortpath_execute: memory_shortpath_execute,
                    prefetch_plan_id: memory_prefetch_plan_id,
                    prefix_cache_reuse_plan_id: memory_prefix_cache_reuse_plan_id,
                })
            } else if memory_has_decision_input {
                anyhow::bail!(
                    "--memory-decision-store is required when W5 planner or Memory Service plan ids are provided"
                );
            } else {
                None
            };
            if engram.state_ref.is_some() {
                engram.enabled = true;
                engram.pool = Qwen3EngramPool::Obmm;
                if engram.context_op == Qwen3EngramContextOp::Disabled {
                    engram.context_op = Qwen3EngramContextOp::CpuReference;
                }
            }
            if engram.enabled && engram.pool != Qwen3EngramPool::Obmm {
                anyhow::bail!(
                    "qwen3-guest-decode-loop --engram currently requires --engram-pool=obmm"
                );
            }

            Ok(Some(Qwen3GuestDecodeLoopCliArgs {
                step_count: step_count.unwrap_or(1),
                prompt,
                prompt_token_ids,
                script_path: script_path.unwrap_or_else(default_qwen3_guest_decode_script_path),
                matmul_batch,
                model,
                weights_path,
                w5_profile,
                engram,
                memory_bootstrap,
                memory_decisions,
            }))
        }
        _ => Ok(None),
    }
}

fn qwen3_range_forward_args_from<I, S>(args: I) -> anyhow::Result<Option<Qwen3RangeForwardCliArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<std::ffi::OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    match args.next() {
        Some(mode) if mode == "qwen3-range-forward" => {
            let mut scenario_path = None;
            let mut prompt = None;
            let mut positionals = Vec::new();
            let mut pending = args.peekable();

            while let Some(value) = pending.next() {
                let text = value.to_string_lossy();
                if text == "--scenario" || text == "--nodes" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("{text} requires a value"))?;
                    scenario_path = Some(qwen3_scenario_path_from_value(&next.to_string_lossy()));
                } else if let Some(value) = text.strip_prefix("--scenario=") {
                    scenario_path = Some(qwen3_scenario_path_from_value(value));
                } else if let Some(value) = text.strip_prefix("--nodes=") {
                    scenario_path = Some(qwen3_scenario_path_from_value(value));
                } else if text == "--prompt" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--prompt requires a value"))?;
                    prompt = Some(next.to_string_lossy().to_string());
                } else if let Some(value) = text.strip_prefix("--prompt=") {
                    prompt = Some(value.to_string());
                } else if text.starts_with("--") {
                    anyhow::bail!("unknown qwen3-range-forward option: {text}");
                } else {
                    positionals.push(value);
                }
            }

            let mut positional_index = 0usize;
            if scenario_path.is_none() {
                if let Some(value) = positionals.get(positional_index) {
                    scenario_path = Some(qwen3_scenario_path_from_value(&value.to_string_lossy()));
                    positional_index += 1;
                }
            }
            if prompt.is_none() {
                if let Some(value) = positionals.get(positional_index) {
                    prompt = Some(value.to_string_lossy().to_string());
                }
            }

            Ok(Some(Qwen3RangeForwardCliArgs {
                scenario_path: scenario_path.unwrap_or_else(default_scenario_path),
                prompt: prompt.unwrap_or_else(|| "Hello Qwen3".to_string()),
            }))
        }
        _ => Ok(None),
    }
}

fn default_qwen3_guest_decode_script_path() -> PathBuf {
    Path::new("guest-linux")
        .join("aarch64")
        .join("scripts")
        .join("run_ub_eight_node_w5_inference_cluster.sh")
}

fn validate_w5_inference_profile(value: &str) -> anyhow::Result<String> {
    match value {
        "qwen3_0_6b_decode"
        | "qwen3_14b_decode"
        | "qwen3_0_6b_engram_decode"
        | "qwen3_14b_engram_decode" => Ok(value.to_string()),
        _ => anyhow::bail!("unsupported --w5-profile: {value}"),
    }
}

fn qwen3_guest_default_w5_profile(
    runtime: &Qwen3DenseGuestRuntime,
    engram: &Qwen3EngramConfig,
) -> String {
    let model = if runtime.model_key == "qwen3-14b" {
        "qwen3_14b"
    } else {
        "qwen3_0_6b"
    };
    let mode = if engram.enabled {
        "engram_decode"
    } else {
        "decode"
    };
    format!("{model}_{mode}")
}

fn parse_positive_usize(label: &str, value: &str) -> anyhow::Result<usize> {
    let parsed = value
        .parse::<usize>()
        .with_context(|| format!("invalid {label}: {value}"))?;
    if parsed == 0 {
        anyhow::bail!("{label} must be > 0");
    }
    Ok(parsed)
}

fn parse_nonnegative_usize(label: &str, value: &str) -> anyhow::Result<usize> {
    value
        .parse::<usize>()
        .with_context(|| format!("invalid {label}: {value}"))
}

fn parse_nonnegative_u64(label: &str, value: &str) -> anyhow::Result<u64> {
    value
        .parse::<u64>()
        .with_context(|| format!("invalid {label}: {value}"))
}

fn parse_cli_u32(label: &str, value: &str) -> anyhow::Result<u32> {
    value
        .parse::<u32>()
        .with_context(|| format!("invalid {label}: {value}"))
}

fn parse_cli_bool(label: &str, value: &str) -> anyhow::Result<bool> {
    match value {
        "1" | "true" | "yes" | "on" => Ok(true),
        "0" | "false" | "no" | "off" => Ok(false),
        _ => anyhow::bail!("invalid {label}: {value}"),
    }
}

fn parse_qwen3_engram_mode(value: &str) -> anyhow::Result<Qwen3EngramMode> {
    match value {
        "cpu" => Ok(Qwen3EngramMode::Cpu),
        "fused-simt" => Ok(Qwen3EngramMode::FusedSimt),
        _ => anyhow::bail!("unsupported --engram-mode: {value}"),
    }
}

fn parse_qwen3_engram_pool(value: &str) -> anyhow::Result<Qwen3EngramPool> {
    match value {
        "inline" => Ok(Qwen3EngramPool::Inline),
        "object" => Ok(Qwen3EngramPool::Object),
        "obmm" => Ok(Qwen3EngramPool::Obmm),
        _ => anyhow::bail!("unsupported --engram-pool: {value}"),
    }
}

fn parse_qwen3_engram_context_op(value: &str) -> anyhow::Result<Qwen3EngramContextOp> {
    match value {
        "disabled" | "none" | "off" => Ok(Qwen3EngramContextOp::Disabled),
        "cpu" | "cpu-reference" => Ok(Qwen3EngramContextOp::CpuReference),
        "fused-simt" => Ok(Qwen3EngramContextOp::FusedSimt),
        "simpler-host" => Ok(Qwen3EngramContextOp::SimplerHost),
        _ => anyhow::bail!("unsupported --engram-context-op: {value}"),
    }
}

fn parse_qwen3_engram_owner_node(value: &str) -> anyhow::Result<usize> {
    let owner_node = parse_positive_usize("--engram-owner-node", value)?;

    if owner_node > 8 {
        anyhow::bail!("--engram-owner-node must be in 1..=8");
    }
    Ok(owner_node)
}

fn parse_qwen3_engram_report(value: &str) -> anyhow::Result<Qwen3EngramReport> {
    match value {
        "none" => Ok(Qwen3EngramReport::None),
        "summary" => Ok(Qwen3EngramReport::Summary),
        "steps" => Ok(Qwen3EngramReport::Steps),
        "verbose" => Ok(Qwen3EngramReport::Verbose),
        _ => anyhow::bail!("unsupported --engram-report: {value}"),
    }
}

fn validate_qwen3_engram_state_ref(value: &str) -> anyhow::Result<String> {
    let trimmed = value.trim();
    if trimmed.is_empty() {
        anyhow::bail!("--engram-state-ref must not be empty");
    }
    Ok(trimmed.to_string())
}

fn parse_repetition_penalty_milli(value: &str) -> anyhow::Result<u32> {
    let (whole, frac) = value.split_once('.').unwrap_or((value, ""));
    let whole = whole
        .parse::<u32>()
        .with_context(|| format!("invalid --repetition-penalty: {value}"))?;
    let mut frac_milli = 0u32;
    let mut scale = 100u32;
    for ch in frac.chars().take(3) {
        let digit = ch
            .to_digit(10)
            .ok_or_else(|| anyhow::anyhow!("invalid --repetition-penalty: {value}"))?;
        frac_milli += digit * scale;
        scale /= 10;
    }
    if frac.chars().count() > 3 {
        anyhow::bail!("--repetition-penalty supports at most 3 decimal places");
    }
    let milli = whole
        .checked_mul(1000)
        .and_then(|base| base.checked_add(frac_milli))
        .ok_or_else(|| anyhow::anyhow!("--repetition-penalty is too large"))?;
    if milli < 1000 {
        anyhow::bail!("--repetition-penalty must be >= 1.0");
    }
    Ok(milli)
}

fn qwen3_guest_engram_env_vars(
    config: &Qwen3EngramConfig,
    session_id: u64,
) -> Vec<(String, String)> {
    qwen3_guest_engram_env_vars_from_lookup(config, session_id, |key| env::var(key).ok())
}

fn qwen3_guest_engram_env_vars_from_lookup<F>(
    config: &Qwen3EngramConfig,
    session_id: u64,
    mut lookup: F,
) -> Vec<(String, String)>
where
    F: FnMut(&str) -> Option<String>,
{
    if !config.enabled {
        return Vec::new();
    }
    let mut vars = vec![
        ("SIM_QWEN3_GUEST_ENGRAM".to_string(), "1".to_string()),
        (
            "SIM_QWEN3_GUEST_ENGRAM_MODE".to_string(),
            qwen3_engram_mode_name(config.mode).to_string(),
        ),
        (
            "SIM_QWEN3_GUEST_ENGRAM_SESSION_ID".to_string(),
            format!("{session_id:016x}"),
        ),
        (
            "SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE".to_string(),
            config.owner_node.to_string(),
        ),
        (
            "SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE".to_string(),
            config.no_repeat_ngram_size.to_string(),
        ),
        (
            "SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI".to_string(),
            config.repetition_penalty_milli.to_string(),
        ),
        (
            "SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW".to_string(),
            config.history_window.to_string(),
        ),
        (
            "SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS".to_string(),
            config
                .blocked_token_ids
                .iter()
                .map(u64::to_string)
                .collect::<Vec<_>>()
                .join(","),
        ),
        (
            "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP".to_string(),
            qwen3_engram_context_op_name(config.context_op).to_string(),
        ),
    ];
    for key in [
        SIM_QWEN3_GUEST_ENGRAM_STATE_REF,
        SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR,
        SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT,
    ] {
        let value = match key {
            SIM_QWEN3_GUEST_ENGRAM_STATE_REF => config.state_ref.clone(),
            SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR => config
                .object_registry_dir
                .as_ref()
                .map(|path| path.display().to_string()),
            SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT => config
                .object_service_snapshot_path
                .as_ref()
                .map(|path| path.display().to_string()),
            _ => None,
        }
        .or_else(|| lookup(key))
        .filter(|value| !value.trim().is_empty());
        if let Some(value) = value {
            vars.push((key.to_string(), value));
        }
    }
    vars
}

fn qwen3_scenario_path_from_value(value: &str) -> PathBuf {
    match value {
        "2" | "2host" | "2-host" | "2node" | "2-node" => {
            Path::new("scenarios").join("mvp_2host_single_domain.yaml")
        }
        "4" | "4host" | "4-host" | "4node" | "4-node" => {
            Path::new("scenarios").join("mvp_4host_single_domain.yaml")
        }
        "8" | "8host" | "8-host" | "8node" | "8-node" => {
            Path::new("scenarios").join("mvp_8host_single_domain.yaml")
        }
        _ => PathBuf::from(value),
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Qwen3DecodeReportVerbosity {
    Summary,
    Steps,
    Verbose,
}

fn qwen3_decode_report_verbosity() -> Qwen3DecodeReportVerbosity {
    qwen3_decode_report_verbosity_from_env(
        env::var("SIM_QWEN3_DECODE_REPORT").ok().as_deref(),
        env::var("SIM_QWEN3_DECODE_VERBOSE").ok().as_deref(),
    )
}

fn qwen3_decode_report_verbosity_from_env(
    report: Option<&str>,
    verbose: Option<&str>,
) -> Qwen3DecodeReportVerbosity {
    if let Some(report) = report {
        return match report.trim().to_ascii_lowercase().as_str() {
            "step" | "steps" | "compact" => Qwen3DecodeReportVerbosity::Steps,
            "1" | "true" | "yes" | "on" | "verbose" | "full" | "detail" | "details" => {
                Qwen3DecodeReportVerbosity::Verbose
            }
            _ => Qwen3DecodeReportVerbosity::Summary,
        };
    }
    if verbose.map(env_flag_enabled).unwrap_or(false) {
        Qwen3DecodeReportVerbosity::Verbose
    } else {
        Qwen3DecodeReportVerbosity::Summary
    }
}

fn env_flag_enabled(value: &str) -> bool {
    matches!(
        value.trim().to_ascii_lowercase().as_str(),
        "1" | "true" | "yes" | "on"
    )
}

fn run_lingqu_object_service_cli() -> anyhow::Result<()> {
    if env::args_os()
        .skip(2)
        .any(|arg| arg == "stress" || arg == "--stress")
    {
        return run_lingqu_object_service_stress_cli();
    }

    let mut service = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
    publish_lingqu_object_cli_sample(
        &mut service,
        "qwen3/model/reference/layer/00/q_proj/shard/0",
        LingquObjectKind::WeightShard,
        LingquPayloadBackend::Block,
        4096,
        0x1001,
        0,
    )?;
    publish_lingqu_object_cli_sample(
        &mut service,
        "qwen3/session/demo/kv/layer/00/tile/0/position/00000001/k",
        LingquObjectKind::KvCacheBlock,
        LingquPayloadBackend::Shmem,
        2048,
        0x2002,
        1,
    )?;
    publish_lingqu_object_cli_sample(
        &mut service,
        "qwen3/session/demo/hidden/boundary/node/0/to/1/step/0",
        LingquObjectKind::RuntimeTensor,
        LingquPayloadBackend::Shmem,
        2048,
        0x3003,
        2,
    )?;
    resolve_lingqu_object_cli_sample(
        &mut service,
        "qwen3/model/reference/layer/00/q_proj/shard/0",
        &[LingquPayloadBackend::Block],
        3,
    )?;
    resolve_lingqu_object_cli_sample(
        &mut service,
        "qwen3/session/demo/kv/layer/00/tile/0/position/00000001/k",
        &[LingquPayloadBackend::Shmem],
        4,
    )?;
    resolve_lingqu_object_cli_sample(
        &mut service,
        "qwen3/session/demo/hidden/boundary/node/0/to/1/step/0",
        &[LingquPayloadBackend::Shmem],
        5,
    )?;

    let events = service.poll_ready(1000);
    let success_count = events
        .iter()
        .filter(|event| event.status == CompletionStatus::Success)
        .count();
    let report = service.report();
    println!("lingqu_object_service");
    println!("  events: {}", events.len());
    println!("  success: {}", success_count);
    println!("  publish_count: {}", report.publish_count);
    println!("  resolve_count: {}", report.resolve_count);
    println!("  metadata_put_count: {}", report.metadata_put_count);
    println!("  metadata_get_count: {}", report.metadata_get_count);
    println!("  shmem_write_count: {}", report.shmem_write_count);
    println!("  shmem_read_count: {}", report.shmem_read_count);
    println!("  block_write_count: {}", report.block_write_count);
    println!("  block_read_count: {}", report.block_read_count);
    println!("  obmm_pool_enabled: {}", report.obmm_pool_enabled);
    println!(
        "  obmm_pool_payload_write_count: {}",
        report.obmm_pool_payload_write_count
    );
    println!(
        "  obmm_pool_payload_read_count: {}",
        report.obmm_pool_payload_read_count
    );
    println!(
        "  obmm_pool_queue_submit_count: {}",
        report.obmm_pool_queue_submit_count
    );
    println!(
        "  obmm_pool_queue_deliver_count: {}",
        report.obmm_pool_queue_deliver_count
    );
    println!("  obmm_pool_bytes_used: {}", report.obmm_pool_bytes_used);
    println!(
        "  obmm_pool_reserved_bytes: {}",
        report.obmm_pool_reserved_bytes
    );
    println!("  obmm_pool_block_count: {}", report.obmm_pool_block_count);
    println!(
        "  obmm_pool_multi_block_write_count: {}",
        report.obmm_pool_multi_block_write_count
    );
    println!(
        "  obmm_pool_max_blocks_per_payload: {}",
        report.obmm_pool_max_blocks_per_payload
    );
    println!(
        "  committed_object_count: {}",
        report.committed_object_count
    );
    println!("  missing_resolve_count: {}", report.missing_resolve_count);
    println!("  checksum: {:#x}", report.checksum);
    Ok(())
}

fn run_lingqu_durable_cli() -> anyhow::Result<()> {
    let mut raw_args = env::args_os().skip(2);
    let mode = raw_args
        .next()
        .map(|arg| arg.to_string_lossy().into_owned())
        .unwrap_or_else(|| "stat".to_string());
    let args = raw_args
        .map(|arg| arg.to_string_lossy().into_owned())
        .collect::<Vec<_>>();
    match mode.as_str() {
        "append-log" => run_lingqu_durable_append_log_cli(&args),
        "batch" => run_lingqu_durable_batch_cli(&args),
        "init" => run_lingqu_durable_init_cli(&args),
        "list" => run_lingqu_durable_list_cli(&args),
        "read-log" => run_lingqu_durable_read_log_cli(&args),
        "stat" => run_lingqu_durable_stat_cli(&args),
        "validate" => run_lingqu_durable_validate_cli(&args),
        _ => anyhow::bail!(
            "unknown lingqu-durable mode `{mode}`; expected append-log, batch, init, list, read-log, stat, or validate"
        ),
    }
}

fn run_lingqu_durable_init_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let force = cli_flag(args, "--force");
    if store_path.exists() && !force {
        anyhow::bail!(
            "durable store already exists at {}; pass --force to replace it",
            store_path.display()
        );
    }
    let sim = LingquDurableSim::default();
    save_lingqu_durable_sim(&store_path, &sim)?;
    let snapshot = sim.export_snapshot().context("export durable snapshot")?;
    println!("lingqu_durable");
    println!("  mode: init");
    println!("  store: {}", store_path.display());
    println!("  kind: {}", snapshot.kind);
    println!("  schema_version: {}", snapshot.schema_version);
    println!("  checksum: {:#x}", snapshot.checksum);
    Ok(())
}

fn run_lingqu_durable_stat_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let snapshot = load_lingqu_durable_snapshot(&store_path)?;
    let _sim = LingquDurableSim::import_snapshot(snapshot.clone())
        .with_context(|| format!("import durable store {}", store_path.display()))?;
    let inline_file_versions = snapshot
        .dfs
        .files
        .iter()
        .filter(|record| {
            matches!(
                record.content_ref,
                sim_services::durable::LingquDfsContentRef::Inline(_)
            )
        })
        .count();
    let block_backed_file_versions = snapshot.dfs.files.len() - inline_file_versions;
    let dfs_bytes = snapshot
        .dfs
        .files
        .iter()
        .map(|record| record.bytes)
        .sum::<u64>();
    let block_bytes = snapshot
        .block
        .blocks
        .iter()
        .map(|record| record.bytes.len() as u64)
        .sum::<u64>();

    println!("lingqu_durable");
    println!("  mode: stat");
    println!("  store: {}", store_path.display());
    println!("  kind: {}", snapshot.kind);
    println!("  schema_version: {}", snapshot.schema_version);
    println!("  dfs_file_versions: {}", snapshot.dfs.files.len());
    println!("  dfs_directories: {}", snapshot.dfs.directories.len());
    println!("  inline_file_versions: {inline_file_versions}");
    println!("  block_backed_file_versions: {block_backed_file_versions}");
    println!("  block_versions: {}", snapshot.block.blocks.len());
    println!("  dfs_bytes: {dfs_bytes}");
    println!("  block_bytes: {block_bytes}");
    println!("  next_timestamp_us: {}", snapshot.next_timestamp_us);
    println!("  checksum: {:#x}", snapshot.checksum);
    Ok(())
}

fn run_lingqu_durable_list_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let prefix = optional_cli_arg(args, "--prefix")?.unwrap_or_else(|| "/lingqu/".to_string());
    let include_tombstoned = cli_flag(args, "--include-tombstoned");
    let snapshot = load_lingqu_durable_snapshot(&store_path)?;
    let sim = LingquDurableSim::import_snapshot(snapshot)
        .with_context(|| format!("import durable store {}", store_path.display()))?;
    let mut options = LingquDfsListOptions::new(prefix.clone());
    options.include_tombstoned = include_tombstoned;
    let entries = sim
        .dfs_list(options)
        .context("list durable DFS namespace")?;

    println!("lingqu_durable");
    println!("  mode: list");
    println!("  store: {}", store_path.display());
    println!("  prefix: {prefix}");
    println!("  entries: {}", entries.len());
    for entry in entries {
        println!(
            "  entry path={} version={} state={:?} bytes={} checksum={:#x}",
            entry.path, entry.version, entry.state, entry.bytes, entry.checksum
        );
    }
    Ok(())
}

fn run_lingqu_durable_validate_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let snapshot = load_lingqu_durable_snapshot(&store_path)?;
    let sim = LingquDurableSim::import_snapshot(snapshot)
        .with_context(|| format!("import durable store {}", store_path.display()))?;
    let report = sim.validate_store().context("validate durable store")?;

    println!("lingqu_durable");
    println!("  mode: validate");
    println!("  store: {}", store_path.display());
    println!("  dfs_file_versions: {}", report.dfs_file_versions);
    println!("  dfs_append_records: {}", report.dfs_append_records);
    println!("  block_versions: {}", report.block_versions);
    println!("  missing_block_refs: {}", report.missing_block_refs.len());
    println!("  orphan_blocks: {}", report.orphan_blocks.len());
    println!("  append_log_paths: {}", report.append_log_paths.len());
    println!("  checksum: {:#x}", report.checksum);
    if !report.missing_block_refs.is_empty() || !report.orphan_blocks.is_empty() {
        anyhow::bail!(
            "durable store validation found missing_block_refs={} orphan_blocks={}",
            report.missing_block_refs.len(),
            report.orphan_blocks.len()
        );
    }
    Ok(())
}

fn run_lingqu_durable_append_log_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let log_path = required_cli_arg(args, "--log")?;
    let payload = if let Some(payload_file) = optional_cli_arg(args, "--payload-file")? {
        fs::read(&payload_file).with_context(|| format!("read payload file {payload_file}"))?
    } else {
        required_cli_arg(args, "--payload")?.into_bytes()
    };
    let expected_next_seq = optional_cli_u64(args, "--expected-next-seq")?;
    let snapshot = load_lingqu_durable_snapshot(&store_path)?;
    let mut sim = LingquDurableSim::import_snapshot(snapshot)
        .with_context(|| format!("import durable store {}", store_path.display()))?;
    let record = sim
        .dfs_append_log_append(
            log_path.clone(),
            payload,
            LingquDfsAppendOptions {
                expected_next_seq,
                ..LingquDfsAppendOptions::default()
            },
        )
        .context("append durable DFS log record")?;
    save_lingqu_durable_sim(&store_path, &sim)?;

    println!("lingqu_durable");
    println!("  mode: append-log");
    println!("  store: {}", store_path.display());
    println!("  log: {}", record.path);
    println!("  seq: {}", record.seq);
    println!("  bytes: {}", record.bytes.len());
    println!("  checksum: {:#x}", record.checksum);
    println!("  chain_checksum: {:#x}", record.chain_checksum);
    Ok(())
}

fn run_lingqu_durable_read_log_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let log_path = required_cli_arg(args, "--log")?;
    let start_seq = optional_cli_u64(args, "--start-seq")?.unwrap_or(1);
    let max_records = optional_cli_u64(args, "--max-records")?
        .map(|value| {
            usize::try_from(value).map_err(|_| anyhow::anyhow!("--max-records exceeds usize"))
        })
        .transpose()?;
    let snapshot = load_lingqu_durable_snapshot(&store_path)?;
    let mut sim = LingquDurableSim::import_snapshot(snapshot)
        .with_context(|| format!("import durable store {}", store_path.display()))?;
    let records = sim
        .dfs_append_log_read(&log_path, start_seq, max_records)
        .context("read durable DFS log")?;

    println!("lingqu_durable");
    println!("  mode: read-log");
    println!("  store: {}", store_path.display());
    println!("  log: {log_path}");
    println!("  records: {}", records.len());
    for record in records {
        println!(
            "  record seq={} bytes={} checksum={:#x} chain_checksum={:#x}",
            record.seq,
            record.bytes.len(),
            record.checksum,
            record.chain_checksum
        );
    }
    Ok(())
}

fn run_lingqu_durable_batch_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let manifest_path = PathBuf::from(required_cli_arg(args, "--manifest")?);
    let manifest_bytes = fs::read(&manifest_path)
        .with_context(|| format!("read durable batch manifest {}", manifest_path.display()))?;
    let manifest = serde_json::from_slice::<LingquDurableBatchManifest>(&manifest_bytes)
        .with_context(|| format!("decode durable batch manifest {}", manifest_path.display()))?;
    if manifest.ops.is_empty() {
        anyhow::bail!("durable batch manifest ops must not be empty");
    }
    let mut ops = Vec::with_capacity(manifest.ops.len());
    for op in manifest.ops {
        match op {
            LingquDurableBatchManifestOp::DfsWrite { path, payload } => {
                ops.push(LingquDurableBatchOp::DfsWrite {
                    path,
                    bytes: payload.into_bytes(),
                    options: LingquDfsWriteOptions::default(),
                });
            }
            LingquDurableBatchManifestOp::DfsAppendLog { path, payload } => {
                ops.push(LingquDurableBatchOp::DfsAppendLog {
                    path,
                    bytes: payload.into_bytes(),
                    options: LingquDfsAppendOptions::default(),
                });
            }
            LingquDurableBatchManifestOp::BlockWrite { block, payload } => {
                ops.push(LingquDurableBatchOp::BlockWrite {
                    block,
                    bytes: payload.into_bytes(),
                    options: LingquBlockWriteOptions::default(),
                });
            }
        }
    }
    let snapshot = load_lingqu_durable_snapshot(&store_path)?;
    let mut sim = LingquDurableSim::import_snapshot(snapshot)
        .with_context(|| format!("import durable store {}", store_path.display()))?;
    let outcomes = sim.commit_batch(ops).context("commit durable batch")?;
    save_lingqu_durable_sim(&store_path, &sim)?;

    println!("lingqu_durable");
    println!("  mode: batch");
    println!("  store: {}", store_path.display());
    println!("  manifest: {}", manifest_path.display());
    println!("  outcomes: {}", outcomes.len());
    Ok(())
}

fn run_lingqu_memory_cli() -> anyhow::Result<()> {
    let mut raw_args = env::args_os().skip(2);
    let mode = raw_args
        .next()
        .map(|arg| arg.to_string_lossy().into_owned())
        .unwrap_or_else(|| "validate-service-path".to_string());
    let args = raw_args
        .map(|arg| arg.to_string_lossy().into_owned())
        .collect::<Vec<_>>();
    match mode.as_str() {
        "boundary-lookup" => run_lingqu_memory_boundary_lookup_cli(&args),
        "boundary-request-from-w5-summary" => {
            run_lingqu_memory_boundary_request_from_w5_summary_cli(&args)
        }
        "boundary-lookup-from-observation" => {
            run_lingqu_memory_boundary_lookup_from_observation_cli(&args)
        }
        "build-index" => run_lingqu_memory_build_index_cli(&args),
        "ingest" => run_lingqu_memory_ingest_cli(&args),
        "list-prefetch-plans" => run_lingqu_memory_list_prefetch_plans_cli(&args),
        "list-prefix-cache-reuse" => run_lingqu_memory_list_prefix_cache_reuse_cli(&args),
        "list-query-results" => run_lingqu_memory_list_query_results_cli(&args),
        "list-record-lifecycle" => run_lingqu_memory_list_record_lifecycle_cli(&args),
        "list-shortpath-decisions" => run_lingqu_memory_list_shortpath_decisions_cli(&args),
        "list-shortpath-supports" => run_lingqu_memory_list_shortpath_supports_cli(&args),
        "lookup-prefix-cache" => run_lingqu_memory_lookup_prefix_cache_cli(&args),
        "materialize-engram-state" => run_lingqu_memory_materialize_engram_state_cli(&args),
        "materialize-hot-state" => run_lingqu_memory_materialize_hot_state_cli(&args),
        "plan-prefetch" => run_lingqu_memory_plan_prefetch_cli(&args),
        "publish-w5-engram-state-ref" => run_lingqu_memory_publish_w5_engram_state_ref_cli(&args),
        "query" => run_lingqu_memory_query_cli(&args),
        "record-boundary-observations-from-w5-summary" => {
            run_lingqu_memory_record_boundary_observations_from_w5_summary_cli(&args)
        }
        "register-execution-artifact" => run_lingqu_memory_register_execution_artifact_cli(&args),
        "register-prefix-cache" => run_lingqu_memory_register_prefix_cache_cli(&args),
        "update-record-state" => run_lingqu_memory_update_record_state_cli(&args),
        "validate-service-path" => run_lingqu_memory_validate_service_path(),
        "validate-durable-store" => run_lingqu_memory_validate_durable_store(),
        "validate-flat-query" => run_lingqu_memory_validate_flat_query(),
        "validate-flat-materialize" => run_lingqu_memory_validate_flat_materialize(),
        "validate-w5-engram-object-ref" => run_lingqu_memory_validate_w5_engram_object_ref(),
        _ => anyhow::bail!(
            "unknown lingqu-memory mode `{mode}`; expected ingest, build-index, query, list-query-results, list-record-lifecycle, list-shortpath-supports, list-shortpath-decisions, list-prefetch-plans, list-prefix-cache-reuse, update-record-state, register-execution-artifact, boundary-lookup, boundary-lookup-from-observation, boundary-request-from-w5-summary, record-boundary-observations-from-w5-summary, plan-prefetch, register-prefix-cache, lookup-prefix-cache, materialize-hot-state, materialize-engram-state, publish-w5-engram-state-ref, validate-service-path, validate-durable-store, validate-flat-query, validate-flat-materialize, or validate-w5-engram-object-ref"
        ),
    }
}

#[derive(Debug, Deserialize)]
struct LingquMemoryEmbeddingInput {
    model_version: String,
    dims: u32,
    vectors: Vec<LingquMemoryEmbeddingVectorInput>,
}

#[derive(Debug, Deserialize)]
struct LingquMemoryEmbeddingVectorInput {
    chunk_id: String,
    values: Vec<f32>,
}

#[derive(Debug, Deserialize)]
struct LingquMemoryQueryEmbeddingInput {
    model_version: String,
    values: Vec<f32>,
}

#[derive(Debug, Deserialize)]
struct LingquMemoryGateWeightInput {
    values: Vec<f32>,
}

#[derive(Debug, Deserialize)]
struct LingquDurableBatchManifest {
    ops: Vec<LingquDurableBatchManifestOp>,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
enum LingquDurableBatchManifestOp {
    DfsWrite { path: String, payload: String },
    DfsAppendLog { path: String, payload: String },
    BlockWrite { block: String, payload: String },
}

#[derive(Debug, Default, Deserialize, Serialize)]
struct LingquMemoryExecutionArtifactRegistry {
    artifacts: Vec<ExecutionArtifactObject>,
}

#[derive(Debug, Default, Deserialize, Serialize)]
struct LingquMemoryPrefixCacheRegistry {
    artifacts: Vec<PrefixCacheArtifact>,
}

fn run_lingqu_memory_register_execution_artifact_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let artifact_path = PathBuf::from(required_cli_arg(args, "--artifact")?);
    let artifact_bytes = fs::read(&artifact_path)
        .with_context(|| format!("read execution artifact {}", artifact_path.display()))?;
    let artifact = serde_json::from_slice::<ExecutionArtifactObject>(&artifact_bytes)
        .with_context(|| format!("decode execution artifact {}", artifact_path.display()))?;

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let mut memory_service = LingquMemoryService::new();
    let mut registry =
        rebuild_lingqu_memory_execution_registry_artifacts(&mut memory_service, &mut durable_store)
            .context("rebuild execution artifact registry")?;
    validate_lingqu_execution_artifact_payloads(&mut durable_store, &artifact, "register")
        .context("validate execution artifact payloads")?;
    memory_service
        .register_execution_artifact(artifact.clone())
        .context("register execution artifact")?;

    registry
        .artifacts
        .retain(|entry| entry.artifact_id != artifact.artifact_id);
    registry.artifacts.push(artifact.clone());
    registry
        .artifacts
        .sort_by(|left, right| left.artifact_id.cmp(&right.artifact_id));
    memory_service
        .persist_execution_artifacts_to_dfs(&mut durable_store)
        .context("persist execution artifact DFS manifest")?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;

    println!("lingqu_memory_service");
    println!("  mode: register-execution-artifact");
    println!("  store_path: {}", store_path.display());
    println!(
        "  manifest_path: {}",
        sim_memory::LINGQU_EXECUTION_ARTIFACT_MANIFEST_PATH
    );
    println!("  artifact_path: {}", artifact_path.display());
    println!("  artifact: {}", artifact.artifact_id);
    println!("  kind: {:?}", artifact.kind);
    println!(
        "  boundary: step={} node={} layer={}..{} position={}",
        artifact.producer_boundary.step_index,
        artifact.producer_boundary.node_index,
        artifact.producer_boundary.layer_start,
        artifact.producer_boundary.layer_end,
        artifact.producer_boundary.position
    );
    println!(
        "  target_layer_range: {}..{}",
        artifact.target_layer_start, artifact.target_layer_end
    );
    println!("  confidence_milli: {}", artifact.confidence_milli);
    println!("  registry_artifacts: {}", registry.artifacts.len());
    Ok(())
}

fn run_lingqu_memory_boundary_request_from_w5_summary_cli(args: &[String]) -> anyhow::Result<()> {
    let summary_path = PathBuf::from(required_cli_arg(args, "--summary")?);
    let output_path = PathBuf::from(required_cli_arg(args, "--output")?);
    let step = required_cli_u64(args, "--step")?;
    let node = required_cli_arg(args, "--node")?;
    let position = required_cli_u64(args, "--position")?;
    let model = sim_memory::InferenceModelBinding {
        model_id: required_cli_arg(args, "--model-id")?,
        model_key: required_cli_arg(args, "--model-key")?,
        tokenizer_hash: required_cli_u64_auto(args, "--tokenizer-hash")?,
        profile_hash: required_cli_u64_auto(args, "--profile-hash")?,
    };
    let min_confidence_milli = optional_cli_u64(args, "--min-confidence-milli")?.unwrap_or(900);
    if min_confidence_milli > 1000 {
        anyhow::bail!("--min-confidence-milli must be in [0, 1000]");
    }
    let allowed_actions = parse_shortpath_actions(
        optional_cli_arg(args, "--allowed-actions")?
            .as_deref()
            .unwrap_or("jump-to-terminal"),
    )?;
    let created_at_us = optional_cli_u64(args, "--created-at-us")?.unwrap_or(1);
    let request_id = optional_cli_arg(args, "--request-id")?.unwrap_or_else(|| {
        format!(
            "boundary/{}/step{}/{}/position{}",
            model.model_key, step, node, position
        )
    });
    let engram_state_id = optional_cli_arg(args, "--engram-state-id")?;

    let summary = fs::read_to_string(&summary_path)
        .with_context(|| format!("read W5 summary {}", summary_path.display()))?;
    let observation = find_w5_boundary_observation(&summary, step, &node)
        .with_context(|| format!("find boundary observation step={step} node={node}"))?;
    let layer_start = required_summary_u32(&observation, "layer_start")?;
    let layer_end = required_summary_u32(&observation, "layer_end")?;
    let node_index = parse_node_index(&node)?;
    let target = required_summary_field(&observation, "target")?;
    let next_node_index = Some(parse_node_index(target)?);
    let hidden_key = required_summary_field(&observation, "hidden_key")?.to_string();
    let hidden_bytes = required_summary_u64(&observation, "hidden_bytes")?;
    let hidden_checksum = required_summary_u64_auto(&observation, "hidden_checksum")?;
    let hidden_version = required_summary_u64(&observation, "hidden_version")?;

    let request = BoundaryLookupRequest {
        request_id,
        model,
        boundary: sim_memory::RangeBoundary {
            phase: sim_memory::RangeBoundaryPhase::RangeExit,
            step_index: step,
            node_index,
            layer_start,
            layer_end,
            next_node_index,
            position,
        },
        hidden_state: sim_memory::HotTensorObjectRef {
            object_key: hidden_key.clone(),
            version: hidden_version,
            backend: sim_memory::HotObjectBackend::ObmmShmem,
            storage_ref: format!("obmm://{hidden_key}"),
            segment: None,
            offset: 0,
            bytes: hidden_bytes,
            checksum: hidden_checksum,
            dtype: sim_core::TensorDType::Opaque,
            shape: vec![hidden_bytes],
        },
        engram_state_id,
        min_confidence_milli: min_confidence_milli as u32,
        allowed_actions,
        created_at_us,
    };
    request
        .validate()
        .map_err(|err| anyhow::anyhow!("validate boundary lookup request: {err}"))?;
    if let Some(parent) = output_path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create boundary request dir {}", parent.display()))?;
    }
    fs::write(
        &output_path,
        serde_json::to_vec_pretty(&request).context("encode boundary lookup request")?,
    )
    .with_context(|| format!("write boundary lookup request {}", output_path.display()))?;
    println!("lingqu_memory_service");
    println!("  mode: boundary-request-from-w5-summary");
    println!("  summary: {}", summary_path.display());
    println!("  output: {}", output_path.display());
    println!("  request: {}", request.request_id);
    println!("  step: {}", request.boundary.step_index);
    println!("  node: {}", request.boundary.node_index);
    println!(
        "  layers: [{},{})",
        request.boundary.layer_start, request.boundary.layer_end
    );
    println!("  position: {}", request.boundary.position);
    println!("  hidden_key: {}", request.hidden_state.object_key);
    println!("  hidden_checksum: {:#x}", request.hidden_state.checksum);
    Ok(())
}

fn run_lingqu_memory_record_boundary_observations_from_w5_summary_cli(
    args: &[String],
) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let summary_path = PathBuf::from(required_cli_arg(args, "--summary")?);
    let step = required_cli_u64(args, "--step")?;
    let position = required_cli_u64(args, "--position")?;
    let created_at_us = optional_cli_u64(args, "--created-at-us")?.unwrap_or(1);
    let model = sim_memory::InferenceModelBinding {
        model_id: required_cli_arg(args, "--model-id")?,
        model_key: required_cli_arg(args, "--model-key")?,
        tokenizer_hash: required_cli_u64_auto(args, "--tokenizer-hash")?,
        profile_hash: required_cli_u64_auto(args, "--profile-hash")?,
    };
    model
        .validate()
        .map_err(|err| anyhow::anyhow!("validate model binding: {err}"))?;

    let summary = fs::read_to_string(&summary_path)
        .with_context(|| format!("read W5 summary {}", summary_path.display()))?;
    let run_id = optional_cli_arg(args, "--run-id")?
        .or_else(|| derive_w5_run_id_from_summary(&summary))
        .ok_or_else(|| anyhow::anyhow!("missing --run-id and summary run_dir is unavailable"))?;
    let observations = w5_boundary_observations_from_summary(
        &summary,
        &run_id,
        model,
        step,
        position,
        created_at_us,
    )?;
    if observations.is_empty() {
        anyhow::bail!("summary has no boundary observations for step={step}");
    }

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let mut memory_service = LingquMemoryService::new();
    for observation in observations {
        memory_service
            .register_boundary_observation(observation)
            .context("register boundary observation")?;
    }
    memory_service
        .persist_boundary_observations_to_dfs(&mut durable_store)
        .context("persist boundary observation DFS audit")?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;
    let persisted = durable_store
        .load_boundary_observation_manifest()
        .context("reload boundary observation DFS audit")?;

    println!("lingqu_memory_service");
    println!("  mode: record-boundary-observations-from-w5-summary");
    println!("  store_path: {}", store_path.display());
    println!("  summary: {}", summary_path.display());
    println!(
        "  audit_log: {}",
        sim_memory::LINGQU_BOUNDARY_OBSERVATION_AUDIT_LOG_PATH
    );
    println!("  run_id: {run_id}");
    println!("  step: {step}");
    println!("  observations: {}", persisted.len());
    Ok(())
}

fn run_lingqu_memory_boundary_lookup_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let request_path = PathBuf::from(required_cli_arg(args, "--request")?);
    let response_path = PathBuf::from(required_cli_arg(args, "--response")?);
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);

    let (response, planner_decision) =
        run_w5_memory_boundary_lookup(&store_path, &request_path, now_us)?;
    let response_bytes =
        serde_json::to_vec_pretty(&response).context("encode boundary lookup response")?;
    if let Some(parent) = response_path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create boundary lookup response dir {}", parent.display()))?;
    }
    fs::write(&response_path, response_bytes)
        .with_context(|| format!("write boundary lookup response {}", response_path.display()))?;
    println!("lingqu_memory_service");
    println!("  mode: boundary-lookup");
    println!("  store_path: {}", store_path.display());
    println!(
        "  manifest_path: {}",
        sim_memory::LINGQU_EXECUTION_ARTIFACT_MANIFEST_PATH
    );
    println!("  request_path: {}", request_path.display());
    println!("  response_path: {}", response_path.display());
    println!("  request: {}", response.request_id);
    println!("  support_id: {}", response.support.support_id);
    println!(
        "  supported_action: {:?}",
        response.support.supported_action
    );
    println!(
        "  supported_artifact: {}",
        response.support.artifact_id.as_deref().unwrap_or("")
    );
    println!(
        "  support_confidence_milli: {}",
        response.support.confidence_milli
    );
    println!(
        "  support_verify_required: {}",
        response.support.verify_required
    );
    println!(
        "  support_proof_checksum: {:#x}",
        response.support.proof_checksum
    );
    println!("  planner: w5_runtime_planner");
    println!("  planner_decision_id: {}", planner_decision.decision_id);
    println!("  planner_action: {:?}", planner_decision.action);
    println!(
        "  planner_proof_checksum: {:#x}",
        planner_decision.proof_checksum
    );
    Ok(())
}

fn run_lingqu_memory_boundary_lookup_from_observation_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let observation_id = required_cli_arg(args, "--observation-id")?;
    let response_path = PathBuf::from(required_cli_arg(args, "--response")?);
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);
    let min_confidence_milli = optional_cli_u64(args, "--min-confidence-milli")?.unwrap_or(900);
    if min_confidence_milli > 1000 {
        anyhow::bail!("--min-confidence-milli must be in [0, 1000]");
    }
    let allowed_actions = parse_shortpath_actions(
        optional_cli_arg(args, "--allowed-actions")?
            .as_deref()
            .unwrap_or("jump-to-terminal"),
    )?;
    let engram_state_id = optional_cli_arg(args, "--engram-state-id")?;

    let (response, planner_decision) = run_w5_memory_boundary_lookup_from_observation(
        &store_path,
        &observation_id,
        engram_state_id,
        min_confidence_milli as u32,
        allowed_actions,
        now_us,
    )?;
    let response_bytes =
        serde_json::to_vec_pretty(&response).context("encode boundary lookup response")?;
    if let Some(parent) = response_path.parent() {
        fs::create_dir_all(parent).with_context(|| {
            format!(
                "create boundary lookup observation response dir {}",
                parent.display()
            )
        })?;
    }
    fs::write(&response_path, response_bytes).with_context(|| {
        format!(
            "write boundary lookup observation response {}",
            response_path.display()
        )
    })?;

    println!("lingqu_memory_service");
    println!("  mode: boundary-lookup-from-observation");
    println!("  store_path: {}", store_path.display());
    println!("  observation_id: {observation_id}");
    println!("  response_path: {}", response_path.display());
    println!("  request: {}", response.request_id);
    println!("  support_id: {}", response.support.support_id);
    println!(
        "  supported_action: {:?}",
        response.support.supported_action
    );
    println!("  planner_decision_id: {}", planner_decision.decision_id);
    println!("  planner_action: {:?}", planner_decision.action);
    Ok(())
}

fn run_w5_memory_boundary_lookup(
    store_path: &Path,
    request_path: &Path,
    now_us: u64,
) -> anyhow::Result<(
    sim_memory::BoundaryLookupResponse,
    sim_memory::ShortpathDecisionRecord,
)> {
    let request_bytes = fs::read(request_path)
        .with_context(|| format!("read boundary lookup request {}", request_path.display()))?;
    let request = serde_json::from_slice::<BoundaryLookupRequest>(&request_bytes)
        .with_context(|| format!("decode boundary lookup request {}", request_path.display()))?;
    run_w5_memory_boundary_lookup_request(store_path, request, now_us)
}

fn run_w5_memory_boundary_lookup_from_observation(
    store_path: &Path,
    observation_id: &str,
    engram_state_id: Option<String>,
    min_confidence_milli: u32,
    allowed_actions: Vec<sim_memory::ShortpathAction>,
    now_us: u64,
) -> anyhow::Result<(
    sim_memory::BoundaryLookupResponse,
    sim_memory::ShortpathDecisionRecord,
)> {
    let mut durable_store = load_lingqu_memory_durable_store(store_path)?;
    let observations = durable_store
        .load_boundary_observation_manifest()
        .context("load boundary observation audit")?;
    let observation = observations
        .into_iter()
        .find(|candidate| candidate.observation_id == observation_id)
        .ok_or_else(|| anyhow::anyhow!("boundary observation not found: {observation_id}"))?;
    let request_id = format!("boundary-lookup/{observation_id}");
    let request_created_at_us = if now_us == 0 {
        observation.created_at_us
    } else {
        now_us
    };
    let request = observation
        .to_lookup_request(
            request_id,
            engram_state_id,
            min_confidence_milli,
            allowed_actions,
            request_created_at_us,
        )
        .map_err(|err| anyhow::anyhow!("build lookup request from observation: {err}"))?;
    run_w5_memory_boundary_lookup_request(store_path, request, now_us)
}

fn run_w5_memory_boundary_lookup_request(
    store_path: &Path,
    request: BoundaryLookupRequest,
    now_us: u64,
) -> anyhow::Result<(
    sim_memory::BoundaryLookupResponse,
    sim_memory::ShortpathDecisionRecord,
)> {
    let mut durable_store = load_lingqu_memory_durable_store(store_path)?;
    let effective_now_us = if now_us == 0 {
        request.created_at_us
    } else {
        now_us
    };

    let mut memory_service = LingquMemoryService::new();
    load_required_lingqu_memory_execution_registry_artifacts(
        &mut memory_service,
        &mut durable_store,
    )
    .context("load execution artifact manifest")?;
    rebuild_lingqu_memory_shortpath_supports(&mut memory_service, &mut durable_store)
        .context("rebuild shortpath support audit")?;
    let response = memory_service
        .boundary_lookup(request, effective_now_us)
        .context("run boundary lookup")?;
    let planner_decision = w5_plan_shortpath_decision_from_memory_support(&response)
        .context("plan W5 shortpath execution decision from Memory Service support")?;
    memory_service
        .persist_shortpath_supports_to_dfs(&mut durable_store)
        .context("persist shortpath support DFS audit")?;
    durable_store
        .persist_shortpath_decision_manifest(vec![planner_decision.clone()])
        .context("persist W5 planner shortpath decision DFS audit")?;
    save_lingqu_memory_durable_store(store_path, &durable_store)?;
    Ok((response, planner_decision))
}

fn run_lingqu_memory_plan_prefetch_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let request_path = PathBuf::from(required_cli_arg(args, "--request")?);
    let plan_path = PathBuf::from(required_cli_arg(args, "--plan")?);
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let request_bytes = fs::read(&request_path)
        .with_context(|| format!("read prefetch plan request {}", request_path.display()))?;
    let request = serde_json::from_slice::<PrefetchPlanRequest>(&request_bytes)
        .with_context(|| format!("decode prefetch plan request {}", request_path.display()))?;

    let mut memory_service = LingquMemoryService::new();
    load_required_lingqu_memory_execution_registry_artifacts(
        &mut memory_service,
        &mut durable_store,
    )
    .context("load execution artifact manifest")?;
    rebuild_lingqu_memory_prefetch_plans(&mut memory_service, &mut durable_store)
        .context("rebuild prefetch plan audit")?;
    let plan = memory_service
        .plan_prefetch(request, now_us)
        .context("plan prefetch")?;
    let plan_bytes = serde_json::to_vec_pretty(&plan).context("encode prefetch plan")?;
    if let Some(parent) = plan_path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create prefetch plan dir {}", parent.display()))?;
    }
    fs::write(&plan_path, plan_bytes)
        .with_context(|| format!("write prefetch plan {}", plan_path.display()))?;
    memory_service
        .persist_prefetch_plans_to_dfs(&mut durable_store)
        .context("persist prefetch plan DFS audit")?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;

    println!("lingqu_memory_service");
    println!("  mode: plan-prefetch");
    println!("  store_path: {}", store_path.display());
    println!(
        "  manifest_path: {}",
        sim_memory::LINGQU_EXECUTION_ARTIFACT_MANIFEST_PATH
    );
    println!("  request_path: {}", request_path.display());
    println!("  plan_path: {}", plan_path.display());
    println!("  plan: {}", plan.plan_id);
    println!("  scope: {:?}", plan.scope);
    println!("  target_step_index: {}", plan.target_step_index);
    println!("  target_position: {}", plan.target_position);
    println!(
        "  planned_artifacts: {}",
        plan.planned_artifact_ids.join(",")
    );
    println!("  checksum: {:#x}", plan.checksum);
    Ok(())
}

fn run_lingqu_memory_register_prefix_cache_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let artifact_path = PathBuf::from(required_cli_arg(args, "--artifact")?);
    let artifact_bytes = fs::read(&artifact_path)
        .with_context(|| format!("read prefix cache artifact {}", artifact_path.display()))?;
    let artifact = serde_json::from_slice::<PrefixCacheArtifact>(&artifact_bytes)
        .with_context(|| format!("decode prefix cache artifact {}", artifact_path.display()))?;

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let mut memory_service = LingquMemoryService::new();
    let mut registry = rebuild_lingqu_memory_prefix_cache_registry_artifacts(
        &mut memory_service,
        &mut durable_store,
    )
    .context("rebuild prefix cache registry")?;
    validate_lingqu_prefix_cache_payloads(&mut durable_store, &artifact, "register")
        .context("validate prefix cache artifact payloads")?;
    memory_service
        .register_prefix_cache_artifact(artifact.clone())
        .context("register prefix cache artifact")?;

    registry
        .artifacts
        .retain(|entry| entry.artifact_id != artifact.artifact_id);
    registry.artifacts.push(artifact.clone());
    registry
        .artifacts
        .sort_by(|left, right| left.artifact_id.cmp(&right.artifact_id));
    memory_service
        .persist_prefix_cache_artifacts_to_dfs(&mut durable_store)
        .context("persist prefix cache DFS manifest")?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;

    println!("lingqu_memory_service");
    println!("  mode: register-prefix-cache");
    println!("  store_path: {}", store_path.display());
    println!(
        "  manifest_path: {}",
        sim_memory::LINGQU_PREFIX_CACHE_MANIFEST_PATH
    );
    println!("  artifact_path: {}", artifact_path.display());
    println!("  artifact: {}", artifact.artifact_id);
    println!("  prefix_tokens: {}", artifact.key.prefix_token_count);
    println!(
        "  layer_range: {}..{}",
        artifact.key.layer_start, artifact.key.layer_end
    );
    println!("  confidence_milli: {}", artifact.confidence_milli);
    println!("  registry_artifacts: {}", registry.artifacts.len());
    Ok(())
}

fn run_lingqu_memory_lookup_prefix_cache_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let request_path = PathBuf::from(required_cli_arg(args, "--request")?);
    let response_path = PathBuf::from(required_cli_arg(args, "--response")?);
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let mut memory_service = LingquMemoryService::new();
    let _registry = load_required_lingqu_memory_prefix_cache_registry_artifacts(
        &mut memory_service,
        &mut durable_store,
    )
    .context("load prefix cache manifest")?;
    rebuild_lingqu_memory_prefix_cache_reuse_plans(&mut memory_service, &mut durable_store)
        .context("rebuild prefix cache reuse audit")?;
    let request_bytes = fs::read(&request_path).with_context(|| {
        format!(
            "read prefix cache lookup request {}",
            request_path.display()
        )
    })?;
    let request =
        serde_json::from_slice::<PrefixCacheLookupRequest>(&request_bytes).with_context(|| {
            format!(
                "decode prefix cache lookup request {}",
                request_path.display()
            )
        })?;

    let response = memory_service
        .lookup_prefix_cache(request, now_us)
        .context("lookup prefix cache")?;
    let response_bytes =
        serde_json::to_vec_pretty(&response).context("encode prefix cache lookup response")?;
    if let Some(parent) = response_path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create prefix cache response dir {}", parent.display()))?;
    }
    fs::write(&response_path, response_bytes).with_context(|| {
        format!(
            "write prefix cache lookup response {}",
            response_path.display()
        )
    })?;
    memory_service
        .persist_prefix_cache_reuse_plans_to_dfs(&mut durable_store)
        .context("persist prefix cache reuse DFS audit")?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;

    println!("lingqu_memory_service");
    println!("  mode: lookup-prefix-cache");
    println!("  store_path: {}", store_path.display());
    println!(
        "  manifest_path: {}",
        sim_memory::LINGQU_PREFIX_CACHE_MANIFEST_PATH
    );
    println!("  request_path: {}", request_path.display());
    println!("  response_path: {}", response_path.display());
    println!("  request: {}", response.request_id);
    println!("  action: {:?}", response.reuse_plan.action);
    println!(
        "  artifact: {}",
        response.reuse_plan.artifact_id.as_deref().unwrap_or("")
    );
    println!(
        "  matched_prefix_tokens: {}",
        response.reuse_plan.matched_prefix_token_count
    );
    println!("  verify_required: {}", response.reuse_plan.verify_required);
    println!(
        "  proof_checksum: {:#x}",
        response.reuse_plan.proof_checksum
    );
    Ok(())
}

fn run_lingqu_memory_query_cli(args: &[String]) -> anyhow::Result<()> {
    let catalog_path = PathBuf::from(required_cli_arg(args, "--catalog")?);
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let query_embedding_path = PathBuf::from(required_cli_arg(args, "--query-embedding-json")?);
    let query_id = required_cli_arg(args, "--query-id")?;
    let top_k = required_cli_u32(args, "--top-k")?;
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);

    if top_k == 0 {
        anyhow::bail!("--top-k must be non-zero");
    }
    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let snapshot =
        load_required_lingqu_memory_catalog_snapshot(args, &catalog_path, &mut durable_store)?;
    let query_embedding_bytes = fs::read(&query_embedding_path).with_context(|| {
        format!(
            "read query embedding json {}",
            query_embedding_path.display()
        )
    })?;
    let query_embedding =
        serde_json::from_slice::<LingquMemoryQueryEmbeddingInput>(&query_embedding_bytes)
            .with_context(|| {
                format!(
                    "decode query embedding json {}",
                    query_embedding_path.display()
                )
            })?;
    validate_lingqu_memory_query_embedding_input(&query_embedding)?;

    let query_payload = cli_f32_vec_to_le_bytes(&query_embedding.values);
    let query_block = optional_cli_arg(args, "--query-block")?
        .unwrap_or_else(|| format!("block/memory/query/{}", cli_path_id(&query_id)));
    let query_ref = durable_store
        .write_block_payload(query_block, query_payload)
        .context("write query embedding payload to Lingqu Block store")?;

    let mut memory_service = LingquMemoryService::new();
    memory_service
        .import_catalog_snapshot(snapshot.clone())
        .context("import catalog snapshot")?;
    let result = memory_service
        .query_memory_flat(
            &mut durable_store,
            MemoryQuery {
                query_id: query_id.clone(),
                corpus_ids: vec![snapshot.catalog.catalog_id.clone()],
                scope_filter: vec![MemoryScope::Project],
                visibility_filter: vec![MemoryVisibility::ProjectShared],
                min_trust: MemoryTrustLevel::UserConfirmed,
                min_confidence: 0.0,
                embedding_model_version: query_embedding.model_version.clone(),
                top_k: usize::try_from(top_k)
                    .map_err(|_| anyhow::anyhow!("--top-k exceeds usize"))?,
                query_embedding_ref: Some(query_ref.clone()),
            },
            now_us,
        )
        .context("query Lingqu memory flat index")?;
    let query_result_path = durable_store
        .persist_query_result(&result)
        .context("persist query result")?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;

    println!("lingqu_memory_service");
    println!("  mode: query");
    println!("  catalog: {}", snapshot.catalog.catalog_id);
    println!("  catalog_path: {}", catalog_path.display());
    println!("  store_path: {}", store_path.display());
    println!("  query_id: {query_id}");
    println!("  query_result: {}", result.result_id);
    println!("  query_result_manifest: {}", query_result_path.path);
    println!("  query_result_version: {}", result.version);
    println!("  query_result_checksum: {:#x}", result.checksum);
    println!(
        "  embedding_model_version: {}",
        query_embedding.model_version
    );
    println!("  query_embedding_block: {}", query_ref.block.0);
    println!("  query_embedding_bytes: {}", query_ref.bytes);
    println!("  query_embedding_checksum: {:#x}", query_ref.checksum);
    println!("  matches: {}", result.matches.len());
    println!(
        "  selected_records: {}",
        result.selected_record_ids.join(",")
    );
    println!("  selected_chunks: {}", result.selected_chunk_ids.join(","));
    if let Some(top) = result.matches.first() {
        println!("  top_record: {}", top.record_id);
        println!("  top_chunk: {}", top.chunk_id);
        println!("  top_score: {:.6}", top.score);
    }
    Ok(())
}

fn run_lingqu_memory_list_query_results_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let result_id_filter = optional_cli_arg(args, "--result-id")?;

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let results = durable_store
        .load_query_result_audit_manifest()
        .context("load query result audit")?;
    let filtered_results = results
        .iter()
        .filter(|result| {
            result_id_filter
                .as_ref()
                .map(|result_id| result.result_id == *result_id)
                .unwrap_or(true)
        })
        .collect::<Vec<_>>();
    if let Some(result_id) = result_id_filter.as_ref() {
        if filtered_results.is_empty() {
            anyhow::bail!("query result `{result_id}` not found in durable audit log");
        }
    }

    println!("lingqu_memory_service");
    println!("  mode: list-query-results");
    println!("  store_path: {}", store_path.display());
    println!(
        "  audit_path: {}",
        sim_memory::LINGQU_QUERY_RESULT_AUDIT_LOG_PATH
    );
    println!("  results: {}", filtered_results.len());
    for result in filtered_results {
        println!(
            "  result id={} query_id={} version={} checksum={:#x} matches={} selected_records={} selected_chunks={} created_at_us={}",
            result.result_id,
            result.query_id,
            result.version,
            result.checksum,
            result.matches.len(),
            result.selected_record_ids.join(","),
            result.selected_chunk_ids.join(","),
            result.created_at_us
        );
    }
    Ok(())
}

fn run_lingqu_memory_list_record_lifecycle_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let record_id_filter = optional_cli_arg(args, "--record-id")?;

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let events = durable_store
        .load_record_lifecycle_event_manifest()
        .context("load record lifecycle audit")?;
    let filtered_events = events
        .iter()
        .filter(|event| {
            record_id_filter
                .as_ref()
                .map(|record_id| event.record_id == *record_id)
                .unwrap_or(true)
        })
        .collect::<Vec<_>>();
    if let Some(record_id) = record_id_filter.as_ref() {
        if filtered_events.is_empty() {
            anyhow::bail!("record `{record_id}` not found in lifecycle audit log");
        }
    }

    println!("lingqu_memory_service");
    println!("  mode: list-record-lifecycle");
    println!("  store_path: {}", store_path.display());
    println!(
        "  audit_path: {}",
        sim_memory::LINGQU_RECORD_LIFECYCLE_AUDIT_LOG_PATH
    );
    println!("  events: {}", filtered_events.len());
    for event in filtered_events {
        println!(
            "  event id={} catalog_id={} record_id={} previous_state={:?} new_state={:?} previous_record_version={} new_record_version={} previous_catalog_version={} new_catalog_version={} actor={} reason={} checksum={:#x} created_at_us={}",
            event.event_id,
            event.catalog_id,
            event.record_id,
            event.previous_state,
            event.new_state,
            event.previous_record_version,
            event.new_record_version,
            event.previous_catalog_version,
            event.new_catalog_version,
            event.actor,
            event.reason,
            event.checksum,
            event.created_at_us
        );
    }
    Ok(())
}

fn run_lingqu_memory_list_shortpath_decisions_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let decision_id_filter = optional_cli_arg(args, "--decision-id")?;

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let decisions = durable_store
        .load_shortpath_decision_manifest()
        .context("load shortpath decision audit")?;
    let filtered_decisions = decisions
        .iter()
        .filter(|decision| {
            decision_id_filter
                .as_ref()
                .map(|decision_id| decision.decision_id == *decision_id)
                .unwrap_or(true)
        })
        .collect::<Vec<_>>();
    if let Some(decision_id) = decision_id_filter.as_ref() {
        if filtered_decisions.is_empty() {
            anyhow::bail!("shortpath decision `{decision_id}` not found in durable audit log");
        }
    }

    println!("w5_runtime_planner");
    println!("  mode: list-shortpath-decisions");
    println!("  store_path: {}", store_path.display());
    println!(
        "  audit_path: {}",
        sim_memory::LINGQU_SHORTPATH_DECISION_AUDIT_LOG_PATH
    );
    println!("  decisions: {}", filtered_decisions.len());
    for decision in filtered_decisions {
        println!(
            "  decision id={} request_id={} support_id={} action={:?} artifact={} confidence_milli={} verify_required={} proof_checksum={:#x} created_at_us={}",
            decision.decision_id,
            decision.request_id,
            decision.support_id.as_deref().unwrap_or(""),
            decision.action,
            decision.artifact_id.as_deref().unwrap_or(""),
            decision.confidence_milli,
            decision.verify_required,
            decision.proof_checksum,
            decision.created_at_us
        );
    }
    Ok(())
}

fn run_lingqu_memory_list_shortpath_supports_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let support_id_filter = optional_cli_arg(args, "--support-id")?;

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let supports = durable_store
        .load_shortpath_support_manifest()
        .context("load shortpath support audit")?;
    let filtered_supports = supports
        .iter()
        .filter(|support| {
            support_id_filter
                .as_ref()
                .map(|support_id| support.support_id == *support_id)
                .unwrap_or(true)
        })
        .collect::<Vec<_>>();
    if let Some(support_id) = support_id_filter.as_ref() {
        if filtered_supports.is_empty() {
            anyhow::bail!("shortpath support `{support_id}` not found in durable audit log");
        }
    }

    println!("lingqu_memory_service");
    println!("  mode: list-shortpath-supports");
    println!("  store_path: {}", store_path.display());
    println!(
        "  audit_path: {}",
        sim_memory::LINGQU_SHORTPATH_SUPPORT_AUDIT_LOG_PATH
    );
    println!("  supports: {}", filtered_supports.len());
    for support in filtered_supports {
        println!(
            "  support id={} request_id={} supported_action={:?} artifact={} confidence_milli={} verify_required={} proof_checksum={:#x} created_at_us={}",
            support.support_id,
            support.request_id,
            support.supported_action,
            support.artifact_id.as_deref().unwrap_or(""),
            support.confidence_milli,
            support.verify_required,
            support.proof_checksum,
            support.created_at_us
        );
    }
    Ok(())
}

fn run_lingqu_memory_list_prefetch_plans_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let plan_id_filter = optional_cli_arg(args, "--plan-id")?;

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let plans = durable_store
        .load_prefetch_plan_manifest()
        .context("load prefetch plan audit")?;
    let filtered_plans = plans
        .iter()
        .filter(|plan| {
            plan_id_filter
                .as_ref()
                .map(|plan_id| plan.plan_id == *plan_id)
                .unwrap_or(true)
        })
        .collect::<Vec<_>>();
    if let Some(plan_id) = plan_id_filter.as_ref() {
        if filtered_plans.is_empty() {
            anyhow::bail!("prefetch plan `{plan_id}` not found in durable audit log");
        }
    }

    println!("lingqu_memory_service");
    println!("  mode: list-prefetch-plans");
    println!("  store_path: {}", store_path.display());
    println!(
        "  audit_path: {}",
        sim_memory::LINGQU_PREFETCH_PLAN_AUDIT_LOG_PATH
    );
    println!("  plans: {}", filtered_plans.len());
    for plan in filtered_plans {
        println!(
            "  plan id={} request_id={} scope={:?} target_step_index={} target_position={} state={:?} checksum={:#x} planned_artifacts={} created_at_us={}",
            plan.plan_id,
            plan.request_id,
            plan.scope,
            plan.target_step_index,
            plan.target_position,
            plan.state,
            plan.checksum,
            plan.planned_artifact_ids.join(","),
            plan.created_at_us
        );
    }
    Ok(())
}

fn run_lingqu_memory_list_prefix_cache_reuse_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let plan_id_filter = optional_cli_arg(args, "--plan-id")?;

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let plans = durable_store
        .load_prefix_cache_reuse_plan_manifest()
        .context("load prefix cache reuse audit")?;
    let filtered_plans = plans
        .iter()
        .filter(|plan| {
            plan_id_filter
                .as_ref()
                .map(|plan_id| plan.plan_id == *plan_id)
                .unwrap_or(true)
        })
        .collect::<Vec<_>>();
    if let Some(plan_id) = plan_id_filter.as_ref() {
        if filtered_plans.is_empty() {
            anyhow::bail!("prefix cache reuse plan `{plan_id}` not found in durable audit log");
        }
    }

    println!("lingqu_memory_service");
    println!("  mode: list-prefix-cache-reuse");
    println!("  store_path: {}", store_path.display());
    println!(
        "  audit_path: {}",
        sim_memory::LINGQU_PREFIX_CACHE_REUSE_AUDIT_LOG_PATH
    );
    println!("  plans: {}", filtered_plans.len());
    for plan in filtered_plans {
        println!(
            "  plan id={} request_id={} action={:?} artifact={} matched_prefix_tokens={} confidence_milli={} verify_required={} proof_checksum={:#x} created_at_us={}",
            plan.plan_id,
            plan.request_id,
            plan.action,
            plan.artifact_id.as_deref().unwrap_or(""),
            plan.matched_prefix_token_count,
            plan.confidence_milli,
            plan.verify_required,
            plan.proof_checksum,
            plan.created_at_us
        );
    }
    Ok(())
}

fn validate_lingqu_memory_query_embedding_input(
    input: &LingquMemoryQueryEmbeddingInput,
) -> anyhow::Result<()> {
    if input.model_version.trim().is_empty() {
        anyhow::bail!("query embedding json model_version must not be empty");
    }
    if input.values.is_empty() {
        anyhow::bail!("query embedding json values must not be empty");
    }
    Ok(())
}

fn run_lingqu_memory_materialize_hot_state_cli(args: &[String]) -> anyhow::Result<()> {
    let catalog_path = PathBuf::from(required_cli_arg(args, "--catalog")?);
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let object_store_path = PathBuf::from(required_cli_arg(args, "--object-store")?);
    let query_result_manifest = required_cli_arg(args, "--query-result-manifest")?;
    let state_id = required_cli_arg(args, "--state-id")?;
    let hot_state_path = PathBuf::from(required_cli_arg(args, "--hot-state")?);
    let owner_entity = optional_cli_u64(args, "--owner-entity")?.unwrap_or(0);
    let producer_entity = optional_cli_u64(args, "--producer-entity")?.unwrap_or(0);
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let snapshot =
        load_required_lingqu_memory_catalog_snapshot(args, &catalog_path, &mut durable_store)?;
    let query_result_path = LingquDfsPath::new(query_result_manifest);
    let query_result = durable_store
        .load_query_result(&query_result_path)
        .context("load query result manifest")?;

    let mut memory_service = LingquMemoryService::new();
    memory_service
        .import_catalog_snapshot(snapshot.clone())
        .context("import catalog snapshot")?;
    memory_service
        .register_query_result(query_result.clone())
        .context("register query result")?;

    let mut object_service = if let Some(snapshot) =
        load_lingqu_object_service_snapshot(&object_store_path, &mut durable_store)?
    {
        LingquObjectServiceStub::import_snapshot(snapshot)
            .with_context(|| format!("import object store {}", object_store_path.display()))?
    } else {
        LingquObjectServiceStub::new(LingquObjectServiceProfile::default())
    };
    let hot_state = memory_service
        .materialize_hot_state_from_query(
            &mut durable_store,
            &mut object_service,
            HotMemoryMaterializeFromQueryReq {
                state_id: state_id.clone(),
                query_result_id: query_result.result_id.clone(),
                owner_entity,
                producer_entity,
                now_us,
            },
        )
        .context("materialize hot memory state")?;
    let hot_state_bytes =
        serde_json::to_vec_pretty(&hot_state).context("encode hot memory state")?;
    if let Some(parent) = hot_state_path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create hot state dir {}", parent.display()))?;
    }
    fs::write(&hot_state_path, hot_state_bytes)
        .with_context(|| format!("write hot state {}", hot_state_path.display()))?;
    save_lingqu_object_service_snapshot(&object_store_path, &mut durable_store, &object_service)?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;
    let object_report = object_service.report();

    println!("lingqu_memory_service");
    println!("  mode: materialize-hot-state");
    println!("  catalog: {}", snapshot.catalog.catalog_id);
    println!("  catalog_path: {}", catalog_path.display());
    println!("  store_path: {}", store_path.display());
    println!("  object_store_path: {}", object_store_path.display());
    println!("  query_result: {}", query_result.result_id);
    println!("  query_result_manifest: {}", query_result_path.path);
    println!("  hot_state: {}", hot_state.state_id);
    println!("  hot_state_path: {}", hot_state_path.display());
    println!("  hot_table_object: {}", hot_state.table.object_key);
    println!("  hot_indices_object: {}", hot_state.indices.object_key);
    println!("  hot_scores_object: {}", hot_state.scores.object_key);
    println!("  hot_table_shape: {:?}", hot_state.table.shape);
    println!(
        "  selected_chunks: {}",
        hot_state.selected_chunk_ids.join(",")
    );
    println!(
        "  obmm_payload_writes: {}",
        object_report.obmm_pool_payload_write_count
    );
    println!(
        "  committed_object_count: {}",
        object_report.committed_object_count
    );
    Ok(())
}

fn run_lingqu_memory_materialize_engram_state_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let object_store_path = PathBuf::from(required_cli_arg(args, "--object-store")?);
    let hot_state_path = PathBuf::from(required_cli_arg(args, "--hot-state")?);
    let gate_weight_path = PathBuf::from(required_cli_arg(args, "--gate-weight-json")?);
    let state_id = required_cli_arg(args, "--state-id")?;
    let engram_state_path = PathBuf::from(required_cli_arg(args, "--engram-state")?);
    let owner_entity = optional_cli_u64(args, "--owner-entity")?.unwrap_or(0);
    let producer_entity = optional_cli_u64(args, "--producer-entity")?.unwrap_or(0);
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let object_snapshot =
        load_lingqu_object_service_snapshot(&object_store_path, &mut durable_store)?.ok_or_else(
            || {
                anyhow::anyhow!(
                    "object store snapshot does not exist: {}",
                    object_store_path.display()
                )
            },
        )?;
    let mut object_service = LingquObjectServiceStub::import_snapshot(object_snapshot)
        .with_context(|| format!("import object store {}", object_store_path.display()))?;

    let hot_state_bytes = fs::read(&hot_state_path)
        .with_context(|| format!("read hot state {}", hot_state_path.display()))?;
    let hot_state = serde_json::from_slice::<HotMemoryStateObject>(&hot_state_bytes)
        .with_context(|| format!("decode hot state {}", hot_state_path.display()))?;
    let gate_weight_bytes = fs::read(&gate_weight_path)
        .with_context(|| format!("read gate weight json {}", gate_weight_path.display()))?;
    let gate_weight = serde_json::from_slice::<LingquMemoryGateWeightInput>(&gate_weight_bytes)
        .with_context(|| format!("decode gate weight json {}", gate_weight_path.display()))?;
    validate_lingqu_memory_gate_weight_input(&gate_weight)?;

    let gate_payload = cli_f32_vec_to_le_bytes(&gate_weight.values);
    let gate_block = optional_cli_arg(args, "--gate-weight-block")?
        .unwrap_or_else(|| format!("block/memory/engram-gate/{}", cli_path_id(&state_id)));
    let gate_weight_ref = durable_store
        .write_block_payload(gate_block, gate_payload)
        .context("write gate weight payload to Lingqu Block store")?;

    let mut memory_service = LingquMemoryService::new();
    memory_service
        .register_hot_state(&object_service, hot_state.clone())
        .context("register hot memory state")?;
    let engram_state = memory_service
        .materialize_engram_state_from_block(
            &mut durable_store,
            &mut object_service,
            EngramStateMaterializeFromBlockReq {
                state_id: state_id.clone(),
                hot_memory_state_id: hot_state.state_id.clone(),
                gate_weight_ref: gate_weight_ref.clone(),
                compatible_models: Vec::new(),
                owner_entity,
                producer_entity,
                now_us,
                expires_at_us: None,
            },
        )
        .context("materialize engram state")?;

    let engram_state_bytes =
        serde_json::to_vec_pretty(&engram_state).context("encode engram state")?;
    if let Some(parent) = engram_state_path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create engram state dir {}", parent.display()))?;
    }
    fs::write(&engram_state_path, engram_state_bytes)
        .with_context(|| format!("write engram state {}", engram_state_path.display()))?;
    save_lingqu_object_service_snapshot(&object_store_path, &mut durable_store, &object_service)?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;
    let object_report = object_service.report();

    println!("lingqu_memory_service");
    println!("  mode: materialize-engram-state");
    println!("  store_path: {}", store_path.display());
    println!("  object_store_path: {}", object_store_path.display());
    println!("  hot_state: {}", hot_state.state_id);
    println!("  engram_state: {}", engram_state.state_id);
    println!("  engram_state_path: {}", engram_state_path.display());
    println!("  table_object: {}", engram_state.table.object_key);
    println!("  indices_object: {}", engram_state.indices.object_key);
    if let Some(gate) = &engram_state.gate {
        println!("  gate_object: {}", gate.object_key);
        println!("  gate_shape: {:?}", gate.shape);
    }
    println!("  gate_weight_block: {}", gate_weight_ref.block.0);
    println!(
        "  obmm_payload_writes: {}",
        object_report.obmm_pool_payload_write_count
    );
    println!(
        "  committed_object_count: {}",
        object_report.committed_object_count
    );
    Ok(())
}

fn validate_lingqu_memory_gate_weight_input(
    input: &LingquMemoryGateWeightInput,
) -> anyhow::Result<()> {
    if input.values.is_empty() {
        anyhow::bail!("gate weight json values must not be empty");
    }
    Ok(())
}

fn lingqu_object_payload_checksum(bytes: &[u8]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for byte in bytes {
        acc ^= u64::from(*byte);
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn qwen3_lingqu_key_hash(key: &str) -> u64 {
    key.as_bytes()
        .iter()
        .fold(1_469_598_103_934_665_603u64, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(1_099_511_628_211)
        })
}

#[derive(Debug)]
struct W5EngramStateRefPublication {
    state_ref_hex: String,
    engram_state_id: String,
    table_bytes: usize,
    indices_bytes: usize,
    gate_bytes: usize,
    state_manifest_bytes: u64,
    object_service_snapshot_path: Option<PathBuf>,
}

#[derive(Debug)]
struct W5MemoryDecisionBundle {
    shortpath: Option<sim_memory::ShortpathDecisionRecord>,
    shortpath_artifact: Option<sim_memory::ExecutionArtifactObject>,
    prefetch: Option<sim_memory::PrefetchPlanRecord>,
    prefetch_artifacts: Vec<sim_memory::ExecutionArtifactObject>,
    prefix_cache: Option<sim_memory::PrefixCacheReusePlan>,
    prefix_cache_artifact: Option<sim_memory::PrefixCacheArtifact>,
}

#[derive(Debug)]
struct W5MemoryDecisionArtifactPublication {
    shortpath_ref: Option<W5MemoryPublishedArtifactRef>,
    prefetch_refs: Vec<W5MemoryPublishedArtifactRef>,
    prefix_cache_ref: Option<W5MemoryPublishedArtifactRef>,
    object_service_snapshot_path: Option<PathBuf>,
}

#[derive(Debug)]
struct W5MemoryPublishedArtifactRef {
    artifact_id: String,
    ref_hex: String,
    payload_bytes: usize,
    payload_checksum: u64,
}

fn load_w5_memory_decisions_from_store(
    config: &W5MemoryDecisionConfig,
) -> anyhow::Result<W5MemoryDecisionBundle> {
    let boundary_decision =
        if let Some(boundary_request_path) = config.boundary_request_path.as_ref() {
            let (_response, decision) =
                run_w5_memory_boundary_lookup(&config.store_path, boundary_request_path, 0)
                    .context("run W5 Memory Service boundary lookup")?;
            Some(decision)
        } else if let Some(observation_id) = config.boundary_observation_id.as_ref() {
            let (_response, decision) = run_w5_memory_boundary_lookup_from_observation(
                &config.store_path,
                observation_id,
                None,
                900,
                vec![sim_memory::ShortpathAction::JumpToTerminal],
                0,
            )
            .context("run W5 Memory Service boundary lookup from observation")?;
            Some(decision)
        } else {
            None
        };
    let mut durable_store = load_lingqu_memory_durable_store(&config.store_path)?;
    let shortpath = if let Some(decision) = boundary_decision {
        Some(decision)
    } else if let Some(decision_id) = &config.shortpath_decision_id {
        let decisions = durable_store
            .load_shortpath_decision_manifest()
            .with_context(|| {
                format!(
                    "load W5 planner shortpath decisions from {}",
                    config.store_path.display()
                )
            })?;
        Some(
            decisions
                .into_iter()
                .find(|decision| decision.decision_id == *decision_id)
                .ok_or_else(|| anyhow::anyhow!("shortpath decision not found: {decision_id}"))?,
        )
    } else {
        None
    };
    let prefetch = if let Some(plan_id) = &config.prefetch_plan_id {
        let plans = durable_store
            .load_prefetch_plan_manifest()
            .with_context(|| {
                format!(
                    "load Memory Service prefetch plans from {}",
                    config.store_path.display()
                )
            })?;
        Some(
            plans
                .into_iter()
                .find(|plan| plan.plan_id == *plan_id)
                .ok_or_else(|| anyhow::anyhow!("prefetch plan not found: {plan_id}"))?,
        )
    } else {
        None
    };
    let prefix_cache = if let Some(plan_id) = &config.prefix_cache_reuse_plan_id {
        let plans = durable_store
            .load_prefix_cache_reuse_plan_manifest()
            .with_context(|| {
                format!(
                    "load Memory Service prefix-cache reuse plans from {}",
                    config.store_path.display()
                )
            })?;
        Some(
            plans
                .into_iter()
                .find(|plan| plan.plan_id == *plan_id)
                .ok_or_else(|| anyhow::anyhow!("prefix-cache reuse plan not found: {plan_id}"))?,
        )
    } else {
        None
    };
    let needs_execution_artifacts = shortpath
        .as_ref()
        .and_then(|decision| decision.artifact_id.as_ref())
        .is_some()
        || prefetch
            .as_ref()
            .map(|plan| !plan.planned_artifact_ids.is_empty())
            .unwrap_or(false);
    let execution_artifacts = if needs_execution_artifacts {
        durable_store
            .load_execution_artifact_manifest()
            .with_context(|| {
                format!(
                    "load Memory Service execution artifacts from {}",
                    config.store_path.display()
                )
            })?
    } else {
        Vec::new()
    };
    let shortpath_artifact = if let Some(decision) = shortpath.as_ref() {
        if let Some(artifact_id) = decision.artifact_id.as_ref() {
            let artifact = w5_find_verified_execution_artifact(
                &execution_artifacts,
                artifact_id,
                "shortpath decision",
            )?;
            validate_w5_shortpath_artifact_contract(decision, &artifact)?;
            validate_lingqu_execution_artifact_payloads(
                &mut durable_store,
                &artifact,
                "shortpath decision",
            )?;
            Some(artifact)
        } else {
            None
        }
    } else {
        None
    };
    let mut prefetch_artifacts = Vec::new();
    if let Some(plan) = &prefetch {
        for artifact_id in &plan.planned_artifact_ids {
            let artifact = w5_find_verified_execution_artifact(
                &execution_artifacts,
                artifact_id,
                "prefetch plan",
            )?;
            validate_lingqu_execution_artifact_payloads(
                &mut durable_store,
                &artifact,
                "prefetch plan",
            )?;
            prefetch_artifacts.push(artifact);
        }
    }
    let prefix_cache_artifact = if let Some(artifact_id) = prefix_cache
        .as_ref()
        .and_then(|plan| plan.artifact_id.as_ref())
    {
        let artifacts = durable_store
            .load_prefix_cache_manifest()
            .with_context(|| {
                format!(
                    "load Memory Service prefix-cache artifacts from {}",
                    config.store_path.display()
                )
            })?;
        let artifact = w5_find_verified_prefix_cache_artifact(
            &artifacts,
            artifact_id,
            "prefix-cache reuse plan",
        )?;
        validate_lingqu_prefix_cache_payloads(
            &mut durable_store,
            &artifact,
            "prefix-cache reuse plan",
        )?;
        Some(artifact)
    } else {
        None
    };
    Ok(W5MemoryDecisionBundle {
        shortpath,
        shortpath_artifact,
        prefetch,
        prefetch_artifacts,
        prefix_cache,
        prefix_cache_artifact,
    })
}

fn w5_find_verified_execution_artifact(
    artifacts: &[sim_memory::ExecutionArtifactObject],
    artifact_id: &str,
    source: &str,
) -> anyhow::Result<sim_memory::ExecutionArtifactObject> {
    let artifact = artifacts
        .iter()
        .find(|artifact| artifact.artifact_id == artifact_id)
        .ok_or_else(|| {
            anyhow::anyhow!("{source} references missing execution artifact: {artifact_id}")
        })?;
    if artifact.state != sim_memory::ExecutionArtifactState::Verified {
        anyhow::bail!(
            "{source} references non-verified execution artifact: {}",
            artifact.artifact_id
        );
    }
    Ok(artifact.clone())
}

fn w5_find_verified_prefix_cache_artifact(
    artifacts: &[sim_memory::PrefixCacheArtifact],
    artifact_id: &str,
    source: &str,
) -> anyhow::Result<sim_memory::PrefixCacheArtifact> {
    let artifact = artifacts
        .iter()
        .find(|artifact| artifact.artifact_id == artifact_id)
        .ok_or_else(|| {
            anyhow::anyhow!("{source} references missing prefix-cache artifact: {artifact_id}")
        })?;
    if artifact.state != sim_memory::ExecutionArtifactState::Verified {
        anyhow::bail!(
            "{source} references non-verified prefix-cache artifact: {}",
            artifact.artifact_id
        );
    }
    Ok(artifact.clone())
}

fn w5_plan_shortpath_decision_from_memory_support(
    response: &sim_memory::BoundaryLookupResponse,
) -> anyhow::Result<sim_memory::ShortpathDecisionRecord> {
    response
        .validate()
        .map_err(|err| anyhow::anyhow!("invalid Memory Service boundary support: {err}"))?;
    let support = &response.support;
    let artifact_checksum = response
        .artifact
        .as_ref()
        .map(|artifact| artifact.checksum)
        .unwrap_or(0);
    let decision_id = format!("shortpath-decision/{}", support.request_id);
    let proof_checksum = qwen3_checksum_words(&[
        support.proof_checksum,
        w5_shortpath_action_tag(support.supported_action),
        u64::from(support.confidence_milli),
        artifact_checksum,
        support.created_at_us,
        support.version,
    ]);
    let decision = sim_memory::ShortpathDecisionRecord {
        decision_id,
        request_id: support.request_id.clone(),
        support_id: Some(support.support_id.clone()),
        action: support.supported_action,
        artifact_id: support.artifact_id.clone(),
        target_layer_start: support.target_layer_start,
        target_layer_end: support.target_layer_end,
        confidence_milli: support.confidence_milli,
        verify_required: support.verify_required,
        proof_checksum,
        reason: format!("w5_planner_accepted_memory_support:{}", support.reason),
        created_at_us: support.created_at_us,
        version: 1,
    };
    decision
        .validate()
        .map_err(|err| anyhow::anyhow!("invalid W5 planner shortpath decision: {err}"))?;
    Ok(decision)
}

fn w5_shortpath_action_tag(action: sim_memory::ShortpathAction) -> u64 {
    match action {
        sim_memory::ShortpathAction::Continue => 1,
        sim_memory::ShortpathAction::JumpToLayer => 2,
        sim_memory::ShortpathAction::JumpToTerminal => 3,
        sim_memory::ShortpathAction::RequireVerify => 4,
    }
}

fn validate_w5_shortpath_artifact_contract(
    decision: &sim_memory::ShortpathDecisionRecord,
    artifact: &sim_memory::ExecutionArtifactObject,
) -> anyhow::Result<()> {
    let expected_kind = match decision.action {
        sim_memory::ShortpathAction::JumpToLayer => sim_memory::ExecutionArtifactKind::HiddenState,
        sim_memory::ShortpathAction::JumpToTerminal => sim_memory::ExecutionArtifactKind::Logits,
        sim_memory::ShortpathAction::Continue | sim_memory::ShortpathAction::RequireVerify => {
            return Ok(());
        }
    };
    if artifact.kind != expected_kind {
        anyhow::bail!(
            "shortpath decision {} action {:?} requires {:?} artifact, got {:?}",
            decision.decision_id,
            decision.action,
            expected_kind,
            artifact.kind
        );
    }
    if decision.target_layer_start != Some(artifact.target_layer_start)
        || decision.target_layer_end != Some(artifact.target_layer_end)
    {
        anyhow::bail!(
            "shortpath decision {} target layer range does not match artifact {}",
            decision.decision_id,
            artifact.artifact_id
        );
    }
    if decision.action == sim_memory::ShortpathAction::JumpToLayer
        && artifact.target_layer_end <= artifact.target_layer_start
    {
        anyhow::bail!(
            "shortpath decision {} jump-to-layer requires a non-empty layer range",
            decision.decision_id
        );
    }
    if decision.action == sim_memory::ShortpathAction::JumpToTerminal
        && artifact.target_layer_end != artifact.target_layer_start
    {
        anyhow::bail!(
            "shortpath decision {} jump-to-terminal requires a terminal zero-length target layer range",
            decision.decision_id
        );
    }
    Ok(())
}

fn validate_lingqu_execution_artifact_payloads(
    durable_store: &mut LingquMemoryDurableStore,
    artifact: &sim_memory::ExecutionArtifactObject,
    source: &str,
) -> anyhow::Result<()> {
    if let Some(payload_ref) = &artifact.durable_payload_ref {
        let bytes = durable_store
            .read_block_payload(payload_ref)
            .with_context(|| {
                format!(
                    "{source} execution artifact {} durable payload unavailable",
                    artifact.artifact_id
                )
            })?;
        if bytes.len() as u64 != payload_ref.bytes {
            anyhow::bail!(
                "{source} execution artifact {} durable payload bytes mismatch: got {} expected {}",
                artifact.artifact_id,
                bytes.len(),
                payload_ref.bytes
            );
        }
        if artifact.kind == sim_memory::ExecutionArtifactKind::Logits {
            validate_w5_terminal_logits_payload(&bytes, artifact, source)?;
        }
    }
    Ok(())
}

const W5_TERMINAL_LOGITS_MARKER: u64 = 0x713377346c6f6730;
const W5_TERMINAL_TOKEN_TEXT_MARKER: u64 = 0x7133773474787430;
const W5_TERMINAL_LOGITS_HEADER_BYTES: usize = 64;
const W5_TERMINAL_LOGITS_ENTRY_WORDS: u64 = 45;
const W5_TERMINAL_LOGITS_ENTRY_BYTES: usize = W5_TERMINAL_LOGITS_ENTRY_WORDS as usize * 8;
const W5_TERMINAL_TOKEN_TEXT_HEADER_BYTES: usize = 64;
const W5_TERMINAL_TOKEN_TEXT_ENTRY_WORDS: u64 = 8;
const W5_TERMINAL_TOKEN_TEXT_ENTRY_BYTES: usize = W5_TERMINAL_TOKEN_TEXT_ENTRY_WORDS as usize * 8;

fn validate_w5_terminal_logits_payload(
    bytes: &[u8],
    artifact: &sim_memory::ExecutionArtifactObject,
    source: &str,
) -> anyhow::Result<()> {
    let marker = read_w5_u64(bytes, 0, "terminal_logits.marker")?;
    let count = read_w5_u64(bytes, 8, "terminal_logits.count")?;
    let entry_words = read_w5_u64(bytes, 16, "terminal_logits.entry_words")?;
    let table_bytes = read_w5_u64(bytes, 24, "terminal_logits.table_bytes")?;
    if marker != W5_TERMINAL_LOGITS_MARKER
        || count == 0
        || entry_words != W5_TERMINAL_LOGITS_ENTRY_WORDS
        || table_bytes != count * W5_TERMINAL_LOGITS_ENTRY_BYTES as u64
    {
        anyhow::bail!(
            "{source} execution artifact {} terminal logits header invalid",
            artifact.artifact_id
        );
    }
    let count_usize = usize::try_from(count)
        .map_err(|_| anyhow::anyhow!("terminal logits count too large: {count}"))?;
    let logits_table_bytes = usize::try_from(table_bytes)
        .map_err(|_| anyhow::anyhow!("terminal logits table too large: {table_bytes}"))?;
    let token_text_header = W5_TERMINAL_LOGITS_HEADER_BYTES
        .checked_add(logits_table_bytes)
        .ok_or_else(|| anyhow::anyhow!("terminal logits table overflow"))?;
    let token_text_base = token_text_header
        .checked_add(W5_TERMINAL_TOKEN_TEXT_HEADER_BYTES)
        .ok_or_else(|| anyhow::anyhow!("terminal token text header overflow"))?;
    let token_text_marker = read_w5_u64(bytes, token_text_header, "terminal_token_text.marker")?;
    let token_text_count = read_w5_u64(bytes, token_text_header + 8, "terminal_token_text.count")?;
    let token_text_entry_words = read_w5_u64(
        bytes,
        token_text_header + 16,
        "terminal_token_text.entry_words",
    )?;
    let token_text_table_bytes = read_w5_u64(
        bytes,
        token_text_header + 24,
        "terminal_token_text.table_bytes",
    )?;
    let token_text_total_bytes = read_w5_u64(
        bytes,
        token_text_header + 32,
        "terminal_token_text.total_bytes",
    )?;
    if token_text_marker != W5_TERMINAL_TOKEN_TEXT_MARKER
        || token_text_count != count
        || token_text_entry_words != W5_TERMINAL_TOKEN_TEXT_ENTRY_WORDS
        || token_text_table_bytes != count * W5_TERMINAL_TOKEN_TEXT_ENTRY_BYTES as u64
        || token_text_total_bytes == 0
    {
        anyhow::bail!(
            "{source} execution artifact {} terminal token text header invalid",
            artifact.artifact_id
        );
    }
    let token_text_table_len = usize::try_from(token_text_table_bytes)
        .map_err(|_| anyhow::anyhow!("terminal token text table too large"))?;
    let token_text_end = token_text_base
        .checked_add(token_text_table_len)
        .ok_or_else(|| anyhow::anyhow!("terminal token text table overflow"))?;
    if bytes.len() < token_text_end {
        anyhow::bail!(
            "{source} execution artifact {} terminal payload truncated: got {} expected at least {}",
            artifact.artifact_id,
            bytes.len(),
            token_text_end
        );
    }
    for entry in 0..count_usize {
        validate_w5_terminal_logits_entry(bytes, entry, count, artifact, source)?;
        validate_w5_terminal_token_text_entry(bytes, token_text_base, entry, artifact, source)?;
    }
    Ok(())
}

fn validate_w5_terminal_logits_entry(
    bytes: &[u8],
    entry: usize,
    count: u64,
    artifact: &sim_memory::ExecutionArtifactObject,
    source: &str,
) -> anyhow::Result<()> {
    let logits_base = W5_TERMINAL_LOGITS_HEADER_BYTES + entry * W5_TERMINAL_LOGITS_ENTRY_BYTES;
    let sampled_token = read_w5_u64(bytes, logits_base + 32, "terminal_logits.sampled_token")?;
    let runner_up_token = read_w5_u64(bytes, logits_base + 40, "terminal_logits.runner_up_token")?;
    let margin_milli = read_w5_u64(bytes, logits_base + 48, "terminal_logits.margin_milli")?;
    let logits_checksum = read_w5_u64(bytes, logits_base + 56, "terminal_logits.checksum")?;
    let text_checksum = read_w5_u64(bytes, logits_base + 64, "terminal_logits.text_checksum")?;
    let step_index = read_w5_u64(bytes, logits_base + 72, "terminal_logits.step_index")?;
    let full_vocab_checked = read_w5_u64(
        bytes,
        logits_base + 104,
        "terminal_logits.full_vocab_checked",
    )?;
    let full_vocab_checksum = read_w5_u64(
        bytes,
        logits_base + 112,
        "terminal_logits.full_vocab_checksum",
    )?;
    let top_logit_bits = read_w5_u64(bytes, logits_base + 120, "terminal_logits.top_logit_bits")?;
    let runner_up_logit_bits = read_w5_u64(
        bytes,
        logits_base + 128,
        "terminal_logits.runner_up_logit_bits",
    )?;
    let candidate_count = read_w5_u64(bytes, logits_base + 160, "terminal_logits.candidate_count")?;
    if sampled_token >= full_vocab_checked
        || runner_up_token >= full_vocab_checked
        || sampled_token == runner_up_token
        || margin_milli == 0
        || logits_checksum == 0
        || text_checksum == 0
        || step_index != entry as u64
        || full_vocab_checked == 0
        || full_vocab_checksum == 0
        || top_logit_bits == 0
        || runner_up_logit_bits == 0
        || candidate_count == 0
        || candidate_count > 4
    {
        anyhow::bail!(
            "{source} execution artifact {} terminal logits entry invalid at index {entry}",
            artifact.artifact_id
        );
    }
    for candidate in 0..usize::try_from(candidate_count).unwrap_or(0) {
        let candidate_base = logits_base + 168 + candidate * 48;
        let token = read_w5_u64(bytes, candidate_base, "terminal_logits.candidate_token")?;
        let logit_bits = read_w5_u64(bytes, candidate_base + 8, "terminal_logits.candidate_logit")?;
        let candidate_text_checksum =
            read_w5_u64(bytes, candidate_base + 16, "terminal_logits.candidate_text")?;
        let piece_bytes = read_w5_u64(
            bytes,
            candidate_base + 24,
            "terminal_logits.candidate_piece_bytes",
        )?;
        if token >= full_vocab_checked
            || logit_bits == 0
            || candidate_text_checksum == 0
            || piece_bytes == 0
        {
            anyhow::bail!(
                "{source} execution artifact {} terminal logits candidate invalid at index {entry}:{candidate}",
                artifact.artifact_id
            );
        }
        if candidate == 0 && token != sampled_token {
            anyhow::bail!(
                "{source} execution artifact {} terminal logits candidate0 does not match sampled token",
                artifact.artifact_id
            );
        }
    }
    if count != 1 && entry == 0 && step_index != 0 {
        anyhow::bail!(
            "{source} execution artifact {} terminal logits first step invalid",
            artifact.artifact_id
        );
    }
    Ok(())
}

fn validate_w5_terminal_token_text_entry(
    bytes: &[u8],
    token_text_base: usize,
    entry: usize,
    artifact: &sim_memory::ExecutionArtifactObject,
    source: &str,
) -> anyhow::Result<()> {
    let logits_base = W5_TERMINAL_LOGITS_HEADER_BYTES + entry * W5_TERMINAL_LOGITS_ENTRY_BYTES;
    let text_base = token_text_base + entry * W5_TERMINAL_TOKEN_TEXT_ENTRY_BYTES;
    let sampled_token = read_w5_u64(bytes, logits_base + 32, "terminal_logits.sampled_token")?;
    let text_checksum = read_w5_u64(bytes, logits_base + 64, "terminal_logits.text_checksum")?;
    let text_step = read_w5_u64(bytes, text_base, "terminal_token_text.step")?;
    let text_token = read_w5_u64(bytes, text_base + 8, "terminal_token_text.token")?;
    let byte_len = read_w5_u64(bytes, text_base + 24, "terminal_token_text.byte_len")?;
    let piece_word0 = read_w5_u64(bytes, text_base + 32, "terminal_token_text.piece_word0")?;
    let checksum = read_w5_u64(bytes, text_base + 48, "terminal_token_text.checksum")?;
    if text_step != entry as u64
        || text_token != sampled_token
        || byte_len == 0
        || piece_word0 == 0
        || checksum != text_checksum
    {
        anyhow::bail!(
            "{source} execution artifact {} terminal token text entry invalid at index {entry}",
            artifact.artifact_id
        );
    }
    Ok(())
}

fn read_w5_u64(bytes: &[u8], offset: usize, field: &str) -> anyhow::Result<u64> {
    let end = offset
        .checked_add(8)
        .ok_or_else(|| anyhow::anyhow!("{field} offset overflow"))?;
    let raw = bytes
        .get(offset..end)
        .ok_or_else(|| anyhow::anyhow!("{field} out of bounds"))?;
    Ok(u64::from_le_bytes(
        raw.try_into().expect("u64 field length"),
    ))
}

fn validate_lingqu_prefix_cache_payloads(
    durable_store: &mut LingquMemoryDurableStore,
    artifact: &sim_memory::PrefixCacheArtifact,
    source: &str,
) -> anyhow::Result<()> {
    for payload_ref in &artifact.durable_payload_refs {
        let bytes = durable_store
            .read_block_payload(payload_ref)
            .with_context(|| {
                format!(
                    "{source} prefix cache artifact {} durable payload unavailable",
                    artifact.artifact_id
                )
            })?;
        if bytes.len() as u64 != payload_ref.bytes {
            anyhow::bail!(
                "{source} prefix cache artifact {} durable payload bytes mismatch: got {} expected {}",
                artifact.artifact_id,
                bytes.len(),
                payload_ref.bytes
            );
        }
    }
    Ok(())
}

fn w5_memory_decisions_reference_artifacts(bundle: &W5MemoryDecisionBundle) -> bool {
    bundle.shortpath_artifact.is_some()
        || !bundle.prefetch_artifacts.is_empty()
        || bundle.prefix_cache_artifact.is_some()
}

fn publish_w5_memory_decision_artifact_refs(
    config: &W5MemoryBootstrapConfig,
    bundle: &W5MemoryDecisionBundle,
) -> anyhow::Result<W5MemoryDecisionArtifactPublication> {
    let mut durable_store = load_lingqu_memory_durable_store(&config.store_path)?;
    let object_snapshot =
        load_lingqu_object_service_snapshot(&config.object_store_path, &mut durable_store)?;
    let mut object_service = if let Some(snapshot) = object_snapshot {
        LingquObjectServiceStub::import_snapshot(snapshot).with_context(|| {
            format!("import object store {}", config.object_store_path.display())
        })?
    } else {
        LingquObjectServiceStub::new(LingquObjectServiceProfile::default())
    };
    let shortpath_ref = bundle
        .shortpath_artifact
        .as_ref()
        .map(|artifact| {
            publish_w5_execution_artifact_ref(
                &mut durable_store,
                &mut object_service,
                artifact,
                config.owner_entity,
                config.producer_entity,
                "shortpath",
            )
        })
        .transpose()?;
    let mut prefetch_refs = Vec::new();
    for artifact in &bundle.prefetch_artifacts {
        prefetch_refs.push(publish_w5_execution_artifact_ref(
            &mut durable_store,
            &mut object_service,
            artifact,
            config.owner_entity,
            config.producer_entity,
            "prefetch",
        )?);
    }
    let prefix_cache_ref = bundle
        .prefix_cache_artifact
        .as_ref()
        .map(|artifact| {
            publish_w5_prefix_cache_artifact_ref(
                &mut durable_store,
                &mut object_service,
                artifact,
                config.owner_entity,
                config.producer_entity,
            )
        })
        .transpose()?;

    save_lingqu_object_service_snapshot(
        &config.object_store_path,
        &mut durable_store,
        &object_service,
    )?;
    save_lingqu_memory_durable_store(&config.store_path, &durable_store)?;
    let snapshot_path = export_w5_object_service_snapshot(&config.registry_dir, &object_service)?;

    Ok(W5MemoryDecisionArtifactPublication {
        shortpath_ref,
        prefetch_refs,
        prefix_cache_ref,
        object_service_snapshot_path: Some(snapshot_path),
    })
}

fn publish_w5_execution_artifact_ref(
    durable_store: &mut LingquMemoryDurableStore,
    object_service: &mut LingquObjectServiceStub,
    artifact: &sim_memory::ExecutionArtifactObject,
    owner_entity: u32,
    producer_entity: u32,
    source: &str,
) -> anyhow::Result<W5MemoryPublishedArtifactRef> {
    let payload_ref = artifact.durable_payload_ref.as_ref().ok_or_else(|| {
        anyhow::anyhow!(
            "{source} execution artifact {} is not backed by a durable payload",
            artifact.artifact_id
        )
    })?;
    let payload = durable_store
        .read_block_payload(payload_ref)
        .with_context(|| {
            format!(
                "{source} execution artifact {} durable payload unavailable",
                artifact.artifact_id
            )
        })?;
    let object_key = format!(
        "lingqu/memory/execution/{}/v{}",
        artifact.artifact_id, artifact.version
    );
    let object_ref = publish_w5_object_service_payload_ref(
        object_service,
        w5_execution_artifact_obmm_kind(artifact.kind),
        w5_execution_artifact_object_kind(artifact.kind),
        owner_entity,
        producer_entity,
        &object_key,
        &payload,
        source,
    )?;
    Ok(W5MemoryPublishedArtifactRef {
        artifact_id: artifact.artifact_id.clone(),
        ref_hex: qwen3_obmm_object_ref_wire_to_hex(&object_ref),
        payload_bytes: payload.len(),
        payload_checksum: object_ref.payload_checksum,
    })
}

fn publish_w5_prefix_cache_artifact_ref(
    durable_store: &mut LingquMemoryDurableStore,
    object_service: &mut LingquObjectServiceStub,
    artifact: &sim_memory::PrefixCacheArtifact,
    owner_entity: u32,
    producer_entity: u32,
) -> anyhow::Result<W5MemoryPublishedArtifactRef> {
    if artifact.durable_payload_refs.is_empty() {
        anyhow::bail!(
            "prefix-cache artifact {} is not backed by durable payloads",
            artifact.artifact_id
        );
    }
    let mut payload = Vec::new();
    for payload_ref in &artifact.durable_payload_refs {
        payload.extend_from_slice(&durable_store.read_block_payload(payload_ref).with_context(
            || {
                format!(
                    "prefix-cache artifact {} durable payload unavailable",
                    artifact.artifact_id
                )
            },
        )?);
    }
    let object_key = format!(
        "lingqu/memory/prefix-cache/{}/v{}",
        artifact.artifact_id, artifact.version
    );
    let object_ref = publish_w5_object_service_payload_ref(
        object_service,
        QWEN3_DENSE_PROFILE_OBMM_KIND_QWEN3_KV_STATE,
        LingquObjectKind::KvCacheBlock,
        owner_entity,
        producer_entity,
        &object_key,
        &payload,
        "prefix-cache",
    )?;
    Ok(W5MemoryPublishedArtifactRef {
        artifact_id: artifact.artifact_id.clone(),
        ref_hex: qwen3_obmm_object_ref_wire_to_hex(&object_ref),
        payload_bytes: payload.len(),
        payload_checksum: object_ref.payload_checksum,
    })
}

fn w5_execution_artifact_obmm_kind(kind: sim_memory::ExecutionArtifactKind) -> u16 {
    match kind {
        sim_memory::ExecutionArtifactKind::HiddenState => {
            QWEN3_DENSE_PROFILE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT
        }
        sim_memory::ExecutionArtifactKind::KvCache => QWEN3_DENSE_PROFILE_OBMM_KIND_QWEN3_KV_STATE,
        sim_memory::ExecutionArtifactKind::Logits => QWEN3_DENSE_PROFILE_OBMM_KIND_TERMINAL_LOGITS,
    }
}

fn w5_execution_artifact_object_kind(kind: sim_memory::ExecutionArtifactKind) -> LingquObjectKind {
    match kind {
        sim_memory::ExecutionArtifactKind::HiddenState => LingquObjectKind::RuntimeTensor,
        sim_memory::ExecutionArtifactKind::KvCache => LingquObjectKind::KvCacheBlock,
        sim_memory::ExecutionArtifactKind::Logits => LingquObjectKind::Logits,
    }
}

fn publish_w5_object_service_payload_ref(
    object_service: &mut LingquObjectServiceStub,
    obmm_kind: u16,
    object_kind: LingquObjectKind,
    owner_entity: u32,
    producer_entity: u32,
    object_key: &str,
    payload: &[u8],
    source: &str,
) -> anyhow::Result<LingquObmmObjectRefWire> {
    if payload.is_empty() {
        anyhow::bail!("{source} object payload is empty");
    }
    let payload_checksum = lingqu_object_payload_checksum(payload);
    object_service
        .submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: object_key.to_string(),
                kind: object_kind,
                producer_entity: u64::from(producer_entity),
                owner_entity: Some(u64::from(owner_entity)),
                expected_version: None,
                metadata: LingquObjectMetadata {
                    bytes: payload.len() as u64,
                    checksum: payload_checksum,
                    dtype: None,
                    shape: vec![payload.len() as u64],
                    layout: None,
                    expires_at_us: None,
                },
                placements: vec![LingquPayloadPlacement {
                    backend: LingquPayloadBackend::ObmmShmem,
                    storage_ref: format!("{object_key}/payload"),
                    segment: None,
                    offset: 0,
                    bytes: payload.len() as u64,
                    checksum: payload_checksum,
                    locality: LingquObjectLocality::DomainShared(0),
                }],
                payload_bytes: payload.to_vec(),
            },
            1,
        )
        .with_context(|| format!("publish {source} payload into Object Service"))?;
    let record = object_service
        .latest_record(object_key)
        .ok_or_else(|| anyhow::anyhow!("{source} Object Service record missing: {object_key}"))?;
    let placement = record
        .placements
        .iter()
        .find(|placement| placement.backend == LingquPayloadBackend::ObmmShmem)
        .ok_or_else(|| anyhow::anyhow!("{source} Object Service OBMM placement missing"))?;
    Ok(qwen3_obmm_object_ref_for_payload(
        obmm_kind,
        owner_entity,
        producer_entity,
        record.version,
        &record.key,
        placement.offset,
        record.bytes,
        record.checksum,
    ))
}

fn w5_memory_decision_env_vars(
    config: &W5MemoryDecisionConfig,
    bundle: &W5MemoryDecisionBundle,
    publication: Option<&W5MemoryDecisionArtifactPublication>,
) -> Vec<(String, String)> {
    let mut vars = vec![
        (
            "SIM_W5_MEMORY_SERVICE".to_string(),
            "lingqu_memory_service".to_string(),
        ),
        (
            "SIM_W5_MEMORY_DECISION_STORE".to_string(),
            config.store_path.display().to_string(),
        ),
    ];
    if let Some(decision) = &bundle.shortpath {
        vars.extend([
            (
                "SIM_W5_MEMORY_SHORTPATH_DECISION_ID".to_string(),
                decision.decision_id.clone(),
            ),
            (
                "SIM_W5_MEMORY_SHORTPATH_SUPPORT_ID".to_string(),
                decision.support_id.clone().unwrap_or_default(),
            ),
            (
                "SIM_W5_MEMORY_SHORTPATH_ACTION".to_string(),
                w5_shortpath_action_name(decision.action).to_string(),
            ),
            (
                "SIM_W5_MEMORY_SHORTPATH_ARTIFACT_ID".to_string(),
                decision.artifact_id.clone().unwrap_or_default(),
            ),
            (
                "SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_START".to_string(),
                decision
                    .target_layer_start
                    .map(|value| value.to_string())
                    .unwrap_or_default(),
            ),
            (
                "SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_END".to_string(),
                decision
                    .target_layer_end
                    .map(|value| value.to_string())
                    .unwrap_or_default(),
            ),
            (
                "SIM_W5_MEMORY_SHORTPATH_PROOF_CHECKSUM".to_string(),
                format!("{:#x}", decision.proof_checksum),
            ),
        ]);
        if config.shortpath_execute {
            vars.push((
                "SIM_W5_MEMORY_SHORTPATH_EXECUTE".to_string(),
                "1".to_string(),
            ));
        }
        if let Some(artifact) = &bundle.shortpath_artifact {
            vars.extend([
                (
                    "SIM_W5_MEMORY_SHORTPATH_ARTIFACT_KIND".to_string(),
                    w5_execution_artifact_kind_name(artifact.kind).to_string(),
                ),
                (
                    "SIM_W5_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM".to_string(),
                    format!("{:#x}", artifact.checksum),
                ),
                (
                    "SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_START".to_string(),
                    artifact.producer_boundary.layer_start.to_string(),
                ),
                (
                    "SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_END".to_string(),
                    artifact.producer_boundary.layer_end.to_string(),
                ),
                (
                    "SIM_W5_MEMORY_SHORTPATH_PRODUCER_POSITION".to_string(),
                    artifact.producer_boundary.position.to_string(),
                ),
            ]);
        }
        if let Some(published) = publication.and_then(|published| published.shortpath_ref.as_ref())
        {
            vars.push((
                "SIM_W5_MEMORY_SHORTPATH_ARTIFACT_REF".to_string(),
                published.ref_hex.clone(),
            ));
        }
    }
    if let Some(plan) = &bundle.prefetch {
        vars.extend([
            (
                "SIM_W5_MEMORY_PREFETCH_PLAN_ID".to_string(),
                plan.plan_id.clone(),
            ),
            (
                "SIM_W5_MEMORY_PREFETCH_SCOPE".to_string(),
                w5_prefetch_scope_name(plan.scope).to_string(),
            ),
            (
                "SIM_W5_MEMORY_PREFETCH_TARGET_STEP_INDEX".to_string(),
                plan.target_step_index.to_string(),
            ),
            (
                "SIM_W5_MEMORY_PREFETCH_CHECKSUM".to_string(),
                format!("{:#x}", plan.checksum),
            ),
        ]);
        if !bundle.prefetch_artifacts.is_empty() {
            vars.extend([
                (
                    "SIM_W5_MEMORY_PREFETCH_ARTIFACT_IDS".to_string(),
                    bundle
                        .prefetch_artifacts
                        .iter()
                        .map(|artifact| artifact.artifact_id.as_str())
                        .collect::<Vec<_>>()
                        .join(","),
                ),
                (
                    "SIM_W5_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS".to_string(),
                    bundle
                        .prefetch_artifacts
                        .iter()
                        .map(|artifact| format!("{:#x}", artifact.checksum))
                        .collect::<Vec<_>>()
                        .join(","),
                ),
            ]);
        }
        if let Some(published) = publication {
            if !published.prefetch_refs.is_empty() {
                vars.push((
                    "SIM_W5_MEMORY_PREFETCH_ARTIFACT_REFS".to_string(),
                    published
                        .prefetch_refs
                        .iter()
                        .map(|artifact| artifact.ref_hex.as_str())
                        .collect::<Vec<_>>()
                        .join(","),
                ));
            }
        }
    }
    if let Some(plan) = &bundle.prefix_cache {
        vars.extend([
            (
                "SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID".to_string(),
                plan.plan_id.clone(),
            ),
            (
                "SIM_W5_MEMORY_PREFIX_CACHE_ACTION".to_string(),
                w5_prefix_cache_reuse_action_name(plan.action).to_string(),
            ),
            (
                "SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_ID".to_string(),
                plan.artifact_id.clone().unwrap_or_default(),
            ),
            (
                "SIM_W5_MEMORY_PREFIX_CACHE_PROOF_CHECKSUM".to_string(),
                format!("{:#x}", plan.proof_checksum),
            ),
        ]);
        if let Some(artifact) = &bundle.prefix_cache_artifact {
            vars.push((
                "SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM".to_string(),
                format!("{:#x}", artifact.checksum),
            ));
        }
        if let Some(published) =
            publication.and_then(|published| published.prefix_cache_ref.as_ref())
        {
            vars.push((
                "SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_REF".to_string(),
                published.ref_hex.clone(),
            ));
        }
    }
    vars
}

fn w5_execution_artifact_kind_name(kind: sim_memory::ExecutionArtifactKind) -> &'static str {
    match kind {
        sim_memory::ExecutionArtifactKind::HiddenState => "hidden-state",
        sim_memory::ExecutionArtifactKind::KvCache => "kv-cache",
        sim_memory::ExecutionArtifactKind::Logits => "logits",
    }
}

fn w5_shortpath_action_name(action: sim_memory::ShortpathAction) -> &'static str {
    match action {
        sim_memory::ShortpathAction::Continue => "continue",
        sim_memory::ShortpathAction::JumpToLayer => "jump-to-layer",
        sim_memory::ShortpathAction::JumpToTerminal => "jump-to-terminal",
        sim_memory::ShortpathAction::RequireVerify => "require-verify",
    }
}

fn w5_prefetch_scope_name(scope: sim_memory::PrefetchScope) -> &'static str {
    match scope {
        sim_memory::PrefetchScope::Range => "range",
        sim_memory::PrefetchScope::Step => "step",
        sim_memory::PrefetchScope::MultiStep => "multi-step",
    }
}

fn w5_prefix_cache_reuse_action_name(action: sim_memory::PrefixCacheReuseAction) -> &'static str {
    match action {
        sim_memory::PrefixCacheReuseAction::Miss => "miss",
        sim_memory::PrefixCacheReuseAction::Reuse => "reuse",
        sim_memory::PrefixCacheReuseAction::RequireVerify => "require-verify",
    }
}

fn publish_w5_engram_state_ref_from_memory(
    config: &W5MemoryBootstrapConfig,
) -> anyhow::Result<W5EngramStateRefPublication> {
    let mut durable_store = load_lingqu_memory_durable_store(&config.store_path)?;
    let object_snapshot =
        load_lingqu_object_service_snapshot(&config.object_store_path, &mut durable_store)?
            .ok_or_else(|| {
                anyhow::anyhow!(
                    "object store snapshot does not exist: {}",
                    config.object_store_path.display()
                )
            })?;
    let object_service = LingquObjectServiceStub::import_snapshot(object_snapshot)
        .with_context(|| format!("import object store {}", config.object_store_path.display()))?;
    publish_w5_engram_state_ref_from_object_service(
        &object_service,
        &config.engram_state_path,
        &config.registry_dir,
        config.owner_entity,
        config.producer_entity,
    )
}

fn publish_w5_engram_state_ref_from_object_service(
    object_service: &LingquObjectServiceStub,
    engram_state_path: &Path,
    registry_dir: &Path,
    owner_entity: u32,
    producer_entity: u32,
) -> anyhow::Result<W5EngramStateRefPublication> {
    let engram_state_bytes = fs::read(engram_state_path)
        .with_context(|| format!("read engram state {}", engram_state_path.display()))?;
    let engram_state = serde_json::from_slice::<EngramStateObject>(&engram_state_bytes)
        .with_context(|| format!("decode engram state {}", engram_state_path.display()))?;
    let gate = engram_state
        .gate
        .as_ref()
        .context("engram state is missing gate object ref")?;

    let table_payload = object_service
        .get_copy(
            &engram_state.table.object_key,
            LingquObjectVersionSelector::LatestCommitted,
        )
        .with_context(|| format!("resolve table object {}", engram_state.table.object_key))?;
    let indices_payload = object_service
        .get_copy(
            &engram_state.indices.object_key,
            LingquObjectVersionSelector::LatestCommitted,
        )
        .with_context(|| format!("resolve indices object {}", engram_state.indices.object_key))?;
    let gate_payload = object_service
        .get_copy(
            &gate.object_key,
            LingquObjectVersionSelector::LatestCommitted,
        )
        .with_context(|| format!("resolve gate object {}", gate.object_key))?;

    let previous_registry_dir = env::var_os(SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR);
    env::set_var(SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR, registry_dir);
    let publish_result = (|| -> anyhow::Result<_> {
        let table_ref = qwen3_publish_object_registry_payload(
            QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_TABLE,
            owner_entity,
            producer_entity,
            &engram_state.table.object_key,
            &table_payload,
        )
        .map_err(anyhow::Error::msg)?;
        let indices_ref = qwen3_publish_object_registry_payload(
            QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_INDICES,
            owner_entity,
            producer_entity,
            &engram_state.indices.object_key,
            &indices_payload,
        )
        .map_err(anyhow::Error::msg)?;
        let gate_ref = qwen3_publish_object_registry_payload(
            QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_GATE_WEIGHT,
            owner_entity,
            producer_entity,
            &gate.object_key,
            &gate_payload,
        )
        .map_err(anyhow::Error::msg)?;
        let state_ref = qwen3_publish_engram_state_registry_payload(
            owner_entity,
            producer_entity,
            &engram_state.state_id,
            &table_ref,
            &indices_ref,
            &gate_ref,
        )
        .map_err(anyhow::Error::msg)?;
        Ok(state_ref)
    })();
    if let Some(previous) = previous_registry_dir {
        env::set_var(SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR, previous);
    } else {
        env::remove_var(SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR);
    }
    let state_ref = publish_result?;
    Ok(W5EngramStateRefPublication {
        state_ref_hex: qwen3_obmm_object_ref_wire_to_hex(&state_ref),
        engram_state_id: engram_state.state_id,
        table_bytes: table_payload.len(),
        indices_bytes: indices_payload.len(),
        gate_bytes: gate_payload.len(),
        state_manifest_bytes: state_ref.payload_bytes,
        object_service_snapshot_path: None,
    })
}

fn w5_hot_tensor_object_ref_from_object_service(
    object_service: &LingquObjectServiceStub,
    object_kind: u16,
    hot_ref: &sim_memory::HotTensorObjectRef,
) -> anyhow::Result<LingquObmmObjectRefWire> {
    let record = object_service
        .latest_record(&hot_ref.object_key)
        .ok_or_else(|| anyhow::anyhow!("missing Object Service record {}", hot_ref.object_key))?;
    if record.version != hot_ref.version {
        anyhow::bail!(
            "Object Service record version mismatch key={} got={} expected={}",
            hot_ref.object_key,
            record.version,
            hot_ref.version
        );
    }
    let placement = record
        .placements
        .iter()
        .find(|placement| placement.backend == LingquPayloadBackend::ObmmShmem)
        .ok_or_else(|| anyhow::anyhow!("missing OBMM placement {}", hot_ref.object_key))?;
    if placement.bytes != hot_ref.bytes
        || placement.offset != hot_ref.offset
        || placement.checksum != hot_ref.checksum
        || record.bytes != hot_ref.bytes
        || record.checksum != hot_ref.checksum
    {
        anyhow::bail!(
            "Object Service hot ref metadata mismatch key={}",
            hot_ref.object_key
        );
    }
    let owner_entity = u32::try_from(record.owner_entity.unwrap_or(record.producer_entity))
        .with_context(|| format!("owner entity too large for {}", hot_ref.object_key))?;
    let producer_entity = u32::try_from(record.producer_entity)
        .with_context(|| format!("producer entity too large for {}", hot_ref.object_key))?;
    Ok(qwen3_obmm_object_ref_for_payload(
        object_kind,
        owner_entity,
        producer_entity,
        record.version,
        &record.key,
        placement.offset,
        record.bytes,
        record.checksum,
    ))
}

fn publish_w5_engram_state_ref_from_memory_objects(
    config: &W5MemoryBootstrapConfig,
) -> anyhow::Result<W5EngramStateRefPublication> {
    let mut durable_store = load_lingqu_memory_durable_store(&config.store_path)?;
    let object_snapshot =
        load_lingqu_object_service_snapshot(&config.object_store_path, &mut durable_store)?
            .ok_or_else(|| {
                anyhow::anyhow!(
                    "object store snapshot does not exist: {}",
                    config.object_store_path.display()
                )
            })?;
    let mut object_service = LingquObjectServiceStub::import_snapshot(object_snapshot)
        .with_context(|| format!("import object store {}", config.object_store_path.display()))?;
    let engram_state_bytes = fs::read(&config.engram_state_path)
        .with_context(|| format!("read engram state {}", config.engram_state_path.display()))?;
    let engram_state = serde_json::from_slice::<EngramStateObject>(&engram_state_bytes)
        .with_context(|| format!("decode engram state {}", config.engram_state_path.display()))?;
    let gate = engram_state
        .gate
        .as_ref()
        .context("engram state is missing gate object ref")?;

    let table_ref = w5_hot_tensor_object_ref_from_object_service(
        &object_service,
        QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_TABLE,
        &engram_state.table,
    )?;
    let indices_ref = w5_hot_tensor_object_ref_from_object_service(
        &object_service,
        QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_INDICES,
        &engram_state.indices,
    )?;
    let gate_ref = w5_hot_tensor_object_ref_from_object_service(
        &object_service,
        QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_GATE_WEIGHT,
        gate,
    )?;
    let state_payload =
        sim_uapi::qwen3_engram_state_manifest_payload(&table_ref, &indices_ref, &gate_ref)
            .map_err(anyhow::Error::msg)?;
    let state_key = format!(
        "lingqu/memory/engram/{}/state_manifest",
        engram_state.state_id
    );
    let state_checksum = lingqu_object_payload_checksum(&state_payload);
    object_service
        .submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: state_key.clone(),
                kind: LingquObjectKind::Metadata,
                producer_entity: u64::from(config.producer_entity),
                owner_entity: Some(u64::from(config.owner_entity)),
                expected_version: None,
                metadata: LingquObjectMetadata {
                    bytes: state_payload.len() as u64,
                    checksum: state_checksum,
                    dtype: None,
                    shape: vec![state_payload.len() as u64],
                    layout: None,
                    expires_at_us: None,
                },
                placements: vec![LingquPayloadPlacement {
                    backend: LingquPayloadBackend::ObmmShmem,
                    storage_ref: format!("{state_key}/payload"),
                    segment: None,
                    offset: 0,
                    bytes: state_payload.len() as u64,
                    checksum: state_checksum,
                    locality: LingquObjectLocality::DomainShared(0),
                }],
                payload_bytes: state_payload,
            },
            1,
        )
        .context("publish W5 EngramStateObjectRef into Object Service")?;
    let state_record = object_service
        .latest_record(&state_key)
        .ok_or_else(|| anyhow::anyhow!("missing published Engram state manifest"))?;
    let state_placement = state_record
        .placements
        .iter()
        .find(|placement| placement.backend == LingquPayloadBackend::ObmmShmem)
        .ok_or_else(|| anyhow::anyhow!("missing Engram state manifest OBMM placement"))?;
    let state_ref = qwen3_obmm_object_ref_for_payload(
        QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_STATE,
        config.owner_entity,
        config.producer_entity,
        state_record.version,
        &state_record.key,
        state_placement.offset,
        state_record.bytes,
        state_record.checksum,
    );

    save_lingqu_object_service_snapshot(
        &config.object_store_path,
        &mut durable_store,
        &object_service,
    )?;
    save_lingqu_memory_durable_store(&config.store_path, &durable_store)?;

    let snapshot_path = export_w5_object_service_snapshot(&config.registry_dir, &object_service)?;

    Ok(W5EngramStateRefPublication {
        state_ref_hex: qwen3_obmm_object_ref_wire_to_hex(&state_ref),
        engram_state_id: engram_state.state_id,
        table_bytes: usize::try_from(table_ref.payload_bytes)
            .context("table payload bytes exceed usize")?,
        indices_bytes: usize::try_from(indices_ref.payload_bytes)
            .context("indices payload bytes exceed usize")?,
        gate_bytes: usize::try_from(gate_ref.payload_bytes).context("gate bytes exceed usize")?,
        state_manifest_bytes: state_ref.payload_bytes,
        object_service_snapshot_path: Some(snapshot_path),
    })
}

fn export_w5_object_service_snapshot(
    registry_dir: &Path,
    object_service: &LingquObjectServiceStub,
) -> anyhow::Result<PathBuf> {
    let snapshot_path = registry_dir.join("lingqu_object_service_snapshot.json");
    if let Some(parent) = snapshot_path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create object service snapshot dir {}", parent.display()))?;
    }
    let snapshot_bytes = object_service
        .export_snapshot()
        .to_json_bytes()
        .context("encode Object Service snapshot for W5")?;
    fs::write(&snapshot_path, snapshot_bytes)
        .with_context(|| format!("write Object Service snapshot {}", snapshot_path.display()))?;
    export_w5_object_service_payload_index(&snapshot_path, &object_service.export_snapshot())?;
    Ok(snapshot_path)
}

const W5_OBJECT_SERVICE_PAYLOAD_INDEX_MAGIC: u64 = 0x3059_4150_4f53_514c;
const W5_OBJECT_SERVICE_PAYLOAD_INDEX_VERSION: u32 = 1;
const W5_OBJECT_SERVICE_PAYLOAD_INDEX_HEADER_BYTES: usize = 32;
const W5_OBJECT_SERVICE_PAYLOAD_INDEX_RECORD_BYTES: usize = 48;

fn export_w5_object_service_payload_index(
    snapshot_path: &Path,
    snapshot: &LingquObjectServiceSnapshot,
) -> anyhow::Result<()> {
    struct PayloadIndexRecord<'a> {
        key_hash: u64,
        owner_entity: u32,
        producer_entity: u32,
        version: u64,
        bytes: u64,
        checksum: u64,
        payload: &'a [u8],
    }

    let mut records = Vec::new();
    for record in &snapshot.records {
        if record.state != LingquObjectState::Committed || record.payload_bytes.is_empty() {
            continue;
        }
        let owner = record.owner_entity.unwrap_or(record.producer_entity);
        let Ok(owner_entity) = u32::try_from(owner) else {
            continue;
        };
        let Ok(producer_entity) = u32::try_from(record.producer_entity) else {
            continue;
        };
        records.push(PayloadIndexRecord {
            key_hash: qwen3_lingqu_key_hash(&record.key),
            owner_entity,
            producer_entity,
            version: record.version,
            bytes: record.bytes,
            checksum: record.checksum,
            payload: &record.payload_bytes,
        });
    }

    let header_bytes = W5_OBJECT_SERVICE_PAYLOAD_INDEX_HEADER_BYTES;
    let record_table_bytes = records
        .len()
        .checked_mul(W5_OBJECT_SERVICE_PAYLOAD_INDEX_RECORD_BYTES)
        .ok_or_else(|| anyhow::anyhow!("Object Service payload index record table overflow"))?;
    let payload_base = header_bytes
        .checked_add(record_table_bytes)
        .ok_or_else(|| anyhow::anyhow!("Object Service payload index payload base overflow"))?;
    let mut bytes = vec![0u8; payload_base];
    bytes[0..8].copy_from_slice(&W5_OBJECT_SERVICE_PAYLOAD_INDEX_MAGIC.to_le_bytes());
    bytes[8..12].copy_from_slice(&W5_OBJECT_SERVICE_PAYLOAD_INDEX_VERSION.to_le_bytes());
    bytes[12..16]
        .copy_from_slice(&(W5_OBJECT_SERVICE_PAYLOAD_INDEX_RECORD_BYTES as u32).to_le_bytes());
    bytes[16..24].copy_from_slice(&(records.len() as u64).to_le_bytes());

    let mut payload_offset = payload_base;
    for (index, record) in records.iter().enumerate() {
        let base = header_bytes + index * W5_OBJECT_SERVICE_PAYLOAD_INDEX_RECORD_BYTES;
        bytes[base..base + 8].copy_from_slice(&record.key_hash.to_le_bytes());
        bytes[base + 8..base + 12].copy_from_slice(&record.owner_entity.to_le_bytes());
        bytes[base + 12..base + 16].copy_from_slice(&record.producer_entity.to_le_bytes());
        bytes[base + 16..base + 24].copy_from_slice(&record.version.to_le_bytes());
        bytes[base + 24..base + 32].copy_from_slice(&record.bytes.to_le_bytes());
        bytes[base + 32..base + 40].copy_from_slice(&record.checksum.to_le_bytes());
        bytes[base + 40..base + 48].copy_from_slice(&(payload_offset as u64).to_le_bytes());
        bytes.extend_from_slice(record.payload);
        payload_offset = payload_offset
            .checked_add(record.payload.len())
            .ok_or_else(|| anyhow::anyhow!("Object Service payload index payload overflow"))?;
    }

    let sidecar_path = w5_object_service_payload_index_path(snapshot_path);
    fs::write(&sidecar_path, bytes).with_context(|| {
        format!(
            "write Object Service payload index {}",
            sidecar_path.display()
        )
    })?;
    Ok(())
}

fn w5_object_service_payload_index_path(snapshot_path: &Path) -> PathBuf {
    snapshot_path.with_extension("bin")
}

fn run_lingqu_memory_publish_w5_engram_state_ref_cli(args: &[String]) -> anyhow::Result<()> {
    let store_path = optional_cli_arg(args, "--store")?.map(PathBuf::from);
    let object_store_path = PathBuf::from(required_cli_arg(args, "--object-store")?);
    let engram_state_path = PathBuf::from(required_cli_arg(args, "--engram-state")?);
    let registry_dir = PathBuf::from(required_cli_arg(args, "--registry-dir")?);
    let owner_entity = optional_cli_u64(args, "--owner-entity")?.unwrap_or(0);
    let producer_entity = optional_cli_u64(args, "--producer-entity")?.unwrap_or(0);
    let owner_entity =
        u32::try_from(owner_entity).map_err(|_| anyhow::anyhow!("--owner-entity exceeds u32"))?;
    let producer_entity = u32::try_from(producer_entity)
        .map_err(|_| anyhow::anyhow!("--producer-entity exceeds u32"))?;
    let publication = if let Some(store_path) = store_path {
        publish_w5_engram_state_ref_from_memory(&W5MemoryBootstrapConfig {
            store_path,
            object_store_path: object_store_path.clone(),
            engram_state_path: engram_state_path.clone(),
            registry_dir: registry_dir.clone(),
            owner_entity,
            producer_entity,
        })?
    } else {
        let object_snapshot = load_lingqu_object_service_snapshot_file(&object_store_path)?
            .ok_or_else(|| {
                anyhow::anyhow!(
                    "object store snapshot does not exist: {}",
                    object_store_path.display()
                )
            })?;
        let object_service = LingquObjectServiceStub::import_snapshot(object_snapshot)
            .with_context(|| format!("import object store {}", object_store_path.display()))?;
        publish_w5_engram_state_ref_from_object_service(
            &object_service,
            &engram_state_path,
            &registry_dir,
            owner_entity,
            producer_entity,
        )?
    };

    println!("lingqu_memory_service");
    println!("  mode: publish-w5-engram-state-ref");
    println!("  object_store_path: {}", object_store_path.display());
    println!("  engram_state: {}", publication.engram_state_id);
    println!("  registry_dir: {}", registry_dir.display());
    println!(
        "  {}={}",
        SIM_QWEN3_GUEST_ENGRAM_STATE_REF, publication.state_ref_hex
    );
    println!(
        "  {}={}",
        SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR,
        registry_dir.display()
    );
    println!("  registry_table_bytes: {}", publication.table_bytes);
    println!("  registry_indices_bytes: {}", publication.indices_bytes);
    println!("  registry_gate_bytes: {}", publication.gate_bytes);
    println!(
        "  registry_state_manifest_bytes: {}",
        publication.state_manifest_bytes
    );
    Ok(())
}

fn run_lingqu_memory_build_index_cli(args: &[String]) -> anyhow::Result<()> {
    let catalog_path = PathBuf::from(required_cli_arg(args, "--catalog")?);
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let embedding_path = PathBuf::from(required_cli_arg(args, "--embedding-json")?);
    let index_id = required_cli_arg(args, "--index-id")?;
    let segment_id = required_cli_arg(args, "--segment-id")?;
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let snapshot =
        load_required_lingqu_memory_catalog_snapshot(args, &catalog_path, &mut durable_store)?;
    let embedding_bytes = fs::read(&embedding_path)
        .with_context(|| format!("read embedding json {}", embedding_path.display()))?;
    let embedding_input = serde_json::from_slice::<LingquMemoryEmbeddingInput>(&embedding_bytes)
        .with_context(|| format!("decode embedding json {}", embedding_path.display()))?;
    validate_lingqu_memory_embedding_input(&embedding_input)?;

    let mut memory_service = LingquMemoryService::new();
    memory_service
        .import_catalog_snapshot(snapshot.clone())
        .context("import catalog snapshot")?;

    let mut embedding_values =
        Vec::with_capacity(embedding_input.vectors.len() * embedding_input.dims as usize);
    let mut row_map = Vec::with_capacity(embedding_input.vectors.len());
    for (row, vector) in embedding_input.vectors.iter().enumerate() {
        if !snapshot
            .chunks
            .iter()
            .any(|chunk| chunk.chunk_id == vector.chunk_id)
        {
            anyhow::bail!(
                "embedding vector references unknown chunk `{}`",
                vector.chunk_id
            );
        }
        embedding_values.extend_from_slice(&vector.values);
        row_map.push(EmbeddingRow {
            chunk_id: vector.chunk_id.clone(),
            row: row as u32,
        });
    }
    let embedding_payload = cli_f32_vec_to_le_bytes(&embedding_values);
    let embedding_checksum = cli_bytes_checksum(&embedding_payload);
    let embedding_block = optional_cli_arg(args, "--embedding-block")?
        .unwrap_or_else(|| format!("block/memory/embedding/{}", cli_path_id(&segment_id)));
    let embedding_ref = durable_store
        .write_block_payload(embedding_block, embedding_payload)
        .context("write embedding payload to Lingqu Block store")?;
    let row_stride_bytes = embedding_input
        .dims
        .checked_mul(4)
        .ok_or_else(|| anyhow::anyhow!("embedding dims overflow row_stride_bytes"))?;
    let row_count = u32::try_from(embedding_input.vectors.len())
        .map_err(|_| anyhow::anyhow!("embedding vector count exceeds u32"))?;

    memory_service
        .register_embedding_segment(EmbeddingSegment {
            segment_id: segment_id.clone(),
            model_version: embedding_input.model_version.clone(),
            dims: embedding_input.dims,
            row_count,
            row_stride_bytes,
            dtype: sim_core::TensorDType::F32,
            vector_block_refs: vec![embedding_ref.clone()],
            row_map,
            checksum: embedding_checksum,
            version: 1,
        })
        .context("register embedding segment")?;

    let mut catalog = snapshot.catalog.clone();
    if !catalog.vector_index_ids.iter().any(|id| id == &index_id) {
        catalog.vector_index_ids.push(index_id.clone());
    }
    catalog.version = catalog.version.saturating_add(1);
    catalog.updated_at_us = now_us;
    memory_service
        .publish_catalog(catalog.clone())
        .context("publish updated catalog")?;
    memory_service
        .register_vector_index(VectorIndexObject {
            index_id: index_id.clone(),
            corpus_id: catalog.catalog_id.clone(),
            kind: VectorIndexKind::Flat,
            embedding_model_version: embedding_input.model_version.clone(),
            segment_ids: vec![segment_id.clone()],
            manifest_path: LingquDfsPath::new(format!(
                "/lingqu/memory/corpus/{}/index/{}.json",
                cli_path_id(&catalog.catalog_id),
                cli_path_id(&index_id)
            )),
            created_at_us: now_us,
            updated_at_us: now_us,
            version: 1,
        })
        .context("register vector index")?;
    let updated_snapshot = memory_service
        .export_catalog_snapshot(&catalog.catalog_id)
        .context("export updated catalog snapshot")?;

    write_lingqu_memory_catalog_snapshot(&catalog_path, &updated_snapshot)?;
    durable_store
        .persist_catalog_snapshot(&updated_snapshot)
        .context("persist catalog snapshot to durable DFS")?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;

    println!("lingqu_memory_service");
    println!("  mode: build-index");
    println!("  catalog: {}", catalog.catalog_id);
    println!("  catalog_path: {}", catalog_path.display());
    println!("  store_path: {}", store_path.display());
    println!("  index: {index_id}");
    println!("  segment: {segment_id}");
    println!(
        "  embedding_model_version: {}",
        embedding_input.model_version
    );
    println!("  rows: {}", embedding_input.vectors.len());
    println!("  dims: {}", embedding_input.dims);
    println!("  embedding_block: {}", embedding_ref.block.0);
    println!("  embedding_bytes: {}", embedding_ref.bytes);
    println!("  embedding_checksum: {:#x}", embedding_ref.checksum);
    Ok(())
}

fn validate_lingqu_memory_embedding_input(
    input: &LingquMemoryEmbeddingInput,
) -> anyhow::Result<()> {
    if input.model_version.trim().is_empty() {
        anyhow::bail!("embedding json model_version must not be empty");
    }
    if input.dims == 0 {
        anyhow::bail!("embedding json dims must be non-zero");
    }
    if input.vectors.is_empty() {
        anyhow::bail!("embedding json vectors must not be empty");
    }
    let dims = input.dims as usize;
    for vector in &input.vectors {
        if vector.chunk_id.trim().is_empty() {
            anyhow::bail!("embedding json vector chunk_id must not be empty");
        }
        if vector.values.len() != dims {
            anyhow::bail!(
                "embedding vector for `{}` has {} dims, expected {}",
                vector.chunk_id,
                vector.values.len(),
                dims
            );
        }
    }
    Ok(())
}

fn run_lingqu_memory_update_record_state_cli(args: &[String]) -> anyhow::Result<()> {
    let catalog_path = PathBuf::from(required_cli_arg(args, "--catalog")?);
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let catalog_id = required_cli_arg(args, "--catalog-id")?;
    let record_id = required_cli_arg(args, "--record-id")?;
    let state = required_cli_arg(args, "--state")?;
    let state = memory_record_state_from_cli(&state)?;
    let actor = required_cli_arg(args, "--actor")?;
    let reason = required_cli_arg(args, "--reason")?;
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let snapshot =
        load_required_lingqu_memory_catalog_snapshot(args, &catalog_path, &mut durable_store)?;
    let mut memory_service = LingquMemoryService::new();
    memory_service
        .import_catalog_snapshot(snapshot)
        .context("import catalog snapshot")?;
    let updated = memory_service
        .update_record_state(&catalog_id, &record_id, state, now_us, &actor, &reason)
        .context("update memory record state")?;
    let updated_snapshot = memory_service
        .export_catalog_snapshot(&catalog_id)
        .context("export updated catalog snapshot")?;

    write_lingqu_memory_catalog_snapshot(&catalog_path, &updated_snapshot)?;
    durable_store
        .persist_catalog_snapshot(&updated_snapshot)
        .context("persist catalog snapshot to durable DFS")?;
    memory_service
        .persist_record_lifecycle_events_to_dfs(&mut durable_store)
        .context("persist record lifecycle DFS audit")?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;

    println!("lingqu_memory_service");
    println!("  mode: update-record-state");
    println!("  catalog: {catalog_id}");
    println!("  catalog_path: {}", catalog_path.display());
    println!("  store_path: {}", store_path.display());
    println!("  record: {record_id}");
    println!("  state: {:?}", updated.state);
    println!("  record_version: {}", updated.version);
    println!("  actor: {actor}");
    println!("  reason: {reason}");
    println!("  updated_at_us: {}", updated.updated_at_us);
    Ok(())
}

fn memory_record_state_from_cli(value: &str) -> anyhow::Result<MemoryRecordState> {
    match value {
        "pending" => Ok(MemoryRecordState::Pending),
        "committed" => Ok(MemoryRecordState::Committed),
        "tombstoned" => Ok(MemoryRecordState::Tombstoned),
        "quarantined" => Ok(MemoryRecordState::Quarantined),
        _ => anyhow::bail!(
            "unknown memory record state `{value}`; expected pending, committed, tombstoned, or quarantined"
        ),
    }
}

fn run_lingqu_memory_ingest_cli(args: &[String]) -> anyhow::Result<()> {
    let catalog_path = PathBuf::from(required_cli_arg(args, "--catalog")?);
    let store_path = PathBuf::from(required_cli_arg(args, "--store")?);
    let source_path = PathBuf::from(required_cli_arg(args, "--source")?);
    let token_count = required_cli_u32(args, "--token-count")?;
    let now_us = optional_cli_u64(args, "--now-us")?.unwrap_or(1);
    let embedding_model_version = required_cli_arg(args, "--embedding-model-version")?;

    let source_bytes = fs::read(&source_path)
        .with_context(|| format!("read source file {}", source_path.display()))?;
    if source_bytes.is_empty() {
        anyhow::bail!("source file must not be empty: {}", source_path.display());
    }

    let mut durable_store = load_lingqu_memory_durable_store(&store_path)?;
    let source_name = source_path
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or("source");
    let source_block = optional_cli_arg(args, "--source-block")?
        .unwrap_or_else(|| format!("block/memory/source/{source_name}"));
    let text_block_ref = durable_store
        .write_block_payload(source_block, source_bytes)
        .context("write source payload to Lingqu Block store")?;

    let existing_snapshot = load_lingqu_memory_catalog_snapshot_from_file_or_store(
        args,
        &catalog_path,
        &mut durable_store,
    )?;
    let catalog_existed = existing_snapshot.is_some();
    let mut catalog = if let Some(snapshot) = existing_snapshot.as_ref() {
        if let Some(catalog_id) = optional_cli_arg(args, "--catalog-id")? {
            if catalog_id != snapshot.catalog.catalog_id {
                anyhow::bail!(
                    "catalog id mismatch: file has `{}`, args requested `{}`",
                    snapshot.catalog.catalog_id,
                    catalog_id
                );
            }
        }
        snapshot.catalog.clone()
    } else {
        let catalog_id = required_cli_arg(args, "--catalog-id")?;
        let namespace = required_cli_arg(args, "--namespace")?;
        MemoryCorpusCatalog {
            catalog_id: catalog_id.clone(),
            namespace,
            dfs_path: LingquDfsPath::new(format!(
                "/lingqu/memory/corpus/{}/catalog.json",
                cli_path_id(&catalog_id)
            )),
            version: 1,
            record_ids: Vec::new(),
            vector_index_ids: Vec::new(),
            created_at_us: now_us,
            updated_at_us: now_us,
        }
    };

    let record_id = optional_cli_arg(args, "--record-id")?
        .unwrap_or_else(|| format!("record/{:016x}", text_block_ref.checksum));
    let chunk_id = optional_cli_arg(args, "--chunk-id")?
        .unwrap_or_else(|| format!("chunk/{:016x}/0", text_block_ref.checksum));
    let content_type = memory_content_type_from_path(&source_path);

    let mut memory_service = LingquMemoryService::new();
    if let Some(snapshot) = existing_snapshot {
        memory_service
            .import_catalog_snapshot(snapshot)
            .context("import existing catalog snapshot")?;
    } else {
        memory_service
            .publish_catalog(catalog.clone())
            .context("publish new catalog")?;
    }

    if !catalog.record_ids.iter().any(|id| id == &record_id) {
        catalog.record_ids.push(record_id.clone());
    }
    if catalog_existed {
        catalog.version = catalog.version.saturating_add(1);
    }
    catalog.updated_at_us = now_us;

    let chunk = MemoryChunk {
        chunk_id: chunk_id.clone(),
        record_id: record_id.clone(),
        ordinal: 0,
        text_block_ref: text_block_ref.clone(),
        token_start: 0,
        token_count,
        checksum: text_block_ref.checksum,
    };
    let record = MemoryRecord {
        record_id: record_id.clone(),
        corpus_id: catalog.catalog_id.clone(),
        scope: MemoryScope::Project,
        visibility: MemoryVisibility::ProjectShared,
        source_kind: MemorySourceKind::UserProvided,
        source_uri: format!("file://{}", source_path.display()),
        source_checksum: text_block_ref.checksum,
        content_type,
        token_count,
        trust_level: MemoryTrustLevel::UserConfirmed,
        confidence: 1.0,
        retention_policy: MemoryRetentionPolicy::Durable,
        security_label: MemorySecurityLabel::Internal,
        pii_state: MemoryPiiState::Unknown,
        chunk_refs: vec![chunk_id.clone()],
        embedding_model_versions: vec![embedding_model_version],
        evidence_refs: vec![format!("file://{}", source_path.display())],
        created_at_us: now_us,
        updated_at_us: now_us,
        expires_at_us: None,
        version: 1,
        state: MemoryRecordState::Committed,
    };
    memory_service
        .ingest_record(record, vec![chunk])
        .context("ingest memory record")?;
    memory_service
        .publish_catalog(catalog.clone())
        .context("publish updated catalog")?;
    let snapshot = memory_service
        .export_catalog_snapshot(&catalog.catalog_id)
        .context("export updated catalog snapshot")?;

    write_lingqu_memory_catalog_snapshot(&catalog_path, &snapshot)?;
    durable_store
        .persist_catalog_snapshot(&snapshot)
        .context("persist catalog snapshot to durable DFS")?;
    save_lingqu_memory_durable_store(&store_path, &durable_store)?;

    println!("lingqu_memory_service");
    println!("  mode: ingest");
    println!("  catalog: {}", catalog.catalog_id);
    println!("  catalog_path: {}", catalog_path.display());
    println!("  store_path: {}", store_path.display());
    println!("  record: {record_id}");
    println!("  chunk: {chunk_id}");
    println!("  source_block: {}", text_block_ref.block.0);
    println!("  source_bytes: {}", text_block_ref.bytes);
    println!("  source_checksum: {:#x}", text_block_ref.checksum);
    println!("  token_count: {token_count}");
    Ok(())
}

fn required_cli_arg(args: &[String], name: &'static str) -> anyhow::Result<String> {
    optional_cli_arg(args, name)?.ok_or_else(|| anyhow::anyhow!("missing required argument {name}"))
}

fn optional_cli_arg(args: &[String], name: &'static str) -> anyhow::Result<Option<String>> {
    let mut index = 0usize;
    while index < args.len() {
        if args[index] == name {
            let value = args
                .get(index + 1)
                .ok_or_else(|| anyhow::anyhow!("missing value for argument {name}"))?;
            if value.starts_with("--") {
                anyhow::bail!("missing value for argument {name}");
            }
            return Ok(Some(value.clone()));
        }
        index += 1;
    }
    Ok(None)
}

fn cli_flag(args: &[String], name: &'static str) -> bool {
    args.iter().any(|arg| arg == name)
}

fn required_cli_u32(args: &[String], name: &'static str) -> anyhow::Result<u32> {
    let value = required_cli_arg(args, name)?;
    value
        .parse::<u32>()
        .with_context(|| format!("parse {name} as u32"))
}

fn required_cli_u64(args: &[String], name: &'static str) -> anyhow::Result<u64> {
    let value = required_cli_arg(args, name)?;
    value
        .parse::<u64>()
        .with_context(|| format!("parse {name} as u64"))
}

fn required_cli_u64_auto(args: &[String], name: &'static str) -> anyhow::Result<u64> {
    let value = required_cli_arg(args, name)?;
    parse_u64_auto(&value).with_context(|| format!("parse {name} as u64"))
}

fn optional_cli_u64(args: &[String], name: &'static str) -> anyhow::Result<Option<u64>> {
    optional_cli_arg(args, name)?
        .map(|value| {
            value
                .parse::<u64>()
                .with_context(|| format!("parse {name} as u64"))
        })
        .transpose()
}

fn parse_u64_auto(value: &str) -> anyhow::Result<u64> {
    if let Some(hex) = value
        .strip_prefix("0x")
        .or_else(|| value.strip_prefix("0X"))
    {
        u64::from_str_radix(hex, 16).with_context(|| format!("parse hex u64 {value}"))
    } else {
        value
            .parse::<u64>()
            .with_context(|| format!("parse decimal u64 {value}"))
    }
}

fn parse_shortpath_actions(value: &str) -> anyhow::Result<Vec<sim_memory::ShortpathAction>> {
    let mut actions = Vec::new();
    for item in value.split(',') {
        let item = item.trim();
        let action = match item {
            "continue" => sim_memory::ShortpathAction::Continue,
            "jump-to-layer" => sim_memory::ShortpathAction::JumpToLayer,
            "jump-to-terminal" => sim_memory::ShortpathAction::JumpToTerminal,
            "require-verify" => sim_memory::ShortpathAction::RequireVerify,
            "" => continue,
            _ => anyhow::bail!("unsupported shortpath action `{item}`"),
        };
        actions.push(action);
    }
    if actions.is_empty() {
        anyhow::bail!("shortpath actions must not be empty");
    }
    Ok(actions)
}

fn parse_node_index(value: &str) -> anyhow::Result<u32> {
    let raw = value
        .strip_prefix("node")
        .ok_or_else(|| anyhow::anyhow!("node must use nodeN form, got `{value}`"))?;
    let parsed = raw
        .parse::<u32>()
        .with_context(|| format!("parse node index from {value}"))?;
    if parsed == 0 {
        anyhow::bail!("node index must be one-based, got `{value}`");
    }
    Ok(parsed)
}

fn find_w5_boundary_observation(
    summary: &str,
    step: u64,
    node: &str,
) -> anyhow::Result<std::collections::HashMap<String, String>> {
    for line in summary.lines() {
        if !line.starts_with("memory_boundary_observation: ") {
            continue;
        }
        let fields = parse_summary_fields(line);
        if required_summary_u64(&fields, "step")? == step
            && required_summary_field(&fields, "node")? == node
        {
            return Ok(fields);
        }
    }
    anyhow::bail!("missing memory_boundary_observation for step={step} node={node}")
}

fn w5_boundary_observations_from_summary(
    summary: &str,
    run_id: &str,
    model: sim_memory::InferenceModelBinding,
    step: u64,
    position: u64,
    created_at_us: u64,
) -> anyhow::Result<Vec<sim_memory::BoundaryObservationRecord>> {
    let mut observations = Vec::new();
    for line in summary.lines() {
        if !line.starts_with("memory_boundary_observation: ") {
            continue;
        }
        let fields = parse_summary_fields(line);
        if required_summary_u64(&fields, "step")? != step {
            continue;
        }
        let node = required_summary_field(&fields, "node")?;
        let target = required_summary_field(&fields, "target")?;
        let layer_start = required_summary_u32(&fields, "layer_start")?;
        let layer_end = required_summary_u32(&fields, "layer_end")?;
        let hidden_key = required_summary_field(&fields, "hidden_key")?.to_string();
        let hidden_bytes = required_summary_u64(&fields, "hidden_bytes")?;
        let hidden_checksum = required_summary_u64_auto(&fields, "hidden_checksum")?;
        let hidden_version = required_summary_u64(&fields, "hidden_version")?;
        let observation_id = format!("boundary-observation/{run_id}/step{step}/{node}");
        let observation = sim_memory::BoundaryObservationRecord::new(
            observation_id,
            run_id.to_string(),
            model.clone(),
            sim_memory::RangeBoundary {
                phase: sim_memory::RangeBoundaryPhase::RangeExit,
                step_index: step,
                node_index: parse_node_index(node)?,
                layer_start,
                layer_end,
                next_node_index: Some(parse_node_index(target)?),
                position,
            },
            sim_memory::HotTensorObjectRef {
                object_key: hidden_key.clone(),
                version: hidden_version,
                backend: sim_memory::HotObjectBackend::ObmmShmem,
                storage_ref: format!("obmm://{hidden_key}"),
                segment: None,
                offset: 0,
                bytes: hidden_bytes,
                checksum: hidden_checksum,
                dtype: sim_core::TensorDType::Opaque,
                shape: vec![hidden_bytes],
            },
            node.to_string(),
            target.to_string(),
            "w5_guest_range_exit".to_string(),
            1,
            created_at_us,
        )
        .with_context(|| format!("build boundary observation step={step} node={node}"))?;
        observations.push(observation);
    }
    observations.sort_by(|left, right| left.observation_id.cmp(&right.observation_id));
    Ok(observations)
}

fn derive_w5_run_id_from_summary(summary: &str) -> Option<String> {
    let run_dir = summary.lines().find_map(|line| {
        line.strip_prefix("summary: run_dir=")
            .map(|value| value.trim())
    })?;
    let name = Path::new(run_dir).file_name()?.to_string_lossy();
    Some(
        name.strip_suffix("_headless8")
            .unwrap_or(name.as_ref())
            .to_string(),
    )
}

fn parse_summary_fields(line: &str) -> std::collections::HashMap<String, String> {
    line.split_ascii_whitespace()
        .filter_map(|field| field.split_once('='))
        .map(|(key, value)| (key.trim_end_matches(':').to_string(), value.to_string()))
        .collect()
}

fn required_summary_field<'a>(
    fields: &'a std::collections::HashMap<String, String>,
    name: &'static str,
) -> anyhow::Result<&'a str> {
    fields
        .get(name)
        .map(|value| value.as_str())
        .ok_or_else(|| anyhow::anyhow!("missing summary field {name}"))
}

fn required_summary_u64(
    fields: &std::collections::HashMap<String, String>,
    name: &'static str,
) -> anyhow::Result<u64> {
    required_summary_field(fields, name)?
        .parse::<u64>()
        .with_context(|| format!("parse summary field {name} as u64"))
}

fn required_summary_u32(
    fields: &std::collections::HashMap<String, String>,
    name: &'static str,
) -> anyhow::Result<u32> {
    required_summary_field(fields, name)?
        .parse::<u32>()
        .with_context(|| format!("parse summary field {name} as u32"))
}

fn required_summary_u64_auto(
    fields: &std::collections::HashMap<String, String>,
    name: &'static str,
) -> anyhow::Result<u64> {
    parse_u64_auto(required_summary_field(fields, name)?)
        .with_context(|| format!("parse summary field {name} as u64"))
}

fn load_lingqu_durable_snapshot(path: &Path) -> anyhow::Result<LingquDurableSimSnapshot> {
    let bytes = fs::read(path).with_context(|| format!("read durable store {}", path.display()))?;
    LingquDurableSimSnapshot::from_json_bytes(&bytes)
        .with_context(|| format!("decode durable store {}", path.display()))
}

fn save_lingqu_durable_sim(path: &Path, sim: &LingquDurableSim) -> anyhow::Result<()> {
    let snapshot = sim.export_snapshot().context("export durable snapshot")?;
    let bytes = snapshot
        .to_json_bytes()
        .context("encode durable snapshot")?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create durable store dir {}", parent.display()))?;
    }
    fs::write(path, bytes).with_context(|| format!("write durable store {}", path.display()))
}

fn load_lingqu_memory_durable_store(path: &Path) -> anyhow::Result<LingquMemoryDurableStore> {
    if !path.exists() {
        return Ok(LingquMemoryDurableStore::new());
    }
    let bytes = fs::read(path).with_context(|| format!("read durable store {}", path.display()))?;
    if let Ok(snapshot) = LingquDurableSimSnapshot::from_json_bytes(&bytes) {
        return LingquMemoryDurableStore::import_durable_sim_snapshot(snapshot)
            .with_context(|| format!("import durable sim store {}", path.display()));
    }
    let legacy_snapshot = LingquMemoryDurableStoreSnapshot::from_json_bytes(&bytes)
        .with_context(|| format!("decode durable store {}", path.display()))?;
    LingquMemoryDurableStore::import_snapshot(legacy_snapshot)
        .with_context(|| format!("import legacy durable store {}", path.display()))
}

fn save_lingqu_memory_durable_store(
    path: &Path,
    store: &LingquMemoryDurableStore,
) -> anyhow::Result<()> {
    let snapshot = store
        .export_durable_sim_snapshot()
        .context("export durable sim store")?;
    let bytes = snapshot
        .to_json_bytes()
        .context("encode durable sim store")?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create durable store dir {}", parent.display()))?;
    }
    fs::write(path, bytes).with_context(|| format!("write durable store {}", path.display()))
}

fn rebuild_lingqu_memory_execution_registry_artifacts(
    memory_service: &mut LingquMemoryService,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<LingquMemoryExecutionArtifactRegistry> {
    match memory_service.rebuild_execution_artifacts_from_dfs(store) {
        Ok(artifacts) => Ok(LingquMemoryExecutionArtifactRegistry { artifacts }),
        Err(sim_memory::LingquMemoryError::MissingDfsPath(_)) => {
            Ok(LingquMemoryExecutionArtifactRegistry::default())
        }
        Err(err) => {
            Err(err).context("rebuild execution artifact registry from durable DFS manifest")
        }
    }
}

fn load_required_lingqu_memory_execution_registry_artifacts(
    memory_service: &mut LingquMemoryService,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<LingquMemoryExecutionArtifactRegistry> {
    let artifacts = memory_service
        .rebuild_execution_artifacts_from_dfs(store)
        .context("rebuild execution artifact registry from durable DFS manifest")?;
    Ok(LingquMemoryExecutionArtifactRegistry { artifacts })
}

fn rebuild_lingqu_memory_prefix_cache_registry_artifacts(
    memory_service: &mut LingquMemoryService,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<LingquMemoryPrefixCacheRegistry> {
    match memory_service.rebuild_prefix_cache_artifacts_from_dfs(store) {
        Ok(artifacts) => Ok(LingquMemoryPrefixCacheRegistry { artifacts }),
        Err(sim_memory::LingquMemoryError::MissingDfsPath(_)) => {
            Ok(LingquMemoryPrefixCacheRegistry::default())
        }
        Err(err) => Err(err).context("rebuild prefix cache registry from durable DFS manifest"),
    }
}

fn load_required_lingqu_memory_prefix_cache_registry_artifacts(
    memory_service: &mut LingquMemoryService,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<LingquMemoryPrefixCacheRegistry> {
    let artifacts = memory_service
        .rebuild_prefix_cache_artifacts_from_dfs(store)
        .context("rebuild prefix cache registry from durable DFS manifest")?;
    Ok(LingquMemoryPrefixCacheRegistry { artifacts })
}

fn rebuild_lingqu_memory_shortpath_supports(
    memory_service: &mut LingquMemoryService,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<()> {
    match memory_service.rebuild_shortpath_supports_from_dfs(store) {
        Ok(_) | Err(sim_memory::LingquMemoryError::MissingDfsPath(_)) => Ok(()),
        Err(err) => Err(err).context("rebuild shortpath support audit from durable DFS manifest"),
    }
}

fn rebuild_lingqu_memory_prefetch_plans(
    memory_service: &mut LingquMemoryService,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<()> {
    match memory_service.rebuild_prefetch_plans_from_dfs(store) {
        Ok(_) | Err(sim_memory::LingquMemoryError::MissingDfsPath(_)) => Ok(()),
        Err(err) => Err(err).context("rebuild prefetch plan audit from durable DFS manifest"),
    }
}

fn rebuild_lingqu_memory_prefix_cache_reuse_plans(
    memory_service: &mut LingquMemoryService,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<()> {
    match memory_service.rebuild_prefix_cache_reuse_plans_from_dfs(store) {
        Ok(_) | Err(sim_memory::LingquMemoryError::MissingDfsPath(_)) => Ok(()),
        Err(err) => Err(err).context("rebuild prefix cache reuse audit from durable DFS manifest"),
    }
}

fn load_lingqu_object_service_snapshot(
    path: &Path,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<Option<LingquObjectServiceSnapshot>> {
    match store.load_object_service_checkpoint() {
        Ok(snapshot) => return Ok(Some(snapshot)),
        Err(sim_memory::LingquMemoryError::MissingDfsPath(_)) => {}
        Err(err) => return Err(err).context("load object service checkpoint from durable DFS"),
    }
    load_lingqu_object_service_snapshot_file(path)
}

fn load_lingqu_object_service_snapshot_file(
    path: &Path,
) -> anyhow::Result<Option<LingquObjectServiceSnapshot>> {
    if !path.exists() {
        return Ok(None);
    }
    let bytes = fs::read(path).with_context(|| format!("read object store {}", path.display()))?;
    let snapshot = LingquObjectServiceSnapshot::from_json_bytes(&bytes)
        .with_context(|| format!("decode object store {}", path.display()))?;
    Ok(Some(snapshot))
}

fn save_lingqu_object_service_snapshot(
    path: &Path,
    store: &mut LingquMemoryDurableStore,
    service: &LingquObjectServiceStub,
) -> anyhow::Result<()> {
    let _ = path;
    store
        .persist_object_service_checkpoint(service)
        .context("persist object service checkpoint to durable DFS")?;
    Ok(())
}

fn load_lingqu_memory_catalog_snapshot_if_exists(
    path: &Path,
) -> anyhow::Result<Option<MemoryCatalogSnapshot>> {
    if !path.exists() {
        return Ok(None);
    }
    let bytes = fs::read(path).with_context(|| format!("read catalog {}", path.display()))?;
    let snapshot = MemoryCatalogSnapshot::from_json_bytes(&bytes)
        .with_context(|| format!("decode catalog {}", path.display()))?;
    Ok(Some(snapshot))
}

fn load_lingqu_memory_catalog_snapshot_from_file_or_store(
    args: &[String],
    path: &Path,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<Option<MemoryCatalogSnapshot>> {
    if let Some(snapshot) = load_lingqu_memory_catalog_snapshot_if_exists(path)? {
        return Ok(Some(snapshot));
    }
    let Some(catalog_id) = optional_cli_arg(args, "--catalog-id")? else {
        return Ok(None);
    };
    let dfs_path = catalog_dfs_path_from_id(&catalog_id);
    match store.load_catalog_snapshot(&dfs_path) {
        Ok(snapshot) => Ok(Some(snapshot)),
        Err(sim_memory::LingquMemoryError::MissingDfsPath(_)) => Ok(None),
        Err(err) => Err(err)
            .with_context(|| format!("load catalog snapshot {} from durable store", dfs_path.path)),
    }
}

fn load_required_lingqu_memory_catalog_snapshot(
    args: &[String],
    path: &Path,
    store: &mut LingquMemoryDurableStore,
) -> anyhow::Result<MemoryCatalogSnapshot> {
    load_lingqu_memory_catalog_snapshot_from_file_or_store(args, path, store)?.ok_or_else(|| {
        anyhow::anyhow!(
            "catalog snapshot does not exist: {}; pass --catalog-id to load it from durable store",
            path.display()
        )
    })
}

fn catalog_dfs_path_from_id(catalog_id: &str) -> LingquDfsPath {
    LingquDfsPath::new(format!(
        "/lingqu/memory/corpus/{}/catalog.json",
        cli_path_id(catalog_id)
    ))
}

fn write_lingqu_memory_catalog_snapshot(
    path: &Path,
    snapshot: &MemoryCatalogSnapshot,
) -> anyhow::Result<()> {
    let bytes = snapshot.to_json_bytes().context("encode catalog")?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create catalog dir {}", parent.display()))?;
    }
    fs::write(path, bytes).with_context(|| format!("write catalog {}", path.display()))
}

fn memory_content_type_from_path(path: &Path) -> MemoryContentType {
    match path.extension().and_then(|extension| extension.to_str()) {
        Some("md") | Some("markdown") => MemoryContentType::Markdown,
        Some("json") => MemoryContentType::Json,
        Some("txt") | Some("text") => MemoryContentType::PlainText,
        _ => MemoryContentType::Binary,
    }
}

fn cli_path_id(id: &str) -> String {
    id.chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' {
                ch
            } else {
                '_'
            }
        })
        .collect()
}

fn cli_bytes_checksum(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325u64;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

fn build_lingqu_memory_cli_sample() -> anyhow::Result<LingquMemoryService> {
    let mut memory_service = LingquMemoryService::new();
    memory_service.publish_catalog(MemoryCorpusCatalog {
        catalog_id: "corpus/default".to_string(),
        namespace: "project/default".to_string(),
        dfs_path: LingquDfsPath::new("/lingqu/memory/corpus/default/catalog.json"),
        version: 1,
        record_ids: vec!["record/default/0".to_string()],
        vector_index_ids: vec!["index/default/flat".to_string()],
        created_at_us: 1,
        updated_at_us: 1,
    })?;
    memory_service.ingest_record(
        MemoryRecord {
            record_id: "record/default/0".to_string(),
            corpus_id: "corpus/default".to_string(),
            scope: MemoryScope::Project,
            visibility: MemoryVisibility::ProjectShared,
            source_kind: MemorySourceKind::UserProvided,
            source_uri: "dfs://lingqu/memory/source/default.md".to_string(),
            source_checksum: 0x1001,
            content_type: MemoryContentType::Markdown,
            token_count: 32,
            trust_level: MemoryTrustLevel::UserConfirmed,
            confidence: 0.95,
            retention_policy: MemoryRetentionPolicy::Durable,
            security_label: MemorySecurityLabel::Internal,
            pii_state: MemoryPiiState::None,
            chunk_refs: vec!["chunk/default/0".to_string()],
            embedding_model_versions: vec!["embed/default/v1".to_string()],
            evidence_refs: vec!["import://lingqu-memory-cli/default".to_string()],
            created_at_us: 1,
            updated_at_us: 1,
            expires_at_us: None,
            version: 1,
            state: MemoryRecordState::Committed,
        },
        vec![MemoryChunk {
            chunk_id: "chunk/default/0".to_string(),
            record_id: "record/default/0".to_string(),
            ordinal: 0,
            text_block_ref: LingquBlockPayloadRef::new("block/text/default/0", 0, 128, 0x2002),
            token_start: 0,
            token_count: 32,
            checksum: 0x3003,
        }],
    )?;
    memory_service.register_embedding_segment(EmbeddingSegment {
        segment_id: "segment/default/0".to_string(),
        model_version: "embed/default/v1".to_string(),
        dims: 4,
        row_count: 1,
        row_stride_bytes: 16,
        dtype: sim_core::TensorDType::F32,
        vector_block_refs: vec![LingquBlockPayloadRef::new(
            "block/embed/default/0",
            0,
            16,
            0x4004,
        )],
        row_map: vec![EmbeddingRow {
            chunk_id: "chunk/default/0".to_string(),
            row: 0,
        }],
        checksum: 0x5005,
        version: 1,
    })?;
    memory_service.register_vector_index(VectorIndexObject {
        index_id: "index/default/flat".to_string(),
        corpus_id: "corpus/default".to_string(),
        kind: VectorIndexKind::Flat,
        embedding_model_version: "embed/default/v1".to_string(),
        segment_ids: vec!["segment/default/0".to_string()],
        manifest_path: LingquDfsPath::new("/lingqu/memory/corpus/default/index/flat.json"),
        created_at_us: 2,
        updated_at_us: 2,
        version: 1,
    })?;
    Ok(memory_service)
}

fn run_lingqu_memory_validate_service_path() -> anyhow::Result<()> {
    let mut memory_service = build_lingqu_memory_cli_sample()?;

    let query_result = memory_service.query_memory(
        MemoryQuery {
            query_id: "query/default/0".to_string(),
            corpus_ids: vec!["corpus/default".to_string()],
            scope_filter: vec![MemoryScope::Project],
            visibility_filter: vec![MemoryVisibility::ProjectShared],
            min_trust: MemoryTrustLevel::UserConfirmed,
            min_confidence: 0.5,
            embedding_model_version: "embed/default/v1".to_string(),
            top_k: 1,
            query_embedding_ref: None,
        },
        100,
    )?;
    let mut object_service = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
    let hot_state = memory_service.materialize_hot_state(
        &mut object_service,
        HotMemoryMaterializeReq {
            state_id: "hot/default/0".to_string(),
            query_result_id: query_result.result_id.clone(),
            query_result_manifest_ref: None,
            table_shape: vec![1, 4],
            table_values: vec![0.1, 0.2, 0.3, 0.4],
            indices: vec![0],
            owner_entity: 0,
            producer_entity: 0,
            now_us: 200,
        },
    )?;
    let engram_state = memory_service.build_engram_state(
        "engram/default/0",
        &hot_state.state_id,
        None,
        Vec::new(),
        300,
        None,
    )?;
    let object_report = object_service.report();

    println!("lingqu_memory_service");
    println!("  mode: validate-service-path");
    println!("  query_result: {}", query_result.result_id);
    println!("  matches: {}", query_result.matches.len());
    println!("  hot_state: {}", hot_state.state_id);
    println!("  hot_table_object: {}", hot_state.table.object_key);
    println!("  hot_indices_object: {}", hot_state.indices.object_key);
    println!("  engram_state: {}", engram_state.state_id);
    println!(
        "  obmm_payload_writes: {}",
        object_report.obmm_pool_payload_write_count
    );
    println!(
        "  committed_object_count: {}",
        object_report.committed_object_count
    );
    Ok(())
}

fn run_lingqu_memory_validate_durable_store() -> anyhow::Result<()> {
    let memory_service = build_lingqu_memory_cli_sample()?;
    let mut durable_store = LingquMemoryDurableStore::new();
    let catalog_path =
        memory_service.persist_catalog_to_dfs(&mut durable_store, "corpus/default")?;
    let mut restored_service = LingquMemoryService::new();
    let restored_catalog =
        restored_service.rebuild_catalog_from_dfs(&mut durable_store, &catalog_path)?;
    let restored = restored_service.export_catalog_snapshot(&restored_catalog.catalog_id)?;
    let chunk_payload_ref = durable_store.write_block_payload(
        "block/text/default/0",
        b"durable lingqu memory chunk payload".to_vec(),
    )?;
    let chunk_payload = durable_store.read_block_payload(&chunk_payload_ref)?;
    let embedding_payload_ref = durable_store.write_block_payload(
        "block/embed/default/0",
        vec![
            0, 0, 0, 0, 205, 204, 204, 61, 205, 204, 76, 62, 154, 153, 153, 62,
        ],
    )?;
    let embedding_payload = durable_store.read_block_payload(&embedding_payload_ref)?;
    let stats = durable_store.stats();

    println!("lingqu_memory_service");
    println!("  mode: validate-durable-store");
    println!("  catalog_path: {}", catalog_path.path);
    println!("  restored_records: {}", restored.records.len());
    println!("  restored_chunks: {}", restored.chunks.len());
    println!(
        "  restored_embedding_segments: {}",
        restored.embedding_segments.len()
    );
    println!("  chunk_block_ref: {}", chunk_payload_ref.block.0);
    println!("  chunk_payload_bytes: {}", chunk_payload.len());
    println!("  embedding_block_ref: {}", embedding_payload_ref.block.0);
    println!("  embedding_payload_bytes: {}", embedding_payload.len());
    println!("  dfs_catalog_writes: {}", stats.dfs_catalog_writes);
    println!("  dfs_catalog_reads: {}", stats.dfs_catalog_reads);
    println!("  block_payload_writes: {}", stats.block_payload_writes);
    println!("  block_payload_reads: {}", stats.block_payload_reads);
    Ok(())
}

fn build_lingqu_memory_flat_query_sample(
) -> anyhow::Result<(LingquMemoryService, LingquMemoryDurableStore, QueryResult)> {
    let mut memory_service = LingquMemoryService::new();
    memory_service.publish_catalog(MemoryCorpusCatalog {
        catalog_id: "corpus/flat".to_string(),
        namespace: "project/default".to_string(),
        dfs_path: LingquDfsPath::new("/lingqu/memory/corpus/flat/catalog.json"),
        version: 1,
        record_ids: vec!["record/flat/a".to_string(), "record/flat/b".to_string()],
        vector_index_ids: vec!["index/flat".to_string()],
        created_at_us: 1,
        updated_at_us: 1,
    })?;
    for (record_id, chunk_id) in [
        ("record/flat/a", "chunk/flat/a"),
        ("record/flat/b", "chunk/flat/b"),
    ] {
        memory_service.ingest_record(
            MemoryRecord {
                record_id: record_id.to_string(),
                corpus_id: "corpus/flat".to_string(),
                scope: MemoryScope::Project,
                visibility: MemoryVisibility::ProjectShared,
                source_kind: MemorySourceKind::UserProvided,
                source_uri: format!("dfs://lingqu/memory/source/{record_id}.md"),
                source_checksum: 0x1001,
                content_type: MemoryContentType::Markdown,
                token_count: 16,
                trust_level: MemoryTrustLevel::UserConfirmed,
                confidence: 0.95,
                retention_policy: MemoryRetentionPolicy::Durable,
                security_label: MemorySecurityLabel::Internal,
                pii_state: MemoryPiiState::None,
                chunk_refs: vec![chunk_id.to_string()],
                embedding_model_versions: vec!["embed/default/v1".to_string()],
                evidence_refs: vec!["import://lingqu-memory-cli/flat".to_string()],
                created_at_us: 1,
                updated_at_us: 1,
                expires_at_us: None,
                version: 1,
                state: MemoryRecordState::Committed,
            },
            vec![MemoryChunk {
                chunk_id: chunk_id.to_string(),
                record_id: record_id.to_string(),
                ordinal: 0,
                text_block_ref: LingquBlockPayloadRef::new(
                    format!("block/text/{chunk_id}"),
                    0,
                    64,
                    0x2002,
                ),
                token_start: 0,
                token_count: 16,
                checksum: 0x3003,
            }],
        )?;
    }

    let mut durable_store = LingquMemoryDurableStore::new();
    let segment_ref = durable_store.write_block_payload(
        "block/embed/flat",
        cli_f32_vec_to_le_bytes(&[1.0, 0.0, 0.0, 1.0]),
    )?;
    let query_ref = durable_store
        .write_block_payload("block/query/flat", cli_f32_vec_to_le_bytes(&[0.0, 1.0]))?;
    memory_service.register_embedding_segment(EmbeddingSegment {
        segment_id: "segment/flat".to_string(),
        model_version: "embed/default/v1".to_string(),
        dims: 2,
        row_count: 2,
        row_stride_bytes: 8,
        dtype: sim_core::TensorDType::F32,
        vector_block_refs: vec![segment_ref],
        row_map: vec![
            EmbeddingRow {
                chunk_id: "chunk/flat/a".to_string(),
                row: 0,
            },
            EmbeddingRow {
                chunk_id: "chunk/flat/b".to_string(),
                row: 1,
            },
        ],
        checksum: 0x5005,
        version: 1,
    })?;
    memory_service.register_vector_index(VectorIndexObject {
        index_id: "index/flat".to_string(),
        corpus_id: "corpus/flat".to_string(),
        kind: VectorIndexKind::Flat,
        embedding_model_version: "embed/default/v1".to_string(),
        segment_ids: vec!["segment/flat".to_string()],
        manifest_path: LingquDfsPath::new("/lingqu/memory/corpus/flat/index.json"),
        created_at_us: 2,
        updated_at_us: 2,
        version: 1,
    })?;
    let result = memory_service.query_memory_flat(
        &mut durable_store,
        MemoryQuery {
            query_id: "query/flat".to_string(),
            corpus_ids: vec!["corpus/flat".to_string()],
            scope_filter: vec![MemoryScope::Project],
            visibility_filter: vec![MemoryVisibility::ProjectShared],
            min_trust: MemoryTrustLevel::UserConfirmed,
            min_confidence: 0.5,
            embedding_model_version: "embed/default/v1".to_string(),
            top_k: 1,
            query_embedding_ref: Some(query_ref),
        },
        100,
    )?;
    Ok((memory_service, durable_store, result))
}

fn run_lingqu_memory_validate_flat_query() -> anyhow::Result<()> {
    let (_memory_service, mut durable_store, result) = build_lingqu_memory_flat_query_sample()?;
    let query_result_path = durable_store.persist_query_result(&result)?;
    let stats = durable_store.stats();

    println!("lingqu_memory_service");
    println!("  mode: validate-flat-query");
    println!("  query_result: {}", result.result_id);
    println!("  query_result_manifest: {}", query_result_path.path);
    println!("  query_result_version: {}", result.version);
    println!("  query_result_checksum: {:#x}", result.checksum);
    println!("  vector_indexes: {}", result.vector_index_ids.join(","));
    println!(
        "  segment_versions: {}",
        result
            .embedding_segment_versions
            .iter()
            .map(|segment| format!(
                "{}@{}:{:#x}",
                segment.segment_id, segment.version, segment.checksum
            ))
            .collect::<Vec<_>>()
            .join(",")
    );
    println!(
        "  selected_records: {}",
        result.selected_record_ids.join(",")
    );
    println!("  matches: {}", result.matches.len());
    if let Some(top) = result.matches.first() {
        println!("  top_record: {}", top.record_id);
        println!("  top_chunk: {}", top.chunk_id);
        println!("  top_score: {:.6}", top.score);
    }
    println!("  block_payload_reads: {}", stats.block_payload_reads);
    Ok(())
}

fn run_lingqu_memory_validate_flat_materialize() -> anyhow::Result<()> {
    let (mut memory_service, mut durable_store, result) = build_lingqu_memory_flat_query_sample()?;
    let mut object_service = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
    let hot_state = memory_service.materialize_hot_state_from_query(
        &mut durable_store,
        &mut object_service,
        HotMemoryMaterializeFromQueryReq {
            state_id: "hot/flat".to_string(),
            query_result_id: result.result_id.clone(),
            owner_entity: 0,
            producer_entity: 0,
            now_us: 200,
        },
    )?;
    let query_result_path = hot_state
        .query_result_manifest_ref
        .as_ref()
        .context("missing hot query result manifest ref")?;
    let engram_state = memory_service.build_engram_state(
        "engram/flat",
        &hot_state.state_id,
        None,
        Vec::new(),
        300,
        None,
    )?;
    let object_report = object_service.report();
    let stats = durable_store.stats();

    println!("lingqu_memory_service");
    println!("  mode: validate-flat-materialize");
    println!("  query_result: {}", result.result_id);
    println!("  query_result_manifest: {}", query_result_path.path);
    println!("  query_result_version: {}", result.version);
    println!("  query_result_checksum: {:#x}", result.checksum);
    println!("  vector_indexes: {}", result.vector_index_ids.join(","));
    println!(
        "  segment_versions: {}",
        result
            .embedding_segment_versions
            .iter()
            .map(|segment| format!(
                "{}@{}:{:#x}",
                segment.segment_id, segment.version, segment.checksum
            ))
            .collect::<Vec<_>>()
            .join(",")
    );
    println!(
        "  selected_records: {}",
        result.selected_record_ids.join(",")
    );
    println!("  matches: {}", result.matches.len());
    println!("  hot_state: {}", hot_state.state_id);
    println!("  hot_table_object: {}", hot_state.table.object_key);
    println!("  hot_table_shape: {:?}", hot_state.table.shape);
    println!("  hot_indices_object: {}", hot_state.indices.object_key);
    println!("  hot_scores_object: {}", hot_state.scores.object_key);
    println!(
        "  selected_chunks: {}",
        hot_state.selected_chunk_ids.join(",")
    );
    println!("  engram_state: {}", engram_state.state_id);
    println!("  block_payload_reads: {}", stats.block_payload_reads);
    println!(
        "  obmm_payload_writes: {}",
        object_report.obmm_pool_payload_write_count
    );
    println!(
        "  committed_object_count: {}",
        object_report.committed_object_count
    );
    Ok(())
}

fn run_lingqu_memory_validate_w5_engram_object_ref() -> anyhow::Result<()> {
    const W5_ENGRAM_ROWS: usize = 8;
    const W5_ENGRAM_HIDDEN_SIZE: usize = 1024;

    let mut memory_service = LingquMemoryService::new();
    let record_ids = (0..W5_ENGRAM_ROWS)
        .map(|row| format!("record/w5/engram/{row}"))
        .collect::<Vec<_>>();
    memory_service.publish_catalog(MemoryCorpusCatalog {
        catalog_id: "corpus/w5/engram".to_string(),
        namespace: "project/default".to_string(),
        dfs_path: LingquDfsPath::new("/lingqu/memory/corpus/w5/engram/catalog.json"),
        version: 1,
        record_ids: record_ids.clone(),
        vector_index_ids: vec!["index/w5/engram/flat".to_string()],
        created_at_us: 1,
        updated_at_us: 1,
    })?;
    for (row, record_id) in record_ids.iter().enumerate() {
        let chunk_id = format!("chunk/w5/engram/{row}");
        memory_service.ingest_record(
            MemoryRecord {
                record_id: record_id.clone(),
                corpus_id: "corpus/w5/engram".to_string(),
                scope: MemoryScope::Project,
                visibility: MemoryVisibility::ProjectShared,
                source_kind: MemorySourceKind::UserProvided,
                source_uri: format!("dfs://lingqu/memory/source/w5/engram/{row}.md"),
                source_checksum: 0x1001 + row as u64,
                content_type: MemoryContentType::Markdown,
                token_count: 32,
                trust_level: MemoryTrustLevel::UserConfirmed,
                confidence: 0.95,
                retention_policy: MemoryRetentionPolicy::Durable,
                security_label: MemorySecurityLabel::Internal,
                pii_state: MemoryPiiState::None,
                chunk_refs: vec![chunk_id.clone()],
                embedding_model_versions: vec!["embed/w5/engram/v1".to_string()],
                evidence_refs: vec![format!("import://lingqu-memory-cli/w5/engram/{row}")],
                created_at_us: 1,
                updated_at_us: 1,
                expires_at_us: None,
                version: 1,
                state: MemoryRecordState::Committed,
            },
            vec![MemoryChunk {
                chunk_id,
                record_id: record_id.clone(),
                ordinal: 0,
                text_block_ref: LingquBlockPayloadRef::new(
                    format!("block/text/w5/engram/{row}"),
                    0,
                    128,
                    0x2002 + row as u64,
                ),
                token_start: 0,
                token_count: 32,
                checksum: 0x3003 + row as u64,
            }],
        )?;
    }

    let mut durable_store = LingquMemoryDurableStore::new();
    let mut embedding_values = Vec::with_capacity(W5_ENGRAM_ROWS * W5_ENGRAM_HIDDEN_SIZE);
    for row in 0..W5_ENGRAM_ROWS {
        for dim in 0..W5_ENGRAM_HIDDEN_SIZE {
            embedding_values.push(((row * 31 + dim * 17) % 257) as f32 / 8192.0);
        }
    }
    let segment_ref = durable_store.write_block_payload(
        "block/embed/w5/engram/table",
        cli_f32_vec_to_le_bytes(&embedding_values),
    )?;
    let query_values = (0..W5_ENGRAM_HIDDEN_SIZE)
        .map(|dim| ((dim % 23) as f32 + 1.0) / 1024.0)
        .collect::<Vec<_>>();
    let query_ref = durable_store.write_block_payload(
        "block/query/w5/engram",
        cli_f32_vec_to_le_bytes(&query_values),
    )?;
    memory_service.register_embedding_segment(EmbeddingSegment {
        segment_id: "segment/w5/engram/table".to_string(),
        model_version: "embed/w5/engram/v1".to_string(),
        dims: W5_ENGRAM_HIDDEN_SIZE as u32,
        row_count: W5_ENGRAM_ROWS as u32,
        row_stride_bytes: (W5_ENGRAM_HIDDEN_SIZE * std::mem::size_of::<f32>()) as u32,
        dtype: sim_core::TensorDType::F32,
        vector_block_refs: vec![segment_ref],
        row_map: (0..W5_ENGRAM_ROWS)
            .map(|row| EmbeddingRow {
                chunk_id: format!("chunk/w5/engram/{row}"),
                row: row as u32,
            })
            .collect(),
        checksum: 0x5005,
        version: 1,
    })?;
    memory_service.register_vector_index(VectorIndexObject {
        index_id: "index/w5/engram/flat".to_string(),
        corpus_id: "corpus/w5/engram".to_string(),
        kind: VectorIndexKind::Flat,
        embedding_model_version: "embed/w5/engram/v1".to_string(),
        segment_ids: vec!["segment/w5/engram/table".to_string()],
        manifest_path: LingquDfsPath::new("/lingqu/memory/corpus/w5/engram/index.json"),
        created_at_us: 2,
        updated_at_us: 2,
        version: 1,
    })?;
    let result = memory_service.query_memory_flat(
        &mut durable_store,
        MemoryQuery {
            query_id: "query/w5/engram".to_string(),
            corpus_ids: vec!["corpus/w5/engram".to_string()],
            scope_filter: vec![MemoryScope::Project],
            visibility_filter: vec![MemoryVisibility::ProjectShared],
            min_trust: MemoryTrustLevel::UserConfirmed,
            min_confidence: 0.5,
            embedding_model_version: "embed/w5/engram/v1".to_string(),
            top_k: W5_ENGRAM_ROWS,
            query_embedding_ref: Some(query_ref),
        },
        100,
    )?;
    let mut object_service = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
    let hot_state = memory_service.materialize_hot_state_from_query(
        &mut durable_store,
        &mut object_service,
        HotMemoryMaterializeFromQueryReq {
            state_id: "hot/w5/engram".to_string(),
            query_result_id: result.result_id.clone(),
            owner_entity: 0,
            producer_entity: 0,
            now_us: 200,
        },
    )?;
    let query_result_path = hot_state
        .query_result_manifest_ref
        .as_ref()
        .context("missing hot query result manifest ref")?;
    let gate_values = (0..W5_ENGRAM_HIDDEN_SIZE)
        .map(|dim| ((dim % 29) as f32 - 14.0) / 16384.0)
        .collect::<Vec<_>>();
    let gate_weight_ref = durable_store.write_block_payload(
        "block/engram/w5/object-ref/gate_weight",
        cli_f32_vec_to_le_bytes(&gate_values),
    )?;
    let gate_weight_block = gate_weight_ref.block.0.clone();
    let engram_state = memory_service.materialize_engram_state_from_block(
        &mut durable_store,
        &mut object_service,
        EngramStateMaterializeFromBlockReq {
            state_id: "engram/w5/object-ref".to_string(),
            hot_memory_state_id: hot_state.state_id.clone(),
            gate_weight_ref,
            compatible_models: Vec::new(),
            owner_entity: 0,
            producer_entity: 0,
            now_us: 300,
            expires_at_us: None,
        },
    )?;
    let table_payload = object_service
        .get_copy(
            &hot_state.table.object_key,
            LingquObjectVersionSelector::LatestCommitted,
        )
        .context("missing hot table payload")?;
    let indices_payload = object_service
        .get_copy(
            &hot_state.indices.object_key,
            LingquObjectVersionSelector::LatestCommitted,
        )
        .context("missing hot indices payload")?;
    let gate = engram_state
        .gate
        .as_ref()
        .context("missing materialized engram gate object")?;
    let gate_payload = object_service
        .get_copy(
            &gate.object_key,
            LingquObjectVersionSelector::LatestCommitted,
        )
        .context("missing hot gate payload")?;
    let table_ref = qwen3_publish_object_registry_payload(
        QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_TABLE,
        0,
        0,
        &hot_state.table.object_key,
        &table_payload,
    )
    .map_err(anyhow::Error::msg)?;
    let indices_ref = qwen3_publish_object_registry_payload(
        QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_INDICES,
        0,
        0,
        &hot_state.indices.object_key,
        &indices_payload,
    )
    .map_err(anyhow::Error::msg)?;
    let gate_ref = qwen3_publish_object_registry_payload(
        QWEN3_DENSE_PROFILE_OBMM_KIND_ENGRAM_CONTEXT_GATE_WEIGHT,
        0,
        0,
        &gate.object_key,
        &gate_payload,
    )
    .map_err(anyhow::Error::msg)?;
    let state_ref = qwen3_publish_engram_state_registry_payload(
        0,
        0,
        &engram_state.state_id,
        &table_ref,
        &indices_ref,
        &gate_ref,
    )
    .map_err(anyhow::Error::msg)?;

    println!("lingqu_memory_service");
    println!("  mode: validate-w5-engram-object-ref");
    println!("  query_result: {}", result.result_id);
    println!("  query_result_manifest: {}", query_result_path.path);
    println!("  query_result_version: {}", result.version);
    println!("  query_result_checksum: {:#x}", result.checksum);
    println!("  vector_indexes: {}", result.vector_index_ids.join(","));
    println!(
        "  segment_versions: {}",
        result
            .embedding_segment_versions
            .iter()
            .map(|segment| format!(
                "{}@{}:{:#x}",
                segment.segment_id, segment.version, segment.checksum
            ))
            .collect::<Vec<_>>()
            .join(",")
    );
    println!(
        "  selected_records: {}",
        result.selected_record_ids.join(",")
    );
    println!("  evidence_refs: {}", result.evidence_refs.join(","));
    println!("  matches: {}", result.matches.len());
    println!("  hot_state: {}", hot_state.state_id);
    println!("  hot_table_shape: {:?}", hot_state.table.shape);
    println!(
        "  selected_chunks: {}",
        hot_state.selected_chunk_ids.join(",")
    );
    println!("  registry_env_recommended:");
    println!(
        "    {}={}",
        SIM_QWEN3_GUEST_ENGRAM_STATE_REF,
        qwen3_obmm_object_ref_wire_to_hex(&state_ref)
    );
    println!(
        "    {}=<optional registry dir override>",
        SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR
    );
    println!("  registry_table_bytes: {}", table_payload.len());
    println!("  registry_indices_bytes: {}", indices_payload.len());
    println!("  registry_gate_bytes: {}", gate_payload.len());
    println!(
        "  registry_state_manifest_bytes: {}",
        state_ref.payload_bytes
    );
    let durable_stats = durable_store.stats();
    let object_report = object_service.report();
    println!("  gate_weight_block_ref: {}", gate_weight_block);
    println!(
        "  block_payload_writes: {}",
        durable_stats.block_payload_writes
    );
    println!(
        "  block_payload_reads: {}",
        durable_stats.block_payload_reads
    );
    println!(
        "  obmm_payload_writes: {}",
        object_report.obmm_pool_payload_write_count
    );
    Ok(())
}

fn cli_f32_vec_to_le_bytes(values: &[f32]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(values.len() * 4);
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    bytes
}

fn run_lingqu_object_service_stress_cli() -> anyhow::Result<()> {
    let mut profile = LingquObjectServiceProfile::default();
    profile.queue_depth = 4096;
    profile.obmm_pool.queue_depth = 4096;
    profile.obmm_pool.queue_auto_drain = true;
    profile.obmm_pool.pool_bytes = 96 * 1024 * 1024;
    profile.obmm_pool.payload_base_offset = 2 * 1024 * 1024;
    profile.obmm_pool.payload_alignment = 64;
    profile.obmm_pool.payload_block_tiers = [256 * 1024, 512 * 1024, 1024 * 1024, 2 * 1024 * 1024];

    let mut service = LingquObjectServiceStub::new(profile);
    let payload_sizes = [
        128 * 1024,
        256 * 1024,
        256 * 1024 + 1,
        512 * 1024,
        512 * 1024 + 1,
        1024 * 1024,
        1024 * 1024 + 1,
        2 * 1024 * 1024,
        2 * 1024 * 1024 + 1,
        5 * 1024 * 1024 + 123,
    ];
    let mut checksums = Vec::new();

    for (index, bytes) in payload_sizes.iter().copied().enumerate() {
        let payload = lingqu_stress_payload(0x5150_u64 + index as u64, bytes as usize);
        let checksum = lingqu_stress_checksum(&payload);
        let key = format!("obmm-pool/stress/tiered-span/{index}");
        service.submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: key.clone(),
                kind: LingquObjectKind::KvCacheBlock,
                producer_entity: index as u64 % 4,
                owner_entity: Some((index as u64 + 1) % 4),
                expected_version: None,
                metadata: LingquObjectMetadata {
                    bytes,
                    checksum,
                    dtype: None,
                    shape: Vec::new(),
                    layout: None,
                    expires_at_us: None,
                },
                placements: vec![LingquPayloadPlacement {
                    backend: LingquPayloadBackend::Shmem,
                    storage_ref: format!("obmm-pool/stress/tiered-span/payload/{index}"),
                    segment: Some(SegmentHandle(0x5150)),
                    offset: 0,
                    bytes,
                    checksum,
                    locality: LingquObjectLocality::DomainShared(0),
                }],
                payload_bytes: payload,
            },
            10 + index as u64,
        )?;
        checksums.push((key, checksum, (index as u64 + 1) % 4));
    }

    let publish_events = service.poll_ready(1000);
    if publish_events.len() != payload_sizes.len()
        || publish_events
            .iter()
            .any(|event| event.status != CompletionStatus::Success)
    {
        anyhow::bail!("lingqu object service stress publish failed");
    }

    for (index, (key, _, requester)) in checksums.iter().enumerate() {
        service.submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: key.clone(),
                requester_entity: *requester,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![LingquPayloadBackend::Shmem],
            },
            2000 + index as u64,
        )?;
    }

    let resolve_events = service.poll_ready(3000);
    if resolve_events.len() != payload_sizes.len()
        || resolve_events
            .iter()
            .any(|event| event.status != CompletionStatus::Success)
    {
        anyhow::bail!("lingqu object service stress resolve failed");
    }

    for (key, checksum, _) in &checksums {
        let Some(copy) = service.get_copy(key, LingquObjectVersionSelector::LatestCommitted) else {
            anyhow::bail!("missing stress payload copy for {key}");
        };
        let Some(view) = service.get_ref(key, LingquObjectVersionSelector::LatestCommitted) else {
            anyhow::bail!("missing stress payload view for {key}");
        };
        if lingqu_stress_checksum(&copy) != *checksum || lingqu_stress_checksum(view) != *checksum {
            anyhow::bail!("stress payload checksum mismatch for {key}");
        }
    }

    let report = service.report();
    println!("lingqu_object_service_stress");
    println!("  objects: {}", payload_sizes.len());
    println!("  publish_count: {}", report.publish_count);
    println!("  resolve_count: {}", report.resolve_count);
    println!(
        "  obmm_pool_payload_write_count: {}",
        report.obmm_pool_payload_write_count
    );
    println!(
        "  obmm_pool_payload_read_count: {}",
        report.obmm_pool_payload_read_count
    );
    println!(
        "  obmm_pool_queue_submit_count: {}",
        report.obmm_pool_queue_submit_count
    );
    println!(
        "  obmm_pool_queue_deliver_count: {}",
        report.obmm_pool_queue_deliver_count
    );
    println!("  obmm_pool_bytes_used: {}", report.obmm_pool_bytes_used);
    println!(
        "  obmm_pool_reserved_bytes: {}",
        report.obmm_pool_reserved_bytes
    );
    println!("  obmm_pool_block_count: {}", report.obmm_pool_block_count);
    println!(
        "  obmm_pool_multi_block_write_count: {}",
        report.obmm_pool_multi_block_write_count
    );
    println!(
        "  obmm_pool_max_blocks_per_payload: {}",
        report.obmm_pool_max_blocks_per_payload
    );
    println!("  checksum: {:#x}", report.checksum);
    Ok(())
}

fn lingqu_stress_payload(seed: u64, bytes: usize) -> Vec<u8> {
    let mut state = seed ^ 0x9e37_79b9_7f4a_7c15;
    let mut payload = Vec::with_capacity(bytes);
    for index in 0..bytes {
        state ^= state << 7;
        state ^= state >> 9;
        state ^= state << 8;
        payload.push((state as u8) ^ (index as u8).wrapping_mul(31));
    }
    payload
}

fn lingqu_stress_checksum(bytes: &[u8]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for byte in bytes {
        acc ^= u64::from(*byte);
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn publish_lingqu_object_cli_sample(
    service: &mut LingquObjectServiceStub,
    key: &str,
    kind: LingquObjectKind,
    backend: LingquPayloadBackend,
    bytes: u64,
    checksum: u64,
    producer_entity: u64,
) -> anyhow::Result<()> {
    service
        .submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: key.to_string(),
                kind,
                producer_entity,
                owner_entity: Some(producer_entity),
                expected_version: None,
                metadata: LingquObjectMetadata {
                    bytes,
                    checksum,
                    dtype: None,
                    shape: vec![bytes],
                    layout: None,
                    expires_at_us: None,
                },
                placements: vec![LingquPayloadPlacement {
                    backend,
                    storage_ref: format!("{key}/payload"),
                    segment: None,
                    offset: 0,
                    bytes,
                    checksum,
                    locality: LingquObjectLocality::DomainShared(0),
                }],
                payload_bytes: checksum.to_le_bytes().to_vec(),
            },
            producer_entity,
        )
        .map(|_| ())
        .map_err(anyhow::Error::from)
}

fn resolve_lingqu_object_cli_sample(
    service: &mut LingquObjectServiceStub,
    key: &str,
    preferred_backends: &[LingquPayloadBackend],
    requester_entity: u64,
) -> anyhow::Result<()> {
    service
        .submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: key.to_string(),
                requester_entity,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: preferred_backends.to_vec(),
            },
            requester_entity,
        )
        .map(|_| ())
        .map_err(anyhow::Error::from)
}

#[cfg(test)]
mod tests {
    use super::{
        cli_f32_vec_to_le_bytes, lingqu_durable_args_from, lingqu_memory_args_from,
        lingqu_object_service_args_from, load_lingqu_memory_durable_store,
        load_w5_memory_decisions_from_store, publish_w5_engram_state_ref_from_memory,
        publish_w5_memory_decision_artifact_refs, qwen3_decode_loop_args_from,
        qwen3_decode_report_verbosity_from_env, qwen3_dense_weights_path_from_env,
        qwen3_engram_policy_checksum, qwen3_engram_select_token, qwen3_engram_state_words,
        qwen3_guest_candidate_records, qwen3_guest_decode_loop_args_from,
        qwen3_guest_default_w5_profile, qwen3_guest_dense_runtime,
        qwen3_guest_engram_candidate_counts, qwen3_guest_engram_env_vars,
        qwen3_guest_engram_env_vars_from_lookup, qwen3_guest_engram_expected_terminal_rewrites,
        qwen3_guest_engram_history_lengths, qwen3_guest_engram_object_transport_report,
        qwen3_guest_engram_report, qwen3_guest_engram_report_from_guest_log,
        qwen3_guest_engram_select_history_lengths, qwen3_guest_engram_selected_tokens,
        qwen3_guest_log_dir_from_script_output, qwen3_guest_log_match_count,
        qwen3_guest_terminal_candidate_records, qwen3_guest_terminal_text_lossy_from_tokenizer,
        qwen3_guest_terminal_tokens, qwen3_guest_timing_summary, qwen3_range_forward_args_from,
        run_lingqu_durable_append_log_cli, run_lingqu_durable_batch_cli,
        run_lingqu_durable_init_cli, run_lingqu_durable_list_cli, run_lingqu_durable_read_log_cli,
        run_lingqu_durable_stat_cli, run_lingqu_durable_validate_cli,
        run_lingqu_memory_boundary_lookup_cli,
        run_lingqu_memory_boundary_lookup_from_observation_cli,
        run_lingqu_memory_boundary_request_from_w5_summary_cli, run_lingqu_memory_build_index_cli,
        run_lingqu_memory_ingest_cli, run_lingqu_memory_list_prefetch_plans_cli,
        run_lingqu_memory_list_prefix_cache_reuse_cli, run_lingqu_memory_list_query_results_cli,
        run_lingqu_memory_list_record_lifecycle_cli,
        run_lingqu_memory_list_shortpath_decisions_cli,
        run_lingqu_memory_list_shortpath_supports_cli, run_lingqu_memory_lookup_prefix_cache_cli,
        run_lingqu_memory_materialize_engram_state_cli,
        run_lingqu_memory_materialize_hot_state_cli, run_lingqu_memory_plan_prefetch_cli,
        run_lingqu_memory_publish_w5_engram_state_ref_cli, run_lingqu_memory_query_cli,
        run_lingqu_memory_record_boundary_observations_from_w5_summary_cli,
        run_lingqu_memory_register_execution_artifact_cli,
        run_lingqu_memory_register_prefix_cache_cli, run_lingqu_memory_update_record_state_cli,
        run_lingqu_memory_validate_durable_store, run_lingqu_memory_validate_flat_materialize,
        run_lingqu_memory_validate_flat_query, run_lingqu_memory_validate_w5_engram_object_ref,
        save_lingqu_durable_sim, save_lingqu_memory_durable_store,
        simpler_host_matmul_artifact_producer_path, validate_qwen3_dense_weights_path,
        validate_w5_inference_profile, w5_memory_decision_env_vars,
        w5_object_service_payload_index_path, LingquDurableSim, LingquDurableSimSnapshot,
        LingquMemoryDurableStore, LingquMemoryDurableStoreSnapshot, LingquObjectServiceStub,
        LingquObjectVersionSelector, MemoryCatalogSnapshot, QueryResult, Qwen3CandidateRecord,
        Qwen3DecodeReportVerbosity, Qwen3EngramConfig, Qwen3EngramContextOp, Qwen3EngramMode,
        Qwen3EngramPool, Qwen3EngramReport, Qwen3GuestDecodeLoopCliArgs, W5MemoryBootstrapConfig,
        W5MemoryDecisionConfig, SIM_QWEN3_GUEST_ENGRAM_STATE_REF,
        SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR, SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT,
    };
    use std::env;
    use std::fs;
    use std::path::{Path, PathBuf};

    fn sample_w5_terminal_logits_payload() -> Vec<u8> {
        const LOGITS_HEADER_BYTES: usize = 64;
        const LOGITS_ENTRY_BYTES: usize = 45 * 8;
        const TOKEN_TEXT_HEADER_BYTES: usize = 64;
        const TOKEN_TEXT_ENTRY_BYTES: usize = 8 * 8;
        let token_text_header = LOGITS_HEADER_BYTES + LOGITS_ENTRY_BYTES;
        let token_text_base = token_text_header + TOKEN_TEXT_HEADER_BYTES;
        let mut payload = vec![0u8; token_text_base + TOKEN_TEXT_ENTRY_BYTES];

        write_test_u64(&mut payload, 0, 0x713377346c6f6730);
        write_test_u64(&mut payload, 8, 1);
        write_test_u64(&mut payload, 16, 45);
        write_test_u64(&mut payload, 24, LOGITS_ENTRY_BYTES as u64);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 16, 1_000_000);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 24, 151_936);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 32, 11);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 40, 358);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 48, 100);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 56, 0xaaa0);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 64, 0xbbb0);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 80, 0x1110);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 88, 0x2220);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 96, 0x3330);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 104, 151_936);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 112, 0xccc0);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 120, 0x3f80_0000);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 128, 0x3f00_0000);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 136, 40);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 144, 0x4440);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 152, 0x5550);
        write_test_u64(&mut payload, LOGITS_HEADER_BYTES + 160, 2);
        write_test_terminal_candidate(
            &mut payload,
            LOGITS_HEADER_BYTES + 168,
            11,
            0x3f80_0000,
            0xbbb0,
            1,
            0x41,
        );
        write_test_terminal_candidate(
            &mut payload,
            LOGITS_HEADER_BYTES + 216,
            358,
            0x3f00_0000,
            0xddd0,
            1,
            0x42,
        );

        write_test_u64(&mut payload, token_text_header, 0x7133773474787430);
        write_test_u64(&mut payload, token_text_header + 8, 1);
        write_test_u64(&mut payload, token_text_header + 16, 8);
        write_test_u64(
            &mut payload,
            token_text_header + 24,
            TOKEN_TEXT_ENTRY_BYTES as u64,
        );
        write_test_u64(&mut payload, token_text_header + 32, 1);
        write_test_u64(&mut payload, token_text_header + 40, 0x1234);
        write_test_u64(&mut payload, token_text_header + 48, 2);
        write_test_u64(&mut payload, token_text_base, 0);
        write_test_u64(&mut payload, token_text_base + 8, 11);
        write_test_u64(&mut payload, token_text_base + 24, 1);
        write_test_u64(&mut payload, token_text_base + 32, 0x41);
        write_test_u64(&mut payload, token_text_base + 48, 0xbbb0);
        write_test_u64(&mut payload, token_text_base + 56, 2);
        payload
    }

    fn write_test_terminal_candidate(
        payload: &mut [u8],
        offset: usize,
        token: u64,
        logit_bits: u64,
        text_checksum: u64,
        piece_bytes: u64,
        piece_word0: u64,
    ) {
        write_test_u64(payload, offset, token);
        write_test_u64(payload, offset + 8, logit_bits);
        write_test_u64(payload, offset + 16, text_checksum);
        write_test_u64(payload, offset + 24, piece_bytes);
        write_test_u64(payload, offset + 32, piece_word0);
    }

    fn write_test_u64(payload: &mut [u8], offset: usize, value: u64) {
        payload[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
    }

    #[test]
    fn qwen3_decode_loop_args_default_to_two_steps() {
        let args = qwen3_decode_loop_args_from([
            "qwen3-decode-loop",
            "scenarios/mvp_2host_single_domain.yaml",
        ])
        .expect("parse decode loop args")
        .expect("decode loop args");
        assert_eq!(
            args.scenario_path,
            PathBuf::from("scenarios/mvp_2host_single_domain.yaml")
        );
        assert_eq!(args.step_count, 2);
        assert_eq!(args.prompt, None);
        assert_eq!(args.matmul_batch, None);
    }

    #[test]
    fn qwen3_decode_loop_args_accept_explicit_step_count() {
        let args = qwen3_decode_loop_args_from([
            "qwen3-decode-loop",
            "scenarios/mvp_2host_single_domain.yaml",
            "4",
        ])
        .expect("parse decode loop args")
        .expect("decode loop args");
        assert_eq!(
            args.scenario_path,
            PathBuf::from("scenarios/mvp_2host_single_domain.yaml")
        );
        assert_eq!(args.step_count, 4);
        assert_eq!(args.prompt, None);
    }

    #[test]
    fn qwen3_decode_loop_args_accept_prompt() {
        let args = qwen3_decode_loop_args_from([
            "qwen3-decode-loop",
            "scenarios/mvp_2host_single_domain.yaml",
            "2",
            "Hello Qwen3",
        ])
        .expect("parse decode loop args")
        .expect("decode loop args");
        assert_eq!(
            args.scenario_path,
            PathBuf::from("scenarios/mvp_2host_single_domain.yaml")
        );
        assert_eq!(args.step_count, 2);
        assert_eq!(args.prompt.as_deref(), Some("Hello Qwen3"));
    }

    #[test]
    fn qwen3_decode_loop_args_accept_named_options() {
        let args = qwen3_decode_loop_args_from([
            "qwen3-decode-loop",
            "--scenario",
            "4host",
            "--steps",
            "32",
            "--prompt",
            "Capital of China is",
            "--matmul-batch",
            "4",
        ])
        .expect("parse decode loop args")
        .expect("decode loop args");
        assert_eq!(
            args.scenario_path,
            PathBuf::from("scenarios/mvp_4host_single_domain.yaml")
        );
        assert_eq!(args.step_count, 32);
        assert_eq!(args.prompt.as_deref(), Some("Capital of China is"));
        assert_eq!(args.matmul_batch, Some(4));
    }

    #[test]
    fn qwen3_decode_loop_args_accept_trailing_prompt_with_options() {
        let args = qwen3_decode_loop_args_from([
            "qwen3-decode-loop",
            "--scenario=8host",
            "--steps=8",
            "--matmul-batch=2",
            "Capital of China is",
        ])
        .expect("parse decode loop args")
        .expect("decode loop args");
        assert_eq!(
            args.scenario_path,
            PathBuf::from("scenarios/mvp_8host_single_domain.yaml")
        );
        assert_eq!(args.step_count, 8);
        assert_eq!(args.prompt.as_deref(), Some("Capital of China is"));
        assert_eq!(args.matmul_batch, Some(2));
    }

    #[test]
    fn qwen3_decode_loop_weights_env_prefers_dense_and_accepts_legacy_alias() {
        assert_eq!(
            qwen3_dense_weights_path_from_env(
                Some("/models/dense".into()),
                Some("/models/legacy".into())
            ),
            Some("/models/dense".into())
        );
        assert_eq!(
            qwen3_dense_weights_path_from_env(None, Some("/models/legacy".into())),
            Some("/models/legacy".into())
        );
        assert_eq!(qwen3_dense_weights_path_from_env(None, None), None);
    }

    #[test]
    fn qwen3_range_forward_args_accept_named_options() {
        let args = qwen3_range_forward_args_from([
            "qwen3-range-forward",
            "--scenario=8host",
            "--prompt",
            "Capital of China is",
        ])
        .expect("parse range forward args")
        .expect("range forward args");
        assert_eq!(
            args.scenario_path,
            PathBuf::from("scenarios/mvp_8host_single_domain.yaml")
        );
        assert_eq!(args.prompt.as_str(), "Capital of China is");
    }

    #[test]
    fn qwen3_guest_decode_loop_args_accept_named_options() {
        let args = qwen3_guest_decode_loop_args_from([
            "qwen3-guest-decode-loop",
            "--steps=1",
            "--prompt",
            "Capital of China is",
            "--prompt-token-ids=9707,1207,16948,18,358",
            "--script",
            "guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh",
            "--matmul-batch=16",
            "--model",
            "Qwen/Qwen3-14B",
            "--weights-path=/models/qwen3-14b",
        ])
        .expect("parse guest decode loop args")
        .expect("guest decode loop args");
        assert_eq!(args.step_count, 1);
        assert_eq!(args.prompt.as_deref(), Some("Capital of China is"));
        assert_eq!(
            args.prompt_token_ids.as_deref(),
            Some("9707,1207,16948,18,358")
        );
        assert_eq!(
            args.script_path,
            PathBuf::from("guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh")
        );
        assert_eq!(args.matmul_batch, Some(16));
        assert_eq!(args.model.as_deref(), Some("Qwen/Qwen3-14B"));
        assert_eq!(args.weights_path, Some(PathBuf::from("/models/qwen3-14b")));
        assert_eq!(args.w5_profile, None);
        assert_eq!(args.engram, Qwen3EngramConfig::default());
    }

    #[test]
    fn qwen3_guest_decode_loop_args_default_to_w5_runner() {
        let args = qwen3_guest_decode_loop_args_from(["qwen3-guest-decode-loop"])
            .expect("parse guest decode loop args")
            .expect("guest decode loop args");

        assert_eq!(
            args.script_path,
            PathBuf::from("guest-linux/aarch64/scripts/run_ub_eight_node_w5_inference_cluster.sh")
        );
        assert_eq!(args.w5_profile, None);
    }

    #[test]
    fn w5_inference_cluster_args_accept_profile() {
        let args = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--w5-profile=qwen3_14b_engram_decode",
            "--engram",
            "--engram-pool=obmm",
        ])
        .expect("parse w5 inference cluster args")
        .expect("w5 inference cluster args");

        assert_eq!(args.w5_profile.as_deref(), Some("qwen3_14b_engram_decode"));
        assert!(args.engram.enabled);
        assert_eq!(args.engram.pool, Qwen3EngramPool::Obmm);
    }

    #[test]
    fn w5_inference_cluster_args_reject_unknown_profile() {
        let err =
            qwen3_guest_decode_loop_args_from(["w5-inference-cluster", "--w5-profile=unknown"])
                .expect_err("unknown w5 profile should fail");

        assert!(err.to_string().contains("unsupported --w5-profile"));
    }

    #[test]
    fn qwen3_guest_decode_loop_args_accept_engram_options() {
        let args = qwen3_guest_decode_loop_args_from([
            "qwen3-guest-decode-loop",
            "--steps=8",
            "--engram",
            "--engram-mode=cpu",
            "--engram-pool=obmm",
            "--engram-owner-node=3",
            "--no-repeat-ngram-size=3",
            "--repetition-penalty=1.250",
            "--engram-block-token-id=11",
            "--engram-block-token-ids=358,1128",
            "--engram-context-op=cpu-reference",
            "--engram-history-window=64",
            "--engram-report=steps",
        ])
        .expect("parse guest decode loop args")
        .expect("guest decode loop args");
        assert!(args.engram.enabled);
        assert_eq!(args.engram.mode, Qwen3EngramMode::Cpu);
        assert_eq!(args.engram.pool, Qwen3EngramPool::Obmm);
        assert_eq!(args.engram.owner_node, 3);
        assert_eq!(args.engram.no_repeat_ngram_size, 3);
        assert_eq!(args.engram.repetition_penalty_milli, 1250);
        assert_eq!(args.engram.blocked_token_ids, vec![11, 358, 1128]);
        assert_eq!(args.engram.context_op, Qwen3EngramContextOp::CpuReference);
        assert_eq!(args.engram.history_window, 64);
        assert_eq!(args.engram.report, Qwen3EngramReport::Steps);
    }

    #[test]
    fn qwen3_guest_decode_loop_args_accept_fused_simt_engram_mode() {
        let args = qwen3_guest_decode_loop_args_from([
            "qwen3-guest-decode-loop",
            "--engram",
            "--engram-mode=fused-simt",
            "--engram-pool=obmm",
        ])
        .expect("parse guest decode loop args")
        .expect("guest decode loop args");

        assert_eq!(args.engram.mode, Qwen3EngramMode::FusedSimt);
    }

    #[test]
    fn qwen3_guest_decode_loop_args_accept_fused_simt_context_op() {
        let args = qwen3_guest_decode_loop_args_from([
            "qwen3-guest-decode-loop",
            "--engram",
            "--engram-pool=obmm",
            "--engram-context-op=fused-simt",
        ])
        .expect("parse guest decode loop args")
        .expect("guest decode loop args");

        assert_eq!(args.engram.context_op, Qwen3EngramContextOp::FusedSimt);
    }

    #[test]
    fn qwen3_guest_decode_loop_args_accept_simpler_host_context_op() {
        let args = qwen3_guest_decode_loop_args_from([
            "qwen3-guest-decode-loop",
            "--engram",
            "--engram-pool=obmm",
            "--engram-context-op=simpler-host",
        ])
        .expect("parse guest decode loop args")
        .expect("guest decode loop args");

        assert_eq!(args.engram.context_op, Qwen3EngramContextOp::SimplerHost);
        let vars = qwen3_guest_engram_env_vars(&args.engram, 0x1234);
        assert!(vars.contains(&(
            "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP".to_string(),
            "simpler-host".to_string()
        )));
    }

    #[test]
    fn qwen3_guest_decode_loop_args_accept_explicit_state_ref_entrypoint() {
        let args = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--steps=2",
            "--engram-state-ref=abcd",
            "--object-registry-dir=/tmp/qwen3-registry",
        ])
        .expect("parse guest decode loop args")
        .expect("guest decode loop args");

        assert!(args.engram.enabled);
        assert_eq!(args.engram.pool, Qwen3EngramPool::Obmm);
        assert_eq!(args.engram.context_op, Qwen3EngramContextOp::CpuReference);
        assert_eq!(args.engram.state_ref.as_deref(), Some("abcd"));
        assert_eq!(
            args.engram.object_registry_dir.as_deref(),
            Some(Path::new("/tmp/qwen3-registry"))
        );
    }

    #[test]
    fn qwen3_guest_decode_loop_args_accept_object_service_state_ref_entrypoint() {
        let args = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--steps=2",
            "--engram-state-ref=abcd",
            "--object-service-snapshot=/tmp/lingqu-object-service-snapshot.json",
        ])
        .expect("parse guest decode loop args")
        .expect("guest decode loop args");

        assert!(args.engram.enabled);
        assert_eq!(args.engram.pool, Qwen3EngramPool::Obmm);
        assert_eq!(args.engram.context_op, Qwen3EngramContextOp::CpuReference);
        assert_eq!(args.engram.state_ref.as_deref(), Some("abcd"));
        assert_eq!(
            args.engram.object_service_snapshot_path.as_deref(),
            Some(Path::new("/tmp/lingqu-object-service-snapshot.json"))
        );
    }

    #[test]
    fn w5_inference_cluster_args_accept_memory_service_bootstrap() {
        let args = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--steps=2",
            "--memory-store=/tmp/lingqu-memory-store.json",
            "--memory-object-store=/tmp/lingqu-object-store.json",
            "--memory-engram-state=/tmp/engram-state.json",
            "--memory-registry-dir=/tmp/qwen3-registry",
            "--memory-owner-entity=3",
            "--memory-producer-entity=4",
        ])
        .expect("parse w5 memory bootstrap args")
        .expect("w5 memory bootstrap args");

        assert_eq!(args.engram, Qwen3EngramConfig::default());
        let memory_bootstrap = args.memory_bootstrap.expect("memory bootstrap");
        assert_eq!(
            memory_bootstrap.store_path,
            PathBuf::from("/tmp/lingqu-memory-store.json")
        );
        assert_eq!(
            memory_bootstrap.object_store_path,
            PathBuf::from("/tmp/lingqu-object-store.json")
        );
        assert_eq!(
            memory_bootstrap.engram_state_path,
            PathBuf::from("/tmp/engram-state.json")
        );
        assert_eq!(
            memory_bootstrap.registry_dir,
            PathBuf::from("/tmp/qwen3-registry")
        );
        assert_eq!(memory_bootstrap.owner_entity, 3);
        assert_eq!(memory_bootstrap.producer_entity, 4);
    }

    #[test]
    fn w5_inference_cluster_args_accept_memory_service_decisions() {
        let args = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--memory-decision-store=/tmp/lingqu-memory-store.json",
            "--memory-shortpath-decision-id=shortpath-decision/boundary/0",
            "--memory-shortpath-execute",
            "--memory-prefetch-plan-id=prefetch-plan/range/0",
            "--memory-prefix-cache-reuse-plan-id=prefix-cache-reuse/prefix/0",
        ])
        .expect("parse w5 memory decision args")
        .expect("w5 memory decision args");

        let memory_decisions = args.memory_decisions.expect("memory decisions");
        assert_eq!(
            memory_decisions.store_path,
            PathBuf::from("/tmp/lingqu-memory-store.json")
        );
        assert_eq!(memory_decisions.boundary_request_path, None);
        assert_eq!(memory_decisions.boundary_observation_id, None);
        assert_eq!(
            memory_decisions.shortpath_decision_id.as_deref(),
            Some("shortpath-decision/boundary/0")
        );
        assert!(memory_decisions.shortpath_execute);
        assert_eq!(
            memory_decisions.prefetch_plan_id.as_deref(),
            Some("prefetch-plan/range/0")
        );
        assert_eq!(
            memory_decisions.prefix_cache_reuse_plan_id.as_deref(),
            Some("prefix-cache-reuse/prefix/0")
        );
    }

    #[test]
    fn w5_inference_cluster_args_accept_memory_boundary_request() {
        let args = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--memory-decision-store=/tmp/lingqu-memory-store.json",
            "--memory-boundary-request=/tmp/boundary-request.json",
        ])
        .expect("parse w5 memory boundary request args")
        .expect("w5 memory boundary request args");

        let memory_decisions = args.memory_decisions.expect("memory decisions");
        assert_eq!(
            memory_decisions.store_path,
            PathBuf::from("/tmp/lingqu-memory-store.json")
        );
        assert_eq!(
            memory_decisions.boundary_request_path.as_deref(),
            Some(Path::new("/tmp/boundary-request.json"))
        );
        assert_eq!(memory_decisions.boundary_observation_id, None);
        assert_eq!(memory_decisions.shortpath_decision_id, None);
    }

    #[test]
    fn w5_inference_cluster_args_accept_memory_boundary_observation() {
        let args = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--memory-decision-store=/tmp/lingqu-memory-store.json",
            "--memory-boundary-observation-id=boundary-observation/run0/step2/node3",
        ])
        .expect("parse w5 memory boundary observation args")
        .expect("w5 memory boundary observation args");

        let memory_decisions = args.memory_decisions.expect("memory decisions");
        assert_eq!(
            memory_decisions.store_path,
            PathBuf::from("/tmp/lingqu-memory-store.json")
        );
        assert_eq!(memory_decisions.boundary_request_path, None);
        assert_eq!(
            memory_decisions.boundary_observation_id.as_deref(),
            Some("boundary-observation/run0/step2/node3")
        );
        assert_eq!(memory_decisions.shortpath_decision_id, None);
    }

    #[test]
    fn w5_inference_cluster_args_reject_boundary_request_and_decision_id() {
        let err = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--memory-decision-store=/tmp/lingqu-memory-store.json",
            "--memory-boundary-request=/tmp/boundary-request.json",
            "--memory-shortpath-decision-id=shortpath-decision/boundary/0",
        ])
        .expect_err("boundary request and explicit shortpath decision should be ambiguous");

        assert!(err
            .to_string()
            .contains("--memory-boundary-request, --memory-boundary-observation-id"));
    }

    #[test]
    fn w5_inference_cluster_args_reject_decision_id_without_store() {
        let err = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--memory-shortpath-decision-id=shortpath-decision/boundary/0",
        ])
        .expect_err("decision id without store should fail");

        assert!(err
            .to_string()
            .contains("--memory-decision-store is required"));
    }

    #[test]
    fn w5_inference_cluster_args_reject_empty_decision_store() {
        let err = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--memory-decision-store=/tmp/lingqu-memory-store.json",
        ])
        .expect_err("decision store without ids should fail");

        assert!(err
            .to_string()
            .contains("--memory-decision-store requires at least one"));
    }

    #[test]
    fn w5_inference_cluster_args_reject_partial_memory_bootstrap() {
        let err = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--memory-store=/tmp/lingqu-memory-store.json",
        ])
        .expect_err("partial memory bootstrap should fail");

        assert!(err
            .to_string()
            .contains("--memory-store, --memory-object-store, --memory-engram-state"));
    }

    #[test]
    fn w5_inference_cluster_args_reject_mixed_memory_and_state_ref_bootstrap() {
        let err = qwen3_guest_decode_loop_args_from([
            "w5-inference-cluster",
            "--engram-state-ref=abcd",
            "--object-registry-dir=/tmp/qwen3-registry-explicit",
            "--memory-store=/tmp/lingqu-memory-store.json",
            "--memory-object-store=/tmp/lingqu-object-store.json",
            "--memory-engram-state=/tmp/engram-state.json",
            "--memory-registry-dir=/tmp/qwen3-registry",
        ])
        .expect_err("mixed memory and state-ref bootstrap should fail");

        assert!(err
            .to_string()
            .contains("--memory-* bootstrap cannot be combined"));
    }

    #[test]
    fn qwen3_guest_decode_loop_args_require_registry_with_state_ref() {
        let err =
            qwen3_guest_decode_loop_args_from(["w5-inference-cluster", "--engram-state-ref=abcd"])
                .expect_err("state ref without object source should fail");

        assert!(err.to_string().contains(
            "--engram-state-ref requires --object-registry-dir or --object-service-snapshot"
        ));
    }

    #[test]
    fn qwen3_guest_decode_loop_engram_requires_obmm_pool() {
        let err = qwen3_guest_decode_loop_args_from([
            "qwen3-guest-decode-loop",
            "--engram",
            "--engram-pool=object",
        ])
        .expect_err("object pool should be rejected for guest engram");
        assert!(err.to_string().contains("--engram-pool=obmm"));
    }

    #[test]
    fn qwen3_guest_engram_env_vars_include_policy_knobs() {
        let config = Qwen3EngramConfig {
            enabled: true,
            mode: Qwen3EngramMode::FusedSimt,
            pool: Qwen3EngramPool::Obmm,
            owner_node: 3,
            no_repeat_ngram_size: 2,
            repetition_penalty_milli: 3000,
            history_window: 64,
            blocked_token_ids: vec![2776, 151645],
            context_op: Qwen3EngramContextOp::FusedSimt,
            ..Qwen3EngramConfig::default()
        };
        let vars = qwen3_guest_engram_env_vars(&config, 0x1234);
        assert!(vars.contains(&("SIM_QWEN3_GUEST_ENGRAM".to_string(), "1".to_string())));
        assert!(vars.contains(&(
            "SIM_QWEN3_GUEST_ENGRAM_MODE".to_string(),
            "fused-simt".to_string()
        )));
        assert!(vars.contains(&(
            "SIM_QWEN3_GUEST_ENGRAM_SESSION_ID".to_string(),
            "0000000000001234".to_string()
        )));
        assert!(vars.contains(&(
            "SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE".to_string(),
            "3".to_string()
        )));
        assert!(vars.contains(&(
            "SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE".to_string(),
            "2".to_string()
        )));
        assert!(vars.contains(&(
            "SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI".to_string(),
            "3000".to_string()
        )));
        assert!(vars.contains(&(
            "SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW".to_string(),
            "64".to_string()
        )));
        assert!(vars.contains(&(
            "SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS".to_string(),
            "2776,151645".to_string()
        )));
        assert!(vars.contains(&(
            "SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP".to_string(),
            "fused-simt".to_string()
        )));
    }

    #[test]
    fn qwen3_guest_engram_env_vars_forward_state_object_ref_contract() {
        let config = Qwen3EngramConfig {
            enabled: true,
            context_op: Qwen3EngramContextOp::CpuReference,
            state_ref: Some("state-ref".to_string()),
            object_registry_dir: Some(PathBuf::from("/tmp/qwen3-registry")),
            object_service_snapshot_path: Some(PathBuf::from(
                "/tmp/lingqu-object-service-snapshot.json",
            )),
            ..Qwen3EngramConfig::default()
        };
        let vars = qwen3_guest_engram_env_vars_from_lookup(&config, 0x1234, |_key| None);

        assert!(vars.contains(&(
            SIM_QWEN3_GUEST_ENGRAM_STATE_REF.to_string(),
            "state-ref".to_string()
        )));
        assert!(vars.contains(&(
            SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR.to_string(),
            "/tmp/qwen3-registry".to_string()
        )));
        assert!(vars.contains(&(
            SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT.to_string(),
            "/tmp/lingqu-object-service-snapshot.json".to_string()
        )));
    }

    #[test]
    fn simpler_host_matmul_artifact_producer_path_supports_ub_sim_layout() {
        let path = simpler_host_matmul_artifact_producer_path();
        assert!(path.ends_with("prepare_simpler_host_matmul_artifacts.py"));
        assert!(path.exists());
    }

    #[test]
    fn qwen3_weights_path_validation_requires_real_assets() {
        let dir = env::temp_dir().join(format!(
            "sim_cli_qwen3_weights_validation_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("temp qwen3 weights dir");

        let err = validate_qwen3_dense_weights_path(&dir)
            .expect_err("empty weights dir should be rejected");
        assert!(err.to_string().contains("config.json"));

        fs::write(dir.join("config.json"), b"{}").expect("write config");
        fs::write(dir.join("tokenizer.json"), b"{}").expect("write tokenizer");
        let err = validate_qwen3_dense_weights_path(&dir)
            .expect_err("weights dir without tensor assets should be rejected");
        assert!(err.to_string().contains("model.safetensors"));

        for file in ["model.safetensors"] {
            fs::write(dir.join(file), b"stub").expect("write required qwen3 asset");
        }
        validate_qwen3_dense_weights_path(&dir).expect("required qwen3 assets should pass");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn qwen3_dense_weights_path_validation_accepts_sharded_assets() {
        let dir = env::temp_dir().join(format!(
            "sim_cli_qwen3_dense_weights_validation_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("temp qwen3 dense weights dir");

        fs::write(dir.join("config.json"), b"{}").expect("write config");
        fs::write(dir.join("tokenizer.json"), b"{}").expect("write tokenizer");
        fs::write(dir.join("model.safetensors.index.json"), b"{}").expect("write index");

        validate_qwen3_dense_weights_path(&dir).expect("sharded qwen3 assets should pass");
        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn qwen3_guest_dense_runtime_accepts_0_6b_generic_profile() {
        let dir = env::temp_dir().join(format!(
            "sim_cli_qwen3_guest_dense_reference_runtime_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("temp qwen3 runtime dir");
        fs::write(
            dir.join("config.json"),
            r#"{
                "_name_or_path": "Qwen/Qwen3-0.6B",
                "vocab_size": 151936,
                "hidden_size": 1024,
                "intermediate_size": 3072,
                "num_hidden_layers": 28,
                "num_attention_heads": 16,
                "num_key_value_heads": 8,
                "head_dim": 128,
                "max_position_embeddings": 40960,
                "rope_theta": 1000000
            }"#,
        )
        .expect("write config");
        fs::write(dir.join("tokenizer.json"), b"{}").expect("write tokenizer");
        fs::write(dir.join("model.safetensors"), b"stub").expect("write weights");

        let args = Qwen3GuestDecodeLoopCliArgs {
            step_count: 1,
            prompt: None,
            prompt_token_ids: None,
            script_path: PathBuf::from("guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh"),
            matmul_batch: None,
            model: None,
            weights_path: Some(dir.clone()),
            w5_profile: None,
            engram: Qwen3EngramConfig::default(),
            memory_bootstrap: None,
            memory_decisions: None,
        };
        let runtime = qwen3_guest_dense_runtime(&args).expect("dense runtime");
        assert_eq!(runtime.model_key, "qwen3-0-6b");
        assert_eq!(runtime.chipbackend_profile, "qwen3_dense");
        assert_eq!(
            qwen3_guest_default_w5_profile(&runtime, &Qwen3EngramConfig::default()),
            "qwen3_0_6b_decode"
        );

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn qwen3_guest_dense_runtime_detects_reference_shape_without_model_id() {
        let dir = env::temp_dir().join(format!(
            "Qwen3-reference-sim-cli-runtime-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("temp qwen3 runtime dir");
        fs::write(
            dir.join("config.json"),
            r#"{
                "vocab_size": 151936,
                "hidden_size": 1024,
                "intermediate_size": 3072,
                "num_hidden_layers": 28,
                "num_attention_heads": 16,
                "num_key_value_heads": 8,
                "head_dim": 128,
                "max_position_embeddings": 40960,
                "rope_theta": 1000000
            }"#,
        )
        .expect("write config");
        fs::write(dir.join("tokenizer.json"), b"{}").expect("write tokenizer");
        fs::write(dir.join("model.safetensors"), b"stub").expect("write weights");

        let args = Qwen3GuestDecodeLoopCliArgs {
            step_count: 1,
            prompt: None,
            prompt_token_ids: None,
            script_path: PathBuf::from("guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh"),
            matmul_batch: None,
            model: None,
            weights_path: Some(dir.clone()),
            w5_profile: None,
            engram: Qwen3EngramConfig::default(),
            memory_bootstrap: None,
            memory_decisions: None,
        };
        let runtime = qwen3_guest_dense_runtime(&args).expect("reference shape runtime");
        assert!(runtime
            .model_key
            .starts_with("qwen3-reference-sim-cli-runtime"));
        assert_eq!(runtime.chipbackend_profile, "qwen3_dense");

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn qwen3_guest_dense_runtime_accepts_14b_generic_profile() {
        let dir = env::temp_dir().join(format!(
            "sim_cli_qwen3_guest_dense_14b_runtime_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("temp qwen3 runtime dir");
        fs::write(
            dir.join("config.json"),
            r#"{
                "_name_or_path": "Qwen/Qwen3-14B",
                "vocab_size": 151936,
                "hidden_size": 5120,
                "intermediate_size": 17408,
                "num_hidden_layers": 40,
                "num_attention_heads": 40,
                "num_key_value_heads": 8,
                "head_dim": 128,
                "max_position_embeddings": 40960,
                "rope_theta": 1000000
            }"#,
        )
        .expect("write config");
        fs::write(dir.join("tokenizer.json"), b"{}").expect("write tokenizer");
        fs::write(dir.join("model.safetensors.index.json"), b"{}").expect("write index");

        let args = Qwen3GuestDecodeLoopCliArgs {
            step_count: 1,
            prompt: None,
            prompt_token_ids: None,
            script_path: PathBuf::from("guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh"),
            matmul_batch: None,
            model: None,
            weights_path: Some(dir.clone()),
            w5_profile: None,
            engram: Qwen3EngramConfig::default(),
            memory_bootstrap: None,
            memory_decisions: None,
        };
        let runtime = qwen3_guest_dense_runtime(&args).expect("14B generic runtime");
        assert_eq!(runtime.model_key, "qwen3-14b");
        assert_eq!(runtime.chipbackend_profile, "qwen3_dense");
        assert_eq!(runtime.profile.hidden_size, 5120);
        assert_eq!(runtime.profile.num_hidden_layers, 40);
        let engram = Qwen3EngramConfig {
            enabled: true,
            pool: Qwen3EngramPool::Obmm,
            ..Qwen3EngramConfig::default()
        };
        assert_eq!(
            qwen3_guest_default_w5_profile(&runtime, &engram),
            "qwen3_14b_engram_decode"
        );

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn validate_w5_inference_profile_accepts_known_values() {
        assert_eq!(
            validate_w5_inference_profile("qwen3_0_6b_decode").expect("valid profile"),
            "qwen3_0_6b_decode"
        );
        assert!(validate_w5_inference_profile("w4_guest").is_err());
        assert!(validate_w5_inference_profile("qwen3_prefill_decode").is_err());
    }

    #[test]
    fn qwen3_guest_log_match_count_counts_worker_markers() {
        let log = "\
stage uapi_qwen3_range_runtime_forward node=0
stage qwen3_range_forward_runtime_input_loaded node=2
stage qwen3_range_forward_runtime_output_publish node=1
stage uapi_qwen3_range_runtime_forward node=1
stage qwen3_range_forward_runtime_output_publish node=2
";
        assert_eq!(
            qwen3_guest_log_match_count(log, "stage uapi_qwen3_range_runtime_forward "),
            2
        );
        assert_eq!(
            qwen3_guest_log_match_count(log, "stage qwen3_range_forward_runtime_input_loaded "),
            1
        );
        assert_eq!(
            qwen3_guest_log_match_count(log, "stage qwen3_range_forward_runtime_output_publish "),
            2
        );
    }

    #[test]
    fn qwen3_guest_terminal_tokens_parse_in_step_order() {
        let log = "\
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=1 token=38511 status=ok
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=0 token=99710 status=ok
";
        assert_eq!(qwen3_guest_terminal_tokens(log), vec![99710, 38511]);
    }

    #[test]
    fn qwen3_guest_engram_selected_tokens_parse_in_step_order() {
        let log = "\
[w4_guest] stage qwen3_engram_token_select local=node8 step=1 history_tokens=5 raw_token=1 runner_up=2 selected_token=358 blocked=0 fallback=0 status=ok
[w4_guest] stage qwen3_engram_token_select local=node8 step=0 history_tokens=4 raw_token=3 runner_up=4 selected_token=11 blocked=0 fallback=0 status=ok
";
        assert_eq!(qwen3_guest_engram_selected_tokens(log), vec![11, 358]);
    }

    #[test]
    fn qwen3_guest_engram_select_history_lengths_parse_in_step_order() {
        let log = "\
[w4_guest] stage qwen3_engram_token_select local=node8 step=1 history_tokens=5 raw_token=1 runner_up=2 selected_token=358 blocked=0 fallback=0 status=ok
[w4_guest] stage qwen3_engram_token_select local=node8 step=0 history_tokens=4 raw_token=3 runner_up=4 selected_token=11 blocked=0 fallback=0 status=ok
";
        assert_eq!(qwen3_guest_engram_select_history_lengths(log), vec![4, 5]);
    }

    #[test]
    fn qwen3_guest_engram_history_lengths_parse_in_step_order() {
        let log = "\
[w4_guest] stage qwen3_engram_decision_publish local=node8 step=1 objects=3 history_tokens=6 selected_token=358 status=ok
[w4_guest] stage qwen3_engram_decision_publish local=node8 step=0 objects=3 history_tokens=5 selected_token=11 status=ok
";
        assert_eq!(qwen3_guest_engram_history_lengths(log), vec![5, 6]);
    }

    #[test]
    fn qwen3_guest_engram_candidate_counts_parse_in_step_order() {
        let log = "\
[w4_guest] stage qwen3_engram_candidates_publish local=node8 step=1 candidate_count=2 status=ok
[w4_guest] stage qwen3_engram_candidates_publish local=node8 step=0 candidate_count=1 status=ok
";
        assert_eq!(qwen3_guest_engram_candidate_counts(log), vec![1, 2]);
    }

    #[test]
    fn qwen3_guest_candidate_records_parse_terminal_top_and_runner_up() {
        let log = "\
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=0 token=11 runner_up=358 margin_milli=122 text_checksum=0xd47f6aad369a54ea status=ok
";
        let records = qwen3_guest_terminal_candidate_records(log);
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].len(), 2);
        assert_eq!(records[0][0].token_id, 11);
        assert_eq!(records[0][0].logit_milli, 122);
        assert_eq!(records[0][0].token_piece_checksum, 0xd47f6aad369a54ea);
        assert_eq!(records[0][1].token_id, 358);
    }

    #[test]
    fn qwen3_guest_candidate_records_prefer_engram_raw_candidates() {
        let log = "\
[w4_guest] stage qwen3_engram_token_select local=node8 step=0 history_tokens=5 raw_token=2776 runner_up=1079 selected_token=1079 candidate_count=4 candidate2=264 candidate3=11 blocked=1 fallback=0 top_score_milli=606 runner_up_score_milli=0 status=ok
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=0 token=1079 runner_up=1079 margin_milli=606 text_checksum=0xea198295636f6f11 status=ok
";
        let records = qwen3_guest_candidate_records(log);
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].len(), 4);
        assert_eq!(records[0][0].token_id, 2776);
        assert_eq!(records[0][0].logit_milli, 606);
        assert_eq!(records[0][1].token_id, 1079);
        assert_eq!(records[0][1].logit_milli, 0);
        assert_eq!(records[0][2].token_id, 264);
        assert_eq!(records[0][2].rank, 2);
        assert_eq!(records[0][3].token_id, 11);
        assert_eq!(records[0][3].rank, 3);
    }

    #[test]
    fn qwen3_engram_neutral_policy_preserves_terminal_tokens() {
        let config = Qwen3EngramConfig {
            enabled: true,
            ..Qwen3EngramConfig::default()
        };
        let log = "\
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=0 token=11 runner_up=0 margin_milli=122 text_checksum=0x1 status=ok
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=1 token=358 runner_up=1128 margin_milli=1350 text_checksum=0x2 status=ok
";
        let report =
            qwen3_guest_engram_report(&config, 7, &[9707, 1207], log).expect("build engram report");
        assert_eq!(report.selected_tokens, vec![11, 358]);
        assert_eq!(report.candidate_count, 4);
    }

    #[test]
    fn qwen3_engram_no_repeat_ngram_blocks_repeated_candidate() {
        let config = Qwen3EngramConfig {
            enabled: true,
            no_repeat_ngram_size: 2,
            ..Qwen3EngramConfig::default()
        };
        let candidates = vec![
            Qwen3CandidateRecord {
                step_index: 0,
                rank: 0,
                token_id: 2,
                logit_milli: 100,
                adjusted_score_milli: 100,
                token_piece_checksum: 0,
            },
            Qwen3CandidateRecord {
                step_index: 0,
                rank: 1,
                token_id: 3,
                logit_milli: 0,
                adjusted_score_milli: 0,
                token_piece_checksum: 0,
            },
        ];
        let decision =
            qwen3_engram_select_token(&config, 1, &[1, 2, 1], candidates).expect("select token");
        assert_eq!(decision.selected_token, 3);
        assert_eq!(decision.blocked_token_count, 1);
        assert!(!decision.fallback_used);
    }

    #[test]
    fn qwen3_engram_block_token_id_selects_runner_up() {
        let config = Qwen3EngramConfig {
            enabled: true,
            blocked_token_ids: vec![2776],
            ..Qwen3EngramConfig::default()
        };
        let candidates = vec![
            Qwen3CandidateRecord {
                step_index: 0,
                rank: 0,
                token_id: 2776,
                logit_milli: 606,
                adjusted_score_milli: 606,
                token_piece_checksum: 0,
            },
            Qwen3CandidateRecord {
                step_index: 0,
                rank: 1,
                token_id: 1079,
                logit_milli: 0,
                adjusted_score_milli: 0,
                token_piece_checksum: 0,
            },
        ];
        let decision =
            qwen3_engram_select_token(&config, 1, &[9707, 1207], candidates).expect("select token");
        assert_eq!(decision.selected_token, 1079);
        assert_eq!(decision.blocked_token_count, 1);
        assert!(!decision.fallback_used);
        assert_eq!(decision.state.raw_sampled_token, 2776);
        assert_eq!(decision.state.runner_up_token, 1079);
        assert_eq!(decision.state.top_score_milli, 606);
        assert_eq!(decision.state.runner_up_score_milli, 0);
        assert_eq!(decision.state.history_window, 0);
        assert_ne!(decision.state.logits_checksum, 0);
        let state_words = qwen3_engram_state_words(&decision.state);
        assert_eq!(state_words.len(), 16);
        assert_eq!(state_words[2], 1079);
        assert_eq!(state_words[6], 1);
        assert_eq!(state_words[7], 0);
        assert_eq!(state_words[8], 2776);
        assert_eq!(state_words[9], 1079);
    }

    #[test]
    fn qwen3_engram_repetition_penalty_can_downrank_repeated_token() {
        let config = Qwen3EngramConfig {
            enabled: true,
            repetition_penalty_milli: 2000,
            ..Qwen3EngramConfig::default()
        };
        let candidates = vec![
            Qwen3CandidateRecord {
                step_index: 4,
                rank: 0,
                token_id: 9,
                logit_milli: 100,
                adjusted_score_milli: 100,
                token_piece_checksum: 0,
            },
            Qwen3CandidateRecord {
                step_index: 4,
                rank: 1,
                token_id: 10,
                logit_milli: 0,
                adjusted_score_milli: 0,
                token_piece_checksum: 0,
            },
        ];
        let decision =
            qwen3_engram_select_token(&config, 1, &[7, 9], candidates).expect("select token");
        assert_eq!(decision.selected_token, 10);
        assert_eq!(decision.candidates[0].adjusted_score_milli, -900);
    }

    #[test]
    fn qwen3_engram_neutral_tie_preserves_top_rank() {
        let config = Qwen3EngramConfig {
            enabled: true,
            ..Qwen3EngramConfig::default()
        };
        let candidates = vec![
            Qwen3CandidateRecord {
                step_index: 4,
                rank: 0,
                token_id: 20,
                logit_milli: 0,
                adjusted_score_milli: 0,
                token_piece_checksum: 0,
            },
            Qwen3CandidateRecord {
                step_index: 4,
                rank: 1,
                token_id: 10,
                logit_milli: 0,
                adjusted_score_milli: 0,
                token_piece_checksum: 0,
            },
        ];
        let decision =
            qwen3_engram_select_token(&config, 1, &[], candidates).expect("select token");
        assert_eq!(decision.selected_token, 20);
    }

    #[test]
    fn qwen3_engram_stop_token_priority_bypasses_policy() {
        let config = Qwen3EngramConfig {
            enabled: true,
            no_repeat_ngram_size: 2,
            repetition_penalty_milli: 2000,
            ..Qwen3EngramConfig::default()
        };
        let candidates = vec![
            Qwen3CandidateRecord {
                step_index: 2,
                rank: 0,
                token_id: 151_645,
                logit_milli: 1,
                adjusted_score_milli: 1,
                token_piece_checksum: 0,
            },
            Qwen3CandidateRecord {
                step_index: 2,
                rank: 1,
                token_id: 42,
                logit_milli: 1000,
                adjusted_score_milli: 1000,
                token_piece_checksum: 0,
            },
        ];
        let decision =
            qwen3_engram_select_token(&config, 1, &[151_645], candidates).expect("select token");
        assert_eq!(decision.selected_token, 151_645);
        assert_eq!(decision.blocked_token_count, 0);
    }

    #[test]
    fn qwen3_engram_policy_checksum_is_deterministic() {
        let config = Qwen3EngramConfig {
            enabled: true,
            repetition_penalty_milli: 1250,
            ..Qwen3EngramConfig::default()
        };
        let log = "\
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=0 token=11 runner_up=0 margin_milli=122 text_checksum=0x1 status=ok
";
        let first =
            qwen3_guest_engram_report(&config, 7, &[9707], log).expect("first engram report");
        let second =
            qwen3_guest_engram_report(&config, 7, &[9707], log).expect("second engram report");
        assert_eq!(first.state_checksum, second.state_checksum);
        assert_eq!(
            qwen3_engram_policy_checksum(&config, &first.steps),
            qwen3_engram_policy_checksum(&config, &second.steps)
        );
    }

    #[test]
    fn qwen3_engram_object_pool_publishes_versioned_records() {
        let config = Qwen3EngramConfig {
            enabled: true,
            pool: Qwen3EngramPool::Object,
            ..Qwen3EngramConfig::default()
        };
        let log = "\
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=0 token=11 runner_up=0 margin_milli=122 text_checksum=0x1 status=ok
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=1 token=358 runner_up=1128 margin_milli=1350 text_checksum=0x2 status=ok
";
        let report = qwen3_guest_engram_report(&config, 7, &[9707, 1207], log)
            .expect("build object-backed engram report");
        let object = report.object_service.expect("object report");
        assert_eq!(object.object_puts, 9);
        assert_eq!(object.object_resolves, 3);
        assert_eq!(object.token_history_versions, 3);
        assert_eq!(object.state_versions, 2);
        assert_eq!(object.candidate_versions, 2);
        assert_eq!(object.selected_token_versions, 2);
        assert_eq!(object.history_token_count, 4);
        assert_eq!(object.obmm_payload_writes, 0);
        assert_eq!(object.obmm_payload_reads, 0);
        assert_eq!(object.obmm_queue_submits, 0);
        assert_eq!(object.obmm_queue_delivers, 0);
    }

    #[test]
    fn qwen3_engram_obmm_pool_uses_payload_descriptors() {
        let config = Qwen3EngramConfig {
            enabled: true,
            pool: Qwen3EngramPool::Obmm,
            ..Qwen3EngramConfig::default()
        };
        let log = "\
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=0 token=11 runner_up=0 margin_milli=122 text_checksum=0x1 status=ok
[w4_guest] stage qwen3_engram_candidates_publish local=node8 step=0 candidate_count=4 status=ok
[w4_guest] stage qwen3_engram_candidates_wait step=0 bytes=256 status=ok
[w4_guest] stage qwen3_engram_decision_publish local=node8 step=0 objects=3 history_tokens=2 selected_token=11 status=ok
[w4_guest] stage qwen3_engram_selected_token_wait step=0 bytes=64 token=11 status=ok
[w4_guest] stage qwen3_engram_selected_writeback local=node8 step=0 selected_token=11 status=ok
";
        let report = qwen3_guest_engram_report(&config, 7, &[9707], log)
            .expect("build obmm-backed engram report");
        assert!(report.object_service.is_none());
        let transport = qwen3_guest_engram_object_transport_report(log);
        assert_eq!(transport.object_puts, 4);
        assert_eq!(transport.object_waits, 2);
        assert_eq!(transport.candidate_publishes, 1);
        assert_eq!(transport.candidate_waits, 1);
        assert_eq!(transport.decision_publishes, 1);
        assert_eq!(transport.selected_waits, 1);
        assert_eq!(transport.selected_writebacks, 1);
        assert_eq!(transport.payload_write_bytes, 256 + (2 + 2) * 8 + 64 + 128);
        assert_eq!(transport.payload_read_bytes, 256 + 64);
        assert_eq!(transport.queue_submits, 4);
        assert_eq!(transport.queue_delivers, 2);
    }

    #[test]
    fn qwen3_guest_engram_report_from_guest_log_uses_guest_decisions() {
        let config = Qwen3EngramConfig {
            enabled: true,
            pool: Qwen3EngramPool::Obmm,
            blocked_token_ids: vec![2776],
            ..Qwen3EngramConfig::default()
        };
        let log = "\
[w4_guest] stage qwen3_engram_token_select local=node8 step=0 history_tokens=5 raw_token=2776 runner_up=1079 selected_token=1079 candidate_count=4 candidate2=264 candidate3=11 blocked=1 fallback=0 top_score_milli=606 runner_up_score_milli=101 no_repeat_ngram_size=0 repetition_penalty_milli=1000 history_window=0 candidate_checksum=0x1 source=guest_policy status=ok
[w4_guest] stage qwen3_engram_decision_publish local=node8 step=0 objects=3 history_tokens=6 selected_token=1079 history_key=qwen3/session/abc/tokens/history history_version=1 selected_key=qwen3/session/abc/step/0/tokens/selected state_key=qwen3/session/abc/step/0/engram/state history_checksum=0x11 selected_checksum=0x22 state_checksum=0x33 status=ok
[w4_guest] stage qwen3_terminal_token_result_publish local=node8 step=0 token=1079 runner_up=1079 margin_milli=606 text_checksum=0x1 status=ok
";
        let report = qwen3_guest_engram_report_from_guest_log(&config, 7, &[9707], log)
            .expect("guest log report");
        assert_eq!(report.selected_tokens, vec![1079]);
        assert_eq!(report.history_tokens, vec![9707, 1079]);
        assert_eq!(report.candidate_count, 4);
        assert_eq!(report.blocked_token_count, 1);
        assert_eq!(report.state_checksum, 0x33);
        assert_eq!(report.steps[0].state.rolling_hash, 0x11);
        assert_eq!(qwen3_guest_engram_expected_terminal_rewrites(&report), 1);
        assert!(report.object_service.is_none());
    }

    #[test]
    fn qwen3_guest_terminal_text_decodes_tokenizer_bytes() {
        let dir = env::temp_dir().join(format!(
            "sim-cli-qwen3-tokenizer-test-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("create tokenizer test dir");
        fs::write(dir.join("tokenizer_config.json"), "{}").expect("write tokenizer config");
        fs::write(dir.join("vocab.json"), "{}").expect("write vocab");
        fs::write(
            dir.join("tokenizer.json"),
            r#"{"model":{"vocab":{"Hello":0,"Ġworld":1}}}"#,
        )
        .expect("write tokenizer json");

        let text = qwen3_guest_terminal_text_lossy_from_tokenizer(&[0, 1], &dir)
            .expect("decode terminal text");
        assert_eq!(text, "Hello world");

        fs::remove_dir_all(&dir).expect("remove tokenizer test dir");
    }

    #[test]
    fn qwen3_guest_timing_summary_tracks_slowest_stages() {
        let log = "\
[w4_guest] stage qwen3_worker_timing local=nodeA step=0 node=1 layers=[0,4) count=4 next=2 total_ms=100 terminal_gate_ms=0 setup_ms=31 obmm_stage_ms=10 cluster_ms=20 map_ms=1 seed_payload_ms=9 descriptor_ms=6 input_wait_ms=0 compute_window_ms=30 submit_ms=2 base_submit_ms=1 doorbell_submit_ms=1 max_batch_submit_ms=1 dispatch_ms=20 doorbell_log_ms=1 batch_sleep_ms=4 post_batch_ms=1 completion_decode_ms=1 compute_unaccounted_ms=1 publish_ms=4 verify_publish_ms=4 round_done_ms=1 barrier_ms=0 unaccounted_ms=19 dispatch_ms_per_layer_milli=5000
[w4_guest] stage qwen3_worker_timing local=nodeB step=0 node=2 layers=[4,8) count=4 next=3 total_ms=250 terminal_gate_ms=0 setup_ms=2 obmm_stage_ms=0 cluster_ms=0 map_ms=1 seed_payload_ms=11 descriptor_ms=9 input_wait_ms=90 compute_window_ms=120 submit_ms=3 base_submit_ms=1 doorbell_submit_ms=2 max_batch_submit_ms=2 dispatch_ms=100 doorbell_log_ms=2 batch_sleep_ms=5 post_batch_ms=2 completion_decode_ms=6 compute_unaccounted_ms=2 publish_ms=4 verify_publish_ms=4 round_done_ms=1 barrier_ms=0 unaccounted_ms=13 dispatch_ms_per_layer_milli=25000
[w4_guest] stage qwen3_worker_barrier_timing local=nodeB step=0 node=2 barrier_ms=77 total_with_barrier_ms=327
";
        let summary = qwen3_guest_timing_summary(log);

        assert_eq!(summary.worker_count, 2);
        assert_eq!(summary.max_total_ms, 250);
        assert_eq!(summary.max_setup_ms, 31);
        assert_eq!(summary.max_seed_payload_ms, 11);
        assert_eq!(summary.max_descriptor_ms, 9);
        assert_eq!(summary.max_compute_window_ms, 120);
        assert_eq!(summary.max_submit_ms, 3);
        assert_eq!(summary.max_base_submit_ms, 1);
        assert_eq!(summary.max_doorbell_submit_ms, 2);
        assert_eq!(summary.max_batch_submit_ms, 2);
        assert_eq!(summary.max_dispatch_ms, 100);
        assert_eq!(summary.max_doorbell_log_ms, 2);
        assert_eq!(summary.max_batch_sleep_ms, 5);
        assert_eq!(summary.max_post_batch_ms, 2);
        assert_eq!(summary.max_completion_decode_ms, 6);
        assert_eq!(summary.max_compute_unaccounted_ms, 2);
        assert_eq!(summary.max_publish_ms, 4);
        assert_eq!(summary.max_input_wait_ms, 90);
        assert_eq!(summary.max_unaccounted_ms, 19);
        assert_eq!(summary.max_barrier_ms, 77);
    }

    #[test]
    fn qwen3_guest_log_dir_from_script_output_uses_run_id() {
        let log_dir = qwen3_guest_log_dir_from_script_output(
            "[w4guest8] prepare: launch headless env run_id=2026-05-08_09-34-14_w4guest8_9435\n",
            &PathBuf::from("guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest.sh"),
        )
        .expect("log dir");
        assert_eq!(
            log_dir,
            PathBuf::from("guest-linux/aarch64/logs/2026-05-08_09-34-14_w4guest8_9435_headless8")
        );
    }

    #[test]
    fn qwen3_decode_report_verbosity_defaults_to_summary() {
        assert_eq!(
            qwen3_decode_report_verbosity_from_env(None, None),
            Qwen3DecodeReportVerbosity::Summary
        );
    }

    #[test]
    fn qwen3_decode_report_verbosity_accepts_steps() {
        assert_eq!(
            qwen3_decode_report_verbosity_from_env(Some("steps"), None),
            Qwen3DecodeReportVerbosity::Steps
        );
    }

    #[test]
    fn qwen3_decode_report_verbosity_accepts_verbose() {
        assert_eq!(
            qwen3_decode_report_verbosity_from_env(Some("verbose"), None),
            Qwen3DecodeReportVerbosity::Verbose
        );
        assert_eq!(
            qwen3_decode_report_verbosity_from_env(None, Some("1")),
            Qwen3DecodeReportVerbosity::Verbose
        );
    }

    #[test]
    fn lingqu_object_service_args_detects_command() {
        assert!(lingqu_object_service_args_from(["lingqu-object-service"]));
        assert!(!lingqu_object_service_args_from(["qwen3-decode-loop"]));
    }

    #[test]
    fn lingqu_durable_args_detects_command() {
        assert!(lingqu_durable_args_from(["lingqu-durable"]));
        assert!(!lingqu_durable_args_from(["lingqu-memory"]));
    }

    #[test]
    fn lingqu_durable_cli_init_and_stat_runs() {
        let root = env::temp_dir().join(format!(
            "sim-cli-lingqu-durable-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time")
                .as_nanos()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let store_path = root.join("durable.json");
        let args = vec!["--store".to_string(), store_path.display().to_string()];

        run_lingqu_durable_init_cli(&args).expect("init durable sim");
        run_lingqu_durable_stat_cli(&args).expect("stat durable sim");

        let store_bytes = fs::read(&store_path).expect("read durable sim snapshot");
        let snapshot =
            LingquDurableSimSnapshot::from_json_bytes(&store_bytes).expect("decode snapshot");
        assert_eq!(snapshot.kind, "lingqu_durable_sim");
        assert_eq!(snapshot.schema_version, 1);
        assert!(snapshot.dfs.files.is_empty());
        assert!(snapshot.block.blocks.is_empty());

        fs::remove_dir_all(&root).expect("remove temp dir");
    }

    #[test]
    fn lingqu_durable_cli_lists_validates_and_replays_append_log() {
        let root = env::temp_dir().join(format!(
            "sim-cli-lingqu-durable-ops-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time")
                .as_nanos()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let store_path = root.join("durable.json");
        let batch_manifest_path = root.join("batch.json");
        let mut sim = LingquDurableSim::default();
        sim.dfs_write(
            "/lingqu/memory/catalogs/test.json",
            br#"{"catalog":"test"}"#.to_vec(),
            sim_services::durable::LingquDfsWriteOptions::default(),
        )
        .expect("write DFS catalog");
        save_lingqu_durable_sim(&store_path, &sim).expect("save durable sim");

        run_lingqu_durable_list_cli(&[
            "--store".to_string(),
            store_path.display().to_string(),
            "--prefix".to_string(),
            "/lingqu/memory/catalogs/".to_string(),
        ])
        .expect("list durable DFS");
        run_lingqu_durable_append_log_cli(&[
            "--store".to_string(),
            store_path.display().to_string(),
            "--log".to_string(),
            "/lingqu/memory/audit/test.log".to_string(),
            "--payload".to_string(),
            "{\"event\":\"created\"}".to_string(),
            "--expected-next-seq".to_string(),
            "1".to_string(),
        ])
        .expect("append durable log");
        run_lingqu_durable_read_log_cli(&[
            "--store".to_string(),
            store_path.display().to_string(),
            "--log".to_string(),
            "/lingqu/memory/audit/test.log".to_string(),
        ])
        .expect("read durable log");
        run_lingqu_durable_validate_cli(&["--store".to_string(), store_path.display().to_string()])
            .expect("validate durable store");
        fs::write(
            &batch_manifest_path,
            serde_json::json!({
                "ops": [
                    {
                        "kind": "block_write",
                        "block": "block/memory/batch/cli",
                        "payload": "batch-block"
                    },
                    {
                        "kind": "dfs_write",
                        "path": "/lingqu/memory/catalogs/batch.json",
                        "payload": "{\"catalog\":\"batch\"}"
                    },
                    {
                        "kind": "dfs_append_log",
                        "path": "/lingqu/memory/audit/test.log",
                        "payload": "{\"event\":\"batched\"}"
                    }
                ]
            })
            .to_string(),
        )
        .expect("write batch manifest");
        run_lingqu_durable_batch_cli(&[
            "--store".to_string(),
            store_path.display().to_string(),
            "--manifest".to_string(),
            batch_manifest_path.display().to_string(),
        ])
        .expect("commit durable batch");

        let store_bytes = fs::read(&store_path).expect("read durable store");
        let snapshot =
            LingquDurableSimSnapshot::from_json_bytes(&store_bytes).expect("decode snapshot");
        assert_eq!(snapshot.dfs.files.len(), 2);
        assert_eq!(snapshot.dfs.append_logs.len(), 2);
        assert_eq!(snapshot.block.blocks.len(), 1);
        assert_eq!(snapshot.dfs.append_logs[0].seq, 1);
        assert_eq!(snapshot.dfs.append_logs[1].seq, 2);

        fs::remove_dir_all(&root).expect("remove temp dir");
    }

    #[test]
    fn lingqu_memory_store_load_save_migrates_legacy_snapshot() {
        let root = env::temp_dir().join(format!(
            "sim-cli-lingqu-memory-legacy-store-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time")
                .as_nanos()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let store_path = root.join("store.json");
        let mut legacy_store = LingquMemoryDurableStore::new();
        let payload_ref = legacy_store
            .write_block_payload("block/legacy/payload", b"legacy payload".to_vec())
            .expect("write legacy payload");
        let legacy_snapshot = legacy_store.export_snapshot().expect("export legacy view");
        fs::write(
            &store_path,
            legacy_snapshot
                .to_json_bytes()
                .expect("encode legacy durable store"),
        )
        .expect("write legacy durable store");

        let mut migrated = load_lingqu_memory_durable_store(&store_path)
            .expect("load legacy durable store through migration path");
        assert_eq!(
            migrated
                .read_block_payload(&payload_ref)
                .expect("read migrated payload"),
            b"legacy payload".to_vec()
        );
        save_lingqu_memory_durable_store(&store_path, &migrated)
            .expect("save migrated durable store");

        let store_bytes = fs::read(&store_path).expect("read migrated durable store");
        let snapshot =
            LingquDurableSimSnapshot::from_json_bytes(&store_bytes).expect("decode new snapshot");
        assert_eq!(snapshot.kind, "lingqu_durable_sim");
        assert_eq!(snapshot.block.blocks.len(), 1);
        assert!(LingquMemoryDurableStoreSnapshot::from_json_bytes(&store_bytes).is_err());

        fs::remove_dir_all(&root).expect("remove temp dir");
    }

    #[test]
    fn lingqu_memory_args_detects_command() {
        assert!(lingqu_memory_args_from(["lingqu-memory"]));
        assert!(!lingqu_memory_args_from(["lingqu-object-service"]));
    }

    #[test]
    fn lingqu_memory_durable_store_cli_smoke_runs() {
        run_lingqu_memory_validate_durable_store().expect("durable store validation");
    }

    #[test]
    fn lingqu_memory_flat_query_cli_smoke_runs() {
        run_lingqu_memory_validate_flat_query().expect("flat query validation");
    }

    #[test]
    fn lingqu_memory_flat_materialize_cli_smoke_runs() {
        run_lingqu_memory_validate_flat_materialize().expect("flat materialize validation");
    }

    #[test]
    fn lingqu_memory_w5_engram_object_ref_cli_smoke_runs() {
        run_lingqu_memory_validate_w5_engram_object_ref().expect("w5 engram object-ref validation");
    }

    #[test]
    fn lingqu_memory_execution_artifact_cli_runs_boundary_and_prefetch() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_execution_artifact_cli_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let store = root.join("store.json");
        let logits_artifact_path = root.join("logits_artifact.json");
        let kv_artifact_path = root.join("kv_artifact.json");
        let boundary_request_path = root.join("boundary_lookup_request.json");
        let boundary_response_path = root.join("boundary_lookup_response.json");
        let observation_response_path = root.join("boundary_observation_response.json");
        let prefetch_request_path = root.join("prefetch_plan_request.json");
        let prefetch_plan_path = root.join("prefetch_plan.json");
        let mut seed_store = LingquMemoryDurableStore::new();
        let logits_payload_ref = seed_store
            .write_block_payload(
                "block/logits/step3/node4",
                sample_w5_terminal_logits_payload(),
            )
            .expect("write logits payload");
        let kv_payload_ref = seed_store
            .write_block_payload("block/kv/step4/node4", vec![0x77; 16])
            .expect("write kv payload");
        save_lingqu_memory_durable_store(&store, &seed_store)
            .expect("save seeded durable payloads");
        let model = sim_memory::InferenceModelBinding {
            model_id: "qwen3-test".to_string(),
            model_key: "qwen3-test-key".to_string(),
            tokenizer_hash: 0x1001,
            profile_hash: 0x2002,
        };
        let exit_boundary = sim_memory::RangeBoundary {
            phase: sim_memory::RangeBoundaryPhase::RangeExit,
            step_index: 3,
            node_index: 4,
            layer_start: 4,
            layer_end: 8,
            next_node_index: Some(5),
            position: 12,
        };
        let start_boundary = sim_memory::RangeBoundary {
            phase: sim_memory::RangeBoundaryPhase::RangeStart,
            step_index: 3,
            node_index: 4,
            layer_start: 8,
            layer_end: 12,
            next_node_index: Some(5),
            position: 12,
        };
        let hidden_ref = sim_memory::HotTensorObjectRef {
            object_key: "hidden/range/node4/step3".to_string(),
            version: 1,
            backend: sim_memory::HotObjectBackend::ObmmShmem,
            storage_ref: "obmm://hidden/range/node4/step3".to_string(),
            segment: None,
            offset: 0,
            bytes: 16,
            checksum: 0x4444_4444,
            dtype: sim_core::TensorDType::F32,
            shape: vec![1, 4],
        };
        let logits_artifact = sim_memory::ExecutionArtifactObject {
            artifact_id: "artifact/logits/step3/node4".to_string(),
            kind: sim_memory::ExecutionArtifactKind::Logits,
            model: model.clone(),
            producer_boundary: exit_boundary.clone(),
            boundary_hidden_fingerprint: sim_memory::BoundaryTensorFingerprint::from_hot_ref(
                &hidden_ref,
            ),
            target_layer_start: 8,
            target_layer_end: 8,
            dtype: sim_core::TensorDType::F32,
            shape: vec![1, 4],
            durable_payload_ref: Some(logits_payload_ref),
            hot_object_ref: None,
            source_query_result_id: None,
            source_engram_state_id: None,
            confidence_milli: 980,
            state: sim_memory::ExecutionArtifactState::Verified,
            checksum: 0x6666_6666,
            version: 1,
            created_at_us: 10,
            expires_at_us: Some(100),
        };
        let kv_artifact = sim_memory::ExecutionArtifactObject {
            artifact_id: "artifact/kv/step4/node4".to_string(),
            kind: sim_memory::ExecutionArtifactKind::KvCache,
            model: model.clone(),
            producer_boundary: sim_memory::RangeBoundary {
                phase: sim_memory::RangeBoundaryPhase::RangeExit,
                step_index: 4,
                node_index: 4,
                layer_start: 8,
                layer_end: 12,
                next_node_index: Some(5),
                position: 13,
            },
            boundary_hidden_fingerprint: sim_memory::BoundaryTensorFingerprint {
                bytes: 16,
                checksum: 0x7777_7777,
                dtype: sim_core::TensorDType::F32,
                shape: vec![1, 4],
            },
            target_layer_start: 8,
            target_layer_end: 12,
            dtype: sim_core::TensorDType::F32,
            shape: vec![1, 4],
            durable_payload_ref: Some(kv_payload_ref),
            hot_object_ref: None,
            source_query_result_id: None,
            source_engram_state_id: None,
            confidence_milli: 940,
            state: sim_memory::ExecutionArtifactState::Verified,
            checksum: 0x8888_8888,
            version: 1,
            created_at_us: 11,
            expires_at_us: Some(100),
        };
        let boundary_request = sim_memory::BoundaryLookupRequest {
            request_id: "boundary/step3/node4".to_string(),
            model: model.clone(),
            boundary: exit_boundary,
            hidden_state: hidden_ref.clone(),
            engram_state_id: None,
            min_confidence_milli: 900,
            allowed_actions: vec![sim_memory::ShortpathAction::JumpToTerminal],
            created_at_us: 12,
        };
        let prefetch_request = sim_memory::PrefetchPlanRequest {
            request_id: "prefetch/step3/node4".to_string(),
            model: model.clone(),
            boundary: start_boundary,
            engram_state_id: None,
            scope: sim_memory::PrefetchScope::MultiStep,
            lookahead_steps: 2,
            artifact_kinds: vec![sim_memory::ExecutionArtifactKind::KvCache],
            created_at_us: 12,
        };
        fs::write(
            &logits_artifact_path,
            serde_json::to_vec_pretty(&logits_artifact).expect("encode logits artifact"),
        )
        .expect("write logits artifact");
        fs::write(
            &kv_artifact_path,
            serde_json::to_vec_pretty(&kv_artifact).expect("encode kv artifact"),
        )
        .expect("write kv artifact");
        fs::write(
            &boundary_request_path,
            serde_json::to_vec_pretty(&boundary_request).expect("encode boundary request"),
        )
        .expect("write boundary request");
        fs::write(
            &prefetch_request_path,
            serde_json::to_vec_pretty(&prefetch_request).expect("encode prefetch request"),
        )
        .expect("write prefetch request");

        for artifact_path in [&logits_artifact_path, &kv_artifact_path] {
            run_lingqu_memory_register_execution_artifact_cli(&[
                "--store".to_string(),
                store.to_string_lossy().into_owned(),
                "--artifact".to_string(),
                artifact_path.to_string_lossy().into_owned(),
            ])
            .expect("register execution artifact");
        }
        let observation = sim_memory::BoundaryObservationRecord::new(
            "boundary-observation/run0/step3/node4".to_string(),
            "run0".to_string(),
            model.clone(),
            sim_memory::RangeBoundary {
                phase: sim_memory::RangeBoundaryPhase::RangeExit,
                step_index: 3,
                node_index: 4,
                layer_start: 4,
                layer_end: 8,
                next_node_index: Some(5),
                position: 12,
            },
            hidden_ref.clone(),
            "node4".to_string(),
            "node5".to_string(),
            "w5_guest_range_exit".to_string(),
            1,
            12,
        )
        .expect("build boundary observation");
        let mut observation_store =
            load_lingqu_memory_durable_store(&store).expect("load store for observation");
        observation_store
            .persist_boundary_observation_manifest(vec![observation])
            .expect("persist boundary observation");
        save_lingqu_memory_durable_store(&store, &observation_store)
            .expect("save store with observation");
        let auto_lookup_store = root.join("auto_boundary_lookup_store.json");
        fs::copy(&store, &auto_lookup_store).expect("copy store for W5 auto boundary lookup");
        let auto_bundle = load_w5_memory_decisions_from_store(&W5MemoryDecisionConfig {
            store_path: auto_lookup_store.clone(),
            boundary_request_path: Some(boundary_request_path.clone()),
            boundary_observation_id: None,
            shortpath_decision_id: None,
            shortpath_execute: false,
            prefetch_plan_id: None,
            prefix_cache_reuse_plan_id: None,
        })
        .expect("W5 entrypoint should run boundary lookup from request");
        assert_eq!(
            auto_bundle
                .shortpath
                .as_ref()
                .expect("auto shortpath decision")
                .artifact_id
                .as_deref(),
            Some("artifact/logits/step3/node4")
        );
        assert_eq!(
            auto_bundle
                .shortpath_artifact
                .as_ref()
                .expect("auto shortpath artifact")
                .artifact_id,
            "artifact/logits/step3/node4"
        );
        let mut auto_durable_store = load_lingqu_memory_durable_store(&auto_lookup_store)
            .expect("load auto lookup durable store");
        let auto_decisions = auto_durable_store
            .load_shortpath_decision_manifest()
            .expect("load auto W5 planner decision audit");
        assert_eq!(auto_decisions.len(), 1);
        assert_eq!(
            auto_decisions[0].decision_id,
            "shortpath-decision/boundary/step3/node4"
        );
        let auto_observation_store = root.join("auto_boundary_observation_store.json");
        fs::copy(&store, &auto_observation_store)
            .expect("copy store for W5 auto boundary observation lookup");
        let auto_observation_bundle =
            load_w5_memory_decisions_from_store(&W5MemoryDecisionConfig {
                store_path: auto_observation_store.clone(),
                boundary_request_path: None,
                boundary_observation_id: Some("boundary-observation/run0/step3/node4".to_string()),
                shortpath_decision_id: None,
                shortpath_execute: false,
                prefetch_plan_id: None,
                prefix_cache_reuse_plan_id: None,
            })
            .expect("W5 entrypoint should run boundary lookup from observation");
        assert_eq!(
            auto_observation_bundle
                .shortpath
                .as_ref()
                .expect("auto observation shortpath decision")
                .artifact_id
                .as_deref(),
            Some("artifact/logits/step3/node4")
        );
        let observation_cli_store = root.join("boundary_observation_cli_store.json");
        fs::copy(&store, &observation_cli_store)
            .expect("copy store for boundary observation CLI lookup");
        run_lingqu_memory_boundary_lookup_from_observation_cli(&[
            "--store".to_string(),
            observation_cli_store.to_string_lossy().into_owned(),
            "--observation-id".to_string(),
            "boundary-observation/run0/step3/node4".to_string(),
            "--response".to_string(),
            observation_response_path.to_string_lossy().into_owned(),
            "--now-us".to_string(),
            "20".to_string(),
        ])
        .expect("boundary lookup from observation");
        run_lingqu_memory_boundary_lookup_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--request".to_string(),
            boundary_request_path.to_string_lossy().into_owned(),
            "--response".to_string(),
            boundary_response_path.to_string_lossy().into_owned(),
            "--now-us".to_string(),
            "20".to_string(),
        ])
        .expect("boundary lookup");
        run_lingqu_memory_plan_prefetch_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--request".to_string(),
            prefetch_request_path.to_string_lossy().into_owned(),
            "--plan".to_string(),
            prefetch_plan_path.to_string_lossy().into_owned(),
            "--now-us".to_string(),
            "21".to_string(),
        ])
        .expect("plan prefetch");
        run_lingqu_memory_list_shortpath_decisions_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--decision-id".to_string(),
            "shortpath-decision/boundary/step3/node4".to_string(),
        ])
        .expect("list shortpath decision audit");
        run_lingqu_memory_list_shortpath_supports_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--support-id".to_string(),
            "shortpath-support/boundary/step3/node4".to_string(),
        ])
        .expect("list shortpath support audit");
        run_lingqu_memory_list_prefetch_plans_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--plan-id".to_string(),
            "prefetch-plan/prefetch/step3/node4".to_string(),
        ])
        .expect("list prefetch plan audit");

        let boundary_response_bytes =
            fs::read(&boundary_response_path).expect("read boundary response");
        let boundary_response =
            serde_json::from_slice::<sim_memory::BoundaryLookupResponse>(&boundary_response_bytes)
                .expect("decode boundary response");
        assert_eq!(
            boundary_response.support.supported_action,
            sim_memory::ShortpathAction::JumpToTerminal
        );
        assert_eq!(
            boundary_response.support.artifact_id.as_deref(),
            Some("artifact/logits/step3/node4")
        );
        assert_eq!(boundary_response.support.confidence_milli, 980);

        let prefetch_plan_bytes = fs::read(&prefetch_plan_path).expect("read prefetch plan");
        let prefetch_plan =
            serde_json::from_slice::<sim_memory::PrefetchPlanRecord>(&prefetch_plan_bytes)
                .expect("decode prefetch plan");
        assert_eq!(prefetch_plan.scope, sim_memory::PrefetchScope::MultiStep);
        assert_eq!(prefetch_plan.target_step_index, 5);
        assert_eq!(
            prefetch_plan.planned_artifact_ids,
            vec!["artifact/kv/step4/node4".to_string()]
        );
        assert!(prefetch_plan.checksum != 0);
        let store_bytes = fs::read(&store).expect("read durable store");
        let durable_snapshot =
            LingquDurableSimSnapshot::from_json_bytes(&store_bytes).expect("decode durable store");
        let mut durable_store =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_snapshot)
                .expect("import durable store");
        let artifacts = durable_store
            .load_execution_artifact_manifest()
            .expect("load execution artifact manifest after restart");
        assert_eq!(artifacts.len(), 2);
        let decisions = durable_store
            .load_shortpath_decision_manifest()
            .expect("load W5 planner decision audit after restart");
        assert_eq!(decisions.len(), 1);
        assert_eq!(
            decisions[0].artifact_id.as_deref(),
            Some("artifact/logits/step3/node4")
        );
        let supports = durable_store
            .load_shortpath_support_manifest()
            .expect("load Memory Service support audit after restart");
        assert_eq!(supports.len(), 1);
        assert_eq!(
            supports[0].artifact_id.as_deref(),
            Some("artifact/logits/step3/node4")
        );
        let plans = durable_store
            .load_prefetch_plan_manifest()
            .expect("load prefetch audit after restart");
        assert_eq!(plans.len(), 1);
        assert_eq!(
            plans[0].planned_artifact_ids,
            vec!["artifact/kv/step4/node4".to_string()]
        );
        let decision_config = W5MemoryDecisionConfig {
            store_path: store.clone(),
            boundary_request_path: None,
            boundary_observation_id: None,
            shortpath_decision_id: Some("shortpath-decision/boundary/step3/node4".to_string()),
            shortpath_execute: true,
            prefetch_plan_id: Some("prefetch-plan/prefetch/step3/node4".to_string()),
            prefix_cache_reuse_plan_id: None,
        };
        let bundle = load_w5_memory_decisions_from_store(&decision_config)
            .expect("load w5 memory decision bundle");
        assert_eq!(
            bundle
                .shortpath
                .as_ref()
                .expect("shortpath decision")
                .artifact_id
                .as_deref(),
            Some("artifact/logits/step3/node4")
        );
        assert_eq!(
            bundle
                .prefetch
                .as_ref()
                .expect("prefetch plan")
                .target_step_index,
            5
        );
        assert_eq!(
            bundle
                .shortpath_artifact
                .as_ref()
                .expect("shortpath artifact")
                .artifact_id,
            "artifact/logits/step3/node4"
        );
        assert_eq!(bundle.prefetch_artifacts.len(), 1);
        assert_eq!(
            bundle.prefetch_artifacts[0].artifact_id,
            "artifact/kv/step4/node4"
        );
        let publication = publish_w5_memory_decision_artifact_refs(
            &W5MemoryBootstrapConfig {
                store_path: store.clone(),
                object_store_path: root.join("unused-object-store.json"),
                engram_state_path: root.join("unused-engram-state.json"),
                registry_dir: root.join("qwen3-object-registry"),
                owner_entity: 1,
                producer_entity: 2,
            },
            &bundle,
        )
        .expect("publish w5 memory decision artifact refs");
        let snapshot_path = publication
            .object_service_snapshot_path
            .as_ref()
            .expect("decision artifact object service snapshot");
        assert!(snapshot_path.exists());
        assert!(w5_object_service_payload_index_path(snapshot_path).exists());
        let publication_entries = fs::read_dir(root.join("qwen3-object-registry"))
            .expect("read object service export dir")
            .map(|entry| {
                entry
                    .expect("object service export entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect::<Vec<_>>();
        assert!(publication_entries
            .iter()
            .any(|entry| entry == "lingqu_object_service_snapshot.json"));
        assert!(publication_entries
            .iter()
            .any(|entry| entry == "lingqu_object_service_snapshot.bin"));
        assert!(
            publication_entries
                .iter()
                .all(|entry| !entry.starts_with("kind")),
            "Object Service publication must not create qwen registry payload files: {publication_entries:?}"
        );
        assert_eq!(
            publication
                .shortpath_ref
                .as_ref()
                .expect("shortpath ref")
                .ref_hex
                .len(),
            128
        );
        assert_eq!(publication.prefetch_refs.len(), 1);
        let env_vars = w5_memory_decision_env_vars(&decision_config, &bundle, Some(&publication));
        assert!(env_vars
            .iter()
            .any(|(key, value)| key == "SIM_W5_MEMORY_SHORTPATH_ACTION"
                && value == "jump-to-terminal"));
        assert!(env_vars.iter().any(|(key, value)| {
            key == "SIM_W5_MEMORY_SHORTPATH_SUPPORT_ID"
                && value == "shortpath-support/boundary/step3/node4"
        }));
        assert!(env_vars.iter().any(
            |(key, value)| key == "SIM_W5_MEMORY_SHORTPATH_ARTIFACT_KIND" && value == "logits"
        ));
        assert!(env_vars.iter().any(|(key, value)| {
            key == "SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_START" && value == "4"
        }));
        assert!(env_vars.iter().any(|(key, value)| {
            key == "SIM_W5_MEMORY_SHORTPATH_PRODUCER_LAYER_END" && value == "8"
        }));
        assert!(env_vars.iter().any(
            |(key, value)| key == "SIM_W5_MEMORY_SHORTPATH_ARTIFACT_REF" && value.len() == 128
        ));
        assert!(env_vars
            .iter()
            .any(|(key, value)| key == "SIM_W5_MEMORY_SHORTPATH_EXECUTE" && value == "1"));
        assert!(env_vars.iter().any(|(key, value)| {
            key == "SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_START" && value == "8"
        }));
        assert!(env_vars.iter().any(|(key, value)| {
            key == "SIM_W5_MEMORY_SHORTPATH_TARGET_LAYER_END" && value == "8"
        }));
        assert!(env_vars.iter().any(
            |(key, value)| key == "SIM_W5_MEMORY_PREFETCH_ARTIFACT_REFS" && value.len() == 128
        ));
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn lingqu_memory_boundary_lookup_cli_requires_execution_manifest() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_boundary_missing_manifest_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let store = root.join("store.json");
        let request_path = root.join("boundary_lookup_request.json");
        let response_path = root.join("boundary_lookup_response.json");
        let request = sim_memory::BoundaryLookupRequest {
            request_id: "boundary/missing-manifest".to_string(),
            model: sim_memory::InferenceModelBinding {
                model_id: "qwen3-test".to_string(),
                model_key: "qwen3-test-key".to_string(),
                tokenizer_hash: 0x1001,
                profile_hash: 0x2002,
            },
            boundary: sim_memory::RangeBoundary {
                phase: sim_memory::RangeBoundaryPhase::RangeExit,
                step_index: 1,
                node_index: 2,
                layer_start: 4,
                layer_end: 8,
                next_node_index: Some(3),
                position: 12,
            },
            hidden_state: sim_memory::HotTensorObjectRef {
                object_key: "hidden/range/node2/step1".to_string(),
                version: 1,
                backend: sim_memory::HotObjectBackend::ObmmShmem,
                storage_ref: "obmm://hidden/range/node2/step1".to_string(),
                segment: None,
                offset: 0,
                bytes: 16,
                checksum: 0x4444_4444,
                dtype: sim_core::TensorDType::F32,
                shape: vec![1, 4],
            },
            engram_state_id: None,
            min_confidence_milli: 900,
            allowed_actions: vec![sim_memory::ShortpathAction::JumpToTerminal],
            created_at_us: 12,
        };
        fs::write(
            &request_path,
            serde_json::to_vec_pretty(&request).expect("encode boundary request"),
        )
        .expect("write boundary request");

        let err = run_lingqu_memory_boundary_lookup_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--request".to_string(),
            request_path.to_string_lossy().into_owned(),
            "--response".to_string(),
            response_path.to_string_lossy().into_owned(),
            "--now-us".to_string(),
            "20".to_string(),
        ])
        .expect_err("missing execution manifest must fail boundary lookup");

        let err_text = format!("{err:#}");
        assert!(err_text.contains("load execution artifact manifest"));
        assert!(err_text.contains(sim_memory::LINGQU_EXECUTION_ARTIFACT_MANIFEST_PATH));
        assert!(!response_path.exists());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn lingqu_memory_boundary_request_from_w5_summary_uses_real_hidden_observation() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_boundary_request_from_summary_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let summary_path = root.join("w5_summary.txt");
        let request_path = root.join("boundary_request.json");
        fs::write(
            &summary_path,
            concat!(
                "summary: run_dir=/tmp/w5\n",
                "memory_boundary_observation: phase=range_exit step=2 node=node3 ",
                "target=node4 layers=[8,12) layer_start=8 layer_end=12 layer_count=4 ",
                "hidden_key=hidden/qwen3-0-6b/node4/range-runtime-input/decode-step2 ",
                "hidden_key_hash=0x000000000000abcd hidden_version=7 hidden_bytes=262144 ",
                "hidden_checksum=0x0000000000001234 hidden_dtype=opaque hidden_shape=262144 ",
                "producer_publish_ms=100 producer_publish_mono_ms=20 backing=obmm_shmem ",
                "metadata=lingqu_object_service queue=obmm_spsc status=ok\n"
            ),
        )
        .expect("write summary");

        run_lingqu_memory_boundary_request_from_w5_summary_cli(&[
            "--summary".to_string(),
            summary_path.to_string_lossy().into_owned(),
            "--output".to_string(),
            request_path.to_string_lossy().into_owned(),
            "--step".to_string(),
            "2".to_string(),
            "--node".to_string(),
            "node3".to_string(),
            "--position".to_string(),
            "6".to_string(),
            "--model-id".to_string(),
            "qwen3-0.6b-test".to_string(),
            "--model-key".to_string(),
            "qwen3-0-6b".to_string(),
            "--tokenizer-hash".to_string(),
            "0x1001".to_string(),
            "--profile-hash".to_string(),
            "0x2002".to_string(),
        ])
        .expect("build boundary request from summary");

        let request = serde_json::from_slice::<sim_memory::BoundaryLookupRequest>(
            &fs::read(&request_path).expect("read request"),
        )
        .expect("decode request");
        assert_eq!(
            request.request_id,
            "boundary/qwen3-0-6b/step2/node3/position6"
        );
        assert_eq!(request.boundary.step_index, 2);
        assert_eq!(request.boundary.node_index, 3);
        assert_eq!(request.boundary.layer_start, 8);
        assert_eq!(request.boundary.layer_end, 12);
        assert_eq!(request.boundary.next_node_index, Some(4));
        assert_eq!(request.boundary.position, 6);
        assert_eq!(
            request.hidden_state.object_key,
            "hidden/qwen3-0-6b/node4/range-runtime-input/decode-step2"
        );
        assert_eq!(request.hidden_state.version, 7);
        assert_eq!(request.hidden_state.bytes, 262144);
        assert_eq!(request.hidden_state.checksum, 0x1234);
        assert_eq!(request.hidden_state.dtype, sim_core::TensorDType::Opaque);
        assert_eq!(
            request.allowed_actions,
            vec![sim_memory::ShortpathAction::JumpToTerminal]
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn lingqu_memory_records_boundary_observations_from_w5_summary() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_record_boundary_observations_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let summary_path = root.join("w5_summary.txt");
        let store_path = root.join("store.json");
        fs::write(
            &summary_path,
            concat!(
                "summary: run_dir=/tmp/run0_headless8\n",
                "memory_boundary_observation: phase=range_exit step=2 node=node3 ",
                "target=node4 layers=[8,12) layer_start=8 layer_end=12 layer_count=4 ",
                "hidden_key=hidden/qwen3-0-6b/node4/range-runtime-input/decode-step2 ",
                "hidden_key_hash=0x000000000000abcd hidden_version=7 hidden_bytes=262144 ",
                "hidden_checksum=0x0000000000001234 hidden_dtype=opaque hidden_shape=262144 ",
                "producer_publish_ms=100 producer_publish_mono_ms=20 backing=obmm_shmem ",
                "metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
                "memory_boundary_observation: phase=range_exit step=2 node=node4 ",
                "target=node5 layers=[12,16) layer_start=12 layer_end=16 layer_count=4 ",
                "hidden_key=hidden/qwen3-0-6b/node5/range-runtime-input/decode-step2 ",
                "hidden_key_hash=0x000000000000bcde hidden_version=1 hidden_bytes=262144 ",
                "hidden_checksum=0x0000000000005678 hidden_dtype=opaque hidden_shape=262144 ",
                "producer_publish_ms=200 producer_publish_mono_ms=30 backing=obmm_shmem ",
                "metadata=lingqu_object_service queue=obmm_spsc status=ok\n"
            ),
        )
        .expect("write summary");

        run_lingqu_memory_record_boundary_observations_from_w5_summary_cli(&[
            "--store".to_string(),
            store_path.to_string_lossy().into_owned(),
            "--summary".to_string(),
            summary_path.to_string_lossy().into_owned(),
            "--step".to_string(),
            "2".to_string(),
            "--position".to_string(),
            "6".to_string(),
            "--model-id".to_string(),
            "qwen3-0.6b-test".to_string(),
            "--model-key".to_string(),
            "qwen3-0-6b".to_string(),
            "--tokenizer-hash".to_string(),
            "0x1001".to_string(),
            "--profile-hash".to_string(),
            "0x2002".to_string(),
            "--created-at-us".to_string(),
            "100".to_string(),
        ])
        .expect("record observations");

        let mut durable =
            load_lingqu_memory_durable_store(&store_path).expect("reload durable store");
        let observations = durable
            .load_boundary_observation_manifest()
            .expect("load boundary observation audit");
        assert_eq!(observations.len(), 2);
        assert_eq!(observations[0].run_id, "run0");
        assert_eq!(
            observations[0].observation_id,
            "boundary-observation/run0/step2/node3"
        );
        assert_eq!(observations[0].boundary.node_index, 3);
        assert_eq!(observations[0].boundary.position, 6);
        assert_eq!(observations[0].hidden_state.checksum, 0x1234);
        assert_ne!(observations[0].checksum, 0);

        run_lingqu_memory_record_boundary_observations_from_w5_summary_cli(&[
            "--store".to_string(),
            store_path.to_string_lossy().into_owned(),
            "--summary".to_string(),
            summary_path.to_string_lossy().into_owned(),
            "--step".to_string(),
            "2".to_string(),
            "--position".to_string(),
            "6".to_string(),
            "--model-id".to_string(),
            "qwen3-0.6b-test".to_string(),
            "--model-key".to_string(),
            "qwen3-0-6b".to_string(),
            "--tokenizer-hash".to_string(),
            "0x1001".to_string(),
            "--profile-hash".to_string(),
            "0x2002".to_string(),
            "--created-at-us".to_string(),
            "100".to_string(),
        ])
        .expect("record observations idempotently");
        let mut durable =
            load_lingqu_memory_durable_store(&store_path).expect("reload durable store");
        assert_eq!(
            durable
                .load_boundary_observation_manifest()
                .expect("reload observation audit")
                .len(),
            2
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn w5_memory_decision_load_rejects_missing_artifact_payload() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_w5_memory_decision_missing_payload_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let store = root.join("store.json");
        let model = sim_memory::InferenceModelBinding {
            model_id: "qwen3-test".to_string(),
            model_key: "qwen3-test-key".to_string(),
            tokenizer_hash: 0x1001,
            profile_hash: 0x2002,
        };
        let boundary = sim_memory::RangeBoundary {
            phase: sim_memory::RangeBoundaryPhase::RangeExit,
            step_index: 3,
            node_index: 4,
            layer_start: 4,
            layer_end: 8,
            next_node_index: Some(5),
            position: 12,
        };
        let artifact = sim_memory::ExecutionArtifactObject {
            artifact_id: "artifact/logits/missing-payload".to_string(),
            kind: sim_memory::ExecutionArtifactKind::Logits,
            model,
            producer_boundary: boundary,
            boundary_hidden_fingerprint: sim_memory::BoundaryTensorFingerprint {
                bytes: 16,
                checksum: 0x4444,
                dtype: sim_core::TensorDType::F32,
                shape: vec![1, 4],
            },
            target_layer_start: 8,
            target_layer_end: 8,
            dtype: sim_core::TensorDType::F32,
            shape: vec![1, 4],
            durable_payload_ref: Some(sim_memory::LingquBlockPayloadRef::new(
                "block/logits/missing-payload",
                0,
                16,
                0x5555,
            )),
            hot_object_ref: None,
            source_query_result_id: None,
            source_engram_state_id: None,
            confidence_milli: 980,
            state: sim_memory::ExecutionArtifactState::Verified,
            checksum: 0x6666,
            version: 1,
            created_at_us: 10,
            expires_at_us: Some(100),
        };
        let decision = sim_memory::ShortpathDecisionRecord {
            decision_id: "shortpath-decision/missing-payload".to_string(),
            request_id: "boundary/missing-payload".to_string(),
            support_id: None,
            action: sim_memory::ShortpathAction::JumpToTerminal,
            artifact_id: Some("artifact/logits/missing-payload".to_string()),
            target_layer_start: Some(8),
            target_layer_end: Some(8),
            confidence_milli: 980,
            verify_required: false,
            proof_checksum: 0x7777,
            reason: "test missing durable payload".to_string(),
            created_at_us: 11,
            version: 1,
        };
        let mut durable_store = LingquMemoryDurableStore::new();
        durable_store
            .persist_execution_artifact_manifest(vec![artifact])
            .expect("persist execution artifact manifest");
        durable_store
            .persist_shortpath_decision_manifest(vec![decision])
            .expect("persist shortpath decision manifest");
        save_lingqu_memory_durable_store(&store, &durable_store).expect("save durable store");

        let err = load_w5_memory_decisions_from_store(&W5MemoryDecisionConfig {
            store_path: store,
            boundary_request_path: None,
            boundary_observation_id: None,
            shortpath_decision_id: Some("shortpath-decision/missing-payload".to_string()),
            shortpath_execute: false,
            prefetch_plan_id: None,
            prefix_cache_reuse_plan_id: None,
        })
        .expect_err("missing durable payload must fail W5 decision load");
        let err_text = format!("{err:#}");
        assert!(err_text.contains("shortpath decision execution artifact"));
        assert!(err_text.contains("durable payload unavailable"));
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn lingqu_memory_prefix_cache_cli_registers_and_looks_up_artifact() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_prefix_cache_cli_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let store = root.join("store.json");
        let artifact_path = root.join("prefix_cache_artifact.json");
        let request_path = root.join("prefix_cache_lookup_request.json");
        let response_path = root.join("prefix_cache_lookup_response.json");
        let mut seed_store = LingquMemoryDurableStore::new();
        let prefix_payload_ref = seed_store
            .write_block_payload("block/prefix/test/8", vec![0x18; 64])
            .expect("write prefix cache payload");
        save_lingqu_memory_durable_store(&store, &seed_store)
            .expect("save seeded prefix cache payload");
        let key = sim_memory::PrefixCacheKey {
            model: sim_memory::InferenceModelBinding {
                model_id: "qwen3-test".to_string(),
                model_key: "qwen3-test-key".to_string(),
                tokenizer_hash: 0x1001,
                profile_hash: 0x2002,
            },
            namespace: "tenant/project/session".to_string(),
            chat_template_hash: 0x3003,
            prefix_token_hash: 0x4004,
            prefix_token_count: 8,
            rope_config_hash: 0x5005,
            kv_layout_hash: 0x6006,
            layer_start: 0,
            layer_end: 28,
            position_start: 0,
            position_end: 8,
            security_label: sim_memory::MemorySecurityLabel::Internal,
        };
        let artifact = sim_memory::PrefixCacheArtifact {
            artifact_id: "prefix-cache/test/8".to_string(),
            key: key.clone(),
            kv_artifact_ids: Vec::new(),
            durable_payload_refs: vec![prefix_payload_ref],
            hot_object_refs: Vec::new(),
            dtype: sim_core::TensorDType::F32,
            shape: vec![8, 4],
            confidence_milli: 950,
            state: sim_memory::ExecutionArtifactState::Verified,
            checksum: 0x2222_2222,
            version: 1,
            created_at_us: 10,
            expires_at_us: Some(100),
            last_used_at_us: 10,
            use_count: 1,
        };
        let request = sim_memory::PrefixCacheLookupRequest {
            request_id: "prefix-lookup/test/0".to_string(),
            candidate_keys: vec![key],
            min_confidence_milli: 900,
            allow_verify: false,
            created_at_us: 12,
        };
        fs::write(
            &artifact_path,
            serde_json::to_vec_pretty(&artifact).expect("encode artifact"),
        )
        .expect("write artifact");
        fs::write(
            &request_path,
            serde_json::to_vec_pretty(&request).expect("encode request"),
        )
        .expect("write request");

        run_lingqu_memory_register_prefix_cache_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--artifact".to_string(),
            artifact_path.to_string_lossy().into_owned(),
        ])
        .expect("register prefix cache artifact");
        run_lingqu_memory_lookup_prefix_cache_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--request".to_string(),
            request_path.to_string_lossy().into_owned(),
            "--response".to_string(),
            response_path.to_string_lossy().into_owned(),
            "--now-us".to_string(),
            "20".to_string(),
        ])
        .expect("lookup prefix cache artifact");
        run_lingqu_memory_list_prefix_cache_reuse_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--plan-id".to_string(),
            "prefix-cache-reuse/prefix-lookup/test/0".to_string(),
        ])
        .expect("list prefix cache reuse audit");

        let store_bytes = fs::read(&store).expect("read durable store");
        let durable_snapshot =
            LingquDurableSimSnapshot::from_json_bytes(&store_bytes).expect("decode durable store");
        let mut durable_store =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_snapshot)
                .expect("import durable store");
        let registry_artifacts = durable_store
            .load_prefix_cache_manifest()
            .expect("load prefix cache manifest after restart");
        assert_eq!(registry_artifacts.len(), 1);
        assert_eq!(registry_artifacts[0].artifact_id, "prefix-cache/test/8");
        let reuse_plans = durable_store
            .load_prefix_cache_reuse_plan_manifest()
            .expect("load prefix cache reuse audit after restart");
        assert_eq!(reuse_plans.len(), 1);
        assert_eq!(
            reuse_plans[0].plan_id,
            "prefix-cache-reuse/prefix-lookup/test/0"
        );
        let response_bytes = fs::read(&response_path).expect("read response");
        let response =
            serde_json::from_slice::<sim_memory::PrefixCacheLookupResponse>(&response_bytes)
                .expect("decode response");
        assert_eq!(
            response.reuse_plan.action,
            sim_memory::PrefixCacheReuseAction::Reuse
        );
        assert_eq!(
            response.reuse_plan.artifact_id.as_deref(),
            Some("prefix-cache/test/8")
        );
        assert_eq!(response.reuse_plan.matched_prefix_token_count, 8);
        assert!(!response.reuse_plan.verify_required);
        assert!(response.reuse_plan.proof_checksum != 0);
        let decision_config = W5MemoryDecisionConfig {
            store_path: store.clone(),
            boundary_request_path: None,
            boundary_observation_id: None,
            shortpath_decision_id: None,
            shortpath_execute: false,
            prefetch_plan_id: None,
            prefix_cache_reuse_plan_id: Some("prefix-cache-reuse/prefix-lookup/test/0".to_string()),
        };
        let bundle = load_w5_memory_decisions_from_store(&decision_config)
            .expect("load w5 prefix-cache decision bundle");
        assert_eq!(
            bundle
                .prefix_cache
                .as_ref()
                .expect("prefix-cache plan")
                .artifact_id
                .as_deref(),
            Some("prefix-cache/test/8")
        );
        assert_eq!(
            bundle
                .prefix_cache_artifact
                .as_ref()
                .expect("prefix-cache artifact")
                .artifact_id,
            "prefix-cache/test/8"
        );
        let publication = publish_w5_memory_decision_artifact_refs(
            &W5MemoryBootstrapConfig {
                store_path: store.clone(),
                object_store_path: root.join("unused-object-store.json"),
                engram_state_path: root.join("unused-engram-state.json"),
                registry_dir: root.join("qwen3-prefix-registry"),
                owner_entity: 1,
                producer_entity: 2,
            },
            &bundle,
        )
        .expect("publish prefix-cache artifact ref");
        assert_eq!(
            publication
                .prefix_cache_ref
                .as_ref()
                .expect("prefix cache ref")
                .ref_hex
                .len(),
            128
        );
        let env_vars = w5_memory_decision_env_vars(&decision_config, &bundle, Some(&publication));
        assert!(env_vars.iter().any(|(key, value)| {
            key == "SIM_W5_MEMORY_PREFIX_CACHE_ARTIFACT_REF" && value.len() == 128
        }));
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn lingqu_memory_prefix_lookup_cli_requires_prefix_manifest() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_prefix_missing_manifest_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let store = root.join("store.json");
        let request_path = root.join("prefix_cache_lookup_request.json");
        let response_path = root.join("prefix_cache_lookup_response.json");
        let request = sim_memory::PrefixCacheLookupRequest {
            request_id: "prefix/missing-manifest".to_string(),
            candidate_keys: vec![sim_memory::PrefixCacheKey {
                model: sim_memory::InferenceModelBinding {
                    model_id: "qwen3-test".to_string(),
                    model_key: "qwen3-test-key".to_string(),
                    tokenizer_hash: 0x1001,
                    profile_hash: 0x2002,
                },
                namespace: "tenant/project/session".to_string(),
                chat_template_hash: 0x3003,
                prefix_token_hash: 0x4004,
                prefix_token_count: 8,
                rope_config_hash: 0x5005,
                kv_layout_hash: 0x6006,
                layer_start: 0,
                layer_end: 28,
                position_start: 0,
                position_end: 8,
                security_label: sim_memory::MemorySecurityLabel::Internal,
            }],
            min_confidence_milli: 900,
            allow_verify: false,
            created_at_us: 12,
        };
        fs::write(
            &request_path,
            serde_json::to_vec_pretty(&request).expect("encode prefix request"),
        )
        .expect("write prefix request");

        let err = run_lingqu_memory_lookup_prefix_cache_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--request".to_string(),
            request_path.to_string_lossy().into_owned(),
            "--response".to_string(),
            response_path.to_string_lossy().into_owned(),
            "--now-us".to_string(),
            "20".to_string(),
        ])
        .expect_err("missing prefix manifest must fail prefix lookup");

        let err_text = format!("{err:#}");
        assert!(err_text.contains("load prefix cache manifest"));
        assert!(err_text.contains(sim_memory::LINGQU_PREFIX_CACHE_MANIFEST_PATH));
        assert!(!response_path.exists());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn lingqu_memory_ingest_cli_persists_catalog_and_store() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_ingest_cli_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let source = root.join("note.md");
        let catalog = root.join("catalog.json");
        let store = root.join("store.json");
        fs::write(&source, b"# Note\nreal memory source\n").expect("write source");

        run_lingqu_memory_ingest_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--source".to_string(),
            source.to_string_lossy().into_owned(),
            "--catalog-id".to_string(),
            "corpus/test".to_string(),
            "--namespace".to_string(),
            "project/test".to_string(),
            "--record-id".to_string(),
            "record/test/0".to_string(),
            "--chunk-id".to_string(),
            "chunk/test/0".to_string(),
            "--token-count".to_string(),
            "4".to_string(),
            "--embedding-model-version".to_string(),
            "embed/test/v1".to_string(),
        ])
        .expect("ingest");

        let catalog_bytes = fs::read(&catalog).expect("read catalog");
        let snapshot =
            MemoryCatalogSnapshot::from_json_bytes(&catalog_bytes).expect("decode catalog");
        assert_eq!(snapshot.catalog.catalog_id, "corpus/test");
        assert_eq!(snapshot.records.len(), 1);
        assert_eq!(snapshot.chunks.len(), 1);
        assert_eq!(snapshot.records[0].record_id, "record/test/0");
        assert_eq!(snapshot.chunks[0].chunk_id, "chunk/test/0");

        let store_bytes = fs::read(&store).expect("read store");
        let durable_snapshot =
            LingquDurableSimSnapshot::from_json_bytes(&store_bytes).expect("decode durable store");
        let mut durable_store =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_snapshot)
                .expect("import durable store");
        let restored_catalog = durable_store
            .load_catalog_snapshot(&sim_memory::LingquDfsPath::new(
                "/lingqu/memory/corpus/corpus_test/catalog.json",
            ))
            .expect("load durable catalog snapshot");
        assert_eq!(restored_catalog.catalog.catalog_id, "corpus/test");
        let store_snapshot = durable_store.export_snapshot().expect("export legacy view");
        assert_eq!(store_snapshot.block_payloads.len(), 1);
        assert_eq!(
            store_snapshot.block_payloads[0].bytes,
            b"# Note\nreal memory source\n"
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn lingqu_memory_build_index_cli_persists_flat_index() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_build_index_cli_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let source = root.join("note.md");
        let catalog = root.join("catalog.json");
        let store = root.join("store.json");
        let embeddings = root.join("embeddings.json");
        fs::write(&source, b"# Note\nreal memory source\n").expect("write source");
        fs::write(
            &embeddings,
            serde_json::json!({
                "model_version": "embed/test/v1",
                "dims": 2,
                "vectors": [
                    {"chunk_id": "chunk/test/0", "values": [0.25, 0.75]}
                ]
            })
            .to_string(),
        )
        .expect("write embeddings");

        run_lingqu_memory_ingest_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--source".to_string(),
            source.to_string_lossy().into_owned(),
            "--catalog-id".to_string(),
            "corpus/test".to_string(),
            "--namespace".to_string(),
            "project/test".to_string(),
            "--record-id".to_string(),
            "record/test/0".to_string(),
            "--chunk-id".to_string(),
            "chunk/test/0".to_string(),
            "--token-count".to_string(),
            "4".to_string(),
            "--embedding-model-version".to_string(),
            "embed/test/v1".to_string(),
        ])
        .expect("ingest");
        run_lingqu_memory_build_index_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--embedding-json".to_string(),
            embeddings.to_string_lossy().into_owned(),
            "--index-id".to_string(),
            "index/test/flat".to_string(),
            "--segment-id".to_string(),
            "segment/test/0".to_string(),
        ])
        .expect("build index");

        let catalog_bytes = fs::read(&catalog).expect("read catalog");
        let snapshot =
            MemoryCatalogSnapshot::from_json_bytes(&catalog_bytes).expect("decode catalog");
        assert_eq!(snapshot.vector_indexes.len(), 1);
        assert_eq!(snapshot.embedding_segments.len(), 1);
        assert_eq!(snapshot.vector_indexes[0].index_id, "index/test/flat");
        assert_eq!(snapshot.embedding_segments[0].segment_id, "segment/test/0");
        assert_eq!(snapshot.embedding_segments[0].row_count, 1);
        assert_eq!(snapshot.embedding_segments[0].dims, 2);

        let store_bytes = fs::read(&store).expect("read store");
        let durable_snapshot =
            LingquDurableSimSnapshot::from_json_bytes(&store_bytes).expect("decode durable store");
        let store_snapshot =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_snapshot)
                .expect("import durable store")
                .export_snapshot()
                .expect("export legacy view");
        assert_eq!(store_snapshot.block_payloads.len(), 2);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn lingqu_memory_query_cli_persists_query_result() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_query_cli_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let source = root.join("note.md");
        let catalog = root.join("catalog.json");
        let store = root.join("store.json");
        let embeddings = root.join("embeddings.json");
        let query_embedding = root.join("query_embedding.json");
        fs::write(&source, b"# Note\nreal memory source\n").expect("write source");
        fs::write(
            &embeddings,
            serde_json::json!({
                "model_version": "embed/test/v1",
                "dims": 2,
                "vectors": [
                    {"chunk_id": "chunk/test/0", "values": [0.25, 0.75]}
                ]
            })
            .to_string(),
        )
        .expect("write embeddings");
        fs::write(
            &query_embedding,
            serde_json::json!({
                "model_version": "embed/test/v1",
                "values": [0.25, 0.75]
            })
            .to_string(),
        )
        .expect("write query embedding");

        run_lingqu_memory_ingest_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--source".to_string(),
            source.to_string_lossy().into_owned(),
            "--catalog-id".to_string(),
            "corpus/test".to_string(),
            "--namespace".to_string(),
            "project/test".to_string(),
            "--record-id".to_string(),
            "record/test/0".to_string(),
            "--chunk-id".to_string(),
            "chunk/test/0".to_string(),
            "--token-count".to_string(),
            "4".to_string(),
            "--embedding-model-version".to_string(),
            "embed/test/v1".to_string(),
        ])
        .expect("ingest");
        run_lingqu_memory_build_index_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--embedding-json".to_string(),
            embeddings.to_string_lossy().into_owned(),
            "--index-id".to_string(),
            "index/test/flat".to_string(),
            "--segment-id".to_string(),
            "segment/test/0".to_string(),
        ])
        .expect("build index");
        fs::remove_file(&catalog).expect("remove catalog file before durable restart query");
        run_lingqu_memory_query_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--catalog-id".to_string(),
            "corpus/test".to_string(),
            "--query-embedding-json".to_string(),
            query_embedding.to_string_lossy().into_owned(),
            "--query-id".to_string(),
            "query/test/0".to_string(),
            "--top-k".to_string(),
            "1".to_string(),
        ])
        .expect("query");

        let store_bytes = fs::read(&store).expect("read store");
        let durable_snapshot =
            LingquDurableSimSnapshot::from_json_bytes(&store_bytes).expect("decode durable store");
        let query_audit_records = durable_snapshot
            .dfs
            .append_logs
            .iter()
            .filter(|record| record.path == sim_memory::LINGQU_QUERY_RESULT_AUDIT_LOG_PATH)
            .collect::<Vec<_>>();
        assert_eq!(query_audit_records.len(), 1);
        assert_eq!(query_audit_records[0].seq, 1);
        let store_snapshot =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_snapshot)
                .expect("import durable store")
                .export_snapshot()
                .expect("export legacy view");
        assert_eq!(store_snapshot.block_payloads.len(), 3);
        let query_result_payload = store_snapshot
            .dfs_payloads
            .iter()
            .find(|payload| payload.path.contains("query-result_query_test_0"))
            .expect("query result dfs payload");
        let query_result =
            QueryResult::from_json_bytes(&query_result_payload.bytes).expect("query result");
        assert_eq!(query_result.result_id, "query-result/query/test/0");
        assert_eq!(query_result.selected_record_ids, ["record/test/0"]);
        assert_eq!(query_result.selected_chunk_ids, ["chunk/test/0"]);
        assert_eq!(query_result.matches.len(), 1);
        assert_eq!(query_result.matches[0].score, 0.625);
        run_lingqu_memory_list_query_results_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
        ])
        .expect("list query result audit");
        run_lingqu_memory_list_query_results_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--result-id".to_string(),
            "query-result/query/test/0".to_string(),
        ])
        .expect("filter query result audit");
        let missing_result_err = run_lingqu_memory_list_query_results_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--result-id".to_string(),
            "query-result/missing".to_string(),
        ])
        .expect_err("missing query result filter must fail");
        assert!(format!("{missing_result_err:#}").contains("not found in durable audit log"));

        run_lingqu_memory_update_record_state_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--catalog-id".to_string(),
            "corpus/test".to_string(),
            "--record-id".to_string(),
            "record/test/0".to_string(),
            "--state".to_string(),
            "tombstoned".to_string(),
            "--actor".to_string(),
            "unit-test".to_string(),
            "--reason".to_string(),
            "verify tombstone filtering".to_string(),
            "--now-us".to_string(),
            "2".to_string(),
        ])
        .expect("tombstone record");
        run_lingqu_memory_list_record_lifecycle_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--record-id".to_string(),
            "record/test/0".to_string(),
        ])
        .expect("list record lifecycle audit");
        fs::remove_file(&catalog).expect("remove catalog file before tombstone query");
        run_lingqu_memory_query_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--catalog-id".to_string(),
            "corpus/test".to_string(),
            "--query-embedding-json".to_string(),
            query_embedding.to_string_lossy().into_owned(),
            "--query-id".to_string(),
            "query/test/tombstoned".to_string(),
            "--top-k".to_string(),
            "1".to_string(),
            "--now-us".to_string(),
            "3".to_string(),
        ])
        .expect("query after tombstone");
        let store_bytes = fs::read(&store).expect("read store after tombstone query");
        let durable_snapshot =
            LingquDurableSimSnapshot::from_json_bytes(&store_bytes).expect("decode durable store");
        let query_audit_records = durable_snapshot
            .dfs
            .append_logs
            .iter()
            .filter(|record| record.path == sim_memory::LINGQU_QUERY_RESULT_AUDIT_LOG_PATH)
            .collect::<Vec<_>>();
        assert_eq!(query_audit_records.len(), 2);
        assert_eq!(query_audit_records[0].seq, 1);
        assert_eq!(query_audit_records[1].seq, 2);
        let lifecycle_audit_records = durable_snapshot
            .dfs
            .append_logs
            .iter()
            .filter(|record| record.path == sim_memory::LINGQU_RECORD_LIFECYCLE_AUDIT_LOG_PATH)
            .collect::<Vec<_>>();
        assert_eq!(lifecycle_audit_records.len(), 1);
        assert_eq!(lifecycle_audit_records[0].seq, 1);
        let mut lifecycle_store =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_snapshot.clone())
                .expect("import lifecycle durable store");
        let lifecycle_events = lifecycle_store
            .load_record_lifecycle_event_manifest()
            .expect("load lifecycle audit after restart");
        assert_eq!(lifecycle_events.len(), 1);
        assert_eq!(lifecycle_events[0].record_id, "record/test/0");
        assert_eq!(
            lifecycle_events[0].previous_state,
            sim_memory::MemoryRecordState::Committed
        );
        assert_eq!(
            lifecycle_events[0].new_state,
            sim_memory::MemoryRecordState::Tombstoned
        );
        let store_snapshot =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_snapshot)
                .expect("import durable store")
                .export_snapshot()
                .expect("export legacy view");
        let tombstoned_query_payload = store_snapshot
            .dfs_payloads
            .iter()
            .find(|payload| payload.path.contains("query-result_query_test_tombstoned"))
            .expect("tombstoned query result dfs payload");
        let tombstoned_query_result = QueryResult::from_json_bytes(&tombstoned_query_payload.bytes)
            .expect("tombstoned query result");
        assert!(tombstoned_query_result.matches.is_empty());
        assert!(tombstoned_query_result.selected_record_ids.is_empty());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn lingqu_memory_materialize_hot_state_cli_uses_query_result_manifest() {
        let root = std::env::temp_dir().join(format!(
            "ub_sim_lingqu_memory_materialize_cli_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).expect("create temp dir");
        let source = root.join("note.md");
        let catalog = root.join("catalog.json");
        let store = root.join("store.json");
        let object_store = root.join("object_store.json");
        let embeddings = root.join("embeddings.json");
        let query_embedding = root.join("query_embedding.json");
        let hot_state = root.join("hot_state.json");
        let gate_weight = root.join("gate_weight.json");
        let engram_state = root.join("engram_state.json");
        let registry_dir = root.join("qwen3_registry");
        fs::write(&source, b"# Note\nreal memory source\n").expect("write source");
        fs::write(
            &embeddings,
            serde_json::json!({
                "model_version": "embed/test/v1",
                "dims": 2,
                "vectors": [
                    {"chunk_id": "chunk/test/0", "values": [0.25, 0.75]}
                ]
            })
            .to_string(),
        )
        .expect("write embeddings");
        fs::write(
            &query_embedding,
            serde_json::json!({
                "model_version": "embed/test/v1",
                "values": [0.25, 0.75]
            })
            .to_string(),
        )
        .expect("write query embedding");
        fs::write(
            &gate_weight,
            serde_json::json!({
                "values": [0.5, 0.75]
            })
            .to_string(),
        )
        .expect("write gate weight");

        run_lingqu_memory_ingest_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--source".to_string(),
            source.to_string_lossy().into_owned(),
            "--catalog-id".to_string(),
            "corpus/test".to_string(),
            "--namespace".to_string(),
            "project/test".to_string(),
            "--record-id".to_string(),
            "record/test/0".to_string(),
            "--chunk-id".to_string(),
            "chunk/test/0".to_string(),
            "--token-count".to_string(),
            "4".to_string(),
            "--embedding-model-version".to_string(),
            "embed/test/v1".to_string(),
        ])
        .expect("ingest");
        run_lingqu_memory_build_index_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--embedding-json".to_string(),
            embeddings.to_string_lossy().into_owned(),
            "--index-id".to_string(),
            "index/test/flat".to_string(),
            "--segment-id".to_string(),
            "segment/test/0".to_string(),
        ])
        .expect("build index");
        fs::remove_file(&catalog).expect("remove catalog file before durable restart query");
        run_lingqu_memory_query_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--catalog-id".to_string(),
            "corpus/test".to_string(),
            "--query-embedding-json".to_string(),
            query_embedding.to_string_lossy().into_owned(),
            "--query-id".to_string(),
            "query/test/0".to_string(),
            "--top-k".to_string(),
            "1".to_string(),
        ])
        .expect("query");
        run_lingqu_memory_materialize_hot_state_cli(&[
            "--catalog".to_string(),
            catalog.to_string_lossy().into_owned(),
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--catalog-id".to_string(),
            "corpus/test".to_string(),
            "--object-store".to_string(),
            object_store.to_string_lossy().into_owned(),
            "--query-result-manifest".to_string(),
            "/lingqu/memory/query-results/query-result_query_test_0.json".to_string(),
            "--state-id".to_string(),
            "hot/test/0".to_string(),
            "--hot-state".to_string(),
            hot_state.to_string_lossy().into_owned(),
        ])
        .expect("materialize hot state");

        let hot_state_json = fs::read(&hot_state).expect("read hot state");
        let hot_state_value: serde_json::Value =
            serde_json::from_slice(&hot_state_json).expect("decode hot state");
        assert_eq!(hot_state_value["state_id"], "hot/test/0");
        assert_eq!(
            hot_state_value["query_result_id"],
            "query-result/query/test/0"
        );
        assert_eq!(hot_state_value["selected_chunk_ids"][0], "chunk/test/0");
        assert_eq!(hot_state_value["table"]["shape"][0], 1);
        assert_eq!(hot_state_value["table"]["shape"][1], 2);
        assert_eq!(hot_state_value["indices"]["shape"][0], 1);
        assert_eq!(hot_state_value["scores"]["shape"][0], 1);
        assert!(
            !object_store.exists(),
            "object store should be checkpointed into durable DFS"
        );
        let durable_snapshot = LingquDurableSimSnapshot::from_json_bytes(
            &fs::read(&store).expect("read durable store"),
        )
        .expect("decode durable store");
        let mut durable_store =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_snapshot)
                .expect("import durable store");
        let object_snapshot = durable_store
            .load_object_service_checkpoint()
            .expect("load object checkpoint from durable store");
        let object_service =
            LingquObjectServiceStub::import_snapshot(object_snapshot).expect("import object store");
        let table_key = hot_state_value["table"]["object_key"]
            .as_str()
            .expect("table object key");
        let table_payload = object_service
            .get_copy(table_key, LingquObjectVersionSelector::LatestCommitted)
            .expect("hot table payload after object-store reload");
        assert_eq!(table_payload, cli_f32_vec_to_le_bytes(&[0.25, 0.75]));

        run_lingqu_memory_materialize_engram_state_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--object-store".to_string(),
            object_store.to_string_lossy().into_owned(),
            "--hot-state".to_string(),
            hot_state.to_string_lossy().into_owned(),
            "--gate-weight-json".to_string(),
            gate_weight.to_string_lossy().into_owned(),
            "--state-id".to_string(),
            "engram/test/0".to_string(),
            "--engram-state".to_string(),
            engram_state.to_string_lossy().into_owned(),
        ])
        .expect("materialize engram state");
        let engram_state_json = fs::read(&engram_state).expect("read engram state");
        let engram_state_value: serde_json::Value =
            serde_json::from_slice(&engram_state_json).expect("decode engram state");
        assert_eq!(engram_state_value["state_id"], "engram/test/0");
        assert_eq!(engram_state_value["hot_memory_state_id"], "hot/test/0");
        assert_eq!(
            engram_state_value["query_result_id"],
            "query-result/query/test/0"
        );
        assert_eq!(engram_state_value["operator_kind"], "ContextGate");
        assert_eq!(engram_state_value["dtype"], "F32");
        assert_eq!(engram_state_value["hidden_size"], 2);
        assert_eq!(engram_state_value["table_rows"], 1);
        assert_eq!(engram_state_value["version"], 1);
        assert!(
            engram_state_value["operator_config_hash"]
                .as_u64()
                .expect("operator config hash")
                > 0
        );
        assert!(
            engram_state_value["checksum"]
                .as_u64()
                .expect("engram state checksum")
                > 0
        );
        assert_eq!(engram_state_value["gate"]["shape"][0], 2);
        assert!(
            !object_store.exists(),
            "object store should remain a legacy input path only"
        );
        let durable_snapshot = LingquDurableSimSnapshot::from_json_bytes(
            &fs::read(&store).expect("read updated durable store"),
        )
        .expect("decode updated durable store");
        let mut durable_store =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_snapshot)
                .expect("import updated durable store");
        let object_snapshot = durable_store
            .load_object_service_checkpoint()
            .expect("load updated object checkpoint from durable store");
        let object_service = LingquObjectServiceStub::import_snapshot(object_snapshot)
            .expect("import updated object store");
        let gate_key = engram_state_value["gate"]["object_key"]
            .as_str()
            .expect("gate object key");
        let gate_payload = object_service
            .get_copy(gate_key, LingquObjectVersionSelector::LatestCommitted)
            .expect("gate payload after object-store reload");
        assert_eq!(gate_payload, cli_f32_vec_to_le_bytes(&[0.5, 0.75]));

        run_lingqu_memory_publish_w5_engram_state_ref_cli(&[
            "--store".to_string(),
            store.to_string_lossy().into_owned(),
            "--object-store".to_string(),
            object_store.to_string_lossy().into_owned(),
            "--engram-state".to_string(),
            engram_state.to_string_lossy().into_owned(),
            "--registry-dir".to_string(),
            registry_dir.to_string_lossy().into_owned(),
        ])
        .expect("publish w5 engram state ref");
        let registry_entries = fs::read_dir(&registry_dir)
            .expect("read registry dir")
            .map(|entry| {
                entry
                    .expect("registry entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect::<Vec<_>>();
        for kind in ["kind0015", "kind0016", "kind0017", "kind0018"] {
            assert!(
                registry_entries.iter().any(|entry| entry.contains(kind)),
                "missing registry entry kind {kind}: {registry_entries:?}"
            );
        }
        let bootstrap_registry_dir = root.join("qwen3_registry_bootstrap");
        let publication = publish_w5_engram_state_ref_from_memory(&W5MemoryBootstrapConfig {
            store_path: store.clone(),
            object_store_path: object_store.clone(),
            engram_state_path: engram_state.clone(),
            registry_dir: bootstrap_registry_dir.clone(),
            owner_entity: 0,
            producer_entity: 0,
        })
        .expect("publish w5 engram state ref from memory bootstrap");
        assert_eq!(publication.engram_state_id, "engram/test/0");
        assert!(!publication.state_ref_hex.is_empty());
        assert_eq!(publication.table_bytes, 8);
        assert_eq!(publication.indices_bytes, 4);
        assert_eq!(publication.gate_bytes, 8);
        let bootstrap_registry_entries = fs::read_dir(&bootstrap_registry_dir)
            .expect("read bootstrap registry dir")
            .map(|entry| {
                entry
                    .expect("bootstrap registry entry")
                    .file_name()
                    .to_string_lossy()
                    .into_owned()
            })
            .collect::<Vec<_>>();
        for kind in ["kind0015", "kind0016", "kind0017", "kind0018"] {
            assert!(
                bootstrap_registry_entries
                    .iter()
                    .any(|entry| entry.contains(kind)),
                "missing bootstrap registry entry kind {kind}: {bootstrap_registry_entries:?}"
            );
        }
        let _ = fs::remove_dir_all(&root);
    }
}

fn run_qwen3_decode_loop_cli(args: &Qwen3DecodeLoopCliArgs) -> anyhow::Result<()> {
    configure_simpler_dispatch_logging();
    let scenario_path = &args.scenario_path;
    let config = ScenarioConfig::from_yaml_file(scenario_path).with_context(|| {
        format!(
            "failed to load scenario config from {}",
            scenario_path.display()
        )
    })?;
    let topology = SimTopology::from_config(&config).context("failed to build topology")?;
    prepare_qwen3_decode_loop_environment(args)?;
    std::env::set_var("SIM_QWEN3_DECODE_PROGRESS", "1");
    eprintln!(
        "qwen3-decode-loop: scenario={} steps={} prompt_bytes={} matmul_batch={}",
        scenario_path.display(),
        args.step_count,
        args.prompt.as_deref().map(str::len).unwrap_or(0),
        args.matmul_batch
            .map(|value| value.to_string())
            .unwrap_or_else(|| "default".to_string())
    );
    let report = if let Some(prompt) = args.prompt.as_deref() {
        qwen3_dense_reference_decode_loop_report_with_prompt(&topology, args.step_count, prompt)
    } else {
        qwen3_dense_reference_decode_loop_report(&topology, args.step_count)
    }
    .map_err(anyhow::Error::msg)
    .context("failed to run Qwen3 decode loop")?;
    println!("qwen3_dense_reference_decode_loop");
    println!("  scenario: {}", scenario_path.display());
    println!("  steps: {}", report.steps.len());
    println!(
        "  final_guest_input_checksum: {:#x}",
        report.final_guest_input_checksum
    );
    println!("  decode_chain: {:#x}", report.decode_chain_checksum);
    println!(
        "  generated_text_lossy: {}",
        report.generated_text_lossy.escape_debug()
    );
    println!(
        "  generated_bytes: len={} checksum={:#x}",
        report.generated_byte_len, report.generated_byte_checksum
    );
    match qwen3_decode_report_verbosity() {
        Qwen3DecodeReportVerbosity::Summary => {}
        Qwen3DecodeReportVerbosity::Steps => {
            for step in &report.steps {
                println!(
                    "  step={} runtime_prefill={} input_tokens={} sampled_tokens={} text_bytes={} contract_ready={} blockers={} synthetic_stages={} full_forward_math={} full_vocab_logits={} object_ready={} object_publish={} object_resolve={} object_append={} kv_resolve={} kv_append={} obmm_pool={} obmm_queue={} weight_payload_bytes={} weight_payload_slices={} weight_payload_complete={} weight_reconstructed_tensors={} weight_reconstructed_checksum={:#x} weight_payload_checksum={:#x} global_weight_objects={} global_weight_payload_bytes={} global_weight_tensors={} global_weight_checksum={:#x} object_checksum={:#x} input_checksum={:#x} next_input_checksum={:#x}",
                    step.step_index,
                    step.runtime_prefill_executed,
                    step.text_output.guest_input.prompt_token_count,
                    step.sampled_token_count,
                    step.text_output.byte_len,
                    step.real_inference_contract.ready,
                    step.real_inference_contract.blocker_count,
                    step.real_inference_contract.synthetic_stage_count,
                    step.real_inference_contract.full_forward_math,
                    step.real_inference_contract.full_vocab_logits,
                    step.object_service.ready,
                    step.object_service.publish_count,
                    step.object_service.resolve_count,
                    step.object_service.append_count,
                    step.object_service.kv_index_resolve_count,
                    step.object_service.kv_index_append_count,
                    step.object_service.obmm_pool_enabled,
                    step.object_service.obmm_pool_queue_submit_count,
                    step.object_service.weight_payload_bytes,
                    step.object_service.weight_payload_slice_count,
                    step.object_service.weight_payload_complete,
                    step.object_service.weight_reconstructed_tensor_count,
                    step.object_service.weight_reconstructed_tensor_checksum,
                    step.object_service.weight_payload_checksum,
                    step.object_service.global_weight_object_count,
                    step.object_service.global_weight_payload_bytes,
                    step.object_service.global_weight_tensor_count,
                    step.object_service.global_weight_payload_checksum,
                    step.object_service.object_checksum,
                    step.guest_input_checksum,
                    step.next_guest_input_checksum
                );
            }
        }
        Qwen3DecodeReportVerbosity::Verbose => {
            print_qwen3_decode_verbose_steps(&report.steps);
        }
    }
    Ok(())
}

fn run_qwen3_guest_decode_loop_cli(args: &Qwen3GuestDecodeLoopCliArgs) -> anyhow::Result<()> {
    let script_path = if args.script_path.is_absolute() {
        args.script_path.clone()
    } else {
        env::current_dir()
            .context("failed to read current directory")?
            .join(&args.script_path)
    };
    if !script_path.exists() {
        anyhow::bail!(
            "qwen3 guest decode script not found: {}",
            script_path.display()
        );
    }
    let runtime = qwen3_guest_dense_runtime(args)?;
    let mut effective_engram = args.engram.clone();
    let memory_publication = if let Some(memory_bootstrap) = &args.memory_bootstrap {
        let publication = publish_w5_engram_state_ref_from_memory_objects(memory_bootstrap)
            .context("publish Memory Service EngramStateObjectRef for W5")?;
        effective_engram.enabled = true;
        effective_engram.pool = Qwen3EngramPool::Obmm;
        if effective_engram.context_op == Qwen3EngramContextOp::Disabled {
            effective_engram.context_op = Qwen3EngramContextOp::CpuReference;
        }
        effective_engram.state_ref = Some(publication.state_ref_hex.clone());
        effective_engram.object_service_snapshot_path =
            publication.object_service_snapshot_path.clone();
        Some(publication)
    } else {
        None
    };
    let memory_decisions = if let Some(memory_decision_config) = &args.memory_decisions {
        Some(
            load_w5_memory_decisions_from_store(memory_decision_config)
                .context("load W5 execution decisions and Memory Service plans")?,
        )
    } else {
        None
    };
    let memory_decision_publication = if let Some(decisions) = &memory_decisions {
        if w5_memory_decisions_reference_artifacts(decisions) {
            let memory_bootstrap = args.memory_bootstrap.as_ref().ok_or_else(|| {
                anyhow::anyhow!(
                    "W5 artifact-backed decisions and Memory Service plans require --memory-store, --memory-object-store, --memory-engram-state, and --memory-registry-dir so artifacts can be published as object refs"
                )
            })?;
            Some(
                publish_w5_memory_decision_artifact_refs(memory_bootstrap, decisions)
                    .context("publish W5 execution artifact object refs")?,
            )
        } else {
            None
        }
    } else {
        None
    };
    if let Some(publication) = &memory_decision_publication {
        if let Some(snapshot_path) = &publication.object_service_snapshot_path {
            effective_engram.object_service_snapshot_path = Some(snapshot_path.clone());
        }
    }
    let engram_simt = qwen3_prepare_engram_simt_mode(&effective_engram)?;
    let engram_registry_validation =
        qwen3_validate_guest_engram_state_registry(&effective_engram, &runtime.profile)?;
    let w5_profile = args
        .w5_profile
        .clone()
        .unwrap_or_else(|| qwen3_guest_default_w5_profile(&runtime, &effective_engram));
    println!("qwen3_guest_decode_loop");
    println!("  script: {}", script_path.display());
    println!("  workload: w5 inference cluster");
    println!("  w5_profile: {}", w5_profile);
    println!("  model_id: {}", runtime.profile.model_id);
    println!("  model_key: {}", runtime.model_key);
    println!("  weights_path: {}", runtime.weights_path.display());
    println!(
        "  hidden_range_bytes: {}",
        hidden_range_bytes(&runtime.profile)
    );
    println!(
        "  decode_hidden_bytes: {}",
        decode_hidden_bytes(&runtime.profile)
    );
    println!("  steps: {}", args.step_count);
    if let Some(prompt) = &args.prompt {
        println!("  prompt_bytes: {}", prompt.len());
    }
    if let Some(matmul_batch) = args.matmul_batch {
        prepare_qwen3_matmul_batch_environment(matmul_batch)?;
        println!("  matmul_batch: {}", matmul_batch);
    }
    if let Some(publication) = &memory_publication {
        println!("  memory_service: lingqu_memory_service");
        println!("  memory_fixture_backed: false");
        println!("  memory_engram_state: {}", publication.engram_state_id);
        println!(
            "  memory_object_service_snapshot: {}",
            effective_engram
                .object_service_snapshot_path
                .as_ref()
                .map(|path| path.display().to_string())
                .unwrap_or_default()
        );
        println!("  memory_engram_state_ref: {}", publication.state_ref_hex);
        println!("  memory_context_table_bytes: {}", publication.table_bytes);
        println!(
            "  memory_context_indices_bytes: {}",
            publication.indices_bytes
        );
        println!("  memory_context_gate_bytes: {}", publication.gate_bytes);
        println!(
            "  memory_context_state_manifest_bytes: {}",
            publication.state_manifest_bytes
        );
    }
    if let Some(decisions) = &memory_decisions {
        println!("  w5_planner: w5_runtime_planner");
        if let Some(decision) = &decisions.shortpath {
            println!(
                "  w5_shortpath_decision: id={} support_id={} action={} artifact_id={} artifact_kind={} artifact_checksum={} proof_checksum={:#x}",
                decision.decision_id,
                decision.support_id.as_deref().unwrap_or(""),
                w5_shortpath_action_name(decision.action),
                decision.artifact_id.as_deref().unwrap_or(""),
                decisions
                    .shortpath_artifact
                    .as_ref()
                    .map(|artifact| w5_execution_artifact_kind_name(artifact.kind))
                    .unwrap_or("none"),
                decisions
                    .shortpath_artifact
                    .as_ref()
                    .map(|artifact| format!("{:#x}", artifact.checksum))
                    .unwrap_or_else(|| "none".to_string()),
                decision.proof_checksum
            );
        }
        if let Some(plan) = &decisions.prefetch {
            println!(
                "  memory_prefetch_plan: id={} scope={} target_step_index={} artifact_count={} checksum={:#x}",
                plan.plan_id,
                w5_prefetch_scope_name(plan.scope),
                plan.target_step_index,
                decisions.prefetch_artifacts.len(),
                plan.checksum
            );
        }
        if let Some(plan) = &decisions.prefix_cache {
            println!(
                "  memory_prefix_cache_reuse_plan: id={} action={} artifact_id={} artifact_checksum={} proof_checksum={:#x}",
                plan.plan_id,
                w5_prefix_cache_reuse_action_name(plan.action),
                plan.artifact_id.as_deref().unwrap_or(""),
                decisions
                    .prefix_cache_artifact
                    .as_ref()
                    .map(|artifact| format!("{:#x}", artifact.checksum))
                    .unwrap_or_else(|| "none".to_string()),
                plan.proof_checksum
            );
        }
    }
    if let Some(publication) = &memory_decision_publication {
        if let Some(snapshot_path) = &publication.object_service_snapshot_path {
            println!(
                "  memory_artifact_object_service_snapshot: {}",
                snapshot_path.display()
            );
        }
        if let Some(published) = &publication.shortpath_ref {
            println!(
                "  memory_shortpath_artifact_ref: id={} payload_bytes={} payload_checksum={:#x}",
                published.artifact_id, published.payload_bytes, published.payload_checksum
            );
        }
        if !publication.prefetch_refs.is_empty() {
            println!(
                "  memory_prefetch_artifact_refs: count={} payload_bytes={} checksums={}",
                publication.prefetch_refs.len(),
                publication
                    .prefetch_refs
                    .iter()
                    .map(|published| published.payload_bytes.to_string())
                    .collect::<Vec<_>>()
                    .join(","),
                publication
                    .prefetch_refs
                    .iter()
                    .map(|published| format!("{:#x}", published.payload_checksum))
                    .collect::<Vec<_>>()
                    .join(",")
            );
        }
        if let Some(published) = &publication.prefix_cache_ref {
            println!(
                "  memory_prefix_cache_artifact_ref: id={} payload_bytes={} payload_checksum={:#x}",
                published.artifact_id, published.payload_bytes, published.payload_checksum
            );
        }
    }
    if effective_engram.enabled {
        println!(
            "  engram: enabled=true mode={} pool={} owner_node={} no_repeat_ngram_size={} repetition_penalty_milli={} history_window={} blocked_token_ids={:?} context_op={} report={}",
            qwen3_engram_mode_name(effective_engram.mode),
            qwen3_engram_pool_name(effective_engram.pool),
            effective_engram.owner_node,
            effective_engram.no_repeat_ngram_size,
            effective_engram.repetition_penalty_milli,
            effective_engram.history_window,
            effective_engram.blocked_token_ids,
            qwen3_engram_context_op_name(effective_engram.context_op),
            qwen3_engram_report_name(effective_engram.report)
        );
        if let Some(spec) = &engram_simt {
            println!(
                "  engram_simt: artifact_dir={} symbol={} case={} run_mode={} soc_version={}",
                spec.binary_path
                    .parent()
                    .map(Path::display)
                    .map(|display| display.to_string())
                    .unwrap_or_else(|| "<unknown>".to_string()),
                spec.symbol,
                spec.case_name,
                spec.run_mode,
                spec.soc_version
            );
        }
        if let Some(validation) = &engram_registry_validation {
            println!(
                "  engram_state_registry: hidden_size={} table_rows={} table_bytes={} indices_bytes={} gate_weight_bytes={}",
                validation.hidden_size,
                validation.table_rows,
                validation.table_bytes,
                validation.indices_bytes,
                validation.gate_weight_bytes
            );
        }
    }
    println!("  worker_path: 8-node W5 inference cluster OBMM object-service range forward");
    let trace_file = env::temp_dir().join(format!(
        "qwen3_guest_decode_loop_{}.trace",
        std::process::id()
    ));
    let prompt_token_ids = args
        .prompt_token_ids
        .clone()
        .map(Ok)
        .or_else(|| {
            args.prompt
                .as_deref()
                .map(|prompt| qwen3_guest_prompt_token_ids_env(prompt, &runtime.weights_path))
        })
        .transpose()?
        .unwrap_or_default();
    let prompt_history_tokens = qwen3_parse_token_id_csv(&prompt_token_ids)?;
    let engram_session_id = qwen3_guest_session_id(&prompt_history_tokens);
    let mut command = Command::new(&script_path);
    command
        .env(
            "SIM_UAPI_W4_CHIPBACKEND_PROFILE",
            runtime.chipbackend_profile,
        )
        .env("SIM_UAPI_W5_PROFILE", &w5_profile)
        .env("SIM_QWEN3_DENSE_MODEL_ID", &runtime.profile.model_id)
        .env("SIM_QWEN3_DENSE_MODEL_KEY", &runtime.model_key)
        .env("SIM_QWEN3_DENSE_WEIGHTS_PATH", &runtime.weights_path)
        .env(
            "SIM_QWEN3_DENSE_VOCAB_SIZE",
            runtime.profile.vocab_size.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_HIDDEN_SIZE",
            runtime.profile.hidden_size.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_INTERMEDIATE_SIZE",
            runtime.profile.intermediate_size.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS",
            runtime.profile.num_hidden_layers.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS",
            runtime.profile.num_attention_heads.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS",
            runtime.profile.num_key_value_heads.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_HEAD_DIM",
            runtime.profile.head_dim.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_PREFILL_TOKENS",
            runtime.profile.prefill_tokens.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_DECODE_TOKENS",
            runtime.profile.decode_tokens.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_TP_NODES",
            runtime.profile.tp_nodes.to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES",
            hidden_range_bytes(&runtime.profile).to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES",
            decode_hidden_bytes(&runtime.profile).to_string(),
        )
        .env(
            "SIM_QWEN3_DENSE_KV_STATE_BYTES",
            kv_state_bytes_for_layer_count(&runtime.profile, runtime.profile.num_hidden_layers)
                .to_string(),
        )
        .env("SIM_QWEN3_GUEST_DECODE_STEPS", args.step_count.to_string())
        .env(
            "SIM_QWEN3_GUEST_PROMPT",
            args.prompt.clone().unwrap_or_default(),
        )
        .env("SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS", prompt_token_ids)
        .env("TRACE_FILE", &trace_file)
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit());
    command.env("SIM_QWEN3_DENSE_WEIGHTS_PATH", &runtime.weights_path);
    for (key, value) in qwen3_guest_engram_env_vars(&effective_engram, engram_session_id) {
        command.env(key, value);
    }
    if let (Some(config), Some(decisions)) = (&args.memory_decisions, &memory_decisions) {
        for (key, value) in
            w5_memory_decision_env_vars(config, decisions, memory_decision_publication.as_ref())
        {
            command.env(key, value);
        }
    }
    if let Some(spec) = &engram_simt {
        command
            .env("SIM_ENGRAM_SIMT_SELECTED_SYMBOL", &spec.symbol)
            .env("SIM_ENGRAM_SIMT_SELECTED_CASE", &spec.case_name)
            .env("SIM_ENGRAM_SIMT_BINARY_PATH", &spec.binary_path)
            .env(
                "SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH",
                &spec.kernel_library_path,
            );
    }
    let status = command
        .status()
        .with_context(|| format!("failed to run {}", script_path.display()))?;
    let mut combined = fs::read_to_string(&trace_file).unwrap_or_default();
    if let Some(log_dir) = qwen3_guest_log_dir_from_script_output(&combined, &script_path) {
        combined.push_str(&qwen3_guest_read_log_dir(&log_dir)?);
    }
    let runtime_forward_count =
        qwen3_guest_log_match_count(&combined, "stage uapi_qwen3_range_runtime_forward ");
    let runtime_publish_count = qwen3_guest_log_match_count(
        &combined,
        "stage qwen3_range_forward_runtime_output_publish ",
    );
    let runtime_input_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_range_forward_runtime_input_loaded ");
    let terminal_token_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_terminal_token_result_publish ");
    let guest_engram_select_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_engram_token_select ");
    let guest_engram_history_wait_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_engram_history_wait ");
    let guest_engram_state_wait_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_engram_state_wait ");
    let guest_engram_state_resolved_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_decode_round_engram_state_resolved ");
    let guest_engram_candidate_publish_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_engram_candidates_publish ");
    let guest_engram_candidate_wait_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_engram_candidates_wait ");
    let guest_engram_selected_wait_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_engram_selected_token_wait ");
    let guest_engram_selected_writeback_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_engram_selected_writeback ");
    let guest_engram_terminal_rewrite_count =
        qwen3_guest_log_match_count(&combined, "stage qwen3_engram_terminal_record_rewrite ");
    let expected_runtime_forward_count = 8 * args.step_count;
    let expected_runtime_input_count = 7 * args.step_count;
    let expected_runtime_publish_count = 8 * args.step_count;
    let expected_terminal_token_count = args.step_count;
    let expected_guest_engram_select_count = if effective_engram.enabled {
        args.step_count
    } else {
        0
    };
    let expected_guest_engram_candidate_publish_count = expected_guest_engram_select_count;
    let expected_guest_engram_candidate_wait_count = expected_guest_engram_select_count;
    let expected_guest_engram_selected_wait_count = expected_guest_engram_select_count;
    let expected_guest_engram_selected_writeback_count = expected_guest_engram_select_count;
    let expected_guest_engram_history_wait_count = if effective_engram.enabled {
        let mut wait_nodes = [false; 9];
        wait_nodes[1] = true;
        wait_nodes[8] = true;
        wait_nodes[effective_engram.owner_node] = true;
        let wait_node_count = wait_nodes.iter().filter(|enabled| **enabled).count();
        wait_node_count * args.step_count.saturating_sub(1)
    } else {
        0
    };
    let expected_guest_engram_state_wait_count = expected_guest_engram_history_wait_count;
    let expected_guest_engram_state_resolved_count = expected_guest_engram_history_wait_count;
    let timing_summary = qwen3_guest_timing_summary(&combined);
    let terminal_tokens = qwen3_guest_terminal_tokens(&combined);
    let guest_engram_selected_tokens = qwen3_guest_engram_selected_tokens(&combined);
    let guest_engram_select_history_lengths = qwen3_guest_engram_select_history_lengths(&combined);
    let guest_engram_history_lengths = qwen3_guest_engram_history_lengths(&combined);
    let guest_engram_candidate_counts = qwen3_guest_engram_candidate_counts(&combined);
    let terminal_text = qwen3_guest_terminal_text_lossy(&terminal_tokens, &runtime.weights_path);
    let pass = combined.contains("eight-node w5 inference cluster validation passed")
        || combined.contains("PASS: eight-node w5 inference cluster")
        || combined.contains("eight-node w4 guest validation passed")
        || combined.contains("PASS: eight-node w4 guest");
    println!(
        "  guest_worker_summary: pass={} steps={} range_forwards={} runtime_inputs={} runtime_outputs={} terminal_tokens={}",
        pass, args.step_count, runtime_forward_count, runtime_input_count, runtime_publish_count, terminal_token_count
    );
    if !terminal_tokens.is_empty() {
        println!("  terminal_tokens: {:?}", terminal_tokens);
        match &terminal_text {
            Some(text) => println!("  generated_text_lossy: {}", text.escape_debug()),
            None => println!("  generated_text_lossy: <tokenizer unavailable>"),
        }
    }
    if !status.success() || !pass {
        anyhow::bail!(
            "qwen3 guest decode worker failed: status={} pass={}",
            status,
            pass
        );
    }
    let engram_report = if effective_engram.enabled {
        let session_id = qwen3_guest_session_id(&prompt_history_tokens);
        if effective_engram.pool == Qwen3EngramPool::Obmm {
            Some(qwen3_guest_engram_report_from_guest_log(
                &effective_engram,
                session_id,
                &prompt_history_tokens,
                &combined,
            )?)
        } else {
            Some(qwen3_guest_engram_report(
                &effective_engram,
                session_id,
                &prompt_history_tokens,
                &combined,
            )?)
        }
    } else {
        None
    };
    let guest_engram_object_transport =
        if effective_engram.enabled && effective_engram.pool == Qwen3EngramPool::Obmm {
            Some(qwen3_guest_engram_object_transport_report(&combined))
        } else {
            None
        };
    if let Some(report) = &engram_report {
        print_qwen3_guest_engram_report(report);
        if let Some(transport) = &guest_engram_object_transport {
            println!(
                "  guest_engram_object_transport: object_puts={} object_waits={} candidate_publishes={} candidate_waits={} decision_publishes={} selected_waits={} selected_writebacks={} history_waits={} state_waits={} state_resolved={} payload_write_bytes={} payload_read_bytes={} queue_submits={} queue_delivers={} checksum={:#x}",
                transport.object_puts,
                transport.object_waits,
                transport.candidate_publishes,
                transport.candidate_waits,
                transport.decision_publishes,
                transport.selected_waits,
                transport.selected_writebacks,
                transport.history_waits,
                transport.state_waits,
                transport.state_resolved,
                transport.payload_write_bytes,
                transport.payload_read_bytes,
                transport.queue_submits,
                transport.queue_delivers,
                transport.checksum
            );
        }
        if report.selected_tokens != terminal_tokens {
            anyhow::bail!(
                "engram policy selected tokens are not wired into guest writeback yet: selected={:?} terminal={:?}",
                report.selected_tokens,
                terminal_tokens
            );
        }
        if !guest_engram_selected_tokens.is_empty() {
            let expected_terminal_rewrites = qwen3_guest_engram_expected_terminal_rewrites(report);
            let expected_history_lengths =
                if guest_engram_select_history_lengths.len() == args.step_count {
                    guest_engram_select_history_lengths
                        .iter()
                        .map(|length| length + 1)
                        .collect::<Vec<_>>()
                } else {
                    (0..args.step_count)
                        .map(|step| prompt_history_tokens.len() as u64 + step as u64 + 1)
                        .collect::<Vec<_>>()
                };
            let blocked_writeback_tokens = terminal_tokens
                .iter()
                .copied()
                .filter(|token| effective_engram.blocked_token_ids.contains(token))
                .collect::<Vec<_>>();
            println!(
                "  guest_engram_writeback: selected_tokens={:?} terminal_tokens={:?} blocked_token_ids={:?} blocked_writeback_tokens={:?} history_lengths={:?} candidate_counts={:?} candidate_publishes={} candidate_waits={} selected_waits={} selected_writebacks={} terminal_rewrites={} select_logs={} history_waits={} state_waits={} state_resolved={} matches_terminal={}",
                guest_engram_selected_tokens,
                terminal_tokens,
                effective_engram.blocked_token_ids,
                blocked_writeback_tokens,
                guest_engram_history_lengths,
                guest_engram_candidate_counts,
                guest_engram_candidate_publish_count,
                guest_engram_candidate_wait_count,
                guest_engram_selected_wait_count,
                guest_engram_selected_writeback_count,
                guest_engram_terminal_rewrite_count,
                guest_engram_select_count,
                guest_engram_history_wait_count,
                guest_engram_state_wait_count,
                guest_engram_state_resolved_count,
                guest_engram_selected_tokens == terminal_tokens
            );
            if guest_engram_selected_tokens != terminal_tokens {
                anyhow::bail!(
                    "guest engram selected tokens do not match terminal writeback: selected={:?} terminal={:?}",
                    guest_engram_selected_tokens,
                    terminal_tokens
                );
            }
            if guest_engram_history_lengths != expected_history_lengths {
                anyhow::bail!(
                    "guest engram history object lengths are wrong: got={:?} expected={:?}",
                    guest_engram_history_lengths,
                    expected_history_lengths
                );
            }
            let expected_candidate_counts = report
                .steps
                .iter()
                .map(|step| step.candidates.len() as u64)
                .collect::<Vec<_>>();
            if guest_engram_candidate_counts != expected_candidate_counts {
                anyhow::bail!(
                    "guest engram candidate object counts are wrong: got={:?} expected={:?}",
                    guest_engram_candidate_counts,
                    expected_candidate_counts
                );
            }
            if !blocked_writeback_tokens.is_empty() {
                anyhow::bail!(
                    "guest engram blocked tokens reached writeback: blocked={:?} terminal={:?}",
                    blocked_writeback_tokens,
                    terminal_tokens
                );
            }
            if guest_engram_terminal_rewrite_count != expected_terminal_rewrites {
                anyhow::bail!(
                    "guest engram terminal rewrite count is wrong: rewrites={} expected={}",
                    guest_engram_terminal_rewrite_count,
                    expected_terminal_rewrites
                );
            }
        }
    }
    if timing_summary.worker_count > 0 {
        println!(
            "  guest_worker_timing: workers={} max_total_ms={} max_setup_ms={} max_seed_payload_ms={} max_descriptor_ms={} max_compute_window_ms={} max_submit_ms={} max_base_submit_ms={} max_doorbell_submit_ms={} max_batch_submit_ms={} max_dispatch_ms={} max_doorbell_log_ms={} max_batch_sleep_ms={} max_post_batch_ms={} max_completion_decode_ms={} max_compute_unaccounted_ms={} max_publish_ms={} max_input_wait_ms={} max_unaccounted_ms={} max_barrier_ms={}",
            timing_summary.worker_count,
            timing_summary.max_total_ms,
            timing_summary.max_setup_ms,
            timing_summary.max_seed_payload_ms,
            timing_summary.max_descriptor_ms,
            timing_summary.max_compute_window_ms,
            timing_summary.max_submit_ms,
            timing_summary.max_base_submit_ms,
            timing_summary.max_doorbell_submit_ms,
            timing_summary.max_batch_submit_ms,
            timing_summary.max_dispatch_ms,
            timing_summary.max_doorbell_log_ms,
            timing_summary.max_batch_sleep_ms,
            timing_summary.max_post_batch_ms,
            timing_summary.max_completion_decode_ms,
            timing_summary.max_compute_unaccounted_ms,
            timing_summary.max_publish_ms,
            timing_summary.max_input_wait_ms,
            timing_summary.max_unaccounted_ms,
            timing_summary.max_barrier_ms
        );
    }
    if runtime_forward_count != expected_runtime_forward_count
        || runtime_publish_count != expected_runtime_publish_count
        || runtime_input_count != expected_runtime_input_count
        || terminal_token_count != expected_terminal_token_count
        || guest_engram_select_count != expected_guest_engram_select_count
        || guest_engram_candidate_publish_count != expected_guest_engram_candidate_publish_count
        || guest_engram_candidate_wait_count != expected_guest_engram_candidate_wait_count
        || guest_engram_selected_wait_count != expected_guest_engram_selected_wait_count
        || guest_engram_selected_writeback_count != expected_guest_engram_selected_writeback_count
        || guest_engram_history_wait_count != expected_guest_engram_history_wait_count
        || guest_engram_state_wait_count != expected_guest_engram_state_wait_count
        || guest_engram_state_resolved_count != expected_guest_engram_state_resolved_count
    {
        anyhow::bail!(
            "qwen3 guest decode worker incomplete: range_forwards={}/{} runtime_inputs={}/{} runtime_outputs={}/{} terminal_tokens={}/{} engram_selects={}/{} engram_candidate_publishes={}/{} engram_candidate_waits={}/{} engram_selected_waits={}/{} engram_selected_writebacks={}/{} engram_history_waits={}/{} engram_state_waits={}/{} engram_state_resolved={}/{}",
            runtime_forward_count,
            expected_runtime_forward_count,
            runtime_input_count,
            expected_runtime_input_count,
            runtime_publish_count,
            expected_runtime_publish_count,
            terminal_token_count,
            expected_terminal_token_count,
            guest_engram_select_count,
            expected_guest_engram_select_count,
            guest_engram_candidate_publish_count,
            expected_guest_engram_candidate_publish_count,
            guest_engram_candidate_wait_count,
            expected_guest_engram_candidate_wait_count,
            guest_engram_selected_wait_count,
            expected_guest_engram_selected_wait_count,
            guest_engram_selected_writeback_count,
            expected_guest_engram_selected_writeback_count,
            guest_engram_history_wait_count,
            expected_guest_engram_history_wait_count,
            guest_engram_state_wait_count,
            expected_guest_engram_state_wait_count,
            guest_engram_state_resolved_count,
            expected_guest_engram_state_resolved_count
        );
    }
    Ok(())
}

fn qwen3_validate_guest_engram_state_registry(
    engram: &Qwen3EngramConfig,
    profile: &Qwen3DenseProfile,
) -> anyhow::Result<Option<Qwen3EngramStateRegistryValidation>> {
    let Some(state_ref) = &engram.state_ref else {
        return Ok(None);
    };
    let hidden_size = usize::try_from(profile.hidden_size)
        .with_context(|| format!("qwen3 hidden_size too large: {}", profile.hidden_size))?;
    if let Some(snapshot_path) = &engram.object_service_snapshot_path {
        return qwen3_validate_engram_state_object_service_payload(
            state_ref,
            snapshot_path,
            hidden_size,
        )
        .map(Some)
        .map_err(|err| {
            anyhow::anyhow!(
                "invalid W5 engram Object Service snapshot for {} hidden_size={}: {}",
                profile.model_id,
                hidden_size,
                err
            )
        });
    }
    let Some(registry_dir) = &engram.object_registry_dir else {
        anyhow::bail!(
            "W5 engram state ref requires Object Service snapshot or object registry payloads"
        );
    };
    qwen3_validate_engram_state_registry_payload(state_ref, registry_dir, hidden_size)
        .map(Some)
        .map_err(|err| {
            anyhow::anyhow!(
                "invalid W5 engram state registry for {} hidden_size={}: {}",
                profile.model_id,
                hidden_size,
                err
            )
        })
}

fn qwen3_prepare_engram_simt_mode(
    config: &Qwen3EngramConfig,
) -> anyhow::Result<Option<EngramSimtLaunchSpec>> {
    if !config.enabled
        || (config.mode != Qwen3EngramMode::FusedSimt
            && config.context_op != Qwen3EngramContextOp::FusedSimt)
    {
        return Ok(None);
    }

    let artifact_config = artifact_config_from_env(1, 65_536).map_err(|err| {
        anyhow::anyhow!(
            "qwen3 fused-simt engram mode requires a vendor artifact: {err}. \
             Set SIM_ENGRAM_SIMT_ARTIFACT_DIR to vendor/pto-isa/kernels/manual/a5/engram_simt/build \
             after building it with run.sh -r sim -v Ascend910_9599 -p"
        )
    })?;
    let spec = discover_engram_simt_artifact(&artifact_config).map_err(|err| {
        anyhow::anyhow!(
            "qwen3 fused-simt engram artifact is not usable: {err}. \
             Rebuild vendor/pto-isa/kernels/manual/a5/engram_simt with run.sh -r sim -v Ascend910_9599 -p"
        )
    })?;
    Ok(Some(spec))
}

fn qwen3_guest_log_match_count(haystack: &str, needle: &str) -> usize {
    haystack.match_indices(needle).count()
}

fn qwen3_guest_terminal_tokens(log: &str) -> Vec<u64> {
    let mut tokens = log
        .lines()
        .filter(|line| line.contains("stage qwen3_terminal_token_result_publish "))
        .map(|line| {
            (
                qwen3_guest_log_u64_field(line, "step"),
                qwen3_guest_log_u64_field(line, "token"),
            )
        })
        .collect::<Vec<_>>();
    tokens.sort_by_key(|(step, _)| *step);
    tokens.into_iter().map(|(_, token)| token).collect()
}

fn qwen3_guest_engram_selected_tokens(log: &str) -> Vec<u64> {
    let mut tokens = log
        .lines()
        .filter(|line| line.contains("stage qwen3_engram_token_select "))
        .map(|line| {
            (
                qwen3_guest_log_u64_field(line, "step"),
                qwen3_guest_log_u64_field(line, "selected_token"),
            )
        })
        .collect::<Vec<_>>();
    tokens.sort_by_key(|(step, _)| *step);
    tokens.into_iter().map(|(_, token)| token).collect()
}

fn qwen3_guest_engram_select_history_lengths(log: &str) -> Vec<u64> {
    let mut lengths = log
        .lines()
        .filter(|line| line.contains("stage qwen3_engram_token_select "))
        .map(|line| {
            (
                qwen3_guest_log_u64_field(line, "step"),
                qwen3_guest_log_u64_field(line, "history_tokens"),
            )
        })
        .collect::<Vec<_>>();
    lengths.sort_by_key(|(step, _)| *step);
    lengths.into_iter().map(|(_, length)| length).collect()
}

fn qwen3_guest_engram_history_lengths(log: &str) -> Vec<u64> {
    let mut lengths = log
        .lines()
        .filter(|line| line.contains("stage qwen3_engram_decision_publish "))
        .map(|line| {
            (
                qwen3_guest_log_u64_field(line, "step"),
                qwen3_guest_log_u64_field(line, "history_tokens"),
            )
        })
        .collect::<Vec<_>>();
    lengths.sort_by_key(|(step, _)| *step);
    lengths.into_iter().map(|(_, length)| length).collect()
}

fn qwen3_guest_engram_candidate_counts(log: &str) -> Vec<u64> {
    let mut counts = log
        .lines()
        .filter(|line| line.contains("stage qwen3_engram_candidates_publish "))
        .map(|line| {
            (
                qwen3_guest_log_u64_field(line, "step"),
                qwen3_guest_log_u64_field(line, "candidate_count"),
            )
        })
        .collect::<Vec<_>>();
    counts.sort_by_key(|(step, _)| *step);
    counts.into_iter().map(|(_, count)| count).collect()
}

fn qwen3_guest_engram_expected_terminal_rewrites(report: &Qwen3EngramRunReport) -> usize {
    report
        .steps
        .iter()
        .filter(|step| {
            step.candidates
                .first()
                .map(|candidate| candidate.token_id != step.selected_token)
                .unwrap_or(false)
        })
        .count()
}

fn qwen3_guest_terminal_text_lossy(tokens: &[u64], weights_path: &Path) -> Option<String> {
    let tokenizer_path = if weights_path.join("tokenizer.json").is_file() {
        weights_path.to_path_buf()
    } else {
        qwen3_guest_tokenizer_path()?
    };
    qwen3_guest_terminal_text_lossy_from_tokenizer(tokens, &tokenizer_path).ok()
}

fn qwen3_guest_prompt_token_ids_env(prompt: &str, weights_path: &Path) -> anyhow::Result<String> {
    let tokenizer_path = if weights_path.join("tokenizer.json").is_file() {
        weights_path.to_path_buf()
    } else {
        qwen3_guest_tokenizer_path()
            .ok_or_else(|| anyhow::anyhow!("qwen3 guest tokenizer path missing"))?
    };
    let tokenized = tokenize_prompt_from_tokenizer_path(&tokenizer_path, prompt)
        .map_err(anyhow::Error::msg)
        .context("failed to tokenize Qwen3 guest prompt")?;
    Ok(tokenized
        .token_ids
        .iter()
        .map(u64::to_string)
        .collect::<Vec<_>>()
        .join(","))
}

fn qwen3_guest_terminal_text_lossy_from_tokenizer(
    tokens: &[u64],
    tokenizer_path: &Path,
) -> Result<String, String> {
    if tokens.is_empty() {
        return Ok(String::new());
    }
    let mut bytes = Vec::new();
    for token in tokens {
        let piece = token_piece_bytes_from_tokenizer_path(tokenizer_path, *token)?;
        bytes.extend(token_piece_decode_bytes(&piece));
    }
    Ok(String::from_utf8_lossy(&bytes).into_owned())
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3EngramRunReport {
    config: Qwen3EngramConfig,
    steps: Vec<Qwen3EngramStepDecision>,
    selected_tokens: Vec<u64>,
    history_tokens: Vec<u64>,
    object_service: Option<Qwen3EngramObjectReport>,
    candidate_count: usize,
    blocked_token_count: u32,
    selected_token: u64,
    state_checksum: u64,
    policy_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3EngramObjectReport {
    object_puts: u64,
    object_resolves: u64,
    token_history_versions: u64,
    state_versions: u64,
    candidate_versions: u64,
    selected_token_versions: u64,
    history_token_count: u64,
    obmm_payload_writes: u64,
    obmm_payload_reads: u64,
    obmm_queue_submits: u64,
    obmm_queue_delivers: u64,
    obmm_bytes: u64,
    checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3GuestEngramObjectTransportReport {
    object_puts: u64,
    object_waits: u64,
    candidate_publishes: u64,
    candidate_waits: u64,
    decision_publishes: u64,
    selected_waits: u64,
    selected_writebacks: u64,
    history_waits: u64,
    state_waits: u64,
    state_resolved: u64,
    payload_write_bytes: u64,
    payload_read_bytes: u64,
    queue_submits: u64,
    queue_delivers: u64,
    checksum: u64,
}

fn qwen3_guest_engram_report(
    config: &Qwen3EngramConfig,
    session_id: u64,
    prompt_tokens: &[u64],
    log: &str,
) -> anyhow::Result<Qwen3EngramRunReport> {
    let candidates_by_step = qwen3_guest_candidate_records(log);
    let mut history = prompt_tokens.to_vec();
    let mut steps = Vec::with_capacity(candidates_by_step.len());

    for candidates in candidates_by_step {
        if candidates.is_empty() {
            continue;
        }
        let decision = qwen3_engram_select_token(config, session_id, &history, candidates.clone())?;
        history.push(decision.selected_token);
        steps.push(decision);
    }

    let selected_tokens = steps
        .iter()
        .map(|step| step.selected_token)
        .collect::<Vec<_>>();
    let candidate_count = steps.iter().map(|step| step.candidates.len()).sum();
    let blocked_token_count = steps
        .iter()
        .map(|step| step.blocked_token_count)
        .sum::<u32>();
    let selected_token = selected_tokens.last().copied().unwrap_or(0);
    let state_checksum = steps
        .last()
        .map(|step| step.state.state_checksum)
        .unwrap_or_else(|| qwen3_checksum_words(&[session_id]));
    let policy_checksum = qwen3_engram_policy_checksum(config, &steps);
    let object_service = match config.pool {
        Qwen3EngramPool::Inline => None,
        Qwen3EngramPool::Object => Some(qwen3_engram_publish_object_records(
            config.pool,
            session_id,
            prompt_tokens,
            &steps,
        )?),
        Qwen3EngramPool::Obmm => None,
    };

    Ok(Qwen3EngramRunReport {
        config: config.clone(),
        steps,
        selected_tokens,
        history_tokens: history,
        object_service,
        candidate_count,
        blocked_token_count,
        selected_token,
        state_checksum,
        policy_checksum,
    })
}

fn qwen3_guest_engram_report_from_guest_log(
    config: &Qwen3EngramConfig,
    session_id: u64,
    prompt_tokens: &[u64],
    log: &str,
) -> anyhow::Result<Qwen3EngramRunReport> {
    let mut decision_checksums = log
        .lines()
        .filter(|line| line.contains("stage qwen3_engram_decision_publish "))
        .map(|line| {
            (
                qwen3_guest_log_u64_field(line, "step"),
                qwen3_guest_log_u64_field(line, "history_tokens"),
                qwen3_guest_log_hex_u64_field(line, "history_checksum"),
                qwen3_guest_log_hex_u64_field(line, "state_checksum"),
                qwen3_guest_log_hex_u64_field(line, "logits_checksum"),
                qwen3_guest_log_hex_u64_field(line, "text_checksum"),
            )
        })
        .collect::<Vec<_>>();
    decision_checksums.sort_by_key(|(step, _, _, _, _, _)| *step);

    let mut steps = log
        .lines()
        .filter(|line| line.contains("stage qwen3_engram_token_select "))
        .map(|line| {
            let step = qwen3_guest_log_u64_field(line, "step");
            let candidate_count = qwen3_guest_log_u64_field(line, "candidate_count").clamp(1, 4);
            let raw_token = qwen3_guest_log_u64_field(line, "raw_token");
            let runner_up = qwen3_guest_log_u64_field(line, "runner_up");
            let candidate2 = qwen3_guest_log_u64_field(line, "candidate2");
            let candidate3 = qwen3_guest_log_u64_field(line, "candidate3");
            let selected_token = qwen3_guest_log_u64_field(line, "selected_token");
            let blocked_token_count = qwen3_guest_log_u64_field(line, "blocked") as u32;
            let fallback_used = qwen3_guest_log_u64_field(line, "fallback") != 0;
            let top_score = qwen3_guest_log_i64_field(line, "top_score_milli") as i32;
            let runner_up_score = qwen3_guest_log_i64_field(line, "runner_up_score_milli") as i32;
            let tokens = [raw_token, runner_up, candidate2, candidate3];
            let candidates = tokens
                .into_iter()
                .take(candidate_count as usize)
                .enumerate()
                .map(|(rank, token_id)| Qwen3CandidateRecord {
                    step_index: step,
                    rank: rank as u64,
                    token_id,
                    logit_milli: match rank {
                        0 => top_score,
                        1 => runner_up_score,
                        _ => -(rank as i32),
                    },
                    adjusted_score_milli: 0,
                    token_piece_checksum: 0,
                })
                .collect::<Vec<_>>();
            let (
                _,
                history_tokens,
                history_checksum,
                state_checksum,
                logits_checksum,
                text_checksum,
            ) = decision_checksums
                .iter()
                .find(|(decision_step, _, _, _, _, _)| *decision_step == step)
                .copied()
                .unwrap_or((step, prompt_tokens.len() as u64 + step + 1, 0, 0, 0, 0));
            let state = Qwen3EngramState {
                session_id,
                step_index: step,
                token_count: history_tokens,
                rolling_hash: history_checksum,
                ngram_window: config.no_repeat_ngram_size.min(u8::MAX as usize) as u8,
                repetition_penalty_milli: config.repetition_penalty_milli,
                blocked_token_count,
                fallback_used,
                raw_sampled_token: raw_token,
                runner_up_token: runner_up,
                top_score_milli: top_score,
                runner_up_score_milli: runner_up_score,
                history_window: config.history_window as u64,
                logits_checksum,
                text_checksum,
                selected_token,
                state_checksum,
            };

            Qwen3EngramStepDecision {
                step_index: step,
                candidates,
                selected_token,
                blocked_token_count,
                fallback_used,
                state,
            }
        })
        .collect::<Vec<_>>();
    steps.sort_by_key(|step| step.step_index);

    if steps.is_empty() {
        anyhow::bail!("guest engram decision log is empty");
    }
    let selected_tokens = steps
        .iter()
        .map(|step| step.selected_token)
        .collect::<Vec<_>>();
    let mut history_tokens = prompt_tokens.to_vec();
    history_tokens.extend(selected_tokens.iter().copied());
    let candidate_count = steps.iter().map(|step| step.candidates.len()).sum();
    let blocked_token_count = steps
        .iter()
        .map(|step| step.blocked_token_count)
        .sum::<u32>();
    let selected_token = selected_tokens.last().copied().unwrap_or(0);
    let state_checksum = steps
        .last()
        .map(|step| step.state.state_checksum)
        .unwrap_or_else(|| qwen3_checksum_words(&[session_id]));
    let mut policy_words = vec![
        config.enabled as u64,
        config.no_repeat_ngram_size as u64,
        config.repetition_penalty_milli as u64,
        config.history_window as u64,
        qwen3_checksum_words(&config.blocked_token_ids),
    ];
    for step in &steps {
        policy_words.extend_from_slice(&[
            step.step_index,
            step.selected_token,
            step.blocked_token_count as u64,
            step.fallback_used as u64,
            step.candidates.len() as u64,
            step.state.state_checksum,
        ]);
    }

    Ok(Qwen3EngramRunReport {
        config: config.clone(),
        steps,
        selected_tokens,
        history_tokens,
        object_service: None,
        candidate_count,
        blocked_token_count,
        selected_token,
        state_checksum,
        policy_checksum: qwen3_checksum_words(&policy_words),
    })
}

fn qwen3_guest_engram_object_transport_report(log: &str) -> Qwen3GuestEngramObjectTransportReport {
    let candidate_publishes =
        qwen3_guest_log_match_count(log, "stage qwen3_engram_candidates_publish ") as u64;
    let candidate_waits =
        qwen3_guest_log_match_count(log, "stage qwen3_engram_candidates_wait ") as u64;
    let decision_publishes =
        qwen3_guest_log_match_count(log, "stage qwen3_engram_decision_publish ") as u64;
    let selected_waits =
        qwen3_guest_log_match_count(log, "stage qwen3_engram_selected_token_wait ") as u64;
    let selected_writebacks =
        qwen3_guest_log_match_count(log, "stage qwen3_engram_selected_writeback ") as u64;
    let history_waits = qwen3_guest_log_match_count(log, "stage qwen3_engram_history_wait ") as u64;
    let state_waits = qwen3_guest_log_match_count(log, "stage qwen3_engram_state_wait ") as u64;
    let state_resolved =
        qwen3_guest_log_match_count(log, "stage qwen3_decode_round_engram_state_resolved ") as u64;
    let history_write_bytes = log
        .lines()
        .filter(|line| line.contains("stage qwen3_engram_decision_publish "))
        .map(|line| {
            let history_tokens = qwen3_guest_log_u64_field(line, "history_tokens");
            (history_tokens + 2) * 8 + 64 + 128
        })
        .sum::<u64>();
    let payload_write_bytes = candidate_publishes * 256 + history_write_bytes;
    let payload_read_bytes =
        qwen3_guest_log_sum_u64_field(log, "stage qwen3_engram_candidates_wait ", "bytes")
            + qwen3_guest_log_sum_u64_field(
                log,
                "stage qwen3_engram_selected_token_wait ",
                "bytes",
            )
            + qwen3_guest_log_sum_u64_field(log, "stage qwen3_engram_history_wait ", "bytes")
            + qwen3_guest_log_sum_u64_field(log, "stage qwen3_engram_state_wait ", "bytes");
    let object_puts = candidate_publishes + decision_publishes * 3;
    let object_waits = candidate_waits + selected_waits + history_waits + state_waits;
    let queue_submits = candidate_publishes + decision_publishes * 3;
    let queue_delivers = object_waits;
    let checksum = qwen3_checksum_words(&[
        object_puts,
        object_waits,
        candidate_publishes,
        candidate_waits,
        decision_publishes,
        selected_waits,
        selected_writebacks,
        history_waits,
        state_waits,
        state_resolved,
        payload_write_bytes,
        payload_read_bytes,
        queue_submits,
        queue_delivers,
    ]);

    Qwen3GuestEngramObjectTransportReport {
        object_puts,
        object_waits,
        candidate_publishes,
        candidate_waits,
        decision_publishes,
        selected_waits,
        selected_writebacks,
        history_waits,
        state_waits,
        state_resolved,
        payload_write_bytes,
        payload_read_bytes,
        queue_submits,
        queue_delivers,
        checksum,
    }
}

fn qwen3_guest_log_sum_u64_field(log: &str, stage: &str, key: &str) -> u64 {
    log.lines()
        .filter(|line| line.contains(stage))
        .map(|line| qwen3_guest_log_u64_field(line, key))
        .sum()
}

fn print_qwen3_guest_engram_report(report: &Qwen3EngramRunReport) {
    if report.config.report == Qwen3EngramReport::None {
        return;
    }
    println!(
        "  engram_summary: engram_enabled=true engram_mode={} engram_pool={} engram_steps={} engram_history_tokens={} engram_candidate_count={} engram_blocked_token_count={} engram_selected_token={} engram_state_checksum={:#x} engram_policy_checksum={:#x}",
        qwen3_engram_mode_name(report.config.mode),
        qwen3_engram_pool_name(report.config.pool),
        report.steps.len(),
        report.history_tokens.len(),
        report.candidate_count,
        report.blocked_token_count,
        report.selected_token,
        report.state_checksum,
        report.policy_checksum
    );
    if let Some(object) = &report.object_service {
        println!(
            "  engram_object_service: engram_object_puts={} engram_object_resolves={} engram_token_history_versions={} engram_state_versions={} engram_candidate_versions={} engram_selected_token_versions={} engram_history_tokens={} engram_obmm_payload_writes={} engram_obmm_payload_reads={} engram_queue_submits={} engram_queue_delivers={} engram_obmm_bytes={} checksum={:#x}",
            object.object_puts,
            object.object_resolves,
            object.token_history_versions,
            object.state_versions,
            object.candidate_versions,
            object.selected_token_versions,
            object.history_token_count,
            object.obmm_payload_writes,
            object.obmm_payload_reads,
            object.obmm_queue_submits,
            object.obmm_queue_delivers,
            object.obmm_bytes,
            object.checksum
        );
    }
    if matches!(
        report.config.report,
        Qwen3EngramReport::Steps | Qwen3EngramReport::Verbose
    ) {
        for step in &report.steps {
            println!(
                "  engram step={} candidates={} blocked={} selected={} fallback_used={} state_checksum={:#x}",
                step.step_index,
                step.candidates.len(),
                step.blocked_token_count,
                step.selected_token,
                step.fallback_used,
                step.state.state_checksum
            );
            if report.config.report == Qwen3EngramReport::Verbose {
                for candidate in &step.candidates {
                    println!(
                        "    candidate rank={} token={} logit_milli={} adjusted_score_milli={} piece_checksum={:#x}",
                        candidate.rank,
                        candidate.token_id,
                        candidate.logit_milli,
                        candidate.adjusted_score_milli,
                        candidate.token_piece_checksum
                    );
                }
            }
        }
    }
}

fn qwen3_guest_terminal_candidate_records(log: &str) -> Vec<Vec<Qwen3CandidateRecord>> {
    let mut by_step = log
        .lines()
        .filter(|line| line.contains("stage qwen3_terminal_token_result_publish "))
        .map(|line| {
            let step = qwen3_guest_log_u64_field(line, "step");
            let token = qwen3_guest_log_u64_field(line, "token");
            let runner_up = qwen3_guest_log_u64_field(line, "runner_up");
            let margin = qwen3_guest_log_u64_field(line, "margin_milli").min(i32::MAX as u64);
            let text_checksum = qwen3_guest_log_hex_u64_field(line, "text_checksum");
            let mut candidates = vec![Qwen3CandidateRecord {
                step_index: step,
                rank: 0,
                token_id: token,
                logit_milli: margin as i32,
                adjusted_score_milli: margin as i32,
                token_piece_checksum: text_checksum,
            }];
            if runner_up != token {
                candidates.push(Qwen3CandidateRecord {
                    step_index: step,
                    rank: 1,
                    token_id: runner_up,
                    logit_milli: 0,
                    adjusted_score_milli: 0,
                    token_piece_checksum: 0,
                });
            }
            (step, candidates)
        })
        .collect::<Vec<_>>();
    by_step.sort_by_key(|(step, _)| *step);
    by_step
        .into_iter()
        .map(|(_, candidates)| candidates)
        .collect()
}

fn qwen3_guest_candidate_records(log: &str) -> Vec<Vec<Qwen3CandidateRecord>> {
    let engram_candidates = qwen3_guest_engram_candidate_records(log);

    if engram_candidates.is_empty() {
        qwen3_guest_terminal_candidate_records(log)
    } else {
        engram_candidates
    }
}

fn qwen3_guest_engram_candidate_records(log: &str) -> Vec<Vec<Qwen3CandidateRecord>> {
    let terminal_margins = log
        .lines()
        .filter(|line| line.contains("stage qwen3_terminal_token_result_publish "))
        .map(|line| {
            (
                qwen3_guest_log_u64_field(line, "step"),
                qwen3_guest_log_u64_field(line, "margin_milli").min(i32::MAX as u64),
                qwen3_guest_log_hex_u64_field(line, "text_checksum"),
            )
        })
        .collect::<Vec<_>>();
    let mut by_step = log
        .lines()
        .filter(|line| line.contains("stage qwen3_engram_token_select "))
        .map(|line| {
            let step = qwen3_guest_log_u64_field(line, "step");
            let token = qwen3_guest_log_u64_field(line, "raw_token");
            let runner_up = qwen3_guest_log_u64_field(line, "runner_up");
            let candidate_count = qwen3_guest_log_u64_field(line, "candidate_count");
            let candidate2 = qwen3_guest_log_u64_field(line, "candidate2");
            let candidate3 = qwen3_guest_log_u64_field(line, "candidate3");
            let (margin, text_checksum) = terminal_margins
                .iter()
                .find(|(terminal_step, _, _)| *terminal_step == step)
                .map(|(_, margin, text_checksum)| (*margin, *text_checksum))
                .unwrap_or((0, 0));
            let mut candidates = vec![Qwen3CandidateRecord {
                step_index: step,
                rank: 0,
                token_id: token,
                logit_milli: margin as i32,
                adjusted_score_milli: margin as i32,
                token_piece_checksum: text_checksum,
            }];
            if runner_up != 0 && runner_up != token {
                candidates.push(Qwen3CandidateRecord {
                    step_index: step,
                    rank: 1,
                    token_id: runner_up,
                    logit_milli: 0,
                    adjusted_score_milli: 0,
                    token_piece_checksum: 0,
                });
            }
            if candidate_count > 2
                && candidate2 != 0
                && !candidates
                    .iter()
                    .any(|candidate| candidate.token_id == candidate2)
            {
                candidates.push(Qwen3CandidateRecord {
                    step_index: step,
                    rank: candidates.len() as u64,
                    token_id: candidate2,
                    logit_milli: -2,
                    adjusted_score_milli: -2,
                    token_piece_checksum: 0,
                });
            }
            if candidate_count > 3
                && candidate3 != 0
                && !candidates
                    .iter()
                    .any(|candidate| candidate.token_id == candidate3)
            {
                candidates.push(Qwen3CandidateRecord {
                    step_index: step,
                    rank: candidates.len() as u64,
                    token_id: candidate3,
                    logit_milli: -3,
                    adjusted_score_milli: -3,
                    token_piece_checksum: 0,
                });
            }
            (step, candidates)
        })
        .collect::<Vec<_>>();
    by_step.sort_by_key(|(step, _)| *step);
    by_step
        .into_iter()
        .map(|(_, candidates)| candidates)
        .collect()
}

fn qwen3_engram_select_token(
    config: &Qwen3EngramConfig,
    session_id: u64,
    history: &[u64],
    mut candidates: Vec<Qwen3CandidateRecord>,
) -> anyhow::Result<Qwen3EngramStepDecision> {
    let Some(first) = candidates.first() else {
        anyhow::bail!("engram candidate table is empty");
    };
    let step_index = first.step_index;
    let first_token = first.token_id;
    if qwen3_engram_is_stop_token(first_token) {
        let state = qwen3_engram_state(
            config,
            session_id,
            step_index,
            history,
            &candidates,
            first_token,
            0,
            false,
        );
        return Ok(Qwen3EngramStepDecision {
            step_index,
            candidates,
            selected_token: first_token,
            blocked_token_count: 0,
            fallback_used: false,
            state,
        });
    }

    let effective_history = qwen3_engram_effective_history(config, history);
    let mut blocked_count = 0u32;
    let mut best: Option<(usize, i32, u64, u64)> = None;

    for (index, candidate) in candidates.iter_mut().enumerate() {
        let repeated = effective_history.contains(&candidate.token_id);
        let mut adjusted = candidate.logit_milli;
        if repeated && config.repetition_penalty_milli > 1000 {
            adjusted = adjusted.saturating_sub((config.repetition_penalty_milli - 1000) as i32);
        }
        candidate.adjusted_score_milli = adjusted;

        if config.blocked_token_ids.contains(&candidate.token_id)
            || qwen3_engram_repeats_ngram(
                effective_history,
                candidate.token_id,
                config.no_repeat_ngram_size,
            )
        {
            blocked_count += 1;
            continue;
        }

        let tie = qwen3_checksum_words(&[
            step_index,
            candidate.token_id,
            qwen3_checksum_words(effective_history),
        ]);
        match best {
            Some((_, best_score, best_rank, best_tie))
                if adjusted < best_score
                    || (adjusted == best_score && candidate.rank > best_rank)
                    || (adjusted == best_score
                        && candidate.rank == best_rank
                        && tie >= best_tie) => {}
            _ => best = Some((index, adjusted, candidate.rank, tie)),
        }
    }

    let (selected_index, fallback_used) = best
        .map(|(index, _, _, _)| (index, false))
        .unwrap_or((0, true));
    let selected_token = candidates[selected_index].token_id;
    let state = qwen3_engram_state(
        config,
        session_id,
        step_index,
        history,
        &candidates,
        selected_token,
        blocked_count,
        fallback_used,
    );

    Ok(Qwen3EngramStepDecision {
        step_index,
        candidates,
        selected_token,
        blocked_token_count: blocked_count,
        fallback_used,
        state,
    })
}

fn qwen3_engram_state(
    config: &Qwen3EngramConfig,
    session_id: u64,
    step_index: u64,
    history: &[u64],
    candidates: &[Qwen3CandidateRecord],
    selected_token: u64,
    blocked_token_count: u32,
    fallback_used: bool,
) -> Qwen3EngramState {
    let token_count = history.len() as u64 + 1;
    let rolling_hash = qwen3_checksum_words(history);
    let raw_sampled_token = candidates
        .first()
        .map(|candidate| candidate.token_id)
        .unwrap_or(0);
    let runner_up_token = candidates
        .get(1)
        .map(|candidate| candidate.token_id)
        .unwrap_or(0);
    let top_score_milli = candidates
        .first()
        .map(|candidate| candidate.logit_milli)
        .unwrap_or(0);
    let runner_up_score_milli = candidates
        .get(1)
        .map(|candidate| candidate.logit_milli)
        .unwrap_or(0);
    let logits_checksum = qwen3_checksum_words(&qwen3_engram_candidate_words(candidates));
    let text_checksum = qwen3_checksum_words(
        &candidates
            .iter()
            .map(|candidate| candidate.token_piece_checksum)
            .collect::<Vec<_>>(),
    );
    let state_checksum = qwen3_checksum_words(&[
        step_index,
        token_count,
        selected_token,
        rolling_hash,
        config.no_repeat_ngram_size as u64,
        config.repetition_penalty_milli as u64,
        blocked_token_count as u64,
        fallback_used as u64,
        raw_sampled_token,
        runner_up_token,
        top_score_milli as i64 as u64,
        runner_up_score_milli as i64 as u64,
        config.history_window as u64,
        logits_checksum,
        text_checksum,
    ]);
    Qwen3EngramState {
        session_id,
        step_index,
        token_count,
        rolling_hash,
        ngram_window: config.no_repeat_ngram_size.min(u8::MAX as usize) as u8,
        repetition_penalty_milli: config.repetition_penalty_milli,
        blocked_token_count,
        fallback_used,
        raw_sampled_token,
        runner_up_token,
        top_score_milli,
        runner_up_score_milli,
        history_window: config.history_window as u64,
        logits_checksum,
        text_checksum,
        selected_token,
        state_checksum,
    }
}

fn qwen3_engram_effective_history<'a>(config: &Qwen3EngramConfig, history: &'a [u64]) -> &'a [u64] {
    if config.history_window == 0 || history.len() <= config.history_window {
        history
    } else {
        &history[history.len() - config.history_window..]
    }
}

fn qwen3_engram_repeats_ngram(history: &[u64], token: u64, ngram_size: usize) -> bool {
    if ngram_size == 0 || history.len() + 1 < ngram_size {
        return false;
    }
    let prefix_len = ngram_size - 1;
    let prefix_start = history.len().saturating_sub(prefix_len);
    let prefix = &history[prefix_start..];
    history
        .windows(ngram_size)
        .any(|window| window[..prefix_len] == *prefix && window[prefix_len] == token)
}

fn qwen3_engram_is_stop_token(token: u64) -> bool {
    matches!(token, 151_643 | 151_645)
}

fn qwen3_engram_policy_checksum(
    config: &Qwen3EngramConfig,
    steps: &[Qwen3EngramStepDecision],
) -> u64 {
    let mut words = vec![
        config.enabled as u64,
        config.no_repeat_ngram_size as u64,
        config.repetition_penalty_milli as u64,
        config.history_window as u64,
        qwen3_checksum_words(&config.blocked_token_ids),
    ];
    for step in steps {
        words.extend_from_slice(&[
            step.step_index,
            step.selected_token,
            step.blocked_token_count as u64,
            step.fallback_used as u64,
            step.state.state_checksum,
        ]);
    }
    qwen3_checksum_words(&words)
}

fn qwen3_engram_publish_object_records(
    pool: Qwen3EngramPool,
    session_id: u64,
    prompt_tokens: &[u64],
    steps: &[Qwen3EngramStepDecision],
) -> anyhow::Result<Qwen3EngramObjectReport> {
    let mut profile = LingquObjectServiceProfile::default();
    profile.inline_value_limit = 1 << 20;
    profile.queue_depth = 4096;
    profile.obmm_pool.enabled = pool == Qwen3EngramPool::Obmm;
    let mut service = LingquObjectServiceStub::new(profile);
    let payload_backend = match pool {
        Qwen3EngramPool::Inline | Qwen3EngramPool::Object => LingquPayloadBackend::Inline,
        Qwen3EngramPool::Obmm => LingquPayloadBackend::Shmem,
    };
    let session = format!("{session_id:016x}");
    let history_key = format!("qwen3/session/{session}/tokens/history");
    let mut history = prompt_tokens.to_vec();
    let mut clock = 1u64;

    qwen3_engram_object_publish_words(
        &mut service,
        &history_key,
        LingquObjectKind::TokenBuffer,
        &qwen3_engram_history_words(&history),
        payload_backend,
        None,
        clock,
    )?;
    qwen3_engram_object_assert_latest_version(&service, &history_key, 1)?;
    clock += 1;

    for step in steps {
        let step_index = step.step_index;
        let current_history_version = step_index + 1;
        qwen3_engram_object_resolve_exact(
            &mut service,
            &history_key,
            current_history_version,
            payload_backend,
            clock,
        )?;
        clock += 1;

        if step_index > 0 {
            let previous_state_key = format!(
                "qwen3/session/{session}/step/{}/engram/state",
                step_index - 1
            );
            qwen3_engram_object_resolve_exact(
                &mut service,
                &previous_state_key,
                1,
                payload_backend,
                clock,
            )?;
            clock += 1;
        }

        let candidates_key = format!("qwen3/session/{session}/step/{step_index}/candidates/topk");
        qwen3_engram_object_publish_words(
            &mut service,
            &candidates_key,
            LingquObjectKind::Logits,
            &qwen3_engram_candidate_words(&step.candidates),
            payload_backend,
            None,
            clock,
        )?;
        qwen3_engram_object_assert_latest_version(&service, &candidates_key, 1)?;
        clock += 1;

        let selected_key = format!("qwen3/session/{session}/step/{step_index}/tokens/selected");
        qwen3_engram_object_publish_words(
            &mut service,
            &selected_key,
            LingquObjectKind::TokenBuffer,
            &[step.step_index, step.selected_token],
            payload_backend,
            None,
            clock,
        )?;
        qwen3_engram_object_assert_latest_version(&service, &selected_key, 1)?;
        clock += 1;

        let state_key = format!("qwen3/session/{session}/step/{step_index}/engram/state");
        qwen3_engram_object_publish_words(
            &mut service,
            &state_key,
            LingquObjectKind::Metadata,
            &qwen3_engram_state_words(&step.state),
            payload_backend,
            None,
            clock,
        )?;
        qwen3_engram_object_assert_latest_version(&service, &state_key, 1)?;
        clock += 1;

        history.push(step.selected_token);
        qwen3_engram_object_publish_words(
            &mut service,
            &history_key,
            LingquObjectKind::TokenBuffer,
            &qwen3_engram_history_words(&history),
            payload_backend,
            Some(current_history_version),
            clock,
        )?;
        qwen3_engram_object_assert_latest_version(
            &service,
            &history_key,
            current_history_version + 1,
        )?;
        clock += 1;
    }

    let report = service.report();
    let object_report = Qwen3EngramObjectReport {
        object_puts: report.publish_count,
        object_resolves: report.resolve_count,
        token_history_versions: steps.len() as u64 + 1,
        state_versions: steps.len() as u64,
        candidate_versions: steps.len() as u64,
        selected_token_versions: steps.len() as u64,
        history_token_count: history.len() as u64,
        obmm_payload_writes: report.obmm_pool_payload_write_count,
        obmm_payload_reads: report.obmm_pool_payload_read_count,
        obmm_queue_submits: report.obmm_pool_queue_submit_count,
        obmm_queue_delivers: report.obmm_pool_queue_deliver_count,
        obmm_bytes: report.obmm_pool_bytes_used,
        checksum: qwen3_checksum_words(&[
            report.publish_count,
            report.resolve_count,
            steps.len() as u64 + 1,
            history.len() as u64,
            report.obmm_pool_payload_write_count,
            report.obmm_pool_payload_read_count,
            report.obmm_pool_queue_submit_count,
            report.obmm_pool_queue_deliver_count,
            report.checksum,
        ]),
    };
    if object_report.history_token_count != prompt_tokens.len() as u64 + steps.len() as u64 {
        anyhow::bail!("engram token history object length mismatch");
    }
    Ok(object_report)
}

fn qwen3_engram_object_publish_words(
    service: &mut LingquObjectServiceStub,
    key: &str,
    kind: LingquObjectKind,
    words: &[u64],
    backend: LingquPayloadBackend,
    expected_version: Option<u64>,
    now: u64,
) -> anyhow::Result<()> {
    let payload = qwen3_words_to_bytes(words);
    let checksum = qwen3_checksum_words(words);
    service
        .submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: key.to_string(),
                kind,
                producer_entity: 0,
                owner_entity: Some(0),
                expected_version,
                metadata: LingquObjectMetadata {
                    bytes: payload.len() as u64,
                    checksum,
                    dtype: None,
                    shape: vec![words.len() as u64],
                    layout: None,
                    expires_at_us: None,
                },
                placements: vec![LingquPayloadPlacement {
                    backend,
                    storage_ref: format!("{key}/{}", qwen3_payload_backend_name(backend)),
                    segment: None,
                    offset: 0,
                    bytes: payload.len() as u64,
                    checksum,
                    locality: LingquObjectLocality::DomainShared(0),
                }],
                payload_bytes: payload,
            },
            now,
        )
        .map(|_| ())
        .map_err(anyhow::Error::from)
}

fn qwen3_engram_object_resolve_exact(
    service: &mut LingquObjectServiceStub,
    key: &str,
    version: u64,
    backend: LingquPayloadBackend,
    now: u64,
) -> anyhow::Result<Vec<u8>> {
    service
        .submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: key.to_string(),
                requester_entity: 0,
                version: LingquObjectVersionSelector::Exact(version),
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![backend],
            },
            now,
        )
        .map_err(anyhow::Error::from)?;
    service
        .get_copy(key, LingquObjectVersionSelector::Exact(version))
        .ok_or_else(|| anyhow::anyhow!("engram object exact resolve failed: {key}@{version}"))
}

fn qwen3_engram_object_assert_latest_version(
    service: &LingquObjectServiceStub,
    key: &str,
    expected_version: u64,
) -> anyhow::Result<()> {
    let record = service
        .latest_record(key)
        .ok_or_else(|| anyhow::anyhow!("engram object publish missing: {key}"))?;
    if record.version != expected_version {
        anyhow::bail!(
            "engram object version mismatch: {key} got={} expected={}",
            record.version,
            expected_version
        );
    }
    Ok(())
}

fn qwen3_engram_history_words(history: &[u64]) -> Vec<u64> {
    let mut words = Vec::with_capacity(history.len() + 2);
    words.push(history.len() as u64);
    words.extend_from_slice(history);
    words.push(qwen3_checksum_words(history));
    words
}

fn qwen3_engram_candidate_words(candidates: &[Qwen3CandidateRecord]) -> Vec<u64> {
    let mut words = Vec::with_capacity(1 + candidates.len() * 6);
    words.push(candidates.len() as u64);
    for candidate in candidates {
        words.extend_from_slice(&[
            candidate.step_index,
            candidate.rank,
            candidate.token_id,
            candidate.logit_milli as i64 as u64,
            candidate.adjusted_score_milli as i64 as u64,
            candidate.token_piece_checksum,
        ]);
    }
    words
}

fn qwen3_engram_state_words(state: &Qwen3EngramState) -> Vec<u64> {
    vec![
        state.step_index,
        state.token_count,
        state.selected_token,
        state.rolling_hash,
        state.ngram_window as u64,
        state.repetition_penalty_milli as u64,
        state.blocked_token_count as u64,
        state.fallback_used as u64,
        state.raw_sampled_token,
        state.runner_up_token,
        state.top_score_milli as i64 as u64,
        state.runner_up_score_milli as i64 as u64,
        state.history_window,
        state.logits_checksum,
        state.text_checksum,
        state.state_checksum,
    ]
}

fn qwen3_words_to_bytes(words: &[u64]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(words.len() * std::mem::size_of::<u64>());
    for word in words {
        bytes.extend_from_slice(&word.to_le_bytes());
    }
    bytes
}

fn qwen3_guest_session_id(prompt_tokens: &[u64]) -> u64 {
    qwen3_checksum_words(prompt_tokens)
}

fn qwen3_parse_token_id_csv(csv: &str) -> anyhow::Result<Vec<u64>> {
    if csv.trim().is_empty() {
        return Ok(Vec::new());
    }
    csv.split(',')
        .map(|token| {
            token
                .trim()
                .parse::<u64>()
                .with_context(|| format!("invalid token id: {token}"))
        })
        .collect()
}

fn qwen3_checksum_words(words: &[u64]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for word in words {
        acc ^= *word;
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn qwen3_engram_mode_name(mode: Qwen3EngramMode) -> &'static str {
    match mode {
        Qwen3EngramMode::Cpu => "cpu",
        Qwen3EngramMode::FusedSimt => "fused-simt",
    }
}

fn qwen3_engram_pool_name(pool: Qwen3EngramPool) -> &'static str {
    match pool {
        Qwen3EngramPool::Inline => "inline",
        Qwen3EngramPool::Object => "object",
        Qwen3EngramPool::Obmm => "obmm",
    }
}

fn qwen3_engram_context_op_name(context_op: Qwen3EngramContextOp) -> &'static str {
    match context_op {
        Qwen3EngramContextOp::Disabled => "disabled",
        Qwen3EngramContextOp::CpuReference => "cpu-reference",
        Qwen3EngramContextOp::FusedSimt => "fused-simt",
        Qwen3EngramContextOp::SimplerHost => "simpler-host",
    }
}

fn qwen3_engram_report_name(report: Qwen3EngramReport) -> &'static str {
    match report {
        Qwen3EngramReport::None => "none",
        Qwen3EngramReport::Summary => "summary",
        Qwen3EngramReport::Steps => "steps",
        Qwen3EngramReport::Verbose => "verbose",
    }
}

fn qwen3_payload_backend_name(backend: LingquPayloadBackend) -> &'static str {
    match backend {
        LingquPayloadBackend::Inline => "inline",
        LingquPayloadBackend::Shmem => "obmm",
        LingquPayloadBackend::ObmmShmem => "obmm_shmem",
        LingquPayloadBackend::Block => "block",
        LingquPayloadBackend::Dfs => "dfs",
        LingquPayloadBackend::External => "external",
    }
}

fn qwen3_guest_tokenizer_path() -> Option<PathBuf> {
    let path = env::var_os("SIM_QWEN3_DENSE_WEIGHTS_PATH")?;
    let path = PathBuf::from(path);
    if path.join("tokenizer.json").is_file() {
        Some(path)
    } else {
        None
    }
}

fn validate_qwen3_dense_weights_path(path: &Path) -> anyhow::Result<()> {
    if !path.is_dir() {
        anyhow::bail!(
            "Qwen3 dense weights path must point to a model directory: {}",
            path.display()
        );
    }
    for required in ["config.json", "tokenizer.json"] {
        let candidate = path.join(required);
        if !candidate.is_file() {
            anyhow::bail!(
                "Qwen3 dense weights path is missing required file {} in {}",
                required,
                path.display()
            );
        }
    }
    if !path.join("model.safetensors").is_file()
        && !path.join("model.safetensors.index.json").is_file()
    {
        anyhow::bail!(
            "Qwen3 dense weights path requires model.safetensors or model.safetensors.index.json in {}",
            path.display()
        );
    }
    Ok(())
}

fn qwen3_guest_dense_runtime(
    args: &Qwen3GuestDecodeLoopCliArgs,
) -> anyhow::Result<Qwen3DenseGuestRuntime> {
    let weights_path = args
        .weights_path
        .clone()
        .or_else(|| env::var_os("SIM_QWEN3_DENSE_WEIGHTS_PATH").map(PathBuf::from))
        .ok_or_else(|| {
            anyhow::anyhow!(
                "qwen3-guest-decode-loop requires --weights-path or SIM_QWEN3_DENSE_WEIGHTS_PATH"
            )
        })?;
    validate_qwen3_dense_weights_path(&weights_path)?;

    let profile = profile_from_weights_dir(
        &weights_path,
        args.model.as_deref(),
        QWEN3_DENSE_DEFAULT_TP_NODES,
        QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
        QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
    )
    .map_err(anyhow::Error::msg)?;
    let model_key = qwen3_dense_model_key(&profile.model_id);
    let chipbackend_profile = "qwen3_dense";

    Ok(Qwen3DenseGuestRuntime {
        profile,
        model_key,
        weights_path,
        chipbackend_profile,
    })
}

#[derive(Default)]
struct Qwen3GuestTimingSummary {
    worker_count: usize,
    max_total_ms: u64,
    max_setup_ms: u64,
    max_seed_payload_ms: u64,
    max_descriptor_ms: u64,
    max_compute_window_ms: u64,
    max_submit_ms: u64,
    max_base_submit_ms: u64,
    max_doorbell_submit_ms: u64,
    max_batch_submit_ms: u64,
    max_dispatch_ms: u64,
    max_doorbell_log_ms: u64,
    max_batch_sleep_ms: u64,
    max_post_batch_ms: u64,
    max_completion_decode_ms: u64,
    max_compute_unaccounted_ms: u64,
    max_publish_ms: u64,
    max_input_wait_ms: u64,
    max_unaccounted_ms: u64,
    max_barrier_ms: u64,
}

fn qwen3_guest_timing_summary(log: &str) -> Qwen3GuestTimingSummary {
    let mut summary = Qwen3GuestTimingSummary::default();

    for line in log.lines() {
        if line.contains("stage qwen3_worker_timing ") {
            summary.worker_count += 1;
            summary.max_total_ms = summary
                .max_total_ms
                .max(qwen3_guest_log_u64_field(line, "total_ms"));
            summary.max_setup_ms = summary
                .max_setup_ms
                .max(qwen3_guest_log_u64_field(line, "setup_ms"));
            summary.max_seed_payload_ms = summary
                .max_seed_payload_ms
                .max(qwen3_guest_log_u64_field(line, "seed_payload_ms"));
            summary.max_descriptor_ms = summary
                .max_descriptor_ms
                .max(qwen3_guest_log_u64_field(line, "descriptor_ms"));
            summary.max_compute_window_ms = summary
                .max_compute_window_ms
                .max(qwen3_guest_log_u64_field(line, "compute_window_ms"));
            summary.max_submit_ms = summary
                .max_submit_ms
                .max(qwen3_guest_log_u64_field(line, "submit_ms"));
            summary.max_base_submit_ms = summary
                .max_base_submit_ms
                .max(qwen3_guest_log_u64_field(line, "base_submit_ms"));
            summary.max_doorbell_submit_ms = summary
                .max_doorbell_submit_ms
                .max(qwen3_guest_log_u64_field(line, "doorbell_submit_ms"));
            summary.max_batch_submit_ms = summary
                .max_batch_submit_ms
                .max(qwen3_guest_log_u64_field(line, "max_batch_submit_ms"));
            summary.max_dispatch_ms = summary
                .max_dispatch_ms
                .max(qwen3_guest_log_u64_field(line, "dispatch_ms"));
            summary.max_doorbell_log_ms = summary
                .max_doorbell_log_ms
                .max(qwen3_guest_log_u64_field(line, "doorbell_log_ms"));
            summary.max_batch_sleep_ms = summary
                .max_batch_sleep_ms
                .max(qwen3_guest_log_u64_field(line, "batch_sleep_ms"));
            summary.max_post_batch_ms = summary
                .max_post_batch_ms
                .max(qwen3_guest_log_u64_field(line, "post_batch_ms"));
            summary.max_completion_decode_ms = summary
                .max_completion_decode_ms
                .max(qwen3_guest_log_u64_field(line, "completion_decode_ms"));
            summary.max_compute_unaccounted_ms = summary
                .max_compute_unaccounted_ms
                .max(qwen3_guest_log_u64_field(line, "compute_unaccounted_ms"));
            summary.max_publish_ms = summary
                .max_publish_ms
                .max(qwen3_guest_log_u64_field(line, "publish_ms"));
            summary.max_input_wait_ms = summary
                .max_input_wait_ms
                .max(qwen3_guest_log_u64_field(line, "input_wait_ms"));
            summary.max_unaccounted_ms = summary
                .max_unaccounted_ms
                .max(qwen3_guest_log_u64_field(line, "unaccounted_ms"));
        } else if line.contains("stage qwen3_worker_barrier_timing ") {
            summary.max_barrier_ms = summary
                .max_barrier_ms
                .max(qwen3_guest_log_u64_field(line, "barrier_ms"));
        }
    }
    summary
}

fn qwen3_guest_log_u64_field(line: &str, key: &str) -> u64 {
    let prefix = format!("{key}=");

    line.split_ascii_whitespace()
        .find_map(|field| field.strip_prefix(&prefix))
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or(0)
}

fn qwen3_guest_log_i64_field(line: &str, key: &str) -> i64 {
    let prefix = format!("{key}=");

    line.split_ascii_whitespace()
        .find_map(|field| field.strip_prefix(&prefix))
        .and_then(|value| value.parse::<i64>().ok())
        .unwrap_or(0)
}

fn qwen3_guest_log_hex_u64_field(line: &str, key: &str) -> u64 {
    let prefix = format!("{key}=");

    line.split_ascii_whitespace()
        .find_map(|field| field.strip_prefix(&prefix))
        .and_then(|value| value.strip_prefix("0x").or(Some(value)))
        .and_then(|value| u64::from_str_radix(value, 16).ok())
        .unwrap_or(0)
}

fn qwen3_guest_log_dir_from_script_output(output: &str, script_path: &Path) -> Option<PathBuf> {
    let run_id = output
        .lines()
        .find_map(|line| line.split_once("run_id=").map(|(_, run_id)| run_id.trim()))?;
    let script_dir = script_path.parent()?;
    let root_dir = script_dir.parent()?;
    Some(root_dir.join("logs").join(format!("{run_id}_headless8")))
}

fn qwen3_guest_read_log_dir(log_dir: &Path) -> anyhow::Result<String> {
    let mut out = String::new();
    if !log_dir.is_dir() {
        return Ok(out);
    }
    let mut entries = fs::read_dir(log_dir)
        .with_context(|| format!("failed to read {}", log_dir.display()))?
        .collect::<Result<Vec<_>, _>>()
        .with_context(|| format!("failed to list {}", log_dir.display()))?;
    entries.sort_by_key(|entry| entry.path());
    for entry in entries {
        let path = entry.path();
        if path
            .file_name()
            .and_then(|name| name.to_str())
            .map(|name| name.ends_with("_guest.log"))
            .unwrap_or(false)
        {
            out.push_str(
                &fs::read_to_string(&path)
                    .with_context(|| format!("failed to read guest log {}", path.display()))?,
            );
            out.push('\n');
        }
    }
    Ok(out)
}

fn run_qwen3_range_forward_cli(args: &Qwen3RangeForwardCliArgs) -> anyhow::Result<()> {
    let scenario_path = &args.scenario_path;
    let config = ScenarioConfig::from_yaml_file(scenario_path).with_context(|| {
        format!(
            "failed to load scenario config from {}",
            scenario_path.display()
        )
    })?;
    let topology = SimTopology::from_config(&config).context("failed to build topology")?;
    let report = qwen3_dense_reference_range_forward_report_with_prompt(&topology, &args.prompt)
        .map_err(anyhow::Error::msg)
        .context("failed to run Qwen3 range forward")?;
    println!("qwen3_dense_reference_range_forward");
    println!("  scenario: {}", scenario_path.display());
    println!("  prompt_bytes: {}", args.prompt.len());
    println!(
        "  ready={} nodes={} layers={} prompt_tokens={} weight_objects={} global_weight_objects={} hidden_objects={} handoff_matches={} checksum={:#x}",
        report.ready,
        report.node_count,
        report.layer_count,
        report.prompt_token_count,
        report.weight_object_count,
        report.global_weight_object_count,
        report.hidden_object_count,
        report.handoff_match_count,
        report.aggregate_checksum
    );
    for worker in &report.workers {
        println!(
            "  worker node={} layers=[{}, {}) count={} input_bytes={} output_bytes={} input_checksum={:#x} output_checksum={:#x} weight_bytes={} weight_slices={} tensors={} handoff_match={} checksum={:#x}",
            worker.node_id,
            worker.first_layer_id,
            worker.last_layer_id + 1,
            worker.layer_count,
            worker.input_payload_bytes,
            worker.output_payload_bytes,
            worker.input_payload_checksum,
            worker.output_payload_checksum,
            worker.weight_payload_bytes,
            worker.weight_payload_slice_count,
            worker.weight_reconstructed_tensor_count,
            worker.handoff_input_matches_previous_output,
            worker.aggregate_checksum
        );
    }
    if !report.ready {
        anyhow::bail!("qwen3 range forward incomplete");
    }
    Ok(())
}

fn print_qwen3_decode_verbose_steps(steps: &[sim_uapi::Qwen3DenseReferenceDecodeLoopStepReport]) {
    for step in steps {
        let real_logits_tokens = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.token_count)
            .unwrap_or(0);
        let real_logits_candidates = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.candidate_count)
            .unwrap_or(0);
        let real_logits_selection = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.selection_checksum)
            .unwrap_or(0);
        let real_logits_row_bytes = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.row_byte_count)
            .unwrap_or(0);
        let real_logits_rows = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.row_checksum)
            .unwrap_or(0);
        let real_logits_logits = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.logit_checksum)
            .unwrap_or(0);
        let real_logits_selection_matches = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.selection_match_count)
            .unwrap_or(0);
        let real_logits_margin_matches = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.margin_match_count)
            .unwrap_or(0);
        let real_logits_checksum_matches = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.checksum_match_count)
            .unwrap_or(0);
        let real_logits_comparison = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.comparison_checksum)
            .unwrap_or(0);
        let real_qkv_stage_links = step
            .text_output
            .real_qkv
            .as_ref()
            .map(|real_qkv| real_qkv.stage_link_count)
            .unwrap_or(0);
        let real_qkv_value_checksum = step
            .text_output
            .real_qkv
            .as_ref()
            .map(|real_qkv| real_qkv.real_value_checksum)
            .unwrap_or(0);
        let real_mlp_table_checksum = step
            .text_output
            .real_mlp
            .as_ref()
            .map(|real_mlp| real_mlp.table_checksum)
            .unwrap_or(0);
        let real_mlp_output_checksum = step
            .text_output
            .real_mlp
            .as_ref()
            .map(|real_mlp| real_mlp.real_output_checksum)
            .unwrap_or(0);
        let real_path_digest = if step
            .text_output
            .samples
            .iter()
            .any(|sample| sample.real_path_digest != 0)
        {
            step.text_output
                .samples
                .iter()
                .fold(0xcbf2_9ce4_8422_2325u64, |acc, sample| {
                    acc.wrapping_mul(0x0000_0100_0000_01b3)
                        ^ sample.step_index
                        ^ sample.real_path_digest.rotate_left(17)
                })
        } else {
            0
        };
        println!(
            "  step={} runtime_prefill={} input_checksum={:#x} next_input_checksum={:#x} guest_input_real={} guest_prompt_bytes={} guest_prompt_tokens={} guest_prompt_token_checksum={:#x} guest_tokenizer={:#x} transition_writes={} applied_writes={} readback_matches={} transition={:#x} sampled_tokens={} text_bytes={} text_checksum={:#x} logits_checksum={:#x} synthetic_stages={} synthetic_mask={:#x} synthetic_checksum={:#x} qkv_base_real={} attention_score_real={} attention_context_real={} mlp_activation_real={} mlp_output_real={} logits_candidates_real={} token_text_real={} token_readback={:#x} transition_slot={:#x} kv_descriptors={} kv_states={} kv_read_digest={:#x} attention_score={} attention_softmax={} attention_context={} attention={:#x} post_mlp_activation={} post_host_partial={} post_mlp_output={} post_residual={} post_next_partial={} post_attention={:#x} result_publish={} result_resolve={} result_round1_compute={} result_flow={:#x} layers={} layer_execs={} pipeline_nodes={} embedding_real={} embedding_tokens={} embedding_row_bytes={} embedding_rows={:#x} embedding_values={:#x} embedding={:#x} hidden_tensor_bytes={} hidden_tensor_carry={} hidden_tensor_all={} hidden_tensor={:#x} hidden_tensor_real_refs={} hidden_tensor_real_refs_all={} hidden_tensor_real_refs_checksum={:#x} real_qkv_layers={} real_qkv_all_layers={} real_qkv_layer_checksum={:#x} real_mlp_layers={} real_mlp_all_layers={} real_mlp_layer_checksum={:#x} real_layer_execs={} real_layer_execs_all={} real_layer_exec_checksum={:#x} node_range_count={} min_layers_per_node={} max_layers_per_node={} balanced_layers={} node_ranges={:#x} layer_transitions={} layer_boundaries={} final_layer={} final_layer_checksum={:#x} hidden_pipeline={:#x} full_layer_path_count={} full_layer_path_real={} full_layer_path={:#x} full_layer_final={:#x} layer0={:#x} layer1={:#x} logits_path={:#x} layer_progress={:#x} real_qkv_stage_links={} real_qkv_value={:#x} real_mlp_table={:#x} real_mlp_output={:#x} real_path={:#x} real_logits_tokens={} real_logits_candidates={} real_logits_row_bytes={} real_logits_rows={:#x} real_logits_logits={:#x} real_logits_selection_matches={} real_logits_margin_matches={} real_logits_checksum_matches={} real_logits_compare={:#x} real_logits_selection={:#x}",
            step.step_index,
            step.runtime_prefill_executed,
            step.guest_input_checksum,
            step.next_guest_input_checksum,
            step.text_output.synthetic.guest_input_real_backed,
            step.text_output.guest_input.prompt_byte_len,
            step.text_output.guest_input.prompt_token_count,
            step.text_output.guest_input.prompt_token_checksum,
            step.text_output.guest_input.tokenizer_asset_checksum,
            step.input_transition.write_count,
            step.input_transition.applied_write_count,
            step.input_transition.write_readback_match_count,
            step.input_transition.transition_checksum,
            step.sampled_token_count,
            step.text_output.byte_len,
            step.text_output.text_checksum,
            step.text_output.logits_checksum,
            step.text_output.synthetic.stage_count,
            step.text_output.synthetic.stage_mask,
            step.text_output.synthetic.stage_checksum,
            step.text_output.synthetic.qkv_base_tile_real_backed,
            step.text_output.synthetic.attention_score_real_backed,
            step.text_output.synthetic.attention_context_real_backed,
            step.text_output.synthetic.mlp_activation_real_backed,
            step.text_output.synthetic.mlp_output_real_backed,
            step.text_output.synthetic.logits_candidates_real_backed,
            step.text_output.synthetic.token_text_real_backed,
            step.input_transition.readback_token_checksum,
            step.input_transition.checksum_slot_value,
            step.text_output.kvcache.descriptor_count,
            step.text_output.kvcache.state_count,
            step.text_output.kvcache.read_digest_checksum,
            step.text_output.attention.score_count,
            step.text_output.attention.softmax_count,
            step.text_output.attention.context_count,
            step.text_output.attention.aggregate_checksum,
            step.text_output.post_attention.mlp_activation_count,
            step.text_output.post_attention.host_partial_count,
            step.text_output.post_attention.mlp_output_count,
            step.text_output.post_attention.residual_norm_count,
            step.text_output.post_attention.next_partial_count,
            step.text_output.post_attention.aggregate_checksum,
            step.text_output.result_flow.publish_count,
            step.text_output.result_flow.resolve_count,
            step.text_output.result_flow.round1_compute_count,
            step.text_output.result_flow.aggregate_checksum,
            step.hidden_layer_pipeline.layer_count,
            step.hidden_layer_pipeline.layer_executions.len(),
            step.hidden_layer_pipeline.node_count,
            step.hidden_layer_pipeline.input_embedding_real_backed,
            step.hidden_layer_pipeline.input_embedding_token_count,
            step.hidden_layer_pipeline.input_embedding_row_byte_count,
            step.hidden_layer_pipeline.input_embedding_row_checksum,
            step.hidden_layer_pipeline.input_embedding_value_checksum,
            step.hidden_layer_pipeline.input_embedding_checksum,
            step.hidden_layer_pipeline.hidden_tensor_byte_count,
            step.hidden_layer_pipeline.hidden_tensor_carry_count,
            step.hidden_layer_pipeline.hidden_tensor_carry_all_present,
            step.hidden_layer_pipeline.hidden_tensor_carry_checksum,
            step.hidden_layer_pipeline.hidden_tensor_real_reference_count,
            step.hidden_layer_pipeline
                .hidden_tensor_real_references_all_present,
            step.hidden_layer_pipeline.hidden_tensor_real_reference_checksum,
            step.hidden_layer_pipeline.real_qkv_layer_count,
            step.hidden_layer_pipeline.real_qkv_all_layers_present,
            step.hidden_layer_pipeline.real_qkv_layer_checksum,
            step.hidden_layer_pipeline.real_mlp_layer_count,
            step.hidden_layer_pipeline.real_mlp_all_layers_present,
            step.hidden_layer_pipeline.real_mlp_layer_checksum,
            step.hidden_layer_pipeline.real_layer_execution_count,
            step.hidden_layer_pipeline.real_layer_executions_all_present,
            step.hidden_layer_pipeline.real_layer_execution_checksum,
            step.hidden_layer_pipeline.node_ranges.len(),
            step.hidden_layer_pipeline.min_layers_per_node,
            step.hidden_layer_pipeline.max_layers_per_node,
            step.hidden_layer_pipeline.balanced_layer_spread,
            step.hidden_layer_pipeline.node_range_checksum,
            step.hidden_layer_pipeline.transition_count,
            step.hidden_layer_pipeline.boundary_count,
            step.hidden_layer_pipeline.last_layer_id,
            step.hidden_layer_pipeline.final_layer_checksum,
            step.hidden_layer_pipeline.aggregate_checksum,
            step.layer_progress.full_layer_path_count,
            step.layer_progress.full_layer_path_real_backed,
            step.layer_progress.full_layer_path_checksum,
            step.layer_progress.full_layer_final_checksum,
            step.layer_progress.layer0_path_checksum,
            step.layer_progress.layer1_path_checksum,
            step.layer_progress.logits_path_checksum,
            step.layer_progress.aggregate_checksum,
            real_qkv_stage_links,
            real_qkv_value_checksum,
            real_mlp_table_checksum,
            real_mlp_output_checksum,
            real_path_digest,
            real_logits_tokens,
            real_logits_candidates,
            real_logits_row_bytes,
            real_logits_rows,
            real_logits_logits,
            real_logits_selection_matches,
            real_logits_margin_matches,
            real_logits_checksum_matches,
            real_logits_comparison,
            real_logits_selection
        );
        println!(
            "  real_inference_contract step={} ready={} blockers={} checksum={:#x} synthetic_stages={} synthetic_mask={:#x} candidate_logits_only={} deterministic_hidden={} embedding_hidden_proxy={} round1_hidden={} full_forward_math={} full_vocab_logits={} sampled_text_reference_checked={} blocker_list={}",
            step.step_index,
            step.real_inference_contract.ready,
            step.real_inference_contract.blocker_count,
            step.real_inference_contract.aggregate_checksum,
            step.real_inference_contract.synthetic_stage_count,
            step.real_inference_contract.synthetic_stage_mask,
            step.real_inference_contract.uses_candidate_logits_only,
            step.real_inference_contract.uses_deterministic_hidden,
            step.real_inference_contract.uses_embedding_hidden_as_final_hidden,
            step.real_inference_contract.uses_round1_output_hidden_for_logits,
            step.real_inference_contract.full_forward_math,
            step.real_inference_contract.full_vocab_logits,
            step.real_inference_contract.sampled_text_reference_checked,
            step.real_inference_contract.blockers.join(",")
        );
        println!(
            "  object_service step={} ready={} publish={} resolve={} append={} kv_resolve={} kv_append={} metadata_put={} metadata_get={} shmem_write={} shmem_read={} block_write={} block_read={} inline_write={} inline_read={} obmm_pool={} obmm_write={} obmm_read={} obmm_queue_submit={} obmm_queue_deliver={} obmm_bytes={} committed={} missing_resolve={} token_objects={} kv_objects={} weight_objects={} weight_payload_bytes={} weight_payload_slices={} weight_payload_complete={} weight_reconstructed_tensors={} weight_reconstructed_checksum={:#x} weight_payload_checksum={:#x} global_weight_objects={} global_weight_payload_bytes={} global_weight_tensors={} global_weight_checksum={:#x} runtime_tensor_objects={} logits_objects={} checksum={:#x}",
            step.step_index,
            step.object_service.ready,
            step.object_service.publish_count,
            step.object_service.resolve_count,
            step.object_service.append_count,
            step.object_service.kv_index_resolve_count,
            step.object_service.kv_index_append_count,
            step.object_service.metadata_put_count,
            step.object_service.metadata_get_count,
            step.object_service.shmem_write_count,
            step.object_service.shmem_read_count,
            step.object_service.block_write_count,
            step.object_service.block_read_count,
            step.object_service.inline_write_count,
            step.object_service.inline_read_count,
            step.object_service.obmm_pool_enabled,
            step.object_service.obmm_pool_payload_write_count,
            step.object_service.obmm_pool_payload_read_count,
            step.object_service.obmm_pool_queue_submit_count,
            step.object_service.obmm_pool_queue_deliver_count,
            step.object_service.obmm_pool_bytes_used,
            step.object_service.committed_object_count,
            step.object_service.missing_resolve_count,
            step.object_service.token_objects,
            step.object_service.kv_objects,
            step.object_service.weight_objects,
            step.object_service.weight_payload_bytes,
            step.object_service.weight_payload_slice_count,
            step.object_service.weight_payload_complete,
            step.object_service.weight_reconstructed_tensor_count,
            step.object_service.weight_reconstructed_tensor_checksum,
            step.object_service.weight_payload_checksum,
            step.object_service.global_weight_object_count,
            step.object_service.global_weight_payload_bytes,
            step.object_service.global_weight_tensor_count,
            step.object_service.global_weight_payload_checksum,
            step.object_service.runtime_tensor_objects,
            step.object_service.logits_objects,
            step.object_service.object_checksum
        );
    }
}

fn prepare_qwen3_decode_loop_environment(args: &Qwen3DecodeLoopCliArgs) -> anyhow::Result<()> {
    if env::var_os("SIM_QWEN3_DENSE_WEIGHTS_PATH").is_none() {
        if let Some(weights_path) =
            qwen3_dense_weights_path_from_env(None, env::var_os("SIM_QWEN3_0_6B_WEIGHTS_PATH"))
        {
            env::set_var("SIM_QWEN3_DENSE_WEIGHTS_PATH", weights_path);
        }
    }

    let scenario_env_path = args
        .scenario_path
        .canonicalize()
        .unwrap_or_else(|_| args.scenario_path.clone());
    std::env::set_var("SIM_UAPI_SCENARIO_CONFIG", &scenario_env_path);

    let Some(matmul_batch) = args.matmul_batch else {
        return Ok(());
    };
    prepare_qwen3_matmul_batch_environment(matmul_batch)
}

fn qwen3_dense_weights_path_from_env(
    dense: Option<std::ffi::OsString>,
    legacy_0_6b: Option<std::ffi::OsString>,
) -> Option<std::ffi::OsString> {
    dense.or(legacy_0_6b)
}

fn prepare_qwen3_matmul_batch_environment(matmul_batch: usize) -> anyhow::Result<()> {
    std::env::set_var("SIM_QWEN3_ROUND1_DISPATCH_BATCH", matmul_batch.to_string());
    if matmul_batch == 1 {
        return Ok(());
    }

    let base_manifest = std::env::var_os("SIMPLER_HOST_MATMUL_MANIFEST")
        .map(PathBuf::from)
        .unwrap_or_else(default_simpler_host_matmul_manifest_path);
    ensure_simpler_host_matmul_manifest(&base_manifest, None)?;
    std::env::set_var("SIMPLER_HOST_MATMUL_MANIFEST", &base_manifest);

    let batch_manifest = std::env::var_os("SIMPLER_HOST_MATMUL_BATCH_MANIFEST")
        .map(PathBuf::from)
        .unwrap_or_else(|| default_simpler_host_matmul_batch_manifest_path(matmul_batch));
    ensure_simpler_host_matmul_manifest(&batch_manifest, Some((matmul_batch, &base_manifest)))?;
    std::env::set_var("SIMPLER_HOST_MATMUL_BATCH_MANIFEST", &batch_manifest);
    Ok(())
}

fn ensure_simpler_host_matmul_manifest(
    manifest_path: &Path,
    batch: Option<(usize, &Path)>,
) -> anyhow::Result<()> {
    ensure_simpler_host_matmul_manifest_for_platform(manifest_path, batch, None)
}

fn ensure_simpler_host_matmul_manifest_for_platform(
    manifest_path: &Path,
    batch: Option<(usize, &Path)>,
    platform: Option<&str>,
) -> anyhow::Result<()> {
    if manifest_path.exists() {
        if let Some((tile_batch, base_manifest)) = batch {
            if simpler_host_matmul_batch_manifest_is_current(
                manifest_path,
                tile_batch,
                base_manifest,
            )? {
                return Ok(());
            }
        } else {
            return Ok(());
        }
    }
    let output_dir = manifest_path
        .parent()
        .ok_or_else(|| anyhow::anyhow!("manifest has no parent: {}", manifest_path.display()))?;
    let script = simpler_host_matmul_artifact_producer_path();
    if !script.exists() {
        anyhow::bail!(
            "missing simpler host matmul artifact producer: {}",
            script.display()
        );
    }

    let mut command = Command::new("python3");
    command.arg(&script).arg("--output-dir").arg(output_dir);
    if let Some(simpler_root) = simpler_runtime_root_for_host_artifacts() {
        command.arg("--simpler-root").arg(simpler_root);
    }
    if let Some(platform) = platform {
        command.arg("--platform").arg(platform);
    }
    if let Some((tile_batch, base_manifest)) = batch {
        command
            .arg("--tile-batch")
            .arg(tile_batch.to_string())
            .arg("--reuse-runtime-manifest")
            .arg(base_manifest);
    }
    let status = command
        .status()
        .with_context(|| format!("failed to run {}", script.display()))?;
    if !status.success() {
        anyhow::bail!(
            "simpler host matmul artifact producer failed: {} status={status}",
            script.display()
        );
    }
    if !manifest_path.exists() {
        anyhow::bail!(
            "simpler host matmul artifact producer did not create {}",
            manifest_path.display()
        );
    }
    Ok(())
}

fn simpler_host_matmul_artifact_producer_path() -> PathBuf {
    let root = repo_root();
    let candidates = [
        root.join("scripts")
            .join("prepare_simpler_host_matmul_artifacts.py"),
        root.join("guest-linux")
            .join("aarch64")
            .join("scripts")
            .join("prepare_simpler_host_matmul_artifacts.py"),
    ];
    candidates
        .iter()
        .find(|path| path.exists())
        .cloned()
        .unwrap_or_else(|| candidates[0].clone())
}

fn simpler_runtime_root_for_host_artifacts() -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(path) = std::env::var_os("SIMPLER_ROOT") {
        candidates.push(PathBuf::from(path));
    }
    if let Some(workspace_root) = repo_root().parent() {
        candidates.push(workspace_root.join("pypto").join("runtime"));
        candidates.push(workspace_root.join("modules").join("simpler"));
    }
    candidates.push(repo_root().join("vendor").join("simpler"));

    candidates.into_iter().find(|path| {
        path.join("tests")
            .join("st")
            .join("a2a3")
            .join("host_build_graph")
            .join("matmul")
            .exists()
            || path
                .join("examples")
                .join("a2a3")
                .join("host_build_graph")
                .join("matmul")
                .exists()
    })
}

fn simpler_host_matmul_batch_manifest_is_current(
    manifest_path: &Path,
    tile_batch: usize,
    base_manifest: &Path,
) -> anyhow::Result<bool> {
    let manifest_text = fs::read_to_string(manifest_path)
        .with_context(|| format!("failed to read {}", manifest_path.display()))?;
    let base_text = fs::read_to_string(base_manifest)
        .with_context(|| format!("failed to read {}", base_manifest.display()))?;
    let manifest: serde_json::Value = serde_json::from_str(&manifest_text)
        .with_context(|| format!("failed to parse {}", manifest_path.display()))?;
    let base: serde_json::Value = serde_json::from_str(&base_text)
        .with_context(|| format!("failed to parse {}", base_manifest.display()))?;
    let runtime = &manifest["simpler_runtime"];
    let base_runtime = &base["simpler_runtime"];
    let actual_tile_batch = runtime["tile_batch"].as_u64();
    let actual_runtime_env = &runtime["runtime_env"];
    let base_runtime_env = &base_runtime["runtime_env"];
    Ok(actual_tile_batch == Some(tile_batch as u64) && actual_runtime_env == base_runtime_env)
}

fn default_simpler_host_matmul_manifest_path() -> PathBuf {
    Path::new("/tmp")
        .join("simpler-host-matmul-artifacts")
        .join("host_matmul_manifest.json")
}

fn default_simpler_qwen3_runtime_manifest_path(runtime_name: &str, platform: &str) -> PathBuf {
    let runtime_name = runtime_name.replace('_', "-");
    Path::new("/tmp")
        .join(format!(
            "simpler-qwen3-{runtime_name}-{platform}-runtime-artifacts"
        ))
        .join("simpler_runtime_manifest.json")
}

fn ensure_simpler_qwen3_runtime_manifest(
    manifest_path: &Path,
    runtime_name: &str,
    platform: &str,
) -> anyhow::Result<()> {
    if manifest_path.exists() {
        return Ok(());
    }
    let output_dir = manifest_path
        .parent()
        .ok_or_else(|| anyhow::anyhow!("manifest has no parent: {}", manifest_path.display()))?;
    let script = simpler_runtime_artifact_producer_path();
    if !script.exists() {
        anyhow::bail!(
            "missing simpler runtime artifact producer: {}",
            script.display()
        );
    }
    let mut command = Command::new("python3");
    command
        .arg(&script)
        .arg("--output-dir")
        .arg(output_dir)
        .arg("--runtime-name")
        .arg(runtime_name)
        .arg("--platform")
        .arg(platform)
        .arg("--aicpu-thread-num")
        .arg("4");
    if let Some(simpler_root) = simpler_runtime_root_for_host_artifacts() {
        command.arg("--simpler-root").arg(simpler_root);
    }
    let status = command
        .stdout(Stdio::null())
        .status()
        .with_context(|| format!("failed to run {}", script.display()))?;
    if !status.success() {
        anyhow::bail!(
            "simpler runtime artifact producer failed: {} status={status}",
            script.display()
        );
    }
    if !manifest_path.exists() {
        anyhow::bail!(
            "simpler runtime artifact producer did not create {}",
            manifest_path.display()
        );
    }
    Ok(())
}

fn simpler_runtime_artifact_producer_path() -> PathBuf {
    repo_root()
        .join("guest-linux")
        .join("aarch64")
        .join("scripts")
        .join("prepare_simpler_runtime_artifacts.py")
}

fn default_simpler_host_matmul_batch_manifest_path(tile_batch: usize) -> PathBuf {
    Path::new("/tmp")
        .join(format!(
            "simpler-host-matmul-batch{tile_batch}-reuse-runtime-artifacts"
        ))
        .join("host_matmul_manifest.json")
}

fn repo_root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn configure_simpler_dispatch_logging() {
    if simpler_dispatch_log_enabled() {
        return;
    }
    if std::env::var("PTO_LOG_LEVEL").is_err() {
        std::env::set_var("PTO_LOG_LEVEL", "off");
    }
    if std::env::var("PTO_LOG_FILE").is_err() {
        std::env::set_var("PTO_LOG_FILE", "/dev/null");
    }
}

fn simpler_dispatch_log_enabled() -> bool {
    std::env::var("SIM_SIMPLER_DISPATCH_LOG")
        .map(|value| {
            matches!(
                value.as_str(),
                "1" | "true" | "TRUE" | "yes" | "YES" | "on" | "ON"
            )
        })
        .unwrap_or(false)
}

fn run_qwen3_text_output_cli(scenario_path: &Path) -> anyhow::Result<()> {
    let config = ScenarioConfig::from_yaml_file(scenario_path).with_context(|| {
        format!(
            "failed to load scenario config from {}",
            scenario_path.display()
        )
    })?;
    let topology = SimTopology::from_config(&config).context("failed to build topology")?;
    let guest_input = qwen3_dense_reference_default_guest_input();
    let report = qwen3_dense_reference_prefill_text_output_report(&topology, &guest_input)
        .map_err(anyhow::Error::msg)
        .context("failed to run Qwen3 text output prefill")?;
    println!("qwen3_dense_reference_text_output");
    println!("  scenario: {}", scenario_path.display());
    println!("  tokens: {}", report.token_count);
    println!("  bytes: {}", report.byte_len);
    println!("  padded_bytes: {}", report.padded_byte_len);
    println!("  byte_checksum: {:#x}", report.byte_checksum);
    println!("  sequence_checksum: {:#x}", report.sequence_checksum);
    println!("  token_checksum: {:#x}", report.token_checksum);
    println!("  text_checksum: {:#x}", report.text_checksum);
    println!("  logits_checksum: {:#x}", report.logits_checksum);
    println!("  tokenizer_policy_kind: {}", report.tokenizer_policy_kind);
    println!(
        "  synthetic: stages={} mask={:#x} checksum={:#x} guest_input_real={} qkv_base_real={} attention_score_real={} attention_context_real={} mlp_activation_real={} mlp_output_real={} logits_candidates_real={} token_text_real={}",
        report.synthetic.stage_count,
        report.synthetic.stage_mask,
        report.synthetic.stage_checksum,
        report.synthetic.guest_input_real_backed,
        report.synthetic.qkv_base_tile_real_backed,
        report.synthetic.attention_score_real_backed,
        report.synthetic.attention_context_real_backed,
        report.synthetic.mlp_activation_real_backed,
        report.synthetic.mlp_output_real_backed,
        report.synthetic.logits_candidates_real_backed,
        report.synthetic.token_text_real_backed
    );
    println!(
        "  kvcache: descriptors={} states={} append_blocks={} update_seq_sum={} prefill_entries={} decode_entries={} read_window_end_max={} read_digest={:#x}",
        report.kvcache.descriptor_count,
        report.kvcache.state_count,
        report.kvcache.append_block_count,
        report.kvcache.update_seq_sum,
        report.kvcache.prefill_entry_count,
        report.kvcache.decode_entry_count,
        report.kvcache.read_window_end_max,
        report.kvcache.read_digest_checksum
    );
    println!(
        "  attention: score={} softmax={} context={} stage_mask={:#x} score_checksum={:#x} softmax_checksum={:#x} context_checksum={:#x} aggregate={:#x}",
        report.attention.score_count,
        report.attention.softmax_count,
        report.attention.context_count,
        report.attention.stage_mask,
        report.attention.score_checksum,
        report.attention.softmax_checksum,
        report.attention.context_checksum,
        report.attention.aggregate_checksum
    );
    println!(
        "  post_attention: mlp_activation={} host_partial={} mlp_output={} residual_norm={} next_partial={} stage_mask={:#x} activation_checksum={:#x} host_partial_checksum={:#x} mlp_output_checksum={:#x} residual_norm_checksum={:#x} next_partial_checksum={:#x} aggregate={:#x}",
        report.post_attention.mlp_activation_count,
        report.post_attention.host_partial_count,
        report.post_attention.mlp_output_count,
        report.post_attention.residual_norm_count,
        report.post_attention.next_partial_count,
        report.post_attention.stage_mask,
        report.post_attention.mlp_activation_checksum,
        report.post_attention.host_partial_checksum,
        report.post_attention.mlp_output_checksum,
        report.post_attention.residual_norm_checksum,
        report.post_attention.next_partial_checksum,
        report.post_attention.aggregate_checksum
    );
    println!(
        "  result_flow: publish={} resolve={} round1_compute={} result_count={} round0_distinct={} round1_distinct={} round0_checksum={:#x} round1_checksum={:#x} aggregate={:#x}",
        report.result_flow.publish_count,
        report.result_flow.resolve_count,
        report.result_flow.round1_compute_count,
        report.result_flow.result_count,
        report.result_flow.round0_distinct_count,
        report.result_flow.round1_distinct_count,
        report.result_flow.round0_checksum,
        report.result_flow.round1_checksum,
        report.result_flow.aggregate_checksum
    );
    match &report.real_qkv {
        Some(real_qkv) => println!(
            "  real_qkv: layer={} reference_layers={} shards={} stage_links={} stage_mask={:#x} weight_bytes={} qkv_rows={} aggregate={:#x} stage_checksum={:#x} layer0_stage={:#x} layer1_stage={:#x} synthetic={:#x} real_weight={:#x} real_value={:#x} real_output={:#x}",
            real_qkv.layer_id,
            real_qkv.reference_layer_count,
            real_qkv.shard_count,
            real_qkv.stage_link_count,
            real_qkv.stage_kind_mask,
            real_qkv.total_weight_bytes,
            real_qkv.qkv_rows,
            real_qkv.aggregate_checksum,
            real_qkv.stage_link_checksum,
            real_qkv.reference_layer_checksum,
            real_qkv.next_reference_layer_checksum,
            real_qkv.synthetic_checksum,
            real_qkv.real_weight_checksum,
            real_qkv.real_value_checksum,
            real_qkv.real_output_checksum
        ),
        None => println!("  real_qkv: unavailable"),
    }
    match &report.real_mlp {
        Some(real_mlp) => println!(
            "  real_mlp: layer={} next_layer={} shards={} next_shards={} weight_bytes={} next_weight_bytes={} intermediate_rows={} next_intermediate_rows={} aggregate={:#x} next_aggregate={:#x} real_weight={:#x} activation={:#x} output={:#x} next_output={:#x} samples={:#x} table={:#x}",
            real_mlp.layer_id,
            real_mlp.next_layer_id,
            real_mlp.shard_count,
            real_mlp.next_shard_count,
            real_mlp.total_weight_bytes,
            real_mlp.next_total_weight_bytes,
            real_mlp.total_intermediate_rows,
            real_mlp.next_total_intermediate_rows,
            real_mlp.aggregate_checksum,
            real_mlp.next_aggregate_checksum,
            real_mlp.real_weight_checksum,
            real_mlp.real_activation_checksum,
            real_mlp.real_output_checksum,
            real_mlp.next_real_output_checksum,
            real_mlp.sample_checksum,
            real_mlp.table_checksum
        ),
        None => println!("  real_mlp: unavailable"),
    }
    match &report.real_logits {
        Some(real_logits) => println!(
            "  real_logits: tokens={} candidates={} distinct_steps={} distinct_tokens={} row_bytes={} row_checksum={:#x} logit_checksum={:#x} sampled_pairs={} selection_matches={} margin_matches={} checksum_matches={} max_margin_delta={} vocab={} hidden={} aggregate={:#x} final_norm={:#x} top_bits={:#x} runner_bits={:#x} compare={:#x} selection={:#x}",
            real_logits.token_count,
            real_logits.candidate_count,
            real_logits.distinct_step_count,
            real_logits.distinct_token_count,
            real_logits.row_byte_count,
            real_logits.row_checksum,
            real_logits.logit_checksum,
            real_logits.sampled_pair_count,
            real_logits.selection_match_count,
            real_logits.margin_match_count,
            real_logits.checksum_match_count,
            real_logits.max_margin_delta_milli,
            real_logits.vocab_size,
            real_logits.hidden_size,
            real_logits.aggregate_checksum,
            real_logits.final_norm_checksum,
            real_logits.top_logit_bits_checksum,
            real_logits.runner_logit_bits_checksum,
            real_logits.comparison_checksum,
            real_logits.selection_checksum
        ),
        None => println!("  real_logits: unavailable"),
    }
    println!("  text_lossy: {}", report.text_lossy.escape_debug());
    println!("  samples:");
    for sample in &report.samples {
        println!(
            "    step={} shard={} tile={} token={} runner_up={} margin_milli={} logits_checksum={:#x} kv_read_digest={:#x} qkv_digest={:#x} real_path={:#x} text_checksum={:#x} offset={} bytes={} flags={:#x} piece={}",
            sample.step_index,
            sample.shard_id,
            sample.tile_id,
            sample.sampled_token,
            sample.runner_up_token,
            sample.margin_milli,
            sample.logits_checksum,
            sample.kvcache_read_digest,
            sample.qkv_reference_digest,
            sample.real_path_digest,
            sample.text_checksum,
            sample.text_byte_offset,
            sample.byte_len,
            sample.boundary_flags,
            sample.piece_lossy.escape_debug()
        );
    }
    Ok(())
}

fn host_vector_manifest_from_args() -> Option<PathBuf> {
    let mut args = env::args_os().skip(1);
    match args.next() {
        Some(mode) if mode == "host-vector" => args.next().map(PathBuf::from),
        _ => None,
    }
}

fn run_host_vector_cli(manifest_path: &Path) -> anyhow::Result<()> {
    let scenario_path = default_scenario_path();
    let yaml = std::fs::read_to_string(&scenario_path).with_context(|| {
        format!(
            "failed to read scenario config from {}",
            scenario_path.display()
        )
    })?;
    let yaml = yaml.replace("chip_backend_mode: stub", "chip_backend_mode: simpler_capi");
    let config = ScenarioConfig::from_yaml_str(&yaml)
        .context("failed to parse host-vector scenario config")?;
    let topology = SimTopology::from_config(&config).context("failed to build topology")?;
    let report = run_host_vector_dispatch(&config, &topology, manifest_path, 16_384)
        .context("failed to run host_vector via simpler_capi")?;
    println!("host_vector_simpler_capi");
    println!("  manifest: {}", manifest_path.display());
    println!("  elems: {}", report.elems);
    println!("  completion: {:?}", report.completion_status);
    println!("  all_match_expected: {}", report.all_match_expected);
    println!("  first_values: {:?}", report.first_values);
    if !report.all_match_expected || report.completion_status != CompletionStatus::Success {
        anyhow::bail!("host_vector_simpler_capi_failed");
    }
    Ok(())
}

fn scenario_path_from_args() -> PathBuf {
    env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(default_scenario_path)
}

fn default_scenario_path() -> PathBuf {
    Path::new("scenarios").join("mvp_2host_single_domain.yaml")
}

fn include_auxiliary_debug() -> bool {
    matches!(env::var("SIM_CLI_INCLUDE_AUX").as_deref(), Ok("1"))
}

fn run_demo(config: &ScenarioConfig, topology: &SimTopology) -> anyhow::Result<Vec<SimEvent>> {
    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let block = BlockHash("demo-block-0".to_string());

    let planner = RecursiveRoutePlanner::from_config(config);
    let mut store = InMemoryBlockStore::from_config(config);
    let mut sink = VecEventSink::default();

    sink.emit(SimEvent::TaskCreated {
        at: 0,
        task: task.clone(),
    });

    let decision = planner
        .plan(
            RouteRequest {
                task: task.clone(),
                current_level: PlLevel::L4,
                block: block.clone(),
            },
            topology,
        )
        .context("route planning failed")?;

    sink.emit(SimEvent::RoutePlanned {
        at: 1,
        task: task.clone(),
        decision: decision.clone(),
    });

    store
        .stage_insert(PromotionPlan {
            block: block.clone(),
        })
        .context("block insert failed")?;

    let placement = store
        .lookup(&block)
        .placement
        .context("block placement missing after insert")?;

    sink.emit(SimEvent::BlockPromoted {
        at: 2,
        block: block.clone(),
        placement,
    });

    let evicted = store
        .evict(EvictionPlan { max_blocks: 1 })
        .context("block eviction failed")?;

    if let Some(evicted_block) = evicted.into_iter().next() {
        sink.emit(SimEvent::BlockEvicted {
            at: 3,
            block: evicted_block,
            from: sim_core::BlockPlacement {
                block: block.clone(),
                level: PlLevel::L2,
                node: 0,
            },
        });
    }

    let mut success_runtime = LocalRuntimeEngine::from_config(config);
    success_runtime
        .submit_dispatch(
            DispatchRequest {
                task: task.clone(),
                function: FunctionLabel {
                    name: "runtime_demo_dispatch".into(),
                    level: PlLevel::L4,
                },
                backend_spec: None,
                request: None,
                target_level: PlLevel::L4,
                target_node: decision.selected_node,
                input_segments: vec![SegmentHandle(1)],
            },
            &mut sink,
        )
        .context("runtime dispatch submit failed")?;
    success_runtime
        .submit_copy(CopyRequest {
            task: task.clone(),
            direction: CopyDirection::HostToDevice,
            bytes: 4096,
            src: MemoryEndpoint {
                node: topology.hosts[0].node_id,
                segment: SegmentHandle(1),
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: topology.ubpus[0].node_id,
                segment: SegmentHandle(2),
                offset: 0,
            },
        })
        .context("runtime copy submit failed")?;
    let _ = success_runtime.poll_completions(30, &mut sink);

    let mut retry_runtime = LocalRuntimeEngine::with_policy(15, 30, 5, 4, 1);
    retry_runtime
        .submit_dispatch(
            DispatchRequest {
                task: task.clone(),
                function: FunctionLabel {
                    name: "runtime_demo_timeout".into(),
                    level: PlLevel::L4,
                },
                backend_spec: None,
                request: None,
                target_level: PlLevel::L4,
                target_node: decision.selected_node,
                input_segments: vec![SegmentHandle(3)],
            },
            &mut sink,
        )
        .context("retry runtime dispatch submit failed")?;
    retry_runtime.advance_to(5, &mut sink);
    let _ = retry_runtime.poll_completions(10, &mut sink);

    Ok(sink.into_events())
}

fn run_uapi_demo(topology: &SimTopology) -> anyhow::Result<UapiDemoReport> {
    let mut surface = LocalGuestUapiSurface::new(topology.clone());
    let topo = match surface
        .execute(UapiCommand::QueryTopology)
        .context("query topology failed")?
    {
        UapiResponse::TopologySnapshot(snapshot) => snapshot,
        response => anyhow::bail!("unexpected topology response: {response:?}"),
    };
    let mut events = Vec::new();

    let cq = match surface
        .execute(UapiCommand::RegisterCq { owner: 0 })
        .context("register cq failed")?
    {
        UapiResponse::CqRegistered(cq) => cq,
        response => anyhow::bail!("unexpected cq response: {response:?}"),
    };
    let cmdq = match surface
        .execute(UapiCommand::CreateCmdQueue {
            cq,
            owner: 0,
            depth: 16,
        })
        .context("create cmdq failed")?
    {
        UapiResponse::CmdQueueCreated(cmdq) => cmdq,
        response => anyhow::bail!("unexpected cmdq response: {response:?}"),
    };
    let cmdq_depth = 16usize;

    let segment = match surface
        .execute(UapiCommand::CreateSegment { bytes: 4096 })
        .context("create segment failed")?
    {
        UapiResponse::SegmentCreated(segment) => segment,
        response => anyhow::bail!("unexpected segment response: {response:?}"),
    };

    let write_op = match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::Io(IoSubmitReq {
                op_id: 100,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: Some(segment),
                block: Some(BlockHash("uapi-block-0".to_string())),
            }),
        })
        .context("enqueue write failed")?
    {
        UapiResponse::CommandEnqueued { depth, .. } => depth as u64,
        response => anyhow::bail!("unexpected write response: {response:?}"),
    };
    match surface
        .execute(UapiCommand::RingDoorbell {
            cmdq,
            owner: 0,
            max_batch: Some(1),
        })
        .context("ring write doorbell failed")?
    {
        UapiResponse::DoorbellRung { submitted: 1, .. } => {}
        response => anyhow::bail!("unexpected write doorbell response: {response:?}"),
    }

    let health = match surface
        .execute(UapiCommand::GetHealth { entity: 0 })
        .context("get health failed")?
    {
        UapiResponse::HealthStatus(health) => health,
        response => anyhow::bail!("unexpected health response: {response:?}"),
    };

    let write_completions = match surface
        .execute(UapiCommand::DrainCq { cq, owner: 0 })
        .context("drain write cq failed")?
    {
        UapiResponse::Completions { events, .. } => events,
        response => anyhow::bail!("unexpected drain response: {response:?}"),
    };
    for completion in write_completions {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    let read_op = match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::Io(IoSubmitReq {
                op_id: 101,
                task: None,
                entity: 0,
                opcode: IoOpcode::ReadBlock,
                segment: Some(segment),
                block: Some(BlockHash("uapi-block-0".to_string())),
            }),
        })
        .context("enqueue read failed")?
    {
        UapiResponse::CommandEnqueued { depth, .. } => depth as u64,
        response => anyhow::bail!("unexpected read response: {response:?}"),
    };
    match surface
        .execute(UapiCommand::RingDoorbell {
            cmdq,
            owner: 0,
            max_batch: Some(1),
        })
        .context("ring read doorbell failed")?
    {
        UapiResponse::DoorbellRung { submitted: 1, .. } => {}
        response => anyhow::bail!("unexpected read doorbell response: {response:?}"),
    }

    let read_completions = match surface
        .execute(UapiCommand::DrainCq { cq, owner: 0 })
        .context("drain read cq failed")?
    {
        UapiResponse::Completions { events, .. } => events,
        response => anyhow::bail!("unexpected drain response: {response:?}"),
    };
    for completion in read_completions {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::ShmemPut(ShmemPutReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 4096,
            }),
        })
        .context("enqueue shmem put failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected shmem put response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::ShmemGet(ShmemGetReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 4096,
            }),
        })
        .context("enqueue shmem get failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected shmem get response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::DfsWrite(DfsWriteReq {
                task: None,
                path: "/weights/uapi-demo.bin".into(),
                bytes: 4096,
            }),
        })
        .context("enqueue dfs write failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected dfs write response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::DfsRead(DfsReadReq {
                task: None,
                path: "/weights/uapi-demo.bin".into(),
            }),
        })
        .context("enqueue dfs read failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected dfs read response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::DbPut(DbPutReq {
                task: None,
                key: "uapi:kv:weights".into(),
                bytes: 128,
            }),
        })
        .context("enqueue db put failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected db put response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::DbGet(DbGetReq {
                task: None,
                key: "uapi:kv:weights".into(),
            }),
        })
        .context("enqueue db get failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected db get response: {response:?}"),
    };
    let cmdq_pending_after_partial_ring = match surface
        .execute(UapiCommand::RingDoorbell {
            cmdq,
            owner: 0,
            max_batch: Some(3),
        })
        .context("ring partial service doorbell failed")?
    {
        UapiResponse::DoorbellRung {
            submitted: 3,
            pending,
        } => pending,
        response => anyhow::bail!("unexpected partial service doorbell response: {response:?}"),
    };

    let cq_remaining_after_partial_poll = match surface
        .execute(UapiCommand::PollCq {
            cq,
            owner: 0,
            max_entries: Some(2),
        })
        .context("poll partial service cq failed")?
    {
        UapiResponse::Completions {
            events: partial_events,
            remaining,
        } => {
            for completion in partial_events {
                events.push(SimEvent::CompletionObserved {
                    at: completion.finished_at,
                    completion,
                });
            }
            remaining
        }
        response => anyhow::bail!("unexpected partial poll response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::RingDoorbell {
            cmdq,
            owner: 0,
            max_batch: None,
        })
        .context("ring service doorbell failed")?
    {
        UapiResponse::DoorbellRung {
            submitted: 3,
            pending: 0,
        } => {}
        response => anyhow::bail!("unexpected service doorbell response: {response:?}"),
    }

    let service_completions = match surface
        .execute(UapiCommand::DrainCq { cq, owner: 0 })
        .context("drain service cq failed")?
    {
        UapiResponse::Completions { events, .. } => events,
        response => anyhow::bail!("unexpected drain response: {response:?}"),
    };
    for completion in service_completions {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    let _ = write_op;
    let _ = read_op;

    Ok(UapiDemoReport {
        hosts_count: topo.hosts,
        ubpus_count: topo.ubpus,
        entities_count: topo.entities,
        domains_count: topo.domains,
        cq,
        cmdq,
        cmdq_depth,
        segment,
        health,
        cmdq_pending_after_partial_ring,
        cq_remaining_after_partial_poll,
        summary: summarize_events(&events),
        events,
    })
}

fn run_qemu_backend_demo(topology: &SimTopology) -> anyhow::Result<QemuBackendDemoReport> {
    let mut handler = QemuMmioHandler::new(LinquDeviceModel::new(topology.clone()));
    let mmio = handler.device().mmio();
    let topo = handler
        .device_mut()
        .query_topology()
        .context("qemu backend query topology failed")?;
    let (endpoint, layout) = handler
        .device_mut()
        .realize_endpoint(0)
        .context("realize endpoint failed")?;
    let segment = handler
        .device_mut()
        .create_segment(endpoint, 4096)
        .context("create qemu backend segment failed")?;
    let mut events = Vec::new();

    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            0,
            GuestDescriptor::Io(GuestIoDescriptor {
                op_id: 300,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: Some(segment),
                block: Some(BlockHash("qemu-device-block-0".into())),
            }),
        )
        .context("write cmd descriptor 0 failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            1,
            GuestDescriptor::Io(GuestIoDescriptor {
                op_id: 301,
                task: None,
                entity: 0,
                opcode: IoOpcode::ReadBlock,
                segment: Some(segment),
                block: Some(BlockHash("qemu-device-block-0".into())),
            }),
        )
        .context("write cmd descriptor 1 failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            2,
            GuestDescriptor::Service(GuestServiceDescriptor::ShmemPut(ShmemPutReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 4096,
            })),
        )
        .context("write qemu shmem put descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            3,
            GuestDescriptor::Service(GuestServiceDescriptor::ShmemGet(ShmemGetReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 4096,
            })),
        )
        .context("write qemu shmem get descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            4,
            GuestDescriptor::Service(GuestServiceDescriptor::DfsWrite(DfsWriteReq {
                task: None,
                path: "/weights/qemu-demo.bin".into(),
                bytes: 4096,
            })),
        )
        .context("write qemu dfs write descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            5,
            GuestDescriptor::Service(GuestServiceDescriptor::DfsRead(DfsReadReq {
                task: None,
                path: "/weights/qemu-demo.bin".into(),
            })),
        )
        .context("write qemu dfs read descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            6,
            GuestDescriptor::Service(GuestServiceDescriptor::DbPut(DbPutReq {
                task: None,
                key: "qemu:kv:weights".into(),
                bytes: 128,
            })),
        )
        .context("write qemu db put descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            7,
            GuestDescriptor::Service(GuestServiceDescriptor::DbGet(DbGetReq {
                task: None,
                key: "qemu:kv:weights".into(),
            })),
        )
        .context("write qemu db get descriptor failed")?;
    handler
        .write(mmio.cmdq_tail_addr(endpoint), 8)
        .context("write qemu backend cmdq tail register failed")?;

    handler
        .write(mmio.doorbell_addr(endpoint), 8)
        .context("write qemu backend doorbell register failed")?;
    let status_after_ring_word = handler
        .read(mmio.status_addr(endpoint))
        .context("read qemu backend status register failed")?;
    let cmdq_head_after_ring = handler
        .read(mmio.cmdq_head_addr(endpoint))
        .context("read qemu backend cmdq head register failed")?
        as usize;
    let cmdq_tail_after_submit = handler
        .read(mmio.cmdq_tail_addr(endpoint))
        .context("read qemu backend cmdq tail register failed")?
        as usize;
    let cq_tail_after_ring = handler
        .read(mmio.cq_tail_addr(endpoint))
        .context("read qemu backend cq tail register failed")?
        as usize;
    let irq_status_after_ring = handler
        .read(mmio.irq_status_addr(endpoint))
        .context("read qemu backend irq status register failed")?;
    let pending_after_ring = ((status_after_ring_word >> 16) & 0xffff) as usize;

    let health = handler
        .device_mut()
        .get_health(0)
        .context("qemu backend get health failed")?;

    let (partial_events, cq_remaining_after_partial_poll) = handler
        .device_mut()
        .poll_cq(endpoint, Some(1))
        .context("qemu backend partial poll failed")?;
    let cq_head_after_partial_poll = handler
        .read(mmio.cq_head_addr(endpoint))
        .context("read qemu backend cq head register after partial poll failed")?
        as usize;
    for completion in partial_events {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    let drained = handler
        .device_mut()
        .drain_cq(endpoint)
        .context("qemu backend drain failed")?;
    for completion in drained {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    handler
        .write(mmio.irq_ack_addr(endpoint), irq_status_after_ring)
        .context("write qemu backend irq ack register failed")?;
    let irq_status_after_ack = handler
        .read(mmio.irq_status_addr(endpoint))
        .context("read qemu backend irq status after ack failed")?;

    Ok(QemuBackendDemoReport {
        hosts_count: topo.hosts,
        ubpus_count: topo.ubpus,
        entities_count: topo.entities,
        domains_count: topo.domains,
        endpoint_id: endpoint.0,
        cmdq_depth: layout.cmdq_depth as usize,
        cmdq_head_after_ring,
        cmdq_tail_after_submit,
        cq_head_after_partial_poll,
        cq_tail_after_ring,
        irq_status_after_ring,
        irq_status_after_ack,
        segment,
        health,
        pending_after_ring,
        cq_remaining_after_partial_poll,
        summary: summarize_events(&events),
        events,
    })
}

fn print_report(report: &CliReport) {
    println!("scenario: {}", report.scenario_name);
    println!("group: {}", report.group.as_deref().unwrap_or("-"));
    println!("variant: {}", report.variant.as_deref().unwrap_or("-"));
    println!("logical_system: {}", report.logical_system);
    println!("scenario_file: {}", report.scenario_file);
    println!(
        "topology: hosts={} ubpus={} ubcs={} ummus={} entities={} decoders={} domains={} routes={}",
        report.topology.hosts_count,
        report.topology.ubpus_count,
        report.topology.ubcs_count,
        report.topology.ummus_count,
        report.topology.entities_count,
        report.topology.decoders_count,
        report.topology.domains_count,
        report.topology.routes_count
    );
    println!();
    println!("hosts:");
    for host in &report.topology.hosts {
        println!(
            "  host id={} node_id={} health={:?}",
            host.id, host.node_id, host.health
        );
    }

    println!("ubpus:");
    for ubpu in &report.topology.ubpus {
        println!(
            "  ubpu id={} node_id={} host_id={} health={:?}",
            ubpu.id, ubpu.node_id, ubpu.host_id, ubpu.health
        );
    }

    println!("entities:");
    for entity in &report.topology.entities {
        println!(
            "  entity id={} eid={} ubpu_id={} ubc_id={} health={:?}",
            entity.id, entity.eid, entity.ubpu_id, entity.ubc_id, entity.health
        );
    }

    println!("ubcs:");
    for ubc in &report.topology.ubcs {
        println!(
            "  ubc id={} node_id={} ubpu_id={} host_id={} health={:?}",
            ubc.id, ubc.node_id, ubc.ubpu_id, ubc.host_id, ubc.health
        );
    }

    println!("ummus:");
    for ummu in &report.topology.ummus {
        println!(
            "  ummu id={} node_id={} ubc_id={} domain_id={} health={:?}",
            ummu.id, ummu.node_id, ummu.ubc_id, ummu.domain_id, ummu.health
        );
    }

    println!("decoders:");
    for decoder in &report.topology.decoders {
        println!(
            "  decoder id={} node_id={} ubc_id={} kind={:?} health={:?}",
            decoder.id, decoder.node_id, decoder.ubc_id, decoder.kind, decoder.health
        );
    }

    println!("domains:");
    for domain in &report.topology.domains {
        println!(
            "  domain id={} label={} node_id={} hosts={:?} health={:?}",
            domain.id, domain.label, domain.node_id, domain.hosts, domain.health
        );
    }

    println!("routes:");
    for route in &report.topology.routes {
        println!(
            "  route id={} scope={:?} from_node={} to_node={} level={:?} domain_id={} health={:?}",
            route.id,
            route.scope,
            route.from_node,
            route.to_node,
            route.level,
            route.domain_id,
            route.health
        );
    }

    println!();
    println!("workload_report:");
    println!(
        "  kind={} profile={} requests={} blocks={} hits={} misses={} promotions={} evictions={} completions={}",
        report.workload_report.workload_kind,
        report.workload_report.workload_profile,
        report.workload_report.requests_total,
        report.workload_report.blocks_total,
        report.workload_report.hits,
        report.workload_report.misses,
        report.workload_report.promotions,
        report.workload_report.evictions,
        report.workload_report.completions
    );
    println!(
        "  summary: prefix_hits={} tail_misses={} fallback_reads={} shmem_puts={} shmem_gets={} shmem_denied={} dfs_cold_reads={} dfs_warm_reads={} block_read_misses={} block_writes={} block_writebacks={} block_retryable_failures={} block_queue_rejections={} dfs_seed_writes={} db_puts={} db_gets={} db_retryable_failures={}",
        report.workload_report.prefix_hits,
        report.workload_report.tail_misses,
        report.workload_report.fallback_reads,
        report.workload_report.shmem_puts,
        report.workload_report.shmem_gets,
        report.workload_report.shmem_denied,
        report.workload_report.dfs_cold_reads,
        report.workload_report.dfs_warm_reads,
        report.workload_report.block_read_misses,
        report.workload_report.block_writes,
        report.workload_report.block_writebacks,
        report.workload_report.block_retryable_failures,
        report.workload_report.block_queue_rejections,
        report.workload_report.dfs_seed_writes,
        report.workload_report.db_puts,
        report.workload_report.db_gets,
        report.workload_report.db_retryable_failures
    );
    println!(
        "  summary: completions={} block={} dfs={} shmem={} db={} retryable={} fatal={} runtime_retried={} runtime_failed={}",
        report.workload_report.summary.completions_total,
        report.workload_report.summary.completions_by_source.block_service,
        report.workload_report.summary.completions_by_source.dfs_service,
        report.workload_report.summary.completions_by_source.shmem_service,
        report.workload_report.summary.completions_by_source.db_service,
        report.workload_report.summary.completions_by_status.retryable_failure,
        report.workload_report.summary.completions_by_status.fatal_failure,
        report.workload_report.summary.runtime_retried,
        report.workload_report.summary.runtime_failed
    );
    if let Some(assessment) = compute_w4_assessment(&report.workload_report.summary) {
        let w4 = &report.workload_report.summary.w4_results_handled;
        let service = &report.workload_report.summary.w4_service_results;
        println!(
            "  w4_handled: total={} payload_validated={} begin={} active={} finish={} control_only={} hot_hit={} filled_from_block={} stable_hot={} promoted_hot={} reloaded_hot={}",
            w4.total,
            w4.payload_validated,
            w4.begin,
            w4.active,
            w4.finish,
            w4.request_control_only,
            w4.hot_hit,
            w4.filled_from_block,
            w4.stable_hot,
            w4.promoted_hot,
            w4.reloaded_hot
        );
        println!(
            "  w4_assessment: payload_coverage={}/{} service_coverage={}/5 complete={}",
            w4.payload_validated, w4.total, assessment.service_covered, assessment.complete
        );
        println!(
            "  w4_service_results: total={} request_control={} kvcache={} request_republished={} finish_control_refresh={} kv_republished={} hot_hit_refresh={} reload_refresh={}",
            service.total,
            service.request_control,
            service.kvcache,
            service.request_republished,
            service.finish_control_refresh,
            service.kv_republished,
            service.hot_hit_refresh,
            service.reload_refresh
        );
        if !assessment.missing.is_empty() {
            println!("  w4_missing: {}", assessment.missing.join(","));
        }
    }
    println!("  events:");
    for event in &report.workload_report.events {
        println!("    {:?}", event);
    }

    println!();
    println!("report_json:");
    println!(
        "{}",
        serde_json::to_string_pretty(report).expect("report json serialization")
    );

    if let Some(aux) = &report.auxiliary {
        println!();
        println!("runtime_demo_events:");
        println!(
            "  summary: completions={} chip={} success={} fatal={} retried={} failed={}",
            aux.runtime_summary.completions_total,
            aux.runtime_summary.completions_by_source.chip_backend,
            aux.runtime_summary.completions_by_status.success,
            aux.runtime_summary.completions_by_status.fatal_failure,
            aux.runtime_summary.runtime_retried,
            aux.runtime_summary.runtime_failed
        );
        for event in &aux.runtime_events {
            println!("  {:?}", event);
        }

        println!();
        println!("uapi_demo:");
        println!(
            "  snapshot => hosts={} ubpus={} entities={} domains={}",
            aux.uapi_report.hosts_count,
            aux.uapi_report.ubpus_count,
            aux.uapi_report.entities_count,
            aux.uapi_report.domains_count
        );
        println!("  cq => {:?}", aux.uapi_report.cq);
        println!(
            "  cmdq => {:?} depth={} pending_after_partial_ring={} cq_remaining_after_partial_poll={}",
            aux.uapi_report.cmdq,
            aux.uapi_report.cmdq_depth,
            aux.uapi_report.cmdq_pending_after_partial_ring,
            aux.uapi_report.cq_remaining_after_partial_poll
        );
        println!("  segment => {:?}", aux.uapi_report.segment);
        println!("  health(entity=0) => {:?}", aux.uapi_report.health);
        println!(
            "  summary: completions={} block={} shmem={} dfs={} db={} success={} retryable={} fatal={}",
            aux.uapi_report.summary.completions_total,
            aux.uapi_report.summary.completions_by_source.block_service,
            aux.uapi_report.summary.completions_by_source.shmem_service,
            aux.uapi_report.summary.completions_by_source.dfs_service,
            aux.uapi_report.summary.completions_by_source.db_service,
            aux.uapi_report.summary.completions_by_status.success,
            aux.uapi_report.summary.completions_by_status.retryable_failure,
            aux.uapi_report.summary.completions_by_status.fatal_failure
        );
        println!("  events:");
        for event in &aux.uapi_report.events {
            println!("    {:?}", event);
        }

        println!();
        println!("qemu_backend_demo:");
        println!(
            "  snapshot => hosts={} ubpus={} entities={} domains={}",
            aux.qemu_backend_report.hosts_count,
            aux.qemu_backend_report.ubpus_count,
            aux.qemu_backend_report.entities_count,
            aux.qemu_backend_report.domains_count
        );
        println!(
            "  endpoint={} cmdq_depth={} cmdq_head_after_ring={} cmdq_tail_after_submit={} cq_head_after_partial_poll={} cq_tail_after_ring={} pending_after_ring={} cq_remaining_after_partial_poll={} irq_status_after_ring=0x{:x} irq_status_after_ack=0x{:x}",
            aux.qemu_backend_report.endpoint_id,
            aux.qemu_backend_report.cmdq_depth,
            aux.qemu_backend_report.cmdq_head_after_ring,
            aux.qemu_backend_report.cmdq_tail_after_submit,
            aux.qemu_backend_report.cq_head_after_partial_poll,
            aux.qemu_backend_report.cq_tail_after_ring,
            aux.qemu_backend_report.pending_after_ring,
            aux.qemu_backend_report.cq_remaining_after_partial_poll,
            aux.qemu_backend_report.irq_status_after_ring,
            aux.qemu_backend_report.irq_status_after_ack
        );
        println!("  segment => {:?}", aux.qemu_backend_report.segment);
        println!("  health(entity=0) => {:?}", aux.qemu_backend_report.health);
        println!(
            "  summary: completions={} block={} shmem={} dfs={} db={} success={} retryable={} fatal={}",
            aux.qemu_backend_report.summary.completions_total,
            aux.qemu_backend_report.summary.completions_by_source.block_service,
            aux.qemu_backend_report.summary.completions_by_source.shmem_service,
            aux.qemu_backend_report.summary.completions_by_source.dfs_service,
            aux.qemu_backend_report.summary.completions_by_source.db_service,
            aux.qemu_backend_report.summary.completions_by_status.success,
            aux.qemu_backend_report.summary.completions_by_status.retryable_failure,
            aux.qemu_backend_report.summary.completions_by_status.fatal_failure
        );
        println!("  events:");
        for event in &aux.qemu_backend_report.events {
            println!("    {:?}", event);
        }
    }
}

struct W4Assessment {
    service_covered: usize,
    complete: bool,
    missing: Vec<&'static str>,
}

fn compute_w4_assessment(summary: &EventSummary) -> Option<W4Assessment> {
    if summary.w4_results_handled.total == 0 && summary.w4_service_results.total == 0 {
        return None;
    }

    let w4 = &summary.w4_results_handled;
    let service = &summary.w4_service_results;
    let payload_complete = w4.payload_validated == w4.total;
    let request_republished_complete = w4.total > 0 && service.request_republished >= w4.total;
    let finish_refresh_complete = w4.finish == 0 || service.finish_control_refresh >= w4.finish;
    let kv_republished_complete = w4.active == 0 || service.kv_republished >= w4.active;
    let hot_hit_refresh_complete = w4.hot_hit == 0 || service.hot_hit_refresh > 0;
    let reload_refresh_complete = w4.reloaded_hot == 0 || service.reload_refresh > 0;
    let service_checks = [
        request_republished_complete,
        finish_refresh_complete,
        kv_republished_complete,
        hot_hit_refresh_complete,
        reload_refresh_complete,
    ];
    let service_covered = service_checks.into_iter().filter(|ok| *ok).count();
    let service_complete = service_checks.into_iter().all(|ok| ok);
    let mut missing = Vec::new();
    if !payload_complete {
        missing.push("payload_validation");
    }
    if !request_republished_complete {
        missing.push("request_republished");
    }
    if !finish_refresh_complete {
        missing.push("finish_control_refresh");
    }
    if !kv_republished_complete {
        missing.push("kv_republished");
    }
    if !hot_hit_refresh_complete {
        missing.push("hot_hit_refresh");
    }
    if !reload_refresh_complete {
        missing.push("reload_refresh");
    }

    Some(W4Assessment {
        service_covered,
        complete: payload_complete && service_complete,
        missing,
    })
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

fn topology_report(topology: &SimTopology) -> TopologyReport {
    let snapshot = topology.snapshot();

    TopologyReport {
        hosts_count: snapshot.hosts,
        ubpus_count: snapshot.ubpus,
        ubcs_count: snapshot.ubcs,
        ummus_count: snapshot.ummus,
        entities_count: snapshot.entities,
        decoders_count: snapshot.decoders,
        domains_count: snapshot.domains,
        routes_count: snapshot.routes,
        hosts: topology
            .hosts
            .iter()
            .map(|host| HostReport {
                id: host.id,
                node_id: host.node_id,
                health: host.health,
            })
            .collect(),
        ubpus: topology
            .ubpus
            .iter()
            .map(|ubpu| UbpuReport {
                id: ubpu.id,
                node_id: ubpu.node_id,
                host_id: ubpu.host_id,
                health: ubpu.health,
            })
            .collect(),
        ubcs: topology
            .ubcs
            .iter()
            .map(|ubc| UbcReport {
                id: ubc.id,
                node_id: ubc.node_id,
                ubpu_id: ubc.ubpu_id,
                host_id: ubc.host_id,
                health: ubc.health,
            })
            .collect(),
        ummus: topology
            .ummus
            .iter()
            .map(|ummu| UmmuReport {
                id: ummu.id,
                node_id: ummu.node_id,
                ubc_id: ummu.ubc_id,
                domain_id: ummu.domain_id,
                health: ummu.health,
            })
            .collect(),
        entities: topology
            .entities
            .iter()
            .map(|entity| EntityReport {
                id: entity.id,
                eid: entity.eid,
                ubpu_id: entity.ubpu_id,
                ubc_id: entity.ubc_id,
                health: entity.health,
            })
            .collect(),
        decoders: topology
            .decoders
            .iter()
            .map(|decoder| DecoderReport {
                id: decoder.id,
                node_id: decoder.node_id,
                ubc_id: decoder.ubc_id,
                kind: decoder.kind,
                health: decoder.health,
            })
            .collect(),
        domains: topology
            .domains
            .iter()
            .map(|domain| DomainReport {
                id: domain.id,
                label: domain.label.clone(),
                node_id: domain.node_id,
                hosts: domain.hosts.clone(),
                health: domain.health,
            })
            .collect(),
        routes: topology
            .routes
            .iter()
            .map(|route| RouteReport {
                id: route.binding.id,
                scope: route.binding.scope,
                from_node: route.binding.from_node,
                to_node: route.binding.to_node,
                level: route.binding.level,
                domain_id: route.domain_id,
                health: route.health,
            })
            .collect(),
    }
}
