//! Serializable report schema for simulator outputs.

use serde::{Deserialize, Serialize};
use sim_core::{
    CmdQueueHandle, CqHandle, DecoderKind, HealthStatus, PlLevel, RouteScope, SegmentHandle,
    SimEvent,
};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HostReport {
    pub id: u32,
    pub node_id: u64,
    pub health: HealthStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UbpuReport {
    pub id: u32,
    pub node_id: u64,
    pub host_id: u32,
    pub health: HealthStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EntityReport {
    pub id: u32,
    pub eid: u32,
    pub ubpu_id: u32,
    pub ubc_id: u32,
    pub health: HealthStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UbcReport {
    pub id: u32,
    pub node_id: u64,
    pub ubpu_id: u32,
    pub host_id: u32,
    pub health: HealthStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UmmuReport {
    pub id: u32,
    pub node_id: u64,
    pub ubc_id: u32,
    pub domain_id: u32,
    pub health: HealthStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DecoderReport {
    pub id: u32,
    pub node_id: u64,
    pub ubc_id: u32,
    pub kind: DecoderKind,
    pub health: HealthStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DomainReport {
    pub id: u32,
    pub label: String,
    pub node_id: u64,
    pub hosts: Vec<u32>,
    pub health: HealthStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RouteReport {
    pub id: u32,
    pub scope: RouteScope,
    pub from_node: u64,
    pub to_node: u64,
    pub level: PlLevel,
    pub domain_id: u32,
    pub health: HealthStatus,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TopologyReport {
    pub hosts_count: usize,
    pub ubpus_count: usize,
    pub ubcs_count: usize,
    pub ummus_count: usize,
    pub entities_count: usize,
    pub decoders_count: usize,
    pub domains_count: usize,
    pub routes_count: usize,
    pub hosts: Vec<HostReport>,
    pub ubpus: Vec<UbpuReport>,
    pub ubcs: Vec<UbcReport>,
    pub ummus: Vec<UmmuReport>,
    pub entities: Vec<EntityReport>,
    pub decoders: Vec<DecoderReport>,
    pub domains: Vec<DomainReport>,
    pub routes: Vec<RouteReport>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CompletionSourceStats {
    pub chip_backend: u64,
    pub block_service: u64,
    pub shmem_service: u64,
    pub dfs_service: u64,
    pub db_service: u64,
    pub guest_uapi: u64,
    #[serde(default)]
    pub remote_node: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CompletionStatusStats {
    pub success: u64,
    pub retryable_failure: u64,
    pub fatal_failure: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EventSummary {
    pub total_events: u64,
    pub tasks_created: u64,
    pub routes_planned: u64,
    pub blocks_promoted: u64,
    pub blocks_evicted: u64,
    pub dispatch_submitted: u64,
    pub completions_total: u64,
    pub runtime_retried: u64,
    pub runtime_failed: u64,
    pub faults_injected: u64,
    pub completions_by_source: CompletionSourceStats,
    pub completions_by_status: CompletionStatusStats,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UapiDemoReport {
    pub hosts_count: usize,
    pub ubpus_count: usize,
    pub entities_count: usize,
    pub domains_count: usize,
    pub cq: CqHandle,
    pub cmdq: CmdQueueHandle,
    pub cmdq_depth: usize,
    pub segment: SegmentHandle,
    pub health: HealthStatus,
    pub cmdq_pending_after_partial_ring: usize,
    pub cq_remaining_after_partial_poll: usize,
    pub summary: EventSummary,
    pub events: Vec<SimEvent>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct QemuBackendDemoReport {
    pub hosts_count: usize,
    pub ubpus_count: usize,
    pub entities_count: usize,
    pub domains_count: usize,
    pub endpoint_id: u16,
    pub cmdq_depth: usize,
    pub cmdq_head_after_ring: usize,
    pub cmdq_tail_after_submit: usize,
    pub cq_head_after_partial_poll: usize,
    pub cq_tail_after_ring: usize,
    pub irq_status_after_ring: u64,
    pub irq_status_after_ack: u64,
    pub segment: SegmentHandle,
    pub health: HealthStatus,
    pub pending_after_ring: usize,
    pub cq_remaining_after_partial_poll: usize,
    pub summary: EventSummary,
    pub events: Vec<SimEvent>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WorkloadRunReport {
    pub workload_kind: String,
    pub workload_profile: String,
    pub requests_total: u64,
    pub blocks_total: u64,
    pub hits: u64,
    pub misses: u64,
    pub prefix_hits: u64,
    pub tail_misses: u64,
    pub fallback_reads: u64,
    pub shmem_puts: u64,
    pub shmem_gets: u64,
    pub shmem_denied: u64,
    pub dfs_cold_reads: u64,
    pub dfs_warm_reads: u64,
    pub block_read_misses: u64,
    pub block_writes: u64,
    pub block_writebacks: u64,
    pub block_retryable_failures: u64,
    pub block_queue_rejections: u64,
    pub dfs_seed_writes: u64,
    pub db_puts: u64,
    pub db_gets: u64,
    pub db_retryable_failures: u64,
    pub promotions: u64,
    pub evictions: u64,
    pub completions: u64,
    pub summary: EventSummary,
    pub events: Vec<SimEvent>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AuxiliaryDebugReport {
    pub runtime_summary: EventSummary,
    pub runtime_events: Vec<SimEvent>,
    pub uapi_report: UapiDemoReport,
    pub qemu_backend_report: QemuBackendDemoReport,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CliReport {
    pub scenario_name: String,
    pub group: Option<String>,
    pub variant: Option<String>,
    pub logical_system: String,
    pub scenario_file: String,
    pub topology: TopologyReport,
    pub workload_report: WorkloadRunReport,
    pub auxiliary: Option<AuxiliaryDebugReport>,
}
