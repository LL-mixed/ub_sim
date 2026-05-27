use crate::engram_context::{
    validate_engram_context_op, validate_paper_engram_context_op, EngramContextOp,
    PaperEngramContextOp, ENGRAM_CONTEXT_HIDDEN_SIZE, ENGRAM_CONTEXT_INDICES_PER_BATCH,
    ENGRAM_CONTEXT_SUPPORTED_BATCHES,
};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::fs;
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

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct EngramSimtRuntimeInput {
    pub mode: &'static str,
    pub table: Vec<f32>,
    pub table_rows: usize,
    pub indices: Vec<i32>,
    pub hidden: Vec<f32>,
    pub gate_weight: Vec<f32>,
    pub batch: usize,
    pub hidden_size: usize,
    pub source_lookup_count_per_batch: Vec<usize>,
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

pub fn materialize_engram_simt_runtime_case_data(
    spec: &EngramSimtLaunchSpec,
    input: &EngramSimtRuntimeInput,
    expected_output: &[f32],
) -> Result<PathBuf, String> {
    validate_engram_simt_runtime_input_for_spec(spec, input, expected_output)?;
    let case_dir = engram_simt_runtime_case_dir(spec)?;
    fs::create_dir_all(&case_dir).map_err(|err| {
        format!(
            "engram_simt_runtime_case_dir_create_failed:path={}:{}",
            case_dir.display(),
            err
        )
    })?;
    write_runtime_case_file(&case_dir, "table.bin", &f32s_to_le_bytes(&input.table))?;
    write_runtime_case_file(&case_dir, "indices.bin", &i32s_to_le_bytes(&input.indices))?;
    write_runtime_case_file(&case_dir, "hidden.bin", &f32s_to_le_bytes(&input.hidden))?;
    write_runtime_case_file(
        &case_dir,
        "gate_weight.bin",
        &f32s_to_le_bytes(&input.gate_weight),
    )?;
    write_runtime_case_file(&case_dir, "golden.bin", &f32s_to_le_bytes(expected_output))?;
    Ok(case_dir)
}

pub fn run_engram_simt_runtime_input_case(
    spec: &EngramSimtLaunchSpec,
    npu_id: u32,
    input: &EngramSimtRuntimeInput,
    expected_output: &[f32],
) -> Result<EngramSimtLaunchReport, String> {
    materialize_engram_simt_runtime_case_data(spec, input, expected_output)?;
    run_engram_simt_artifact_case(spec, npu_id)
}

pub fn build_engram_simt_runtime_input_from_legacy_op(
    op: &EngramContextOp<'_>,
) -> Result<EngramSimtRuntimeInput, String> {
    validate_engram_context_op(op)?;
    validate_engram_simt_runtime_shape(op.batch, op.hidden_size)?;
    Ok(EngramSimtRuntimeInput {
        mode: "legacy-single-table",
        table: op.table.to_vec(),
        table_rows: op.table_rows,
        indices: op.indices.to_vec(),
        hidden: op.hidden.to_vec(),
        gate_weight: op.gate_weight.to_vec(),
        batch: op.batch,
        hidden_size: op.hidden_size,
        source_lookup_count_per_batch: vec![ENGRAM_CONTEXT_INDICES_PER_BATCH; op.batch],
    })
}

pub fn build_engram_simt_runtime_input_from_paper_op(
    op: &PaperEngramContextOp<'_>,
) -> Result<EngramSimtRuntimeInput, String> {
    validate_paper_engram_context_op(op)?;
    validate_engram_simt_runtime_shape(op.batch, op.hidden_size)?;

    let mut table_offsets = BTreeMap::<(u8, u16), usize>::new();
    let mut table = Vec::<f32>::new();
    let mut table_rows = 0usize;
    for view in op.tables {
        table_offsets.insert((view.order, view.head), table_rows);
        table_rows = table_rows
            .checked_add(view.table_rows)
            .ok_or_else(|| "engram_simt_paper_table_rows_overflow".to_string())?;
        table.extend_from_slice(view.table);
    }
    if table_rows == 0 {
        return Err("engram_simt_paper_table_rows_must_be_positive".to_string());
    }

    let mut indices = Vec::with_capacity(op.batch * ENGRAM_CONTEXT_INDICES_PER_BATCH);
    let mut source_lookup_count_per_batch = Vec::with_capacity(op.batch);
    for batch_index in 0..op.batch {
        let lookups = op
            .lookups
            .iter()
            .filter(|lookup| lookup.batch_index == batch_index)
            .collect::<Vec<_>>();
        let lookup_count = lookups.len();
        if lookup_count == 0 {
            return Err(format!(
                "engram_simt_paper_lookup_count_zero:batch_index={batch_index}"
            ));
        }
        if lookup_count > ENGRAM_CONTEXT_INDICES_PER_BATCH
            || ENGRAM_CONTEXT_INDICES_PER_BATCH % lookup_count != 0
        {
            return Err(format!(
                "engram_simt_paper_lookup_count_unsupported:batch_index={batch_index}:count={lookup_count}:slots={ENGRAM_CONTEXT_INDICES_PER_BATCH}"
            ));
        }
        let repeat = ENGRAM_CONTEXT_INDICES_PER_BATCH / lookup_count;
        for lookup in lookups {
            let table_offset =
                table_offsets
                    .get(&(lookup.order, lookup.head))
                    .ok_or_else(|| {
                        format!(
                            "engram_simt_paper_table_missing:order={}:head={}",
                            lookup.order, lookup.head
                        )
                    })?;
            let local_row = usize::try_from(lookup.row).map_err(|_| {
                format!(
                    "engram_simt_paper_row_exceeds_usize:order={}:head={}:row={}",
                    lookup.order, lookup.head, lookup.row
                )
            })?;
            let global_row = table_offset
                .checked_add(local_row)
                .ok_or_else(|| "engram_simt_paper_global_row_overflow".to_string())?;
            let global_row = i32::try_from(global_row).map_err(|_| {
                format!("engram_simt_paper_global_row_exceeds_i32:row={global_row}")
            })?;
            for _ in 0..repeat {
                indices.push(global_row);
            }
        }
        source_lookup_count_per_batch.push(lookup_count);
    }

    Ok(EngramSimtRuntimeInput {
        mode: "paper-packed-single-table",
        table,
        table_rows,
        indices,
        hidden: op.hidden.to_vec(),
        gate_weight: op.gate_weight.to_vec(),
        batch: op.batch,
        hidden_size: op.hidden_size,
        source_lookup_count_per_batch,
    })
}

fn validate_engram_simt_runtime_shape(batch: usize, hidden_size: usize) -> Result<(), String> {
    if !ENGRAM_CONTEXT_SUPPORTED_BATCHES.contains(&batch) {
        return Err(format!("unsupported_engram_simt_batch:{batch}"));
    }
    if hidden_size != ENGRAM_CONTEXT_HIDDEN_SIZE {
        return Err(format!(
            "unsupported_engram_simt_hidden_size:{hidden_size}:expected={ENGRAM_CONTEXT_HIDDEN_SIZE}"
        ));
    }
    Ok(())
}

fn validate_engram_simt_runtime_input_for_spec(
    spec: &EngramSimtLaunchSpec,
    input: &EngramSimtRuntimeInput,
    expected_output: &[f32],
) -> Result<(), String> {
    if input.hidden_size != spec.emb_dim {
        return Err(format!(
            "engram_simt_runtime_hidden_size_mismatch:input={}:spec={}",
            input.hidden_size, spec.emb_dim
        ));
    }
    if input.batch != spec.batch {
        return Err(format!(
            "engram_simt_runtime_batch_mismatch:input={}:spec={}",
            input.batch, spec.batch
        ));
    }
    if input.table_rows != spec.table_rows {
        return Err(format!(
            "engram_simt_runtime_table_rows_mismatch:input={}:spec={}",
            input.table_rows, spec.table_rows
        ));
    }
    let table_elems = input
        .table_rows
        .checked_mul(input.hidden_size)
        .ok_or_else(|| "engram_simt_runtime_table_len_overflow".to_string())?;
    if input.table.len() != table_elems {
        return Err(format!(
            "engram_simt_runtime_table_len_mismatch:got={}:expected={table_elems}",
            input.table.len()
        ));
    }
    let vector_elems = input
        .batch
        .checked_mul(input.hidden_size)
        .ok_or_else(|| "engram_simt_runtime_vector_len_overflow".to_string())?;
    if input.hidden.len() != vector_elems || input.gate_weight.len() != vector_elems {
        return Err(format!(
            "engram_simt_runtime_vector_len_mismatch:hidden={} gate={} expected={vector_elems}",
            input.hidden.len(),
            input.gate_weight.len()
        ));
    }
    let index_elems = input
        .batch
        .checked_mul(spec.indices_per_batch)
        .ok_or_else(|| "engram_simt_runtime_indices_len_overflow".to_string())?;
    if input.indices.len() != index_elems {
        return Err(format!(
            "engram_simt_runtime_indices_len_mismatch:got={}:expected={index_elems}",
            input.indices.len()
        ));
    }
    if expected_output.len() != vector_elems {
        return Err(format!(
            "engram_simt_runtime_golden_len_mismatch:got={}:expected={vector_elems}",
            expected_output.len()
        ));
    }
    Ok(())
}

fn engram_simt_runtime_case_dir(spec: &EngramSimtLaunchSpec) -> Result<PathBuf, String> {
    let build_dir = spec.binary_path.parent().ok_or_else(|| {
        format!(
            "engram_simt_binary_parent_missing:path={}",
            spec.binary_path.display()
        )
    })?;
    let artifact_root = build_dir.parent().ok_or_else(|| {
        format!(
            "engram_simt_artifact_root_missing:path={}",
            build_dir.display()
        )
    })?;
    Ok(artifact_root.join("data").join(&spec.case_name))
}

fn write_runtime_case_file(case_dir: &Path, name: &str, bytes: &[u8]) -> Result<(), String> {
    let path = case_dir.join(name);
    fs::write(&path, bytes).map_err(|err| {
        format!(
            "engram_simt_runtime_case_write_failed:path={}:{}",
            path.display(),
            err
        )
    })
}

fn f32s_to_le_bytes(values: &[f32]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(std::mem::size_of_val(values));
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    bytes
}

fn i32s_to_le_bytes(values: &[i32]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(std::mem::size_of_val(values));
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    bytes
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
    use crate::engram_context::{PaperEngramContextLookupRef, PaperEngramContextTableView};
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
    #[cfg(unix)]
    fn engram_simt_runtime_input_materializes_and_launches_vendor_case_data() {
        let root = temp_dir("sim_models_engram_simt_runtime_case");
        let build_dir = root.join("build");
        fs::create_dir_all(&build_dir).expect("create build dir");
        let binary_path = build_dir.join(ENGRAM_SIMT_BINARY_NAME);
        fs::write(
            &binary_path,
            b"#!/bin/sh\ncase_dir='../data/ENGRAMSIMTTest.fused_E1024_B1_T8'\ntest -f \"$case_dir/table.bin\" || exit 7\ntest -f \"$case_dir/indices.bin\" || exit 8\ntest -f \"$case_dir/hidden.bin\" || exit 9\ntest -f \"$case_dir/gate_weight.bin\" || exit 10\ntest -f \"$case_dir/golden.bin\" || exit 11\nprintf '[PASS] ENGRAMSIMTTest.fused_E1024_B1_T8\\n'\nprintf '[engram-simt] Results: 1 passed, 0 failed, 1 total\\n'\n",
        )
        .expect("write binary");
        fs::set_permissions(&binary_path, Permissions::from_mode(0o755)).expect("chmod binary");
        fs::write(build_dir.join(ENGRAM_SIMT_KERNEL_LIBRARY_NAME), b"stub").expect("write kernel");
        let spec = discover_engram_simt_artifact(&EngramSimtArtifactConfig::new(&build_dir, 1, 8))
            .expect("discover artifact");
        let hidden_size = ENGRAM_CONTEXT_HIDDEN_SIZE;
        let input = EngramSimtRuntimeInput {
            mode: "legacy-single-table",
            table: vec![0.25; 8 * hidden_size],
            table_rows: 8,
            indices: (0..ENGRAM_CONTEXT_INDICES_PER_BATCH)
                .map(|index| index as i32)
                .collect(),
            hidden: vec![0.125; hidden_size],
            gate_weight: vec![0.0; hidden_size],
            batch: 1,
            hidden_size,
            source_lookup_count_per_batch: vec![ENGRAM_CONTEXT_INDICES_PER_BATCH],
        };
        let expected_output = vec![0.5; hidden_size];

        let report = run_engram_simt_runtime_input_case(&spec, 3, &input, &expected_output)
            .expect("launch runtime case");
        let case_dir = root.join("data").join("ENGRAMSIMTTest.fused_E1024_B1_T8");

        assert_eq!(report.working_dir, build_dir);
        assert_eq!(report.npu_id, 3);
        assert_eq!(
            fs::read(case_dir.join("table.bin"))
                .expect("read table")
                .len(),
            input.table.len() * std::mem::size_of::<f32>()
        );
        assert_eq!(
            fs::read(case_dir.join("indices.bin"))
                .expect("read indices")
                .len(),
            input.indices.len() * std::mem::size_of::<i32>()
        );
        assert_eq!(
            fs::read(case_dir.join("golden.bin"))
                .expect("read golden")
                .len(),
            expected_output.len() * std::mem::size_of::<f32>()
        );
        let _ = fs::remove_dir_all(root);
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

    #[test]
    fn engram_simt_runtime_input_packs_paper_op_as_single_table() {
        let hidden_size = ENGRAM_CONTEXT_HIDDEN_SIZE;
        let hidden = vec![0.0f32; hidden_size];
        let gate_weight = vec![0.0f32; hidden_size];
        let table_20 = constant_table(2, hidden_size, 0.125);
        let table_21 = constant_table(2, hidden_size, 0.25);
        let table_30 = constant_table(2, hidden_size, 0.5);
        let table_31 = constant_table(2, hidden_size, 1.0);
        let tables = vec![
            PaperEngramContextTableView {
                order: 2,
                head: 0,
                table: &table_20,
                table_rows: 2,
            },
            PaperEngramContextTableView {
                order: 2,
                head: 1,
                table: &table_21,
                table_rows: 2,
            },
            PaperEngramContextTableView {
                order: 3,
                head: 0,
                table: &table_30,
                table_rows: 2,
            },
            PaperEngramContextTableView {
                order: 3,
                head: 1,
                table: &table_31,
                table_rows: 2,
            },
        ];
        let lookups = vec![
            PaperEngramContextLookupRef {
                batch_index: 0,
                order: 2,
                head: 0,
                row: 0,
                exact_key: 0x20,
            },
            PaperEngramContextLookupRef {
                batch_index: 0,
                order: 2,
                head: 1,
                row: 1,
                exact_key: 0x21,
            },
            PaperEngramContextLookupRef {
                batch_index: 0,
                order: 3,
                head: 0,
                row: 0,
                exact_key: 0x30,
            },
            PaperEngramContextLookupRef {
                batch_index: 0,
                order: 3,
                head: 1,
                row: 1,
                exact_key: 0x31,
            },
        ];
        let paper_op = PaperEngramContextOp {
            tables: &tables,
            lookups: &lookups,
            hidden: &hidden,
            gate_weight: &gate_weight,
            batch: 1,
            hidden_size,
        };

        let paper = crate::engram_context::run_paper_engram_context_reference(&paper_op)
            .expect("run paper reference");
        let packed = build_engram_simt_runtime_input_from_paper_op(&paper_op)
            .expect("pack paper op for fused SIMT");
        let packed_op = EngramContextOp {
            table: &packed.table,
            table_rows: packed.table_rows,
            indices: &packed.indices,
            hidden: &packed.hidden,
            gate_weight: &packed.gate_weight,
            batch: packed.batch,
            hidden_size: packed.hidden_size,
        };
        let legacy = crate::engram_context::run_engram_context_reference(&packed_op)
            .expect("run packed reference");

        assert_eq!(packed.mode, "paper-packed-single-table");
        assert_eq!(packed.table_rows, 8);
        assert_eq!(packed.source_lookup_count_per_batch, vec![4]);
        assert_eq!(packed.indices, vec![0, 0, 3, 3, 4, 4, 7, 7]);
        assert_eq!(legacy.output, paper.output);
        assert_eq!(legacy.report.output_checksum, paper.report.output_checksum);
    }

    #[test]
    fn engram_simt_runtime_input_rejects_unrepresentable_paper_lookup_count() {
        let hidden_size = ENGRAM_CONTEXT_HIDDEN_SIZE;
        let hidden = vec![0.0f32; hidden_size];
        let gate_weight = vec![0.0f32; hidden_size];
        let table_20 = constant_table(2, hidden_size, 0.125);
        let table_21 = constant_table(2, hidden_size, 0.25);
        let table_30 = constant_table(2, hidden_size, 0.5);
        let tables = vec![
            PaperEngramContextTableView {
                order: 2,
                head: 0,
                table: &table_20,
                table_rows: 2,
            },
            PaperEngramContextTableView {
                order: 2,
                head: 1,
                table: &table_21,
                table_rows: 2,
            },
            PaperEngramContextTableView {
                order: 3,
                head: 0,
                table: &table_30,
                table_rows: 2,
            },
        ];
        let lookups = vec![
            PaperEngramContextLookupRef {
                batch_index: 0,
                order: 2,
                head: 0,
                row: 0,
                exact_key: 0x20,
            },
            PaperEngramContextLookupRef {
                batch_index: 0,
                order: 2,
                head: 1,
                row: 1,
                exact_key: 0x21,
            },
            PaperEngramContextLookupRef {
                batch_index: 0,
                order: 3,
                head: 0,
                row: 0,
                exact_key: 0x30,
            },
        ];
        let paper_op = PaperEngramContextOp {
            tables: &tables,
            lookups: &lookups,
            hidden: &hidden,
            gate_weight: &gate_weight,
            batch: 1,
            hidden_size,
        };

        let err = build_engram_simt_runtime_input_from_paper_op(&paper_op)
            .expect_err("three paper lookups cannot fill eight SIMT slots evenly");

        assert!(err.contains("engram_simt_paper_lookup_count_unsupported"));
    }

    fn temp_dir(prefix: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("{prefix}_{}", unique_suffix()));
        fs::create_dir_all(&dir).expect("create temp dir");
        dir
    }

    fn constant_table(rows: usize, hidden_size: usize, value: f32) -> Vec<f32> {
        vec![value; rows * hidden_size]
    }

    fn unique_suffix() -> u128 {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time")
            .as_nanos()
    }
}
