//! Minimal QEMU-facing backend skeleton for the simulator workspace.

mod adapter;
mod device;
mod ffi;
mod memory;
mod mmio;
mod types;

pub use adapter::QemuBackendAdapter;
pub use device::{LinquDeviceModel, MmioDevice};
pub use mmio::QemuMmioHandler;
pub use types::{
    DeviceErrorCode, DeviceInterruptStatus, DeviceQueueStatus, DoorbellWrite, EndpointId, GuestDescriptor,
    GuestEndpointLayout, GuestEndpointSession, GuestIoDescriptor, GuestServiceDescriptor,
    MachineProfile, MmioRegisterMap,
};

#[cfg(test)]
mod tests {
    use super::{
        DeviceErrorCode, DeviceInterruptStatus, DeviceQueueStatus, GuestDescriptor,
        GuestIoDescriptor, GuestServiceDescriptor, LinquDeviceModel, QemuBackendAdapter,
        QemuMmioHandler,
    };
    use sim_config::ScenarioConfig;
    use sim_core::{BlockHash, CompletionSource, IoOpcode};
    use sim_services::{
        db::{DbGetReq, DbPutReq},
        dfs::{DfsReadReq, DfsWriteReq},
        shmem::{ShmemGetReq, ShmemPutReq},
    };
    use sim_topology::SimTopology;

    fn test_adapter() -> QemuBackendAdapter {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        QemuBackendAdapter::new(topology)
    }

    #[test]
    fn qemu_backend_adapter_round_trips_guest_io_descriptor() {
        let mut adapter = test_adapter();
        let session = adapter.register_endpoint(0).expect("register endpoint");
        let segment = adapter.create_segment(&session, 4096).expect("segment");

        let (depth, remaining) = adapter
            .enqueue_descriptor(
                &session,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 10,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("guest-block-0".into())),
                }),
            )
            .expect("enqueue");
        assert_eq!(depth, 1);
        assert!(remaining < session.layout.cmdq_depth as usize);

        let (submitted, pending) = adapter.ring_doorbell(&session, Some(1)).expect("doorbell");
        assert_eq!(submitted, 1);
        assert_eq!(pending, 0);

        let events = adapter.drain_cq(&session).expect("drain");
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].source, CompletionSource::BlockService);
    }

    #[test]
    fn qemu_backend_adapter_supports_partial_cq_poll() {
        let mut adapter = test_adapter();
        let session = adapter.register_endpoint(0).expect("register endpoint");
        let segment = adapter.create_segment(&session, 4096).expect("segment");

        for opcode in [IoOpcode::WriteBlock, IoOpcode::ReadBlock] {
            let _ = adapter
                .enqueue_descriptor(
                    &session,
                    GuestDescriptor::Io(GuestIoDescriptor {
                        op_id: 20 + opcode as u64,
                        task: None,
                        entity: 0,
                        opcode,
                        segment: Some(segment),
                        block: Some(BlockHash("guest-block-1".into())),
                    }),
                )
                .expect("enqueue");
        }
        let _ = adapter.ring_doorbell(&session, None).expect("doorbell");

        let (events, remaining) = adapter.poll_cq(&session, Some(1)).expect("poll");
        assert_eq!(events.len(), 1);
        assert_eq!(remaining, 1);
    }

    #[test]
    fn qemu_backend_adapter_maps_guest_service_descriptor() {
        let mut adapter = test_adapter();
        let session = adapter.register_endpoint(0).expect("register endpoint");
        let segment = adapter.create_segment(&session, 4096).expect("segment");

        let _ = adapter
            .enqueue_descriptor(
                &session,
                GuestDescriptor::Service(GuestServiceDescriptor::ShmemPut(ShmemPutReq {
                    task: None,
                    requester_entity: 0,
                    segment,
                    bytes: 1024,
                })),
            )
            .expect("enqueue");
        let _ = adapter.ring_doorbell(&session, Some(1)).expect("doorbell");

        let events = adapter.drain_cq(&session).expect("drain");
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].source, CompletionSource::ShmemService);
    }

    #[test]
    fn linqu_device_model_bridges_mmio_ring_to_backend_adapter() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut device = LinquDeviceModel::new(topology);

        let (endpoint, layout) = device.realize_endpoint(0).expect("realize endpoint");
        assert_eq!(layout.descriptor_bytes, 64);
        let segment = device.create_segment(endpoint, 4096).expect("segment");

        device
            .write_cmd_descriptor(
                endpoint,
                0,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 100,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("device-path-block".into())),
                }),
            )
            .expect("write descriptor");
        device
            .mmio_write_cmdq_tail(endpoint, 1)
            .expect("advance tail");

        let (submitted, pending) = device
            .mmio_ring_doorbell(super::DoorbellWrite {
                endpoint,
                batch: Some(1),
            })
            .expect("ring");
        assert_eq!(submitted, 1);
        assert_eq!(pending, 0);

        let events = device.drain_cq(endpoint).expect("drain");
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].source, CompletionSource::BlockService);
    }

    #[test]
    fn linqu_device_model_supports_partial_cq_poll() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut device = LinquDeviceModel::new(topology);

        let (endpoint, _) = device.realize_endpoint(0).expect("realize endpoint");
        let segment = device.create_segment(endpoint, 4096).expect("segment");

        device
            .write_cmd_descriptor(
                endpoint,
                0,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 200,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("device-poll-block".into())),
                }),
            )
            .expect("write descriptor 0");
        device
            .write_cmd_descriptor(
                endpoint,
                1,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 201,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::ReadBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("device-poll-block".into())),
                }),
            )
            .expect("write descriptor 1");
        device
            .mmio_write_cmdq_tail(endpoint, 2)
            .expect("advance tail");
        let _ = device
            .mmio_ring_doorbell(super::DoorbellWrite {
                endpoint,
                batch: Some(2),
            })
            .expect("ring");

        let (events, remaining) = device.poll_cq(endpoint, Some(1)).expect("poll");
        assert_eq!(events.len(), 1);
        assert_eq!(remaining, 1);
    }

    #[test]
    fn linqu_device_model_tracks_cmdq_head_tail_status() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut device = LinquDeviceModel::new(topology);

        let (endpoint, _) = device.realize_endpoint(0).expect("realize endpoint");
        let segment = device.create_segment(endpoint, 4096).expect("segment");

        device
            .write_cmd_descriptor(
                endpoint,
                0,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 210,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("device-status-block-0".into())),
                }),
            )
            .expect("write descriptor 0");
        device
            .write_cmd_descriptor(
                endpoint,
                1,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 211,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::ReadBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("device-status-block-0".into())),
                }),
            )
            .expect("write descriptor 1");
        device
            .mmio_write_cmdq_tail(endpoint, 2)
            .expect("advance tail");

        assert_eq!(
            device.read_status(endpoint).expect("status before ring"),
            DeviceQueueStatus {
                cmdq_head: 0,
                cmdq_tail: 2,
                cmdq_pending: 2,
                cq_head: 0,
                cq_tail: 0,
                cq_pending: 0,
            }
        );

        let _ = device
            .mmio_ring_doorbell(super::DoorbellWrite {
                endpoint,
                batch: Some(1),
            })
            .expect("ring");

        let status = device.read_status(endpoint).expect("status after ring");
        assert_eq!(status.cmdq_head, 1);
        assert_eq!(status.cmdq_tail, 2);
        assert_eq!(status.cmdq_pending, 1);
        assert_eq!(status.cq_head, 0);
        assert_eq!(status.cq_tail, 1);
        assert_eq!(status.cq_pending, 1);
    }

    #[test]
    fn linqu_device_model_tracks_cq_head_tail_after_poll() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut device = LinquDeviceModel::new(topology);

        let (endpoint, _) = device.realize_endpoint(0).expect("realize endpoint");
        let segment = device.create_segment(endpoint, 4096).expect("segment");

        device
            .write_cmd_descriptor(
                endpoint,
                0,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 220,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("device-cq-block".into())),
                }),
            )
            .expect("write descriptor 0");
        device
            .write_cmd_descriptor(
                endpoint,
                1,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 221,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::ReadBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("device-cq-block".into())),
                }),
            )
            .expect("write descriptor 1");
        device
            .mmio_write_cmdq_tail(endpoint, 2)
            .expect("advance tail");
        let _ = device
            .mmio_ring_doorbell(super::DoorbellWrite {
                endpoint,
                batch: Some(2),
            })
            .expect("ring");

        let status_before_poll = device.read_status(endpoint).expect("status before cq poll");
        assert_eq!(status_before_poll.cq_head, 0);
        assert_eq!(status_before_poll.cq_tail, 2);
        assert_eq!(status_before_poll.cq_pending, 2);

        let (events, remaining) = device.poll_cq(endpoint, Some(1)).expect("poll one");
        assert_eq!(events.len(), 1);
        assert_eq!(remaining, 1);

        let status_after_poll = device.read_status(endpoint).expect("status after cq poll");
        assert_eq!(status_after_poll.cq_head, 1);
        assert_eq!(status_after_poll.cq_tail, 2);
        assert_eq!(status_after_poll.cq_pending, 1);
    }

    #[test]
    fn linqu_device_model_exposes_mmio_register_reads_and_writes() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut device = LinquDeviceModel::new(topology);

        let (endpoint, _) = device.realize_endpoint(0).expect("realize endpoint");
        let mmio = device.mmio();
        let segment = device.create_segment(endpoint, 4096).expect("segment");

        device
            .write_cmd_descriptor(
                endpoint,
                0,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 230,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("device-mmio-block".into())),
                }),
            )
            .expect("write descriptor");
        device
            .mmio_write(mmio.cmdq_tail_addr(endpoint), 1)
            .expect("write cmdq tail");
        assert_eq!(
            device
                .mmio_read(mmio.cmdq_tail_addr(endpoint))
                .expect("read cmdq tail"),
            1
        );
        device
            .mmio_write(mmio.doorbell_addr(endpoint), 1)
            .expect("ring doorbell");

        let status_word = device
            .mmio_read(mmio.status_addr(endpoint))
            .expect("read status");
        assert_ne!(status_word, 0);
        assert_eq!(
            device
                .mmio_read(mmio.cq_tail_addr(endpoint))
                .expect("read cq tail"),
            1
        );
    }

    #[test]
    fn linqu_device_model_sets_and_acks_irq_status() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut handler = QemuMmioHandler::new(LinquDeviceModel::new(topology));

        let mmio = handler.device().mmio();
        let (endpoint, _) = handler.device_mut().realize_endpoint(0).expect("realize endpoint");
        let segment = handler
            .device_mut()
            .create_segment(endpoint, 4096)
            .expect("segment");

        handler
            .device_mut()
            .write_cmd_descriptor(
                endpoint,
                0,
                GuestDescriptor::Io(GuestIoDescriptor {
                    op_id: 400,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("irq-block".into())),
                }),
            )
            .expect("write descriptor");
        handler
            .write(mmio.cmdq_tail_addr(endpoint), 1)
            .expect("tail write");
        handler
            .write(mmio.doorbell_addr(endpoint), 1)
            .expect("doorbell");

        let irq = handler
            .read(mmio.irq_status_addr(endpoint))
            .expect("irq status");
        assert_ne!(irq & DeviceInterruptStatus::COMPLETION, 0);

        handler
            .write(mmio.irq_ack_addr(endpoint), DeviceInterruptStatus::COMPLETION)
            .expect("irq ack");
        let irq_after_ack = handler
            .read(mmio.irq_status_addr(endpoint))
            .expect("irq status after ack");
        assert_eq!(irq_after_ack & DeviceInterruptStatus::COMPLETION, 0);
    }

    #[test]
    fn linqu_device_model_raises_error_irq_on_invalid_mmio_write() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut handler = QemuMmioHandler::new(LinquDeviceModel::new(topology));
        let mmio = handler.device().mmio();
        let (endpoint, _) = handler.device_mut().realize_endpoint(0).expect("realize endpoint");

        let err = handler
            .write(mmio.cmdq_head_addr(endpoint), 1)
            .expect_err("cmdq head is read-only");
        assert!(matches!(err, sim_core::SimError::InvalidInput("mmio register is read-only")));

        let irq = handler
            .read(mmio.irq_status_addr(endpoint))
            .expect("irq status");
        assert_ne!(irq & DeviceInterruptStatus::ERROR, 0);
        let last_error = handler
            .read(mmio.last_error_addr(endpoint))
            .expect("last error");
        assert_eq!(last_error, DeviceErrorCode::InvalidRegisterWrite as u64);
    }

    #[test]
    fn linqu_device_model_supports_service_descriptors_via_mmio_path() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut device = LinquDeviceModel::new(topology);

        let (endpoint, _) = device.realize_endpoint(0).expect("realize endpoint");
        let mmio = device.mmio();
        let segment = device.create_segment(endpoint, 4096).expect("segment");

        let descriptors = [
            GuestDescriptor::Service(GuestServiceDescriptor::ShmemPut(ShmemPutReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 1024,
            })),
            GuestDescriptor::Service(GuestServiceDescriptor::ShmemGet(ShmemGetReq {
                task: None,
                requester_entity: 0,
                segment,
                bytes: 1024,
            })),
            GuestDescriptor::Service(GuestServiceDescriptor::DfsWrite(DfsWriteReq {
                task: None,
                path: "/weights/device-service.bin".into(),
                bytes: 1024,
            })),
            GuestDescriptor::Service(GuestServiceDescriptor::DfsRead(DfsReadReq {
                task: None,
                path: "/weights/device-service.bin".into(),
            })),
            GuestDescriptor::Service(GuestServiceDescriptor::DbPut(DbPutReq {
                task: None,
                key: "device:service:key".into(),
                bytes: 64,
            })),
            GuestDescriptor::Service(GuestServiceDescriptor::DbGet(DbGetReq {
                task: None,
                key: "device:service:key".into(),
            })),
        ];

        for (slot, desc) in descriptors.into_iter().enumerate() {
            device
                .write_cmd_descriptor(endpoint, slot, desc)
                .expect("write service descriptor");
        }
        device
            .mmio_write(mmio.cmdq_tail_addr(endpoint), 6)
            .expect("write cmdq tail");
        device
            .mmio_write(mmio.doorbell_addr(endpoint), 6)
            .expect("ring doorbell");

        let events = device.drain_cq(endpoint).expect("drain");
        assert_eq!(events.len(), 6);
        assert!(events.iter().any(|event| event.source == CompletionSource::ShmemService));
        assert!(events.iter().any(|event| event.source == CompletionSource::DfsService));
        assert!(events.iter().any(|event| event.source == CompletionSource::DbService));
    }

    #[test]
    fn linqu_device_model_reports_last_error_for_invalid_mmio_write() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut device = LinquDeviceModel::new(topology);

        let (endpoint, _) = device.realize_endpoint(0).expect("realize endpoint");
        let mmio = device.mmio();

        let err = device
            .mmio_write(mmio.status_addr(endpoint), 1)
            .expect_err("status register should be read-only");
        assert!(matches!(err, sim_core::SimError::InvalidInput(_)));
        assert_eq!(
            device
                .mmio_read(mmio.last_error_addr(endpoint))
                .expect("read last_error"),
            DeviceErrorCode::InvalidRegisterWrite as u64
        );
    }

    #[test]
    fn linqu_device_model_reports_last_error_for_invalid_offset() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut device = LinquDeviceModel::new(topology);

        let (endpoint, _) = device.realize_endpoint(0).expect("realize endpoint");
        let mmio = device.mmio();
        let err = device
            .mmio_write(mmio.endpoint_offset(endpoint) + 0x58, 1)
            .expect_err("unknown register should fail");
        assert!(matches!(err, sim_core::SimError::InvalidInput(_)));
        assert_eq!(
            device
                .mmio_read(mmio.last_error_addr(endpoint))
                .expect("read last_error"),
            DeviceErrorCode::InvalidOffset as u64
        );
    }

    #[test]
    fn linqu_device_model_reports_last_error_for_queue_overflow() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut device = LinquDeviceModel::new(topology);

        let (endpoint, layout) = device.realize_endpoint(0).expect("realize endpoint");
        let mmio = device.mmio();
        let overflow_tail = (layout.cmdq_depth as u64) + 1;
        let err = device
            .mmio_write(mmio.cmdq_tail_addr(endpoint), overflow_tail)
            .expect_err("overflow tail should fail");
        assert!(matches!(err, sim_core::SimError::InvalidInput(_)));
        assert_eq!(
            device
                .mmio_read(mmio.last_error_addr(endpoint))
                .expect("read last_error"),
            DeviceErrorCode::RangeError as u64
        );
    }

    #[test]
    fn qemu_mmio_handler_wraps_device_reads_and_writes() {
        let scenario_path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../scenarios/mvp_2host_single_domain.yaml");
        let config = ScenarioConfig::from_yaml_file(&scenario_path).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut handler = QemuMmioHandler::new(LinquDeviceModel::new(topology));

        let (endpoint, _) = handler
            .device_mut()
            .realize_endpoint(0)
            .expect("realize endpoint");
        let mmio = handler.device().mmio();
        let segment = handler
            .device_mut()
            .create_segment(endpoint, 4096)
            .expect("segment");
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
                    block: Some(BlockHash("handler-block".into())),
                }),
            )
            .expect("write descriptor");

        handler
            .write(mmio.cmdq_tail_addr(endpoint), 1)
            .expect("write cmd tail");
        handler
            .write(mmio.doorbell_addr(endpoint), 1)
            .expect("ring");
        let status = handler
            .read(mmio.status_addr(endpoint))
            .expect("read status register");
        assert_ne!(status, 0);
    }
}
