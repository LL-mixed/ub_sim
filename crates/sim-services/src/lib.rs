//! Host-side service simulation entry points.

use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};

use serde::{Deserialize, Serialize};
use sim_core::{
    BlockHash, CompletionEvent, CompletionSource, CompletionStatus, SegmentHandle, ServiceOpHandle,
    SimTimestamp, TaskKey, TensorDType, TensorLayout,
};
use sim_runtime::{BlockReadReq, BlockService, BlockWriteReq};
use thiserror::Error;

#[derive(Debug, Clone)]
struct QueuedCompletion {
    ready_at: SimTimestamp,
    event: CompletionEvent,
}

fn drain_ready(queue: &mut VecDeque<QueuedCompletion>, now: SimTimestamp) -> Vec<CompletionEvent> {
    let mut ready = Vec::new();

    while matches!(queue.front(), Some(item) if item.ready_at <= now) {
        if let Some(item) = queue.pop_front() {
            ready.push(item.event);
        }
    }

    ready
}

pub mod block {
    use super::*;

    #[derive(Debug, Clone, Copy)]
    pub struct BlockServiceProfile {
        pub queue_depth: usize,
        pub read_hit_latency_us: SimTimestamp,
        pub read_miss_latency_us: SimTimestamp,
        pub write_latency_us: SimTimestamp,
        pub writeback_latency_us: SimTimestamp,
    }

    impl Default for BlockServiceProfile {
        fn default() -> Self {
            Self {
                queue_depth: 16,
                read_hit_latency_us: 5,
                read_miss_latency_us: 30,
                write_latency_us: 8,
                writeback_latency_us: 20,
            }
        }
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum BlockState {
        Clean,
        Dirty,
    }

    #[derive(Debug)]
    pub struct BlockServiceStub {
        profile: BlockServiceProfile,
        blocks: HashMap<BlockHash, BlockState>,
        completions: VecDeque<QueuedCompletion>,
        next_op_id: u64,
    }

    impl BlockServiceStub {
        pub fn new() -> Self {
            Self::with_profile(BlockServiceProfile::default())
        }

        pub fn with_profile(profile: BlockServiceProfile) -> Self {
            Self {
                profile,
                blocks: HashMap::new(),
                completions: VecDeque::new(),
                next_op_id: 0,
            }
        }

        fn next_handle(&mut self) -> ServiceOpHandle {
            self.next_op_id += 1;
            ServiceOpHandle(self.next_op_id)
        }

        fn ensure_queue_capacity(&self) -> Result<(), sim_core::SimError> {
            if self.completions.len() >= self.profile.queue_depth {
                return Err(sim_core::SimError::InvalidInput("block queue full"));
            }
            Ok(())
        }

        pub fn submit_read(
            &mut self,
            req: BlockReadReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            let (status, ready_at) = if self.blocks.contains_key(&req.block) {
                (
                    CompletionStatus::Success,
                    now + self.profile.read_hit_latency_us,
                )
            } else {
                (
                    CompletionStatus::RetryableFailure {
                        code: "block_miss".to_string(),
                    },
                    now + self.profile.read_miss_latency_us,
                )
            };

            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task: req.task,
                    source: CompletionSource::BlockService,
                    status,
                    finished_at: ready_at,
                },
            });

            Ok(handle)
        }

        pub fn submit_write(
            &mut self,
            req: BlockWriteReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            self.blocks.insert(req.block, BlockState::Dirty);
            let ready_at = now + self.profile.write_latency_us;
            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task: req.task,
                    source: CompletionSource::BlockService,
                    status: CompletionStatus::Success,
                    finished_at: ready_at,
                },
            });
            Ok(handle)
        }

        pub fn submit_writeback(
            &mut self,
            block: BlockHash,
            task: Option<TaskKey>,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            let status = match self.blocks.get_mut(&block) {
                Some(state @ BlockState::Dirty) => {
                    *state = BlockState::Clean;
                    CompletionStatus::Success
                }
                Some(BlockState::Clean) => CompletionStatus::Success,
                None => CompletionStatus::RetryableFailure {
                    code: "writeback_missing_block".to_string(),
                },
            };
            let ready_at = now + self.profile.writeback_latency_us;
            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task,
                    source: CompletionSource::BlockService,
                    status,
                    finished_at: ready_at,
                },
            });
            Ok(handle)
        }

        pub fn poll_ready(&mut self, now: SimTimestamp) -> Vec<CompletionEvent> {
            drain_ready(&mut self.completions, now)
        }
    }

    impl BlockService for BlockServiceStub {
        fn read(&self, _req: BlockReadReq) -> Result<ServiceOpHandle, sim_core::SimError> {
            Err(sim_core::SimError::NotImplemented)
        }

        fn write(&self, _req: BlockWriteReq) -> Result<ServiceOpHandle, sim_core::SimError> {
            Err(sim_core::SimError::NotImplemented)
        }

        fn poll_completion(&self, _now: SimTimestamp) -> Vec<CompletionEvent> {
            Vec::new()
        }
    }
}

pub mod shmem {
    use super::*;

    pub const DEFAULT_MAX_SEGMENT_BYTES: u64 = 8 * 1024 * 1024;

    #[derive(Debug, Clone)]
    pub struct ShmemPutReq {
        pub task: Option<TaskKey>,
        pub requester_entity: u32,
        pub segment: SegmentHandle,
        pub bytes: u64,
    }

    #[derive(Debug, Clone)]
    pub struct ShmemGetReq {
        pub task: Option<TaskKey>,
        pub requester_entity: u32,
        pub segment: SegmentHandle,
        pub bytes: u64,
    }

    #[derive(Debug, Clone, Copy)]
    pub struct ShmemServiceProfile {
        pub default_latency_us: SimTimestamp,
        pub max_segment_bytes: u64,
        pub max_segments: usize,
        pub peer_count: u32,
        pub queue_depth: usize,
    }

    impl Default for ShmemServiceProfile {
        fn default() -> Self {
            Self {
                default_latency_us: 3,
                max_segment_bytes: DEFAULT_MAX_SEGMENT_BYTES,
                max_segments: 64,
                peer_count: 2,
                queue_depth: 16,
            }
        }
    }

    #[derive(Debug, Clone, Copy)]
    struct SegmentMeta {
        owner_entity: u32,
        bytes: u64,
    }

    #[derive(Debug)]
    pub struct ShmemServiceStub {
        profile: ShmemServiceProfile,
        segments: HashMap<SegmentHandle, SegmentMeta>,
        completions: VecDeque<QueuedCompletion>,
        next_op_id: u64,
    }

    impl ShmemServiceStub {
        pub fn new(profile: ShmemServiceProfile) -> Self {
            Self {
                profile,
                segments: HashMap::new(),
                completions: VecDeque::new(),
                next_op_id: 0,
            }
        }

        fn next_handle(&mut self) -> ServiceOpHandle {
            self.next_op_id += 1;
            ServiceOpHandle(self.next_op_id)
        }

        fn ensure_queue_capacity(&self) -> Result<(), sim_core::SimError> {
            if self.completions.len() >= self.profile.queue_depth {
                return Err(sim_core::SimError::InvalidInput("shmem queue full"));
            }
            Ok(())
        }

        pub fn register_segment(
            &mut self,
            segment: SegmentHandle,
            owner_entity: u32,
            bytes: u64,
        ) -> Result<(), sim_core::SimError> {
            if bytes == 0 {
                return Err(sim_core::SimError::InvalidInput(
                    "shmem segment bytes must be positive",
                ));
            }
            if bytes > self.profile.max_segment_bytes {
                return Err(sim_core::SimError::InvalidInput(
                    "shmem segment exceeds size limit",
                ));
            }
            if !self.segments.contains_key(&segment)
                && self.segments.len() >= self.profile.max_segments
            {
                return Err(sim_core::SimError::InvalidInput("shmem segment table full"));
            }
            self.segments.insert(
                segment,
                SegmentMeta {
                    owner_entity,
                    bytes,
                },
            );
            Ok(())
        }

        fn check_access(
            &self,
            segment: SegmentHandle,
            requester_entity: u32,
            bytes: u64,
        ) -> CompletionStatus {
            match self.segments.get(&segment) {
                None => CompletionStatus::RetryableFailure {
                    code: "missing_segment".to_string(),
                },
                Some(meta) if bytes > meta.bytes => CompletionStatus::RetryableFailure {
                    code: "short_segment".to_string(),
                },
                Some(meta)
                    if requester_entity != meta.owner_entity
                        && requester_entity >= self.profile.peer_count =>
                {
                    CompletionStatus::FatalFailure {
                        code: "shmem_access_denied".to_string(),
                    }
                }
                Some(_) => CompletionStatus::Success,
            }
        }

        pub fn submit_put(
            &mut self,
            req: ShmemPutReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            let status = self.check_access(req.segment, req.requester_entity, req.bytes);
            let ready_at = now + self.profile.default_latency_us;
            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task: req.task,
                    source: CompletionSource::ShmemService,
                    status,
                    finished_at: ready_at,
                },
            });
            Ok(handle)
        }

        pub fn submit_get(
            &mut self,
            req: ShmemGetReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            let status = self.check_access(req.segment, req.requester_entity, req.bytes);
            let ready_at = now + self.profile.default_latency_us;
            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task: req.task,
                    source: CompletionSource::ShmemService,
                    status,
                    finished_at: ready_at,
                },
            });
            Ok(handle)
        }

        pub fn poll_ready(&mut self, now: SimTimestamp) -> Vec<CompletionEvent> {
            drain_ready(&mut self.completions, now)
        }
    }
}

pub mod dfs {
    use super::*;

    #[derive(Debug, Clone, Copy)]
    pub struct DfsServiceProfile {
        pub metadata_latency_us: SimTimestamp,
        pub data_latency_us: SimTimestamp,
        pub cold_metadata_penalty_us: SimTimestamp,
        pub cold_data_penalty_us: SimTimestamp,
        pub queue_depth: usize,
    }

    impl Default for DfsServiceProfile {
        fn default() -> Self {
            Self {
                metadata_latency_us: 20,
                data_latency_us: 80,
                cold_metadata_penalty_us: 15,
                cold_data_penalty_us: 60,
                queue_depth: 16,
            }
        }
    }

    #[derive(Debug, Clone)]
    pub struct DfsReadReq {
        pub task: Option<TaskKey>,
        pub path: String,
    }

    #[derive(Debug, Clone)]
    pub struct DfsWriteReq {
        pub task: Option<TaskKey>,
        pub path: String,
        pub bytes: u64,
    }

    #[derive(Debug, Clone, Copy)]
    struct FileMeta {
        bytes: u64,
        warm: bool,
    }

    #[derive(Debug)]
    pub struct DfsServiceStub {
        profile: DfsServiceProfile,
        files: HashMap<String, FileMeta>,
        completions: VecDeque<QueuedCompletion>,
        next_op_id: u64,
    }

    impl DfsServiceStub {
        pub fn new(profile: DfsServiceProfile) -> Self {
            Self {
                profile,
                files: HashMap::new(),
                completions: VecDeque::new(),
                next_op_id: 0,
            }
        }

        fn next_handle(&mut self) -> ServiceOpHandle {
            self.next_op_id += 1;
            ServiceOpHandle(self.next_op_id)
        }

        fn ensure_queue_capacity(&self) -> Result<(), sim_core::SimError> {
            if self.completions.len() >= self.profile.queue_depth {
                return Err(sim_core::SimError::InvalidInput("dfs queue full"));
            }
            Ok(())
        }

        pub fn submit_write(
            &mut self,
            req: DfsWriteReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            self.files.insert(
                req.path,
                FileMeta {
                    bytes: req.bytes,
                    warm: false,
                },
            );
            let ready_at = now + self.profile.metadata_latency_us + self.profile.data_latency_us;
            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task: req.task,
                    source: CompletionSource::DfsService,
                    status: CompletionStatus::Success,
                    finished_at: ready_at,
                },
            });
            Ok(handle)
        }

        pub fn submit_read(
            &mut self,
            req: DfsReadReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            let (status, ready_at) = match self.files.get_mut(&req.path) {
                Some(meta) => {
                    let _bytes = meta.bytes;
                    let penalty = if meta.warm {
                        0
                    } else {
                        meta.warm = true;
                        self.profile.cold_metadata_penalty_us + self.profile.cold_data_penalty_us
                    };
                    (
                        CompletionStatus::Success,
                        now + self.profile.metadata_latency_us
                            + self.profile.data_latency_us
                            + penalty,
                    )
                }
                None => (
                    CompletionStatus::RetryableFailure {
                        code: "missing_path".to_string(),
                    },
                    now + self.profile.metadata_latency_us,
                ),
            };
            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task: req.task,
                    source: CompletionSource::DfsService,
                    status,
                    finished_at: ready_at,
                },
            });
            Ok(handle)
        }

        pub fn poll_ready(&mut self, now: SimTimestamp) -> Vec<CompletionEvent> {
            drain_ready(&mut self.completions, now)
        }
    }
}

pub mod durable {
    use super::*;

    pub const LINGQU_DURABLE_SIM_KIND: &str = "lingqu_durable_sim";
    pub const LINGQU_DURABLE_SIM_SCHEMA_VERSION: u32 = 1;

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDfsPath {
        pub path: String,
    }

    impl LingquDfsPath {
        pub fn new(path: impl Into<String>) -> Self {
            Self { path: path.into() }
        }
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquBlockPayloadRef {
        pub block: BlockHash,
        pub version: u64,
        pub offset: u64,
        pub bytes: u64,
        pub checksum: u64,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub enum LingquVersionSelector {
        LatestCommitted,
        Exact(u64),
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub enum LingquDfsContentType {
        Json,
        Binary,
        Text,
        Manifest,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub enum LingquDfsFileState {
        Committed,
        Tombstoned,
        Quarantined,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub enum LingquBlockDurableState {
        Committed,
        Sealed,
        Tombstoned,
        Quarantined,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub enum LingquBlockCacheState {
        Clean,
        Dirty,
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub enum LingquDfsContentRef {
        Inline(Vec<u8>),
        Block(LingquBlockPayloadRef),
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDfsFileRecord {
        pub path: String,
        pub version: u64,
        pub state: LingquDfsFileState,
        pub content_ref: LingquDfsContentRef,
        pub bytes: u64,
        pub checksum: u64,
        pub content_type: LingquDfsContentType,
        pub created_at_us: u64,
        pub updated_at_us: u64,
        pub writer: Option<String>,
        pub metadata: BTreeMap<String, String>,
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDfsDirectoryRecord {
        pub path: String,
        pub created_at_us: u64,
        pub updated_at_us: u64,
        pub metadata: BTreeMap<String, String>,
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDfsAppendLogRecord {
        pub path: String,
        pub seq: u64,
        pub bytes: Vec<u8>,
        pub checksum: u64,
        pub prev_chain_checksum: u64,
        pub chain_checksum: u64,
        pub created_at_us: u64,
        pub writer: Option<String>,
        pub metadata: BTreeMap<String, String>,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct LingquDfsAppendOptions {
        pub expected_next_seq: Option<u64>,
        pub writer: Option<String>,
        pub metadata: BTreeMap<String, String>,
    }

    impl Default for LingquDfsAppendOptions {
        fn default() -> Self {
            Self {
                expected_next_seq: None,
                writer: None,
                metadata: BTreeMap::new(),
            }
        }
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct LingquDfsListOptions {
        pub prefix: String,
        pub include_tombstoned: bool,
    }

    impl LingquDfsListOptions {
        pub fn new(prefix: impl Into<String>) -> Self {
            Self {
                prefix: prefix.into(),
                include_tombstoned: false,
            }
        }
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDfsListEntry {
        pub path: String,
        pub version: u64,
        pub state: LingquDfsFileState,
        pub bytes: u64,
        pub checksum: u64,
        pub updated_at_us: u64,
        pub content_type: LingquDfsContentType,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub enum LingquDurableBatchOp {
        DfsWrite {
            path: String,
            bytes: Vec<u8>,
            options: LingquDfsWriteOptions,
        },
        DfsAppendLog {
            path: String,
            bytes: Vec<u8>,
            options: LingquDfsAppendOptions,
        },
        BlockWrite {
            block: String,
            bytes: Vec<u8>,
            options: LingquBlockWriteOptions,
        },
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub enum LingquDurableBatchOutcome {
        DfsPath(LingquDfsPath),
        DfsAppendLog(LingquDfsAppendLogRecord),
        BlockPayloadRef(LingquBlockPayloadRef),
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquBlockRecord {
        pub block: BlockHash,
        pub version: u64,
        pub durable_state: LingquBlockDurableState,
        pub cache_state: LingquBlockCacheState,
        pub bytes: Vec<u8>,
        pub checksum: u64,
        pub created_at_us: u64,
        pub updated_at_us: u64,
        pub writer: Option<String>,
        pub metadata: BTreeMap<String, String>,
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDfsSimSnapshot {
        pub files: Vec<LingquDfsFileRecord>,
        pub directories: Vec<LingquDfsDirectoryRecord>,
        #[serde(default)]
        pub append_logs: Vec<LingquDfsAppendLogRecord>,
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquBlockSimSnapshot {
        pub blocks: Vec<LingquBlockRecord>,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDfsTimingProfile {
        pub metadata_latency_us: SimTimestamp,
        pub data_latency_us: SimTimestamp,
        pub cold_metadata_penalty_us: SimTimestamp,
        pub cold_data_penalty_us: SimTimestamp,
        pub queue_depth: usize,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquBlockTimingProfile {
        pub queue_depth: usize,
        pub read_hit_latency_us: SimTimestamp,
        pub read_miss_latency_us: SimTimestamp,
        pub write_latency_us: SimTimestamp,
        pub writeback_latency_us: SimTimestamp,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDurableSimProfile {
        pub dfs: LingquDfsTimingProfile,
        pub block: LingquBlockTimingProfile,
        pub default_inline_threshold_bytes: usize,
    }

    impl Default for LingquDurableSimProfile {
        fn default() -> Self {
            Self {
                dfs: dfs::DfsServiceProfile::default().into(),
                block: block::BlockServiceProfile::default().into(),
                default_inline_threshold_bytes: 4096,
            }
        }
    }

    impl From<dfs::DfsServiceProfile> for LingquDfsTimingProfile {
        fn from(profile: dfs::DfsServiceProfile) -> Self {
            Self {
                metadata_latency_us: profile.metadata_latency_us,
                data_latency_us: profile.data_latency_us,
                cold_metadata_penalty_us: profile.cold_metadata_penalty_us,
                cold_data_penalty_us: profile.cold_data_penalty_us,
                queue_depth: profile.queue_depth,
            }
        }
    }

    impl From<LingquDfsTimingProfile> for dfs::DfsServiceProfile {
        fn from(profile: LingquDfsTimingProfile) -> Self {
            Self {
                metadata_latency_us: profile.metadata_latency_us,
                data_latency_us: profile.data_latency_us,
                cold_metadata_penalty_us: profile.cold_metadata_penalty_us,
                cold_data_penalty_us: profile.cold_data_penalty_us,
                queue_depth: profile.queue_depth,
            }
        }
    }

    impl From<block::BlockServiceProfile> for LingquBlockTimingProfile {
        fn from(profile: block::BlockServiceProfile) -> Self {
            Self {
                queue_depth: profile.queue_depth,
                read_hit_latency_us: profile.read_hit_latency_us,
                read_miss_latency_us: profile.read_miss_latency_us,
                write_latency_us: profile.write_latency_us,
                writeback_latency_us: profile.writeback_latency_us,
            }
        }
    }

    impl From<LingquBlockTimingProfile> for block::BlockServiceProfile {
        fn from(profile: LingquBlockTimingProfile) -> Self {
            Self {
                queue_depth: profile.queue_depth,
                read_hit_latency_us: profile.read_hit_latency_us,
                read_miss_latency_us: profile.read_miss_latency_us,
                write_latency_us: profile.write_latency_us,
                writeback_latency_us: profile.writeback_latency_us,
            }
        }
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDurableSimSnapshot {
        pub kind: String,
        pub schema_version: u32,
        pub profile: LingquDurableSimProfile,
        pub dfs: LingquDfsSimSnapshot,
        pub block: LingquBlockSimSnapshot,
        pub next_timestamp_us: u64,
        pub checksum: u64,
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct LingquDfsWriteOptions {
        pub expected_version: Option<u64>,
        pub content_type: LingquDfsContentType,
        pub writer: Option<String>,
        pub metadata: BTreeMap<String, String>,
        pub inline_threshold_bytes: usize,
    }

    impl Default for LingquDfsWriteOptions {
        fn default() -> Self {
            Self {
                expected_version: None,
                content_type: LingquDfsContentType::Binary,
                writer: None,
                metadata: BTreeMap::new(),
                inline_threshold_bytes: LingquDurableSimProfile::default()
                    .default_inline_threshold_bytes,
            }
        }
    }

    #[derive(Debug, Clone, PartialEq, Eq)]
    pub struct LingquBlockWriteOptions {
        pub expected_version: Option<u64>,
        pub seal: bool,
        pub writer: Option<String>,
        pub metadata: BTreeMap<String, String>,
    }

    impl Default for LingquBlockWriteOptions {
        fn default() -> Self {
            Self {
                expected_version: None,
                seal: false,
                writer: None,
                metadata: BTreeMap::new(),
            }
        }
    }

    #[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDurableStats {
        pub dfs_reads: u64,
        pub dfs_writes: u64,
        pub dfs_bytes_read: u64,
        pub dfs_bytes_written: u64,
        pub block_reads: u64,
        pub block_writes: u64,
        pub block_bytes_read: u64,
        pub block_bytes_written: u64,
        pub checksum_failures: u64,
        pub version_conflicts: u64,
        pub missing_refs: u64,
    }

    #[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquDurableValidationReport {
        pub dfs_file_versions: usize,
        pub dfs_append_records: usize,
        pub block_versions: usize,
        pub missing_block_refs: Vec<String>,
        pub orphan_blocks: Vec<String>,
        pub append_log_paths: Vec<String>,
        pub checksum: u64,
    }

    #[derive(Debug, Error, Clone, PartialEq, Eq)]
    pub enum LingquDurableError {
        #[error("invalid DFS path: {0}")]
        InvalidPath(String),
        #[error("invalid block: {0}")]
        InvalidBlock(String),
        #[error("empty payload")]
        EmptyPayload,
        #[error("missing DFS path: {0}")]
        MissingDfsPath(String),
        #[error("missing block: {0}")]
        MissingBlock(String),
        #[error("version conflict for {id}: expected {expected:?}, actual {actual:?}")]
        VersionConflict {
            id: String,
            expected: Option<u64>,
            actual: Option<u64>,
        },
        #[error("checksum mismatch for {id}: expected {expected:#x}, actual {actual:#x}")]
        ChecksumMismatch {
            id: String,
            expected: u64,
            actual: u64,
        },
        #[error("range overflow for {0}")]
        RangeOverflow(String),
        #[error("tombstoned ref: {0}")]
        Tombstoned(String),
        #[error("quarantined ref: {0}")]
        Quarantined(String),
        #[error("sealed block: {0}")]
        Sealed(String),
        #[error("queue full: {0}")]
        QueueFull(String),
        #[error("snapshot codec failed: {0}")]
        SnapshotCodec(String),
        #[error("snapshot validation failed: {0}")]
        SnapshotValidation(String),
    }

    pub type LingquDurableResult<T> = Result<T, LingquDurableError>;

    #[derive(Debug)]
    pub struct LingquDurableSim {
        profile: LingquDurableSimProfile,
        dfs_service: dfs::DfsServiceStub,
        block_service: block::BlockServiceStub,
        dfs_files: Vec<LingquDfsFileRecord>,
        dfs_directories: Vec<LingquDfsDirectoryRecord>,
        dfs_append_logs: Vec<LingquDfsAppendLogRecord>,
        block_records: Vec<LingquBlockRecord>,
        next_timestamp_us: SimTimestamp,
        stats: LingquDurableStats,
    }

    impl LingquDurableSim {
        pub fn new(profile: LingquDurableSimProfile) -> Self {
            Self {
                dfs_service: dfs::DfsServiceStub::new(profile.dfs.into()),
                block_service: block::BlockServiceStub::with_profile(profile.block.into()),
                profile,
                dfs_files: Vec::new(),
                dfs_directories: Vec::new(),
                dfs_append_logs: Vec::new(),
                block_records: Vec::new(),
                next_timestamp_us: 1,
                stats: LingquDurableStats::default(),
            }
        }

        pub fn export_snapshot(&self) -> LingquDurableResult<LingquDurableSimSnapshot> {
            let mut dfs_files = self.dfs_files.clone();
            dfs_files.sort_by(|left, right| {
                left.path
                    .cmp(&right.path)
                    .then_with(|| left.version.cmp(&right.version))
            });
            let mut dfs_directories = self.dfs_directories.clone();
            dfs_directories.sort_by(|left, right| left.path.cmp(&right.path));
            let mut append_logs = self.dfs_append_logs.clone();
            append_logs.sort_by(|left, right| {
                left.path
                    .cmp(&right.path)
                    .then_with(|| left.seq.cmp(&right.seq))
            });
            let mut block_records = self.block_records.clone();
            block_records.sort_by(|left, right| {
                left.block
                    .0
                    .cmp(&right.block.0)
                    .then_with(|| left.version.cmp(&right.version))
            });
            let mut snapshot = LingquDurableSimSnapshot {
                kind: LINGQU_DURABLE_SIM_KIND.to_string(),
                schema_version: LINGQU_DURABLE_SIM_SCHEMA_VERSION,
                profile: self.profile,
                dfs: LingquDfsSimSnapshot {
                    files: dfs_files,
                    directories: dfs_directories,
                    append_logs,
                },
                block: LingquBlockSimSnapshot {
                    blocks: block_records,
                },
                next_timestamp_us: self.next_timestamp_us,
                checksum: 0,
            };
            snapshot.checksum = durable_snapshot_checksum(&snapshot);
            snapshot.validate()?;
            Ok(snapshot)
        }

        pub fn import_snapshot(snapshot: LingquDurableSimSnapshot) -> LingquDurableResult<Self> {
            snapshot.validate()?;
            let mut sim = Self::new(snapshot.profile);
            sim.next_timestamp_us = snapshot.next_timestamp_us;
            sim.dfs_directories = snapshot.dfs.directories;
            sim.dfs_append_logs = snapshot.dfs.append_logs;
            sim.block_records = snapshot.block.blocks;
            sim.dfs_files = snapshot.dfs.files;
            sim.rebuild_service_indexes()?;
            Ok(sim)
        }

        pub fn stats(&self) -> LingquDurableStats {
            self.stats
        }

        pub fn dfs_write(
            &mut self,
            path: impl Into<String>,
            bytes: Vec<u8>,
            mut options: LingquDfsWriteOptions,
        ) -> LingquDurableResult<LingquDfsPath> {
            let path = path.into();
            validate_dfs_path(&path)?;
            if bytes.is_empty() {
                return Err(LingquDurableError::EmptyPayload);
            }
            if options.inline_threshold_bytes == 0 {
                options.inline_threshold_bytes = self.profile.default_inline_threshold_bytes;
            }
            let current_version = self
                .latest_dfs_any_state(&path)
                .map(|record| record.version);
            self.check_expected_version(&path, current_version, options.expected_version)?;
            let version = current_version.unwrap_or(0).saturating_add(1);
            let content_ref = if bytes.len() <= options.inline_threshold_bytes {
                LingquDfsContentRef::Inline(bytes.clone())
            } else {
                let block = generated_dfs_block_id(&path, version);
                let payload_ref = self.block_write(
                    block,
                    bytes.clone(),
                    LingquBlockWriteOptions {
                        expected_version: None,
                        seal: true,
                        writer: options.writer.clone(),
                        metadata: options.metadata.clone(),
                    },
                )?;
                LingquDfsContentRef::Block(payload_ref)
            };
            let checksum = checksum64(&bytes);
            let now = self.next_timestamp();
            let record = LingquDfsFileRecord {
                path: path.clone(),
                version,
                state: LingquDfsFileState::Committed,
                content_ref,
                bytes: bytes.len() as u64,
                checksum,
                content_type: options.content_type,
                created_at_us: now,
                updated_at_us: now,
                writer: options.writer,
                metadata: options.metadata,
            };
            validate_dfs_file_record(&record, &self.block_records)?;
            self.submit_dfs_write(path.clone(), bytes.len() as u64)?;
            self.stats.dfs_writes += 1;
            self.stats.dfs_bytes_written += bytes.len() as u64;
            self.dfs_files.push(record);
            Ok(LingquDfsPath::new(path))
        }

        pub fn dfs_read(
            &mut self,
            path: &str,
            selector: LingquVersionSelector,
        ) -> LingquDurableResult<Vec<u8>> {
            validate_dfs_path(path)?;
            let record = self.select_dfs_record(path, selector)?.clone();
            self.submit_dfs_read(path)?;
            let bytes = match &record.content_ref {
                LingquDfsContentRef::Inline(bytes) => bytes.clone(),
                LingquDfsContentRef::Block(payload_ref) => self.block_read(payload_ref)?,
            };
            let actual = checksum64(&bytes);
            if actual != record.checksum {
                self.stats.checksum_failures += 1;
                return Err(LingquDurableError::ChecksumMismatch {
                    id: path.to_string(),
                    expected: record.checksum,
                    actual,
                });
            }
            self.stats.dfs_reads += 1;
            self.stats.dfs_bytes_read += bytes.len() as u64;
            Ok(bytes)
        }

        pub fn dfs_stat(
            &self,
            path: &str,
            selector: LingquVersionSelector,
        ) -> LingquDurableResult<LingquDfsFileRecord> {
            validate_dfs_path(path)?;
            self.select_dfs_record(path, selector).cloned()
        }

        pub fn dfs_list(
            &self,
            options: LingquDfsListOptions,
        ) -> LingquDurableResult<Vec<LingquDfsListEntry>> {
            validate_dfs_prefix(&options.prefix)?;
            let mut latest_by_path = BTreeMap::<String, &LingquDfsFileRecord>::new();
            for record in &self.dfs_files {
                if !record.path.starts_with(&options.prefix) {
                    continue;
                }
                let replace = latest_by_path
                    .get(&record.path)
                    .map(|existing| existing.version < record.version)
                    .unwrap_or(true);
                if replace {
                    latest_by_path.insert(record.path.clone(), record);
                }
            }
            let mut entries = Vec::new();
            for record in latest_by_path.values() {
                if record.state != LingquDfsFileState::Committed && !options.include_tombstoned {
                    continue;
                }
                entries.push(LingquDfsListEntry {
                    path: record.path.clone(),
                    version: record.version,
                    state: record.state,
                    bytes: record.bytes,
                    checksum: record.checksum,
                    updated_at_us: record.updated_at_us,
                    content_type: record.content_type,
                });
            }
            entries.sort_by(|left, right| left.path.cmp(&right.path));
            Ok(entries)
        }

        pub fn dfs_tombstone(
            &mut self,
            path: &str,
            expected_version: Option<u64>,
        ) -> LingquDurableResult<()> {
            validate_dfs_path(path)?;
            let current_version = self.latest_dfs_any_state(path).map(|record| record.version);
            self.check_expected_version(path, current_version, expected_version)?;
            let version = current_version
                .ok_or_else(|| LingquDurableError::MissingDfsPath(path.to_string()))?
                .saturating_add(1);
            let now = self.next_timestamp();
            self.dfs_files.push(LingquDfsFileRecord {
                path: path.to_string(),
                version,
                state: LingquDfsFileState::Tombstoned,
                content_ref: LingquDfsContentRef::Inline(Vec::new()),
                bytes: 0,
                checksum: checksum64(&[]),
                content_type: LingquDfsContentType::Manifest,
                created_at_us: now,
                updated_at_us: now,
                writer: None,
                metadata: BTreeMap::new(),
            });
            Ok(())
        }

        pub fn dfs_append_log_append(
            &mut self,
            path: impl Into<String>,
            bytes: Vec<u8>,
            options: LingquDfsAppendOptions,
        ) -> LingquDurableResult<LingquDfsAppendLogRecord> {
            let path = path.into();
            validate_dfs_path(&path)?;
            if bytes.is_empty() {
                return Err(LingquDurableError::EmptyPayload);
            }
            let (latest_seq, prev_chain_checksum) = self
                .latest_append_log_record(&path)
                .map(|record| (record.seq, record.chain_checksum))
                .unwrap_or((0, 0));
            let next_seq = latest_seq.saturating_add(1);
            self.check_expected_version(&path, Some(next_seq), options.expected_next_seq)?;
            let checksum = checksum64(&bytes);
            let chain_checksum =
                append_log_chain_checksum(&path, next_seq, checksum, prev_chain_checksum, &bytes);
            let now = self.next_timestamp();
            let record = LingquDfsAppendLogRecord {
                path: path.clone(),
                seq: next_seq,
                bytes,
                checksum,
                prev_chain_checksum,
                chain_checksum,
                created_at_us: now,
                writer: options.writer,
                metadata: options.metadata,
            };
            validate_append_log_record(&record)?;
            self.submit_dfs_write(path, record.bytes.len() as u64)?;
            self.stats.dfs_writes += 1;
            self.stats.dfs_bytes_written += record.bytes.len() as u64;
            self.dfs_append_logs.push(record.clone());
            Ok(record)
        }

        pub fn dfs_append_log_read(
            &mut self,
            path: &str,
            start_seq: u64,
            max_records: Option<usize>,
        ) -> LingquDurableResult<Vec<LingquDfsAppendLogRecord>> {
            validate_dfs_path(path)?;
            let max_records = max_records.unwrap_or(usize::MAX);
            let mut records = self
                .dfs_append_logs
                .iter()
                .filter(|record| record.path == path && record.seq >= start_seq)
                .cloned()
                .collect::<Vec<_>>();
            records.sort_by_key(|record| record.seq);
            records.truncate(max_records);
            if records.is_empty() {
                return Err(LingquDurableError::MissingDfsPath(path.to_string()));
            }
            self.submit_dfs_read(path)?;
            self.stats.dfs_reads += 1;
            self.stats.dfs_bytes_read += records
                .iter()
                .map(|record| record.bytes.len() as u64)
                .sum::<u64>();
            Ok(records)
        }

        pub fn validate_store(&self) -> LingquDurableResult<LingquDurableValidationReport> {
            let snapshot = self.export_snapshot()?;
            snapshot.validation_report()
        }

        pub fn commit_batch(
            &mut self,
            ops: Vec<LingquDurableBatchOp>,
        ) -> LingquDurableResult<Vec<LingquDurableBatchOutcome>> {
            if ops.is_empty() {
                return Err(LingquDurableError::SnapshotValidation(
                    "durable batch must contain at least one operation".to_string(),
                ));
            }
            let original_stats = self.stats;
            let snapshot = self.export_snapshot()?;
            let mut staged = Self::import_snapshot(snapshot)?;
            let mut outcomes = Vec::with_capacity(ops.len());
            for op in ops {
                let outcome = match op {
                    LingquDurableBatchOp::DfsWrite {
                        path,
                        bytes,
                        options,
                    } => {
                        LingquDurableBatchOutcome::DfsPath(staged.dfs_write(path, bytes, options)?)
                    }
                    LingquDurableBatchOp::DfsAppendLog {
                        path,
                        bytes,
                        options,
                    } => LingquDurableBatchOutcome::DfsAppendLog(
                        staged.dfs_append_log_append(path, bytes, options)?,
                    ),
                    LingquDurableBatchOp::BlockWrite {
                        block,
                        bytes,
                        options,
                    } => LingquDurableBatchOutcome::BlockPayloadRef(
                        staged.block_write(block, bytes, options)?,
                    ),
                };
                outcomes.push(outcome);
            }
            staged.stats = add_durable_stats(original_stats, staged.stats);
            *self = staged;
            Ok(outcomes)
        }

        pub fn block_write(
            &mut self,
            block: impl Into<String>,
            bytes: Vec<u8>,
            options: LingquBlockWriteOptions,
        ) -> LingquDurableResult<LingquBlockPayloadRef> {
            let block = BlockHash(block.into());
            validate_block(&block)?;
            if bytes.is_empty() {
                return Err(LingquDurableError::EmptyPayload);
            }
            let current_record = self.latest_block_any_state(&block);
            if matches!(
                current_record.map(|record| record.durable_state),
                Some(LingquBlockDurableState::Sealed)
            ) {
                return Err(LingquDurableError::Sealed(block.0));
            }
            let current_version = current_record.map(|record| record.version);
            self.check_expected_version(&block.0, current_version, options.expected_version)?;
            let version = current_version.unwrap_or(0).saturating_add(1);
            let checksum = checksum64(&bytes);
            let now = self.next_timestamp();
            self.submit_block_write(block.clone())?;
            let record = LingquBlockRecord {
                block: block.clone(),
                version,
                durable_state: if options.seal {
                    LingquBlockDurableState::Sealed
                } else {
                    LingquBlockDurableState::Committed
                },
                cache_state: LingquBlockCacheState::Dirty,
                bytes: bytes.clone(),
                checksum,
                created_at_us: now,
                updated_at_us: now,
                writer: options.writer,
                metadata: options.metadata,
            };
            validate_block_record(&record)?;
            self.stats.block_writes += 1;
            self.stats.block_bytes_written += bytes.len() as u64;
            self.block_records.push(record);
            Ok(LingquBlockPayloadRef {
                block,
                version,
                offset: 0,
                bytes: bytes.len() as u64,
                checksum,
            })
        }

        pub fn block_read(
            &mut self,
            payload_ref: &LingquBlockPayloadRef,
        ) -> LingquDurableResult<Vec<u8>> {
            validate_block_payload_ref(payload_ref)?;
            let record = self
                .block_records
                .iter()
                .find(|record| {
                    record.block == payload_ref.block && record.version == payload_ref.version
                })
                .cloned()
                .ok_or_else(|| {
                    self.stats.missing_refs += 1;
                    LingquDurableError::MissingBlock(payload_ref.block.0.clone())
                })?;
            match record.durable_state {
                LingquBlockDurableState::Committed | LingquBlockDurableState::Sealed => {}
                LingquBlockDurableState::Tombstoned => {
                    return Err(LingquDurableError::Tombstoned(record.block.0));
                }
                LingquBlockDurableState::Quarantined => {
                    return Err(LingquDurableError::Quarantined(record.block.0));
                }
            }
            self.submit_block_read(payload_ref.block.clone())?;
            let start = payload_ref.offset as usize;
            let end = payload_ref
                .offset
                .checked_add(payload_ref.bytes)
                .ok_or_else(|| LingquDurableError::RangeOverflow(payload_ref.block.0.clone()))?
                as usize;
            if end > record.bytes.len() {
                return Err(LingquDurableError::RangeOverflow(
                    payload_ref.block.0.clone(),
                ));
            }
            let selected = record.bytes[start..end].to_vec();
            let actual = checksum64(&selected);
            if actual != payload_ref.checksum {
                self.stats.checksum_failures += 1;
                return Err(LingquDurableError::ChecksumMismatch {
                    id: payload_ref.block.0.clone(),
                    expected: payload_ref.checksum,
                    actual,
                });
            }
            self.stats.block_reads += 1;
            self.stats.block_bytes_read += selected.len() as u64;
            Ok(selected)
        }

        pub fn block_stat(
            &self,
            block: &BlockHash,
            selector: LingquVersionSelector,
        ) -> LingquDurableResult<LingquBlockRecord> {
            validate_block(block)?;
            self.select_block_record(block, selector).cloned()
        }

        pub fn block_seal(
            &mut self,
            block: &BlockHash,
            expected_version: Option<u64>,
        ) -> LingquDurableResult<()> {
            validate_block(block)?;
            let latest = self
                .latest_block_any_state(block)
                .ok_or_else(|| LingquDurableError::MissingBlock(block.0.clone()))?
                .clone();
            self.check_expected_version(&block.0, Some(latest.version), expected_version)?;
            if latest.durable_state == LingquBlockDurableState::Sealed {
                return Ok(());
            }
            let version = latest.version.saturating_add(1);
            let now = self.next_timestamp();
            self.block_records.push(LingquBlockRecord {
                block: block.clone(),
                version,
                durable_state: LingquBlockDurableState::Sealed,
                cache_state: latest.cache_state,
                bytes: latest.bytes,
                checksum: latest.checksum,
                created_at_us: now,
                updated_at_us: now,
                writer: latest.writer,
                metadata: latest.metadata,
            });
            Ok(())
        }

        pub fn block_tombstone(
            &mut self,
            block: &BlockHash,
            expected_version: Option<u64>,
        ) -> LingquDurableResult<()> {
            validate_block(block)?;
            let latest = self
                .latest_block_any_state(block)
                .ok_or_else(|| LingquDurableError::MissingBlock(block.0.clone()))?
                .clone();
            self.check_expected_version(&block.0, Some(latest.version), expected_version)?;
            let version = latest.version.saturating_add(1);
            let now = self.next_timestamp();
            self.block_records.push(LingquBlockRecord {
                block: block.clone(),
                version,
                durable_state: LingquBlockDurableState::Tombstoned,
                cache_state: latest.cache_state,
                bytes: latest.bytes,
                checksum: latest.checksum,
                created_at_us: now,
                updated_at_us: now,
                writer: latest.writer,
                metadata: latest.metadata,
            });
            Ok(())
        }

        fn latest_dfs_any_state(&self, path: &str) -> Option<&LingquDfsFileRecord> {
            self.dfs_files
                .iter()
                .filter(|record| record.path == path)
                .max_by_key(|record| record.version)
        }

        fn latest_block_any_state(&self, block: &BlockHash) -> Option<&LingquBlockRecord> {
            self.block_records
                .iter()
                .filter(|record| record.block == *block)
                .max_by_key(|record| record.version)
        }

        fn latest_append_log_record(&self, path: &str) -> Option<&LingquDfsAppendLogRecord> {
            self.dfs_append_logs
                .iter()
                .filter(|record| record.path == path)
                .max_by_key(|record| record.seq)
        }

        fn select_dfs_record(
            &self,
            path: &str,
            selector: LingquVersionSelector,
        ) -> LingquDurableResult<&LingquDfsFileRecord> {
            let record = match selector {
                LingquVersionSelector::LatestCommitted => self
                    .latest_dfs_any_state(path)
                    .ok_or_else(|| LingquDurableError::MissingDfsPath(path.to_string()))?,
                LingquVersionSelector::Exact(version) => self
                    .dfs_files
                    .iter()
                    .find(|record| record.path == path && record.version == version)
                    .ok_or_else(|| LingquDurableError::MissingDfsPath(path.to_string()))?,
            };
            match record.state {
                LingquDfsFileState::Committed => Ok(record),
                LingquDfsFileState::Tombstoned => {
                    Err(LingquDurableError::Tombstoned(path.to_string()))
                }
                LingquDfsFileState::Quarantined => {
                    Err(LingquDurableError::Quarantined(path.to_string()))
                }
            }
        }

        fn select_block_record(
            &self,
            block: &BlockHash,
            selector: LingquVersionSelector,
        ) -> LingquDurableResult<&LingquBlockRecord> {
            let record = match selector {
                LingquVersionSelector::LatestCommitted => self
                    .latest_block_any_state(block)
                    .ok_or_else(|| LingquDurableError::MissingBlock(block.0.clone()))?,
                LingquVersionSelector::Exact(version) => self
                    .block_records
                    .iter()
                    .find(|record| record.block == *block && record.version == version)
                    .ok_or_else(|| LingquDurableError::MissingBlock(block.0.clone()))?,
            };
            match record.durable_state {
                LingquBlockDurableState::Committed | LingquBlockDurableState::Sealed => Ok(record),
                LingquBlockDurableState::Tombstoned => {
                    Err(LingquDurableError::Tombstoned(block.0.clone()))
                }
                LingquBlockDurableState::Quarantined => {
                    Err(LingquDurableError::Quarantined(block.0.clone()))
                }
            }
        }

        fn check_expected_version(
            &mut self,
            id: &str,
            actual: Option<u64>,
            expected: Option<u64>,
        ) -> LingquDurableResult<()> {
            if let Some(expected) = expected {
                if actual != Some(expected) {
                    self.stats.version_conflicts += 1;
                    return Err(LingquDurableError::VersionConflict {
                        id: id.to_string(),
                        expected: Some(expected),
                        actual,
                    });
                }
            }
            Ok(())
        }

        fn submit_dfs_write(&mut self, path: String, bytes: u64) -> LingquDurableResult<()> {
            let now = self.next_timestamp();
            let handle = self
                .dfs_service
                .submit_write(
                    dfs::DfsWriteReq {
                        task: None,
                        path,
                        bytes,
                    },
                    now,
                )
                .map_err(|err| service_error_to_durable("dfs_write", err))?;
            expect_success(
                "dfs_write",
                handle.0,
                self.dfs_service.poll_ready(SimTimestamp::MAX),
            )
        }

        fn submit_dfs_read(&mut self, path: &str) -> LingquDurableResult<()> {
            let now = self.next_timestamp();
            let handle = self
                .dfs_service
                .submit_read(
                    dfs::DfsReadReq {
                        task: None,
                        path: path.to_string(),
                    },
                    now,
                )
                .map_err(|err| service_error_to_durable("dfs_read", err))?;
            expect_success(
                "dfs_read",
                handle.0,
                self.dfs_service.poll_ready(SimTimestamp::MAX),
            )
        }

        fn submit_block_write(&mut self, block: BlockHash) -> LingquDurableResult<()> {
            let now = self.next_timestamp();
            let handle = self
                .block_service
                .submit_write(BlockWriteReq { task: None, block }, now)
                .map_err(|err| service_error_to_durable("block_write", err))?;
            expect_success(
                "block_write",
                handle.0,
                self.block_service.poll_ready(SimTimestamp::MAX),
            )
        }

        fn submit_block_read(&mut self, block: BlockHash) -> LingquDurableResult<()> {
            let now = self.next_timestamp();
            let handle = self
                .block_service
                .submit_read(BlockReadReq { task: None, block }, now)
                .map_err(|err| service_error_to_durable("block_read", err))?;
            expect_success(
                "block_read",
                handle.0,
                self.block_service.poll_ready(SimTimestamp::MAX),
            )
        }

        fn rebuild_service_indexes(&mut self) -> LingquDurableResult<()> {
            let mut committed_blocks = self
                .block_records
                .iter()
                .filter(|record| {
                    matches!(
                        record.durable_state,
                        LingquBlockDurableState::Committed | LingquBlockDurableState::Sealed
                    )
                })
                .map(|record| record.block.clone())
                .collect::<Vec<_>>();
            committed_blocks.sort_by(|left, right| left.0.cmp(&right.0));
            committed_blocks.dedup();
            for block in committed_blocks {
                self.submit_block_write(block)?;
            }

            let mut committed_paths = self
                .dfs_files
                .iter()
                .filter(|record| record.state == LingquDfsFileState::Committed)
                .map(|record| (record.path.clone(), record.bytes))
                .collect::<Vec<_>>();
            committed_paths.sort_by(|left, right| left.0.cmp(&right.0));
            committed_paths.dedup_by(|left, right| left.0 == right.0);
            for (path, bytes) in committed_paths {
                self.submit_dfs_write(path, bytes)?;
            }
            let mut append_paths = self
                .dfs_append_logs
                .iter()
                .map(|record| (record.path.clone(), record.bytes.len() as u64))
                .collect::<Vec<_>>();
            append_paths.sort_by(|left, right| left.0.cmp(&right.0));
            append_paths.dedup_by(|left, right| left.0 == right.0);
            for (path, bytes) in append_paths {
                self.submit_dfs_write(path, bytes)?;
            }
            self.stats = LingquDurableStats::default();
            Ok(())
        }

        fn next_timestamp(&mut self) -> SimTimestamp {
            let now = self.next_timestamp_us;
            self.next_timestamp_us = self.next_timestamp_us.saturating_add(1);
            now
        }
    }

    impl Default for LingquDurableSim {
        fn default() -> Self {
            Self::new(LingquDurableSimProfile::default())
        }
    }

    impl LingquDurableSimSnapshot {
        pub fn validate(&self) -> LingquDurableResult<()> {
            if self.kind != LINGQU_DURABLE_SIM_KIND {
                return Err(LingquDurableError::SnapshotValidation(format!(
                    "unexpected kind `{}`",
                    self.kind
                )));
            }
            if self.schema_version != LINGQU_DURABLE_SIM_SCHEMA_VERSION {
                return Err(LingquDurableError::SnapshotValidation(format!(
                    "unsupported schema version {}",
                    self.schema_version
                )));
            }
            if self.next_timestamp_us == 0 {
                return Err(LingquDurableError::SnapshotValidation(
                    "next_timestamp_us must be non-zero".to_string(),
                ));
            }
            validate_block_snapshot(&self.block)?;
            validate_dfs_snapshot(&self.dfs, &self.block.blocks)?;
            validate_append_logs(&self.dfs.append_logs)?;
            let actual = durable_snapshot_checksum(self);
            if actual != self.checksum {
                return Err(LingquDurableError::ChecksumMismatch {
                    id: "lingqu_durable_sim_snapshot".to_string(),
                    expected: self.checksum,
                    actual,
                });
            }
            Ok(())
        }

        pub fn to_json_bytes(&self) -> LingquDurableResult<Vec<u8>> {
            self.validate()?;
            serde_json::to_vec_pretty(self)
                .map_err(|err| LingquDurableError::SnapshotCodec(err.to_string()))
        }

        pub fn from_json_bytes(bytes: &[u8]) -> LingquDurableResult<Self> {
            let snapshot = serde_json::from_slice::<Self>(bytes)
                .map_err(|err| LingquDurableError::SnapshotCodec(err.to_string()))?;
            snapshot.validate()?;
            Ok(snapshot)
        }

        pub fn validation_report(&self) -> LingquDurableResult<LingquDurableValidationReport> {
            self.validate()?;
            let mut referenced_blocks = HashSet::<(String, u64)>::new();
            let mut missing_block_refs = Vec::<String>::new();
            for record in &self.dfs.files {
                if let LingquDfsContentRef::Block(payload_ref) = &record.content_ref {
                    referenced_blocks.insert((payload_ref.block.0.clone(), payload_ref.version));
                    let present = self.block.blocks.iter().any(|block_record| {
                        block_record.block == payload_ref.block
                            && block_record.version == payload_ref.version
                    });
                    if !present {
                        missing_block_refs
                            .push(format!("{}@{}", payload_ref.block.0, payload_ref.version));
                    }
                }
            }
            let mut orphan_blocks = self
                .block
                .blocks
                .iter()
                .filter(|record| {
                    !referenced_blocks.contains(&(record.block.0.clone(), record.version))
                        && record.block.0.starts_with("block/dfs/")
                })
                .map(|record| format!("{}@{}", record.block.0, record.version))
                .collect::<Vec<_>>();
            orphan_blocks.sort();
            let mut append_log_paths = self
                .dfs
                .append_logs
                .iter()
                .map(|record| record.path.clone())
                .collect::<Vec<_>>();
            append_log_paths.sort();
            append_log_paths.dedup();
            Ok(LingquDurableValidationReport {
                dfs_file_versions: self.dfs.files.len(),
                dfs_append_records: self.dfs.append_logs.len(),
                block_versions: self.block.blocks.len(),
                missing_block_refs,
                orphan_blocks,
                append_log_paths,
                checksum: self.checksum,
            })
        }
    }

    fn validate_dfs_snapshot(
        snapshot: &LingquDfsSimSnapshot,
        block_records: &[LingquBlockRecord],
    ) -> LingquDurableResult<()> {
        let mut seen = HashSet::new();
        for record in &snapshot.files {
            if !seen.insert((record.path.clone(), record.version)) {
                return Err(LingquDurableError::SnapshotValidation(format!(
                    "duplicate DFS version {}:{}",
                    record.path, record.version
                )));
            }
            validate_dfs_file_record(record, block_records)?;
        }
        for directory in &snapshot.directories {
            validate_dfs_path(&directory.path)?;
        }
        Ok(())
    }

    fn validate_block_snapshot(snapshot: &LingquBlockSimSnapshot) -> LingquDurableResult<()> {
        let mut seen = HashSet::new();
        for record in &snapshot.blocks {
            if !seen.insert((record.block.0.clone(), record.version)) {
                return Err(LingquDurableError::SnapshotValidation(format!(
                    "duplicate Block version {}:{}",
                    record.block.0, record.version
                )));
            }
            validate_block_record(record)?;
        }
        Ok(())
    }

    fn validate_append_logs(records: &[LingquDfsAppendLogRecord]) -> LingquDurableResult<()> {
        let mut by_path = BTreeMap::<String, Vec<&LingquDfsAppendLogRecord>>::new();
        for record in records {
            validate_append_log_record(record)?;
            by_path.entry(record.path.clone()).or_default().push(record);
        }
        for (path, records) in by_path.iter_mut() {
            records.sort_by_key(|record| record.seq);
            let mut expected_seq = 1u64;
            let mut prev_chain_checksum = 0u64;
            for record in records {
                if record.seq != expected_seq {
                    return Err(LingquDurableError::SnapshotValidation(format!(
                        "append log {path} has non-contiguous seq {} expected {expected_seq}",
                        record.seq
                    )));
                }
                if record.prev_chain_checksum != prev_chain_checksum {
                    return Err(LingquDurableError::SnapshotValidation(format!(
                        "append log {path} seq {} prev checksum mismatch",
                        record.seq
                    )));
                }
                prev_chain_checksum = record.chain_checksum;
                expected_seq = expected_seq.saturating_add(1);
            }
        }
        Ok(())
    }

    fn validate_append_log_record(record: &LingquDfsAppendLogRecord) -> LingquDurableResult<()> {
        validate_dfs_path(&record.path)?;
        if record.seq == 0 {
            return Err(LingquDurableError::SnapshotValidation(
                "append log seq must be non-zero".to_string(),
            ));
        }
        validate_payload_metadata(
            &record.path,
            &record.bytes,
            record.bytes.len() as u64,
            record.checksum,
        )?;
        let chain_checksum = append_log_chain_checksum(
            &record.path,
            record.seq,
            record.checksum,
            record.prev_chain_checksum,
            &record.bytes,
        );
        if chain_checksum != record.chain_checksum {
            return Err(LingquDurableError::ChecksumMismatch {
                id: format!("{}#{}", record.path, record.seq),
                expected: record.chain_checksum,
                actual: chain_checksum,
            });
        }
        Ok(())
    }

    fn validate_dfs_file_record(
        record: &LingquDfsFileRecord,
        block_records: &[LingquBlockRecord],
    ) -> LingquDurableResult<()> {
        validate_dfs_path(&record.path)?;
        if record.version == 0 {
            return Err(LingquDurableError::SnapshotValidation(
                "DFS version must be non-zero".to_string(),
            ));
        }
        match (&record.state, &record.content_ref) {
            (LingquDfsFileState::Committed, LingquDfsContentRef::Inline(bytes)) => {
                validate_payload_metadata(&record.path, bytes, record.bytes, record.checksum)?;
            }
            (LingquDfsFileState::Committed, LingquDfsContentRef::Block(payload_ref)) => {
                validate_block_payload_ref(payload_ref)?;
                if record.bytes != payload_ref.bytes || record.checksum != payload_ref.checksum {
                    return Err(LingquDurableError::SnapshotValidation(format!(
                        "DFS block ref metadata mismatch for {}",
                        record.path
                    )));
                }
                validate_block_ref_against_records(payload_ref, block_records)?;
            }
            (_, LingquDfsContentRef::Inline(bytes)) => {
                validate_payload_metadata(&record.path, bytes, record.bytes, record.checksum)?;
            }
            (_, LingquDfsContentRef::Block(payload_ref)) => {
                validate_block_payload_ref(payload_ref)?;
            }
        }
        Ok(())
    }

    fn validate_block_record(record: &LingquBlockRecord) -> LingquDurableResult<()> {
        validate_block(&record.block)?;
        if record.version == 0 {
            return Err(LingquDurableError::SnapshotValidation(
                "Block version must be non-zero".to_string(),
            ));
        }
        validate_payload_metadata(
            &record.block.0,
            &record.bytes,
            record.bytes.len() as u64,
            record.checksum,
        )
    }

    fn validate_block_payload_ref(payload_ref: &LingquBlockPayloadRef) -> LingquDurableResult<()> {
        validate_block(&payload_ref.block)?;
        if payload_ref.version == 0 {
            return Err(LingquDurableError::SnapshotValidation(
                "Block payload ref version must be non-zero".to_string(),
            ));
        }
        if payload_ref.bytes == 0 {
            return Err(LingquDurableError::EmptyPayload);
        }
        Ok(())
    }

    fn validate_block_ref_against_records(
        payload_ref: &LingquBlockPayloadRef,
        records: &[LingquBlockRecord],
    ) -> LingquDurableResult<()> {
        let record = records
            .iter()
            .find(|record| {
                record.block == payload_ref.block && record.version == payload_ref.version
            })
            .ok_or_else(|| LingquDurableError::MissingBlock(payload_ref.block.0.clone()))?;
        let start = payload_ref.offset as usize;
        let end = payload_ref
            .offset
            .checked_add(payload_ref.bytes)
            .ok_or_else(|| LingquDurableError::RangeOverflow(payload_ref.block.0.clone()))?
            as usize;
        if end > record.bytes.len() {
            return Err(LingquDurableError::RangeOverflow(
                payload_ref.block.0.clone(),
            ));
        }
        let actual = checksum64(&record.bytes[start..end]);
        if actual != payload_ref.checksum {
            return Err(LingquDurableError::ChecksumMismatch {
                id: payload_ref.block.0.clone(),
                expected: payload_ref.checksum,
                actual,
            });
        }
        Ok(())
    }

    fn validate_payload_metadata(
        id: &str,
        bytes: &[u8],
        expected_bytes: u64,
        expected_checksum: u64,
    ) -> LingquDurableResult<()> {
        if bytes.len() as u64 != expected_bytes {
            return Err(LingquDurableError::SnapshotValidation(format!(
                "payload byte count mismatch for {id}"
            )));
        }
        let actual = checksum64(bytes);
        if actual != expected_checksum {
            return Err(LingquDurableError::ChecksumMismatch {
                id: id.to_string(),
                expected: expected_checksum,
                actual,
            });
        }
        Ok(())
    }

    fn validate_dfs_path(path: &str) -> LingquDurableResult<()> {
        if !path.starts_with("/lingqu/") || path.ends_with('/') {
            return Err(LingquDurableError::InvalidPath(path.to_string()));
        }
        for segment in path.split('/') {
            if segment == "." || segment == ".." {
                return Err(LingquDurableError::InvalidPath(path.to_string()));
            }
        }
        if path.contains("//") {
            return Err(LingquDurableError::InvalidPath(path.to_string()));
        }
        Ok(())
    }

    fn validate_dfs_prefix(prefix: &str) -> LingquDurableResult<()> {
        if prefix == "/lingqu" || prefix == "/lingqu/" {
            return Ok(());
        }
        if !prefix.starts_with("/lingqu/") {
            return Err(LingquDurableError::InvalidPath(prefix.to_string()));
        }
        for segment in prefix.split('/') {
            if segment == "." || segment == ".." {
                return Err(LingquDurableError::InvalidPath(prefix.to_string()));
            }
        }
        if prefix.contains("//") {
            return Err(LingquDurableError::InvalidPath(prefix.to_string()));
        }
        Ok(())
    }

    fn validate_block(block: &BlockHash) -> LingquDurableResult<()> {
        if block.0.trim().is_empty() {
            return Err(LingquDurableError::InvalidBlock(block.0.clone()));
        }
        Ok(())
    }

    fn service_error_to_durable(op: &'static str, err: sim_core::SimError) -> LingquDurableError {
        if err.to_string().contains("queue full") {
            LingquDurableError::QueueFull(op.to_string())
        } else {
            LingquDurableError::SnapshotValidation(format!("{op}: {err}"))
        }
    }

    fn expect_success(
        op: &'static str,
        op_id: u64,
        events: Vec<CompletionEvent>,
    ) -> LingquDurableResult<()> {
        let event = events
            .into_iter()
            .find(|event| event.op_id == op_id)
            .ok_or_else(|| {
                LingquDurableError::SnapshotValidation(format!("{op} did not complete"))
            })?;
        match event.status {
            CompletionStatus::Success => Ok(()),
            CompletionStatus::RetryableFailure { code } => Err(
                LingquDurableError::SnapshotValidation(format!("{op}: {code}")),
            ),
            CompletionStatus::FatalFailure { code } => Err(LingquDurableError::SnapshotValidation(
                format!("{op}: {code}"),
            )),
        }
    }

    fn generated_dfs_block_id(path: &str, version: u64) -> String {
        let sanitized = path
            .chars()
            .map(|ch| {
                if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' {
                    ch
                } else {
                    '_'
                }
            })
            .collect::<String>();
        format!("block/dfs/{sanitized}/v{version}")
    }

    fn durable_snapshot_checksum(snapshot: &LingquDurableSimSnapshot) -> u64 {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(snapshot.kind.as_bytes());
        bytes.extend_from_slice(&snapshot.schema_version.to_le_bytes());
        bytes.extend_from_slice(&snapshot.next_timestamp_us.to_le_bytes());
        let mut dfs_files = snapshot.dfs.files.iter().collect::<Vec<_>>();
        dfs_files.sort_by(|left, right| {
            left.path
                .cmp(&right.path)
                .then_with(|| left.version.cmp(&right.version))
        });
        let mut block_records = snapshot.block.blocks.iter().collect::<Vec<_>>();
        block_records.sort_by(|left, right| {
            left.block
                .0
                .cmp(&right.block.0)
                .then_with(|| left.version.cmp(&right.version))
        });
        let mut append_logs = snapshot.dfs.append_logs.iter().collect::<Vec<_>>();
        append_logs.sort_by(|left, right| {
            left.path
                .cmp(&right.path)
                .then_with(|| left.seq.cmp(&right.seq))
        });
        for record in dfs_files {
            bytes.extend_from_slice(record.path.as_bytes());
            bytes.extend_from_slice(&record.version.to_le_bytes());
            bytes.extend_from_slice(&dfs_state_tag(record.state).to_le_bytes());
            bytes.extend_from_slice(&record.bytes.to_le_bytes());
            bytes.extend_from_slice(&record.checksum.to_le_bytes());
        }
        for record in append_logs {
            bytes.extend_from_slice(record.path.as_bytes());
            bytes.extend_from_slice(&record.seq.to_le_bytes());
            bytes.extend_from_slice(&record.checksum.to_le_bytes());
            bytes.extend_from_slice(&record.prev_chain_checksum.to_le_bytes());
            bytes.extend_from_slice(&record.chain_checksum.to_le_bytes());
            bytes.extend_from_slice(&(record.bytes.len() as u64).to_le_bytes());
        }
        for record in block_records {
            bytes.extend_from_slice(record.block.0.as_bytes());
            bytes.extend_from_slice(&record.version.to_le_bytes());
            bytes.extend_from_slice(&block_durable_state_tag(record.durable_state).to_le_bytes());
            bytes.extend_from_slice(&record.checksum.to_le_bytes());
            bytes.extend_from_slice(&(record.bytes.len() as u64).to_le_bytes());
        }
        checksum64(&bytes)
    }

    fn append_log_chain_checksum(
        path: &str,
        seq: u64,
        checksum: u64,
        prev_chain_checksum: u64,
        payload: &[u8],
    ) -> u64 {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(path.as_bytes());
        bytes.extend_from_slice(&seq.to_le_bytes());
        bytes.extend_from_slice(&checksum.to_le_bytes());
        bytes.extend_from_slice(&prev_chain_checksum.to_le_bytes());
        bytes.extend_from_slice(&(payload.len() as u64).to_le_bytes());
        bytes.extend_from_slice(payload);
        checksum64(&bytes)
    }

    fn dfs_state_tag(state: LingquDfsFileState) -> u64 {
        match state {
            LingquDfsFileState::Committed => 1,
            LingquDfsFileState::Tombstoned => 2,
            LingquDfsFileState::Quarantined => 3,
        }
    }

    fn block_durable_state_tag(state: LingquBlockDurableState) -> u64 {
        match state {
            LingquBlockDurableState::Committed => 1,
            LingquBlockDurableState::Sealed => 2,
            LingquBlockDurableState::Tombstoned => 3,
            LingquBlockDurableState::Quarantined => 4,
        }
    }

    fn add_durable_stats(
        left: LingquDurableStats,
        right: LingquDurableStats,
    ) -> LingquDurableStats {
        LingquDurableStats {
            dfs_reads: left.dfs_reads.saturating_add(right.dfs_reads),
            dfs_writes: left.dfs_writes.saturating_add(right.dfs_writes),
            dfs_bytes_read: left.dfs_bytes_read.saturating_add(right.dfs_bytes_read),
            dfs_bytes_written: left
                .dfs_bytes_written
                .saturating_add(right.dfs_bytes_written),
            block_reads: left.block_reads.saturating_add(right.block_reads),
            block_writes: left.block_writes.saturating_add(right.block_writes),
            block_bytes_read: left.block_bytes_read.saturating_add(right.block_bytes_read),
            block_bytes_written: left
                .block_bytes_written
                .saturating_add(right.block_bytes_written),
            checksum_failures: left
                .checksum_failures
                .saturating_add(right.checksum_failures),
            version_conflicts: left
                .version_conflicts
                .saturating_add(right.version_conflicts),
            missing_refs: left.missing_refs.saturating_add(right.missing_refs),
        }
    }

    fn checksum64(bytes: &[u8]) -> u64 {
        let mut hash = 0xcbf2_9ce4_8422_2325u64;
        for byte in bytes {
            hash ^= u64::from(*byte);
            hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
        }
        hash
    }
}

pub mod db {
    use super::*;

    #[derive(Debug, Clone)]
    pub struct DbPutReq {
        pub task: Option<TaskKey>,
        pub key: String,
        pub bytes: u64,
    }

    #[derive(Debug, Clone)]
    pub struct DbGetReq {
        pub task: Option<TaskKey>,
        pub key: String,
    }

    #[derive(Debug, Clone, Copy)]
    pub struct DbServiceProfile {
        pub metadata_latency_us: SimTimestamp,
        pub value_latency_us: SimTimestamp,
        pub inline_value_limit: u64,
        pub pipeline_batch_limit: u64,
        pub queue_depth: usize,
    }

    impl Default for DbServiceProfile {
        fn default() -> Self {
            Self {
                metadata_latency_us: 8,
                value_latency_us: 16,
                inline_value_limit: 64,
                pipeline_batch_limit: 16,
                queue_depth: 16,
            }
        }
    }

    #[derive(Debug)]
    pub struct DbServiceStub {
        profile: DbServiceProfile,
        rows: HashMap<String, u64>,
        completions: VecDeque<QueuedCompletion>,
        next_op_id: u64,
    }

    impl DbServiceStub {
        pub fn new(profile: DbServiceProfile) -> Self {
            Self {
                profile,
                rows: HashMap::new(),
                completions: VecDeque::new(),
                next_op_id: 0,
            }
        }

        fn next_handle(&mut self) -> ServiceOpHandle {
            self.next_op_id += 1;
            ServiceOpHandle(self.next_op_id)
        }

        fn ensure_queue_capacity(&self) -> Result<(), sim_core::SimError> {
            if self.completions.len() >= self.profile.queue_depth {
                return Err(sim_core::SimError::InvalidInput("db queue full"));
            }
            Ok(())
        }

        pub fn submit_put(
            &mut self,
            req: DbPutReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            let max_bytes = self.profile.inline_value_limit * self.profile.pipeline_batch_limit;
            let (status, ready_at) = if req.bytes > max_bytes {
                (
                    CompletionStatus::RetryableFailure {
                        code: "db_batch_limit_exceeded".to_string(),
                    },
                    now + self.profile.metadata_latency_us,
                )
            } else {
                self.rows.insert(req.key, req.bytes);
                let value_penalty = if req.bytes > self.profile.inline_value_limit {
                    self.profile.value_latency_us
                } else {
                    0
                };
                (
                    CompletionStatus::Success,
                    now + self.profile.metadata_latency_us + value_penalty,
                )
            };

            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task: req.task,
                    source: CompletionSource::DbService,
                    status,
                    finished_at: ready_at,
                },
            });

            Ok(handle)
        }

        pub fn submit_get(
            &mut self,
            req: DbGetReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            let (status, ready_at) = match self.rows.get(&req.key) {
                Some(bytes) => {
                    let value_penalty = if *bytes > self.profile.inline_value_limit {
                        self.profile.value_latency_us
                    } else {
                        0
                    };
                    (
                        CompletionStatus::Success,
                        now + self.profile.metadata_latency_us + value_penalty,
                    )
                }
                None => (
                    CompletionStatus::RetryableFailure {
                        code: "db_missing_key".to_string(),
                    },
                    now + self.profile.metadata_latency_us,
                ),
            };

            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task: req.task,
                    source: CompletionSource::DbService,
                    status,
                    finished_at: ready_at,
                },
            });

            Ok(handle)
        }

        pub fn poll_ready(&mut self, now: SimTimestamp) -> Vec<CompletionEvent> {
            drain_ready(&mut self.completions, now)
        }
    }
}

pub mod object {
    use super::*;

    #[derive(Debug, Clone, Copy, Eq, PartialEq, Serialize, Deserialize)]
    pub enum LingquObjectKind {
        WeightShard,
        KvCacheBlock,
        RuntimeTensor,
        TokenBuffer,
        TokenizerAsset,
        Logits,
        Metadata,
    }

    #[derive(Debug, Clone, Copy, Eq, PartialEq, Serialize, Deserialize)]
    pub enum LingquPayloadBackend {
        Inline,
        Shmem,
        ObmmShmem,
        Block,
        Dfs,
        External,
    }

    pub const LINGQU_OBMM_OBJECT_REF_MAGIC: u64 = 0x514f_424d_4d52_4546;
    pub const LINGQU_OBMM_OBJECT_REF_LAYOUT_VERSION: u16 = 1;
    pub const LINGQU_OBJECT_STATE_PENDING_WIRE: u16 = 1;
    pub const LINGQU_OBJECT_STATE_COMMITTED_WIRE: u16 = 2;
    pub const LINGQU_OBJECT_STATE_TOMBSTONED_WIRE: u16 = 3;
    pub const LINGQU_OBJECT_STATE_QUARANTINED_WIRE: u16 = 4;

    #[derive(Debug, Clone, Copy, Eq, PartialEq)]
    #[repr(C)]
    pub struct LingquObmmObjectRefWire {
        pub magic: u64,
        pub layout_version: u16,
        pub object_kind: u16,
        pub state: u16,
        pub flags: u16,
        pub owner_entity: u32,
        pub producer_entity: u32,
        pub object_version: u64,
        pub key_hash: u64,
        pub payload_offset: u64,
        pub payload_bytes: u64,
        pub payload_checksum: u64,
    }

    const _: [(); 64] = [(); std::mem::size_of::<LingquObmmObjectRefWire>()];

    impl LingquObmmObjectRefWire {
        pub const BYTE_LEN: usize = 64;

        pub fn committed(
            object_kind: u16,
            owner_entity: u32,
            producer_entity: u32,
            object_version: u64,
            key_hash: u64,
            payload_offset: u64,
            payload_bytes: u64,
            payload_checksum: u64,
        ) -> Self {
            Self {
                magic: LINGQU_OBMM_OBJECT_REF_MAGIC,
                layout_version: LINGQU_OBMM_OBJECT_REF_LAYOUT_VERSION,
                object_kind,
                state: LINGQU_OBJECT_STATE_COMMITTED_WIRE,
                flags: 0,
                owner_entity,
                producer_entity,
                object_version,
                key_hash,
                payload_offset,
                payload_bytes,
                payload_checksum,
            }
        }

        pub fn from_le_bytes(bytes: &[u8]) -> Result<Self, &'static str> {
            if bytes.len() != Self::BYTE_LEN {
                return Err("lingqu_obmm_object_ref_bad_len");
            }
            Ok(Self {
                magic: u64::from_le_bytes(
                    bytes[0..8]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_magic_bytes")?,
                ),
                layout_version: u16::from_le_bytes(
                    bytes[8..10]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_layout_bytes")?,
                ),
                object_kind: u16::from_le_bytes(
                    bytes[10..12]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_kind_bytes")?,
                ),
                state: u16::from_le_bytes(
                    bytes[12..14]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_state_bytes")?,
                ),
                flags: u16::from_le_bytes(
                    bytes[14..16]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_flags_bytes")?,
                ),
                owner_entity: u32::from_le_bytes(
                    bytes[16..20]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_owner_bytes")?,
                ),
                producer_entity: u32::from_le_bytes(
                    bytes[20..24]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_producer_bytes")?,
                ),
                object_version: u64::from_le_bytes(
                    bytes[24..32]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_version_bytes")?,
                ),
                key_hash: u64::from_le_bytes(
                    bytes[32..40]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_key_hash_bytes")?,
                ),
                payload_offset: u64::from_le_bytes(
                    bytes[40..48]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_offset_bytes")?,
                ),
                payload_bytes: u64::from_le_bytes(
                    bytes[48..56]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_bytes_bytes")?,
                ),
                payload_checksum: u64::from_le_bytes(
                    bytes[56..64]
                        .try_into()
                        .map_err(|_| "lingqu_obmm_object_ref_bad_checksum_bytes")?,
                ),
            })
        }

        pub fn validate(&self) -> Result<(), &'static str> {
            if self.magic != LINGQU_OBMM_OBJECT_REF_MAGIC {
                return Err("lingqu_obmm_object_ref_bad_magic");
            }
            if self.layout_version != LINGQU_OBMM_OBJECT_REF_LAYOUT_VERSION {
                return Err("lingqu_obmm_object_ref_bad_layout_version");
            }
            if self.state != LINGQU_OBJECT_STATE_COMMITTED_WIRE {
                return Err("lingqu_obmm_object_ref_not_committed");
            }
            if self.payload_bytes == 0 {
                return Err("lingqu_obmm_object_ref_empty_payload");
            }
            Ok(())
        }
    }

    #[derive(Debug, Clone, Copy, Eq, PartialEq, Serialize, Deserialize)]
    pub enum LingquObjectState {
        Pending,
        Committed,
        Tombstoned,
        Quarantined,
    }

    #[derive(Debug, Clone, Copy, Eq, PartialEq, Serialize, Deserialize)]
    pub enum LingquObjectLocality {
        EntityLocal(u64),
        DomainShared(u64),
        Global,
    }

    #[derive(Debug, Clone, Copy, Eq, PartialEq)]
    pub enum LingquObjectVersionSelector {
        LatestCommitted,
        Exact(u64),
        AtLeast(u64),
    }

    #[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
    pub struct LingquPayloadPlacement {
        pub backend: LingquPayloadBackend,
        pub storage_ref: String,
        pub segment: Option<SegmentHandle>,
        pub offset: u64,
        pub bytes: u64,
        pub checksum: u64,
        pub locality: LingquObjectLocality,
    }

    #[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
    pub struct LingquObjectMetadata {
        pub bytes: u64,
        pub checksum: u64,
        pub dtype: Option<TensorDType>,
        pub shape: Vec<u64>,
        pub layout: Option<TensorLayout>,
        pub expires_at_us: Option<u64>,
    }

    #[derive(Debug, Clone, Eq, PartialEq, Serialize, Deserialize)]
    pub struct LingquObjectRecord {
        pub key: String,
        pub kind: LingquObjectKind,
        pub version: u64,
        pub state: LingquObjectState,
        pub producer_entity: u64,
        pub owner_entity: Option<u64>,
        pub bytes: u64,
        pub checksum: u64,
        pub dtype: Option<TensorDType>,
        pub shape: Vec<u64>,
        pub layout: Option<TensorLayout>,
        pub placements: Vec<LingquPayloadPlacement>,
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        pub payload_bytes: Vec<u8>,
        pub created_at_us: u64,
        pub committed_at_us: Option<u64>,
        pub expires_at_us: Option<u64>,
    }

    #[derive(Debug, Clone)]
    pub struct LingquObjectPublishReq {
        pub task: Option<TaskKey>,
        pub key: String,
        pub kind: LingquObjectKind,
        pub producer_entity: u64,
        pub owner_entity: Option<u64>,
        pub expected_version: Option<u64>,
        pub metadata: LingquObjectMetadata,
        pub placements: Vec<LingquPayloadPlacement>,
        pub payload_bytes: Vec<u8>,
    }

    #[derive(Debug, Clone)]
    pub struct LingquObjectResolveReq {
        pub task: Option<TaskKey>,
        pub key: String,
        pub requester_entity: u64,
        pub version: LingquObjectVersionSelector,
        pub min_state: LingquObjectState,
        pub preferred_backends: Vec<LingquPayloadBackend>,
    }

    #[derive(Debug, Clone)]
    pub struct LingquObjectAppendReq {
        pub task: Option<TaskKey>,
        pub base_key: String,
        pub suffix: String,
        pub kind: LingquObjectKind,
        pub producer_entity: u64,
        pub owner_entity: Option<u64>,
        pub previous_version: Option<u64>,
        pub metadata: LingquObjectMetadata,
        pub placements: Vec<LingquPayloadPlacement>,
        pub payload_bytes: Vec<u8>,
    }

    #[derive(Debug, Clone, Copy, Eq, PartialEq, Default)]
    pub struct LingquObjectServiceReport {
        pub publish_count: u64,
        pub resolve_count: u64,
        pub append_count: u64,
        pub metadata_put_count: u64,
        pub metadata_get_count: u64,
        pub shmem_write_count: u64,
        pub shmem_read_count: u64,
        pub block_write_count: u64,
        pub block_read_count: u64,
        pub inline_write_count: u64,
        pub inline_read_count: u64,
        pub committed_object_count: u64,
        pub quarantined_object_count: u64,
        pub missing_resolve_count: u64,
        pub obmm_pool_enabled: bool,
        pub obmm_pool_payload_write_count: u64,
        pub obmm_pool_payload_read_count: u64,
        pub obmm_pool_queue_submit_count: u64,
        pub obmm_pool_queue_deliver_count: u64,
        pub obmm_pool_bytes_used: u64,
        pub obmm_pool_reserved_bytes: u64,
        pub obmm_pool_block_count: u64,
        pub obmm_pool_multi_block_write_count: u64,
        pub obmm_pool_max_blocks_per_payload: u64,
        pub checksum: u64,
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquObjectServiceProfile {
        pub metadata_latency_us: SimTimestamp,
        pub shmem_latency_us: SimTimestamp,
        pub block_latency_us: SimTimestamp,
        pub inline_value_limit: u64,
        pub queue_depth: usize,
        pub obmm_pool: LingquObmmPoolProfile,
    }

    impl Default for LingquObjectServiceProfile {
        fn default() -> Self {
            Self {
                metadata_latency_us: 8,
                shmem_latency_us: 3,
                block_latency_us: 30,
                inline_value_limit: 64,
                queue_depth: 16,
                obmm_pool: LingquObmmPoolProfile::default(),
            }
        }
    }

    #[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquObmmPoolProfile {
        pub enabled: bool,
        pub node_count: u64,
        pub queue_depth: usize,
        pub pool_bytes: u64,
        pub payload_base_offset: u64,
        pub payload_alignment: u64,
        pub payload_block_tiers: [u64; 4],
        pub queue_auto_drain: bool,
    }

    impl Default for LingquObmmPoolProfile {
        fn default() -> Self {
            Self {
                enabled: true,
                node_count: 8,
                queue_depth: 1024,
                pool_bytes: 256 * 1024 * 1024,
                payload_base_offset: 2 * 1024 * 1024,
                payload_alignment: 64,
                payload_block_tiers: [256 * 1024, 512 * 1024, 1024 * 1024, 2 * 1024 * 1024],
                queue_auto_drain: true,
            }
        }
    }

    #[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
    pub struct LingquObjectServiceSnapshot {
        pub profile: LingquObjectServiceProfile,
        pub records: Vec<LingquObjectRecord>,
    }

    impl LingquObjectServiceSnapshot {
        pub fn to_json_bytes(&self) -> Result<Vec<u8>, serde_json::Error> {
            serde_json::to_vec_pretty(self)
        }

        pub fn from_json_bytes(bytes: &[u8]) -> Result<Self, serde_json::Error> {
            serde_json::from_slice(bytes)
        }
    }

    #[derive(Debug, Clone, Eq, PartialEq)]
    struct LingquObmmPoolPayload {
        storage_ref: String,
        offset: u64,
        bytes: u64,
        reserved_bytes: u64,
        block_bytes: u64,
        block_count: u64,
        checksum: u64,
        owner_entity: u64,
        payload: Vec<u8>,
    }

    #[derive(Debug, Clone, Eq, PartialEq)]
    struct LingquObmmQueueDesc {
        producer_entity: u64,
        consumer_entity: u64,
        storage_ref: String,
        offset: u64,
        bytes: u64,
        checksum: u64,
        version: u64,
    }

    #[derive(Debug, Clone, Copy, Eq, PartialEq)]
    #[repr(C)]
    struct LingquObmmQueueWireDesc {
        seq: u64,
        region_id: u32,
        payload_len: u32,
        payload_offset: u64,
        desc_type: u16,
        flags: u16,
        cookie: u32,
    }

    const LINGQU_OBMM_DESC_READY: u16 = 6;
    const LINGQU_OBMM_QUEUE_MIN_DEPTH: usize = 64;
    const LINGQU_OBMM_QUEUE_MAX_DEPTH: usize = 65536;
    const _: [(); 32] = [(); std::mem::size_of::<LingquObmmQueueWireDesc>()];

    #[derive(Debug, Clone)]
    struct LingquObmmSpscQueue {
        head: u32,
        tail: u32,
        mask: u32,
        slots: Vec<Option<LingquObmmQueueWireDesc>>,
    }

    impl LingquObmmSpscQueue {
        fn new(depth: usize) -> Result<Self, &'static str> {
            if depth == 0 {
                return Err("obmm_pool_queue_disabled");
            }
            if !(LINGQU_OBMM_QUEUE_MIN_DEPTH..=LINGQU_OBMM_QUEUE_MAX_DEPTH).contains(&depth) {
                return Err("obmm_pool_queue_depth_out_of_range");
            }
            if !depth.is_power_of_two() {
                return Err("obmm_pool_queue_depth_not_power_of_two");
            }
            if depth > u32::MAX as usize {
                return Err("obmm_pool_queue_depth_too_large");
            }
            Ok(Self {
                head: 0,
                tail: 0,
                mask: depth as u32 - 1,
                slots: vec![None; depth],
            })
        }

        fn push(&mut self, desc: LingquObmmQueueWireDesc) -> Result<(), &'static str> {
            let depth = self.slots.len() as u32;
            if self.tail.wrapping_sub(self.head) == depth {
                return Err("obmm_pool_queue_full");
            }
            let index = (self.tail & self.mask) as usize;
            self.slots[index] = Some(desc);
            self.tail = self.tail.wrapping_add(1);
            Ok(())
        }

        fn pop(&mut self) -> Option<LingquObmmQueueWireDesc> {
            if self.head == self.tail {
                return None;
            }
            let index = (self.head & self.mask) as usize;
            let desc = self.slots[index].take();
            self.head = self.head.wrapping_add(1);
            desc
        }
    }

    #[derive(Debug, Clone, Copy, Default)]
    struct LingquObmmPoolStats {
        payload_write_count: u64,
        payload_read_count: u64,
        queue_submit_count: u64,
        queue_deliver_count: u64,
        bytes_used: u64,
        reserved_bytes: u64,
        block_count: u64,
        multi_block_write_count: u64,
        max_blocks_per_payload: u64,
    }

    #[derive(Debug, Clone)]
    struct LingquObmmPoolBackend {
        profile: LingquObmmPoolProfile,
        next_payload_offset: u64,
        payloads: HashMap<String, LingquObmmPoolPayload>,
        delivered_descs: Vec<LingquObmmQueueDesc>,
        queues: HashMap<(u64, u64), LingquObmmSpscQueue>,
        pending_descs: HashMap<u64, LingquObmmQueueDesc>,
        stats: LingquObmmPoolStats,
    }

    impl LingquObmmPoolBackend {
        fn new(profile: LingquObmmPoolProfile) -> Self {
            Self {
                profile,
                next_payload_offset: profile.payload_base_offset,
                payloads: HashMap::new(),
                delivered_descs: Vec::new(),
                queues: HashMap::new(),
                pending_descs: HashMap::new(),
                stats: LingquObmmPoolStats::default(),
            }
        }

        fn publish_payloads(
            &mut self,
            placements: &mut [LingquPayloadPlacement],
            payload: &[u8],
            producer_entity: u64,
            owner_entity: Option<u64>,
            version: u64,
        ) -> Result<(), &'static str> {
            let mut staged = self.clone();
            staged.publish_payloads_in_place(
                placements,
                payload,
                producer_entity,
                owner_entity,
                version,
            )?;
            *self = staged;
            Ok(())
        }

        fn publish_payloads_in_place(
            &mut self,
            placements: &mut [LingquPayloadPlacement],
            payload: &[u8],
            producer_entity: u64,
            owner_entity: Option<u64>,
            version: u64,
        ) -> Result<(), &'static str> {
            if !self.profile.enabled {
                return Ok(());
            }
            for placement in placements.iter_mut().filter(|placement| {
                matches!(
                    placement.backend,
                    LingquPayloadBackend::Shmem | LingquPayloadBackend::ObmmShmem
                )
            }) {
                let owner = owner_entity.unwrap_or(producer_entity);
                let storage_ref = if placement.storage_ref.is_empty() {
                    format!("obmm://node/{owner}/payload/{version}/{}", placement.offset)
                } else {
                    placement.storage_ref.clone()
                };
                let (offset, reserved_bytes, block_bytes, block_count) =
                    self.allocate_payload_span(placement.bytes)?;
                let segment_id =
                    0x0b00_0000u64 + owner.min(self.profile.node_count.saturating_sub(1));

                placement.storage_ref = storage_ref.clone();
                placement.segment = Some(SegmentHandle(segment_id));
                placement.offset = offset;
                self.payloads.insert(
                    storage_ref.clone(),
                    LingquObmmPoolPayload {
                        storage_ref: storage_ref.clone(),
                        offset,
                        bytes: placement.bytes,
                        reserved_bytes,
                        block_bytes,
                        block_count,
                        checksum: placement.checksum,
                        owner_entity: owner,
                        payload: payload.to_vec(),
                    },
                );
                self.stats.payload_write_count += 1;
                self.stats.bytes_used = self
                    .stats
                    .bytes_used
                    .max(offset.saturating_add(reserved_bytes));
                self.stats.reserved_bytes =
                    self.stats.reserved_bytes.saturating_add(reserved_bytes);
                self.stats.block_count = self.stats.block_count.saturating_add(block_count);
                if block_count > 1 {
                    self.stats.multi_block_write_count =
                        self.stats.multi_block_write_count.saturating_add(1);
                }
                self.stats.max_blocks_per_payload =
                    self.stats.max_blocks_per_payload.max(block_count);
                self.enqueue_desc(LingquObmmQueueDesc {
                    producer_entity,
                    consumer_entity: owner,
                    storage_ref,
                    offset,
                    bytes: placement.bytes,
                    checksum: placement.checksum,
                    version,
                })?;
            }
            Ok(())
        }

        fn get_ref(&self, storage_ref: &str) -> Option<&[u8]> {
            self.payloads
                .get(storage_ref)
                .map(|payload| payload.payload.as_slice())
        }

        fn import_record_payload(
            &mut self,
            record: &LingquObjectRecord,
        ) -> Result<(), &'static str> {
            if !self.profile.enabled || record.payload_bytes.is_empty() {
                return Ok(());
            }
            for placement in record.placements.iter().filter(|placement| {
                matches!(
                    placement.backend,
                    LingquPayloadBackend::Shmem | LingquPayloadBackend::ObmmShmem
                )
            }) {
                if placement.storage_ref.is_empty() {
                    return Err("obmm_pool_snapshot_missing_storage_ref");
                }
                self.payloads.insert(
                    placement.storage_ref.clone(),
                    LingquObmmPoolPayload {
                        storage_ref: placement.storage_ref.clone(),
                        offset: placement.offset,
                        bytes: placement.bytes,
                        reserved_bytes: placement.bytes,
                        block_bytes: placement.bytes.max(1),
                        block_count: 1,
                        checksum: placement.checksum,
                        owner_entity: record.owner_entity.unwrap_or(record.producer_entity),
                        payload: record.payload_bytes.clone(),
                    },
                );
                self.stats.bytes_used = self
                    .stats
                    .bytes_used
                    .max(placement.offset.saturating_add(placement.bytes));
                self.stats.reserved_bytes =
                    self.stats.reserved_bytes.saturating_add(placement.bytes);
                self.stats.block_count = self.stats.block_count.saturating_add(1);
                self.next_payload_offset = self
                    .next_payload_offset
                    .max(placement.offset.saturating_add(placement.bytes));
            }
            Ok(())
        }

        fn stats(&self) -> LingquObmmPoolStats {
            self.stats
        }

        fn allocate_payload_span(
            &mut self,
            bytes: u64,
        ) -> Result<(u64, u64, u64, u64), &'static str> {
            let (block_bytes, block_count, reserved_bytes) = self.block_span(bytes)?;
            let align = self.profile.payload_alignment.max(1);
            let offset = align_up_u64(self.next_payload_offset, align.max(block_bytes))?;
            let next = offset
                .checked_add(reserved_bytes)
                .ok_or("obmm_pool_payload_offset_overflow")?;
            if next > self.profile.pool_bytes {
                return Err("obmm_pool_full");
            }
            self.next_payload_offset = next;
            Ok((offset, reserved_bytes, block_bytes, block_count))
        }

        fn block_span(&self, bytes: u64) -> Result<(u64, u64, u64), &'static str> {
            if bytes == 0 {
                return Err("obmm_pool_payload_empty");
            }
            let mut tiers = self.profile.payload_block_tiers;
            tiers.sort_unstable();
            let max_block_bytes = tiers
                .iter()
                .copied()
                .filter(|tier| *tier > 0)
                .max()
                .ok_or("obmm_pool_block_tiers_empty")?;
            let block_bytes = tiers
                .iter()
                .copied()
                .filter(|tier| *tier > 0)
                .find(|tier| bytes <= *tier)
                .unwrap_or(max_block_bytes);
            let block_count = bytes
                .checked_add(block_bytes - 1)
                .ok_or("obmm_pool_block_span_overflow")?
                / block_bytes;
            let reserved_bytes = block_count
                .checked_mul(block_bytes)
                .ok_or("obmm_pool_block_span_overflow")?;
            Ok((block_bytes, block_count, reserved_bytes))
        }

        fn enqueue_desc(&mut self, desc: LingquObmmQueueDesc) -> Result<(), &'static str> {
            let seq = self.stats.queue_submit_count.wrapping_add(1);
            let wire_desc = Self::wire_desc(seq, &desc)?;
            let queue = self.queue_mut(desc.producer_entity, desc.consumer_entity)?;
            queue.push(wire_desc)?;
            self.stats.queue_submit_count += 1;
            self.pending_descs.insert(seq, desc);
            if self.profile.queue_auto_drain {
                self.drain_queue(wire_desc);
            }
            Ok(())
        }

        fn drain_consumer(&mut self, consumer_entity: u64) -> Result<(), &'static str> {
            if !self.profile.enabled {
                return Ok(());
            }
            if consumer_entity >= self.profile.node_count {
                return Err("obmm_pool_queue_consumer_oob");
            }
            let queue_keys = self
                .queues
                .keys()
                .copied()
                .filter(|(_, consumer)| *consumer == consumer_entity)
                .collect::<Vec<_>>();
            let mut delivered = Vec::new();
            for key in queue_keys {
                if let Some(queue) = self.queues.get_mut(&key) {
                    while let Some(wire_desc) = queue.pop() {
                        delivered.push(wire_desc);
                    }
                }
            }
            self.deliver_wire_descs(delivered);
            Ok(())
        }

        fn queue_mut(
            &mut self,
            producer_entity: u64,
            consumer_entity: u64,
        ) -> Result<&mut LingquObmmSpscQueue, &'static str> {
            if producer_entity >= self.profile.node_count
                || consumer_entity >= self.profile.node_count
            {
                return Err("obmm_pool_queue_entity_oob");
            }
            if !self
                .queues
                .contains_key(&(producer_entity, consumer_entity))
            {
                let queue = LingquObmmSpscQueue::new(self.profile.queue_depth)?;
                self.queues
                    .insert((producer_entity, consumer_entity), queue);
            }
            self.queues
                .get_mut(&(producer_entity, consumer_entity))
                .ok_or("obmm_pool_queue_missing")
        }

        fn drain_queue(&mut self, desc: LingquObmmQueueWireDesc) {
            let producer_entity = u64::from(desc.flags);
            let consumer_entity = u64::from(desc.region_id);
            let mut delivered = Vec::new();
            if let Some(queue) = self.queues.get_mut(&(producer_entity, consumer_entity)) {
                while let Some(wire_desc) = queue.pop() {
                    delivered.push(wire_desc);
                }
            }
            self.deliver_wire_descs(delivered);
        }

        fn deliver_wire_descs(&mut self, delivered: Vec<LingquObmmQueueWireDesc>) {
            for wire_desc in delivered {
                if let Some(desc) = self.pending_descs.remove(&wire_desc.seq) {
                    self.stats.queue_deliver_count += 1;
                    self.delivered_descs.push(desc);
                }
            }
        }

        fn wire_desc(
            seq: u64,
            desc: &LingquObmmQueueDesc,
        ) -> Result<LingquObmmQueueWireDesc, &'static str> {
            let region_id =
                u32::try_from(desc.consumer_entity).map_err(|_| "obmm_pool_queue_consumer_oob")?;
            let payload_len =
                u32::try_from(desc.bytes).map_err(|_| "obmm_pool_payload_too_large")?;
            let flags =
                u16::try_from(desc.producer_entity).map_err(|_| "obmm_pool_queue_producer_oob")?;
            Ok(LingquObmmQueueWireDesc {
                seq,
                region_id,
                payload_len,
                payload_offset: desc.offset,
                desc_type: LINGQU_OBMM_DESC_READY,
                flags,
                cookie: (desc.checksum ^ desc.version.rotate_left(17)) as u32,
            })
        }
    }

    fn align_up_u64(value: u64, align: u64) -> Result<u64, &'static str> {
        let mask = align.checked_sub(1).ok_or("obmm_pool_bad_alignment")?;
        value
            .checked_add(mask)
            .map(|value| value & !mask)
            .ok_or("obmm_pool_alignment_overflow")
    }

    #[derive(Debug)]
    pub struct LingquObjectServiceStub {
        profile: LingquObjectServiceProfile,
        records: HashMap<String, Vec<LingquObjectRecord>>,
        completions: VecDeque<QueuedCompletion>,
        next_op_id: u64,
        report: LingquObjectServiceReport,
        obmm_pool: LingquObmmPoolBackend,
    }

    impl LingquObjectServiceStub {
        pub fn new(profile: LingquObjectServiceProfile) -> Self {
            Self {
                profile,
                records: HashMap::new(),
                completions: VecDeque::new(),
                next_op_id: 0,
                report: LingquObjectServiceReport {
                    obmm_pool_enabled: profile.obmm_pool.enabled,
                    ..LingquObjectServiceReport::default()
                },
                obmm_pool: LingquObmmPoolBackend::new(profile.obmm_pool),
            }
        }

        fn next_handle(&mut self) -> ServiceOpHandle {
            self.next_op_id += 1;
            ServiceOpHandle(self.next_op_id)
        }

        fn ensure_queue_capacity(&self) -> Result<(), sim_core::SimError> {
            if self.completions.len() >= self.profile.queue_depth {
                return Err(sim_core::SimError::InvalidInput("object queue full"));
            }
            Ok(())
        }

        pub fn submit_publish(
            &mut self,
            req: LingquObjectPublishReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            let (status, ready_at) = self.publish_record(req.clone(), now);
            self.queue_completion(handle, req.task, status, ready_at);
            Ok(handle)
        }

        pub fn submit_append(
            &mut self,
            req: LingquObjectAppendReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let key = format!("{}/{}", req.base_key.trim_end_matches('/'), req.suffix);
            let publish_req = LingquObjectPublishReq {
                task: req.task.clone(),
                key,
                kind: req.kind,
                producer_entity: req.producer_entity,
                owner_entity: req.owner_entity,
                expected_version: req.previous_version,
                metadata: req.metadata,
                placements: req.placements,
                payload_bytes: req.payload_bytes,
            };
            let handle = self.next_handle();
            let (status, ready_at) = self.publish_record(publish_req, now);
            if matches!(status, CompletionStatus::Success) {
                self.report.append_count += 1;
            }
            self.queue_completion(handle, req.task, status, ready_at);
            Ok(handle)
        }

        pub fn submit_resolve(
            &mut self,
            req: LingquObjectResolveReq,
            now: SimTimestamp,
        ) -> Result<ServiceOpHandle, sim_core::SimError> {
            self.ensure_queue_capacity()?;
            let handle = self.next_handle();
            if let Err(code) = self.obmm_pool.drain_consumer(req.requester_entity) {
                self.report.metadata_get_count += 1;
                self.report.checksum = self.report_checksum();
                self.queue_completion(
                    handle,
                    req.task,
                    CompletionStatus::RetryableFailure {
                        code: code.to_string(),
                    },
                    now + self.profile.metadata_latency_us,
                );
                return Ok(handle);
            }
            let resolved = self.resolve_record(&req).cloned();
            let (status, latency) = match resolved.as_ref() {
                Some(record) if record.state == LingquObjectState::Quarantined => {
                    self.report.metadata_get_count += 1;
                    (
                        CompletionStatus::RetryableFailure {
                            code: "object_quarantined".to_string(),
                        },
                        self.profile.metadata_latency_us,
                    )
                }
                Some(record) if record.state == LingquObjectState::Committed => {
                    self.report.resolve_count += 1;
                    self.report.metadata_get_count += 1;
                    let placement = self.select_placement(record, &req.preferred_backends);
                    let payload_latency = self.record_read(placement);
                    self.report.checksum = self.report_checksum();
                    (
                        CompletionStatus::Success,
                        self.profile.metadata_latency_us + payload_latency,
                    )
                }
                Some(_) => {
                    self.report.metadata_get_count += 1;
                    (
                        CompletionStatus::RetryableFailure {
                            code: "object_pending".to_string(),
                        },
                        self.profile.metadata_latency_us,
                    )
                }
                None => {
                    self.report.metadata_get_count += 1;
                    self.report.missing_resolve_count += 1;
                    self.report.checksum = self.report_checksum();
                    (
                        CompletionStatus::RetryableFailure {
                            code: "object_missing".to_string(),
                        },
                        self.profile.metadata_latency_us,
                    )
                }
            };
            self.queue_completion(handle, req.task, status, now + latency);
            Ok(handle)
        }

        pub fn poll_ready(&mut self, now: SimTimestamp) -> Vec<CompletionEvent> {
            drain_ready(&mut self.completions, now)
        }

        pub fn report(&self) -> LingquObjectServiceReport {
            let mut report = self.report;
            let obmm = self.obmm_pool.stats();
            report.obmm_pool_enabled = self.profile.obmm_pool.enabled;
            report.obmm_pool_payload_write_count = obmm.payload_write_count;
            report.obmm_pool_payload_read_count = obmm.payload_read_count;
            report.obmm_pool_queue_submit_count = obmm.queue_submit_count;
            report.obmm_pool_queue_deliver_count = obmm.queue_deliver_count;
            report.obmm_pool_bytes_used = obmm.bytes_used;
            report.obmm_pool_reserved_bytes = obmm.reserved_bytes;
            report.obmm_pool_block_count = obmm.block_count;
            report.obmm_pool_multi_block_write_count = obmm.multi_block_write_count;
            report.obmm_pool_max_blocks_per_payload = obmm.max_blocks_per_payload;
            report.committed_object_count = self
                .records
                .values()
                .flatten()
                .filter(|record| record.state == LingquObjectState::Committed)
                .count() as u64;
            report.quarantined_object_count = self
                .records
                .values()
                .flatten()
                .filter(|record| record.state == LingquObjectState::Quarantined)
                .count() as u64;
            report.checksum = self.report_checksum();
            report
        }

        pub fn export_snapshot(&self) -> LingquObjectServiceSnapshot {
            let mut records = self.records.values().flatten().cloned().collect::<Vec<_>>();
            records.sort_by(|lhs, rhs| {
                lhs.key
                    .cmp(&rhs.key)
                    .then_with(|| lhs.version.cmp(&rhs.version))
            });
            LingquObjectServiceSnapshot {
                profile: self.profile,
                records,
            }
        }

        pub fn import_snapshot(
            snapshot: LingquObjectServiceSnapshot,
        ) -> Result<Self, sim_core::SimError> {
            let mut service = Self::new(snapshot.profile);
            for record in snapshot.records {
                Self::validate_snapshot_record(&record)?;
                if service
                    .records
                    .get(&record.key)
                    .map(|records| {
                        records
                            .iter()
                            .any(|existing| existing.version == record.version)
                    })
                    .unwrap_or(false)
                {
                    return Err(sim_core::SimError::InvalidInput(
                        "object snapshot duplicate key version",
                    ));
                }
                service
                    .obmm_pool
                    .import_record_payload(&record)
                    .map_err(sim_core::SimError::InvalidInput)?;
                service
                    .records
                    .entry(record.key.clone())
                    .or_default()
                    .push(record);
            }
            for records in service.records.values_mut() {
                records.sort_by_key(|record| record.version);
            }
            service.report.checksum = service.report_checksum();
            Ok(service)
        }

        fn validate_snapshot_record(record: &LingquObjectRecord) -> Result<(), sim_core::SimError> {
            if record.key.trim().is_empty() {
                return Err(sim_core::SimError::InvalidInput(
                    "object snapshot record key is empty",
                ));
            }
            if record.version == 0 {
                return Err(sim_core::SimError::InvalidInput(
                    "object snapshot record version is zero",
                ));
            }
            if record.payload_bytes.len() as u64 != record.bytes {
                return Err(sim_core::SimError::InvalidInput(
                    "object snapshot payload byte count mismatch",
                ));
            }
            for placement in &record.placements {
                if placement.bytes != record.bytes {
                    return Err(sim_core::SimError::InvalidInput(
                        "object snapshot placement byte count mismatch",
                    ));
                }
                if placement.checksum != record.checksum {
                    return Err(sim_core::SimError::InvalidInput(
                        "object snapshot placement checksum mismatch",
                    ));
                }
            }
            Ok(())
        }

        pub fn latest_record(&self, key: &str) -> Option<&LingquObjectRecord> {
            self.records
                .get(key)?
                .iter()
                .rev()
                .find(|record| record.state == LingquObjectState::Committed)
        }

        pub fn get_copy(&self, key: &str, version: LingquObjectVersionSelector) -> Option<Vec<u8>> {
            let record = self.record_for_selector(key, version)?;
            if let Some(placement) = self.select_placement(
                record,
                &[LingquPayloadBackend::ObmmShmem, LingquPayloadBackend::Shmem],
            ) {
                if let Some(payload) = self.obmm_pool.payloads.get(&placement.storage_ref) {
                    return Some(payload.payload.clone());
                }
            }
            Some(record.payload_bytes.clone())
        }

        pub fn get_ref(&self, key: &str, version: LingquObjectVersionSelector) -> Option<&[u8]> {
            let record = self.record_for_selector(key, version)?;
            if let Some(placement) = self.select_placement(
                record,
                &[LingquPayloadBackend::ObmmShmem, LingquPayloadBackend::Shmem],
            ) {
                if let Some(payload) = self.obmm_pool.get_ref(&placement.storage_ref) {
                    return Some(payload);
                }
            }
            Some(record.payload_bytes.as_slice())
        }

        pub fn quarantine_latest(&mut self, key: &str) -> bool {
            let Some(record) = self
                .records
                .get_mut(key)
                .and_then(|records| records.iter_mut().rev().next())
            else {
                return false;
            };
            record.state = LingquObjectState::Quarantined;
            self.report.checksum = self.report_checksum();
            true
        }

        fn publish_record(
            &mut self,
            req: LingquObjectPublishReq,
            now: SimTimestamp,
        ) -> (CompletionStatus, SimTimestamp) {
            self.report.publish_count += 1;
            self.report.metadata_put_count += 1;
            let next_version = self
                .records
                .get(&req.key)
                .and_then(|records| records.last())
                .map(|record| record.version + 1)
                .unwrap_or(1);
            if let Some(expected) = req.expected_version {
                let current = next_version.saturating_sub(1);
                if current != expected {
                    self.report.checksum = self.report_checksum();
                    return (
                        CompletionStatus::RetryableFailure {
                            code: "object_version_conflict".to_string(),
                        },
                        now + self.profile.metadata_latency_us,
                    );
                }
            }
            if req.metadata.bytes > self.profile.inline_value_limit && req.placements.is_empty() {
                self.report.checksum = self.report_checksum();
                return (
                    CompletionStatus::RetryableFailure {
                        code: "object_payload_too_large".to_string(),
                    },
                    now + self.profile.metadata_latency_us,
                );
            }
            let mut placements = req.placements;
            if let Err(code) = self.obmm_pool.publish_payloads(
                &mut placements,
                &req.payload_bytes,
                req.producer_entity,
                req.owner_entity,
                next_version,
            ) {
                self.report.checksum = self.report_checksum();
                return (
                    CompletionStatus::RetryableFailure {
                        code: code.to_string(),
                    },
                    now + self.profile.metadata_latency_us,
                );
            }
            let placement_latency = self.record_writes(&placements);
            let record = LingquObjectRecord {
                key: req.key.clone(),
                kind: req.kind,
                version: next_version,
                state: LingquObjectState::Committed,
                producer_entity: req.producer_entity,
                owner_entity: req.owner_entity,
                bytes: req.metadata.bytes,
                checksum: req.metadata.checksum,
                dtype: req.metadata.dtype,
                shape: req.metadata.shape,
                layout: req.metadata.layout,
                placements,
                payload_bytes: req.payload_bytes,
                created_at_us: now,
                committed_at_us: Some(now + self.profile.metadata_latency_us + placement_latency),
                expires_at_us: req.metadata.expires_at_us,
            };
            self.records.entry(req.key).or_default().push(record);
            self.report.checksum = self.report_checksum();
            (
                CompletionStatus::Success,
                now + self.profile.metadata_latency_us + placement_latency,
            )
        }

        fn resolve_record(&self, req: &LingquObjectResolveReq) -> Option<&LingquObjectRecord> {
            self.record_for_selector(&req.key, req.version)
                .filter(|record| self.state_satisfies(record.state, req.min_state))
        }

        fn record_for_selector(
            &self,
            key: &str,
            version: LingquObjectVersionSelector,
        ) -> Option<&LingquObjectRecord> {
            let records = self.records.get(key)?;
            match version {
                LingquObjectVersionSelector::LatestCommitted => {
                    records.iter().rev().find(|record| {
                        matches!(
                            record.state,
                            LingquObjectState::Committed | LingquObjectState::Quarantined
                        )
                    })
                }
                LingquObjectVersionSelector::Exact(version) => {
                    records.iter().find(|record| record.version == version)
                }
                LingquObjectVersionSelector::AtLeast(version) => {
                    records.iter().find(|record| record.version >= version)
                }
            }
        }

        fn state_satisfies(&self, state: LingquObjectState, min_state: LingquObjectState) -> bool {
            match min_state {
                LingquObjectState::Pending => true,
                LingquObjectState::Committed => {
                    matches!(
                        state,
                        LingquObjectState::Committed | LingquObjectState::Quarantined
                    )
                }
                LingquObjectState::Tombstoned => state == LingquObjectState::Tombstoned,
                LingquObjectState::Quarantined => state == LingquObjectState::Quarantined,
            }
        }

        fn select_placement<'a>(
            &self,
            record: &'a LingquObjectRecord,
            preferred: &[LingquPayloadBackend],
        ) -> Option<&'a LingquPayloadPlacement> {
            preferred
                .iter()
                .find_map(|backend| {
                    record
                        .placements
                        .iter()
                        .find(|placement| placement.backend == *backend)
                })
                .or_else(|| record.placements.first())
        }

        fn record_writes(&mut self, placements: &[LingquPayloadPlacement]) -> SimTimestamp {
            let mut latency = 0;
            if placements.is_empty() {
                self.report.inline_write_count += 1;
                return latency;
            }
            for placement in placements {
                match placement.backend {
                    LingquPayloadBackend::Inline => self.report.inline_write_count += 1,
                    LingquPayloadBackend::Shmem | LingquPayloadBackend::ObmmShmem => {
                        self.report.shmem_write_count += 1;
                        latency = latency.max(self.profile.shmem_latency_us);
                    }
                    LingquPayloadBackend::Block => {
                        self.report.block_write_count += 1;
                        latency = latency.max(self.profile.block_latency_us);
                    }
                    LingquPayloadBackend::Dfs | LingquPayloadBackend::External => {
                        latency = latency.max(self.profile.block_latency_us);
                    }
                }
            }
            latency
        }

        fn record_read(&mut self, placement: Option<&LingquPayloadPlacement>) -> SimTimestamp {
            match placement.map(|placement| placement.backend) {
                Some(LingquPayloadBackend::Inline) | None => {
                    self.report.inline_read_count += 1;
                    0
                }
                Some(LingquPayloadBackend::Shmem | LingquPayloadBackend::ObmmShmem) => {
                    self.report.shmem_read_count += 1;
                    self.obmm_pool.stats.payload_read_count += 1;
                    self.profile.shmem_latency_us
                }
                Some(LingquPayloadBackend::Block) => {
                    self.report.block_read_count += 1;
                    self.profile.block_latency_us
                }
                Some(LingquPayloadBackend::Dfs | LingquPayloadBackend::External) => {
                    self.profile.block_latency_us
                }
            }
        }

        fn queue_completion(
            &mut self,
            handle: ServiceOpHandle,
            task: Option<TaskKey>,
            status: CompletionStatus,
            ready_at: SimTimestamp,
        ) {
            self.completions.push_back(QueuedCompletion {
                ready_at,
                event: CompletionEvent {
                    op_id: handle.0,
                    task,
                    source: CompletionSource::DbService,
                    status,
                    finished_at: ready_at,
                },
            });
        }

        fn report_checksum(&self) -> u64 {
            let mut acc = 0xcbf2_9ce4_8422_2325u64;
            let mut keys = self.records.keys().collect::<Vec<_>>();
            keys.sort();
            for key in keys {
                acc = checksum_str(acc, key);
                if let Some(records) = self.records.get(key) {
                    for record in records {
                        acc = acc.wrapping_mul(0x0000_0100_0000_01b3)
                            ^ record.version
                            ^ record.bytes.rotate_left(7)
                            ^ record.checksum.rotate_left(13)
                            ^ record.producer_entity.rotate_left(17)
                            ^ record.placements.len() as u64;
                    }
                }
            }
            acc
        }
    }

    fn checksum_str(mut acc: u64, value: &str) -> u64 {
        for byte in value.as_bytes() {
            acc = acc.wrapping_mul(0x0000_0100_0000_01b3) ^ u64::from(*byte);
        }
        acc
    }
}

pub mod weights {
    use super::block::{BlockServiceProfile, BlockServiceStub};
    use super::db::{DbPutReq, DbServiceProfile, DbServiceStub};
    use super::shmem::{ShmemPutReq, ShmemServiceProfile, ShmemServiceStub};
    use super::*;

    #[derive(Debug, Clone, Copy, Eq, PartialEq)]
    pub enum WeightStorageKind {
        Block,
        Shmem,
    }

    #[derive(Debug, Clone, Copy, Eq, PartialEq)]
    pub enum ServiceObjectKind {
        WeightShard,
        KvCacheBlock,
        ActivationTile,
        PartialResultTile,
    }

    #[derive(Debug, Clone)]
    pub struct WeightMetadataPut {
        pub key: String,
        pub bytes: u64,
    }

    #[derive(Debug, Clone)]
    pub struct WeightPayloadWrite {
        pub storage_ref: String,
        pub storage_kind: WeightStorageKind,
        pub segment: SegmentHandle,
        pub offset: u64,
        pub bytes: u64,
        pub checksum: u64,
    }

    #[derive(Debug, Clone)]
    pub struct WeightsLoadReq {
        pub task: Option<TaskKey>,
        pub requester_entity: u32,
        pub metadata_puts: Vec<WeightMetadataPut>,
        pub payload_writes: Vec<WeightPayloadWrite>,
    }

    #[derive(Debug, Clone)]
    pub struct ServiceObjectMetadataPut {
        pub key: String,
        pub object_kind: ServiceObjectKind,
        pub bytes: u64,
    }

    #[derive(Debug, Clone)]
    pub struct ServiceObjectPayloadWrite {
        pub storage_ref: String,
        pub object_kind: ServiceObjectKind,
        pub storage_kind: WeightStorageKind,
        pub segment: SegmentHandle,
        pub offset: u64,
        pub bytes: u64,
        pub checksum: u64,
        pub producer_entity: u32,
    }

    #[derive(Debug, Clone)]
    pub struct ServiceObjectPublishReq {
        pub task: Option<TaskKey>,
        pub requester_entity: u32,
        pub metadata_puts: Vec<ServiceObjectMetadataPut>,
        pub payload_writes: Vec<ServiceObjectPayloadWrite>,
    }

    #[derive(Debug, Clone)]
    pub struct WeightsResolveReq {
        pub task: Option<TaskKey>,
        pub requester_entity: u32,
        pub metadata_key: String,
        pub storage_ref: String,
        pub storage_kind: WeightStorageKind,
        pub segment: SegmentHandle,
        pub bytes: u64,
    }

    #[derive(Debug, Clone)]
    pub struct ServiceObjectResolveReq {
        pub task: Option<TaskKey>,
        pub requester_entity: u32,
        pub metadata_key: String,
        pub object_kind: ServiceObjectKind,
        pub storage_ref: String,
        pub storage_kind: WeightStorageKind,
        pub segment: SegmentHandle,
        pub bytes: u64,
    }

    #[derive(Debug, Clone, Copy, Eq, PartialEq)]
    pub struct WeightsLoadStats {
        pub metadata_puts: usize,
        pub block_writes: usize,
        pub shmem_writes: usize,
        pub payload_bytes: u64,
    }

    #[derive(Debug, Clone, Copy, Eq, PartialEq)]
    pub struct WeightsResolveStats {
        pub metadata_gets: usize,
        pub block_reads: usize,
        pub shmem_reads: usize,
    }

    #[derive(Debug)]
    pub struct WeightsServiceStub {
        db_service: DbServiceStub,
        block_service: BlockServiceStub,
        shmem_service: ShmemServiceStub,
        loaded_payloads: HashMap<String, ServiceObjectPayloadWrite>,
    }

    impl WeightsServiceStub {
        pub fn new() -> Self {
            Self::with_profiles(
                DbServiceProfile {
                    queue_depth: 4096,
                    pipeline_batch_limit: 4096,
                    ..DbServiceProfile::default()
                },
                BlockServiceProfile {
                    queue_depth: 4096,
                    ..BlockServiceProfile::default()
                },
                ShmemServiceProfile {
                    queue_depth: 4096,
                    max_segments: 4096,
                    ..ShmemServiceProfile::default()
                },
            )
        }

        pub fn with_profiles(
            db_profile: DbServiceProfile,
            block_profile: BlockServiceProfile,
            shmem_profile: ShmemServiceProfile,
        ) -> Self {
            Self {
                db_service: DbServiceStub::new(db_profile),
                block_service: BlockServiceStub::with_profile(block_profile),
                shmem_service: ShmemServiceStub::new(shmem_profile),
                loaded_payloads: HashMap::new(),
            }
        }

        pub fn submit_load(
            &mut self,
            req: WeightsLoadReq,
            now: SimTimestamp,
        ) -> Result<WeightsLoadStats, sim_core::SimError> {
            let publish_req = ServiceObjectPublishReq {
                task: req.task,
                requester_entity: req.requester_entity,
                metadata_puts: req
                    .metadata_puts
                    .into_iter()
                    .map(|metadata| ServiceObjectMetadataPut {
                        key: metadata.key,
                        object_kind: ServiceObjectKind::WeightShard,
                        bytes: metadata.bytes,
                    })
                    .collect(),
                payload_writes: req
                    .payload_writes
                    .into_iter()
                    .map(|payload| ServiceObjectPayloadWrite {
                        storage_ref: payload.storage_ref,
                        object_kind: ServiceObjectKind::WeightShard,
                        storage_kind: payload.storage_kind,
                        segment: payload.segment,
                        offset: payload.offset,
                        bytes: payload.bytes,
                        checksum: payload.checksum,
                        producer_entity: req.requester_entity,
                    })
                    .collect(),
            };
            self.submit_publish_object(publish_req, now)
        }

        pub fn submit_publish_object(
            &mut self,
            req: ServiceObjectPublishReq,
            now: SimTimestamp,
        ) -> Result<WeightsLoadStats, sim_core::SimError> {
            let mut stats = WeightsLoadStats {
                metadata_puts: 0,
                block_writes: 0,
                shmem_writes: 0,
                payload_bytes: 0,
            };
            for metadata in req.metadata_puts {
                self.db_service.submit_put(
                    DbPutReq {
                        task: req.task.clone(),
                        key: metadata.key,
                        bytes: metadata.bytes,
                    },
                    now,
                )?;
                stats.metadata_puts += 1;
            }
            for payload in req.payload_writes {
                match payload.storage_kind {
                    WeightStorageKind::Block => {
                        self.block_service.submit_write(
                            BlockWriteReq {
                                task: req.task.clone(),
                                block: BlockHash(payload.storage_ref.clone()),
                            },
                            now,
                        )?;
                        stats.block_writes += 1;
                    }
                    WeightStorageKind::Shmem => {
                        self.shmem_service.register_segment(
                            payload.segment,
                            req.requester_entity,
                            payload.bytes,
                        )?;
                        self.shmem_service.submit_put(
                            ShmemPutReq {
                                task: req.task.clone(),
                                requester_entity: req.requester_entity,
                                segment: payload.segment,
                                bytes: payload.bytes,
                            },
                            now,
                        )?;
                        stats.shmem_writes += 1;
                    }
                }
                stats.payload_bytes += payload.bytes;
                self.loaded_payloads
                    .insert(payload.storage_ref.clone(), payload);
            }
            Ok(stats)
        }

        pub fn submit_resolve(
            &mut self,
            req: WeightsResolveReq,
            now: SimTimestamp,
        ) -> Result<WeightsResolveStats, sim_core::SimError> {
            self.submit_resolve_object(
                ServiceObjectResolveReq {
                    task: req.task,
                    requester_entity: req.requester_entity,
                    metadata_key: req.metadata_key,
                    object_kind: ServiceObjectKind::WeightShard,
                    storage_ref: req.storage_ref,
                    storage_kind: req.storage_kind,
                    segment: req.segment,
                    bytes: req.bytes,
                },
                now,
            )
        }

        pub fn submit_resolve_object(
            &mut self,
            req: ServiceObjectResolveReq,
            now: SimTimestamp,
        ) -> Result<WeightsResolveStats, sim_core::SimError> {
            self.db_service.submit_get(
                super::db::DbGetReq {
                    task: req.task.clone(),
                    key: req.metadata_key,
                },
                now,
            )?;
            let mut stats = WeightsResolveStats {
                metadata_gets: 1,
                block_reads: 0,
                shmem_reads: 0,
            };
            match req.storage_kind {
                WeightStorageKind::Block => {
                    self.block_service.submit_read(
                        BlockReadReq {
                            task: req.task.clone(),
                            block: BlockHash(req.storage_ref),
                        },
                        now,
                    )?;
                    stats.block_reads += 1;
                }
                WeightStorageKind::Shmem => {
                    self.shmem_service.submit_get(
                        super::shmem::ShmemGetReq {
                            task: req.task,
                            requester_entity: req.requester_entity,
                            segment: req.segment,
                            bytes: req.bytes,
                        },
                        now,
                    )?;
                    stats.shmem_reads += 1;
                }
            }
            Ok(stats)
        }

        pub fn payload_loaded(&self, storage_ref: &str) -> bool {
            self.loaded_payloads.contains_key(storage_ref)
        }

        pub fn poll_ready(&mut self, now: SimTimestamp) -> Vec<CompletionEvent> {
            let mut events = Vec::new();
            events.extend(self.db_service.poll_ready(now));
            events.extend(self.block_service.poll_ready(now));
            events.extend(self.shmem_service.poll_ready(now));
            events
        }
    }
}

#[cfg(test)]
mod tests {
    use super::block::{BlockServiceProfile, BlockServiceStub};
    use super::db::{DbGetReq, DbPutReq, DbServiceProfile, DbServiceStub};
    use super::dfs::{DfsReadReq, DfsServiceProfile, DfsServiceStub, DfsWriteReq};
    use super::durable::{
        LingquBlockPayloadRef, LingquBlockWriteOptions, LingquDfsAppendOptions,
        LingquDfsContentRef, LingquDfsContentType, LingquDfsListOptions, LingquDfsWriteOptions,
        LingquDurableBatchOp, LingquDurableBatchOutcome, LingquDurableError, LingquDurableSim,
        LingquVersionSelector,
    };
    use super::object::{
        LingquObjectKind, LingquObjectLocality, LingquObjectMetadata, LingquObjectPublishReq,
        LingquObjectResolveReq, LingquObjectServiceProfile, LingquObjectServiceSnapshot,
        LingquObjectServiceStub, LingquObjectState, LingquObjectVersionSelector,
        LingquObmmObjectRefWire, LingquObmmPoolProfile, LingquPayloadBackend,
        LingquPayloadPlacement, LINGQU_OBJECT_STATE_COMMITTED_WIRE,
        LINGQU_OBMM_OBJECT_REF_LAYOUT_VERSION, LINGQU_OBMM_OBJECT_REF_MAGIC,
    };
    use super::shmem::{ShmemGetReq, ShmemPutReq, ShmemServiceProfile, ShmemServiceStub};
    use super::weights::{
        ServiceObjectKind, ServiceObjectMetadataPut, ServiceObjectPayloadWrite,
        ServiceObjectPublishReq, ServiceObjectResolveReq, WeightMetadataPut, WeightPayloadWrite,
        WeightStorageKind, WeightsLoadReq, WeightsResolveReq, WeightsServiceStub,
    };
    use sim_core::{BlockHash, CompletionSource, CompletionStatus, SegmentHandle};
    use sim_runtime::{BlockReadReq, BlockWriteReq};

    fn object_metadata(bytes: u64, checksum: u64) -> LingquObjectMetadata {
        LingquObjectMetadata {
            bytes,
            checksum,
            dtype: None,
            shape: Vec::new(),
            layout: None,
            expires_at_us: None,
        }
    }

    fn object_placement(backend: LingquPayloadBackend, bytes: u64) -> LingquPayloadPlacement {
        LingquPayloadPlacement {
            backend,
            storage_ref: format!("object/{backend:?}/{bytes}"),
            segment: Some(SegmentHandle(10)),
            offset: 0,
            bytes,
            checksum: bytes ^ 0x55aa,
            locality: LingquObjectLocality::DomainShared(0),
        }
    }

    #[test]
    fn obmm_object_ref_wire_is_stable_and_validates_committed_refs() {
        assert_eq!(std::mem::size_of::<LingquObmmObjectRefWire>(), 64);
        let reference =
            LingquObmmObjectRefWire::committed(5, 2, 1, 7, 0x1234, 0x200000, 4096, 0xfeed);

        assert_eq!(reference.magic, LINGQU_OBMM_OBJECT_REF_MAGIC);
        assert_eq!(
            reference.layout_version,
            LINGQU_OBMM_OBJECT_REF_LAYOUT_VERSION
        );
        assert_eq!(reference.state, LINGQU_OBJECT_STATE_COMMITTED_WIRE);
        assert_eq!(reference.owner_entity, 2);
        assert_eq!(reference.payload_offset, 0x200000);
        assert_eq!(reference.payload_bytes, 4096);
        reference.validate().expect("committed object ref");
    }

    fn payload_checksum_fnv1a(bytes: &[u8]) -> u64 {
        let mut acc = 0xcbf2_9ce4_8422_2325u64;
        for byte in bytes {
            acc ^= u64::from(*byte);
            acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
        }
        acc
    }

    fn growing_payload(seed: u64, len: usize) -> Vec<u8> {
        let mut payload = Vec::with_capacity(len);
        for index in 0..len {
            let value = seed
                .wrapping_add((index as u64).wrapping_mul(31))
                .wrapping_add((index as u64) >> 7);
            payload.push((value & 0xff) as u8);
        }
        payload
    }

    #[test]
    fn block_service_stub_write_then_read_completes() {
        let mut svc = BlockServiceStub::new();
        let block = BlockHash("block-0".into());

        svc.submit_write(
            BlockWriteReq {
                task: None,
                block: block.clone(),
            },
            10,
        )
        .expect("write");
        assert!(svc.poll_ready(17).is_empty());
        let write_events = svc.poll_ready(18);
        assert_eq!(write_events.len(), 1);
        assert_eq!(write_events[0].status, CompletionStatus::Success);

        svc.submit_read(BlockReadReq { task: None, block }, 20)
            .expect("read");
        assert!(svc.poll_ready(24).is_empty());
        let read_events = svc.poll_ready(25);
        assert_eq!(read_events.len(), 1);
        assert_eq!(read_events[0].status, CompletionStatus::Success);
    }

    #[test]
    fn block_service_stub_miss_uses_slower_latency() {
        let mut svc = BlockServiceStub::new();

        svc.submit_read(
            BlockReadReq {
                task: None,
                block: BlockHash("missing".into()),
            },
            50,
        )
        .expect("read miss");
        assert!(svc.poll_ready(79).is_empty());
        let miss_events = svc.poll_ready(80);
        assert_eq!(miss_events.len(), 1);
        assert_eq!(
            miss_events[0].status,
            CompletionStatus::RetryableFailure {
                code: "block_miss".into()
            }
        );
    }

    #[test]
    fn block_service_stub_writeback_cleans_dirty_block() {
        let mut svc = BlockServiceStub::new();
        let block = BlockHash("block-1".into());

        svc.submit_write(
            BlockWriteReq {
                task: None,
                block: block.clone(),
            },
            0,
        )
        .expect("write");
        let _ = svc.poll_ready(8);

        svc.submit_writeback(block, None, 10).expect("writeback");
        assert!(svc.poll_ready(29).is_empty());
        let events = svc.poll_ready(30);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].status, CompletionStatus::Success);
    }

    #[test]
    fn block_service_stub_rejects_when_queue_is_full() {
        let mut svc = BlockServiceStub::with_profile(BlockServiceProfile {
            queue_depth: 1,
            ..BlockServiceProfile::default()
        });

        svc.submit_write(
            BlockWriteReq {
                task: None,
                block: BlockHash("block-a".into()),
            },
            0,
        )
        .expect("first write");

        let err = svc
            .submit_write(
                BlockWriteReq {
                    task: None,
                    block: BlockHash("block-b".into()),
                },
                1,
            )
            .expect_err("queue full");
        assert!(matches!(
            err,
            sim_core::SimError::InvalidInput("block queue full")
        ));
    }

    #[test]
    fn durable_sim_dfs_write_read_snapshot_roundtrip() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/session/a/prefix-cache.json";
        sim.dfs_write(
            path,
            br#"{"cache":"ready"}"#.to_vec(),
            LingquDfsWriteOptions {
                content_type: LingquDfsContentType::Json,
                ..LingquDfsWriteOptions::default()
            },
        )
        .expect("write dfs file");

        let snapshot = sim.export_snapshot().expect("export snapshot");
        let bytes = snapshot.to_json_bytes().expect("encode snapshot");
        let decoded = super::durable::LingquDurableSimSnapshot::from_json_bytes(&bytes)
            .expect("decode snapshot");
        let mut restored = LingquDurableSim::import_snapshot(decoded).expect("import snapshot");

        assert_eq!(
            restored
                .dfs_read(path, LingquVersionSelector::LatestCommitted)
                .expect("read restored file"),
            br#"{"cache":"ready"}"#.to_vec()
        );
    }

    #[test]
    fn durable_sim_large_dfs_payload_is_block_backed() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/session/a/hot-state.bin";
        let payload = growing_payload(7, 128);
        sim.dfs_write(
            path,
            payload.clone(),
            LingquDfsWriteOptions {
                inline_threshold_bytes: 16,
                ..LingquDfsWriteOptions::default()
            },
        )
        .expect("write large dfs file");

        let stat = sim
            .dfs_stat(path, LingquVersionSelector::LatestCommitted)
            .expect("stat large dfs file");
        assert!(matches!(stat.content_ref, LingquDfsContentRef::Block(_)));
        assert_eq!(
            sim.dfs_read(path, LingquVersionSelector::LatestCommitted)
                .expect("read block backed file"),
            payload
        );
    }

    #[test]
    fn durable_sim_dfs_list_reports_latest_namespace_entries() {
        let mut sim = LingquDurableSim::default();
        sim.dfs_write(
            "/lingqu/memory/session/a/catalog.json",
            b"v1".to_vec(),
            LingquDfsWriteOptions::default(),
        )
        .expect("write v1");
        sim.dfs_write(
            "/lingqu/memory/session/a/catalog.json",
            b"v2".to_vec(),
            LingquDfsWriteOptions::default(),
        )
        .expect("write v2");
        sim.dfs_write(
            "/lingqu/memory/session/a/query.json",
            b"q".to_vec(),
            LingquDfsWriteOptions::default(),
        )
        .expect("write query");
        sim.dfs_write(
            "/lingqu/memory/session/b/catalog.json",
            b"other".to_vec(),
            LingquDfsWriteOptions::default(),
        )
        .expect("write other");

        let entries = sim
            .dfs_list(LingquDfsListOptions::new("/lingqu/memory/session/a/"))
            .expect("list prefix");
        assert_eq!(entries.len(), 2);
        assert_eq!(entries[0].path, "/lingqu/memory/session/a/catalog.json");
        assert_eq!(entries[0].version, 2);
        assert_eq!(entries[1].path, "/lingqu/memory/session/a/query.json");
    }

    #[test]
    fn durable_sim_append_log_round_trips_with_checksum_chain() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/audit/shortpath.log";
        let first = sim
            .dfs_append_log_append(
                path,
                br#"{"decision":"continue"}"#.to_vec(),
                LingquDfsAppendOptions {
                    expected_next_seq: Some(1),
                    ..LingquDfsAppendOptions::default()
                },
            )
            .expect("append first");
        let second = sim
            .dfs_append_log_append(
                path,
                br#"{"decision":"jump"}"#.to_vec(),
                LingquDfsAppendOptions {
                    expected_next_seq: Some(2),
                    ..LingquDfsAppendOptions::default()
                },
            )
            .expect("append second");

        assert_eq!(first.seq, 1);
        assert_eq!(second.seq, 2);
        assert_eq!(second.prev_chain_checksum, first.chain_checksum);
        let snapshot = sim.export_snapshot().expect("export snapshot");
        let bytes = snapshot.to_json_bytes().expect("snapshot json");
        let decoded = super::durable::LingquDurableSimSnapshot::from_json_bytes(&bytes)
            .expect("decode snapshot");
        let mut restored = LingquDurableSim::import_snapshot(decoded).expect("import snapshot");
        let records = restored
            .dfs_append_log_read(path, 1, None)
            .expect("read append log");
        assert_eq!(records.len(), 2);
        assert_eq!(records[0].bytes, br#"{"decision":"continue"}"#.to_vec());
        assert_eq!(records[1].bytes, br#"{"decision":"jump"}"#.to_vec());
    }

    #[test]
    fn durable_sim_append_log_rejects_stale_expected_seq() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/audit/prefetch.log";
        sim.dfs_append_log_append(path, b"first".to_vec(), LingquDfsAppendOptions::default())
            .expect("append first");

        let err = sim
            .dfs_append_log_append(
                path,
                b"second".to_vec(),
                LingquDfsAppendOptions {
                    expected_next_seq: Some(1),
                    ..LingquDfsAppendOptions::default()
                },
            )
            .expect_err("stale append seq must fail");
        assert!(matches!(err, LingquDurableError::VersionConflict { .. }));
    }

    #[test]
    fn durable_sim_batch_commit_applies_all_operations_atomically() {
        let mut sim = LingquDurableSim::default();
        let outcomes = sim
            .commit_batch(vec![
                LingquDurableBatchOp::BlockWrite {
                    block: "block/memory/batch/payload".to_string(),
                    bytes: b"payload".to_vec(),
                    options: LingquBlockWriteOptions::default(),
                },
                LingquDurableBatchOp::DfsWrite {
                    path: "/lingqu/memory/batch/catalog.json".to_string(),
                    bytes: br#"{"ok":true}"#.to_vec(),
                    options: LingquDfsWriteOptions {
                        content_type: LingquDfsContentType::Json,
                        ..LingquDfsWriteOptions::default()
                    },
                },
                LingquDurableBatchOp::DfsAppendLog {
                    path: "/lingqu/memory/batch/audit.log".to_string(),
                    bytes: b"committed".to_vec(),
                    options: LingquDfsAppendOptions::default(),
                },
            ])
            .expect("commit batch");

        assert_eq!(outcomes.len(), 3);
        assert!(matches!(
            outcomes[0],
            LingquDurableBatchOutcome::BlockPayloadRef(_)
        ));
        assert_eq!(
            sim.dfs_read(
                "/lingqu/memory/batch/catalog.json",
                LingquVersionSelector::LatestCommitted,
            )
            .expect("read batch DFS file"),
            br#"{"ok":true}"#.to_vec()
        );
        assert_eq!(
            sim.dfs_append_log_read("/lingqu/memory/batch/audit.log", 1, None)
                .expect("read batch append log")[0]
                .bytes,
            b"committed".to_vec()
        );
    }

    #[test]
    fn durable_sim_batch_commit_rolls_back_on_conflict() {
        let mut sim = LingquDurableSim::default();
        sim.dfs_write(
            "/lingqu/memory/batch/existing.json",
            b"v1".to_vec(),
            LingquDfsWriteOptions::default(),
        )
        .expect("write existing");

        let err = sim
            .commit_batch(vec![
                LingquDurableBatchOp::DfsWrite {
                    path: "/lingqu/memory/batch/new.json".to_string(),
                    bytes: b"new".to_vec(),
                    options: LingquDfsWriteOptions::default(),
                },
                LingquDurableBatchOp::DfsWrite {
                    path: "/lingqu/memory/batch/existing.json".to_string(),
                    bytes: b"v2".to_vec(),
                    options: LingquDfsWriteOptions {
                        expected_version: Some(7),
                        ..LingquDfsWriteOptions::default()
                    },
                },
            ])
            .expect_err("conflicting batch must fail");
        assert!(matches!(err, LingquDurableError::VersionConflict { .. }));
        assert!(matches!(
            sim.dfs_read(
                "/lingqu/memory/batch/new.json",
                LingquVersionSelector::LatestCommitted
            ),
            Err(LingquDurableError::MissingDfsPath(_))
        ));
        assert_eq!(
            sim.dfs_read(
                "/lingqu/memory/batch/existing.json",
                LingquVersionSelector::LatestCommitted
            )
            .expect("existing file survives"),
            b"v1".to_vec()
        );
    }

    #[test]
    fn durable_sim_import_rejects_corrupt_append_log_chain() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/audit/corrupt.log";
        sim.dfs_append_log_append(path, b"first".to_vec(), LingquDfsAppendOptions::default())
            .expect("append first");
        sim.dfs_append_log_append(path, b"second".to_vec(), LingquDfsAppendOptions::default())
            .expect("append second");
        let mut snapshot = sim.export_snapshot().expect("export snapshot");
        snapshot.dfs.append_logs[1].prev_chain_checksum ^= 1;

        let err = LingquDurableSim::import_snapshot(snapshot)
            .expect_err("corrupt append chain must fail import");
        assert!(matches!(
            err,
            LingquDurableError::ChecksumMismatch { .. } | LingquDurableError::SnapshotValidation(_)
        ));
    }

    #[test]
    fn durable_sim_block_ref_rejects_checksum_mismatch() {
        let mut sim = LingquDurableSim::default();
        let mut payload_ref = sim
            .block_write(
                "block/memory/hot-state/0",
                b"persistent state".to_vec(),
                LingquBlockWriteOptions::default(),
            )
            .expect("write block");
        payload_ref.checksum ^= 0x55aa;

        let err = sim
            .block_read(&payload_ref)
            .expect_err("checksum mismatch must fail");
        assert!(matches!(err, LingquDurableError::ChecksumMismatch { .. }));
        assert_eq!(sim.stats().checksum_failures, 1);
    }

    #[test]
    fn durable_sim_dfs_tombstone_hides_latest_but_keeps_exact_version() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/session/a/artifact.json";
        sim.dfs_write(path, b"v1".to_vec(), LingquDfsWriteOptions::default())
            .expect("write v1");
        sim.dfs_tombstone(path, Some(1)).expect("tombstone");

        let latest_err = sim
            .dfs_read(path, LingquVersionSelector::LatestCommitted)
            .expect_err("latest tombstone must hide file");
        assert!(matches!(latest_err, LingquDurableError::Tombstoned(_)));
        assert_eq!(
            sim.dfs_read(path, LingquVersionSelector::Exact(1))
                .expect("exact committed version survives"),
            b"v1".to_vec()
        );
    }

    #[test]
    fn durable_sim_expected_version_conflict_fails() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/session/a/index.json";
        sim.dfs_write(path, b"v1".to_vec(), LingquDfsWriteOptions::default())
            .expect("write v1");

        let err = sim
            .dfs_write(
                path,
                b"v2".to_vec(),
                LingquDfsWriteOptions {
                    expected_version: Some(7),
                    ..LingquDfsWriteOptions::default()
                },
            )
            .expect_err("stale expected version must fail");
        assert!(matches!(err, LingquDurableError::VersionConflict { .. }));
        assert_eq!(sim.stats().version_conflicts, 1);
    }

    #[test]
    fn durable_sim_sealed_block_rejects_overwrite() {
        let mut sim = LingquDurableSim::default();
        sim.block_write(
            "block/memory/prefix-cache/0",
            b"sealed".to_vec(),
            LingquBlockWriteOptions {
                seal: true,
                ..LingquBlockWriteOptions::default()
            },
        )
        .expect("write sealed block");

        let err = sim
            .block_write(
                "block/memory/prefix-cache/0",
                b"new bytes".to_vec(),
                LingquBlockWriteOptions::default(),
            )
            .expect_err("sealed block must reject overwrite");
        assert!(matches!(err, LingquDurableError::Sealed(_)));
    }

    #[test]
    fn durable_sim_import_rejects_duplicate_dfs_versions() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/session/a/duplicate.json";
        sim.dfs_write(path, b"v1".to_vec(), LingquDfsWriteOptions::default())
            .expect("write v1");
        let mut snapshot = sim.export_snapshot().expect("export snapshot");
        let duplicate = snapshot.dfs.files[0].clone();
        snapshot.dfs.files.push(duplicate);

        let err = LingquDurableSim::import_snapshot(snapshot)
            .expect_err("duplicate DFS version must fail import");
        assert!(matches!(err, LingquDurableError::SnapshotValidation(_)));
    }

    #[test]
    fn durable_sim_import_rejects_missing_block_backed_dfs_payload() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/session/a/block-backed.bin";
        sim.dfs_write(
            path,
            growing_payload(9, 64),
            LingquDfsWriteOptions {
                inline_threshold_bytes: 8,
                ..LingquDfsWriteOptions::default()
            },
        )
        .expect("write block-backed DFS file");
        let mut snapshot = sim.export_snapshot().expect("export snapshot");
        snapshot.block.blocks.clear();

        let err = LingquDurableSim::import_snapshot(snapshot)
            .expect_err("missing block-backed DFS payload must fail import");
        assert!(matches!(err, LingquDurableError::MissingBlock(_)));
    }

    #[test]
    fn durable_sim_block_ref_rejects_range_overflow() {
        let mut sim = LingquDurableSim::default();
        let mut payload_ref = sim
            .block_write(
                "block/memory/range/0",
                b"range payload".to_vec(),
                LingquBlockWriteOptions::default(),
            )
            .expect("write block");
        payload_ref.offset = 8;
        payload_ref.bytes = 128;

        let err = sim
            .block_read(&payload_ref)
            .expect_err("range overflow must fail");
        assert!(matches!(err, LingquDurableError::RangeOverflow(_)));
    }

    #[test]
    fn durable_sim_import_rejects_block_ref_range_overflow() {
        let mut sim = LingquDurableSim::default();
        let path = "/lingqu/memory/session/a/range.bin";
        sim.dfs_write(
            path,
            growing_payload(11, 64),
            LingquDfsWriteOptions {
                inline_threshold_bytes: 8,
                ..LingquDfsWriteOptions::default()
            },
        )
        .expect("write block-backed DFS file");
        let mut snapshot = sim.export_snapshot().expect("export snapshot");
        let payload_ref = match &mut snapshot.dfs.files[0].content_ref {
            LingquDfsContentRef::Block(payload_ref) => payload_ref,
            LingquDfsContentRef::Inline(_) => panic!("expected block-backed DFS payload"),
        };
        *payload_ref = LingquBlockPayloadRef {
            block: payload_ref.block.clone(),
            version: payload_ref.version,
            offset: 32,
            bytes: 128,
            checksum: payload_ref.checksum,
        };

        let err = LingquDurableSim::import_snapshot(snapshot)
            .expect_err("overflowing DFS block ref must fail import");
        assert!(matches!(
            err,
            LingquDurableError::SnapshotValidation(_) | LingquDurableError::RangeOverflow(_)
        ));
    }

    #[test]
    fn shmem_service_stub_applies_latency_and_round_trips() {
        let mut svc = ShmemServiceStub::new(ShmemServiceProfile::default());
        svc.register_segment(SegmentHandle(1), 0, 4096)
            .expect("register segment");

        svc.submit_put(
            ShmemPutReq {
                task: None,
                requester_entity: 0,
                segment: SegmentHandle(1),
                bytes: 4096,
            },
            10,
        )
        .expect("put");
        assert!(svc.poll_ready(12).is_empty());
        let put_events = svc.poll_ready(13);
        assert_eq!(put_events.len(), 1);
        assert_eq!(put_events[0].status, CompletionStatus::Success);

        svc.submit_get(
            ShmemGetReq {
                task: None,
                requester_entity: 0,
                segment: SegmentHandle(1),
                bytes: 4096,
            },
            20,
        )
        .expect("get");
        let get_events = svc.poll_ready(23);
        assert_eq!(get_events.len(), 1);
        assert_eq!(get_events[0].status, CompletionStatus::Success);
    }

    #[test]
    fn shmem_service_stub_rejects_when_queue_is_full() {
        let mut svc = ShmemServiceStub::new(ShmemServiceProfile {
            queue_depth: 1,
            ..ShmemServiceProfile::default()
        });
        svc.register_segment(SegmentHandle(9), 0, 1024)
            .expect("register segment");

        svc.submit_put(
            ShmemPutReq {
                task: None,
                requester_entity: 0,
                segment: SegmentHandle(9),
                bytes: 512,
            },
            0,
        )
        .expect("first put");

        let err = svc
            .submit_get(
                ShmemGetReq {
                    task: None,
                    requester_entity: 0,
                    segment: SegmentHandle(9),
                    bytes: 512,
                },
                1,
            )
            .expect_err("queue full");
        assert!(matches!(
            err,
            sim_core::SimError::InvalidInput("shmem queue full")
        ));
    }

    #[test]
    fn dfs_service_stub_applies_metadata_and_data_latency() {
        let mut svc = DfsServiceStub::new(DfsServiceProfile::default());

        svc.submit_write(
            DfsWriteReq {
                task: None,
                path: "/weights/layer0.bin".into(),
                bytes: 8192,
            },
            100,
        )
        .expect("write");
        assert!(svc.poll_ready(199).is_empty());
        let write_events = svc.poll_ready(200);
        assert_eq!(write_events.len(), 1);
        assert_eq!(write_events[0].status, CompletionStatus::Success);

        svc.submit_read(
            DfsReadReq {
                task: None,
                path: "/weights/layer0.bin".into(),
            },
            300,
        )
        .expect("read");
        assert!(svc.poll_ready(474).is_empty());
        let read_events = svc.poll_ready(475);
        assert_eq!(read_events.len(), 1);
        assert_eq!(read_events[0].status, CompletionStatus::Success);
    }

    #[test]
    fn dfs_service_stub_distinguishes_cold_and_warm_reads() {
        let mut svc = DfsServiceStub::new(DfsServiceProfile::default());
        svc.submit_write(
            DfsWriteReq {
                task: None,
                path: "/weights/layer1.bin".into(),
                bytes: 4096,
            },
            0,
        )
        .expect("write");
        let _ = svc.poll_ready(100);

        svc.submit_read(
            DfsReadReq {
                task: None,
                path: "/weights/layer1.bin".into(),
            },
            100,
        )
        .expect("cold read");
        assert!(svc.poll_ready(274).is_empty());
        let cold = svc.poll_ready(275);
        assert_eq!(cold.len(), 1);
        assert_eq!(cold[0].status, CompletionStatus::Success);

        svc.submit_read(
            DfsReadReq {
                task: None,
                path: "/weights/layer1.bin".into(),
            },
            300,
        )
        .expect("warm read");
        assert!(svc.poll_ready(399).is_empty());
        let warm = svc.poll_ready(400);
        assert_eq!(warm.len(), 1);
        assert_eq!(warm[0].status, CompletionStatus::Success);
    }

    #[test]
    fn dfs_service_stub_rejects_when_queue_is_full() {
        let mut svc = DfsServiceStub::new(DfsServiceProfile {
            queue_depth: 1,
            ..DfsServiceProfile::default()
        });

        svc.submit_write(
            DfsWriteReq {
                task: None,
                path: "/weights/queued.bin".into(),
                bytes: 1024,
            },
            0,
        )
        .expect("first write");

        let err = svc
            .submit_read(
                DfsReadReq {
                    task: None,
                    path: "/weights/queued.bin".into(),
                },
                1,
            )
            .expect_err("queue full");
        assert!(matches!(
            err,
            sim_core::SimError::InvalidInput("dfs queue full")
        ));
    }

    #[test]
    fn shmem_service_stub_enforces_owner_and_size_limit() {
        let mut svc = ShmemServiceStub::new(ShmemServiceProfile {
            max_segment_bytes: 4096,
            peer_count: 2,
            ..ShmemServiceProfile::default()
        });
        svc.register_segment(SegmentHandle(7), 0, 2048)
            .expect("register segment");

        svc.submit_get(
            ShmemGetReq {
                task: None,
                requester_entity: 3,
                segment: SegmentHandle(7),
                bytes: 1024,
            },
            0,
        )
        .expect("submit denied get");
        let denied = svc.poll_ready(3);
        assert_eq!(
            denied[0].status,
            CompletionStatus::FatalFailure {
                code: "shmem_access_denied".into()
            }
        );

        svc.submit_get(
            ShmemGetReq {
                task: None,
                requester_entity: 0,
                segment: SegmentHandle(7),
                bytes: 4096,
            },
            10,
        )
        .expect("submit oversized get");
        let too_large = svc.poll_ready(13);
        assert_eq!(
            too_large[0].status,
            CompletionStatus::RetryableFailure {
                code: "short_segment".into()
            }
        );
    }

    #[test]
    fn db_service_stub_round_trips_inline_and_external_values() {
        let mut svc = DbServiceStub::new(DbServiceProfile::default());

        svc.submit_put(
            DbPutReq {
                task: None,
                key: "meta:small".into(),
                bytes: 32,
            },
            10,
        )
        .expect("inline put");
        let inline_events = svc.poll_ready(18);
        assert_eq!(inline_events.len(), 1);
        assert_eq!(inline_events[0].status, CompletionStatus::Success);

        svc.submit_put(
            DbPutReq {
                task: None,
                key: "meta:large".into(),
                bytes: 512,
            },
            20,
        )
        .expect("external put");
        assert!(svc.poll_ready(43).is_empty());
        let external_events = svc.poll_ready(44);
        assert_eq!(external_events.len(), 1);
        assert_eq!(external_events[0].status, CompletionStatus::Success);

        svc.submit_get(
            DbGetReq {
                task: None,
                key: "meta:large".into(),
            },
            50,
        )
        .expect("db get");
        assert!(svc.poll_ready(73).is_empty());
        let get_events = svc.poll_ready(74);
        assert_eq!(get_events.len(), 1);
        assert_eq!(get_events[0].status, CompletionStatus::Success);
    }

    #[test]
    fn db_service_stub_surfaces_missing_key_and_batch_limit() {
        let mut svc = DbServiceStub::new(DbServiceProfile::default());

        svc.submit_get(
            DbGetReq {
                task: None,
                key: "missing".into(),
            },
            0,
        )
        .expect("missing get");
        let missing = svc.poll_ready(8);
        assert_eq!(
            missing[0].status,
            CompletionStatus::RetryableFailure {
                code: "db_missing_key".into()
            }
        );

        svc.submit_put(
            DbPutReq {
                task: None,
                key: "too-large".into(),
                bytes: 2048,
            },
            10,
        )
        .expect("oversized put");
        let oversized = svc.poll_ready(18);
        assert_eq!(
            oversized[0].status,
            CompletionStatus::RetryableFailure {
                code: "db_batch_limit_exceeded".into()
            }
        );
    }

    #[test]
    fn db_service_stub_rejects_when_queue_is_full() {
        let mut svc = DbServiceStub::new(DbServiceProfile {
            queue_depth: 1,
            ..DbServiceProfile::default()
        });

        svc.submit_put(
            DbPutReq {
                task: None,
                key: "qfull".into(),
                bytes: 32,
            },
            0,
        )
        .expect("first put");

        let err = svc
            .submit_get(
                DbGetReq {
                    task: None,
                    key: "qfull".into(),
                },
                1,
            )
            .expect_err("queue full");
        assert!(matches!(
            err,
            sim_core::SimError::InvalidInput("db queue full")
        ));
    }

    #[test]
    fn object_service_publishes_and_resolves_latest_inline_object() {
        let mut svc = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        svc.submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: "qwen3/session/s0/tokens/input".to_string(),
                kind: LingquObjectKind::TokenBuffer,
                producer_entity: 0,
                owner_entity: Some(0),
                expected_version: None,
                metadata: object_metadata(16, 0x1234),
                placements: vec![object_placement(LingquPayloadBackend::Inline, 16)],
                payload_bytes: 0x1234u64.to_le_bytes().to_vec(),
            },
            10,
        )
        .expect("publish object");
        let publish_events = svc.poll_ready(18);
        assert_eq!(publish_events.len(), 1);
        assert_eq!(publish_events[0].source, CompletionSource::DbService);
        assert_eq!(publish_events[0].status, CompletionStatus::Success);
        assert_eq!(
            svc.latest_record("qwen3/session/s0/tokens/input")
                .expect("latest record")
                .version,
            1
        );
        assert_eq!(
            svc.get_copy(
                "qwen3/session/s0/tokens/input",
                LingquObjectVersionSelector::LatestCommitted
            )
            .expect("payload copy"),
            0x1234u64.to_le_bytes().to_vec()
        );
        assert_eq!(
            svc.get_ref(
                "qwen3/session/s0/tokens/input",
                LingquObjectVersionSelector::LatestCommitted
            )
            .expect("payload ref"),
            0x1234u64.to_le_bytes().as_slice()
        );

        svc.submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: "qwen3/session/s0/tokens/input".to_string(),
                requester_entity: 1,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![LingquPayloadBackend::Inline],
            },
            20,
        )
        .expect("resolve object");
        let resolve_events = svc.poll_ready(28);
        assert_eq!(resolve_events.len(), 1);
        assert_eq!(resolve_events[0].status, CompletionStatus::Success);
        let report = svc.report();
        assert_eq!(report.publish_count, 1);
        assert_eq!(report.resolve_count, 1);
        assert_eq!(report.inline_write_count, 1);
        assert_eq!(report.inline_read_count, 1);
        assert_eq!(report.committed_object_count, 1);
        assert_ne!(report.checksum, 0);
    }

    #[test]
    fn object_service_snapshot_roundtrips_obmm_payloads() {
        let mut svc = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        let payload = vec![1, 2, 3, 4, 5, 6, 7, 8];
        let checksum = payload.len() as u64 ^ 0x55aa;
        svc.submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: "object/hot/table".to_string(),
                kind: LingquObjectKind::RuntimeTensor,
                producer_entity: 0,
                owner_entity: Some(1),
                expected_version: None,
                metadata: object_metadata(payload.len() as u64, checksum),
                placements: vec![object_placement(
                    LingquPayloadBackend::ObmmShmem,
                    payload.len() as u64,
                )],
                payload_bytes: payload.clone(),
            },
            10,
        )
        .unwrap();
        let encoded = svc.export_snapshot().to_json_bytes().unwrap();
        let decoded = LingquObjectServiceSnapshot::from_json_bytes(&encoded).unwrap();

        let restored = LingquObjectServiceStub::import_snapshot(decoded).unwrap();
        let restored_payload = restored
            .get_copy(
                "object/hot/table",
                LingquObjectVersionSelector::LatestCommitted,
            )
            .expect("restored payload");
        let restored_record = restored
            .latest_record("object/hot/table")
            .expect("restored record");

        assert_eq!(restored_payload, payload);
        assert_eq!(restored_record.version, 1);
        assert_eq!(
            restored_record.placements[0].backend,
            LingquPayloadBackend::ObmmShmem
        );
    }

    #[test]
    fn object_service_tracks_shmem_and_block_placements() {
        let mut svc = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        svc.submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: "qwen3/model/Qwen3-0.6B/layer/00/q_proj/shard/0".to_string(),
                kind: LingquObjectKind::WeightShard,
                producer_entity: 0,
                owner_entity: None,
                expected_version: None,
                metadata: object_metadata(4096, 0x4567),
                placements: vec![
                    object_placement(LingquPayloadBackend::Block, 4096),
                    object_placement(LingquPayloadBackend::Shmem, 1024),
                ],
                payload_bytes: 0x4567u64.to_le_bytes().to_vec(),
            },
            0,
        )
        .expect("publish weight");
        let events = svc.poll_ready(38);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].status, CompletionStatus::Success);
        let report = svc.report();
        assert_eq!(report.block_write_count, 1);
        assert_eq!(report.shmem_write_count, 1);
        assert!(report.obmm_pool_enabled);
        assert_eq!(report.obmm_pool_payload_write_count, 1);
        assert_eq!(report.obmm_pool_queue_submit_count, 1);
        let record = svc
            .latest_record("qwen3/model/Qwen3-0.6B/layer/00/q_proj/shard/0")
            .expect("latest weight");
        let shmem = record
            .placements
            .iter()
            .find(|placement| placement.backend == LingquPayloadBackend::Shmem)
            .expect("shmem placement");
        assert_eq!(shmem.segment, Some(SegmentHandle(0x0b00_0000)));
        assert!(shmem.offset >= LingquObmmPoolProfile::default().payload_base_offset);

        svc.submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: "qwen3/model/Qwen3-0.6B/layer/00/q_proj/shard/0".to_string(),
                requester_entity: 7,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![LingquPayloadBackend::Shmem, LingquPayloadBackend::Block],
            },
            40,
        )
        .expect("resolve hot weight");
        let resolve_events = svc.poll_ready(51);
        assert_eq!(resolve_events.len(), 1);
        assert_eq!(resolve_events[0].status, CompletionStatus::Success);
        assert_eq!(svc.report().shmem_read_count, 1);
        assert_eq!(svc.report().obmm_pool_payload_read_count, 1);
    }

    #[test]
    fn object_service_obmm_queue_enforces_spsc_depth() {
        let mut profile = LingquObjectServiceProfile::default();
        profile.queue_depth = 128;
        profile.obmm_pool.queue_depth = 64;
        profile.obmm_pool.queue_auto_drain = false;
        let mut svc = LingquObjectServiceStub::new(profile);

        for index in 0..64 {
            svc.submit_publish(
                LingquObjectPublishReq {
                    task: None,
                    key: format!("qwen3/session/s0/kv/layer/00/tile/0/position/{index:08}/k"),
                    kind: LingquObjectKind::KvCacheBlock,
                    producer_entity: 0,
                    owner_entity: Some(1),
                    expected_version: None,
                    metadata: object_metadata(128, 0x1111 + index),
                    placements: vec![object_placement(LingquPayloadBackend::Shmem, 128)],
                    payload_bytes: vec![index as u8; 128],
                },
                index,
            )
            .expect("publish should fit queue until depth is reached");
        }
        assert_eq!(svc.report().obmm_pool_queue_submit_count, 64);
        assert_eq!(svc.report().obmm_pool_queue_deliver_count, 0);

        svc.submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: "qwen3/session/s0/kv/layer/00/tile/0/position/00000064/k".to_string(),
                kind: LingquObjectKind::KvCacheBlock,
                producer_entity: 0,
                owner_entity: Some(1),
                expected_version: None,
                metadata: object_metadata(128, 0x2222),
                placements: vec![object_placement(LingquPayloadBackend::Shmem, 128)],
                payload_bytes: vec![0x22; 128],
            },
            64,
        )
        .expect("queue-full publish is reported through completion");

        let events = svc.poll_ready(100);
        assert_eq!(events.len(), 65);
        assert!(events[..64]
            .iter()
            .all(|event| event.status == CompletionStatus::Success));
        assert_eq!(
            events[64].status,
            CompletionStatus::RetryableFailure {
                code: "obmm_pool_queue_full".to_string(),
            }
        );
        assert_eq!(svc.report().obmm_pool_queue_submit_count, 64);
        assert_eq!(svc.report().obmm_pool_queue_deliver_count, 0);
        assert_eq!(svc.report().obmm_pool_payload_write_count, 64);

        svc.submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: "qwen3/session/s0/kv/layer/00/tile/0/position/00000000/k".to_string(),
                requester_entity: 1,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![LingquPayloadBackend::Shmem],
            },
            100,
        )
        .expect("consumer resolve should drain queued descriptors");
        let resolve_events = svc.poll_ready(111);
        assert_eq!(resolve_events.len(), 1);
        assert_eq!(resolve_events[0].status, CompletionStatus::Success);
        assert_eq!(svc.report().obmm_pool_queue_deliver_count, 64);

        svc.submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: "qwen3/session/s0/kv/layer/00/tile/0/position/00000064/k".to_string(),
                kind: LingquObjectKind::KvCacheBlock,
                producer_entity: 0,
                owner_entity: Some(1),
                expected_version: None,
                metadata: object_metadata(128, 0x3333),
                placements: vec![object_placement(LingquPayloadBackend::Shmem, 128)],
                payload_bytes: vec![0x33; 128],
            },
            112,
        )
        .expect("publish should fit after consumer drains queue");
        let retry_events = svc.poll_ready(123);
        assert_eq!(retry_events.len(), 1);
        assert_eq!(retry_events[0].status, CompletionStatus::Success);
        assert_eq!(svc.report().obmm_pool_queue_submit_count, 65);
    }

    #[test]
    fn object_service_detects_version_conflict_and_missing_key() {
        let mut svc = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        svc.submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: "qwen3/session/s0/kv/layer/00/tile/0/position/00000001/k".to_string(),
                kind: LingquObjectKind::KvCacheBlock,
                producer_entity: 0,
                owner_entity: Some(0),
                expected_version: Some(7),
                metadata: object_metadata(128, 0x9999),
                placements: vec![object_placement(LingquPayloadBackend::Shmem, 128)],
                payload_bytes: 0x9999u64.to_le_bytes().to_vec(),
            },
            0,
        )
        .expect("submit conflicting publish");
        let conflict = svc.poll_ready(8);
        assert_eq!(
            conflict[0].status,
            CompletionStatus::RetryableFailure {
                code: "object_version_conflict".to_string()
            }
        );

        svc.submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: "missing".to_string(),
                requester_entity: 0,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: Vec::new(),
            },
            10,
        )
        .expect("missing resolve");
        let missing = svc.poll_ready(18);
        assert_eq!(
            missing[0].status,
            CompletionStatus::RetryableFailure {
                code: "object_missing".to_string()
            }
        );
        assert_eq!(svc.report().missing_resolve_count, 1);
    }

    #[test]
    fn object_service_quarantine_blocks_normal_resolve() {
        let mut svc = LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        let key = "qwen3/session/s0/hidden/boundary/node/0/to/1/step/12";
        svc.submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: key.to_string(),
                kind: LingquObjectKind::RuntimeTensor,
                producer_entity: 0,
                owner_entity: Some(1),
                expected_version: None,
                metadata: object_metadata(2048, 0xabcd),
                placements: vec![object_placement(LingquPayloadBackend::Shmem, 2048)],
                payload_bytes: 0xabcdu64.to_le_bytes().to_vec(),
            },
            0,
        )
        .expect("publish hidden boundary");
        assert_eq!(svc.poll_ready(11)[0].status, CompletionStatus::Success);
        assert!(svc.quarantine_latest(key));
        svc.submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: key.to_string(),
                requester_entity: 1,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![LingquPayloadBackend::Shmem],
            },
            20,
        )
        .expect("resolve quarantined");
        let events = svc.poll_ready(28);
        assert_eq!(
            events[0].status,
            CompletionStatus::RetryableFailure {
                code: "object_quarantined".to_string()
            }
        );
        assert_eq!(svc.report().quarantined_object_count, 1);
    }

    #[test]
    fn weights_service_supports_host_load_and_multi_node_resolve_entries() {
        let mut svc = WeightsServiceStub::new();
        let load_stats = svc
            .submit_load(
                WeightsLoadReq {
                    task: None,
                    requester_entity: 0,
                    metadata_puts: vec![WeightMetadataPut {
                        key: "qwen3/layer0/shard0/q_proj".to_string(),
                        bytes: 256,
                    }],
                    payload_writes: vec![
                        WeightPayloadWrite {
                            storage_ref: "qwen3/block/layer0/shard0/q_proj".to_string(),
                            storage_kind: WeightStorageKind::Block,
                            segment: SegmentHandle(100),
                            offset: 0,
                            bytes: 4096,
                            checksum: 11,
                        },
                        WeightPayloadWrite {
                            storage_ref: "qwen3/shmem/layer0/shard0/q_proj_hot".to_string(),
                            storage_kind: WeightStorageKind::Shmem,
                            segment: SegmentHandle(101),
                            offset: 0,
                            bytes: 1024,
                            checksum: 12,
                        },
                    ],
                },
                0,
            )
            .expect("host load weights");
        assert_eq!(load_stats.metadata_puts, 1);
        assert_eq!(load_stats.block_writes, 1);
        assert_eq!(load_stats.shmem_writes, 1);
        assert_eq!(load_stats.payload_bytes, 5120);
        assert!(svc.payload_loaded("qwen3/block/layer0/shard0/q_proj"));
        assert!(svc.payload_loaded("qwen3/shmem/layer0/shard0/q_proj_hot"));

        for node in 0..8 {
            let stats = svc
                .submit_resolve(
                    WeightsResolveReq {
                        task: None,
                        requester_entity: node,
                        metadata_key: "qwen3/layer0/shard0/q_proj".to_string(),
                        storage_ref: "qwen3/block/layer0/shard0/q_proj".to_string(),
                        storage_kind: WeightStorageKind::Block,
                        segment: SegmentHandle(100),
                        bytes: 4096,
                    },
                    20 + node as u64,
                )
                .expect("node block resolve");
            assert_eq!(stats.metadata_gets, 1);
            assert_eq!(stats.block_reads, 1);
            assert_eq!(stats.shmem_reads, 0);
        }

        let shmem_stats = svc
            .submit_resolve(
                WeightsResolveReq {
                    task: None,
                    requester_entity: 7,
                    metadata_key: "qwen3/layer0/shard0/q_proj".to_string(),
                    storage_ref: "qwen3/shmem/layer0/shard0/q_proj_hot".to_string(),
                    storage_kind: WeightStorageKind::Shmem,
                    segment: SegmentHandle(101),
                    bytes: 1024,
                },
                40,
            )
            .expect("node shmem resolve");
        assert_eq!(shmem_stats.metadata_gets, 1);
        assert_eq!(shmem_stats.block_reads, 0);
        assert_eq!(shmem_stats.shmem_reads, 1);

        let ready = svc.poll_ready(1000);
        assert!(ready
            .iter()
            .any(|event| event.source == CompletionSource::DbService));
        assert!(ready
            .iter()
            .any(|event| event.source == CompletionSource::BlockService));
        assert!(ready
            .iter()
            .any(|event| event.source == CompletionSource::ShmemService));
    }

    #[test]
    fn weights_service_allows_each_node_entry_to_load_and_publish_weights() {
        let mut svc = WeightsServiceStub::new();
        for node in 0..8 {
            let block_ref = format!("qwen3/block/layer0/shard{node}/q_proj");
            let metadata_key = format!("qwen3/layer0/shard{node}/q_proj");
            let stats = svc
                .submit_load(
                    WeightsLoadReq {
                        task: None,
                        requester_entity: node,
                        metadata_puts: vec![WeightMetadataPut {
                            key: metadata_key.clone(),
                            bytes: 256 + node as u64,
                        }],
                        payload_writes: vec![WeightPayloadWrite {
                            storage_ref: block_ref.clone(),
                            storage_kind: WeightStorageKind::Block,
                            segment: SegmentHandle(200 + node as u64),
                            offset: 0,
                            bytes: 4096 + node as u64,
                            checksum: 1000 + node as u64,
                        }],
                    },
                    node as u64,
                )
                .expect("node load weights");
            assert_eq!(stats.metadata_puts, 1);
            assert_eq!(stats.block_writes, 1);
            assert_eq!(stats.shmem_writes, 0);
            assert!(svc.payload_loaded(&block_ref));

            let resolver = (node + 1) % 8;
            let resolve_stats = svc
                .submit_resolve(
                    WeightsResolveReq {
                        task: None,
                        requester_entity: resolver,
                        metadata_key,
                        storage_ref: block_ref,
                        storage_kind: WeightStorageKind::Block,
                        segment: SegmentHandle(200 + node as u64),
                        bytes: 4096 + node as u64,
                    },
                    100 + node as u64,
                )
                .expect("cross-node resolve");
            assert_eq!(resolve_stats.metadata_gets, 1);
            assert_eq!(resolve_stats.block_reads, 1);
            assert_eq!(resolve_stats.shmem_reads, 0);
        }

        let ready = svc.poll_ready(1000);
        assert!(
            ready
                .iter()
                .filter(|event| event.source == CompletionSource::DbService)
                .count()
                >= 16
        );
        assert!(
            ready
                .iter()
                .filter(|event| event.source == CompletionSource::BlockService)
                .count()
                >= 16
        );
    }

    #[test]
    fn weights_service_publishes_and_resolves_runtime_tiles_from_any_node() {
        let mut svc = WeightsServiceStub::new();
        for node in 0..8 {
            let producer = node;
            let consumer = (node + 3) % 8;
            let key = format!("qwen3/layer0/producer{producer}/activation_tile0");
            let storage_ref = format!("qwen3/runtime/producer{producer}/activation_tile0");
            let publish_stats = svc
                .submit_publish_object(
                    ServiceObjectPublishReq {
                        task: None,
                        requester_entity: producer,
                        metadata_puts: vec![ServiceObjectMetadataPut {
                            key: key.clone(),
                            object_kind: ServiceObjectKind::ActivationTile,
                            bytes: 192,
                        }],
                        payload_writes: vec![ServiceObjectPayloadWrite {
                            storage_ref: storage_ref.clone(),
                            object_kind: ServiceObjectKind::ActivationTile,
                            storage_kind: WeightStorageKind::Shmem,
                            segment: SegmentHandle(300 + producer as u64),
                            offset: 0,
                            bytes: 2048,
                            checksum: 2000 + producer as u64,
                            producer_entity: producer,
                        }],
                    },
                    10 + producer as u64,
                )
                .expect("node publish activation tile");
            assert_eq!(publish_stats.metadata_puts, 1);
            assert_eq!(publish_stats.shmem_writes, 1);
            assert!(svc.payload_loaded(&storage_ref));

            let resolve_stats = svc
                .submit_resolve_object(
                    ServiceObjectResolveReq {
                        task: None,
                        requester_entity: consumer,
                        metadata_key: key,
                        object_kind: ServiceObjectKind::ActivationTile,
                        storage_ref,
                        storage_kind: WeightStorageKind::Shmem,
                        segment: SegmentHandle(300 + producer as u64),
                        bytes: 2048,
                    },
                    50 + consumer as u64,
                )
                .expect("consumer resolve activation tile");
            assert_eq!(resolve_stats.metadata_gets, 1);
            assert_eq!(resolve_stats.block_reads, 0);
            assert_eq!(resolve_stats.shmem_reads, 1);
        }

        let ready = svc.poll_ready(1000);
        assert!(
            ready
                .iter()
                .filter(|event| event.source == CompletionSource::DbService)
                .count()
                >= 16
        );
        assert!(
            ready
                .iter()
                .filter(|event| event.source == CompletionSource::ShmemService)
                .count()
                >= 16
        );
    }

    #[test]
    fn object_service_stability_multi_node_handoff_progressive_growth() {
        let mut profile = LingquObjectServiceProfile::default();
        profile.queue_depth = 4096;
        profile.obmm_pool.queue_depth = 512;
        profile.obmm_pool.queue_auto_drain = true;
        profile.obmm_pool.pool_bytes = 700 * 1024u64 * 1024u64;
        let mut svc = LingquObjectServiceStub::new(profile);

        let node_count: u64 = 8;
        let steps: u64 = 128;
        let mut expected_published = 0u64;

        for step in 0..steps {
            let now = step.saturating_mul(10_000);
            let mut expected: Vec<(String, Vec<u8>, LingquObjectKind)> = Vec::new();

            for node in 0..node_count {
                let next_node = (node + 1) % node_count;
                let hidden_len = 1_024u64 + step.saturating_mul(37) + node.saturating_mul(11);
                let kv_len = 768u64 + step.saturating_mul(13) + node.saturating_mul(7);
                let hidden_payload = growing_payload(
                    node.saturating_mul(0x31)
                        .saturating_add(step.saturating_mul(0x17)),
                    hidden_len as usize,
                );
                let kv_payload = growing_payload(
                    0x9b_u64
                        .saturating_add(node.saturating_mul(0x7f))
                        .saturating_add(step.saturating_mul(0x41)),
                    kv_len as usize,
                );

                let hidden_key =
                    format!("qwen3/stress/step{step}/handoff/node{node}->node{next_node}/hidden");
                let kv_key =
                    format!("qwen3/stress/step{step}/handoff/node{node}->node{next_node}/kvcache");

                svc.submit_publish(
                    LingquObjectPublishReq {
                        task: None,
                        key: hidden_key.clone(),
                        kind: LingquObjectKind::RuntimeTensor,
                        producer_entity: node,
                        owner_entity: Some(next_node),
                        expected_version: None,
                        metadata: object_metadata(
                            hidden_len,
                            payload_checksum_fnv1a(&hidden_payload),
                        ),
                        placements: vec![LingquPayloadPlacement {
                            backend: LingquPayloadBackend::Shmem,
                            storage_ref: format!(
                                "qwen3/stress/payload/hidden/node{node}/step{step}"
                            ),
                            segment: Some(SegmentHandle(32)),
                            offset: 0,
                            bytes: hidden_len,
                            checksum: payload_checksum_fnv1a(&hidden_payload),
                            locality: LingquObjectLocality::DomainShared(0),
                        }],
                        payload_bytes: hidden_payload.clone(),
                    },
                    now,
                )
                .expect("publish handoff hidden object");

                svc.submit_publish(
                    LingquObjectPublishReq {
                        task: None,
                        key: kv_key.clone(),
                        kind: LingquObjectKind::KvCacheBlock,
                        producer_entity: node,
                        owner_entity: Some(next_node),
                        expected_version: None,
                        metadata: object_metadata(kv_len, payload_checksum_fnv1a(&kv_payload)),
                        placements: vec![LingquPayloadPlacement {
                            backend: LingquPayloadBackend::Shmem,
                            storage_ref: format!("qwen3/stress/payload/kv/node{node}/step{step}"),
                            segment: Some(SegmentHandle(40)),
                            offset: 0,
                            bytes: kv_len,
                            checksum: payload_checksum_fnv1a(&kv_payload),
                            locality: LingquObjectLocality::DomainShared(0),
                        }],
                        payload_bytes: kv_payload.clone(),
                    },
                    now,
                )
                .expect("publish handoff kvcache object");

                expected.push((hidden_key, hidden_payload, LingquObjectKind::RuntimeTensor));
                expected.push((kv_key, kv_payload, LingquObjectKind::KvCacheBlock));
                expected_published += 2;
            }

            let publish_events = svc.poll_ready(now + 200);
            assert_eq!(
                publish_events.len(),
                (node_count.saturating_mul(2)) as usize
            );
            assert!(publish_events
                .iter()
                .all(|event| event.status == CompletionStatus::Success));

            for node in 0..node_count {
                let next_node = (node + 1) % node_count;
                let hidden_key =
                    format!("qwen3/stress/step{step}/handoff/node{node}->node{next_node}/hidden");
                let kv_key =
                    format!("qwen3/stress/step{step}/handoff/node{node}->node{next_node}/kvcache");

                svc.submit_resolve(
                    LingquObjectResolveReq {
                        task: None,
                        key: hidden_key,
                        requester_entity: next_node,
                        version: LingquObjectVersionSelector::LatestCommitted,
                        min_state: LingquObjectState::Committed,
                        preferred_backends: vec![LingquPayloadBackend::Shmem],
                    },
                    now + 300,
                )
                .expect("resolve hidden handoff");

                svc.submit_resolve(
                    LingquObjectResolveReq {
                        task: None,
                        key: kv_key,
                        requester_entity: next_node,
                        version: LingquObjectVersionSelector::LatestCommitted,
                        min_state: LingquObjectState::Committed,
                        preferred_backends: vec![LingquPayloadBackend::Shmem],
                    },
                    now + 300,
                )
                .expect("resolve kv handoff");
            }

            let resolve_events = svc.poll_ready(now + 450);
            assert_eq!(
                resolve_events.len(),
                (node_count.saturating_mul(2)) as usize
            );
            assert!(resolve_events
                .iter()
                .all(|event| event.status == CompletionStatus::Success));

            for (key, expected_payload, expected_kind) in expected {
                let record = svc
                    .latest_record(&key)
                    .expect("latest object record should exist");
                assert_eq!(record.kind, expected_kind);
                assert_eq!(record.checksum, payload_checksum_fnv1a(&expected_payload));
                assert_eq!(record.placements.len(), 1);
                let payload_copy = svc
                    .get_copy(&key, LingquObjectVersionSelector::LatestCommitted)
                    .expect("get_copy should find payload");
                let payload_ref = svc
                    .get_ref(&key, LingquObjectVersionSelector::LatestCommitted)
                    .expect("get_ref should find payload");
                assert_eq!(payload_copy, expected_payload);
                assert_eq!(payload_ref, expected_payload.as_slice());
            }
        }

        let report = svc.report();
        assert_eq!(report.publish_count, expected_published);
        assert_eq!(report.resolve_count, expected_published);
        assert_eq!(report.obmm_pool_payload_write_count, expected_published);
        assert_eq!(report.obmm_pool_payload_read_count, expected_published);
        assert_eq!(report.obmm_pool_queue_submit_count, expected_published);
        assert!(report.obmm_pool_queue_deliver_count > 0);
        assert_eq!(
            report.obmm_pool_queue_submit_count,
            report.obmm_pool_queue_deliver_count
        );
        assert_eq!(report.missing_resolve_count, 0);
    }

    #[test]
    fn object_service_stability_reused_key_growth_with_versioning() {
        let mut profile = LingquObjectServiceProfile::default();
        profile.queue_depth = 4096;
        profile.obmm_pool.queue_depth = 512;
        profile.obmm_pool.queue_auto_drain = true;
        let mut svc = LingquObjectServiceStub::new(profile);

        let steps: u64 = 256;
        let key = "qwen3/stress/reused/node-range/hidden";
        let mut expected_version = 0u64;
        let mut expected_publish_count = 0u64;

        for step in 0..steps {
            let now = step.saturating_mul(20_000);
            let payload_len = 1_024u64 + step.saturating_mul(16);
            let payload = growing_payload(0x55aa_u64 ^ step, payload_len as usize);
            let checksum = payload_checksum_fnv1a(&payload);
            svc.submit_publish(
                LingquObjectPublishReq {
                    task: None,
                    key: key.to_string(),
                    kind: LingquObjectKind::RuntimeTensor,
                    producer_entity: 0,
                    owner_entity: None,
                    expected_version: Some(expected_version),
                    metadata: object_metadata(payload_len, checksum),
                    placements: vec![LingquPayloadPlacement {
                        backend: LingquPayloadBackend::Shmem,
                        storage_ref: "qwen3/stress/reused/payload/hidden".to_string(),
                        segment: Some(SegmentHandle(50)),
                        offset: 0,
                        bytes: payload_len,
                        checksum,
                        locality: LingquObjectLocality::DomainShared(0),
                    }],
                    payload_bytes: payload.clone(),
                },
                now,
            )
            .expect("publish reused hidden object");
            expected_publish_count += 1;

            let publish_events = svc.poll_ready(now + 200);
            assert_eq!(publish_events.len(), 1);
            assert_eq!(publish_events[0].status, CompletionStatus::Success);

            let resolver_entity = step % 8;
            svc.submit_resolve(
                LingquObjectResolveReq {
                    task: None,
                    key: key.to_string(),
                    requester_entity: resolver_entity,
                    version: LingquObjectVersionSelector::LatestCommitted,
                    min_state: LingquObjectState::Committed,
                    preferred_backends: vec![LingquPayloadBackend::Shmem],
                },
                now + 250,
            )
            .expect("resolve reused hidden object");
            let resolve_events = svc.poll_ready(now + 500);
            assert_eq!(resolve_events.len(), 1);
            assert_eq!(resolve_events[0].status, CompletionStatus::Success);

            let record = svc.latest_record(key).expect("latest record after publish");
            assert_eq!(record.version, expected_version.saturating_add(1));
            assert_eq!(record.checksum, checksum);
            let resolved_copy = svc
                .get_copy(key, LingquObjectVersionSelector::Exact(record.version))
                .expect("resolved copy");
            let resolved_ref = svc
                .get_ref(key, LingquObjectVersionSelector::Exact(record.version))
                .expect("resolved ref");
            assert_eq!(resolved_copy, payload);
            assert_eq!(resolved_ref, payload.as_slice());

            expected_version = expected_version.saturating_add(1);
        }

        let report = svc.report();
        assert_eq!(report.publish_count, expected_publish_count);
        assert_eq!(report.resolve_count, expected_publish_count);
        assert_eq!(report.obmm_pool_payload_write_count, expected_publish_count);
        assert_eq!(report.obmm_pool_payload_read_count, expected_publish_count);
        assert_eq!(report.obmm_pool_queue_submit_count, expected_publish_count);
        assert!(report.obmm_pool_queue_deliver_count >= expected_publish_count);
        assert_eq!(report.missing_resolve_count, 0);
    }

    #[test]
    fn object_service_stability_randomized_32mb_range() {
        let mut profile = LingquObjectServiceProfile::default();
        profile.queue_depth = 4096;
        profile.obmm_pool.queue_depth = 1024;
        profile.obmm_pool.queue_auto_drain = true;

        let range_size_bytes = 96u64 * 1024u64 * 1024u64;
        let pool_total_bytes = 768u64 * 1024u64 * 1024u64;
        let min_payload_base = 2u64 * 1024u64 * 1024u64;

        let mut rng_state = 0x4a8d_f6e3_u64;
        let next_rand = |state: &mut u64| {
            *state ^= *state << 7;
            *state ^= *state >> 9;
            *state ^= *state << 8;
            *state
        };

        let span = pool_total_bytes
            .saturating_sub(range_size_bytes)
            .saturating_sub(min_payload_base);
        let payload_base_offset = if span > 0 {
            min_payload_base.saturating_add(next_rand(&mut rng_state) % span)
        } else {
            min_payload_base
        };

        profile.obmm_pool.pool_bytes =
            payload_base_offset.saturating_add(range_size_bytes.saturating_mul(2));
        profile.obmm_pool.payload_base_offset = payload_base_offset;
        let mut svc = LingquObjectServiceStub::new(profile);

        let node_count = 8u64;
        let mut published = 0u64;
        let mut remaining = range_size_bytes;
        let mut published_bytes = 0u64;

        for step in 0..64u64 {
            let now = 25_000u64 + step.saturating_mul(10_000);
            let node = step % node_count;
            let next_node = (node + 1) % node_count;

            let kv_len = {
                let draw = next_rand(&mut rng_state) % 1000;
                64u64 * 1024u64 + (draw * (4u64 * 1024u64 * 1024u64 - 64u64 * 1024u64)) / 999
            };
            let hidden_len = {
                let draw = next_rand(&mut rng_state) % 1000;
                64u64 * 1024u64 + (draw * (2u64 * 1024u64 * 1024u64 - 64u64 * 1024u64)) / 999
            };
            let pair_len = kv_len.saturating_add(hidden_len);

            if pair_len.saturating_add(1024) > remaining {
                break;
            }
            remaining -= pair_len;
            published_bytes = published_bytes.saturating_add(pair_len);

            let kv_payload = growing_payload(0x9e37_u64.saturating_add(step), kv_len as usize);
            let hidden_payload = growing_payload(
                0x1a5u64.saturating_add(step.saturating_mul(17)),
                hidden_len as usize,
            );
            let kv_checksum = payload_checksum_fnv1a(&kv_payload);
            let hidden_checksum = payload_checksum_fnv1a(&hidden_payload);

            let kv_key =
                format!("qwen3/stress/random-range/step{step}/node{node}->node{next_node}/kvcache");
            let hidden_key =
                format!("qwen3/stress/random-range/step{step}/node{node}->node{next_node}/hidden");

            svc.submit_publish(
                LingquObjectPublishReq {
                    task: None,
                    key: kv_key.clone(),
                    kind: LingquObjectKind::KvCacheBlock,
                    producer_entity: node,
                    owner_entity: Some(next_node),
                    expected_version: None,
                    metadata: object_metadata(kv_len, kv_checksum),
                    placements: vec![LingquPayloadPlacement {
                        backend: LingquPayloadBackend::Shmem,
                        storage_ref: format!("qwen3/stress/random-range/payload/kv/{step}"),
                        segment: Some(SegmentHandle(77)),
                        offset: 0,
                        bytes: kv_len,
                        checksum: kv_checksum,
                        locality: LingquObjectLocality::DomainShared(0),
                    }],
                    payload_bytes: kv_payload,
                },
                now,
            )
            .expect("publish random range kv object");

            svc.submit_publish(
                LingquObjectPublishReq {
                    task: None,
                    key: hidden_key.clone(),
                    kind: LingquObjectKind::RuntimeTensor,
                    producer_entity: node,
                    owner_entity: Some(next_node),
                    expected_version: None,
                    metadata: object_metadata(hidden_len, hidden_checksum),
                    placements: vec![LingquPayloadPlacement {
                        backend: LingquPayloadBackend::Shmem,
                        storage_ref: format!("qwen3/stress/random-range/payload/hidden/{step}"),
                        segment: Some(SegmentHandle(80)),
                        offset: 0,
                        bytes: hidden_len,
                        checksum: hidden_checksum,
                        locality: LingquObjectLocality::DomainShared(0),
                    }],
                    payload_bytes: hidden_payload,
                },
                now + 100,
            )
            .expect("publish random range hidden object");

            published += 2;

            let publish_events = svc.poll_ready(now + 240);
            assert_eq!(publish_events.len(), 2);
            assert!(publish_events
                .iter()
                .all(|event| event.status == CompletionStatus::Success));

            svc.submit_resolve(
                LingquObjectResolveReq {
                    task: None,
                    key: kv_key.clone(),
                    requester_entity: next_node,
                    version: LingquObjectVersionSelector::LatestCommitted,
                    min_state: LingquObjectState::Committed,
                    preferred_backends: vec![LingquPayloadBackend::Shmem],
                },
                now + 300,
            )
            .expect("resolve random range kv object");

            svc.submit_resolve(
                LingquObjectResolveReq {
                    task: None,
                    key: hidden_key.clone(),
                    requester_entity: next_node,
                    version: LingquObjectVersionSelector::LatestCommitted,
                    min_state: LingquObjectState::Committed,
                    preferred_backends: vec![LingquPayloadBackend::Shmem],
                },
                now + 300,
            )
            .expect("resolve random range hidden object");

            let resolve_events = svc.poll_ready(now + 460);
            assert_eq!(resolve_events.len(), 2);
            assert!(resolve_events
                .iter()
                .all(|event| event.status == CompletionStatus::Success));

            let kv_record = svc
                .latest_record(&kv_key)
                .expect("kv latest record should exist");
            assert_eq!(kv_record.kind, LingquObjectKind::KvCacheBlock);
            assert_eq!(kv_record.checksum, kv_checksum);
            let kv_copy = svc
                .get_copy(&kv_key, LingquObjectVersionSelector::LatestCommitted)
                .expect("kv payload copy");
            assert_eq!(payload_checksum_fnv1a(&kv_copy), kv_checksum);
            let kv_ref = svc
                .get_ref(&kv_key, LingquObjectVersionSelector::LatestCommitted)
                .expect("kv payload ref");
            assert_eq!(payload_checksum_fnv1a(kv_ref), kv_checksum);

            let hidden_record = svc
                .latest_record(&hidden_key)
                .expect("hidden latest record should exist");
            assert_eq!(hidden_record.kind, LingquObjectKind::RuntimeTensor);
            assert_eq!(hidden_record.checksum, hidden_checksum);
            let hidden_copy = svc
                .get_copy(&hidden_key, LingquObjectVersionSelector::LatestCommitted)
                .expect("hidden payload copy");
            assert_eq!(payload_checksum_fnv1a(&hidden_copy), hidden_checksum);
            let hidden_ref = svc
                .get_ref(&hidden_key, LingquObjectVersionSelector::LatestCommitted)
                .expect("hidden payload ref");
            assert_eq!(payload_checksum_fnv1a(hidden_ref), hidden_checksum);
        }

        let report = svc.report();
        assert!(published > 0);
        assert!(published_bytes >= range_size_bytes / 2);
        assert!(published_bytes <= range_size_bytes);
        assert_eq!(report.publish_count, published);
        assert_eq!(report.resolve_count, published);
        assert_eq!(report.obmm_pool_payload_write_count, published);
        assert_eq!(report.obmm_pool_payload_read_count, published);
        assert_eq!(report.obmm_pool_queue_submit_count, published);
        assert!(report.obmm_pool_queue_deliver_count >= published);
        assert_eq!(report.missing_resolve_count, 0);
        assert!(report.obmm_pool_bytes_used >= payload_base_offset);
        assert!(report.obmm_pool_reserved_bytes >= published_bytes);
        assert!(report.obmm_pool_bytes_used <= profile.obmm_pool.pool_bytes);
    }

    #[test]
    fn object_service_obmm_pool_pressure_uses_tiered_block_spans() {
        let mut profile = LingquObjectServiceProfile::default();
        profile.queue_depth = 4096;
        profile.obmm_pool.queue_depth = 4096;
        profile.obmm_pool.queue_auto_drain = true;
        profile.obmm_pool.pool_bytes = 96 * 1024 * 1024;
        profile.obmm_pool.payload_base_offset = 2 * 1024 * 1024;
        profile.obmm_pool.payload_alignment = 64;
        profile.obmm_pool.payload_block_tiers =
            [256 * 1024, 512 * 1024, 1024 * 1024, 2 * 1024 * 1024];
        let mut svc = LingquObjectServiceStub::new(profile);

        let payload_sizes = [
            128 * 1024,
            256 * 1024,
            256 * 1024 + 1,
            512 * 1024,
            512 * 1024 + 1,
            1024 * 1024,
            1024 * 1024 + 1,
            2 * 1024 * 1024,
            2 * 1024 * 1024 + 1,
            5 * 1024 * 1024 + 123,
        ];
        let expected_reserved = [
            256 * 1024,
            256 * 1024,
            512 * 1024,
            512 * 1024,
            1024 * 1024,
            1024 * 1024,
            2 * 1024 * 1024,
            2 * 1024 * 1024,
            4 * 1024 * 1024,
            6 * 1024 * 1024,
        ];
        let expected_blocks = [1, 1, 1, 1, 1, 1, 1, 1, 2, 3];
        let expected_block_bytes = [
            256 * 1024,
            256 * 1024,
            512 * 1024,
            512 * 1024,
            1024 * 1024,
            1024 * 1024,
            2 * 1024 * 1024,
            2 * 1024 * 1024,
            2 * 1024 * 1024,
            2 * 1024 * 1024,
        ];

        let mut expected_checksums = Vec::new();
        let mut expected_offsets = Vec::new();
        let mut expected_offset = profile.obmm_pool.payload_base_offset;

        for (index, bytes) in payload_sizes.iter().copied().enumerate() {
            let payload = growing_payload(0x5150_u64 + index as u64, bytes as usize);
            let checksum = payload_checksum_fnv1a(&payload);
            let key = format!("obmm-pool/stress/tiered-span/{index}");
            let storage_ref = format!("obmm-pool/stress/tiered-span/payload/{index}");

            expected_offset = ((expected_offset + expected_block_bytes[index] - 1)
                / expected_block_bytes[index])
                * expected_block_bytes[index];
            expected_offsets.push(expected_offset);
            expected_offset += expected_reserved[index];

            svc.submit_publish(
                LingquObjectPublishReq {
                    task: None,
                    key: key.clone(),
                    kind: LingquObjectKind::KvCacheBlock,
                    producer_entity: index as u64 % 4,
                    owner_entity: Some((index as u64 + 1) % 4),
                    expected_version: None,
                    metadata: object_metadata(bytes, checksum),
                    placements: vec![LingquPayloadPlacement {
                        backend: LingquPayloadBackend::Shmem,
                        storage_ref,
                        segment: Some(SegmentHandle(0x5150)),
                        offset: 0,
                        bytes,
                        checksum,
                        locality: LingquObjectLocality::DomainShared(0),
                    }],
                    payload_bytes: payload,
                },
                10 + index as u64,
            )
            .expect("publish tiered span object");
            expected_checksums.push((key, bytes, checksum));
        }

        let publish_events = svc.poll_ready(1000);
        assert_eq!(publish_events.len(), payload_sizes.len());
        assert!(publish_events
            .iter()
            .all(|event| event.status == CompletionStatus::Success));

        for (index, (key, bytes, checksum)) in expected_checksums.iter().enumerate() {
            let record = svc.latest_record(key).expect("tiered span record");
            assert_eq!(record.kind, LingquObjectKind::KvCacheBlock);
            assert_eq!(record.bytes, *bytes);
            assert_eq!(record.checksum, *checksum);
            let placement = record.placements.first().expect("tiered span placement");
            assert_eq!(placement.offset, expected_offsets[index]);
            assert_eq!(placement.bytes, *bytes);

            svc.submit_resolve(
                LingquObjectResolveReq {
                    task: None,
                    key: key.clone(),
                    requester_entity: ((index as u64) + 1) % 4,
                    version: LingquObjectVersionSelector::LatestCommitted,
                    min_state: LingquObjectState::Committed,
                    preferred_backends: vec![LingquPayloadBackend::Shmem],
                },
                2_000 + index as u64,
            )
            .expect("resolve tiered span object");
        }

        let resolve_events = svc.poll_ready(3000);
        assert_eq!(resolve_events.len(), payload_sizes.len());
        assert!(resolve_events
            .iter()
            .all(|event| event.status == CompletionStatus::Success));

        for (key, _, checksum) in expected_checksums {
            let copy = svc
                .get_copy(&key, LingquObjectVersionSelector::LatestCommitted)
                .expect("tiered span copy");
            let view = svc
                .get_ref(&key, LingquObjectVersionSelector::LatestCommitted)
                .expect("tiered span ref");
            assert_eq!(payload_checksum_fnv1a(&copy), checksum);
            assert_eq!(payload_checksum_fnv1a(view), checksum);
        }

        let report = svc.report();
        assert_eq!(report.publish_count, payload_sizes.len() as u64);
        assert_eq!(report.resolve_count, payload_sizes.len() as u64);
        assert_eq!(
            report.obmm_pool_payload_write_count,
            payload_sizes.len() as u64
        );
        assert_eq!(
            report.obmm_pool_payload_read_count,
            payload_sizes.len() as u64
        );
        assert_eq!(
            report.obmm_pool_reserved_bytes,
            expected_reserved.iter().sum::<u64>()
        );
        assert_eq!(
            report.obmm_pool_block_count,
            expected_blocks.iter().sum::<u64>()
        );
        assert_eq!(report.obmm_pool_multi_block_write_count, 2);
        assert_eq!(report.obmm_pool_max_blocks_per_payload, 3);
        assert_eq!(report.obmm_pool_bytes_used, expected_offset);
        assert_eq!(report.missing_resolve_count, 0);
    }

    #[test]
    fn object_service_stability_longrun_decode_like_handoff_8node() {
        let mut profile = LingquObjectServiceProfile::default();
        profile.queue_depth = 8192;
        profile.obmm_pool.queue_depth = 2048;
        profile.obmm_pool.queue_auto_drain = true;
        profile.obmm_pool.pool_bytes = 1400 * 1024u64 * 1024u64;

        let mut svc = LingquObjectServiceStub::new(profile);
        let node_count = 8u64;
        let slot_count = 8u64;
        let steps = 256u64;

        let mut rng_state = 0xcafe_f00d_u64;
        let next_rand = |state: &mut u64| {
            *state ^= *state << 5;
            *state ^= *state >> 11;
            *state ^= *state << 8;
            *state
        };

        let mut expected_kv_versions = vec![vec![0u64; slot_count as usize]; node_count as usize];
        let mut expected_hidden_versions =
            vec![vec![0u64; slot_count as usize]; node_count as usize];
        let mut expected_publish = 0u64;
        let mut expected_resolve = 0u64;
        let mut kv_bytes_total = 0u64;
        let mut hidden_bytes_total = 0u64;
        let mut max_kv_len = 0u64;
        let mut max_hidden_len = 0u64;
        let mut min_kv_len = u64::MAX;
        let mut min_hidden_len = u64::MAX;

        for step in 0..steps {
            let now = 40_000u64 + step.saturating_mul(7_500);
            let mut step_kv_checksums = Vec::with_capacity(node_count as usize);
            let mut step_hidden_checksums = Vec::with_capacity(node_count as usize);

            for node in 0..node_count {
                let next_node = (node + 1) % node_count;
                let slot = (step % slot_count) as usize;
                let node_idx = node as usize;
                let growth_factor = step.min(128);

                let kv_len = {
                    let jitter = next_rand(&mut rng_state) % 768;
                    (20_000u64 + growth_factor.saturating_mul(180) + jitter).min(96_000)
                };
                let hidden_len = {
                    let jitter = next_rand(&mut rng_state) % 1024;
                    (12_000u64 + growth_factor.saturating_mul(96) + jitter).min(68_000)
                };

                let kv_payload = growing_payload(
                    0x11aa_u64
                        .saturating_add(node.saturating_mul(11))
                        .saturating_add(step.saturating_mul(31)),
                    kv_len as usize,
                );
                let hidden_payload = growing_payload(
                    0x22bb_u64
                        .saturating_add(node.saturating_mul(17))
                        .saturating_add(step.saturating_mul(29)),
                    hidden_len as usize,
                );
                let kv_checksum = payload_checksum_fnv1a(&kv_payload);
                let hidden_checksum = payload_checksum_fnv1a(&hidden_payload);

                let kv_key =
                    format!("qwen3/stress/longrun/node{node}->node{next_node}/slot{slot}/kvcache");
                let hidden_key =
                    format!("qwen3/stress/longrun/node{node}->node{next_node}/slot{slot}/hidden");

                let kv_expected_version = expected_kv_versions[node_idx][slot];
                let hidden_expected_version = expected_hidden_versions[node_idx][slot];
                expected_kv_versions[node_idx][slot] =
                    expected_kv_versions[node_idx][slot].saturating_add(1);
                expected_hidden_versions[node_idx][slot] =
                    expected_hidden_versions[node_idx][slot].saturating_add(1);

                svc.submit_publish(
                    LingquObjectPublishReq {
                        task: None,
                        key: kv_key.clone(),
                        kind: LingquObjectKind::KvCacheBlock,
                        producer_entity: node,
                        owner_entity: Some(next_node),
                        expected_version: Some(kv_expected_version),
                        metadata: object_metadata(kv_len, kv_checksum),
                        placements: vec![LingquPayloadPlacement {
                            backend: LingquPayloadBackend::Shmem,
                            storage_ref: format!(
                                "qwen3/stress/longrun/payload/kv/node{node}/slot{slot}"
                            ),
                            segment: Some(SegmentHandle(90)),
                            offset: 0,
                            bytes: kv_len,
                            checksum: kv_checksum,
                            locality: LingquObjectLocality::DomainShared(0),
                        }],
                        payload_bytes: kv_payload,
                    },
                    now,
                )
                .expect("publish longrun kv object");

                svc.submit_publish(
                    LingquObjectPublishReq {
                        task: None,
                        key: hidden_key.clone(),
                        kind: LingquObjectKind::RuntimeTensor,
                        producer_entity: node,
                        owner_entity: Some(next_node),
                        expected_version: Some(hidden_expected_version),
                        metadata: object_metadata(hidden_len, hidden_checksum),
                        placements: vec![LingquPayloadPlacement {
                            backend: LingquPayloadBackend::Shmem,
                            storage_ref: format!(
                                "qwen3/stress/longrun/payload/hidden/node{node}/slot{slot}"
                            ),
                            segment: Some(SegmentHandle(91)),
                            offset: 0,
                            bytes: hidden_len,
                            checksum: hidden_checksum,
                            locality: LingquObjectLocality::DomainShared(0),
                        }],
                        payload_bytes: hidden_payload,
                    },
                    now + 50,
                )
                .expect("publish longrun hidden object");

                expected_publish = expected_publish.saturating_add(2);
                kv_bytes_total = kv_bytes_total.saturating_add(kv_len);
                hidden_bytes_total = hidden_bytes_total.saturating_add(hidden_len);
                max_kv_len = max_kv_len.max(kv_len);
                max_hidden_len = max_hidden_len.max(hidden_len);
                min_kv_len = min_kv_len.min(kv_len);
                min_hidden_len = min_hidden_len.min(hidden_len);

                step_kv_checksums.push((
                    kv_key,
                    kv_expected_version.saturating_add(1),
                    kv_checksum,
                ));
                step_hidden_checksums.push((
                    hidden_key,
                    hidden_expected_version.saturating_add(1),
                    hidden_checksum,
                ));
            }

            let publish_events = svc.poll_ready(now + 260);
            assert_eq!(
                publish_events.len(),
                (node_count.saturating_mul(2)) as usize
            );
            assert!(publish_events
                .iter()
                .all(|event| event.status == CompletionStatus::Success));

            for node in 0..node_count {
                let next_node = (node + 1) % node_count;
                let slot = (step % slot_count) as usize;
                let kv_key =
                    format!("qwen3/stress/longrun/node{node}->node{next_node}/slot{slot}/kvcache");
                let hidden_key =
                    format!("qwen3/stress/longrun/node{node}->node{next_node}/slot{slot}/hidden");

                svc.submit_resolve(
                    LingquObjectResolveReq {
                        task: None,
                        key: kv_key,
                        requester_entity: next_node,
                        version: LingquObjectVersionSelector::LatestCommitted,
                        min_state: LingquObjectState::Committed,
                        preferred_backends: vec![LingquPayloadBackend::Shmem],
                    },
                    now + 300,
                )
                .expect("resolve longrun kv object");
                svc.submit_resolve(
                    LingquObjectResolveReq {
                        task: None,
                        key: hidden_key,
                        requester_entity: next_node,
                        version: LingquObjectVersionSelector::LatestCommitted,
                        min_state: LingquObjectState::Committed,
                        preferred_backends: vec![LingquPayloadBackend::Shmem],
                    },
                    now + 300,
                )
                .expect("resolve longrun hidden object");
            }

            let resolve_events = svc.poll_ready(now + 520);
            assert_eq!(
                resolve_events.len(),
                (node_count.saturating_mul(2)) as usize
            );
            assert!(resolve_events
                .iter()
                .all(|event| event.status == CompletionStatus::Success));

            expected_resolve += node_count.saturating_mul(2);

            for (kv_key, expected_kv_version, expected_checksum) in step_kv_checksums {
                let kv_record = svc
                    .latest_record(&kv_key)
                    .expect("kv latest record should exist");
                assert_eq!(kv_record.version, expected_kv_version);
                assert_eq!(kv_record.kind, LingquObjectKind::KvCacheBlock);
                assert_eq!(kv_record.checksum, expected_checksum);
                let kv_copy = svc
                    .get_copy(&kv_key, LingquObjectVersionSelector::LatestCommitted)
                    .expect("kv copy");
                assert_eq!(payload_checksum_fnv1a(&kv_copy), expected_checksum);
            }

            for (hidden_key, expected_hidden_version, expected_checksum) in step_hidden_checksums {
                let hidden_record = svc
                    .latest_record(&hidden_key)
                    .expect("hidden latest record should exist");
                assert_eq!(hidden_record.version, expected_hidden_version);
                assert_eq!(hidden_record.kind, LingquObjectKind::RuntimeTensor);
                assert_eq!(hidden_record.checksum, expected_checksum);
                let hidden_copy = svc
                    .get_copy(&hidden_key, LingquObjectVersionSelector::LatestCommitted)
                    .expect("hidden copy");
                assert_eq!(payload_checksum_fnv1a(&hidden_copy), expected_checksum);
            }
        }

        let report = svc.report();
        assert_eq!(report.publish_count, expected_publish);
        assert_eq!(report.resolve_count, expected_resolve);
        assert_eq!(report.obmm_pool_payload_write_count, expected_publish);
        assert_eq!(report.obmm_pool_payload_read_count, expected_resolve);
        assert_eq!(report.obmm_pool_queue_submit_count, expected_publish);
        assert!(report.obmm_pool_queue_deliver_count >= expected_publish);
        assert_eq!(report.missing_resolve_count, 0);
        assert!(report.obmm_pool_bytes_used > 0);
        assert_eq!(svc.report().obmm_pool_payload_write_count, expected_publish);
        assert!(kv_bytes_total > 0);
        assert!(hidden_bytes_total > 0);
        assert!(max_kv_len > 0);
        assert!(max_hidden_len > 0);
        assert!(min_kv_len > 0);
        assert!(min_hidden_len > 0);
        assert!(min_kv_len <= max_kv_len);
        assert!(min_hidden_len <= max_hidden_len);
        assert!(kv_bytes_total + hidden_bytes_total > max_kv_len + max_hidden_len);
    }

    #[test]
    fn object_service_stability_longrun_decode_like_handoff_8node_500_steps() {
        let mut profile = LingquObjectServiceProfile::default();
        profile.queue_depth = 8192;
        profile.obmm_pool.queue_depth = 4096;
        profile.obmm_pool.queue_auto_drain = true;
        profile.obmm_pool.pool_bytes = 2400 * 1024u64 * 1024u64;

        let mut svc = LingquObjectServiceStub::new(profile);
        let node_count = 8u64;
        let slot_count = 8u64;
        let steps = 500u64;

        let mut rng_state = 0x55aa_c0de_u64;
        let next_rand = |state: &mut u64| {
            *state ^= *state << 13;
            *state ^= *state >> 7;
            *state ^= *state << 17;
            *state
        };

        let mut expected_kv_versions = vec![vec![0u64; slot_count as usize]; node_count as usize];
        let mut expected_hidden_versions =
            vec![vec![0u64; slot_count as usize]; node_count as usize];
        let mut expected_publish = 0u64;
        let mut expected_resolve = 0u64;
        let mut kv_bytes_total = 0u64;
        let mut hidden_bytes_total = 0u64;
        let mut max_kv_len = 0u64;
        let mut max_hidden_len = 0u64;
        let mut min_kv_len = u64::MAX;
        let mut min_hidden_len = u64::MAX;
        let mut throughput_points = Vec::with_capacity(steps as usize);

        for step in 0..steps {
            let now = 60_000u64 + step.saturating_mul(9_000);
            let mut step_kv_checksums = Vec::with_capacity(node_count as usize);
            let mut step_hidden_checksums = Vec::with_capacity(node_count as usize);
            let mut step_kv_total: u64 = 0;
            let mut step_hidden_total: u64 = 0;

            for node in 0..node_count {
                let next_node = (node + 1) % node_count;
                let slot = (step % slot_count) as usize;
                let node_idx = node as usize;
                let growth_factor = 64u64 + (step / 4).min(192);

                let kv_len = {
                    let jitter = next_rand(&mut rng_state) % 1536;
                    let base = 24_000u64 + growth_factor.saturating_mul(220);
                    (base + jitter).min(96_000)
                };
                let hidden_len = {
                    let jitter = next_rand(&mut rng_state) % 2048;
                    let base = 14_000u64 + growth_factor.saturating_mul(140);
                    (base + jitter).min(72_000)
                };

                let kv_payload = growing_payload(
                    0x0bad_u64
                        .saturating_add(node.saturating_mul(13))
                        .saturating_add(step.saturating_mul(17)),
                    kv_len as usize,
                );
                let hidden_payload = growing_payload(
                    0x00a5_u64
                        .saturating_add(node.saturating_mul(19))
                        .saturating_add(step.saturating_mul(23)),
                    hidden_len as usize,
                );
                let kv_checksum = payload_checksum_fnv1a(&kv_payload);
                let hidden_checksum = payload_checksum_fnv1a(&hidden_payload);

                let kv_key = format!(
                    "qwen3/stress/longrun-500/node{node}->node{next_node}/slot{slot}/kvcache"
                );
                let hidden_key = format!(
                    "qwen3/stress/longrun-500/node{node}->node{next_node}/slot{slot}/hidden"
                );

                let kv_expected_version = expected_kv_versions[node_idx][slot];
                let hidden_expected_version = expected_hidden_versions[node_idx][slot];
                expected_kv_versions[node_idx][slot] =
                    expected_kv_versions[node_idx][slot].saturating_add(1);
                expected_hidden_versions[node_idx][slot] =
                    expected_hidden_versions[node_idx][slot].saturating_add(1);

                svc.submit_publish(
                    LingquObjectPublishReq {
                        task: None,
                        key: kv_key.clone(),
                        kind: LingquObjectKind::KvCacheBlock,
                        producer_entity: node,
                        owner_entity: Some(next_node),
                        expected_version: Some(kv_expected_version),
                        metadata: object_metadata(kv_len, kv_checksum),
                        placements: vec![LingquPayloadPlacement {
                            backend: LingquPayloadBackend::Shmem,
                            storage_ref: format!(
                                "qwen3/stress/longrun-500/payload/kv/node{node}/slot{slot}"
                            ),
                            segment: Some(SegmentHandle(95)),
                            offset: 0,
                            bytes: kv_len,
                            checksum: kv_checksum,
                            locality: LingquObjectLocality::DomainShared(0),
                        }],
                        payload_bytes: kv_payload,
                    },
                    now,
                )
                .expect("publish longrun-500 kv object");

                svc.submit_publish(
                    LingquObjectPublishReq {
                        task: None,
                        key: hidden_key.clone(),
                        kind: LingquObjectKind::RuntimeTensor,
                        producer_entity: node,
                        owner_entity: Some(next_node),
                        expected_version: Some(hidden_expected_version),
                        metadata: object_metadata(hidden_len, hidden_checksum),
                        placements: vec![LingquPayloadPlacement {
                            backend: LingquPayloadBackend::Shmem,
                            storage_ref: format!(
                                "qwen3/stress/longrun-500/payload/hidden/node{node}/slot{slot}"
                            ),
                            segment: Some(SegmentHandle(96)),
                            offset: 0,
                            bytes: hidden_len,
                            checksum: hidden_checksum,
                            locality: LingquObjectLocality::DomainShared(0),
                        }],
                        payload_bytes: hidden_payload,
                    },
                    now + 50,
                )
                .expect("publish longrun-500 hidden object");

                expected_publish = expected_publish.saturating_add(2);
                kv_bytes_total = kv_bytes_total.saturating_add(kv_len);
                hidden_bytes_total = hidden_bytes_total.saturating_add(hidden_len);
                step_kv_total = step_kv_total.saturating_add(kv_len);
                step_hidden_total = step_hidden_total.saturating_add(hidden_len);
                max_kv_len = max_kv_len.max(kv_len);
                max_hidden_len = max_hidden_len.max(hidden_len);
                min_kv_len = min_kv_len.min(kv_len);
                min_hidden_len = min_hidden_len.min(hidden_len);

                step_kv_checksums.push((
                    kv_key,
                    kv_expected_version.saturating_add(1),
                    kv_checksum,
                ));
                step_hidden_checksums.push((
                    hidden_key,
                    hidden_expected_version.saturating_add(1),
                    hidden_checksum,
                ));
            }

            let publish_events = svc.poll_ready(now + 300);
            assert_eq!(
                publish_events.len(),
                (node_count.saturating_mul(2)) as usize
            );
            assert!(publish_events
                .iter()
                .all(|event| event.status == CompletionStatus::Success));

            for node in 0..node_count {
                let next_node = (node + 1) % node_count;
                let slot = (step % slot_count) as usize;
                let kv_key = format!(
                    "qwen3/stress/longrun-500/node{node}->node{next_node}/slot{slot}/kvcache"
                );
                let hidden_key = format!(
                    "qwen3/stress/longrun-500/node{node}->node{next_node}/slot{slot}/hidden"
                );

                svc.submit_resolve(
                    LingquObjectResolveReq {
                        task: None,
                        key: kv_key,
                        requester_entity: next_node,
                        version: LingquObjectVersionSelector::LatestCommitted,
                        min_state: LingquObjectState::Committed,
                        preferred_backends: vec![LingquPayloadBackend::Shmem],
                    },
                    now + 380,
                )
                .expect("resolve longrun-500 kv object");
                svc.submit_resolve(
                    LingquObjectResolveReq {
                        task: None,
                        key: hidden_key,
                        requester_entity: next_node,
                        version: LingquObjectVersionSelector::LatestCommitted,
                        min_state: LingquObjectState::Committed,
                        preferred_backends: vec![LingquPayloadBackend::Shmem],
                    },
                    now + 380,
                )
                .expect("resolve longrun-500 hidden object");
            }

            let resolve_events = svc.poll_ready(now + 680);
            assert_eq!(
                resolve_events.len(),
                (node_count.saturating_mul(2)) as usize
            );
            assert!(resolve_events
                .iter()
                .all(|event| event.status == CompletionStatus::Success));

            expected_resolve += node_count.saturating_mul(2);
            throughput_points.push((step, step_kv_total, step_hidden_total));

            for (kv_key, expected_kv_version, expected_checksum) in step_kv_checksums {
                let kv_record = svc
                    .latest_record(&kv_key)
                    .expect("kv latest record should exist");
                assert_eq!(kv_record.version, expected_kv_version);
                assert_eq!(kv_record.kind, LingquObjectKind::KvCacheBlock);
                assert_eq!(kv_record.checksum, expected_checksum);
                let kv_copy = svc
                    .get_copy(&kv_key, LingquObjectVersionSelector::LatestCommitted)
                    .expect("kv copy");
                assert_eq!(payload_checksum_fnv1a(&kv_copy), expected_checksum);
            }

            for (hidden_key, expected_hidden_version, expected_checksum) in step_hidden_checksums {
                let hidden_record = svc
                    .latest_record(&hidden_key)
                    .expect("hidden latest record should exist");
                assert_eq!(hidden_record.version, expected_hidden_version);
                assert_eq!(hidden_record.kind, LingquObjectKind::RuntimeTensor);
                assert_eq!(hidden_record.checksum, expected_checksum);
                let hidden_copy = svc
                    .get_copy(&hidden_key, LingquObjectVersionSelector::LatestCommitted)
                    .expect("hidden copy");
                assert_eq!(payload_checksum_fnv1a(&hidden_copy), expected_checksum);
            }
        }

        let report = svc.report();
        assert_eq!(report.publish_count, expected_publish);
        assert_eq!(report.resolve_count, expected_resolve);
        assert_eq!(report.obmm_pool_payload_write_count, expected_publish);
        assert_eq!(report.obmm_pool_payload_read_count, expected_resolve);
        assert_eq!(report.obmm_pool_queue_submit_count, expected_publish);
        assert!(report.obmm_pool_queue_deliver_count >= expected_publish);
        assert_eq!(report.missing_resolve_count, 0);
        assert_eq!(svc.report().obmm_pool_payload_write_count, expected_publish);

        let total_payload = kv_bytes_total.saturating_add(hidden_bytes_total);
        let expected_objects = expected_publish;
        let mean_step_bytes = total_payload / steps;
        let mean_object_bytes = total_payload / expected_objects.max(1);
        let max_step_bytes = throughput_points
            .iter()
            .map(|(_, kv_len, hidden_len)| (*kv_len).saturating_add(*hidden_len))
            .max()
            .unwrap_or(0);
        let min_step_bytes = throughput_points
            .iter()
            .map(|(_, kv_len, hidden_len)| (*kv_len).saturating_add(*hidden_len))
            .min()
            .unwrap_or(u64::MAX);

        assert!(total_payload > 0);
        assert!(total_payload >= kv_bytes_total);
        assert!(mean_step_bytes >= min_step_bytes);
        assert!(mean_step_bytes <= max_step_bytes);
        assert!(max_kv_len > 0);
        assert!(max_hidden_len > 0);
        assert!(min_kv_len > 0);
        assert!(min_hidden_len > 0);
        assert!(max_kv_len >= 24_000);
        assert!(max_hidden_len >= 15_000);
        assert!(report.obmm_pool_bytes_used <= profile.obmm_pool.pool_bytes);
        assert!(mean_object_bytes > 10_000);
        assert_eq!(throughput_points.len() as u64, steps);
    }
}
