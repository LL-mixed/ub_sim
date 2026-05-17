//! Product-shaped Lingqu Memory Service core.
//!
//! The crate owns the durable memory model and the hot-state materialization
//! contract. It intentionally keeps only one service layer in process; Host
//! and Guest deployments can wrap the same API with their own transport.

use std::cmp::Ordering;
use std::collections::{BTreeSet, HashMap, HashSet};

use serde::{Deserialize, Serialize};
use sim_core::{BlockHash, CompletionStatus, SegmentHandle, SimTimestamp, TensorDType};
use sim_runtime::{BlockReadReq, BlockWriteReq};
use sim_services::block::{BlockServiceProfile, BlockServiceStub};
use sim_services::dfs::{DfsReadReq, DfsServiceProfile, DfsServiceStub, DfsWriteReq};
use sim_services::object::{
    LingquObjectKind, LingquObjectLocality, LingquObjectMetadata, LingquObjectPublishReq,
    LingquObjectServiceStub, LingquPayloadBackend, LingquPayloadPlacement,
};
use thiserror::Error;

pub type MemoryResult<T> = Result<T, LingquMemoryError>;

#[derive(Debug, Error, PartialEq, Eq)]
pub enum LingquMemoryError {
    #[error("missing required field: {0}")]
    MissingField(&'static str),
    #[error("invalid value for {field}: {reason}")]
    InvalidValue {
        field: &'static str,
        reason: &'static str,
    },
    #[error("missing catalog: {0}")]
    MissingCatalog(String),
    #[error("missing record: {0}")]
    MissingRecord(String),
    #[error("missing chunk: {0}")]
    MissingChunk(String),
    #[error("missing embedding segment: {0}")]
    MissingEmbeddingSegment(String),
    #[error("missing vector index: {0}")]
    MissingVectorIndex(String),
    #[error("missing query result: {0}")]
    MissingQueryResult(String),
    #[error("missing dfs path: {0}")]
    MissingDfsPath(String),
    #[error("missing block payload: {0}")]
    MissingBlockPayload(String),
    #[error("durable service operation failed: {0}")]
    DurableServiceFailed(String),
    #[error("payload checksum mismatch for {id}: expected {expected:#x}, got {actual:#x}")]
    PayloadChecksumMismatch {
        id: String,
        expected: u64,
        actual: u64,
    },
    #[error("object publish failed: {0}")]
    ObjectPublishFailed(String),
    #[error("hot memory state must use OBMM-backed object refs: {0}")]
    NonObmmHotPlacement(String),
    #[error("catalog snapshot serialization failed: {0}")]
    SnapshotCodec(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MemoryScope {
    User,
    Session,
    Project,
    Corpus,
    System,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MemoryVisibility {
    Private,
    ProjectShared,
    ClusterShared,
    Public,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MemorySourceKind {
    UserProvided,
    ToolResult,
    SystemObservation,
    Derived,
    ImportedCorpus,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MemoryContentType {
    PlainText,
    Markdown,
    Json,
    Binary,
    EmbeddingOnly,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Serialize, Deserialize)]
pub enum MemoryTrustLevel {
    Untrusted,
    ModelDerived,
    UserConfirmed,
    SystemVerified,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MemoryRetentionPolicy {
    Ephemeral,
    Session,
    Project,
    Durable,
    LegalHold,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MemorySecurityLabel {
    Public,
    Internal,
    Confidential,
    Restricted,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MemoryPiiState {
    Unknown,
    None,
    Present,
    Redacted,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum MemoryRecordState {
    Pending,
    Committed,
    Tombstoned,
    Quarantined,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquDfsPath {
    pub path: String,
}

impl LingquDfsPath {
    pub fn new(path: impl Into<String>) -> Self {
        Self { path: path.into() }
    }

    fn validate(&self, field: &'static str) -> MemoryResult<()> {
        if self.path.trim().is_empty() {
            return Err(LingquMemoryError::MissingField(field));
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquBlockPayloadRef {
    pub block: BlockHash,
    pub offset: u64,
    pub bytes: u64,
    pub checksum: u64,
}

impl LingquBlockPayloadRef {
    pub fn new(block: impl Into<String>, offset: u64, bytes: u64, checksum: u64) -> Self {
        Self {
            block: BlockHash(block.into()),
            offset,
            bytes,
            checksum,
        }
    }

    fn validate(&self, field: &'static str) -> MemoryResult<()> {
        if self.block.0.trim().is_empty() {
            return Err(LingquMemoryError::MissingField(field));
        }
        if self.bytes == 0 {
            return Err(LingquMemoryError::InvalidValue {
                field,
                reason: "payload bytes must be non-zero",
            });
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct MemoryCorpusCatalog {
    pub catalog_id: String,
    pub namespace: String,
    pub dfs_path: LingquDfsPath,
    pub version: u64,
    pub record_ids: Vec<String>,
    pub vector_index_ids: Vec<String>,
    pub created_at_us: u64,
    pub updated_at_us: u64,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MemoryCatalogSnapshot {
    pub catalog: MemoryCorpusCatalog,
    pub records: Vec<MemoryRecord>,
    pub chunks: Vec<MemoryChunk>,
    pub embedding_segments: Vec<EmbeddingSegment>,
    pub vector_indexes: Vec<VectorIndexObject>,
}

impl MemoryCatalogSnapshot {
    pub fn validate(&self) -> MemoryResult<()> {
        self.catalog.validate()?;
        let record_ids = self
            .records
            .iter()
            .map(|record| record.record_id.clone())
            .collect::<HashSet<_>>();
        for expected_record_id in &self.catalog.record_ids {
            if !record_ids.contains(expected_record_id) {
                return Err(LingquMemoryError::MissingRecord(expected_record_id.clone()));
            }
        }

        let chunk_ids = self
            .chunks
            .iter()
            .map(|chunk| chunk.chunk_id.clone())
            .collect::<HashSet<_>>();
        for record in &self.records {
            record.validate()?;
            for chunk_ref in &record.chunk_refs {
                if !chunk_ids.contains(chunk_ref) {
                    return Err(LingquMemoryError::MissingChunk(chunk_ref.clone()));
                }
            }
        }
        for chunk in &self.chunks {
            chunk.validate()?;
            if !record_ids.contains(&chunk.record_id) {
                return Err(LingquMemoryError::MissingRecord(chunk.record_id.clone()));
            }
        }

        let segment_ids = self
            .embedding_segments
            .iter()
            .map(|segment| segment.segment_id.clone())
            .collect::<HashSet<_>>();
        for segment in &self.embedding_segments {
            segment.validate()?;
            for row in &segment.row_map {
                if !chunk_ids.contains(&row.chunk_id) {
                    return Err(LingquMemoryError::MissingChunk(row.chunk_id.clone()));
                }
            }
        }

        let index_ids = self
            .vector_indexes
            .iter()
            .map(|index| index.index_id.clone())
            .collect::<HashSet<_>>();
        for expected_index_id in &self.catalog.vector_index_ids {
            if !index_ids.contains(expected_index_id) {
                return Err(LingquMemoryError::MissingVectorIndex(
                    expected_index_id.clone(),
                ));
            }
        }
        for index in &self.vector_indexes {
            index.validate()?;
            if index.corpus_id != self.catalog.catalog_id {
                return Err(LingquMemoryError::InvalidValue {
                    field: "vector_index.corpus_id",
                    reason: "index must belong to snapshot catalog",
                });
            }
            for segment_id in &index.segment_ids {
                if !segment_ids.contains(segment_id) {
                    return Err(LingquMemoryError::MissingEmbeddingSegment(
                        segment_id.clone(),
                    ));
                }
            }
        }
        Ok(())
    }

    pub fn to_json_bytes(&self) -> MemoryResult<Vec<u8>> {
        self.validate()?;
        serde_json::to_vec(self).map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
    }

    pub fn from_json_bytes(bytes: &[u8]) -> MemoryResult<Self> {
        let snapshot = serde_json::from_slice::<Self>(bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        snapshot.validate()?;
        Ok(snapshot)
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquMemoryDurableStats {
    pub dfs_catalog_writes: u64,
    pub dfs_catalog_reads: u64,
    pub block_payload_writes: u64,
    pub block_payload_reads: u64,
    pub dfs_bytes_written: u64,
    pub dfs_bytes_read: u64,
    pub block_bytes_written: u64,
    pub block_bytes_read: u64,
}

#[derive(Debug)]
pub struct LingquMemoryDurableStore {
    dfs_service: DfsServiceStub,
    block_service: BlockServiceStub,
    dfs_payloads: HashMap<String, Vec<u8>>,
    block_payloads: HashMap<BlockHash, Vec<u8>>,
    now_us: SimTimestamp,
    stats: LingquMemoryDurableStats,
}

impl LingquMemoryDurableStore {
    pub fn new() -> Self {
        Self::with_profiles(DfsServiceProfile::default(), BlockServiceProfile::default())
    }

    pub fn with_profiles(
        dfs_profile: DfsServiceProfile,
        block_profile: BlockServiceProfile,
    ) -> Self {
        Self {
            dfs_service: DfsServiceStub::new(dfs_profile),
            block_service: BlockServiceStub::with_profile(block_profile),
            dfs_payloads: HashMap::new(),
            block_payloads: HashMap::new(),
            now_us: 1,
            stats: LingquMemoryDurableStats::default(),
        }
    }

    pub fn stats(&self) -> LingquMemoryDurableStats {
        self.stats
    }

    pub fn persist_catalog_snapshot(
        &mut self,
        snapshot: &MemoryCatalogSnapshot,
    ) -> MemoryResult<LingquDfsPath> {
        let bytes = snapshot.to_json_bytes()?;
        let path = snapshot.catalog.dfs_path.clone();
        path.validate("catalog.dfs_path")?;
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_catalog_snapshot(
        &mut self,
        path: &LingquDfsPath,
    ) -> MemoryResult<MemoryCatalogSnapshot> {
        path.validate("catalog.dfs_path")?;
        let bytes = self.submit_dfs_read(&path.path)?;
        MemoryCatalogSnapshot::from_json_bytes(&bytes)
    }

    pub fn persist_query_result(&mut self, result: &QueryResult) -> MemoryResult<LingquDfsPath> {
        let bytes = result.to_json_bytes()?;
        let path = query_result_dfs_path(&result.result_id)?;
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_query_result(&mut self, path: &LingquDfsPath) -> MemoryResult<QueryResult> {
        path.validate("query_result.dfs_path")?;
        let bytes = self.submit_dfs_read(&path.path)?;
        QueryResult::from_json_bytes(&bytes)
    }

    pub fn write_block_payload(
        &mut self,
        block: impl Into<String>,
        bytes: Vec<u8>,
    ) -> MemoryResult<LingquBlockPayloadRef> {
        if bytes.is_empty() {
            return Err(LingquMemoryError::MissingField("block_payload"));
        }
        let block = BlockHash(block.into());
        if block.0.trim().is_empty() {
            return Err(LingquMemoryError::MissingField("block"));
        }
        let checksum = checksum64(&bytes);
        let payload_ref = LingquBlockPayloadRef {
            block: block.clone(),
            offset: 0,
            bytes: bytes.len() as u64,
            checksum,
        };
        self.submit_block_write(block, bytes)?;
        Ok(payload_ref)
    }

    pub fn read_block_payload(
        &mut self,
        payload_ref: &LingquBlockPayloadRef,
    ) -> MemoryResult<Vec<u8>> {
        payload_ref.validate("block_payload_ref")?;
        let payload = self.submit_block_read(&payload_ref.block)?;
        let start = payload_ref.offset as usize;
        let end = payload_ref.offset.checked_add(payload_ref.bytes).ok_or(
            LingquMemoryError::InvalidValue {
                field: "block_payload_ref",
                reason: "payload range overflow",
            },
        )? as usize;
        if end > payload.len() {
            return Err(LingquMemoryError::InvalidValue {
                field: "block_payload_ref",
                reason: "payload range exceeds block bytes",
            });
        }
        let selected = payload[start..end].to_vec();
        let actual = checksum64(&selected);
        if actual != payload_ref.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: payload_ref.block.0.clone(),
                expected: payload_ref.checksum,
                actual,
            });
        }
        self.stats.block_bytes_read += selected.len() as u64;
        Ok(selected)
    }

    fn submit_dfs_write(&mut self, path: String, bytes: Vec<u8>) -> MemoryResult<()> {
        let now = self.next_timestamp();
        let handle = self
            .dfs_service
            .submit_write(
                DfsWriteReq {
                    task: None,
                    path: path.clone(),
                    bytes: bytes.len() as u64,
                },
                now,
            )
            .map_err(|err| LingquMemoryError::DurableServiceFailed(err.to_string()))?;
        let events = self.dfs_service.poll_ready(SimTimestamp::MAX);
        expect_success("dfs_write", handle.0, events)?;
        self.stats.dfs_catalog_writes += 1;
        self.stats.dfs_bytes_written += bytes.len() as u64;
        self.dfs_payloads.insert(path, bytes);
        Ok(())
    }

    fn submit_dfs_read(&mut self, path: &str) -> MemoryResult<Vec<u8>> {
        if !self.dfs_payloads.contains_key(path) {
            return Err(LingquMemoryError::MissingDfsPath(path.to_string()));
        }
        let now = self.next_timestamp();
        let handle = self
            .dfs_service
            .submit_read(
                DfsReadReq {
                    task: None,
                    path: path.to_string(),
                },
                now,
            )
            .map_err(|err| LingquMemoryError::DurableServiceFailed(err.to_string()))?;
        let events = self.dfs_service.poll_ready(SimTimestamp::MAX);
        expect_success("dfs_read", handle.0, events)?;
        let bytes = self
            .dfs_payloads
            .get(path)
            .cloned()
            .ok_or_else(|| LingquMemoryError::MissingDfsPath(path.to_string()))?;
        self.stats.dfs_catalog_reads += 1;
        self.stats.dfs_bytes_read += bytes.len() as u64;
        Ok(bytes)
    }

    fn submit_block_write(&mut self, block: BlockHash, bytes: Vec<u8>) -> MemoryResult<()> {
        let now = self.next_timestamp();
        let handle = self
            .block_service
            .submit_write(
                BlockWriteReq {
                    task: None,
                    block: block.clone(),
                },
                now,
            )
            .map_err(|err| LingquMemoryError::DurableServiceFailed(err.to_string()))?;
        let events = self.block_service.poll_ready(SimTimestamp::MAX);
        expect_success("block_write", handle.0, events)?;
        self.stats.block_payload_writes += 1;
        self.stats.block_bytes_written += bytes.len() as u64;
        self.block_payloads.insert(block, bytes);
        Ok(())
    }

    fn submit_block_read(&mut self, block: &BlockHash) -> MemoryResult<Vec<u8>> {
        if !self.block_payloads.contains_key(block) {
            return Err(LingquMemoryError::MissingBlockPayload(block.0.clone()));
        }
        let now = self.next_timestamp();
        let handle = self
            .block_service
            .submit_read(
                BlockReadReq {
                    task: None,
                    block: block.clone(),
                },
                now,
            )
            .map_err(|err| LingquMemoryError::DurableServiceFailed(err.to_string()))?;
        let events = self.block_service.poll_ready(SimTimestamp::MAX);
        expect_success("block_read", handle.0, events)?;
        self.stats.block_payload_reads += 1;
        self.block_payloads
            .get(block)
            .cloned()
            .ok_or_else(|| LingquMemoryError::MissingBlockPayload(block.0.clone()))
    }

    fn next_timestamp(&mut self) -> SimTimestamp {
        let now = self.now_us;
        self.now_us = self.now_us.saturating_add(1);
        now
    }
}

impl Default for LingquMemoryDurableStore {
    fn default() -> Self {
        Self::new()
    }
}

impl MemoryCorpusCatalog {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.catalog_id, "catalog_id")?;
        required_str(&self.namespace, "namespace")?;
        self.dfs_path.validate("dfs_path")?;
        nonzero(self.version, "version")?;
        monotonic_time(self.created_at_us, self.updated_at_us)?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MemoryRecord {
    pub record_id: String,
    pub corpus_id: String,
    pub scope: MemoryScope,
    pub visibility: MemoryVisibility,
    pub source_kind: MemorySourceKind,
    pub source_uri: String,
    pub source_checksum: u64,
    pub content_type: MemoryContentType,
    pub token_count: u32,
    pub trust_level: MemoryTrustLevel,
    pub confidence: f32,
    pub retention_policy: MemoryRetentionPolicy,
    pub security_label: MemorySecurityLabel,
    pub pii_state: MemoryPiiState,
    pub chunk_refs: Vec<String>,
    pub embedding_model_versions: Vec<String>,
    pub evidence_refs: Vec<String>,
    pub created_at_us: u64,
    pub updated_at_us: u64,
    pub expires_at_us: Option<u64>,
    pub version: u64,
    pub state: MemoryRecordState,
}

impl MemoryRecord {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.record_id, "record_id")?;
        required_str(&self.corpus_id, "corpus_id")?;
        required_str(&self.source_uri, "source_uri")?;
        nonzero(self.source_checksum, "source_checksum")?;
        nonzero(u64::from(self.token_count), "token_count")?;
        require_nonempty(&self.chunk_refs, "chunk_refs")?;
        require_nonempty(&self.embedding_model_versions, "embedding_model_versions")?;
        nonzero(self.version, "version")?;
        monotonic_time(self.created_at_us, self.updated_at_us)?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "expires_at_us",
                    reason: "expiration must be after creation",
                });
            }
        }
        if !self.confidence.is_finite() || !(0.0..=1.0).contains(&self.confidence) {
            return Err(LingquMemoryError::InvalidValue {
                field: "confidence",
                reason: "confidence must be in [0.0, 1.0]",
            });
        }
        if self.source_kind == MemorySourceKind::Derived
            && self.trust_level >= MemoryTrustLevel::UserConfirmed
            && self.evidence_refs.is_empty()
        {
            return Err(LingquMemoryError::InvalidValue {
                field: "evidence_refs",
                reason: "high-trust derived memory needs evidence refs",
            });
        }
        if self.security_label == MemorySecurityLabel::Public
            && self.pii_state == MemoryPiiState::Present
        {
            return Err(LingquMemoryError::InvalidValue {
                field: "pii_state",
                reason: "public memory cannot carry unredacted pii",
            });
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MemoryChunk {
    pub chunk_id: String,
    pub record_id: String,
    pub ordinal: u32,
    pub text_block_ref: LingquBlockPayloadRef,
    pub token_start: u32,
    pub token_count: u32,
    pub checksum: u64,
}

impl MemoryChunk {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.chunk_id, "chunk_id")?;
        required_str(&self.record_id, "record_id")?;
        self.text_block_ref.validate("text_block_ref")?;
        nonzero(u64::from(self.token_count), "chunk_token_count")?;
        nonzero(self.checksum, "chunk_checksum")?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EmbeddingRow {
    pub chunk_id: String,
    pub row: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct EmbeddingSegment {
    pub segment_id: String,
    pub model_version: String,
    pub dims: u32,
    pub row_count: u32,
    pub row_stride_bytes: u32,
    pub dtype: TensorDType,
    pub vector_block_refs: Vec<LingquBlockPayloadRef>,
    pub row_map: Vec<EmbeddingRow>,
    pub checksum: u64,
    pub version: u64,
}

impl EmbeddingSegment {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.segment_id, "segment_id")?;
        required_str(&self.model_version, "model_version")?;
        nonzero(u64::from(self.dims), "dims")?;
        nonzero(u64::from(self.row_count), "row_count")?;
        nonzero(u64::from(self.row_stride_bytes), "row_stride_bytes")?;
        require_nonempty(&self.vector_block_refs, "vector_block_refs")?;
        for block_ref in &self.vector_block_refs {
            block_ref.validate("vector_block_refs")?;
        }
        if self.row_map.len() != self.row_count as usize {
            return Err(LingquMemoryError::InvalidValue {
                field: "row_map",
                reason: "row_map length must equal row_count",
            });
        }
        for row in &self.row_map {
            required_str(&row.chunk_id, "row_map.chunk_id")?;
            if row.row >= self.row_count {
                return Err(LingquMemoryError::InvalidValue {
                    field: "row_map.row",
                    reason: "row index out of segment bounds",
                });
            }
        }
        nonzero(self.checksum, "embedding_segment_checksum")?;
        nonzero(self.version, "embedding_segment_version")?;
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum VectorIndexKind {
    Flat,
    Hnsw,
    IvfFlat,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct VectorIndexObject {
    pub index_id: String,
    pub corpus_id: String,
    pub kind: VectorIndexKind,
    pub embedding_model_version: String,
    pub segment_ids: Vec<String>,
    pub manifest_path: LingquDfsPath,
    pub created_at_us: u64,
    pub updated_at_us: u64,
    pub version: u64,
}

impl VectorIndexObject {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.index_id, "index_id")?;
        required_str(&self.corpus_id, "corpus_id")?;
        required_str(&self.embedding_model_version, "embedding_model_version")?;
        require_nonempty(&self.segment_ids, "segment_ids")?;
        self.manifest_path.validate("manifest_path")?;
        nonzero(self.version, "vector_index_version")?;
        monotonic_time(self.created_at_us, self.updated_at_us)?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MemoryQuery {
    pub query_id: String,
    pub corpus_ids: Vec<String>,
    pub scope_filter: Vec<MemoryScope>,
    pub visibility_filter: Vec<MemoryVisibility>,
    pub min_trust: MemoryTrustLevel,
    pub min_confidence: f32,
    pub embedding_model_version: String,
    pub top_k: usize,
    pub query_embedding_ref: Option<LingquBlockPayloadRef>,
}

impl MemoryQuery {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.query_id, "query_id")?;
        require_nonempty(&self.corpus_ids, "corpus_ids")?;
        required_str(&self.embedding_model_version, "embedding_model_version")?;
        nonzero(self.top_k as u64, "top_k")?;
        if !self.min_confidence.is_finite() || !(0.0..=1.0).contains(&self.min_confidence) {
            return Err(LingquMemoryError::InvalidValue {
                field: "min_confidence",
                reason: "min_confidence must be in [0.0, 1.0]",
            });
        }
        if let Some(query_embedding_ref) = &self.query_embedding_ref {
            query_embedding_ref.validate("query_embedding_ref")?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct QueryMatch {
    pub vector_index_id: String,
    pub chunk_id: String,
    pub record_id: String,
    pub segment_id: String,
    pub row: u32,
    pub score: f32,
    pub trust_level: MemoryTrustLevel,
    pub confidence: f32,
}

impl QueryMatch {
    fn validate(&self) -> MemoryResult<()> {
        required_str(&self.vector_index_id, "query_match.vector_index_id")?;
        required_str(&self.chunk_id, "query_match.chunk_id")?;
        required_str(&self.record_id, "query_match.record_id")?;
        required_str(&self.segment_id, "query_match.segment_id")?;
        if !self.score.is_finite() {
            return Err(LingquMemoryError::InvalidValue {
                field: "query_match.score",
                reason: "score must be finite",
            });
        }
        if !self.confidence.is_finite() || !(0.0..=1.0).contains(&self.confidence) {
            return Err(LingquMemoryError::InvalidValue {
                field: "query_match.confidence",
                reason: "confidence must be in [0.0, 1.0]",
            });
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct QuerySegmentVersion {
    pub segment_id: String,
    pub version: u64,
    pub checksum: u64,
}

impl QuerySegmentVersion {
    fn validate(&self) -> MemoryResult<()> {
        required_str(&self.segment_id, "query_segment_version.segment_id")?;
        nonzero(self.version, "query_segment_version.version")?;
        nonzero(self.checksum, "query_segment_version.checksum")?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct QueryResult {
    pub result_id: String,
    pub query_id: String,
    pub vector_index_ids: Vec<String>,
    pub matches: Vec<QueryMatch>,
    pub selected_record_ids: Vec<String>,
    pub selected_chunk_ids: Vec<String>,
    pub embedding_segment_versions: Vec<QuerySegmentVersion>,
    pub evidence_refs: Vec<String>,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
}

impl QueryResult {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.result_id, "query_result_id")?;
        required_str(&self.query_id, "query_id")?;
        nonzero(self.version, "query_result_version")?;
        nonzero(self.checksum, "query_result_checksum")?;
        for vector_index_id in &self.vector_index_ids {
            required_str(vector_index_id, "query_result.vector_index_ids")?;
        }
        for query_match in &self.matches {
            query_match.validate()?;
        }
        for record_id in &self.selected_record_ids {
            required_str(record_id, "query_result.selected_record_ids")?;
        }
        for chunk_id in &self.selected_chunk_ids {
            required_str(chunk_id, "query_result.selected_chunk_ids")?;
        }
        for segment in &self.embedding_segment_versions {
            segment.validate()?;
        }
        for evidence_ref in &self.evidence_refs {
            required_str(evidence_ref, "query_result.evidence_refs")?;
        }
        let actual = query_result_audit_checksum(
            &self.result_id,
            &self.query_id,
            &self.vector_index_ids,
            &self.matches,
            &self.selected_record_ids,
            &self.selected_chunk_ids,
            &self.embedding_segment_versions,
            &self.evidence_refs,
            self.version,
            self.created_at_us,
        );
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.result_id.clone(),
                expected: self.checksum,
                actual,
            });
        }
        Ok(())
    }

    pub fn to_json_bytes(&self) -> MemoryResult<Vec<u8>> {
        self.validate()?;
        serde_json::to_vec(self).map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
    }

    pub fn from_json_bytes(bytes: &[u8]) -> MemoryResult<Self> {
        let result = serde_json::from_slice::<Self>(bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        result.validate()?;
        Ok(result)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum HotObjectBackend {
    ObmmShmem,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct HotTensorObjectRef {
    pub object_key: String,
    pub version: u64,
    pub backend: HotObjectBackend,
    pub storage_ref: String,
    pub segment: Option<SegmentHandle>,
    pub offset: u64,
    pub bytes: u64,
    pub checksum: u64,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct HotMemoryStateObject {
    pub state_id: String,
    pub query_result_id: String,
    pub query_result_manifest_ref: Option<LingquDfsPath>,
    pub table: HotTensorObjectRef,
    pub indices: HotTensorObjectRef,
    pub scores: HotTensorObjectRef,
    pub selected_chunk_ids: Vec<String>,
    pub created_at_us: u64,
}

impl HotMemoryStateObject {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.state_id, "hot_state_id")?;
        required_str(&self.query_result_id, "query_result_id")?;
        if let Some(path) = &self.query_result_manifest_ref {
            path.validate("query_result_manifest_ref")?;
        }
        validate_hot_ref(&self.table)?;
        validate_hot_ref(&self.indices)?;
        validate_hot_ref(&self.scores)?;
        require_nonempty(&self.selected_chunk_ids, "selected_chunk_ids")?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EngramStateObject {
    pub state_id: String,
    pub hot_memory_state_id: String,
    pub query_result_manifest_ref: Option<LingquDfsPath>,
    pub table: HotTensorObjectRef,
    pub indices: HotTensorObjectRef,
    pub gate: Option<HotTensorObjectRef>,
    pub created_at_us: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct HotMemoryMaterializeReq {
    pub state_id: String,
    pub query_result_id: String,
    pub query_result_manifest_ref: Option<LingquDfsPath>,
    pub table_shape: Vec<u64>,
    pub table_values: Vec<f32>,
    pub indices: Vec<u32>,
    pub owner_entity: u64,
    pub producer_entity: u64,
    pub now_us: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HotMemoryMaterializeFromQueryReq {
    pub state_id: String,
    pub query_result_id: String,
    pub owner_entity: u64,
    pub producer_entity: u64,
    pub now_us: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct EngramStateMaterializeReq {
    pub state_id: String,
    pub hot_memory_state_id: String,
    pub gate_values: Vec<f32>,
    pub owner_entity: u64,
    pub producer_entity: u64,
    pub now_us: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct EngramStateMaterializeFromBlockReq {
    pub state_id: String,
    pub hot_memory_state_id: String,
    pub gate_weight_ref: LingquBlockPayloadRef,
    pub owner_entity: u64,
    pub producer_entity: u64,
    pub now_us: u64,
}

#[derive(Debug, Default)]
pub struct LingquMemoryService {
    catalogs: HashMap<String, MemoryCorpusCatalog>,
    records: HashMap<String, MemoryRecord>,
    chunks: HashMap<String, MemoryChunk>,
    embedding_segments: HashMap<String, EmbeddingSegment>,
    vector_indexes: HashMap<String, VectorIndexObject>,
    query_results: HashMap<String, QueryResult>,
    hot_states: HashMap<String, HotMemoryStateObject>,
    engram_states: HashMap<String, EngramStateObject>,
}

impl LingquMemoryService {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn publish_catalog(&mut self, catalog: MemoryCorpusCatalog) -> MemoryResult<()> {
        catalog.validate()?;
        self.catalogs.insert(catalog.catalog_id.clone(), catalog);
        Ok(())
    }

    pub fn export_catalog_snapshot(&self, catalog_id: &str) -> MemoryResult<MemoryCatalogSnapshot> {
        let catalog = self
            .catalogs
            .get(catalog_id)
            .ok_or_else(|| LingquMemoryError::MissingCatalog(catalog_id.to_string()))?;
        let mut records = Vec::new();
        let mut chunks = Vec::new();
        let mut chunk_ids = HashSet::new();
        for record_id in &catalog.record_ids {
            let record = self
                .records
                .get(record_id)
                .ok_or_else(|| LingquMemoryError::MissingRecord(record_id.clone()))?;
            for chunk_id in &record.chunk_refs {
                let chunk = self
                    .chunks
                    .get(chunk_id)
                    .ok_or_else(|| LingquMemoryError::MissingChunk(chunk_id.clone()))?;
                if chunk_ids.insert(chunk_id.clone()) {
                    chunks.push(chunk.clone());
                }
            }
            records.push(record.clone());
        }

        let mut vector_indexes = Vec::new();
        let mut embedding_segments = Vec::new();
        let mut segment_ids = HashSet::new();
        for index_id in &catalog.vector_index_ids {
            let index = self
                .vector_indexes
                .get(index_id)
                .ok_or_else(|| LingquMemoryError::MissingVectorIndex(index_id.clone()))?;
            for segment_id in &index.segment_ids {
                let segment = self.embedding_segments.get(segment_id).ok_or_else(|| {
                    LingquMemoryError::MissingEmbeddingSegment(segment_id.clone())
                })?;
                if segment_ids.insert(segment_id.clone()) {
                    embedding_segments.push(segment.clone());
                }
            }
            vector_indexes.push(index.clone());
        }

        let snapshot = MemoryCatalogSnapshot {
            catalog: catalog.clone(),
            records,
            chunks,
            embedding_segments,
            vector_indexes,
        };
        snapshot.validate()?;
        Ok(snapshot)
    }

    pub fn import_catalog_snapshot(&mut self, snapshot: MemoryCatalogSnapshot) -> MemoryResult<()> {
        snapshot.validate()?;
        for chunk in snapshot.chunks {
            self.chunks.insert(chunk.chunk_id.clone(), chunk);
        }
        for record in snapshot.records {
            self.records.insert(record.record_id.clone(), record);
        }
        for segment in snapshot.embedding_segments {
            self.embedding_segments
                .insert(segment.segment_id.clone(), segment);
        }
        for index in snapshot.vector_indexes {
            self.vector_indexes.insert(index.index_id.clone(), index);
        }
        self.catalogs
            .insert(snapshot.catalog.catalog_id.clone(), snapshot.catalog);
        Ok(())
    }

    pub fn persist_catalog_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
        catalog_id: &str,
    ) -> MemoryResult<LingquDfsPath> {
        let snapshot = self.export_catalog_snapshot(catalog_id)?;
        durable_store.persist_catalog_snapshot(&snapshot)
    }

    pub fn rebuild_catalog_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
        path: &LingquDfsPath,
    ) -> MemoryResult<MemoryCorpusCatalog> {
        let snapshot = durable_store.load_catalog_snapshot(path)?;
        let catalog = snapshot.catalog.clone();
        self.import_catalog_snapshot(snapshot)?;
        Ok(catalog)
    }

    pub fn ingest_record(
        &mut self,
        record: MemoryRecord,
        chunks: Vec<MemoryChunk>,
    ) -> MemoryResult<()> {
        record.validate()?;
        if chunks.is_empty() {
            return Err(LingquMemoryError::MissingField("chunks"));
        }
        let mut chunk_ids = BTreeSet::new();
        for chunk in &chunks {
            chunk.validate()?;
            if chunk.record_id != record.record_id {
                return Err(LingquMemoryError::InvalidValue {
                    field: "chunk.record_id",
                    reason: "chunk must reference the ingested record",
                });
            }
            chunk_ids.insert(chunk.chunk_id.clone());
        }
        for chunk_ref in &record.chunk_refs {
            if !chunk_ids.contains(chunk_ref) {
                return Err(LingquMemoryError::MissingChunk(chunk_ref.clone()));
            }
        }
        for chunk in chunks {
            self.chunks.insert(chunk.chunk_id.clone(), chunk);
        }
        self.records.insert(record.record_id.clone(), record);
        Ok(())
    }

    pub fn register_embedding_segment(&mut self, segment: EmbeddingSegment) -> MemoryResult<()> {
        segment.validate()?;
        for row in &segment.row_map {
            if !self.chunks.contains_key(&row.chunk_id) {
                return Err(LingquMemoryError::MissingChunk(row.chunk_id.clone()));
            }
        }
        self.embedding_segments
            .insert(segment.segment_id.clone(), segment);
        Ok(())
    }

    pub fn register_vector_index(&mut self, index: VectorIndexObject) -> MemoryResult<()> {
        index.validate()?;
        if !self.catalogs.contains_key(&index.corpus_id) {
            return Err(LingquMemoryError::MissingCatalog(index.corpus_id.clone()));
        }
        for segment_id in &index.segment_ids {
            if !self.embedding_segments.contains_key(segment_id) {
                return Err(LingquMemoryError::MissingEmbeddingSegment(
                    segment_id.clone(),
                ));
            }
        }
        self.vector_indexes.insert(index.index_id.clone(), index);
        Ok(())
    }

    fn build_query_result(
        &self,
        query: &MemoryQuery,
        matches: Vec<QueryMatch>,
        now_us: u64,
    ) -> MemoryResult<QueryResult> {
        let mut vector_index_ids = BTreeSet::new();
        let mut selected_record_ids = Vec::new();
        let mut selected_record_seen = HashSet::new();
        let mut selected_chunk_ids = Vec::new();
        let mut selected_chunk_seen = HashSet::new();
        let mut segment_ids = BTreeSet::new();
        let mut evidence_refs = BTreeSet::new();

        for query_match in &matches {
            vector_index_ids.insert(query_match.vector_index_id.clone());
            if selected_record_seen.insert(query_match.record_id.clone()) {
                selected_record_ids.push(query_match.record_id.clone());
            }
            if selected_chunk_seen.insert(query_match.chunk_id.clone()) {
                selected_chunk_ids.push(query_match.chunk_id.clone());
            }
            segment_ids.insert(query_match.segment_id.clone());
            let record = self
                .records
                .get(&query_match.record_id)
                .ok_or_else(|| LingquMemoryError::MissingRecord(query_match.record_id.clone()))?;
            for evidence_ref in &record.evidence_refs {
                evidence_refs.insert(evidence_ref.clone());
            }
        }

        let mut embedding_segment_versions = Vec::with_capacity(segment_ids.len());
        for segment_id in segment_ids {
            let segment = self
                .embedding_segments
                .get(&segment_id)
                .ok_or_else(|| LingquMemoryError::MissingEmbeddingSegment(segment_id.clone()))?;
            embedding_segment_versions.push(QuerySegmentVersion {
                segment_id,
                version: segment.version,
                checksum: segment.checksum,
            });
        }

        let result_id = format!("query-result/{}", query.query_id);
        let vector_index_ids = vector_index_ids.into_iter().collect::<Vec<_>>();
        let evidence_refs = evidence_refs.into_iter().collect::<Vec<_>>();
        let checksum = query_result_audit_checksum(
            &result_id,
            &query.query_id,
            &vector_index_ids,
            &matches,
            &selected_record_ids,
            &selected_chunk_ids,
            &embedding_segment_versions,
            &evidence_refs,
            1,
            now_us,
        );

        let result = QueryResult {
            result_id,
            query_id: query.query_id.clone(),
            vector_index_ids,
            matches,
            selected_record_ids,
            selected_chunk_ids,
            embedding_segment_versions,
            evidence_refs,
            checksum,
            version: 1,
            created_at_us: now_us,
        };
        result.validate()?;
        Ok(result)
    }

    pub fn query_memory(&mut self, query: MemoryQuery, now_us: u64) -> MemoryResult<QueryResult> {
        query.validate()?;
        let corpus_set = query.corpus_ids.iter().cloned().collect::<HashSet<_>>();
        let scope_set = query.scope_filter.iter().copied().collect::<HashSet<_>>();
        let visibility_set = query
            .visibility_filter
            .iter()
            .copied()
            .collect::<HashSet<_>>();
        let mut matches = Vec::new();

        for index in self.vector_indexes.values() {
            if !corpus_set.contains(&index.corpus_id)
                || index.embedding_model_version != query.embedding_model_version
            {
                continue;
            }
            for segment_id in &index.segment_ids {
                let segment = self.embedding_segments.get(segment_id).ok_or_else(|| {
                    LingquMemoryError::MissingEmbeddingSegment(segment_id.clone())
                })?;
                for row in &segment.row_map {
                    let chunk = self
                        .chunks
                        .get(&row.chunk_id)
                        .ok_or_else(|| LingquMemoryError::MissingChunk(row.chunk_id.clone()))?;
                    let record = self
                        .records
                        .get(&chunk.record_id)
                        .ok_or_else(|| LingquMemoryError::MissingRecord(chunk.record_id.clone()))?;
                    if record.state != MemoryRecordState::Committed {
                        continue;
                    }
                    if record.trust_level < query.min_trust
                        || record.confidence < query.min_confidence
                    {
                        continue;
                    }
                    if !scope_set.is_empty() && !scope_set.contains(&record.scope) {
                        continue;
                    }
                    if !visibility_set.is_empty() && !visibility_set.contains(&record.visibility) {
                        continue;
                    }
                    matches.push(QueryMatch {
                        vector_index_id: index.index_id.clone(),
                        chunk_id: row.chunk_id.clone(),
                        record_id: record.record_id.clone(),
                        segment_id: segment.segment_id.clone(),
                        row: row.row,
                        score: deterministic_score(record, row.row),
                        trust_level: record.trust_level,
                        confidence: record.confidence,
                    });
                }
            }
        }

        matches.sort_by(|left, right| {
            right
                .score
                .partial_cmp(&left.score)
                .unwrap_or(Ordering::Equal)
                .then_with(|| left.record_id.cmp(&right.record_id))
                .then_with(|| left.chunk_id.cmp(&right.chunk_id))
        });
        matches.truncate(query.top_k);

        let result = self.build_query_result(&query, matches, now_us)?;
        self.query_results
            .insert(result.result_id.clone(), result.clone());
        Ok(result)
    }

    pub fn query_memory_flat(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
        query: MemoryQuery,
        now_us: u64,
    ) -> MemoryResult<QueryResult> {
        query.validate()?;
        let query_embedding_ref = query
            .query_embedding_ref
            .as_ref()
            .ok_or(LingquMemoryError::MissingField("query_embedding_ref"))?;
        let query_vector = read_f32_payload(durable_store, query_embedding_ref)?;
        if query_vector.is_empty() {
            return Err(LingquMemoryError::InvalidValue {
                field: "query_embedding_ref",
                reason: "query embedding must not be empty",
            });
        }

        let corpus_set = query.corpus_ids.iter().cloned().collect::<HashSet<_>>();
        let scope_set = query.scope_filter.iter().copied().collect::<HashSet<_>>();
        let visibility_set = query
            .visibility_filter
            .iter()
            .copied()
            .collect::<HashSet<_>>();
        let mut matches = Vec::new();

        for index in self.vector_indexes.values() {
            if index.kind != VectorIndexKind::Flat
                || !corpus_set.contains(&index.corpus_id)
                || index.embedding_model_version != query.embedding_model_version
            {
                continue;
            }
            for segment_id in &index.segment_ids {
                let segment = self.embedding_segments.get(segment_id).ok_or_else(|| {
                    LingquMemoryError::MissingEmbeddingSegment(segment_id.clone())
                })?;
                if segment.dtype != TensorDType::F32 {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "embedding_segment.dtype",
                        reason: "flat query currently requires f32 embeddings",
                    });
                }
                if segment.dims as usize != query_vector.len() {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "embedding_segment.dims",
                        reason: "segment dims must match query embedding dims",
                    });
                }
                let segment_bytes = read_segment_bytes(durable_store, segment)?;
                for row in &segment.row_map {
                    let chunk = self
                        .chunks
                        .get(&row.chunk_id)
                        .ok_or_else(|| LingquMemoryError::MissingChunk(row.chunk_id.clone()))?;
                    let record = self
                        .records
                        .get(&chunk.record_id)
                        .ok_or_else(|| LingquMemoryError::MissingRecord(chunk.record_id.clone()))?;
                    if !record_selectable(record, &query, &scope_set, &visibility_set) {
                        continue;
                    }
                    let row_vector = segment_row_f32_values(segment, &segment_bytes, row.row)?;
                    matches.push(QueryMatch {
                        vector_index_id: index.index_id.clone(),
                        chunk_id: row.chunk_id.clone(),
                        record_id: record.record_id.clone(),
                        segment_id: segment.segment_id.clone(),
                        row: row.row,
                        score: dot_product(&query_vector, &row_vector),
                        trust_level: record.trust_level,
                        confidence: record.confidence,
                    });
                }
            }
        }

        matches.sort_by(|left, right| {
            right
                .score
                .partial_cmp(&left.score)
                .unwrap_or(Ordering::Equal)
                .then_with(|| left.record_id.cmp(&right.record_id))
                .then_with(|| left.chunk_id.cmp(&right.chunk_id))
        });
        matches.truncate(query.top_k);

        let result = self.build_query_result(&query, matches, now_us)?;
        self.query_results
            .insert(result.result_id.clone(), result.clone());
        Ok(result)
    }

    pub fn materialize_hot_state(
        &mut self,
        object_service: &mut LingquObjectServiceStub,
        req: HotMemoryMaterializeReq,
    ) -> MemoryResult<HotMemoryStateObject> {
        required_str(&req.state_id, "hot_state_id")?;
        required_str(&req.query_result_id, "query_result_id")?;
        if let Some(path) = &req.query_result_manifest_ref {
            path.validate("query_result_manifest_ref")?;
        }
        let query_result = self
            .query_results
            .get(&req.query_result_id)
            .ok_or_else(|| LingquMemoryError::MissingQueryResult(req.query_result_id.clone()))?;
        if query_result.matches.is_empty() {
            return Err(LingquMemoryError::InvalidValue {
                field: "query_result_id",
                reason: "query result has no matches to materialize",
            });
        }
        validate_table_shape(&req.table_shape, req.table_values.len())?;
        if req.indices.is_empty() {
            return Err(LingquMemoryError::MissingField("indices"));
        }
        let scores = query_result
            .matches
            .iter()
            .map(|query_match| query_match.score)
            .collect::<Vec<_>>();
        let base_key = format!("lingqu/memory/hot/{}", req.state_id);
        let table = publish_hot_tensor(
            object_service,
            format!("{base_key}/table"),
            f32_vec_to_le_bytes(&req.table_values),
            TensorDType::F32,
            req.table_shape.clone(),
            req.producer_entity,
            req.owner_entity,
            req.now_us,
        )?;
        let indices = publish_hot_tensor(
            object_service,
            format!("{base_key}/indices"),
            u32_vec_to_le_bytes(&req.indices),
            TensorDType::U32,
            vec![req.indices.len() as u64],
            req.producer_entity,
            req.owner_entity,
            req.now_us.saturating_add(1),
        )?;
        let scores = publish_hot_tensor(
            object_service,
            format!("{base_key}/scores"),
            f32_vec_to_le_bytes(&scores),
            TensorDType::F32,
            vec![scores.len() as u64],
            req.producer_entity,
            req.owner_entity,
            req.now_us.saturating_add(2),
        )?;

        let state = HotMemoryStateObject {
            state_id: req.state_id,
            query_result_id: req.query_result_id,
            query_result_manifest_ref: req.query_result_manifest_ref,
            table,
            indices,
            scores,
            selected_chunk_ids: query_result
                .matches
                .iter()
                .map(|query_match| query_match.chunk_id.clone())
                .collect(),
            created_at_us: req.now_us,
        };
        state.validate()?;
        self.hot_states
            .insert(state.state_id.clone(), state.clone());
        Ok(state)
    }

    pub fn materialize_hot_state_from_query(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
        object_service: &mut LingquObjectServiceStub,
        req: HotMemoryMaterializeFromQueryReq,
    ) -> MemoryResult<HotMemoryStateObject> {
        required_str(&req.state_id, "hot_state_id")?;
        required_str(&req.query_result_id, "query_result_id")?;
        let query_result = self
            .query_results
            .get(&req.query_result_id)
            .ok_or_else(|| LingquMemoryError::MissingQueryResult(req.query_result_id.clone()))?
            .clone();
        if query_result.matches.is_empty() {
            return Err(LingquMemoryError::InvalidValue {
                field: "query_result_id",
                reason: "query result has no matches to materialize",
            });
        }

        let mut segment_bytes_by_id = HashMap::<String, Vec<u8>>::new();
        let mut table_values = Vec::new();
        let mut table_dims = None;
        let mut indices = Vec::with_capacity(query_result.matches.len());
        for (selected_row, query_match) in query_result.matches.iter().enumerate() {
            let segment = self
                .embedding_segments
                .get(&query_match.segment_id)
                .ok_or_else(|| {
                    LingquMemoryError::MissingEmbeddingSegment(query_match.segment_id.clone())
                })?;
            if segment.dtype != TensorDType::F32 {
                return Err(LingquMemoryError::InvalidValue {
                    field: "embedding_segment.dtype",
                    reason: "hot memory materialization currently requires f32 embeddings",
                });
            }
            if let Some(expected_dims) = table_dims {
                if expected_dims != segment.dims {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "embedding_segment.dims",
                        reason: "selected embedding rows must have the same dims",
                    });
                }
            } else {
                table_dims = Some(segment.dims);
            }
            let segment_bytes =
                if let Some(segment_bytes) = segment_bytes_by_id.get(&query_match.segment_id) {
                    segment_bytes
                } else {
                    let segment_bytes = read_segment_bytes(durable_store, segment)?;
                    segment_bytes_by_id.insert(query_match.segment_id.clone(), segment_bytes);
                    segment_bytes_by_id
                        .get(&query_match.segment_id)
                        .expect("segment bytes inserted")
                };
            let row_values = segment_row_f32_values(segment, segment_bytes, query_match.row)?;
            table_values.extend_from_slice(&row_values);
            indices.push(u32::try_from(selected_row).map_err(|_| {
                LingquMemoryError::InvalidValue {
                    field: "indices",
                    reason: "selected row count exceeds u32",
                }
            })?);
        }

        let query_result_manifest_ref = Some(durable_store.persist_query_result(&query_result)?);
        self.materialize_hot_state(
            object_service,
            HotMemoryMaterializeReq {
                state_id: req.state_id,
                query_result_id: req.query_result_id,
                query_result_manifest_ref,
                table_shape: vec![
                    query_result.matches.len() as u64,
                    u64::from(table_dims.unwrap()),
                ],
                table_values,
                indices,
                owner_entity: req.owner_entity,
                producer_entity: req.producer_entity,
                now_us: req.now_us,
            },
        )
    }

    pub fn build_engram_state(
        &mut self,
        state_id: impl Into<String>,
        hot_memory_state_id: &str,
        gate: Option<HotTensorObjectRef>,
        created_at_us: u64,
    ) -> MemoryResult<EngramStateObject> {
        let state_id = state_id.into();
        required_str(&state_id, "engram_state_id")?;
        let hot_state = self
            .hot_states
            .get(hot_memory_state_id)
            .ok_or_else(|| LingquMemoryError::MissingField("hot_memory_state_id"))?;
        if let Some(gate) = gate.as_ref() {
            validate_hot_ref(gate)?;
        }
        let engram = EngramStateObject {
            state_id,
            hot_memory_state_id: hot_memory_state_id.to_string(),
            query_result_manifest_ref: hot_state.query_result_manifest_ref.clone(),
            table: hot_state.table.clone(),
            indices: hot_state.indices.clone(),
            gate,
            created_at_us,
        };
        self.engram_states
            .insert(engram.state_id.clone(), engram.clone());
        Ok(engram)
    }

    pub fn materialize_engram_state(
        &mut self,
        object_service: &mut LingquObjectServiceStub,
        req: EngramStateMaterializeReq,
    ) -> MemoryResult<EngramStateObject> {
        required_str(&req.state_id, "engram_state_id")?;
        required_str(&req.hot_memory_state_id, "hot_memory_state_id")?;
        require_nonempty(&req.gate_values, "gate_values")?;
        let hot_state = self
            .hot_states
            .get(&req.hot_memory_state_id)
            .ok_or_else(|| LingquMemoryError::MissingField("hot_memory_state_id"))?;
        let table_shape = &hot_state.table.shape;
        let hidden_size = table_shape
            .last()
            .copied()
            .ok_or(LingquMemoryError::InvalidValue {
                field: "hot_state.table.shape",
                reason: "table shape must include hidden dimension",
            })?;
        if hidden_size as usize != req.gate_values.len() {
            return Err(LingquMemoryError::InvalidValue {
                field: "gate_values",
                reason: "gate value count must match hot table hidden dimension",
            });
        }
        let gate_key = format!("lingqu/memory/engram/{}/gate_weight", req.state_id);
        let gate = publish_hot_tensor(
            object_service,
            gate_key,
            f32_vec_to_le_bytes(&req.gate_values),
            TensorDType::F32,
            vec![hidden_size],
            req.producer_entity,
            req.owner_entity,
            req.now_us,
        )?;
        self.build_engram_state(
            req.state_id,
            &req.hot_memory_state_id,
            Some(gate),
            req.now_us,
        )
    }

    pub fn materialize_engram_state_from_block(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
        object_service: &mut LingquObjectServiceStub,
        req: EngramStateMaterializeFromBlockReq,
    ) -> MemoryResult<EngramStateObject> {
        req.gate_weight_ref.validate("gate_weight_ref")?;
        let gate_values = read_f32_payload(durable_store, &req.gate_weight_ref)?;
        self.materialize_engram_state(
            object_service,
            EngramStateMaterializeReq {
                state_id: req.state_id,
                hot_memory_state_id: req.hot_memory_state_id,
                gate_values,
                owner_entity: req.owner_entity,
                producer_entity: req.producer_entity,
                now_us: req.now_us,
            },
        )
    }

    pub fn record(&self, record_id: &str) -> Option<&MemoryRecord> {
        self.records.get(record_id)
    }

    pub fn query_result(&self, result_id: &str) -> Option<&QueryResult> {
        self.query_results.get(result_id)
    }
}

fn publish_hot_tensor(
    object_service: &mut LingquObjectServiceStub,
    key: String,
    payload: Vec<u8>,
    dtype: TensorDType,
    shape: Vec<u64>,
    producer_entity: u64,
    owner_entity: u64,
    now_us: u64,
) -> MemoryResult<HotTensorObjectRef> {
    if payload.is_empty() {
        return Err(LingquMemoryError::MissingField("payload"));
    }
    let checksum = checksum64(&payload);
    let bytes = payload.len() as u64;
    object_service
        .submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: key.clone(),
                kind: LingquObjectKind::RuntimeTensor,
                producer_entity,
                owner_entity: Some(owner_entity),
                expected_version: None,
                metadata: LingquObjectMetadata {
                    bytes,
                    checksum,
                    dtype: Some(dtype),
                    shape,
                    layout: None,
                    expires_at_us: None,
                },
                placements: vec![LingquPayloadPlacement {
                    backend: LingquPayloadBackend::ObmmShmem,
                    storage_ref: format!("{key}/payload"),
                    segment: None,
                    offset: 0,
                    bytes,
                    checksum,
                    locality: LingquObjectLocality::DomainShared(0),
                }],
                payload_bytes: payload,
            },
            now_us,
        )
        .map_err(|err| LingquMemoryError::ObjectPublishFailed(err.to_string()))?;
    let record = object_service
        .latest_record(&key)
        .ok_or_else(|| LingquMemoryError::ObjectPublishFailed(key.clone()))?;
    let placement = record
        .placements
        .iter()
        .find(|placement| placement.backend == LingquPayloadBackend::ObmmShmem)
        .ok_or_else(|| LingquMemoryError::NonObmmHotPlacement(key.clone()))?;
    Ok(HotTensorObjectRef {
        object_key: key,
        version: record.version,
        backend: HotObjectBackend::ObmmShmem,
        storage_ref: placement.storage_ref.clone(),
        segment: placement.segment,
        offset: placement.offset,
        bytes: placement.bytes,
        checksum: placement.checksum,
        dtype,
        shape: record.shape.clone(),
    })
}

fn validate_hot_ref(hot_ref: &HotTensorObjectRef) -> MemoryResult<()> {
    required_str(&hot_ref.object_key, "hot_ref.object_key")?;
    required_str(&hot_ref.storage_ref, "hot_ref.storage_ref")?;
    nonzero(hot_ref.version, "hot_ref.version")?;
    nonzero(hot_ref.bytes, "hot_ref.bytes")?;
    nonzero(hot_ref.checksum, "hot_ref.checksum")?;
    if hot_ref.backend != HotObjectBackend::ObmmShmem {
        return Err(LingquMemoryError::NonObmmHotPlacement(
            hot_ref.object_key.clone(),
        ));
    }
    Ok(())
}

fn validate_table_shape(shape: &[u64], value_count: usize) -> MemoryResult<()> {
    require_nonempty(shape, "table_shape")?;
    let mut elements = 1u64;
    for dim in shape {
        nonzero(*dim, "table_shape")?;
        elements = elements
            .checked_mul(*dim)
            .ok_or(LingquMemoryError::InvalidValue {
                field: "table_shape",
                reason: "shape element count overflow",
            })?;
    }
    if elements != value_count as u64 {
        return Err(LingquMemoryError::InvalidValue {
            field: "table_values",
            reason: "table value count must match shape",
        });
    }
    Ok(())
}

fn expect_success(
    op_name: &'static str,
    op_id: u64,
    events: Vec<sim_core::CompletionEvent>,
) -> MemoryResult<()> {
    let event = events
        .into_iter()
        .find(|event| event.op_id == op_id)
        .ok_or_else(|| {
            LingquMemoryError::DurableServiceFailed(format!("{op_name}:missing_completion"))
        })?;
    match event.status {
        CompletionStatus::Success => Ok(()),
        CompletionStatus::RetryableFailure { code } | CompletionStatus::FatalFailure { code } => {
            Err(LingquMemoryError::DurableServiceFailed(format!(
                "{op_name}:{code}"
            )))
        }
    }
}

fn record_selectable(
    record: &MemoryRecord,
    query: &MemoryQuery,
    scope_set: &HashSet<MemoryScope>,
    visibility_set: &HashSet<MemoryVisibility>,
) -> bool {
    if record.state != MemoryRecordState::Committed {
        return false;
    }
    if record.trust_level < query.min_trust || record.confidence < query.min_confidence {
        return false;
    }
    if !scope_set.is_empty() && !scope_set.contains(&record.scope) {
        return false;
    }
    if !visibility_set.is_empty() && !visibility_set.contains(&record.visibility) {
        return false;
    }
    true
}

fn read_f32_payload(
    durable_store: &mut LingquMemoryDurableStore,
    payload_ref: &LingquBlockPayloadRef,
) -> MemoryResult<Vec<f32>> {
    let bytes = durable_store.read_block_payload(payload_ref)?;
    f32_values_from_le_bytes(&bytes)
}

fn read_segment_bytes(
    durable_store: &mut LingquMemoryDurableStore,
    segment: &EmbeddingSegment,
) -> MemoryResult<Vec<u8>> {
    let mut bytes = Vec::new();
    for payload_ref in &segment.vector_block_refs {
        bytes.extend_from_slice(&durable_store.read_block_payload(payload_ref)?);
    }
    Ok(bytes)
}

fn segment_row_f32_values(
    segment: &EmbeddingSegment,
    segment_bytes: &[u8],
    row: u32,
) -> MemoryResult<Vec<f32>> {
    let row_start = u64::from(row)
        .checked_mul(u64::from(segment.row_stride_bytes))
        .ok_or(LingquMemoryError::InvalidValue {
            field: "embedding_segment.row_stride_bytes",
            reason: "row byte offset overflow",
        })? as usize;
    let row_bytes = usize::try_from(segment.dims)
        .ok()
        .and_then(|dims| dims.checked_mul(4))
        .ok_or(LingquMemoryError::InvalidValue {
            field: "embedding_segment.dims",
            reason: "row byte length overflow",
        })?;
    let row_end = row_start
        .checked_add(row_bytes)
        .ok_or(LingquMemoryError::InvalidValue {
            field: "embedding_segment.row_stride_bytes",
            reason: "row byte range overflow",
        })?;
    if row_end > segment_bytes.len() {
        return Err(LingquMemoryError::InvalidValue {
            field: "embedding_segment.vector_block_refs",
            reason: "segment vector page is shorter than row map requires",
        });
    }
    f32_values_from_le_bytes(&segment_bytes[row_start..row_end])
}

fn f32_values_from_le_bytes(bytes: &[u8]) -> MemoryResult<Vec<f32>> {
    if bytes.len() % 4 != 0 {
        return Err(LingquMemoryError::InvalidValue {
            field: "f32_payload",
            reason: "payload length must be a multiple of 4",
        });
    }
    let mut values = Vec::with_capacity(bytes.len() / 4);
    for chunk in bytes.chunks_exact(4) {
        values.push(f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]));
    }
    Ok(values)
}

fn dot_product(left: &[f32], right: &[f32]) -> f32 {
    left.iter()
        .zip(right.iter())
        .map(|(left, right)| left * right)
        .sum()
}

fn deterministic_score(record: &MemoryRecord, row: u32) -> f32 {
    let trust = match record.trust_level {
        MemoryTrustLevel::SystemVerified => 1.0,
        MemoryTrustLevel::UserConfirmed => 0.9,
        MemoryTrustLevel::ModelDerived => 0.7,
        MemoryTrustLevel::Untrusted => 0.2,
    };
    let row_penalty = 1.0 / (1.0 + row as f32);
    record.confidence.mul_add(0.75, trust * 0.25) * row_penalty
}

fn required_str(value: &str, field: &'static str) -> MemoryResult<()> {
    if value.trim().is_empty() {
        return Err(LingquMemoryError::MissingField(field));
    }
    Ok(())
}

fn require_nonempty<T>(value: &[T], field: &'static str) -> MemoryResult<()> {
    if value.is_empty() {
        return Err(LingquMemoryError::MissingField(field));
    }
    Ok(())
}

fn nonzero(value: u64, field: &'static str) -> MemoryResult<()> {
    if value == 0 {
        return Err(LingquMemoryError::InvalidValue {
            field,
            reason: "value must be non-zero",
        });
    }
    Ok(())
}

fn monotonic_time(created_at_us: u64, updated_at_us: u64) -> MemoryResult<()> {
    if updated_at_us < created_at_us {
        return Err(LingquMemoryError::InvalidValue {
            field: "updated_at_us",
            reason: "updated_at_us must be >= created_at_us",
        });
    }
    Ok(())
}

fn query_result_audit_checksum(
    result_id: &str,
    query_id: &str,
    vector_index_ids: &[String],
    matches: &[QueryMatch],
    selected_record_ids: &[String],
    selected_chunk_ids: &[String],
    embedding_segment_versions: &[QuerySegmentVersion],
    evidence_refs: &[String],
    version: u64,
    created_at_us: u64,
) -> u64 {
    fn push_str(bytes: &mut Vec<u8>, value: &str) {
        bytes.extend_from_slice(&(value.len() as u64).to_le_bytes());
        bytes.extend_from_slice(value.as_bytes());
    }

    let mut bytes = Vec::new();
    push_str(&mut bytes, result_id);
    push_str(&mut bytes, query_id);
    bytes.extend_from_slice(&version.to_le_bytes());
    bytes.extend_from_slice(&created_at_us.to_le_bytes());
    for id in vector_index_ids {
        push_str(&mut bytes, id);
    }
    for query_match in matches {
        push_str(&mut bytes, &query_match.vector_index_id);
        push_str(&mut bytes, &query_match.record_id);
        push_str(&mut bytes, &query_match.chunk_id);
        push_str(&mut bytes, &query_match.segment_id);
        bytes.extend_from_slice(&query_match.row.to_le_bytes());
        bytes.extend_from_slice(&query_match.score.to_bits().to_le_bytes());
        bytes.extend_from_slice(&(query_match.trust_level as u8).to_le_bytes());
        bytes.extend_from_slice(&query_match.confidence.to_bits().to_le_bytes());
    }
    for id in selected_record_ids {
        push_str(&mut bytes, id);
    }
    for id in selected_chunk_ids {
        push_str(&mut bytes, id);
    }
    for segment in embedding_segment_versions {
        push_str(&mut bytes, &segment.segment_id);
        bytes.extend_from_slice(&segment.version.to_le_bytes());
        bytes.extend_from_slice(&segment.checksum.to_le_bytes());
    }
    for evidence_ref in evidence_refs {
        push_str(&mut bytes, evidence_ref);
    }
    checksum64(&bytes)
}

fn query_result_dfs_path(result_id: &str) -> MemoryResult<LingquDfsPath> {
    required_str(result_id, "query_result_id")?;
    let mut escaped = String::with_capacity(result_id.len());
    for ch in result_id.chars() {
        if ch.is_ascii_alphanumeric() || matches!(ch, '-' | '_' | '.') {
            escaped.push(ch);
        } else {
            escaped.push('_');
        }
    }
    Ok(LingquDfsPath::new(format!(
        "/lingqu/memory/query-results/{escaped}.json"
    )))
}

fn checksum64(bytes: &[u8]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for byte in bytes {
        acc ^= u64::from(*byte);
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn f32_vec_to_le_bytes(values: &[f32]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(values.len() * 4);
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    bytes
}

fn u32_vec_to_le_bytes(values: &[u32]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(values.len() * 4);
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    bytes
}

#[cfg(test)]
mod tests {
    use super::*;
    use sim_services::object::{LingquObjectServiceProfile, LingquObjectVersionSelector};

    #[test]
    fn record_validation_requires_evidence_for_high_trust_derived_memory() {
        let mut record = sample_record("record/0", "chunk/0");
        record.source_kind = MemorySourceKind::Derived;
        record.trust_level = MemoryTrustLevel::SystemVerified;
        record.evidence_refs.clear();

        assert!(matches!(
            record.validate(),
            Err(LingquMemoryError::InvalidValue {
                field: "evidence_refs",
                ..
            })
        ));
    }

    #[test]
    fn embedding_segment_rejects_per_vector_block_refs() {
        let mut segment = sample_embedding_segment("segment/0", "chunk/0");
        segment.vector_block_refs.clear();

        assert_eq!(
            segment.validate(),
            Err(LingquMemoryError::MissingField("vector_block_refs"))
        );
    }

    #[test]
    fn query_filters_quarantined_and_low_trust_memory() {
        let mut service = populated_service();
        let mut quarantined = sample_record("record/q", "chunk/q");
        quarantined.state = MemoryRecordState::Quarantined;
        service
            .ingest_record(quarantined, vec![sample_chunk("chunk/q", "record/q")])
            .unwrap();
        service
            .register_embedding_segment(sample_embedding_segment("segment/q", "chunk/q"))
            .unwrap();
        let result = service
            .query_memory(
                MemoryQuery {
                    query_id: "q0".to_string(),
                    corpus_ids: vec!["corpus/0".to_string()],
                    scope_filter: vec![MemoryScope::Project],
                    visibility_filter: vec![MemoryVisibility::ProjectShared],
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.5,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 8,
                    query_embedding_ref: None,
                },
                100,
            )
            .unwrap();

        assert_eq!(result.matches.len(), 1);
        assert_eq!(result.matches[0].chunk_id, "chunk/0");
    }

    #[test]
    fn catalog_snapshot_round_trips_through_json_bytes() {
        let service = populated_service();
        let snapshot = service
            .export_catalog_snapshot("corpus/0")
            .expect("export catalog snapshot");
        let json_bytes = snapshot.to_json_bytes().expect("snapshot json");
        let decoded =
            MemoryCatalogSnapshot::from_json_bytes(&json_bytes).expect("decode snapshot json");

        let mut restored = LingquMemoryService::new();
        restored
            .import_catalog_snapshot(decoded)
            .expect("import catalog snapshot");
        let result = restored
            .query_memory(
                MemoryQuery {
                    query_id: "q0".to_string(),
                    corpus_ids: vec!["corpus/0".to_string()],
                    scope_filter: Vec::new(),
                    visibility_filter: Vec::new(),
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.0,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 1,
                    query_embedding_ref: None,
                },
                100,
            )
            .unwrap();

        assert_eq!(result.matches.len(), 1);
        assert_eq!(result.matches[0].chunk_id, "chunk/0");
    }

    #[test]
    fn durable_store_persists_catalog_snapshot_through_dfs() {
        let service = populated_service();
        let snapshot = service
            .export_catalog_snapshot("corpus/0")
            .expect("export catalog snapshot");
        let mut durable = LingquMemoryDurableStore::new();

        let path = durable
            .persist_catalog_snapshot(&snapshot)
            .expect("persist catalog snapshot");
        let restored = durable
            .load_catalog_snapshot(&path)
            .expect("load catalog snapshot");
        let stats = durable.stats();

        assert_eq!(restored.catalog.catalog_id, "corpus/0");
        assert_eq!(restored.records.len(), 1);
        assert_eq!(stats.dfs_catalog_writes, 1);
        assert_eq!(stats.dfs_catalog_reads, 1);
        assert!(stats.dfs_bytes_written > 0);
        assert_eq!(stats.dfs_bytes_written, stats.dfs_bytes_read);
    }

    #[test]
    fn memory_service_rebuilds_catalog_from_dfs_snapshot() {
        let service = populated_service();
        let mut durable = LingquMemoryDurableStore::new();
        let path = service
            .persist_catalog_to_dfs(&mut durable, "corpus/0")
            .expect("persist catalog through service");
        let mut restored = LingquMemoryService::new();

        let catalog = restored
            .rebuild_catalog_from_dfs(&mut durable, &path)
            .expect("rebuild catalog through service");
        let result = restored
            .query_memory(
                MemoryQuery {
                    query_id: "q/rebuilt".to_string(),
                    corpus_ids: vec![catalog.catalog_id],
                    scope_filter: Vec::new(),
                    visibility_filter: Vec::new(),
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.0,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 1,
                    query_embedding_ref: None,
                },
                100,
            )
            .unwrap();

        assert_eq!(result.matches.len(), 1);
        assert_eq!(result.matches[0].record_id, "record/0");
    }

    #[test]
    fn durable_store_round_trips_block_payload_and_checks_checksum() {
        let mut durable = LingquMemoryDurableStore::new();
        let payload_ref = durable
            .write_block_payload("block/chunk/0", b"persistent chunk".to_vec())
            .expect("write block payload");

        let bytes = durable
            .read_block_payload(&payload_ref)
            .expect("read block payload");
        let mut bad_ref = payload_ref.clone();
        bad_ref.checksum ^= 1;
        let err = durable
            .read_block_payload(&bad_ref)
            .expect_err("checksum mismatch should fail");

        assert_eq!(bytes, b"persistent chunk");
        assert!(matches!(
            err,
            LingquMemoryError::PayloadChecksumMismatch { .. }
        ));
        assert_eq!(durable.stats().block_payload_writes, 1);
        assert_eq!(durable.stats().block_payload_reads, 2);
    }

    #[test]
    fn flat_query_ranks_block_backed_embedding_vectors() {
        let mut service = LingquMemoryService::new();
        service
            .publish_catalog(MemoryCorpusCatalog {
                catalog_id: "corpus/flat".to_string(),
                namespace: "project/default".to_string(),
                dfs_path: LingquDfsPath::new("/lingqu/memory/corpus/flat/catalog.json"),
                version: 1,
                record_ids: vec!["record/a".to_string(), "record/b".to_string()],
                vector_index_ids: vec!["index/flat".to_string()],
                created_at_us: 1,
                updated_at_us: 1,
            })
            .unwrap();
        service
            .ingest_record(
                sample_record("record/a", "chunk/a"),
                vec![sample_chunk("chunk/a", "record/a")],
            )
            .unwrap();
        service
            .ingest_record(
                sample_record("record/b", "chunk/b"),
                vec![sample_chunk("chunk/b", "record/b")],
            )
            .unwrap();

        let mut durable = LingquMemoryDurableStore::new();
        let segment_ref = durable
            .write_block_payload(
                "block/embed/flat",
                f32_vec_to_le_bytes(&[1.0, 0.0, 0.0, 1.0]),
            )
            .unwrap();
        let query_ref = durable
            .write_block_payload("block/query/flat", f32_vec_to_le_bytes(&[0.0, 1.0]))
            .unwrap();
        service
            .register_embedding_segment(EmbeddingSegment {
                segment_id: "segment/flat".to_string(),
                model_version: "embed/v1".to_string(),
                dims: 2,
                row_count: 2,
                row_stride_bytes: 8,
                dtype: TensorDType::F32,
                vector_block_refs: vec![segment_ref],
                row_map: vec![
                    EmbeddingRow {
                        chunk_id: "chunk/a".to_string(),
                        row: 0,
                    },
                    EmbeddingRow {
                        chunk_id: "chunk/b".to_string(),
                        row: 1,
                    },
                ],
                checksum: 0x5151,
                version: 1,
            })
            .unwrap();
        service
            .register_vector_index(VectorIndexObject {
                index_id: "index/flat".to_string(),
                corpus_id: "corpus/flat".to_string(),
                kind: VectorIndexKind::Flat,
                embedding_model_version: "embed/v1".to_string(),
                segment_ids: vec!["segment/flat".to_string()],
                manifest_path: LingquDfsPath::new("/lingqu/memory/corpus/flat/index.json"),
                created_at_us: 2,
                updated_at_us: 2,
                version: 1,
            })
            .unwrap();

        let result = service
            .query_memory_flat(
                &mut durable,
                MemoryQuery {
                    query_id: "query/flat".to_string(),
                    corpus_ids: vec!["corpus/flat".to_string()],
                    scope_filter: Vec::new(),
                    visibility_filter: Vec::new(),
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.0,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 1,
                    query_embedding_ref: Some(query_ref),
                },
                100,
            )
            .unwrap();

        assert_eq!(result.matches.len(), 1);
        assert_eq!(result.version, 1);
        assert_ne!(result.checksum, 0);
        assert_eq!(result.vector_index_ids, ["index/flat"]);
        assert_eq!(result.selected_record_ids, ["record/b"]);
        assert_eq!(result.selected_chunk_ids, ["chunk/b"]);
        assert_eq!(result.evidence_refs, ["tool://importer/0"]);
        assert_eq!(
            result.embedding_segment_versions,
            [QuerySegmentVersion {
                segment_id: "segment/flat".to_string(),
                version: 1,
                checksum: 0x5151,
            }]
        );
        assert_eq!(result.matches[0].vector_index_id, "index/flat");
        assert_eq!(result.matches[0].chunk_id, "chunk/b");
        assert_eq!(result.matches[0].score, 1.0);
        assert_eq!(durable.stats().block_payload_reads, 2);

        let result_path = durable
            .persist_query_result(&result)
            .expect("persist query result");
        let restored = durable
            .load_query_result(&result_path)
            .expect("load query result");
        assert_eq!(restored, result);
        assert!(result_path
            .path
            .starts_with("/lingqu/memory/query-results/query-result_query_flat"));
    }

    #[test]
    fn flat_query_materialization_publishes_selected_vectors_to_obmm() {
        let mut service = LingquMemoryService::new();
        service
            .publish_catalog(MemoryCorpusCatalog {
                catalog_id: "corpus/flat".to_string(),
                namespace: "project/default".to_string(),
                dfs_path: LingquDfsPath::new("/lingqu/memory/corpus/flat/catalog.json"),
                version: 1,
                record_ids: vec!["record/a".to_string(), "record/b".to_string()],
                vector_index_ids: vec!["index/flat".to_string()],
                created_at_us: 1,
                updated_at_us: 1,
            })
            .unwrap();
        service
            .ingest_record(
                sample_record("record/a", "chunk/a"),
                vec![sample_chunk("chunk/a", "record/a")],
            )
            .unwrap();
        service
            .ingest_record(
                sample_record("record/b", "chunk/b"),
                vec![sample_chunk("chunk/b", "record/b")],
            )
            .unwrap();

        let mut durable = LingquMemoryDurableStore::new();
        let segment_ref = durable
            .write_block_payload(
                "block/embed/flat",
                f32_vec_to_le_bytes(&[1.0, 0.0, 0.0, 1.0]),
            )
            .unwrap();
        let query_ref = durable
            .write_block_payload("block/query/flat", f32_vec_to_le_bytes(&[1.0, 0.0]))
            .unwrap();
        service
            .register_embedding_segment(EmbeddingSegment {
                segment_id: "segment/flat".to_string(),
                model_version: "embed/v1".to_string(),
                dims: 2,
                row_count: 2,
                row_stride_bytes: 8,
                dtype: TensorDType::F32,
                vector_block_refs: vec![segment_ref],
                row_map: vec![
                    EmbeddingRow {
                        chunk_id: "chunk/a".to_string(),
                        row: 0,
                    },
                    EmbeddingRow {
                        chunk_id: "chunk/b".to_string(),
                        row: 1,
                    },
                ],
                checksum: 0x5151,
                version: 1,
            })
            .unwrap();
        service
            .register_vector_index(VectorIndexObject {
                index_id: "index/flat".to_string(),
                corpus_id: "corpus/flat".to_string(),
                kind: VectorIndexKind::Flat,
                embedding_model_version: "embed/v1".to_string(),
                segment_ids: vec!["segment/flat".to_string()],
                manifest_path: LingquDfsPath::new("/lingqu/memory/corpus/flat/index.json"),
                created_at_us: 2,
                updated_at_us: 2,
                version: 1,
            })
            .unwrap();
        let result = service
            .query_memory_flat(
                &mut durable,
                MemoryQuery {
                    query_id: "query/flat".to_string(),
                    corpus_ids: vec!["corpus/flat".to_string()],
                    scope_filter: Vec::new(),
                    visibility_filter: Vec::new(),
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.0,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 2,
                    query_embedding_ref: Some(query_ref),
                },
                100,
            )
            .unwrap();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());

        let hot_state = service
            .materialize_hot_state_from_query(
                &mut durable,
                &mut object_service,
                HotMemoryMaterializeFromQueryReq {
                    state_id: "hot/flat".to_string(),
                    query_result_id: result.result_id,
                    owner_entity: 1,
                    producer_entity: 0,
                    now_us: 200,
                },
            )
            .unwrap();

        assert_eq!(
            hot_state
                .query_result_manifest_ref
                .as_ref()
                .expect("query result manifest")
                .path,
            "/lingqu/memory/query-results/query-result_query_flat.json"
        );
        assert_eq!(hot_state.selected_chunk_ids, ["chunk/a", "chunk/b"]);
        assert_eq!(hot_state.table.shape, vec![2, 2]);
        assert_eq!(hot_state.indices.shape, vec![2]);
        assert_eq!(hot_state.scores.shape, vec![2]);
        let table_bytes = object_service
            .get_copy(
                &hot_state.table.object_key,
                LingquObjectVersionSelector::LatestCommitted,
            )
            .expect("hot table payload");
        let table_values = f32_values_from_le_bytes(&table_bytes).unwrap();
        assert_eq!(table_values, vec![1.0, 0.0, 0.0, 1.0]);
        let stats = durable.stats();
        assert_eq!(stats.block_payload_reads, 3);
    }

    #[test]
    fn durable_store_reports_missing_dfs_and_block_refs() {
        let mut durable = LingquMemoryDurableStore::new();
        let missing_path = LingquDfsPath::new("/lingqu/memory/missing/catalog.json");
        let missing_block = LingquBlockPayloadRef::new("block/missing", 0, 4, 0x1234);

        assert_eq!(
            durable.load_catalog_snapshot(&missing_path),
            Err(LingquMemoryError::MissingDfsPath(
                "/lingqu/memory/missing/catalog.json".to_string()
            ))
        );
        assert_eq!(
            durable.read_block_payload(&missing_block),
            Err(LingquMemoryError::MissingBlockPayload(
                "block/missing".to_string()
            ))
        );
    }

    #[test]
    fn hot_state_materialization_publishes_obmm_objects() {
        let mut service = populated_service();
        let result = service
            .query_memory(
                MemoryQuery {
                    query_id: "q0".to_string(),
                    corpus_ids: vec!["corpus/0".to_string()],
                    scope_filter: vec![MemoryScope::Project],
                    visibility_filter: vec![MemoryVisibility::ProjectShared],
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.5,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 1,
                    query_embedding_ref: None,
                },
                100,
            )
            .unwrap();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());

        let hot_state = service
            .materialize_hot_state(
                &mut object_service,
                HotMemoryMaterializeReq {
                    state_id: "hot/0".to_string(),
                    query_result_id: result.result_id,
                    query_result_manifest_ref: None,
                    table_shape: vec![1, 4],
                    table_values: vec![0.1, 0.2, 0.3, 0.4],
                    indices: vec![0],
                    owner_entity: 1,
                    producer_entity: 0,
                    now_us: 200,
                },
            )
            .unwrap();

        assert_eq!(hot_state.table.backend, HotObjectBackend::ObmmShmem);
        let table_record = object_service
            .latest_record(&hot_state.table.object_key)
            .expect("hot table object");
        assert_eq!(
            table_record.placements[0].backend,
            LingquPayloadBackend::ObmmShmem
        );
        let table_bytes = object_service
            .get_copy(
                &hot_state.table.object_key,
                LingquObjectVersionSelector::LatestCommitted,
            )
            .expect("hot table payload");
        assert_eq!(table_bytes.len(), 16);
    }

    #[test]
    fn engram_state_reuses_hot_state_object_refs() {
        let mut service = populated_service();
        let result = service
            .query_memory(
                MemoryQuery {
                    query_id: "q0".to_string(),
                    corpus_ids: vec!["corpus/0".to_string()],
                    scope_filter: Vec::new(),
                    visibility_filter: Vec::new(),
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.0,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 1,
                    query_embedding_ref: None,
                },
                100,
            )
            .unwrap();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        let hot_state = service
            .materialize_hot_state(
                &mut object_service,
                HotMemoryMaterializeReq {
                    state_id: "hot/0".to_string(),
                    query_result_id: result.result_id,
                    query_result_manifest_ref: None,
                    table_shape: vec![1, 2],
                    table_values: vec![0.1, 0.2],
                    indices: vec![0],
                    owner_entity: 0,
                    producer_entity: 0,
                    now_us: 200,
                },
            )
            .unwrap();

        let engram = service
            .build_engram_state("engram/0", &hot_state.state_id, None, 300)
            .unwrap();

        assert_eq!(
            engram.query_result_manifest_ref,
            hot_state.query_result_manifest_ref
        );
        assert_eq!(engram.table.object_key, hot_state.table.object_key);
        assert_eq!(engram.indices.object_key, hot_state.indices.object_key);
    }

    #[test]
    fn engram_state_materialization_publishes_gate_weight_object() {
        let mut service = populated_service();
        let result = service
            .query_memory(
                MemoryQuery {
                    query_id: "q0".to_string(),
                    corpus_ids: vec!["corpus/0".to_string()],
                    scope_filter: Vec::new(),
                    visibility_filter: Vec::new(),
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.0,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 1,
                    query_embedding_ref: None,
                },
                100,
            )
            .unwrap();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        let hot_state = service
            .materialize_hot_state(
                &mut object_service,
                HotMemoryMaterializeReq {
                    state_id: "hot/0".to_string(),
                    query_result_id: result.result_id,
                    query_result_manifest_ref: None,
                    table_shape: vec![1, 4],
                    table_values: vec![0.1, 0.2, 0.3, 0.4],
                    indices: vec![0],
                    owner_entity: 0,
                    producer_entity: 0,
                    now_us: 200,
                },
            )
            .unwrap();

        let engram = service
            .materialize_engram_state(
                &mut object_service,
                EngramStateMaterializeReq {
                    state_id: "engram/0".to_string(),
                    hot_memory_state_id: hot_state.state_id.clone(),
                    gate_values: vec![0.5, 0.6, 0.7, 0.8],
                    owner_entity: 0,
                    producer_entity: 0,
                    now_us: 300,
                },
            )
            .unwrap();

        let gate = engram.gate.expect("gate object ref");
        assert_eq!(engram.table.object_key, hot_state.table.object_key);
        assert_eq!(engram.indices.object_key, hot_state.indices.object_key);
        assert_eq!(gate.backend, HotObjectBackend::ObmmShmem);
        assert_eq!(gate.shape, vec![4]);
        let gate_bytes = object_service
            .get_copy(
                &gate.object_key,
                LingquObjectVersionSelector::LatestCommitted,
            )
            .expect("gate payload");
        let gate_values = f32_values_from_le_bytes(&gate_bytes).unwrap();
        assert_eq!(gate_values, vec![0.5, 0.6, 0.7, 0.8]);
    }

    #[test]
    fn engram_state_materialization_reads_gate_weight_from_block() {
        let mut service = populated_service();
        let result = service
            .query_memory(
                MemoryQuery {
                    query_id: "q0".to_string(),
                    corpus_ids: vec!["corpus/0".to_string()],
                    scope_filter: Vec::new(),
                    visibility_filter: Vec::new(),
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.0,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 1,
                    query_embedding_ref: None,
                },
                100,
            )
            .unwrap();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        let hot_state = service
            .materialize_hot_state(
                &mut object_service,
                HotMemoryMaterializeReq {
                    state_id: "hot/0".to_string(),
                    query_result_id: result.result_id,
                    query_result_manifest_ref: None,
                    table_shape: vec![1, 4],
                    table_values: vec![0.1, 0.2, 0.3, 0.4],
                    indices: vec![0],
                    owner_entity: 0,
                    producer_entity: 0,
                    now_us: 200,
                },
            )
            .unwrap();
        let mut durable = LingquMemoryDurableStore::new();
        let gate_ref = durable
            .write_block_payload(
                "block/engram/gate/0",
                f32_vec_to_le_bytes(&[1.0, 2.0, 3.0, 4.0]),
            )
            .unwrap();

        let engram = service
            .materialize_engram_state_from_block(
                &mut durable,
                &mut object_service,
                EngramStateMaterializeFromBlockReq {
                    state_id: "engram/0".to_string(),
                    hot_memory_state_id: hot_state.state_id.clone(),
                    gate_weight_ref: gate_ref,
                    owner_entity: 0,
                    producer_entity: 0,
                    now_us: 300,
                },
            )
            .unwrap();

        let gate = engram.gate.expect("gate object ref");
        let gate_bytes = object_service
            .get_copy(
                &gate.object_key,
                LingquObjectVersionSelector::LatestCommitted,
            )
            .expect("gate payload");
        assert_eq!(
            f32_values_from_le_bytes(&gate_bytes).unwrap(),
            vec![1.0, 2.0, 3.0, 4.0]
        );
        let stats = durable.stats();
        assert_eq!(stats.block_payload_writes, 1);
        assert_eq!(stats.block_payload_reads, 1);
    }

    fn populated_service() -> LingquMemoryService {
        let mut service = LingquMemoryService::new();
        service
            .publish_catalog(MemoryCorpusCatalog {
                catalog_id: "corpus/0".to_string(),
                namespace: "project/default".to_string(),
                dfs_path: LingquDfsPath::new("/lingqu/memory/corpus/0/catalog.json"),
                version: 1,
                record_ids: vec!["record/0".to_string()],
                vector_index_ids: vec!["index/0".to_string()],
                created_at_us: 1,
                updated_at_us: 1,
            })
            .unwrap();
        service
            .ingest_record(
                sample_record("record/0", "chunk/0"),
                vec![sample_chunk("chunk/0", "record/0")],
            )
            .unwrap();
        service
            .register_embedding_segment(sample_embedding_segment("segment/0", "chunk/0"))
            .unwrap();
        service
            .register_vector_index(VectorIndexObject {
                index_id: "index/0".to_string(),
                corpus_id: "corpus/0".to_string(),
                kind: VectorIndexKind::Flat,
                embedding_model_version: "embed/v1".to_string(),
                segment_ids: vec!["segment/0".to_string()],
                manifest_path: LingquDfsPath::new("/lingqu/memory/corpus/0/index/flat.json"),
                created_at_us: 2,
                updated_at_us: 2,
                version: 1,
            })
            .unwrap();
        service
    }

    fn sample_record(record_id: &str, chunk_id: &str) -> MemoryRecord {
        MemoryRecord {
            record_id: record_id.to_string(),
            corpus_id: "corpus/0".to_string(),
            scope: MemoryScope::Project,
            visibility: MemoryVisibility::ProjectShared,
            source_kind: MemorySourceKind::UserProvided,
            source_uri: "dfs://lingqu/memory/source.md".to_string(),
            source_checksum: 0x1001,
            content_type: MemoryContentType::Markdown,
            token_count: 32,
            trust_level: MemoryTrustLevel::UserConfirmed,
            confidence: 0.95,
            retention_policy: MemoryRetentionPolicy::Durable,
            security_label: MemorySecurityLabel::Internal,
            pii_state: MemoryPiiState::None,
            chunk_refs: vec![chunk_id.to_string()],
            embedding_model_versions: vec!["embed/v1".to_string()],
            evidence_refs: vec!["tool://importer/0".to_string()],
            created_at_us: 1,
            updated_at_us: 1,
            expires_at_us: None,
            version: 1,
            state: MemoryRecordState::Committed,
        }
    }

    fn sample_chunk(chunk_id: &str, record_id: &str) -> MemoryChunk {
        MemoryChunk {
            chunk_id: chunk_id.to_string(),
            record_id: record_id.to_string(),
            ordinal: 0,
            text_block_ref: LingquBlockPayloadRef::new("block/text/0", 0, 128, 0x2002),
            token_start: 0,
            token_count: 32,
            checksum: 0x3003,
        }
    }

    fn sample_embedding_segment(segment_id: &str, chunk_id: &str) -> EmbeddingSegment {
        EmbeddingSegment {
            segment_id: segment_id.to_string(),
            model_version: "embed/v1".to_string(),
            dims: 4,
            row_count: 1,
            row_stride_bytes: 16,
            dtype: TensorDType::F32,
            vector_block_refs: vec![LingquBlockPayloadRef::new("block/embed/0", 0, 16, 0x4004)],
            row_map: vec![EmbeddingRow {
                chunk_id: chunk_id.to_string(),
                row: 0,
            }],
            checksum: 0x5005,
            version: 1,
        }
    }
}
