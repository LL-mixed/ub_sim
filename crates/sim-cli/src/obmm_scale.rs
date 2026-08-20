use anyhow::Context;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use std::ffi::{OsStr, OsString};
use std::fs;
use std::path::{Path, PathBuf};

const SCALE_REPORT_SCHEMA: u32 = 1;
const DEFAULT_CASE_IDS: &str = "S1-submit-await-demand,S3-async-load-demand";

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ObmmScaleCliArgs {
    manifest_path: PathBuf,
    raw_dir: PathBuf,
    output_dir: PathBuf,
    case_ids: Vec<String>,
}

#[derive(Debug, Deserialize)]
struct RunManifest {
    schema: u32,
    scenario_file_sha256: String,
    topology_hosts: u32,
    cases: Vec<ManifestCase>,
}

#[derive(Clone, Debug, Deserialize)]
struct ManifestCase {
    run_id: String,
    case_id: String,
    mode: String,
    seed: u64,
    operations: u64,
    outcome: String,
    model_file_sha256: String,
    model_manifest_hash: String,
}

#[derive(Clone, Debug, Serialize)]
struct ScaleRunMetrics {
    run_id: String,
    case_id: String,
    mode: String,
    seed: u64,
    node_count: u32,
    checksum: String,
    cluster_operations: u64,
    cluster_makespan_ns: u64,
    cluster_throughput_ops_per_second: f64,
    application_cpu_ns: u64,
    el0_scheduler_ns: u64,
    qemu_sha256: String,
    kernel_sha256: String,
    initramfs_sha256: String,
}

#[derive(Clone, Debug, Serialize)]
struct RunValidation {
    run_id: String,
    case_id: String,
    seed: u64,
    status: String,
    reasons: Vec<String>,
    metrics: Option<ScaleRunMetrics>,
}

#[derive(Clone, Debug, Serialize)]
struct ScaleValidation {
    schema: u32,
    status: String,
    node_count: u32,
    expected_runs: usize,
    valid_runs: usize,
    invalid_reasons: Vec<String>,
    runs: Vec<RunValidation>,
}

#[derive(Clone, Debug, Serialize)]
struct ScaleGroupSummary {
    case_id: String,
    mode: String,
    valid_seeds: usize,
    node_count: u32,
    operations_per_node: u64,
    cluster_throughput_ops_per_second_median: f64,
    cluster_throughput_ops_per_second_min: f64,
    cluster_throughput_ops_per_second_max: f64,
    cluster_makespan_ns_median: u64,
    application_cpu_ns_median: u64,
    el0_scheduler_ns_median: u64,
}

#[derive(Clone, Debug, Serialize)]
struct ScaleSummary {
    schema: u32,
    status: String,
    node_count: u32,
    scenario_file_sha256: String,
    qemu_sha256: Option<String>,
    kernel_sha256: Option<String>,
    initramfs_sha256: Option<String>,
    groups: Vec<ScaleGroupSummary>,
    async_load_vs_submit_await_throughput_percent: Option<f64>,
    async_load_vs_submit_await_makespan_reduction_percent: Option<f64>,
}

pub(crate) fn args() -> anyhow::Result<Option<ObmmScaleCliArgs>> {
    args_from(std::env::args_os().skip(1))
}

fn args_from<I, S>(args: I) -> anyhow::Result<Option<ObmmScaleCliArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    if args.next().as_deref() != Some(OsStr::new("obmm-remote-load-scale-report")) {
        return Ok(None);
    }

    let mut manifest_path = None;
    let mut raw_dir = None;
    let mut output_dir = None;
    let mut case_ids = DEFAULT_CASE_IDS.to_string();
    let mut pending = args.peekable();
    while let Some(argument) = pending.next() {
        let text = argument.to_string_lossy();
        let (name, value) = if let Some((name, value)) = text.split_once('=') {
            (name.to_string(), value.to_string())
        } else if text.starts_with("--") {
            let value = pending
                .next()
                .ok_or_else(|| anyhow::anyhow!("{text} requires a value"))?;
            (text.into_owned(), value.to_string_lossy().into_owned())
        } else {
            anyhow::bail!("unexpected obmm-remote-load-scale-report argument: {text}");
        };
        match name.as_str() {
            "--manifest" => manifest_path = Some(PathBuf::from(value)),
            "--raw-dir" => raw_dir = Some(PathBuf::from(value)),
            "--output-dir" => output_dir = Some(PathBuf::from(value)),
            "--case-ids" => case_ids = value,
            _ => anyhow::bail!("unknown obmm-remote-load-scale-report option: {name}"),
        }
    }

    let case_ids = parse_case_ids(&case_ids)?;
    Ok(Some(ObmmScaleCliArgs {
        manifest_path: manifest_path.ok_or_else(|| anyhow::anyhow!("--manifest is required"))?,
        raw_dir: raw_dir.ok_or_else(|| anyhow::anyhow!("--raw-dir is required"))?,
        output_dir: output_dir.ok_or_else(|| anyhow::anyhow!("--output-dir is required"))?,
        case_ids,
    }))
}

fn parse_case_ids(value: &str) -> anyhow::Result<Vec<String>> {
    let case_ids = value
        .split(',')
        .map(str::trim)
        .filter(|item| !item.is_empty())
        .map(str::to_owned)
        .collect::<BTreeSet<_>>();
    if case_ids.is_empty() {
        anyhow::bail!("--case-ids must contain at least one case ID");
    }
    Ok(case_ids.into_iter().collect())
}

pub(crate) fn run(args: &ObmmScaleCliArgs) -> anyhow::Result<()> {
    let manifest: RunManifest = serde_json::from_slice(
        &fs::read(&args.manifest_path)
            .with_context(|| format!("read {}", args.manifest_path.display()))?,
    )
    .with_context(|| format!("decode {}", args.manifest_path.display()))?;
    if manifest.schema != 1 {
        anyhow::bail!("unsupported run manifest schema {}", manifest.schema);
    }
    if manifest.topology_hosts == 0 {
        anyhow::bail!("run manifest topology_hosts must be positive");
    }

    let selected = manifest
        .cases
        .iter()
        .filter(|case| args.case_ids.contains(&case.case_id))
        .collect::<Vec<_>>();
    for case_id in &args.case_ids {
        if !selected.iter().any(|case| &case.case_id == case_id) {
            anyhow::bail!("case ID {case_id} is absent from the run manifest");
        }
    }

    let mut validations = Vec::with_capacity(selected.len());
    for case in selected {
        let log_path = args.raw_dir.join(format!("{}.log", case.run_id));
        let jsonl_path = args.raw_dir.join(format!("{}.jsonl", case.run_id));
        let path = if log_path.exists() {
            log_path
        } else {
            jsonl_path
        };
        let result = fs::read_to_string(&path)
            .with_context(|| format!("read {}", path.display()))
            .and_then(|text| parse_scale_log(case, &manifest, &text));
        match result {
            Ok(metrics) => validations.push(RunValidation {
                run_id: case.run_id.clone(),
                case_id: case.case_id.clone(),
                seed: case.seed,
                status: "pass".into(),
                reasons: Vec::new(),
                metrics: Some(metrics),
            }),
            Err(error) => validations.push(RunValidation {
                run_id: case.run_id.clone(),
                case_id: case.case_id.clone(),
                seed: case.seed,
                status: "invalid".into(),
                reasons: vec![format!("{error:#}")],
                metrics: None,
            }),
        }
    }

    let mut invalid_reasons = Vec::new();
    validate_cross_case_identity(&validations, &mut invalid_reasons);
    validate_artifact_identity(&validations, &mut invalid_reasons);
    let valid_runs = validations
        .iter()
        .filter(|run| run.status == "pass")
        .count();
    if valid_runs != validations.len() {
        invalid_reasons.push(format!(
            "{} of {} selected runs are invalid",
            validations.len() - valid_runs,
            validations.len()
        ));
    }
    let status = if invalid_reasons.is_empty() {
        "pass"
    } else {
        "invalid"
    };
    let groups = aggregate_groups(&validations, manifest.topology_hosts)?;
    let submit_await = groups
        .iter()
        .find(|group| group.case_id == "S1-submit-await-demand");
    let async_load = groups
        .iter()
        .find(|group| group.case_id == "S3-async-load-demand");
    let throughput_comparison = submit_await
        .zip(async_load)
        .map(|(submit_await, async_load)| {
            (async_load.cluster_throughput_ops_per_second_median
                / submit_await.cluster_throughput_ops_per_second_median
                - 1.0)
                * 100.0
        });
    let makespan_comparison = submit_await
        .zip(async_load)
        .map(|(submit_await, async_load)| {
            (1.0 - async_load.cluster_makespan_ns_median as f64
                / submit_await.cluster_makespan_ns_median as f64)
                * 100.0
        });
    let first_metrics = validations.iter().find_map(|run| run.metrics.as_ref());
    let summary = ScaleSummary {
        schema: SCALE_REPORT_SCHEMA,
        status: status.into(),
        node_count: manifest.topology_hosts,
        scenario_file_sha256: manifest.scenario_file_sha256.clone(),
        qemu_sha256: first_metrics.map(|metrics| metrics.qemu_sha256.clone()),
        kernel_sha256: first_metrics.map(|metrics| metrics.kernel_sha256.clone()),
        initramfs_sha256: first_metrics.map(|metrics| metrics.initramfs_sha256.clone()),
        groups,
        async_load_vs_submit_await_throughput_percent: throughput_comparison,
        async_load_vs_submit_await_makespan_reduction_percent: makespan_comparison,
    };
    let validation = ScaleValidation {
        schema: SCALE_REPORT_SCHEMA,
        status: status.into(),
        node_count: manifest.topology_hosts,
        expected_runs: validations.len(),
        valid_runs,
        invalid_reasons,
        runs: validations,
    };

    fs::create_dir_all(&args.output_dir)?;
    write_json(&args.output_dir.join("validation.json"), &validation)?;
    write_json(&args.output_dir.join("summary.json"), &summary)?;
    fs::write(
        args.output_dir.join("report.md"),
        markdown_report(&summary, &validation),
    )?;
    println!(
        "OBMM_SCALE_REPORT schema=1 nodes={} valid_runs={} expected_runs={} status={}",
        manifest.topology_hosts, valid_runs, validation.expected_runs, status
    );
    if status != "pass" {
        anyhow::bail!("scale evidence is invalid; inspect validation.json");
    }
    Ok(())
}

fn parse_scale_log(
    case: &ManifestCase,
    manifest: &RunManifest,
    text: &str,
) -> anyhow::Result<ScaleRunMetrics> {
    if case.outcome != "success" {
        anyhow::bail!("scale report only accepts success cases");
    }
    let run_lines = matching_lines(text, "OBMM_RUN_EVIDENCE ");
    if run_lines.len() != 1 {
        anyhow::bail!("expected one OBMM_RUN_EVIDENCE, found {}", run_lines.len());
    }
    let run = fields(&run_lines[0]);
    require_field(&run, "node_count", &manifest.topology_hosts.to_string())?;
    require_field(&run, "scenario_sha256", &manifest.scenario_file_sha256)?;
    require_field(&run, "model_file_sha256", &case.model_file_sha256)?;
    require_field(&run, "model_contract_hash", &case.model_manifest_hash)?;
    require_field(&run, "qemu_destroyed", "1")?;
    for name in ["qemu_sha256", "kernel_sha256", "initramfs_sha256"] {
        if !run.get(name).is_some_and(|value| is_sha256(value)) {
            anyhow::bail!("{name} is not a SHA-256 digest");
        }
    }

    let node_lines = matching_lines(text, "OBMM_NODE_EVIDENCE ");
    if node_lines.len() != manifest.topology_hosts as usize {
        anyhow::bail!(
            "expected {} OBMM_NODE_EVIDENCE records, found {}",
            manifest.topology_hosts,
            node_lines.len()
        );
    }
    let mut nodes = BTreeSet::new();
    let mut checksums = BTreeSet::new();
    let mut cluster_operations = 0_u64;
    let mut cluster_makespan_ns = 0_u64;
    let mut application_cpu_ns = 0_u64;
    let mut el0_scheduler_ns = 0_u64;
    let mut node_a = None;
    for line in node_lines {
        let summary = fields(&line);
        let node = required(&summary, "node")?;
        if !nodes.insert(node.to_string()) {
            anyhow::bail!("duplicate node evidence for {node}");
        }
        require_field(&summary, "drained", "1")?;
        require_field(&summary, "summary", "schema=1")?;
        require_field(&summary, "case", &case.case_id)?;
        require_field(&summary, "mode", &case.mode)?;
        require_field(&summary, "seed", &case.seed.to_string())?;
        require_field(&summary, "operations", &case.operations.to_string())?;
        for (name, value) in [
            ("failures", "0"),
            ("timeouts", "0"),
            ("status", "pass"),
            ("model_pending_final", "0"),
            ("backend_pending_final", "0"),
            ("async_load_pending_final", "0"),
            ("counter_overflow", "0"),
            ("clock_regressions", "0"),
            ("fail_closed_process_exit", "0"),
        ] {
            require_field(&summary, name, value)?;
        }
        validate_async_load(case, &summary)?;
        checksums.insert(required(&summary, "checksum")?.to_string());
        cluster_operations = cluster_operations
            .checked_add(parse_u64(&summary, "operations")?)
            .context("cluster operation count overflow")?;
        cluster_makespan_ns = cluster_makespan_ns.max(parse_u64(&summary, "makespan_ns")?);
        application_cpu_ns = application_cpu_ns
            .checked_add(parse_u64(&summary, "application_cpu_ns")?)
            .context("application CPU counter overflow")?;
        el0_scheduler_ns = el0_scheduler_ns
            .checked_add(parse_u64(&summary, "el0_scheduler_ns")?)
            .context("EL0 scheduler counter overflow")?;
        if node == "nodeA" {
            node_a = Some(summary);
        }
    }
    if checksums.len() != 1 {
        anyhow::bail!("node checksums differ within the run");
    }
    if cluster_makespan_ns == 0 {
        anyhow::bail!("cluster makespan is zero");
    }

    let canonical_lines = matching_lines(text, "OBMM_EVAL_SUMMARY ");
    if canonical_lines.len() != 1 {
        anyhow::bail!(
            "expected one OBMM_EVAL_SUMMARY, found {}",
            canonical_lines.len()
        );
    }
    let canonical = fields(&canonical_lines[0]);
    let node_a = node_a.context("nodeA evidence is absent")?;
    for name in [
        "case",
        "mode",
        "seed",
        "operations",
        "checksum",
        "makespan_ns",
    ] {
        if canonical.get(name) != node_a.get(name) {
            anyhow::bail!("canonical summary differs from nodeA field {name}");
        }
    }

    Ok(ScaleRunMetrics {
        run_id: case.run_id.clone(),
        case_id: case.case_id.clone(),
        mode: case.mode.clone(),
        seed: case.seed,
        node_count: manifest.topology_hosts,
        checksum: checksums.into_iter().next().expect("one checksum"),
        cluster_operations,
        cluster_makespan_ns,
        cluster_throughput_ops_per_second: cluster_operations as f64 * 1_000_000_000.0
            / cluster_makespan_ns as f64,
        application_cpu_ns,
        el0_scheduler_ns,
        qemu_sha256: required(&run, "qemu_sha256")?.to_string(),
        kernel_sha256: required(&run, "kernel_sha256")?.to_string(),
        initramfs_sha256: required(&run, "initramfs_sha256")?.to_string(),
    })
}

fn validate_async_load(
    case: &ManifestCase,
    summary: &BTreeMap<String, String>,
) -> anyhow::Result<()> {
    if case.mode != "async-load" {
        return Ok(());
    }
    for name in [
        "qemu_context_saves",
        "qemu_context_restores",
        "qemu_context_switches",
        "qemu_context_bytes",
        "async_load_save_cycles",
        "async_load_schedule_cycles",
        "async_load_restore_cycles",
        "async_load_commit_cycles",
        "el0_upcalls_fault",
    ] {
        require_field(summary, name, "0")?;
    }
    for name in [
        "el0_context_saves",
        "el0_context_restores",
        "el0_context_switches",
        "el0_context_bytes",
        "el0_scheduler_ns",
    ] {
        if parse_u64(summary, name)? == 0 {
            anyhow::bail!("ASYNC_LOAD {name} must be positive");
        }
    }
    require_field(summary, "el0_upcalls_pending", &case.operations.to_string())?;
    require_field(
        summary,
        "el0_upcalls_complete",
        &case.operations.to_string(),
    )?;
    if required(summary, "direct_el0_upcalls")? != required(summary, "el0_context_saves")? {
        anyhow::bail!("ASYNC_LOAD direct_el0_upcalls differs from el0_context_saves");
    }
    Ok(())
}

fn validate_cross_case_identity(validations: &[RunValidation], reasons: &mut Vec<String>) {
    let mut by_seed = BTreeMap::<u64, BTreeSet<&str>>::new();
    for metrics in validations.iter().filter_map(|run| run.metrics.as_ref()) {
        by_seed
            .entry(metrics.seed)
            .or_default()
            .insert(&metrics.checksum);
    }
    for (seed, checksums) in by_seed {
        if checksums.len() != 1 {
            reasons.push(format!("seed {seed} has cross-case checksum mismatch"));
        }
    }
}

fn validate_artifact_identity(validations: &[RunValidation], reasons: &mut Vec<String>) {
    for (name, values) in [
        (
            "QEMU",
            validations
                .iter()
                .filter_map(|run| run.metrics.as_ref().map(|metrics| &metrics.qemu_sha256))
                .collect::<BTreeSet<_>>(),
        ),
        (
            "kernel",
            validations
                .iter()
                .filter_map(|run| run.metrics.as_ref().map(|metrics| &metrics.kernel_sha256))
                .collect::<BTreeSet<_>>(),
        ),
        (
            "initramfs",
            validations
                .iter()
                .filter_map(|run| {
                    run.metrics
                        .as_ref()
                        .map(|metrics| &metrics.initramfs_sha256)
                })
                .collect::<BTreeSet<_>>(),
        ),
    ] {
        if values.len() > 1 {
            reasons.push(format!("{name} artifact hash differs across runs"));
        }
    }
}

fn aggregate_groups(
    validations: &[RunValidation],
    node_count: u32,
) -> anyhow::Result<Vec<ScaleGroupSummary>> {
    let mut grouped = BTreeMap::<(&str, &str), Vec<&ScaleRunMetrics>>::new();
    for metrics in validations.iter().filter_map(|run| run.metrics.as_ref()) {
        grouped
            .entry((&metrics.case_id, &metrics.mode))
            .or_default()
            .push(metrics);
    }
    let mut summaries = Vec::new();
    for ((case_id, mode), metrics) in grouped {
        let mut throughputs = metrics
            .iter()
            .map(|item| (item.cluster_throughput_ops_per_second * 1_000.0).round() as u64)
            .collect::<Vec<_>>();
        let mut makespans = metrics
            .iter()
            .map(|item| item.cluster_makespan_ns)
            .collect::<Vec<_>>();
        let mut application_cpu = metrics
            .iter()
            .map(|item| item.application_cpu_ns)
            .collect::<Vec<_>>();
        let mut scheduler_cpu = metrics
            .iter()
            .map(|item| item.el0_scheduler_ns)
            .collect::<Vec<_>>();
        let throughput_min = *throughputs.iter().min().context("empty scale group")?;
        let throughput_max = *throughputs.iter().max().context("empty scale group")?;
        summaries.push(ScaleGroupSummary {
            case_id: case_id.into(),
            mode: mode.into(),
            valid_seeds: metrics.len(),
            node_count,
            operations_per_node: metrics[0].cluster_operations / u64::from(node_count),
            cluster_throughput_ops_per_second_median: median(&mut throughputs)? as f64 / 1_000.0,
            cluster_throughput_ops_per_second_min: throughput_min as f64 / 1_000.0,
            cluster_throughput_ops_per_second_max: throughput_max as f64 / 1_000.0,
            cluster_makespan_ns_median: median(&mut makespans)?,
            application_cpu_ns_median: median(&mut application_cpu)?,
            el0_scheduler_ns_median: median(&mut scheduler_cpu)?,
        });
    }
    Ok(summaries)
}

fn markdown_report(summary: &ScaleSummary, validation: &ScaleValidation) -> String {
    let mut report = format!(
        "# OBMM remote-load {}-node scale report\n\n\
         - status: `{}`\n\
         - valid runs: `{}/{}`\n\
         - scenario SHA-256: `{}`\n\n\
         | case | mode | seeds | cluster throughput median (ops/s) | min | max | cluster makespan median (ns) | application CPU median (ns) | EL0 scheduler median (ns) |\n\
         |---|---|---:|---:|---:|---:|---:|---:|---:|\n",
        summary.node_count,
        summary.status,
        validation.valid_runs,
        validation.expected_runs,
        summary.scenario_file_sha256
    );
    for group in &summary.groups {
        report.push_str(&format!(
            "| {} | {} | {} | {:.3} | {:.3} | {:.3} | {} | {} | {} |\n",
            group.case_id,
            group.mode,
            group.valid_seeds,
            group.cluster_throughput_ops_per_second_median,
            group.cluster_throughput_ops_per_second_min,
            group.cluster_throughput_ops_per_second_max,
            group.cluster_makespan_ns_median,
            group.application_cpu_ns_median,
            group.el0_scheduler_ns_median
        ));
    }
    if let (Some(throughput), Some(makespan)) = (
        summary.async_load_vs_submit_await_throughput_percent,
        summary.async_load_vs_submit_await_makespan_reduction_percent,
    ) {
        report.push_str(&format!(
            "\nASYNC_LOAD vs submit/await demand: cluster throughput `{throughput:.2}%`; \
             cluster makespan reduction `{makespan:.2}%`.\n"
        ));
    }
    report
}

fn matching_lines(text: &str, prefix: &str) -> Vec<String> {
    text.lines()
        .filter_map(|raw| {
            if raw.starts_with(prefix) {
                return Some(raw.to_string());
            }
            serde_json::from_str::<serde_json::Value>(raw)
                .ok()?
                .get("line")?
                .as_str()
                .filter(|line| line.starts_with(prefix))
                .map(str::to_owned)
        })
        .collect()
}

fn fields(line: &str) -> BTreeMap<String, String> {
    line.split_whitespace()
        .skip(1)
        .filter_map(|field| field.split_once('='))
        .map(|(name, value)| (name.to_string(), value.to_string()))
        .collect()
}

fn required<'a>(fields: &'a BTreeMap<String, String>, name: &str) -> anyhow::Result<&'a str> {
    fields
        .get(name)
        .map(String::as_str)
        .ok_or_else(|| anyhow::anyhow!("evidence field {name} is absent"))
}

fn require_field(
    fields: &BTreeMap<String, String>,
    name: &str,
    expected: &str,
) -> anyhow::Result<()> {
    let actual = required(fields, name)?;
    if actual != expected {
        anyhow::bail!("evidence field {name} expected {expected}, found {actual}");
    }
    Ok(())
}

fn parse_u64(fields: &BTreeMap<String, String>, name: &str) -> anyhow::Result<u64> {
    required(fields, name)?
        .parse()
        .with_context(|| format!("parse evidence field {name}"))
}

fn is_sha256(value: &str) -> bool {
    value.len() == 64
        && value != "0000000000000000000000000000000000000000000000000000000000000000"
        && value
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit() && !byte.is_ascii_uppercase())
}

fn median(values: &mut [u64]) -> anyhow::Result<u64> {
    if values.is_empty() {
        anyhow::bail!("cannot compute a median from an empty sample");
    }
    values.sort_unstable();
    Ok(values[(values.len() - 1) / 2])
}

fn write_json<T: Serialize>(path: &Path, value: &T) -> anyhow::Result<()> {
    let mut bytes = serde_json::to_vec_pretty(value)?;
    bytes.push(b'\n');
    fs::write(path, bytes).with_context(|| format!("write {}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_scale_report_cli() {
        let args = args_from([
            "obmm-remote-load-scale-report",
            "--manifest=run-manifest.json",
            "--raw-dir=raw",
            "--output-dir=summary",
            "--case-ids=S3-async-load-demand,S1-submit-await-demand",
        ])
        .expect("arguments")
        .expect("scale arguments");
        assert_eq!(
            args.case_ids,
            vec![
                "S1-submit-await-demand".to_string(),
                "S3-async-load-demand".to_string()
            ]
        );
    }

    #[test]
    fn async_load_scale_log_requires_guest_el0_and_forbids_qemu_context() {
        let case = ManifestCase {
            run_id: "run-1".into(),
            case_id: "S3-async-load-demand".into(),
            mode: "async-load".into(),
            seed: 1,
            operations: 10,
            outcome: "success".into(),
            model_file_sha256: "1111111111111111111111111111111111111111111111111111111111111111"
                .into(),
            model_manifest_hash: "fnv1a64:1234567890abcdef".into(),
        };
        let manifest = RunManifest {
            schema: 1,
            scenario_file_sha256:
                "2222222222222222222222222222222222222222222222222222222222222222".into(),
            topology_hosts: 2,
            cases: vec![case.clone()],
        };
        let run = "OBMM_RUN_EVIDENCE node_count=2 scenario_sha256=2222222222222222222222222222222222222222222222222222222222222222 model_file_sha256=1111111111111111111111111111111111111111111111111111111111111111 model_contract_hash=fnv1a64:1234567890abcdef qemu_sha256=3333333333333333333333333333333333333333333333333333333333333333 kernel_sha256=4444444444444444444444444444444444444444444444444444444444444444 initramfs_sha256=5555555555555555555555555555555555555555555555555555555555555555 qemu_destroyed=1\n";
        let node = |name: &str, qemu_saves: u64| {
            format!(
            "OBMM_NODE_EVIDENCE node={name} drained=1 summary=schema=1 mode=async-load case=S3-async-load-demand seed=1 operations=10 checksum=abcd failures=0 timeouts=0 makespan_ns=100 application_cpu_ns=80 el0_scheduler_ns=20 model_pending_final=0 backend_pending_final=0 async_load_pending_final=0 counter_overflow=0 clock_regressions=0 fail_closed_process_exit=0 status=pass qemu_context_saves={qemu_saves} qemu_context_restores=0 qemu_context_switches=0 qemu_context_bytes=0 async_load_save_cycles=0 async_load_schedule_cycles=0 async_load_restore_cycles=0 async_load_commit_cycles=0 el0_upcalls_fault=0 el0_upcalls_pending=10 el0_upcalls_complete=10 el0_context_saves=10 el0_context_restores=11 el0_context_switches=9 el0_context_bytes=100 el0_no_ready_waits=0 direct_el0_upcalls=10\n"
        )
        };
        let canonical = "OBMM_EVAL_SUMMARY schema=1 mode=async-load case=S3-async-load-demand seed=1 operations=10 checksum=abcd makespan_ns=100\n";
        let valid = format!("{run}{}{canonical}{}", node("nodeA", 0), node("nodeB", 0));
        assert!(parse_scale_log(&case, &manifest, &valid).is_ok());

        let invalid = format!("{run}{}{canonical}{}", node("nodeA", 1), node("nodeB", 0));
        assert!(parse_scale_log(&case, &manifest, &invalid)
            .expect_err("QEMU context state must fail")
            .to_string()
            .contains("qemu_context_saves"));
    }

    #[test]
    fn extracts_evidence_from_direct_logs_and_evaluator_jsonl() {
        let direct = "OBMM_RUN_EVIDENCE node_count=2\n";
        assert_eq!(
            matching_lines(direct, "OBMM_RUN_EVIDENCE "),
            vec![direct.trim()]
        );

        let jsonl = r#"{"schema":1,"kind":"output","stream":"stdout","line":"OBMM_RUN_EVIDENCE node_count=2"}"#;
        assert_eq!(
            matching_lines(jsonl, "OBMM_RUN_EVIDENCE "),
            vec!["OBMM_RUN_EVIDENCE node_count=2"]
        );
    }
}
