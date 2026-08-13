use anyhow::Context;
use serde::{Deserialize, Serialize};
use sim_config::{RemoteMemoryJitterMode, RemoteMemoryModelConfig, ScenarioConfig};
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::OsString;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const REMOTE_MEMORY_MANIFEST_SCHEMA: u32 = 1;
const BASELINE_RUN_MANIFEST_SCHEMA: u32 = 1;
const CONFORMANCE_RUN_MANIFEST_SCHEMA: u32 = 1;
const PHASE_GATE_SCHEMA: u32 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub(crate) enum ObmmBaselineCase {
    LocalDram,
    ObmmLocalHit,
    SyncRemoteZero,
    SyncRemoteModeled,
}

impl ObmmBaselineCase {
    fn parse(value: &str) -> anyhow::Result<Self> {
        match value {
            "local-dram" => Ok(Self::LocalDram),
            "obmm-local-hit" => Ok(Self::ObmmLocalHit),
            "sync-remote-zero" => Ok(Self::SyncRemoteZero),
            "sync-remote-modeled" => Ok(Self::SyncRemoteModeled),
            _ => anyhow::bail!(
                "unsupported --case `{value}`; expected local-dram, obmm-local-hit, \
                 sync-remote-zero, or sync-remote-modeled"
            ),
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::LocalDram => "local-dram",
            Self::ObmmLocalHit => "obmm-local-hit",
            Self::SyncRemoteZero => "sync-remote-zero",
            Self::SyncRemoteModeled => "sync-remote-modeled",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ObmmBaselineCliArgs {
    pub scenario_path: PathBuf,
    pub case: ObmmBaselineCase,
    pub access_bytes: u32,
    pub warmup: u64,
    pub iterations: u64,
    pub seed: u64,
    pub output_dir: PathBuf,
    pub remote_target: Option<String>,
    pub remote_repo: Option<PathBuf>,
    pub dry_run: bool,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
struct RemoteMemoryManifestPayloadV1 {
    schema: u32,
    scenario_name: String,
    scenario_seed: u64,
    remote_memory_model: RemoteMemoryModelConfig,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq, Eq)]
struct RemoteMemoryManifestV1 {
    schema: u32,
    manifest_hash: String,
    scenario_name: String,
    scenario_seed: u64,
    remote_memory_model: RemoteMemoryModelConfig,
}

#[derive(Clone, Debug, Serialize)]
struct BaselineRunManifestV1 {
    schema: u32,
    mode: &'static str,
    scenario_path: String,
    model_manifest_path: String,
    model_manifest_hash: String,
    case: ObmmBaselineCase,
    access_bytes: u32,
    warmup: u64,
    iterations: u64,
    seed: u64,
    command: Vec<String>,
}

pub(crate) fn baseline_args() -> anyhow::Result<Option<ObmmBaselineCliArgs>> {
    baseline_args_from(std::env::args_os().skip(1))
}

fn baseline_args_from<I, S>(args: I) -> anyhow::Result<Option<ObmmBaselineCliArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    match args.next() {
        Some(mode) if mode == "obmm-remote-load-baseline" => {
            let mut scenario_path = None;
            let mut case = None;
            let mut access_bytes = None;
            let mut warmup = 1_000;
            let mut iterations = 10_000;
            let mut seed = 1;
            let mut output_dir = None;
            let mut remote_target = None;
            let mut remote_repo = None;
            let mut dry_run = false;
            let mut pending = args.peekable();

            while let Some(value) = pending.next() {
                let text = value.to_string_lossy();
                if text == "--dry-run" {
                    dry_run = true;
                } else if let Some((name, inline_value)) = text.split_once('=') {
                    parse_baseline_value(
                        name,
                        inline_value,
                        &mut scenario_path,
                        &mut case,
                        &mut access_bytes,
                        &mut warmup,
                        &mut iterations,
                        &mut seed,
                        &mut output_dir,
                        &mut remote_target,
                        &mut remote_repo,
                    )?;
                } else if text.starts_with("--") {
                    let next = pending
                        .next()
                        .ok_or_else(|| anyhow::anyhow!("{text} requires a value"))?;
                    parse_baseline_value(
                        &text,
                        &next.to_string_lossy(),
                        &mut scenario_path,
                        &mut case,
                        &mut access_bytes,
                        &mut warmup,
                        &mut iterations,
                        &mut seed,
                        &mut output_dir,
                        &mut remote_target,
                        &mut remote_repo,
                    )?;
                } else {
                    anyhow::bail!("unexpected obmm-remote-load-baseline argument: {text}");
                }
            }

            let access_bytes = access_bytes.unwrap_or(8);
            if !matches!(access_bytes, 8 | 64 | 256 | 4096 | 65536) {
                anyhow::bail!("--access-bytes must be one of 8, 64, 256, 4096, 65536");
            }
            if iterations == 0 {
                anyhow::bail!("--iterations must be greater than zero");
            }
            if !dry_run && (remote_target.is_none() || remote_repo.is_none()) {
                anyhow::bail!("baseline execution requires --remote-target and --remote-repo");
            }
            Ok(Some(ObmmBaselineCliArgs {
                scenario_path: scenario_path
                    .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
                case: case.ok_or_else(|| anyhow::anyhow!("--case is required"))?,
                access_bytes,
                warmup,
                iterations,
                seed,
                output_dir: output_dir
                    .unwrap_or_else(|| PathBuf::from("out/obmm-remote-load/dry-run")),
                remote_target,
                remote_repo,
                dry_run,
            }))
        }
        _ => Ok(None),
    }
}

#[allow(clippy::too_many_arguments)]
fn parse_baseline_value(
    name: &str,
    value: &str,
    scenario_path: &mut Option<PathBuf>,
    case: &mut Option<ObmmBaselineCase>,
    access_bytes: &mut Option<u32>,
    warmup: &mut u64,
    iterations: &mut u64,
    seed: &mut u64,
    output_dir: &mut Option<PathBuf>,
    remote_target: &mut Option<String>,
    remote_repo: &mut Option<PathBuf>,
) -> anyhow::Result<()> {
    match name {
        "--scenario" => *scenario_path = Some(PathBuf::from(value)),
        "--case" => *case = Some(ObmmBaselineCase::parse(value)?),
        "--access-bytes" => *access_bytes = Some(parse_u32(name, value)?),
        "--warmup" => *warmup = parse_u64(name, value)?,
        "--iterations" => *iterations = parse_u64(name, value)?,
        "--seed" => *seed = parse_u64(name, value)?,
        "--output-dir" => *output_dir = Some(PathBuf::from(value)),
        "--remote-target" => *remote_target = Some(value.to_string()),
        "--remote-repo" => *remote_repo = Some(PathBuf::from(value)),
        _ => anyhow::bail!("unknown obmm-remote-load-baseline option: {name}"),
    }
    Ok(())
}

fn parse_u32(name: &str, value: &str) -> anyhow::Result<u32> {
    value
        .parse::<u32>()
        .with_context(|| format!("{name} requires an unsigned 32-bit integer"))
}

fn parse_u64(name: &str, value: &str) -> anyhow::Result<u64> {
    value
        .parse::<u64>()
        .with_context(|| format!("{name} requires an unsigned 64-bit integer"))
}

pub(crate) fn run_baseline_cli(args: &ObmmBaselineCliArgs) -> anyhow::Result<()> {
    let mut config = ScenarioConfig::from_yaml_file(&args.scenario_path).with_context(|| {
        format!(
            "load OBMM baseline scenario {}",
            args.scenario_path.display()
        )
    })?;
    if config.topology.hosts != 2 {
        anyhow::bail!("P0 baseline v1 requires a 2-host scenario");
    }
    match args.case {
        ObmmBaselineCase::LocalDram | ObmmBaselineCase::ObmmLocalHit => {
            config.remote_memory_model = RemoteMemoryModelConfig::default();
        }
        ObmmBaselineCase::SyncRemoteZero => {
            config.remote_memory_model.enabled = true;
            config.remote_memory_model.fixed_latency_ns = 0;
            config.remote_memory_model.jitter = Default::default();
            config.remote_memory_model.tail = Default::default();
            config.remote_memory_model.drop_ppm = 0;
            config.remote_memory_model.error_ppm = 0;
            config.remote_memory_model.duplicate_ppm = 0;
            config.remote_memory_model.seed = args.seed;
        }
        ObmmBaselineCase::SyncRemoteModeled => {
            config.remote_memory_model.seed = args.seed;
        }
    }
    config
        .validate()
        .context("validate case-specific baseline model")?;
    let model_manifest = build_model_manifest(&config)?;
    fs::create_dir_all(&args.output_dir).with_context(|| {
        format!(
            "create OBMM baseline output dir {}",
            args.output_dir.display()
        )
    })?;
    let model_manifest_path = args.output_dir.join("remote_memory_model_manifest_v1.json");
    write_pretty_json(&model_manifest_path, &model_manifest)?;

    let command = planned_baseline_command(args, &model_manifest_path);
    let run_manifest = BaselineRunManifestV1 {
        schema: BASELINE_RUN_MANIFEST_SCHEMA,
        mode: "obmm-remote-load-baseline",
        scenario_path: args.scenario_path.display().to_string(),
        model_manifest_path: model_manifest_path.display().to_string(),
        model_manifest_hash: model_manifest.manifest_hash.clone(),
        case: args.case,
        access_bytes: args.access_bytes,
        warmup: args.warmup,
        iterations: args.iterations,
        seed: args.seed,
        command,
    };
    let run_manifest_path = args.output_dir.join("run-manifest.json");
    write_pretty_json(&run_manifest_path, &run_manifest)?;

    if args.dry_run {
        println!(
            "OBMM_BASELINE_DRY_RUN schema=1 case={} access_bytes={} manifest_hash={} \
             model_manifest={} run_manifest={} status=pass",
            args.case.as_str(),
            args.access_bytes,
            model_manifest.manifest_hash,
            model_manifest_path.display(),
            run_manifest_path.display(),
        );
    } else {
        execute_remote_baseline(args, &run_manifest, &model_manifest_path)?;
    }
    Ok(())
}

fn build_model_manifest(config: &ScenarioConfig) -> anyhow::Result<RemoteMemoryManifestV1> {
    let payload = RemoteMemoryManifestPayloadV1 {
        schema: REMOTE_MEMORY_MANIFEST_SCHEMA,
        scenario_name: config.scenario.name.clone(),
        scenario_seed: config.scenario.seed,
        remote_memory_model: config.remote_memory_model.clone(),
    };
    let canonical =
        serde_json::to_vec(&payload).context("encode canonical remote model manifest")?;
    let manifest_hash = format!("fnv1a64:{:016x}", fnv1a64(&canonical));
    Ok(RemoteMemoryManifestV1 {
        schema: payload.schema,
        manifest_hash,
        scenario_name: payload.scenario_name,
        scenario_seed: payload.scenario_seed,
        remote_memory_model: payload.remote_memory_model,
    })
}

#[cfg(test)]
fn scheduler_core_model_spec(config: &ScenarioConfig) -> String {
    let model = &config.scheduler_core_model;

    format!(
        "v2|enabled={}|contexts={}|pending={}|events={}|clock_mhz={}",
        u8::from(model.enabled),
        model.context_entries,
        model.pending_load_entries,
        model.event_queue_depth,
        model.clock_mhz,
    )
}

fn planned_baseline_command(args: &ObmmBaselineCliArgs, manifest_path: &Path) -> Vec<String> {
    vec![
        "zsh".to_string(),
        "guest-linux/aarch64/scripts/run_ub_obmm_eval.sh".to_string(),
        "--node-count".to_string(),
        "2".to_string(),
        "--scenario-config".to_string(),
        args.scenario_path.display().to_string(),
        "--run-id".to_string(),
        format!("p0-{}-{}", args.case.as_str(), args.seed),
        "--remote-memory-model-manifest".to_string(),
        manifest_path.display().to_string(),
        "--obmm-async-args".to_string(),
        format!(
            "--mode sync-mmio --case {} --access-bytes {} --warmup {} \
             --iterations {} --seed {} --coroutines 1 --compute-us 0 \
             --expected-outcome success --eval-band {} --eval-case P0-{} \
             --min-duration-ms 0 --verify",
            args.case.as_str(),
            args.access_bytes,
            args.warmup,
            args.iterations,
            args.seed,
            if args.access_bytes == 8 {
                "scalar"
            } else {
                "range"
            },
            args.case.as_str(),
        ),
    ]
}

fn execute_remote_baseline(
    args: &ObmmBaselineCliArgs,
    manifest: &BaselineRunManifestV1,
    model_manifest_path: &Path,
) -> anyhow::Result<()> {
    let target = args
        .remote_target
        .as_deref()
        .ok_or_else(|| anyhow::anyhow!("--remote-target is required"))?;
    let remote_repo = args
        .remote_repo
        .as_deref()
        .ok_or_else(|| anyhow::anyhow!("--remote-repo is required"))?;
    if args.output_dir.is_absolute()
        || model_manifest_path.is_absolute()
        || args.scenario_path.is_absolute()
    {
        anyhow::bail!("remote baseline paths must be repository-relative");
    }
    stage_remote_file(
        target,
        &remote_repo.join(model_manifest_path),
        model_manifest_path,
    )?;
    let command = format!(
        "cd {} && if [ -f guest-linux/aarch64/remote-build.env ]; then \
         . guest-linux/aarch64/remote-build.env; fi && {}",
        shell_quote(&remote_repo.display().to_string()),
        manifest
            .command
            .iter()
            .map(|part| shell_quote(part))
            .collect::<Vec<_>>()
            .join(" ")
    );
    let output = Command::new("ssh")
        .arg(target)
        .arg(command)
        .output()
        .with_context(|| format!("execute P0 baseline on {target}"))?;
    fs::create_dir_all(args.output_dir.join("raw"))?;
    let raw_path = args.output_dir.join("raw/baseline.log");
    let mut raw = output.stdout.clone();
    raw.extend_from_slice(&output.stderr);
    fs::write(&raw_path, &raw)?;
    let stdout = String::from_utf8_lossy(&output.stdout);
    let baseline_prefix = format!(
        "OBMM_BASELINE_SUMMARY schema=1 case={} status=pass",
        args.case.as_str()
    );
    let baseline_count = stdout
        .lines()
        .filter(|line| line.starts_with(&baseline_prefix))
        .count();
    let eval_count = stdout
        .lines()
        .filter(|line| line.starts_with("OBMM_EVAL_SUMMARY schema=1"))
        .count();
    if !output.status.success()
        || baseline_count != 2
        || eval_count != 1
        || !stdout.contains("OBMM_RUN_EVIDENCE node_count=2")
    {
        anyhow::bail!("P0 baseline failed; inspect {}", raw_path.display());
    }
    print!("{stdout}");
    Ok(())
}

fn stage_remote_file(target: &str, remote_path: &Path, local_path: &Path) -> anyhow::Result<()> {
    let bytes = fs::read(local_path)?;
    let parent = remote_path
        .parent()
        .ok_or_else(|| anyhow::anyhow!("remote path has no parent"))?;
    let command = format!(
        "mkdir -p {} && cat > {}",
        shell_quote(&parent.display().to_string()),
        shell_quote(&remote_path.display().to_string())
    );
    let mut child = Command::new("ssh")
        .arg(target)
        .arg(command)
        .stdin(Stdio::piped())
        .spawn()?;
    child
        .stdin
        .take()
        .ok_or_else(|| anyhow::anyhow!("remote staging stdin is unavailable"))?
        .write_all(&bytes)?;
    if !child.wait()?.success() {
        anyhow::bail!("staging {} on {target} failed", local_path.display());
    }
    Ok(())
}

fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

fn write_pretty_json<T: Serialize>(path: &Path, value: &T) -> anyhow::Result<()> {
    let mut bytes = serde_json::to_vec_pretty(value).context("encode JSON manifest")?;
    bytes.push(b'\n');
    fs::write(path, bytes).with_context(|| format!("write JSON manifest {}", path.display()))
}

fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325_u64;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    hash
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub(crate) enum ObmmConformanceSink {
    Test,
    P2a,
    P2b,
}

impl ObmmConformanceSink {
    fn parse(value: &str) -> anyhow::Result<Self> {
        match value {
            "test" => Ok(Self::Test),
            "p2a" => Ok(Self::P2a),
            "p2b" => Ok(Self::P2b),
            _ => anyhow::bail!("--sink must be test, p2a, or p2b"),
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Test => "test",
            Self::P2a => "p2a",
            Self::P2b => "p2b",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub(crate) enum ObmmConformanceCase {
    Inline,
    Inflight64,
    Reorder,
    Duplicate,
    Timeout,
    CancelRace,
    Retire,
    Capacity,
}

impl ObmmConformanceCase {
    fn parse(value: &str) -> anyhow::Result<Self> {
        match value {
            "inline" => Ok(Self::Inline),
            "inflight64" => Ok(Self::Inflight64),
            "reorder" => Ok(Self::Reorder),
            "duplicate" => Ok(Self::Duplicate),
            "timeout" => Ok(Self::Timeout),
            "cancel-race" => Ok(Self::CancelRace),
            "retire" => Ok(Self::Retire),
            "capacity" => Ok(Self::Capacity),
            _ => anyhow::bail!(
                "--case must be inline, inflight64, reorder, duplicate, timeout, \
                 cancel-race, retire, or capacity"
            ),
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::Inline => "inline",
            Self::Inflight64 => "inflight64",
            Self::Reorder => "reorder",
            Self::Duplicate => "duplicate",
            Self::Timeout => "timeout",
            Self::CancelRace => "cancel-race",
            Self::Retire => "retire",
            Self::Capacity => "capacity",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ObmmConformanceCliArgs {
    pub scenario_path: PathBuf,
    pub sink: ObmmConformanceSink,
    pub case: ObmmConformanceCase,
    pub access_bytes: u32,
    pub seed: u64,
    pub output_dir: PathBuf,
    pub dry_run: bool,
    pub suite: bool,
}

#[derive(Clone, Debug, Serialize)]
struct ConformanceRunManifestV1 {
    schema: u32,
    mode: &'static str,
    scenario_path: String,
    model_manifest_path: String,
    model_manifest_hash: String,
    sink: ObmmConformanceSink,
    case: ObmmConformanceCase,
    access_bytes: u32,
    seed: u64,
    command: Vec<String>,
}

#[derive(Debug, Serialize)]
struct PhaseGateEvidenceV1 {
    schema: u32,
    phase: &'static str,
    status: &'static str,
    scenario_hash: String,
    model_contract_hash: String,
    evidence: Vec<String>,
}

pub(crate) fn conformance_args() -> anyhow::Result<Option<ObmmConformanceCliArgs>> {
    conformance_args_from(std::env::args_os().skip(1))
}

fn conformance_args_from<I, S>(args: I) -> anyhow::Result<Option<ObmmConformanceCliArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    let mode = args.next();
    let suite = match mode.as_deref() {
        Some(mode) if mode == std::ffi::OsStr::new("obmm-remote-backend-conformance") => false,
        Some(mode) if mode == std::ffi::OsStr::new("obmm-remote-backend-conformance-suite") => true,
        _ => return Ok(None),
    };
    let mut scenario_path = PathBuf::from("scenarios/mvp_2host_single_domain.yaml");
    let mut sink = None;
    let mut case = None;
    let mut access_bytes = 8;
    let mut seed = 1;
    let mut output_dir = PathBuf::from("out/obmm-remote-load/conformance-dry-run");
    let mut dry_run = false;
    let mut pending = args.peekable();

    while let Some(value) = pending.next() {
        let text = value.to_string_lossy();
        if text == "--dry-run" {
            dry_run = true;
            continue;
        }
        let (name, owned_value) = if let Some((name, value)) = text.split_once('=') {
            (name.to_string(), value.to_string())
        } else if text.starts_with("--") {
            let next = pending
                .next()
                .ok_or_else(|| anyhow::anyhow!("{text} requires a value"))?;
            (text.to_string(), next.to_string_lossy().into_owned())
        } else {
            anyhow::bail!("unexpected obmm-remote-backend-conformance argument: {text}");
        };
        match name.as_str() {
            "--scenario" => scenario_path = PathBuf::from(owned_value),
            "--sink" => sink = Some(ObmmConformanceSink::parse(&owned_value)?),
            "--case" => case = Some(ObmmConformanceCase::parse(&owned_value)?),
            "--access-bytes" => access_bytes = parse_u32(&name, &owned_value)?,
            "--seed" => seed = parse_u64(&name, &owned_value)?,
            "--output-dir" => output_dir = PathBuf::from(owned_value),
            _ => anyhow::bail!("unknown obmm-remote-backend-conformance option: {name}"),
        }
    }
    let sink = if suite {
        sink.unwrap_or(ObmmConformanceSink::Test)
    } else {
        sink.ok_or_else(|| anyhow::anyhow!("--sink is required"))?
    };
    let case = if suite {
        case.unwrap_or(ObmmConformanceCase::Inline)
    } else {
        case.ok_or_else(|| anyhow::anyhow!("--case is required"))?
    };
    if !matches!(access_bytes, 1 | 2 | 4 | 8 | 64 | 4096 | 65536) {
        anyhow::bail!("--access-bytes must be one of 1, 2, 4, 8, 64, 4096, 65536");
    }
    if sink == ObmmConformanceSink::P2b && access_bytes > 8 {
        anyhow::bail!("P2B sink only supports 1, 2, 4, or 8 bytes");
    }
    Ok(Some(ObmmConformanceCliArgs {
        scenario_path,
        sink,
        case,
        access_bytes,
        seed,
        output_dir,
        dry_run,
        suite,
    }))
}

pub(crate) fn run_conformance_cli(args: &ObmmConformanceCliArgs) -> anyhow::Result<()> {
    let config = ScenarioConfig::from_yaml_file(&args.scenario_path).with_context(|| {
        format!(
            "load P1 conformance scenario {}",
            args.scenario_path.display()
        )
    })?;
    let model_manifest = build_model_manifest(&config)?;
    fs::create_dir_all(&args.output_dir)?;
    let model_manifest_path = args.output_dir.join("remote_memory_model_manifest_v1.json");
    write_pretty_json(&model_manifest_path, &model_manifest)?;
    let command = vec![
        "vendor/qemu_8.2.0_ub/build/tests/unit/test-ub-obmm-remote".to_string(),
        "--conformance".to_string(),
        "--sink".to_string(),
        args.sink.as_str().to_string(),
        "--case".to_string(),
        args.case.as_str().to_string(),
        format!("--access-bytes={}", args.access_bytes),
    ];
    let run_manifest = ConformanceRunManifestV1 {
        schema: CONFORMANCE_RUN_MANIFEST_SCHEMA,
        mode: "obmm-remote-backend-conformance",
        scenario_path: args.scenario_path.display().to_string(),
        model_manifest_path: model_manifest_path.display().to_string(),
        model_manifest_hash: model_manifest.manifest_hash.clone(),
        sink: args.sink,
        case: args.case,
        access_bytes: args.access_bytes,
        seed: args.seed,
        command,
    };
    let run_manifest_path = args.output_dir.join("run-manifest.json");
    write_pretty_json(&run_manifest_path, &run_manifest)?;
    if args.dry_run {
        println!(
            "OBMM_P1_DRY_RUN schema=1 suite={} sink={} case={} access_bytes={} manifest_hash={} \
             run_manifest={} status=pass",
            u8::from(args.suite),
            args.sink.as_str(),
            args.case.as_str(),
            args.access_bytes,
            model_manifest.manifest_hash,
            run_manifest_path.display()
        );
    } else if args.suite {
        run_conformance_suite(args, &model_manifest, &run_manifest)?;
    } else {
        let binary = Path::new(&run_manifest.command[0]);

        if !binary.is_file() {
            anyhow::bail!(
                "P1 conformance binary {} is missing; build QEMU through \
                 guest-linux/aarch64/scripts/build_qemu_binary.sh",
                binary.display()
            );
        }
        let output = Command::new(binary)
            .args(&run_manifest.command[1..])
            .output()
            .with_context(|| format!("run P1 conformance binary {}", binary.display()))?;
        fs::create_dir_all(args.output_dir.join("raw"))?;
        let raw_path = args.output_dir.join("raw").join(format!(
            "{}-{}.log",
            args.sink.as_str(),
            args.case.as_str()
        ));
        let mut raw = output.stdout.clone();
        raw.extend_from_slice(&output.stderr);
        fs::write(&raw_path, &raw)?;
        let stdout = String::from_utf8_lossy(&output.stdout);
        let expected = format!(
            "OBMM_P1_SUMMARY schema=1 sink={} case={}",
            args.sink.as_str(),
            args.case.as_str()
        );
        if !output.status.success()
            || !stdout
                .lines()
                .any(|line| line.starts_with(&expected) && line.ends_with("status=pass"))
        {
            anyhow::bail!("P1 conformance failed; inspect {}", raw_path.display());
        }
        print!("{stdout}");
    }
    Ok(())
}

fn run_conformance_suite(
    args: &ObmmConformanceCliArgs,
    model_manifest: &RemoteMemoryManifestV1,
    run_manifest: &ConformanceRunManifestV1,
) -> anyhow::Result<()> {
    let binary = Path::new(&run_manifest.command[0]);
    let model_binary = binary.with_file_name("test-ub-obmm-remote-model");
    for required in [binary, model_binary.as_path()] {
        if !required.is_file() {
            anyhow::bail!(
                "P1 suite binary {} is missing; build QEMU through the repository wrapper",
                required.display()
            );
        }
    }
    let raw_dir = args.output_dir.join("raw");
    fs::create_dir_all(&raw_dir)?;
    let sinks = [
        ObmmConformanceSink::Test,
        ObmmConformanceSink::P2a,
        ObmmConformanceSink::P2b,
    ];
    let cases = [
        ObmmConformanceCase::Inline,
        ObmmConformanceCase::Inflight64,
        ObmmConformanceCase::Reorder,
        ObmmConformanceCase::Duplicate,
        ObmmConformanceCase::Timeout,
        ObmmConformanceCase::CancelRace,
        ObmmConformanceCase::Retire,
        ObmmConformanceCase::Capacity,
    ];
    let mut evidence = Vec::new();
    let mut count = 0_u64;
    for sink in sinks {
        let sizes: &[u32] = if sink == ObmmConformanceSink::P2b {
            &[1, 2, 4, 8]
        } else {
            &[1, 2, 4, 8, 64, 4096, 65536]
        };
        for case in cases {
            for access_bytes in sizes {
                let output = Command::new(binary)
                    .args([
                        "--conformance",
                        "--sink",
                        sink.as_str(),
                        "--case",
                        case.as_str(),
                    ])
                    .arg(format!("--access-bytes={access_bytes}"))
                    .output()?;
                let relative = format!(
                    "raw/{}-{}-{}.log",
                    sink.as_str(),
                    case.as_str(),
                    access_bytes
                );
                let path = args.output_dir.join(&relative);
                let mut raw = output.stdout.clone();
                raw.extend_from_slice(&output.stderr);
                fs::write(&path, raw)?;
                let stdout = String::from_utf8_lossy(&output.stdout);
                let expected = format!(
                    "OBMM_P1_SUMMARY schema=1 sink={} case={} ",
                    sink.as_str(),
                    case.as_str()
                );
                if !output.status.success()
                    || !stdout
                        .lines()
                        .any(|line| line.starts_with(&expected) && line.ends_with("status=pass"))
                {
                    anyhow::bail!("P1 suite failed; inspect {}", path.display());
                }
                evidence.push(relative);
                count += 1;
            }
        }
    }
    let model_output = Command::new(&model_binary).output()?;
    let model_success = model_output.status.success();
    let model_relative = "raw/remote-model-unit.log";
    let mut model_raw = model_output.stdout;
    model_raw.extend_from_slice(&model_output.stderr);
    fs::write(args.output_dir.join(model_relative), model_raw)?;
    if !model_success {
        anyhow::bail!("P1 model suite failed; inspect {model_relative}");
    }
    evidence.push(model_relative.into());
    let scenario_bytes = fs::read(&args.scenario_path)?;
    write_pretty_json(
        &args.output_dir.join("p1.json"),
        &PhaseGateEvidenceV1 {
            schema: 1,
            phase: "p1",
            status: "pass",
            scenario_hash: format!("fnv1a64:{:016x}", fnv1a64(&scenario_bytes)),
            model_contract_hash: model_manifest.manifest_hash.clone(),
            evidence,
        },
    )?;
    println!(
        "OBMM_P1_SUITE_SUMMARY schema=1 cases={count} model_unit=pass gate={} status=pass",
        args.output_dir.join("p1.json").display()
    );
    Ok(())
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ObmmPhaseGate {
    P0,
    P2a,
    P2b,
    P4,
}

impl ObmmPhaseGate {
    fn parse(value: &str) -> anyhow::Result<Self> {
        match value {
            "p0" => Ok(Self::P0),
            "p2a" => Ok(Self::P2a),
            "p2b" => Ok(Self::P2b),
            "p4" => Ok(Self::P4),
            _ => anyhow::bail!("--phase must be p0, p2a, p2b, or p4"),
        }
    }

    fn as_str(self) -> &'static str {
        match self {
            Self::P0 => "p0",
            Self::P2a => "p2a",
            Self::P2b => "p2b",
            Self::P4 => "p4",
        }
    }

    fn eval_mode(self) -> &'static str {
        match self {
            Self::P0 => "sync-mmio",
            Self::P2a => "async-poll",
            Self::P2b => "scheduler-core",
            Self::P4 => "userfaultfd",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ObmmPhaseGateCliArgs {
    pub phase: ObmmPhaseGate,
    pub scenario_path: PathBuf,
    pub model_manifest_path: PathBuf,
    pub case_model_manifest_paths: Vec<PathBuf>,
    pub evidence_paths: Vec<PathBuf>,
    pub output_dir: PathBuf,
}

pub(crate) fn phase_gate_args() -> anyhow::Result<Option<ObmmPhaseGateCliArgs>> {
    phase_gate_args_from(std::env::args_os().skip(1))
}

fn phase_gate_args_from<I, S>(args: I) -> anyhow::Result<Option<ObmmPhaseGateCliArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    if args.next().as_deref() != Some(std::ffi::OsStr::new("obmm-remote-load-phase-gate")) {
        return Ok(None);
    }
    let mut phase = None;
    let mut scenario_path = None;
    let mut model_manifest_path = None;
    let mut case_model_manifest_paths = Vec::new();
    let mut evidence_paths = Vec::new();
    let mut output_dir = None;
    let mut pending = args.peekable();

    while let Some(value) = pending.next() {
        let text = value.to_string_lossy();
        let (name, owned_value) = if let Some((name, value)) = text.split_once('=') {
            (name.to_string(), value.to_string())
        } else if text.starts_with("--") {
            let next = pending
                .next()
                .ok_or_else(|| anyhow::anyhow!("{text} requires a value"))?;
            (text.into_owned(), next.to_string_lossy().into_owned())
        } else {
            anyhow::bail!("unexpected obmm-remote-load-phase-gate argument: {text}");
        };
        match name.as_str() {
            "--phase" => phase = Some(ObmmPhaseGate::parse(&owned_value)?),
            "--scenario" => scenario_path = Some(PathBuf::from(owned_value)),
            "--model-manifest" => model_manifest_path = Some(PathBuf::from(owned_value)),
            "--case-model-manifest" => case_model_manifest_paths.push(PathBuf::from(owned_value)),
            "--evidence" => evidence_paths.push(PathBuf::from(owned_value)),
            "--output-dir" => output_dir = Some(PathBuf::from(owned_value)),
            _ => anyhow::bail!("unknown obmm-remote-load-phase-gate option: {name}"),
        }
    }
    if evidence_paths.is_empty() {
        anyhow::bail!("at least one --evidence path is required");
    }
    let phase = phase.ok_or_else(|| anyhow::anyhow!("--phase is required"))?;
    if phase == ObmmPhaseGate::P0 && case_model_manifest_paths.is_empty() {
        anyhow::bail!("P0 requires at least one --case-model-manifest");
    }
    if phase != ObmmPhaseGate::P0 && !case_model_manifest_paths.is_empty() {
        anyhow::bail!("--case-model-manifest is only valid for P0");
    }
    Ok(Some(ObmmPhaseGateCliArgs {
        phase,
        scenario_path: scenario_path.ok_or_else(|| anyhow::anyhow!("--scenario is required"))?,
        model_manifest_path: model_manifest_path
            .ok_or_else(|| anyhow::anyhow!("--model-manifest is required"))?,
        case_model_manifest_paths,
        evidence_paths,
        output_dir: output_dir.ok_or_else(|| anyhow::anyhow!("--output-dir is required"))?,
    }))
}

pub(crate) fn run_phase_gate_cli(args: &ObmmPhaseGateCliArgs) -> anyhow::Result<()> {
    let scenario_bytes = fs::read(&args.scenario_path)
        .with_context(|| format!("read gate scenario {}", args.scenario_path.display()))?;
    ScenarioConfig::from_yaml_file(&args.scenario_path)
        .with_context(|| format!("validate gate scenario {}", args.scenario_path.display()))?;
    let (model_bytes, model) = read_model_manifest(&args.model_manifest_path)?;
    let mut model_catalog = BTreeMap::from([(model.manifest_hash.clone(), model.clone())]);
    let mut case_model_documents = Vec::new();
    for path in &args.case_model_manifest_paths {
        let (bytes, case_model) = read_model_manifest(path)?;
        if case_model.scenario_name != model.scenario_name
            || case_model.scenario_seed != model.scenario_seed
        {
            anyhow::bail!(
                "case model manifest {} belongs to a different scenario",
                path.display()
            );
        }
        if let Some(existing) = model_catalog.get(&case_model.manifest_hash) {
            if existing != &case_model {
                anyhow::bail!("model manifest hash collision");
            }
        } else {
            model_catalog.insert(case_model.manifest_hash.clone(), case_model);
        }
        case_model_documents.push((path, bytes));
    }
    validate_phase_model_catalog(args.phase, &model.manifest_hash, &model_catalog)?;
    let accepted_model_hashes = model_catalog.keys().cloned().collect::<BTreeSet<_>>();

    let mut documents = Vec::new();
    for path in &args.evidence_paths {
        let text = fs::read_to_string(path)
            .with_context(|| format!("read phase evidence {}", path.display()))?;
        validate_phase_evidence(args.phase, &accepted_model_hashes, &text)
            .with_context(|| format!("validate phase evidence {}", path.display()))?;
        documents.push((path, text));
    }
    validate_phase_evidence_set(args.phase, &model.manifest_hash, &model_catalog, &documents)?;

    let evidence_dir = args.output_dir.join("evidence").join(args.phase.as_str());
    fs::create_dir_all(&evidence_dir)?;
    let mut packaged = Vec::new();
    for (index, (source, text)) in documents.iter().enumerate() {
        let relative = PathBuf::from("evidence")
            .join(args.phase.as_str())
            .join(format!(
                "run-{index:02}-{:016x}.log",
                fnv1a64(text.as_bytes())
            ));
        fs::write(args.output_dir.join(&relative), text.as_bytes())
            .with_context(|| format!("package phase evidence {}", source.display()))?;
        packaged.push(relative.display().to_string());
    }
    let model_relative = PathBuf::from("evidence")
        .join(args.phase.as_str())
        .join(format!("model-{:016x}.json", fnv1a64(&model_bytes)));
    fs::write(args.output_dir.join(&model_relative), &model_bytes)?;
    packaged.push(model_relative.display().to_string());
    for (index, (source, bytes)) in case_model_documents.iter().enumerate() {
        let relative = PathBuf::from("evidence")
            .join(args.phase.as_str())
            .join(format!(
                "case-model-{index:02}-{:016x}.json",
                fnv1a64(bytes)
            ));
        fs::write(args.output_dir.join(&relative), bytes)
            .with_context(|| format!("package case model manifest {}", source.display()))?;
        packaged.push(relative.display().to_string());
    }

    let phase = args.phase.as_str();
    write_pretty_json(
        &args.output_dir.join(format!("{phase}.json")),
        &PhaseGateEvidenceV1 {
            schema: PHASE_GATE_SCHEMA,
            phase,
            status: "pass",
            scenario_hash: format!("fnv1a64:{:016x}", fnv1a64(&scenario_bytes)),
            model_contract_hash: model.manifest_hash,
            evidence: packaged,
        },
    )?;
    println!(
        "OBMM_PHASE_GATE_SUMMARY schema=1 phase={phase} runs={} gate={} status=pass",
        args.evidence_paths.len(),
        args.output_dir.join(format!("{phase}.json")).display()
    );
    Ok(())
}

fn read_model_manifest(path: &Path) -> anyhow::Result<(Vec<u8>, RemoteMemoryManifestV1)> {
    let bytes =
        fs::read(path).with_context(|| format!("read gate model manifest {}", path.display()))?;
    let model: RemoteMemoryManifestV1 = serde_json::from_slice(&bytes)
        .with_context(|| format!("decode gate model manifest {}", path.display()))?;
    validate_model_manifest(&model)
        .with_context(|| format!("validate gate model manifest {}", path.display()))?;
    Ok((bytes, model))
}

fn model_has_effect(model: &RemoteMemoryModelConfig) -> bool {
    model.fixed_latency_ns != 0
        || model.jitter.max_abs_ns != 0
        || (model.tail.probability_ppm != 0 && model.tail.extra_latency_ns != 0)
        || model.drop_ppm != 0
        || model.error_ppm != 0
        || model.duplicate_ppm != 0
}

fn model_is_zero_effect(model: &RemoteMemoryModelConfig) -> bool {
    model.enabled
        && model.fixed_latency_ns == 0
        && model.jitter.mode == RemoteMemoryJitterMode::None
        && model.jitter.max_abs_ns == 0
        && model.tail.probability_ppm == 0
        && model.tail.extra_latency_ns == 0
        && model.drop_ppm == 0
        && model.error_ppm == 0
        && model.duplicate_ppm == 0
}

fn validate_phase_model_catalog(
    phase: ObmmPhaseGate,
    canonical_hash: &str,
    catalog: &BTreeMap<String, RemoteMemoryManifestV1>,
) -> anyhow::Result<()> {
    if phase != ObmmPhaseGate::P0 {
        if catalog.len() != 1 {
            anyhow::bail!("non-P0 phase requires exactly the canonical model manifest");
        }
        return Ok(());
    }
    let canonical = catalog
        .get(canonical_hash)
        .ok_or_else(|| anyhow::anyhow!("canonical model manifest is missing"))?;
    if !canonical.remote_memory_model.enabled || !model_has_effect(&canonical.remote_memory_model) {
        anyhow::bail!("P0 canonical modeled manifest must enable a non-zero model effect");
    }
    if !catalog
        .values()
        .any(|entry| !entry.remote_memory_model.enabled)
    {
        anyhow::bail!("P0 model catalog is missing the disabled-model contract");
    }
    if !catalog
        .values()
        .any(|entry| model_is_zero_effect(&entry.remote_memory_model))
    {
        anyhow::bail!("P0 model catalog is missing the zero-delay contract");
    }
    Ok(())
}

fn validate_model_manifest(model: &RemoteMemoryManifestV1) -> anyhow::Result<()> {
    if model.schema != REMOTE_MEMORY_MANIFEST_SCHEMA {
        anyhow::bail!("model manifest schema must be {REMOTE_MEMORY_MANIFEST_SCHEMA}");
    }
    let payload = RemoteMemoryManifestPayloadV1 {
        schema: model.schema,
        scenario_name: model.scenario_name.clone(),
        scenario_seed: model.scenario_seed,
        remote_memory_model: model.remote_memory_model.clone(),
    };
    let expected = format!("fnv1a64:{:016x}", fnv1a64(&serde_json::to_vec(&payload)?));
    if model.manifest_hash != expected {
        anyhow::bail!("model manifest hash mismatch");
    }
    Ok(())
}

fn marker_fields(line: &str, prefix: &str) -> Option<BTreeMap<String, String>> {
    let rest = line.strip_prefix(prefix)?;
    Some(
        rest.split_ascii_whitespace()
            .filter_map(|field| field.split_once('='))
            .map(|(name, value)| (name.to_string(), value.to_string()))
            .collect(),
    )
}

fn required_u64(fields: &BTreeMap<String, String>, name: &str) -> anyhow::Result<u64> {
    fields
        .get(name)
        .ok_or_else(|| anyhow::anyhow!("missing {name}"))?
        .parse::<u64>()
        .with_context(|| format!("{name} must be an unsigned integer"))
}

fn require_field<'a>(
    fields: &'a BTreeMap<String, String>,
    name: &str,
    expected: &str,
) -> anyhow::Result<&'a str> {
    let actual = fields
        .get(name)
        .ok_or_else(|| anyhow::anyhow!("missing {name}"))?;
    if actual != expected {
        anyhow::bail!("{name} must be {expected}, got {actual}");
    }
    Ok(actual)
}

fn exactly_one_marker(text: &str, prefix: &str) -> anyhow::Result<BTreeMap<String, String>> {
    let mut matches = text.lines().filter_map(|line| marker_fields(line, prefix));
    let fields = matches
        .next()
        .ok_or_else(|| anyhow::anyhow!("missing {prefix} marker"))?;
    if matches.next().is_some() {
        anyhow::bail!("more than one {prefix} marker");
    }
    Ok(fields)
}

fn node_markers(
    text: &str,
    prefix: &str,
    node_count: u64,
) -> anyhow::Result<Vec<BTreeMap<String, String>>> {
    let markers = text
        .lines()
        .filter_map(|line| marker_fields(line, prefix))
        .collect::<Vec<_>>();
    if markers.len() != node_count as usize {
        anyhow::bail!(
            "{prefix} marker count must equal node_count ({node_count}), got {}",
            markers.len()
        );
    }
    Ok(markers)
}

fn exactly_one_matching_marker(
    text: &str,
    prefix: &str,
    required: &[(&str, &str)],
) -> anyhow::Result<(usize, BTreeMap<String, String>)> {
    let matches = text
        .lines()
        .enumerate()
        .filter_map(|(index, line)| marker_fields(line, prefix).map(|fields| (index, fields)))
        .filter(|(_, fields)| {
            required
                .iter()
                .all(|(name, value)| fields.get(*name).map(String::as_str) == Some(*value))
        })
        .collect::<Vec<_>>();
    if matches.len() != 1 {
        anyhow::bail!(
            "expected one {prefix} marker matching {required:?}, got {}",
            matches.len()
        );
    }
    Ok(matches.into_iter().next().expect("one matching marker"))
}

fn validate_p2b_producer_consumer_evidence(text: &str, node_count: u64) -> anyhow::Result<()> {
    if node_count != 2 {
        anyhow::bail!("P2B producer/consumer gate requires a two-node run");
    }
    let node_evidence = node_markers(text, "OBMM_P2B_NODE_EVIDENCE ", node_count)?;
    let producer_node = node_evidence
        .iter()
        .find(|fields| fields.get("role").map(String::as_str) == Some("producer"))
        .ok_or_else(|| anyhow::anyhow!("missing P2B producer node evidence"))?;
    let consumer_node = node_evidence
        .iter()
        .find(|fields| fields.get("role").map(String::as_str) == Some("consumer"))
        .ok_or_else(|| anyhow::anyhow!("missing P2B consumer node evidence"))?;
    require_field(producer_node, "node", "nodeA")?;
    require_field(producer_node, "status", "ready")?;
    require_field(consumer_node, "node", "nodeB")?;
    require_field(consumer_node, "status", "pass")?;

    let export = exactly_one_marker(text, "OBMM_P2B_EXPORT ")?;
    require_field(&export, "schema", "1")?;
    require_field(&export, "role", "producer")?;
    require_field(&export, "node", "0")?;
    require_field(&export, "status", "ready")?;
    let export_mem_id = export
        .get("export_mem_id")
        .ok_or_else(|| anyhow::anyhow!("missing producer export_mem_id"))?;
    if required_u64(&export, "bytes")? == 0 {
        anyhow::bail!("P2B producer export must be non-empty");
    }

    let import = exactly_one_marker(text, "OBMM_P2B_IMPORT ")?;
    require_field(&import, "schema", "1")?;
    require_field(&import, "role", "consumer")?;
    require_field(&import, "node", "1")?;
    require_field(&import, "producer_node", "0")?;
    require_field(&import, "source_export_mem_id", export_mem_id)?;
    require_field(&import, "status", "mapped")?;

    let summary = exactly_one_marker(text, "OBMM_P2B_SUMMARY ")?;
    require_field(&summary, "schema", "1")?;
    require_field(&summary, "role", "consumer")?;
    require_field(&summary, "producer_node", "0")?;
    require_field(&summary, "consumer_node", "1")?;
    require_field(&summary, "source_export_mem_id", export_mem_id)?;
    require_field(&summary, "status", "pass")?;
    let coroutines = required_u64(&summary, "coroutines")?;
    if coroutines < 2
        || required_u64(&summary, "completed")? != coroutines
        || required_u64(&summary, "values_verified")? != coroutines
        || required_u64(&summary, "el0_upcalls_pending")? != coroutines
        || required_u64(&summary, "el0_upcalls_complete")? != coroutines
        || required_u64(&summary, "el0_upcalls_fault")? != 0
        || required_u64(&summary, "el0_context_saves")? == 0
        || required_u64(&summary, "el0_context_restores")? == 0
        || required_u64(&summary, "el0_context_switches")? == 0
        || required_u64(&summary, "direct_el0_upcalls")? == 0
        || required_u64(&summary, "qemu_context_saves")? != 0
        || required_u64(&summary, "qemu_context_restores")? != 0
        || required_u64(&summary, "qemu_context_switches")? != 0
        || required_u64(&summary, "qemu_context_bytes")? != 0
        || required_u64(&summary, "scc_pending_final")? != 0
        || required_u64(&summary, "backend_pending_final")? != 0
        || required_u64(&summary, "trace_dropped")? != 0
    {
        anyhow::bail!("P2B summary does not prove EL0 scheduler progress and drain");
    }
    if required_u64(&summary, "el0_context_saves")? != required_u64(&summary, "direct_el0_upcalls")?
    {
        anyhow::bail!("P2B EL0 context saves must match direct EL0 upcalls");
    }
    if required_u64(&export, "writes")? != coroutines {
        anyhow::bail!("P2B producer writes must match the coroutine count");
    }

    let mut blocked_load_switches = 0;
    for coroutine in 0..coroutines {
        let coroutine_text = coroutine.to_string();
        let (_, write) = exactly_one_matching_marker(
            text,
            "OBMM_P2B_WRITE ",
            &[("coroutine", coroutine_text.as_str())],
        )?;
        require_field(&write, "export_mem_id", export_mem_id)?;
        let expected = write
            .get("value")
            .ok_or_else(|| anyhow::anyhow!("missing producer value"))?;
        let offset = write
            .get("offset")
            .ok_or_else(|| anyhow::anyhow!("missing producer offset"))?;

        let (_, context) = exactly_one_matching_marker(
            text,
            "OBMM_P2B_CONTEXT ",
            &[("coroutine", coroutine_text.as_str())],
        )?;
        require_field(&context, "state", "ready")?;
        let context_id = context
            .get("context_id")
            .ok_or_else(|| anyhow::anyhow!("missing coroutine context_id"))?;

        let (issue_position, issue) = exactly_one_matching_marker(
            text,
            "OBMM_P2B_LDR ",
            &[("event", "issue"), ("coroutine", coroutine_text.as_str())],
        )?;
        require_field(&issue, "context_id", context_id)?;
        require_field(&issue, "offset", offset)?;
        require_field(&issue, "expected", expected)?;

        let (pending_position, pending) = exactly_one_matching_marker(
            text,
            "OBMM_P2B_UPCALL ",
            &[("event", "pending"), ("coroutine", coroutine_text.as_str())],
        )?;
        require_field(&pending, "context_id", context_id)?;
        require_field(&pending, "bytes", "8")?;
        require_field(&pending, "status", "0")?;

        let (complete_position, complete) = exactly_one_matching_marker(
            text,
            "OBMM_P2B_UPCALL ",
            &[
                ("event", "complete"),
                ("coroutine", coroutine_text.as_str()),
            ],
        )?;
        require_field(&complete, "context_id", context_id)?;
        require_field(&complete, "bytes", "8")?;
        require_field(&complete, "status", "0")?;
        require_field(&complete, "value", expected)?;
        require_field(
            &complete,
            "token",
            pending
                .get("token")
                .ok_or_else(|| anyhow::anyhow!("missing pending token"))?,
        )?;
        require_field(
            &complete,
            "pc",
            pending
                .get("pc")
                .ok_or_else(|| anyhow::anyhow!("missing pending PC"))?,
        )?;

        let (resume_position, resume) = exactly_one_matching_marker(
            text,
            "OBMM_P2B_SCHEDULE ",
            &[
                ("event", "resume"),
                ("to_coroutine", coroutine_text.as_str()),
                ("after_complete", "1"),
            ],
        )?;
        require_field(&resume, "to_context_id", context_id)?;

        let (retire_position, retire) = exactly_one_matching_marker(
            text,
            "OBMM_P2B_LDR ",
            &[("event", "retire"), ("coroutine", coroutine_text.as_str())],
        )?;
        require_field(&retire, "context_id", context_id)?;
        require_field(&retire, "offset", offset)?;
        require_field(&retire, "expected", expected)?;
        require_field(&retire, "actual", expected)?;
        require_field(&retire, "status", "pass")?;

        let (_, coroutine_summary) = exactly_one_matching_marker(
            text,
            "OBMM_P2B_COROUTINE_SUMMARY ",
            &[("coroutine", coroutine_text.as_str())],
        )?;
        require_field(&coroutine_summary, "context_id", context_id)?;
        require_field(&coroutine_summary, "expected", expected)?;
        require_field(&coroutine_summary, "actual", expected)?;
        require_field(&coroutine_summary, "pending", "1")?;
        require_field(&coroutine_summary, "complete", "1")?;
        require_field(&coroutine_summary, "status", "pass")?;
        if required_u64(&coroutine_summary, "resumes_after_complete")? == 0 {
            anyhow::bail!("P2B coroutine {coroutine} was not resumed after completion");
        }
        if !(issue_position < pending_position
            && pending_position < complete_position
            && complete_position < resume_position
            && resume_position < retire_position)
        {
            anyhow::bail!("P2B coroutine {coroutine} causal event order is invalid");
        }
        let interval = text
            .lines()
            .enumerate()
            .filter(|(position, _)| *position > pending_position && *position < complete_position)
            .map(|(_, line)| line)
            .collect::<Vec<_>>();
        let switched_to_another = interval.iter().any(|line| {
            marker_fields(line, "OBMM_P2B_SCHEDULE ").is_some_and(|fields| {
                fields.get("event").map(String::as_str) == Some("resume")
                    && fields.get("to_context_id").map(String::as_str) != Some(context_id.as_str())
            })
        });
        let another_coroutine_issued_load = interval.iter().any(|line| {
            marker_fields(line, "OBMM_P2B_LDR ").is_some_and(|fields| {
                fields.get("event").map(String::as_str) == Some("issue")
                    && fields.get("context_id").map(String::as_str) != Some(context_id.as_str())
            })
        });
        if switched_to_another && another_coroutine_issued_load {
            blocked_load_switches += 1;
        }
    }
    if blocked_load_switches == 0 {
        anyhow::bail!(
            "P2B does not prove that a blocked load switched to another coroutine that issued an LDR"
        );
    }

    let causal = exactly_one_marker(text, "OBMM_P2B_CAUSAL_SUMMARY ")?;
    require_field(&causal, "status", "pass")?;
    if required_u64(&causal, "blocked_load_switches")? != blocked_load_switches {
        anyhow::bail!("P2B blocked-load switch count does not match causal trace");
    }

    for backend in node_markers(text, "OBMM_BACKEND_EVIDENCE ", node_count)? {
        require_field(&backend, "duplicate", "0")?;
        require_field(&backend, "late", "0")?;
        require_field(&backend, "drained", "1")?;
    }
    Ok(())
}

fn validate_phase_evidence(
    phase: ObmmPhaseGate,
    accepted_model_hashes: &BTreeSet<String>,
    text: &str,
) -> anyhow::Result<()> {
    let run = exactly_one_marker(text, "OBMM_RUN_EVIDENCE ")?;
    let model_hash = run
        .get("model_contract_hash")
        .ok_or_else(|| anyhow::anyhow!("missing model_contract_hash"))?;
    if !accepted_model_hashes.contains(model_hash) {
        anyhow::bail!("model_contract_hash is not present in the gate model catalog");
    }
    require_field(&run, "qemu_destroyed", "1")?;
    let node_count = required_u64(&run, "node_count")?;
    if !matches!(node_count, 2 | 4 | 8) {
        anyhow::bail!("node_count must be 2, 4, or 8");
    }
    if phase == ObmmPhaseGate::P2b {
        return validate_p2b_producer_consumer_evidence(text, node_count);
    }

    let summary = exactly_one_marker(text, "OBMM_EVAL_SUMMARY ")?;
    require_field(&summary, "schema", "1")?;
    require_field(&summary, "mode", phase.eval_mode())?;
    require_field(&summary, "status", "pass")?;
    for field in [
        "failures",
        "timeouts",
        "model_pending_final",
        "backend_pending_final",
        "scc_pending_final",
        "trace_dropped",
        "backend_late",
        "backend_duplicate",
        "counter_overflow",
        "clock_regressions",
        "fail_closed_process_exit",
    ] {
        require_field(&summary, field, "0")?;
    }
    let operations = required_u64(&summary, "operations")?;
    if operations == 0 {
        anyhow::bail!("operations must be greater than zero");
    }
    let checksum = summary
        .get("checksum")
        .ok_or_else(|| anyhow::anyhow!("missing checksum"))?;
    if checksum.len() != 16 || u64::from_str_radix(checksum, 16).is_err() {
        anyhow::bail!("checksum must be a 16-digit hexadecimal value");
    }

    let node_evidence = if phase == ObmmPhaseGate::P0 {
        Vec::new()
    } else {
        node_markers(text, "OBMM_NODE_EVIDENCE ", node_count)?
    };
    for node in &node_evidence {
        require_field(node, "drained", "1")?;
        require_field(node, "mode", phase.eval_mode())?;
        require_field(node, "status", "pass")?;
        require_field(node, "checksum", checksum)?;
        if required_u64(node, "operations")? != operations {
            anyhow::bail!("node operations must match OBMM_EVAL_SUMMARY operations");
        }
        for field in [
            "failures",
            "timeouts",
            "model_pending_final",
            "backend_pending_final",
            "scc_pending_final",
            "trace_dropped",
            "backend_late",
            "backend_duplicate",
            "counter_overflow",
            "clock_regressions",
            "fail_closed_process_exit",
        ] {
            require_field(node, field, "0")?;
        }
    }
    if phase != ObmmPhaseGate::P0 {
        for backend in node_markers(text, "OBMM_BACKEND_EVIDENCE ", node_count)? {
            require_field(&backend, "duplicate", "0")?;
            require_field(&backend, "late", "0")?;
            require_field(&backend, "drained", "1")?;
        }
    }

    match phase {
        ObmmPhaseGate::P0 => {
            if node_count != 2 {
                anyhow::bail!("P0 gate requires a two-node run");
            }
            let baseline_lines = text
                .lines()
                .filter_map(|line| marker_fields(line, "OBMM_BASELINE_SUMMARY "))
                .collect::<Vec<_>>();
            if baseline_lines.len() != node_count as usize {
                anyhow::bail!("P0 requires one baseline summary per node");
            }
            for baseline in baseline_lines {
                require_field(&baseline, "schema", "1")?;
                require_field(&baseline, "status", "pass")?;
                require_field(&baseline, "failures", "0")?;
                require_field(&baseline, "timeouts", "0")?;
                require_field(&baseline, "model_pending", "0")?;
                require_field(&baseline, "backend_pending", "0")?;
            }
        }
        ObmmPhaseGate::P2a => {
            require_field(&summary, "extra_vcpus", "0")?;
            for node in &node_evidence {
                require_field(node, "extra_vcpus", "0")?;
                if required_u64(node, "useful_work_ns")? == 0
                    || required_u64(node, "switch_ns_total")? == 0
                    || required_u64(node, "cq_drain_ns_total")? == 0
                {
                    anyhow::bail!("P2A node does not prove await/switch/resume progress");
                }
            }
            for async_summary in node_markers(text, "OBMM_ASYNC_SUMMARY ", node_count)? {
                require_field(&async_summary, "abi", "1")?;
                require_field(&async_summary, "mode", "async-poll")?;
                require_field(&async_summary, "status", "pass")?;
                require_field(&async_summary, "failures", "0")?;
                require_field(&async_summary, "timeouts", "0")?;
                require_field(&async_summary, "stale", "0")?;
                require_field(&async_summary, "model_pending", "0")?;
                require_field(&async_summary, "backend_pending", "0")?;
                require_field(&async_summary, "scc_pending", "0")?;
                require_field(&async_summary, "checksum", checksum)?;
                if required_u64(&async_summary, "coroutines")? < 2
                    || required_u64(&async_summary, "completed")? != operations
                    || required_u64(&async_summary, "switches")? == 0
                {
                    anyhow::bail!("P2A evidence does not prove await/switch/resume progress");
                }
            }
        }
        ObmmPhaseGate::P2b => unreachable!("P2B is validated before common symmetric evidence"),
        ObmmPhaseGate::P4 => {
            require_field(&summary, "extra_vcpus", "1")?;
            let expected_copy_bytes = operations
                .checked_mul(4096)
                .ok_or_else(|| anyhow::anyhow!("P4 sink byte count overflow"))?;
            for node in &node_evidence {
                require_field(node, "extra_vcpus", "1")?;
                if required_u64(node, "helper_cpu_ns")? == 0
                    || required_u64(node, "uffd_handler_cpu_ns")? == 0
                    || required_u64(node, "uffd_worker_cpu_ns")? == 0
                {
                    anyhow::bail!("P4 evidence must account for handler and worker CPU time");
                }
                if required_u64(node, "sink_copy_bytes")? != expected_copy_bytes {
                    anyhow::bail!("P4 sink_copy_bytes must equal operations * 4096");
                }
            }
            for uffd in node_markers(text, "OBMM_UFFD_SUMMARY ", node_count)? {
                require_field(&uffd, "schema", "1")?;
                require_field(&uffd, "case", "missing-remote")?;
                require_field(&uffd, "status", "pass")?;
                require_field(&uffd, "failures", "0")?;
                require_field(&uffd, "duplicates", "0")?;
                require_field(&uffd, "model_pending", "0")?;
                require_field(&uffd, "backend_pending", "0")?;
                require_field(&uffd, "checksum", checksum)?;
                for field in ["pages", "faults", "remote_reads", "copy_ok"] {
                    if required_u64(&uffd, field)? != operations {
                        anyhow::bail!("P4 {field} must equal operations");
                    }
                }
                if required_u64(&uffd, "handler_cpu_ns")? == 0
                    || required_u64(&uffd, "worker_cpu_ns")? == 0
                {
                    anyhow::bail!("P4 detail must account for handler and worker CPU time");
                }
                for field in [
                    "fault_ns_max",
                    "remote_ns_max",
                    "copy_ns_max",
                    "wake_ns_max",
                ] {
                    if required_u64(&uffd, field)? == 0 {
                        anyhow::bail!("{field} must be greater than zero");
                    }
                }
            }
        }
    }
    Ok(())
}

fn validate_phase_evidence_set(
    phase: ObmmPhaseGate,
    canonical_model_hash: &str,
    model_catalog: &BTreeMap<String, RemoteMemoryManifestV1>,
    documents: &[(&PathBuf, String)],
) -> anyhow::Result<()> {
    if phase != ObmmPhaseGate::P0 {
        return Ok(());
    }
    let mut cases = BTreeSet::new();
    let mut checksums = BTreeSet::new();
    let mut operations = BTreeSet::new();
    for (_, text) in documents {
        let run = exactly_one_marker(text, "OBMM_RUN_EVIDENCE ")?;
        let model_hash = run
            .get("model_contract_hash")
            .ok_or_else(|| anyhow::anyhow!("missing model_contract_hash"))?;
        let model = model_catalog
            .get(model_hash)
            .ok_or_else(|| anyhow::anyhow!("P0 run model is not in the model catalog"))?;
        let baseline = text
            .lines()
            .find_map(|line| marker_fields(line, "OBMM_BASELINE_SUMMARY "))
            .ok_or_else(|| anyhow::anyhow!("missing P0 baseline marker"))?;
        let case = baseline
            .get("case")
            .ok_or_else(|| anyhow::anyhow!("missing P0 case"))?;
        validate_p0_case_model(case, model_hash, canonical_model_hash, model)?;
        cases.insert(case.clone());
        checksums.insert(
            baseline
                .get("checksum")
                .ok_or_else(|| anyhow::anyhow!("missing P0 checksum"))?
                .clone(),
        );
        operations.insert(required_u64(&baseline, "iterations")?);
    }
    let required = BTreeSet::from([
        "local-dram".to_string(),
        "obmm-local-hit".to_string(),
        "sync-remote-zero".to_string(),
        "sync-remote-modeled".to_string(),
    ]);
    if cases != required {
        anyhow::bail!("P0 gate requires exactly the four canonical baseline cases");
    }
    if checksums.len() != 1 || operations.len() != 1 {
        anyhow::bail!("P0 baseline payload/operation identity differs across cases");
    }
    Ok(())
}

fn validate_p0_case_model(
    case: &str,
    model_hash: &str,
    canonical_model_hash: &str,
    model: &RemoteMemoryManifestV1,
) -> anyhow::Result<()> {
    match case {
        "local-dram" | "obmm-local-hit" => {
            if model.remote_memory_model.enabled {
                anyhow::bail!("P0 {case} requires a disabled remote-memory model");
            }
        }
        "sync-remote-zero" => {
            if !model_is_zero_effect(&model.remote_memory_model) {
                anyhow::bail!("P0 sync-remote-zero requires an enabled zero-effect model");
            }
        }
        "sync-remote-modeled" => {
            if model_hash != canonical_model_hash {
                anyhow::bail!("P0 sync-remote-modeled must use the canonical model manifest");
            }
        }
        _ => anyhow::bail!("unknown P0 baseline case {case}"),
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT_TEMP_ID: AtomicU64 = AtomicU64::new(0);

    fn scenario_path() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../scenarios/mvp_2host_single_domain.yaml")
    }

    fn temp_output_dir() -> PathBuf {
        std::env::temp_dir().join(format!(
            "ub-sim-obmm-baseline-{}-{}",
            std::process::id(),
            NEXT_TEMP_ID.fetch_add(1, Ordering::Relaxed)
        ))
    }

    fn phase_evidence(phase: ObmmPhaseGate, model_hash: &str) -> String {
        if phase == ObmmPhaseGate::P2b {
            return format!(
                "OBMM_RUN_EVIDENCE node_count=2 model_contract_hash={model_hash} \
                 qemu_destroyed=1\n\
                 OBMM_P2B_NODE_EVIDENCE node=nodeA role=producer \
                 export_mem_id=17 writes=2 status=ready\n\
                 OBMM_P2B_NODE_EVIDENCE node=nodeB role=consumer \
                 source_export_mem_id=17 coroutines=2 completed=2 status=pass\n\
                 OBMM_P2B_CAUSAL_SUMMARY blocked_load_switches=1 status=pass\n\
                 OBMM_P2B_WRITE schema=1 producer_node=0 coroutine=0 \
                 export_mem_id=17 offset=4096 value=1111111111111111\n\
                 OBMM_P2B_WRITE schema=1 producer_node=0 coroutine=1 \
                 export_mem_id=17 offset=8192 value=2222222222222222\n\
                 OBMM_P2B_EXPORT schema=1 role=producer node=0 export_mem_id=17 \
                 remote_uba=0000000000100000 bytes=2097152 writes=2 status=ready\n\
                 OBMM_P2B_IMPORT schema=1 role=consumer node=1 producer_node=0 \
                 source_export_mem_id=17 import_mem_id=33 bytes=2097152 status=mapped\n\
                 OBMM_P2B_CONTEXT schema=1 coroutine=0 context_id=abc0 state=ready\n\
                 OBMM_P2B_CONTEXT schema=1 coroutine=1 context_id=abc1 state=ready\n\
                 OBMM_P2B_SCHEDULE schema=1 event=resume from_context_id=0000 \
                 to_context_id=abc0 to_coroutine=0 after_complete=0\n\
                 OBMM_P2B_LDR schema=1 event=issue coroutine=0 context_id=abc0 \
                 offset=4096 expected=1111111111111111\n\
                 OBMM_P2B_UPCALL schema=1 event=pending coroutine=0 context_id=abc0 \
                 sequence=1 token=1001 pc=2000 bytes=8 rt=0 status=0\n\
                 OBMM_P2B_SCHEDULE schema=1 event=resume from_context_id=abc0 \
                 to_context_id=abc1 to_coroutine=1 after_complete=0\n\
                 OBMM_P2B_LDR schema=1 event=issue coroutine=1 context_id=abc1 \
                 offset=8192 expected=2222222222222222\n\
                 OBMM_P2B_UPCALL schema=1 event=pending coroutine=1 context_id=abc1 \
                 sequence=2 token=1002 pc=3000 bytes=8 rt=1 status=0\n\
                 OBMM_P2B_UPCALL schema=1 event=complete coroutine=0 context_id=abc0 \
                 sequence=3 token=1001 pc=2000 bytes=8 rt=0 \
                 value=1111111111111111 status=0\n\
                 OBMM_P2B_SCHEDULE schema=1 event=resume from_context_id=abc1 \
                 to_context_id=abc0 to_coroutine=0 after_complete=1\n\
                 OBMM_P2B_LDR schema=1 event=retire coroutine=0 context_id=abc0 \
                 offset=4096 expected=1111111111111111 actual=1111111111111111 \
                 latency_ns=10000 status=pass\n\
                 OBMM_P2B_UPCALL schema=1 event=complete coroutine=1 context_id=abc1 \
                 sequence=4 token=1002 pc=3000 bytes=8 rt=1 \
                 value=2222222222222222 status=0\n\
                 OBMM_P2B_SCHEDULE schema=1 event=resume from_context_id=abc0 \
                 to_context_id=abc1 to_coroutine=1 after_complete=1\n\
                 OBMM_P2B_LDR schema=1 event=retire coroutine=1 context_id=abc1 \
                 offset=8192 expected=2222222222222222 actual=2222222222222222 \
                 latency_ns=11000 status=pass\n\
                 OBMM_P2B_COROUTINE_SUMMARY schema=1 coroutine=0 context_id=abc0 \
                 expected=1111111111111111 actual=1111111111111111 pending=1 \
                 complete=1 resumes_after_complete=1 status=pass\n\
                 OBMM_P2B_COROUTINE_SUMMARY schema=1 coroutine=1 context_id=abc1 \
                 expected=2222222222222222 actual=2222222222222222 pending=1 \
                 complete=1 resumes_after_complete=1 status=pass\n\
                 OBMM_P2B_SUMMARY schema=1 role=consumer producer_node=0 \
                 consumer_node=1 source_export_mem_id=17 import_mem_id=33 \
                 coroutines=2 completed=2 values_verified=2 \
                 el0_upcalls_pending=2 el0_upcalls_complete=2 el0_upcalls_fault=0 \
                 el0_context_saves=4 el0_context_restores=6 \
                 el0_context_switches=3 direct_el0_upcalls=4 \
                 qemu_context_saves=0 qemu_context_restores=0 \
                 qemu_context_switches=0 qemu_context_bytes=0 \
                 scc_pending_final=0 backend_pending_final=0 trace_dropped=0 \
                 status=pass\n\
                 OBMM_BACKEND_EVIDENCE node=nodeA duplicate=0 late=0 drained=1\n\
                 OBMM_BACKEND_EVIDENCE node=nodeB duplicate=0 late=0 drained=1\n"
            );
        }
        let (mode, extra_vcpus, phase_fields, node_detail, detail) = match phase {
            ObmmPhaseGate::P0 => (
                "sync-mmio",
                0,
                "switch_ns_total=0 cq_drain_ns_total=0 \
                 scc_save_cycles=0 scc_schedule_cycles=0 scc_restore_cycles=0 \
                 scc_commit_cycles=0 uffd_fault_ns_max=0 uffd_remote_ns_max=0 \
                 uffd_copy_ns_max=0 uffd_wake_ns_max=0",
                "",
                "OBMM_BASELINE_SUMMARY schema=1 case=local-dram status=pass \
                 iterations=4 checksum=0123456789abcdef failures=0 timeouts=0 \
                 model_pending=0 backend_pending=0\n\
                 OBMM_BASELINE_SUMMARY schema=1 case=local-dram status=pass \
                 iterations=4 checksum=0123456789abcdef failures=0 timeouts=0 \
                 model_pending=0 backend_pending=0",
            ),
            ObmmPhaseGate::P2a => (
                "async-poll",
                0,
                "useful_work_ns=40 switch_ns_total=20 cq_drain_ns_total=30 \
                 scc_save_cycles=0 scc_schedule_cycles=0 scc_restore_cycles=0 \
                 scc_commit_cycles=0 uffd_fault_ns_max=0 uffd_remote_ns_max=0 \
                 uffd_copy_ns_max=0 uffd_wake_ns_max=0",
                "OBMM_NODE_EVIDENCE node=nodeA drained=1 mode=async-poll \
                 operations=4 checksum=0123456789abcdef failures=0 timeouts=0 \
                 useful_work_ns=40 helper_cpu_ns=0 extra_vcpus=0 \
                 switch_ns_total=20 cq_drain_ns_total=30 sink_copy_bytes=32 \
                 model_pending_final=0 backend_pending_final=0 scc_pending_final=0 \
                 trace_dropped=0 backend_late=0 backend_duplicate=0 \
                 counter_overflow=0 clock_regressions=0 fail_closed_process_exit=0 \
                 status=pass\n\
                 OBMM_NODE_EVIDENCE node=nodeB drained=1 mode=async-poll \
                 operations=4 checksum=0123456789abcdef failures=0 timeouts=0 \
                 useful_work_ns=40 helper_cpu_ns=0 extra_vcpus=0 \
                 switch_ns_total=20 cq_drain_ns_total=30 sink_copy_bytes=32 \
                 model_pending_final=0 backend_pending_final=0 scc_pending_final=0 \
                 trace_dropped=0 backend_late=0 backend_duplicate=0 \
                 counter_overflow=0 clock_regressions=0 fail_closed_process_exit=0 \
                 status=pass\n",
                "OBMM_ASYNC_SUMMARY abi=1 mode=async-poll status=pass \
                 coroutines=2 completed=4 switches=3 failures=0 timeouts=0 stale=0 \
                 checksum=0123456789abcdef model_pending=0 backend_pending=0 \
                 scc_pending=0\n\
                 OBMM_BACKEND_EVIDENCE node=nodeA duplicate=0 late=0 drained=1\n\
                 OBMM_ASYNC_SUMMARY abi=1 mode=async-poll status=pass \
                 coroutines=2 completed=4 switches=3 failures=0 timeouts=0 stale=0 \
                 checksum=0123456789abcdef model_pending=0 backend_pending=0 \
                 scc_pending=0\n\
                 OBMM_BACKEND_EVIDENCE node=nodeB duplicate=0 late=0 drained=1",
            ),
            ObmmPhaseGate::P2b => unreachable!("P2B fixture returned above"),
            ObmmPhaseGate::P4 => (
                "userfaultfd",
                1,
                "switch_ns_total=0 cq_drain_ns_total=0 \
                 scc_save_cycles=0 scc_schedule_cycles=0 scc_restore_cycles=0 \
                 scc_commit_cycles=0 uffd_fault_ns_max=10 uffd_remote_ns_max=20 \
                 uffd_copy_ns_max=30 uffd_wake_ns_max=40",
                "OBMM_NODE_EVIDENCE node=nodeA drained=1 mode=userfaultfd \
                 operations=4 checksum=0123456789abcdef failures=0 timeouts=0 \
                 helper_cpu_ns=10 extra_vcpus=1 sink_copy_bytes=16384 \
                 uffd_handler_cpu_ns=10 uffd_worker_cpu_ns=10 \
                 model_pending_final=0 backend_pending_final=0 scc_pending_final=0 \
                 trace_dropped=0 backend_late=0 backend_duplicate=0 \
                 counter_overflow=0 clock_regressions=0 fail_closed_process_exit=0 \
                 status=pass\n\
                 OBMM_NODE_EVIDENCE node=nodeB drained=1 mode=userfaultfd \
                 operations=4 checksum=0123456789abcdef failures=0 timeouts=0 \
                 helper_cpu_ns=10 extra_vcpus=1 sink_copy_bytes=16384 \
                 uffd_handler_cpu_ns=10 uffd_worker_cpu_ns=10 \
                 model_pending_final=0 backend_pending_final=0 scc_pending_final=0 \
                 trace_dropped=0 backend_late=0 backend_duplicate=0 \
                 counter_overflow=0 clock_regressions=0 fail_closed_process_exit=0 \
                 status=pass\n",
                "OBMM_UFFD_SUMMARY schema=1 case=missing-remote pages=4 faults=4 \
                 remote_reads=4 copy_ok=4 duplicates=0 failures=0 \
                 checksum=0123456789abcdef handler_cpu_ns=10 worker_cpu_ns=10 \
                 fault_ns_max=10 remote_ns_max=20 copy_ns_max=30 wake_ns_max=40 \
                 model_pending=0 backend_pending=0 status=pass\n\
                 OBMM_BACKEND_EVIDENCE node=nodeA duplicate=0 late=0 drained=1\n\
                 OBMM_UFFD_SUMMARY schema=1 case=missing-remote pages=4 faults=4 \
                 remote_reads=4 copy_ok=4 duplicates=0 failures=0 \
                 checksum=0123456789abcdef handler_cpu_ns=10 worker_cpu_ns=10 \
                 fault_ns_max=10 remote_ns_max=20 copy_ns_max=30 wake_ns_max=40 \
                 model_pending=0 backend_pending=0 status=pass\n\
                 OBMM_BACKEND_EVIDENCE node=nodeB duplicate=0 late=0 drained=1",
            ),
        };
        format!(
            "OBMM_RUN_EVIDENCE node_count=2 model_contract_hash={model_hash} \
             qemu_destroyed=1\n\
             OBMM_EVAL_SUMMARY schema=1 mode={mode} operations=4 \
             checksum=0123456789abcdef failures=0 timeouts=0 \
             model_pending_final=0 backend_pending_final=0 scc_pending_final=0 \
             trace_dropped=0 backend_late=0 backend_duplicate=0 counter_overflow=0 \
             clock_regressions=0 fail_closed_process_exit=0 \
             extra_vcpus={extra_vcpus} {phase_fields} status=pass\n\
             {node_detail}{detail}\n"
        )
    }

    #[test]
    fn baseline_args_parse_canonical_dry_run() {
        let args = baseline_args_from([
            "obmm-remote-load-baseline",
            "--scenario=scenarios/mvp_2host_single_domain.yaml",
            "--case=sync-remote-modeled",
            "--access-bytes=4096",
            "--warmup=10",
            "--iterations=20",
            "--seed=7",
            "--output-dir=out/test-obmm",
            "--dry-run",
        ])
        .expect("parse baseline args")
        .expect("baseline args");

        assert_eq!(args.case, ObmmBaselineCase::SyncRemoteModeled);
        assert_eq!(args.access_bytes, 4096);
        assert_eq!(args.warmup, 10);
        assert_eq!(args.iterations, 20);
        assert_eq!(args.seed, 7);
        assert!(args.dry_run);
    }

    #[test]
    fn phase_gate_args_require_explicit_evidence() {
        let args = phase_gate_args_from([
            "obmm-remote-load-phase-gate",
            "--phase=p2a",
            "--scenario=scenarios/mvp_2host_single_domain.yaml",
            "--model-manifest=out/model.json",
            "--evidence=out/run.log",
            "--output-dir=out/gates",
        ])
        .expect("parse gate args")
        .expect("phase gate args");
        assert_eq!(args.phase, ObmmPhaseGate::P2a);
        assert!(args.case_model_manifest_paths.is_empty());
        assert_eq!(args.evidence_paths, vec![PathBuf::from("out/run.log")]);

        let error = phase_gate_args_from([
            "obmm-remote-load-phase-gate",
            "--phase=p2a",
            "--scenario=scenarios/mvp_2host_single_domain.yaml",
            "--model-manifest=out/model.json",
            "--output-dir=out/gates",
        ])
        .expect_err("gate without evidence must fail");
        assert!(error.to_string().contains("at least one --evidence"));

        let error = phase_gate_args_from([
            "obmm-remote-load-phase-gate",
            "--phase=p0",
            "--scenario=scenarios/mvp_2host_single_domain.yaml",
            "--model-manifest=out/model.json",
            "--evidence=out/run.log",
            "--output-dir=out/gates",
        ])
        .expect_err("P0 gate without case model manifests must fail");
        assert!(error.to_string().contains("--case-model-manifest"));
    }

    #[test]
    fn phase_gate_validators_enforce_phase_specific_progress() {
        let hash = "fnv1a64:0123456789abcdef";
        let accepted = BTreeSet::from([hash.to_string()]);
        for phase in [
            ObmmPhaseGate::P0,
            ObmmPhaseGate::P2a,
            ObmmPhaseGate::P2b,
            ObmmPhaseGate::P4,
        ] {
            validate_phase_evidence(phase, &accepted, &phase_evidence(phase, hash))
                .expect("valid phase evidence");
        }
        let invalid = phase_evidence(ObmmPhaseGate::P2b, hash)
            .replace("el0_context_saves=4", "el0_context_saves=0");
        let error = validate_phase_evidence(ObmmPhaseGate::P2b, &accepted, &invalid)
            .expect_err("missing EL0 save evidence must fail P2B gate");
        assert!(error.to_string().contains("EL0 scheduler progress"));

        let invalid =
            phase_evidence(ObmmPhaseGate::P2b, hash).replace("completed=2", "completed=1");
        let error = validate_phase_evidence(ObmmPhaseGate::P2b, &accepted, &invalid)
            .expect_err("incomplete P2B operation set must fail");
        assert!(error.to_string().contains("EL0 scheduler progress"));

        let invalid = phase_evidence(ObmmPhaseGate::P2b, hash)
            .replace("qemu_context_saves=0", "qemu_context_saves=1");
        let error = validate_phase_evidence(ObmmPhaseGate::P2b, &accepted, &invalid)
            .expect_err("QEMU-owned P2B context save must fail");
        assert!(error.to_string().contains("EL0 scheduler progress"));

        let legacy_symmetric = phase_evidence(ObmmPhaseGate::P2a, hash)
            .replace("mode=async-poll", "mode=scheduler-core");
        let error = validate_phase_evidence(ObmmPhaseGate::P2b, &accepted, &legacy_symmetric)
            .expect_err("legacy symmetric aggregate evidence must not pass P2B");
        assert!(error.to_string().contains("P2B_NODE_EVIDENCE"));

        let invalid = phase_evidence(ObmmPhaseGate::P4, hash).replace("faults=4", "faults=3");
        let error = validate_phase_evidence(ObmmPhaseGate::P4, &accepted, &invalid)
            .expect_err("missing P4 fault must fail");
        assert!(error
            .to_string()
            .contains("P4 faults must equal operations"));

        let invalid = phase_evidence(ObmmPhaseGate::P2a, hash).replacen(
            "OBMM_ASYNC_SUMMARY ",
            "OBMM_ASYNC_SUMMARY_MISSING ",
            1,
        );
        let error = validate_phase_evidence(ObmmPhaseGate::P2a, &accepted, &invalid)
            .expect_err("one P2A marker per node is required");
        assert!(error
            .to_string()
            .contains("marker count must equal node_count"));
    }

    #[test]
    fn p0_gate_enforces_case_specific_model_semantics() {
        let mut config = ScenarioConfig::from_yaml_file(scenario_path()).expect("scenario");
        let modeled = build_model_manifest(&config).expect("modeled manifest");

        config.remote_memory_model = RemoteMemoryModelConfig::default();
        let disabled = build_model_manifest(&config).expect("disabled manifest");

        config.remote_memory_model.enabled = true;
        config.remote_memory_model.reorder_window = 8;
        let zero = build_model_manifest(&config).expect("zero-delay manifest");

        let catalog = BTreeMap::from([
            (modeled.manifest_hash.clone(), modeled.clone()),
            (disabled.manifest_hash.clone(), disabled.clone()),
            (zero.manifest_hash.clone(), zero.clone()),
        ]);
        validate_phase_model_catalog(ObmmPhaseGate::P0, &modeled.manifest_hash, &catalog)
            .expect("complete P0 model catalog");
        let accepted = catalog.keys().cloned().collect::<BTreeSet<_>>();
        let cases = [
            ("local-dram", &disabled.manifest_hash),
            ("obmm-local-hit", &disabled.manifest_hash),
            ("sync-remote-zero", &zero.manifest_hash),
            ("sync-remote-modeled", &modeled.manifest_hash),
        ];
        let documents = cases
            .iter()
            .enumerate()
            .map(|(index, (case, hash))| {
                let text = phase_evidence(ObmmPhaseGate::P0, hash)
                    .replace("case=local-dram", &format!("case={case}"));
                validate_phase_evidence(ObmmPhaseGate::P0, &accepted, &text)
                    .expect("individual P0 evidence");
                (PathBuf::from(format!("case-{index}.log")), text)
            })
            .collect::<Vec<_>>();
        let borrowed = documents
            .iter()
            .map(|(path, text)| (path, text.clone()))
            .collect::<Vec<_>>();
        validate_phase_evidence_set(
            ObmmPhaseGate::P0,
            &modeled.manifest_hash,
            &catalog,
            &borrowed,
        )
        .expect("P0 case/model bindings");

        let invalid = phase_evidence(ObmmPhaseGate::P0, &disabled.manifest_hash)
            .replace("case=local-dram", "case=sync-remote-zero");
        let invalid_path = PathBuf::from("invalid.log");
        let invalid_documents = vec![(&invalid_path, invalid)];
        let error = validate_phase_evidence_set(
            ObmmPhaseGate::P0,
            &modeled.manifest_hash,
            &catalog,
            &invalid_documents,
        )
        .expect_err("zero-delay case must reject disabled model");
        assert!(error.to_string().contains("zero-effect"));
    }

    #[test]
    fn phase_gate_cli_packages_validated_evidence() {
        let output_dir = temp_output_dir();
        let input_dir = temp_output_dir();
        fs::create_dir_all(&input_dir).expect("create input dir");
        let config = ScenarioConfig::from_yaml_file(scenario_path()).expect("scenario");
        let model = build_model_manifest(&config).expect("model manifest");
        let model_path = input_dir.join("model.json");
        let evidence_path = input_dir.join("p2a.log");
        write_pretty_json(&model_path, &model).expect("write model");
        fs::write(
            &evidence_path,
            phase_evidence(ObmmPhaseGate::P2a, &model.manifest_hash),
        )
        .expect("write evidence");

        run_phase_gate_cli(&ObmmPhaseGateCliArgs {
            phase: ObmmPhaseGate::P2a,
            scenario_path: scenario_path(),
            model_manifest_path: model_path,
            case_model_manifest_paths: Vec::new(),
            evidence_paths: vec![evidence_path],
            output_dir: output_dir.clone(),
        })
        .expect("package P2A gate");
        let gate = fs::read_to_string(output_dir.join("p2a.json")).expect("gate JSON");
        assert!(gate.contains("\"phase\": \"p2a\""));
        assert!(gate.contains(&model.manifest_hash));
        assert!(output_dir.join("evidence/p2a").is_dir());

        fs::remove_dir_all(output_dir).expect("remove output");
        fs::remove_dir_all(input_dir).expect("remove input");
    }

    #[test]
    fn baseline_args_fail_closed_on_invalid_case_or_size() {
        let case_error =
            baseline_args_from(["obmm-remote-load-baseline", "--case=unknown", "--dry-run"])
                .expect_err("unknown case must fail");
        assert!(case_error.to_string().contains("unsupported --case"));

        let size_error = baseline_args_from([
            "obmm-remote-load-baseline",
            "--case=local-dram",
            "--access-bytes=7",
            "--dry-run",
        ])
        .expect_err("invalid size must fail");
        assert!(size_error.to_string().contains("--access-bytes"));

        let target_error = baseline_args_from(["obmm-remote-load-baseline", "--case=local-dram"])
            .expect_err("formal run without remote target must fail");
        assert!(target_error.to_string().contains("--remote-target"));
    }

    #[test]
    fn model_manifest_hash_is_deterministic() {
        let config = ScenarioConfig::from_yaml_file(scenario_path()).expect("scenario");
        let left = build_model_manifest(&config).expect("left manifest");
        let right = build_model_manifest(&config).expect("right manifest");

        assert_eq!(left, right);
        assert_eq!(left.schema, 1);
        assert!(left.manifest_hash.starts_with("fnv1a64:"));
    }

    #[test]
    fn scheduler_core_spec_is_canonical_and_scenario_driven() {
        let config = ScenarioConfig::from_yaml_file(scenario_path()).expect("scenario");

        assert_eq!(
            scheduler_core_model_spec(&config),
            "v2|enabled=1|contexts=64|pending=64|events=128|clock_mhz=2000"
        );
    }

    #[test]
    fn baseline_dry_run_writes_reproducible_manifests() {
        let output_dir = temp_output_dir();
        let args = ObmmBaselineCliArgs {
            scenario_path: scenario_path(),
            case: ObmmBaselineCase::SyncRemoteModeled,
            access_bytes: 8,
            warmup: 2,
            iterations: 4,
            seed: 3,
            output_dir: output_dir.clone(),
            remote_target: None,
            remote_repo: None,
            dry_run: true,
        };

        run_baseline_cli(&args).expect("dry run");
        let model_bytes = fs::read(output_dir.join("remote_memory_model_manifest_v1.json"))
            .expect("model manifest");
        let model: RemoteMemoryManifestV1 =
            serde_json::from_slice(&model_bytes).expect("decode model manifest");
        let run_bytes = fs::read(output_dir.join("run-manifest.json")).expect("run manifest");
        let run_text = String::from_utf8(run_bytes).expect("UTF-8 run manifest");

        assert_eq!(model.scenario_name, "mvp_2host_single_domain");
        assert!(run_text.contains("--remote-memory-model-manifest"));
        assert!(run_text.contains(&model.manifest_hash));
        fs::remove_dir_all(output_dir).expect("remove temp output");
    }

    #[test]
    fn conformance_args_validate_sink_size_contract() {
        let args = conformance_args_from([
            "obmm-remote-backend-conformance",
            "--sink=p2a",
            "--case=inflight64",
            "--access-bytes=65536",
            "--seed=7",
            "--dry-run",
        ])
        .expect("parse P2A conformance")
        .expect("conformance args");
        assert_eq!(args.sink, ObmmConformanceSink::P2a);
        assert_eq!(args.case, ObmmConformanceCase::Inflight64);
        assert_eq!(args.access_bytes, 65536);

        let error = conformance_args_from([
            "obmm-remote-backend-conformance",
            "--sink=p2b",
            "--case=inline",
            "--access-bytes=64",
            "--dry-run",
        ])
        .expect_err("P2B vector read must fail");
        assert!(error.to_string().contains("P2B sink"));

        let suite = conformance_args_from([
            "obmm-remote-backend-conformance-suite",
            "--output-dir=out/p1-suite",
            "--dry-run",
        ])
        .expect("parse suite")
        .expect("suite args");
        assert!(suite.suite);
    }

    #[test]
    fn conformance_dry_run_writes_manifest_and_command() {
        let output_dir = temp_output_dir();
        let args = ObmmConformanceCliArgs {
            scenario_path: scenario_path(),
            sink: ObmmConformanceSink::Test,
            case: ObmmConformanceCase::Reorder,
            access_bytes: 4096,
            seed: 9,
            output_dir: output_dir.clone(),
            dry_run: true,
            suite: false,
        };

        run_conformance_cli(&args).expect("P1 conformance dry run");
        let run_text =
            fs::read_to_string(output_dir.join("run-manifest.json")).expect("run manifest");
        assert!(run_text.contains("obmm-remote-backend-conformance"));
        assert!(run_text.contains("--conformance"));
        assert!(run_text.contains("test-ub-obmm-remote"));
        assert!(run_text.contains("remote_memory_model_manifest_v1.json"));
        fs::remove_dir_all(output_dir).expect("remove temp output");
    }

    #[test]
    fn p2b_conformance_uses_the_same_provider_neutral_binary() {
        let output_dir = temp_output_dir();
        let args = ObmmConformanceCliArgs {
            scenario_path: scenario_path(),
            sink: ObmmConformanceSink::P2b,
            case: ObmmConformanceCase::Inline,
            access_bytes: 8,
            seed: 11,
            output_dir: output_dir.clone(),
            dry_run: true,
            suite: false,
        };

        run_conformance_cli(&args).expect("P2B conformance dry run");
        let run_text =
            fs::read_to_string(output_dir.join("run-manifest.json")).expect("run manifest");
        assert!(run_text.contains("test-ub-obmm-remote"));
        assert!(run_text.contains("\"p2b\""));
        fs::remove_dir_all(output_dir).expect("remove temp output");
    }
}
