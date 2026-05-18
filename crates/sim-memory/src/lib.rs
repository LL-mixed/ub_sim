//! Product-shaped Lingqu Memory Service core.
//!
//! The crate owns the durable memory model and the hot-state materialization
//! contract. It intentionally keeps only one service layer in process; Host
//! and Guest deployments can wrap the same API with their own transport.

use std::cmp::Ordering;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};

use serde::{Deserialize, Serialize};
use sim_core::{BlockHash, SegmentHandle, TensorDType};
use sim_services::block::BlockServiceProfile;
use sim_services::dfs::DfsServiceProfile;
use sim_services::durable as durable_sim;
use sim_services::object::{
    LingquObjectKind, LingquObjectLocality, LingquObjectMetadata, LingquObjectPublishReq,
    LingquObjectRecord, LingquObjectServiceSnapshot, LingquObjectServiceStub, LingquPayloadBackend,
    LingquPayloadPlacement,
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
    #[error("missing execution artifact: {0}")]
    MissingExecutionArtifact(String),
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
    #[error("missing object: {0}")]
    MissingObject(String),
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
    pub dfs_audit_appends: u64,
    pub dfs_audit_reads: u64,
    pub block_payload_writes: u64,
    pub block_payload_reads: u64,
    pub dfs_bytes_written: u64,
    pub dfs_bytes_read: u64,
    pub block_bytes_written: u64,
    pub block_bytes_read: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquMemoryDfsPayloadSnapshot {
    pub path: String,
    pub bytes: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquMemoryBlockPayloadSnapshot {
    pub block: String,
    pub bytes: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquMemoryDurableStoreSnapshot {
    pub dfs_payloads: Vec<LingquMemoryDfsPayloadSnapshot>,
    pub block_payloads: Vec<LingquMemoryBlockPayloadSnapshot>,
    pub next_timestamp_us: u64,
}

impl LingquMemoryDurableStoreSnapshot {
    pub fn validate(&self) -> MemoryResult<()> {
        for payload in &self.dfs_payloads {
            if payload.path.trim().is_empty() {
                return Err(LingquMemoryError::MissingField("dfs_payload.path"));
            }
            if payload.bytes.is_empty() {
                return Err(LingquMemoryError::MissingField("dfs_payload.bytes"));
            }
        }
        for payload in &self.block_payloads {
            if payload.block.trim().is_empty() {
                return Err(LingquMemoryError::MissingField("block_payload.block"));
            }
            if payload.bytes.is_empty() {
                return Err(LingquMemoryError::MissingField("block_payload.bytes"));
            }
        }
        nonzero(self.next_timestamp_us, "next_timestamp_us")?;
        Ok(())
    }

    pub fn to_json_bytes(&self) -> MemoryResult<Vec<u8>> {
        self.validate()?;
        serde_json::to_vec_pretty(self)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
    }

    pub fn from_json_bytes(bytes: &[u8]) -> MemoryResult<Self> {
        let snapshot = serde_json::from_slice::<Self>(bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        snapshot.validate()?;
        Ok(snapshot)
    }
}

pub const LINGQU_EXECUTION_ARTIFACT_MANIFEST_PATH: &str =
    "/lingqu/memory/execution-artifacts/manifest.json";
pub const LINGQU_PREFIX_CACHE_MANIFEST_PATH: &str = "/lingqu/memory/prefix-cache/manifest.json";
pub const LINGQU_SHORTPATH_DECISION_MANIFEST_PATH: &str =
    "/lingqu/memory/shortpath-decisions/audit.json";
pub const LINGQU_PREFETCH_PLAN_MANIFEST_PATH: &str = "/lingqu/memory/prefetch-plans/audit.json";
pub const LINGQU_SHORTPATH_DECISION_AUDIT_LOG_PATH: &str =
    "/lingqu/memory/audit/shortpath-decisions.log";
pub const LINGQU_PREFETCH_PLAN_AUDIT_LOG_PATH: &str = "/lingqu/memory/audit/prefetch-plans.log";
pub const LINGQU_OBJECT_SERVICE_CHECKPOINT_PATH: &str =
    "/lingqu/object-service/checkpoints/latest.json";

pub const LINGQU_EXECUTION_ARTIFACT_MANIFEST_KIND: &str =
    "lingqu_memory_execution_artifact_manifest";
pub const LINGQU_PREFIX_CACHE_MANIFEST_KIND: &str = "lingqu_memory_prefix_cache_manifest";
pub const LINGQU_SHORTPATH_DECISION_MANIFEST_KIND: &str =
    "lingqu_memory_shortpath_decision_manifest";
pub const LINGQU_PREFETCH_PLAN_MANIFEST_KIND: &str = "lingqu_memory_prefetch_plan_manifest";
pub const LINGQU_OBJECT_SERVICE_CHECKPOINT_KIND: &str = "lingqu_object_service_checkpoint";
pub const LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION: u32 = 1;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquObjectServiceCheckpoint {
    pub kind: String,
    pub schema_version: u32,
    pub profile: sim_services::object::LingquObjectServiceProfile,
    pub records: Vec<LingquObjectRecordCheckpoint>,
    pub checksum: u64,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquObjectRecordCheckpoint {
    pub record: LingquObjectRecord,
    pub payload_ref: Option<LingquBlockPayloadRef>,
}

impl LingquObjectServiceCheckpoint {
    fn new(snapshot: LingquObjectServiceSnapshot) -> MemoryResult<Self> {
        let mut records = snapshot
            .records
            .into_iter()
            .map(|record| LingquObjectRecordCheckpoint {
                record,
                payload_ref: None,
            })
            .collect::<Vec<_>>();
        records.sort_by(|left, right| {
            left.record
                .key
                .cmp(&right.record.key)
                .then_with(|| left.record.version.cmp(&right.record.version))
        });
        let mut checkpoint = Self {
            kind: LINGQU_OBJECT_SERVICE_CHECKPOINT_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            profile: snapshot.profile,
            records,
            checksum: 0,
        };
        checkpoint.checksum = object_service_checkpoint_checksum(&checkpoint);
        Ok(checkpoint)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_OBJECT_SERVICE_CHECKPOINT_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "object_service_checkpoint.kind",
                reason: "unexpected checkpoint kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "object_service_checkpoint.schema_version",
                reason: "unsupported checkpoint schema version",
            });
        }
        let mut versions = HashSet::new();
        for entry in &self.records {
            validate_object_record_checkpoint(entry)?;
            if !versions.insert((entry.record.key.clone(), entry.record.version)) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "object_service_checkpoint.records",
                    reason: "duplicate object key version",
                });
            }
        }
        let actual = object_service_checkpoint_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_OBJECT_SERVICE_CHECKPOINT_PATH.to_string(),
                expected: self.checksum,
                actual,
            });
        }
        Ok(())
    }

    pub fn to_json_bytes(&self) -> MemoryResult<Vec<u8>> {
        self.validate()?;
        serde_json::to_vec_pretty(self)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
    }

    pub fn from_json_bytes(bytes: &[u8]) -> MemoryResult<Self> {
        let checkpoint = serde_json::from_slice::<Self>(bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        checkpoint.validate()?;
        Ok(checkpoint)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquExecutionArtifactManifest {
    pub kind: String,
    pub schema_version: u32,
    pub artifacts: Vec<ExecutionArtifactObject>,
    pub checksum: u64,
}

impl LingquExecutionArtifactManifest {
    pub fn new(mut artifacts: Vec<ExecutionArtifactObject>) -> MemoryResult<Self> {
        artifacts.sort_by(|left, right| left.artifact_id.cmp(&right.artifact_id));
        let mut manifest = Self {
            kind: LINGQU_EXECUTION_ARTIFACT_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            artifacts,
            checksum: 0,
        };
        manifest.checksum = execution_artifact_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_EXECUTION_ARTIFACT_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "execution_artifact_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "execution_artifact_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for artifact in &self.artifacts {
            artifact.validate()?;
            if !ids.insert(artifact.artifact_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "execution_artifact_manifest.artifacts",
                    reason: "duplicate artifact id",
                });
            }
        }
        let actual = execution_artifact_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_EXECUTION_ARTIFACT_MANIFEST_PATH.to_string(),
                expected: self.checksum,
                actual,
            });
        }
        Ok(())
    }

    pub fn to_json_bytes(&self) -> MemoryResult<Vec<u8>> {
        self.validate()?;
        serde_json::to_vec_pretty(self)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
    }

    pub fn from_json_bytes(bytes: &[u8]) -> MemoryResult<Self> {
        let manifest = serde_json::from_slice::<Self>(bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        manifest.validate()?;
        Ok(manifest)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquPrefixCacheManifest {
    pub kind: String,
    pub schema_version: u32,
    pub artifacts: Vec<PrefixCacheArtifact>,
    pub checksum: u64,
}

impl LingquPrefixCacheManifest {
    pub fn new(mut artifacts: Vec<PrefixCacheArtifact>) -> MemoryResult<Self> {
        artifacts.sort_by(|left, right| left.artifact_id.cmp(&right.artifact_id));
        let mut manifest = Self {
            kind: LINGQU_PREFIX_CACHE_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            artifacts,
            checksum: 0,
        };
        manifest.checksum = prefix_cache_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_PREFIX_CACHE_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for artifact in &self.artifacts {
            artifact.validate()?;
            if !ids.insert(artifact.artifact_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefix_cache_manifest.artifacts",
                    reason: "duplicate artifact id",
                });
            }
        }
        let actual = prefix_cache_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_PREFIX_CACHE_MANIFEST_PATH.to_string(),
                expected: self.checksum,
                actual,
            });
        }
        Ok(())
    }

    pub fn to_json_bytes(&self) -> MemoryResult<Vec<u8>> {
        self.validate()?;
        serde_json::to_vec_pretty(self)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
    }

    pub fn from_json_bytes(bytes: &[u8]) -> MemoryResult<Self> {
        let manifest = serde_json::from_slice::<Self>(bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        manifest.validate()?;
        Ok(manifest)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquShortpathDecisionManifest {
    pub kind: String,
    pub schema_version: u32,
    pub decisions: Vec<ShortpathDecisionRecord>,
    pub checksum: u64,
}

impl LingquShortpathDecisionManifest {
    pub fn new(mut decisions: Vec<ShortpathDecisionRecord>) -> MemoryResult<Self> {
        decisions.sort_by(|left, right| left.decision_id.cmp(&right.decision_id));
        let mut manifest = Self {
            kind: LINGQU_SHORTPATH_DECISION_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            decisions,
            checksum: 0,
        };
        manifest.checksum = shortpath_decision_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_SHORTPATH_DECISION_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "shortpath_decision_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "shortpath_decision_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for decision in &self.decisions {
            decision.validate()?;
            if !ids.insert(decision.decision_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "shortpath_decision_manifest.decisions",
                    reason: "duplicate decision id",
                });
            }
        }
        let actual = shortpath_decision_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_SHORTPATH_DECISION_MANIFEST_PATH.to_string(),
                expected: self.checksum,
                actual,
            });
        }
        Ok(())
    }

    pub fn to_json_bytes(&self) -> MemoryResult<Vec<u8>> {
        self.validate()?;
        serde_json::to_vec_pretty(self)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
    }

    pub fn from_json_bytes(bytes: &[u8]) -> MemoryResult<Self> {
        let manifest = serde_json::from_slice::<Self>(bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        manifest.validate()?;
        Ok(manifest)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct LingquPrefetchPlanManifest {
    pub kind: String,
    pub schema_version: u32,
    pub plans: Vec<PrefetchPlanRecord>,
    pub checksum: u64,
}

impl LingquPrefetchPlanManifest {
    pub fn new(mut plans: Vec<PrefetchPlanRecord>) -> MemoryResult<Self> {
        plans.sort_by(|left, right| left.plan_id.cmp(&right.plan_id));
        let mut manifest = Self {
            kind: LINGQU_PREFETCH_PLAN_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            plans,
            checksum: 0,
        };
        manifest.checksum = prefetch_plan_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_PREFETCH_PLAN_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefetch_plan_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefetch_plan_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for plan in &self.plans {
            plan.validate()?;
            if !ids.insert(plan.plan_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefetch_plan_manifest.plans",
                    reason: "duplicate plan id",
                });
            }
        }
        let actual = prefetch_plan_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_PREFETCH_PLAN_MANIFEST_PATH.to_string(),
                expected: self.checksum,
                actual,
            });
        }
        Ok(())
    }

    pub fn to_json_bytes(&self) -> MemoryResult<Vec<u8>> {
        self.validate()?;
        serde_json::to_vec_pretty(self)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
    }

    pub fn from_json_bytes(bytes: &[u8]) -> MemoryResult<Self> {
        let manifest = serde_json::from_slice::<Self>(bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        manifest.validate()?;
        Ok(manifest)
    }
}

#[derive(Debug)]
pub struct LingquMemoryDurableStore {
    durable: durable_sim::LingquDurableSim,
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
            durable: durable_sim::LingquDurableSim::new(durable_sim::LingquDurableSimProfile {
                dfs: dfs_profile.into(),
                block: block_profile.into(),
                default_inline_threshold_bytes: 4096,
            }),
            stats: LingquMemoryDurableStats::default(),
        }
    }

    pub fn stats(&self) -> LingquMemoryDurableStats {
        self.stats
    }

    pub fn export_snapshot(&self) -> MemoryResult<LingquMemoryDurableStoreSnapshot> {
        let durable_snapshot = self.export_durable_sim_snapshot()?;
        let mut dfs_payloads = legacy_dfs_payloads_from_durable_snapshot(&durable_snapshot)?;
        dfs_payloads.sort_by(|left, right| left.path.cmp(&right.path));

        let mut block_payloads = legacy_block_payloads_from_durable_snapshot(&durable_snapshot);
        block_payloads.sort_by(|left, right| left.block.cmp(&right.block));

        let snapshot = LingquMemoryDurableStoreSnapshot {
            dfs_payloads,
            block_payloads,
            next_timestamp_us: durable_snapshot.next_timestamp_us,
        };
        snapshot.validate()?;
        Ok(snapshot)
    }

    pub fn export_durable_sim_snapshot(
        &self,
    ) -> MemoryResult<durable_sim::LingquDurableSimSnapshot> {
        self.durable
            .export_snapshot()
            .map_err(memory_error_from_durable)
    }

    pub fn import_snapshot(snapshot: LingquMemoryDurableStoreSnapshot) -> MemoryResult<Self> {
        snapshot.validate()?;
        let mut store = Self::new();
        for payload in snapshot.dfs_payloads {
            store.submit_dfs_write(payload.path, payload.bytes)?;
        }
        for payload in snapshot.block_payloads {
            store.submit_block_write(BlockHash(payload.block), payload.bytes)?;
        }
        store.stats = LingquMemoryDurableStats::default();
        Ok(store)
    }

    pub fn import_durable_sim_snapshot(
        snapshot: durable_sim::LingquDurableSimSnapshot,
    ) -> MemoryResult<Self> {
        let durable = durable_sim::LingquDurableSim::import_snapshot(snapshot)
            .map_err(memory_error_from_durable)?;
        Ok(Self {
            durable,
            stats: LingquMemoryDurableStats::default(),
        })
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

    pub fn persist_execution_artifact_manifest(
        &mut self,
        artifacts: Vec<ExecutionArtifactObject>,
    ) -> MemoryResult<LingquDfsPath> {
        let manifest = LingquExecutionArtifactManifest::new(artifacts)?;
        let bytes = manifest.to_json_bytes()?;
        let path = LingquDfsPath::new(LINGQU_EXECUTION_ARTIFACT_MANIFEST_PATH);
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_execution_artifact_manifest(
        &mut self,
    ) -> MemoryResult<Vec<ExecutionArtifactObject>> {
        let bytes = self.submit_dfs_read(LINGQU_EXECUTION_ARTIFACT_MANIFEST_PATH)?;
        Ok(LingquExecutionArtifactManifest::from_json_bytes(&bytes)?.artifacts)
    }

    pub fn persist_prefix_cache_manifest(
        &mut self,
        artifacts: Vec<PrefixCacheArtifact>,
    ) -> MemoryResult<LingquDfsPath> {
        let manifest = LingquPrefixCacheManifest::new(artifacts)?;
        let bytes = manifest.to_json_bytes()?;
        let path = LingquDfsPath::new(LINGQU_PREFIX_CACHE_MANIFEST_PATH);
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_prefix_cache_manifest(&mut self) -> MemoryResult<Vec<PrefixCacheArtifact>> {
        let bytes = self.submit_dfs_read(LINGQU_PREFIX_CACHE_MANIFEST_PATH)?;
        Ok(LingquPrefixCacheManifest::from_json_bytes(&bytes)?.artifacts)
    }

    pub fn persist_shortpath_decision_manifest(
        &mut self,
        decisions: Vec<ShortpathDecisionRecord>,
    ) -> MemoryResult<LingquDfsPath> {
        let path = LingquDfsPath::new(LINGQU_SHORTPATH_DECISION_AUDIT_LOG_PATH);
        let mut decisions = decisions;
        decisions.sort_by(|left, right| left.decision_id.cmp(&right.decision_id));
        let existing =
            self.load_shortpath_decision_audit_entries(true, "persist shortpath decisions")?;
        let mut existing_by_id = HashMap::new();
        for (decision, bytes) in existing {
            if existing_by_id
                .insert(decision.decision_id.clone(), bytes)
                .is_some()
            {
                return Err(LingquMemoryError::InvalidValue {
                    field: "shortpath_decision.decision_id",
                    reason: "duplicate decision id in durable audit log",
                });
            }
        }

        let mut ops = Vec::new();
        let mut bytes_written = 0;
        let mut next_seq = existing_by_id.len() as u64 + 1;
        for decision in decisions {
            decision.validate()?;
            let bytes = serde_json::to_vec(&decision)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            if let Some(existing_bytes) = existing_by_id.get(&decision.decision_id) {
                if existing_bytes != &bytes {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "shortpath_decision.decision_id",
                        reason: "decision id already exists with different payload",
                    });
                }
                continue;
            }
            bytes_written += bytes.len() as u64;
            ops.push(durable_sim::LingquDurableBatchOp::DfsAppendLog {
                path: path.path.clone(),
                bytes,
                options: durable_sim::LingquDfsAppendOptions {
                    expected_next_seq: Some(next_seq),
                    writer: Some("lingqu-memory-service".to_string()),
                    metadata: durable_audit_metadata("shortpath_decision"),
                },
            });
            next_seq += 1;
        }
        if !ops.is_empty() {
            self.durable
                .commit_batch(ops)
                .map_err(memory_error_from_durable)?;
            self.stats.dfs_audit_appends += next_seq - existing_by_id.len() as u64 - 1;
            self.stats.dfs_bytes_written += bytes_written;
        }
        Ok(path)
    }

    pub fn load_shortpath_decision_manifest(
        &mut self,
    ) -> MemoryResult<Vec<ShortpathDecisionRecord>> {
        Ok(self
            .load_shortpath_decision_audit_entries(false, "load shortpath decisions")?
            .into_iter()
            .map(|(decision, _bytes)| decision)
            .collect())
    }

    pub fn persist_prefetch_plan_manifest(
        &mut self,
        plans: Vec<PrefetchPlanRecord>,
    ) -> MemoryResult<LingquDfsPath> {
        let path = LingquDfsPath::new(LINGQU_PREFETCH_PLAN_AUDIT_LOG_PATH);
        let mut plans = plans;
        plans.sort_by(|left, right| left.plan_id.cmp(&right.plan_id));
        let existing = self.load_prefetch_plan_audit_entries(true, "persist prefetch plans")?;
        let mut existing_by_id = HashMap::new();
        for (plan, bytes) in existing {
            if existing_by_id.insert(plan.plan_id.clone(), bytes).is_some() {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefetch_plan.plan_id",
                    reason: "duplicate plan id in durable audit log",
                });
            }
        }

        let mut ops = Vec::new();
        let mut bytes_written = 0;
        let mut next_seq = existing_by_id.len() as u64 + 1;
        for plan in plans {
            plan.validate()?;
            let bytes = serde_json::to_vec(&plan)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            if let Some(existing_bytes) = existing_by_id.get(&plan.plan_id) {
                if existing_bytes != &bytes {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "prefetch_plan.plan_id",
                        reason: "plan id already exists with different payload",
                    });
                }
                continue;
            }
            bytes_written += bytes.len() as u64;
            ops.push(durable_sim::LingquDurableBatchOp::DfsAppendLog {
                path: path.path.clone(),
                bytes,
                options: durable_sim::LingquDfsAppendOptions {
                    expected_next_seq: Some(next_seq),
                    writer: Some("lingqu-memory-service".to_string()),
                    metadata: durable_audit_metadata("prefetch_plan"),
                },
            });
            next_seq += 1;
        }
        if !ops.is_empty() {
            self.durable
                .commit_batch(ops)
                .map_err(memory_error_from_durable)?;
            self.stats.dfs_audit_appends += next_seq - existing_by_id.len() as u64 - 1;
            self.stats.dfs_bytes_written += bytes_written;
        }
        Ok(path)
    }

    pub fn load_prefetch_plan_manifest(&mut self) -> MemoryResult<Vec<PrefetchPlanRecord>> {
        Ok(self
            .load_prefetch_plan_audit_entries(false, "load prefetch plans")?
            .into_iter()
            .map(|(plan, _bytes)| plan)
            .collect())
    }

    pub fn persist_object_service_checkpoint(
        &mut self,
        object_service: &LingquObjectServiceStub,
    ) -> MemoryResult<LingquDfsPath> {
        let snapshot = object_service.export_snapshot();
        let mut checkpoint = LingquObjectServiceCheckpoint::new(snapshot)?;
        let mut ops = Vec::new();
        let mut block_writes = 0;
        let mut block_bytes_written = 0;
        for entry in &mut checkpoint.records {
            if entry.record.payload_bytes.is_empty() {
                if entry.record.bytes != 0 {
                    return Err(LingquMemoryError::MissingField("object_payload"));
                }
                continue;
            }
            let block = object_payload_block(&entry.record);
            let bytes = std::mem::take(&mut entry.record.payload_bytes);
            let payload_ref = LingquBlockPayloadRef {
                block: block.clone(),
                offset: 0,
                bytes: bytes.len() as u64,
                checksum: checksum64(&bytes),
            };
            payload_ref.validate("object_payload_ref")?;
            if payload_ref.bytes != entry.record.bytes
                || payload_ref.checksum != entry.record.checksum
            {
                return Err(LingquMemoryError::PayloadChecksumMismatch {
                    id: entry.record.key.clone(),
                    expected: entry.record.checksum,
                    actual: payload_ref.checksum,
                });
            }
            entry.payload_ref = Some(payload_ref);
            block_bytes_written += bytes.len() as u64;
            block_writes += 1;
            ops.push(durable_sim::LingquDurableBatchOp::BlockWrite {
                block: block.0,
                bytes,
                options: durable_sim::LingquBlockWriteOptions::default(),
            });
        }

        checkpoint.records.sort_by(|left, right| {
            left.record
                .key
                .cmp(&right.record.key)
                .then_with(|| left.record.version.cmp(&right.record.version))
        });
        checkpoint.checksum = object_service_checkpoint_checksum(&checkpoint);
        let checkpoint_bytes = checkpoint.to_json_bytes()?;
        let checkpoint_bytes_len = checkpoint_bytes.len() as u64;
        let path = LingquDfsPath::new(LINGQU_OBJECT_SERVICE_CHECKPOINT_PATH);
        ops.push(durable_sim::LingquDurableBatchOp::DfsWrite {
            path: path.path.clone(),
            bytes: checkpoint_bytes,
            options: durable_sim::LingquDfsWriteOptions {
                content_type: durable_sim::LingquDfsContentType::Json,
                ..durable_sim::LingquDfsWriteOptions::default()
            },
        });
        self.durable
            .commit_batch(ops)
            .map_err(memory_error_from_durable)?;
        self.stats.block_payload_writes += block_writes;
        self.stats.block_bytes_written += block_bytes_written;
        self.stats.dfs_catalog_writes += 1;
        self.stats.dfs_bytes_written += checkpoint_bytes_len;
        Ok(path)
    }

    pub fn load_object_service_checkpoint(&mut self) -> MemoryResult<LingquObjectServiceSnapshot> {
        let bytes = self.submit_dfs_read(LINGQU_OBJECT_SERVICE_CHECKPOINT_PATH)?;
        let checkpoint = LingquObjectServiceCheckpoint::from_json_bytes(&bytes)?;
        let mut records = Vec::with_capacity(checkpoint.records.len());
        for mut entry in checkpoint.records {
            if let Some(payload_ref) = &entry.payload_ref {
                entry.record.payload_bytes = self.read_block_payload(payload_ref)?;
            }
            validate_object_record_checkpoint_payload(&entry)?;
            records.push(entry.record);
        }
        Ok(LingquObjectServiceSnapshot {
            profile: checkpoint.profile,
            records,
        })
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
        self.submit_block_read(payload_ref)
    }

    fn load_shortpath_decision_audit_entries(
        &mut self,
        allow_missing: bool,
        op: &'static str,
    ) -> MemoryResult<Vec<(ShortpathDecisionRecord, Vec<u8>)>> {
        let records = self
            .submit_dfs_append_log_read(LINGQU_SHORTPATH_DECISION_AUDIT_LOG_PATH, allow_missing)?;
        let mut entries = Vec::with_capacity(records.len());
        let mut ids = HashSet::new();
        for record in records {
            let decision = serde_json::from_slice::<ShortpathDecisionRecord>(&record.bytes)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            decision.validate()?;
            if !ids.insert(decision.decision_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "shortpath_decision.decision_id",
                    reason: "duplicate decision id in durable audit log",
                });
            }
            entries.push((decision, record.bytes));
        }
        if entries.is_empty() && !allow_missing {
            return Err(LingquMemoryError::MissingDfsPath(format!(
                "{op}: {LINGQU_SHORTPATH_DECISION_AUDIT_LOG_PATH}"
            )));
        }
        Ok(entries)
    }

    fn load_prefetch_plan_audit_entries(
        &mut self,
        allow_missing: bool,
        op: &'static str,
    ) -> MemoryResult<Vec<(PrefetchPlanRecord, Vec<u8>)>> {
        let records =
            self.submit_dfs_append_log_read(LINGQU_PREFETCH_PLAN_AUDIT_LOG_PATH, allow_missing)?;
        let mut entries = Vec::with_capacity(records.len());
        let mut ids = HashSet::new();
        for record in records {
            let plan = serde_json::from_slice::<PrefetchPlanRecord>(&record.bytes)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            plan.validate()?;
            if !ids.insert(plan.plan_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefetch_plan.plan_id",
                    reason: "duplicate plan id in durable audit log",
                });
            }
            entries.push((plan, record.bytes));
        }
        if entries.is_empty() && !allow_missing {
            return Err(LingquMemoryError::MissingDfsPath(format!(
                "{op}: {LINGQU_PREFETCH_PLAN_AUDIT_LOG_PATH}"
            )));
        }
        Ok(entries)
    }

    fn submit_dfs_write(&mut self, path: String, bytes: Vec<u8>) -> MemoryResult<()> {
        let bytes_len = bytes.len() as u64;
        self.durable
            .dfs_write(
                path,
                bytes,
                durable_sim::LingquDfsWriteOptions {
                    content_type: durable_sim::LingquDfsContentType::Json,
                    ..durable_sim::LingquDfsWriteOptions::default()
                },
            )
            .map_err(memory_error_from_durable)?;
        self.stats.dfs_catalog_writes += 1;
        self.stats.dfs_bytes_written += bytes_len;
        Ok(())
    }

    fn submit_dfs_read(&mut self, path: &str) -> MemoryResult<Vec<u8>> {
        let bytes = self
            .durable
            .dfs_read(path, durable_sim::LingquVersionSelector::LatestCommitted)
            .map_err(memory_error_from_durable)?;
        self.stats.dfs_catalog_reads += 1;
        self.stats.dfs_bytes_read += bytes.len() as u64;
        Ok(bytes)
    }

    fn submit_dfs_append_log_read(
        &mut self,
        path: &str,
        allow_missing: bool,
    ) -> MemoryResult<Vec<durable_sim::LingquDfsAppendLogRecord>> {
        match self.durable.dfs_append_log_read(path, 1, None) {
            Ok(records) => {
                self.stats.dfs_audit_reads += 1;
                self.stats.dfs_bytes_read += records
                    .iter()
                    .map(|record| record.bytes.len() as u64)
                    .sum::<u64>();
                Ok(records)
            }
            Err(durable_sim::LingquDurableError::MissingDfsPath(_)) if allow_missing => {
                Ok(Vec::new())
            }
            Err(err) => Err(memory_error_from_durable(err)),
        }
    }

    fn submit_block_write(&mut self, block: BlockHash, bytes: Vec<u8>) -> MemoryResult<()> {
        let bytes_len = bytes.len() as u64;
        self.durable
            .block_write(
                block.0,
                bytes,
                durable_sim::LingquBlockWriteOptions::default(),
            )
            .map_err(memory_error_from_durable)?;
        self.stats.block_payload_writes += 1;
        self.stats.block_bytes_written += bytes_len;
        Ok(())
    }

    fn submit_block_read(&mut self, payload_ref: &LingquBlockPayloadRef) -> MemoryResult<Vec<u8>> {
        let latest = self
            .durable
            .block_stat(
                &payload_ref.block,
                durable_sim::LingquVersionSelector::LatestCommitted,
            )
            .map_err(memory_error_from_durable)?;
        let durable_payload_ref = durable_sim::LingquBlockPayloadRef {
            block: payload_ref.block.clone(),
            version: latest.version,
            offset: payload_ref.offset,
            bytes: payload_ref.bytes,
            checksum: payload_ref.checksum,
        };
        self.stats.block_payload_reads += 1;
        let bytes = self
            .durable
            .block_read(&durable_payload_ref)
            .map_err(memory_error_from_durable)?;
        Ok(bytes)
    }
}

impl Default for LingquMemoryDurableStore {
    fn default() -> Self {
        Self::new()
    }
}

fn memory_error_from_durable(err: durable_sim::LingquDurableError) -> LingquMemoryError {
    match err {
        durable_sim::LingquDurableError::MissingDfsPath(path) => {
            LingquMemoryError::MissingDfsPath(path)
        }
        durable_sim::LingquDurableError::MissingBlock(block) => {
            LingquMemoryError::MissingBlockPayload(block)
        }
        durable_sim::LingquDurableError::ChecksumMismatch {
            id,
            expected,
            actual,
        } => LingquMemoryError::PayloadChecksumMismatch {
            id,
            expected,
            actual,
        },
        other => LingquMemoryError::DurableServiceFailed(other.to_string()),
    }
}

fn durable_audit_metadata(record_kind: &'static str) -> BTreeMap<String, String> {
    let mut metadata = BTreeMap::new();
    metadata.insert("service".to_string(), "lingqu-memory".to_string());
    metadata.insert("record_kind".to_string(), record_kind.to_string());
    metadata
}

fn validate_object_record_checkpoint(entry: &LingquObjectRecordCheckpoint) -> MemoryResult<()> {
    required_str(&entry.record.key, "object_record.key")?;
    nonzero(entry.record.version, "object_record.version")?;
    if !entry.record.payload_bytes.is_empty() {
        return Err(LingquMemoryError::InvalidValue {
            field: "object_record.payload_bytes",
            reason: "object checkpoint metadata must not inline hot payload bytes",
        });
    }
    if entry.record.bytes > 0 {
        let payload_ref = entry
            .payload_ref
            .as_ref()
            .ok_or_else(|| LingquMemoryError::MissingBlockPayload(entry.record.key.clone()))?;
        payload_ref.validate("object_record.payload_ref")?;
        if payload_ref.bytes != entry.record.bytes {
            return Err(LingquMemoryError::InvalidValue {
                field: "object_record.payload_ref.bytes",
                reason: "payload ref byte count must match object record",
            });
        }
        if payload_ref.checksum != entry.record.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: entry.record.key.clone(),
                expected: entry.record.checksum,
                actual: payload_ref.checksum,
            });
        }
    }
    for placement in &entry.record.placements {
        if placement.bytes != entry.record.bytes {
            return Err(LingquMemoryError::InvalidValue {
                field: "object_record.placement.bytes",
                reason: "placement byte count must match object record",
            });
        }
        if placement.checksum != entry.record.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: entry.record.key.clone(),
                expected: entry.record.checksum,
                actual: placement.checksum,
            });
        }
    }
    Ok(())
}

fn validate_object_record_checkpoint_payload(
    entry: &LingquObjectRecordCheckpoint,
) -> MemoryResult<()> {
    if entry.record.payload_bytes.len() as u64 != entry.record.bytes {
        return Err(LingquMemoryError::InvalidValue {
            field: "object_record.payload_bytes",
            reason: "restored payload byte count must match object record",
        });
    }
    let actual = checksum64(&entry.record.payload_bytes);
    if actual != entry.record.checksum {
        return Err(LingquMemoryError::PayloadChecksumMismatch {
            id: entry.record.key.clone(),
            expected: entry.record.checksum,
            actual,
        });
    }
    Ok(())
}

fn object_payload_block(record: &LingquObjectRecord) -> BlockHash {
    BlockHash(format!(
        "block/object-service/payload/{:016x}/v{}",
        checksum64(record.key.as_bytes()),
        record.version
    ))
}

fn object_service_checkpoint_checksum(checkpoint: &LingquObjectServiceCheckpoint) -> u64 {
    let mut shadow = checkpoint.clone();
    shadow.checksum = 0;
    let bytes = serde_json::to_vec(&shadow).unwrap_or_default();
    checksum64(&bytes)
}

fn legacy_dfs_payloads_from_durable_snapshot(
    snapshot: &durable_sim::LingquDurableSimSnapshot,
) -> MemoryResult<Vec<LingquMemoryDfsPayloadSnapshot>> {
    let mut latest_by_path: HashMap<String, &durable_sim::LingquDfsFileRecord> = HashMap::new();
    for record in &snapshot.dfs.files {
        if record.state != durable_sim::LingquDfsFileState::Committed {
            continue;
        }
        let replace = latest_by_path
            .get(&record.path)
            .map(|current| current.version < record.version)
            .unwrap_or(true);
        if replace {
            latest_by_path.insert(record.path.clone(), record);
        }
    }

    latest_by_path
        .into_values()
        .map(|record| {
            let bytes = durable_dfs_record_bytes(record, &snapshot.block.blocks)?;
            Ok(LingquMemoryDfsPayloadSnapshot {
                path: record.path.clone(),
                bytes,
            })
        })
        .collect()
}

fn legacy_block_payloads_from_durable_snapshot(
    snapshot: &durable_sim::LingquDurableSimSnapshot,
) -> Vec<LingquMemoryBlockPayloadSnapshot> {
    let mut latest_by_block: HashMap<String, &durable_sim::LingquBlockRecord> = HashMap::new();
    for record in &snapshot.block.blocks {
        if !matches!(
            record.durable_state,
            durable_sim::LingquBlockDurableState::Committed
                | durable_sim::LingquBlockDurableState::Sealed
        ) {
            continue;
        }
        let replace = latest_by_block
            .get(&record.block.0)
            .map(|current| current.version < record.version)
            .unwrap_or(true);
        if replace {
            latest_by_block.insert(record.block.0.clone(), record);
        }
    }

    latest_by_block
        .into_values()
        .map(|record| LingquMemoryBlockPayloadSnapshot {
            block: record.block.0.clone(),
            bytes: record.bytes.clone(),
        })
        .collect()
}

fn durable_dfs_record_bytes(
    record: &durable_sim::LingquDfsFileRecord,
    block_records: &[durable_sim::LingquBlockRecord],
) -> MemoryResult<Vec<u8>> {
    match &record.content_ref {
        durable_sim::LingquDfsContentRef::Inline(bytes) => Ok(bytes.clone()),
        durable_sim::LingquDfsContentRef::Block(payload_ref) => {
            let block = block_records
                .iter()
                .find(|candidate| {
                    candidate.block == payload_ref.block && candidate.version == payload_ref.version
                })
                .ok_or_else(|| {
                    LingquMemoryError::MissingBlockPayload(payload_ref.block.0.clone())
                })?;
            let start = payload_ref.offset as usize;
            let end = payload_ref.offset.checked_add(payload_ref.bytes).ok_or(
                LingquMemoryError::InvalidValue {
                    field: "dfs_payload_ref",
                    reason: "payload range overflow",
                },
            )? as usize;
            if end > block.bytes.len() {
                return Err(LingquMemoryError::InvalidValue {
                    field: "dfs_payload_ref",
                    reason: "payload range exceeds block bytes",
                });
            }
            let selected = block.bytes[start..end].to_vec();
            let actual = checksum64(&selected);
            if actual != payload_ref.checksum {
                return Err(LingquMemoryError::PayloadChecksumMismatch {
                    id: payload_ref.block.0.clone(),
                    expected: payload_ref.checksum,
                    actual,
                });
            }
            Ok(selected)
        }
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

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum EngramOperatorKind {
    ContextGate,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EngramStateObject {
    pub state_id: String,
    pub hot_memory_state_id: String,
    pub query_result_id: String,
    pub query_result_manifest_ref: Option<LingquDfsPath>,
    pub operator_kind: EngramOperatorKind,
    pub operator_config_hash: u64,
    pub compatible_models: Vec<InferenceModelBinding>,
    pub table: HotTensorObjectRef,
    pub indices: HotTensorObjectRef,
    pub gate: Option<HotTensorObjectRef>,
    pub dtype: TensorDType,
    pub hidden_size: u64,
    pub table_rows: u64,
    pub execution_artifact_index_ref: Option<LingquDfsPath>,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl EngramStateObject {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.state_id, "engram_state_id")?;
        required_str(&self.hot_memory_state_id, "hot_memory_state_id")?;
        required_str(&self.query_result_id, "engram_state.query_result_id")?;
        if let Some(path) = &self.query_result_manifest_ref {
            path.validate("engram_state.query_result_manifest_ref")?;
        }
        nonzero(
            self.operator_config_hash,
            "engram_state.operator_config_hash",
        )?;
        let expected_operator_config_hash = engram_operator_config_hash(
            self.operator_kind,
            &self.table,
            &self.indices,
            self.gate.as_ref(),
        );
        if expected_operator_config_hash != self.operator_config_hash {
            return Err(LingquMemoryError::InvalidValue {
                field: "engram_state.operator_config_hash",
                reason: "operator config hash does not match Engram operands",
            });
        }
        for model in &self.compatible_models {
            model.validate()?;
        }
        validate_hot_ref(&self.table)?;
        validate_hot_ref(&self.indices)?;
        if let Some(gate) = &self.gate {
            validate_hot_ref(gate)?;
        }
        if self.dtype != self.table.dtype {
            return Err(LingquMemoryError::InvalidValue {
                field: "engram_state.dtype",
                reason: "dtype must match table dtype",
            });
        }
        if self.indices.dtype != TensorDType::U32 {
            return Err(LingquMemoryError::InvalidValue {
                field: "engram_state.indices",
                reason: "indices dtype must be u32",
            });
        }
        if self.table.shape.len() != 2 {
            return Err(LingquMemoryError::InvalidValue {
                field: "engram_state.table.shape",
                reason: "table shape must be [rows, hidden_size]",
            });
        }
        if self.table.shape[0] != self.table_rows || self.table.shape[1] != self.hidden_size {
            return Err(LingquMemoryError::InvalidValue {
                field: "engram_state.table.shape",
                reason: "table shape must match table_rows and hidden_size",
            });
        }
        if self.indices.shape != vec![self.table_rows] {
            return Err(LingquMemoryError::InvalidValue {
                field: "engram_state.indices.shape",
                reason: "indices shape must match table rows",
            });
        }
        if let Some(gate) = &self.gate {
            if gate.dtype != self.dtype || gate.shape != vec![self.hidden_size] {
                return Err(LingquMemoryError::InvalidValue {
                    field: "engram_state.gate",
                    reason: "gate dtype/shape must match hidden dimension",
                });
            }
        }
        if let Some(path) = &self.execution_artifact_index_ref {
            path.validate("engram_state.execution_artifact_index_ref")?;
        }
        nonzero(self.checksum, "engram_state.checksum")?;
        nonzero(self.version, "engram_state.version")?;
        let expected_checksum = engram_state_checksum(
            &self.state_id,
            &self.hot_memory_state_id,
            &self.query_result_id,
            self.operator_kind,
            self.operator_config_hash,
            &self.compatible_models,
            &self.table,
            &self.indices,
            self.gate.as_ref(),
            self.dtype,
            self.hidden_size,
            self.table_rows,
            self.version,
            self.created_at_us,
            self.expires_at_us,
        );
        if expected_checksum != self.checksum {
            return Err(LingquMemoryError::InvalidValue {
                field: "engram_state.checksum",
                reason: "checksum does not match Engram state metadata",
            });
        }
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "engram_state.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum ExecutionArtifactKind {
    HiddenState,
    KvCache,
    Logits,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum ExecutionArtifactState {
    Candidate,
    Verified,
    Rejected,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum ShortpathAction {
    Continue,
    JumpToLayer,
    JumpToTerminal,
    RequireVerify,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum RangeBoundaryPhase {
    RangeStart,
    RangeExit,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum PrefetchScope {
    Range,
    Step,
    MultiStep,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum PrefetchPlanState {
    Planned,
    Issued,
    Completed,
    Cancelled,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum PrefixCacheReuseAction {
    Miss,
    Reuse,
    RequireVerify,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct InferenceModelBinding {
    pub model_id: String,
    pub model_key: String,
    pub tokenizer_hash: u64,
    pub profile_hash: u64,
}

impl InferenceModelBinding {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.model_id, "model_binding.model_id")?;
        required_str(&self.model_key, "model_binding.model_key")?;
        nonzero(self.tokenizer_hash, "model_binding.tokenizer_hash")?;
        nonzero(self.profile_hash, "model_binding.profile_hash")?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct RangeBoundary {
    pub phase: RangeBoundaryPhase,
    pub step_index: u64,
    pub node_index: u32,
    pub layer_start: u32,
    pub layer_end: u32,
    pub next_node_index: Option<u32>,
    pub position: u64,
}

impl RangeBoundary {
    pub fn validate(&self) -> MemoryResult<()> {
        if self.layer_end <= self.layer_start {
            return Err(LingquMemoryError::InvalidValue {
                field: "range_boundary.layer_end",
                reason: "layer_end must be greater than layer_start",
            });
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PrefixCacheKey {
    pub model: InferenceModelBinding,
    pub namespace: String,
    pub chat_template_hash: u64,
    pub prefix_token_hash: u64,
    pub prefix_token_count: u64,
    pub rope_config_hash: u64,
    pub kv_layout_hash: u64,
    pub layer_start: u32,
    pub layer_end: u32,
    pub position_start: u64,
    pub position_end: u64,
    pub security_label: MemorySecurityLabel,
}

impl PrefixCacheKey {
    pub fn validate(&self) -> MemoryResult<()> {
        self.model.validate()?;
        required_str(&self.namespace, "prefix_cache_key.namespace")?;
        nonzero(
            self.chat_template_hash,
            "prefix_cache_key.chat_template_hash",
        )?;
        nonzero(self.prefix_token_hash, "prefix_cache_key.prefix_token_hash")?;
        nonzero(
            self.prefix_token_count,
            "prefix_cache_key.prefix_token_count",
        )?;
        nonzero(self.rope_config_hash, "prefix_cache_key.rope_config_hash")?;
        nonzero(self.kv_layout_hash, "prefix_cache_key.kv_layout_hash")?;
        if self.layer_end <= self.layer_start {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_key.layer_end",
                reason: "layer_end must be greater than layer_start",
            });
        }
        if self.position_end <= self.position_start {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_key.position_end",
                reason: "position_end must be greater than position_start",
            });
        }
        if self.position_end - self.position_start != self.prefix_token_count {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_key.position_range",
                reason: "position range must match prefix token count",
            });
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExecutionArtifactObject {
    pub artifact_id: String,
    pub kind: ExecutionArtifactKind,
    pub model: InferenceModelBinding,
    pub producer_boundary: RangeBoundary,
    pub target_layer_start: u32,
    pub target_layer_end: u32,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
    pub durable_payload_ref: Option<LingquBlockPayloadRef>,
    pub hot_object_ref: Option<HotTensorObjectRef>,
    pub source_query_result_id: Option<String>,
    pub source_engram_state_id: Option<String>,
    pub confidence_milli: u32,
    pub state: ExecutionArtifactState,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl ExecutionArtifactObject {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.artifact_id, "execution_artifact_id")?;
        self.model.validate()?;
        self.producer_boundary.validate()?;
        if self.target_layer_end < self.target_layer_start
            || (self.kind != ExecutionArtifactKind::Logits
                && self.target_layer_end == self.target_layer_start)
        {
            return Err(LingquMemoryError::InvalidValue {
                field: "execution_artifact.target_layer_end",
                reason: "target_layer_end must be greater than target_layer_start unless artifact is terminal logits",
            });
        }
        require_nonempty(&self.shape, "execution_artifact.shape")?;
        for dim in &self.shape {
            nonzero(*dim, "execution_artifact.shape")?;
        }
        if self.durable_payload_ref.is_none() && self.hot_object_ref.is_none() {
            return Err(LingquMemoryError::MissingField(
                "execution_artifact.payload_ref",
            ));
        }
        if let Some(payload_ref) = &self.durable_payload_ref {
            payload_ref.validate("execution_artifact.durable_payload_ref")?;
        }
        if let Some(hot_ref) = &self.hot_object_ref {
            validate_hot_ref(hot_ref)?;
            if hot_ref.dtype != self.dtype || hot_ref.shape != self.shape {
                return Err(LingquMemoryError::InvalidValue {
                    field: "execution_artifact.hot_object_ref",
                    reason: "hot object dtype/shape must match artifact metadata",
                });
            }
        }
        if let Some(query_result_id) = &self.source_query_result_id {
            required_str(query_result_id, "execution_artifact.source_query_result_id")?;
        }
        if let Some(engram_state_id) = &self.source_engram_state_id {
            required_str(engram_state_id, "execution_artifact.source_engram_state_id")?;
        }
        if self.confidence_milli > 1000 {
            return Err(LingquMemoryError::InvalidValue {
                field: "execution_artifact.confidence_milli",
                reason: "confidence_milli must be in [0, 1000]",
            });
        }
        nonzero(self.checksum, "execution_artifact.checksum")?;
        nonzero(self.version, "execution_artifact.version")?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "execution_artifact.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PrefixCacheArtifact {
    pub artifact_id: String,
    pub key: PrefixCacheKey,
    pub kv_artifact_ids: Vec<String>,
    pub durable_payload_refs: Vec<LingquBlockPayloadRef>,
    pub hot_object_refs: Vec<HotTensorObjectRef>,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
    pub confidence_milli: u32,
    pub state: ExecutionArtifactState,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
    pub last_used_at_us: u64,
    pub use_count: u64,
}

impl PrefixCacheArtifact {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.artifact_id, "prefix_cache_artifact.artifact_id")?;
        self.key.validate()?;
        if self.kv_artifact_ids.is_empty()
            && self.durable_payload_refs.is_empty()
            && self.hot_object_refs.is_empty()
        {
            return Err(LingquMemoryError::MissingField(
                "prefix_cache_artifact.payload_ref",
            ));
        }
        for artifact_id in &self.kv_artifact_ids {
            required_str(artifact_id, "prefix_cache_artifact.kv_artifact_ids")?;
        }
        for payload_ref in &self.durable_payload_refs {
            payload_ref.validate("prefix_cache_artifact.durable_payload_refs")?;
        }
        for hot_ref in &self.hot_object_refs {
            validate_hot_ref(hot_ref)?;
            if hot_ref.dtype != self.dtype {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefix_cache_artifact.hot_object_refs",
                    reason: "hot object dtype must match prefix cache artifact dtype",
                });
            }
        }
        require_nonempty(&self.shape, "prefix_cache_artifact.shape")?;
        for dim in &self.shape {
            nonzero(*dim, "prefix_cache_artifact.shape")?;
        }
        if self.confidence_milli > 1000 {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_artifact.confidence_milli",
                reason: "confidence_milli must be in [0, 1000]",
            });
        }
        nonzero(self.checksum, "prefix_cache_artifact.checksum")?;
        nonzero(self.version, "prefix_cache_artifact.version")?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefix_cache_artifact.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BoundaryLookupRequest {
    pub request_id: String,
    pub model: InferenceModelBinding,
    pub boundary: RangeBoundary,
    pub hidden_state: HotTensorObjectRef,
    pub engram_state_id: Option<String>,
    pub min_confidence_milli: u32,
    pub allowed_actions: Vec<ShortpathAction>,
    pub created_at_us: u64,
}

impl BoundaryLookupRequest {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.request_id, "boundary_lookup.request_id")?;
        self.model.validate()?;
        self.boundary.validate()?;
        validate_hot_ref(&self.hidden_state)?;
        if let Some(engram_state_id) = &self.engram_state_id {
            required_str(engram_state_id, "boundary_lookup.engram_state_id")?;
        }
        if self.min_confidence_milli > 1000 {
            return Err(LingquMemoryError::InvalidValue {
                field: "boundary_lookup.min_confidence_milli",
                reason: "min_confidence_milli must be in [0, 1000]",
            });
        }
        require_nonempty(&self.allowed_actions, "boundary_lookup.allowed_actions")?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PrefetchPlanRequest {
    pub request_id: String,
    pub model: InferenceModelBinding,
    pub boundary: RangeBoundary,
    pub engram_state_id: Option<String>,
    pub scope: PrefetchScope,
    pub lookahead_steps: u32,
    pub artifact_kinds: Vec<ExecutionArtifactKind>,
    pub created_at_us: u64,
}

impl PrefetchPlanRequest {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.request_id, "prefetch_plan.request_id")?;
        self.model.validate()?;
        self.boundary.validate()?;
        if self.boundary.phase != RangeBoundaryPhase::RangeStart {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefetch_plan.boundary.phase",
                reason: "prefetch planning must be issued from a range start boundary",
            });
        }
        if let Some(engram_state_id) = &self.engram_state_id {
            required_str(engram_state_id, "prefetch_plan.engram_state_id")?;
        }
        nonzero(
            u64::from(self.lookahead_steps),
            "prefetch_plan.lookahead_steps",
        )?;
        require_nonempty(&self.artifact_kinds, "prefetch_plan.artifact_kinds")?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PrefetchPlanRecord {
    pub plan_id: String,
    pub request_id: String,
    pub model: InferenceModelBinding,
    pub boundary: RangeBoundary,
    pub engram_state_id: Option<String>,
    pub scope: PrefetchScope,
    pub lookahead_steps: u32,
    pub target_step_index: u64,
    pub target_position: u64,
    pub artifact_kinds: Vec<ExecutionArtifactKind>,
    pub planned_artifact_ids: Vec<String>,
    pub state: PrefetchPlanState,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl PrefetchPlanRecord {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.plan_id, "prefetch_plan.plan_id")?;
        required_str(&self.request_id, "prefetch_plan.request_id")?;
        self.model.validate()?;
        self.boundary.validate()?;
        if self.boundary.phase != RangeBoundaryPhase::RangeStart {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefetch_plan.boundary.phase",
                reason: "prefetch plans must originate from a range start boundary",
            });
        }
        if let Some(engram_state_id) = &self.engram_state_id {
            required_str(engram_state_id, "prefetch_plan.engram_state_id")?;
        }
        nonzero(
            u64::from(self.lookahead_steps),
            "prefetch_plan.lookahead_steps",
        )?;
        require_nonempty(&self.artifact_kinds, "prefetch_plan.artifact_kinds")?;
        if self.target_step_index < self.boundary.step_index {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefetch_plan.target_step_index",
                reason: "target step must be >= boundary step",
            });
        }
        nonzero(self.checksum, "prefetch_plan.checksum")?;
        nonzero(self.version, "prefetch_plan.version")?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefetch_plan.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        let expected_checksum = prefetch_plan_checksum(self);
        if expected_checksum != self.checksum {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefetch_plan.checksum",
                reason: "checksum does not match prefetch plan metadata",
            });
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PrefixCacheLookupRequest {
    pub request_id: String,
    pub candidate_keys: Vec<PrefixCacheKey>,
    pub min_confidence_milli: u32,
    pub allow_verify: bool,
    pub created_at_us: u64,
}

impl PrefixCacheLookupRequest {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.request_id, "prefix_cache_lookup.request_id")?;
        require_nonempty(&self.candidate_keys, "prefix_cache_lookup.candidate_keys")?;
        for key in &self.candidate_keys {
            key.validate()?;
        }
        if self.min_confidence_milli > 1000 {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_lookup.min_confidence_milli",
                reason: "confidence_milli must be in [0, 1000]",
            });
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PrefixCacheReusePlan {
    pub plan_id: String,
    pub request_id: String,
    pub action: PrefixCacheReuseAction,
    pub artifact_id: Option<String>,
    pub matched_prefix_token_count: u64,
    pub layer_start: u32,
    pub layer_end: u32,
    pub position_start: u64,
    pub position_end: u64,
    pub confidence_milli: u32,
    pub verify_required: bool,
    pub proof_checksum: u64,
    pub reason: String,
    pub created_at_us: u64,
    pub version: u64,
}

impl PrefixCacheReusePlan {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.plan_id, "prefix_cache_reuse.plan_id")?;
        required_str(&self.request_id, "prefix_cache_reuse.request_id")?;
        required_str(&self.reason, "prefix_cache_reuse.reason")?;
        if self.layer_end < self.layer_start {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_reuse.layer_end",
                reason: "layer_end must be >= layer_start",
            });
        }
        if self.position_end < self.position_start {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_reuse.position_end",
                reason: "position_end must be >= position_start",
            });
        }
        if self.confidence_milli > 1000 {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_reuse.confidence_milli",
                reason: "confidence_milli must be in [0, 1000]",
            });
        }
        match self.action {
            PrefixCacheReuseAction::Miss => {
                if self.artifact_id.is_some()
                    || self.matched_prefix_token_count != 0
                    || self.confidence_milli != 0
                {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "prefix_cache_reuse.action",
                        reason: "miss must not reference an artifact or matched tokens",
                    });
                }
            }
            PrefixCacheReuseAction::Reuse | PrefixCacheReuseAction::RequireVerify => {
                if self.artifact_id.as_deref().unwrap_or("").trim().is_empty() {
                    return Err(LingquMemoryError::MissingField(
                        "prefix_cache_reuse.artifact_id",
                    ));
                }
                nonzero(
                    self.matched_prefix_token_count,
                    "prefix_cache_reuse.matched_prefix_token_count",
                )?;
                if self.layer_end <= self.layer_start {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "prefix_cache_reuse.layer_end",
                        reason: "reuse layer_end must be greater than layer_start",
                    });
                }
                if self.position_end <= self.position_start {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "prefix_cache_reuse.position_end",
                        reason: "reuse position_end must be greater than position_start",
                    });
                }
                if self.action == PrefixCacheReuseAction::RequireVerify && !self.verify_required {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "prefix_cache_reuse.verify_required",
                        reason: "require-verify plans must set verify_required",
                    });
                }
            }
        }
        nonzero(self.proof_checksum, "prefix_cache_reuse.proof_checksum")?;
        nonzero(self.version, "prefix_cache_reuse.version")?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PrefixCacheLookupResponse {
    pub request_id: String,
    pub reuse_plan: PrefixCacheReusePlan,
    pub artifact: Option<PrefixCacheArtifact>,
}

impl PrefixCacheLookupResponse {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.request_id, "prefix_cache_lookup_response.request_id")?;
        self.reuse_plan.validate()?;
        if self.reuse_plan.request_id != self.request_id {
            return Err(LingquMemoryError::InvalidValue {
                field: "prefix_cache_lookup_response.reuse_plan",
                reason: "reuse plan request_id must match response request_id",
            });
        }
        if let Some(artifact) = &self.artifact {
            artifact.validate()?;
            if self.reuse_plan.artifact_id.as_deref() != Some(artifact.artifact_id.as_str()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefix_cache_lookup_response.artifact",
                    reason: "artifact id must match reuse plan artifact id",
                });
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ShortpathDecisionRecord {
    pub decision_id: String,
    pub request_id: String,
    pub action: ShortpathAction,
    pub artifact_id: Option<String>,
    pub target_layer_start: Option<u32>,
    pub target_layer_end: Option<u32>,
    pub confidence_milli: u32,
    pub verify_required: bool,
    pub proof_checksum: u64,
    pub reason: String,
    pub created_at_us: u64,
    pub version: u64,
}

impl ShortpathDecisionRecord {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.decision_id, "shortpath_decision.decision_id")?;
        required_str(&self.request_id, "shortpath_decision.request_id")?;
        required_str(&self.reason, "shortpath_decision.reason")?;
        if self.confidence_milli > 1000 {
            return Err(LingquMemoryError::InvalidValue {
                field: "shortpath_decision.confidence_milli",
                reason: "confidence_milli must be in [0, 1000]",
            });
        }
        match self.action {
            ShortpathAction::Continue => {
                if self.artifact_id.is_some() {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "shortpath_decision.artifact_id",
                        reason: "continue decisions must not reference an artifact",
                    });
                }
            }
            ShortpathAction::JumpToLayer | ShortpathAction::JumpToTerminal => {
                if self.artifact_id.as_deref().unwrap_or("").trim().is_empty() {
                    return Err(LingquMemoryError::MissingField(
                        "shortpath_decision.artifact_id",
                    ));
                }
                let (Some(start), Some(end)) = (self.target_layer_start, self.target_layer_end)
                else {
                    return Err(LingquMemoryError::MissingField(
                        "shortpath_decision.target_layer_range",
                    ));
                };
                if end < start || (self.action == ShortpathAction::JumpToLayer && end == start) {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "shortpath_decision.target_layer_end",
                        reason: "target_layer_end must be greater than target_layer_start unless action is jump-to-terminal",
                    });
                }
            }
            ShortpathAction::RequireVerify => {
                if self.artifact_id.as_deref().unwrap_or("").trim().is_empty() {
                    return Err(LingquMemoryError::MissingField(
                        "shortpath_decision.artifact_id",
                    ));
                }
                if !self.verify_required {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "shortpath_decision.verify_required",
                        reason: "require-verify decisions must set verify_required",
                    });
                }
            }
        }
        nonzero(self.proof_checksum, "shortpath_decision.proof_checksum")?;
        nonzero(self.version, "shortpath_decision.version")?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BoundaryLookupResponse {
    pub request_id: String,
    pub decision: ShortpathDecisionRecord,
    pub artifact: Option<ExecutionArtifactObject>,
}

impl BoundaryLookupResponse {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.request_id, "boundary_lookup_response.request_id")?;
        self.decision.validate()?;
        if self.decision.request_id != self.request_id {
            return Err(LingquMemoryError::InvalidValue {
                field: "boundary_lookup_response.decision",
                reason: "decision request_id must match response request_id",
            });
        }
        if let Some(artifact) = &self.artifact {
            artifact.validate()?;
            if self.decision.artifact_id.as_deref() != Some(artifact.artifact_id.as_str()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "boundary_lookup_response.artifact",
                    reason: "artifact id must match decision artifact id",
                });
            }
        }
        Ok(())
    }
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
    pub compatible_models: Vec<InferenceModelBinding>,
    pub owner_entity: u64,
    pub producer_entity: u64,
    pub now_us: u64,
    pub expires_at_us: Option<u64>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct EngramStateMaterializeFromBlockReq {
    pub state_id: String,
    pub hot_memory_state_id: String,
    pub gate_weight_ref: LingquBlockPayloadRef,
    pub compatible_models: Vec<InferenceModelBinding>,
    pub owner_entity: u64,
    pub producer_entity: u64,
    pub now_us: u64,
    pub expires_at_us: Option<u64>,
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
    execution_artifacts: HashMap<String, ExecutionArtifactObject>,
    prefix_cache_artifacts: HashMap<String, PrefixCacheArtifact>,
    prefix_cache_reuse_plans: HashMap<String, PrefixCacheReusePlan>,
    prefetch_plans: HashMap<String, PrefetchPlanRecord>,
    shortpath_decisions: HashMap<String, ShortpathDecisionRecord>,
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

    pub fn persist_execution_artifacts_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut artifacts = self
            .execution_artifacts
            .values()
            .cloned()
            .collect::<Vec<_>>();
        artifacts.sort_by(|left, right| left.artifact_id.cmp(&right.artifact_id));
        durable_store.persist_execution_artifact_manifest(artifacts)
    }

    pub fn rebuild_execution_artifacts_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<ExecutionArtifactObject>> {
        let artifacts = durable_store.load_execution_artifact_manifest()?;
        for artifact in &artifacts {
            self.register_execution_artifact(artifact.clone())?;
        }
        Ok(artifacts)
    }

    pub fn persist_prefix_cache_artifacts_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut artifacts = self
            .prefix_cache_artifacts
            .values()
            .cloned()
            .collect::<Vec<_>>();
        artifacts.sort_by(|left, right| left.artifact_id.cmp(&right.artifact_id));
        durable_store.persist_prefix_cache_manifest(artifacts)
    }

    pub fn rebuild_prefix_cache_artifacts_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PrefixCacheArtifact>> {
        let artifacts = durable_store.load_prefix_cache_manifest()?;
        for artifact in &artifacts {
            self.register_prefix_cache_artifact(artifact.clone())?;
        }
        Ok(artifacts)
    }

    pub fn persist_shortpath_decisions_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut decisions = self
            .shortpath_decisions
            .values()
            .cloned()
            .collect::<Vec<_>>();
        decisions.sort_by(|left, right| left.decision_id.cmp(&right.decision_id));
        durable_store.persist_shortpath_decision_manifest(decisions)
    }

    pub fn rebuild_shortpath_decisions_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<ShortpathDecisionRecord>> {
        let decisions = durable_store.load_shortpath_decision_manifest()?;
        for decision in &decisions {
            decision.validate()?;
            self.shortpath_decisions
                .insert(decision.decision_id.clone(), decision.clone());
        }
        Ok(decisions)
    }

    pub fn persist_prefetch_plans_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut plans = self.prefetch_plans.values().cloned().collect::<Vec<_>>();
        plans.sort_by(|left, right| left.plan_id.cmp(&right.plan_id));
        durable_store.persist_prefetch_plan_manifest(plans)
    }

    pub fn rebuild_prefetch_plans_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PrefetchPlanRecord>> {
        let plans = durable_store.load_prefetch_plan_manifest()?;
        for plan in &plans {
            plan.validate()?;
            self.prefetch_plans
                .insert(plan.plan_id.clone(), plan.clone());
        }
        Ok(plans)
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

    pub fn register_query_result(&mut self, result: QueryResult) -> MemoryResult<()> {
        result.validate()?;
        for vector_index_id in &result.vector_index_ids {
            if !self.vector_indexes.contains_key(vector_index_id) {
                return Err(LingquMemoryError::MissingVectorIndex(
                    vector_index_id.clone(),
                ));
            }
        }
        for query_match in &result.matches {
            if !self
                .vector_indexes
                .contains_key(&query_match.vector_index_id)
            {
                return Err(LingquMemoryError::MissingVectorIndex(
                    query_match.vector_index_id.clone(),
                ));
            }
            let chunk = self
                .chunks
                .get(&query_match.chunk_id)
                .ok_or_else(|| LingquMemoryError::MissingChunk(query_match.chunk_id.clone()))?;
            let record = self
                .records
                .get(&query_match.record_id)
                .ok_or_else(|| LingquMemoryError::MissingRecord(query_match.record_id.clone()))?;
            if chunk.record_id != record.record_id {
                return Err(LingquMemoryError::InvalidValue {
                    field: "query_match.record_id",
                    reason: "query match record must own the matched chunk",
                });
            }
            let segment = self
                .embedding_segments
                .get(&query_match.segment_id)
                .ok_or_else(|| {
                    LingquMemoryError::MissingEmbeddingSegment(query_match.segment_id.clone())
                })?;
            if !segment
                .row_map
                .iter()
                .any(|row| row.chunk_id == query_match.chunk_id && row.row == query_match.row)
            {
                return Err(LingquMemoryError::InvalidValue {
                    field: "query_match.row",
                    reason: "query match row must reference the matched chunk",
                });
            }
        }
        for segment_version in &result.embedding_segment_versions {
            let segment = self
                .embedding_segments
                .get(&segment_version.segment_id)
                .ok_or_else(|| {
                    LingquMemoryError::MissingEmbeddingSegment(segment_version.segment_id.clone())
                })?;
            if segment.version != segment_version.version
                || segment.checksum != segment_version.checksum
            {
                return Err(LingquMemoryError::PayloadChecksumMismatch {
                    id: segment.segment_id.clone(),
                    expected: segment.checksum,
                    actual: segment_version.checksum,
                });
            }
        }
        self.query_results.insert(result.result_id.clone(), result);
        Ok(())
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

    pub fn register_hot_state(
        &mut self,
        object_service: &LingquObjectServiceStub,
        state: HotMemoryStateObject,
    ) -> MemoryResult<()> {
        state.validate()?;
        validate_hot_object_record(object_service, &state.table, "hot_state.table")?;
        validate_hot_object_record(object_service, &state.indices, "hot_state.indices")?;
        validate_hot_object_record(object_service, &state.scores, "hot_state.scores")?;
        self.hot_states.insert(state.state_id.clone(), state);
        Ok(())
    }

    pub fn build_engram_state(
        &mut self,
        state_id: impl Into<String>,
        hot_memory_state_id: &str,
        gate: Option<HotTensorObjectRef>,
        compatible_models: Vec<InferenceModelBinding>,
        created_at_us: u64,
        expires_at_us: Option<u64>,
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
        for model in &compatible_models {
            model.validate()?;
        }
        let table_shape = &hot_state.table.shape;
        if table_shape.len() != 2 {
            return Err(LingquMemoryError::InvalidValue {
                field: "hot_state.table.shape",
                reason: "table shape must be [rows, hidden_size]",
            });
        }
        let table_rows = table_shape[0];
        let hidden_size = table_shape[1];
        let operator_kind = EngramOperatorKind::ContextGate;
        let operator_config_hash = engram_operator_config_hash(
            operator_kind,
            &hot_state.table,
            &hot_state.indices,
            gate.as_ref(),
        );
        let checksum = engram_state_checksum(
            &state_id,
            hot_memory_state_id,
            &hot_state.query_result_id,
            operator_kind,
            operator_config_hash,
            &compatible_models,
            &hot_state.table,
            &hot_state.indices,
            gate.as_ref(),
            hot_state.table.dtype,
            hidden_size,
            table_rows,
            1,
            created_at_us,
            expires_at_us,
        );
        let engram = EngramStateObject {
            state_id,
            hot_memory_state_id: hot_memory_state_id.to_string(),
            query_result_id: hot_state.query_result_id.clone(),
            query_result_manifest_ref: hot_state.query_result_manifest_ref.clone(),
            operator_kind,
            operator_config_hash,
            compatible_models,
            table: hot_state.table.clone(),
            indices: hot_state.indices.clone(),
            gate,
            dtype: hot_state.table.dtype,
            hidden_size,
            table_rows,
            execution_artifact_index_ref: None,
            checksum,
            version: 1,
            created_at_us,
            expires_at_us,
        };
        engram.validate()?;
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
            req.compatible_models,
            req.now_us,
            req.expires_at_us,
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
                compatible_models: req.compatible_models,
                owner_entity: req.owner_entity,
                producer_entity: req.producer_entity,
                now_us: req.now_us,
                expires_at_us: req.expires_at_us,
            },
        )
    }

    pub fn register_execution_artifact(
        &mut self,
        artifact: ExecutionArtifactObject,
    ) -> MemoryResult<()> {
        artifact.validate()?;
        if let Some(query_result_id) = &artifact.source_query_result_id {
            if !self.query_results.contains_key(query_result_id) {
                return Err(LingquMemoryError::MissingQueryResult(
                    query_result_id.clone(),
                ));
            }
        }
        if let Some(engram_state_id) = &artifact.source_engram_state_id {
            if !self.engram_states.contains_key(engram_state_id) {
                return Err(LingquMemoryError::MissingField(
                    "execution_artifact.source_engram_state_id",
                ));
            }
        }
        self.execution_artifacts
            .insert(artifact.artifact_id.clone(), artifact);
        Ok(())
    }

    pub fn register_prefix_cache_artifact(
        &mut self,
        artifact: PrefixCacheArtifact,
    ) -> MemoryResult<()> {
        artifact.validate()?;
        for artifact_id in &artifact.kv_artifact_ids {
            let Some(kv_artifact) = self.execution_artifacts.get(artifact_id) else {
                return Err(LingquMemoryError::MissingExecutionArtifact(
                    artifact_id.clone(),
                ));
            };
            if kv_artifact.kind != ExecutionArtifactKind::KvCache {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefix_cache_artifact.kv_artifact_ids",
                    reason: "referenced execution artifacts must be kv_cache artifacts",
                });
            }
            if kv_artifact.model != artifact.key.model {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefix_cache_artifact.kv_artifact_ids",
                    reason: "referenced kv artifact model must match prefix cache key",
                });
            }
        }
        self.prefix_cache_artifacts
            .insert(artifact.artifact_id.clone(), artifact);
        Ok(())
    }

    pub fn lookup_prefix_cache(
        &mut self,
        req: PrefixCacheLookupRequest,
        now_us: u64,
    ) -> MemoryResult<PrefixCacheLookupResponse> {
        req.validate()?;
        let candidate = self
            .prefix_cache_artifacts
            .values()
            .filter(|artifact| {
                artifact.state == ExecutionArtifactState::Verified
                    || (req.allow_verify && artifact.state == ExecutionArtifactState::Candidate)
            })
            .filter(|artifact| artifact.confidence_milli >= req.min_confidence_milli)
            .filter(|artifact| {
                artifact
                    .expires_at_us
                    .map(|expires_at_us| expires_at_us > now_us)
                    .unwrap_or(true)
            })
            .filter(|artifact| req.candidate_keys.iter().any(|key| key == &artifact.key))
            .max_by(|left, right| {
                left.key
                    .prefix_token_count
                    .cmp(&right.key.prefix_token_count)
                    .then_with(|| left.confidence_milli.cmp(&right.confidence_milli))
                    .then_with(|| left.version.cmp(&right.version))
                    .then_with(|| left.artifact_id.cmp(&right.artifact_id))
            })
            .cloned();

        let reuse_plan = if let Some(artifact) = candidate.as_ref() {
            let action = if artifact.state == ExecutionArtifactState::Verified {
                PrefixCacheReuseAction::Reuse
            } else {
                PrefixCacheReuseAction::RequireVerify
            };
            let plan_id = format!("prefix-cache-reuse/{}", req.request_id);
            let proof_checksum = prefix_cache_reuse_plan_checksum(
                &plan_id,
                &req.request_id,
                action,
                Some(&artifact.artifact_id),
                artifact.key.prefix_token_count,
                artifact.key.layer_start,
                artifact.key.layer_end,
                artifact.key.position_start,
                artifact.key.position_end,
                artifact.confidence_milli,
                artifact.checksum,
                now_us,
            );
            PrefixCacheReusePlan {
                plan_id,
                request_id: req.request_id.clone(),
                action,
                artifact_id: Some(artifact.artifact_id.clone()),
                matched_prefix_token_count: artifact.key.prefix_token_count,
                layer_start: artifact.key.layer_start,
                layer_end: artifact.key.layer_end,
                position_start: artifact.key.position_start,
                position_end: artifact.key.position_end,
                confidence_milli: artifact.confidence_milli,
                verify_required: artifact.state != ExecutionArtifactState::Verified,
                proof_checksum,
                reason: "prefix_cache_hit".to_string(),
                created_at_us: now_us,
                version: 1,
            }
        } else {
            let plan_id = format!("prefix-cache-reuse/{}", req.request_id);
            let proof_checksum = prefix_cache_reuse_plan_checksum(
                &plan_id,
                &req.request_id,
                PrefixCacheReuseAction::Miss,
                None,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                now_us,
            );
            PrefixCacheReusePlan {
                plan_id,
                request_id: req.request_id.clone(),
                action: PrefixCacheReuseAction::Miss,
                artifact_id: None,
                matched_prefix_token_count: 0,
                layer_start: 0,
                layer_end: 0,
                position_start: 0,
                position_end: 0,
                confidence_milli: 0,
                verify_required: false,
                proof_checksum,
                reason: "prefix_cache_miss".to_string(),
                created_at_us: now_us,
                version: 1,
            }
        };
        reuse_plan.validate()?;
        self.prefix_cache_reuse_plans
            .insert(reuse_plan.plan_id.clone(), reuse_plan.clone());
        let response = PrefixCacheLookupResponse {
            request_id: req.request_id,
            reuse_plan,
            artifact: candidate,
        };
        response.validate()?;
        Ok(response)
    }

    pub fn boundary_lookup(
        &mut self,
        req: BoundaryLookupRequest,
        now_us: u64,
    ) -> MemoryResult<BoundaryLookupResponse> {
        req.validate()?;
        if req.boundary.phase != RangeBoundaryPhase::RangeExit {
            return Err(LingquMemoryError::InvalidValue {
                field: "boundary_lookup.boundary.phase",
                reason: "shortpath lookup must be issued from a range exit boundary",
            });
        }
        if let Some(engram_state_id) = &req.engram_state_id {
            if !self.engram_states.contains_key(engram_state_id) {
                return Err(LingquMemoryError::MissingField(
                    "boundary_lookup.engram_state_id",
                ));
            }
        }

        let allow_terminal = req
            .allowed_actions
            .iter()
            .any(|action| *action == ShortpathAction::JumpToTerminal);
        let allow_layer = req
            .allowed_actions
            .iter()
            .any(|action| *action == ShortpathAction::JumpToLayer);
        let candidate = self
            .execution_artifacts
            .values()
            .filter(|artifact| artifact.state == ExecutionArtifactState::Verified)
            .filter(|artifact| artifact.model == req.model)
            .filter(|artifact| artifact.producer_boundary.layer_end == req.boundary.layer_end)
            .filter(|artifact| artifact.producer_boundary.position == req.boundary.position)
            .filter(|artifact| artifact.confidence_milli >= req.min_confidence_milli)
            .filter(|artifact| {
                if let Some(engram_state_id) = &req.engram_state_id {
                    artifact.source_engram_state_id.as_deref() == Some(engram_state_id.as_str())
                } else {
                    true
                }
            })
            .filter(|artifact| match artifact.kind {
                ExecutionArtifactKind::Logits => allow_terminal,
                ExecutionArtifactKind::HiddenState => allow_layer,
                ExecutionArtifactKind::KvCache => false,
            })
            .max_by(|left, right| {
                left.confidence_milli
                    .cmp(&right.confidence_milli)
                    .then_with(|| left.version.cmp(&right.version))
                    .then_with(|| left.artifact_id.cmp(&right.artifact_id))
            })
            .cloned();

        let decision = if let Some(artifact) = candidate.as_ref() {
            let action = if artifact.kind == ExecutionArtifactKind::Logits {
                ShortpathAction::JumpToTerminal
            } else {
                ShortpathAction::JumpToLayer
            };
            let decision_id = format!("shortpath-decision/{}", req.request_id);
            let proof_checksum = shortpath_decision_checksum(
                &decision_id,
                &req.request_id,
                action,
                Some(&artifact.artifact_id),
                artifact.target_layer_start,
                artifact.target_layer_end,
                artifact.confidence_milli,
                artifact.checksum,
                now_us,
            );
            ShortpathDecisionRecord {
                decision_id,
                request_id: req.request_id.clone(),
                action,
                artifact_id: Some(artifact.artifact_id.clone()),
                target_layer_start: Some(artifact.target_layer_start),
                target_layer_end: Some(artifact.target_layer_end),
                confidence_milli: artifact.confidence_milli,
                verify_required: artifact.state != ExecutionArtifactState::Verified,
                proof_checksum,
                reason: "verified_execution_artifact_hit".to_string(),
                created_at_us: now_us,
                version: 1,
            }
        } else {
            let decision_id = format!("shortpath-decision/{}", req.request_id);
            let proof_checksum = shortpath_decision_checksum(
                &decision_id,
                &req.request_id,
                ShortpathAction::Continue,
                None,
                req.boundary.layer_start,
                req.boundary.layer_end,
                0,
                req.hidden_state.checksum,
                now_us,
            );
            ShortpathDecisionRecord {
                decision_id,
                request_id: req.request_id.clone(),
                action: ShortpathAction::Continue,
                artifact_id: None,
                target_layer_start: None,
                target_layer_end: None,
                confidence_milli: 0,
                verify_required: false,
                proof_checksum,
                reason: "no_verified_execution_artifact_hit".to_string(),
                created_at_us: now_us,
                version: 1,
            }
        };
        decision.validate()?;
        self.shortpath_decisions
            .insert(decision.decision_id.clone(), decision.clone());
        let response = BoundaryLookupResponse {
            request_id: req.request_id,
            decision,
            artifact: candidate,
        };
        response.validate()?;
        Ok(response)
    }

    pub fn plan_prefetch(
        &mut self,
        req: PrefetchPlanRequest,
        now_us: u64,
    ) -> MemoryResult<PrefetchPlanRecord> {
        req.validate()?;
        if let Some(engram_state_id) = &req.engram_state_id {
            if !self.engram_states.contains_key(engram_state_id) {
                return Err(LingquMemoryError::MissingField(
                    "prefetch_plan.engram_state_id",
                ));
            }
        }
        let target_step_index = match req.scope {
            PrefetchScope::Range => req.boundary.step_index,
            PrefetchScope::Step | PrefetchScope::MultiStep => req
                .boundary
                .step_index
                .saturating_add(u64::from(req.lookahead_steps)),
        };
        let mut planned_artifact_ids = self
            .execution_artifacts
            .values()
            .filter(|artifact| artifact.model == req.model)
            .filter(|artifact| artifact.producer_boundary.position >= req.boundary.position)
            .filter(|artifact| artifact.producer_boundary.step_index <= target_step_index)
            .filter(|artifact| req.artifact_kinds.contains(&artifact.kind))
            .filter(|artifact| {
                if let Some(engram_state_id) = &req.engram_state_id {
                    artifact.source_engram_state_id.as_deref() == Some(engram_state_id.as_str())
                } else {
                    true
                }
            })
            .map(|artifact| artifact.artifact_id.clone())
            .collect::<Vec<_>>();
        planned_artifact_ids.sort();
        let mut plan = PrefetchPlanRecord {
            plan_id: format!("prefetch-plan/{}", req.request_id),
            request_id: req.request_id,
            model: req.model,
            boundary: req.boundary.clone(),
            engram_state_id: req.engram_state_id,
            scope: req.scope,
            lookahead_steps: req.lookahead_steps,
            target_step_index,
            target_position: req
                .boundary
                .position
                .saturating_add(u64::from(req.lookahead_steps)),
            artifact_kinds: req.artifact_kinds,
            planned_artifact_ids,
            state: PrefetchPlanState::Planned,
            checksum: 0,
            version: 1,
            created_at_us: now_us,
            expires_at_us: None,
        };
        plan.checksum = prefetch_plan_checksum(&plan);
        plan.validate()?;
        self.prefetch_plans
            .insert(plan.plan_id.clone(), plan.clone());
        Ok(plan)
    }

    pub fn record(&self, record_id: &str) -> Option<&MemoryRecord> {
        self.records.get(record_id)
    }

    pub fn query_result(&self, result_id: &str) -> Option<&QueryResult> {
        self.query_results.get(result_id)
    }

    pub fn execution_artifact(&self, artifact_id: &str) -> Option<&ExecutionArtifactObject> {
        self.execution_artifacts.get(artifact_id)
    }

    pub fn prefix_cache_artifact(&self, artifact_id: &str) -> Option<&PrefixCacheArtifact> {
        self.prefix_cache_artifacts.get(artifact_id)
    }

    pub fn prefix_cache_reuse_plan(&self, plan_id: &str) -> Option<&PrefixCacheReusePlan> {
        self.prefix_cache_reuse_plans.get(plan_id)
    }

    pub fn shortpath_decision(&self, decision_id: &str) -> Option<&ShortpathDecisionRecord> {
        self.shortpath_decisions.get(decision_id)
    }

    pub fn prefetch_plan(&self, plan_id: &str) -> Option<&PrefetchPlanRecord> {
        self.prefetch_plans.get(plan_id)
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

fn validate_hot_object_record(
    object_service: &LingquObjectServiceStub,
    hot_ref: &HotTensorObjectRef,
    field: &'static str,
) -> MemoryResult<()> {
    validate_hot_ref(hot_ref)?;
    let record = object_service
        .latest_record(&hot_ref.object_key)
        .ok_or_else(|| LingquMemoryError::MissingObject(hot_ref.object_key.clone()))?;
    if record.version != hot_ref.version {
        return Err(LingquMemoryError::InvalidValue {
            field,
            reason: "object version does not match hot ref",
        });
    }
    if record.bytes != hot_ref.bytes || record.checksum != hot_ref.checksum {
        return Err(LingquMemoryError::InvalidValue {
            field,
            reason: "object bytes/checksum does not match hot ref",
        });
    }
    if record.dtype != Some(hot_ref.dtype) || record.shape != hot_ref.shape {
        return Err(LingquMemoryError::InvalidValue {
            field,
            reason: "object dtype/shape does not match hot ref",
        });
    }
    let placement = record
        .placements
        .iter()
        .find(|placement| placement.backend == LingquPayloadBackend::ObmmShmem)
        .ok_or_else(|| LingquMemoryError::NonObmmHotPlacement(hot_ref.object_key.clone()))?;
    if placement.storage_ref != hot_ref.storage_ref
        || placement.segment != hot_ref.segment
        || placement.offset != hot_ref.offset
        || placement.bytes != hot_ref.bytes
        || placement.checksum != hot_ref.checksum
    {
        return Err(LingquMemoryError::InvalidValue {
            field,
            reason: "object placement does not match hot ref",
        });
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

fn engram_operator_config_hash(
    operator_kind: EngramOperatorKind,
    table: &HotTensorObjectRef,
    indices: &HotTensorObjectRef,
    gate: Option<&HotTensorObjectRef>,
) -> u64 {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&engram_operator_kind_tag(operator_kind).to_le_bytes());
    bytes.extend_from_slice(&table.checksum.to_le_bytes());
    bytes.extend_from_slice(&indices.checksum.to_le_bytes());
    if let Some(gate) = gate {
        bytes.extend_from_slice(&gate.checksum.to_le_bytes());
    }
    checksum64(&bytes)
}

fn engram_state_checksum(
    state_id: &str,
    hot_memory_state_id: &str,
    query_result_id: &str,
    operator_kind: EngramOperatorKind,
    operator_config_hash: u64,
    compatible_models: &[InferenceModelBinding],
    table: &HotTensorObjectRef,
    indices: &HotTensorObjectRef,
    gate: Option<&HotTensorObjectRef>,
    dtype: TensorDType,
    hidden_size: u64,
    table_rows: u64,
    version: u64,
    created_at_us: u64,
    expires_at_us: Option<u64>,
) -> u64 {
    fn push_str(bytes: &mut Vec<u8>, value: &str) {
        bytes.extend_from_slice(&(value.len() as u64).to_le_bytes());
        bytes.extend_from_slice(value.as_bytes());
    }

    let mut bytes = Vec::new();
    push_str(&mut bytes, state_id);
    push_str(&mut bytes, hot_memory_state_id);
    push_str(&mut bytes, query_result_id);
    bytes.extend_from_slice(&engram_operator_kind_tag(operator_kind).to_le_bytes());
    bytes.extend_from_slice(&operator_config_hash.to_le_bytes());
    for model in compatible_models {
        push_str(&mut bytes, &model.model_id);
        push_str(&mut bytes, &model.model_key);
        bytes.extend_from_slice(&model.tokenizer_hash.to_le_bytes());
        bytes.extend_from_slice(&model.profile_hash.to_le_bytes());
    }
    bytes.extend_from_slice(&table.checksum.to_le_bytes());
    bytes.extend_from_slice(&indices.checksum.to_le_bytes());
    if let Some(gate) = gate {
        bytes.extend_from_slice(&gate.checksum.to_le_bytes());
    }
    bytes.extend_from_slice(&tensor_dtype_tag(dtype).to_le_bytes());
    bytes.extend_from_slice(&hidden_size.to_le_bytes());
    bytes.extend_from_slice(&table_rows.to_le_bytes());
    bytes.extend_from_slice(&version.to_le_bytes());
    bytes.extend_from_slice(&created_at_us.to_le_bytes());
    bytes.extend_from_slice(&expires_at_us.unwrap_or(0).to_le_bytes());
    checksum64(&bytes)
}

fn engram_operator_kind_tag(kind: EngramOperatorKind) -> u64 {
    match kind {
        EngramOperatorKind::ContextGate => 1,
    }
}

fn tensor_dtype_tag(dtype: TensorDType) -> u64 {
    match dtype {
        TensorDType::U8 => 1,
        TensorDType::U32 => 2,
        TensorDType::U64 => 3,
        TensorDType::F32 => 4,
        TensorDType::Opaque => 5,
    }
}

fn prefetch_plan_checksum(plan: &PrefetchPlanRecord) -> u64 {
    fn push_str(bytes: &mut Vec<u8>, value: &str) {
        bytes.extend_from_slice(&(value.len() as u64).to_le_bytes());
        bytes.extend_from_slice(value.as_bytes());
    }

    let mut bytes = Vec::new();
    push_str(&mut bytes, &plan.plan_id);
    push_str(&mut bytes, &plan.request_id);
    push_str(&mut bytes, &plan.model.model_id);
    push_str(&mut bytes, &plan.model.model_key);
    bytes.extend_from_slice(&plan.model.tokenizer_hash.to_le_bytes());
    bytes.extend_from_slice(&plan.model.profile_hash.to_le_bytes());
    bytes.extend_from_slice(&range_boundary_phase_tag(plan.boundary.phase).to_le_bytes());
    bytes.extend_from_slice(&plan.boundary.step_index.to_le_bytes());
    bytes.extend_from_slice(&plan.boundary.node_index.to_le_bytes());
    bytes.extend_from_slice(&plan.boundary.layer_start.to_le_bytes());
    bytes.extend_from_slice(&plan.boundary.layer_end.to_le_bytes());
    bytes.extend_from_slice(
        &plan
            .boundary
            .next_node_index
            .unwrap_or(u32::MAX)
            .to_le_bytes(),
    );
    bytes.extend_from_slice(&plan.boundary.position.to_le_bytes());
    if let Some(engram_state_id) = &plan.engram_state_id {
        push_str(&mut bytes, engram_state_id);
    }
    bytes.extend_from_slice(&prefetch_scope_tag(plan.scope).to_le_bytes());
    bytes.extend_from_slice(&plan.lookahead_steps.to_le_bytes());
    bytes.extend_from_slice(&plan.target_step_index.to_le_bytes());
    bytes.extend_from_slice(&plan.target_position.to_le_bytes());
    for kind in &plan.artifact_kinds {
        bytes.extend_from_slice(&execution_artifact_kind_tag(*kind).to_le_bytes());
    }
    for artifact_id in &plan.planned_artifact_ids {
        push_str(&mut bytes, artifact_id);
    }
    bytes.extend_from_slice(&prefetch_plan_state_tag(plan.state).to_le_bytes());
    bytes.extend_from_slice(&plan.version.to_le_bytes());
    bytes.extend_from_slice(&plan.created_at_us.to_le_bytes());
    bytes.extend_from_slice(&plan.expires_at_us.unwrap_or(0).to_le_bytes());
    checksum64(&bytes)
}

fn range_boundary_phase_tag(phase: RangeBoundaryPhase) -> u64 {
    match phase {
        RangeBoundaryPhase::RangeStart => 1,
        RangeBoundaryPhase::RangeExit => 2,
    }
}

fn prefetch_scope_tag(scope: PrefetchScope) -> u64 {
    match scope {
        PrefetchScope::Range => 1,
        PrefetchScope::Step => 2,
        PrefetchScope::MultiStep => 3,
    }
}

fn prefetch_plan_state_tag(state: PrefetchPlanState) -> u64 {
    match state {
        PrefetchPlanState::Planned => 1,
        PrefetchPlanState::Issued => 2,
        PrefetchPlanState::Completed => 3,
        PrefetchPlanState::Cancelled => 4,
    }
}

fn execution_artifact_kind_tag(kind: ExecutionArtifactKind) -> u64 {
    match kind {
        ExecutionArtifactKind::HiddenState => 1,
        ExecutionArtifactKind::KvCache => 2,
        ExecutionArtifactKind::Logits => 3,
    }
}

fn prefix_cache_reuse_plan_checksum(
    plan_id: &str,
    request_id: &str,
    action: PrefixCacheReuseAction,
    artifact_id: Option<&str>,
    matched_prefix_token_count: u64,
    layer_start: u32,
    layer_end: u32,
    position_start: u64,
    position_end: u64,
    confidence_milli: u32,
    artifact_checksum: u64,
    created_at_us: u64,
) -> u64 {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&(plan_id.len() as u64).to_le_bytes());
    bytes.extend_from_slice(plan_id.as_bytes());
    bytes.extend_from_slice(&(request_id.len() as u64).to_le_bytes());
    bytes.extend_from_slice(request_id.as_bytes());
    bytes.extend_from_slice(&prefix_cache_reuse_action_tag(action).to_le_bytes());
    if let Some(artifact_id) = artifact_id {
        bytes.extend_from_slice(&(artifact_id.len() as u64).to_le_bytes());
        bytes.extend_from_slice(artifact_id.as_bytes());
    }
    bytes.extend_from_slice(&matched_prefix_token_count.to_le_bytes());
    bytes.extend_from_slice(&layer_start.to_le_bytes());
    bytes.extend_from_slice(&layer_end.to_le_bytes());
    bytes.extend_from_slice(&position_start.to_le_bytes());
    bytes.extend_from_slice(&position_end.to_le_bytes());
    bytes.extend_from_slice(&confidence_milli.to_le_bytes());
    bytes.extend_from_slice(&artifact_checksum.to_le_bytes());
    bytes.extend_from_slice(&created_at_us.to_le_bytes());
    checksum64(&bytes)
}

fn prefix_cache_reuse_action_tag(action: PrefixCacheReuseAction) -> u64 {
    match action {
        PrefixCacheReuseAction::Miss => 1,
        PrefixCacheReuseAction::Reuse => 2,
        PrefixCacheReuseAction::RequireVerify => 3,
    }
}

fn shortpath_action_tag(action: ShortpathAction) -> u64 {
    match action {
        ShortpathAction::Continue => 1,
        ShortpathAction::JumpToLayer => 2,
        ShortpathAction::JumpToTerminal => 3,
        ShortpathAction::RequireVerify => 4,
    }
}

fn shortpath_decision_checksum(
    decision_id: &str,
    request_id: &str,
    action: ShortpathAction,
    artifact_id: Option<&str>,
    target_layer_start: u32,
    target_layer_end: u32,
    confidence_milli: u32,
    artifact_checksum: u64,
    created_at_us: u64,
) -> u64 {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&(decision_id.len() as u64).to_le_bytes());
    bytes.extend_from_slice(decision_id.as_bytes());
    bytes.extend_from_slice(&(request_id.len() as u64).to_le_bytes());
    bytes.extend_from_slice(request_id.as_bytes());
    bytes.extend_from_slice(&(action as u8).to_le_bytes());
    if let Some(artifact_id) = artifact_id {
        bytes.extend_from_slice(&(artifact_id.len() as u64).to_le_bytes());
        bytes.extend_from_slice(artifact_id.as_bytes());
    }
    bytes.extend_from_slice(&target_layer_start.to_le_bytes());
    bytes.extend_from_slice(&target_layer_end.to_le_bytes());
    bytes.extend_from_slice(&confidence_milli.to_le_bytes());
    bytes.extend_from_slice(&artifact_checksum.to_le_bytes());
    bytes.extend_from_slice(&created_at_us.to_le_bytes());
    checksum64(&bytes)
}

fn execution_artifact_manifest_checksum(manifest: &LingquExecutionArtifactManifest) -> u64 {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(manifest.kind.as_bytes());
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    let mut artifacts = manifest.artifacts.iter().collect::<Vec<_>>();
    artifacts.sort_by(|left, right| left.artifact_id.cmp(&right.artifact_id));
    for artifact in artifacts {
        push_checksum_str(&mut bytes, &artifact.artifact_id);
        bytes.extend_from_slice(&execution_artifact_kind_tag(artifact.kind).to_le_bytes());
        push_checksum_str(&mut bytes, &artifact.model.model_key);
        bytes.extend_from_slice(&artifact.producer_boundary.step_index.to_le_bytes());
        bytes.extend_from_slice(&artifact.producer_boundary.node_index.to_le_bytes());
        bytes.extend_from_slice(&artifact.producer_boundary.layer_start.to_le_bytes());
        bytes.extend_from_slice(&artifact.producer_boundary.layer_end.to_le_bytes());
        bytes.extend_from_slice(&artifact.target_layer_start.to_le_bytes());
        bytes.extend_from_slice(&artifact.target_layer_end.to_le_bytes());
        bytes.extend_from_slice(&artifact.confidence_milli.to_le_bytes());
        bytes.extend_from_slice(&artifact.checksum.to_le_bytes());
        bytes.extend_from_slice(&artifact.version.to_le_bytes());
    }
    checksum64(&bytes)
}

fn prefix_cache_manifest_checksum(manifest: &LingquPrefixCacheManifest) -> u64 {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(manifest.kind.as_bytes());
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    let mut artifacts = manifest.artifacts.iter().collect::<Vec<_>>();
    artifacts.sort_by(|left, right| left.artifact_id.cmp(&right.artifact_id));
    for artifact in artifacts {
        push_checksum_str(&mut bytes, &artifact.artifact_id);
        push_checksum_str(&mut bytes, &artifact.key.model.model_key);
        push_checksum_str(&mut bytes, &artifact.key.namespace);
        bytes.extend_from_slice(&artifact.key.prefix_token_hash.to_le_bytes());
        bytes.extend_from_slice(&artifact.key.prefix_token_count.to_le_bytes());
        bytes.extend_from_slice(&artifact.key.layer_start.to_le_bytes());
        bytes.extend_from_slice(&artifact.key.layer_end.to_le_bytes());
        bytes.extend_from_slice(&artifact.confidence_milli.to_le_bytes());
        bytes.extend_from_slice(&artifact.checksum.to_le_bytes());
        bytes.extend_from_slice(&artifact.version.to_le_bytes());
        bytes.extend_from_slice(&artifact.use_count.to_le_bytes());
    }
    checksum64(&bytes)
}

fn shortpath_decision_manifest_checksum(manifest: &LingquShortpathDecisionManifest) -> u64 {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(manifest.kind.as_bytes());
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    let mut decisions = manifest.decisions.iter().collect::<Vec<_>>();
    decisions.sort_by(|left, right| left.decision_id.cmp(&right.decision_id));
    for decision in decisions {
        push_checksum_str(&mut bytes, &decision.decision_id);
        push_checksum_str(&mut bytes, &decision.request_id);
        bytes.extend_from_slice(&shortpath_action_tag(decision.action).to_le_bytes());
        if let Some(artifact_id) = &decision.artifact_id {
            push_checksum_str(&mut bytes, artifact_id);
        }
        bytes.extend_from_slice(&decision.confidence_milli.to_le_bytes());
        bytes.extend_from_slice(&decision.proof_checksum.to_le_bytes());
        bytes.extend_from_slice(&decision.version.to_le_bytes());
    }
    checksum64(&bytes)
}

fn prefetch_plan_manifest_checksum(manifest: &LingquPrefetchPlanManifest) -> u64 {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(manifest.kind.as_bytes());
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    let mut plans = manifest.plans.iter().collect::<Vec<_>>();
    plans.sort_by(|left, right| left.plan_id.cmp(&right.plan_id));
    for plan in plans {
        push_checksum_str(&mut bytes, &plan.plan_id);
        push_checksum_str(&mut bytes, &plan.request_id);
        push_checksum_str(&mut bytes, &plan.model.model_key);
        bytes.extend_from_slice(&prefetch_scope_tag(plan.scope).to_le_bytes());
        bytes.extend_from_slice(&plan.lookahead_steps.to_le_bytes());
        bytes.extend_from_slice(&plan.target_step_index.to_le_bytes());
        bytes.extend_from_slice(&plan.target_position.to_le_bytes());
        for artifact_id in &plan.planned_artifact_ids {
            push_checksum_str(&mut bytes, artifact_id);
        }
        bytes.extend_from_slice(&prefetch_plan_state_tag(plan.state).to_le_bytes());
        bytes.extend_from_slice(&plan.checksum.to_le_bytes());
        bytes.extend_from_slice(&plan.version.to_le_bytes());
    }
    checksum64(&bytes)
}

fn push_checksum_str(bytes: &mut Vec<u8>, value: &str) {
    bytes.extend_from_slice(&(value.len() as u64).to_le_bytes());
    bytes.extend_from_slice(value.as_bytes());
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
    fn execution_artifact_requires_model_bound_payload() {
        let artifact = ExecutionArtifactObject {
            artifact_id: "artifact/missing-payload".to_string(),
            kind: ExecutionArtifactKind::HiddenState,
            model: sample_model_binding(),
            producer_boundary: sample_range_boundary(),
            target_layer_start: 8,
            target_layer_end: 16,
            dtype: TensorDType::F32,
            shape: vec![1, 1024],
            durable_payload_ref: None,
            hot_object_ref: None,
            source_query_result_id: None,
            source_engram_state_id: None,
            confidence_milli: 900,
            state: ExecutionArtifactState::Verified,
            checksum: 0x1234,
            version: 1,
            created_at_us: 10,
            expires_at_us: Some(20),
        };

        assert_eq!(
            artifact.validate(),
            Err(LingquMemoryError::MissingField(
                "execution_artifact.payload_ref"
            ))
        );
    }

    #[test]
    fn boundary_lookup_returns_continue_without_verified_artifact() {
        let mut service = LingquMemoryService::new();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        let hidden_ref = publish_hot_tensor(
            &mut object_service,
            "hidden/range/node2/step0".to_string(),
            f32_vec_to_le_bytes(&[0.1, 0.2, 0.3, 0.4]),
            TensorDType::F32,
            vec![1, 4],
            1,
            2,
            10,
        )
        .unwrap();

        let response = service
            .boundary_lookup(
                BoundaryLookupRequest {
                    request_id: "boundary/continue".to_string(),
                    model: sample_model_binding(),
                    boundary: sample_range_boundary(),
                    hidden_state: hidden_ref,
                    engram_state_id: None,
                    min_confidence_milli: 900,
                    allowed_actions: vec![ShortpathAction::JumpToTerminal],
                    created_at_us: 11,
                },
                12,
            )
            .unwrap();

        assert_eq!(response.decision.action, ShortpathAction::Continue);
        assert_eq!(response.artifact, None);
        assert_eq!(
            service
                .shortpath_decision("shortpath-decision/boundary/continue")
                .unwrap()
                .reason,
            "no_verified_execution_artifact_hit"
        );
    }

    #[test]
    fn boundary_lookup_returns_terminal_jump_for_verified_logits_artifact() {
        let mut service = LingquMemoryService::new();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        let hidden_ref = publish_hot_tensor(
            &mut object_service,
            "hidden/range/node4/step3".to_string(),
            f32_vec_to_le_bytes(&[0.1, 0.2, 0.3, 0.4]),
            TensorDType::F32,
            vec![1, 4],
            1,
            2,
            10,
        )
        .unwrap();
        let logits_ref = publish_hot_tensor(
            &mut object_service,
            "logits/shortpath/node4/step3".to_string(),
            f32_vec_to_le_bytes(&[1.0, 0.0, -1.0, -2.0]),
            TensorDType::F32,
            vec![1, 4],
            1,
            2,
            11,
        )
        .unwrap();
        let artifact = ExecutionArtifactObject {
            artifact_id: "artifact/logits/step3/node4".to_string(),
            kind: ExecutionArtifactKind::Logits,
            model: sample_model_binding(),
            producer_boundary: sample_range_boundary(),
            target_layer_start: 8,
            target_layer_end: 8,
            dtype: TensorDType::F32,
            shape: vec![1, 4],
            durable_payload_ref: None,
            hot_object_ref: Some(logits_ref),
            source_query_result_id: None,
            source_engram_state_id: None,
            confidence_milli: 980,
            state: ExecutionArtifactState::Verified,
            checksum: 0x8899,
            version: 1,
            created_at_us: 12,
            expires_at_us: Some(40),
        };
        service.register_execution_artifact(artifact).unwrap();

        let response = service
            .boundary_lookup(
                BoundaryLookupRequest {
                    request_id: "boundary/jump-terminal".to_string(),
                    model: sample_model_binding(),
                    boundary: sample_range_boundary(),
                    hidden_state: hidden_ref,
                    engram_state_id: None,
                    min_confidence_milli: 900,
                    allowed_actions: vec![ShortpathAction::JumpToTerminal],
                    created_at_us: 13,
                },
                14,
            )
            .unwrap();

        assert_eq!(response.decision.action, ShortpathAction::JumpToTerminal);
        assert_eq!(
            response.decision.artifact_id.as_deref(),
            Some("artifact/logits/step3/node4")
        );
        assert_eq!(response.decision.target_layer_start, Some(8));
        assert_eq!(response.decision.target_layer_end, Some(8));
        assert_eq!(response.decision.confidence_milli, 980);
        assert_eq!(
            service
                .execution_artifact("artifact/logits/step3/node4")
                .unwrap()
                .kind,
            ExecutionArtifactKind::Logits
        );
    }

    #[test]
    fn boundary_lookup_rejects_range_start_boundary() {
        let mut service = LingquMemoryService::new();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        let hidden_ref = publish_hot_tensor(
            &mut object_service,
            "hidden/range/node4/start/step3".to_string(),
            f32_vec_to_le_bytes(&[0.1, 0.2, 0.3, 0.4]),
            TensorDType::F32,
            vec![1, 4],
            1,
            2,
            10,
        )
        .unwrap();
        let mut boundary = sample_range_boundary();
        boundary.phase = RangeBoundaryPhase::RangeStart;

        let err = service
            .boundary_lookup(
                BoundaryLookupRequest {
                    request_id: "boundary/start".to_string(),
                    model: sample_model_binding(),
                    boundary,
                    hidden_state: hidden_ref,
                    engram_state_id: None,
                    min_confidence_milli: 900,
                    allowed_actions: vec![ShortpathAction::JumpToTerminal],
                    created_at_us: 13,
                },
                14,
            )
            .unwrap_err();
        assert_eq!(
            err,
            LingquMemoryError::InvalidValue {
                field: "boundary_lookup.boundary.phase",
                reason: "shortpath lookup must be issued from a range exit boundary"
            }
        );
    }

    #[test]
    fn prefetch_plan_records_range_start_lookahead() {
        let mut service = LingquMemoryService::new();
        let mut boundary = sample_range_boundary();
        boundary.phase = RangeBoundaryPhase::RangeStart;
        boundary.layer_start = 8;
        boundary.layer_end = 12;

        let plan = service
            .plan_prefetch(
                PrefetchPlanRequest {
                    request_id: "prefetch/node4/step3".to_string(),
                    model: sample_model_binding(),
                    boundary,
                    engram_state_id: None,
                    scope: PrefetchScope::MultiStep,
                    lookahead_steps: 2,
                    artifact_kinds: vec![ExecutionArtifactKind::KvCache],
                    created_at_us: 13,
                },
                14,
            )
            .unwrap();

        assert_eq!(plan.scope, PrefetchScope::MultiStep);
        assert_eq!(plan.target_step_index, 5);
        assert_eq!(plan.target_position, 14);
        assert_eq!(plan.state, PrefetchPlanState::Planned);
        assert!(plan.checksum != 0);
        assert_eq!(
            service
                .prefetch_plan("prefetch-plan/prefetch/node4/step3")
                .unwrap()
                .lookahead_steps,
            2
        );
    }

    #[test]
    fn prefix_cache_lookup_returns_longest_verified_candidate() {
        let mut service = LingquMemoryService::new();
        let short_key = sample_prefix_cache_key(8, 0x1111);
        let long_key = sample_prefix_cache_key(16, 0x2222);
        service
            .register_prefix_cache_artifact(PrefixCacheArtifact {
                artifact_id: "prefix-cache/short".to_string(),
                key: short_key.clone(),
                kv_artifact_ids: Vec::new(),
                durable_payload_refs: vec![LingquBlockPayloadRef::new(
                    "block/prefix/short",
                    0,
                    64,
                    0x1234,
                )],
                hot_object_refs: Vec::new(),
                dtype: TensorDType::F32,
                shape: vec![8, 4],
                confidence_milli: 950,
                state: ExecutionArtifactState::Verified,
                checksum: 0x9001,
                version: 1,
                created_at_us: 10,
                expires_at_us: Some(100),
                last_used_at_us: 10,
                use_count: 1,
            })
            .unwrap();
        service
            .register_prefix_cache_artifact(PrefixCacheArtifact {
                artifact_id: "prefix-cache/long".to_string(),
                key: long_key.clone(),
                kv_artifact_ids: Vec::new(),
                durable_payload_refs: vec![LingquBlockPayloadRef::new(
                    "block/prefix/long",
                    0,
                    128,
                    0x5678,
                )],
                hot_object_refs: Vec::new(),
                dtype: TensorDType::F32,
                shape: vec![16, 4],
                confidence_milli: 940,
                state: ExecutionArtifactState::Verified,
                checksum: 0x9002,
                version: 1,
                created_at_us: 10,
                expires_at_us: Some(100),
                last_used_at_us: 10,
                use_count: 1,
            })
            .unwrap();

        let response = service
            .lookup_prefix_cache(
                PrefixCacheLookupRequest {
                    request_id: "prefix-lookup/0".to_string(),
                    candidate_keys: vec![short_key, long_key],
                    min_confidence_milli: 900,
                    allow_verify: false,
                    created_at_us: 20,
                },
                21,
            )
            .unwrap();

        assert_eq!(response.reuse_plan.action, PrefixCacheReuseAction::Reuse);
        assert_eq!(
            response.reuse_plan.artifact_id.as_deref(),
            Some("prefix-cache/long")
        );
        assert_eq!(response.reuse_plan.matched_prefix_token_count, 16);
        assert_eq!(
            service
                .prefix_cache_reuse_plan("prefix-cache-reuse/prefix-lookup/0")
                .unwrap()
                .reason,
            "prefix_cache_hit"
        );
    }

    #[test]
    fn prefix_cache_lookup_miss_is_auditable() {
        let mut service = LingquMemoryService::new();
        let response = service
            .lookup_prefix_cache(
                PrefixCacheLookupRequest {
                    request_id: "prefix-lookup/miss".to_string(),
                    candidate_keys: vec![sample_prefix_cache_key(8, 0x1111)],
                    min_confidence_milli: 900,
                    allow_verify: false,
                    created_at_us: 20,
                },
                21,
            )
            .unwrap();

        assert_eq!(response.reuse_plan.action, PrefixCacheReuseAction::Miss);
        assert_eq!(response.artifact, None);
        assert_eq!(response.reuse_plan.reason, "prefix_cache_miss");
        assert!(response.reuse_plan.proof_checksum != 0);
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
    fn durable_store_snapshot_round_trips_dfs_and_block_payloads() {
        let service = populated_service();
        let mut durable = LingquMemoryDurableStore::new();
        let catalog_path = service
            .persist_catalog_to_dfs(&mut durable, "corpus/0")
            .expect("persist catalog");
        let block_ref = durable
            .write_block_payload("block/chunk/snapshot", b"snapshot payload".to_vec())
            .expect("write block");
        let snapshot = durable.export_snapshot().expect("export snapshot");
        let json = snapshot.to_json_bytes().expect("snapshot json");
        let decoded =
            LingquMemoryDurableStoreSnapshot::from_json_bytes(&json).expect("decode snapshot");
        let mut restored =
            LingquMemoryDurableStore::import_snapshot(decoded).expect("import snapshot");

        let restored_catalog = restored
            .load_catalog_snapshot(&catalog_path)
            .expect("load catalog");
        let restored_block = restored.read_block_payload(&block_ref).expect("read block");

        assert_eq!(restored_catalog.catalog.catalog_id, "corpus/0");
        assert_eq!(restored_block, b"snapshot payload");
    }

    #[test]
    fn durable_store_restarts_with_execution_and_prefix_manifests() {
        let mut durable = LingquMemoryDurableStore::new();
        let execution_artifact = ExecutionArtifactObject {
            artifact_id: "artifact/logits/restart".to_string(),
            kind: ExecutionArtifactKind::Logits,
            model: sample_model_binding(),
            producer_boundary: sample_range_boundary(),
            target_layer_start: 8,
            target_layer_end: 8,
            dtype: TensorDType::F32,
            shape: vec![1, 4],
            durable_payload_ref: Some(LingquBlockPayloadRef::new(
                "block/logits/restart",
                0,
                16,
                0x1111,
            )),
            hot_object_ref: None,
            source_query_result_id: None,
            source_engram_state_id: None,
            confidence_milli: 980,
            state: ExecutionArtifactState::Verified,
            checksum: 0x2222,
            version: 1,
            created_at_us: 10,
            expires_at_us: Some(100),
        };
        let prefix_artifact = PrefixCacheArtifact {
            artifact_id: "prefix-cache/restart/8".to_string(),
            key: sample_prefix_cache_key(8, 0x3333),
            kv_artifact_ids: Vec::new(),
            durable_payload_refs: vec![LingquBlockPayloadRef::new(
                "block/prefix/restart/8",
                0,
                64,
                0x4444,
            )],
            hot_object_refs: Vec::new(),
            dtype: TensorDType::F32,
            shape: vec![8, 4],
            confidence_milli: 950,
            state: ExecutionArtifactState::Verified,
            checksum: 0x5555,
            version: 1,
            created_at_us: 10,
            expires_at_us: Some(100),
            last_used_at_us: 10,
            use_count: 1,
        };

        durable
            .persist_execution_artifact_manifest(vec![execution_artifact.clone()])
            .expect("persist execution artifact manifest");
        durable
            .persist_prefix_cache_manifest(vec![prefix_artifact.clone()])
            .expect("persist prefix cache manifest");
        let durable_snapshot = durable
            .export_durable_sim_snapshot()
            .expect("export durable snapshot");
        let json = durable_snapshot.to_json_bytes().expect("snapshot json");
        let decoded = durable_sim::LingquDurableSimSnapshot::from_json_bytes(&json)
            .expect("decode durable snapshot");
        let mut restored =
            LingquMemoryDurableStore::import_durable_sim_snapshot(decoded).expect("import store");
        let mut restored_service = LingquMemoryService::new();

        assert_eq!(
            restored
                .load_execution_artifact_manifest()
                .expect("load execution artifacts"),
            vec![execution_artifact]
        );
        assert_eq!(
            restored
                .load_prefix_cache_manifest()
                .expect("load prefix artifacts"),
            vec![prefix_artifact]
        );
        restored_service
            .rebuild_execution_artifacts_from_dfs(&mut restored)
            .expect("rebuild execution artifacts");
        restored_service
            .rebuild_prefix_cache_artifacts_from_dfs(&mut restored)
            .expect("rebuild prefix cache artifacts");

        let boundary = restored_service
            .boundary_lookup(
                BoundaryLookupRequest {
                    request_id: "boundary/restart".to_string(),
                    model: sample_model_binding(),
                    boundary: sample_range_boundary(),
                    hidden_state: HotTensorObjectRef {
                        object_key: "hidden/restart".to_string(),
                        version: 1,
                        backend: HotObjectBackend::ObmmShmem,
                        storage_ref: "obmm://hidden/restart".to_string(),
                        segment: None,
                        offset: 0,
                        bytes: 16,
                        checksum: 0x6666,
                        dtype: TensorDType::F32,
                        shape: vec![1, 4],
                    },
                    engram_state_id: None,
                    min_confidence_milli: 900,
                    allowed_actions: vec![ShortpathAction::JumpToTerminal],
                    created_at_us: 20,
                },
                21,
            )
            .expect("boundary lookup after restart");
        assert_eq!(boundary.decision.action, ShortpathAction::JumpToTerminal);
        assert_eq!(
            boundary.decision.artifact_id.as_deref(),
            Some("artifact/logits/restart")
        );

        let prefix = restored_service
            .lookup_prefix_cache(
                PrefixCacheLookupRequest {
                    request_id: "prefix/restart".to_string(),
                    candidate_keys: vec![sample_prefix_cache_key(8, 0x3333)],
                    min_confidence_milli: 900,
                    allow_verify: false,
                    created_at_us: 20,
                },
                21,
            )
            .expect("prefix cache lookup after restart");
        assert_eq!(prefix.reuse_plan.action, PrefixCacheReuseAction::Reuse);
        assert_eq!(
            prefix.reuse_plan.artifact_id.as_deref(),
            Some("prefix-cache/restart/8")
        );
        let prefetch = restored_service
            .plan_prefetch(
                PrefetchPlanRequest {
                    request_id: "prefetch/restart".to_string(),
                    model: sample_model_binding(),
                    boundary: RangeBoundary {
                        phase: RangeBoundaryPhase::RangeStart,
                        step_index: 3,
                        node_index: 4,
                        layer_start: 4,
                        layer_end: 8,
                        next_node_index: Some(5),
                        position: 12,
                    },
                    engram_state_id: None,
                    scope: PrefetchScope::MultiStep,
                    lookahead_steps: 2,
                    artifact_kinds: vec![ExecutionArtifactKind::KvCache],
                    created_at_us: 20,
                },
                21,
            )
            .expect("prefetch plan after restart");
        assert_eq!(prefetch.plan_id, "prefetch-plan/prefetch/restart");

        restored_service
            .persist_shortpath_decisions_to_dfs(&mut restored)
            .expect("persist shortpath audit");
        restored_service
            .persist_prefetch_plans_to_dfs(&mut restored)
            .expect("persist prefetch audit");
        restored_service
            .persist_shortpath_decisions_to_dfs(&mut restored)
            .expect("persist shortpath audit idempotently");
        restored_service
            .persist_prefetch_plans_to_dfs(&mut restored)
            .expect("persist prefetch audit idempotently");
        let audit_snapshot = restored
            .export_durable_sim_snapshot()
            .expect("export audit durable snapshot");
        let shortpath_log_records = audit_snapshot
            .dfs
            .append_logs
            .iter()
            .filter(|record| record.path == LINGQU_SHORTPATH_DECISION_AUDIT_LOG_PATH)
            .collect::<Vec<_>>();
        let prefetch_log_records = audit_snapshot
            .dfs
            .append_logs
            .iter()
            .filter(|record| record.path == LINGQU_PREFETCH_PLAN_AUDIT_LOG_PATH)
            .collect::<Vec<_>>();
        assert_eq!(shortpath_log_records.len(), 1);
        assert_eq!(shortpath_log_records[0].seq, 1);
        assert_eq!(prefetch_log_records.len(), 1);
        assert_eq!(prefetch_log_records[0].seq, 1);
        assert!(audit_snapshot.dfs.files.iter().all(|record| record.path
            != LINGQU_SHORTPATH_DECISION_MANIFEST_PATH
            && record.path != LINGQU_PREFETCH_PLAN_MANIFEST_PATH));
        let audit_json = audit_snapshot.to_json_bytes().expect("audit json");
        let audit_decoded = durable_sim::LingquDurableSimSnapshot::from_json_bytes(&audit_json)
            .expect("decode audit durable snapshot");
        let mut audit_store = LingquMemoryDurableStore::import_durable_sim_snapshot(audit_decoded)
            .expect("import audit store");
        let mut audit_service = LingquMemoryService::new();
        audit_service
            .rebuild_shortpath_decisions_from_dfs(&mut audit_store)
            .expect("rebuild shortpath audit");
        audit_service
            .rebuild_prefetch_plans_from_dfs(&mut audit_store)
            .expect("rebuild prefetch audit");
        assert!(audit_service
            .shortpath_decision("shortpath-decision/boundary/restart")
            .is_some());
        assert!(audit_service
            .prefetch_plan("prefetch-plan/prefetch/restart")
            .is_some());
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
    fn query_result_manifest_registers_after_catalog_reload() {
        let mut service = LingquMemoryService::new();
        service
            .publish_catalog(MemoryCorpusCatalog {
                catalog_id: "corpus/flat".to_string(),
                namespace: "project/default".to_string(),
                dfs_path: LingquDfsPath::new("/lingqu/memory/corpus/flat/catalog.json"),
                version: 1,
                record_ids: vec!["record/a".to_string()],
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
        let mut durable = LingquMemoryDurableStore::new();
        let segment_ref = durable
            .write_block_payload("block/embed/flat", f32_vec_to_le_bytes(&[1.0, 0.0]))
            .unwrap();
        let query_ref = durable
            .write_block_payload("block/query/flat", f32_vec_to_le_bytes(&[1.0, 0.0]))
            .unwrap();
        service
            .register_embedding_segment(EmbeddingSegment {
                segment_id: "segment/flat".to_string(),
                model_version: "embed/v1".to_string(),
                dims: 2,
                row_count: 1,
                row_stride_bytes: 8,
                dtype: TensorDType::F32,
                vector_block_refs: vec![segment_ref],
                row_map: vec![EmbeddingRow {
                    chunk_id: "chunk/a".to_string(),
                    row: 0,
                }],
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
        let path = service
            .persist_catalog_to_dfs(&mut durable, "corpus/flat")
            .unwrap();
        let result_path = durable.persist_query_result(&result).unwrap();

        let mut restored = LingquMemoryService::new();
        restored
            .rebuild_catalog_from_dfs(&mut durable, &path)
            .expect("rebuild catalog");
        let restored_result = durable.load_query_result(&result_path).unwrap();
        restored
            .register_query_result(restored_result)
            .expect("register query result");

        assert_eq!(
            restored.query_result("query-result/query/flat").unwrap(),
            &result
        );
    }

    #[test]
    fn flat_query_hot_state_materializes_after_durable_snapshot_restart() {
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
                "block/embed/restart-flat",
                f32_vec_to_le_bytes(&[1.0, 0.0, 0.0, 1.0]),
            )
            .unwrap();
        let query_ref = durable
            .write_block_payload("block/query/restart-flat", f32_vec_to_le_bytes(&[0.0, 1.0]))
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
                    query_id: "query/restart-flat".to_string(),
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
        let catalog_path = service
            .persist_catalog_to_dfs(&mut durable, "corpus/flat")
            .unwrap();
        let result_path = durable.persist_query_result(&result).unwrap();
        let durable_snapshot = durable.export_durable_sim_snapshot().unwrap();
        let durable_json = durable_snapshot.to_json_bytes().unwrap();
        let durable_decoded = durable_sim::LingquDurableSimSnapshot::from_json_bytes(&durable_json)
            .expect("decode durable snapshot");
        let mut restored_durable =
            LingquMemoryDurableStore::import_durable_sim_snapshot(durable_decoded)
                .expect("import durable store");
        let mut restored_service = LingquMemoryService::new();
        restored_service
            .rebuild_catalog_from_dfs(&mut restored_durable, &catalog_path)
            .expect("rebuild catalog after restart");
        let restored_result = restored_durable
            .load_query_result(&result_path)
            .expect("load query result after restart");
        restored_service
            .register_query_result(restored_result)
            .expect("register query result after restart");
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());

        let hot_state = restored_service
            .materialize_hot_state_from_query(
                &mut restored_durable,
                &mut object_service,
                HotMemoryMaterializeFromQueryReq {
                    state_id: "hot/restart-flat".to_string(),
                    query_result_id: "query-result/query/restart-flat".to_string(),
                    owner_entity: 1,
                    producer_entity: 0,
                    now_us: 200,
                },
            )
            .expect("materialize hot state after restart");

        assert_eq!(hot_state.selected_chunk_ids, ["chunk/b", "chunk/a"]);
        let table_bytes = object_service
            .get_copy(
                &hot_state.table.object_key,
                LingquObjectVersionSelector::LatestCommitted,
            )
            .expect("hot table payload after restart");
        let table_values = f32_values_from_le_bytes(&table_bytes).unwrap();
        assert_eq!(table_values, vec![0.0, 1.0, 1.0, 0.0]);

        restored_durable
            .persist_object_service_checkpoint(&object_service)
            .expect("persist object service checkpoint");
        let checkpoint_snapshot = restored_durable
            .export_durable_sim_snapshot()
            .expect("export object checkpoint durable snapshot");
        let checkpoint_record = checkpoint_snapshot
            .dfs
            .files
            .iter()
            .find(|record| record.path == LINGQU_OBJECT_SERVICE_CHECKPOINT_PATH)
            .expect("object service checkpoint dfs record");
        let checkpoint_bytes =
            durable_dfs_record_bytes(checkpoint_record, &checkpoint_snapshot.block.blocks)
                .expect("checkpoint bytes");
        let checkpoint = LingquObjectServiceCheckpoint::from_json_bytes(&checkpoint_bytes)
            .expect("decode object checkpoint");
        assert_eq!(checkpoint.records.len(), 3);
        assert!(checkpoint
            .records
            .iter()
            .all(|entry| entry.record.payload_bytes.is_empty() && entry.payload_ref.is_some()));
        let restored_snapshot = restored_durable
            .load_object_service_checkpoint()
            .expect("load object service checkpoint");
        let restored_object_service =
            LingquObjectServiceStub::import_snapshot(restored_snapshot).expect("restore objects");
        let restored_table_bytes = restored_object_service
            .get_copy(
                &hot_state.table.object_key,
                LingquObjectVersionSelector::LatestCommitted,
            )
            .expect("hot table payload after object checkpoint reload");
        assert_eq!(restored_table_bytes, table_bytes);
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
    fn hot_state_materialization_fails_on_missing_embedding_block_payload() {
        let mut service = populated_service();
        let result = service
            .query_memory(
                MemoryQuery {
                    query_id: "q/missing-block".to_string(),
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
            .expect("query memory");
        let mut durable = LingquMemoryDurableStore::new();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());

        let err = service
            .materialize_hot_state_from_query(
                &mut durable,
                &mut object_service,
                HotMemoryMaterializeFromQueryReq {
                    state_id: "hot/missing-block".to_string(),
                    query_result_id: result.result_id,
                    owner_entity: 1,
                    producer_entity: 0,
                    now_us: 200,
                },
            )
            .expect_err("missing embedding block must fail materialization");
        assert_eq!(
            err,
            LingquMemoryError::MissingBlockPayload("block/embed/0".to_string())
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
            .build_engram_state("engram/0", &hot_state.state_id, None, Vec::new(), 300, None)
            .unwrap();

        assert_eq!(
            engram.query_result_manifest_ref,
            hot_state.query_result_manifest_ref
        );
        assert_eq!(engram.query_result_id, hot_state.query_result_id);
        assert_eq!(engram.operator_kind, EngramOperatorKind::ContextGate);
        assert_eq!(engram.table.object_key, hot_state.table.object_key);
        assert_eq!(engram.indices.object_key, hot_state.indices.object_key);
        assert_eq!(engram.hidden_size, 2);
        assert_eq!(engram.table_rows, 1);
        assert_eq!(engram.version, 1);
        assert!(engram.checksum != 0);
        let mut corrupted = engram.clone();
        corrupted.hidden_size = 4;
        assert_eq!(
            corrupted.validate(),
            Err(LingquMemoryError::InvalidValue {
                field: "engram_state.table.shape",
                reason: "table shape must match table_rows and hidden_size"
            })
        );
        let mut corrupted = engram.clone();
        corrupted.checksum ^= 1;
        assert_eq!(
            corrupted.validate(),
            Err(LingquMemoryError::InvalidValue {
                field: "engram_state.checksum",
                reason: "checksum does not match Engram state metadata"
            })
        );
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
                    compatible_models: vec![sample_model_binding()],
                    owner_entity: 0,
                    producer_entity: 0,
                    now_us: 300,
                    expires_at_us: Some(400),
                },
            )
            .unwrap();

        assert_eq!(engram.compatible_models, vec![sample_model_binding()]);
        assert_eq!(engram.expires_at_us, Some(400));
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
                    compatible_models: Vec::new(),
                    owner_entity: 0,
                    producer_entity: 0,
                    now_us: 300,
                    expires_at_us: None,
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

    fn sample_model_binding() -> InferenceModelBinding {
        InferenceModelBinding {
            model_id: "Qwen3-0.6B".to_string(),
            model_key: "qwen3-0.6b".to_string(),
            tokenizer_hash: 0x6006,
            profile_hash: 0x7007,
        }
    }

    fn sample_range_boundary() -> RangeBoundary {
        RangeBoundary {
            phase: RangeBoundaryPhase::RangeExit,
            step_index: 3,
            node_index: 4,
            layer_start: 4,
            layer_end: 8,
            next_node_index: Some(5),
            position: 12,
        }
    }

    fn sample_prefix_cache_key(prefix_token_count: u64, prefix_token_hash: u64) -> PrefixCacheKey {
        PrefixCacheKey {
            model: sample_model_binding(),
            namespace: "tenant/project/session".to_string(),
            chat_template_hash: 0x1001,
            prefix_token_hash,
            prefix_token_count,
            rope_config_hash: 0x2002,
            kv_layout_hash: 0x3003,
            layer_start: 0,
            layer_end: 28,
            position_start: 0,
            position_end: prefix_token_count,
            security_label: MemorySecurityLabel::Internal,
        }
    }
}
