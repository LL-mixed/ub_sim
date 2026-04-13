use anyhow::Context;
use sim_core::{
    BlockHash, CompletionSource, CompletionStatus, CopyDirection, CopyRequest, DispatchRequest,
    FunctionLabel, HierarchyCoord, IoOpcode, IoSubmitReq, LogicalSystemId, MemoryEndpoint,
    PlLevel, SegmentHandle, SimEvent, TaskKey,
};
use sim_config::ScenarioConfig;
use sim_report::{
    AuxiliaryDebugReport, CliReport, CompletionSourceStats, CompletionStatusStats, DecoderReport,
    DomainReport, EntityReport, EventSummary, HostReport, QemuBackendDemoReport, RouteReport,
    TopologyReport, UapiDemoReport, UbcReport, UbpuReport, UmmuReport,
};
use sim_qemu::{
    GuestDescriptor, GuestIoDescriptor, GuestServiceDescriptor, LinquDeviceModel, QemuMmioHandler,
};
use sim_runtime::{
    EventSink, EvictionPlan, InMemoryBlockStore, LocalRuntimeEngine, PromotionPlan,
    RecursiveRoutePlanner, RoutePlanner, RouteRequest, SimBlockStore, VecEventSink,
};
use sim_services::{
    db::{DbGetReq, DbPutReq},
    dfs::{DfsReadReq, DfsWriteReq},
    shmem::{ShmemGetReq, ShmemPutReq},
};
use sim_topology::SimTopology;
use sim_uapi::{LocalGuestUapiSurface, UapiCommand, UapiDescriptor, UapiResponse};
use sim_workloads::run_minimal_workload;
use std::env;
use std::path::{Path, PathBuf};

fn main() -> anyhow::Result<()> {
    let scenario_path = scenario_path_from_args();
    let config = ScenarioConfig::from_yaml_file(&scenario_path).with_context(|| {
        format!(
            "failed to load scenario config from {}",
            scenario_path.display()
        )
    })?;
    let topology = SimTopology::from_config(&config).context("failed to build topology")?;
    let mut workload_report =
        run_minimal_workload(&config, &topology).context("failed to run minimal workload")?;
    workload_report.summary = summarize_events(&workload_report.events);
    let auxiliary = if include_auxiliary_debug() {
        let runtime_events =
            run_demo(&config, &topology).context("failed to run route/store/event demo")?;
        let uapi_report = run_uapi_demo(&topology).context("failed to run local uapi demo")?;
        let qemu_backend_report =
            run_qemu_backend_demo(&topology).context("failed to run qemu backend demo")?;
        Some(AuxiliaryDebugReport {
            runtime_summary: summarize_events(&runtime_events),
            runtime_events,
            uapi_report,
            qemu_backend_report,
        })
    } else {
        None
    };

    let report = CliReport {
        scenario_name: config.scenario.name,
        group: config.scenario.group,
        variant: config.scenario.variant,
        logical_system: config.scenario.logical_system,
        scenario_file: scenario_path.display().to_string(),
        topology: topology_report(&topology),
        workload_report,
        auxiliary,
    };

    print_report(&report);

    Ok(())
}

fn scenario_path_from_args() -> PathBuf {
    env::args_os()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(default_scenario_path)
}

fn default_scenario_path() -> PathBuf {
    Path::new("scenarios").join("mvp_2host_single_domain.yaml")
}

fn include_auxiliary_debug() -> bool {
    matches!(env::var("SIM_CLI_INCLUDE_AUX").as_deref(), Ok("1"))
}

fn run_demo(config: &ScenarioConfig, topology: &SimTopology) -> anyhow::Result<Vec<SimEvent>> {
    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let block = BlockHash("demo-block-0".to_string());

    let planner = RecursiveRoutePlanner::from_config(config);
    let mut store = InMemoryBlockStore::from_config(config);
    let mut sink = VecEventSink::default();

    sink.emit(SimEvent::TaskCreated {
        at: 0,
        task: task.clone(),
    });

    let decision = planner
        .plan(
            RouteRequest {
                task: task.clone(),
                current_level: PlLevel::L4,
                block: block.clone(),
            },
            topology,
        )
        .context("route planning failed")?;

    sink.emit(SimEvent::RoutePlanned {
        at: 1,
        task: task.clone(),
        decision: decision.clone(),
    });

    store
        .stage_insert(PromotionPlan {
            block: block.clone(),
        })
        .context("block insert failed")?;

    let placement = store
        .lookup(&block)
        .placement
        .context("block placement missing after insert")?;

    sink.emit(SimEvent::BlockPromoted {
        at: 2,
        block: block.clone(),
        placement,
    });

    let evicted = store
        .evict(EvictionPlan { max_blocks: 1 })
        .context("block eviction failed")?;

    if let Some(evicted_block) = evicted.into_iter().next() {
        sink.emit(SimEvent::BlockEvicted {
            at: 3,
            block: evicted_block,
            from: sim_core::BlockPlacement {
                block: block.clone(),
                level: PlLevel::L2,
                node: 0,
            },
        });
    }

    let mut success_runtime = LocalRuntimeEngine::from_config(config);
    success_runtime
        .submit_dispatch(
            DispatchRequest {
                task: task.clone(),
                function: FunctionLabel {
                    name: "runtime_demo_dispatch".into(),
                    level: PlLevel::L4,
                },
                target_level: PlLevel::L4,
                target_node: decision.selected_node,
                input_segments: vec![SegmentHandle(1)],
            },
            &mut sink,
        )
        .context("runtime dispatch submit failed")?;
    success_runtime
        .submit_copy(CopyRequest {
            task: task.clone(),
            direction: CopyDirection::HostToDevice,
            bytes: 4096,
            src: MemoryEndpoint {
                node: topology.hosts[0].node_id,
                segment: SegmentHandle(1),
                offset: 0,
            },
            dst: MemoryEndpoint {
                node: topology.ubpus[0].node_id,
                segment: SegmentHandle(2),
                offset: 0,
            },
        })
        .context("runtime copy submit failed")?;
    let _ = success_runtime.poll_completions(30, &mut sink);

    let mut retry_runtime = LocalRuntimeEngine::with_policy(15, 30, 5, 4, 1);
    retry_runtime
        .submit_dispatch(
            DispatchRequest {
                task: task.clone(),
                function: FunctionLabel {
                    name: "runtime_demo_timeout".into(),
                    level: PlLevel::L4,
                },
                target_level: PlLevel::L4,
                target_node: decision.selected_node,
                input_segments: vec![SegmentHandle(3)],
            },
            &mut sink,
        )
        .context("retry runtime dispatch submit failed")?;
    retry_runtime.advance_to(5, &mut sink);
    let _ = retry_runtime.poll_completions(10, &mut sink);

    Ok(sink.into_events())
}

fn run_uapi_demo(topology: &SimTopology) -> anyhow::Result<UapiDemoReport> {
    let mut surface = LocalGuestUapiSurface::new(topology.clone());
    let topo = match surface
        .execute(UapiCommand::QueryTopology)
        .context("query topology failed")?
    {
        UapiResponse::TopologySnapshot(snapshot) => snapshot,
        response => anyhow::bail!("unexpected topology response: {response:?}"),
    };
    let mut events = Vec::new();

    let cq = match surface
        .execute(UapiCommand::RegisterCq { owner: 0 })
        .context("register cq failed")?
    {
        UapiResponse::CqRegistered(cq) => cq,
        response => anyhow::bail!("unexpected cq response: {response:?}"),
    };
    let cmdq = match surface
        .execute(UapiCommand::CreateCmdQueue {
            cq,
            owner: 0,
            depth: 16,
        })
        .context("create cmdq failed")?
    {
        UapiResponse::CmdQueueCreated(cmdq) => cmdq,
        response => anyhow::bail!("unexpected cmdq response: {response:?}"),
    };
    let cmdq_depth = 16usize;

    let segment = match surface
        .execute(UapiCommand::CreateSegment { bytes: 4096 })
        .context("create segment failed")?
    {
        UapiResponse::SegmentCreated(segment) => segment,
        response => anyhow::bail!("unexpected segment response: {response:?}"),
    };

    let write_op = match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::Io(IoSubmitReq {
                op_id: 100,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: Some(segment),
                block: Some(BlockHash("uapi-block-0".to_string())),
            }),
        })
        .context("enqueue write failed")?
    {
        UapiResponse::CommandEnqueued { depth, .. } => depth as u64,
        response => anyhow::bail!("unexpected write response: {response:?}"),
    };
    match surface
        .execute(UapiCommand::RingDoorbell {
            cmdq,
            owner: 0,
            max_batch: Some(1),
        })
        .context("ring write doorbell failed")?
    {
        UapiResponse::DoorbellRung { submitted: 1, .. } => {}
        response => anyhow::bail!("unexpected write doorbell response: {response:?}"),
    }

    let health = match surface
        .execute(UapiCommand::GetHealth { entity: 0 })
        .context("get health failed")?
    {
        UapiResponse::HealthStatus(health) => health,
        response => anyhow::bail!("unexpected health response: {response:?}"),
    };

    let write_completions = match surface
        .execute(UapiCommand::DrainCq { cq, owner: 0 })
        .context("drain write cq failed")?
    {
        UapiResponse::Completions { events, .. } => events,
        response => anyhow::bail!("unexpected drain response: {response:?}"),
    };
    for completion in write_completions {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    let read_op = match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::Io(IoSubmitReq {
                op_id: 101,
                task: None,
                entity: 0,
                opcode: IoOpcode::ReadBlock,
                segment: Some(segment),
                block: Some(BlockHash("uapi-block-0".to_string())),
            }),
        })
        .context("enqueue read failed")?
    {
        UapiResponse::CommandEnqueued { depth, .. } => depth as u64,
        response => anyhow::bail!("unexpected read response: {response:?}"),
    };
    match surface
        .execute(UapiCommand::RingDoorbell {
            cmdq,
            owner: 0,
            max_batch: Some(1),
        })
        .context("ring read doorbell failed")?
    {
        UapiResponse::DoorbellRung { submitted: 1, .. } => {}
        response => anyhow::bail!("unexpected read doorbell response: {response:?}"),
    }

    let read_completions = match surface
        .execute(UapiCommand::DrainCq { cq, owner: 0 })
        .context("drain read cq failed")?
    {
        UapiResponse::Completions { events, .. } => events,
        response => anyhow::bail!("unexpected drain response: {response:?}"),
    };
    for completion in read_completions {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::ShmemPut(ShmemPutReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 4096,
            }),
        })
        .context("enqueue shmem put failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected shmem put response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::ShmemGet(ShmemGetReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 4096,
            }),
        })
        .context("enqueue shmem get failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected shmem get response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::DfsWrite(DfsWriteReq {
                task: None,
                path: "/weights/uapi-demo.bin".into(),
                bytes: 4096,
            }),
        })
        .context("enqueue dfs write failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected dfs write response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::DfsRead(DfsReadReq {
                task: None,
                path: "/weights/uapi-demo.bin".into(),
            }),
        })
        .context("enqueue dfs read failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected dfs read response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::DbPut(DbPutReq {
                task: None,
                key: "uapi:kv:weights".into(),
                bytes: 128,
            }),
        })
        .context("enqueue db put failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected db put response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::EnqueueCmd {
            cmdq,
            owner: 0,
            desc: UapiDescriptor::DbGet(DbGetReq {
                task: None,
                key: "uapi:kv:weights".into(),
            }),
        })
        .context("enqueue db get failed")?
    {
        UapiResponse::CommandEnqueued { .. } => {}
        response => anyhow::bail!("unexpected db get response: {response:?}"),
    };
    let cmdq_pending_after_partial_ring = match surface
        .execute(UapiCommand::RingDoorbell {
            cmdq,
            owner: 0,
            max_batch: Some(3),
        })
        .context("ring partial service doorbell failed")?
    {
        UapiResponse::DoorbellRung {
            submitted: 3,
            pending,
        } => pending,
        response => anyhow::bail!("unexpected partial service doorbell response: {response:?}"),
    };

    let cq_remaining_after_partial_poll = match surface
        .execute(UapiCommand::PollCq {
            cq,
            owner: 0,
            max_entries: Some(2),
        })
        .context("poll partial service cq failed")?
    {
        UapiResponse::Completions {
            events: partial_events,
            remaining,
        } => {
            for completion in partial_events {
                events.push(SimEvent::CompletionObserved {
                    at: completion.finished_at,
                    completion,
                });
            }
            remaining
        }
        response => anyhow::bail!("unexpected partial poll response: {response:?}"),
    };

    match surface
        .execute(UapiCommand::RingDoorbell {
            cmdq,
            owner: 0,
            max_batch: None,
        })
        .context("ring service doorbell failed")?
    {
        UapiResponse::DoorbellRung { submitted: 3, pending: 0 } => {}
        response => anyhow::bail!("unexpected service doorbell response: {response:?}"),
    }

    let service_completions = match surface
        .execute(UapiCommand::DrainCq { cq, owner: 0 })
        .context("drain service cq failed")?
    {
        UapiResponse::Completions { events, .. } => events,
        response => anyhow::bail!("unexpected drain response: {response:?}"),
    };
    for completion in service_completions {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    let _ = write_op;
    let _ = read_op;

    Ok(UapiDemoReport {
        hosts_count: topo.hosts,
        ubpus_count: topo.ubpus,
        entities_count: topo.entities,
        domains_count: topo.domains,
        cq,
        cmdq,
        cmdq_depth,
        segment,
        health,
        cmdq_pending_after_partial_ring,
        cq_remaining_after_partial_poll,
        summary: summarize_events(&events),
        events,
    })
}

fn run_qemu_backend_demo(topology: &SimTopology) -> anyhow::Result<QemuBackendDemoReport> {
    let mut handler = QemuMmioHandler::new(LinquDeviceModel::new(topology.clone()));
    let mmio = handler.device().mmio();
    let topo = handler
        .device_mut()
        .query_topology()
        .context("qemu backend query topology failed")?;
    let (endpoint, layout) = handler
        .device_mut()
        .realize_endpoint(0)
        .context("realize endpoint failed")?;
    let segment = handler
        .device_mut()
        .create_segment(endpoint, 4096)
        .context("create qemu backend segment failed")?;
    let mut events = Vec::new();

    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            0,
            GuestDescriptor::Io(GuestIoDescriptor {
                op_id: 300,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: Some(segment),
                block: Some(BlockHash("qemu-device-block-0".into())),
            }),
        )
        .context("write cmd descriptor 0 failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            1,
            GuestDescriptor::Io(GuestIoDescriptor {
                op_id: 301,
                task: None,
                entity: 0,
                opcode: IoOpcode::ReadBlock,
                segment: Some(segment),
                block: Some(BlockHash("qemu-device-block-0".into())),
            }),
        )
        .context("write cmd descriptor 1 failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            2,
            GuestDescriptor::Service(GuestServiceDescriptor::ShmemPut(ShmemPutReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 4096,
            })),
        )
        .context("write qemu shmem put descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            3,
            GuestDescriptor::Service(GuestServiceDescriptor::ShmemGet(ShmemGetReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 4096,
            })),
        )
        .context("write qemu shmem get descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            4,
            GuestDescriptor::Service(GuestServiceDescriptor::DfsWrite(DfsWriteReq {
                task: None,
                path: "/weights/qemu-demo.bin".into(),
                bytes: 4096,
            })),
        )
        .context("write qemu dfs write descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            5,
            GuestDescriptor::Service(GuestServiceDescriptor::DfsRead(DfsReadReq {
                task: None,
                path: "/weights/qemu-demo.bin".into(),
            })),
        )
        .context("write qemu dfs read descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            6,
            GuestDescriptor::Service(GuestServiceDescriptor::DbPut(DbPutReq {
                task: None,
                key: "qemu:kv:weights".into(),
                bytes: 128,
            })),
        )
        .context("write qemu db put descriptor failed")?;
    handler
        .device_mut()
        .write_cmd_descriptor(
            endpoint,
            7,
            GuestDescriptor::Service(GuestServiceDescriptor::DbGet(DbGetReq {
                task: None,
                key: "qemu:kv:weights".into(),
            })),
        )
        .context("write qemu db get descriptor failed")?;
    handler
        .write(mmio.cmdq_tail_addr(endpoint), 8)
        .context("write qemu backend cmdq tail register failed")?;

    handler
        .write(mmio.doorbell_addr(endpoint), 8)
        .context("write qemu backend doorbell register failed")?;
    let status_after_ring_word = handler
        .read(mmio.status_addr(endpoint))
        .context("read qemu backend status register failed")?;
    let cmdq_head_after_ring = handler
        .read(mmio.cmdq_head_addr(endpoint))
        .context("read qemu backend cmdq head register failed")? as usize;
    let cmdq_tail_after_submit = handler
        .read(mmio.cmdq_tail_addr(endpoint))
        .context("read qemu backend cmdq tail register failed")? as usize;
    let cq_tail_after_ring = handler
        .read(mmio.cq_tail_addr(endpoint))
        .context("read qemu backend cq tail register failed")? as usize;
    let irq_status_after_ring = handler
        .read(mmio.irq_status_addr(endpoint))
        .context("read qemu backend irq status register failed")?;
    let pending_after_ring = ((status_after_ring_word >> 16) & 0xffff) as usize;

    let health = handler
        .device_mut()
        .get_health(0)
        .context("qemu backend get health failed")?;

    let (partial_events, cq_remaining_after_partial_poll) = handler
        .device_mut()
        .poll_cq(endpoint, Some(1))
        .context("qemu backend partial poll failed")?;
    let cq_head_after_partial_poll = handler
        .read(mmio.cq_head_addr(endpoint))
        .context("read qemu backend cq head register after partial poll failed")? as usize;
    for completion in partial_events {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    let drained = handler
        .device_mut()
        .drain_cq(endpoint)
        .context("qemu backend drain failed")?;
    for completion in drained {
        events.push(SimEvent::CompletionObserved {
            at: completion.finished_at,
            completion,
        });
    }

    handler
        .write(mmio.irq_ack_addr(endpoint), irq_status_after_ring)
        .context("write qemu backend irq ack register failed")?;
    let irq_status_after_ack = handler
        .read(mmio.irq_status_addr(endpoint))
        .context("read qemu backend irq status after ack failed")?;

    Ok(QemuBackendDemoReport {
        hosts_count: topo.hosts,
        ubpus_count: topo.ubpus,
        entities_count: topo.entities,
        domains_count: topo.domains,
        endpoint_id: endpoint.0,
        cmdq_depth: layout.cmdq_depth as usize,
        cmdq_head_after_ring,
        cmdq_tail_after_submit,
        cq_head_after_partial_poll,
        cq_tail_after_ring,
        irq_status_after_ring,
        irq_status_after_ack,
        segment,
        health,
        pending_after_ring,
        cq_remaining_after_partial_poll,
        summary: summarize_events(&events),
        events,
    })
}

fn print_report(report: &CliReport) {
    println!("scenario: {}", report.scenario_name);
    println!("group: {}", report.group.as_deref().unwrap_or("-"));
    println!("variant: {}", report.variant.as_deref().unwrap_or("-"));
    println!("logical_system: {}", report.logical_system);
    println!("scenario_file: {}", report.scenario_file);
    println!(
        "topology: hosts={} ubpus={} ubcs={} ummus={} entities={} decoders={} domains={} routes={}",
        report.topology.hosts_count,
        report.topology.ubpus_count,
        report.topology.ubcs_count,
        report.topology.ummus_count,
        report.topology.entities_count,
        report.topology.decoders_count,
        report.topology.domains_count,
        report.topology.routes_count
    );
    println!();
    println!("hosts:");
    for host in &report.topology.hosts {
        println!(
            "  host id={} node_id={} health={:?}",
            host.id, host.node_id, host.health
        );
    }

    println!("ubpus:");
    for ubpu in &report.topology.ubpus {
        println!(
            "  ubpu id={} node_id={} host_id={} health={:?}",
            ubpu.id, ubpu.node_id, ubpu.host_id, ubpu.health
        );
    }

    println!("entities:");
    for entity in &report.topology.entities {
        println!(
            "  entity id={} eid={} ubpu_id={} ubc_id={} health={:?}",
            entity.id, entity.eid, entity.ubpu_id, entity.ubc_id, entity.health
        );
    }

    println!("ubcs:");
    for ubc in &report.topology.ubcs {
        println!(
            "  ubc id={} node_id={} ubpu_id={} host_id={} health={:?}",
            ubc.id, ubc.node_id, ubc.ubpu_id, ubc.host_id, ubc.health
        );
    }

    println!("ummus:");
    for ummu in &report.topology.ummus {
        println!(
            "  ummu id={} node_id={} ubc_id={} domain_id={} health={:?}",
            ummu.id, ummu.node_id, ummu.ubc_id, ummu.domain_id, ummu.health
        );
    }

    println!("decoders:");
    for decoder in &report.topology.decoders {
        println!(
            "  decoder id={} node_id={} ubc_id={} kind={:?} health={:?}",
            decoder.id, decoder.node_id, decoder.ubc_id, decoder.kind, decoder.health
        );
    }

    println!("domains:");
    for domain in &report.topology.domains {
        println!(
            "  domain id={} label={} node_id={} hosts={:?} health={:?}",
            domain.id, domain.label, domain.node_id, domain.hosts, domain.health
        );
    }

    println!("routes:");
    for route in &report.topology.routes {
        println!(
            "  route id={} scope={:?} from_node={} to_node={} level={:?} domain_id={} health={:?}",
            route.id,
            route.scope,
            route.from_node,
            route.to_node,
            route.level,
            route.domain_id,
            route.health
        );
    }

    println!();
    println!("workload_report:");
    println!(
        "  kind={} profile={} requests={} blocks={} hits={} misses={} promotions={} evictions={} completions={}",
        report.workload_report.workload_kind,
        report.workload_report.workload_profile,
        report.workload_report.requests_total,
        report.workload_report.blocks_total,
        report.workload_report.hits,
        report.workload_report.misses,
        report.workload_report.promotions,
        report.workload_report.evictions,
        report.workload_report.completions
    );
    println!(
        "  summary: prefix_hits={} tail_misses={} fallback_reads={} shmem_puts={} shmem_gets={} shmem_denied={} dfs_cold_reads={} dfs_warm_reads={} block_read_misses={} block_writes={} block_writebacks={} block_retryable_failures={} block_queue_rejections={} dfs_seed_writes={} db_puts={} db_gets={} db_retryable_failures={}",
        report.workload_report.prefix_hits,
        report.workload_report.tail_misses,
        report.workload_report.fallback_reads,
        report.workload_report.shmem_puts,
        report.workload_report.shmem_gets,
        report.workload_report.shmem_denied,
        report.workload_report.dfs_cold_reads,
        report.workload_report.dfs_warm_reads,
        report.workload_report.block_read_misses,
        report.workload_report.block_writes,
        report.workload_report.block_writebacks,
        report.workload_report.block_retryable_failures,
        report.workload_report.block_queue_rejections,
        report.workload_report.dfs_seed_writes,
        report.workload_report.db_puts,
        report.workload_report.db_gets,
        report.workload_report.db_retryable_failures
    );
    println!(
        "  summary: completions={} block={} dfs={} shmem={} db={} retryable={} fatal={} runtime_retried={} runtime_failed={}",
        report.workload_report.summary.completions_total,
        report.workload_report.summary.completions_by_source.block_service,
        report.workload_report.summary.completions_by_source.dfs_service,
        report.workload_report.summary.completions_by_source.shmem_service,
        report.workload_report.summary.completions_by_source.db_service,
        report.workload_report.summary.completions_by_status.retryable_failure,
        report.workload_report.summary.completions_by_status.fatal_failure,
        report.workload_report.summary.runtime_retried,
        report.workload_report.summary.runtime_failed
    );
    println!("  events:");
    for event in &report.workload_report.events {
        println!("    {:?}", event);
    }

    println!();
    println!("report_json:");
    println!(
        "{}",
        serde_json::to_string_pretty(report).expect("report json serialization")
    );

    if let Some(aux) = &report.auxiliary {
        println!();
        println!("runtime_demo_events:");
        println!(
            "  summary: completions={} chip={} success={} fatal={} retried={} failed={}",
            aux.runtime_summary.completions_total,
            aux.runtime_summary.completions_by_source.chip_backend,
            aux.runtime_summary.completions_by_status.success,
            aux.runtime_summary.completions_by_status.fatal_failure,
            aux.runtime_summary.runtime_retried,
            aux.runtime_summary.runtime_failed
        );
        for event in &aux.runtime_events {
            println!("  {:?}", event);
        }

        println!();
        println!("uapi_demo:");
        println!(
            "  snapshot => hosts={} ubpus={} entities={} domains={}",
            aux.uapi_report.hosts_count,
            aux.uapi_report.ubpus_count,
            aux.uapi_report.entities_count,
            aux.uapi_report.domains_count
        );
        println!("  cq => {:?}", aux.uapi_report.cq);
        println!(
            "  cmdq => {:?} depth={} pending_after_partial_ring={} cq_remaining_after_partial_poll={}",
            aux.uapi_report.cmdq,
            aux.uapi_report.cmdq_depth,
            aux.uapi_report.cmdq_pending_after_partial_ring,
            aux.uapi_report.cq_remaining_after_partial_poll
        );
        println!("  segment => {:?}", aux.uapi_report.segment);
        println!("  health(entity=0) => {:?}", aux.uapi_report.health);
        println!(
            "  summary: completions={} block={} shmem={} dfs={} db={} success={} retryable={} fatal={}",
            aux.uapi_report.summary.completions_total,
            aux.uapi_report.summary.completions_by_source.block_service,
            aux.uapi_report.summary.completions_by_source.shmem_service,
            aux.uapi_report.summary.completions_by_source.dfs_service,
            aux.uapi_report.summary.completions_by_source.db_service,
            aux.uapi_report.summary.completions_by_status.success,
            aux.uapi_report.summary.completions_by_status.retryable_failure,
            aux.uapi_report.summary.completions_by_status.fatal_failure
        );
        println!("  events:");
        for event in &aux.uapi_report.events {
            println!("    {:?}", event);
        }

        println!();
        println!("qemu_backend_demo:");
        println!(
            "  snapshot => hosts={} ubpus={} entities={} domains={}",
            aux.qemu_backend_report.hosts_count,
            aux.qemu_backend_report.ubpus_count,
            aux.qemu_backend_report.entities_count,
            aux.qemu_backend_report.domains_count
        );
        println!(
            "  endpoint={} cmdq_depth={} cmdq_head_after_ring={} cmdq_tail_after_submit={} cq_head_after_partial_poll={} cq_tail_after_ring={} pending_after_ring={} cq_remaining_after_partial_poll={} irq_status_after_ring=0x{:x} irq_status_after_ack=0x{:x}",
            aux.qemu_backend_report.endpoint_id,
            aux.qemu_backend_report.cmdq_depth,
            aux.qemu_backend_report.cmdq_head_after_ring,
            aux.qemu_backend_report.cmdq_tail_after_submit,
            aux.qemu_backend_report.cq_head_after_partial_poll,
            aux.qemu_backend_report.cq_tail_after_ring,
            aux.qemu_backend_report.pending_after_ring,
            aux.qemu_backend_report.cq_remaining_after_partial_poll,
            aux.qemu_backend_report.irq_status_after_ring,
            aux.qemu_backend_report.irq_status_after_ack
        );
        println!("  segment => {:?}", aux.qemu_backend_report.segment);
        println!("  health(entity=0) => {:?}", aux.qemu_backend_report.health);
        println!(
            "  summary: completions={} block={} shmem={} dfs={} db={} success={} retryable={} fatal={}",
            aux.qemu_backend_report.summary.completions_total,
            aux.qemu_backend_report.summary.completions_by_source.block_service,
            aux.qemu_backend_report.summary.completions_by_source.shmem_service,
            aux.qemu_backend_report.summary.completions_by_source.dfs_service,
            aux.qemu_backend_report.summary.completions_by_source.db_service,
            aux.qemu_backend_report.summary.completions_by_status.success,
            aux.qemu_backend_report.summary.completions_by_status.retryable_failure,
            aux.qemu_backend_report.summary.completions_by_status.fatal_failure
        );
        println!("  events:");
        for event in &aux.qemu_backend_report.events {
            println!("    {:?}", event);
        }
    }
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

fn topology_report(topology: &SimTopology) -> TopologyReport {
    let snapshot = topology.snapshot();

    TopologyReport {
        hosts_count: snapshot.hosts,
        ubpus_count: snapshot.ubpus,
        ubcs_count: snapshot.ubcs,
        ummus_count: snapshot.ummus,
        entities_count: snapshot.entities,
        decoders_count: snapshot.decoders,
        domains_count: snapshot.domains,
        routes_count: snapshot.routes,
        hosts: topology
            .hosts
            .iter()
            .map(|host| HostReport {
                id: host.id,
                node_id: host.node_id,
                health: host.health,
            })
            .collect(),
        ubpus: topology
            .ubpus
            .iter()
            .map(|ubpu| UbpuReport {
                id: ubpu.id,
                node_id: ubpu.node_id,
                host_id: ubpu.host_id,
                health: ubpu.health,
            })
            .collect(),
        ubcs: topology
            .ubcs
            .iter()
            .map(|ubc| UbcReport {
                id: ubc.id,
                node_id: ubc.node_id,
                ubpu_id: ubc.ubpu_id,
                host_id: ubc.host_id,
                health: ubc.health,
            })
            .collect(),
        ummus: topology
            .ummus
            .iter()
            .map(|ummu| UmmuReport {
                id: ummu.id,
                node_id: ummu.node_id,
                ubc_id: ummu.ubc_id,
                domain_id: ummu.domain_id,
                health: ummu.health,
            })
            .collect(),
        entities: topology
            .entities
            .iter()
            .map(|entity| EntityReport {
                id: entity.id,
                eid: entity.eid,
                ubpu_id: entity.ubpu_id,
                ubc_id: entity.ubc_id,
                health: entity.health,
            })
            .collect(),
        decoders: topology
            .decoders
            .iter()
            .map(|decoder| DecoderReport {
                id: decoder.id,
                node_id: decoder.node_id,
                ubc_id: decoder.ubc_id,
                kind: decoder.kind,
                health: decoder.health,
            })
            .collect(),
        domains: topology
            .domains
            .iter()
            .map(|domain| DomainReport {
                id: domain.id,
                label: domain.label.clone(),
                node_id: domain.node_id,
                hosts: domain.hosts.clone(),
                health: domain.health,
            })
            .collect(),
        routes: topology
            .routes
            .iter()
            .map(|route| RouteReport {
                id: route.binding.id,
                scope: route.binding.scope,
                from_node: route.binding.from_node,
                to_node: route.binding.to_node,
                level: route.binding.level,
                domain_id: route.domain_id,
                health: route.health,
            })
            .collect(),
    }
}
