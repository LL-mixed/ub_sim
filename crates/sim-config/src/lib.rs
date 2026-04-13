//! Strongly typed simulator configuration.

use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ScenarioConfig {
    pub scenario: ScenarioMetaConfig,
    pub platform: PlatformConfig,
    pub topology: TopologyConfig,
    pub ub_runtime: UbRuntimeConfig,
    pub pypto: PyptoConfig,
    pub lingqu_data: LingquDataConfig,
    pub levels: LevelsConfig,
    pub routing: RoutingConfig,
    pub workload: WorkloadConfig,
    pub faults: Vec<FaultConfig>,
    pub outputs: OutputConfig,
}

impl ScenarioConfig {
    pub fn from_yaml_str(input: &str) -> Result<Self, ConfigLoadError> {
        let config: Self = serde_yaml::from_str(input)?;
        config.validate()?;
        Ok(config)
    }

    pub fn from_yaml_file(path: impl AsRef<Path>) -> Result<Self, ConfigLoadError> {
        let content = std::fs::read_to_string(path)?;
        Self::from_yaml_str(&content)
    }

    pub fn validate(&self) -> Result<(), ConfigError> {
        if self.scenario.name.trim().is_empty() {
            return Err(ConfigError::EmptyField("scenario.name"));
        }
        if self.scenario.duration_us == 0 {
            return Err(ConfigError::NonPositive("scenario.duration_us"));
        }
        if self.topology.hosts == 0 {
            return Err(ConfigError::NonPositive("topology.hosts"));
        }
        if self.topology.ubpus_per_host == 0 {
            return Err(ConfigError::NonPositive("topology.ubpus_per_host"));
        }
        if self.topology.entities_per_ubpu == 0 {
            return Err(ConfigError::NonPositive("topology.entities_per_ubpu"));
        }
        if self.ub_runtime.active_levels.is_empty() {
            return Err(ConfigError::EmptyField("ub_runtime.active_levels"));
        }
        if self.pypto.scope_runtime.max_scope_depth == 0 {
            return Err(ConfigError::NonPositive("pypto.scope_runtime.max_scope_depth"));
        }

        for domain in &self.topology.ub_domains {
            if domain.hosts.is_empty() {
                return Err(ConfigError::EmptyField("topology.ub_domains[].hosts"));
            }
            for &host_id in &domain.hosts {
                if host_id >= self.topology.hosts {
                    return Err(ConfigError::HostOutOfRange {
                        host_id,
                        hosts: self.topology.hosts,
                    });
                }
            }
        }

        self.levels.l2_ubpu_tier.validate("levels.l2_ubpu_tier")?;
        self.levels.l3_host_tier.validate("levels.l3_host_tier")?;
        self.levels.l4_domain_tier.validate("levels.l4_domain_tier")?;

        match &self.workload {
            WorkloadConfig::HotsetLoop(cfg) => cfg.validate()?,
            WorkloadConfig::TraceReplay(cfg) => cfg.validate()?,
            WorkloadConfig::RustLlmMvp(cfg) => cfg.validate()?,
        }

        Ok(())
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ScenarioMetaConfig {
    pub name: String,
    pub group: Option<String>,
    pub variant: Option<String>,
    pub seed: u64,
    pub duration_us: u64,
    pub logical_system: String,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PlatformBackend {
    Qemu,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum DeviceModelMode {
    Stub,
    Mixed,
    GuestVisible,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct PlatformConfig {
    pub backend: PlatformBackend,
    pub machine_profile: String,
    pub cpu_model: String,
    pub memory_model: String,
    pub device_model_mode: DeviceModelMode,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct UbDomainConfig {
    pub id: String,
    pub hosts: Vec<u32>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct CollapseConfig {
    pub fabric: bool,
    pub global: bool,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct TopologyConfig {
    pub hosts: u32,
    pub ubpus_per_host: u32,
    pub entities_per_ubpu: u32,
    pub ub_domains: Vec<UbDomainConfig>,
    pub collapse: CollapseConfig,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct UbRuntimeConfig {
    pub active_levels: Vec<u8>,
    pub reserved_levels: Vec<u8>,
    pub preserve_full_task_coord: bool,
}

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq)]
pub enum RuntimeDefaultLevel {
    CHIP,
    HOST,
    #[serde(rename = "CLUSTER_0")]
    Cluster0,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct SimplerBoundaryConfig {
    pub enabled: bool,
    pub chip_backend_mode: String,
    pub dispatch_latency_us: Option<u64>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ScopeRuntimeConfig {
    pub enable_multi_layer_ring: bool,
    pub enable_pl_free: bool,
    pub max_scope_depth: u32,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct PyptoConfig {
    pub enable_function_labels: bool,
    pub default_level: RuntimeDefaultLevel,
    pub allow_levels: Vec<RuntimeDefaultLevel>,
    pub simpler_boundary: SimplerBoundaryConfig,
    pub scope_runtime: ScopeRuntimeConfig,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ShmemConfig {
    pub enabled: bool,
    pub pe_count: Option<u32>,
    pub default_latency_us: Option<u64>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct BlockDeviceConfig {
    pub uba: String,
    pub blocks: u64,
    pub block_size: u64,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct BlockConfig {
    pub enabled: bool,
    pub devices: Vec<BlockDeviceConfig>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct DfsConfig {
    pub enabled: bool,
    pub namespace_root: Option<String>,
    pub metadata_latency_us: Option<u64>,
    pub data_latency_us: Option<u64>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct DbConfig {
    pub enabled: bool,
    pub inline_value_limit: Option<u64>,
    pub pipeline_batch_limit: Option<u64>,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct LingquDataConfig {
    pub shmem: ShmemConfig,
    pub block: BlockConfig,
    pub dfs: DfsConfig,
    pub db: DbConfig,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct TierConfig {
    pub capacity_blocks: u64,
    pub high_watermark: f64,
    pub low_watermark: f64,
    pub hit_latency_us: Option<u64>,
    pub fetch_latency_us: Option<u64>,
}

impl TierConfig {
    fn validate(&self, field: &'static str) -> Result<(), ConfigError> {
        if self.capacity_blocks == 0 {
            return Err(ConfigError::NonPositiveCapacity(field));
        }
        if !(0.0..=1.0).contains(&self.high_watermark) {
            return Err(ConfigError::InvalidWatermark {
                field,
                value: self.high_watermark,
            });
        }
        if !(0.0..=1.0).contains(&self.low_watermark) {
            return Err(ConfigError::InvalidWatermark {
                field,
                value: self.low_watermark,
            });
        }
        if self.low_watermark > self.high_watermark {
            return Err(ConfigError::WatermarkOrder(field));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct LevelsConfig {
    pub l2_ubpu_tier: TierConfig,
    pub l3_host_tier: TierConfig,
    pub l4_domain_tier: TierConfig,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum RoutingMode {
    Recursive,
    Flat,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct RoutingConfig {
    pub mode: RoutingMode,
    pub hit_weight: f64,
    pub load_weight: f64,
    pub capacity_weight: f64,
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct HotsetLoopWorkloadConfig {
    pub qps: u64,
    pub unique_prefixes: u64,
    pub blocks_per_request: u32,
    pub function_label_mode: String,
}

impl HotsetLoopWorkloadConfig {
    fn validate(&self) -> Result<(), ConfigError> {
        if self.qps == 0 {
            return Err(ConfigError::NonPositive("workload.qps"));
        }
        if self.unique_prefixes == 0 {
            return Err(ConfigError::NonPositive("workload.unique_prefixes"));
        }
        if self.blocks_per_request == 0 {
            return Err(ConfigError::NonPositive("workload.blocks_per_request"));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct TraceReplayWorkloadConfig {
    pub trace_path: String,
}

impl TraceReplayWorkloadConfig {
    fn validate(&self) -> Result<(), ConfigError> {
        if self.trace_path.trim().is_empty() {
            return Err(ConfigError::EmptyField("workload.trace_path"));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct RustLlmMvpWorkloadConfig {
    pub profile: String,
    pub qps: u64,
    pub unique_prefixes: u64,
    pub blocks_per_request: u32,
    pub function_label_mode: String,
}

impl RustLlmMvpWorkloadConfig {
    fn validate(&self) -> Result<(), ConfigError> {
        if self.profile.trim().is_empty() {
            return Err(ConfigError::EmptyField("workload.profile"));
        }
        if self.qps == 0 {
            return Err(ConfigError::NonPositive("workload.qps"));
        }
        if self.unique_prefixes == 0 {
            return Err(ConfigError::NonPositive("workload.unique_prefixes"));
        }
        if self.blocks_per_request == 0 {
            return Err(ConfigError::NonPositive("workload.blocks_per_request"));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(tag = "type")]
pub enum WorkloadConfig {
    #[serde(rename = "hotset_loop")]
    HotsetLoop(HotsetLoopWorkloadConfig),
    #[serde(rename = "trace_replay")]
    TraceReplay(TraceReplayWorkloadConfig),
    #[serde(rename = "rust_llm_server_mvp")]
    RustLlmMvp(RustLlmMvpWorkloadConfig),
}

#[derive(Debug, Clone, Deserialize, Serialize)]
#[serde(tag = "type")]
pub enum FaultConfig {
    #[serde(rename = "host_degraded")]
    HostDegraded { at_us: u64, host_id: u32 },
    #[serde(rename = "block_corruption")]
    BlockCorruption {
        at_us: u64,
        level: String,
        node_id: String,
        block_hash: String,
    },
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct OutputConfig {
    pub trace: bool,
    pub metrics_csv: bool,
    pub summary_json: bool,
    pub emit_task_coord_trace: bool,
    pub emit_data_service_trace: bool,
    pub emit_qemu_platform_trace: bool,
}

#[derive(Debug, thiserror::Error)]
pub enum ConfigError {
    #[error("field `{0}` must not be empty")]
    EmptyField(&'static str),
    #[error("field `{0}` must be positive")]
    NonPositive(&'static str),
    #[error("tier `{0}` capacity must be positive")]
    NonPositiveCapacity(&'static str),
    #[error("tier `{field}` has invalid watermark value {value}")]
    InvalidWatermark { field: &'static str, value: f64 },
    #[error("tier `{0}` low watermark must not exceed high watermark")]
    WatermarkOrder(&'static str),
    #[error("domain host id {host_id} is out of range for topology.hosts={hosts}")]
    HostOutOfRange { host_id: u32, hosts: u32 },
}

#[derive(Debug, thiserror::Error)]
pub enum ConfigLoadError {
    #[error(transparent)]
    Io(#[from] std::io::Error),
    #[error(transparent)]
    Parse(#[from] serde_yaml::Error),
    #[error(transparent)]
    Validation(#[from] ConfigError),
}

#[cfg(test)]
mod tests {
    use super::ScenarioConfig;

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
  qps: 2000
  unique_prefixes: 256
  blocks_per_request: 4
  function_label_mode: host_orchestration
faults:
  - type: host_degraded
    at_us: 300000
    host_id: 0
outputs:
  trace: true
  metrics_csv: true
  summary_json: true
  emit_task_coord_trace: true
  emit_data_service_trace: true
  emit_qemu_platform_trace: true
"#;

    #[test]
    fn parses_and_validates_scenario_yaml() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid scenario yaml");
        assert_eq!(config.topology.hosts, 2);
        assert_eq!(config.topology.ub_domains.len(), 1);
    }
}
