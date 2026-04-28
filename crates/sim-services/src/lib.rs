//! Host-side service simulation entry points.

use std::collections::{HashMap, VecDeque};

use sim_core::{
    BlockHash, CompletionEvent, CompletionSource, CompletionStatus, SegmentHandle, ServiceOpHandle,
    SimTimestamp, TaskKey,
};
use sim_runtime::{BlockReadReq, BlockService, BlockWriteReq};

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
                max_segment_bytes: 1 << 20,
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
    use super::shmem::{ShmemGetReq, ShmemPutReq, ShmemServiceProfile, ShmemServiceStub};
    use super::weights::{
        ServiceObjectKind, ServiceObjectMetadataPut, ServiceObjectPayloadWrite,
        ServiceObjectPublishReq, ServiceObjectResolveReq, WeightMetadataPut, WeightPayloadWrite,
        WeightStorageKind, WeightsLoadReq, WeightsResolveReq, WeightsServiceStub,
    };
    use sim_core::{BlockHash, CompletionSource, CompletionStatus, SegmentHandle};
    use sim_runtime::{BlockReadReq, BlockWriteReq};

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
        assert!(ready
            .iter()
            .filter(|event| event.source == CompletionSource::DbService)
            .count()
            >= 16);
        assert!(ready
            .iter()
            .filter(|event| event.source == CompletionSource::BlockService)
            .count()
            >= 16);
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
        assert!(ready
            .iter()
            .filter(|event| event.source == CompletionSource::DbService)
            .count()
            >= 16);
        assert!(ready
            .iter()
            .filter(|event| event.source == CompletionSource::ShmemService)
            .count()
            >= 16);
    }
}
