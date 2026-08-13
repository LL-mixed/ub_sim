use anyhow::Context;
use serde::{Deserialize, Serialize};
use sim_config::{
    RemoteMemoryJitterConfig, RemoteMemoryJitterMode, RemoteMemoryModelConfig,
    RemoteMemoryTailConfig, ScenarioConfig,
};
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::{OsStr, OsString};
use std::fs;
use std::io::{Read, Write};
#[cfg(unix)]
use std::os::unix::process::CommandExt;
use std::path::Component;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Stdio};
use std::thread;
use std::time::{Duration, Instant};

const EVAL_MATRIX_SCHEMA: u32 = 1;
const EVAL_RUN_MANIFEST_SCHEMA: u32 = 1;
const EVAL_VALIDATION_SCHEMA: u32 = 1;
const EVAL_INCOMPLETE_REASON: &str = "formal execution has not completed";
const EVAL_DRY_RUN_REASON: &str = "dry-run contains no formal execution evidence";
const MODEL_MANIFEST_SCHEMA: u32 = 1;
const PAGE_BYTES: u64 = 4096;
const HOST_EVIDENCE_TIMEOUT: Duration = Duration::from_secs(15);
const REMOTE_STAGE_TIMEOUT: Duration = Duration::from_secs(60);
const REMOTE_CASE_TIMEOUT_MARGIN: Duration = Duration::from_secs(120);
const REMOTE_CONNECT_ATTEMPTS: u32 = 3;
const REMOTE_CONNECT_RETRY_DELAY: Duration = Duration::from_millis(200);

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "kebab-case")]
enum EvalBand {
    Scalar,
    Range,
    Transparency,
}

impl EvalBand {
    fn parse_list(value: &str) -> anyhow::Result<BTreeSet<Self>> {
        let mut bands = BTreeSet::new();

        for item in value.split(',') {
            bands.insert(match item {
                "scalar" => Self::Scalar,
                "range" => Self::Range,
                "transparency" => Self::Transparency,
                _ => anyhow::bail!("--bands entries must be scalar, range, or transparency"),
            });
        }
        if bands.is_empty() {
            anyhow::bail!("--bands must not be empty");
        }
        Ok(bands)
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Scalar => "scalar",
            Self::Range => "range",
            Self::Transparency => "transparency",
        }
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
enum EvalMode {
    SyncMmio,
    AsyncPoll,
    SchedulerCore,
    Userfaultfd,
}

impl EvalMode {
    fn as_str(self) -> &'static str {
        match self {
            Self::SyncMmio => "sync-mmio",
            Self::AsyncPoll => "async-poll",
            Self::SchedulerCore => "scheduler-core",
            Self::Userfaultfd => "userfaultfd",
        }
    }

    fn uses_coroutines(self) -> bool {
        matches!(self, Self::SyncMmio | Self::AsyncPoll | Self::SchedulerCore)
    }

    fn uses_inflight(self) -> bool {
        self == Self::AsyncPoll
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "kebab-case")]
enum EvalIssue {
    Demand,
    Lookahead,
    Fault,
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "kebab-case")]
enum JitterProfile {
    None,
    #[serde(rename = "uniform-10pct")]
    Uniform10pct,
    #[serde(rename = "tail-1pct-10x")]
    Tail1pct10x,
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "kebab-case")]
enum OutcomeProfile {
    Success,
    Error,
    DropTimeout,
    DuplicateLate,
}

impl OutcomeProfile {
    fn as_str(self) -> &'static str {
        match self {
            Self::Success => "success",
            Self::Error => "error",
            Self::DropTimeout => "drop-timeout",
            Self::DuplicateLate => "duplicate-late",
        }
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
#[serde(rename_all = "kebab-case")]
enum EvalPattern {
    Sequential,
    Random,
    Dependent,
    Mixed,
}

impl EvalPattern {
    fn as_str(self) -> &'static str {
        match self {
            Self::Sequential => "sequential",
            Self::Random => "random",
            Self::Dependent => "dependent",
            Self::Mixed => "mixed",
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct EvalMinimums {
    scalar_operations: u64,
    range_bytes: u64,
    duration_ms: u64,
    warmup_scalar_operations: u64,
    warmup_range_pages: u64,
    formal_seed_count: u32,
    host_noise_threshold_milli: u32,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct EvalFactors {
    model_latency_us: Vec<u64>,
    jitter_profiles: Vec<JitterProfile>,
    outcomes: Vec<OutcomeProfile>,
    coroutines: Vec<u32>,
    inflight: Vec<u32>,
    lookahead: Vec<u32>,
    compute_us: Vec<u32>,
    patterns: Vec<EvalPattern>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct EvalCaseSpec {
    id: String,
    band: EvalBand,
    mode: EvalMode,
    access_bytes: u32,
    issue: EvalIssue,
    #[serde(default)]
    lookahead: Option<u32>,
    #[serde(default)]
    sweep_lookahead: bool,
    extra_vcpus: u32,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct EvalSweep {
    id: String,
    cases: Vec<String>,
    #[serde(default)]
    operations: Option<u64>,
    model_latency_us: Vec<u64>,
    jitter_profiles: Vec<JitterProfile>,
    outcomes: Vec<OutcomeProfile>,
    coroutines: Vec<u32>,
    inflight: Vec<u32>,
    lookahead: Vec<u32>,
    compute_us: Vec<u32>,
    patterns: Vec<EvalPattern>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct EvalMatrix {
    schema: u32,
    name: String,
    minimums: EvalMinimums,
    factors: EvalFactors,
    cases: Vec<EvalCaseSpec>,
    sweeps: Vec<EvalSweep>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ObmmEvalCliArgs {
    pub matrix_path: PathBuf,
    pub scenario_path: PathBuf,
    pub bands: String,
    pub seeds: String,
    pub output_dir: PathBuf,
    pub gate_dir: PathBuf,
    pub remote_target: Option<String>,
    pub remote_repo: Option<PathBuf>,
    pub local_repo: Option<PathBuf>,
    pub aggregate_only: bool,
    pub dry_run: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
struct ExpandedEvalCase {
    run_id: String,
    sweep: String,
    case_id: String,
    band: EvalBand,
    mode: EvalMode,
    issue: EvalIssue,
    seed: u64,
    access_bytes: u32,
    operations: u64,
    deadline_us: u64,
    warmup_operations: u64,
    minimum_duration_ms: u64,
    model_latency_us: u64,
    jitter_profile: JitterProfile,
    outcome: OutcomeProfile,
    coroutines: u32,
    inflight: u32,
    lookahead: u32,
    compute_us: u32,
    pattern: EvalPattern,
    extra_vcpus: u32,
    diagnostic_trace_required: bool,
    operation_list_hash: String,
    model_manifest_hash: String,
    model_file_sha256: String,
    model_manifest_path: String,
    order_index: u64,
    command: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct EvalRunManifest {
    schema: u32,
    #[serde(default)]
    run_namespace: String,
    matrix_name: String,
    matrix_path: String,
    matrix_hash: String,
    scenario_path: String,
    scenario_hash: String,
    scenario_file_sha256: String,
    topology_hosts: u32,
    topology_hash: String,
    selected_bands: Vec<String>,
    seeds: Vec<u64>,
    gate_dir: String,
    gates_passed: bool,
    valid_for_execution: bool,
    cases: Vec<ExpandedEvalCase>,
}

#[derive(Clone, Debug, Serialize)]
struct ModelManifestPayload {
    schema: u32,
    scenario_name: String,
    scenario_seed: u64,
    remote_memory_model: RemoteMemoryModelConfig,
}

#[derive(Clone, Debug, Serialize)]
struct ModelManifest {
    schema: u32,
    manifest_hash: String,
    scenario_name: String,
    scenario_seed: u64,
    remote_memory_model: RemoteMemoryModelConfig,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct GateEvidence {
    schema: u32,
    phase: String,
    status: String,
    scenario_hash: String,
    model_contract_hash: String,
    evidence: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct GateValidation {
    phase: String,
    path: String,
    status: String,
    reasons: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct EvalValidation {
    schema: u32,
    status: String,
    gates: Vec<GateValidation>,
    invalid_reasons: Vec<String>,
    expanded_cases: usize,
    formal_seed_count_met: bool,
    runs: Vec<RunValidation>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct RunValidation {
    run_id: String,
    case_id: String,
    seed: u64,
    status: String,
    reasons: Vec<String>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct GuestEvalSummary {
    schema: u32,
    band: String,
    mode: String,
    case_id: String,
    seed: u64,
    operations: u64,
    checksum: String,
    failures: u64,
    timeouts: u64,
    guest_ns_p50: u64,
    guest_ns_p95: u64,
    guest_ns_p99: u64,
    guest_ns_max: u64,
    makespan_ns: u64,
    model_wait_ns: Option<u64>,
    useful_work_ns: u64,
    application_cpu_ns: u64,
    helper_cpu_ns: u64,
    extra_vcpus: u32,
    trace_sample_ppm: u32,
    trace_sampled: u64,
    trace_dropped: u64,
    fail_closed_process_exit: bool,
    phase: GuestPhaseMetrics,
    status: String,
}

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
struct GuestPhaseMetrics {
    ready_ns: u64,
    wait_ns: u64,
    idle_ns: u64,
    no_ready: u64,
    submit_ns_p50: u64,
    submit_ns_total: u64,
    switch_ns_p50: u64,
    switch_ns_total: u64,
    cq_drain_ns_p50: u64,
    cq_drain_ns_total: u64,
    configured_lookahead: u32,
    backend_pending_high: u64,
    backend_capacity: u64,
    sink_copy_bytes: u64,
    sink_copy_ns: u64,
    backend_late: u64,
    backend_duplicate: u64,
    scc_save_cycles: u64,
    scc_schedule_cycles: u64,
    scc_restore_cycles: u64,
    scc_commit_cycles: u64,
    el0_upcalls_pending: u64,
    el0_upcalls_complete: u64,
    el0_upcalls_fault: u64,
    el0_context_saves: u64,
    el0_context_restores: u64,
    el0_context_switches: u64,
    el0_context_bytes: u64,
    el0_scheduler_ns: u64,
    el0_no_ready_waits: u64,
    direct_el0_upcalls: u64,
    qemu_context_saves: u64,
    qemu_context_restores: u64,
    qemu_context_switches: u64,
    qemu_context_bytes: u64,
    uffd_fault_ns_p50: u64,
    uffd_fault_ns_p95: u64,
    uffd_fault_ns_p99: u64,
    uffd_fault_ns_max: u64,
    uffd_remote_ns_p50: u64,
    uffd_remote_ns_p95: u64,
    uffd_remote_ns_p99: u64,
    uffd_remote_ns_max: u64,
    uffd_copy_ns_p50: u64,
    uffd_copy_ns_p95: u64,
    uffd_copy_ns_p99: u64,
    uffd_copy_ns_max: u64,
    uffd_wake_ns_p50: u64,
    uffd_wake_ns_p95: u64,
    uffd_wake_ns_p99: u64,
    uffd_wake_ns_max: u64,
    uffd_handler_cpu_ns: u64,
    uffd_worker_cpu_ns: u64,
    model_pending_final: u64,
    backend_pending_final: u64,
    scc_pending_final: u64,
    counter_overflow: u64,
    clock_regressions: u64,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct HostEvidence {
    elapsed_ns: u64,
    load1_milli: Option<u64>,
    online_cpus: Option<u32>,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
struct RawRunRecord {
    schema: u32,
    kind: String,
    run_id: String,
    exit_code: Option<i32>,
    host: HostEvidence,
    summary: Option<GuestEvalSummary>,
    #[serde(default)]
    diagnostic_summary: Option<GuestEvalSummary>,
    initial_reasons: Vec<String>,
}

#[derive(Clone, Debug, Serialize)]
struct RawOutputLine<'a> {
    schema: u32,
    kind: &'static str,
    stream: &'static str,
    line: &'a str,
}

#[cfg(test)]
#[derive(Clone, Copy, Debug, PartialEq)]
struct DerivedMetrics {
    overlap_hidden_ns: u64,
    overlap_efficiency: Option<f64>,
    schedule_ahead_gain_ns: i128,
    mechanism_gain_ns: i128,
    core_efficiency: Option<f64>,
}

pub(crate) fn args() -> anyhow::Result<Option<ObmmEvalCliArgs>> {
    args_from(std::env::args_os().skip(1))
}

fn args_from<I, S>(args: I) -> anyhow::Result<Option<ObmmEvalCliArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    if args.next().as_deref() != Some(OsStr::new("obmm-remote-load-eval")) {
        return Ok(None);
    }
    let mut matrix_path = None;
    let mut scenario_path = None;
    let mut bands = "scalar,range,transparency".to_string();
    let mut seeds = "1..7".to_string();
    let mut output_dir = None;
    let mut gate_dir = PathBuf::from("out/obmm-remote-load/gates");
    let mut remote_target = None;
    let mut remote_repo = None;
    let mut local_repo = None;
    let mut aggregate_only = false;
    let mut dry_run = false;
    let mut pending = args.peekable();

    while let Some(argument) = pending.next() {
        let text = argument.to_string_lossy();
        if text == "--dry-run" {
            dry_run = true;
            continue;
        }
        if text == "--aggregate-only" {
            aggregate_only = true;
            continue;
        }
        let (name, value) = if let Some((name, value)) = text.split_once('=') {
            (name.to_string(), value.to_string())
        } else if text.starts_with("--") {
            let value = pending
                .next()
                .ok_or_else(|| anyhow::anyhow!("{text} requires a value"))?;
            (text.into_owned(), value.to_string_lossy().into_owned())
        } else {
            anyhow::bail!("unexpected obmm-remote-load-eval argument: {text}");
        };
        match name.as_str() {
            "--matrix" => matrix_path = Some(PathBuf::from(value)),
            "--scenario" => scenario_path = Some(PathBuf::from(value)),
            "--bands" => bands = value,
            "--seeds" => seeds = value,
            "--output-dir" => output_dir = Some(PathBuf::from(value)),
            "--gate-dir" => gate_dir = PathBuf::from(value),
            "--remote-target" => remote_target = Some(value),
            "--remote-repo" => remote_repo = Some(PathBuf::from(value)),
            "--local-repo" => local_repo = Some(PathBuf::from(value)),
            _ => anyhow::bail!("unknown obmm-remote-load-eval option: {name}"),
        }
    }
    EvalBand::parse_list(&bands)?;
    parse_seeds(&seeds)?;
    if dry_run && aggregate_only {
        anyhow::bail!("--dry-run and --aggregate-only are mutually exclusive");
    }
    let remote_partial = remote_target.is_some() != remote_repo.is_some();
    if remote_partial {
        anyhow::bail!("--remote-target and --remote-repo must be provided together");
    }
    if local_repo.is_some() && remote_target.is_some() {
        anyhow::bail!("--local-repo is mutually exclusive with remote execution options");
    }
    if !dry_run && !aggregate_only && local_repo.is_none() && remote_target.is_none() {
        anyhow::bail!(
            "formal execution requires --local-repo or --remote-target with --remote-repo; \
             use --aggregate-only to process existing raw evidence"
        );
    }
    Ok(Some(ObmmEvalCliArgs {
        matrix_path: matrix_path.ok_or_else(|| anyhow::anyhow!("--matrix is required"))?,
        scenario_path: scenario_path.ok_or_else(|| anyhow::anyhow!("--scenario is required"))?,
        bands,
        seeds,
        output_dir: output_dir.ok_or_else(|| anyhow::anyhow!("--output-dir is required"))?,
        gate_dir,
        remote_target,
        remote_repo,
        local_repo,
        aggregate_only,
        dry_run,
    }))
}

pub(crate) fn run(args: &ObmmEvalCliArgs) -> anyhow::Result<()> {
    let matrix_bytes = fs::read(&args.matrix_path)
        .with_context(|| format!("read matrix {}", args.matrix_path.display()))?;
    let matrix: EvalMatrix = serde_yaml::from_slice(&matrix_bytes)
        .with_context(|| format!("parse matrix {}", args.matrix_path.display()))?;
    validate_matrix(&matrix)?;
    let scenario_bytes = fs::read(&args.scenario_path)
        .with_context(|| format!("read scenario {}", args.scenario_path.display()))?;
    let scenario = ScenarioConfig::from_yaml_file(&args.scenario_path)
        .with_context(|| format!("parse scenario {}", args.scenario_path.display()))?;
    let bands = EvalBand::parse_list(&args.bands)?;
    let seeds = parse_seeds(&args.seeds)?;
    let matrix_hash = hash_bytes(&matrix_bytes);
    let scenario_hash = hash_bytes(&scenario_bytes);
    let scenario_file_sha256 = sha256_file(&args.scenario_path)?;
    let topology_hash = hash_json(&scenario.topology)?;

    fs::create_dir_all(args.output_dir.join("models"))?;
    fs::create_dir_all(args.output_dir.join("raw"))?;
    fs::create_dir_all(args.output_dir.join("summary"))?;
    if !args.aggregate_only && args.output_dir.join("run-manifest.json").exists() {
        anyhow::bail!(
            "refusing to overwrite an existing evaluation at {}; use a new --output-dir so \
             reruns receive new run IDs",
            args.output_dir.display()
        );
    }
    let run_namespace = eval_run_namespace(&args.output_dir)?;

    let (gates_passed, gate_validation) = validate_gates(&args.gate_dir, &scenario_hash)?;
    let mut cases = expand_matrix(
        &matrix,
        &scenario,
        &args.scenario_path,
        &bands,
        &seeds,
        &args.output_dir,
    )?;
    assign_deterministic_order(&mut cases);
    let formal_seed_count_met = seeds.len() >= matrix.minimums.formal_seed_count as usize;
    let manifest = EvalRunManifest {
        schema: EVAL_RUN_MANIFEST_SCHEMA,
        run_namespace,
        matrix_name: matrix.name.clone(),
        matrix_path: args.matrix_path.display().to_string(),
        matrix_hash,
        scenario_path: args.scenario_path.display().to_string(),
        scenario_hash,
        scenario_file_sha256,
        topology_hosts: scenario.topology.hosts,
        topology_hash,
        selected_bands: bands.iter().map(|band| band.as_str().to_string()).collect(),
        seeds,
        gate_dir: args.gate_dir.display().to_string(),
        gates_passed,
        valid_for_execution: gates_passed,
        cases,
    };
    write_json(&args.output_dir.join("run-manifest.json"), &manifest)?;
    let mut invalid_reasons = Vec::new();
    if !gates_passed {
        invalid_reasons.push("one or more P0-P4 gate evidence files are missing or invalid".into());
    }
    if !formal_seed_count_met {
        invalid_reasons.push(format!(
            "formal statistics require at least {} seeds",
            matrix.minimums.formal_seed_count
        ));
    }
    invalid_reasons.push(if args.dry_run {
        EVAL_DRY_RUN_REASON.into()
    } else {
        EVAL_INCOMPLETE_REASON.into()
    });
    let mut validation = EvalValidation {
        schema: EVAL_VALIDATION_SCHEMA,
        status: "invalid".into(),
        gates: gate_validation,
        invalid_reasons,
        expanded_cases: manifest.cases.len(),
        formal_seed_count_met,
        runs: Vec::new(),
    };
    write_json(&args.output_dir.join("validation.json"), &validation)?;
    if args.dry_run {
        write_empty_summaries(&args.output_dir, &bands)?;
        write_dry_run_report(&args.output_dir, &manifest, &validation)?;
        println!(
            "OBMM_EVAL_DRY_RUN schema=1 cases={} gates={} run_manifest={} \
             validation={} status=dry-run validation_status=invalid",
            manifest.cases.len(),
            if gates_passed { "pass" } else { "invalid" },
            args.output_dir.join("run-manifest.json").display(),
            args.output_dir.join("validation.json").display(),
        );
        return Ok(());
    }
    if !gates_passed {
        anyhow::bail!(
            "formal execution is blocked by P0-P4 gates; inspect {}",
            args.output_dir.join("validation.json").display()
        );
    }
    if !args.aggregate_only {
        execute_cases(args, &manifest)?;
    }
    validation.runs = aggregate_results(&args.output_dir, &manifest, &matrix, &bands)?;
    finalize_validation(&mut validation, manifest.cases.len());
    write_json(&args.output_dir.join("validation.json"), &validation)?;
    write_final_report(&args.output_dir, &manifest, &validation)?;
    println!(
        "OBMM_EVAL_COMPLETE schema=1 cases={} valid_runs={} status={} \
         report={}",
        manifest.cases.len(),
        validation
            .runs
            .iter()
            .filter(|run| run.status == "pass")
            .count(),
        validation.status,
        args.output_dir.join("report.md").display(),
    );
    Ok(())
}

fn finalize_validation(validation: &mut EvalValidation, expected_runs: usize) {
    validation
        .invalid_reasons
        .retain(|reason| reason != EVAL_INCOMPLETE_REASON);
    if validation.runs.len() != expected_runs {
        validation.invalid_reasons.push(format!(
            "expected {expected_runs} raw runs, collected {}",
            validation.runs.len()
        ));
    }
    if validation.runs.iter().any(|run| run.status != "pass") {
        validation
            .invalid_reasons
            .push("one or more raw runs are invalid".into());
    }
    validation.invalid_reasons.sort();
    validation.invalid_reasons.dedup();
    validation.status = if validation.invalid_reasons.is_empty() {
        "pass"
    } else {
        "invalid"
    }
    .into();
}

fn validate_matrix(matrix: &EvalMatrix) -> anyhow::Result<()> {
    if matrix.schema != EVAL_MATRIX_SCHEMA {
        anyhow::bail!("matrix schema must be {EVAL_MATRIX_SCHEMA}");
    }
    if matrix.name.trim().is_empty() || matrix.cases.is_empty() || matrix.sweeps.is_empty() {
        anyhow::bail!("matrix name, cases, and sweeps must be non-empty");
    }
    if matrix.minimums.scalar_operations < 10_000
        || matrix.minimums.range_bytes < 1_073_741_824
        || matrix.minimums.duration_ms < 2_000
        || matrix.minimums.formal_seed_count < 7
    {
        anyhow::bail!("matrix minimums are below the P3 statistical contract");
    }
    let mut case_ids = BTreeSet::new();
    for case in &matrix.cases {
        if !case_ids.insert(case.id.as_str()) {
            anyhow::bail!("duplicate case id {}", case.id);
        }
        match case.mode {
            EvalMode::SchedulerCore if case.access_bytes != 8 || case.band != EvalBand::Scalar => {
                anyhow::bail!("{} violates the P2B scalar 8-byte band", case.id)
            }
            EvalMode::Userfaultfd if case.access_bytes != 4096 || case.band != EvalBand::Range => {
                anyhow::bail!("{} violates the P4 page-range band", case.id)
            }
            _ => {}
        }
        if case.issue == EvalIssue::Demand && case.lookahead.unwrap_or(0) != 0 {
            anyhow::bail!("{} demand case must have lookahead=0", case.id);
        }
        if case.sweep_lookahead != (case.issue == EvalIssue::Lookahead) {
            anyhow::bail!("{} lookahead issue/sweep contract is inconsistent", case.id);
        }
    }
    for sweep in &matrix.sweeps {
        if sweep.id.trim().is_empty() || sweep.cases.is_empty() {
            anyhow::bail!("sweep ids and case lists must be non-empty");
        }
        if sweep.operations.is_some_and(|operations| operations == 0) {
            anyhow::bail!("sweep {} operations override must be non-zero", sweep.id);
        }
        if sweep.operations.is_some()
            && sweep.outcomes.iter().any(|outcome| {
                !matches!(outcome, OutcomeProfile::Error | OutcomeProfile::DropTimeout)
            })
        {
            anyhow::bail!(
                "sweep {} may override operations only for failure-semantics outcomes",
                sweep.id
            );
        }
        for case in &sweep.cases {
            if !case_ids.contains(case.as_str()) {
                anyhow::bail!("sweep {} references unknown case {case}", sweep.id);
            }
        }
        validate_subset(
            "model_latency_us",
            &sweep.model_latency_us,
            &matrix.factors.model_latency_us,
        )?;
        validate_subset(
            "jitter_profiles",
            &sweep.jitter_profiles,
            &matrix.factors.jitter_profiles,
        )?;
        validate_subset("outcomes", &sweep.outcomes, &matrix.factors.outcomes)?;
        validate_subset("coroutines", &sweep.coroutines, &matrix.factors.coroutines)?;
        validate_subset("inflight", &sweep.inflight, &matrix.factors.inflight)?;
        validate_subset("lookahead", &sweep.lookahead, &matrix.factors.lookahead)?;
        validate_subset("compute_us", &sweep.compute_us, &matrix.factors.compute_us)?;
        validate_subset("patterns", &sweep.patterns, &matrix.factors.patterns)?;
    }
    Ok(())
}

fn validate_subset<T: Ord + std::fmt::Debug>(
    name: &str,
    values: &[T],
    domain: &[T],
) -> anyhow::Result<()> {
    if values.is_empty() || values.iter().any(|value| !domain.contains(value)) {
        anyhow::bail!("sweep {name} must be a non-empty subset of its factor domain");
    }
    Ok(())
}

fn parse_seeds(value: &str) -> anyhow::Result<Vec<u64>> {
    let mut seeds = BTreeSet::new();

    for part in value.split(',') {
        if let Some((start, end)) = part.split_once("..") {
            let start = start.parse::<u64>().context("parse seed range start")?;
            let end = end.parse::<u64>().context("parse seed range end")?;
            if start == 0 || end < start || end - start > 1023 {
                anyhow::bail!("seed ranges must be positive, ascending, and at most 1024 wide");
            }
            seeds.extend(start..=end);
        } else {
            let seed = part.parse::<u64>().context("parse seed")?;
            if seed == 0 {
                anyhow::bail!("seeds must be non-zero");
            }
            seeds.insert(seed);
        }
    }
    if seeds.is_empty() {
        anyhow::bail!("--seeds must not be empty");
    }
    Ok(seeds.into_iter().collect())
}

fn expand_matrix(
    matrix: &EvalMatrix,
    scenario: &ScenarioConfig,
    scenario_path: &Path,
    bands: &BTreeSet<EvalBand>,
    seeds: &[u64],
    output_dir: &Path,
) -> anyhow::Result<Vec<ExpandedEvalCase>> {
    let run_namespace = eval_run_namespace(output_dir)?;
    let specs: BTreeMap<_, _> = matrix
        .cases
        .iter()
        .map(|case| (case.id.as_str(), case))
        .collect();
    let mut expanded = Vec::new();
    let mut model_paths: BTreeMap<String, PathBuf> = BTreeMap::new();
    let mut operation_hashes = BTreeMap::new();

    for sweep in &matrix.sweeps {
        for case_id in &sweep.cases {
            let spec = specs[case_id.as_str()];
            if !bands.contains(&spec.band) {
                continue;
            }
            let coroutines: Vec<u32> = if spec.mode == EvalMode::Userfaultfd {
                sweep
                    .coroutines
                    .iter()
                    .copied()
                    .filter(|value| matches!(value, 1 | 2 | 4 | 8))
                    .collect()
            } else if spec.mode.uses_coroutines() {
                sweep.coroutines.clone()
            } else {
                vec![1]
            };
            let inflight = if spec.mode.uses_inflight() {
                sweep.inflight.clone()
            } else {
                vec![1]
            };
            let lookahead: Vec<u32> = if spec.sweep_lookahead {
                sweep
                    .lookahead
                    .iter()
                    .copied()
                    .filter(|value| *value > 0)
                    .collect()
            } else {
                vec![spec.lookahead.unwrap_or(0)]
            };
            let compute_us = if spec.mode == EvalMode::Userfaultfd {
                vec![0]
            } else {
                sweep.compute_us.clone()
            };
            let patterns: Vec<EvalPattern> = sweep
                .patterns
                .iter()
                .copied()
                .filter(|pattern| {
                    *pattern != EvalPattern::Dependent
                        || (spec.mode != EvalMode::Userfaultfd && !spec.sweep_lookahead)
                })
                .filter(|pattern| {
                    *pattern != EvalPattern::Mixed || spec.mode != EvalMode::Userfaultfd
                })
                .collect();

            for &seed in seeds {
                for &latency in &sweep.model_latency_us {
                    for &jitter in &sweep.jitter_profiles {
                        for &outcome in &sweep.outcomes {
                            let model = model_for_case(
                                &scenario.remote_memory_model,
                                latency,
                                jitter,
                                outcome,
                                seed,
                            );
                            let manifest = build_model_manifest(scenario, model)?;
                            let model_path = if let Some(path) =
                                model_paths.get(&manifest.manifest_hash)
                            {
                                path.clone()
                            } else {
                                let path = output_dir
                                    .join("models")
                                    .join(format!("{}.json", safe_hash(&manifest.manifest_hash)));
                                write_json(&path, &manifest)?;
                                model_paths.insert(manifest.manifest_hash.clone(), path.clone());
                                path
                            };
                            let model_file_sha256 = sha256_file(&model_path)?;
                            for &coroutine_count in &coroutines {
                                for &inflight_count in &inflight {
                                    for &lookahead_count in &lookahead {
                                        if lookahead_count > inflight_count
                                            && spec.mode == EvalMode::AsyncPoll
                                        {
                                            continue;
                                        }
                                        for &compute in &compute_us {
                                            for &pattern in &patterns {
                                                let operations =
                                                    sweep.operations.unwrap_or_else(|| {
                                                        if spec.band == EvalBand::Scalar {
                                                            matrix.minimums.scalar_operations
                                                        } else {
                                                            matrix.minimums.range_bytes / PAGE_BYTES
                                                        }
                                                    });
                                                let deadline_us = match outcome {
                                                    OutcomeProfile::DropTimeout => {
                                                        latency.saturating_mul(20).max(1_000)
                                                    }
                                                    _ => 1_000_000,
                                                };
                                                let warmup = if matches!(
                                                    outcome,
                                                    OutcomeProfile::Error
                                                        | OutcomeProfile::DropTimeout
                                                ) {
                                                    0
                                                } else if spec.band == EvalBand::Scalar {
                                                    matrix.minimums.warmup_scalar_operations
                                                } else {
                                                    matrix.minimums.warmup_range_pages
                                                };
                                                let operation_hash_key = (
                                                    spec.band,
                                                    seed,
                                                    spec.access_bytes,
                                                    operations,
                                                    coroutine_count,
                                                    pattern,
                                                );
                                                let operation_list_hash = operation_hashes
                                                    .entry(operation_hash_key)
                                                    .or_insert_with(|| {
                                                        operation_list_hash(
                                                            spec.band,
                                                            seed,
                                                            spec.access_bytes,
                                                            operations,
                                                            coroutine_count,
                                                            pattern,
                                                        )
                                                    })
                                                    .clone();
                                                let identity = format!(
                                                    "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
                                                    run_namespace,
                                                    sweep.id,
                                                    spec.id,
                                                    seed,
                                                    latency,
                                                    enum_json(jitter)?,
                                                    enum_json(outcome)?,
                                                    coroutine_count,
                                                    inflight_count,
                                                    lookahead_count,
                                                    compute,
                                                    enum_json(pattern)?,
                                                    operation_list_hash,
                                                    deadline_us,
                                                );
                                                let run_id = format!(
                                                    "{}-{:016x}",
                                                    spec.id.to_ascii_lowercase(),
                                                    fnv1a64(identity.as_bytes())
                                                );
                                                let mut case = ExpandedEvalCase {
                                                    run_id,
                                                    sweep: sweep.id.clone(),
                                                    case_id: spec.id.clone(),
                                                    band: spec.band,
                                                    mode: spec.mode,
                                                    issue: spec.issue,
                                                    seed,
                                                    access_bytes: spec.access_bytes,
                                                    operations,
                                                    deadline_us,
                                                    warmup_operations: warmup,
                                                    minimum_duration_ms: matrix
                                                        .minimums
                                                        .duration_ms,
                                                    model_latency_us: latency,
                                                    jitter_profile: jitter,
                                                    outcome,
                                                    coroutines: coroutine_count,
                                                    inflight: inflight_count,
                                                    lookahead: lookahead_count,
                                                    compute_us: compute,
                                                    pattern,
                                                    extra_vcpus: spec.extra_vcpus,
                                                    diagnostic_trace_required: false,
                                                    operation_list_hash,
                                                    model_manifest_hash: manifest
                                                        .manifest_hash
                                                        .clone(),
                                                    model_file_sha256: model_file_sha256.clone(),
                                                    model_manifest_path: model_path
                                                        .display()
                                                        .to_string(),
                                                    order_index: 0,
                                                    command: Vec::new(),
                                                };
                                                case.command =
                                                    planned_command(&case, scenario, scenario_path);
                                                case.diagnostic_trace_required =
                                                    needs_diagnostic_trace(
                                                        &case,
                                                        seeds.first().copied(),
                                                    );
                                                expanded.push(case);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Ok(expanded)
}

fn model_for_case(
    base: &RemoteMemoryModelConfig,
    latency_us: u64,
    jitter: JitterProfile,
    outcome: OutcomeProfile,
    seed: u64,
) -> RemoteMemoryModelConfig {
    let fixed_latency_ns = latency_us.saturating_mul(1_000);
    let mut model = base.clone();

    model.enabled = true;
    model.fixed_latency_ns = fixed_latency_ns;
    model.jitter = match jitter {
        JitterProfile::Uniform10pct => RemoteMemoryJitterConfig {
            mode: RemoteMemoryJitterMode::Uniform,
            max_abs_ns: fixed_latency_ns / 10,
        },
        _ => RemoteMemoryJitterConfig::default(),
    };
    model.tail = match jitter {
        JitterProfile::Tail1pct10x => RemoteMemoryTailConfig {
            probability_ppm: 10_000,
            extra_latency_ns: fixed_latency_ns.saturating_mul(9),
        },
        _ => RemoteMemoryTailConfig::default(),
    };
    model.drop_ppm = 0;
    model.error_ppm = 0;
    model.duplicate_ppm = 0;
    match outcome {
        OutcomeProfile::Success => {}
        OutcomeProfile::Error => model.error_ppm = 1_000_000,
        OutcomeProfile::DropTimeout => model.drop_ppm = 1_000_000,
        OutcomeProfile::DuplicateLate => {
            model.duplicate_ppm = 1_000_000;
            model.duplicate_delay_ns = fixed_latency_ns.max(1_000).saturating_mul(10);
        }
    }
    model.seed = seed;
    model
}

fn build_model_manifest(
    scenario: &ScenarioConfig,
    remote_memory_model: RemoteMemoryModelConfig,
) -> anyhow::Result<ModelManifest> {
    let payload = ModelManifestPayload {
        schema: MODEL_MANIFEST_SCHEMA,
        scenario_name: scenario.scenario.name.clone(),
        scenario_seed: scenario.scenario.seed,
        remote_memory_model,
    };
    let canonical = serde_json::to_vec(&payload)?;
    Ok(ModelManifest {
        schema: payload.schema,
        manifest_hash: hash_bytes(&canonical),
        scenario_name: payload.scenario_name,
        scenario_seed: payload.scenario_seed,
        remote_memory_model: payload.remote_memory_model,
    })
}

fn planned_command(
    case: &ExpandedEvalCase,
    scenario: &ScenarioConfig,
    scenario_path: &Path,
) -> Vec<String> {
    let mut command = vec![
        "zsh".into(),
        "guest-linux/aarch64/scripts/run_ub_obmm_eval.sh".into(),
        "--node-count".into(),
        scenario.topology.hosts.to_string(),
        "--scenario-config".into(),
        scenario_path.display().to_string(),
        "--run-id".into(),
        case.run_id.clone(),
        "--remote-memory-model-manifest".into(),
        case.model_manifest_path.clone(),
        "--timeout-sec".into(),
        if case.band == EvalBand::Range {
            "900".into()
        } else {
            "300".into()
        },
    ];
    if case.mode == EvalMode::SchedulerCore {
        let scheduler = &scenario.scheduler_core_model;
        command.extend([
            "--scheduler-core-model".into(),
            format!(
                "v2|enabled={}|contexts={}|pending={}|events={}|clock_mhz={}",
                u8::from(scheduler.enabled),
                scheduler.context_entries,
                scheduler.pending_load_entries,
                scheduler.event_queue_depth,
                scheduler.clock_mhz,
            ),
        ]);
    }
    let common = format!(
        "--mode {} --access-bytes {} --pattern {} --iterations {} \
         --deadline-us {} --seed {} --verify --eval-band {} --eval-case {} \
         --expected-outcome {} \
         --warmup {} --min-duration-ms {}",
        case.mode.as_str(),
        case.access_bytes,
        case.pattern.as_str(),
        case.operations,
        case.deadline_us,
        case.seed,
        case.band.as_str(),
        case.case_id,
        case.outcome.as_str(),
        case.warmup_operations,
        case.minimum_duration_ms,
    );
    let guest_args = match case.mode {
        EvalMode::SyncMmio => format!(
            "{common} --coroutines {} --compute-us {}",
            case.coroutines, case.compute_us,
        ),
        EvalMode::AsyncPoll => format!(
            "{common} --coroutines {} --inflight {} --lookahead {} --compute-us {}",
            case.coroutines, case.inflight, case.lookahead, case.compute_us,
        ),
        EvalMode::SchedulerCore => format!(
            "{common} --coroutines {} --inflight 1 --lookahead 0 --compute-us {}",
            case.coroutines, case.compute_us,
        ),
        EvalMode::Userfaultfd => format!(
            "{common} --uffd-case missing-remote --worker-threads {} \
             --handler-cpu 0 --pages 512",
            case.coroutines,
        ),
    };
    command.extend(["--obmm-async-args".into(), guest_args]);
    command
}

fn operation_list_hash(
    band: EvalBand,
    seed: u64,
    access_bytes: u32,
    operations: u64,
    coroutines: u32,
    pattern: EvalPattern,
) -> String {
    let slots = (2 * 1024 * 1024_u64) / u64::from(access_bytes);
    let mut random_states: Vec<u64> = (0..coroutines)
        .map(|worker| seed ^ (u64::from(worker) << 32) ^ 1)
        .collect();
    let mut dependent_offsets: Vec<u64> = (0..coroutines).map(u64::from).collect();
    let mut hash = 0xcbf2_9ce4_8422_2325_u64;

    for ordinal in 0..operations {
        let worker = (ordinal % u64::from(coroutines)) as usize;
        let local_ordinal = ordinal / u64::from(coroutines);
        let slot = match pattern {
            EvalPattern::Random if access_bytes == 4096 => {
                logical_worker_page(seed, worker as u32, local_ordinal, coroutines, slots, true)
            }
            EvalPattern::Random => {
                random_states[worker] = xorshift64(random_states[worker]);
                random_states[worker] % slots
            }
            EvalPattern::Dependent => dependent_offsets[worker] % slots,
            EvalPattern::Sequential if access_bytes == 4096 => {
                logical_worker_page(seed, worker as u32, local_ordinal, coroutines, slots, false)
            }
            EvalPattern::Mixed if access_bytes == 4096 => {
                logical_worker_page(seed, worker as u32, local_ordinal, coroutines, slots, false)
            }
            EvalPattern::Sequential | EvalPattern::Mixed => ordinal % slots,
        };
        let offset = slot * u64::from(access_bytes);
        if pattern == EvalPattern::Dependent {
            dependent_offsets[worker] = payload_value(seed, offset, access_bytes);
        }
        let remote = pattern != EvalPattern::Mixed || local_ordinal & 1 == 1;
        for bytes in [
            u64::from(remote).to_le_bytes().as_slice(),
            (worker as u64).to_le_bytes().as_slice(),
            local_ordinal.to_le_bytes().as_slice(),
            1_u64.to_le_bytes().as_slice(),
            offset.to_le_bytes().as_slice(),
            access_bytes.to_le_bytes().as_slice(),
        ] {
            for byte in bytes {
                hash ^= u64::from(*byte);
                hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
            }
        }
    }
    let _ = band;
    format!("fnv1a64:{hash:016x}")
}

fn xorshift64(mut value: u64) -> u64 {
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    value
}

fn logical_gcd(mut left: u64, mut right: u64) -> u64 {
    while right != 0 {
        let remainder = left % right;
        left = right;
        right = remainder;
    }
    left
}

fn logical_page_index(seed: u64, ordinal: u64, pages: u64, random: bool) -> u64 {
    if pages == 0 {
        return 0;
    }
    let cycle = ordinal / pages;
    let position = ordinal % pages;
    if !random || pages == 1 {
        return position;
    }
    let mut multiplier = splitmix64(seed ^ cycle) % pages;
    if multiplier == 0 {
        multiplier = 1;
    }
    while logical_gcd(multiplier, pages) != 1 {
        multiplier += 1;
        if multiplier == pages {
            multiplier = 1;
        }
    }
    let addend = splitmix64(seed ^ cycle ^ 0xd1b5_4a32_d192_ed03) % pages;
    (position * multiplier + addend) % pages
}

fn logical_worker_page(
    seed: u64,
    worker: u32,
    local_ordinal: u64,
    workers: u32,
    pages: u64,
    random: bool,
) -> u64 {
    let pages_per_worker = pages / u64::from(workers);
    logical_page_index(
        seed ^ (u64::from(worker) << 32),
        local_ordinal,
        pages_per_worker,
        random,
    ) * u64::from(workers)
        + u64::from(worker)
}

fn payload_value(seed: u64, offset: u64, access_bytes: u32) -> u64 {
    let mut value = 0_u64;
    for index in 0..access_bytes.min(8) {
        let byte = ((seed.wrapping_add(offset + u64::from(index)))
            .wrapping_mul(0x9e37_79b9)
            .wrapping_add(0x85eb_ca77)
            & 0xff) as u8;
        value |= u64::from(byte) << (index * 8);
    }
    value
}

fn assign_deterministic_order(cases: &mut [ExpandedEvalCase]) {
    cases.sort_by_key(|case| {
        (
            case.seed,
            splitmix64(fnv1a64(case.run_id.as_bytes()) ^ case.seed),
            case.run_id.clone(),
        )
    });
    let mut indexes = BTreeMap::new();
    for case in cases {
        let index = indexes.entry(case.seed).or_insert(0_u64);
        case.order_index = *index;
        *index += 1;
    }
}

fn validate_gates(
    gate_dir: &Path,
    scenario_hash: &str,
) -> anyhow::Result<(bool, Vec<GateValidation>)> {
    let mut all_pass = true;
    let mut validations = Vec::new();
    let mut shared_model_contract_hash = None;

    for phase in ["p0", "p1", "p2a", "p2b", "p4"] {
        let path = gate_dir.join(format!("{phase}.json"));
        let mut reasons = Vec::new();
        let status = match fs::read(&path) {
            Ok(bytes) => match serde_json::from_slice::<GateEvidence>(&bytes) {
                Ok(evidence) => {
                    if evidence.schema != 1 {
                        reasons.push("schema must be 1".into());
                    }
                    if evidence.phase != phase {
                        reasons.push(format!("phase must be {phase}"));
                    }
                    if evidence.status != "pass" {
                        reasons.push("status is not pass".into());
                    }
                    if evidence.scenario_hash != scenario_hash {
                        reasons.push("scenario hash mismatch".into());
                    }
                    if evidence.model_contract_hash.is_empty() || evidence.evidence.is_empty() {
                        reasons.push("model hash and evidence list must be non-empty".into());
                    }
                    if let Some(expected) = shared_model_contract_hash.as_deref() {
                        if evidence.model_contract_hash != expected {
                            reasons.push("model contract hash differs across phase gates".into());
                        }
                    } else if !evidence.model_contract_hash.is_empty() {
                        shared_model_contract_hash = Some(evidence.model_contract_hash.clone());
                    }
                    for relative in &evidence.evidence {
                        let relative_path = Path::new(relative);
                        if relative_path.is_absolute()
                            || relative_path.components().any(|component| {
                                matches!(component, Component::ParentDir | Component::RootDir)
                            })
                        {
                            reasons.push(format!(
                                "evidence path must stay below gate dir: {relative}"
                            ));
                            continue;
                        }
                        let evidence_path = gate_dir.join(relative_path);
                        match fs::metadata(&evidence_path) {
                            Ok(metadata) if metadata.is_file() && metadata.len() > 0 => {}
                            Ok(_) => reasons.push(format!(
                                "evidence is not a non-empty file: {}",
                                evidence_path.display()
                            )),
                            Err(error) => reasons.push(format!(
                                "evidence is unavailable: {}: {error}",
                                evidence_path.display()
                            )),
                        }
                    }
                    if reasons.is_empty() {
                        "pass"
                    } else {
                        "invalid"
                    }
                }
                Err(error) => {
                    reasons.push(format!("decode failed: {error}"));
                    "invalid"
                }
            },
            Err(error) => {
                reasons.push(format!("missing: {error}"));
                "missing"
            }
        };
        if status != "pass" {
            all_pass = false;
        }
        validations.push(GateValidation {
            phase: phase.into(),
            path: path.display().to_string(),
            status: status.into(),
            reasons,
        });
    }
    Ok((all_pass, validations))
}

fn execute_cases(args: &ObmmEvalCliArgs, manifest: &EvalRunManifest) -> anyhow::Result<()> {
    let remote = args
        .remote_target
        .as_deref()
        .zip(args.remote_repo.as_deref());
    let execution_repo = args
        .local_repo
        .as_deref()
        .or_else(|| args.remote_repo.as_deref())
        .ok_or_else(|| anyhow::anyhow!("formal execution target is absent"))?;
    if args.output_dir.is_absolute() {
        anyhow::bail!("formal execution requires a repository-relative --output-dir");
    }
    ensure_raw_targets_are_new(&args.output_dir, manifest)?;
    if let Some((target, remote_repo)) = remote {
        for model_path in manifest
            .cases
            .iter()
            .map(|case| case.model_manifest_path.as_str())
            .collect::<BTreeSet<_>>()
        {
            let local_path = Path::new(model_path);
            if local_path.is_absolute() {
                anyhow::bail!("model path {model_path} must be repository-relative");
            }
            stage_remote_file(target, &remote_repo.join(local_path), local_path)?;
        }
    }
    for (index, case) in manifest.cases.iter().enumerate() {
        println!(
            "{}",
            eval_progress_line(&case.run_id, index, manifest.cases.len(), "dispatch", None,)
        );
        let host_before = if let Some((target, _)) = remote {
            collect_remote_host_evidence(target)?
        } else {
            collect_local_host_evidence()?
        };
        let started = Instant::now();
        let timeout = remote_case_timeout(&case.command)?;
        let output = execute_case_command(remote, execution_repo, &case.command, timeout)
            .with_context(|| format!("dispatch {}", case.run_id))?
            .ok_or_else(|| {
                anyhow::anyhow!(
                    "case {} exceeded its {} second outer deadline",
                    case.run_id,
                    timeout.as_secs()
                )
            })?;
        let elapsed_ns = u64::try_from(started.elapsed().as_nanos()).unwrap_or(u64::MAX);
        let stdout = String::from_utf8_lossy(&output.stdout);
        let stderr = String::from_utf8_lossy(&output.stderr);
        let mut initial_reasons = Vec::new();
        let summary = match parse_guest_summary(&stdout) {
            Ok(summary) => Some(summary),
            Err(error) => {
                initial_reasons.push(error.to_string());
                None
            }
        };
        let mut diagnostic_summary = None;
        let mut diagnostic_stdout = String::new();
        let mut diagnostic_stderr = String::new();
        if case.diagnostic_trace_required {
            let diagnostic_parts = diagnostic_trace_command(case);
            let diagnostic_timeout = remote_case_timeout(&diagnostic_parts)?;
            let diagnostic_output = execute_case_command(
                remote,
                execution_repo,
                &diagnostic_parts,
                diagnostic_timeout,
            )
            .with_context(|| format!("dispatch diagnostic trace {}", case.run_id))?
            .ok_or_else(|| {
                anyhow::anyhow!(
                    "diagnostic trace {} exceeded its {} second outer deadline",
                    case.run_id,
                    diagnostic_timeout.as_secs()
                )
            })?;
            diagnostic_stdout = String::from_utf8_lossy(&diagnostic_output.stdout).into_owned();
            diagnostic_stderr = String::from_utf8_lossy(&diagnostic_output.stderr).into_owned();
            match parse_guest_summary(&diagnostic_stdout) {
                Ok(diagnostic) => {
                    if diagnostic_output.status.code() != Some(0) {
                        initial_reasons
                            .push("diagnostic trace process did not exit successfully".into());
                    }
                    if let Some(timed) = summary.as_ref() {
                        validate_diagnostic_trace(timed, &diagnostic, &mut initial_reasons);
                    }
                    diagnostic_summary = Some(diagnostic);
                }
                Err(error) => initial_reasons.push(format!("diagnostic trace summary: {error}")),
            }
        }
        let record = RawRunRecord {
            schema: 1,
            kind: "run".into(),
            run_id: case.run_id.clone(),
            exit_code: output.status.code(),
            host: HostEvidence {
                elapsed_ns,
                load1_milli: host_before.0,
                online_cpus: host_before.1,
            },
            summary,
            diagnostic_summary,
            initial_reasons,
        };
        let combined_stdout = if diagnostic_stdout.is_empty() {
            stdout.into_owned()
        } else {
            format!(
                "{}\nOBMM_DIAGNOSTIC_TRACE_BEGIN sample_ppm=10000\n{}",
                stdout, diagnostic_stdout
            )
        };
        let combined_stderr = if diagnostic_stderr.is_empty() {
            stderr.into_owned()
        } else {
            format!(
                "{}\nOBMM_DIAGNOSTIC_TRACE_STDERR_BEGIN\n{}",
                stderr, diagnostic_stderr
            )
        };
        write_raw_run(
            &args
                .output_dir
                .join("raw")
                .join(format!("{}.jsonl", case.run_id)),
            &record,
            &combined_stdout,
            &combined_stderr,
        )?;
        println!(
            "{}",
            eval_progress_line(
                &case.run_id,
                index + 1,
                manifest.cases.len(),
                "collected",
                output.status.code(),
            )
        );
    }
    Ok(())
}

fn ensure_raw_targets_are_new(output_dir: &Path, manifest: &EvalRunManifest) -> anyhow::Result<()> {
    let existing_raw = manifest
        .cases
        .iter()
        .map(|case| {
            output_dir
                .join("raw")
                .join(format!("{}.jsonl", case.run_id))
        })
        .filter(|path| path.exists())
        .collect::<Vec<_>>();
    if !existing_raw.is_empty() {
        anyhow::bail!(
            "refusing to overwrite {} existing raw run(s), beginning with {}; use a new \
             --output-dir so reruns receive a new run ID",
            existing_raw.len(),
            existing_raw[0].display()
        );
    }
    Ok(())
}

fn eval_progress_line(
    run_id: &str,
    completed: usize,
    total: usize,
    stage: &str,
    exit_code: Option<i32>,
) -> String {
    format!(
        "OBMM_EVAL_PROGRESS schema=1 completed={completed} total={total} \
         run_id={run_id} stage={stage} exit_code={}",
        exit_code.map_or_else(|| "na".to_string(), |value| value.to_string())
    )
}

fn remote_case_command(remote_repo: &Path, command: &[String]) -> String {
    format!(
        "cd {} && if [ -f guest-linux/aarch64/remote-build.env ]; then \
         . guest-linux/aarch64/remote-build.env; fi && {}",
        shell_quote(&remote_repo.display().to_string()),
        command
            .iter()
            .map(|part| shell_quote(part))
            .collect::<Vec<_>>()
            .join(" ")
    )
}

fn execute_case_command(
    remote: Option<(&str, &Path)>,
    execution_repo: &Path,
    command: &[String],
    timeout: Duration,
) -> anyhow::Result<Option<TimedCommandOutput>> {
    let shell_command = remote_case_command(execution_repo, command);
    if let Some((target, _)) = remote {
        remote_command_with_connect_retry(target, &shell_command, timeout)
    } else {
        let mut local = Command::new("zsh");
        local.arg("-c").arg(shell_command);
        command_full_output_with_timeout(&mut local, timeout)
    }
}

fn needs_diagnostic_trace(case: &ExpandedEvalCase, first_seed: Option<u64>) -> bool {
    case.sweep == "correctness"
        && Some(case.seed) == first_seed
        && case.model_latency_us == 100
        && case.jitter_profile == JitterProfile::None
        && case.outcome == OutcomeProfile::Success
        && case.coroutines == 8
        && case.pattern == EvalPattern::Sequential
        && case.compute_us
            == if case.mode == EvalMode::Userfaultfd {
                0
            } else {
                100
            }
        && (case.mode != EvalMode::AsyncPoll || case.inflight == 32)
        && (case.issue != EvalIssue::Lookahead || case.lookahead == 16)
}

fn diagnostic_trace_command(case: &ExpandedEvalCase) -> Vec<String> {
    let mut command = case.command.clone();
    for index in 0..command.len().saturating_sub(1) {
        if command[index] == "--run-id" {
            command[index + 1] = format!("{}-trace", case.run_id);
        }
        if command[index] == "--obmm-async-args" {
            command[index + 1].push_str(" --trace-sample-ppm 10000");
        }
    }
    command
}

fn validate_diagnostic_trace(
    timed: &GuestEvalSummary,
    diagnostic: &GuestEvalSummary,
    reasons: &mut Vec<String>,
) {
    if diagnostic.trace_sample_ppm != 10_000
        || diagnostic.trace_sampled == 0
        || diagnostic.trace_dropped != 0
    {
        reasons.push("diagnostic trace sampling contract failed".into());
    }
    if timed.band != diagnostic.band
        || timed.mode != diagnostic.mode
        || timed.case_id != diagnostic.case_id
        || timed.seed != diagnostic.seed
        || timed.operations != diagnostic.operations
        || timed.checksum != diagnostic.checksum
        || timed.failures != diagnostic.failures
        || timed.timeouts != diagnostic.timeouts
        || timed.status != diagnostic.status
    {
        reasons.push("diagnostic trace changed workload counters or outcome".into());
    }
}

fn stage_remote_file(target: &str, remote_path: &Path, local_path: &Path) -> anyhow::Result<()> {
    let bytes = fs::read(local_path)
        .with_context(|| format!("read staged file {}", local_path.display()))?;
    let parent = remote_path
        .parent()
        .ok_or_else(|| anyhow::anyhow!("remote path has no parent"))?;
    let command = format!(
        "mkdir -p {} && cat > {}",
        shell_quote(&parent.display().to_string()),
        shell_quote(&remote_path.display().to_string())
    );
    let mut remote = ssh_command(target);
    configure_process_group(&mut remote);
    let mut child = remote
        .arg(command)
        .stdin(Stdio::piped())
        .spawn()
        .with_context(|| format!("stage {} on {target}", local_path.display()))?;
    child
        .stdin
        .take()
        .ok_or_else(|| anyhow::anyhow!("ssh staging stdin is unavailable"))?
        .write_all(&bytes)?;
    let status = wait_child_with_timeout(&mut child, REMOTE_STAGE_TIMEOUT)?.ok_or_else(|| {
        anyhow::anyhow!(
            "staging {} on {target} exceeded {} seconds",
            local_path.display(),
            REMOTE_STAGE_TIMEOUT.as_secs()
        )
    })?;
    if !status.success() {
        anyhow::bail!("staging {} on {target} failed", local_path.display());
    }
    Ok(())
}

fn collect_remote_host_evidence(target: &str) -> anyhow::Result<(Option<u64>, Option<u32>)> {
    let mut command = ssh_command(target);
    command
        .arg("getconf _NPROCESSORS_ONLN; cat /proc/loadavg")
        .stderr(Stdio::null());
    let Some((status, stdout)) = command_output_with_timeout(&mut command, HOST_EVIDENCE_TIMEOUT)
        .with_context(|| format!("collect host evidence from {target}"))?
    else {
        eprintln!(
            "OBMM_EVAL_HOST_EVIDENCE schema=1 target={target} status=timeout timeout_ms={}",
            HOST_EVIDENCE_TIMEOUT.as_millis()
        );
        return Ok((None, None));
    };
    if !status.success() {
        return Ok((None, None));
    }
    let text = String::from_utf8_lossy(&stdout);
    let mut lines = text.lines();
    let online_cpus = lines.next().and_then(|line| line.trim().parse().ok());
    let load1_milli = lines
        .next()
        .and_then(|line| line.split_whitespace().next())
        .and_then(parse_decimal_milli);
    Ok((load1_milli, online_cpus))
}

fn collect_local_host_evidence() -> anyhow::Result<(Option<u64>, Option<u32>)> {
    let mut command = Command::new("zsh");
    command
        .args(["-c", "getconf _NPROCESSORS_ONLN; cat /proc/loadavg"])
        .stderr(Stdio::null());
    let Some((status, stdout)) = command_output_with_timeout(&mut command, HOST_EVIDENCE_TIMEOUT)?
    else {
        eprintln!(
            "OBMM_EVAL_HOST_EVIDENCE schema=1 target=local status=timeout timeout_ms={}",
            HOST_EVIDENCE_TIMEOUT.as_millis()
        );
        return Ok((None, None));
    };
    if !status.success() {
        return Ok((None, None));
    }
    let text = String::from_utf8_lossy(&stdout);
    let mut lines = text.lines();
    let online_cpus = lines.next().and_then(|line| line.trim().parse().ok());
    let load1_milli = lines
        .next()
        .and_then(|line| line.split_whitespace().next())
        .and_then(parse_decimal_milli);
    Ok((load1_milli, online_cpus))
}

fn ssh_command(target: &str) -> Command {
    let mut command = Command::new("ssh");
    command
        .args([
            "-o",
            "ConnectTimeout=15",
            "-o",
            "ControlMaster=no",
            "-o",
            "ControlPath=none",
            "-o",
            "ServerAliveInterval=15",
            "-o",
            "ServerAliveCountMax=3",
        ])
        .arg(target);
    command
}

fn remote_command_with_connect_retry(
    target: &str,
    remote_command: &str,
    timeout: Duration,
) -> anyhow::Result<Option<TimedCommandOutput>> {
    let mut retry_markers = Vec::new();

    for attempt in 1..=REMOTE_CONNECT_ATTEMPTS {
        let mut command = ssh_command(target);
        command.arg(remote_command);
        let Some(mut output) = command_full_output_with_timeout(&mut command, timeout)? else {
            return Ok(None);
        };
        if !retryable_ssh_connect_failure(output.status.code(), &output.stdout, &output.stderr)
            || attempt == REMOTE_CONNECT_ATTEMPTS
        {
            if !retry_markers.is_empty() {
                let mut stderr = retry_markers.join("\n").into_bytes();
                stderr.push(b'\n');
                stderr.extend_from_slice(&output.stderr);
                output.stderr = stderr;
            }
            return Ok(Some(output));
        }
        let detail = String::from_utf8_lossy(&output.stderr)
            .lines()
            .collect::<Vec<_>>()
            .join(" | ");
        let marker = format!(
            "OBMM_EVAL_SSH_RETRY schema=1 attempt={attempt} next_attempt={} detail={detail}",
            attempt + 1
        );
        eprintln!("{marker}");
        retry_markers.push(marker);
        thread::sleep(REMOTE_CONNECT_RETRY_DELAY);
    }
    unreachable!("bounded SSH retry loop must return")
}

fn retryable_ssh_connect_failure(exit_code: Option<i32>, stdout: &[u8], stderr: &[u8]) -> bool {
    if exit_code != Some(255) || !stdout.is_empty() {
        return false;
    }
    let stderr = String::from_utf8_lossy(stderr);
    [
        "Connection timed out during banner exchange",
        "kex_exchange_identification:",
        "ssh_exchange_identification:",
        "Connection refused",
    ]
    .iter()
    .any(|pattern| stderr.contains(pattern))
}

fn remote_case_timeout(command: &[String]) -> anyhow::Result<Duration> {
    let runner_seconds = command
        .windows(2)
        .find_map(|pair| (pair[0] == "--timeout-sec").then_some(pair[1].as_str()))
        .ok_or_else(|| anyhow::anyhow!("remote case command is missing --timeout-sec"))?
        .parse::<u64>()
        .context("remote case --timeout-sec must be an unsigned integer")?;
    Duration::from_secs(runner_seconds)
        .checked_add(REMOTE_CASE_TIMEOUT_MARGIN)
        .ok_or_else(|| anyhow::anyhow!("remote case timeout overflows Duration"))
}

#[derive(Debug)]
struct TimedCommandOutput {
    status: ExitStatus,
    stdout: Vec<u8>,
    stderr: Vec<u8>,
}

fn wait_child_with_timeout(
    child: &mut std::process::Child,
    timeout: Duration,
) -> anyhow::Result<Option<ExitStatus>> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(status) = child.try_wait()? {
            return Ok(Some(status));
        }
        if Instant::now() >= deadline {
            terminate_child_tree(child)?;
            child.wait()?;
            return Ok(None);
        }
        thread::sleep(Duration::from_millis(20));
    }
}

fn command_full_output_with_timeout(
    command: &mut Command,
    timeout: Duration,
) -> anyhow::Result<Option<TimedCommandOutput>> {
    configure_process_group(command);
    let mut child = command
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    let mut stdout = child
        .stdout
        .take()
        .ok_or_else(|| anyhow::anyhow!("timed command stdout was not captured"))?;
    let mut stderr = child
        .stderr
        .take()
        .ok_or_else(|| anyhow::anyhow!("timed command stderr was not captured"))?;
    let stdout_reader = thread::spawn(move || {
        let mut bytes = Vec::new();
        stdout.read_to_end(&mut bytes).map(|_| bytes)
    });
    let stderr_reader = thread::spawn(move || {
        let mut bytes = Vec::new();
        stderr.read_to_end(&mut bytes).map(|_| bytes)
    });
    let status = wait_child_with_timeout(&mut child, timeout)?;
    let stdout = stdout_reader
        .join()
        .map_err(|_| anyhow::anyhow!("timed command stdout reader panicked"))??;
    let stderr = stderr_reader
        .join()
        .map_err(|_| anyhow::anyhow!("timed command stderr reader panicked"))??;
    Ok(status.map(|status| TimedCommandOutput {
        status,
        stdout,
        stderr,
    }))
}

#[cfg(unix)]
fn configure_process_group(command: &mut Command) {
    command.process_group(0);
}

#[cfg(not(unix))]
fn configure_process_group(_command: &mut Command) {}

#[cfg(unix)]
fn terminate_child_tree(child: &mut std::process::Child) -> anyhow::Result<()> {
    let process_group = i32::try_from(child.id()).context("child PID does not fit pid_t")?;
    let result = unsafe { libc::kill(-process_group, libc::SIGKILL) };
    if result == 0 {
        return Ok(());
    }
    let error = std::io::Error::last_os_error();
    if error.raw_os_error() == Some(libc::ESRCH) {
        Ok(())
    } else {
        Err(error.into())
    }
}

#[cfg(not(unix))]
fn terminate_child_tree(child: &mut std::process::Child) -> anyhow::Result<()> {
    match child.kill() {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::InvalidInput => Ok(()),
        Err(error) => Err(error.into()),
    }
}

fn command_output_with_timeout(
    command: &mut Command,
    timeout: Duration,
) -> anyhow::Result<Option<(ExitStatus, Vec<u8>)>> {
    Ok(command_full_output_with_timeout(command, timeout)?
        .map(|output| (output.status, output.stdout)))
}

fn parse_decimal_milli(value: &str) -> Option<u64> {
    let (whole, fraction) = value.split_once('.').unwrap_or((value, ""));
    let whole = whole.parse::<u64>().ok()?;
    let mut fraction = fraction.chars().take(3).collect::<String>();
    while fraction.len() < 3 {
        fraction.push('0');
    }
    Some(whole.saturating_mul(1_000) + fraction.parse::<u64>().ok()?)
}

fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

fn write_raw_run(
    path: &Path,
    record: &RawRunRecord,
    stdout: &str,
    stderr: &str,
) -> anyhow::Result<()> {
    let mut file = fs::File::create(path)
        .with_context(|| format!("create raw evidence {}", path.display()))?;
    serde_json::to_writer(&mut file, record)?;
    file.write_all(b"\n")?;
    for (stream, text) in [("stdout", stdout), ("stderr", stderr)] {
        for line in text.lines() {
            serde_json::to_writer(
                &mut file,
                &RawOutputLine {
                    schema: 1,
                    kind: "output",
                    stream,
                    line,
                },
            )?;
            file.write_all(b"\n")?;
        }
    }
    Ok(())
}

fn parse_guest_summary(output: &str) -> anyhow::Result<GuestEvalSummary> {
    let lines: Vec<_> = output
        .lines()
        .filter(|line| line.starts_with("OBMM_EVAL_SUMMARY "))
        .collect();
    if lines.len() != 1 {
        anyhow::bail!(
            "expected exactly one OBMM_EVAL_SUMMARY, found {}",
            lines.len()
        );
    }
    let fields: BTreeMap<_, _> = lines[0]
        .split_whitespace()
        .skip(1)
        .filter_map(|field| field.split_once('='))
        .collect();
    let required = |name: &str| -> anyhow::Result<&str> {
        fields
            .get(name)
            .copied()
            .ok_or_else(|| anyhow::anyhow!("OBMM_EVAL_SUMMARY missing {name}"))
    };
    let parse_u64_field = |name: &str| -> anyhow::Result<u64> {
        required(name)?
            .parse::<u64>()
            .with_context(|| format!("parse OBMM_EVAL_SUMMARY {name}"))
    };
    let model_wait = required("model_wait_ns")?;
    Ok(GuestEvalSummary {
        schema: parse_u64_field("schema")?.try_into()?,
        band: required("band")?.into(),
        mode: required("mode")?.into(),
        case_id: required("case")?.into(),
        seed: parse_u64_field("seed")?,
        operations: parse_u64_field("operations")?,
        checksum: required("checksum")?.into(),
        failures: parse_u64_field("failures")?,
        timeouts: parse_u64_field("timeouts")?,
        guest_ns_p50: parse_u64_field("guest_ns_p50")?,
        guest_ns_p95: parse_u64_field("guest_ns_p95")?,
        guest_ns_p99: parse_u64_field("guest_ns_p99")?,
        guest_ns_max: parse_u64_field("guest_ns_max")?,
        makespan_ns: parse_u64_field("makespan_ns")?,
        model_wait_ns: if model_wait == "na" {
            None
        } else {
            Some(model_wait.parse().context("parse model_wait_ns")?)
        },
        useful_work_ns: parse_u64_field("useful_work_ns")?,
        application_cpu_ns: parse_u64_field("application_cpu_ns")?,
        helper_cpu_ns: parse_u64_field("helper_cpu_ns")?,
        extra_vcpus: parse_u64_field("extra_vcpus")?.try_into()?,
        trace_sample_ppm: parse_u64_field("trace_sample_ppm")?.try_into()?,
        trace_sampled: parse_u64_field("trace_sampled")?,
        trace_dropped: parse_u64_field("trace_dropped")?,
        fail_closed_process_exit: match parse_u64_field("fail_closed_process_exit")? {
            0 => false,
            1 => true,
            _ => anyhow::bail!("fail_closed_process_exit must be 0 or 1"),
        },
        phase: GuestPhaseMetrics {
            ready_ns: parse_u64_field("ready_ns")?,
            wait_ns: parse_u64_field("wait_ns")?,
            idle_ns: parse_u64_field("idle_ns")?,
            no_ready: parse_u64_field("no_ready")?,
            submit_ns_p50: parse_u64_field("submit_ns_p50")?,
            submit_ns_total: parse_u64_field("submit_ns_total")?,
            switch_ns_p50: parse_u64_field("switch_ns_p50")?,
            switch_ns_total: parse_u64_field("switch_ns_total")?,
            cq_drain_ns_p50: parse_u64_field("cq_drain_ns_p50")?,
            cq_drain_ns_total: parse_u64_field("cq_drain_ns_total")?,
            configured_lookahead: parse_u64_field("configured_lookahead")?.try_into()?,
            backend_pending_high: parse_u64_field("backend_pending_high")?,
            backend_capacity: parse_u64_field("backend_capacity")?,
            sink_copy_bytes: parse_u64_field("sink_copy_bytes")?,
            sink_copy_ns: parse_u64_field("sink_copy_ns")?,
            backend_late: parse_u64_field("backend_late")?,
            backend_duplicate: parse_u64_field("backend_duplicate")?,
            scc_save_cycles: parse_u64_field("scc_save_cycles")?,
            scc_schedule_cycles: parse_u64_field("scc_schedule_cycles")?,
            scc_restore_cycles: parse_u64_field("scc_restore_cycles")?,
            scc_commit_cycles: parse_u64_field("scc_commit_cycles")?,
            el0_upcalls_pending: parse_u64_field("el0_upcalls_pending")?,
            el0_upcalls_complete: parse_u64_field("el0_upcalls_complete")?,
            el0_upcalls_fault: parse_u64_field("el0_upcalls_fault")?,
            el0_context_saves: parse_u64_field("el0_context_saves")?,
            el0_context_restores: parse_u64_field("el0_context_restores")?,
            el0_context_switches: parse_u64_field("el0_context_switches")?,
            el0_context_bytes: parse_u64_field("el0_context_bytes")?,
            el0_scheduler_ns: parse_u64_field("el0_scheduler_ns")?,
            el0_no_ready_waits: parse_u64_field("el0_no_ready_waits")?,
            direct_el0_upcalls: parse_u64_field("direct_el0_upcalls")?,
            qemu_context_saves: parse_u64_field("qemu_context_saves")?,
            qemu_context_restores: parse_u64_field("qemu_context_restores")?,
            qemu_context_switches: parse_u64_field("qemu_context_switches")?,
            qemu_context_bytes: parse_u64_field("qemu_context_bytes")?,
            uffd_fault_ns_p50: parse_u64_field("uffd_fault_ns_p50")?,
            uffd_fault_ns_p95: parse_u64_field("uffd_fault_ns_p95")?,
            uffd_fault_ns_p99: parse_u64_field("uffd_fault_ns_p99")?,
            uffd_fault_ns_max: parse_u64_field("uffd_fault_ns_max")?,
            uffd_remote_ns_p50: parse_u64_field("uffd_remote_ns_p50")?,
            uffd_remote_ns_p95: parse_u64_field("uffd_remote_ns_p95")?,
            uffd_remote_ns_p99: parse_u64_field("uffd_remote_ns_p99")?,
            uffd_remote_ns_max: parse_u64_field("uffd_remote_ns_max")?,
            uffd_copy_ns_p50: parse_u64_field("uffd_copy_ns_p50")?,
            uffd_copy_ns_p95: parse_u64_field("uffd_copy_ns_p95")?,
            uffd_copy_ns_p99: parse_u64_field("uffd_copy_ns_p99")?,
            uffd_copy_ns_max: parse_u64_field("uffd_copy_ns_max")?,
            uffd_wake_ns_p50: parse_u64_field("uffd_wake_ns_p50")?,
            uffd_wake_ns_p95: parse_u64_field("uffd_wake_ns_p95")?,
            uffd_wake_ns_p99: parse_u64_field("uffd_wake_ns_p99")?,
            uffd_wake_ns_max: parse_u64_field("uffd_wake_ns_max")?,
            uffd_handler_cpu_ns: parse_u64_field("uffd_handler_cpu_ns")?,
            uffd_worker_cpu_ns: parse_u64_field("uffd_worker_cpu_ns")?,
            model_pending_final: parse_u64_field("model_pending_final")?,
            backend_pending_final: parse_u64_field("backend_pending_final")?,
            scc_pending_final: parse_u64_field("scc_pending_final")?,
            counter_overflow: parse_u64_field("counter_overflow")?,
            clock_regressions: parse_u64_field("clock_regressions")?,
        },
        status: required("status")?.into(),
    })
}

struct CollectedRun {
    case_index: usize,
    record: Option<RawRunRecord>,
    artifact_fingerprint: Option<String>,
    reasons: Vec<String>,
}

#[derive(Clone)]
struct AggregateRow {
    case: ExpandedEvalCase,
    valid_seeds: usize,
    checksum_set: String,
    success_count_median: Option<u64>,
    failure_count_median: Option<u64>,
    timeout_count_median: Option<u64>,
    requests_per_second: Option<f64>,
    bytes_per_second: Option<f64>,
    makespan_median: Option<u64>,
    makespan_min: Option<u64>,
    makespan_max: Option<u64>,
    host_elapsed_median: Option<u64>,
    ci95: Option<(u64, u64)>,
    guest_p50_median: Option<u64>,
    guest_p95_median: Option<u64>,
    guest_p99_median: Option<u64>,
    guest_max_median: Option<u64>,
    model_wait_median: Option<u64>,
    useful_work_median: Option<u64>,
    application_cpu_median: Option<u64>,
    helper_cpu_median: Option<u64>,
    phase_medians: BTreeMap<&'static str, u64>,
    mechanism_gain_ns: Option<i128>,
    schedule_ahead_gain_ns: Option<i128>,
    overlap_efficiency: Option<f64>,
    core_efficiency: Option<f64>,
    status: String,
}

const PHASE_METRIC_FIELDS: [&str; 53] = [
    "ready_ns",
    "wait_ns",
    "idle_ns",
    "no_ready",
    "submit_ns_p50",
    "submit_ns_total",
    "switch_ns_p50",
    "switch_ns_total",
    "cq_drain_ns_p50",
    "cq_drain_ns_total",
    "backend_pending_high",
    "backend_capacity",
    "sink_copy_bytes",
    "sink_copy_ns",
    "backend_late",
    "backend_duplicate",
    "scc_save_cycles",
    "scc_schedule_cycles",
    "scc_restore_cycles",
    "scc_commit_cycles",
    "el0_upcalls_pending",
    "el0_upcalls_complete",
    "el0_upcalls_fault",
    "el0_context_saves",
    "el0_context_restores",
    "el0_context_switches",
    "el0_context_bytes",
    "el0_scheduler_ns",
    "el0_no_ready_waits",
    "direct_el0_upcalls",
    "qemu_context_saves",
    "qemu_context_restores",
    "qemu_context_switches",
    "qemu_context_bytes",
    "uffd_fault_ns_p50",
    "uffd_fault_ns_p95",
    "uffd_fault_ns_p99",
    "uffd_fault_ns_max",
    "uffd_remote_ns_p50",
    "uffd_remote_ns_p95",
    "uffd_remote_ns_p99",
    "uffd_remote_ns_max",
    "uffd_copy_ns_p50",
    "uffd_copy_ns_p95",
    "uffd_copy_ns_p99",
    "uffd_copy_ns_max",
    "uffd_wake_ns_p50",
    "uffd_wake_ns_p95",
    "uffd_wake_ns_p99",
    "uffd_wake_ns_max",
    "uffd_handler_cpu_ns",
    "uffd_worker_cpu_ns",
    "configured_lookahead",
];

fn phase_metric(metrics: &GuestPhaseMetrics, name: &str) -> u64 {
    match name {
        "ready_ns" => metrics.ready_ns,
        "wait_ns" => metrics.wait_ns,
        "idle_ns" => metrics.idle_ns,
        "no_ready" => metrics.no_ready,
        "submit_ns_p50" => metrics.submit_ns_p50,
        "submit_ns_total" => metrics.submit_ns_total,
        "switch_ns_p50" => metrics.switch_ns_p50,
        "switch_ns_total" => metrics.switch_ns_total,
        "cq_drain_ns_p50" => metrics.cq_drain_ns_p50,
        "cq_drain_ns_total" => metrics.cq_drain_ns_total,
        "backend_pending_high" => metrics.backend_pending_high,
        "backend_capacity" => metrics.backend_capacity,
        "sink_copy_bytes" => metrics.sink_copy_bytes,
        "sink_copy_ns" => metrics.sink_copy_ns,
        "backend_late" => metrics.backend_late,
        "backend_duplicate" => metrics.backend_duplicate,
        "scc_save_cycles" => metrics.scc_save_cycles,
        "scc_schedule_cycles" => metrics.scc_schedule_cycles,
        "scc_restore_cycles" => metrics.scc_restore_cycles,
        "scc_commit_cycles" => metrics.scc_commit_cycles,
        "el0_upcalls_pending" => metrics.el0_upcalls_pending,
        "el0_upcalls_complete" => metrics.el0_upcalls_complete,
        "el0_upcalls_fault" => metrics.el0_upcalls_fault,
        "el0_context_saves" => metrics.el0_context_saves,
        "el0_context_restores" => metrics.el0_context_restores,
        "el0_context_switches" => metrics.el0_context_switches,
        "el0_context_bytes" => metrics.el0_context_bytes,
        "el0_scheduler_ns" => metrics.el0_scheduler_ns,
        "el0_no_ready_waits" => metrics.el0_no_ready_waits,
        "direct_el0_upcalls" => metrics.direct_el0_upcalls,
        "qemu_context_saves" => metrics.qemu_context_saves,
        "qemu_context_restores" => metrics.qemu_context_restores,
        "qemu_context_switches" => metrics.qemu_context_switches,
        "qemu_context_bytes" => metrics.qemu_context_bytes,
        "uffd_fault_ns_p50" => metrics.uffd_fault_ns_p50,
        "uffd_fault_ns_p95" => metrics.uffd_fault_ns_p95,
        "uffd_fault_ns_p99" => metrics.uffd_fault_ns_p99,
        "uffd_fault_ns_max" => metrics.uffd_fault_ns_max,
        "uffd_remote_ns_p50" => metrics.uffd_remote_ns_p50,
        "uffd_remote_ns_p95" => metrics.uffd_remote_ns_p95,
        "uffd_remote_ns_p99" => metrics.uffd_remote_ns_p99,
        "uffd_remote_ns_max" => metrics.uffd_remote_ns_max,
        "uffd_copy_ns_p50" => metrics.uffd_copy_ns_p50,
        "uffd_copy_ns_p95" => metrics.uffd_copy_ns_p95,
        "uffd_copy_ns_p99" => metrics.uffd_copy_ns_p99,
        "uffd_copy_ns_max" => metrics.uffd_copy_ns_max,
        "uffd_wake_ns_p50" => metrics.uffd_wake_ns_p50,
        "uffd_wake_ns_p95" => metrics.uffd_wake_ns_p95,
        "uffd_wake_ns_p99" => metrics.uffd_wake_ns_p99,
        "uffd_wake_ns_max" => metrics.uffd_wake_ns_max,
        "uffd_handler_cpu_ns" => metrics.uffd_handler_cpu_ns,
        "uffd_worker_cpu_ns" => metrics.uffd_worker_cpu_ns,
        "configured_lookahead" => u64::from(metrics.configured_lookahead),
        _ => unreachable!("unknown phase metric {name}"),
    }
}

fn aggregate_results(
    output_dir: &Path,
    manifest: &EvalRunManifest,
    matrix: &EvalMatrix,
    bands: &BTreeSet<EvalBand>,
) -> anyhow::Result<Vec<RunValidation>> {
    let mut collected = Vec::with_capacity(manifest.cases.len());
    for (case_index, case) in manifest.cases.iter().enumerate() {
        let path = output_dir
            .join("raw")
            .join(format!("{}.jsonl", case.run_id));
        match fs::read_to_string(&path) {
            Ok(evidence) => {
                let first = evidence.lines().next().unwrap_or_default();
                match serde_json::from_str::<RawRunRecord>(first) {
                    Ok(record) => {
                        let reasons = validate_raw_run(case, &record, &evidence, manifest);
                        collected.push(CollectedRun {
                            case_index,
                            record: Some(record),
                            artifact_fingerprint: artifact_fingerprint(&evidence),
                            reasons,
                        });
                    }
                    Err(error) => collected.push(CollectedRun {
                        case_index,
                        record: None,
                        artifact_fingerprint: None,
                        reasons: vec![format!("decode raw run record: {error}")],
                    }),
                }
            }
            Err(error) => collected.push(CollectedRun {
                case_index,
                record: None,
                artifact_fingerprint: None,
                reasons: vec![format!("missing raw evidence: {error}")],
            }),
        }
    }
    apply_host_noise_gate(
        &mut collected,
        manifest,
        matrix.minimums.host_noise_threshold_milli,
    );
    apply_artifact_consistency(&mut collected);
    apply_checksum_oracle(&mut collected, manifest);

    let mut grouped: BTreeMap<String, Vec<usize>> = BTreeMap::new();
    for (index, run) in collected.iter().enumerate() {
        grouped
            .entry(aggregate_key(&manifest.cases[run.case_index]))
            .or_default()
            .push(index);
    }
    let mut rows = Vec::new();
    for indexes in grouped.values() {
        rows.push(build_aggregate_row(
            indexes,
            &collected,
            manifest,
            matrix.minimums.formal_seed_count as usize,
        ));
    }
    derive_row_comparisons(&mut rows);
    write_summary_rows(output_dir, bands, &rows)?;

    Ok(collected
        .into_iter()
        .map(|run| {
            let case = &manifest.cases[run.case_index];
            RunValidation {
                run_id: case.run_id.clone(),
                case_id: case.case_id.clone(),
                seed: case.seed,
                status: if run.reasons.is_empty() {
                    "pass"
                } else {
                    "invalid"
                }
                .into(),
                reasons: run.reasons,
            }
        })
        .collect())
}

fn validate_raw_run(
    case: &ExpandedEvalCase,
    record: &RawRunRecord,
    evidence: &str,
    manifest: &EvalRunManifest,
) -> Vec<String> {
    let mut reasons = record.initial_reasons.clone();
    if record.schema != 1 || record.kind != "run" || record.run_id != case.run_id {
        reasons.push("raw record identity/schema mismatch".into());
    }
    let Some(summary) = &record.summary else {
        reasons.push("guest summary is absent".into());
        return reasons;
    };
    if summary.schema != 1
        || summary.band != case.band.as_str()
        || summary.mode != case.mode.as_str()
        || summary.case_id != case.case_id
        || summary.seed != case.seed
    {
        reasons.push("guest summary does not match run manifest".into());
    }
    if summary.extra_vcpus != case.extra_vcpus {
        reasons.push("extra_vcpus differs from canonical case".into());
    }
    if summary.phase.configured_lookahead != case.lookahead {
        reasons.push("configured lookahead differs from canonical case".into());
    }
    if summary.trace_sample_ppm != 0 || summary.trace_sampled != 0 {
        reasons.push("timed run enabled per-request trace".into());
    }
    if summary.trace_dropped != 0 {
        reasons.push("trace records were dropped".into());
    }
    let expected_process_fail_stop = case.mode == EvalMode::Userfaultfd
        && matches!(
            case.outcome,
            OutcomeProfile::Error | OutcomeProfile::DropTimeout
        );
    if summary.fail_closed_process_exit && !expected_process_fail_stop {
        reasons.push("process fail-stop is not allowed for this canonical case".into());
    }
    if case.diagnostic_trace_required {
        match record.diagnostic_summary.as_ref() {
            Some(diagnostic) => {
                validate_diagnostic_trace(summary, diagnostic, &mut reasons);
                let records = evidence.matches("OBMM_OPERATION_TRACE schema=1").count();
                if records as u64 != diagnostic.trace_sampled {
                    reasons.push("diagnostic trace record count differs from summary".into());
                }
            }
            None => reasons.push("required diagnostic trace replay is absent".into()),
        }
    }
    if !(summary.guest_ns_p50 <= summary.guest_ns_p95
        && summary.guest_ns_p95 <= summary.guest_ns_p99
        && summary.guest_ns_p99 <= summary.guest_ns_max)
    {
        reasons.push("guest latency quantiles are not monotonic".into());
    }
    if summary.checksum.len() != 16 || u64::from_str_radix(&summary.checksum, 16).is_err() {
        reasons.push("checksum is not a 16-digit hexadecimal value".into());
    }
    if summary.model_wait_ns.is_none() {
        reasons.push("model_wait_ns is absent".into());
    }
    if summary.phase.counter_overflow != 0 {
        reasons.push("one or more metric counters overflowed".into());
    }
    if summary.phase.clock_regressions != 0 {
        reasons.push("guest clock regression was observed".into());
    }
    match case.outcome {
        OutcomeProfile::Success | OutcomeProfile::DuplicateLate => {
            if record.exit_code != Some(0)
                || summary.status != "pass"
                || summary.fail_closed_process_exit
                || summary.operations != case.operations
                || summary.failures != 0
                || summary.timeouts != 0
            {
                reasons.push("success outcome did not complete exactly once".into());
            }
            if summary.makespan_ns < case.minimum_duration_ms.saturating_mul(1_000_000) {
                reasons.push("timed run is shorter than the minimum duration".into());
            }
        }
        OutcomeProfile::Error => {
            if summary.status != "fail" || summary.failures == 0 || summary.timeouts != 0 {
                reasons.push("error outcome did not fail closed".into());
            }
        }
        OutcomeProfile::DropTimeout => {
            if summary.status != "fail" || summary.failures == 0 || summary.timeouts == 0 {
                reasons.push("drop outcome did not become an explicit timeout".into());
            }
        }
    }
    if case.outcome == OutcomeProfile::DuplicateLate
        && !evidence_counter_positive(
            evidence,
            &[
                "duplicate",
                "duplicates",
                "backend_duplicate",
                "model_duplicated",
                "model_duplicate_published",
                "scc_duplicate",
                "late",
                "backend_late",
            ],
        )
    {
        reasons.push("duplicate/late counter evidence is absent".into());
    }
    if summary.fail_closed_process_exit {
        if !evidence.contains("qemu_destroyed=1") {
            reasons.push("process fail-stop cleanup evidence is absent".into());
        }
    } else {
        if summary.phase.model_pending_final != 0
            || summary.phase.backend_pending_final != 0
            || !evidence.contains("model_pending=0")
            || !evidence.contains("backend_pending=0")
        {
            reasons.push("drain evidence is absent".into());
        }
    }
    if case.mode == EvalMode::SchedulerCore
        && (summary.phase.scc_pending_final != 0 || !evidence.contains("scc_pending=0"))
    {
        reasons.push("scheduler-core drain evidence is absent".into());
    }
    validate_mode_metrics(case, summary, &mut reasons);
    validate_artifact_evidence(case, manifest, evidence, &mut reasons);
    reasons
}

fn validate_mode_metrics(
    case: &ExpandedEvalCase,
    summary: &GuestEvalSummary,
    reasons: &mut Vec<String>,
) {
    if case.mode != EvalMode::SchedulerCore {
        return;
    }

    let phase = &summary.phase;
    if phase.qemu_context_saves != 0
        || phase.qemu_context_restores != 0
        || phase.qemu_context_switches != 0
        || phase.qemu_context_bytes != 0
        || phase.scc_save_cycles != 0
        || phase.scc_schedule_cycles != 0
        || phase.scc_restore_cycles != 0
        || phase.scc_commit_cycles != 0
    {
        reasons.push("P2B used forbidden QEMU-owned scheduler/context state".into());
    }

    if matches!(
        case.outcome,
        OutcomeProfile::Success | OutcomeProfile::DuplicateLate
    ) && (phase.el0_upcalls_pending != summary.operations
        || phase.el0_upcalls_complete != summary.operations
        || phase.el0_upcalls_fault != 0
        || phase.el0_context_saves == 0
        || phase.el0_context_restores == 0
        || phase.el0_context_switches == 0
        || phase.el0_context_bytes == 0
        || phase.el0_scheduler_ns == 0
        || phase.direct_el0_upcalls != phase.el0_context_saves)
    {
        reasons.push("P2B does not prove ABI v2 guest-EL0 scheduler progress".into());
    }
}

fn validate_artifact_evidence(
    case: &ExpandedEvalCase,
    manifest: &EvalRunManifest,
    evidence: &str,
    reasons: &mut Vec<String>,
) {
    let expected_count = if case.diagnostic_trace_required { 2 } else { 1 };
    let evidence_lines = raw_output_lines(evidence, "OBMM_RUN_EVIDENCE ");
    if evidence_lines.len() != expected_count {
        reasons.push(format!(
            "expected {expected_count} artifact evidence record(s), found {}",
            evidence_lines.len()
        ));
        return;
    }
    for line in evidence_lines {
        let topology_hosts = manifest.topology_hosts.to_string();
        let fields: BTreeMap<_, _> = line
            .split_whitespace()
            .skip(1)
            .filter_map(|field| field.split_once('='))
            .collect();
        let expected = [
            ("scenario_sha256", manifest.scenario_file_sha256.as_str()),
            ("model_file_sha256", case.model_file_sha256.as_str()),
            ("model_contract_hash", case.model_manifest_hash.as_str()),
            ("node_count", topology_hosts.as_str()),
            ("qemu_destroyed", "1"),
        ];
        for (name, value) in expected {
            if fields.get(name).copied() != Some(value) {
                reasons.push(format!("artifact evidence {name} mismatch"));
            }
        }
        for name in [
            "scenario_sha256",
            "model_file_sha256",
            "qemu_sha256",
            "kernel_sha256",
            "initramfs_sha256",
        ] {
            if !fields.get(name).copied().is_some_and(is_sha256) {
                reasons.push(format!("artifact evidence {name} is not a SHA-256 digest"));
            }
        }
    }
}

fn raw_output_lines(evidence: &str, prefix: &str) -> Vec<String> {
    evidence
        .lines()
        .filter_map(|raw| {
            serde_json::from_str::<serde_json::Value>(raw)
                .ok()?
                .get("line")?
                .as_str()
                .filter(|line| line.starts_with(prefix))
                .map(str::to_owned)
        })
        .collect()
}

fn artifact_fingerprint(evidence: &str) -> Option<String> {
    let line = raw_output_lines(evidence, "OBMM_RUN_EVIDENCE ")
        .into_iter()
        .next()?;
    let fields: BTreeMap<_, _> = line
        .split_whitespace()
        .skip(1)
        .filter_map(|field| field.split_once('='))
        .collect();
    Some(format!(
        "{}|{}|{}|{}",
        fields.get("scenario_sha256")?,
        fields.get("qemu_sha256")?,
        fields.get("kernel_sha256")?,
        fields.get("initramfs_sha256")?,
    ))
}

fn is_sha256(value: &str) -> bool {
    value.len() == 64
        && value != "0000000000000000000000000000000000000000000000000000000000000000"
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn evidence_counter_positive(evidence: &str, names: &[&str]) -> bool {
    evidence.lines().any(|line| {
        line.split_whitespace().any(|field| {
            let field =
                field.trim_matches(|character: char| matches!(character, ',' | '"' | '{' | '}'));
            field.split_once('=').is_some_and(|(name, value)| {
                names.contains(&name)
                    && value
                        .trim_matches(|character: char| matches!(character, ',' | '"' | '}'))
                        .parse::<u64>()
                        .is_ok_and(|counter| counter > 0)
            })
        })
    })
}

fn aggregate_key(case: &ExpandedEvalCase) -> String {
    format!(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        case.sweep,
        case.case_id,
        case.band.as_str(),
        case.mode.as_str(),
        case.access_bytes,
        case.model_latency_us,
        enum_json(case.jitter_profile).unwrap_or_default(),
        enum_json(case.outcome).unwrap_or_default(),
        case.coroutines,
        case.inflight,
        case.lookahead,
        format!("{}|{}", case.compute_us, case.pattern.as_str()),
    )
}

fn fairness_key(case: &ExpandedEvalCase) -> String {
    format!(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        case.sweep,
        case.band.as_str(),
        case.seed,
        case.access_bytes,
        case.operations,
        case.model_latency_us,
        enum_json(case.jitter_profile).unwrap_or_default(),
        enum_json(case.outcome).unwrap_or_default(),
        case.coroutines,
        case.compute_us,
        case.pattern.as_str(),
    )
}

fn apply_checksum_oracle(collected: &mut [CollectedRun], manifest: &EvalRunManifest) {
    let mut oracle = BTreeMap::new();
    for run in collected.iter() {
        let case = &manifest.cases[run.case_index];
        if run.reasons.is_empty()
            && case.mode == EvalMode::SyncMmio
            && matches!(
                case.outcome,
                OutcomeProfile::Success | OutcomeProfile::DuplicateLate
            )
        {
            if let Some(summary) = run
                .record
                .as_ref()
                .and_then(|record| record.summary.as_ref())
            {
                oracle.insert(fairness_key(case), summary.checksum.clone());
            }
        }
    }
    for run in collected.iter_mut() {
        let case = &manifest.cases[run.case_index];
        if !matches!(
            case.outcome,
            OutcomeProfile::Success | OutcomeProfile::DuplicateLate
        ) {
            continue;
        }
        let key = fairness_key(case);
        let Some(expected) = oracle.get(&key) else {
            run.reasons
                .push("matching sync checksum oracle is absent".into());
            continue;
        };
        if run
            .record
            .as_ref()
            .and_then(|record| record.summary.as_ref())
            .is_some_and(|summary| &summary.checksum != expected)
        {
            run.reasons
                .push("checksum differs from matching sync oracle".into());
        }
    }
}

fn apply_host_noise_gate(
    collected: &mut [CollectedRun],
    manifest: &EvalRunManifest,
    threshold_milli: u32,
) {
    let mut groups: BTreeMap<String, Vec<usize>> = BTreeMap::new();
    for (index, run) in collected.iter().enumerate() {
        groups
            .entry(aggregate_key(&manifest.cases[run.case_index]))
            .or_default()
            .push(index);
    }
    for indexes in groups.values() {
        let mut elapsed: Vec<u64> = indexes
            .iter()
            .filter_map(|index| collected[*index].record.as_ref())
            .map(|record| record.host.elapsed_ns)
            .collect();
        let Some(center) = median(&mut elapsed) else {
            continue;
        };
        if center == 0 {
            continue;
        }
        for &index in indexes {
            let Some(record) = collected[index].record.as_ref() else {
                continue;
            };
            let deviation = record
                .host
                .elapsed_ns
                .abs_diff(center)
                .saturating_mul(1_000)
                / center;
            let overloaded = record
                .host
                .load1_milli
                .zip(record.host.online_cpus)
                .is_some_and(|(load, cpus)| load > u64::from(cpus) * 1_000);
            if deviation > u64::from(threshold_milli) && overloaded {
                collected[index]
                    .reasons
                    .push("host elapsed outlier with host-load evidence".into());
            }
        }
    }
}

fn apply_artifact_consistency(collected: &mut [CollectedRun]) {
    let fingerprints: BTreeSet<&str> = collected
        .iter()
        .filter_map(|run| run.artifact_fingerprint.as_deref())
        .collect();
    if fingerprints.len() <= 1 {
        return;
    }
    for run in collected.iter_mut() {
        if run.artifact_fingerprint.is_some() {
            run.reasons
                .push("QEMU/kernel/initramfs build differs within the report".into());
        }
    }
}

fn build_aggregate_row(
    indexes: &[usize],
    collected: &[CollectedRun],
    manifest: &EvalRunManifest,
    formal_seed_count: usize,
) -> AggregateRow {
    let case = manifest.cases[collected[indexes[0]].case_index].clone();
    let valid_records: Vec<&RawRunRecord> = indexes
        .iter()
        .filter_map(|index| {
            let run = &collected[*index];
            run.reasons
                .is_empty()
                .then(|| run.record.as_ref())
                .flatten()
        })
        .collect();
    let summaries: Vec<&GuestEvalSummary> = valid_records
        .iter()
        .filter_map(|record| record.summary.as_ref())
        .collect();
    let checksums: BTreeSet<&str> = summaries
        .iter()
        .map(|summary| summary.checksum.as_str())
        .collect();
    let checksum_set = if checksums.is_empty() {
        "na".into()
    } else {
        checksums.iter().copied().collect::<Vec<_>>().join("|")
    };
    let mut success_counts: Vec<u64> = summaries
        .iter()
        .map(|summary| summary.operations.saturating_sub(summary.failures))
        .collect();
    let mut failure_counts: Vec<u64> = summaries.iter().map(|summary| summary.failures).collect();
    let mut timeout_counts: Vec<u64> = summaries.iter().map(|summary| summary.timeouts).collect();
    let mut host_elapsed: Vec<u64> = valid_records
        .iter()
        .map(|record| record.host.elapsed_ns)
        .collect();
    let mut makespans: Vec<u64> = summaries
        .iter()
        .map(|summary| summary.makespan_ns)
        .collect();
    let mut p50: Vec<u64> = summaries
        .iter()
        .map(|summary| summary.guest_ns_p50)
        .collect();
    let mut p95: Vec<u64> = summaries
        .iter()
        .map(|summary| summary.guest_ns_p95)
        .collect();
    let mut p99: Vec<u64> = summaries
        .iter()
        .map(|summary| summary.guest_ns_p99)
        .collect();
    let mut max: Vec<u64> = summaries
        .iter()
        .map(|summary| summary.guest_ns_max)
        .collect();
    let mut model_wait: Vec<u64> = summaries
        .iter()
        .filter_map(|summary| summary.model_wait_ns)
        .collect();
    let mut useful: Vec<u64> = summaries
        .iter()
        .map(|summary| summary.useful_work_ns)
        .collect();
    let mut app_cpu: Vec<u64> = summaries
        .iter()
        .map(|summary| summary.application_cpu_ns)
        .collect();
    let mut helper_cpu: Vec<u64> = summaries
        .iter()
        .map(|summary| summary.helper_cpu_ns)
        .collect();
    let ci95 = bootstrap_median_ci95(&makespans, fnv1a64(aggregate_key(&case).as_bytes()));
    let makespan_min = makespans.iter().min().copied();
    let makespan_max = makespans.iter().max().copied();
    let makespan_median = median(&mut makespans);
    let requests_per_second = makespan_median.and_then(|makespan| {
        (makespan != 0).then_some(case.operations as f64 * 1_000_000_000.0 / makespan as f64)
    });
    let bytes_per_second =
        requests_per_second.map(|requests| requests * f64::from(case.access_bytes));
    let valid_seeds = summaries.len();
    let useful_work_median = median(&mut useful);
    let application_cpu_median = median(&mut app_cpu);
    let helper_cpu_median = median(&mut helper_cpu);
    let phase_medians = PHASE_METRIC_FIELDS
        .iter()
        .map(|name| {
            let mut values: Vec<u64> = summaries
                .iter()
                .map(|summary| phase_metric(&summary.phase, name))
                .collect();
            (*name, median(&mut values).unwrap_or(0))
        })
        .collect();
    let core_efficiency = useful_work_median
        .zip(application_cpu_median.zip(helper_cpu_median))
        .and_then(|(work, (application, helper))| {
            let total = application.saturating_add(helper);
            (total != 0).then_some(work as f64 / total as f64)
        });
    AggregateRow {
        case,
        valid_seeds,
        checksum_set,
        success_count_median: median(&mut success_counts),
        failure_count_median: median(&mut failure_counts),
        timeout_count_median: median(&mut timeout_counts),
        requests_per_second,
        bytes_per_second,
        makespan_median,
        makespan_min,
        makespan_max,
        host_elapsed_median: median(&mut host_elapsed),
        ci95,
        guest_p50_median: median(&mut p50),
        guest_p95_median: median(&mut p95),
        guest_p99_median: median(&mut p99),
        guest_max_median: median(&mut max),
        model_wait_median: median(&mut model_wait),
        useful_work_median,
        application_cpu_median,
        helper_cpu_median,
        phase_medians,
        mechanism_gain_ns: None,
        schedule_ahead_gain_ns: None,
        overlap_efficiency: None,
        core_efficiency,
        status: if valid_seeds >= formal_seed_count {
            "pass"
        } else {
            "invalid"
        }
        .into(),
    }
}

fn comparison_key(case: &ExpandedEvalCase) -> String {
    format!(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}",
        case.sweep,
        case.band.as_str(),
        case.access_bytes,
        case.model_latency_us,
        enum_json(case.jitter_profile).unwrap_or_default(),
        enum_json(case.outcome).unwrap_or_default(),
        case.coroutines,
        case.compute_us,
        case.pattern.as_str(),
    )
}

fn derive_row_comparisons(rows: &mut [AggregateRow]) {
    let mut sync = BTreeMap::new();
    let mut demand = BTreeMap::new();
    for row in rows.iter() {
        if row.status != "pass" {
            continue;
        }
        let key = comparison_key(&row.case);
        if row.case.mode == EvalMode::SyncMmio {
            sync.insert(key, row.makespan_median);
        } else if row.case.mode == EvalMode::AsyncPoll && row.case.issue == EvalIssue::Demand {
            demand.insert(
                format!("{}|{}", key, row.case.inflight),
                row.makespan_median,
            );
        }
    }
    for row in rows.iter_mut() {
        let key = comparison_key(&row.case);
        if let (Some(Some(sync_ns)), Some(mode_ns)) = (sync.get(&key), row.makespan_median) {
            row.mechanism_gain_ns = Some(i128::from(*sync_ns) - i128::from(mode_ns));
        }
        if row.case.mode == EvalMode::AsyncPoll && row.case.issue == EvalIssue::Lookahead {
            if let (Some(Some(demand_ns)), Some(lookahead_ns)) = (
                demand.get(&format!("{}|{}", key, row.case.inflight)),
                row.makespan_median,
            ) {
                row.schedule_ahead_gain_ns =
                    Some(i128::from(*demand_ns) - i128::from(lookahead_ns));
            }
        }
        if let (Some(gain), Some(work), Some(model_wait)) = (
            row.mechanism_gain_ns,
            row.useful_work_median,
            row.model_wait_median,
        ) {
            let hidden = gain.max(0) as u64;
            let denominator = work.min(model_wait);
            if denominator != 0 {
                row.overlap_efficiency = Some(hidden as f64 / denominator as f64);
            }
        }
    }
}

fn write_summary_rows(
    output_dir: &Path,
    bands: &BTreeSet<EvalBand>,
    rows: &[AggregateRow],
) -> anyhow::Result<()> {
    let header = summary_header();
    for band in bands {
        if *band == EvalBand::Transparency {
            write_transparency_summary(output_dir)?;
            continue;
        }
        let mut csv = header.clone();
        for row in rows.iter().filter(|row| row.case.band == *band) {
            let (ci_low, ci_high) = row.ci95.unzip();
            let mut fields = vec![
                row.case.case_id.clone(),
                row.case.mode.as_str().into(),
                row.case.sweep.clone(),
                row.case.model_latency_us.to_string(),
                enum_json(row.case.jitter_profile)?,
                enum_json(row.case.outcome)?,
                row.case.coroutines.to_string(),
                row.case.inflight.to_string(),
                row.case.lookahead.to_string(),
                row.case.compute_us.to_string(),
                row.case.pattern.as_str().into(),
                row.valid_seeds.to_string(),
                row.checksum_set.clone(),
                option_u64(row.success_count_median),
                option_u64(row.failure_count_median),
                option_u64(row.timeout_count_median),
                option_f64(row.requests_per_second),
                option_f64(row.bytes_per_second),
                option_u64(row.guest_p50_median),
                option_u64(row.guest_p95_median),
                option_u64(row.guest_p99_median),
                option_u64(row.guest_max_median),
                option_u64(row.model_wait_median),
                option_u64(row.makespan_median),
                option_u64(row.makespan_min),
                option_u64(row.makespan_max),
                option_u64(row.host_elapsed_median),
                option_u64(ci_low),
                option_u64(ci_high),
                option_u64(row.useful_work_median),
                option_u64(row.application_cpu_median),
                option_u64(row.helper_cpu_median),
            ];
            fields.extend(PHASE_METRIC_FIELDS.iter().map(|name| {
                row.phase_medians
                    .get(name)
                    .copied()
                    .unwrap_or(0)
                    .to_string()
            }));
            fields.extend([
                option_i128(row.mechanism_gain_ns),
                option_i128(row.schedule_ahead_gain_ns),
                option_f64(row.overlap_efficiency),
                option_f64(row.core_efficiency),
                row.status.clone(),
            ]);
            csv.push_str(&fields.join(","));
            csv.push('\n');
        }
        fs::write(
            output_dir
                .join("summary")
                .join(format!("{}.csv", band.as_str())),
            csv,
        )?;
    }
    write_break_even_summary(output_dir, rows)?;
    Ok(())
}

fn summary_header() -> String {
    format!(
        "case,mode,sweep,latency_us,jitter,outcome,coroutines,inflight,lookahead,compute_us,pattern,valid_seeds,checksum_set,success_count,failure_count,timeout_count,requests_per_second,bytes_per_second,guest_ns_p50,guest_ns_p95,guest_ns_p99,guest_ns_max,model_wait_ns,makespan_ns_median,makespan_ns_min,makespan_ns_max,host_elapsed_ns_median,ci95_low,ci95_high,useful_work_ns,application_cpu_ns,helper_cpu_ns,{},mechanism_gain_ns,schedule_ahead_gain_ns,overlap_efficiency,core_efficiency,status\n",
        PHASE_METRIC_FIELDS.join(",")
    )
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct BreakEvenInterval {
    measured_min_us: Option<u64>,
    measured_max_us: Option<u64>,
    nonpositive_us: Option<u64>,
    positive_us: Option<u64>,
    nonpositive_gain_ns: Option<i128>,
    positive_gain_ns: Option<i128>,
    status: &'static str,
}

fn break_even_interval(points: &[(u64, i128)]) -> BreakEvenInterval {
    let mut ordered = points.to_vec();
    ordered.sort_unstable_by_key(|(latency, _)| *latency);
    let measured_min_us = ordered.first().map(|(latency, _)| *latency);
    let measured_max_us = ordered.last().map(|(latency, _)| *latency);
    let Some(first_positive) = ordered.iter().position(|(_, gain)| *gain > 0) else {
        return BreakEvenInterval {
            measured_min_us,
            measured_max_us,
            nonpositive_us: None,
            positive_us: None,
            nonpositive_gain_ns: None,
            positive_gain_ns: None,
            status: "not-observed",
        };
    };
    let non_monotonic = ordered[first_positive + 1..]
        .iter()
        .any(|(_, gain)| *gain <= 0);
    let (nonpositive_us, nonpositive_gain_ns) = first_positive
        .checked_sub(1)
        .map(|index| (Some(ordered[index].0), Some(ordered[index].1)))
        .unwrap_or((None, None));
    BreakEvenInterval {
        measured_min_us,
        measured_max_us,
        nonpositive_us,
        positive_us: Some(ordered[first_positive].0),
        nonpositive_gain_ns,
        positive_gain_ns: Some(ordered[first_positive].1),
        status: if non_monotonic {
            "non-monotonic"
        } else if first_positive == 0 {
            "positive-at-minimum"
        } else {
            "bracketed"
        },
    }
}

fn break_even_key(row: &AggregateRow, gain_kind: &str) -> String {
    format!(
        "{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        row.case.sweep,
        row.case.band.as_str(),
        row.case.case_id,
        row.case.mode.as_str(),
        gain_kind,
        row.case.compute_us,
        row.case.coroutines,
        row.case.inflight,
        row.case.lookahead,
        row.case.pattern.as_str(),
        enum_json(row.case.jitter_profile).unwrap_or_default(),
        enum_json(row.case.outcome).unwrap_or_default(),
        row.case.access_bytes,
    )
}

fn write_break_even_summary(output_dir: &Path, rows: &[AggregateRow]) -> anyhow::Result<()> {
    let mut series: BTreeMap<String, (&AggregateRow, &'static str, Vec<(u64, i128)>)> =
        BTreeMap::new();
    for row in rows.iter().filter(|row| {
        row.status == "pass"
            && row.case.outcome == OutcomeProfile::Success
            && row.case.mode != EvalMode::SyncMmio
    }) {
        for (gain_kind, gain) in [
            ("mechanism", row.mechanism_gain_ns),
            ("schedule-ahead", row.schedule_ahead_gain_ns),
        ] {
            let Some(gain) = gain else {
                continue;
            };
            series
                .entry(break_even_key(row, gain_kind))
                .or_insert_with(|| (row, gain_kind, Vec::new()))
                .2
                .push((row.case.model_latency_us, gain));
        }
    }

    let mut csv = String::from(
        "sweep,band,case,mode,gain_kind,compute_us,coroutines,inflight,lookahead,pattern,jitter,outcome,access_bytes,measured_latency_min_us,measured_latency_max_us,nonpositive_latency_us,positive_latency_us,nonpositive_gain_ns,positive_gain_ns,status\n",
    );
    for (_, (row, gain_kind, points)) in series {
        let interval = break_even_interval(&points);
        let fields = [
            row.case.sweep.clone(),
            row.case.band.as_str().into(),
            row.case.case_id.clone(),
            row.case.mode.as_str().into(),
            gain_kind.into(),
            row.case.compute_us.to_string(),
            row.case.coroutines.to_string(),
            row.case.inflight.to_string(),
            row.case.lookahead.to_string(),
            row.case.pattern.as_str().into(),
            enum_json(row.case.jitter_profile)?,
            enum_json(row.case.outcome)?,
            row.case.access_bytes.to_string(),
            option_u64(interval.measured_min_us),
            option_u64(interval.measured_max_us),
            option_u64(interval.nonpositive_us),
            option_u64(interval.positive_us),
            option_i128(interval.nonpositive_gain_ns),
            option_i128(interval.positive_gain_ns),
            interval.status.into(),
        ];
        csv.push_str(&fields.join(","));
        csv.push('\n');
    }
    fs::write(output_dir.join("summary/break-even.csv"), csv)?;
    Ok(())
}

fn write_transparency_summary(output_dir: &Path) -> anyhow::Result<()> {
    const CSV: &str = "path,hot_path_source,machine_code_suspension,suspension_owner,extra_vcpus,completion_bytes_min,completion_bytes_max,software_components,software_change_surface,custom_hardware_components,custom_hardware_state,cost_interpretation\n\
P2A,submit/test/await,runtime-await,EL0-runtime,0,1,65536,3,application+runtime+UAPI,0,none,explicit-software-interface\n\
P2B,ordinary-scalar-LDR,pending-unretired-LDR,guest-EL0-runtime,0,1,8,2,control-plane+UAPI,2,PLT+event-queue+direct-upcall+atomic-resume,custom-core-state\n\
P4,ordinary-shadow-range-load,page-fault-kernel-block,kernel-UFFD+userspace-handler,1,4096,4096,2,application+UFFD-handler,0,none,page-granularity-and-handler-core\n";
    fs::write(output_dir.join("summary/transparency.csv"), CSV)?;
    Ok(())
}

fn option_u64(value: Option<u64>) -> String {
    value.map_or_else(|| "na".into(), |value| value.to_string())
}

fn option_i128(value: Option<i128>) -> String {
    value.map_or_else(|| "na".into(), |value| value.to_string())
}

fn option_f64(value: Option<f64>) -> String {
    value.map_or_else(|| "na".into(), |value| format!("{value:.6}"))
}

fn write_final_report(
    output_dir: &Path,
    manifest: &EvalRunManifest,
    validation: &EvalValidation,
) -> anyhow::Result<()> {
    let valid = validation
        .runs
        .iter()
        .filter(|run| run.status == "pass")
        .count();
    let invalid = validation.runs.len().saturating_sub(valid);
    let conclusion = if validation.status == "pass" {
        "All formal gates and seed requirements passed. Mechanism and schedule-ahead gains are reported separately in the band CSV files."
    } else {
        "No performance conclusion is valid: at least one gate, seed requirement, or raw run is invalid. Raw evidence is retained for diagnosis."
    };
    let gate_rows = validation
        .gates
        .iter()
        .map(|gate| {
            format!(
                "| {} | {} | `{}` | {} |",
                gate.phase,
                gate.status,
                gate.path,
                if gate.reasons.is_empty() {
                    "none".into()
                } else {
                    gate.reasons.join("; ")
                }
            )
        })
        .collect::<Vec<_>>()
        .join("\n");
    let report = format!(
        "# OBMM remote-load evaluation\n\n\
         Status: **{}**\n\n\
         - Valid raw runs: {}\n\
         - Invalid raw runs: {}\n\
         - Matrix hash: `{}`\n\
         - Scenario hash: `{}`\n\
         - Scenario file SHA-256: `{}`\n\
         - Topology: {} hosts; hash `{}`\n\n\
         {}\n\n\
         ## Phase gates\n\n\
         | Phase | Status | Evidence | Reasons |\n\
         |---|---|---|---|\n\
         {}\n\n\
         ## Comparison bands\n\n\
         - Band S (`summary/scalar.csv`) compares 8-byte sync, P2A demand/lookahead, and P2B demand.\n\
         - Band R (`summary/range.csv`) compares the same 4-KiB logical operations for sync, P2A, and userfaultfd.\n\
         - Band T (`summary/transparency.csv`) reports interface, software/custom-hardware surface, and extra-core cost; it is not collapsed into a throughput ranking.\n\
         - `summary/break-even.csv` reports only measured L/W/concurrency intervals: `bracketed`, `positive-at-minimum`, `not-observed`, or `non-monotonic`. It never extrapolates beyond measured latency points.\n\n\
         Each Band S/R row also carries the median ready/wait/idle, P2A submit/switch/CQ, P1 pending/copy/late/duplicate, P2B save/schedule/restore/commit, and P4 fault/remote/copy/wake phase metrics. `run-manifest.json`, `validation.json`, and `raw/*.jsonl` preserve the artifact hashes and per-seed provenance.\n\n\
         Fields with a zero or unavailable denominator are `na`. Invalid rows never contribute to medians or confidence intervals.\n",
        validation.status,
        valid,
        invalid,
        manifest.matrix_hash,
        manifest.scenario_hash,
        manifest.scenario_file_sha256,
        manifest.topology_hosts,
        manifest.topology_hash,
        conclusion,
        gate_rows,
    );
    fs::write(output_dir.join("report.md"), report)?;
    Ok(())
}

fn write_empty_summaries(output_dir: &Path, bands: &BTreeSet<EvalBand>) -> anyhow::Result<()> {
    let header = summary_header();

    for band in bands {
        if *band == EvalBand::Transparency {
            write_transparency_summary(output_dir)?;
            continue;
        }
        fs::write(
            output_dir
                .join("summary")
                .join(format!("{}.csv", band.as_str())),
            &header,
        )?;
    }
    write_break_even_summary(output_dir, &[])?;
    Ok(())
}

fn write_dry_run_report(
    output_dir: &Path,
    manifest: &EvalRunManifest,
    validation: &EvalValidation,
) -> anyhow::Result<()> {
    let report = format!(
        "# OBMM remote-load evaluation\n\n\
         Status: **{}** (dry-run only)\n\n\
         - Expanded cases: {}\n\
         - P0-P4 gates: {}\n\
         - Matrix hash: `{}`\n\
         - Scenario hash: `{}`\n\
         - Scenario file SHA-256: `{}`\n\
         - Topology: {} hosts; hash `{}`\n\n\
         No performance conclusion is emitted until every gate passes and raw rows for valid \
         seeds have been aggregated. Band S compares 8-byte demand paths; Band R compares 4-KiB \
         range paths. They are never merged into one ranking. `summary/break-even.csv` remains \
         empty until valid measured points are available.\n\n\
         ## Transparency/resource contract\n\n\
         | Path | Hot path | Suspension owner | Extra vCPU | Granularity |\n\
         |---|---|---|---:|---:|\n\
         | P2A | submit/await | EL0 runtime | 0 | 1 B-64 KiB |\n\
         | P2B | ordinary LDR | guest EL0 runtime | 0 | 1/2/4/8 B |\n\
         | P4 | shadow pointer load | kernel UFFD + handler | 1 | 4 KiB |\n",
        validation.status,
        manifest.cases.len(),
        if manifest.gates_passed {
            "pass"
        } else {
            "invalid"
        },
        manifest.matrix_hash,
        manifest.scenario_hash,
        manifest.scenario_file_sha256,
        manifest.topology_hosts,
        manifest.topology_hash,
    );
    fs::write(output_dir.join("report.md"), report)?;
    Ok(())
}

#[cfg(test)]
fn derive_metrics(
    sync_makespan_ns: u64,
    demand_makespan_ns: u64,
    lookahead_makespan_ns: u64,
    total_model_wait_ns: u64,
    available_useful_work_ns: u64,
    useful_work_ns: u64,
    total_cpu_ns: u64,
) -> DerivedMetrics {
    let overlap_hidden_ns = sync_makespan_ns.saturating_sub(demand_makespan_ns);
    let overlap_denominator = total_model_wait_ns.min(available_useful_work_ns);
    DerivedMetrics {
        overlap_hidden_ns,
        overlap_efficiency: (overlap_denominator != 0)
            .then_some(overlap_hidden_ns as f64 / overlap_denominator as f64),
        schedule_ahead_gain_ns: i128::from(demand_makespan_ns) - i128::from(lookahead_makespan_ns),
        mechanism_gain_ns: i128::from(sync_makespan_ns) - i128::from(demand_makespan_ns),
        core_efficiency: (total_cpu_ns != 0).then_some(useful_work_ns as f64 / total_cpu_ns as f64),
    }
}

fn median(values: &mut [u64]) -> Option<u64> {
    if values.is_empty() {
        return None;
    }
    values.sort_unstable();
    Some(values[(values.len() - 1) / 2])
}

fn bootstrap_median_ci95(values: &[u64], seed: u64) -> Option<(u64, u64)> {
    if values.is_empty() {
        return None;
    }
    let mut state = seed;
    let mut samples = Vec::with_capacity(2_000);
    for _ in 0..2_000 {
        let mut resample = Vec::with_capacity(values.len());
        for _ in values {
            state = splitmix64(state);
            resample.push(values[state as usize % values.len()]);
        }
        samples.push(median(&mut resample).expect("non-empty resample"));
    }
    samples.sort_unstable();
    Some((samples[49], samples[1_949]))
}

fn enum_json<T: Serialize>(value: T) -> anyhow::Result<String> {
    Ok(serde_json::to_string(&value)?.trim_matches('"').to_string())
}

fn safe_hash(hash: &str) -> &str {
    hash.strip_prefix("fnv1a64:").unwrap_or(hash)
}

fn hash_json<T: Serialize>(value: &T) -> anyhow::Result<String> {
    Ok(hash_bytes(&serde_json::to_vec(value)?))
}

fn hash_bytes(bytes: &[u8]) -> String {
    format!("fnv1a64:{:016x}", fnv1a64(bytes))
}

fn eval_run_namespace(output_dir: &Path) -> anyhow::Result<String> {
    let canonical = fs::canonicalize(output_dir)
        .with_context(|| format!("canonicalize output directory {}", output_dir.display()))?;
    Ok(format!(
        "{:016x}",
        fnv1a64(canonical.as_os_str().as_encoded_bytes())
    ))
}

fn sha256_file(path: &Path) -> anyhow::Result<String> {
    let attempts: [(&str, &[&str]); 2] = [("sha256sum", &[]), ("shasum", &["-a", "256"])];
    let mut unavailable = Vec::new();
    for (program, arguments) in attempts {
        match Command::new(program).args(arguments).arg(path).output() {
            Ok(output) if output.status.success() => {
                let stdout = String::from_utf8(output.stdout)
                    .with_context(|| format!("decode {program} output for {}", path.display()))?;
                let digest = stdout.split_whitespace().next().unwrap_or_default();
                if digest.len() == 64
                    && digest
                        .bytes()
                        .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
                {
                    return Ok(digest.to_string());
                }
                anyhow::bail!(
                    "{program} returned an invalid SHA-256 digest for {}",
                    path.display()
                );
            }
            Ok(output) => unavailable.push(format!("{program}: {}", output.status)),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
                unavailable.push(format!("{program}: unavailable"));
            }
            Err(error) => {
                return Err(error).with_context(|| format!("run {program} for {}", path.display()));
            }
        }
    }
    anyhow::bail!(
        "no working SHA-256 utility for {} ({})",
        path.display(),
        unavailable.join(", ")
    )
}

fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325_u64;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

fn splitmix64(mut value: u64) -> u64 {
    value = value.wrapping_add(0x9e37_79b9_7f4a_7c15);
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    value ^ (value >> 31)
}

fn write_json<T: Serialize>(path: &Path, value: &T) -> anyhow::Result<()> {
    let mut bytes = serde_json::to_vec_pretty(value)?;
    bytes.push(b'\n');
    fs::write(path, bytes).with_context(|| format!("write {}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT_ID: AtomicU64 = AtomicU64::new(0);

    fn matrix_path() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/experiments/obmm_remote_load_eval_v1.yaml")
    }

    fn acceptance_matrix_path() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/experiments/obmm_remote_load_eval_acceptance_v1.yaml")
    }

    fn scenario_path() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../scenarios/mvp_2host_single_domain.yaml")
    }

    fn temp_dir() -> PathBuf {
        std::env::temp_dir().join(format!(
            "ub-sim-obmm-eval-{}-{}",
            std::process::id(),
            NEXT_ID.fetch_add(1, Ordering::Relaxed)
        ))
    }

    fn load() -> EvalMatrix {
        let matrix: EvalMatrix =
            serde_yaml::from_slice(&fs::read(matrix_path()).expect("matrix bytes"))
                .expect("matrix");
        validate_matrix(&matrix).expect("valid matrix");
        matrix
    }

    fn test_case() -> ExpandedEvalCase {
        ExpandedEvalCase {
            run_id: "test-run".into(),
            sweep: "test-sweep".into(),
            case_id: "S0-sync".into(),
            band: EvalBand::Scalar,
            mode: EvalMode::SyncMmio,
            issue: EvalIssue::Demand,
            seed: 1,
            access_bytes: 8,
            operations: 10_000,
            deadline_us: 1_000_000,
            warmup_operations: 1_000,
            minimum_duration_ms: 0,
            model_latency_us: 100,
            jitter_profile: JitterProfile::None,
            outcome: OutcomeProfile::Success,
            coroutines: 8,
            inflight: 1,
            lookahead: 0,
            compute_us: 10,
            pattern: EvalPattern::Sequential,
            extra_vcpus: 0,
            diagnostic_trace_required: false,
            operation_list_hash: "fnv1a64:0000000000000001".into(),
            model_manifest_hash: "fnv1a64:0000000000000002".into(),
            model_file_sha256: "1111111111111111111111111111111111111111111111111111111111111111"
                .into(),
            model_manifest_path: "out/models/test.json".into(),
            order_index: 0,
            command: vec!["runner".into()],
        }
    }

    fn test_summary(makespan_ns: u64) -> GuestEvalSummary {
        GuestEvalSummary {
            schema: 1,
            band: "scalar".into(),
            mode: "sync-mmio".into(),
            case_id: "S0-sync".into(),
            seed: 1,
            operations: 10_000,
            checksum: "0123456789abcdef".into(),
            failures: 0,
            timeouts: 0,
            guest_ns_p50: 10,
            guest_ns_p95: 20,
            guest_ns_p99: 30,
            guest_ns_max: 40,
            makespan_ns,
            model_wait_ns: Some(500),
            useful_work_ns: 100,
            application_cpu_ns: 200,
            helper_cpu_ns: 0,
            extra_vcpus: 0,
            trace_sample_ppm: 0,
            trace_sampled: 0,
            trace_dropped: 0,
            fail_closed_process_exit: false,
            phase: GuestPhaseMetrics::default(),
            status: "pass".into(),
        }
    }

    fn test_manifest(cases: Vec<ExpandedEvalCase>) -> EvalRunManifest {
        EvalRunManifest {
            schema: 1,
            run_namespace: "test-namespace".into(),
            matrix_name: "test".into(),
            matrix_path: "matrix.yaml".into(),
            matrix_hash: "matrix-hash".into(),
            scenario_path: "scenario.yaml".into(),
            scenario_hash: "scenario-hash".into(),
            scenario_file_sha256:
                "2222222222222222222222222222222222222222222222222222222222222222".into(),
            topology_hosts: 2,
            topology_hash: "topology-hash".into(),
            selected_bands: vec!["scalar".into()],
            seeds: vec![1],
            gate_dir: "gates".into(),
            gates_passed: true,
            valid_for_execution: true,
            cases,
        }
    }

    #[test]
    fn parses_cli_and_seed_range() {
        let args = args_from([
            "obmm-remote-load-eval",
            "--matrix=scenarios/experiments/obmm_remote_load_eval_v1.yaml",
            "--scenario=scenarios/mvp_2host_single_domain.yaml",
            "--bands=scalar,range,transparency",
            "--seeds=1..7",
            "--output-dir=out/eval",
            "--dry-run",
        ])
        .expect("args")
        .expect("eval args");
        assert!(args.dry_run);
        assert_eq!(
            parse_seeds(&args.seeds).expect("seeds"),
            (1..=7).collect::<Vec<_>>()
        );

        let local = args_from([
            "obmm-remote-load-eval",
            "--matrix=matrix.yaml",
            "--scenario=scenario.yaml",
            "--output-dir=out/eval",
            "--local-repo=/srv/ub_sim",
        ])
        .expect("local arguments")
        .expect("local eval arguments");
        assert_eq!(local.local_repo, Some(PathBuf::from("/srv/ub_sim")));
        assert!(args_from([
            "obmm-remote-load-eval",
            "--matrix=matrix.yaml",
            "--scenario=scenario.yaml",
            "--output-dir=out/eval",
            "--local-repo=/srv/ub_sim",
            "--remote-target=host",
            "--remote-repo=/srv/ub_sim",
        ])
        .expect_err("local and remote execution must be exclusive")
        .to_string()
        .contains("mutually exclusive"));
    }

    #[test]
    fn progress_line_reports_actionable_case_state() {
        assert_eq!(
            eval_progress_line("case-7", 6, 49, "dispatch", None),
            "OBMM_EVAL_PROGRESS schema=1 completed=6 total=49 run_id=case-7 \
             stage=dispatch exit_code=na"
        );
        assert_eq!(
            eval_progress_line("case-7", 7, 49, "collected", Some(0)),
            "OBMM_EVAL_PROGRESS schema=1 completed=7 total=49 run_id=case-7 \
             stage=collected exit_code=0"
        );
    }

    #[test]
    fn auxiliary_command_timeout_is_bounded() {
        let mut success = Command::new("sh");
        success.arg("-c").arg("printf ready; printf warning >&2");
        let full = command_full_output_with_timeout(&mut success, Duration::from_secs(1))
            .expect("successful full command")
            .expect("full command result");
        assert!(full.status.success());
        assert_eq!(full.stdout, b"ready");
        assert_eq!(full.stderr, b"warning");

        let mut stdout_only = Command::new("sh");
        stdout_only.arg("-c").arg("printf ready");
        let (status, stdout) =
            command_output_with_timeout(&mut stdout_only, Duration::from_secs(1))
                .expect("successful command")
                .expect("command result");
        assert!(status.success());
        assert_eq!(stdout, b"ready");

        let mut blocked = Command::new("sh");
        blocked.arg("-c").arg("sleep 2 & wait");
        let started = Instant::now();
        assert!(
            command_output_with_timeout(&mut blocked, Duration::from_millis(50),)
                .expect("bounded command")
                .is_none()
        );
        assert!(started.elapsed() < Duration::from_secs(1));

        let local = execute_case_command(
            None,
            Path::new("/tmp"),
            &["sh".into(), "-c".into(), "printf local-ready".into()],
            Duration::from_secs(1),
        )
        .expect("local case command")
        .expect("local case output");
        assert!(local.status.success());
        assert_eq!(local.stdout, b"local-ready");
    }

    #[test]
    fn remote_commands_have_connection_and_outer_deadlines() {
        let command = ssh_command("remote-test");
        let arguments = command
            .get_args()
            .map(|argument| argument.to_string_lossy().into_owned())
            .collect::<Vec<_>>();
        assert_eq!(
            arguments,
            [
                "-o",
                "ConnectTimeout=15",
                "-o",
                "ControlMaster=no",
                "-o",
                "ControlPath=none",
                "-o",
                "ServerAliveInterval=15",
                "-o",
                "ServerAliveCountMax=3",
                "remote-test",
            ]
        );
        assert_eq!(
            remote_case_timeout(&["--timeout-sec".into(), "300".into()]).expect("case timeout"),
            Duration::from_secs(420)
        );
        assert!(remote_case_timeout(&[]).is_err());
    }

    #[test]
    fn ssh_retry_is_limited_to_proven_pre_dispatch_failures() {
        assert!(retryable_ssh_connect_failure(
            Some(255),
            b"",
            b"Connection timed out during banner exchange\n"
        ));
        assert!(retryable_ssh_connect_failure(
            Some(255),
            b"",
            b"kex_exchange_identification: read: Connection reset by peer\n"
        ));
        assert!(!retryable_ssh_connect_failure(
            Some(255),
            b"remote command started\n",
            b"Connection timed out during banner exchange\n"
        ));
        assert!(!retryable_ssh_connect_failure(
            Some(1),
            b"",
            b"Connection refused\n"
        ));
        assert!(!retryable_ssh_connect_failure(
            Some(255),
            b"",
            b"Connection to host closed by remote host.\n"
        ));
    }

    #[test]
    fn matrix_expansion_is_deterministic_and_band_strict() {
        let matrix = load();
        let scenario = ScenarioConfig::from_yaml_file(scenario_path()).expect("scenario");
        let output = temp_dir();
        fs::create_dir_all(output.join("models")).expect("model dir");
        let bands = EvalBand::parse_list("scalar,range").expect("bands");
        let scenario_path = scenario_path();
        let mut left =
            expand_matrix(&matrix, &scenario, &scenario_path, &bands, &[1], &output).expect("left");
        let mut right = expand_matrix(&matrix, &scenario, &scenario_path, &bands, &[1], &output)
            .expect("right");
        let retry_output = temp_dir();
        fs::create_dir_all(retry_output.join("models")).expect("retry model dir");
        let retry = expand_matrix(
            &matrix,
            &scenario,
            &scenario_path,
            &bands,
            &[1],
            &retry_output,
        )
        .expect("retry");
        assert_eq!(left.len(), retry.len());
        assert!(left.iter().zip(&retry).all(|(original, rerun)| {
            original.run_id != rerun.run_id
                && original.operation_list_hash == rerun.operation_list_hash
        }));
        assign_deterministic_order(&mut left);
        assign_deterministic_order(&mut right);
        assert_eq!(left, right);
        assert!(left.iter().all(|case| {
            (case.band == EvalBand::Scalar && case.access_bytes == 8)
                || (case.band == EvalBand::Range && case.access_bytes == 4096)
        }));
        assert!(left.iter().all(|case| {
            case.mode != EvalMode::Userfaultfd || case.pattern != EvalPattern::Dependent
        }));
        assert!(left.iter().all(|case| {
            case.command.first().map(String::as_str) == Some("zsh")
                && case.command.get(1).map(String::as_str)
                    == Some("guest-linux/aarch64/scripts/run_ub_obmm_eval.sh")
                && case
                    .command
                    .windows(2)
                    .any(|pair| pair == ["--node-count", "2"])
                && case.command.windows(2).any(|pair| {
                    pair[0] == "--scenario-config" && pair[1] == scenario_path.display().to_string()
                })
                && case
                    .command
                    .windows(2)
                    .any(|pair| pair[0] == "--run-id" && pair[1] == case.run_id)
        }));
        let diagnostic_cases: BTreeSet<_> = left
            .iter()
            .filter(|case| case.diagnostic_trace_required)
            .map(|case| case.case_id.as_str())
            .collect();
        assert_eq!(
            diagnostic_cases,
            BTreeSet::from([
                "S0-sync",
                "S1-p2a-demand",
                "S2-p2a-lookahead",
                "S3-p2b-demand",
                "R0-sync-range",
                "R1-p2a-range",
                "R2-userfaultfd",
            ])
        );
        for case in left.iter().filter(|case| case.diagnostic_trace_required) {
            let diagnostic = diagnostic_trace_command(case);
            assert!(diagnostic
                .windows(2)
                .any(|pair| pair[0] == "--run-id" && pair[1].ends_with("-trace")));
            assert!(diagnostic.windows(2).any(|pair| {
                pair[0] == "--obmm-async-args" && pair[1].contains("--trace-sample-ppm 10000")
            }));
        }
        for case in left
            .iter()
            .filter(|case| case.mode == EvalMode::SchedulerCore)
        {
            let guest_args = case
                .command
                .windows(2)
                .find_map(|pair| (pair[0] == "--obmm-async-args").then_some(pair[1].as_str()))
                .expect("P2B guest arguments");
            assert!(guest_args.contains("--inflight 1 --lookahead 0"));
        }
        for case in &left {
            let timeout = case
                .command
                .windows(2)
                .find_map(|pair| (pair[0] == "--timeout-sec").then_some(pair[1].as_str()))
                .expect("bounded remote case timeout");
            assert_eq!(
                timeout,
                if case.band == EvalBand::Range {
                    "900"
                } else {
                    "300"
                }
            );
        }
        for case in left.iter().filter(|case| case.sweep == "failure-semantics") {
            assert_eq!(case.operations, 64);
            assert_eq!(
                case.deadline_us,
                if case.outcome == OutcomeProfile::DropTimeout {
                    2_000
                } else {
                    1_000_000
                }
            );
        }
        assert!(left
            .iter()
            .filter(|case| case.outcome == OutcomeProfile::DuplicateLate)
            .all(|case| case.operations
                == if case.band == EvalBand::Scalar {
                    matrix.minimums.scalar_operations
                } else {
                    matrix.minimums.range_bytes / PAGE_BYTES
                }));
        fs::remove_dir_all(output).expect("cleanup");
        fs::remove_dir_all(retry_output).expect("retry cleanup");
    }

    #[test]
    fn raw_evidence_is_never_overwritten() {
        let output = temp_dir();
        fs::create_dir_all(output.join("raw")).expect("raw dir");
        let case = test_case();
        let manifest = test_manifest(vec![case.clone()]);
        ensure_raw_targets_are_new(&output, &manifest).expect("new target");
        fs::write(
            output.join("raw").join(format!("{}.jsonl", case.run_id)),
            "preserved\n",
        )
        .expect("raw evidence");
        let error = ensure_raw_targets_are_new(&output, &manifest)
            .expect_err("existing raw evidence must fail closed");
        assert!(error
            .to_string()
            .contains("refusing to overwrite 1 existing raw run"));
        assert!(error.to_string().contains("new run ID"));
        fs::remove_dir_all(output).expect("cleanup");
    }

    #[test]
    fn acceptance_matrix_covers_every_canonical_path_for_seven_seeds() {
        let matrix: EvalMatrix = serde_yaml::from_slice(
            &fs::read(acceptance_matrix_path()).expect("acceptance matrix bytes"),
        )
        .expect("acceptance matrix");
        validate_matrix(&matrix).expect("valid acceptance matrix");
        let scenario = ScenarioConfig::from_yaml_file(scenario_path()).expect("scenario");
        let output = temp_dir();
        fs::create_dir_all(output.join("models")).expect("model dir");
        let bands = EvalBand::parse_list("scalar,range").expect("bands");
        let cases = expand_matrix(
            &matrix,
            &scenario,
            &scenario_path(),
            &bands,
            &(1..=7).collect::<Vec<_>>(),
            &output,
        )
        .expect("acceptance expansion");
        assert_eq!(cases.len(), 49);
        for seed in 1..=7 {
            let paths = cases
                .iter()
                .filter(|case| case.seed == seed)
                .map(|case| case.case_id.as_str())
                .collect::<BTreeSet<_>>();
            assert_eq!(
                paths,
                BTreeSet::from([
                    "S0-sync",
                    "S1-p2a-demand",
                    "S2-p2a-lookahead",
                    "S3-p2b-demand",
                    "R0-sync-range",
                    "R1-p2a-range",
                    "R2-userfaultfd",
                ])
            );
        }
        fs::remove_dir_all(output).expect("cleanup");
    }

    #[test]
    fn summary_parser_requires_one_complete_numeric_summary() {
        let line = "OBMM_EVAL_SUMMARY schema=1 band=scalar mode=sync-mmio \
                    case=S0-sync seed=1 operations=10000 checksum=0123456789abcdef \
                    failures=0 timeouts=0 guest_ns_p50=10 guest_ns_p95=20 \
                    guest_ns_p99=30 guest_ns_max=40 makespan_ns=2000000000 \
                    model_wait_ns=1000000 useful_work_ns=100 application_cpu_ns=200 \
                    helper_cpu_ns=0 extra_vcpus=0 trace_sample_ppm=0 \
                    trace_sampled=0 trace_dropped=0 ready_ns=0 wait_ns=0 \
                    idle_ns=0 no_ready=0 submit_ns_p50=0 submit_ns_total=0 \
                    switch_ns_p50=0 switch_ns_total=0 cq_drain_ns_p50=0 \
                    cq_drain_ns_total=0 configured_lookahead=0 \
                    backend_pending_high=0 backend_capacity=0 \
                    sink_copy_bytes=0 sink_copy_ns=0 backend_late=0 \
                    backend_duplicate=0 scc_save_cycles=0 \
                    scc_schedule_cycles=0 scc_restore_cycles=0 \
                    scc_commit_cycles=0 el0_upcalls_pending=0 \
                    el0_upcalls_complete=0 el0_upcalls_fault=0 \
                    el0_context_saves=0 el0_context_restores=0 \
                    el0_context_switches=0 el0_context_bytes=0 \
                    el0_scheduler_ns=0 el0_no_ready_waits=0 \
                    direct_el0_upcalls=0 qemu_context_saves=0 \
                    qemu_context_restores=0 qemu_context_switches=0 \
                    qemu_context_bytes=0 uffd_fault_ns_p50=0 \
                    uffd_fault_ns_p95=0 uffd_fault_ns_p99=0 \
                    uffd_fault_ns_max=0 uffd_remote_ns_p50=0 \
                    uffd_remote_ns_p95=0 uffd_remote_ns_p99=0 \
                    uffd_remote_ns_max=0 uffd_copy_ns_p50=0 \
                    uffd_copy_ns_p95=0 uffd_copy_ns_p99=0 \
                    uffd_copy_ns_max=0 uffd_wake_ns_p50=0 \
                    uffd_wake_ns_p95=0 uffd_wake_ns_p99=0 \
                    uffd_wake_ns_max=0 uffd_handler_cpu_ns=0 \
                    uffd_worker_cpu_ns=0 model_pending_final=0 \
                    backend_pending_final=0 scc_pending_final=0 \
                    counter_overflow=0 clock_regressions=0 \
                    fail_closed_process_exit=0 status=pass";
        let parsed = parse_guest_summary(line).expect("summary");
        assert_eq!(parsed.model_wait_ns, Some(1_000_000));
        assert_eq!(parsed.operations, 10_000);
        assert!(parse_guest_summary(&format!("{line}\n{line}\n")).is_err());
        assert!(parse_guest_summary(&line.replace("model_wait_ns=1000000 ", "")).is_err());
    }

    #[test]
    fn p2b_validation_requires_guest_el0_progress_and_zero_qemu_context_state() {
        let mut case = test_case();
        case.case_id = "S3-p2b-demand".into();
        case.mode = EvalMode::SchedulerCore;
        let mut summary = test_summary(2_000_000_000);
        summary.case_id = case.case_id.clone();
        summary.mode = case.mode.as_str().into();
        summary.phase.el0_upcalls_pending = summary.operations;
        summary.phase.el0_upcalls_complete = summary.operations;
        summary.phase.el0_context_saves = 15_000;
        summary.phase.el0_context_restores = 15_008;
        summary.phase.el0_context_switches = 12_000;
        summary.phase.el0_context_bytes = 24_960_000;
        summary.phase.el0_scheduler_ns = 500_000;
        summary.phase.direct_el0_upcalls = summary.phase.el0_context_saves;

        let mut reasons = Vec::new();
        validate_mode_metrics(&case, &summary, &mut reasons);
        assert!(reasons.is_empty(), "{reasons:?}");

        summary.phase.qemu_context_saves = 1;
        summary.phase.el0_context_switches = 0;
        let mut reasons = Vec::new();
        validate_mode_metrics(&case, &summary, &mut reasons);
        assert_eq!(reasons.len(), 2, "{reasons:?}");
        assert!(reasons[0].contains("QEMU-owned"));
        assert!(reasons[1].contains("guest-EL0"));
    }

    #[test]
    fn duplicate_evidence_requires_a_positive_counter() {
        assert!(!evidence_counter_positive(
            "OBMM_ASYNC_SUMMARY duplicate=0 late=0",
            &["duplicate", "late"],
        ));
        assert!(evidence_counter_positive(
            "OBMM_ASYNC_SUMMARY model_duplicated=1 backend_late=0",
            &["model_duplicated", "backend_late"],
        ));
    }

    #[test]
    fn sha256_and_artifact_evidence_fail_closed() {
        let directory = temp_dir();
        fs::create_dir_all(&directory).expect("temp directory");
        let empty = directory.join("empty");
        fs::write(&empty, []).expect("empty file");
        assert_eq!(
            sha256_file(&empty).expect("SHA-256"),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );

        let case = test_case();
        let manifest = test_manifest(vec![case.clone()]);
        let line = format!(
            "OBMM_RUN_EVIDENCE node_count=2 scenario_sha256={} \
             model_file_sha256={} model_contract_hash={} \
             qemu_sha256={0} kernel_sha256={0} initramfs_sha256={0} \
             qemu_destroyed=1",
            manifest.scenario_file_sha256, case.model_file_sha256, case.model_manifest_hash,
        );
        let evidence = serde_json::json!({"line": line}).to_string();
        let mut reasons = Vec::new();
        validate_artifact_evidence(&case, &manifest, &evidence, &mut reasons);
        assert!(reasons.is_empty(), "{reasons:?}");

        let tampered = evidence.replace("node_count=2", "node_count=4");
        validate_artifact_evidence(&case, &manifest, &tampered, &mut reasons);
        assert!(reasons
            .iter()
            .any(|reason| reason == "artifact evidence node_count mismatch"));
        fs::remove_dir_all(directory).expect("cleanup");
    }

    #[test]
    fn invalid_runs_never_contribute_to_aggregate_statistics() {
        let case = test_case();
        let manifest = test_manifest(vec![case]);
        let valid = RawRunRecord {
            schema: 1,
            kind: "run".into(),
            run_id: "test-run".into(),
            exit_code: Some(0),
            host: HostEvidence {
                elapsed_ns: 100,
                load1_milli: None,
                online_cpus: None,
            },
            summary: Some(test_summary(100)),
            diagnostic_summary: None,
            initial_reasons: Vec::new(),
        };
        let mut invalid = valid.clone();
        invalid.summary = Some(test_summary(1));
        let collected = vec![
            CollectedRun {
                case_index: 0,
                record: Some(valid),
                artifact_fingerprint: Some("build-a".into()),
                reasons: Vec::new(),
            },
            CollectedRun {
                case_index: 0,
                record: Some(invalid),
                artifact_fingerprint: Some("build-a".into()),
                reasons: vec!["deliberately invalid".into()],
            },
        ];
        let row = build_aggregate_row(&[0, 1], &collected, &manifest, 1);
        assert_eq!(row.valid_seeds, 1);
        assert_eq!(row.makespan_median, Some(100));
        assert_eq!(row.host_elapsed_median, Some(100));
        assert_eq!(row.checksum_set, "0123456789abcdef");
        assert_eq!(row.requests_per_second, Some(100_000_000_000.0));
        assert_eq!(row.status, "pass");
    }

    #[test]
    fn mixed_artifact_builds_invalidate_every_affected_run() {
        let mut runs = vec![
            CollectedRun {
                case_index: 0,
                record: None,
                artifact_fingerprint: Some("build-a".into()),
                reasons: Vec::new(),
            },
            CollectedRun {
                case_index: 0,
                record: None,
                artifact_fingerprint: Some("build-b".into()),
                reasons: Vec::new(),
            },
        ];
        apply_artifact_consistency(&mut runs);
        assert!(runs.iter().all(|run| run.reasons.len() == 1));
    }

    #[test]
    fn break_even_reports_only_measured_intervals() {
        let bracketed = break_even_interval(&[(0, -10), (5, 0), (10, 20), (50, 40)]);
        assert_eq!(bracketed.nonpositive_us, Some(5));
        assert_eq!(bracketed.positive_us, Some(10));
        assert_eq!(bracketed.status, "bracketed");

        let minimum = break_even_interval(&[(1, 2), (5, 3)]);
        assert_eq!(minimum.nonpositive_us, None);
        assert_eq!(minimum.positive_us, Some(1));
        assert_eq!(minimum.status, "positive-at-minimum");

        let absent = break_even_interval(&[(1, -2), (5, 0)]);
        assert_eq!(absent.positive_us, None);
        assert_eq!(absent.status, "not-observed");

        let non_monotonic = break_even_interval(&[(1, -2), (5, 3), (10, -1)]);
        assert_eq!(non_monotonic.status, "non-monotonic");
    }

    #[test]
    fn logical_operation_identity_ignores_mechanism() {
        let sync = operation_list_hash(EvalBand::Scalar, 7, 8, 10_000, 8, EvalPattern::Sequential);
        let p2a = operation_list_hash(EvalBand::Scalar, 7, 8, 10_000, 8, EvalPattern::Sequential);
        assert_eq!(sync, p2a);
    }

    #[test]
    fn formulas_use_na_for_zero_denominators() {
        let metrics = derive_metrics(1_000, 700, 600, 0, 0, 300, 0);
        assert_eq!(metrics.overlap_hidden_ns, 300);
        assert_eq!(metrics.overlap_efficiency, None);
        assert_eq!(metrics.core_efficiency, None);
        assert_eq!(metrics.schedule_ahead_gain_ns, 100);
        assert_eq!(metrics.mechanism_gain_ns, 300);
    }

    #[test]
    fn bootstrap_ci_is_deterministic_and_contains_median() {
        let values = [90, 100, 105, 110, 120, 95, 115];
        let left = bootstrap_median_ci95(&values, 7).expect("left CI");
        let right = bootstrap_median_ci95(&values, 7).expect("right CI");
        let mut copy = values;
        let center = median(&mut copy).expect("median");
        assert_eq!(left, right);
        assert!(left.0 <= center && center <= left.1);
    }

    #[test]
    fn validation_passes_only_after_every_expected_run_passes() {
        let passing_run = RunValidation {
            run_id: "run-1".into(),
            case_id: "S0-sync".into(),
            seed: 1,
            status: "pass".into(),
            reasons: Vec::new(),
        };
        let mut complete = EvalValidation {
            schema: EVAL_VALIDATION_SCHEMA,
            status: "invalid".into(),
            gates: Vec::new(),
            invalid_reasons: vec![EVAL_INCOMPLETE_REASON.into()],
            expanded_cases: 1,
            formal_seed_count_met: true,
            runs: vec![passing_run.clone()],
        };
        finalize_validation(&mut complete, 1);
        assert_eq!(complete.status, "pass");
        assert!(complete.invalid_reasons.is_empty());

        let mut missing = complete.clone();
        missing.status = "invalid".into();
        missing.invalid_reasons = vec![EVAL_INCOMPLETE_REASON.into()];
        missing.runs.clear();
        finalize_validation(&mut missing, 1);
        assert_eq!(missing.status, "invalid");
        assert_eq!(
            missing.invalid_reasons,
            vec!["expected 1 raw runs, collected 0"]
        );

        let mut failed = complete;
        failed.status = "invalid".into();
        failed.invalid_reasons = vec![EVAL_INCOMPLETE_REASON.into()];
        failed.runs[0].status = "invalid".into();
        finalize_validation(&mut failed, 1);
        assert_eq!(failed.status, "invalid");
        assert_eq!(
            failed.invalid_reasons,
            vec!["one or more raw runs are invalid"]
        );
    }

    #[test]
    fn dry_run_preserves_missing_gates_as_invalid_evidence() {
        let output = temp_dir();
        let args = ObmmEvalCliArgs {
            matrix_path: matrix_path(),
            scenario_path: scenario_path(),
            bands: "scalar,transparency".into(),
            seeds: "1".into(),
            output_dir: output.clone(),
            gate_dir: output.join("missing-gates"),
            remote_target: None,
            remote_repo: None,
            local_repo: None,
            aggregate_only: false,
            dry_run: true,
        };
        run(&args).expect("dry run");
        let validation = fs::read_to_string(output.join("validation.json")).expect("validation");
        let manifest = fs::read_to_string(output.join("run-manifest.json")).expect("manifest");
        assert!(validation.contains("\"status\": \"invalid\""));
        assert!(validation.contains(EVAL_DRY_RUN_REASON));
        assert!(manifest.contains("\"valid_for_execution\": false"));
        assert!(manifest.contains("--eval-case"));
        let transparency =
            fs::read_to_string(output.join("summary/transparency.csv")).expect("Band T");
        assert!(transparency.contains("custom_hardware_components"));
        assert!(transparency.contains("P2B,ordinary-scalar-LDR"));
        let break_even =
            fs::read_to_string(output.join("summary/break-even.csv")).expect("break even");
        assert!(break_even.contains("nonpositive_latency_us,positive_latency_us"));
        let original_manifest = fs::read(output.join("run-manifest.json")).expect("manifest bytes");
        let error = run(&args).expect_err("rerun must not overwrite evaluation evidence");
        assert!(error.to_string().contains("new --output-dir"));
        assert_eq!(
            fs::read(output.join("run-manifest.json")).expect("preserved manifest"),
            original_manifest
        );
        fs::remove_dir_all(output).expect("cleanup");
    }
}
