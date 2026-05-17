//! Product-shaped Lingqu Memory Service core.
//!
//! The crate owns the durable memory model and the hot-state materialization
//! contract. It intentionally keeps only one service layer in process; Host
//! and Guest deployments can wrap the same API with their own transport.

use std::cmp::Ordering;
use std::collections::{BTreeSet, HashMap, HashSet};

use serde::{Deserialize, Serialize};
use sim_core::{BlockHash, SegmentHandle, TensorDType};
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
    pub chunk_id: String,
    pub record_id: String,
    pub segment_id: String,
    pub row: u32,
    pub score: f32,
    pub trust_level: MemoryTrustLevel,
    pub confidence: f32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct QueryResult {
    pub result_id: String,
    pub query_id: String,
    pub matches: Vec<QueryMatch>,
    pub created_at_us: u64,
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
    pub table: HotTensorObjectRef,
    pub indices: HotTensorObjectRef,
    pub gate: Option<HotTensorObjectRef>,
    pub created_at_us: u64,
}

#[derive(Debug, Clone, PartialEq)]
pub struct HotMemoryMaterializeReq {
    pub state_id: String,
    pub query_result_id: String,
    pub table_shape: Vec<u64>,
    pub table_values: Vec<f32>,
    pub indices: Vec<u32>,
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

        let result = QueryResult {
            result_id: format!("query-result/{}", query.query_id),
            query_id: query.query_id.clone(),
            matches,
            created_at_us: now_us,
        };
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
            table: hot_state.table.clone(),
            indices: hot_state.indices.clone(),
            gate,
            created_at_us,
        };
        self.engram_states
            .insert(engram.state_id.clone(), engram.clone());
        Ok(engram)
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

        assert_eq!(engram.table.object_key, hot_state.table.object_key);
        assert_eq!(engram.indices.object_key, hot_state.indices.object_key);
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
        }
    }
}
