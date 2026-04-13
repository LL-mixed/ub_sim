//! Topology objects for the simulator.

use sim_config::{ConfigError, ScenarioConfig};
use sim_core::{
    DecoderId, DecoderKind, DomainId, Eid, EntityId, HealthStatus, HostId, NodeId, PlLevel,
    RouteBinding, RouteId, RouteScope, UbcId, UbpuId, UmmuId,
};

#[derive(Debug, Clone)]
pub struct SimHost {
    pub id: HostId,
    pub node_id: NodeId,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimUbpu {
    pub id: UbpuId,
    pub node_id: NodeId,
    pub host_id: HostId,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimUbc {
    pub id: UbcId,
    pub node_id: NodeId,
    pub ubpu_id: UbpuId,
    pub host_id: HostId,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimUmmu {
    pub id: UmmuId,
    pub node_id: NodeId,
    pub ubc_id: UbcId,
    pub domain_id: DomainId,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimEntity {
    pub id: EntityId,
    pub eid: Eid,
    pub ubpu_id: UbpuId,
    pub ubc_id: UbcId,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimDecoder {
    pub id: DecoderId,
    pub node_id: NodeId,
    pub ubc_id: UbcId,
    pub kind: DecoderKind,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimDomain {
    pub id: DomainId,
    pub node_id: NodeId,
    pub label: String,
    pub hosts: Vec<HostId>,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct SimRoute {
    pub binding: RouteBinding,
    pub domain_id: DomainId,
    pub health: HealthStatus,
}

#[derive(Debug, Clone)]
pub struct TopologySnapshot {
    pub hosts: usize,
    pub ubpus: usize,
    pub ubcs: usize,
    pub ummus: usize,
    pub entities: usize,
    pub decoders: usize,
    pub domains: usize,
    pub routes: usize,
}

#[derive(Debug, Clone)]
pub struct SimTopology {
    pub hosts: Vec<SimHost>,
    pub ubpus: Vec<SimUbpu>,
    pub ubcs: Vec<SimUbc>,
    pub ummus: Vec<SimUmmu>,
    pub entities: Vec<SimEntity>,
    pub decoders: Vec<SimDecoder>,
    pub domains: Vec<SimDomain>,
    pub routes: Vec<SimRoute>,
}

impl SimTopology {
    pub fn from_config(config: &ScenarioConfig) -> Result<Self, ConfigError> {
        config.validate()?;

        let mut next_node_id: NodeId = 1;
        let mut hosts = Vec::with_capacity(config.topology.hosts as usize);
        let mut ubpus = Vec::with_capacity((config.topology.hosts * config.topology.ubpus_per_host) as usize);
        let mut ubcs = Vec::with_capacity((config.topology.hosts * config.topology.ubpus_per_host) as usize);
        let mut ummus = Vec::with_capacity(config.topology.ub_domains.len());
        let mut entities = Vec::with_capacity(
            (config.topology.hosts * config.topology.ubpus_per_host * config.topology.entities_per_ubpu)
                as usize,
        );
        let mut decoders = Vec::with_capacity((config.topology.hosts * config.topology.ubpus_per_host * 2) as usize);
        let mut domains = Vec::with_capacity(config.topology.ub_domains.len());
        let mut routes = Vec::new();

        for host_idx in 0..config.topology.hosts {
            hosts.push(SimHost {
                id: host_idx,
                node_id: next_node_id,
                health: HealthStatus::Healthy,
            });
            next_node_id += 1;
        }

        let mut next_ubpu_id: UbpuId = 0;
        let mut next_ubc_id: UbcId = 0;
        let mut next_ummu_id: UmmuId = 0;
        let mut next_entity_id: EntityId = 0;
        let mut next_eid: Eid = 1;
        let mut next_decoder_id: DecoderId = 0;
        let mut next_route_id: RouteId = 0;

        for host_idx in 0..config.topology.hosts {
            for _ in 0..config.topology.ubpus_per_host {
                let ubpu_id = next_ubpu_id;
                ubpus.push(SimUbpu {
                    id: ubpu_id,
                    node_id: next_node_id,
                    host_id: host_idx,
                    health: HealthStatus::Healthy,
                });
                next_node_id += 1;
                next_ubpu_id += 1;

                let ubc_id = next_ubc_id;
                ubcs.push(SimUbc {
                    id: ubc_id,
                    node_id: next_node_id,
                    ubpu_id,
                    host_id: host_idx,
                    health: HealthStatus::Healthy,
                });
                next_node_id += 1;
                next_ubc_id += 1;

                decoders.push(SimDecoder {
                    id: next_decoder_id,
                    node_id: next_node_id,
                    ubc_id,
                    kind: DecoderKind::PlToNode,
                    health: HealthStatus::Healthy,
                });
                next_node_id += 1;
                next_decoder_id += 1;

                decoders.push(SimDecoder {
                    id: next_decoder_id,
                    node_id: next_node_id,
                    ubc_id,
                    kind: DecoderKind::EidToEntity,
                    health: HealthStatus::Healthy,
                });
                next_node_id += 1;
                next_decoder_id += 1;

                for _ in 0..config.topology.entities_per_ubpu {
                    entities.push(SimEntity {
                        id: next_entity_id,
                        eid: next_eid,
                        ubpu_id,
                        ubc_id,
                        health: HealthStatus::Healthy,
                    });
                    next_entity_id += 1;
                    next_eid += 1;
                }
            }
        }

        for (domain_idx, domain_cfg) in config.topology.ub_domains.iter().enumerate() {
            let domain_id = domain_idx as DomainId;
            domains.push(SimDomain {
                id: domain_id,
                node_id: next_node_id,
                label: domain_cfg.id.clone(),
                hosts: domain_cfg.hosts.clone(),
                health: HealthStatus::Healthy,
            });
            next_node_id += 1;

            ummus.push(SimUmmu {
                id: next_ummu_id,
                node_id: next_node_id,
                ubc_id: domain_cfg.hosts[0] * config.topology.ubpus_per_host,
                domain_id,
                health: HealthStatus::Healthy,
            });
            next_node_id += 1;
            next_ummu_id += 1;

            for &host_id in &domain_cfg.hosts {
                for ubpu_index in 0..config.topology.ubpus_per_host {
                    let ubpu_id = host_id * config.topology.ubpus_per_host + ubpu_index;
                    let ubc_id = ubpu_id;
                    routes.push(SimRoute {
                        binding: RouteBinding {
                            id: next_route_id,
                            scope: if domain_cfg.hosts.len() == 1 {
                                RouteScope::HostLocal
                            } else {
                                RouteScope::DomainShared
                            },
                            from_node: host_id as NodeId + 1,
                            to_node: domains[domain_idx].node_id,
                            level: PlLevel::L4,
                        },
                        domain_id,
                        health: HealthStatus::Healthy,
                    });
                    next_route_id += 1;

                    routes.push(SimRoute {
                        binding: RouteBinding {
                            id: next_route_id,
                            scope: RouteScope::UbLocal,
                            from_node: ubpus[ubpu_id as usize].node_id,
                            to_node: ubcs[ubc_id as usize].node_id,
                            level: PlLevel::L2,
                        },
                        domain_id,
                        health: HealthStatus::Healthy,
                    });
                    next_route_id += 1;
                }
            }
        }

        Ok(Self {
            hosts,
            ubpus,
            ubcs,
            ummus,
            entities,
            decoders,
            domains,
            routes,
        })
    }

    pub fn snapshot(&self) -> TopologySnapshot {
        TopologySnapshot {
            hosts: self.hosts.len(),
            ubpus: self.ubpus.len(),
            ubcs: self.ubcs.len(),
            ummus: self.ummus.len(),
            entities: self.entities.len(),
            decoders: self.decoders.len(),
            domains: self.domains.len(),
            routes: self.routes.len(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::SimTopology;
    use sim_config::ScenarioConfig;

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
    fn builds_expected_topology_from_valid_config() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology build");
        let snapshot = topology.snapshot();

        assert_eq!(snapshot.hosts, 2);
        assert_eq!(snapshot.ubpus, 4);
        assert_eq!(snapshot.ubcs, 4);
        assert_eq!(snapshot.ummus, 1);
        assert_eq!(snapshot.entities, 8);
        assert_eq!(snapshot.decoders, 8);
        assert_eq!(snapshot.domains, 1);
        assert_eq!(snapshot.routes, 8);
    }
}
