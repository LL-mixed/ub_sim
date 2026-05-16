use anyhow::Context;
use sim_config::ScenarioConfig;
use sim_core::{
    BlockHash, CompletionSource, CompletionStatus, CopyDirection, CopyRequest, DispatchRequest,
    FunctionLabel, HierarchyCoord, IoOpcode, IoSubmitReq, LogicalSystemId, MemoryEndpoint, PlLevel,
    SegmentHandle, SimEvent, TaskKey,
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
    object::{
        LingquObjectKind, LingquObjectLocality, LingquObjectMetadata, LingquObjectPublishReq,
        LingquObjectResolveReq, LingquObjectServiceProfile, LingquObjectServiceStub,
        LingquObjectState, LingquObjectVersionSelector, LingquPayloadBackend,
        LingquPayloadPlacement,
    },
    shmem::{ShmemGetReq, ShmemPutReq},
};
use sim_topology::SimTopology;
use sim_uapi::{
    qwen3_dense_reference_decode_loop_report, qwen3_dense_reference_decode_loop_report_with_prompt,
    qwen3_dense_reference_default_guest_input, qwen3_dense_reference_prefill_text_output_report,
    qwen3_dense_reference_range_forward_report_with_prompt, LocalGuestUapiSurface, UapiCommand,
    UapiDescriptor, UapiResponse,
};
use sim_workloads::{run_host_vector_dispatch, run_minimal_workload};
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

mod qwen3_simpler;

fn main() -> anyhow::Result<()> {
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
    if !config.enabled {
        return Vec::new();
    }
    vec![
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
    ]
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
        lingqu_object_service_args_from, qwen3_decode_loop_args_from,
        qwen3_decode_report_verbosity_from_env, qwen3_dense_weights_path_from_env,
        qwen3_engram_policy_checksum, qwen3_engram_select_token, qwen3_engram_state_words,
        qwen3_guest_candidate_records, qwen3_guest_decode_loop_args_from,
        qwen3_guest_default_w5_profile,
        qwen3_guest_dense_runtime, qwen3_guest_engram_candidate_counts,
        qwen3_guest_engram_env_vars, qwen3_guest_engram_expected_terminal_rewrites,
        qwen3_guest_engram_history_lengths, qwen3_guest_engram_object_transport_report,
        qwen3_guest_engram_report, qwen3_guest_engram_report_from_guest_log,
        qwen3_guest_engram_selected_tokens, qwen3_guest_log_dir_from_script_output,
        qwen3_guest_log_match_count, qwen3_guest_terminal_candidate_records,
        qwen3_guest_terminal_text_lossy_from_tokenizer, qwen3_guest_terminal_tokens,
        qwen3_guest_timing_summary, qwen3_range_forward_args_from,
        simpler_host_matmul_artifact_producer_path, validate_qwen3_dense_weights_path,
        validate_w5_inference_profile, Qwen3CandidateRecord, Qwen3DecodeReportVerbosity,
        Qwen3EngramConfig, Qwen3EngramContextOp, Qwen3EngramMode, Qwen3EngramPool,
        Qwen3EngramReport, Qwen3GuestDecodeLoopCliArgs,
    };
    use std::env;
    use std::fs;
    use std::path::PathBuf;

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
    let engram_simt = qwen3_prepare_engram_simt_mode(&args.engram)?;
    let w5_profile = args
        .w5_profile
        .clone()
        .unwrap_or_else(|| qwen3_guest_default_w5_profile(&runtime, &args.engram));
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
    if args.engram.enabled {
        println!(
            "  engram: enabled=true mode={} pool={} owner_node={} no_repeat_ngram_size={} repetition_penalty_milli={} history_window={} blocked_token_ids={:?} context_op={} report={}",
            qwen3_engram_mode_name(args.engram.mode),
            qwen3_engram_pool_name(args.engram.pool),
            args.engram.owner_node,
            args.engram.no_repeat_ngram_size,
            args.engram.repetition_penalty_milli,
            args.engram.history_window,
            args.engram.blocked_token_ids,
            qwen3_engram_context_op_name(args.engram.context_op),
            qwen3_engram_report_name(args.engram.report)
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
        .or_else(|| args.prompt.as_deref().map(qwen3_guest_prompt_token_ids_env))
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
    for (key, value) in qwen3_guest_engram_env_vars(&args.engram, engram_session_id) {
        command.env(key, value);
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
    let expected_guest_engram_select_count = if args.engram.enabled {
        args.step_count
    } else {
        0
    };
    let expected_guest_engram_candidate_publish_count = expected_guest_engram_select_count;
    let expected_guest_engram_candidate_wait_count = expected_guest_engram_select_count;
    let expected_guest_engram_selected_wait_count = expected_guest_engram_select_count;
    let expected_guest_engram_selected_writeback_count = expected_guest_engram_select_count;
    let expected_guest_engram_history_wait_count = if args.engram.enabled {
        let mut wait_nodes = [false; 9];
        wait_nodes[1] = true;
        wait_nodes[8] = true;
        wait_nodes[args.engram.owner_node] = true;
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
    let guest_engram_history_lengths = qwen3_guest_engram_history_lengths(&combined);
    let guest_engram_candidate_counts = qwen3_guest_engram_candidate_counts(&combined);
    let terminal_text = qwen3_guest_terminal_text_lossy(&terminal_tokens);
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
    let engram_report = if args.engram.enabled {
        let session_id = qwen3_guest_session_id(&prompt_history_tokens);
        if args.engram.pool == Qwen3EngramPool::Obmm {
            Some(qwen3_guest_engram_report_from_guest_log(
                &args.engram,
                session_id,
                &prompt_history_tokens,
                &combined,
            )?)
        } else {
            Some(qwen3_guest_engram_report(
                &args.engram,
                session_id,
                &prompt_history_tokens,
                &combined,
            )?)
        }
    } else {
        None
    };
    let guest_engram_object_transport =
        if args.engram.enabled && args.engram.pool == Qwen3EngramPool::Obmm {
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
            let expected_history_lengths = (0..args.step_count)
                .map(|step| prompt_history_tokens.len() as u64 + step as u64 + 1)
                .collect::<Vec<_>>();
            let blocked_writeback_tokens = terminal_tokens
                .iter()
                .copied()
                .filter(|token| args.engram.blocked_token_ids.contains(token))
                .collect::<Vec<_>>();
            println!(
                "  guest_engram_writeback: selected_tokens={:?} terminal_tokens={:?} blocked_token_ids={:?} blocked_writeback_tokens={:?} history_lengths={:?} candidate_counts={:?} candidate_publishes={} candidate_waits={} selected_waits={} selected_writebacks={} terminal_rewrites={} select_logs={} history_waits={} state_waits={} state_resolved={} matches_terminal={}",
                guest_engram_selected_tokens,
                terminal_tokens,
                args.engram.blocked_token_ids,
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

fn qwen3_guest_terminal_text_lossy(tokens: &[u64]) -> Option<String> {
    let tokenizer_path = qwen3_guest_tokenizer_path()?;
    qwen3_guest_terminal_text_lossy_from_tokenizer(tokens, &tokenizer_path).ok()
}

fn qwen3_guest_prompt_token_ids_env(prompt: &str) -> anyhow::Result<String> {
    let tokenizer_path = qwen3_guest_tokenizer_path()
        .ok_or_else(|| anyhow::anyhow!("qwen3 guest tokenizer path missing"))?;
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
