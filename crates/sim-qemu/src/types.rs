use sim_core::{
    BlockHash, CmdQueueHandle, CompletionEvent, CompletionSource, CompletionStatus, CqHandle,
    EntityId, IoOpcode, IoSubmitReq, SegmentHandle, TaskKey,
};
use sim_services::{
    db::{DbGetReq, DbPutReq},
    dfs::{DfsReadReq, DfsWriteReq},
    shmem::{ShmemGetReq, ShmemPutReq},
};
use sim_uapi::UapiDescriptor;

#[derive(Debug, Clone)]
pub struct MachineProfile {
    pub name: String,
    pub endpoint: GuestEndpointLayout,
}

impl Default for MachineProfile {
    fn default() -> Self {
        Self {
            name: "ub-host-minimal".into(),
            endpoint: GuestEndpointLayout::default(),
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct GuestEndpointLayout {
    pub descriptor_bytes: u32,
    pub cmdq_depth: u16,
    pub cq_depth: u16,
    pub doorbell_stride_bytes: u32,
}

impl Default for GuestEndpointLayout {
    fn default() -> Self {
        Self {
            descriptor_bytes: 64,
            cmdq_depth: 32,
            cq_depth: 64,
            doorbell_stride_bytes: 8,
        }
    }
}

#[derive(Debug, Clone)]
pub enum GuestDescriptor {
    Io(GuestIoDescriptor),
    Service(GuestServiceDescriptor),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GuestIoDescriptor {
    pub op_id: u64,
    pub task: Option<TaskKey>,
    pub entity: EntityId,
    pub opcode: IoOpcode,
    pub segment: Option<SegmentHandle>,
    pub block: Option<BlockHash>,
}

#[derive(Debug, Clone)]
pub enum GuestServiceDescriptor {
    BlockWriteback {
        block: BlockHash,
        task: Option<TaskKey>,
    },
    ShmemPut(ShmemPutReq),
    ShmemGet(ShmemGetReq),
    DfsRead(DfsReadReq),
    DfsWrite(DfsWriteReq),
    DbPut(DbPutReq),
    DbGet(DbGetReq),
}

impl GuestDescriptor {
    pub fn to_uapi_descriptor(self) -> UapiDescriptor {
        match self {
            GuestDescriptor::Io(io) => UapiDescriptor::Io(IoSubmitReq {
                op_id: io.op_id,
                task: io.task,
                entity: io.entity,
                opcode: io.opcode,
                segment: io.segment,
                block: io.block,
            }),
            GuestDescriptor::Service(service) => match service {
                GuestServiceDescriptor::BlockWriteback { block, task } => {
                    UapiDescriptor::BlockWriteback { block, task }
                }
                GuestServiceDescriptor::ShmemPut(req) => UapiDescriptor::ShmemPut(req),
                GuestServiceDescriptor::ShmemGet(req) => UapiDescriptor::ShmemGet(req),
                GuestServiceDescriptor::DfsRead(req) => UapiDescriptor::DfsRead(req),
                GuestServiceDescriptor::DfsWrite(req) => UapiDescriptor::DfsWrite(req),
                GuestServiceDescriptor::DbPut(req) => UapiDescriptor::DbPut(req),
                GuestServiceDescriptor::DbGet(req) => UapiDescriptor::DbGet(req),
            },
        }
    }

    pub fn encode(&self, slot_bytes: usize) -> Result<Vec<u8>, &'static str> {
        let mut buf = Vec::with_capacity(slot_bytes);
        match self {
            GuestDescriptor::Io(io) => {
                write_u8(&mut buf, 1);
                write_u64(&mut buf, io.op_id);
                encode_task(&mut buf, &io.task);
                write_u32(&mut buf, io.entity);
                write_u8(
                    &mut buf,
                    match io.opcode {
                        IoOpcode::ReadBlock => 1,
                        IoOpcode::WriteBlock => 2,
                        IoOpcode::Dispatch => 3,
                        IoOpcode::RemoteFetch | IoOpcode::RemoteStore => {
                            return Err("remote fetch/store not yet supported in guest descriptor encoding")
                        }
                    },
                );
                encode_opt_segment(&mut buf, io.segment);
                encode_opt_string(&mut buf, io.block.as_ref().map(|block| block.0.as_str()))?;
            }
            GuestDescriptor::Service(service) => match service {
                GuestServiceDescriptor::BlockWriteback { block, task } => {
                    write_u8(&mut buf, 2);
                    encode_task(&mut buf, task);
                    encode_string(&mut buf, &block.0)?;
                }
                GuestServiceDescriptor::ShmemPut(req) => {
                    write_u8(&mut buf, 3);
                    encode_task(&mut buf, &req.task);
                    write_u32(&mut buf, req.requester_entity);
                    write_u64(&mut buf, req.segment.0);
                    write_u64(&mut buf, req.bytes);
                }
                GuestServiceDescriptor::ShmemGet(req) => {
                    write_u8(&mut buf, 4);
                    encode_task(&mut buf, &req.task);
                    write_u32(&mut buf, req.requester_entity);
                    write_u64(&mut buf, req.segment.0);
                    write_u64(&mut buf, req.bytes);
                }
                GuestServiceDescriptor::DfsRead(req) => {
                    write_u8(&mut buf, 5);
                    encode_task(&mut buf, &req.task);
                    encode_string(&mut buf, &req.path)?;
                }
                GuestServiceDescriptor::DfsWrite(req) => {
                    write_u8(&mut buf, 6);
                    encode_task(&mut buf, &req.task);
                    encode_string(&mut buf, &req.path)?;
                    write_u64(&mut buf, req.bytes);
                }
                GuestServiceDescriptor::DbPut(req) => {
                    write_u8(&mut buf, 7);
                    encode_task(&mut buf, &req.task);
                    encode_string(&mut buf, &req.key)?;
                    write_u64(&mut buf, req.bytes);
                }
                GuestServiceDescriptor::DbGet(req) => {
                    write_u8(&mut buf, 8);
                    encode_task(&mut buf, &req.task);
                    encode_string(&mut buf, &req.key)?;
                }
            },
        }
        pad_slot(buf, slot_bytes)
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, &'static str> {
        let mut cursor = Cursor::new(bytes);
        match read_u8(&mut cursor)? {
            1 => {
                let op_id = read_u64(&mut cursor)?;
                let task = decode_task(&mut cursor)?;
                let entity = read_u32(&mut cursor)?;
                let opcode = match read_u8(&mut cursor)? {
                    1 => IoOpcode::ReadBlock,
                    2 => IoOpcode::WriteBlock,
                    3 => IoOpcode::Dispatch,
                    _ => return Err("invalid io opcode"),
                };
                let segment = decode_opt_segment(&mut cursor)?;
                let block = decode_opt_string(&mut cursor)?.map(BlockHash);
                Ok(GuestDescriptor::Io(GuestIoDescriptor {
                    op_id,
                    task,
                    entity,
                    opcode,
                    segment,
                    block,
                }))
            }
            2 => Ok(GuestDescriptor::Service(GuestServiceDescriptor::BlockWriteback {
                task: decode_task(&mut cursor)?,
                block: BlockHash(decode_string(&mut cursor)?),
            })),
            3 => Ok(GuestDescriptor::Service(GuestServiceDescriptor::ShmemPut(
                ShmemPutReq {
                    task: decode_task(&mut cursor)?,
                    requester_entity: read_u32(&mut cursor)?,
                    segment: SegmentHandle(read_u64(&mut cursor)?),
                    bytes: read_u64(&mut cursor)?,
                },
            ))),
            4 => Ok(GuestDescriptor::Service(GuestServiceDescriptor::ShmemGet(
                ShmemGetReq {
                    task: decode_task(&mut cursor)?,
                    requester_entity: read_u32(&mut cursor)?,
                    segment: SegmentHandle(read_u64(&mut cursor)?),
                    bytes: read_u64(&mut cursor)?,
                },
            ))),
            5 => Ok(GuestDescriptor::Service(GuestServiceDescriptor::DfsRead(
                DfsReadReq {
                    task: decode_task(&mut cursor)?,
                    path: decode_string(&mut cursor)?,
                },
            ))),
            6 => Ok(GuestDescriptor::Service(GuestServiceDescriptor::DfsWrite(
                DfsWriteReq {
                    task: decode_task(&mut cursor)?,
                    path: decode_string(&mut cursor)?,
                    bytes: read_u64(&mut cursor)?,
                },
            ))),
            7 => Ok(GuestDescriptor::Service(GuestServiceDescriptor::DbPut(
                DbPutReq {
                    task: decode_task(&mut cursor)?,
                    key: decode_string(&mut cursor)?,
                    bytes: read_u64(&mut cursor)?,
                },
            ))),
            8 => Ok(GuestDescriptor::Service(GuestServiceDescriptor::DbGet(
                DbGetReq {
                    task: decode_task(&mut cursor)?,
                    key: decode_string(&mut cursor)?,
                },
            ))),
            _ => Err("invalid descriptor kind"),
        }
    }
}

#[derive(Debug, Clone)]
pub struct GuestEndpointSession {
    pub entity: EntityId,
    pub cq: CqHandle,
    pub cmdq: CmdQueueHandle,
    pub layout: GuestEndpointLayout,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct EndpointId(pub u16);

#[derive(Debug, Clone, Copy)]
pub struct MmioRegisterMap {
    pub endpoint_base: u64,
    pub endpoint_stride: u64,
    pub cmdq_head_offset: u64,
    pub cmdq_tail_offset: u64,
    pub cq_head_offset: u64,
    pub cq_tail_offset: u64,
    pub status_offset: u64,
    pub doorbell_offset: u64,
    pub last_error_offset: u64,
    pub irq_status_offset: u64,
    pub irq_ack_offset: u64,
}

impl Default for MmioRegisterMap {
    fn default() -> Self {
        Self {
            endpoint_base: 0x1000,
            endpoint_stride: 0x100,
            cmdq_head_offset: 0x00,
            cmdq_tail_offset: 0x08,
            cq_head_offset: 0x10,
            cq_tail_offset: 0x18,
            status_offset: 0x20,
            doorbell_offset: 0x28,
            last_error_offset: 0x30,
            irq_status_offset: 0x38,
            irq_ack_offset: 0x40,
        }
    }
}

impl MmioRegisterMap {
    pub fn endpoint_offset(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_base + (u64::from(endpoint.0.saturating_sub(1)) * self.endpoint_stride)
    }

    pub fn cmdq_head_addr(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_offset(endpoint) + self.cmdq_head_offset
    }

    pub fn cmdq_tail_addr(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_offset(endpoint) + self.cmdq_tail_offset
    }

    pub fn cq_head_addr(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_offset(endpoint) + self.cq_head_offset
    }

    pub fn cq_tail_addr(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_offset(endpoint) + self.cq_tail_offset
    }

    pub fn status_addr(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_offset(endpoint) + self.status_offset
    }

    pub fn doorbell_addr(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_offset(endpoint) + self.doorbell_offset
    }

    pub fn last_error_addr(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_offset(endpoint) + self.last_error_offset
    }

    pub fn irq_status_addr(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_offset(endpoint) + self.irq_status_offset
    }

    pub fn irq_ack_addr(&self, endpoint: EndpointId) -> u64 {
        self.endpoint_offset(endpoint) + self.irq_ack_offset
    }
}

#[derive(Debug, Clone, Copy)]
pub struct DoorbellWrite {
    pub endpoint: EndpointId,
    pub batch: Option<usize>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DeviceQueueStatus {
    pub cmdq_head: usize,
    pub cmdq_tail: usize,
    pub cmdq_pending: usize,
    pub cq_head: usize,
    pub cq_tail: usize,
    pub cq_pending: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u64)]
pub enum DeviceErrorCode {
    None = 0,
    InvalidRegisterWrite = 1,
    InvalidRegisterRead = 2,
    InvalidOffset = 3,
    EndpointNotFound = 4,
    QueueOverflow = 5,
    RangeError = 6,
    InvalidDescriptor = 7,
    NotImplemented = 8,
}

pub struct DeviceInterruptStatus;

impl DeviceInterruptStatus {
    pub const COMPLETION: u64 = 1 << 0;
    pub const ERROR: u64 = 1 << 1;
    pub const CQ_OVERFLOW: u64 = 1 << 2;
}

pub fn encode_completion(event: &CompletionEvent, slot_bytes: usize) -> Result<Vec<u8>, &'static str> {
    let mut buf = Vec::with_capacity(slot_bytes);
    write_u64(&mut buf, event.op_id);
    encode_task(&mut buf, &event.task);
    write_u8(
        &mut buf,
        match event.source {
            CompletionSource::ChipBackend => 1,
            CompletionSource::BlockService => 2,
            CompletionSource::ShmemService => 3,
            CompletionSource::DfsService => 4,
            CompletionSource::DbService => 5,
            CompletionSource::GuestUapi => 6,
            CompletionSource::RemoteNode => 7,
        },
    );
    match &event.status {
        CompletionStatus::Success => write_u8(&mut buf, 1),
        CompletionStatus::RetryableFailure { code } => {
            write_u8(&mut buf, 2);
            encode_string(&mut buf, code)?;
        }
        CompletionStatus::FatalFailure { code } => {
            write_u8(&mut buf, 3);
            encode_string(&mut buf, code)?;
        }
    }
    write_u64(&mut buf, event.finished_at);
    pad_slot(buf, slot_bytes)
}

pub fn decode_completion(bytes: &[u8]) -> Result<CompletionEvent, &'static str> {
    let mut cursor = Cursor::new(bytes);
    let op_id = read_u64(&mut cursor)?;
    let task = decode_task(&mut cursor)?;
    let source = match read_u8(&mut cursor)? {
        1 => CompletionSource::ChipBackend,
        2 => CompletionSource::BlockService,
        3 => CompletionSource::ShmemService,
        4 => CompletionSource::DfsService,
        5 => CompletionSource::DbService,
        6 => CompletionSource::GuestUapi,
        7 => CompletionSource::RemoteNode,
        _ => return Err("invalid completion source"),
    };
    let status = match read_u8(&mut cursor)? {
        1 => CompletionStatus::Success,
        2 => CompletionStatus::RetryableFailure {
            code: decode_string(&mut cursor)?,
        },
        3 => CompletionStatus::FatalFailure {
            code: decode_string(&mut cursor)?,
        },
        _ => return Err("invalid completion status"),
    };
    let finished_at = read_u64(&mut cursor)?;
    Ok(CompletionEvent {
        op_id,
        task,
        source,
        status,
        finished_at,
    })
}

struct Cursor<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> Cursor<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }

    fn take(&mut self, len: usize) -> Result<&'a [u8], &'static str> {
        if self.offset + len > self.bytes.len() {
            return Err("slot underflow");
        }
        let start = self.offset;
        self.offset += len;
        Ok(&self.bytes[start..start + len])
    }
}

fn write_u8(buf: &mut Vec<u8>, value: u8) {
    buf.push(value);
}

fn write_u32(buf: &mut Vec<u8>, value: u32) {
    buf.extend_from_slice(&value.to_le_bytes());
}

fn write_u64(buf: &mut Vec<u8>, value: u64) {
    buf.extend_from_slice(&value.to_le_bytes());
}

fn read_u8(cursor: &mut Cursor<'_>) -> Result<u8, &'static str> {
    Ok(cursor.take(1)?[0])
}

fn read_u32(cursor: &mut Cursor<'_>) -> Result<u32, &'static str> {
    let bytes = cursor.take(4)?;
    let mut arr = [0; 4];
    arr.copy_from_slice(bytes);
    Ok(u32::from_le_bytes(arr))
}

fn read_u64(cursor: &mut Cursor<'_>) -> Result<u64, &'static str> {
    let bytes = cursor.take(8)?;
    let mut arr = [0; 8];
    arr.copy_from_slice(bytes);
    Ok(u64::from_le_bytes(arr))
}

fn encode_string(buf: &mut Vec<u8>, value: &str) -> Result<(), &'static str> {
    if value.len() > u8::MAX as usize {
        return Err("string too long for slot encoding");
    }
    write_u8(buf, value.len() as u8);
    buf.extend_from_slice(value.as_bytes());
    Ok(())
}

fn decode_string(cursor: &mut Cursor<'_>) -> Result<String, &'static str> {
    let len = read_u8(cursor)? as usize;
    let bytes = cursor.take(len)?;
    std::str::from_utf8(bytes)
        .map(|s| s.to_string())
        .map_err(|_| "invalid utf8 in slot")
}

fn encode_opt_string(buf: &mut Vec<u8>, value: Option<&str>) -> Result<(), &'static str> {
    match value {
        Some(value) => {
            write_u8(buf, 1);
            encode_string(buf, value)
        }
        None => {
            write_u8(buf, 0);
            Ok(())
        }
    }
}

fn decode_opt_string(cursor: &mut Cursor<'_>) -> Result<Option<String>, &'static str> {
    match read_u8(cursor)? {
        0 => Ok(None),
        1 => Ok(Some(decode_string(cursor)?)),
        _ => Err("invalid optional string flag"),
    }
}

fn encode_opt_segment(buf: &mut Vec<u8>, segment: Option<SegmentHandle>) {
    match segment {
        Some(segment) => {
            write_u8(buf, 1);
            write_u64(buf, segment.0);
        }
        None => write_u8(buf, 0),
    }
}

fn decode_opt_segment(cursor: &mut Cursor<'_>) -> Result<Option<SegmentHandle>, &'static str> {
    match read_u8(cursor)? {
        0 => Ok(None),
        1 => Ok(Some(SegmentHandle(read_u64(cursor)?))),
        _ => Err("invalid optional segment flag"),
    }
}

fn encode_task(buf: &mut Vec<u8>, task: &Option<TaskKey>) {
    match task {
        Some(task) => {
            write_u8(buf, 1);
            write_u32(buf, task.logical_system.0);
            for level in task.coord.levels {
                write_u32(buf, level);
            }
            write_u32(buf, task.scope_depth);
            write_u64(buf, task.task_id);
        }
        None => write_u8(buf, 0),
    }
}

fn decode_task(cursor: &mut Cursor<'_>) -> Result<Option<TaskKey>, &'static str> {
    match read_u8(cursor)? {
        0 => Ok(None),
        1 => {
            let logical_system = sim_core::LogicalSystemId(read_u32(cursor)?);
            let mut levels = [0u32; 8];
            for level in &mut levels {
                *level = read_u32(cursor)?;
            }
            let scope_depth = read_u32(cursor)?;
            let task_id = read_u64(cursor)?;
            Ok(Some(TaskKey {
                logical_system,
                coord: sim_core::HierarchyCoord { levels },
                scope_depth,
                task_id,
            }))
        }
        _ => Err("invalid task flag"),
    }
}

fn pad_slot(mut buf: Vec<u8>, slot_bytes: usize) -> Result<Vec<u8>, &'static str> {
    if buf.len() > slot_bytes {
        return Err("encoded payload exceeds slot size");
    }
    buf.resize(slot_bytes, 0);
    Ok(buf)
}
