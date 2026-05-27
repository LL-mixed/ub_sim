use crate::engram_context::{
    ENGRAM_CONTEXT_HIDDEN_SIZE, ENGRAM_CONTEXT_INDICES_PER_BATCH, ENGRAM_CONTEXT_SUPPORTED_BATCHES,
};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::process::Command;

pub const ENGRAM_SIMT_VENDOR_DIR: &str = "vendor/pto-isa/kernels/manual/a5/engram_simt";
pub const ENGRAM_SIMT_BINARY_NAME: &str = "engram-simt";
pub const ENGRAM_SIMT_KERNEL_LIBRARY_NAME: &str = "libengram-simt_kernel.so";
pub const ENGRAM_SIMT_DEFAULT_SOC_VERSION: &str = "Ascend910_9599";

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct EngramSimtLaunchSpec {
    pub mode: &'static str,
    pub emb_dim: usize,
    pub batch: usize,
    pub table_rows: usize,
    pub indices_per_batch: usize,
    pub symbol: String,
    pub case_name: String,
    pub binary_path: PathBuf,
    pub kernel_library_path: PathBuf,
    pub run_mode: String,
    pub soc_version: String,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct EngramSimtLaunchReport {
    pub mode: &'static str,
    pub case_name: String,
    pub binary_path: PathBuf,
    pub kernel_library_path: PathBuf,
    pub working_dir: PathBuf,
    pub npu_id: u32,
    pub status_code: Option<i32>,
    pub passed_cases: usize,
    pub failed_cases: usize,
    pub total_cases: usize,
    pub selected_case_passed: bool,
    pub stdout: String,
    pub stderr: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct EngramSimtArtifactConfig {
    pub artifact_dir: PathBuf,
    pub emb_dim: usize,
    pub batch: usize,
    pub table_rows: usize,
    pub run_mode: String,
    pub soc_version: String,
}

impl EngramSimtArtifactConfig {
    pub fn new(
        artifact_dir: impl Into<PathBuf>,
        batch: usize,
        table_rows: usize,
    ) -> EngramSimtArtifactConfig {
        EngramSimtArtifactConfig {
            artifact_dir: artifact_dir.into(),
            emb_dim: ENGRAM_CONTEXT_HIDDEN_SIZE,
            batch,
            table_rows,
            run_mode: "sim".to_string(),
            soc_version: ENGRAM_SIMT_DEFAULT_SOC_VERSION.to_string(),
        }
    }
}

pub fn build_engram_simt_case_name(emb_dim: usize, batch: usize, table_rows: usize) -> String {
    format!(
        "ENGRAMSIMTTest.fused_E{emb_dim}_B{batch}_{}",
        table_rows_tag(table_rows)
    )
}

pub fn build_engram_simt_symbol(emb_dim: usize, batch: usize) -> String {
    format!("runEngram_fused_E{emb_dim}_B{batch}")
}

pub fn discover_engram_simt_artifact(
    config: &EngramSimtArtifactConfig,
) -> Result<EngramSimtLaunchSpec, String> {
    validate_engram_simt_shape(config.emb_dim, config.batch, config.table_rows)?;
    validate_engram_simt_runtime(&config.run_mode, &config.soc_version)?;

    let binary_path = config.artifact_dir.join(ENGRAM_SIMT_BINARY_NAME);
    let kernel_library_path = config.artifact_dir.join(ENGRAM_SIMT_KERNEL_LIBRARY_NAME);
    if !config.artifact_dir.is_dir() {
        return Err(format!(
            "engram_simt_artifact_dir_missing:path={}:hint=build with {} run.sh -r sim -v {} -p",
            config.artifact_dir.display(),
            ENGRAM_SIMT_VENDOR_DIR,
            ENGRAM_SIMT_DEFAULT_SOC_VERSION
        ));
    }
    if !binary_path.is_file() {
        return Err(format!(
            "engram_simt_binary_missing:path={}:hint=expected {} in SIM_ENGRAM_SIMT_ARTIFACT_DIR",
            binary_path.display(),
            ENGRAM_SIMT_BINARY_NAME
        ));
    }
    if !kernel_library_path.is_file() {
        return Err(format!(
            "engram_simt_kernel_library_missing:path={}:hint=expected {} in SIM_ENGRAM_SIMT_ARTIFACT_DIR",
            kernel_library_path.display(),
            ENGRAM_SIMT_KERNEL_LIBRARY_NAME
        ));
    }

    Ok(EngramSimtLaunchSpec {
        mode: "fused-simt",
        emb_dim: config.emb_dim,
        batch: config.batch,
        table_rows: config.table_rows,
        indices_per_batch: ENGRAM_CONTEXT_INDICES_PER_BATCH,
        symbol: build_engram_simt_symbol(config.emb_dim, config.batch),
        case_name: build_engram_simt_case_name(config.emb_dim, config.batch, config.table_rows),
        binary_path,
        kernel_library_path,
        run_mode: config.run_mode.clone(),
        soc_version: config.soc_version.clone(),
    })
}

pub fn artifact_config_from_env(
    batch: usize,
    table_rows: usize,
) -> Result<EngramSimtArtifactConfig, String> {
    let artifact_dir = std::env::var("SIM_ENGRAM_SIMT_ARTIFACT_DIR").map_err(|_| {
        "engram_simt_artifact_dir_env_missing:SIM_ENGRAM_SIMT_ARTIFACT_DIR".to_string()
    })?;
    let run_mode = std::env::var("SIM_ENGRAM_SIMT_RUN_MODE").unwrap_or_else(|_| "sim".to_string());
    let soc_version = std::env::var("SIM_ENGRAM_SIMT_SOC_VERSION")
        .unwrap_or_else(|_| ENGRAM_SIMT_DEFAULT_SOC_VERSION.to_string());
    Ok(EngramSimtArtifactConfig {
        artifact_dir: Path::new(&artifact_dir).to_path_buf(),
        emb_dim: ENGRAM_CONTEXT_HIDDEN_SIZE,
        batch,
        table_rows,
        run_mode,
        soc_version,
    })
}

pub fn run_engram_simt_artifact_case(
    spec: &EngramSimtLaunchSpec,
    npu_id: u32,
) -> Result<EngramSimtLaunchReport, String> {
    let working_dir = spec.binary_path.parent().ok_or_else(|| {
        format!(
            "engram_simt_binary_parent_missing:path={}",
            spec.binary_path.display()
        )
    })?;
    let output = Command::new(&spec.binary_path)
        .current_dir(working_dir)
        .arg(format!("--case={}", spec.case_name))
        .arg(format!("--npu={npu_id}"))
        .output()
        .map_err(|err| {
            format!(
                "engram_simt_launch_failed:path={}:case={}:{}",
                spec.binary_path.display(),
                spec.case_name,
                err
            )
        })?;
    let stdout = String::from_utf8_lossy(&output.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&output.stderr).into_owned();
    let status_code = output.status.code();
    if !output.status.success() {
        return Err(format!(
            "engram_simt_case_failed:case={}:status={:?}:stdout={}:stderr={}",
            spec.case_name,
            status_code,
            stdout.escape_debug(),
            stderr.escape_debug()
        ));
    }
    let launch_status = parse_engram_simt_launch_status(&stdout, &spec.case_name)?;
    let report = EngramSimtLaunchReport {
        mode: "fused-simt",
        case_name: spec.case_name.clone(),
        binary_path: spec.binary_path.clone(),
        kernel_library_path: spec.kernel_library_path.clone(),
        working_dir: working_dir.to_path_buf(),
        npu_id,
        status_code,
        passed_cases: launch_status.passed_cases,
        failed_cases: launch_status.failed_cases,
        total_cases: launch_status.total_cases,
        selected_case_passed: launch_status.selected_case_passed,
        stdout,
        stderr,
    };
    Ok(report)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct EngramSimtLaunchStatus {
    passed_cases: usize,
    failed_cases: usize,
    total_cases: usize,
    selected_case_passed: bool,
}

fn parse_engram_simt_launch_status(
    stdout: &str,
    case_name: &str,
) -> Result<EngramSimtLaunchStatus, String> {
    let pass_line = format!("[PASS] {case_name}");
    let fail_line = format!("[FAIL] {case_name}");
    let selected_case_passed = stdout.lines().any(|line| line.trim() == pass_line);
    if stdout.lines().any(|line| line.trim() == fail_line) {
        return Err(format!("engram_simt_selected_case_failed:case={case_name}"));
    }

    let mut result = None;
    for line in stdout.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix("[engram-simt] Results:") {
            result = Some(parse_engram_simt_results_line(rest.trim(), line)?);
        }
    }
    let (passed_cases, failed_cases, total_cases) =
        result.ok_or_else(|| "engram_simt_result_summary_missing".to_string())?;
    if total_cases == 0 {
        return Err("engram_simt_result_summary_empty".to_string());
    }
    if failed_cases != 0 {
        return Err(format!(
            "engram_simt_result_summary_failed:passed={passed_cases}:failed={failed_cases}:total={total_cases}"
        ));
    }
    if passed_cases != total_cases {
        return Err(format!(
            "engram_simt_result_summary_inconsistent:passed={passed_cases}:failed={failed_cases}:total={total_cases}"
        ));
    }
    if !selected_case_passed {
        return Err(format!(
            "engram_simt_selected_case_pass_missing:case={case_name}"
        ));
    }

    Ok(EngramSimtLaunchStatus {
        passed_cases,
        failed_cases,
        total_cases,
        selected_case_passed,
    })
}

fn parse_engram_simt_results_line(
    rest: &str,
    original_line: &str,
) -> Result<(usize, usize, usize), String> {
    let parts = rest
        .split_whitespace()
        .map(|part| part.trim_end_matches(','))
        .collect::<Vec<_>>();
    if parts.len() != 6 || parts[1] != "passed" || parts[3] != "failed" || parts[5] != "total" {
        return Err(format!(
            "engram_simt_result_summary_malformed:line={original_line}"
        ));
    }
    let passed = parts[0]
        .parse::<usize>()
        .map_err(|_| format!("engram_simt_result_summary_malformed:passed={}", parts[0]))?;
    let failed = parts[2]
        .parse::<usize>()
        .map_err(|_| format!("engram_simt_result_summary_malformed:failed={}", parts[2]))?;
    let total = parts[4]
        .parse::<usize>()
        .map_err(|_| format!("engram_simt_result_summary_malformed:total={}", parts[4]))?;
    Ok((passed, failed, total))
}

fn validate_engram_simt_shape(
    emb_dim: usize,
    batch: usize,
    table_rows: usize,
) -> Result<(), String> {
    if emb_dim != ENGRAM_CONTEXT_HIDDEN_SIZE {
        return Err(format!(
            "unsupported_engram_simt_emb_dim:{emb_dim}:expected={ENGRAM_CONTEXT_HIDDEN_SIZE}"
        ));
    }
    if !ENGRAM_CONTEXT_SUPPORTED_BATCHES.contains(&batch) {
        return Err(format!("unsupported_engram_simt_batch:{batch}"));
    }
    if table_rows == 0 {
        return Err("engram_simt_table_rows_must_be_positive".to_string());
    }
    Ok(())
}

fn validate_engram_simt_runtime(run_mode: &str, soc_version: &str) -> Result<(), String> {
    match run_mode {
        "sim" | "npu" => {}
        other => return Err(format!("unsupported_engram_simt_run_mode:{other}")),
    }
    if !soc_version.starts_with("Ascend910_9599") {
        return Err(format!("unsupported_engram_simt_soc_version:{soc_version}"));
    }
    Ok(())
}

fn table_rows_tag(table_rows: usize) -> String {
    match table_rows {
        65_536 => "T64K".to_string(),
        262_144 => "T256K".to_string(),
        1_048_576 => "T1M".to_string(),
        other => format!("T{other}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::time::{SystemTime, UNIX_EPOCH};
    #[cfg(unix)]
    use std::{fs::Permissions, os::unix::fs::PermissionsExt};

    #[test]
    fn engram_simt_symbol_selects_fused_dim_and_batch() {
        assert_eq!(
            build_engram_simt_symbol(1024, 16),
            "runEngram_fused_E1024_B16"
        );
        assert_eq!(
            build_engram_simt_case_name(1024, 16, 65_536),
            "ENGRAMSIMTTest.fused_E1024_B16_T64K"
        );
    }

    #[test]
    fn engram_simt_discovery_reports_missing_artifact_dir() {
        let missing = std::env::temp_dir().join(format!(
            "sim_models_missing_engram_simt_{}",
            unique_suffix()
        ));
        let config = EngramSimtArtifactConfig::new(missing, 1, 65_536);

        let err = discover_engram_simt_artifact(&config).expect_err("missing dir should fail");

        assert!(err.contains("engram_simt_artifact_dir_missing"));
        assert!(err.contains("SIM_ENGRAM_SIMT_ARTIFACT_DIR") || err.contains("run.sh"));
    }

    #[test]
    fn engram_simt_discovery_reports_missing_binary() {
        let dir = temp_dir("sim_models_engram_simt_missing_binary");
        let config = EngramSimtArtifactConfig::new(&dir, 1, 65_536);

        let err = discover_engram_simt_artifact(&config).expect_err("missing binary should fail");

        assert!(err.contains("engram_simt_binary_missing"));
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn engram_simt_discovery_builds_launch_spec_when_artifacts_exist() {
        let dir = temp_dir("sim_models_engram_simt_artifacts");
        fs::write(dir.join(ENGRAM_SIMT_BINARY_NAME), b"stub").expect("write binary");
        fs::write(dir.join(ENGRAM_SIMT_KERNEL_LIBRARY_NAME), b"stub").expect("write kernel");
        let config = EngramSimtArtifactConfig::new(&dir, 4, 65_536);

        let spec = discover_engram_simt_artifact(&config).expect("discover artifact");

        assert_eq!(spec.mode, "fused-simt");
        assert_eq!(spec.symbol, "runEngram_fused_E1024_B4");
        assert_eq!(spec.case_name, "ENGRAMSIMTTest.fused_E1024_B4_T64K");
        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn engram_simt_discovery_rejects_unsupported_batch() {
        let config = EngramSimtArtifactConfig::new("/tmp/unused", 2, 65_536);
        let err = discover_engram_simt_artifact(&config).expect_err("batch should fail");
        assert!(err.contains("unsupported_engram_simt_batch"));
    }

    #[test]
    #[cfg(unix)]
    fn engram_simt_launch_runs_selected_case_in_artifact_dir() {
        let dir = temp_dir("sim_models_engram_simt_launch");
        let binary_path = dir.join(ENGRAM_SIMT_BINARY_NAME);
        fs::write(
            &binary_path,
            b"#!/bin/sh\nprintf 'cwd=%s\\n' \"$(pwd)\"\nprintf 'args=%s\\n' \"$*\"\nprintf '[PASS] ENGRAMSIMTTest.fused_E1024_B4_T64K\\n'\nprintf '\\n[engram-simt] Results: 1 passed, 0 failed, 1 total\\n'\n",
        )
        .expect("write binary");
        fs::set_permissions(&binary_path, Permissions::from_mode(0o755)).expect("chmod binary");
        fs::write(dir.join(ENGRAM_SIMT_KERNEL_LIBRARY_NAME), b"stub").expect("write kernel");
        let spec = discover_engram_simt_artifact(&EngramSimtArtifactConfig::new(&dir, 4, 65_536))
            .expect("discover artifact");

        let report = run_engram_simt_artifact_case(&spec, 2).expect("run selected case");

        assert_eq!(report.mode, "fused-simt");
        assert_eq!(report.working_dir, dir);
        assert_eq!(report.npu_id, 2);
        assert_eq!(report.status_code, Some(0));
        assert_eq!(report.passed_cases, 1);
        assert_eq!(report.failed_cases, 0);
        assert_eq!(report.total_cases, 1);
        assert!(report.selected_case_passed);
        assert!(report.stdout.contains("ENGRAMSIMTTest.fused_E1024_B4_T64K"));
        assert!(report.stdout.contains("--npu=2"));
        let _ = fs::remove_dir_all(report.working_dir);
    }

    #[test]
    fn engram_simt_launch_status_rejects_missing_result_summary() {
        let err = parse_engram_simt_launch_status(
            "[PASS] ENGRAMSIMTTest.fused_E1024_B4_T64K\n",
            "ENGRAMSIMTTest.fused_E1024_B4_T64K",
        )
        .expect_err("missing result summary should fail");

        assert!(err.contains("engram_simt_result_summary_missing"));
    }

    #[test]
    fn engram_simt_launch_status_rejects_selected_case_failure() {
        let err = parse_engram_simt_launch_status(
            "[FAIL] ENGRAMSIMTTest.fused_E1024_B4_T64K\n[engram-simt] Results: 0 passed, 1 failed, 1 total\n",
            "ENGRAMSIMTTest.fused_E1024_B4_T64K",
        )
        .expect_err("selected case failure should fail");

        assert!(err.contains("engram_simt_selected_case_failed"));
    }

    fn temp_dir(prefix: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("{prefix}_{}", unique_suffix()));
        fs::create_dir_all(&dir).expect("create temp dir");
        dir
    }

    fn unique_suffix() -> u128 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time")
            .as_nanos()
    }
}
