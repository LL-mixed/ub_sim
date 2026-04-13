use std::collections::HashMap;

use sim_core::{CompletionEvent, EntityId, HealthStatus, SegmentHandle, SimError};
use sim_topology::{SimTopology, TopologySnapshot};

use crate::adapter::QemuBackendAdapter;
use crate::memory::GuestRingMemory;
use crate::types::{
    DeviceErrorCode, DeviceInterruptStatus, DeviceQueueStatus, DoorbellWrite, EndpointId,
    GuestDescriptor, GuestEndpointLayout, GuestEndpointSession, MachineProfile, MmioRegisterMap,
};

#[derive(Debug)]
struct DeviceEndpointState {
    session: GuestEndpointSession,
    memory: GuestRingMemory,
    cmdq_head: usize,
    cmdq_tail: usize,
    cmdq_count: usize,
    cq_head: usize,
    cq_tail: usize,
    cq_count: usize,
    last_error: u64,
    irq_status: u64,
}

#[derive(Debug)]
pub struct LinquDeviceModel {
    mmio: MmioRegisterMap,
    adapter: QemuBackendAdapter,
    next_endpoint_id: u16,
    endpoints: HashMap<EndpointId, DeviceEndpointState>,
}

pub trait MmioDevice {
    fn mmio_read(&mut self, offset: u64) -> Result<u64, SimError>;
    fn mmio_write(&mut self, offset: u64, value: u64) -> Result<(), SimError>;
}

impl LinquDeviceModel {
    pub fn new(topology: SimTopology) -> Self {
        Self::with_profile(topology, MachineProfile::default())
    }

    pub fn with_profile(topology: SimTopology, profile: MachineProfile) -> Self {
        Self {
            mmio: MmioRegisterMap::default(),
            adapter: QemuBackendAdapter::with_profile(topology, profile),
            next_endpoint_id: 0,
            endpoints: HashMap::new(),
        }
    }

    pub fn mmio(&self) -> MmioRegisterMap {
        self.mmio
    }

    pub fn query_topology(&mut self) -> Result<TopologySnapshot, SimError> {
        self.adapter.query_topology()
    }

    pub fn realize_endpoint(
        &mut self,
        entity: EntityId,
    ) -> Result<(EndpointId, GuestEndpointLayout), SimError> {
        self.next_endpoint_id += 1;
        let endpoint = EndpointId(self.next_endpoint_id);
        let session = self.adapter.register_endpoint(entity)?;
        let layout = session.layout;
        self.endpoints.insert(
            endpoint,
            DeviceEndpointState {
                session,
                memory: GuestRingMemory::new(
                    layout.cmdq_depth as usize,
                    layout.cq_depth as usize,
                    layout.descriptor_bytes as usize,
                ),
                cmdq_head: 0,
                cmdq_tail: 0,
                cmdq_count: 0,
                cq_head: 0,
                cq_tail: 0,
                cq_count: 0,
                last_error: 0,
                irq_status: 0,
            },
        );
        Ok((endpoint, layout))
    }

    pub fn create_segment(
        &mut self,
        endpoint: EndpointId,
        bytes: u64,
    ) -> Result<SegmentHandle, SimError> {
        let state = self
            .endpoints
            .get(&endpoint)
            .ok_or(SimError::NotFound("endpoint"))?;
        self.adapter.create_segment(&state.session, bytes)
    }

    pub fn write_cmd_descriptor(
        &mut self,
        endpoint: EndpointId,
        slot: usize,
        desc: GuestDescriptor,
    ) -> Result<(), SimError> {
        let state = self
            .endpoints
            .get_mut(&endpoint)
            .ok_or(SimError::NotFound("endpoint"))?;
        state
            .memory
            .write_cmd_slot(slot, desc)
            .map_err(SimError::InvalidInput)
    }

    pub fn mmio_write_cmdq_tail(
        &mut self,
        endpoint: EndpointId,
        new_tail: usize,
    ) -> Result<(), SimError> {
        let state = self
            .endpoints
            .get_mut(&endpoint)
            .ok_or(SimError::NotFound("endpoint"))?;
        let depth = state.memory.cmd_depth();
        if new_tail >= depth {
            return Err(SimError::InvalidInput("cmd tail out of range"));
        }

        let advance = if new_tail >= state.cmdq_tail {
            new_tail - state.cmdq_tail
        } else {
            depth - state.cmdq_tail + new_tail
        };
        if state.cmdq_count + advance > depth {
            return Err(SimError::InvalidInput("cmd queue overflow"));
        }

        state.cmdq_tail = new_tail;
        state.cmdq_count += advance;
        Ok(())
    }

    pub fn mmio_write(&mut self, offset: u64, value: u64) -> Result<(), SimError> {
        let (endpoint, reg) = match self.decode_mmio_offset(offset) {
            Ok(decoded) => decoded,
            Err(err) => {
                if let Some(endpoint) = self.try_decode_endpoint(offset) {
                    self.set_last_error(endpoint, &err);
                }
                return Err(err);
            }
        };
        let result = match reg {
            RegisterKind::CmdqTail => self.mmio_write_cmdq_tail(endpoint, value as usize),
            RegisterKind::CqHead => self.mmio_write_cq_head(endpoint, value as usize),
            RegisterKind::Doorbell => self
                .mmio_ring_doorbell(DoorbellWrite {
                    endpoint,
                    batch: Some(value as usize),
                })
                .map(|_| ()),
            RegisterKind::IrqAck => self.mmio_write_irq_ack(endpoint, value),
            RegisterKind::CmdqHead
            | RegisterKind::CqTail
            | RegisterKind::Status
            | RegisterKind::LastError
            | RegisterKind::IrqStatus => Err(SimError::InvalidInput("mmio register is read-only")),
        };

        if let Err(err) = &result {
            self.set_last_error(endpoint, err);
        }
        result
    }

    pub fn mmio_read(&mut self, offset: u64) -> Result<u64, SimError> {
        let (endpoint, reg) = match self.decode_mmio_offset(offset) {
            Ok(decoded) => decoded,
            Err(err) => {
                if let Some(endpoint) = self.try_decode_endpoint(offset) {
                    self.set_last_error(endpoint, &err);
                }
                return Err(err);
            }
        };
        let result = match reg {
            RegisterKind::CmdqHead => Ok(self.read_status(endpoint)?.cmdq_head as u64),
            RegisterKind::CmdqTail => Ok(self.read_status(endpoint)?.cmdq_tail as u64),
            RegisterKind::CqHead => Ok(self.read_status(endpoint)?.cq_head as u64),
            RegisterKind::CqTail => Ok(self.read_status(endpoint)?.cq_tail as u64),
            RegisterKind::Status => Ok(Self::encode_status(self.read_status(endpoint)?)),
            RegisterKind::Doorbell => Ok(0),
            RegisterKind::LastError => {
                let state = self
                    .endpoints
                    .get(&endpoint)
                    .ok_or(SimError::NotFound("endpoint"))?;
                Ok(state.last_error)
            }
            RegisterKind::IrqStatus => {
                let state = self
                    .endpoints
                    .get(&endpoint)
                    .ok_or(SimError::NotFound("endpoint"))?;
                Ok(state.irq_status)
            }
            RegisterKind::IrqAck => Err(SimError::InvalidInput("mmio register is write-only")),
        };
        if let Err(err) = &result {
            self.set_last_error(endpoint, err);
        }
        result
    }

    pub fn read_status(&mut self, endpoint: EndpointId) -> Result<DeviceQueueStatus, SimError> {
        self.flush_cq_ring(endpoint)?;
        let state = self
            .endpoints
            .get(&endpoint)
            .ok_or(SimError::NotFound("endpoint"))?;
        Ok(DeviceQueueStatus {
            cmdq_head: state.cmdq_head,
            cmdq_tail: state.cmdq_tail,
            cmdq_pending: state.cmdq_count,
            cq_head: state.cq_head,
            cq_tail: state.cq_tail,
            cq_pending: state.cq_count,
        })
    }

    pub fn mmio_ring_doorbell(
        &mut self,
        write: DoorbellWrite,
    ) -> Result<(usize, usize), SimError> {
        let (session, enqueued, pending_after_submit) = {
            let state = self
                .endpoints
                .get_mut(&write.endpoint)
                .ok_or(SimError::NotFound("endpoint"))?;

            let depth = state.memory.cmd_depth();
            let batch = write.batch.unwrap_or(state.cmdq_count);
            let to_submit = batch.min(state.cmdq_count);
            let mut enqueued = 0usize;
            for _ in 0..to_submit {
                let slot = state.cmdq_head;
                let desc = state.memory.take_cmd_slot(slot).map_err(SimError::InvalidInput)?;
                let _ = self.adapter.enqueue_descriptor(&state.session, desc)?;
                state.cmdq_head = (state.cmdq_head + 1) % depth;
                state.cmdq_count -= 1;
                enqueued += 1;
            }
            (state.session.clone(), enqueued, state.cmdq_count)
        };
        if enqueued == 0 {
            return Ok((0, pending_after_submit));
        }
        let (submitted, pending) = self.adapter.ring_doorbell(&session, Some(enqueued))?;
        self.flush_cq_ring(write.endpoint)?;
        Ok((submitted, pending))
    }

    pub fn poll_cq(
        &mut self,
        endpoint: EndpointId,
        max_entries: Option<usize>,
    ) -> Result<(Vec<CompletionEvent>, usize), SimError> {
        self.flush_cq_ring(endpoint)?;
        let state = self
            .endpoints
            .get_mut(&endpoint)
            .ok_or(SimError::NotFound("endpoint"))?;
        let limit = max_entries.unwrap_or(state.cq_count).min(state.cq_count);
        let mut events = Vec::with_capacity(limit);
        let depth = state.memory.cq_depth();
        for _ in 0..limit {
            let slot = state.cq_head;
            let completion = state.memory.take_cq_slot(slot).map_err(SimError::InvalidInput)?;
            state.cq_head = (state.cq_head + 1) % depth;
            state.cq_count -= 1;
            events.push(completion);
        }
        Ok((events, state.cq_count))
    }

    pub fn drain_cq(&mut self, endpoint: EndpointId) -> Result<Vec<CompletionEvent>, SimError> {
        self.flush_cq_ring(endpoint)?;
        let (events, _) = self.poll_cq(endpoint, None)?;
        Ok(events)
    }

    pub fn get_health(&mut self, entity: EntityId) -> Result<HealthStatus, SimError> {
        self.adapter.get_health(entity)
    }

    fn mmio_write_cq_head(
        &mut self,
        endpoint: EndpointId,
        new_head: usize,
    ) -> Result<(), SimError> {
        let state = self
            .endpoints
            .get_mut(&endpoint)
            .ok_or(SimError::NotFound("endpoint"))?;
        let depth = state.memory.cq_depth();
        if new_head >= depth {
            return Err(SimError::InvalidInput("cq head out of range"));
        }
        let advance = if new_head >= state.cq_head {
            new_head - state.cq_head
        } else {
            depth - state.cq_head + new_head
        };
        if advance > state.cq_count {
            return Err(SimError::InvalidInput("cq head exceeds pending completions"));
        }
        for _ in 0..advance {
            state.cq_head = (state.cq_head + 1) % depth;
            state.cq_count -= 1;
        }
        Ok(())
    }

    fn mmio_write_irq_ack(&mut self, endpoint: EndpointId, value: u64) -> Result<(), SimError> {
        let state = self
            .endpoints
            .get_mut(&endpoint)
            .ok_or(SimError::NotFound("endpoint"))?;
        state.irq_status &= !value;
        Ok(())
    }

    fn flush_cq_ring(&mut self, endpoint: EndpointId) -> Result<(), SimError> {
        let state = self
            .endpoints
            .get_mut(&endpoint)
            .ok_or(SimError::NotFound("endpoint"))?;
        let available = state.memory.cq_depth() - state.cq_count;
        if available == 0 {
            return Ok(());
        }
        let (events, _) = self.adapter.poll_cq(&state.session, Some(available))?;
        for completion in events {
            let slot = state.cq_tail;
            state
                .memory
                .write_cq_slot(slot, completion)
                .map_err(SimError::InvalidInput)?;
            state.cq_tail = (state.cq_tail + 1) % state.memory.cq_depth();
            state.cq_count += 1;
        }
        if state.cq_count > 0 {
            state.irq_status |= DeviceInterruptStatus::COMPLETION;
        }
        Ok(())
    }

    fn decode_mmio_offset(&self, offset: u64) -> Result<(EndpointId, RegisterKind), SimError> {
        if offset < self.mmio.endpoint_base {
            return Err(SimError::InvalidInput("mmio offset below endpoint base"));
        }
        let relative = offset - self.mmio.endpoint_base;
        let endpoint_index = relative / self.mmio.endpoint_stride;
        let reg_offset = relative % self.mmio.endpoint_stride;
        let endpoint = EndpointId((endpoint_index as u16) + 1);
        if !self.endpoints.contains_key(&endpoint) {
            return Err(SimError::NotFound("endpoint"));
        }
        let reg = if reg_offset == self.mmio.cmdq_head_offset {
            RegisterKind::CmdqHead
        } else if reg_offset == self.mmio.cmdq_tail_offset {
            RegisterKind::CmdqTail
        } else if reg_offset == self.mmio.cq_head_offset {
            RegisterKind::CqHead
        } else if reg_offset == self.mmio.cq_tail_offset {
            RegisterKind::CqTail
        } else if reg_offset == self.mmio.status_offset {
            RegisterKind::Status
        } else if reg_offset == self.mmio.doorbell_offset {
            RegisterKind::Doorbell
        } else if reg_offset == self.mmio.last_error_offset {
            RegisterKind::LastError
        } else if reg_offset == self.mmio.irq_status_offset {
            RegisterKind::IrqStatus
        } else if reg_offset == self.mmio.irq_ack_offset {
            RegisterKind::IrqAck
        } else {
            return Err(SimError::InvalidInput("unknown mmio register"));
        };
        Ok((endpoint, reg))
    }

    fn try_decode_endpoint(&self, offset: u64) -> Option<EndpointId> {
        if offset < self.mmio.endpoint_base {
            return None;
        }
        let relative = offset - self.mmio.endpoint_base;
        let endpoint_index = relative / self.mmio.endpoint_stride;
        Some(EndpointId((endpoint_index as u16) + 1))
    }

    fn encode_status(status: DeviceQueueStatus) -> u64 {
        ((status.cmdq_pending as u64) & 0xffff)
            | (((status.cq_pending as u64) & 0xffff) << 16)
            | (((status.cmdq_head as u64) & 0xff) << 32)
            | (((status.cmdq_tail as u64) & 0xff) << 40)
            | (((status.cq_head as u64) & 0xff) << 48)
            | (((status.cq_tail as u64) & 0xff) << 56)
    }

    fn set_last_error(&mut self, endpoint: EndpointId, err: &SimError) {
        if let Some(state) = self.endpoints.get_mut(&endpoint) {
            state.last_error = Self::classify_error(err) as u64;
            state.irq_status |= DeviceInterruptStatus::ERROR;
        }
    }

    fn classify_error(err: &SimError) -> DeviceErrorCode {
        match err {
            SimError::NotFound(_) => DeviceErrorCode::EndpointNotFound,
            SimError::NotImplemented => DeviceErrorCode::NotImplemented,
            SimError::InvalidInput(message) => match *message {
                "mmio register is read-only" => DeviceErrorCode::InvalidRegisterWrite,
                "unknown mmio register" | "mmio offset below endpoint base" => {
                    DeviceErrorCode::InvalidOffset
                }
                "cmd queue overflow" => DeviceErrorCode::QueueOverflow,
                "cmd slot out of range"
                | "cmd tail out of range"
                | "cq head out of range"
                | "cq head exceeds pending completions" => DeviceErrorCode::RangeError,
                "missing descriptor in cmd ring" | "missing completion in cq ring" => {
                    DeviceErrorCode::InvalidDescriptor
                }
                _ => DeviceErrorCode::InvalidRegisterRead,
            },
        }
    }
}

impl MmioDevice for LinquDeviceModel {
    fn mmio_read(&mut self, offset: u64) -> Result<u64, SimError> {
        LinquDeviceModel::mmio_read(self, offset)
    }

    fn mmio_write(&mut self, offset: u64, value: u64) -> Result<(), SimError> {
        LinquDeviceModel::mmio_write(self, offset, value)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum RegisterKind {
    CmdqHead,
    CmdqTail,
    CqHead,
    CqTail,
    Status,
    Doorbell,
    LastError,
    IrqStatus,
    IrqAck,
}
