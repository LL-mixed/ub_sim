//! Workload harness entry points.

use std::collections::HashSet;

use sim_config::{ScenarioConfig, WorkloadConfig};
use sim_core::{
    BlockHash, CompletionSource, CompletionStatus, HierarchyCoord, IoOpcode, IoSubmitReq,
    LogicalSystemId, PlLevel, SimError, SimEvent, TaskKey,
};
use sim_report::{CompletionSourceStats, CompletionStatusStats, EventSummary, WorkloadRunReport};
use sim_runtime::{
    InMemoryBlockStore, PromotionPlan, RecursiveRoutePlanner, RoutePlanner, RouteRequest,
    SimBlockStore,
};
use sim_services::block::BlockServiceProfile;
use sim_services::db::{DbGetReq, DbPutReq};
use sim_services::dfs::{DfsReadReq, DfsWriteReq};
use sim_topology::SimTopology;
use sim_uapi::{LocalGuestUapiSurface, UapiCommand, UapiResponse};
use sim_uapi::UapiDescriptor;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct RustLlmProfile {
    name: &'static str,
    requests_total_cap: u64,
    prefix_groups: u64,
    prefix_blocks: u64,
    tail_blocks: u64,
    tail_uses_dfs: bool,
    evict_after_request: usize,
}

pub fn run_minimal_workload(
    config: &ScenarioConfig,
    topology: &SimTopology,
) -> Result<WorkloadRunReport, sim_core::SimError> {
    let mut store = InMemoryBlockStore::from_config(config);
    let planner = RecursiveRoutePlanner::from_config(config);
    let mut seeded_dfs_paths = HashSet::new();

    let (workload_kind, workload_profile, requests_total, blocks_per_request, unique_prefixes) =
        match &config.workload {
        WorkloadConfig::HotsetLoop(cfg) => (
            "hotset_loop".to_string(),
            "default".to_string(),
            cfg.qps.min(4),
            cfg.blocks_per_request,
            cfg.unique_prefixes.max(1),
        ),
        WorkloadConfig::TraceReplay(_) => ("trace_replay".to_string(), "default".to_string(), 2, 1, 2),
        WorkloadConfig::RustLlmMvp(cfg) => {
            let profile = rust_llm_profile(&cfg.profile);
            (
                "rust_llm_server_mvp".to_string(),
                profile.name.to_string(),
                cfg.qps.min(profile.requests_total_cap),
                cfg.blocks_per_request.max((profile.prefix_blocks + profile.tail_blocks) as u32),
                cfg.unique_prefixes.max(profile.prefix_groups),
            )
        }
    };

    let rust_profile = match &config.workload {
        WorkloadConfig::RustLlmMvp(cfg) => Some(rust_llm_profile(&cfg.profile)),
        _ => None,
    };
    let allow_queue_retry = matches!(rust_profile, Some(profile) if profile.name == "capacity_pressure");
    let mut surface = if matches!(rust_profile, Some(profile) if profile.name == "capacity_pressure")
    {
        LocalGuestUapiSurface::with_block_profile(
            topology.clone(),
            BlockServiceProfile {
                queue_depth: 2,
                ..BlockServiceProfile::default()
            },
        )
    } else {
        LocalGuestUapiSurface::new(topology.clone())
    };
    let cq = match surface.execute(UapiCommand::RegisterCq { owner: 0 })? {
        UapiResponse::CqRegistered(cq) => cq,
        _ => return Err(sim_core::SimError::InvalidInput("unexpected cq registration response")),
    };
    let cmdq = match surface.execute(UapiCommand::CreateCmdQueue {
        cq,
        owner: 0,
        depth: 32,
    })? {
        UapiResponse::CmdQueueCreated(cmdq) => cmdq,
        _ => {
            return Err(sim_core::SimError::InvalidInput(
                "unexpected command queue creation response",
            ))
        }
    };
    let prefix_segment = match surface.execute(UapiCommand::CreateSegment { bytes: 4096 })? {
        UapiResponse::SegmentCreated(segment) => segment,
        _ => {
            return Err(sim_core::SimError::InvalidInput(
                "unexpected shmem segment creation response",
            ))
        }
    };
    let mut prefix_segment_seeded = false;
    let mut seeded_db_keys = HashSet::new();

    let mut report = WorkloadRunReport {
        workload_kind,
        workload_profile,
        requests_total,
        blocks_total: 0,
        hits: 0,
        misses: 0,
        prefix_hits: 0,
        tail_misses: 0,
        fallback_reads: 0,
        shmem_puts: 0,
        shmem_gets: 0,
        shmem_denied: 0,
        dfs_cold_reads: 0,
        dfs_warm_reads: 0,
        block_read_misses: 0,
        block_writes: 0,
        block_writebacks: 0,
        block_retryable_failures: 0,
        block_queue_rejections: 0,
        dfs_seed_writes: 0,
        db_puts: 0,
        db_gets: 0,
        db_retryable_failures: 0,
        promotions: 0,
        evictions: 0,
        completions: 0,
        summary: EventSummary {
            total_events: 0,
            tasks_created: 0,
            routes_planned: 0,
            blocks_promoted: 0,
            blocks_evicted: 0,
            dispatch_submitted: 0,
            completions_total: 0,
            runtime_retried: 0,
            runtime_failed: 0,
            faults_injected: 0,
            completions_by_source: CompletionSourceStats {
                chip_backend: 0,
                block_service: 0,
                shmem_service: 0,
                dfs_service: 0,
                db_service: 0,
                guest_uapi: 0,
                remote_node: 0,
            },
            completions_by_status: CompletionStatusStats {
                success: 0,
                retryable_failure: 0,
                fatal_failure: 0,
            },
        },
        events: Vec::new(),
    };

    for request_idx in 0..requests_total {
        let task = TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: request_idx + 1,
        };
        report.events.push(SimEvent::TaskCreated {
            at: request_idx,
            task: task.clone(),
        });

        for block_idx in 0..u64::from(blocks_per_request) {
            report.blocks_total += 1;
            let block = block_for_request(
                &report.workload_kind,
                rust_profile,
                request_idx,
                block_idx,
                unique_prefixes,
            );
            let uses_dfs_fallback = uses_dfs_fallback(rust_profile, block_idx);
            let is_prefix_block = is_prefix_block(rust_profile, block_idx);

            let lookup = store.lookup(&block);
            if lookup.found {
                report.hits += 1;
                if is_prefix_block {
                    report.prefix_hits += 1;
                    if report.workload_kind == "rust_llm_server_mvp" {
                        let db_key = format!("prefix-meta:{}", block.0);
                        if seeded_db_keys.insert(db_key.clone()) {
                            enqueue_descriptor_and_ring(
                                &mut surface,
                                cmdq,
                                UapiDescriptor::DbPut(DbPutReq {
                                    task: Some(task.clone()),
                                    key: db_key.clone(),
                                    bytes: 32,
                                }),
                                "unexpected db put enqueue response",
                                "unexpected db put doorbell response",
                            )?;
                            report.db_puts += 1;
                        }
                        enqueue_descriptor_and_ring(
                            &mut surface,
                            cmdq,
                            UapiDescriptor::DbGet(DbGetReq {
                                task: Some(task.clone()),
                                key: db_key,
                            }),
                            "unexpected db get enqueue response",
                            "unexpected db get doorbell response",
                        )?;
                        report.db_gets += 1;
                        if !prefix_segment_seeded {
                            enqueue_descriptor_and_ring(
                                &mut surface,
                                cmdq,
                                UapiDescriptor::ShmemPut(sim_services::shmem::ShmemPutReq {
                                    task: Some(task.clone()),
                                    requester_entity: 0,
                                    segment: prefix_segment,
                                    bytes: 4096,
                                }),
                                "unexpected shmem put enqueue response",
                                "unexpected shmem put doorbell response",
                            )?;
                            report.shmem_puts += 1;
                            prefix_segment_seeded = true;
                        }

                        enqueue_descriptor_and_ring(
                            &mut surface,
                            cmdq,
                            UapiDescriptor::ShmemGet(sim_services::shmem::ShmemGetReq {
                                task: Some(task.clone()),
                                requester_entity: 0,
                                segment: prefix_segment,
                                bytes: 4096,
                            }),
                            "unexpected shmem get enqueue response",
                            "unexpected shmem get doorbell response",
                        )?;
                        report.shmem_gets += 1;
                        drain_and_record(
                            &mut surface,
                            cq,
                            &mut report,
                            "unexpected shmem cq drain response",
                        )?;
                    }
                }
                continue;
            }

            report.misses += 1;
            if !is_prefix_block {
                report.tail_misses += 1;
            }
            let decision = planner.plan(
                RouteRequest {
                    task: task.clone(),
                    current_level: PlLevel::L4,
                    block: block.clone(),
                },
                topology,
            )?;
            report.events.push(SimEvent::RoutePlanned {
                at: request_idx + block_idx,
                task: task.clone(),
                decision,
            });

            store.stage_insert(PromotionPlan {
                block: block.clone(),
            })?;
            report.promotions += 1;

            if let Some(placement) = store.lookup(&block).placement {
                report.events.push(SimEvent::BlockPromoted {
                    at: request_idx + block_idx + 1,
                    block: block.clone(),
                    placement,
                });
            }

            if report.workload_kind == "rust_llm_server_mvp" && uses_dfs_fallback {
                let dfs_path = format!("/weights/{}", block.0);
                let cold_read = seeded_dfs_paths.insert(dfs_path.clone());
                if cold_read {
                    enqueue_descriptor_and_ring(
                        &mut surface,
                        cmdq,
                        UapiDescriptor::DfsWrite(DfsWriteReq {
                            task: Some(task.clone()),
                            path: dfs_path.clone(),
                            bytes: 4096,
                        }),
                        "unexpected dfs write enqueue response",
                        "unexpected dfs write doorbell response",
                    )?;
                    report.dfs_seed_writes += 1;
                }

                submit_block_read(
                    &mut surface,
                    cq,
                    &mut report,
                    task.clone(),
                    block.clone(),
                    allow_queue_retry,
                )?;
                report.fallback_reads += 1;
                if cold_read {
                    report.dfs_cold_reads += 1;
                } else {
                    report.dfs_warm_reads += 1;
                }
                enqueue_descriptor_and_ring(
                    &mut surface,
                    cmdq,
                    UapiDescriptor::DfsRead(DfsReadReq {
                        task: Some(task.clone()),
                        path: dfs_path,
                    }),
                    "unexpected dfs read enqueue response",
                    "unexpected dfs read doorbell response",
                )?;
            } else if report.workload_kind == "rust_llm_server_mvp" {
                submit_block_read(
                    &mut surface,
                    cq,
                    &mut report,
                    task.clone(),
                    block.clone(),
                    allow_queue_retry,
                )?;
            }

            submit_block_write(
                &mut surface,
                cq,
                &mut report,
                task.clone(),
                block,
                allow_queue_retry,
            )?;
            if !matches!(rust_profile, Some(profile) if profile.name == "capacity_pressure") {
                drain_and_record(&mut surface, cq, &mut report, "unexpected cq drain response")?;
            }
        }

        if let Some(profile) = rust_profile {
            if profile.evict_after_request > 0 {
                let evicted = store.evict(sim_runtime::EvictionPlan {
                    max_blocks: profile.evict_after_request,
                })?;
                report.evictions += evicted.len() as u64;
                for block in evicted {
                    submit_block_writeback(
                        &mut surface,
                        cq,
                        &mut report,
                        Some(task.clone()),
                        block.clone(),
                    )?;
                    report.events.push(SimEvent::BlockEvicted {
                        at: request_idx + report.blocks_total,
                        from: sim_core::BlockPlacement {
                            block: block.clone(),
                            level: PlLevel::L2,
                            node: 0,
                        },
                        block,
                    });
                }
            }
        }

        drain_and_record(
            &mut surface,
            cq,
            &mut report,
            "unexpected cq drain response after request",
        )?;
    }

    let evicted = store.evict(sim_runtime::EvictionPlan { max_blocks: 1 })?;
    report.evictions += evicted.len() as u64;
    for block in evicted {
        submit_block_writeback(&mut surface, cq, &mut report, None, block.clone())?;
        report.events.push(SimEvent::BlockEvicted {
            at: requests_total + report.blocks_total,
            from: sim_core::BlockPlacement {
                block: block.clone(),
                level: PlLevel::L2,
                node: 0,
            },
            block,
        });
    }
    drain_and_record(
        &mut surface,
        cq,
        &mut report,
        "unexpected final cq drain response",
    )?;

    report.summary = summarize_events(&report.events);
    Ok(report)
}

fn submit_block_read(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: TaskKey,
    block: BlockHash,
    allow_retry_after_queue_full: bool,
) -> Result<(), SimError> {
    submit_block_io(
        surface,
        cq,
        report,
        IoSubmitReq {
            op_id: 10_000 + report.blocks_total,
            task: Some(task),
            entity: 0,
            opcode: IoOpcode::ReadBlock,
            segment: None,
            block: Some(block),
        },
        allow_retry_after_queue_full,
    )
}

fn enqueue_descriptor_and_ring(
    surface: &mut LocalGuestUapiSurface,
    cmdq: sim_core::CmdQueueHandle,
    desc: UapiDescriptor,
    enqueue_err: &'static str,
    doorbell_err: &'static str,
) -> Result<(), SimError> {
    match surface.execute(UapiCommand::EnqueueCmd {
        cmdq,
        owner: 0,
        desc,
    })? {
        UapiResponse::CommandEnqueued { .. } => {}
        _ => return Err(SimError::InvalidInput(enqueue_err)),
    }

    match surface.execute(UapiCommand::RingDoorbell {
        cmdq,
        owner: 0,
        max_batch: Some(1),
    })? {
        UapiResponse::DoorbellRung { submitted: 1, .. } => Ok(()),
        _ => Err(SimError::InvalidInput(doorbell_err)),
    }
}

fn submit_block_write(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: TaskKey,
    block: BlockHash,
    allow_retry_after_queue_full: bool,
) -> Result<(), SimError> {
    report.block_writes += 1;
    submit_block_io(
        surface,
        cq,
        report,
        IoSubmitReq {
            op_id: 20_000 + report.blocks_total,
            task: Some(task),
            entity: 0,
            opcode: IoOpcode::WriteBlock,
            segment: None,
            block: Some(block),
        },
        allow_retry_after_queue_full,
    )
}

fn submit_block_io(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    req: IoSubmitReq,
    allow_retry_after_queue_full: bool,
) -> Result<(), SimError> {
    match surface.execute(UapiCommand::SubmitIo { req: req.clone() }) {
        Ok(UapiResponse::IoSubmitted(_)) => Ok(()),
        Ok(_) => Err(SimError::InvalidInput("unexpected io submit response")),
        Err(SimError::InvalidInput("block queue full")) if allow_retry_after_queue_full => {
            report.block_queue_rejections += 1;
            drain_and_record(
                surface,
                cq,
                report,
                "unexpected cq drain response while clearing block queue pressure",
            )?;
            match surface.execute(UapiCommand::SubmitIo { req })? {
                UapiResponse::IoSubmitted(_) => Ok(()),
                _ => Err(SimError::InvalidInput("unexpected io submit retry response")),
            }
        }
        Err(err) => Err(err),
    }
}

fn submit_block_writeback(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    task: Option<TaskKey>,
    block: BlockHash,
) -> Result<(), SimError> {
    report.block_writebacks += 1;
    match surface.execute(UapiCommand::SubmitBlockWriteback {
        block: block.clone(),
        task: task.clone(),
    }) {
        Ok(UapiResponse::IoSubmitted(_)) => Ok(()),
        Ok(_) => Err(SimError::InvalidInput("unexpected block writeback response")),
        Err(SimError::InvalidInput("block queue full")) => {
            report.block_queue_rejections += 1;
            drain_and_record(
                surface,
                cq,
                report,
                "unexpected cq drain response while clearing writeback queue pressure",
            )?;
            match surface.execute(UapiCommand::SubmitBlockWriteback {
                block,
                task,
            })? {
                UapiResponse::IoSubmitted(_) => Ok(()),
                _ => Err(SimError::InvalidInput("unexpected block writeback retry response")),
            }
        }
        Err(err) => Err(err),
    }
}

fn drain_and_record(
    surface: &mut LocalGuestUapiSurface,
    cq: sim_core::CqHandle,
    report: &mut WorkloadRunReport,
    response_err: &'static str,
) -> Result<(), SimError> {
    let completions = match surface.execute(UapiCommand::DrainCq { cq, owner: 0 })? {
        UapiResponse::Completions { events, .. } => events,
        _ => return Err(SimError::InvalidInput(response_err)),
    };

    report.completions += completions.len() as u64;
    for completion in completions {
        match completion.source {
            CompletionSource::BlockService => match &completion.status {
                CompletionStatus::RetryableFailure { code } => {
                    report.block_retryable_failures += 1;
                    if code == "block_miss" {
                        report.block_read_misses += 1;
                    }
                }
                CompletionStatus::FatalFailure { .. } | CompletionStatus::Success => {}
            },
            CompletionSource::ShmemService => match &completion.status {
                CompletionStatus::FatalFailure { code } if code == "shmem_access_denied" => {
                    report.shmem_denied += 1;
                }
                CompletionStatus::RetryableFailure { .. }
                | CompletionStatus::FatalFailure { .. }
                | CompletionStatus::Success => {}
            },
            CompletionSource::DbService => {
                if matches!(&completion.status, CompletionStatus::RetryableFailure { .. }) {
                    report.db_retryable_failures += 1;
                }
            }
            CompletionSource::ChipBackend
            | CompletionSource::DfsService
            | CompletionSource::GuestUapi
            | CompletionSource::RemoteNode => {}
        }
        report.events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    Ok(())
}

fn block_for_request(
    workload_kind: &str,
    profile: Option<RustLlmProfile>,
    request_idx: u64,
    block_idx: u64,
    unique_prefixes: u64,
) -> BlockHash {
    let stable_prefixes = unique_prefixes.max(1).min(8);
    let prefix_group = request_idx % stable_prefixes.min(profile.map(|p| p.prefix_groups).unwrap_or(2).max(1));

    match workload_kind {
        "rust_llm_server_mvp" => {
            if is_prefix_block(profile, block_idx) {
                BlockHash(format!("prefix-{prefix_group}-block-{block_idx}"))
            } else {
                BlockHash(format!("tail-req-{request_idx}-block-{block_idx}"))
            }
        }
        "hotset_loop" => BlockHash(format!(
            "hotset-prefix-{}",
            (request_idx + block_idx) % stable_prefixes
        )),
        _ => BlockHash(format!("trace-block-{request_idx}-{block_idx}")),
    }
}

fn rust_llm_profile(profile: &str) -> RustLlmProfile {
    match profile {
        "high_reuse" => RustLlmProfile {
            name: "high_reuse",
            requests_total_cap: 6,
            prefix_groups: 1,
            prefix_blocks: 3,
            tail_blocks: 1,
            tail_uses_dfs: false,
            evict_after_request: 0,
        },
        "capacity_pressure" => RustLlmProfile {
            name: "capacity_pressure",
            requests_total_cap: 6,
            prefix_groups: 2,
            prefix_blocks: 1,
            tail_blocks: 3,
            tail_uses_dfs: true,
            evict_after_request: 2,
        },
        "dfs_heavy_fallback" => RustLlmProfile {
            name: "dfs_heavy_fallback",
            requests_total_cap: 5,
            prefix_groups: 2,
            prefix_blocks: 1,
            tail_blocks: 4,
            tail_uses_dfs: true,
            evict_after_request: 1,
        },
        _ => RustLlmProfile {
            name: "single_domain_basic",
            requests_total_cap: 4,
            prefix_groups: 2,
            prefix_blocks: 2,
            tail_blocks: 2,
            tail_uses_dfs: true,
            evict_after_request: 0,
        },
    }
}

fn is_prefix_block(profile: Option<RustLlmProfile>, block_idx: u64) -> bool {
    profile
        .map(|profile| block_idx < profile.prefix_blocks)
        .unwrap_or(false)
}

fn uses_dfs_fallback(profile: Option<RustLlmProfile>, block_idx: u64) -> bool {
    profile
        .map(|profile| profile.tail_uses_dfs && block_idx >= profile.prefix_blocks)
        .unwrap_or(false)
}

fn summarize_events(events: &[SimEvent]) -> EventSummary {
    let mut summary = EventSummary {
        total_events: events.len() as u64,
        tasks_created: 0,
        routes_planned: 0,
        blocks_promoted: 0,
        blocks_evicted: 0,
        dispatch_submitted: 0,
        completions_total: 0,
        runtime_retried: 0,
        runtime_failed: 0,
        faults_injected: 0,
        completions_by_source: CompletionSourceStats {
            chip_backend: 0,
            block_service: 0,
            shmem_service: 0,
            dfs_service: 0,
            db_service: 0,
            guest_uapi: 0,
            remote_node: 0,
        },
        completions_by_status: CompletionStatusStats {
            success: 0,
            retryable_failure: 0,
            fatal_failure: 0,
        },
    };

    for event in events {
        match event {
            SimEvent::TaskCreated { .. } => summary.tasks_created += 1,
            SimEvent::RoutePlanned { .. } => summary.routes_planned += 1,
            SimEvent::BlockPromoted { .. } => summary.blocks_promoted += 1,
            SimEvent::BlockEvicted { .. } => summary.blocks_evicted += 1,
            SimEvent::DispatchSubmitted { .. } => summary.dispatch_submitted += 1,
            SimEvent::CompletionObserved { completion, .. } => {
                summary.completions_total += 1;
                match completion.source {
                    CompletionSource::ChipBackend => summary.completions_by_source.chip_backend += 1,
                    CompletionSource::BlockService => summary.completions_by_source.block_service += 1,
                    CompletionSource::ShmemService => summary.completions_by_source.shmem_service += 1,
                    CompletionSource::DfsService => summary.completions_by_source.dfs_service += 1,
                    CompletionSource::DbService => summary.completions_by_source.db_service += 1,
                    CompletionSource::GuestUapi => summary.completions_by_source.guest_uapi += 1,
                    CompletionSource::RemoteNode => summary.completions_by_source.remote_node += 1,
                }
                match &completion.status {
                    CompletionStatus::Success => summary.completions_by_status.success += 1,
                    CompletionStatus::RetryableFailure { .. } => {
                        summary.completions_by_status.retryable_failure += 1
                    }
                    CompletionStatus::FatalFailure { .. } => {
                        summary.completions_by_status.fatal_failure += 1
                    }
                }
            }
            SimEvent::RuntimeRetried { .. } => summary.runtime_retried += 1,
            SimEvent::RuntimeFailed { .. } => summary.runtime_failed += 1,
            SimEvent::FaultInjected { .. } => summary.faults_injected += 1,
        }
    }

    summary
}

#[cfg(test)]
mod tests {
    use super::{block_for_request, run_minimal_workload, rust_llm_profile};
    use sim_config::ScenarioConfig;
    use sim_core::CompletionSource;
    use sim_topology::SimTopology;

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
  qps: 3
  unique_prefixes: 2
  blocks_per_request: 4
  function_label_mode: host_orchestration
faults: []
outputs:
  trace: true
  metrics_csv: true
  summary_json: true
  emit_task_coord_trace: true
  emit_data_service_trace: true
  emit_qemu_platform_trace: true
"#;

    #[test]
    fn minimal_workload_runs_and_emits_events() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let report = run_minimal_workload(&config, &topology).expect("workload");

        assert_eq!(report.requests_total, 3);
        assert_eq!(report.workload_profile, "single_domain_basic");
        assert_eq!(report.blocks_total, 12);
        assert!(report.promotions > 0);
        assert!(report.completions > 0);
        assert!(report.tail_misses > 0);
        assert!(report.fallback_reads > 0);
        assert!(report.summary.completions_by_source.shmem_service > 0);
        assert!(!report.events.is_empty());
        assert!(report.events.iter().any(|event| matches!(
            event,
            sim_core::SimEvent::CompletionObserved {
                completion: sim_core::CompletionEvent {
                    source: CompletionSource::DfsService,
                    ..
                },
                ..
            }
        )));
    }

    #[test]
    fn rust_llm_workload_reuses_prefix_blocks() {
        use sim_core::BlockHash;
        let profile = Some(rust_llm_profile("single_domain_basic"));

        assert_eq!(
            block_for_request("rust_llm_server_mvp", profile, 0, 0, 16),
            BlockHash("prefix-0-block-0".into())
        );
        assert_eq!(
            block_for_request("rust_llm_server_mvp", profile, 0, 1, 16),
            BlockHash("prefix-0-block-1".into())
        );
        assert_eq!(
            block_for_request("rust_llm_server_mvp", profile, 0, 3, 16),
            BlockHash("tail-req-0-block-3".into())
        );
    }

    #[test]
    fn high_reuse_profile_increases_prefix_hits() {
        let config = ScenarioConfig::from_yaml_str(&VALID_YAML.replace(
            "profile: single_domain_basic",
            "profile: high_reuse",
        ))
        .expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let report = run_minimal_workload(&config, &topology).expect("workload");

        assert_eq!(report.workload_profile, "high_reuse");
        assert!(report.prefix_hits > 0);
        assert_eq!(report.fallback_reads, 0);
    }

    #[test]
    fn dfs_heavy_profile_increases_fallback_reads() {
        let config = ScenarioConfig::from_yaml_str(&VALID_YAML.replace(
            "profile: single_domain_basic",
            "profile: dfs_heavy_fallback",
        ))
        .expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let report = run_minimal_workload(&config, &topology).expect("workload");

        assert_eq!(report.workload_profile, "dfs_heavy_fallback");
        assert!(report.fallback_reads >= report.requests_total);
        assert!(report.summary.completions_by_source.dfs_service > 0);
    }

    #[test]
    fn capacity_pressure_profile_forces_extra_evictions() {
        let config = ScenarioConfig::from_yaml_str(&VALID_YAML.replace(
            "profile: single_domain_basic",
            "profile: capacity_pressure",
        ))
        .expect("config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let report = run_minimal_workload(&config, &topology).expect("workload");

        assert_eq!(report.workload_profile, "capacity_pressure");
        assert!(report.evictions >= report.requests_total - 1);
        assert!(report.block_writebacks > 0);
        assert!(report.block_queue_rejections > 0);
    }
}
