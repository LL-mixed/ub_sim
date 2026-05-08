use anyhow::Context;
use sim_config::ScenarioConfig;
use sim_core::{
    BlockHash, CompletionSource, CompletionStatus, CopyDirection, CopyRequest, DispatchRequest,
    FunctionLabel, HierarchyCoord, IoOpcode, IoSubmitReq, LogicalSystemId, MemoryEndpoint, PlLevel,
    SegmentHandle, SimEvent, TaskKey,
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
    qwen3_dense_0_6b_decode_loop_report, qwen3_dense_0_6b_decode_loop_report_with_prompt,
    qwen3_dense_0_6b_default_guest_input, qwen3_dense_0_6b_prefill_text_output_report,
    LocalGuestUapiSurface, UapiCommand, UapiDescriptor, UapiResponse,
};
use sim_workloads::{run_host_vector_dispatch, run_minimal_workload};
use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() -> anyhow::Result<()> {
    if lingqu_object_service_args() {
        return run_lingqu_object_service_cli();
    }
    if let Some(args) = qwen3_decode_loop_args()? {
        return run_qwen3_decode_loop_cli(&args);
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

#[derive(Clone, Debug, PartialEq)]
struct Qwen3DecodeLoopCliArgs {
    scenario_path: PathBuf,
    step_count: usize,
    prompt: Option<String>,
    matmul_batch: Option<usize>,
    temperature: Option<f32>,
}

fn qwen3_decode_loop_args() -> anyhow::Result<Option<Qwen3DecodeLoopCliArgs>> {
    qwen3_decode_loop_args_from(env::args_os().skip(1))
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
            let mut temperature = None;
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
                } else if text == "--temperature" {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("--temperature requires a value"))?;
                    temperature = Some(parse_non_negative_f32(
                        "--temperature",
                        &next.to_string_lossy(),
                    )?);
                } else if let Some(value) = text.strip_prefix("--temperature=") {
                    temperature = Some(parse_non_negative_f32("--temperature", value)?);
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
                temperature,
            }))
        }
        _ => Ok(None),
    }
}

fn parse_non_negative_f32(label: &str, value: &str) -> anyhow::Result<f32> {
    let parsed = value
        .parse::<f32>()
        .with_context(|| format!("{label} must be a finite non-negative number"))?;
    if !parsed.is_finite() || parsed < 0.0 {
        anyhow::bail!("{label} must be a finite non-negative number");
    }
    Ok(parsed)
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
    let mut service = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
    publish_lingqu_object_cli_sample(
        &mut service,
        "qwen3/model/Qwen3-0.6B/layer/00/q_proj/shard/0",
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
        "qwen3/model/Qwen3-0.6B/layer/00/q_proj/shard/0",
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
        "  committed_object_count: {}",
        report.committed_object_count
    );
    println!("  missing_resolve_count: {}", report.missing_resolve_count);
    println!("  checksum: {:#x}", report.checksum);
    Ok(())
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
        qwen3_decode_report_verbosity_from_env, Qwen3DecodeReportVerbosity,
    };
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
        assert_eq!(args.temperature, None);
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
            "--temperature",
            "0.7",
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
        assert_eq!(args.temperature, Some(0.7));
    }

    #[test]
    fn qwen3_decode_loop_args_accept_trailing_prompt_with_options() {
        let args = qwen3_decode_loop_args_from([
            "qwen3-decode-loop",
            "--scenario=8host",
            "--steps=8",
            "--matmul-batch=2",
            "--temperature=0",
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
        assert_eq!(args.temperature, Some(0.0));
    }

    #[test]
    fn qwen3_decode_loop_args_reject_negative_temperature() {
        let err = qwen3_decode_loop_args_from([
            "qwen3-decode-loop",
            "--scenario=4host",
            "--temperature=-0.1",
        ])
        .expect_err("negative temperature must fail");
        assert!(err.to_string().contains("--temperature"));
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
        "qwen3-decode-loop: scenario={} steps={} prompt_bytes={} matmul_batch={} temperature={}",
        scenario_path.display(),
        args.step_count,
        args.prompt.as_deref().map(str::len).unwrap_or(0),
        args.matmul_batch
            .map(|value| value.to_string())
            .unwrap_or_else(|| "default".to_string()),
        args.temperature
            .map(|value| value.to_string())
            .unwrap_or_else(|| "0".to_string())
    );
    let report = if let Some(prompt) = args.prompt.as_deref() {
        qwen3_dense_0_6b_decode_loop_report_with_prompt(&topology, args.step_count, prompt)
    } else {
        qwen3_dense_0_6b_decode_loop_report(&topology, args.step_count)
    }
    .map_err(anyhow::Error::msg)
    .context("failed to run Qwen3 decode loop")?;
    println!("qwen3_dense_0_6b_decode_loop");
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
                    "  step={} runtime_prefill={} input_tokens={} sampled_tokens={} text_bytes={} contract_ready={} blockers={} synthetic_stages={} full_forward_math={} full_vocab_logits={} object_ready={} object_publish={} object_resolve={} object_append={} kv_resolve={} kv_append={} obmm_pool={} obmm_queue={} object_checksum={:#x} input_checksum={:#x} next_input_checksum={:#x}",
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

fn print_qwen3_decode_verbose_steps(steps: &[sim_uapi::Qwen3Dense06bDecodeLoopStepReport]) {
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
            "  object_service step={} ready={} publish={} resolve={} append={} kv_resolve={} kv_append={} metadata_put={} metadata_get={} shmem_write={} shmem_read={} block_write={} block_read={} inline_write={} inline_read={} obmm_pool={} obmm_write={} obmm_read={} obmm_queue_submit={} obmm_queue_deliver={} obmm_bytes={} committed={} missing_resolve={} token_objects={} kv_objects={} runtime_tensor_objects={} logits_objects={} checksum={:#x}",
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
            step.object_service.runtime_tensor_objects,
            step.object_service.logits_objects,
            step.object_service.object_checksum
        );
    }
}

fn prepare_qwen3_decode_loop_environment(args: &Qwen3DecodeLoopCliArgs) -> anyhow::Result<()> {
    let scenario_env_path = args
        .scenario_path
        .canonicalize()
        .unwrap_or_else(|_| args.scenario_path.clone());
    std::env::set_var("SIM_UAPI_SCENARIO_CONFIG", &scenario_env_path);

    let Some(matmul_batch) = args.matmul_batch else {
        if let Some(temperature) = args.temperature {
            std::env::set_var("SIM_QWEN3_TEMPERATURE", temperature.to_string());
        }
        return Ok(());
    };
    if let Some(temperature) = args.temperature {
        std::env::set_var("SIM_QWEN3_TEMPERATURE", temperature.to_string());
    }
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
    if manifest_path.exists() {
        return Ok(());
    }
    let output_dir = manifest_path
        .parent()
        .ok_or_else(|| anyhow::anyhow!("manifest has no parent: {}", manifest_path.display()))?;
    let script = repo_root()
        .join("guest-linux")
        .join("aarch64")
        .join("scripts")
        .join("prepare_simpler_host_matmul_artifacts.py");
    if !script.exists() {
        anyhow::bail!(
            "missing simpler host matmul artifact producer: {}",
            script.display()
        );
    }

    let mut command = Command::new("python3");
    command.arg(&script).arg("--output-dir").arg(output_dir);
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

fn default_simpler_host_matmul_manifest_path() -> PathBuf {
    Path::new("/tmp")
        .join("simpler-host-matmul-artifacts")
        .join("host_matmul_manifest.json")
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
    let guest_input = qwen3_dense_0_6b_default_guest_input();
    let report = qwen3_dense_0_6b_prefill_text_output_report(&topology, &guest_input)
        .map_err(anyhow::Error::msg)
        .context("failed to run Qwen3 text output prefill")?;
    println!("qwen3_dense_0_6b_text_output");
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
