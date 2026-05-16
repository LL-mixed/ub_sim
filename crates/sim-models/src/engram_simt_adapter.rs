use crate::engram_context::{
    ENGRAM_CONTEXT_HIDDEN_SIZE, ENGRAM_CONTEXT_INDICES_PER_BATCH, ENGRAM_CONTEXT_SUPPORTED_BATCHES,
};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

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
