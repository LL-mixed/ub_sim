//! Host-side service simulation entry points.

use std::collections::{HashMap, VecDeque};

use sim_core::{
    BlockHash, CompletionEvent, CompletionSource, CompletionStatus, SegmentHandle,
    ServiceOpHandle, SimTimestamp, TaskKey,
};
use sim_runtime::{BlockReadReq, BlockService, BlockWriteReq};

#[derive(Debug, Clone)]
struct QueuedCompletion {
    ready_at: SimTimestamp,
    event: CompletionEvent,
}

fn drain_ready(
    queue: &mut VecDeque<QueuedCompletion>,
    now: SimTimestamp,
) -> Vec<CompletionEvent> {
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
                return Err(sim_core::SimError::InvalidInput("shmem segment bytes must be positive"));
            }
            if bytes > self.profile.max_segment_bytes {
                return Err(sim_core::SimError::InvalidInput("shmem segment exceeds size limit"));
            }
            if !self.segments.contains_key(&segment) && self.segments.len() >= self.profile.max_segments {
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
                        now + self.profile.metadata_latency_us + self.profile.data_latency_us + penalty,
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

#[cfg(test)]
mod tests {
    use super::block::{BlockServiceProfile, BlockServiceStub};
    use super::db::{DbGetReq, DbPutReq, DbServiceProfile, DbServiceStub};
    use super::dfs::{DfsReadReq, DfsServiceProfile, DfsServiceStub, DfsWriteReq};
    use super::shmem::{ShmemGetReq, ShmemPutReq, ShmemServiceProfile, ShmemServiceStub};
    use sim_core::{BlockHash, CompletionStatus, SegmentHandle};
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
        assert!(matches!(err, sim_core::SimError::InvalidInput("shmem queue full")));
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
        assert!(matches!(err, sim_core::SimError::InvalidInput("dfs queue full")));
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
        assert!(matches!(err, sim_core::SimError::InvalidInput("db queue full")));
    }
}
