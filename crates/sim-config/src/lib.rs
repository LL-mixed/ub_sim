//! Strongly typed simulator configuration.

use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct ScenarioConfig {
    pub scenario: ScenarioMetaConfig,
    pub platform: PlatformConfig,
    #[serde(default)]
    pub remote_memory_model: RemoteMemoryModelConfig,
    #[serde(default)]
    pub async_load_model: AsyncLoadModelConfig,
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
            return Err(ConfigError::NonPositive(
                "pypto.scope_runtime.max_scope_depth",
            ));
        }

        self.remote_memory_model.validate()?;
        self.async_load_model.validate()?;

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
        self.levels
            .l4_domain_tier
            .validate("levels.l4_domain_tier")?;

        match &self.workload {
            WorkloadConfig::HotsetLoop(cfg) => cfg.validate()?,
            WorkloadConfig::TraceReplay(cfg) => cfg.validate()?,
            WorkloadConfig::DualNodeShmemMailbox(cfg) => cfg.validate()?,
            WorkloadConfig::DualNodeBlockCompute(cfg) => cfg.validate()?,
            WorkloadConfig::DualNodeCacheFill(cfg) => cfg.validate()?,
            WorkloadConfig::RustLlmMvp(cfg) => cfg.validate()?,
        }

        Ok(())
    }
}

const REMOTE_MEMORY_MAX_LATENCY_NS: u64 = 10_000_000_000;
const REMOTE_MEMORY_MAX_QUEUE_DEPTH: u32 = 65_535;
const REMOTE_MEMORY_PPM_SCALE: u32 = 1_000_000;

#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum RemoteMemoryTimeSource {
    QemuVirtual,
}

impl Default for RemoteMemoryTimeSource {
    fn default() -> Self {
        Self::QemuVirtual
    }
}

#[derive(Debug, Clone, Copy, Deserialize, Serialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum RemoteMemoryJitterMode {
    None,
    Uniform,
}

impl Default for RemoteMemoryJitterMode {
    fn default() -> Self {
        Self::None
    }
}

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq)]
#[serde(default, deny_unknown_fields)]
pub struct RemoteMemoryJitterConfig {
    pub mode: RemoteMemoryJitterMode,
    pub max_abs_ns: u64,
}

impl Default for RemoteMemoryJitterConfig {
    fn default() -> Self {
        Self {
            mode: RemoteMemoryJitterMode::None,
            max_abs_ns: 0,
        }
    }
}

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq, Default)]
#[serde(default, deny_unknown_fields)]
pub struct RemoteMemoryTailConfig {
    pub probability_ppm: u32,
    pub extra_latency_ns: u64,
}

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq)]
#[serde(default, deny_unknown_fields)]
pub struct RemoteMemoryModelConfig {
    pub enabled: bool,
    pub time_source: RemoteMemoryTimeSource,
    pub fixed_latency_ns: u64,
    pub jitter: RemoteMemoryJitterConfig,
    pub tail: RemoteMemoryTailConfig,
    pub queue_depth: u32,
    pub reorder_window: u32,
    pub drop_ppm: u32,
    pub error_ppm: u32,
    pub duplicate_ppm: u32,
    pub duplicate_delay_ns: u64,
    pub seed: u64,
}

impl Default for RemoteMemoryModelConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            time_source: RemoteMemoryTimeSource::QemuVirtual,
            fixed_latency_ns: 0,
            jitter: RemoteMemoryJitterConfig::default(),
            tail: RemoteMemoryTailConfig::default(),
            queue_depth: 64,
            reorder_window: 1,
            drop_ppm: 0,
            error_ppm: 0,
            duplicate_ppm: 0,
            duplicate_delay_ns: 1_000,
            seed: 1,
        }
    }
}

impl RemoteMemoryModelConfig {
    fn validate(&self) -> Result<(), ConfigError> {
        validate_max_u64(
            "remote_memory_model.fixed_latency_ns",
            self.fixed_latency_ns,
            REMOTE_MEMORY_MAX_LATENCY_NS,
        )?;
        validate_max_u64(
            "remote_memory_model.tail.extra_latency_ns",
            self.tail.extra_latency_ns,
            REMOTE_MEMORY_MAX_LATENCY_NS,
        )?;
        validate_max_u64(
            "remote_memory_model.duplicate_delay_ns",
            self.duplicate_delay_ns,
            REMOTE_MEMORY_MAX_LATENCY_NS,
        )?;
        let jitter_max = self
            .fixed_latency_ns
            .checked_add(REMOTE_MEMORY_MAX_LATENCY_NS)
            .expect("validated fixed latency addition cannot overflow");
        validate_max_u64(
            "remote_memory_model.jitter.max_abs_ns",
            self.jitter.max_abs_ns,
            jitter_max,
        )?;
        if self.jitter.mode == RemoteMemoryJitterMode::None && self.jitter.max_abs_ns != 0 {
            return Err(ConfigError::InvalidCombination(
                "remote_memory_model.jitter.max_abs_ns must be zero when mode is none",
            ));
        }
        if self.queue_depth == 0 || self.queue_depth > REMOTE_MEMORY_MAX_QUEUE_DEPTH {
            return Err(ConfigError::ValueOutOfRange {
                field: "remote_memory_model.queue_depth",
                value: u64::from(self.queue_depth),
                min: 1,
                max: u64::from(REMOTE_MEMORY_MAX_QUEUE_DEPTH),
            });
        }
        if self.reorder_window == 0 || self.reorder_window > self.queue_depth {
            return Err(ConfigError::ValueOutOfRange {
                field: "remote_memory_model.reorder_window",
                value: u64::from(self.reorder_window),
                min: 1,
                max: u64::from(self.queue_depth),
            });
        }
        validate_ppm(
            "remote_memory_model.tail.probability_ppm",
            self.tail.probability_ppm,
        )?;
        validate_ppm("remote_memory_model.drop_ppm", self.drop_ppm)?;
        validate_ppm("remote_memory_model.error_ppm", self.error_ppm)?;
        validate_ppm("remote_memory_model.duplicate_ppm", self.duplicate_ppm)?;
        if self.drop_ppm.saturating_add(self.error_ppm) > REMOTE_MEMORY_PPM_SCALE {
            return Err(ConfigError::InvalidCombination(
                "remote_memory_model.drop_ppm + error_ppm must not exceed 1000000",
            ));
        }
        Ok(())
    }
}

const ASYNC_LOAD_MAX_CONTEXT_ENTRIES: u16 = 64;
const ASYNC_LOAD_MAX_PENDING_LOAD_ENTRIES: u16 = 64;
const ASYNC_LOAD_MAX_EVENT_QUEUE_DEPTH: u16 = 128;
const ASYNC_LOAD_MAX_CLOCK_MHZ: u32 = 100_000;

#[derive(Debug, Clone, Deserialize, Serialize, PartialEq, Eq)]
#[serde(default, deny_unknown_fields)]
pub struct AsyncLoadModelConfig {
    pub enabled: bool,
    pub context_entries: u16,
    pub pending_load_entries: u16,
    pub event_queue_depth: u16,
    pub clock_mhz: u32,
}

impl Default for AsyncLoadModelConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            context_entries: 64,
            pending_load_entries: 64,
            event_queue_depth: 128,
            clock_mhz: 2_000,
        }
    }
}

impl AsyncLoadModelConfig {
    fn validate(&self) -> Result<(), ConfigError> {
        validate_range_u32(
            "async_load_model.context_entries",
            u32::from(self.context_entries),
            1,
            u32::from(ASYNC_LOAD_MAX_CONTEXT_ENTRIES),
        )?;
        validate_range_u32(
            "async_load_model.pending_load_entries",
            u32::from(self.pending_load_entries),
            1,
            u32::from(ASYNC_LOAD_MAX_PENDING_LOAD_ENTRIES),
        )?;
        validate_range_u32(
            "async_load_model.event_queue_depth",
            u32::from(self.event_queue_depth),
            1,
            u32::from(ASYNC_LOAD_MAX_EVENT_QUEUE_DEPTH),
        )?;
        validate_range_u32(
            "async_load_model.clock_mhz",
            self.clock_mhz,
            1,
            ASYNC_LOAD_MAX_CLOCK_MHZ,
        )?;
        Ok(())
    }
}

fn validate_range_u32(
    field: &'static str,
    value: u32,
    min: u32,
    max: u32,
) -> Result<(), ConfigError> {
    if value < min || value > max {
        return Err(ConfigError::ValueOutOfRange {
            field,
            value: u64::from(value),
            min: u64::from(min),
            max: u64::from(max),
        });
    }
    Ok(())
}

fn validate_max_u64(field: &'static str, value: u64, max: u64) -> Result<(), ConfigError> {
    if value > max {
        return Err(ConfigError::ValueOutOfRange {
            field,
            value,
            min: 0,
            max,
        });
    }
    Ok(())
}

fn validate_ppm(field: &'static str, value: u32) -> Result<(), ConfigError> {
    if value > REMOTE_MEMORY_PPM_SCALE {
        return Err(ConfigError::ValueOutOfRange {
            field,
            value: u64::from(value),
            min: 0,
            max: u64::from(REMOTE_MEMORY_PPM_SCALE),
        });
    }
    Ok(())
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
pub struct DualNodeShmemMailboxWorkloadConfig {
    pub rounds: u64,
    pub payload_bytes: u64,
}

impl DualNodeShmemMailboxWorkloadConfig {
    fn validate(&self) -> Result<(), ConfigError> {
        if self.rounds == 0 {
            return Err(ConfigError::NonPositive("workload.rounds"));
        }
        if self.payload_bytes == 0 {
            return Err(ConfigError::NonPositive("workload.payload_bytes"));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct DualNodeBlockComputeWorkloadConfig {
    pub rounds: u64,
}

impl DualNodeBlockComputeWorkloadConfig {
    fn validate(&self) -> Result<(), ConfigError> {
        if self.rounds == 0 {
            return Err(ConfigError::NonPositive("workload.rounds"));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Deserialize, Serialize)]
pub struct DualNodeCacheFillWorkloadConfig {
    pub rounds: u64,
}

impl DualNodeCacheFillWorkloadConfig {
    fn validate(&self) -> Result<(), ConfigError> {
        if self.rounds == 0 {
            return Err(ConfigError::NonPositive("workload.rounds"));
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
    #[serde(rename = "dual_node_shmem_mailbox")]
    DualNodeShmemMailbox(DualNodeShmemMailboxWorkloadConfig),
    #[serde(rename = "dual_node_block_compute")]
    DualNodeBlockCompute(DualNodeBlockComputeWorkloadConfig),
    #[serde(rename = "dual_node_cache_fill")]
    DualNodeCacheFill(DualNodeCacheFillWorkloadConfig),
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
    #[error("field `{field}` value {value} is outside [{min}, {max}]")]
    ValueOutOfRange {
        field: &'static str,
        value: u64,
        min: u64,
        max: u64,
    },
    #[error("invalid configuration: {0}")]
    InvalidCombination(&'static str),
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
    use super::{RemoteMemoryJitterMode, ScenarioConfig};

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
        assert!(!config.remote_memory_model.enabled);
        assert_eq!(config.remote_memory_model.queue_depth, 64);
        assert_eq!(config.remote_memory_model.reorder_window, 1);
        assert!(!config.async_load_model.enabled);
        assert_eq!(config.async_load_model.context_entries, 64);
    }

    #[test]
    fn parses_canonical_remote_memory_model() {
        let yaml = VALID_YAML.replace(
            "topology:\n",
            "remote_memory_model:\n  enabled: true\n  time_source: qemu_virtual\n  fixed_latency_ns: 100000\n  jitter:\n    mode: uniform\n    max_abs_ns: 20000\n  tail:\n    probability_ppm: 10000\n    extra_latency_ns: 900000\n  queue_depth: 64\n  reorder_window: 8\n  drop_ppm: 10\n  error_ppm: 20\n  duplicate_ppm: 30\n  duplicate_delay_ns: 1000\n  seed: 7\ntopology:\n",
        );
        let config = ScenarioConfig::from_yaml_str(&yaml).expect("canonical remote model");

        assert!(config.remote_memory_model.enabled);
        assert_eq!(config.remote_memory_model.fixed_latency_ns, 100_000);
        assert_eq!(
            config.remote_memory_model.jitter.mode,
            RemoteMemoryJitterMode::Uniform
        );
        assert_eq!(config.remote_memory_model.reorder_window, 8);
        assert_eq!(config.remote_memory_model.seed, 7);
    }

    #[test]
    fn rejects_unknown_remote_memory_model_field() {
        let yaml = VALID_YAML.replace(
            "topology:\n",
            "remote_memory_model:\n  enabled: true\n  fixed_latncy_ns: 1\ntopology:\n",
        );
        let error = ScenarioConfig::from_yaml_str(&yaml).expect_err("unknown field must fail");

        assert!(error.to_string().contains("fixed_latncy_ns"));
    }

    #[test]
    fn rejects_invalid_remote_memory_model_bounds() {
        let queue_yaml = VALID_YAML.replace(
            "topology:\n",
            "remote_memory_model:\n  queue_depth: 4\n  reorder_window: 5\ntopology:\n",
        );
        let queue_error =
            ScenarioConfig::from_yaml_str(&queue_yaml).expect_err("reorder beyond queue");
        assert!(queue_error
            .to_string()
            .contains("remote_memory_model.reorder_window"));

        let outcome_yaml = VALID_YAML.replace(
            "topology:\n",
            "remote_memory_model:\n  drop_ppm: 600000\n  error_ppm: 500000\ntopology:\n",
        );
        let outcome_error =
            ScenarioConfig::from_yaml_str(&outcome_yaml).expect_err("outcomes exceed ppm scale");
        assert!(outcome_error.to_string().contains("drop_ppm + error_ppm"));

        let jitter_yaml = VALID_YAML.replace(
            "topology:\n",
            "remote_memory_model:\n  jitter:\n    mode: none\n    max_abs_ns: 1\ntopology:\n",
        );
        let jitter_error =
            ScenarioConfig::from_yaml_str(&jitter_yaml).expect_err("none jitter must be zero");
        assert!(jitter_error.to_string().contains("max_abs_ns must be zero"));
    }

    #[test]
    fn parses_and_validates_async_load_model() {
        let yaml = VALID_YAML.replace(
            "topology:\n",
            "async_load_model:\n  enabled: true\n  context_entries: 8\n  pending_load_entries: 4\n  event_queue_depth: 16\n  clock_mhz: 1800\ntopology:\n",
        );
        let config = ScenarioConfig::from_yaml_str(&yaml).expect("scheduler core model");

        assert!(config.async_load_model.enabled);
        assert_eq!(config.async_load_model.context_entries, 8);
        assert_eq!(config.async_load_model.pending_load_entries, 4);
        assert_eq!(config.async_load_model.event_queue_depth, 16);
        assert_eq!(config.async_load_model.clock_mhz, 1_800);
    }

    #[test]
    fn rejects_invalid_or_unknown_async_load_model_fields() {
        let capacity_yaml = VALID_YAML.replace(
            "topology:\n",
            "async_load_model:\n  context_entries: 65\ntopology:\n",
        );
        let capacity_error = ScenarioConfig::from_yaml_str(&capacity_yaml)
            .expect_err("context capacity must be bounded");
        assert!(capacity_error
            .to_string()
            .contains("async_load_model.context_entries"));

        let unknown_yaml = VALID_YAML.replace(
            "topology:\n",
            "async_load_model:\n  save_cycle: 1\ntopology:\n",
        );
        let unknown_error = ScenarioConfig::from_yaml_str(&unknown_yaml)
            .expect_err("unknown scheduler model field must fail");
        assert!(unknown_error.to_string().contains("save_cycle"));
    }

    #[test]
    fn checked_in_scenarios_have_valid_explicit_remote_memory_models() {
        let scenario_root =
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../scenarios");
        for name in [
            "mvp_2host_single_domain.yaml",
            "mvp_3host_single_domain.yaml",
            "mvp_4host_single_domain.yaml",
            "mvp_8host_single_domain.yaml",
        ] {
            let config = ScenarioConfig::from_yaml_file(scenario_root.join(name))
                .unwrap_or_else(|error| panic!("{name} must parse: {error}"));
            assert!(config.remote_memory_model.enabled, "{name}");
            assert_eq!(
                config.remote_memory_model.time_source,
                super::RemoteMemoryTimeSource::QemuVirtual
            );
            assert!(
                config.remote_memory_model.queue_depth >= config.remote_memory_model.reorder_window
            );
            assert!(config.async_load_model.enabled, "{name}");
            assert_eq!(config.async_load_model.context_entries, 64, "{name}");
            assert_eq!(config.async_load_model.pending_load_entries, 64, "{name}");
            assert_eq!(config.async_load_model.event_queue_depth, 128, "{name}");
        }
    }
}
