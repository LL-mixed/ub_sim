//! Product-shaped Lingqu Memory Service core.
//!
//! The crate owns the durable memory model and the hot-state materialization
//! contract. It intentionally keeps only one service layer in process; Host
//! and Guest deployments can wrap the same API with their own transport.

use std::cmp::Ordering;
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use sim_core::{BlockHash, SegmentHandle, TensorDType};
use sim_models::engram_hash::{
    build_engram_lookup_requests_from_step, engram_hash_table_specs,
    Qwen3DenseReferenceEngramHashConfig, Qwen3DenseReferenceEngramHashTableSpec,
    ENGRAM_HASH_ALGORITHM_VERSION,
};
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
pub struct MemoryRecordLifecycleEvent {
    pub event_id: String,
    pub catalog_id: String,
    pub record_id: String,
    pub previous_state: MemoryRecordState,
    pub new_state: MemoryRecordState,
    pub previous_record_version: u64,
    pub new_record_version: u64,
    pub previous_catalog_version: u64,
    pub new_catalog_version: u64,
    pub actor: String,
    pub reason: String,
    pub checksum: u64,
    pub created_at_us: u64,
    pub version: u64,
}

impl MemoryRecordLifecycleEvent {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.event_id, "record_lifecycle.event_id")?;
        required_str(&self.catalog_id, "record_lifecycle.catalog_id")?;
        required_str(&self.record_id, "record_lifecycle.record_id")?;
        required_str(&self.actor, "record_lifecycle.actor")?;
        required_str(&self.reason, "record_lifecycle.reason")?;
        nonzero(
            self.previous_record_version,
            "record_lifecycle.previous_record_version",
        )?;
        nonzero(
            self.new_record_version,
            "record_lifecycle.new_record_version",
        )?;
        nonzero(
            self.previous_catalog_version,
            "record_lifecycle.previous_catalog_version",
        )?;
        nonzero(
            self.new_catalog_version,
            "record_lifecycle.new_catalog_version",
        )?;
        nonzero(self.checksum, "record_lifecycle.checksum")?;
        nonzero(self.created_at_us, "record_lifecycle.created_at_us")?;
        nonzero(self.version, "record_lifecycle.version")?;
        if self.previous_state == self.new_state {
            return Err(LingquMemoryError::InvalidValue {
                field: "record_lifecycle.new_state",
                reason: "lifecycle event must change state",
            });
        }
        if self.new_record_version <= self.previous_record_version {
            return Err(LingquMemoryError::InvalidValue {
                field: "record_lifecycle.new_record_version",
                reason: "new record version must be greater than previous version",
            });
        }
        if self.new_catalog_version <= self.previous_catalog_version {
            return Err(LingquMemoryError::InvalidValue {
                field: "record_lifecycle.new_catalog_version",
                reason: "new catalog version must be greater than previous version",
            });
        }
        let actual = record_lifecycle_event_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.event_id.clone(),
                expected: self.checksum,
                actual,
            });
        }
        Ok(())
    }
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
pub const LINGQU_QUERY_RESULT_AUDIT_LOG_PATH: &str = "/lingqu/memory/audit/query-results.log";
pub const LINGQU_RECORD_LIFECYCLE_AUDIT_LOG_PATH: &str =
    "/lingqu/memory/audit/record-lifecycle.log";
pub const LINGQU_SHORTPATH_DECISION_MANIFEST_PATH: &str =
    "/lingqu/memory/shortpath-decisions/audit.json";
pub const LINGQU_SHORTPATH_SUPPORT_MANIFEST_PATH: &str =
    "/lingqu/memory/shortpath-support/audit.json";
pub const LINGQU_PREFETCH_PLAN_MANIFEST_PATH: &str = "/lingqu/memory/prefetch-plans/audit.json";
pub const LINGQU_BOUNDARY_OBSERVATION_AUDIT_LOG_PATH: &str =
    "/lingqu/memory/audit/boundary-observations.log";
pub const LINGQU_ARTIFACT_ACCESS_AUDIT_LOG_PATH: &str = "/lingqu/memory/audit/artifact-access.log";
pub const LINGQU_SHORTPATH_DECISION_AUDIT_LOG_PATH: &str =
    "/lingqu/memory/audit/shortpath-decisions.log";
pub const LINGQU_SHORTPATH_SUPPORT_AUDIT_LOG_PATH: &str =
    "/lingqu/memory/audit/shortpath-support.log";
pub const LINGQU_PREFETCH_PLAN_AUDIT_LOG_PATH: &str = "/lingqu/memory/audit/prefetch-plans.log";
pub const LINGQU_PREFIX_CACHE_REUSE_AUDIT_LOG_PATH: &str =
    "/lingqu/memory/audit/prefix-cache-reuse.log";
pub const LINGQU_PAPER_ENGRAM_TABLE_SHARD_MANIFEST_PATH: &str =
    "/lingqu/memory/engram/table-shards/manifest.json";
pub const LINGQU_PAPER_ENGRAM_GATE_MANIFEST_PATH: &str =
    "/lingqu/memory/engram/gates/manifest.json";
pub const LINGQU_PAPER_ENGRAM_TOKENIZER_PROJECTION_MANIFEST_PATH: &str =
    "/lingqu/memory/engram/tokenizer-projections/manifest.json";
pub const LINGQU_PAPER_ENGRAM_HASH_CONFIG_MANIFEST_PATH: &str =
    "/lingqu/memory/engram/hash-configs/manifest.json";
pub const LINGQU_PAPER_ENGRAM_TRAINING_RECIPE_MANIFEST_PATH: &str =
    "/lingqu/memory/engram/training-recipes/manifest.json";
pub const LINGQU_PAPER_ENGRAM_EVAL_REPORT_MANIFEST_PATH: &str =
    "/lingqu/memory/engram/eval-reports/manifest.json";
pub const LINGQU_PAPER_ENGRAM_MODULE_REGISTRY_PATH: &str =
    "/lingqu/memory/engram/modules/registry.json";
pub const LINGQU_OBJECT_SERVICE_CHECKPOINT_PATH: &str =
    "/lingqu/object-service/checkpoints/latest.json";

pub const LINGQU_EXECUTION_ARTIFACT_MANIFEST_KIND: &str =
    "lingqu_memory_execution_artifact_manifest";
pub const LINGQU_PREFIX_CACHE_MANIFEST_KIND: &str = "lingqu_memory_prefix_cache_manifest";
pub const LINGQU_PAPER_ENGRAM_TABLE_SHARD_MANIFEST_KIND: &str =
    "lingqu_memory_paper_engram_table_shard_manifest";
pub const LINGQU_PAPER_ENGRAM_GATE_MANIFEST_KIND: &str = "lingqu_memory_paper_engram_gate_manifest";
pub const LINGQU_PAPER_ENGRAM_TOKENIZER_PROJECTION_MANIFEST_KIND: &str =
    "lingqu_memory_paper_engram_tokenizer_projection_manifest";
pub const LINGQU_PAPER_ENGRAM_HASH_CONFIG_MANIFEST_KIND: &str =
    "lingqu_memory_paper_engram_hash_config_manifest";
pub const LINGQU_PAPER_ENGRAM_TRAINING_RECIPE_MANIFEST_KIND: &str =
    "lingqu_memory_paper_engram_training_recipe_manifest";
pub const LINGQU_PAPER_ENGRAM_EVAL_REPORT_MANIFEST_KIND: &str =
    "lingqu_memory_paper_engram_eval_report_manifest";
pub const LINGQU_PAPER_ENGRAM_MODULE_REGISTRY_MANIFEST_KIND: &str =
    "lingqu_memory_paper_engram_module_registry_manifest";
pub const LINGQU_SHORTPATH_DECISION_MANIFEST_KIND: &str =
    "lingqu_memory_shortpath_decision_manifest";
pub const LINGQU_SHORTPATH_SUPPORT_MANIFEST_KIND: &str = "lingqu_memory_shortpath_support_manifest";
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

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PaperEngramQualityClaim {
    None,
    Posttrain,
    Finetune,
    Imported,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum PaperEngramTrainingMode {
    EngramOnlyContinuedPretrain,
    EngramLora,
    FullFinetune,
    ExternalImport,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PaperEngramTrainingRecipeManifest {
    pub recipe_id: String,
    pub model: InferenceModelBinding,
    pub mode: PaperEngramTrainingMode,
    pub base_checkpoint_checksum: u64,
    pub tokenizer_projection_ref: LingquDfsPath,
    pub hash_config_ref: LingquDfsPath,
    pub dataset_refs: Vec<String>,
    pub objective: String,
    pub frozen_base_model: bool,
    pub lora_enabled: bool,
    pub table_init: String,
    pub gate_init: String,
    pub layers: Vec<u32>,
    pub orders: Vec<u8>,
    pub heads_per_order: u32,
    pub table_rows: u64,
    pub evidence_refs: Vec<String>,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl PaperEngramTrainingRecipeManifest {
    pub fn new(mut manifest: PaperEngramTrainingRecipeManifest) -> MemoryResult<Self> {
        manifest.dataset_refs.sort();
        manifest.dataset_refs.dedup();
        manifest.layers.sort();
        manifest.layers.dedup();
        manifest.orders.sort();
        manifest.orders.dedup();
        manifest.evidence_refs.sort();
        manifest.evidence_refs.dedup();
        manifest.checksum = paper_engram_training_recipe_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.recipe_id, "paper_engram_training_recipe.recipe_id")?;
        self.model.validate()?;
        nonzero(
            self.base_checkpoint_checksum,
            "paper_engram_training_recipe.base_checkpoint_checksum",
        )?;
        self.tokenizer_projection_ref
            .validate("paper_engram_training_recipe.tokenizer_projection_ref")?;
        self.hash_config_ref
            .validate("paper_engram_training_recipe.hash_config_ref")?;
        if self.dataset_refs.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_training_recipe.dataset_refs",
            ));
        }
        for dataset_ref in &self.dataset_refs {
            required_str(dataset_ref, "paper_engram_training_recipe.dataset_refs")?;
        }
        required_str(&self.objective, "paper_engram_training_recipe.objective")?;
        required_str(&self.table_init, "paper_engram_training_recipe.table_init")?;
        required_str(&self.gate_init, "paper_engram_training_recipe.gate_init")?;
        if self.layers.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_training_recipe.layers",
            ));
        }
        if self.orders.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_training_recipe.orders",
            ));
        }
        for order in &self.orders {
            nonzero(*order as u64, "paper_engram_training_recipe.orders")?;
        }
        nonzero(
            self.heads_per_order as u64,
            "paper_engram_training_recipe.heads_per_order",
        )?;
        nonzero(self.table_rows, "paper_engram_training_recipe.table_rows")?;
        if matches!(
            self.mode,
            PaperEngramTrainingMode::EngramOnlyContinuedPretrain
        ) && !self.frozen_base_model
        {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_training_recipe.frozen_base_model",
                reason: "Engram-only continued pretrain must freeze the base model",
            });
        }
        if matches!(self.mode, PaperEngramTrainingMode::EngramLora) && !self.lora_enabled {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_training_recipe.lora_enabled",
                reason: "Engram+LoRA recipe must declare LoRA enabled",
            });
        }
        for evidence_ref in &self.evidence_refs {
            required_str(evidence_ref, "paper_engram_training_recipe.evidence_refs")?;
        }
        nonzero(self.version, "paper_engram_training_recipe.version")?;
        nonzero(
            self.created_at_us,
            "paper_engram_training_recipe.created_at_us",
        )?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_training_recipe.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        nonzero(self.checksum, "paper_engram_training_recipe.checksum")?;
        let actual = paper_engram_training_recipe_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.recipe_id.clone(),
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
pub struct PaperEngramEvalReportManifest {
    pub report_id: String,
    pub recipe_id: String,
    pub module_id: String,
    pub model: InferenceModelBinding,
    pub validation_set_refs: Vec<String>,
    pub sample_count: u64,
    pub baseline_loss_milli: u64,
    pub paper_engram_loss_milli: u64,
    pub decode_policy_loss_milli: Option<u64>,
    pub paper_engram_decode_policy_loss_milli: Option<u64>,
    pub max_allowed_regression_milli: u64,
    pub output_checksum: u64,
    pub zero_table_hidden_checksum: Option<u64>,
    pub paper_engram_hidden_checksum: Option<u64>,
    pub zero_table_output_checksum: Option<u64>,
    pub cpu_backend_output_match: Option<bool>,
    pub row_prefetch_requests: Option<u64>,
    pub row_prefetch_hits: Option<u64>,
    pub runtime_context_steps_expected: Option<u64>,
    pub runtime_context_steps_observed: Option<u64>,
    pub max_backend_latency_us: Option<u64>,
    pub max_allowed_backend_latency_us: Option<u64>,
    pub evidence_refs: Vec<String>,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl PaperEngramEvalReportManifest {
    pub fn new(mut manifest: PaperEngramEvalReportManifest) -> MemoryResult<Self> {
        manifest.validation_set_refs.sort();
        manifest.validation_set_refs.dedup();
        manifest.evidence_refs.sort();
        manifest.evidence_refs.dedup();
        manifest.checksum = paper_engram_eval_report_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.report_id, "paper_engram_eval_report.report_id")?;
        required_str(&self.recipe_id, "paper_engram_eval_report.recipe_id")?;
        required_str(&self.module_id, "paper_engram_eval_report.module_id")?;
        self.model.validate()?;
        if self.validation_set_refs.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_eval_report.validation_set_refs",
            ));
        }
        for validation_set_ref in &self.validation_set_refs {
            required_str(
                validation_set_ref,
                "paper_engram_eval_report.validation_set_refs",
            )?;
        }
        nonzero(self.sample_count, "paper_engram_eval_report.sample_count")?;
        nonzero(
            self.baseline_loss_milli,
            "paper_engram_eval_report.baseline_loss_milli",
        )?;
        nonzero(
            self.paper_engram_loss_milli,
            "paper_engram_eval_report.paper_engram_loss_milli",
        )?;
        if self.paper_engram_loss_milli
            > self
                .baseline_loss_milli
                .saturating_add(self.max_allowed_regression_milli)
        {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.paper_engram_loss_milli",
                reason: "paper Engram eval must not regress beyond max_allowed_regression_milli",
            });
        }
        if let Some(decode_policy_loss_milli) = self.decode_policy_loss_milli {
            nonzero(
                decode_policy_loss_milli,
                "paper_engram_eval_report.decode_policy_loss_milli",
            )?;
            if self.paper_engram_loss_milli
                > decode_policy_loss_milli.saturating_add(self.max_allowed_regression_milli)
            {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_eval_report.paper_engram_loss_milli",
                    reason: "paper Engram eval must not regress versus decode policy beyond max_allowed_regression_milli",
                });
            }
        }
        if let Some(paper_decode_loss_milli) = self.paper_engram_decode_policy_loss_milli {
            nonzero(
                paper_decode_loss_milli,
                "paper_engram_eval_report.paper_engram_decode_policy_loss_milli",
            )?;
            if paper_decode_loss_milli
                > self
                    .baseline_loss_milli
                    .saturating_add(self.max_allowed_regression_milli)
            {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_eval_report.paper_engram_decode_policy_loss_milli",
                    reason: "paper Engram plus decode policy eval must not regress beyond max_allowed_regression_milli",
                });
            }
            if let Some(decode_policy_loss_milli) = self.decode_policy_loss_milli {
                if paper_decode_loss_milli
                    > decode_policy_loss_milli.saturating_add(self.max_allowed_regression_milli)
                {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "paper_engram_eval_report.paper_engram_decode_policy_loss_milli",
                        reason: "paper Engram plus decode policy eval must not regress versus decode policy beyond max_allowed_regression_milli",
                    });
                }
            }
        }
        nonzero(
            self.output_checksum,
            "paper_engram_eval_report.output_checksum",
        )?;
        if let Some(checksum) = self.zero_table_hidden_checksum {
            nonzero(
                checksum,
                "paper_engram_eval_report.zero_table_hidden_checksum",
            )?;
        }
        if let Some(checksum) = self.paper_engram_hidden_checksum {
            nonzero(
                checksum,
                "paper_engram_eval_report.paper_engram_hidden_checksum",
            )?;
        }
        if let Some(checksum) = self.zero_table_output_checksum {
            nonzero(
                checksum,
                "paper_engram_eval_report.zero_table_output_checksum",
            )?;
        }
        if matches!(self.cpu_backend_output_match, Some(false)) {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.cpu_backend_output_match",
                reason: "CPU reference and backend outputs must match when reported",
            });
        }
        if let Some(requests) = self.row_prefetch_requests {
            nonzero(requests, "paper_engram_eval_report.row_prefetch_requests")?;
        }
        if let Some(hits) = self.row_prefetch_hits {
            nonzero(hits, "paper_engram_eval_report.row_prefetch_hits")?;
            if let Some(requests) = self.row_prefetch_requests {
                if hits > requests {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "paper_engram_eval_report.row_prefetch_hits",
                        reason: "row prefetch hits must not exceed requests",
                    });
                }
            }
        }
        match (
            self.runtime_context_steps_expected,
            self.runtime_context_steps_observed,
        ) {
            (Some(expected), Some(observed)) => {
                nonzero(
                    expected,
                    "paper_engram_eval_report.runtime_context_steps_expected",
                )?;
                nonzero(
                    observed,
                    "paper_engram_eval_report.runtime_context_steps_observed",
                )?;
                if observed > expected {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "paper_engram_eval_report.runtime_context_steps_observed",
                        reason: "runtime context observed steps must not exceed expected steps",
                    });
                }
            }
            (Some(_), None) => {
                return Err(LingquMemoryError::MissingField(
                    "paper_engram_eval_report.runtime_context_steps_observed",
                ));
            }
            (None, Some(_)) => {
                return Err(LingquMemoryError::MissingField(
                    "paper_engram_eval_report.runtime_context_steps_expected",
                ));
            }
            (None, None) => {}
        }
        if let Some(latency) = self.max_backend_latency_us {
            nonzero(latency, "paper_engram_eval_report.max_backend_latency_us")?;
        }
        if let Some(latency) = self.max_allowed_backend_latency_us {
            nonzero(
                latency,
                "paper_engram_eval_report.max_allowed_backend_latency_us",
            )?;
        }
        if let (Some(latency), Some(max_allowed)) = (
            self.max_backend_latency_us,
            self.max_allowed_backend_latency_us,
        ) {
            if latency > max_allowed {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_eval_report.max_backend_latency_us",
                    reason: "backend latency must not exceed max_allowed_backend_latency_us",
                });
            }
        }
        if self.evidence_refs.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_eval_report.evidence_refs",
            ));
        }
        for evidence_ref in &self.evidence_refs {
            required_str(evidence_ref, "paper_engram_eval_report.evidence_refs")?;
        }
        nonzero(self.version, "paper_engram_eval_report.version")?;
        nonzero(self.created_at_us, "paper_engram_eval_report.created_at_us")?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_eval_report.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        nonzero(self.checksum, "paper_engram_eval_report.checksum")?;
        let actual = paper_engram_eval_report_manifest_checksum(self);
        if actual != self.checksum {
            let legacy_actual =
                paper_engram_eval_report_manifest_legacy_checksum_without_runtime_context(self);
            let is_legacy_runtime_context_absent = self.runtime_context_steps_expected.is_none()
                && self.runtime_context_steps_observed.is_none();
            if !is_legacy_runtime_context_absent || legacy_actual != self.checksum {
                return Err(LingquMemoryError::PayloadChecksumMismatch {
                    id: self.report_id.clone(),
                    expected: self.checksum,
                    actual,
                });
            }
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
pub struct PaperEngramTokenizerProjectionManifest {
    pub projection_id: String,
    pub model_id: String,
    pub tokenizer_id: String,
    pub projection_ref: LingquDfsPath,
    pub projection_checksum: u64,
    pub source_ref: Option<String>,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl PaperEngramTokenizerProjectionManifest {
    pub fn new(mut manifest: PaperEngramTokenizerProjectionManifest) -> MemoryResult<Self> {
        manifest.checksum = paper_engram_tokenizer_projection_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        required_str(
            &self.projection_id,
            "paper_engram_tokenizer_projection.projection_id",
        )?;
        required_str(&self.model_id, "paper_engram_tokenizer_projection.model_id")?;
        required_str(
            &self.tokenizer_id,
            "paper_engram_tokenizer_projection.tokenizer_id",
        )?;
        self.projection_ref
            .validate("paper_engram_tokenizer_projection.projection_ref")?;
        nonzero(
            self.projection_checksum,
            "paper_engram_tokenizer_projection.projection_checksum",
        )?;
        if let Some(source_ref) = &self.source_ref {
            required_str(source_ref, "paper_engram_tokenizer_projection.source_ref")?;
        }
        nonzero(self.version, "paper_engram_tokenizer_projection.version")?;
        nonzero(
            self.created_at_us,
            "paper_engram_tokenizer_projection.created_at_us",
        )?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_tokenizer_projection.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        nonzero(self.checksum, "paper_engram_tokenizer_projection.checksum")?;
        let actual = paper_engram_tokenizer_projection_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.projection_id.clone(),
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
pub struct PaperEngramHashConfigManifest {
    pub hash_config_id: String,
    pub model_id: String,
    pub tokenizer_projection_id: String,
    pub tokenizer_projection_checksum: u64,
    pub hash_config_ref: LingquDfsPath,
    pub hash_config_checksum: u64,
    pub orders: Vec<u8>,
    pub heads_per_order: u32,
    pub table_rows: u64,
    #[serde(default)]
    pub table_specs: Vec<Qwen3DenseReferenceEngramHashTableSpec>,
    pub seed: u64,
    pub algorithm: String,
    pub source_ref: Option<String>,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl PaperEngramHashConfigManifest {
    pub fn new(mut manifest: PaperEngramHashConfigManifest) -> MemoryResult<Self> {
        manifest.orders.sort();
        manifest.orders.dedup();
        if manifest.table_specs.is_empty() {
            manifest.table_specs = paper_engram_default_hash_table_specs(
                &manifest.orders,
                manifest.heads_per_order,
                manifest.table_rows,
                manifest.seed,
            )?;
        } else {
            manifest.table_specs.sort_by(|left, right| {
                left.order
                    .cmp(&right.order)
                    .then_with(|| left.head.cmp(&right.head))
            });
        }
        manifest.checksum = paper_engram_hash_config_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        required_str(
            &self.hash_config_id,
            "paper_engram_hash_config.hash_config_id",
        )?;
        required_str(&self.model_id, "paper_engram_hash_config.model_id")?;
        required_str(
            &self.tokenizer_projection_id,
            "paper_engram_hash_config.tokenizer_projection_id",
        )?;
        nonzero(
            self.tokenizer_projection_checksum,
            "paper_engram_hash_config.tokenizer_projection_checksum",
        )?;
        self.hash_config_ref
            .validate("paper_engram_hash_config.hash_config_ref")?;
        nonzero(
            self.hash_config_checksum,
            "paper_engram_hash_config.hash_config_checksum",
        )?;
        if self.orders.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_hash_config.orders",
            ));
        }
        let mut orders = HashSet::new();
        for order in &self.orders {
            if *order == 0 {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_hash_config.orders",
                    reason: "order must be greater than 0",
                });
            }
            if !orders.insert(*order) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_hash_config.orders",
                    reason: "duplicate order",
                });
            }
        }
        nonzero(
            self.heads_per_order as u64,
            "paper_engram_hash_config.heads_per_order",
        )?;
        nonzero(self.table_rows, "paper_engram_hash_config.table_rows")?;
        validate_paper_engram_hash_table_specs(
            &self.orders,
            self.heads_per_order,
            &self.table_specs,
        )?;
        required_str(&self.algorithm, "paper_engram_hash_config.algorithm")?;
        if self.algorithm != ENGRAM_HASH_ALGORITHM_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_hash_config.algorithm",
                reason: "unsupported canonical hash algorithm",
            });
        }
        if let Some(source_ref) = &self.source_ref {
            required_str(source_ref, "paper_engram_hash_config.source_ref")?;
        }
        nonzero(self.version, "paper_engram_hash_config.version")?;
        nonzero(self.created_at_us, "paper_engram_hash_config.created_at_us")?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_hash_config.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        nonzero(self.checksum, "paper_engram_hash_config.checksum")?;
        let actual = paper_engram_hash_config_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.hash_config_id.clone(),
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

fn paper_engram_default_hash_table_specs(
    orders: &[u8],
    heads_per_order: u32,
    table_rows: u64,
    seed: u64,
) -> MemoryResult<Vec<Qwen3DenseReferenceEngramHashTableSpec>> {
    let heads_per_order =
        usize::try_from(heads_per_order).map_err(|_| LingquMemoryError::InvalidValue {
            field: "paper_engram_hash_config.heads_per_order",
            reason: "heads_per_order exceeds host usize",
        })?;
    let config = Qwen3DenseReferenceEngramHashConfig {
        version: 1,
        projection_checksum: 1,
        orders: orders.to_vec(),
        heads_per_order,
        table_rows,
        seed,
        algorithm: ENGRAM_HASH_ALGORITHM_VERSION.to_string(),
        table_specs: Vec::new(),
    };
    engram_hash_table_specs(&config).map_err(|_| LingquMemoryError::InvalidValue {
        field: "paper_engram_hash_config.table_specs",
        reason: "hash table specs could not be derived",
    })
}

fn validate_paper_engram_hash_table_specs(
    orders: &[u8],
    heads_per_order: u32,
    table_specs: &[Qwen3DenseReferenceEngramHashTableSpec],
) -> MemoryResult<()> {
    if table_specs.is_empty() {
        return Ok(());
    }
    let mut seen = BTreeSet::new();
    for spec in table_specs {
        if !orders.contains(&spec.order) {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_hash_config.table_specs",
                reason: "table spec order must be declared by hash config",
            });
        }
        if u32::from(spec.head) >= heads_per_order {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_hash_config.table_specs",
                reason: "table spec head must be less than heads_per_order",
            });
        }
        nonzero(
            spec.table_rows,
            "paper_engram_hash_config.table_specs.table_rows",
        )?;
        if !seen.insert((spec.order, spec.head)) {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_hash_config.table_specs",
                reason: "duplicate table spec",
            });
        }
    }
    for &order in orders {
        for head in 0..heads_per_order {
            let head = u16::try_from(head).map_err(|_| LingquMemoryError::InvalidValue {
                field: "paper_engram_hash_config.heads_per_order",
                reason: "heads_per_order exceeds table spec head range",
            })?;
            if !seen.contains(&(order, head)) {
                return Err(LingquMemoryError::MissingField(
                    "paper_engram_hash_config.table_specs",
                ));
            }
        }
    }
    Ok(())
}

fn validate_paper_engram_quality_recipe_shape(
    module: &PaperEngramModuleManifest,
    recipe: &PaperEngramTrainingRecipeManifest,
    hash_config: &PaperEngramHashConfigManifest,
) -> MemoryResult<()> {
    if recipe.layers != module.layers {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_training_recipe.layers",
            reason: "training recipe layers must match module layers",
        });
    }
    if recipe.orders != module.orders || recipe.orders != hash_config.orders {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_training_recipe.orders",
            reason: "training recipe orders must match module and hash config orders",
        });
    }
    if recipe.heads_per_order != module.heads_per_order
        || recipe.heads_per_order != hash_config.heads_per_order
    {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_training_recipe.heads_per_order",
            reason: "training recipe heads_per_order must match module and hash config",
        });
    }
    for &order in &hash_config.orders {
        for head in 0..hash_config.heads_per_order {
            let table_rows = paper_engram_hash_config_table_rows(hash_config, order, head)?;
            if recipe.table_rows != table_rows {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_training_recipe.table_rows",
                    reason: "training recipe table_rows must match hash config table specs",
                });
            }
        }
    }
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PaperEngramTableShardManifest {
    pub shard_id: String,
    pub model_id: String,
    pub layer: u32,
    pub order: u8,
    pub head: u32,
    pub row_start: u64,
    pub row_end: u64,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
    pub block_payload_refs: Vec<LingquBlockPayloadRef>,
    pub source_ref: Option<String>,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl PaperEngramTableShardManifest {
    pub fn new(mut manifest: PaperEngramTableShardManifest) -> MemoryResult<Self> {
        let mut sorted_refs = manifest.block_payload_refs.clone();
        sorted_refs.sort_by(|left, right| {
            left.block
                .0
                .cmp(&right.block.0)
                .then_with(|| left.offset.cmp(&right.offset))
                .then_with(|| left.bytes.cmp(&right.bytes))
                .then_with(|| left.checksum.cmp(&right.checksum))
        });
        manifest.block_payload_refs = sorted_refs;
        manifest.checksum = paper_engram_table_shard_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.shard_id, "table_shard_manifest.shard_id")?;
        required_str(&self.model_id, "table_shard_manifest.model_id")?;
        nonzero(self.order as u64, "table_shard_manifest.order")?;
        if self.row_end <= self.row_start {
            return Err(LingquMemoryError::InvalidValue {
                field: "table_shard_manifest.row_range",
                reason: "row_end must be greater than row_start",
            });
        }
        validate_tensor_dtype(self.dtype, "table_shard_manifest.dtype")?;
        if self.shape.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "table_shard_manifest.shape",
            ));
        }
        for dim in &self.shape {
            nonzero(*dim, "table_shard_manifest.shape")?;
        }
        for payload_ref in &self.block_payload_refs {
            payload_ref.validate("table_shard_manifest.block_payload_refs")?;
        }
        if let Some(source_ref) = &self.source_ref {
            required_str(source_ref, "table_shard_manifest.source_ref")?;
        }
        nonzero(self.version, "table_shard_manifest.version")?;
        nonzero(self.created_at_us, "table_shard_manifest.created_at_us")?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "table_shard_manifest.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        nonzero(self.checksum, "table_shard_manifest.checksum")?;
        let actual = paper_engram_table_shard_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.shard_id.clone(),
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
pub struct PaperEngramGateManifest {
    pub gate_id: String,
    pub model_id: String,
    pub layer: u32,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
    pub payload_ref: Option<LingquBlockPayloadRef>,
    pub source_ref: Option<String>,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl PaperEngramGateManifest {
    pub fn new(mut manifest: PaperEngramGateManifest) -> MemoryResult<Self> {
        manifest.checksum = paper_engram_gate_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.gate_id, "gate_manifest.gate_id")?;
        required_str(&self.model_id, "gate_manifest.model_id")?;
        validate_tensor_dtype(self.dtype, "gate_manifest.dtype")?;
        if self.shape.is_empty() {
            return Err(LingquMemoryError::MissingField("gate_manifest.shape"));
        }
        for dim in &self.shape {
            nonzero(*dim, "gate_manifest.shape")?;
        }
        if self.payload_ref.is_none() && self.source_ref.is_none() {
            return Err(LingquMemoryError::MissingField(
                "gate_manifest.payload_source",
            ));
        }
        if let Some(payload_ref) = &self.payload_ref {
            payload_ref.validate("gate_manifest.payload_ref")?;
        }
        if let Some(source_ref) = &self.source_ref {
            required_str(source_ref, "gate_manifest.source_ref")?;
        }
        if self.payload_ref.is_some() && self.source_ref.is_some() {
            return Err(LingquMemoryError::InvalidValue {
                field: "gate_manifest.payload_source",
                reason: "gate payload must reference exactly one storage source",
            });
        }
        nonzero(self.version, "gate_manifest.version")?;
        nonzero(self.created_at_us, "gate_manifest.created_at_us")?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "gate_manifest.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        nonzero(self.checksum, "gate_manifest.checksum")?;
        let actual = paper_engram_gate_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.gate_id.clone(),
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
pub struct PaperEngramModuleManifest {
    pub module_id: String,
    pub module_name: String,
    pub model: InferenceModelBinding,
    pub base_checkpoint_checksum: u64,
    pub tokenizer_id: String,
    pub tokenizer_projection_ref: LingquDfsPath,
    pub hash_config_ref: LingquDfsPath,
    pub table_shard_ids: Vec<String>,
    pub gate_ids: Vec<String>,
    pub layers: Vec<u32>,
    pub orders: Vec<u8>,
    pub heads_per_order: u32,
    pub hidden_size: u64,
    pub memory_dim: u64,
    pub table_dtype: TensorDType,
    pub table_layout: String,
    pub gate_kind: String,
    pub training_recipe_ref: Option<LingquDfsPath>,
    #[serde(default)]
    pub eval_report_ref: Option<LingquDfsPath>,
    pub quality_claim: PaperEngramQualityClaim,
    pub payload_checksums: Vec<u64>,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
    pub expires_at_us: Option<u64>,
}

impl PaperEngramModuleManifest {
    pub fn new(mut manifest: PaperEngramModuleManifest) -> MemoryResult<Self> {
        manifest.table_shard_ids.sort();
        manifest.table_shard_ids.dedup();
        manifest.gate_ids.sort();
        manifest.gate_ids.dedup();
        manifest.layers.sort();
        manifest.orders.sort();
        manifest.payload_checksums.sort();
        manifest.checksum = paper_engram_module_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.module_id, "paper_engram_module.module_id")?;
        required_str(&self.module_name, "paper_engram_module.module_name")?;
        self.model.validate()?;
        nonzero(
            self.base_checkpoint_checksum,
            "paper_engram_module.base_checkpoint_checksum",
        )?;
        required_str(&self.tokenizer_id, "paper_engram_module.tokenizer_id")?;
        self.tokenizer_projection_ref
            .validate("paper_engram_module.tokenizer_projection_ref")?;
        self.hash_config_ref
            .validate("paper_engram_module.hash_config_ref")?;
        if let Some(reference) = &self.training_recipe_ref {
            reference.validate("paper_engram_module.training_recipe_ref")?;
        }
        if let Some(reference) = &self.eval_report_ref {
            reference.validate("paper_engram_module.eval_report_ref")?;
        }
        if matches!(
            self.quality_claim,
            PaperEngramQualityClaim::Posttrain
                | PaperEngramQualityClaim::Finetune
                | PaperEngramQualityClaim::Imported
        ) {
            if self.training_recipe_ref.is_none() {
                return Err(LingquMemoryError::MissingField(
                    "paper_engram_module.training_recipe_ref",
                ));
            }
            if self.eval_report_ref.is_none() {
                return Err(LingquMemoryError::MissingField(
                    "paper_engram_module.eval_report_ref",
                ));
            }
        }
        let mut shard_ids = HashSet::new();
        for id in &self.table_shard_ids {
            required_str(id, "paper_engram_module.table_shard_ids")?;
            if !shard_ids.insert(id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_module.table_shard_ids",
                    reason: "duplicate table_shard_id",
                });
            }
        }
        let mut gate_ids = HashSet::new();
        for id in &self.gate_ids {
            required_str(id, "paper_engram_module.gate_ids")?;
            if !gate_ids.insert(id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_module.gate_ids",
                    reason: "duplicate gate_id",
                });
            }
        }
        if self.layers.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_module.layers",
            ));
        }
        let mut layers = HashSet::new();
        for layer in &self.layers {
            if !layers.insert(*layer) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_module.layers",
                    reason: "duplicate layer index",
                });
            }
        }
        if self.orders.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_module.orders",
            ));
        }
        let mut orders = HashSet::new();
        for order in &self.orders {
            if *order == 0 {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_module.orders",
                    reason: "order must be greater than 0",
                });
            }
            if !orders.insert(*order) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_module.orders",
                    reason: "duplicate order",
                });
            }
        }
        nonzero(
            self.heads_per_order as u64,
            "paper_engram_module.heads_per_order",
        )?;
        nonzero(self.hidden_size, "paper_engram_module.hidden_size")?;
        nonzero(self.memory_dim, "paper_engram_module.memory_dim")?;
        validate_tensor_dtype(self.table_dtype, "paper_engram_module.table_dtype")?;
        required_str(&self.table_layout, "paper_engram_module.table_layout")?;
        required_str(&self.gate_kind, "paper_engram_module.gate_kind")?;
        if self.payload_checksums.is_empty() {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_module.payload_checksums",
            ));
        }
        for checksum in &self.payload_checksums {
            nonzero(*checksum, "paper_engram_module.payload_checksums")?;
        }
        nonzero(self.version, "paper_engram_module.version")?;
        nonzero(self.created_at_us, "paper_engram_module.created_at_us")?;
        if let Some(expires_at_us) = self.expires_at_us {
            if expires_at_us <= self.created_at_us {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_module.expires_at_us",
                    reason: "expires_at_us must be greater than created_at_us",
                });
            }
        }
        nonzero(self.checksum, "paper_engram_module.checksum")?;
        let actual = paper_engram_module_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.module_id.clone(),
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
pub struct PaperEngramModuleRegistryEntry {
    pub module_id: String,
    pub module: PaperEngramModuleManifest,
}

impl PaperEngramModuleRegistryEntry {
    pub fn new(module: PaperEngramModuleManifest) -> MemoryResult<Self> {
        let entry = Self {
            module_id: module.module_id.clone(),
            module,
        };
        entry.validate()?;
        Ok(entry)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        required_str(
            &self.module_id,
            "paper_engram_module_registry_entry.module_id",
        )?;
        if self.module_id != self.module.module_id {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module_registry_entry.module_id",
                reason: "entry module_id must match module.manifest module_id",
            });
        }
        self.module.validate()?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PaperEngramModuleRegistryManifest {
    pub kind: String,
    pub schema_version: u32,
    pub entries: Vec<PaperEngramModuleRegistryEntry>,
    pub checksum: u64,
}

impl PaperEngramModuleRegistryManifest {
    pub fn new(mut entries: Vec<PaperEngramModuleRegistryEntry>) -> MemoryResult<Self> {
        entries.sort_by(|left, right| left.module_id.cmp(&right.module_id));
        let mut manifest = Self {
            kind: LINGQU_PAPER_ENGRAM_MODULE_REGISTRY_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            entries,
            checksum: 0,
        };
        manifest.checksum = paper_engram_module_registry_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_PAPER_ENGRAM_MODULE_REGISTRY_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module_registry_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module_registry_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for entry in &self.entries {
            entry.validate()?;
            if !ids.insert(entry.module_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_module_registry_manifest.entries",
                    reason: "duplicate module_id",
                });
            }
        }
        let actual = paper_engram_module_registry_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_PAPER_ENGRAM_MODULE_REGISTRY_PATH.to_string(),
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
pub struct PaperEngramTrainingRecipeManifestCollection {
    pub kind: String,
    pub schema_version: u32,
    pub recipes: Vec<PaperEngramTrainingRecipeManifest>,
    pub checksum: u64,
}

impl PaperEngramTrainingRecipeManifestCollection {
    pub fn new(mut recipes: Vec<PaperEngramTrainingRecipeManifest>) -> MemoryResult<Self> {
        recipes.sort_by(|left, right| left.recipe_id.cmp(&right.recipe_id));
        let mut manifest = Self {
            kind: LINGQU_PAPER_ENGRAM_TRAINING_RECIPE_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            recipes,
            checksum: 0,
        };
        manifest.checksum = paper_engram_training_recipe_manifest_collection_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_PAPER_ENGRAM_TRAINING_RECIPE_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_training_recipe_collection_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_training_recipe_collection_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for recipe in &self.recipes {
            recipe.validate()?;
            if !ids.insert(recipe.recipe_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_training_recipe_collection_manifest.recipes",
                    reason: "duplicate recipe_id",
                });
            }
        }
        let actual = paper_engram_training_recipe_manifest_collection_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_PAPER_ENGRAM_TRAINING_RECIPE_MANIFEST_PATH.to_string(),
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
pub struct PaperEngramEvalReportManifestCollection {
    pub kind: String,
    pub schema_version: u32,
    pub reports: Vec<PaperEngramEvalReportManifest>,
    pub checksum: u64,
}

impl PaperEngramEvalReportManifestCollection {
    pub fn new(mut reports: Vec<PaperEngramEvalReportManifest>) -> MemoryResult<Self> {
        reports.sort_by(|left, right| left.report_id.cmp(&right.report_id));
        let mut manifest = Self {
            kind: LINGQU_PAPER_ENGRAM_EVAL_REPORT_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            reports,
            checksum: 0,
        };
        manifest.checksum = paper_engram_eval_report_manifest_collection_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_PAPER_ENGRAM_EVAL_REPORT_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report_collection_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report_collection_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for report in &self.reports {
            report.validate()?;
            if !ids.insert(report.report_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_eval_report_collection_manifest.reports",
                    reason: "duplicate report_id",
                });
            }
        }
        let actual = paper_engram_eval_report_manifest_collection_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_PAPER_ENGRAM_EVAL_REPORT_MANIFEST_PATH.to_string(),
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
pub struct PaperEngramTokenizerProjectionManifestCollection {
    pub kind: String,
    pub schema_version: u32,
    pub projections: Vec<PaperEngramTokenizerProjectionManifest>,
    pub checksum: u64,
}

impl PaperEngramTokenizerProjectionManifestCollection {
    pub fn new(mut projections: Vec<PaperEngramTokenizerProjectionManifest>) -> MemoryResult<Self> {
        projections.sort_by(|left, right| left.projection_id.cmp(&right.projection_id));
        let mut manifest = Self {
            kind: LINGQU_PAPER_ENGRAM_TOKENIZER_PROJECTION_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            projections,
            checksum: 0,
        };
        manifest.checksum =
            paper_engram_tokenizer_projection_manifest_collection_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_PAPER_ENGRAM_TOKENIZER_PROJECTION_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_tokenizer_projection_collection_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_tokenizer_projection_collection_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for projection in &self.projections {
            projection.validate()?;
            if !ids.insert(projection.projection_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_tokenizer_projection_collection_manifest.projections",
                    reason: "duplicate projection id",
                });
            }
        }
        let actual = paper_engram_tokenizer_projection_manifest_collection_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_PAPER_ENGRAM_TOKENIZER_PROJECTION_MANIFEST_PATH.to_string(),
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
pub struct PaperEngramHashConfigManifestCollection {
    pub kind: String,
    pub schema_version: u32,
    pub hash_configs: Vec<PaperEngramHashConfigManifest>,
    pub checksum: u64,
}

impl PaperEngramHashConfigManifestCollection {
    pub fn new(mut hash_configs: Vec<PaperEngramHashConfigManifest>) -> MemoryResult<Self> {
        hash_configs.sort_by(|left, right| left.hash_config_id.cmp(&right.hash_config_id));
        let mut manifest = Self {
            kind: LINGQU_PAPER_ENGRAM_HASH_CONFIG_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            hash_configs,
            checksum: 0,
        };
        manifest.checksum = paper_engram_hash_config_manifest_collection_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_PAPER_ENGRAM_HASH_CONFIG_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_hash_config_collection_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_hash_config_collection_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for hash_config in &self.hash_configs {
            hash_config.validate()?;
            if !ids.insert(hash_config.hash_config_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_hash_config_collection_manifest.hash_configs",
                    reason: "duplicate hash config id",
                });
            }
        }
        let actual = paper_engram_hash_config_manifest_collection_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_PAPER_ENGRAM_HASH_CONFIG_MANIFEST_PATH.to_string(),
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
pub struct PaperEngramTableShardManifestCollection {
    pub kind: String,
    pub schema_version: u32,
    pub shards: Vec<PaperEngramTableShardManifest>,
    pub checksum: u64,
}

impl PaperEngramTableShardManifestCollection {
    pub fn new(mut shards: Vec<PaperEngramTableShardManifest>) -> MemoryResult<Self> {
        shards.sort_by(|left, right| left.shard_id.cmp(&right.shard_id));
        let mut manifest = Self {
            kind: LINGQU_PAPER_ENGRAM_TABLE_SHARD_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            shards,
            checksum: 0,
        };
        manifest.checksum = paper_engram_table_shard_manifest_collection_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_PAPER_ENGRAM_TABLE_SHARD_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_shard_collection_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_shard_collection_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for shard in &self.shards {
            shard.validate()?;
            if !ids.insert(shard.shard_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_table_shard_collection_manifest.shards",
                    reason: "duplicate shard id",
                });
            }
        }
        let actual = paper_engram_table_shard_manifest_collection_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_PAPER_ENGRAM_TABLE_SHARD_MANIFEST_PATH.to_string(),
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
pub struct PaperEngramGateManifestCollection {
    pub kind: String,
    pub schema_version: u32,
    pub gates: Vec<PaperEngramGateManifest>,
    pub checksum: u64,
}

impl PaperEngramGateManifestCollection {
    pub fn new(mut gates: Vec<PaperEngramGateManifest>) -> MemoryResult<Self> {
        gates.sort_by(|left, right| left.gate_id.cmp(&right.gate_id));
        let mut manifest = Self {
            kind: LINGQU_PAPER_ENGRAM_GATE_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            gates,
            checksum: 0,
        };
        manifest.checksum = paper_engram_gate_manifest_collection_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_PAPER_ENGRAM_GATE_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_gate_collection_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_gate_collection_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for gate in &self.gates {
            gate.validate()?;
            if !ids.insert(gate.gate_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_gate_collection_manifest.gates",
                    reason: "duplicate gate id",
                });
            }
        }
        let actual = paper_engram_gate_manifest_collection_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_PAPER_ENGRAM_GATE_MANIFEST_PATH.to_string(),
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

fn validate_tensor_dtype(dtype: TensorDType, _field: &'static str) -> MemoryResult<()> {
    match dtype {
        TensorDType::U8
        | TensorDType::U32
        | TensorDType::U64
        | TensorDType::F32
        | TensorDType::Opaque => Ok(()),
    }
}

fn paper_engram_tokenizer_projection_manifest_checksum(
    manifest: &PaperEngramTokenizerProjectionManifest,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.projection_id);
    push_checksum_str(&mut bytes, &manifest.model_id);
    push_checksum_str(&mut bytes, &manifest.tokenizer_id);
    push_checksum_str(&mut bytes, &manifest.projection_ref.path);
    bytes.extend_from_slice(&manifest.projection_checksum.to_le_bytes());
    if let Some(source_ref) = &manifest.source_ref {
        push_checksum_str(&mut bytes, source_ref);
    }
    bytes.extend_from_slice(&manifest.version.to_le_bytes());
    bytes.extend_from_slice(&manifest.created_at_us.to_le_bytes());
    bytes.extend_from_slice(&manifest.expires_at_us.unwrap_or(0).to_le_bytes());
    checksum64(&bytes)
}

fn paper_engram_hash_config_manifest_checksum(manifest: &PaperEngramHashConfigManifest) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.hash_config_id);
    push_checksum_str(&mut bytes, &manifest.model_id);
    push_checksum_str(&mut bytes, &manifest.tokenizer_projection_id);
    bytes.extend_from_slice(&manifest.tokenizer_projection_checksum.to_le_bytes());
    push_checksum_str(&mut bytes, &manifest.hash_config_ref.path);
    bytes.extend_from_slice(&manifest.hash_config_checksum.to_le_bytes());
    for order in &manifest.orders {
        bytes.extend_from_slice(&(*order as u64).to_le_bytes());
    }
    bytes.extend_from_slice(&manifest.heads_per_order.to_le_bytes());
    bytes.extend_from_slice(&manifest.table_rows.to_le_bytes());
    for spec in &manifest.table_specs {
        bytes.extend_from_slice(&(spec.order as u64).to_le_bytes());
        bytes.extend_from_slice(&(spec.head as u64).to_le_bytes());
        bytes.extend_from_slice(&spec.table_rows.to_le_bytes());
        bytes.extend_from_slice(&spec.seed.to_le_bytes());
    }
    bytes.extend_from_slice(&manifest.seed.to_le_bytes());
    push_checksum_str(&mut bytes, &manifest.algorithm);
    if let Some(source_ref) = &manifest.source_ref {
        push_checksum_str(&mut bytes, source_ref);
    }
    bytes.extend_from_slice(&manifest.version.to_le_bytes());
    bytes.extend_from_slice(&manifest.created_at_us.to_le_bytes());
    bytes.extend_from_slice(&manifest.expires_at_us.unwrap_or(0).to_le_bytes());
    checksum64(&bytes)
}

fn paper_engram_table_shard_manifest_checksum(manifest: &PaperEngramTableShardManifest) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.shard_id);
    push_checksum_str(&mut bytes, &manifest.model_id);
    bytes.extend_from_slice(&manifest.layer.to_le_bytes());
    bytes.extend_from_slice(&manifest.order.to_le_bytes());
    bytes.extend_from_slice(&manifest.head.to_le_bytes());
    bytes.extend_from_slice(&manifest.row_start.to_le_bytes());
    bytes.extend_from_slice(&manifest.row_end.to_le_bytes());
    bytes.extend_from_slice(&tensor_dtype_tag(manifest.dtype).to_le_bytes());
    for dim in &manifest.shape {
        bytes.extend_from_slice(&dim.to_le_bytes());
    }
    for payload_ref in &manifest.block_payload_refs {
        push_checksum_str(&mut bytes, &payload_ref.block.0);
        bytes.extend_from_slice(&payload_ref.offset.to_le_bytes());
        bytes.extend_from_slice(&payload_ref.bytes.to_le_bytes());
        bytes.extend_from_slice(&payload_ref.checksum.to_le_bytes());
    }
    if let Some(source_ref) = &manifest.source_ref {
        push_checksum_str(&mut bytes, source_ref);
    }
    bytes.extend_from_slice(&manifest.version.to_le_bytes());
    bytes.extend_from_slice(&manifest.created_at_us.to_le_bytes());
    bytes.extend_from_slice(&manifest.expires_at_us.unwrap_or(0).to_le_bytes());
    checksum64(&bytes)
}

fn paper_engram_gate_manifest_checksum(manifest: &PaperEngramGateManifest) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.gate_id);
    push_checksum_str(&mut bytes, &manifest.model_id);
    bytes.extend_from_slice(&manifest.layer.to_le_bytes());
    bytes.extend_from_slice(&tensor_dtype_tag(manifest.dtype).to_le_bytes());
    for dim in &manifest.shape {
        bytes.extend_from_slice(&dim.to_le_bytes());
    }
    if let Some(payload_ref) = &manifest.payload_ref {
        push_checksum_str(&mut bytes, &payload_ref.block.0);
        bytes.extend_from_slice(&payload_ref.offset.to_le_bytes());
        bytes.extend_from_slice(&payload_ref.bytes.to_le_bytes());
        bytes.extend_from_slice(&payload_ref.checksum.to_le_bytes());
    }
    if let Some(source_ref) = &manifest.source_ref {
        push_checksum_str(&mut bytes, source_ref);
    }
    bytes.extend_from_slice(&manifest.version.to_le_bytes());
    bytes.extend_from_slice(&manifest.created_at_us.to_le_bytes());
    bytes.extend_from_slice(&manifest.expires_at_us.unwrap_or(0).to_le_bytes());
    checksum64(&bytes)
}

fn paper_engram_quality_claim_tag(claim: &PaperEngramQualityClaim) -> u64 {
    match claim {
        PaperEngramQualityClaim::None => 0,
        PaperEngramQualityClaim::Posttrain => 1,
        PaperEngramQualityClaim::Finetune => 2,
        PaperEngramQualityClaim::Imported => 3,
    }
}

pub fn paper_engram_training_recipe_dfs_path(recipe_id: &str) -> LingquDfsPath {
    LingquDfsPath::new(format!(
        "/lingqu/memory/engram/training-recipes/{}.json",
        lingqu_memory_path_id(recipe_id)
    ))
}

pub fn paper_engram_eval_report_dfs_path(report_id: &str) -> LingquDfsPath {
    LingquDfsPath::new(format!(
        "/lingqu/memory/engram/eval-reports/{}.json",
        lingqu_memory_path_id(report_id)
    ))
}

fn lingqu_memory_path_id(id: &str) -> String {
    id.chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' {
                ch
            } else {
                '_'
            }
        })
        .collect()
}

fn paper_engram_training_mode_tag(mode: PaperEngramTrainingMode) -> u64 {
    match mode {
        PaperEngramTrainingMode::EngramOnlyContinuedPretrain => 1,
        PaperEngramTrainingMode::EngramLora => 2,
        PaperEngramTrainingMode::FullFinetune => 3,
        PaperEngramTrainingMode::ExternalImport => 4,
    }
}

fn paper_engram_quality_claim_accepts_training_mode(
    claim: PaperEngramQualityClaim,
    mode: PaperEngramTrainingMode,
) -> bool {
    matches!(
        (claim, mode),
        (
            PaperEngramQualityClaim::Posttrain,
            PaperEngramTrainingMode::EngramOnlyContinuedPretrain
        ) | (
            PaperEngramQualityClaim::Finetune,
            PaperEngramTrainingMode::EngramLora | PaperEngramTrainingMode::FullFinetune
        ) | (
            PaperEngramQualityClaim::Imported,
            PaperEngramTrainingMode::ExternalImport
        )
    )
}

fn paper_engram_training_recipe_manifest_checksum(
    manifest: &PaperEngramTrainingRecipeManifest,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.recipe_id);
    push_checksum_str(&mut bytes, &manifest.model.model_id);
    push_checksum_str(&mut bytes, &manifest.model.model_key);
    bytes.extend_from_slice(&manifest.model.tokenizer_hash.to_le_bytes());
    bytes.extend_from_slice(&manifest.model.profile_hash.to_le_bytes());
    bytes.extend_from_slice(&paper_engram_training_mode_tag(manifest.mode).to_le_bytes());
    bytes.extend_from_slice(&manifest.base_checkpoint_checksum.to_le_bytes());
    push_checksum_str(&mut bytes, &manifest.tokenizer_projection_ref.path);
    push_checksum_str(&mut bytes, &manifest.hash_config_ref.path);
    for dataset_ref in &manifest.dataset_refs {
        push_checksum_str(&mut bytes, dataset_ref);
    }
    push_checksum_str(&mut bytes, &manifest.objective);
    bytes.extend_from_slice(&(manifest.frozen_base_model as u64).to_le_bytes());
    bytes.extend_from_slice(&(manifest.lora_enabled as u64).to_le_bytes());
    push_checksum_str(&mut bytes, &manifest.table_init);
    push_checksum_str(&mut bytes, &manifest.gate_init);
    for layer in &manifest.layers {
        bytes.extend_from_slice(&layer.to_le_bytes());
    }
    for order in &manifest.orders {
        bytes.extend_from_slice(&(*order as u64).to_le_bytes());
    }
    bytes.extend_from_slice(&manifest.heads_per_order.to_le_bytes());
    bytes.extend_from_slice(&manifest.table_rows.to_le_bytes());
    for evidence_ref in &manifest.evidence_refs {
        push_checksum_str(&mut bytes, evidence_ref);
    }
    bytes.extend_from_slice(&manifest.version.to_le_bytes());
    bytes.extend_from_slice(&manifest.created_at_us.to_le_bytes());
    bytes.extend_from_slice(&manifest.expires_at_us.unwrap_or(0).to_le_bytes());
    checksum64(&bytes)
}

fn paper_engram_eval_report_manifest_checksum(manifest: &PaperEngramEvalReportManifest) -> u64 {
    paper_engram_eval_report_manifest_checksum_with_runtime_context(manifest, true)
}

fn paper_engram_eval_report_manifest_legacy_checksum_without_runtime_context(
    manifest: &PaperEngramEvalReportManifest,
) -> u64 {
    paper_engram_eval_report_manifest_checksum_with_runtime_context(manifest, false)
}

fn paper_engram_eval_report_manifest_checksum_with_runtime_context(
    manifest: &PaperEngramEvalReportManifest,
    include_runtime_context: bool,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.report_id);
    push_checksum_str(&mut bytes, &manifest.recipe_id);
    push_checksum_str(&mut bytes, &manifest.module_id);
    push_checksum_str(&mut bytes, &manifest.model.model_id);
    push_checksum_str(&mut bytes, &manifest.model.model_key);
    bytes.extend_from_slice(&manifest.model.tokenizer_hash.to_le_bytes());
    bytes.extend_from_slice(&manifest.model.profile_hash.to_le_bytes());
    for validation_set_ref in &manifest.validation_set_refs {
        push_checksum_str(&mut bytes, validation_set_ref);
    }
    bytes.extend_from_slice(&manifest.sample_count.to_le_bytes());
    bytes.extend_from_slice(&manifest.baseline_loss_milli.to_le_bytes());
    bytes.extend_from_slice(&manifest.paper_engram_loss_milli.to_le_bytes());
    bytes.extend_from_slice(&manifest.decode_policy_loss_milli.unwrap_or(0).to_le_bytes());
    bytes.extend_from_slice(
        &manifest
            .paper_engram_decode_policy_loss_milli
            .unwrap_or(0)
            .to_le_bytes(),
    );
    bytes.extend_from_slice(&manifest.max_allowed_regression_milli.to_le_bytes());
    bytes.extend_from_slice(&manifest.output_checksum.to_le_bytes());
    bytes.extend_from_slice(
        &manifest
            .zero_table_hidden_checksum
            .unwrap_or(0)
            .to_le_bytes(),
    );
    bytes.extend_from_slice(
        &manifest
            .paper_engram_hidden_checksum
            .unwrap_or(0)
            .to_le_bytes(),
    );
    bytes.extend_from_slice(
        &manifest
            .zero_table_output_checksum
            .unwrap_or(0)
            .to_le_bytes(),
    );
    bytes.extend_from_slice(
        &u64::from(manifest.cpu_backend_output_match == Some(true)).to_le_bytes(),
    );
    bytes.extend_from_slice(&manifest.row_prefetch_requests.unwrap_or(0).to_le_bytes());
    bytes.extend_from_slice(&manifest.row_prefetch_hits.unwrap_or(0).to_le_bytes());
    if include_runtime_context {
        bytes.extend_from_slice(
            &manifest
                .runtime_context_steps_expected
                .unwrap_or(0)
                .to_le_bytes(),
        );
        bytes.extend_from_slice(
            &manifest
                .runtime_context_steps_observed
                .unwrap_or(0)
                .to_le_bytes(),
        );
    }
    bytes.extend_from_slice(&manifest.max_backend_latency_us.unwrap_or(0).to_le_bytes());
    bytes.extend_from_slice(
        &manifest
            .max_allowed_backend_latency_us
            .unwrap_or(0)
            .to_le_bytes(),
    );
    for evidence_ref in &manifest.evidence_refs {
        push_checksum_str(&mut bytes, evidence_ref);
    }
    bytes.extend_from_slice(&manifest.version.to_le_bytes());
    bytes.extend_from_slice(&manifest.created_at_us.to_le_bytes());
    bytes.extend_from_slice(&manifest.expires_at_us.unwrap_or(0).to_le_bytes());
    checksum64(&bytes)
}

fn paper_engram_module_manifest_checksum(manifest: &PaperEngramModuleManifest) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.module_id);
    push_checksum_str(&mut bytes, &manifest.module_name);
    push_checksum_str(&mut bytes, &manifest.model.model_id);
    push_checksum_str(&mut bytes, &manifest.model.model_key);
    bytes.extend_from_slice(&manifest.model.tokenizer_hash.to_le_bytes());
    bytes.extend_from_slice(&manifest.model.profile_hash.to_le_bytes());
    bytes.extend_from_slice(&manifest.base_checkpoint_checksum.to_le_bytes());
    push_checksum_str(&mut bytes, &manifest.tokenizer_id);
    push_checksum_str(&mut bytes, &manifest.tokenizer_projection_ref.path);
    push_checksum_str(&mut bytes, &manifest.hash_config_ref.path);
    if let Some(recipe_ref) = &manifest.training_recipe_ref {
        push_checksum_str(&mut bytes, &recipe_ref.path);
    }
    if let Some(eval_ref) = &manifest.eval_report_ref {
        push_checksum_str(&mut bytes, &eval_ref.path);
    }
    for shard_id in &manifest.table_shard_ids {
        push_checksum_str(&mut bytes, shard_id);
    }
    for gate_id in &manifest.gate_ids {
        push_checksum_str(&mut bytes, gate_id);
    }
    for layer in &manifest.layers {
        bytes.extend_from_slice(&layer.to_le_bytes());
    }
    for order in &manifest.orders {
        bytes.extend_from_slice(&(*order as u64).to_le_bytes());
    }
    bytes.extend_from_slice(&manifest.heads_per_order.to_le_bytes());
    bytes.extend_from_slice(&manifest.hidden_size.to_le_bytes());
    bytes.extend_from_slice(&manifest.memory_dim.to_le_bytes());
    bytes.extend_from_slice(&tensor_dtype_tag(manifest.table_dtype).to_le_bytes());
    push_checksum_str(&mut bytes, &manifest.table_layout);
    push_checksum_str(&mut bytes, &manifest.gate_kind);
    bytes.extend_from_slice(&paper_engram_quality_claim_tag(&manifest.quality_claim).to_le_bytes());
    for checksum in &manifest.payload_checksums {
        bytes.extend_from_slice(&checksum.to_le_bytes());
    }
    bytes.extend_from_slice(&manifest.version.to_le_bytes());
    bytes.extend_from_slice(&manifest.created_at_us.to_le_bytes());
    bytes.extend_from_slice(&manifest.expires_at_us.unwrap_or(0).to_le_bytes());
    checksum64(&bytes)
}

fn validate_trained_paper_engram_source_ref(
    field: &'static str,
    source_ref: Option<&str>,
    require_source_ref: bool,
) -> MemoryResult<()> {
    let Some(source_ref) = source_ref else {
        return if require_source_ref {
            Err(LingquMemoryError::MissingField(field))
        } else {
            Ok(())
        };
    };
    required_str(source_ref, field)?;
    if paper_engram_source_ref_is_fixture(source_ref) {
        return Err(LingquMemoryError::InvalidValue {
            field,
            reason: "trained paper Engram quality requires non-fixture table and gate provenance",
        });
    }
    Ok(())
}

fn validate_trained_paper_engram_provenance_ref(
    field: &'static str,
    provenance_ref: &str,
) -> MemoryResult<()> {
    required_str(provenance_ref, field)?;
    if paper_engram_source_ref_is_fixture(provenance_ref) {
        return Err(LingquMemoryError::InvalidValue {
            field,
            reason:
                "trained paper Engram quality requires non-fixture training and eval provenance",
        });
    }
    Ok(())
}

fn validate_trained_paper_engram_train_eval_split(
    recipe: &PaperEngramTrainingRecipeManifest,
    report: &PaperEngramEvalReportManifest,
) -> MemoryResult<()> {
    for validation_set_ref in &report.validation_set_refs {
        if recipe
            .dataset_refs
            .iter()
            .any(|dataset_ref| dataset_ref == validation_set_ref)
        {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.validation_set_refs",
                reason: "trained paper Engram quality requires validation sets distinct from training datasets",
            });
        }
    }
    Ok(())
}

fn validate_paper_engram_phase6_summary_provenance(
    report: &PaperEngramEvalReportManifest,
) -> MemoryResult<()> {
    const VARIANTS: [(&str, &str); 4] = [
        (
            "w5-summary:base:",
            "paper_engram_eval_report.evidence_refs.phase6_base_summary",
        ),
        (
            "w5-summary:base_decode_policy:",
            "paper_engram_eval_report.evidence_refs.phase6_decode_policy_summary",
        ),
        (
            "w5-summary:paper_engram:",
            "paper_engram_eval_report.evidence_refs.phase6_paper_engram_summary",
        ),
        (
            "w5-summary:paper_engram_decode_policy:",
            "paper_engram_eval_report.evidence_refs.phase6_paper_engram_decode_policy_summary",
        ),
    ];
    for (prefix, field) in VARIANTS {
        if !report
            .evidence_refs
            .iter()
            .any(|evidence_ref| evidence_ref.starts_with(prefix))
        {
            return Err(LingquMemoryError::MissingField(field));
        }
    }
    Ok(())
}

fn paper_engram_source_ref_is_fixture(source_ref: &str) -> bool {
    let normalized = source_ref.replace('\\', "/").to_ascii_lowercase();
    normalized == "fixture"
        || normalized.starts_with("fixture://")
        || normalized.starts_with("fixture/")
        || normalized
            .split('/')
            .any(|component| component == "fixture" || component == "fixtures")
}

fn paper_engram_module_registry_manifest_checksum(
    manifest: &PaperEngramModuleRegistryManifest,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.kind);
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    for entry in &manifest.entries {
        push_checksum_str(&mut bytes, &entry.module_id);
        bytes.extend_from_slice(&entry.module.checksum.to_le_bytes());
    }
    checksum64(&bytes)
}

fn paper_engram_training_recipe_manifest_collection_checksum(
    manifest: &PaperEngramTrainingRecipeManifestCollection,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.kind);
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    for recipe in &manifest.recipes {
        push_checksum_str(&mut bytes, &recipe.recipe_id);
        bytes.extend_from_slice(&recipe.checksum.to_le_bytes());
    }
    checksum64(&bytes)
}

fn paper_engram_eval_report_manifest_collection_checksum(
    manifest: &PaperEngramEvalReportManifestCollection,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.kind);
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    for report in &manifest.reports {
        push_checksum_str(&mut bytes, &report.report_id);
        bytes.extend_from_slice(&report.checksum.to_le_bytes());
    }
    checksum64(&bytes)
}

fn paper_engram_tokenizer_projection_manifest_collection_checksum(
    manifest: &PaperEngramTokenizerProjectionManifestCollection,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.kind);
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    for projection in &manifest.projections {
        bytes.extend_from_slice(&projection.checksum.to_le_bytes());
    }
    checksum64(&bytes)
}

fn paper_engram_hash_config_manifest_collection_checksum(
    manifest: &PaperEngramHashConfigManifestCollection,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.kind);
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    for hash_config in &manifest.hash_configs {
        bytes.extend_from_slice(&hash_config.checksum.to_le_bytes());
    }
    checksum64(&bytes)
}

fn paper_engram_table_shard_manifest_collection_checksum(
    manifest: &PaperEngramTableShardManifestCollection,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.kind);
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    for shard in &manifest.shards {
        bytes.extend_from_slice(&shard.checksum.to_le_bytes());
    }
    checksum64(&bytes)
}

fn paper_engram_gate_manifest_collection_checksum(
    manifest: &PaperEngramGateManifestCollection,
) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &manifest.kind);
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    for gate in &manifest.gates {
        bytes.extend_from_slice(&gate.checksum.to_le_bytes());
    }
    checksum64(&bytes)
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
pub struct LingquShortpathSupportManifest {
    pub kind: String,
    pub schema_version: u32,
    pub supports: Vec<ShortpathSupportRecord>,
    pub checksum: u64,
}

impl LingquShortpathSupportManifest {
    pub fn new(mut supports: Vec<ShortpathSupportRecord>) -> MemoryResult<Self> {
        supports.sort_by(|left, right| left.support_id.cmp(&right.support_id));
        let mut manifest = Self {
            kind: LINGQU_SHORTPATH_SUPPORT_MANIFEST_KIND.to_string(),
            schema_version: LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION,
            supports,
            checksum: 0,
        };
        manifest.checksum = shortpath_support_manifest_checksum(&manifest);
        manifest.validate()?;
        Ok(manifest)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        if self.kind != LINGQU_SHORTPATH_SUPPORT_MANIFEST_KIND {
            return Err(LingquMemoryError::InvalidValue {
                field: "shortpath_support_manifest.kind",
                reason: "unexpected manifest kind",
            });
        }
        if self.schema_version != LINGQU_MEMORY_MANIFEST_SCHEMA_VERSION {
            return Err(LingquMemoryError::InvalidValue {
                field: "shortpath_support_manifest.schema_version",
                reason: "unsupported manifest schema version",
            });
        }
        let mut ids = HashSet::new();
        for support in &self.supports {
            support.validate()?;
            if !ids.insert(support.support_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "shortpath_support_manifest.supports",
                    reason: "duplicate support id",
                });
            }
        }
        let actual = shortpath_support_manifest_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: LINGQU_SHORTPATH_SUPPORT_MANIFEST_PATH.to_string(),
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
        let existing = self.load_query_result_audit_entries(true, "persist query result")?;
        let mut existing_by_id = HashMap::new();
        for (existing_result, existing_bytes) in existing {
            if existing_by_id
                .insert(existing_result.result_id.clone(), existing_bytes)
                .is_some()
            {
                return Err(LingquMemoryError::InvalidValue {
                    field: "query_result_id",
                    reason: "duplicate query result id in durable audit log",
                });
            }
        }

        let mut ops = vec![durable_sim::LingquDurableBatchOp::DfsWrite {
            path: path.path.clone(),
            bytes: bytes.clone(),
            options: durable_sim::LingquDfsWriteOptions {
                content_type: durable_sim::LingquDfsContentType::Json,
                ..durable_sim::LingquDfsWriteOptions::default()
            },
        }];
        let mut audit_appends = 0;
        let mut audit_bytes_written = 0;
        let next_seq = existing_by_id.len() as u64 + 1;
        if let Some(existing_bytes) = existing_by_id.get(&result.result_id) {
            if existing_bytes != &bytes {
                return Err(LingquMemoryError::InvalidValue {
                    field: "query_result_id",
                    reason: "query result id already exists with different payload",
                });
            }
        } else {
            audit_appends = 1;
            audit_bytes_written = bytes.len() as u64;
            ops.push(durable_sim::LingquDurableBatchOp::DfsAppendLog {
                path: LINGQU_QUERY_RESULT_AUDIT_LOG_PATH.to_string(),
                bytes: bytes.clone(),
                options: durable_sim::LingquDfsAppendOptions {
                    expected_next_seq: Some(next_seq),
                    writer: Some("lingqu-memory-service".to_string()),
                    metadata: durable_audit_metadata("query_result"),
                },
            });
        }
        self.durable
            .commit_batch(ops)
            .map_err(memory_error_from_durable)?;
        self.stats.dfs_catalog_writes += 1;
        self.stats.dfs_bytes_written += bytes.len() as u64 + audit_bytes_written;
        self.stats.dfs_audit_appends += audit_appends;
        Ok(path)
    }

    pub fn load_query_result(&mut self, path: &LingquDfsPath) -> MemoryResult<QueryResult> {
        path.validate("query_result.dfs_path")?;
        let bytes = self.submit_dfs_read(&path.path)?;
        QueryResult::from_json_bytes(&bytes)
    }

    pub fn load_query_result_by_id(
        &mut self,
        result_id: &str,
    ) -> MemoryResult<(LingquDfsPath, QueryResult)> {
        let path = query_result_dfs_path(result_id)?;
        let result = self.load_query_result(&path)?;
        Ok((path, result))
    }

    pub fn load_query_result_audit_manifest(&mut self) -> MemoryResult<Vec<QueryResult>> {
        Ok(self
            .load_query_result_audit_entries(false, "load query result audit")?
            .into_iter()
            .map(|(result, _bytes)| result)
            .collect())
    }

    pub fn persist_record_lifecycle_event_manifest(
        &mut self,
        events: Vec<MemoryRecordLifecycleEvent>,
    ) -> MemoryResult<LingquDfsPath> {
        let path = LingquDfsPath::new(LINGQU_RECORD_LIFECYCLE_AUDIT_LOG_PATH);
        let mut events = events;
        events.sort_by(|left, right| left.event_id.cmp(&right.event_id));
        let existing =
            self.load_record_lifecycle_event_audit_entries(true, "persist record lifecycle")?;
        let mut existing_by_id = HashMap::new();
        for (event, bytes) in existing {
            if existing_by_id
                .insert(event.event_id.clone(), bytes)
                .is_some()
            {
                return Err(LingquMemoryError::InvalidValue {
                    field: "record_lifecycle.event_id",
                    reason: "duplicate lifecycle event id in durable audit log",
                });
            }
        }

        let mut ops = Vec::new();
        let mut bytes_written = 0;
        let mut next_seq = existing_by_id.len() as u64 + 1;
        for event in events {
            event.validate()?;
            let bytes = serde_json::to_vec(&event)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            if let Some(existing_bytes) = existing_by_id.get(&event.event_id) {
                if existing_bytes != &bytes {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "record_lifecycle.event_id",
                        reason: "lifecycle event id already exists with different payload",
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
                    metadata: durable_audit_metadata("record_lifecycle"),
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

    pub fn load_record_lifecycle_event_manifest(
        &mut self,
    ) -> MemoryResult<Vec<MemoryRecordLifecycleEvent>> {
        Ok(self
            .load_record_lifecycle_event_audit_entries(false, "load record lifecycle")?
            .into_iter()
            .map(|(event, _bytes)| event)
            .collect())
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

    pub fn persist_artifact_access_manifest(
        &mut self,
        events: Vec<ArtifactAccessRecord>,
    ) -> MemoryResult<LingquDfsPath> {
        let path = LingquDfsPath::new(LINGQU_ARTIFACT_ACCESS_AUDIT_LOG_PATH);
        let mut events = events;
        events.sort_by(|left, right| left.event_id.cmp(&right.event_id));
        let existing = self.load_artifact_access_audit_entries(true, "persist artifact access")?;
        let mut existing_by_id = HashMap::new();
        for (event, bytes) in existing {
            if existing_by_id
                .insert(event.event_id.clone(), bytes)
                .is_some()
            {
                return Err(LingquMemoryError::InvalidValue {
                    field: "artifact_access.event_id",
                    reason: "duplicate artifact access event id in durable audit log",
                });
            }
        }

        let mut ops = Vec::new();
        let mut bytes_written = 0;
        for event in events {
            event.validate()?;
            let bytes = serde_json::to_vec(&event)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            if let Some(existing_bytes) = existing_by_id.get(&event.event_id) {
                if existing_bytes != &bytes {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "artifact_access.event_id",
                        reason: "artifact access event id already exists with different payload",
                    });
                }
                continue;
            }
            bytes_written += bytes.len() as u64;
            ops.push(durable_sim::LingquDurableBatchOp::DfsAppendLog {
                path: path.path.clone(),
                bytes,
                options: durable_sim::LingquDfsAppendOptions {
                    expected_next_seq: None,
                    writer: Some("lingqu-memory-service".to_string()),
                    metadata: durable_audit_metadata("artifact_access"),
                },
            });
        }
        if !ops.is_empty() {
            let append_count = ops.len() as u64;
            self.durable
                .commit_batch(ops)
                .map_err(memory_error_from_durable)?;
            self.stats.dfs_audit_appends += append_count;
            self.stats.dfs_bytes_written += bytes_written;
        }
        Ok(path)
    }

    pub fn load_artifact_access_manifest(&mut self) -> MemoryResult<Vec<ArtifactAccessRecord>> {
        Ok(self
            .load_artifact_access_audit_entries(false, "load artifact access")?
            .into_iter()
            .map(|(event, _bytes)| event)
            .collect())
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

    pub fn persist_paper_engram_tokenizer_projection_manifest(
        &mut self,
        manifests: Vec<PaperEngramTokenizerProjectionManifest>,
    ) -> MemoryResult<LingquDfsPath> {
        let manifest = PaperEngramTokenizerProjectionManifestCollection::new(manifests)?;
        let bytes = manifest.to_json_bytes()?;
        let path = LingquDfsPath::new(LINGQU_PAPER_ENGRAM_TOKENIZER_PROJECTION_MANIFEST_PATH);
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_paper_engram_tokenizer_projection_manifest(
        &mut self,
    ) -> MemoryResult<Vec<PaperEngramTokenizerProjectionManifest>> {
        let bytes = self.submit_dfs_read(LINGQU_PAPER_ENGRAM_TOKENIZER_PROJECTION_MANIFEST_PATH)?;
        Ok(PaperEngramTokenizerProjectionManifestCollection::from_json_bytes(&bytes)?.projections)
    }

    pub fn persist_paper_engram_hash_config_manifest(
        &mut self,
        manifests: Vec<PaperEngramHashConfigManifest>,
    ) -> MemoryResult<LingquDfsPath> {
        let manifest = PaperEngramHashConfigManifestCollection::new(manifests)?;
        let bytes = manifest.to_json_bytes()?;
        let path = LingquDfsPath::new(LINGQU_PAPER_ENGRAM_HASH_CONFIG_MANIFEST_PATH);
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_paper_engram_hash_config_manifest(
        &mut self,
    ) -> MemoryResult<Vec<PaperEngramHashConfigManifest>> {
        let bytes = self.submit_dfs_read(LINGQU_PAPER_ENGRAM_HASH_CONFIG_MANIFEST_PATH)?;
        Ok(PaperEngramHashConfigManifestCollection::from_json_bytes(&bytes)?.hash_configs)
    }

    pub fn persist_paper_engram_training_recipe_manifest(
        &mut self,
        manifests: Vec<PaperEngramTrainingRecipeManifest>,
    ) -> MemoryResult<LingquDfsPath> {
        for recipe in &manifests {
            self.submit_dfs_write(
                paper_engram_training_recipe_dfs_path(&recipe.recipe_id).path,
                recipe.to_json_bytes()?,
            )?;
        }
        let manifest = PaperEngramTrainingRecipeManifestCollection::new(manifests)?;
        let bytes = manifest.to_json_bytes()?;
        let path = LingquDfsPath::new(LINGQU_PAPER_ENGRAM_TRAINING_RECIPE_MANIFEST_PATH);
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_paper_engram_training_recipe_manifest(
        &mut self,
    ) -> MemoryResult<Vec<PaperEngramTrainingRecipeManifest>> {
        let bytes = self.submit_dfs_read(LINGQU_PAPER_ENGRAM_TRAINING_RECIPE_MANIFEST_PATH)?;
        Ok(PaperEngramTrainingRecipeManifestCollection::from_json_bytes(&bytes)?.recipes)
    }

    pub fn persist_paper_engram_eval_report_manifest(
        &mut self,
        manifests: Vec<PaperEngramEvalReportManifest>,
    ) -> MemoryResult<LingquDfsPath> {
        for report in &manifests {
            self.submit_dfs_write(
                paper_engram_eval_report_dfs_path(&report.report_id).path,
                report.to_json_bytes()?,
            )?;
        }
        let manifest = PaperEngramEvalReportManifestCollection::new(manifests)?;
        let bytes = manifest.to_json_bytes()?;
        let path = LingquDfsPath::new(LINGQU_PAPER_ENGRAM_EVAL_REPORT_MANIFEST_PATH);
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_paper_engram_eval_report_manifest(
        &mut self,
    ) -> MemoryResult<Vec<PaperEngramEvalReportManifest>> {
        let bytes = self.submit_dfs_read(LINGQU_PAPER_ENGRAM_EVAL_REPORT_MANIFEST_PATH)?;
        Ok(PaperEngramEvalReportManifestCollection::from_json_bytes(&bytes)?.reports)
    }

    pub fn persist_paper_engram_table_shard_manifest(
        &mut self,
        manifests: Vec<PaperEngramTableShardManifest>,
    ) -> MemoryResult<LingquDfsPath> {
        let manifest = PaperEngramTableShardManifestCollection::new(manifests)?;
        let bytes = manifest.to_json_bytes()?;
        let path = LingquDfsPath::new(LINGQU_PAPER_ENGRAM_TABLE_SHARD_MANIFEST_PATH);
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_paper_engram_table_shard_manifest(
        &mut self,
    ) -> MemoryResult<Vec<PaperEngramTableShardManifest>> {
        let bytes = self.submit_dfs_read(LINGQU_PAPER_ENGRAM_TABLE_SHARD_MANIFEST_PATH)?;
        Ok(PaperEngramTableShardManifestCollection::from_json_bytes(&bytes)?.shards)
    }

    pub fn persist_paper_engram_gate_manifest(
        &mut self,
        manifests: Vec<PaperEngramGateManifest>,
    ) -> MemoryResult<LingquDfsPath> {
        let manifest = PaperEngramGateManifestCollection::new(manifests)?;
        let bytes = manifest.to_json_bytes()?;
        let path = LingquDfsPath::new(LINGQU_PAPER_ENGRAM_GATE_MANIFEST_PATH);
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_paper_engram_gate_manifest(
        &mut self,
    ) -> MemoryResult<Vec<PaperEngramGateManifest>> {
        let bytes = self.submit_dfs_read(LINGQU_PAPER_ENGRAM_GATE_MANIFEST_PATH)?;
        Ok(PaperEngramGateManifestCollection::from_json_bytes(&bytes)?.gates)
    }

    pub fn persist_paper_engram_module_registry(
        &mut self,
        entries: Vec<PaperEngramModuleRegistryEntry>,
    ) -> MemoryResult<LingquDfsPath> {
        let manifest = PaperEngramModuleRegistryManifest::new(entries)?;
        let bytes = manifest.to_json_bytes()?;
        let path = LingquDfsPath::new(LINGQU_PAPER_ENGRAM_MODULE_REGISTRY_PATH);
        self.submit_dfs_write(path.path.clone(), bytes)?;
        Ok(path)
    }

    pub fn load_paper_engram_module_registry(
        &mut self,
    ) -> MemoryResult<Vec<PaperEngramModuleRegistryEntry>> {
        let bytes = self.submit_dfs_read(LINGQU_PAPER_ENGRAM_MODULE_REGISTRY_PATH)?;
        Ok(PaperEngramModuleRegistryManifest::from_json_bytes(&bytes)?.entries)
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
                    writer: Some("w5-runtime-planner".to_string()),
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

    pub fn persist_shortpath_support_manifest(
        &mut self,
        supports: Vec<ShortpathSupportRecord>,
    ) -> MemoryResult<LingquDfsPath> {
        let path = LingquDfsPath::new(LINGQU_SHORTPATH_SUPPORT_AUDIT_LOG_PATH);
        let mut supports = supports;
        supports.sort_by(|left, right| left.support_id.cmp(&right.support_id));
        let existing =
            self.load_shortpath_support_audit_entries(true, "persist shortpath supports")?;
        let mut existing_by_id = HashMap::new();
        for (support, bytes) in existing {
            if existing_by_id
                .insert(support.support_id.clone(), bytes)
                .is_some()
            {
                return Err(LingquMemoryError::InvalidValue {
                    field: "shortpath_support.support_id",
                    reason: "duplicate support id in durable audit log",
                });
            }
        }

        let mut ops = Vec::new();
        let mut bytes_written = 0;
        let mut next_seq = existing_by_id.len() as u64 + 1;
        for support in supports {
            support.validate()?;
            let bytes = serde_json::to_vec(&support)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            if let Some(existing_bytes) = existing_by_id.get(&support.support_id) {
                if existing_bytes != &bytes {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "shortpath_support.support_id",
                        reason: "support id already exists with different payload",
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
                    metadata: durable_audit_metadata("shortpath_support"),
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

    pub fn load_shortpath_support_manifest(&mut self) -> MemoryResult<Vec<ShortpathSupportRecord>> {
        Ok(self
            .load_shortpath_support_audit_entries(false, "load shortpath supports")?
            .into_iter()
            .map(|(support, _bytes)| support)
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

    pub fn persist_boundary_observation_manifest(
        &mut self,
        observations: Vec<BoundaryObservationRecord>,
    ) -> MemoryResult<LingquDfsPath> {
        let path = LingquDfsPath::new(LINGQU_BOUNDARY_OBSERVATION_AUDIT_LOG_PATH);
        let mut observations = observations;
        observations.sort_by(|left, right| left.observation_id.cmp(&right.observation_id));
        let existing =
            self.load_boundary_observation_audit_entries(true, "persist boundary observations")?;
        let mut existing_by_id = HashMap::new();
        for (observation, bytes) in existing {
            if existing_by_id
                .insert(observation.observation_id.clone(), bytes)
                .is_some()
            {
                return Err(LingquMemoryError::InvalidValue {
                    field: "boundary_observation.observation_id",
                    reason: "duplicate observation id in durable audit log",
                });
            }
        }

        let mut ops = Vec::new();
        let mut bytes_written = 0;
        let mut next_seq = existing_by_id.len() as u64 + 1;
        for observation in observations {
            observation.validate()?;
            let bytes = serde_json::to_vec(&observation)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            if let Some(existing_bytes) = existing_by_id.get(&observation.observation_id) {
                if existing_bytes != &bytes {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "boundary_observation.observation_id",
                        reason: "observation id already exists with different payload",
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
                    writer: Some("w5-range-exit-observer".to_string()),
                    metadata: durable_audit_metadata("boundary_observation"),
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

    pub fn load_boundary_observation_manifest(
        &mut self,
    ) -> MemoryResult<Vec<BoundaryObservationRecord>> {
        Ok(self
            .load_boundary_observation_audit_entries(false, "load boundary observations")?
            .into_iter()
            .map(|(observation, _bytes)| observation)
            .collect())
    }

    pub fn persist_prefix_cache_reuse_plan_manifest(
        &mut self,
        plans: Vec<PrefixCacheReusePlan>,
    ) -> MemoryResult<LingquDfsPath> {
        let path = LingquDfsPath::new(LINGQU_PREFIX_CACHE_REUSE_AUDIT_LOG_PATH);
        let mut plans = plans;
        plans.sort_by(|left, right| left.plan_id.cmp(&right.plan_id));
        let existing =
            self.load_prefix_cache_reuse_plan_audit_entries(true, "persist prefix cache reuse")?;
        let mut existing_by_id = HashMap::new();
        for (plan, bytes) in existing {
            if existing_by_id.insert(plan.plan_id.clone(), bytes).is_some() {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefix_cache_reuse.plan_id",
                    reason: "duplicate reuse plan id in durable audit log",
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
                        field: "prefix_cache_reuse.plan_id",
                        reason: "reuse plan id already exists with different payload",
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
                    metadata: durable_audit_metadata("prefix_cache_reuse"),
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

    pub fn load_prefix_cache_reuse_plan_manifest(
        &mut self,
    ) -> MemoryResult<Vec<PrefixCacheReusePlan>> {
        Ok(self
            .load_prefix_cache_reuse_plan_audit_entries(false, "load prefix cache reuse")?
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
        let mut staged_payload_blocks = HashSet::new();
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
            let checksum = checksum64(&bytes);
            let payload_ref = LingquBlockPayloadRef {
                block: block.clone(),
                offset: 0,
                bytes: bytes.len() as u64,
                checksum,
            };
            payload_ref.validate("object_payload_ref")?;
            if payload_ref.bytes != entry.record.bytes {
                return Err(LingquMemoryError::InvalidValue {
                    field: "object_payload_ref.bytes",
                    reason: "payload ref byte count must match object record",
                });
            }
            entry.payload_ref = Some(payload_ref);
            if staged_payload_blocks.insert(block.clone())
                && !self.object_payload_block_matches(&block, bytes.len() as u64, checksum)?
            {
                block_bytes_written += bytes.len() as u64;
                block_writes += 1;
                ops.push(durable_sim::LingquDurableBatchOp::BlockWrite {
                    block: block.0,
                    bytes,
                    options: durable_sim::LingquBlockWriteOptions::default(),
                });
            }
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

    fn load_artifact_access_audit_entries(
        &mut self,
        allow_missing: bool,
        op: &'static str,
    ) -> MemoryResult<Vec<(ArtifactAccessRecord, Vec<u8>)>> {
        let records =
            self.submit_dfs_append_log_read(LINGQU_ARTIFACT_ACCESS_AUDIT_LOG_PATH, allow_missing)?;
        let mut entries = Vec::with_capacity(records.len());
        let mut ids = HashSet::new();
        for record in records {
            let event = serde_json::from_slice::<ArtifactAccessRecord>(&record.bytes)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            event.validate()?;
            if !ids.insert(event.event_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "artifact_access.event_id",
                    reason: "duplicate artifact access event id in durable audit log",
                });
            }
            entries.push((event, record.bytes));
        }
        if entries.is_empty() && !allow_missing {
            return Err(LingquMemoryError::MissingDfsPath(format!(
                "{op}: {LINGQU_ARTIFACT_ACCESS_AUDIT_LOG_PATH}"
            )));
        }
        Ok(entries)
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

    fn load_shortpath_support_audit_entries(
        &mut self,
        allow_missing: bool,
        op: &'static str,
    ) -> MemoryResult<Vec<(ShortpathSupportRecord, Vec<u8>)>> {
        let records = self
            .submit_dfs_append_log_read(LINGQU_SHORTPATH_SUPPORT_AUDIT_LOG_PATH, allow_missing)?;
        let mut entries = Vec::with_capacity(records.len());
        let mut ids = HashSet::new();
        for record in records {
            let support = serde_json::from_slice::<ShortpathSupportRecord>(&record.bytes)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            support.validate()?;
            if !ids.insert(support.support_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "shortpath_support.support_id",
                    reason: "duplicate support id in durable audit log",
                });
            }
            entries.push((support, record.bytes));
        }
        if entries.is_empty() && !allow_missing {
            return Err(LingquMemoryError::MissingDfsPath(format!(
                "{op}: {LINGQU_SHORTPATH_SUPPORT_AUDIT_LOG_PATH}"
            )));
        }
        Ok(entries)
    }

    fn load_query_result_audit_entries(
        &mut self,
        allow_missing: bool,
        op: &'static str,
    ) -> MemoryResult<Vec<(QueryResult, Vec<u8>)>> {
        let records =
            self.submit_dfs_append_log_read(LINGQU_QUERY_RESULT_AUDIT_LOG_PATH, allow_missing)?;
        let mut entries = Vec::with_capacity(records.len());
        let mut ids = HashSet::new();
        for record in records {
            let result = QueryResult::from_json_bytes(&record.bytes)?;
            if !ids.insert(result.result_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "query_result_id",
                    reason: "duplicate query result id in durable audit log",
                });
            }
            entries.push((result, record.bytes));
        }
        if entries.is_empty() && !allow_missing {
            return Err(LingquMemoryError::MissingDfsPath(format!(
                "{op}: {LINGQU_QUERY_RESULT_AUDIT_LOG_PATH}"
            )));
        }
        Ok(entries)
    }

    fn load_record_lifecycle_event_audit_entries(
        &mut self,
        allow_missing: bool,
        op: &'static str,
    ) -> MemoryResult<Vec<(MemoryRecordLifecycleEvent, Vec<u8>)>> {
        let records =
            self.submit_dfs_append_log_read(LINGQU_RECORD_LIFECYCLE_AUDIT_LOG_PATH, allow_missing)?;
        let mut entries = Vec::with_capacity(records.len());
        let mut ids = HashSet::new();
        for record in records {
            let event = serde_json::from_slice::<MemoryRecordLifecycleEvent>(&record.bytes)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            event.validate()?;
            if !ids.insert(event.event_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "record_lifecycle.event_id",
                    reason: "duplicate lifecycle event id in durable audit log",
                });
            }
            entries.push((event, record.bytes));
        }
        if entries.is_empty() && !allow_missing {
            return Err(LingquMemoryError::MissingDfsPath(format!(
                "{op}: {LINGQU_RECORD_LIFECYCLE_AUDIT_LOG_PATH}"
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

    fn load_prefix_cache_reuse_plan_audit_entries(
        &mut self,
        allow_missing: bool,
        op: &'static str,
    ) -> MemoryResult<Vec<(PrefixCacheReusePlan, Vec<u8>)>> {
        let records = self
            .submit_dfs_append_log_read(LINGQU_PREFIX_CACHE_REUSE_AUDIT_LOG_PATH, allow_missing)?;
        let mut entries = Vec::with_capacity(records.len());
        let mut ids = HashSet::new();
        for record in records {
            let plan = serde_json::from_slice::<PrefixCacheReusePlan>(&record.bytes)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            plan.validate()?;
            if !ids.insert(plan.plan_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "prefix_cache_reuse.plan_id",
                    reason: "duplicate reuse plan id in durable audit log",
                });
            }
            entries.push((plan, record.bytes));
        }
        if entries.is_empty() && !allow_missing {
            return Err(LingquMemoryError::MissingDfsPath(format!(
                "{op}: {LINGQU_PREFIX_CACHE_REUSE_AUDIT_LOG_PATH}"
            )));
        }
        Ok(entries)
    }

    fn load_boundary_observation_audit_entries(
        &mut self,
        allow_missing: bool,
        op: &'static str,
    ) -> MemoryResult<Vec<(BoundaryObservationRecord, Vec<u8>)>> {
        let records = self.submit_dfs_append_log_read(
            LINGQU_BOUNDARY_OBSERVATION_AUDIT_LOG_PATH,
            allow_missing,
        )?;
        let mut entries = Vec::with_capacity(records.len());
        let mut ids = HashSet::new();
        for record in records {
            let observation = serde_json::from_slice::<BoundaryObservationRecord>(&record.bytes)
                .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
            observation.validate()?;
            if !ids.insert(observation.observation_id.clone()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "boundary_observation.observation_id",
                    reason: "duplicate observation id in durable audit log",
                });
            }
            entries.push((observation, record.bytes));
        }
        if entries.is_empty() && !allow_missing {
            return Err(LingquMemoryError::MissingDfsPath(format!(
                "{op}: {LINGQU_BOUNDARY_OBSERVATION_AUDIT_LOG_PATH}"
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

    fn object_payload_block_matches(
        &self,
        block: &BlockHash,
        bytes: u64,
        checksum: u64,
    ) -> MemoryResult<bool> {
        match self
            .durable
            .block_stat(block, durable_sim::LingquVersionSelector::LatestCommitted)
        {
            Ok(record) => Ok(record.bytes.len() as u64 == bytes && record.checksum == checksum),
            Err(durable_sim::LingquDurableError::MissingBlock(_)) => Ok(false),
            Err(err) => Err(memory_error_from_durable(err)),
        }
    }
}

impl Default for LingquMemoryDurableStore {
    fn default() -> Self {
        Self::new()
    }
}

const LINGQU_DURABLE_EXTERNAL_BLOCK_THRESHOLD_BYTES: u64 = 8 * 1024 * 1024;
const LINGQU_DURABLE_EXTERNAL_BLOCK_OFFSET_KEY: &str = "lingqu_external_block_offset";
const LINGQU_DURABLE_EXTERNAL_BLOCK_BYTES_KEY: &str = "lingqu_external_block_bytes";
const LINGQU_DURABLE_EXTERNAL_BLOCK_CHECKSUM_KEY: &str = "lingqu_external_block_checksum";

pub fn load_lingqu_memory_durable_store_from_path(
    path: &Path,
) -> MemoryResult<LingquMemoryDurableStore> {
    if !path.exists() {
        return Ok(LingquMemoryDurableStore::new());
    }
    let bytes = fs::read(path).map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
    if let Ok(snapshot) = durable_sim::LingquDurableSimSnapshot::from_json_bytes(&bytes) {
        return LingquMemoryDurableStore::import_durable_sim_snapshot(snapshot);
    }
    if let Ok(mut snapshot) =
        serde_json::from_slice::<durable_sim::LingquDurableSimSnapshot>(&bytes)
    {
        if hydrate_lingqu_durable_external_blocks(path, &mut snapshot)? {
            return LingquMemoryDurableStore::import_durable_sim_snapshot(snapshot);
        }
    }
    let legacy_snapshot = LingquMemoryDurableStoreSnapshot::from_json_bytes(&bytes)?;
    LingquMemoryDurableStore::import_snapshot(legacy_snapshot)
}

pub fn save_lingqu_memory_durable_store_to_path(
    path: &Path,
    store: &LingquMemoryDurableStore,
) -> MemoryResult<()> {
    let mut snapshot = store.export_durable_sim_snapshot()?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
    }
    let total_block_bytes = snapshot
        .block
        .blocks
        .iter()
        .map(|record| record.bytes.len() as u64)
        .sum::<u64>();
    let bytes = if total_block_bytes > LINGQU_DURABLE_EXTERNAL_BLOCK_THRESHOLD_BYTES {
        externalize_lingqu_durable_blocks(path, &mut snapshot)?;
        serde_json::to_vec_pretty(&snapshot)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?
    } else {
        snapshot
            .to_json_bytes()
            .map_err(memory_error_from_durable)?
    };
    fs::write(path, bytes).map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
}

fn lingqu_durable_block_sidecar_path(path: &Path) -> PathBuf {
    path.with_extension("bin")
}

fn externalize_lingqu_durable_blocks(
    path: &Path,
    snapshot: &mut durable_sim::LingquDurableSimSnapshot,
) -> MemoryResult<()> {
    let sidecar_path = lingqu_durable_block_sidecar_path(path);
    if let Some(parent) = sidecar_path.parent() {
        fs::create_dir_all(parent)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
    }
    let mut sidecar = fs::File::create(&sidecar_path)
        .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
    let mut offset = 0u64;
    for record in &mut snapshot.block.blocks {
        if record.bytes.is_empty() {
            continue;
        }
        let bytes = std::mem::take(&mut record.bytes);
        sidecar
            .write_all(&bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        record.metadata.insert(
            LINGQU_DURABLE_EXTERNAL_BLOCK_OFFSET_KEY.to_string(),
            offset.to_string(),
        );
        record.metadata.insert(
            LINGQU_DURABLE_EXTERNAL_BLOCK_BYTES_KEY.to_string(),
            bytes.len().to_string(),
        );
        record.metadata.insert(
            LINGQU_DURABLE_EXTERNAL_BLOCK_CHECKSUM_KEY.to_string(),
            format!("{:#x}", record.checksum),
        );
        offset = offset
            .checked_add(bytes.len() as u64)
            .ok_or(LingquMemoryError::InvalidValue {
                field: "durable_block_sidecar.offset",
                reason: "offset overflow",
            })?;
    }
    Ok(())
}

fn hydrate_lingqu_durable_external_blocks(
    path: &Path,
    snapshot: &mut durable_sim::LingquDurableSimSnapshot,
) -> MemoryResult<bool> {
    let needs_hydration = snapshot.block.blocks.iter().any(|record| {
        record.bytes.is_empty()
            && record
                .metadata
                .contains_key(LINGQU_DURABLE_EXTERNAL_BLOCK_OFFSET_KEY)
    });
    if !needs_hydration {
        return Ok(false);
    }

    let sidecar_path = lingqu_durable_block_sidecar_path(path);
    let mut sidecar = fs::File::open(&sidecar_path)
        .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
    for record in &mut snapshot.block.blocks {
        if !record.bytes.is_empty() {
            continue;
        }
        let Some(offset) = record
            .metadata
            .get(LINGQU_DURABLE_EXTERNAL_BLOCK_OFFSET_KEY)
            .map(|value| {
                value
                    .parse::<u64>()
                    .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))
            })
            .transpose()?
        else {
            continue;
        };
        let bytes_len = record
            .metadata
            .get(LINGQU_DURABLE_EXTERNAL_BLOCK_BYTES_KEY)
            .ok_or(LingquMemoryError::MissingField(
                "durable_block_sidecar.bytes",
            ))?
            .parse::<usize>()
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        sidecar
            .seek(SeekFrom::Start(offset))
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        let mut bytes = vec![0u8; bytes_len];
        sidecar
            .read_exact(&mut bytes)
            .map_err(|err| LingquMemoryError::SnapshotCodec(err.to_string()))?;
        record.bytes = bytes;
    }
    Ok(true)
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
    Ok(())
}

fn object_payload_block(record: &LingquObjectRecord) -> BlockHash {
    BlockHash(format!(
        "block/object-service/payload/{:016x}/bytes{}/checksum{:016x}",
        checksum64(record.key.as_bytes()),
        record.bytes,
        record.checksum
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
pub struct BoundaryTensorFingerprint {
    pub bytes: u64,
    pub checksum: u64,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
}

impl BoundaryTensorFingerprint {
    pub fn from_hot_ref(hot_ref: &HotTensorObjectRef) -> Self {
        Self {
            bytes: hot_ref.bytes,
            checksum: hot_ref.checksum,
            dtype: hot_ref.dtype,
            shape: hot_ref.shape.clone(),
        }
    }

    pub fn matches_hot_ref(&self, hot_ref: &HotTensorObjectRef) -> bool {
        self.bytes == hot_ref.bytes
            && self.checksum == hot_ref.checksum
            && self.dtype == hot_ref.dtype
            && self.shape == hot_ref.shape
    }

    fn validate(&self, field: &'static str) -> MemoryResult<()> {
        nonzero(self.bytes, field)?;
        nonzero(self.checksum, field)?;
        require_nonempty(&self.shape, field)?;
        for dim in &self.shape {
            nonzero(*dim, field)?;
        }
        Ok(())
    }
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
pub enum ArtifactAccessKind {
    Produced,
    Consumed,
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
    pub boundary_hidden_fingerprint: BoundaryTensorFingerprint,
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
        self.boundary_hidden_fingerprint
            .validate("execution_artifact.boundary_hidden_fingerprint")?;
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
pub struct ArtifactAccessRecord {
    pub event_id: String,
    pub artifact_id: String,
    pub access: ArtifactAccessKind,
    pub artifact_kind: ExecutionArtifactKind,
    pub model: InferenceModelBinding,
    pub boundary: RangeBoundary,
    pub run_id: String,
    pub batch_id: String,
    pub actor: String,
    pub request_id: Option<String>,
    pub artifact_checksum: u64,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
}

impl ArtifactAccessRecord {
    #[allow(clippy::too_many_arguments)]
    pub fn new(
        event_id: String,
        artifact_id: String,
        access: ArtifactAccessKind,
        artifact_kind: ExecutionArtifactKind,
        model: InferenceModelBinding,
        boundary: RangeBoundary,
        run_id: String,
        batch_id: String,
        actor: String,
        request_id: Option<String>,
        artifact_checksum: u64,
        version: u64,
        created_at_us: u64,
    ) -> MemoryResult<Self> {
        let mut record = Self {
            event_id,
            artifact_id,
            access,
            artifact_kind,
            model,
            boundary,
            run_id,
            batch_id,
            actor,
            request_id,
            artifact_checksum,
            checksum: 0,
            version,
            created_at_us,
        };
        record.checksum = artifact_access_checksum(&record);
        record.validate()?;
        Ok(record)
    }

    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.event_id, "artifact_access.event_id")?;
        required_str(&self.artifact_id, "artifact_access.artifact_id")?;
        self.model.validate()?;
        self.boundary.validate()?;
        required_str(&self.run_id, "artifact_access.run_id")?;
        required_str(&self.batch_id, "artifact_access.batch_id")?;
        required_str(&self.actor, "artifact_access.actor")?;
        if let Some(request_id) = &self.request_id {
            required_str(request_id, "artifact_access.request_id")?;
        }
        nonzero(self.artifact_checksum, "artifact_access.artifact_checksum")?;
        nonzero(self.checksum, "artifact_access.checksum")?;
        nonzero(self.version, "artifact_access.version")?;
        let actual = artifact_access_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.event_id.clone(),
                expected: self.checksum,
                actual,
            });
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
pub struct BoundaryObservationRecord {
    pub observation_id: String,
    pub run_id: String,
    pub model: InferenceModelBinding,
    pub boundary: RangeBoundary,
    pub hidden_state: HotTensorObjectRef,
    pub producer_node: String,
    pub consumer_node: String,
    pub source: String,
    pub checksum: u64,
    pub version: u64,
    pub created_at_us: u64,
}

impl BoundaryObservationRecord {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.observation_id, "boundary_observation.observation_id")?;
        required_str(&self.run_id, "boundary_observation.run_id")?;
        self.model.validate()?;
        self.boundary.validate()?;
        if self.boundary.phase != RangeBoundaryPhase::RangeExit {
            return Err(LingquMemoryError::InvalidValue {
                field: "boundary_observation.boundary.phase",
                reason: "boundary observations must record range exit boundaries",
            });
        }
        validate_hot_ref(&self.hidden_state)?;
        required_str(&self.producer_node, "boundary_observation.producer_node")?;
        required_str(&self.consumer_node, "boundary_observation.consumer_node")?;
        required_str(&self.source, "boundary_observation.source")?;
        nonzero(self.checksum, "boundary_observation.checksum")?;
        nonzero(self.version, "boundary_observation.version")?;
        let actual = boundary_observation_checksum(self);
        if actual != self.checksum {
            return Err(LingquMemoryError::PayloadChecksumMismatch {
                id: self.observation_id.clone(),
                expected: self.checksum,
                actual,
            });
        }
        Ok(())
    }

    pub fn new(
        observation_id: String,
        run_id: String,
        model: InferenceModelBinding,
        boundary: RangeBoundary,
        hidden_state: HotTensorObjectRef,
        producer_node: String,
        consumer_node: String,
        source: String,
        version: u64,
        created_at_us: u64,
    ) -> MemoryResult<Self> {
        let mut record = Self {
            observation_id,
            run_id,
            model,
            boundary,
            hidden_state,
            producer_node,
            consumer_node,
            source,
            checksum: 0,
            version,
            created_at_us,
        };
        record.checksum = boundary_observation_checksum(&record);
        record.validate()?;
        Ok(record)
    }

    pub fn to_lookup_request(
        &self,
        request_id: String,
        engram_state_id: Option<String>,
        min_confidence_milli: u32,
        allowed_actions: Vec<ShortpathAction>,
        created_at_us: u64,
    ) -> MemoryResult<BoundaryLookupRequest> {
        let request = BoundaryLookupRequest {
            request_id,
            model: self.model.clone(),
            boundary: self.boundary.clone(),
            hidden_state: self.hidden_state.clone(),
            engram_state_id,
            min_confidence_milli,
            allowed_actions,
            created_at_us,
        };
        request.validate()?;
        Ok(request)
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
pub struct ShortpathSupportRecord {
    pub support_id: String,
    pub request_id: String,
    pub supported_action: ShortpathAction,
    pub artifact_id: Option<String>,
    #[serde(default)]
    pub producer_position: Option<u64>,
    pub target_layer_start: Option<u32>,
    pub target_layer_end: Option<u32>,
    pub confidence_milli: u32,
    pub verify_required: bool,
    pub proof_checksum: u64,
    pub reason: String,
    pub created_at_us: u64,
    pub version: u64,
}

impl ShortpathSupportRecord {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.support_id, "shortpath_support.support_id")?;
        required_str(&self.request_id, "shortpath_support.request_id")?;
        required_str(&self.reason, "shortpath_support.reason")?;
        if self.confidence_milli > 1000 {
            return Err(LingquMemoryError::InvalidValue {
                field: "shortpath_support.confidence_milli",
                reason: "confidence_milli must be in [0, 1000]",
            });
        }
        match self.supported_action {
            ShortpathAction::Continue => {
                if self.artifact_id.is_some() {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "shortpath_support.artifact_id",
                        reason: "continue support must not reference an artifact",
                    });
                }
            }
            ShortpathAction::JumpToLayer | ShortpathAction::JumpToTerminal => {
                if self.artifact_id.as_deref().unwrap_or("").trim().is_empty() {
                    return Err(LingquMemoryError::MissingField(
                        "shortpath_support.artifact_id",
                    ));
                }
                if self.producer_position.is_none() {
                    return Err(LingquMemoryError::MissingField(
                        "shortpath_support.producer_position",
                    ));
                }
                let (Some(start), Some(end)) = (self.target_layer_start, self.target_layer_end)
                else {
                    return Err(LingquMemoryError::MissingField(
                        "shortpath_support.target_layer_range",
                    ));
                };
                if end < start
                    || (self.supported_action == ShortpathAction::JumpToLayer && end == start)
                {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "shortpath_support.target_layer_end",
                        reason: "target_layer_end must be greater than target_layer_start unless supported action is jump-to-terminal",
                    });
                }
            }
            ShortpathAction::RequireVerify => {
                if self.artifact_id.as_deref().unwrap_or("").trim().is_empty() {
                    return Err(LingquMemoryError::MissingField(
                        "shortpath_support.artifact_id",
                    ));
                }
                if !self.verify_required {
                    return Err(LingquMemoryError::InvalidValue {
                        field: "shortpath_support.verify_required",
                        reason: "require-verify support must set verify_required",
                    });
                }
            }
        }
        nonzero(self.proof_checksum, "shortpath_support.proof_checksum")?;
        nonzero(self.version, "shortpath_support.version")?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ShortpathDecisionRecord {
    pub decision_id: String,
    pub request_id: String,
    pub support_id: Option<String>,
    pub action: ShortpathAction,
    pub artifact_id: Option<String>,
    #[serde(default)]
    pub producer_position: Option<u64>,
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
        if let Some(support_id) = &self.support_id {
            required_str(support_id, "shortpath_decision.support_id")?;
        }
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
                if self.producer_position.is_none() {
                    return Err(LingquMemoryError::MissingField(
                        "shortpath_decision.producer_position",
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
    pub support: ShortpathSupportRecord,
    pub artifact: Option<ExecutionArtifactObject>,
}

impl BoundaryLookupResponse {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.request_id, "boundary_lookup_response.request_id")?;
        self.support.validate()?;
        if self.support.request_id != self.request_id {
            return Err(LingquMemoryError::InvalidValue {
                field: "boundary_lookup_response.support",
                reason: "support request_id must match response request_id",
            });
        }
        if let Some(artifact) = &self.artifact {
            artifact.validate()?;
            if self.support.artifact_id.as_deref() != Some(artifact.artifact_id.as_str()) {
                return Err(LingquMemoryError::InvalidValue {
                    field: "boundary_lookup_response.artifact",
                    reason: "artifact id must match support artifact id",
                });
            }
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PaperEngramRuntimeArtifacts {
    pub module: PaperEngramModuleManifest,
    pub tokenizer_projection: PaperEngramTokenizerProjectionManifest,
    pub hash_config: PaperEngramHashConfigManifest,
    pub table_shards: Vec<PaperEngramTableShardManifest>,
    pub gates: Vec<PaperEngramGateManifest>,
    pub layer_operands: Vec<PaperEngramRuntimeLayerOperands>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PaperEngramRuntimeLayerOperands {
    pub layer: u32,
    pub table_operands: Vec<PaperEngramRuntimeTableOperand>,
    pub gate_operands: Vec<PaperEngramRuntimeGateOperand>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PaperEngramRuntimeTableOperand {
    pub shard_id: String,
    pub layer: u32,
    pub order: u8,
    pub head: u32,
    pub row_start: u64,
    pub row_end: u64,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
    pub block_payload_refs: Vec<LingquBlockPayloadRef>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PaperEngramRuntimeGateOperand {
    pub gate_id: String,
    pub layer: u32,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
    pub payload_ref: LingquBlockPayloadRef,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PaperEngramTableRowBlockRequest {
    pub request_id: String,
    pub module_id: String,
    pub layer: u32,
    pub order: u8,
    pub head: u32,
    pub row_start: u64,
    pub row_end: u64,
    pub created_at_us: u64,
}

impl PaperEngramTableRowBlockRequest {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.request_id, "paper_engram_table_row_block.request_id")?;
        required_str(&self.module_id, "paper_engram_table_row_block.module_id")?;
        nonzero(self.order as u64, "paper_engram_table_row_block.order")?;
        if self.row_end <= self.row_start {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block.row_range",
                reason: "row_end must be greater than row_start",
            });
        }
        nonzero(
            self.created_at_us,
            "paper_engram_table_row_block.created_at_us",
        )?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PaperEngramTableRowBlockResponse {
    pub request_id: String,
    pub module_id: String,
    pub layer: u32,
    pub order: u8,
    pub head: u32,
    pub row_start: u64,
    pub row_end: u64,
    pub shard_id: String,
    pub shard_row_start: u64,
    pub shard_row_end: u64,
    pub dtype: TensorDType,
    pub shape: Vec<u64>,
    #[serde(default)]
    pub row_payload_offset_bytes: u64,
    #[serde(default)]
    pub row_payload_bytes: u64,
    pub block_payload_refs: Vec<LingquBlockPayloadRef>,
}

impl PaperEngramTableRowBlockResponse {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(
            &self.request_id,
            "paper_engram_table_row_block_response.request_id",
        )?;
        required_str(
            &self.module_id,
            "paper_engram_table_row_block_response.module_id",
        )?;
        nonzero(
            self.order as u64,
            "paper_engram_table_row_block_response.order",
        )?;
        if self.row_end <= self.row_start {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block_response.row_range",
                reason: "row_end must be greater than row_start",
            });
        }
        required_str(
            &self.shard_id,
            "paper_engram_table_row_block_response.shard_id",
        )?;
        if self.shard_row_start > self.row_start || self.shard_row_end < self.row_end {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block_response.shard_row_range",
                reason: "shard row range must cover requested row range",
            });
        }
        validate_tensor_dtype(self.dtype, "paper_engram_table_row_block_response.dtype")?;
        require_nonempty(&self.shape, "paper_engram_table_row_block_response.shape")?;
        for dim in &self.shape {
            nonzero(*dim, "paper_engram_table_row_block_response.shape")?;
        }
        validate_paper_engram_row_block_response_payload_window(self)?;
        nonzero(
            self.row_payload_bytes,
            "paper_engram_table_row_block_response.row_payload_bytes",
        )?;
        require_nonempty(
            &self.block_payload_refs,
            "paper_engram_table_row_block_response.block_payload_refs",
        )?;
        for payload_ref in &self.block_payload_refs {
            payload_ref.validate("paper_engram_table_row_block_response.block_payload_refs")?;
        }
        Ok(())
    }
}

fn validate_paper_engram_row_block_response_payload_window(
    response: &PaperEngramTableRowBlockResponse,
) -> MemoryResult<()> {
    let dtype_width = response
        .dtype
        .byte_width()
        .ok_or(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_row_block_response.dtype",
            reason: "row payload window validation requires fixed-width dtype",
        })?;
    if response.shape.len() < 2 {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_row_block_response.shape",
            reason: "row payload window validation requires rows and memory dimension",
        });
    }
    let shard_rows = response.shard_row_end - response.shard_row_start;
    if response.shape[0] != shard_rows {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_row_block_response.shape",
            reason: "first shape dimension must match shard row range",
        });
    }
    let row_stride_elems = response.shape[1..].iter().try_fold(1u64, |acc, dim| {
        acc.checked_mul(*dim)
            .ok_or(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block_response.shape",
                reason: "row stride exceeds u64",
            })
    })?;
    let row_stride_bytes =
        row_stride_elems
            .checked_mul(dtype_width)
            .ok_or(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block_response.shape",
                reason: "row stride bytes exceeds u64",
            })?;
    let expected_offset = (response.row_start - response.shard_row_start)
        .checked_mul(row_stride_bytes)
        .ok_or(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_row_block_response.row_range",
            reason: "row payload offset exceeds u64",
        })?;
    let expected_bytes = (response.row_end - response.row_start)
        .checked_mul(row_stride_bytes)
        .ok_or(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_row_block_response.row_range",
            reason: "row payload bytes exceeds u64",
        })?;
    if response.row_payload_offset_bytes != expected_offset
        || response.row_payload_bytes != expected_bytes
    {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_row_block_response.row_payload_window",
            reason: "row payload window must match row range, dtype, and shape",
        });
    }
    Ok(())
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PaperEngramTableRowPrefetchRequest {
    pub request_id: String,
    pub module_id: String,
    pub canonical_history: Vec<u64>,
    pub from_step: u64,
    pub created_at_us: u64,
}

impl PaperEngramTableRowPrefetchRequest {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(
            &self.request_id,
            "paper_engram_table_row_prefetch.request_id",
        )?;
        required_str(&self.module_id, "paper_engram_table_row_prefetch.module_id")?;
        require_nonempty(
            &self.canonical_history,
            "paper_engram_table_row_prefetch.canonical_history",
        )?;
        if self.from_step >= self.canonical_history.len() as u64 {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_prefetch.from_step",
                reason: "from_step must be within canonical_history",
            });
        }
        nonzero(
            self.created_at_us,
            "paper_engram_table_row_prefetch.created_at_us",
        )?;
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PaperEngramTableRowPrefetchRef {
    pub step_index: u64,
    pub layer: u32,
    pub order: u8,
    pub head: u32,
    pub row: u64,
    pub exact_key: u64,
    pub shard_id: String,
    #[serde(default)]
    pub row_payload_offset_bytes: u64,
    #[serde(default)]
    pub row_payload_bytes: u64,
    pub block_payload_refs: Vec<LingquBlockPayloadRef>,
}

impl PaperEngramTableRowPrefetchRef {
    pub fn validate(&self) -> MemoryResult<()> {
        nonzero(
            self.order as u64,
            "paper_engram_table_row_prefetch_ref.order",
        )?;
        required_str(
            &self.shard_id,
            "paper_engram_table_row_prefetch_ref.shard_id",
        )?;
        nonzero(
            self.row_payload_bytes,
            "paper_engram_table_row_prefetch_ref.row_payload_bytes",
        )?;
        require_nonempty(
            &self.block_payload_refs,
            "paper_engram_table_row_prefetch_ref.block_payload_refs",
        )?;
        for payload_ref in &self.block_payload_refs {
            payload_ref.validate("paper_engram_table_row_prefetch_ref.block_payload_refs")?;
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct PaperEngramTableRowPrefetchPlan {
    pub plan_id: String,
    pub request_id: String,
    pub module_id: String,
    #[serde(default)]
    pub tokenizer_projection_checksum: u64,
    #[serde(default)]
    pub hash_config_checksum: u64,
    pub canonical_history_len: u64,
    pub from_step: u64,
    pub rows: Vec<PaperEngramTableRowPrefetchRef>,
    pub created_at_us: u64,
}

impl PaperEngramTableRowPrefetchPlan {
    pub fn validate(&self) -> MemoryResult<()> {
        required_str(&self.plan_id, "paper_engram_table_row_prefetch.plan_id")?;
        required_str(
            &self.request_id,
            "paper_engram_table_row_prefetch.request_id",
        )?;
        required_str(&self.module_id, "paper_engram_table_row_prefetch.module_id")?;
        nonzero(
            self.tokenizer_projection_checksum,
            "paper_engram_table_row_prefetch.tokenizer_projection_checksum",
        )?;
        nonzero(
            self.hash_config_checksum,
            "paper_engram_table_row_prefetch.hash_config_checksum",
        )?;
        nonzero(
            self.canonical_history_len,
            "paper_engram_table_row_prefetch.canonical_history_len",
        )?;
        if self.from_step >= self.canonical_history_len {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_prefetch.from_step",
                reason: "from_step must be within canonical_history_len",
            });
        }
        nonzero(
            self.created_at_us,
            "paper_engram_table_row_prefetch.created_at_us",
        )?;
        for row in &self.rows {
            row.validate()?;
            if row.step_index < self.from_step || row.step_index >= self.canonical_history_len {
                return Err(LingquMemoryError::InvalidValue {
                    field: "paper_engram_table_row_prefetch_ref.step_index",
                    reason: "row step_index must be covered by the plan step range",
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
    record_lifecycle_events: HashMap<String, MemoryRecordLifecycleEvent>,
    hot_states: HashMap<String, HotMemoryStateObject>,
    engram_states: HashMap<String, EngramStateObject>,
    execution_artifacts: HashMap<String, ExecutionArtifactObject>,
    artifact_access_events: HashMap<String, ArtifactAccessRecord>,
    prefix_cache_artifacts: HashMap<String, PrefixCacheArtifact>,
    prefix_cache_reuse_plans: HashMap<String, PrefixCacheReusePlan>,
    prefetch_plans: HashMap<String, PrefetchPlanRecord>,
    shortpath_supports: HashMap<String, ShortpathSupportRecord>,
    boundary_observations: HashMap<String, BoundaryObservationRecord>,
    paper_engram_tokenizer_projections: HashMap<String, PaperEngramTokenizerProjectionManifest>,
    paper_engram_hash_configs: HashMap<String, PaperEngramHashConfigManifest>,
    paper_engram_training_recipes: HashMap<String, PaperEngramTrainingRecipeManifest>,
    paper_engram_eval_reports: HashMap<String, PaperEngramEvalReportManifest>,
    paper_engram_table_shards: HashMap<String, PaperEngramTableShardManifest>,
    paper_engram_gates: HashMap<String, PaperEngramGateManifest>,
    paper_engram_modules: HashMap<String, PaperEngramModuleManifest>,
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

    pub fn rebuild_query_results_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<QueryResult>> {
        let results = durable_store.load_query_result_audit_manifest()?;
        for result in &results {
            self.register_query_result(result.clone())?;
        }
        Ok(results)
    }

    pub fn persist_record_lifecycle_events_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut events = self
            .record_lifecycle_events
            .values()
            .cloned()
            .collect::<Vec<_>>();
        events.sort_by(|left, right| left.event_id.cmp(&right.event_id));
        durable_store.persist_record_lifecycle_event_manifest(events)
    }

    pub fn rebuild_record_lifecycle_events_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<MemoryRecordLifecycleEvent>> {
        let events = durable_store.load_record_lifecycle_event_manifest()?;
        for event in &events {
            event.validate()?;
            self.record_lifecycle_events
                .insert(event.event_id.clone(), event.clone());
        }
        Ok(events)
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

    pub fn persist_artifact_access_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut events = self
            .artifact_access_events
            .values()
            .cloned()
            .collect::<Vec<_>>();
        events.sort_by(|left, right| left.event_id.cmp(&right.event_id));
        durable_store.persist_artifact_access_manifest(events)
    }

    pub fn rebuild_artifact_access_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<ArtifactAccessRecord>> {
        let events = durable_store.load_artifact_access_manifest()?;
        for event in &events {
            self.record_artifact_access(event.clone())?;
        }
        Ok(events)
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

    pub fn persist_shortpath_supports_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut supports = self
            .shortpath_supports
            .values()
            .cloned()
            .collect::<Vec<_>>();
        supports.sort_by(|left, right| left.support_id.cmp(&right.support_id));
        durable_store.persist_shortpath_support_manifest(supports)
    }

    pub fn rebuild_shortpath_supports_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<ShortpathSupportRecord>> {
        let supports = durable_store.load_shortpath_support_manifest()?;
        for support in &supports {
            support.validate()?;
            self.shortpath_supports
                .insert(support.support_id.clone(), support.clone());
        }
        Ok(supports)
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

    pub fn persist_prefix_cache_reuse_plans_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut plans = self
            .prefix_cache_reuse_plans
            .values()
            .cloned()
            .collect::<Vec<_>>();
        plans.sort_by(|left, right| left.plan_id.cmp(&right.plan_id));
        durable_store.persist_prefix_cache_reuse_plan_manifest(plans)
    }

    pub fn rebuild_prefix_cache_reuse_plans_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PrefixCacheReusePlan>> {
        let plans = durable_store.load_prefix_cache_reuse_plan_manifest()?;
        for plan in &plans {
            plan.validate()?;
            self.prefix_cache_reuse_plans
                .insert(plan.plan_id.clone(), plan.clone());
        }
        Ok(plans)
    }

    pub fn persist_paper_engram_tokenizer_projections_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut manifests = self
            .paper_engram_tokenizer_projections
            .values()
            .cloned()
            .collect::<Vec<_>>();
        manifests.sort_by(|left, right| left.projection_id.cmp(&right.projection_id));
        durable_store.persist_paper_engram_tokenizer_projection_manifest(manifests)
    }

    pub fn rebuild_paper_engram_tokenizer_projections_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PaperEngramTokenizerProjectionManifest>> {
        let manifests = durable_store.load_paper_engram_tokenizer_projection_manifest()?;
        for manifest in &manifests {
            self.register_paper_engram_tokenizer_projection(manifest.clone())?;
        }
        Ok(manifests)
    }

    pub fn persist_paper_engram_hash_configs_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut manifests = self
            .paper_engram_hash_configs
            .values()
            .cloned()
            .collect::<Vec<_>>();
        manifests.sort_by(|left, right| left.hash_config_id.cmp(&right.hash_config_id));
        durable_store.persist_paper_engram_hash_config_manifest(manifests)
    }

    pub fn rebuild_paper_engram_hash_configs_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PaperEngramHashConfigManifest>> {
        let manifests = durable_store.load_paper_engram_hash_config_manifest()?;
        for manifest in &manifests {
            self.register_paper_engram_hash_config(manifest.clone())?;
        }
        Ok(manifests)
    }

    pub fn persist_paper_engram_training_recipes_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut manifests = self
            .paper_engram_training_recipes
            .values()
            .cloned()
            .collect::<Vec<_>>();
        manifests.sort_by(|left, right| left.recipe_id.cmp(&right.recipe_id));
        durable_store.persist_paper_engram_training_recipe_manifest(manifests)
    }

    pub fn rebuild_paper_engram_training_recipes_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PaperEngramTrainingRecipeManifest>> {
        let manifests = durable_store.load_paper_engram_training_recipe_manifest()?;
        for manifest in &manifests {
            self.register_paper_engram_training_recipe(manifest.clone())?;
        }
        Ok(manifests)
    }

    pub fn persist_paper_engram_eval_reports_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut manifests = self
            .paper_engram_eval_reports
            .values()
            .cloned()
            .collect::<Vec<_>>();
        manifests.sort_by(|left, right| left.report_id.cmp(&right.report_id));
        durable_store.persist_paper_engram_eval_report_manifest(manifests)
    }

    pub fn rebuild_paper_engram_eval_reports_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PaperEngramEvalReportManifest>> {
        let manifests = durable_store.load_paper_engram_eval_report_manifest()?;
        for manifest in &manifests {
            self.register_paper_engram_eval_report(manifest.clone())?;
        }
        Ok(manifests)
    }

    pub fn persist_paper_engram_table_shards_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut manifests = self
            .paper_engram_table_shards
            .values()
            .cloned()
            .collect::<Vec<_>>();
        manifests.sort_by(|left, right| left.shard_id.cmp(&right.shard_id));
        durable_store.persist_paper_engram_table_shard_manifest(manifests)
    }

    pub fn rebuild_paper_engram_table_shards_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PaperEngramTableShardManifest>> {
        let manifests = durable_store.load_paper_engram_table_shard_manifest()?;
        for manifest in &manifests {
            self.register_paper_engram_table_shard(manifest.clone())?;
        }
        Ok(manifests)
    }

    pub fn persist_paper_engram_gates_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut manifests = self
            .paper_engram_gates
            .values()
            .cloned()
            .collect::<Vec<_>>();
        manifests.sort_by(|left, right| left.gate_id.cmp(&right.gate_id));
        durable_store.persist_paper_engram_gate_manifest(manifests)
    }

    pub fn rebuild_paper_engram_gates_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PaperEngramGateManifest>> {
        let manifests = durable_store.load_paper_engram_gate_manifest()?;
        for manifest in &manifests {
            self.register_paper_engram_gate(manifest.clone())?;
        }
        Ok(manifests)
    }

    pub fn persist_paper_engram_modules_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut modules = self
            .paper_engram_modules
            .values()
            .cloned()
            .collect::<Vec<_>>();
        modules.sort_by(|left, right| left.module_id.cmp(&right.module_id));
        durable_store.persist_paper_engram_module_registry(
            modules
                .into_iter()
                .map(PaperEngramModuleRegistryEntry::new)
                .collect::<MemoryResult<Vec<_>>>()?,
        )
    }

    pub fn rebuild_paper_engram_modules_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<PaperEngramModuleManifest>> {
        let entries = durable_store.load_paper_engram_module_registry()?;
        let mut manifests = Vec::with_capacity(entries.len());
        for entry in &entries {
            entry.validate()?;
            self.register_paper_engram_module(entry.module.clone())?;
            manifests.push(entry.module.clone());
        }
        Ok(manifests)
    }

    pub fn register_boundary_observation(
        &mut self,
        observation: BoundaryObservationRecord,
    ) -> MemoryResult<()> {
        observation.validate()?;
        self.boundary_observations
            .insert(observation.observation_id.clone(), observation);
        Ok(())
    }

    pub fn boundary_observation(&self, observation_id: &str) -> Option<&BoundaryObservationRecord> {
        self.boundary_observations.get(observation_id)
    }

    pub fn persist_boundary_observations_to_dfs(
        &self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<LingquDfsPath> {
        let mut observations = self
            .boundary_observations
            .values()
            .cloned()
            .collect::<Vec<_>>();
        observations.sort_by(|left, right| left.observation_id.cmp(&right.observation_id));
        durable_store.persist_boundary_observation_manifest(observations)
    }

    pub fn rebuild_boundary_observations_from_dfs(
        &mut self,
        durable_store: &mut LingquMemoryDurableStore,
    ) -> MemoryResult<Vec<BoundaryObservationRecord>> {
        let observations = durable_store.load_boundary_observation_manifest()?;
        for observation in &observations {
            self.register_boundary_observation(observation.clone())?;
        }
        Ok(observations)
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

    pub fn update_record_state(
        &mut self,
        catalog_id: &str,
        record_id: &str,
        state: MemoryRecordState,
        now_us: u64,
        actor: &str,
        reason: &str,
    ) -> MemoryResult<MemoryRecord> {
        required_str(catalog_id, "catalog_id")?;
        required_str(record_id, "record_id")?;
        required_str(actor, "record_lifecycle.actor")?;
        required_str(reason, "record_lifecycle.reason")?;
        let catalog = self
            .catalogs
            .get(catalog_id)
            .ok_or_else(|| LingquMemoryError::MissingCatalog(catalog_id.to_string()))?;
        if !catalog.record_ids.iter().any(|id| id == record_id) {
            return Err(LingquMemoryError::MissingRecord(record_id.to_string()));
        }
        let current = self
            .records
            .get(record_id)
            .ok_or_else(|| LingquMemoryError::MissingRecord(record_id.to_string()))?;
        if now_us < current.updated_at_us {
            return Err(LingquMemoryError::InvalidValue {
                field: "now_us",
                reason: "record mutation time must not move backwards",
            });
        }
        if current.state == state {
            return Ok(current.clone());
        }
        let previous_state = current.state;
        let previous_record_version = current.version;
        let previous_catalog_version = catalog.version;

        let mut updated = current.clone();
        updated.state = state;
        updated.version = updated.version.saturating_add(1);
        updated.updated_at_us = now_us;
        updated.validate()?;

        let mut updated_catalog = catalog.clone();
        if now_us < updated_catalog.updated_at_us {
            return Err(LingquMemoryError::InvalidValue {
                field: "now_us",
                reason: "catalog mutation time must not move backwards",
            });
        }
        updated_catalog.version = updated_catalog.version.saturating_add(1);
        updated_catalog.updated_at_us = now_us;
        updated_catalog.validate()?;

        let event_id = format!(
            "record-lifecycle/{}/{}/{}",
            catalog_id, record_id, updated.version
        );
        let mut event = MemoryRecordLifecycleEvent {
            event_id,
            catalog_id: catalog_id.to_string(),
            record_id: record_id.to_string(),
            previous_state,
            new_state: state,
            previous_record_version,
            new_record_version: updated.version,
            previous_catalog_version,
            new_catalog_version: updated_catalog.version,
            actor: actor.to_string(),
            reason: reason.to_string(),
            checksum: 0,
            created_at_us: now_us,
            version: 1,
        };
        event.checksum = record_lifecycle_event_checksum(&event);
        event.validate()?;

        self.records.insert(record_id.to_string(), updated.clone());
        self.catalogs
            .insert(catalog_id.to_string(), updated_catalog);
        self.record_lifecycle_events
            .insert(event.event_id.clone(), event);
        Ok(updated)
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

    pub fn register_paper_engram_tokenizer_projection(
        &mut self,
        manifest: PaperEngramTokenizerProjectionManifest,
    ) -> MemoryResult<()> {
        manifest.validate()?;
        self.paper_engram_tokenizer_projections
            .insert(manifest.projection_id.clone(), manifest);
        Ok(())
    }

    pub fn register_paper_engram_hash_config(
        &mut self,
        manifest: PaperEngramHashConfigManifest,
    ) -> MemoryResult<()> {
        manifest.validate()?;
        let projection = self
            .paper_engram_tokenizer_projections
            .get(&manifest.tokenizer_projection_id)
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_hash_config.tokenizer_projection_id",
            ))?;
        if projection.projection_checksum != manifest.tokenizer_projection_checksum {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_hash_config.tokenizer_projection_checksum",
                reason: "hash config projection checksum must match tokenizer projection manifest",
            });
        }
        self.paper_engram_hash_configs
            .insert(manifest.hash_config_id.clone(), manifest);
        Ok(())
    }

    pub fn register_paper_engram_training_recipe(
        &mut self,
        manifest: PaperEngramTrainingRecipeManifest,
    ) -> MemoryResult<()> {
        manifest.validate()?;
        if !self
            .paper_engram_tokenizer_projections
            .values()
            .any(|projection| projection.projection_ref == manifest.tokenizer_projection_ref)
        {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_training_recipe.tokenizer_projection_ref",
            ));
        }
        if !self
            .paper_engram_hash_configs
            .values()
            .any(|hash_config| hash_config.hash_config_ref == manifest.hash_config_ref)
        {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_training_recipe.hash_config_ref",
            ));
        }
        self.paper_engram_training_recipes
            .insert(manifest.recipe_id.clone(), manifest);
        Ok(())
    }

    pub fn register_paper_engram_eval_report(
        &mut self,
        manifest: PaperEngramEvalReportManifest,
    ) -> MemoryResult<()> {
        manifest.validate()?;
        if !self
            .paper_engram_training_recipes
            .contains_key(&manifest.recipe_id)
        {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_eval_report.recipe_id",
            ));
        }
        self.paper_engram_eval_reports
            .insert(manifest.report_id.clone(), manifest);
        Ok(())
    }

    pub fn register_paper_engram_table_shard(
        &mut self,
        manifest: PaperEngramTableShardManifest,
    ) -> MemoryResult<()> {
        manifest.validate()?;
        self.paper_engram_table_shards
            .insert(manifest.shard_id.clone(), manifest);
        Ok(())
    }

    pub fn register_paper_engram_gate(
        &mut self,
        manifest: PaperEngramGateManifest,
    ) -> MemoryResult<()> {
        manifest.validate()?;
        self.paper_engram_gates
            .insert(manifest.gate_id.clone(), manifest);
        Ok(())
    }

    pub fn register_paper_engram_module(
        &mut self,
        manifest: PaperEngramModuleManifest,
    ) -> MemoryResult<()> {
        manifest.validate()?;
        if self.paper_engram_modules.values().any(|module| {
            module.module_id != manifest.module_id
                && module.model.model_id == manifest.model.model_id
                && module.module_name == manifest.module_name
        }) {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.module_name",
                reason: "module_name must be unique per model_id",
            });
        }
        for shard_id in &manifest.table_shard_ids {
            if !self.paper_engram_table_shards.contains_key(shard_id) {
                return Err(LingquMemoryError::MissingField(
                    "paper_engram_module.table_shard_ids",
                ));
            }
        }
        for gate_id in &manifest.gate_ids {
            if !self.paper_engram_gates.contains_key(gate_id) {
                return Err(LingquMemoryError::MissingField(
                    "paper_engram_module.gate_ids",
                ));
            }
        }
        let (_, hash_config) = self.validate_paper_engram_module_artifact_bindings(&manifest)?;
        let table_shards =
            resolve_paper_engram_module_table_shards(&manifest, &self.paper_engram_table_shards)?;
        validate_paper_engram_table_row_coverage(&manifest, hash_config, &table_shards)?;
        let gates = resolve_paper_engram_module_gates(&manifest, &self.paper_engram_gates)?;
        let _layer_operands =
            build_paper_engram_runtime_layer_operands(&manifest, &table_shards, &gates)?;
        self.validate_paper_engram_quality_claim(&manifest)?;
        self.paper_engram_modules
            .insert(manifest.module_id.clone(), manifest);
        Ok(())
    }

    fn validate_paper_engram_module_artifact_bindings(
        &self,
        module: &PaperEngramModuleManifest,
    ) -> MemoryResult<(
        &PaperEngramTokenizerProjectionManifest,
        &PaperEngramHashConfigManifest,
    )> {
        let tokenizer_projection = self
            .paper_engram_tokenizer_projections
            .values()
            .find(|projection| projection.projection_ref == module.tokenizer_projection_ref)
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_module.tokenizer_projection_ref",
            ))?;
        let hash_config = self
            .paper_engram_hash_configs
            .values()
            .find(|hash_config| hash_config.hash_config_ref == module.hash_config_ref)
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_module.hash_config_ref",
            ))?;
        if hash_config.tokenizer_projection_id != tokenizer_projection.projection_id {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.hash_config_ref",
                reason: "hash config tokenizer projection must match module projection ref",
            });
        }
        if tokenizer_projection.model_id != module.model.model_id
            || hash_config.model_id != module.model.model_id
        {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.model",
                reason: "projection and hash config model_id must match module model_id",
            });
        }
        if tokenizer_projection.tokenizer_id != module.tokenizer_id {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.tokenizer_id",
                reason: "tokenizer projection tokenizer_id must match module tokenizer_id",
            });
        }
        if hash_config.orders != module.orders {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.orders",
                reason: "hash config orders must match module orders",
            });
        }
        if hash_config.heads_per_order != module.heads_per_order {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.heads_per_order",
                reason: "hash config heads_per_order must match module heads_per_order",
            });
        }
        Ok((tokenizer_projection, hash_config))
    }

    pub fn validate_paper_engram_module_quality(&self, module_id: &str) -> MemoryResult<()> {
        let module = self
            .paper_engram_modules
            .get(module_id)
            .ok_or_else(|| LingquMemoryError::MissingField("paper_engram_module.module_id"))?;
        self.validate_paper_engram_quality_claim(module)
    }

    fn validate_paper_engram_quality_claim(
        &self,
        module: &PaperEngramModuleManifest,
    ) -> MemoryResult<()> {
        let trained_claim = matches!(
            module.quality_claim,
            PaperEngramQualityClaim::Posttrain
                | PaperEngramQualityClaim::Finetune
                | PaperEngramQualityClaim::Imported
        );
        if !trained_claim {
            return Ok(());
        }
        let recipe_ref =
            module
                .training_recipe_ref
                .as_ref()
                .ok_or(LingquMemoryError::MissingField(
                    "paper_engram_module.training_recipe_ref",
                ))?;
        let eval_ref = module
            .eval_report_ref
            .as_ref()
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_module.eval_report_ref",
            ))?;
        let recipe = self
            .paper_engram_training_recipes
            .values()
            .find(|recipe| paper_engram_training_recipe_dfs_path(&recipe.recipe_id) == *recipe_ref)
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_module.training_recipe_ref",
            ))?;
        let report = self
            .paper_engram_eval_reports
            .values()
            .find(|report| paper_engram_eval_report_dfs_path(&report.report_id) == *eval_ref)
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_module.eval_report_ref",
            ))?;

        if recipe.model != module.model || report.model != module.model {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.model",
                reason: "training recipe and eval report model binding must match module model",
            });
        }
        if recipe.base_checkpoint_checksum != module.base_checkpoint_checksum {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.base_checkpoint_checksum",
                reason: "training recipe base checkpoint must match module base checkpoint",
            });
        }
        if recipe.tokenizer_projection_ref != module.tokenizer_projection_ref {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.tokenizer_projection_ref",
                reason: "training recipe tokenizer projection must match module",
            });
        }
        if recipe.hash_config_ref != module.hash_config_ref {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.hash_config_ref",
                reason: "training recipe hash config must match module",
            });
        }
        let hash_config = self
            .paper_engram_hash_configs
            .values()
            .find(|hash_config| hash_config.hash_config_ref == module.hash_config_ref)
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_module.hash_config_ref",
            ))?;
        validate_paper_engram_quality_recipe_shape(module, recipe, hash_config)?;
        if report.recipe_id != recipe.recipe_id || report.module_id != module.module_id {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.eval_report_ref",
                reason: "eval report must bind the referenced recipe and module",
            });
        }
        if !paper_engram_quality_claim_accepts_training_mode(module.quality_claim, recipe.mode) {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.quality_claim",
                reason: "quality claim must match training recipe mode",
            });
        }
        self.validate_paper_engram_quality_artifact_sources(module)?;
        self.validate_paper_engram_quality_provenance(recipe, report)?;
        self.validate_paper_engram_eval_acceptance_evidence(report)?;
        Ok(())
    }

    fn validate_paper_engram_quality_artifact_sources(
        &self,
        module: &PaperEngramModuleManifest,
    ) -> MemoryResult<()> {
        for shard_id in &module.table_shard_ids {
            let shard = self.paper_engram_table_shards.get(shard_id).ok_or(
                LingquMemoryError::MissingField("paper_engram_module.table_shard_ids"),
            )?;
            validate_trained_paper_engram_source_ref(
                "paper_engram_table_shard.source_ref",
                shard.source_ref.as_deref(),
                true,
            )?;
        }
        for gate_id in &module.gate_ids {
            let gate =
                self.paper_engram_gates
                    .get(gate_id)
                    .ok_or(LingquMemoryError::MissingField(
                        "paper_engram_module.gate_ids",
                    ))?;
            validate_trained_paper_engram_source_ref(
                "paper_engram_gate.source_ref",
                gate.source_ref.as_deref(),
                false,
            )?;
        }
        Ok(())
    }

    fn validate_paper_engram_quality_provenance(
        &self,
        recipe: &PaperEngramTrainingRecipeManifest,
        report: &PaperEngramEvalReportManifest,
    ) -> MemoryResult<()> {
        validate_trained_paper_engram_train_eval_split(recipe, report)?;
        for dataset_ref in &recipe.dataset_refs {
            validate_trained_paper_engram_provenance_ref(
                "paper_engram_training_recipe.dataset_refs",
                dataset_ref,
            )?;
        }
        for evidence_ref in &recipe.evidence_refs {
            validate_trained_paper_engram_provenance_ref(
                "paper_engram_training_recipe.evidence_refs",
                evidence_ref,
            )?;
        }
        for validation_set_ref in &report.validation_set_refs {
            validate_trained_paper_engram_provenance_ref(
                "paper_engram_eval_report.validation_set_refs",
                validation_set_ref,
            )?;
        }
        for evidence_ref in &report.evidence_refs {
            validate_trained_paper_engram_provenance_ref(
                "paper_engram_eval_report.evidence_refs",
                evidence_ref,
            )?;
        }
        Ok(())
    }

    fn validate_paper_engram_eval_acceptance_evidence(
        &self,
        report: &PaperEngramEvalReportManifest,
    ) -> MemoryResult<()> {
        report
            .decode_policy_loss_milli
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_eval_report.decode_policy_loss_milli",
            ))?;
        report
            .paper_engram_decode_policy_loss_milli
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_eval_report.paper_engram_decode_policy_loss_milli",
            ))?;
        let zero_hidden =
            report
                .zero_table_hidden_checksum
                .ok_or(LingquMemoryError::MissingField(
                    "paper_engram_eval_report.zero_table_hidden_checksum",
                ))?;
        let paper_hidden =
            report
                .paper_engram_hidden_checksum
                .ok_or(LingquMemoryError::MissingField(
                    "paper_engram_eval_report.paper_engram_hidden_checksum",
                ))?;
        if zero_hidden == paper_hidden {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.paper_engram_hidden_checksum",
                reason: "paper Engram table must change hidden checksum versus zero table",
            });
        }
        let zero_output =
            report
                .zero_table_output_checksum
                .ok_or(LingquMemoryError::MissingField(
                    "paper_engram_eval_report.zero_table_output_checksum",
                ))?;
        if zero_output == report.output_checksum {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.output_checksum",
                reason: "paper Engram table must change output checksum versus zero table",
            });
        }
        if report.cpu_backend_output_match != Some(true) {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.cpu_backend_output_match",
                reason: "trained paper Engram quality requires CPU/backend output match evidence",
            });
        }
        let requests = report
            .row_prefetch_requests
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_eval_report.row_prefetch_requests",
            ))?;
        let hits = report
            .row_prefetch_hits
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_eval_report.row_prefetch_hits",
            ))?;
        if hits == 0 || hits != requests {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.row_prefetch_hits",
                reason:
                    "trained paper Engram quality requires complete row prefetch locality evidence",
            });
        }
        let expected_context_steps =
            report
                .runtime_context_steps_expected
                .ok_or(LingquMemoryError::MissingField(
                    "paper_engram_eval_report.runtime_context_steps_expected",
                ))?;
        let observed_context_steps =
            report
                .runtime_context_steps_observed
                .ok_or(LingquMemoryError::MissingField(
                    "paper_engram_eval_report.runtime_context_steps_observed",
                ))?;
        if observed_context_steps != expected_context_steps {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.runtime_context_steps_observed",
                reason: "trained paper Engram quality requires runtime context evidence for every decode step",
            });
        }
        report
            .max_backend_latency_us
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_eval_report.max_backend_latency_us",
            ))?;
        report
            .max_allowed_backend_latency_us
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_eval_report.max_allowed_backend_latency_us",
            ))?;
        validate_paper_engram_phase6_summary_provenance(report)?;
        Ok(())
    }

    pub fn record_artifact_access(&mut self, event: ArtifactAccessRecord) -> MemoryResult<()> {
        event.validate()?;
        let Some(artifact) = self.execution_artifacts.get(&event.artifact_id) else {
            return Err(LingquMemoryError::MissingExecutionArtifact(
                event.artifact_id.clone(),
            ));
        };
        if artifact.kind != event.artifact_kind {
            return Err(LingquMemoryError::InvalidValue {
                field: "artifact_access.artifact_kind",
                reason: "access event kind must match execution artifact kind",
            });
        }
        if artifact.model != event.model {
            return Err(LingquMemoryError::InvalidValue {
                field: "artifact_access.model",
                reason: "access event model must match execution artifact model",
            });
        }
        if artifact.producer_boundary != event.boundary {
            return Err(LingquMemoryError::InvalidValue {
                field: "artifact_access.boundary",
                reason: "access event boundary must match execution artifact producer boundary",
            });
        }
        if artifact.checksum != event.artifact_checksum {
            return Err(LingquMemoryError::InvalidValue {
                field: "artifact_access.artifact_checksum",
                reason: "access event checksum must match execution artifact checksum",
            });
        }
        self.artifact_access_events
            .insert(event.event_id.clone(), event);
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
            .filter(|artifact| artifact.producer_boundary == req.boundary)
            .filter(|artifact| {
                artifact
                    .boundary_hidden_fingerprint
                    .matches_hot_ref(&req.hidden_state)
            })
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

        let support = if let Some(artifact) = candidate.as_ref() {
            let supported_action = if artifact.kind == ExecutionArtifactKind::Logits {
                ShortpathAction::JumpToTerminal
            } else {
                ShortpathAction::JumpToLayer
            };
            let support_id = format!("shortpath-support/{}", req.request_id);
            let proof_checksum = shortpath_support_checksum(
                &support_id,
                &req.request_id,
                supported_action,
                Some(&artifact.artifact_id),
                Some(artifact.producer_boundary.position),
                artifact.target_layer_start,
                artifact.target_layer_end,
                artifact.confidence_milli,
                artifact.checksum,
                now_us,
            );
            ShortpathSupportRecord {
                support_id,
                request_id: req.request_id.clone(),
                supported_action,
                artifact_id: Some(artifact.artifact_id.clone()),
                producer_position: Some(artifact.producer_boundary.position),
                target_layer_start: Some(artifact.target_layer_start),
                target_layer_end: Some(artifact.target_layer_end),
                confidence_milli: artifact.confidence_milli,
                verify_required: artifact.state != ExecutionArtifactState::Verified,
                proof_checksum,
                reason: "verified_execution_artifact_support".to_string(),
                created_at_us: now_us,
                version: 1,
            }
        } else {
            let support_id = format!("shortpath-support/{}", req.request_id);
            let proof_checksum = shortpath_support_checksum(
                &support_id,
                &req.request_id,
                ShortpathAction::Continue,
                None,
                Some(req.boundary.position),
                req.boundary.layer_start,
                req.boundary.layer_end,
                0,
                req.hidden_state.checksum,
                now_us,
            );
            ShortpathSupportRecord {
                support_id,
                request_id: req.request_id.clone(),
                supported_action: ShortpathAction::Continue,
                artifact_id: None,
                producer_position: Some(req.boundary.position),
                target_layer_start: None,
                target_layer_end: None,
                confidence_milli: 0,
                verify_required: false,
                proof_checksum,
                reason: "no_verified_execution_artifact_support".to_string(),
                created_at_us: now_us,
                version: 1,
            }
        };
        support.validate()?;
        self.shortpath_supports
            .insert(support.support_id.clone(), support.clone());
        let response = BoundaryLookupResponse {
            request_id: req.request_id,
            support,
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

    pub fn record_lifecycle_event(&self, event_id: &str) -> Option<&MemoryRecordLifecycleEvent> {
        self.record_lifecycle_events.get(event_id)
    }

    pub fn execution_artifact(&self, artifact_id: &str) -> Option<&ExecutionArtifactObject> {
        self.execution_artifacts.get(artifact_id)
    }

    pub fn artifact_access_event(&self, event_id: &str) -> Option<&ArtifactAccessRecord> {
        self.artifact_access_events.get(event_id)
    }

    pub fn artifact_access_events(&self) -> Vec<&ArtifactAccessRecord> {
        let mut events = self.artifact_access_events.values().collect::<Vec<_>>();
        events.sort_by(|left, right| left.event_id.cmp(&right.event_id));
        events
    }

    pub fn prefix_cache_artifact(&self, artifact_id: &str) -> Option<&PrefixCacheArtifact> {
        self.prefix_cache_artifacts.get(artifact_id)
    }

    pub fn prefix_cache_reuse_plan(&self, plan_id: &str) -> Option<&PrefixCacheReusePlan> {
        self.prefix_cache_reuse_plans.get(plan_id)
    }

    pub fn paper_engram_table_shard(
        &self,
        shard_id: &str,
    ) -> Option<&PaperEngramTableShardManifest> {
        self.paper_engram_table_shards.get(shard_id)
    }

    pub fn paper_engram_tokenizer_projection(
        &self,
        projection_id: &str,
    ) -> Option<&PaperEngramTokenizerProjectionManifest> {
        self.paper_engram_tokenizer_projections.get(projection_id)
    }

    pub fn paper_engram_hash_config(
        &self,
        hash_config_id: &str,
    ) -> Option<&PaperEngramHashConfigManifest> {
        self.paper_engram_hash_configs.get(hash_config_id)
    }

    pub fn paper_engram_gate(&self, gate_id: &str) -> Option<&PaperEngramGateManifest> {
        self.paper_engram_gates.get(gate_id)
    }

    pub fn paper_engram_module(&self, module_id: &str) -> Option<&PaperEngramModuleManifest> {
        self.paper_engram_modules.get(module_id)
    }

    pub fn paper_engram_module_by_model(
        &self,
        model_id: &str,
        engram_id: &str,
    ) -> MemoryResult<&PaperEngramModuleManifest> {
        required_str(model_id, "paper_engram_module.model_id")?;
        required_str(engram_id, "paper_engram_module.engram_id")?;
        let mut matches = self
            .paper_engram_modules
            .values()
            .filter(|module| module.model.model_id == model_id && module.module_name == engram_id);
        let Some(module) = matches.next() else {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_module.engram_id",
            ));
        };
        if matches.next().is_some() {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.engram_id",
                reason: "model_id and engram_id must resolve exactly one module",
            });
        }
        Ok(module)
    }

    pub fn resolve_paper_engram_runtime_artifacts(
        &self,
        module_id: &str,
    ) -> MemoryResult<PaperEngramRuntimeArtifacts> {
        let module =
            self.paper_engram_modules
                .get(module_id)
                .ok_or(LingquMemoryError::MissingField(
                    "paper_engram_module.module_id",
                ))?;
        let tokenizer_projection = self
            .paper_engram_tokenizer_projections
            .values()
            .find(|projection| projection.projection_ref == module.tokenizer_projection_ref)
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_module.tokenizer_projection_ref",
            ))?;
        let hash_config = self
            .paper_engram_hash_configs
            .values()
            .find(|hash_config| hash_config.hash_config_ref == module.hash_config_ref)
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_module.hash_config_ref",
            ))?;
        if hash_config.tokenizer_projection_id != tokenizer_projection.projection_id {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.hash_config_ref",
                reason: "hash config tokenizer projection must match module projection ref",
            });
        }
        if tokenizer_projection.model_id != module.model.model_id
            || hash_config.model_id != module.model.model_id
        {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.model",
                reason: "projection and hash config model_id must match module model_id",
            });
        }
        if tokenizer_projection.tokenizer_id != module.tokenizer_id {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.tokenizer_id",
                reason: "tokenizer projection tokenizer_id must match module tokenizer_id",
            });
        }
        if hash_config.orders != module.orders {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.orders",
                reason: "hash config orders must match module orders",
            });
        }
        if hash_config.heads_per_order != module.heads_per_order {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_module.heads_per_order",
                reason: "hash config heads_per_order must match module heads_per_order",
            });
        }
        let table_shards =
            resolve_paper_engram_module_table_shards(module, &self.paper_engram_table_shards)?;
        validate_paper_engram_table_row_coverage(module, hash_config, &table_shards)?;
        let gates = resolve_paper_engram_module_gates(module, &self.paper_engram_gates)?;
        let layer_operands =
            build_paper_engram_runtime_layer_operands(module, &table_shards, &gates)?;
        Ok(PaperEngramRuntimeArtifacts {
            module: module.clone(),
            tokenizer_projection: tokenizer_projection.clone(),
            hash_config: hash_config.clone(),
            table_shards,
            gates,
            layer_operands,
        })
    }

    pub fn resolve_paper_engram_table_row_blocks(
        &self,
        req: PaperEngramTableRowBlockRequest,
    ) -> MemoryResult<PaperEngramTableRowBlockResponse> {
        req.validate()?;
        let runtime = self.resolve_paper_engram_runtime_artifacts(&req.module_id)?;
        if !runtime.module.layers.contains(&req.layer) {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block.layer",
                reason: "requested layer must be declared by module",
            });
        }
        if !runtime.module.orders.contains(&req.order) {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block.order",
                reason: "requested order must be declared by module",
            });
        }
        if req.head >= runtime.module.heads_per_order {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block.head",
                reason: "requested head must be less than module heads_per_order",
            });
        }
        let table_rows =
            paper_engram_hash_config_table_rows(&runtime.hash_config, req.order, req.head)?;
        if req.row_end > table_rows {
            return Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block.row_range",
                reason: "requested row range must fit hash config table spec rows",
            });
        }
        let shard = runtime
            .table_shards
            .iter()
            .find(|shard| {
                shard.layer == req.layer
                    && shard.order == req.order
                    && shard.head == req.head
                    && shard.row_start <= req.row_start
                    && shard.row_end >= req.row_end
            })
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_table_row_block.shard",
            ))?;
        let (row_payload_offset_bytes, row_payload_bytes) =
            paper_engram_table_row_payload_window(shard, req.row_start, req.row_end)?;
        let block_payload_refs = paper_engram_table_payload_refs_covering_window(
            shard,
            row_payload_offset_bytes,
            row_payload_bytes,
        )?;
        let response = PaperEngramTableRowBlockResponse {
            request_id: req.request_id,
            module_id: runtime.module.module_id,
            layer: req.layer,
            order: req.order,
            head: req.head,
            row_start: req.row_start,
            row_end: req.row_end,
            shard_id: shard.shard_id.clone(),
            shard_row_start: shard.row_start,
            shard_row_end: shard.row_end,
            dtype: shard.dtype,
            shape: shard.shape.clone(),
            row_payload_offset_bytes,
            row_payload_bytes,
            block_payload_refs,
        };
        response.validate()?;
        Ok(response)
    }

    pub fn plan_paper_engram_table_row_prefetch(
        &self,
        req: PaperEngramTableRowPrefetchRequest,
    ) -> MemoryResult<PaperEngramTableRowPrefetchPlan> {
        req.validate()?;
        let runtime = self.resolve_paper_engram_runtime_artifacts(&req.module_id)?;
        let hash_config = paper_engram_lookup_hash_config(&runtime)?;
        let from_step =
            usize::try_from(req.from_step).map_err(|_| LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_prefetch.from_step",
                reason: "from_step exceeds host usize",
            })?;
        let lookup_requests =
            build_engram_lookup_requests_from_step(&req.canonical_history, from_step, &hash_config)
                .map_err(|_| LingquMemoryError::InvalidValue {
                    field: "paper_engram_table_row_prefetch.hash_config",
                    reason: "hash config could not produce lookup requests",
                })?;
        let mut rows = Vec::new();
        for lookup in lookup_requests {
            for &layer in &runtime.module.layers {
                let row_block =
                    self.resolve_paper_engram_table_row_blocks(PaperEngramTableRowBlockRequest {
                        request_id: format!(
                            "{}/layer-{}/order-{}/head-{}/row-{}",
                            req.request_id, layer, lookup.order, lookup.head, lookup.row
                        ),
                        module_id: req.module_id.clone(),
                        layer,
                        order: lookup.order,
                        head: u32::from(lookup.head),
                        row_start: lookup.row,
                        row_end: lookup.row.saturating_add(1),
                        created_at_us: req.created_at_us,
                    })?;
                rows.push(PaperEngramTableRowPrefetchRef {
                    step_index: lookup.step_index,
                    layer,
                    order: lookup.order,
                    head: u32::from(lookup.head),
                    row: lookup.row,
                    exact_key: lookup.exact_key,
                    shard_id: row_block.shard_id,
                    row_payload_offset_bytes: row_block.row_payload_offset_bytes,
                    row_payload_bytes: row_block.row_payload_bytes,
                    block_payload_refs: row_block.block_payload_refs,
                });
            }
        }
        rows.sort_by(|left, right| {
            left.step_index
                .cmp(&right.step_index)
                .then_with(|| left.layer.cmp(&right.layer))
                .then_with(|| left.order.cmp(&right.order))
                .then_with(|| left.head.cmp(&right.head))
                .then_with(|| left.row.cmp(&right.row))
                .then_with(|| left.exact_key.cmp(&right.exact_key))
        });
        let plan = PaperEngramTableRowPrefetchPlan {
            plan_id: format!("paper-engram-row-prefetch/{}", req.request_id),
            request_id: req.request_id,
            module_id: runtime.module.module_id,
            tokenizer_projection_checksum: runtime.tokenizer_projection.projection_checksum,
            hash_config_checksum: runtime.hash_config.hash_config_checksum,
            canonical_history_len: req.canonical_history.len() as u64,
            from_step: req.from_step,
            rows,
            created_at_us: req.created_at_us,
        };
        plan.validate()?;
        Ok(plan)
    }

    pub fn shortpath_support(&self, support_id: &str) -> Option<&ShortpathSupportRecord> {
        self.shortpath_supports.get(support_id)
    }

    pub fn prefetch_plan(&self, plan_id: &str) -> Option<&PrefetchPlanRecord> {
        self.prefetch_plans.get(plan_id)
    }
}

fn resolve_paper_engram_module_table_shards(
    module: &PaperEngramModuleManifest,
    table_shards: &HashMap<String, PaperEngramTableShardManifest>,
) -> MemoryResult<Vec<PaperEngramTableShardManifest>> {
    let mut resolved = Vec::new();
    for shard_id in &module.table_shard_ids {
        let shard = table_shards
            .get(shard_id)
            .ok_or(LingquMemoryError::MissingField(
                "paper_engram_module.table_shard_ids",
            ))?;
        validate_paper_engram_table_shard_for_module(module, shard)?;
        resolved.push(shard.clone());
    }
    resolved.sort_by(|left, right| {
        left.layer
            .cmp(&right.layer)
            .then_with(|| left.order.cmp(&right.order))
            .then_with(|| left.head.cmp(&right.head))
            .then_with(|| left.row_start.cmp(&right.row_start))
            .then_with(|| left.shard_id.cmp(&right.shard_id))
    });
    validate_paper_engram_table_coverage(module, &resolved)?;
    Ok(resolved)
}

fn paper_engram_lookup_hash_config(
    runtime: &PaperEngramRuntimeArtifacts,
) -> MemoryResult<Qwen3DenseReferenceEngramHashConfig> {
    let heads_per_order = usize::try_from(runtime.hash_config.heads_per_order).map_err(|_| {
        LingquMemoryError::InvalidValue {
            field: "paper_engram_hash_config.heads_per_order",
            reason: "heads_per_order exceeds host usize",
        }
    })?;
    let table_specs = paper_engram_hash_config_table_specs(&runtime.hash_config)?;
    Ok(Qwen3DenseReferenceEngramHashConfig {
        version: runtime.hash_config.version,
        projection_checksum: runtime.tokenizer_projection.projection_checksum,
        orders: runtime.hash_config.orders.clone(),
        heads_per_order,
        table_rows: runtime.hash_config.table_rows,
        seed: runtime.hash_config.seed,
        algorithm: runtime.hash_config.algorithm.clone(),
        table_specs,
    })
}

fn paper_engram_hash_config_table_specs(
    hash_config: &PaperEngramHashConfigManifest,
) -> MemoryResult<Vec<Qwen3DenseReferenceEngramHashTableSpec>> {
    if !hash_config.table_specs.is_empty() {
        return Ok(hash_config.table_specs.clone());
    }
    paper_engram_default_hash_table_specs(
        &hash_config.orders,
        hash_config.heads_per_order,
        hash_config.table_rows,
        hash_config.seed,
    )
}

fn paper_engram_hash_config_table_rows(
    hash_config: &PaperEngramHashConfigManifest,
    order: u8,
    head: u32,
) -> MemoryResult<u64> {
    let specs = paper_engram_hash_config_table_specs(hash_config)?;
    specs
        .into_iter()
        .find(|spec| spec.order == order && u32::from(spec.head) == head)
        .map(|spec| spec.table_rows)
        .ok_or(LingquMemoryError::MissingField(
            "paper_engram_hash_config.table_specs",
        ))
}

fn resolve_paper_engram_module_gates(
    module: &PaperEngramModuleManifest,
    gates: &HashMap<String, PaperEngramGateManifest>,
) -> MemoryResult<Vec<PaperEngramGateManifest>> {
    let mut resolved = Vec::new();
    for gate_id in &module.gate_ids {
        let gate = gates.get(gate_id).ok_or(LingquMemoryError::MissingField(
            "paper_engram_module.gate_ids",
        ))?;
        validate_paper_engram_gate_for_module(module, gate)?;
        resolved.push(gate.clone());
    }
    resolved.sort_by(|left, right| {
        left.layer
            .cmp(&right.layer)
            .then_with(|| left.gate_id.cmp(&right.gate_id))
    });
    validate_paper_engram_gate_coverage(module, &resolved)?;
    Ok(resolved)
}

fn validate_paper_engram_table_shard_for_module(
    module: &PaperEngramModuleManifest,
    shard: &PaperEngramTableShardManifest,
) -> MemoryResult<()> {
    if shard.model_id != module.model.model_id {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.model_id",
            reason: "table shard model_id must match module model_id",
        });
    }
    if !module.layers.contains(&shard.layer) {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.layer",
            reason: "table shard layer must be declared by module",
        });
    }
    if !module.orders.contains(&shard.order) {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.order",
            reason: "table shard order must be declared by module",
        });
    }
    if shard.head >= module.heads_per_order {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.head",
            reason: "table shard head must be less than module heads_per_order",
        });
    }
    if shard.dtype != module.table_dtype {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.dtype",
            reason: "table shard dtype must match module table_dtype",
        });
    }
    if shard.shape.len() < 2 {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.shape",
            reason: "table shard shape must include rows and memory dimension",
        });
    }
    if shard.shape[0] != shard.row_end - shard.row_start {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.shape",
            reason: "table shard first shape dimension must match row range",
        });
    }
    if shard.shape[1] != module.memory_dim {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.shape",
            reason: "table shard memory dimension must match module memory_dim",
        });
    }
    if shard.block_payload_refs.is_empty() {
        return Err(LingquMemoryError::MissingField(
            "paper_engram_table_shard.block_payload_refs",
        ));
    }
    Ok(())
}

fn validate_paper_engram_gate_for_module(
    module: &PaperEngramModuleManifest,
    gate: &PaperEngramGateManifest,
) -> MemoryResult<()> {
    if gate.model_id != module.model.model_id {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_gate.model_id",
            reason: "gate model_id must match module model_id",
        });
    }
    if !module.layers.contains(&gate.layer) {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_gate.layer",
            reason: "gate layer must be declared by module",
        });
    }
    if gate.dtype != TensorDType::F32 {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_gate.dtype",
            reason: "paper Engram gate dtype must be f32",
        });
    }
    if gate.shape != vec![module.hidden_size] {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_gate.shape",
            reason: "gate shape must match module hidden_size",
        });
    }
    let Some(_) = &gate.payload_ref else {
        return Err(LingquMemoryError::MissingField(
            "paper_engram_gate.payload_ref",
        ));
    };
    Ok(())
}

fn validate_paper_engram_table_coverage(
    module: &PaperEngramModuleManifest,
    shards: &[PaperEngramTableShardManifest],
) -> MemoryResult<()> {
    let available = shards
        .iter()
        .map(|shard| (shard.layer, shard.order, shard.head))
        .collect::<BTreeSet<_>>();
    for &layer in &module.layers {
        for &order in &module.orders {
            for head in 0..module.heads_per_order {
                if !available.contains(&(layer, order, head)) {
                    return Err(LingquMemoryError::MissingField(
                        "paper_engram_runtime.table_operand",
                    ));
                }
            }
        }
    }
    Ok(())
}

fn validate_paper_engram_table_row_coverage(
    module: &PaperEngramModuleManifest,
    hash_config: &PaperEngramHashConfigManifest,
    shards: &[PaperEngramTableShardManifest],
) -> MemoryResult<()> {
    for &layer in &module.layers {
        for &order in &module.orders {
            for head in 0..module.heads_per_order {
                let table_rows = paper_engram_hash_config_table_rows(hash_config, order, head)?;
                let mut matching = shards
                    .iter()
                    .filter(|shard| {
                        shard.layer == layer && shard.order == order && shard.head == head
                    })
                    .collect::<Vec<_>>();
                matching.sort_by(|left, right| {
                    left.row_start
                        .cmp(&right.row_start)
                        .then_with(|| left.row_end.cmp(&right.row_end))
                        .then_with(|| left.shard_id.cmp(&right.shard_id))
                });

                let mut next_row = 0;
                for shard in matching {
                    if shard.row_end > table_rows {
                        return Err(LingquMemoryError::InvalidValue {
                            field: "paper_engram_table_shard.row_range",
                            reason: "table shard row range must fit hash config table spec rows",
                        });
                    }
                    if shard.row_start < next_row {
                        return Err(LingquMemoryError::InvalidValue {
                            field: "paper_engram_table_shard.row_range",
                            reason: "table shard row ranges must not overlap",
                        });
                    }
                    if shard.row_start > next_row {
                        return Err(LingquMemoryError::MissingField(
                            "paper_engram_runtime.table_row_block",
                        ));
                    }
                    next_row = shard.row_end;
                }
                if next_row < table_rows {
                    return Err(LingquMemoryError::MissingField(
                        "paper_engram_runtime.table_row_block",
                    ));
                }
            }
        }
    }
    Ok(())
}

fn validate_paper_engram_gate_coverage(
    module: &PaperEngramModuleManifest,
    gates: &[PaperEngramGateManifest],
) -> MemoryResult<()> {
    let available = gates.iter().map(|gate| gate.layer).collect::<BTreeSet<_>>();
    for &layer in &module.layers {
        if !available.contains(&layer) {
            return Err(LingquMemoryError::MissingField(
                "paper_engram_runtime.gate_operand",
            ));
        }
    }
    Ok(())
}

fn paper_engram_table_row_payload_window(
    shard: &PaperEngramTableShardManifest,
    row_start: u64,
    row_end: u64,
) -> MemoryResult<(u64, u64)> {
    let dtype_width = shard
        .dtype
        .byte_width()
        .ok_or(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.dtype",
            reason: "table row payload resolution requires fixed-width dtype",
        })?;
    if shard.shape.len() < 2 {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.shape",
            reason: "table row payload resolution requires rows and memory dimension",
        });
    }
    let shard_rows = shard.row_end - shard.row_start;
    if shard.shape[0] != shard_rows {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.shape",
            reason: "table shard first shape dimension must match row range",
        });
    }
    let row_stride_elems = shard.shape[1..].iter().try_fold(1u64, |acc, dim| {
        acc.checked_mul(*dim)
            .ok_or(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_shard.shape",
                reason: "table row stride exceeds u64",
            })
    })?;
    let row_stride_bytes =
        row_stride_elems
            .checked_mul(dtype_width)
            .ok_or(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_shard.shape",
                reason: "table row stride bytes exceeds u64",
            })?;
    let row_offset = row_start - shard.row_start;
    let row_count = row_end - row_start;
    let offset_bytes =
        row_offset
            .checked_mul(row_stride_bytes)
            .ok_or(LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block.row_range",
                reason: "table row payload offset exceeds u64",
            })?;
    let bytes = row_count
        .checked_mul(row_stride_bytes)
        .ok_or(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_row_block.row_range",
            reason: "table row payload bytes exceeds u64",
        })?;
    Ok((offset_bytes, bytes))
}

fn paper_engram_table_payload_refs_covering_window(
    shard: &PaperEngramTableShardManifest,
    offset_bytes: u64,
    bytes: u64,
) -> MemoryResult<Vec<LingquBlockPayloadRef>> {
    let window_end = offset_bytes
        .checked_add(bytes)
        .ok_or(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_row_block.row_range",
            reason: "table row payload window exceeds u64",
        })?;
    let mut logical_offset = 0u64;
    let mut refs = Vec::new();
    for payload_ref in &shard.block_payload_refs {
        let payload_end = logical_offset.checked_add(payload_ref.bytes).ok_or(
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_shard.block_payload_refs",
                reason: "table payload byte span exceeds u64",
            },
        )?;
        if payload_end > offset_bytes && logical_offset < window_end {
            refs.push(payload_ref.clone());
        }
        logical_offset = payload_end;
    }
    if refs.is_empty() || logical_offset < window_end {
        return Err(LingquMemoryError::InvalidValue {
            field: "paper_engram_table_shard.block_payload_refs",
            reason: "table payload refs must cover requested row byte window",
        });
    }
    Ok(refs)
}

fn build_paper_engram_runtime_layer_operands(
    module: &PaperEngramModuleManifest,
    table_shards: &[PaperEngramTableShardManifest],
    gates: &[PaperEngramGateManifest],
) -> MemoryResult<Vec<PaperEngramRuntimeLayerOperands>> {
    let mut layers = Vec::new();
    for &layer in &module.layers {
        let table_operands = table_shards
            .iter()
            .filter(|shard| shard.layer == layer)
            .map(|shard| PaperEngramRuntimeTableOperand {
                shard_id: shard.shard_id.clone(),
                layer: shard.layer,
                order: shard.order,
                head: shard.head,
                row_start: shard.row_start,
                row_end: shard.row_end,
                dtype: shard.dtype,
                shape: shard.shape.clone(),
                block_payload_refs: shard.block_payload_refs.clone(),
            })
            .collect::<Vec<_>>();
        let gate_operands = gates
            .iter()
            .filter(|gate| gate.layer == layer)
            .map(|gate| {
                let payload_ref =
                    gate.payload_ref
                        .clone()
                        .ok_or(LingquMemoryError::MissingField(
                            "paper_engram_gate.payload_ref",
                        ))?;
                Ok(PaperEngramRuntimeGateOperand {
                    gate_id: gate.gate_id.clone(),
                    layer: gate.layer,
                    dtype: gate.dtype,
                    shape: gate.shape.clone(),
                    payload_ref,
                })
            })
            .collect::<MemoryResult<Vec<_>>>()?;
        layers.push(PaperEngramRuntimeLayerOperands {
            layer,
            table_operands,
            gate_operands,
        });
    }
    Ok(layers)
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

fn record_lifecycle_event_checksum(event: &MemoryRecordLifecycleEvent) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &event.event_id);
    push_checksum_str(&mut bytes, &event.catalog_id);
    push_checksum_str(&mut bytes, &event.record_id);
    bytes.extend_from_slice(&record_state_tag(event.previous_state).to_le_bytes());
    bytes.extend_from_slice(&record_state_tag(event.new_state).to_le_bytes());
    bytes.extend_from_slice(&event.previous_record_version.to_le_bytes());
    bytes.extend_from_slice(&event.new_record_version.to_le_bytes());
    bytes.extend_from_slice(&event.previous_catalog_version.to_le_bytes());
    bytes.extend_from_slice(&event.new_catalog_version.to_le_bytes());
    push_checksum_str(&mut bytes, &event.actor);
    push_checksum_str(&mut bytes, &event.reason);
    bytes.extend_from_slice(&event.created_at_us.to_le_bytes());
    bytes.extend_from_slice(&event.version.to_le_bytes());
    checksum64(&bytes)
}

fn record_state_tag(state: MemoryRecordState) -> u64 {
    match state {
        MemoryRecordState::Pending => 1,
        MemoryRecordState::Committed => 2,
        MemoryRecordState::Tombstoned => 3,
        MemoryRecordState::Quarantined => 4,
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

fn artifact_access_kind_tag(kind: ArtifactAccessKind) -> u64 {
    match kind {
        ArtifactAccessKind::Produced => 1,
        ArtifactAccessKind::Consumed => 2,
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

fn shortpath_support_checksum(
    support_id: &str,
    request_id: &str,
    supported_action: ShortpathAction,
    artifact_id: Option<&str>,
    producer_position: Option<u64>,
    target_layer_start: u32,
    target_layer_end: u32,
    confidence_milli: u32,
    artifact_checksum: u64,
    created_at_us: u64,
) -> u64 {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(&(support_id.len() as u64).to_le_bytes());
    bytes.extend_from_slice(support_id.as_bytes());
    bytes.extend_from_slice(&(request_id.len() as u64).to_le_bytes());
    bytes.extend_from_slice(request_id.as_bytes());
    bytes.extend_from_slice(&shortpath_action_tag(supported_action).to_le_bytes());
    if let Some(artifact_id) = artifact_id {
        bytes.extend_from_slice(&(artifact_id.len() as u64).to_le_bytes());
        bytes.extend_from_slice(artifact_id.as_bytes());
    }
    if let Some(producer_position) = producer_position {
        bytes.extend_from_slice(&producer_position.to_le_bytes());
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
        bytes.extend_from_slice(&artifact.boundary_hidden_fingerprint.bytes.to_le_bytes());
        bytes.extend_from_slice(&artifact.boundary_hidden_fingerprint.checksum.to_le_bytes());
        bytes.extend_from_slice(
            &tensor_dtype_tag(artifact.boundary_hidden_fingerprint.dtype).to_le_bytes(),
        );
        for dim in &artifact.boundary_hidden_fingerprint.shape {
            bytes.extend_from_slice(&dim.to_le_bytes());
        }
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
        if let Some(support_id) = &decision.support_id {
            push_checksum_str(&mut bytes, support_id);
        }
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

fn shortpath_support_manifest_checksum(manifest: &LingquShortpathSupportManifest) -> u64 {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(manifest.kind.as_bytes());
    bytes.extend_from_slice(&manifest.schema_version.to_le_bytes());
    let mut supports = manifest.supports.iter().collect::<Vec<_>>();
    supports.sort_by(|left, right| left.support_id.cmp(&right.support_id));
    for support in supports {
        push_checksum_str(&mut bytes, &support.support_id);
        push_checksum_str(&mut bytes, &support.request_id);
        bytes.extend_from_slice(&shortpath_action_tag(support.supported_action).to_le_bytes());
        if let Some(artifact_id) = &support.artifact_id {
            push_checksum_str(&mut bytes, artifact_id);
        }
        bytes.extend_from_slice(&support.confidence_milli.to_le_bytes());
        bytes.extend_from_slice(&support.proof_checksum.to_le_bytes());
        bytes.extend_from_slice(&support.version.to_le_bytes());
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

fn boundary_observation_checksum(observation: &BoundaryObservationRecord) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &observation.observation_id);
    push_checksum_str(&mut bytes, &observation.run_id);
    push_checksum_str(&mut bytes, &observation.model.model_id);
    push_checksum_str(&mut bytes, &observation.model.model_key);
    bytes.extend_from_slice(&observation.model.tokenizer_hash.to_le_bytes());
    bytes.extend_from_slice(&observation.model.profile_hash.to_le_bytes());
    bytes.extend_from_slice(&range_boundary_phase_tag(observation.boundary.phase).to_le_bytes());
    bytes.extend_from_slice(&observation.boundary.step_index.to_le_bytes());
    bytes.extend_from_slice(&observation.boundary.node_index.to_le_bytes());
    bytes.extend_from_slice(&observation.boundary.layer_start.to_le_bytes());
    bytes.extend_from_slice(&observation.boundary.layer_end.to_le_bytes());
    bytes.extend_from_slice(
        &observation
            .boundary
            .next_node_index
            .unwrap_or(u32::MAX)
            .to_le_bytes(),
    );
    bytes.extend_from_slice(&observation.boundary.position.to_le_bytes());
    push_checksum_str(&mut bytes, &observation.hidden_state.object_key);
    bytes.extend_from_slice(&observation.hidden_state.version.to_le_bytes());
    bytes.extend_from_slice(&observation.hidden_state.bytes.to_le_bytes());
    bytes.extend_from_slice(&observation.hidden_state.checksum.to_le_bytes());
    bytes.extend_from_slice(&tensor_dtype_tag(observation.hidden_state.dtype).to_le_bytes());
    for dim in &observation.hidden_state.shape {
        bytes.extend_from_slice(&dim.to_le_bytes());
    }
    push_checksum_str(&mut bytes, &observation.producer_node);
    push_checksum_str(&mut bytes, &observation.consumer_node);
    push_checksum_str(&mut bytes, &observation.source);
    bytes.extend_from_slice(&observation.version.to_le_bytes());
    bytes.extend_from_slice(&observation.created_at_us.to_le_bytes());
    checksum64(&bytes)
}

fn artifact_access_checksum(event: &ArtifactAccessRecord) -> u64 {
    let mut bytes = Vec::new();
    push_checksum_str(&mut bytes, &event.event_id);
    push_checksum_str(&mut bytes, &event.artifact_id);
    bytes.extend_from_slice(&artifact_access_kind_tag(event.access).to_le_bytes());
    bytes.extend_from_slice(&execution_artifact_kind_tag(event.artifact_kind).to_le_bytes());
    push_checksum_str(&mut bytes, &event.model.model_id);
    push_checksum_str(&mut bytes, &event.model.model_key);
    bytes.extend_from_slice(&event.model.tokenizer_hash.to_le_bytes());
    bytes.extend_from_slice(&event.model.profile_hash.to_le_bytes());
    bytes.extend_from_slice(&range_boundary_phase_tag(event.boundary.phase).to_le_bytes());
    bytes.extend_from_slice(&event.boundary.step_index.to_le_bytes());
    bytes.extend_from_slice(&event.boundary.node_index.to_le_bytes());
    bytes.extend_from_slice(&event.boundary.layer_start.to_le_bytes());
    bytes.extend_from_slice(&event.boundary.layer_end.to_le_bytes());
    bytes.extend_from_slice(
        &event
            .boundary
            .next_node_index
            .unwrap_or(u32::MAX)
            .to_le_bytes(),
    );
    bytes.extend_from_slice(&event.boundary.position.to_le_bytes());
    push_checksum_str(&mut bytes, &event.run_id);
    push_checksum_str(&mut bytes, &event.batch_id);
    push_checksum_str(&mut bytes, &event.actor);
    if let Some(request_id) = &event.request_id {
        push_checksum_str(&mut bytes, request_id);
    }
    bytes.extend_from_slice(&event.artifact_checksum.to_le_bytes());
    bytes.extend_from_slice(&event.version.to_le_bytes());
    bytes.extend_from_slice(&event.created_at_us.to_le_bytes());
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
    use sim_models::engram_hash::ENGRAM_HASH_ALGORITHM_VERSION;
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
    fn update_record_state_versions_catalog_and_filters_queries() {
        let mut service = populated_service();
        let updated = service
            .update_record_state(
                "corpus/0",
                "record/0",
                MemoryRecordState::Tombstoned,
                2,
                "unit-test",
                "verify tombstone filtering",
            )
            .expect("tombstone record");
        assert_eq!(updated.state, MemoryRecordState::Tombstoned);
        assert_eq!(updated.version, 2);
        let event = service
            .record_lifecycle_event("record-lifecycle/corpus/0/record/0/2")
            .expect("record lifecycle event");
        assert_eq!(event.previous_state, MemoryRecordState::Committed);
        assert_eq!(event.new_state, MemoryRecordState::Tombstoned);

        let snapshot = service
            .export_catalog_snapshot("corpus/0")
            .expect("export updated catalog");
        assert_eq!(snapshot.catalog.version, 2);
        assert_eq!(snapshot.records[0].state, MemoryRecordState::Tombstoned);

        let result = service
            .query_memory(
                MemoryQuery {
                    query_id: "q/tombstoned".to_string(),
                    corpus_ids: vec!["corpus/0".to_string()],
                    scope_filter: Vec::new(),
                    visibility_filter: Vec::new(),
                    min_trust: MemoryTrustLevel::UserConfirmed,
                    min_confidence: 0.0,
                    embedding_model_version: "embed/v1".to_string(),
                    top_k: 8,
                    query_embedding_ref: None,
                },
                3,
            )
            .expect("query tombstoned catalog");
        assert!(result.matches.is_empty());

        let mut durable = LingquMemoryDurableStore::new();
        service
            .persist_record_lifecycle_events_to_dfs(&mut durable)
            .expect("persist lifecycle audit");
        service
            .persist_record_lifecycle_events_to_dfs(&mut durable)
            .expect("persist lifecycle audit idempotently");
        let events = durable
            .load_record_lifecycle_event_manifest()
            .expect("load lifecycle audit");
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].record_id, "record/0");
        let mut restored = LingquMemoryService::new();
        restored
            .rebuild_record_lifecycle_events_from_dfs(&mut durable)
            .expect("rebuild lifecycle audit");
        assert!(restored
            .record_lifecycle_event("record-lifecycle/corpus/0/record/0/2")
            .is_some());
    }

    #[test]
    fn execution_artifact_requires_model_bound_payload() {
        let artifact = ExecutionArtifactObject {
            artifact_id: "artifact/missing-payload".to_string(),
            kind: ExecutionArtifactKind::HiddenState,
            model: sample_model_binding(),
            producer_boundary: sample_range_boundary(),
            boundary_hidden_fingerprint: sample_boundary_hidden_fingerprint(),
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
    fn artifact_access_audit_persists_parallel_runs_and_batches() {
        let model = sample_model_binding();
        let boundary = sample_range_boundary();
        let artifact = sample_logits_execution_artifact("artifact/logits/step3/node4");
        let mut service = LingquMemoryService::new();
        service
            .register_execution_artifact(artifact.clone())
            .expect("register execution artifact");
        let produced = ArtifactAccessRecord::new(
            "artifact-access/run0/batch0/produce".to_string(),
            artifact.artifact_id.clone(),
            ArtifactAccessKind::Produced,
            artifact.kind,
            model.clone(),
            boundary.clone(),
            "run0".to_string(),
            "batch0".to_string(),
            "node4".to_string(),
            Some("publish/step3/node4".to_string()),
            artifact.checksum,
            1,
            11,
        )
        .expect("build produce access event");
        let consumed_run0 = ArtifactAccessRecord::new(
            "artifact-access/run0/batch0/consume".to_string(),
            artifact.artifact_id.clone(),
            ArtifactAccessKind::Consumed,
            artifact.kind,
            model.clone(),
            boundary.clone(),
            "run0".to_string(),
            "batch0".to_string(),
            "w5-runtime-planner".to_string(),
            Some("boundary/step3/node4".to_string()),
            artifact.checksum,
            1,
            12,
        )
        .expect("build run0 consume access event");
        let consumed_run1 = ArtifactAccessRecord::new(
            "artifact-access/run1/batch3/consume".to_string(),
            artifact.artifact_id.clone(),
            ArtifactAccessKind::Consumed,
            artifact.kind,
            model,
            boundary,
            "run1".to_string(),
            "batch3".to_string(),
            "w5-runtime-planner".to_string(),
            Some("boundary/step3/node4/run1".to_string()),
            artifact.checksum,
            1,
            13,
        )
        .expect("build run1 consume access event");

        service
            .record_artifact_access(produced)
            .expect("record produce access");
        service
            .record_artifact_access(consumed_run0)
            .expect("record run0 consume access");
        service
            .record_artifact_access(consumed_run1)
            .expect("record run1 consume access");

        let mut durable = LingquMemoryDurableStore::new();
        service
            .persist_execution_artifacts_to_dfs(&mut durable)
            .expect("persist artifacts");
        service
            .persist_artifact_access_to_dfs(&mut durable)
            .expect("persist artifact access audit");
        service
            .persist_artifact_access_to_dfs(&mut durable)
            .expect("persist artifact access audit idempotently");

        let events = durable
            .load_artifact_access_manifest()
            .expect("load artifact access audit");
        assert_eq!(events.len(), 3);
        assert_eq!(
            events
                .iter()
                .filter(|event| event.access == ArtifactAccessKind::Consumed)
                .count(),
            2
        );
        assert!(events
            .iter()
            .any(|event| event.run_id == "run1" && event.batch_id == "batch3"));

        let mut restored = LingquMemoryService::new();
        restored
            .rebuild_execution_artifacts_from_dfs(&mut durable)
            .expect("rebuild artifacts");
        restored
            .rebuild_artifact_access_from_dfs(&mut durable)
            .expect("rebuild artifact access audit");
        assert!(restored
            .artifact_access_event("artifact-access/run0/batch0/produce")
            .is_some());
        assert_eq!(restored.artifact_access_events().len(), 3);
    }

    #[test]
    fn artifact_access_audit_rejects_same_event_id_with_different_payload() {
        let artifact = sample_logits_execution_artifact("artifact/logits/step3/node4");
        let first = ArtifactAccessRecord::new(
            "artifact-access/conflict".to_string(),
            artifact.artifact_id.clone(),
            ArtifactAccessKind::Produced,
            artifact.kind,
            artifact.model.clone(),
            artifact.producer_boundary.clone(),
            "run0".to_string(),
            "batch0".to_string(),
            "node4".to_string(),
            None,
            artifact.checksum,
            1,
            11,
        )
        .expect("build first access event");
        let conflicting = ArtifactAccessRecord::new(
            "artifact-access/conflict".to_string(),
            artifact.artifact_id.clone(),
            ArtifactAccessKind::Consumed,
            artifact.kind,
            artifact.model.clone(),
            artifact.producer_boundary.clone(),
            "run0".to_string(),
            "batch0".to_string(),
            "node5".to_string(),
            None,
            artifact.checksum,
            1,
            12,
        )
        .expect("build conflicting access event");
        let mut durable = LingquMemoryDurableStore::new();
        durable
            .persist_execution_artifact_manifest(vec![artifact])
            .expect("persist artifact");
        durable
            .persist_artifact_access_manifest(vec![first])
            .expect("persist first access event");
        let err = durable
            .persist_artifact_access_manifest(vec![conflicting])
            .expect_err("conflicting event id must fail");
        assert!(matches!(
            err,
            LingquMemoryError::InvalidValue {
                field: "artifact_access.event_id",
                ..
            }
        ));
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

        assert_eq!(response.support.supported_action, ShortpathAction::Continue);
        assert_eq!(response.artifact, None);
        assert_eq!(
            service
                .shortpath_support("shortpath-support/boundary/continue")
                .unwrap()
                .reason,
            "no_verified_execution_artifact_support"
        );
    }

    #[test]
    fn boundary_observation_persists_and_builds_lookup_request() {
        let mut service = LingquMemoryService::new();
        let hidden_ref = HotTensorObjectRef {
            object_key: "hidden/qwen3-0-6b/node4/range-runtime-input/decode-step2".to_string(),
            version: 1,
            backend: HotObjectBackend::ObmmShmem,
            storage_ref: "obmm://hidden/qwen3-0-6b/node4/range-runtime-input/decode-step2"
                .to_string(),
            segment: None,
            offset: 0,
            bytes: 262144,
            checksum: 0xe209_8418_c4d8_4107,
            dtype: TensorDType::Opaque,
            shape: vec![262144],
        };
        let observation = BoundaryObservationRecord::new(
            "boundary-observation/run0/step2/node3".to_string(),
            "run0".to_string(),
            sample_model_binding(),
            RangeBoundary {
                phase: RangeBoundaryPhase::RangeExit,
                step_index: 2,
                node_index: 3,
                layer_start: 8,
                layer_end: 12,
                next_node_index: Some(4),
                position: 6,
            },
            hidden_ref.clone(),
            "node3".to_string(),
            "node4".to_string(),
            "w5_guest_range_exit".to_string(),
            1,
            100,
        )
        .expect("build observation");
        service
            .register_boundary_observation(observation.clone())
            .expect("register observation");
        let request = observation
            .to_lookup_request(
                "boundary/run0/step2/node3".to_string(),
                None,
                900,
                vec![ShortpathAction::JumpToTerminal],
                101,
            )
            .expect("build lookup request");
        assert_eq!(request.hidden_state, hidden_ref);
        assert_eq!(request.boundary.layer_end, 12);

        let mut durable = LingquMemoryDurableStore::new();
        service
            .persist_boundary_observations_to_dfs(&mut durable)
            .expect("persist observations");
        service
            .persist_boundary_observations_to_dfs(&mut durable)
            .expect("persist observations idempotently");
        let persisted = durable
            .load_boundary_observation_manifest()
            .expect("load observations");
        assert_eq!(persisted, vec![observation.clone()]);

        let mut restored = LingquMemoryService::new();
        restored
            .rebuild_boundary_observations_from_dfs(&mut durable)
            .expect("rebuild observations");
        assert_eq!(
            restored
                .boundary_observation("boundary-observation/run0/step2/node3")
                .expect("restored observation"),
            &observation
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
            boundary_hidden_fingerprint: BoundaryTensorFingerprint::from_hot_ref(&hidden_ref),
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

        assert_eq!(
            response.support.supported_action,
            ShortpathAction::JumpToTerminal
        );
        assert_eq!(
            response.support.artifact_id.as_deref(),
            Some("artifact/logits/step3/node4")
        );
        assert_eq!(response.support.target_layer_start, Some(8));
        assert_eq!(response.support.target_layer_end, Some(8));
        assert_eq!(
            response.support.producer_position,
            Some(sample_range_boundary().position)
        );
        assert_eq!(response.support.confidence_milli, 980);
        assert_eq!(
            service
                .execution_artifact("artifact/logits/step3/node4")
                .unwrap()
                .kind,
            ExecutionArtifactKind::Logits
        );
    }

    #[test]
    fn paper_engram_manifests_validate_checksums() {
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let module = sample_paper_engram_module_manifest();

        assert!(projection.checksum != 0);
        assert!(hash_config.checksum != 0);
        assert_eq!(hash_config.table_specs.len(), 1);
        assert_eq!(hash_config.table_specs[0].order, 2);
        assert_eq!(hash_config.table_specs[0].head, 0);
        assert_eq!(
            hash_config.table_specs[0].table_rows,
            hash_config.table_rows
        );
        assert_eq!(hash_config.table_specs[0].seed, hash_config.seed);
        assert!(shard.checksum != 0);
        assert!(gate.checksum != 0);
        assert!(module.checksum != 0);
        let mut projection_without_checksum = projection.clone();
        projection_without_checksum.checksum = 0;
        assert_eq!(
            PaperEngramTokenizerProjectionManifest::new(projection_without_checksum)
                .expect("projection constructor computes checksum"),
            projection
        );
        let mut hash_config_without_checksum = hash_config.clone();
        hash_config_without_checksum.checksum = 0;
        assert_eq!(
            PaperEngramHashConfigManifest::new(hash_config_without_checksum)
                .expect("hash config constructor computes checksum"),
            hash_config
        );

        let mut corrupted_projection = projection;
        corrupted_projection.checksum ^= 1;
        assert!(corrupted_projection.validate().is_err());

        let mut corrupted_hash_config = hash_config;
        corrupted_hash_config.checksum ^= 1;
        assert!(corrupted_hash_config.validate().is_err());

        let mut unsupported_hash_config = sample_paper_engram_hash_config_manifest();
        unsupported_hash_config.algorithm = "custom".to_string();
        unsupported_hash_config.checksum =
            paper_engram_hash_config_manifest_checksum(&unsupported_hash_config);
        assert!(matches!(
            unsupported_hash_config.validate(),
            Err(LingquMemoryError::InvalidValue {
                field: "paper_engram_hash_config.algorithm",
                ..
            })
        ));

        let mut corrupted_shard = shard.clone();
        corrupted_shard.checksum ^= 1;
        assert!(corrupted_shard.validate().is_err());

        let mut corrupted_gate = gate.clone();
        corrupted_gate.checksum ^= 1;
        assert!(corrupted_gate.validate().is_err());

        let mut corrupted_module = module;
        corrupted_module.checksum ^= 1;
        assert!(corrupted_module.validate().is_err());
    }

    #[test]
    fn paper_engram_hash_config_accepts_legacy_manifest_without_table_specs() {
        let mut legacy = sample_paper_engram_hash_config_manifest();
        legacy.table_specs.clear();
        legacy.checksum = paper_engram_hash_config_manifest_checksum(&legacy);
        legacy
            .validate()
            .expect("legacy hash config without table specs should remain valid");
        let mut value = serde_json::to_value(&legacy).expect("encode legacy hash config");
        value
            .as_object_mut()
            .expect("legacy hash config JSON object")
            .remove("table_specs");
        let bytes = serde_json::to_vec_pretty(&value).expect("encode legacy hash config JSON");
        let decoded = PaperEngramHashConfigManifest::from_json_bytes(&bytes)
            .expect("decode legacy hash config without table specs");
        assert!(decoded.table_specs.is_empty());
        assert_eq!(decoded.checksum, legacy.checksum);
    }

    #[test]
    fn paper_engram_hash_config_rejects_incomplete_table_specs() {
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let mut hash_config = PaperEngramHashConfigManifest {
            hash_config_id: "pe-hash-config-incomplete-specs".to_string(),
            model_id: "Qwen3-0.6B".to_string(),
            tokenizer_projection_id: projection.projection_id,
            tokenizer_projection_checksum: projection.projection_checksum,
            hash_config_ref: LingquDfsPath::new(
                "/lingqu/memory/engram/hash-config-incomplete-specs.json",
            ),
            hash_config_checksum: 0x2470,
            orders: vec![2],
            heads_per_order: 2,
            table_rows: 1024,
            table_specs: vec![Qwen3DenseReferenceEngramHashTableSpec {
                order: 2,
                head: 0,
                table_rows: 1024,
                seed: 0x1234_5678,
            }],
            seed: 0x1234_5678,
            algorithm: ENGRAM_HASH_ALGORITHM_VERSION.to_string(),
            source_ref: Some("dfs://pe/hash/incomplete-specs".to_string()),
            checksum: 1,
            version: 1,
            created_at_us: 9,
            expires_at_us: Some(900),
        };
        hash_config.checksum = paper_engram_hash_config_manifest_checksum(&hash_config);
        assert_eq!(
            hash_config
                .validate()
                .expect_err("non-empty table specs must cover every order/head"),
            LingquMemoryError::MissingField("paper_engram_hash_config.table_specs")
        );
    }

    #[test]
    fn paper_engram_eval_report_accepts_legacy_checksum_without_runtime_context() {
        let mut legacy = sample_paper_engram_eval_report_manifest();
        legacy.runtime_context_steps_expected = None;
        legacy.runtime_context_steps_observed = None;
        legacy.checksum =
            paper_engram_eval_report_manifest_legacy_checksum_without_runtime_context(&legacy);
        legacy
            .validate()
            .expect("legacy eval report without runtime context coverage checksum");
        let bytes = serde_json::to_vec_pretty(&legacy).expect("encode legacy eval report");
        assert_eq!(
            PaperEngramEvalReportManifest::from_json_bytes(&bytes)
                .expect("decode legacy eval report"),
            legacy
        );

        let mut invalid = sample_paper_engram_eval_report_manifest();
        invalid.checksum =
            paper_engram_eval_report_manifest_legacy_checksum_without_runtime_context(&invalid);
        assert!(matches!(
            invalid.validate(),
            Err(LingquMemoryError::PayloadChecksumMismatch { .. })
        ));
    }

    #[test]
    fn paper_engram_module_registration_requires_dependencies() {
        let module = sample_paper_engram_module_manifest();
        let mut service = LingquMemoryService::new();
        let err = service
            .register_paper_engram_module(module)
            .expect_err("register module should fail before shards and gates are registered");
        assert!(matches!(
            err,
            LingquMemoryError::MissingField("paper_engram_module.table_shard_ids")
        ));
    }

    #[test]
    fn paper_engram_module_registration_requires_projection_and_hash_bindings() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let module = sample_paper_engram_module_manifest();

        service
            .register_paper_engram_table_shard(shard.clone())
            .expect("register shard");
        service
            .register_paper_engram_gate(gate.clone())
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module.clone())
                .expect_err("module must not register without projection manifest"),
            LingquMemoryError::MissingField("paper_engram_module.tokenizer_projection_ref")
        );

        service
            .register_paper_engram_tokenizer_projection(projection.clone())
            .expect("register projection");
        assert_eq!(
            service
                .register_paper_engram_module(module.clone())
                .expect_err("module must not register without hash config manifest"),
            LingquMemoryError::MissingField("paper_engram_module.hash_config_ref")
        );

        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_module(module)
            .expect("register module with projection and hash config bindings");
    }

    #[test]
    fn paper_engram_module_registration_rejects_projection_hash_mismatch() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let mut alternate_projection = projection.clone();
        alternate_projection.projection_id = "pe-projection-alt".to_string();
        alternate_projection.projection_ref =
            LingquDfsPath::new("/lingqu/memory/engram/tokenizer-proj-alt.json");
        alternate_projection.projection_checksum = 0x2468;
        alternate_projection.checksum =
            paper_engram_tokenizer_projection_manifest_checksum(&alternate_projection);
        alternate_projection
            .validate()
            .expect("build alternate projection");

        let mut alternate_hash_config = sample_paper_engram_hash_config_manifest();
        alternate_hash_config.hash_config_ref =
            LingquDfsPath::new("/lingqu/memory/engram/hash-config-alt.json");
        alternate_hash_config.tokenizer_projection_id = alternate_projection.projection_id.clone();
        alternate_hash_config.tokenizer_projection_checksum =
            alternate_projection.projection_checksum;
        alternate_hash_config.checksum =
            paper_engram_hash_config_manifest_checksum(&alternate_hash_config);
        alternate_hash_config
            .validate()
            .expect("build alternate hash config");

        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let mut module = sample_paper_engram_module_manifest();
        module.hash_config_ref = alternate_hash_config.hash_config_ref.clone();
        module.checksum = paper_engram_module_manifest_checksum(&module);
        module.validate().expect("build mismatched module");

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register module projection");
        service
            .register_paper_engram_tokenizer_projection(alternate_projection)
            .expect("register alternate projection");
        service
            .register_paper_engram_hash_config(alternate_hash_config)
            .expect("register alternate hash config");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("module projection and hash config must bind the same projection"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_module.hash_config_ref",
                reason: "hash config tokenizer projection must match module projection ref"
            }
        );
    }

    #[test]
    fn paper_engram_module_registration_rejects_incompatible_table_shard() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let mut shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let module = sample_paper_engram_module_manifest();

        shard.shape[1] = module.memory_dim + 1;
        shard.checksum = paper_engram_table_shard_manifest_checksum(&shard);
        shard.validate().expect("build incompatible table shard");

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard before module compatibility check");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("module must reject incompatible table shard"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_shard.shape",
                reason: "table shard memory dimension must match module memory_dim"
            }
        );
    }

    #[test]
    fn paper_engram_module_registration_rejects_incompatible_gate() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let mut gate = sample_paper_engram_gate_manifest();
        let module = sample_paper_engram_module_manifest();

        gate.layer = module.layers[0] + 1;
        gate.checksum = paper_engram_gate_manifest_checksum(&gate);
        gate.validate().expect("build incompatible gate");

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate before module compatibility check");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("module must reject incompatible gate"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_gate.layer",
                reason: "gate layer must be declared by module"
            }
        );
    }

    #[test]
    fn paper_engram_module_registration_rejects_incomplete_table_row_coverage() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let mut shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let module = sample_paper_engram_module_manifest();

        shard.row_end = hash_config.table_rows / 2;
        shard.shape[0] = shard.row_end - shard.row_start;
        shard.checksum = paper_engram_table_shard_manifest_checksum(&shard);
        shard
            .validate()
            .expect("build partial-coverage table shard");

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard before row coverage check");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("module must reject incomplete table row coverage"),
            LingquMemoryError::MissingField("paper_engram_runtime.table_row_block")
        );
    }

    #[test]
    fn paper_engram_module_registration_rejects_table_rows_past_hash_config() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let mut shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let module = sample_paper_engram_module_manifest();

        shard.row_end = hash_config.table_rows + 1;
        shard.shape[0] = shard.row_end - shard.row_start;
        shard.checksum = paper_engram_table_shard_manifest_checksum(&shard);
        shard.validate().expect("build out-of-range table shard");

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard before row coverage check");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("module must reject shard rows beyond hash config"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_shard.row_range",
                reason: "table shard row range must fit hash config table spec rows"
            }
        );
    }

    #[test]
    fn paper_engram_module_registration_rejects_incompatible_gate_shape() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let mut gate = sample_paper_engram_gate_manifest();
        let module = sample_paper_engram_module_manifest();

        gate.shape = vec![module.hidden_size + 1];
        gate.checksum = paper_engram_gate_manifest_checksum(&gate);
        gate.validate().expect("build incompatible gate shape");

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate before module compatibility check");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("module must reject incompatible gate shape"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_gate.shape",
                reason: "gate shape must match module hidden_size"
            }
        );
    }

    #[test]
    fn paper_engram_module_resolves_by_model_and_engram_id() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let module = sample_paper_engram_module_manifest();

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");
        service
            .register_paper_engram_module(module.clone())
            .expect("register module");

        let resolved = service
            .paper_engram_module_by_model(&module.model.model_id, &module.module_name)
            .expect("resolve module by model and engram id");
        assert_eq!(resolved.module_id, module.module_id);

        let mut duplicate = module.clone();
        duplicate.module_id = "pe-module-duplicate-name".to_string();
        duplicate.checksum = paper_engram_module_manifest_checksum(&duplicate);
        assert_eq!(
            service
                .register_paper_engram_module(duplicate)
                .expect_err("model_id and engram_id must remain unique"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_module.module_name",
                reason: "module_name must be unique per model_id"
            }
        );
    }

    #[test]
    fn paper_engram_registry_round_trips_via_durable_store() {
        let mut service = LingquMemoryService::new();
        let mut durable = LingquMemoryDurableStore::new();

        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let recipe = sample_paper_engram_training_recipe_manifest();
        let report = sample_paper_engram_eval_report_manifest();
        let mut module = sample_paper_engram_module_manifest();
        module.quality_claim = PaperEngramQualityClaim::Posttrain;
        module.training_recipe_ref = Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
        module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
        module.checksum = paper_engram_module_manifest_checksum(&module);

        service
            .register_paper_engram_tokenizer_projection(projection.clone())
            .expect("register tokenizer projection");
        service
            .register_paper_engram_hash_config(hash_config.clone())
            .expect("register hash config");
        service
            .register_paper_engram_training_recipe(recipe.clone())
            .expect("register training recipe");
        service
            .register_paper_engram_eval_report(report.clone())
            .expect("register eval report");
        service
            .register_paper_engram_table_shard(shard.clone())
            .expect("register shard");
        service
            .register_paper_engram_gate(gate.clone())
            .expect("register gate");
        service
            .register_paper_engram_module(module.clone())
            .expect("register module");
        service
            .persist_paper_engram_tokenizer_projections_to_dfs(&mut durable)
            .expect("persist tokenizer projection manifests");
        service
            .persist_paper_engram_hash_configs_to_dfs(&mut durable)
            .expect("persist hash config manifests");
        service
            .persist_paper_engram_training_recipes_to_dfs(&mut durable)
            .expect("persist training recipe manifests");
        service
            .persist_paper_engram_eval_reports_to_dfs(&mut durable)
            .expect("persist eval report manifests");
        service
            .persist_paper_engram_table_shards_to_dfs(&mut durable)
            .expect("persist table shard manifests");
        service
            .persist_paper_engram_gates_to_dfs(&mut durable)
            .expect("persist gate manifests");
        service
            .persist_paper_engram_modules_to_dfs(&mut durable)
            .expect("persist module manifest");

        let mut restored = LingquMemoryService::new();
        let rebuilt_projections = restored
            .rebuild_paper_engram_tokenizer_projections_from_dfs(&mut durable)
            .expect("rebuild tokenizer projections");
        let rebuilt_hash_configs = restored
            .rebuild_paper_engram_hash_configs_from_dfs(&mut durable)
            .expect("rebuild hash configs");
        let rebuilt_recipes = restored
            .rebuild_paper_engram_training_recipes_from_dfs(&mut durable)
            .expect("rebuild training recipes");
        let rebuilt_reports = restored
            .rebuild_paper_engram_eval_reports_from_dfs(&mut durable)
            .expect("rebuild eval reports");
        let rebuilt_shards = restored
            .rebuild_paper_engram_table_shards_from_dfs(&mut durable)
            .expect("rebuild shards");
        let rebuilt_gates = restored
            .rebuild_paper_engram_gates_from_dfs(&mut durable)
            .expect("rebuild gates");
        let rebuilt_modules = restored
            .rebuild_paper_engram_modules_from_dfs(&mut durable)
            .expect("rebuild modules");

        assert_eq!(rebuilt_projections, vec![projection.clone()]);
        assert_eq!(rebuilt_hash_configs, vec![hash_config.clone()]);
        assert_eq!(rebuilt_recipes, vec![recipe.clone()]);
        assert_eq!(rebuilt_reports, vec![report.clone()]);
        assert_eq!(rebuilt_shards, vec![shard.clone()]);
        assert_eq!(rebuilt_gates, vec![gate.clone()]);
        assert_eq!(rebuilt_modules, vec![module.clone()]);
        assert_eq!(
            restored.paper_engram_tokenizer_projection(&projection.projection_id),
            Some(&projection)
        );
        assert_eq!(
            restored.paper_engram_hash_config(&hash_config.hash_config_id),
            Some(&hash_config)
        );
        assert_eq!(
            restored.paper_engram_table_shard(&shard.shard_id),
            Some(&shard)
        );
        assert_eq!(restored.paper_engram_gate(&gate.gate_id), Some(&gate));
        assert_eq!(
            restored.paper_engram_module(&module.module_id),
            Some(&module)
        );
        restored
            .validate_paper_engram_module_quality(&module.module_id)
            .expect("validate rebuilt trained paper Engram quality claim");
        let runtime = restored
            .resolve_paper_engram_runtime_artifacts(&module.module_id)
            .expect("resolve paper engram runtime artifacts");
        assert_eq!(runtime.tokenizer_projection, projection);
        assert_eq!(runtime.hash_config, hash_config);
        assert_eq!(runtime.table_shards, vec![shard.clone()]);
        assert_eq!(runtime.gates, vec![gate.clone()]);
        assert_eq!(runtime.layer_operands.len(), 1);
        assert_eq!(runtime.layer_operands[0].layer, 3);
        assert_eq!(runtime.layer_operands[0].table_operands.len(), 1);
        assert_eq!(
            runtime.layer_operands[0].table_operands[0].shard_id,
            shard.shard_id
        );
        assert_eq!(
            runtime.layer_operands[0].table_operands[0].block_payload_refs,
            shard.block_payload_refs
        );
        assert_eq!(runtime.layer_operands[0].gate_operands.len(), 1);
        assert_eq!(
            runtime.layer_operands[0].gate_operands[0].gate_id,
            gate.gate_id
        );
        assert_eq!(
            runtime.layer_operands[0].gate_operands[0].payload_ref,
            gate.payload_ref.expect("sample gate payload")
        );

        let row_blocks = restored
            .resolve_paper_engram_table_row_blocks(PaperEngramTableRowBlockRequest {
                request_id: "row-blocks/sample".to_string(),
                module_id: module.module_id.clone(),
                layer: 3,
                order: 2,
                head: 0,
                row_start: 10,
                row_end: 11,
                created_at_us: 15,
            })
            .expect("resolve paper Engram table row blocks");
        assert_eq!(row_blocks.shard_id, shard.shard_id);
        assert_eq!(row_blocks.shard_row_start, 0);
        assert_eq!(row_blocks.shard_row_end, 1024);
        assert_eq!(row_blocks.row_payload_offset_bytes, 10 * 512 * 4);
        assert_eq!(row_blocks.row_payload_bytes, 512 * 4);
        assert_eq!(row_blocks.block_payload_refs, shard.block_payload_refs);

        let prefetch_plan = restored
            .plan_paper_engram_table_row_prefetch(PaperEngramTableRowPrefetchRequest {
                request_id: "prefetch/sample".to_string(),
                module_id: module.module_id.clone(),
                canonical_history: vec![7, 8, 9],
                from_step: 0,
                created_at_us: 16,
            })
            .expect("plan paper Engram table row prefetch");
        assert_eq!(
            prefetch_plan.tokenizer_projection_checksum,
            projection.projection_checksum
        );
        assert_eq!(
            prefetch_plan.hash_config_checksum,
            hash_config.hash_config_checksum
        );
        assert_eq!(prefetch_plan.rows.len(), 2);
        assert!(prefetch_plan
            .rows
            .iter()
            .all(|row| row.layer == 3 && row.order == 2 && row.head == 0));
        assert!(prefetch_plan
            .rows
            .iter()
            .all(|row| row.shard_id == shard.shard_id));
    }

    #[test]
    fn paper_engram_row_prefetch_rejects_out_of_range_steps() {
        let request = PaperEngramTableRowPrefetchRequest {
            request_id: "prefetch/out-of-range".to_string(),
            module_id: "pe-module-0".to_string(),
            canonical_history: vec![7, 8, 9],
            from_step: 3,
            created_at_us: 16,
        };
        assert_eq!(
            request
                .validate()
                .expect_err("request from_step should fail"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_prefetch.from_step",
                reason: "from_step must be within canonical_history"
            }
        );

        let mut plan = PaperEngramTableRowPrefetchPlan {
            plan_id: "paper-engram-row-prefetch/prefetch/out-of-range".to_string(),
            request_id: "prefetch/out-of-range".to_string(),
            module_id: "pe-module-0".to_string(),
            tokenizer_projection_checksum: 0x1357,
            hash_config_checksum: 0x2468,
            canonical_history_len: 3,
            from_step: 1,
            rows: vec![PaperEngramTableRowPrefetchRef {
                step_index: 1,
                layer: 3,
                order: 2,
                head: 0,
                row: 10,
                exact_key: 0x1234,
                shard_id: "pe-shard-0".to_string(),
                row_payload_offset_bytes: 10 * 512 * 4,
                row_payload_bytes: 512 * 4,
                block_payload_refs: vec![LingquBlockPayloadRef::new(
                    "block/pe-shard-0",
                    0,
                    262144,
                    0xabc,
                )],
            }],
            created_at_us: 16,
        };
        plan.validate().expect("valid row prefetch plan");

        plan.from_step = 3;
        assert_eq!(
            plan.validate()
                .expect_err("plan from_step outside history should fail"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_prefetch.from_step",
                reason: "from_step must be within canonical_history_len"
            }
        );

        plan.from_step = 1;
        plan.rows[0].step_index = 0;
        assert_eq!(
            plan.validate()
                .expect_err("row before from_step should fail"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_prefetch_ref.step_index",
                reason: "row step_index must be covered by the plan step range"
            }
        );

        plan.rows[0].step_index = 3;
        assert_eq!(
            plan.validate()
                .expect_err("row outside history should fail"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_prefetch_ref.step_index",
                reason: "row step_index must be covered by the plan step range"
            }
        );
    }

    #[test]
    fn paper_engram_row_block_resolution_filters_payload_chunks() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let mut shard = sample_paper_engram_table_shard_manifest();
        shard.row_end = hash_config.table_rows;
        shard.shape = vec![hash_config.table_rows, 512];
        let row_bytes = 512 * 4;
        let mut block_payload_refs = (0..4)
            .map(|chunk| {
                LingquBlockPayloadRef::new(
                    format!("block/pe-shard-0/chunk-{chunk}"),
                    0,
                    4 * row_bytes,
                    0xabc0 + chunk,
                )
            })
            .collect::<Vec<_>>();
        block_payload_refs.push(LingquBlockPayloadRef::new(
            "block/pe-shard-0/chunk-rest",
            0,
            (hash_config.table_rows - 16) * row_bytes,
            0xabcf,
        ));
        shard.block_payload_refs = block_payload_refs;
        shard.checksum = paper_engram_table_shard_manifest_checksum(&shard);
        shard.validate().expect("chunked paper Engram table shard");
        let mut module = sample_paper_engram_module_manifest();
        module.table_shard_ids = vec![shard.shard_id.clone()];
        module.checksum = paper_engram_module_manifest_checksum(&module);

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_table_shard(shard.clone())
            .expect("register chunked shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");
        service
            .register_paper_engram_module(module.clone())
            .expect("register module");

        let aligned = service
            .resolve_paper_engram_table_row_blocks(PaperEngramTableRowBlockRequest {
                request_id: "row-blocks/aligned".to_string(),
                module_id: module.module_id.clone(),
                layer: 3,
                order: 2,
                head: 0,
                row_start: 4,
                row_end: 8,
                created_at_us: 17,
            })
            .expect("resolve aligned row block");
        assert_eq!(aligned.row_payload_offset_bytes, 4 * 512 * 4);
        assert_eq!(aligned.row_payload_bytes, 4 * 512 * 4);
        assert_eq!(
            aligned.block_payload_refs,
            vec![shard.block_payload_refs[1].clone()]
        );
        let mut bad_window = aligned.clone();
        bad_window.row_payload_offset_bytes = bad_window.row_payload_offset_bytes.saturating_add(4);
        assert_eq!(
            bad_window
                .validate()
                .expect_err("row block response window mismatch should fail"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block_response.row_payload_window",
                reason: "row payload window must match row range, dtype, and shape"
            }
        );

        let straddling = service
            .resolve_paper_engram_table_row_blocks(PaperEngramTableRowBlockRequest {
                request_id: "row-blocks/straddling".to_string(),
                module_id: module.module_id.clone(),
                layer: 3,
                order: 2,
                head: 0,
                row_start: 3,
                row_end: 5,
                created_at_us: 18,
            })
            .expect("resolve straddling row block");
        assert_eq!(straddling.row_payload_offset_bytes, 3 * 512 * 4);
        assert_eq!(straddling.row_payload_bytes, 2 * 512 * 4);
        assert_eq!(
            straddling.block_payload_refs,
            vec![
                shard.block_payload_refs[0].clone(),
                shard.block_payload_refs[1].clone()
            ]
        );
    }

    #[test]
    fn paper_engram_row_block_resolution_uses_table_spec_rows() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let mut hash_config = sample_paper_engram_hash_config_manifest();
        hash_config.table_specs[0].table_rows = 512;
        hash_config.checksum = paper_engram_hash_config_manifest_checksum(&hash_config);
        hash_config
            .validate()
            .expect("build hash config with per-head table rows");
        let gate = sample_paper_engram_gate_manifest();
        let mut shard = sample_paper_engram_table_shard_manifest();
        shard.row_end = 512;
        shard.shape[0] = 512;
        shard.block_payload_refs = vec![LingquBlockPayloadRef::new(
            "block/pe-shard-0",
            0,
            512 * 512 * 4,
            0xabc,
        )];
        shard.checksum = paper_engram_table_shard_manifest_checksum(&shard);
        shard
            .validate()
            .expect("build shard covering table spec rows");
        let mut module = sample_paper_engram_module_manifest();
        module.table_shard_ids = vec![shard.shard_id.clone()];
        module.checksum = paper_engram_module_manifest_checksum(&module);

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_table_shard(shard.clone())
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");
        service
            .register_paper_engram_module(module.clone())
            .expect("register module with per-head row coverage");

        let row_block = service
            .resolve_paper_engram_table_row_blocks(PaperEngramTableRowBlockRequest {
                request_id: "row-blocks/table-spec-ok".to_string(),
                module_id: module.module_id.clone(),
                layer: 3,
                order: 2,
                head: 0,
                row_start: 511,
                row_end: 512,
                created_at_us: 17,
            })
            .expect("resolve last row covered by table spec");
        assert_eq!(row_block.shard_id, shard.shard_id);
        assert_eq!(row_block.shard_row_end, 512);

        assert_eq!(
            service
                .resolve_paper_engram_table_row_blocks(PaperEngramTableRowBlockRequest {
                    request_id: "row-blocks/table-spec-oob".to_string(),
                    module_id: module.module_id,
                    layer: 3,
                    order: 2,
                    head: 0,
                    row_start: 512,
                    row_end: 513,
                    created_at_us: 18,
                })
                .expect_err("row beyond per-head table spec should fail"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_row_block.row_range",
                reason: "requested row range must fit hash config table spec rows"
            }
        );
    }

    #[test]
    fn paper_engram_quality_claim_requires_training_recipe_and_eval_report() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let recipe = sample_paper_engram_training_recipe_manifest();
        let report = sample_paper_engram_eval_report_manifest();
        let mut module = sample_paper_engram_module_manifest();
        module.quality_claim = PaperEngramQualityClaim::Posttrain;
        module.training_recipe_ref = Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
        module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
        module.checksum = paper_engram_module_manifest_checksum(&module);
        module.validate().expect("trained module manifest");

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_training_recipe(recipe.clone())
            .expect("register training recipe");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        let err = service
            .register_paper_engram_module(module.clone())
            .expect_err("trained module requires eval report");
        assert_eq!(
            err,
            LingquMemoryError::MissingField("paper_engram_module.eval_report_ref")
        );

        service
            .register_paper_engram_eval_report(report)
            .expect("register eval report");
        service
            .register_paper_engram_module(module.clone())
            .expect("register trained module with evidence");
        service
            .validate_paper_engram_module_quality(&module.module_id)
            .expect("validate trained module quality claim");
    }

    #[test]
    fn paper_engram_imported_quality_claim_requires_external_import_evidence() {
        let mut module = sample_paper_engram_module_manifest();
        module.quality_claim = PaperEngramQualityClaim::Imported;
        module.checksum = paper_engram_module_manifest_checksum(&module);
        assert_eq!(
            module
                .validate()
                .expect_err("imported module must bind provenance evidence"),
            LingquMemoryError::MissingField("paper_engram_module.training_recipe_ref")
        );

        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let mut recipe = sample_paper_engram_training_recipe_manifest();
        recipe.mode = PaperEngramTrainingMode::ExternalImport;
        recipe.objective = "external-paper-engram-import".to_string();
        recipe.evidence_refs = vec!["dfs://imports/pe-module-0/provenance.json".to_string()];
        recipe.checksum = paper_engram_training_recipe_manifest_checksum(&recipe);
        recipe
            .validate()
            .expect("external import recipe remains valid");
        let report = sample_paper_engram_eval_report_manifest();
        module.training_recipe_ref = Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
        module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
        module.checksum = paper_engram_module_manifest_checksum(&module);

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_training_recipe(recipe)
            .expect("register external import recipe");
        service
            .register_paper_engram_eval_report(report)
            .expect("register eval report");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");
        service
            .register_paper_engram_module(module.clone())
            .expect("register imported module with provenance evidence");
        service
            .validate_paper_engram_module_quality(&module.module_id)
            .expect("validate imported module quality claim");
    }

    #[test]
    fn paper_engram_finetune_quality_claim_accepts_lora_and_full_finetune_recipes() {
        for mode in [
            PaperEngramTrainingMode::EngramLora,
            PaperEngramTrainingMode::FullFinetune,
        ] {
            let mut service = LingquMemoryService::new();
            let projection = sample_paper_engram_tokenizer_projection_manifest();
            let hash_config = sample_paper_engram_hash_config_manifest();
            let shard = sample_paper_engram_table_shard_manifest();
            let gate = sample_paper_engram_gate_manifest();
            let mut recipe = sample_paper_engram_training_recipe_manifest();
            recipe.mode = mode;
            recipe.objective = match mode {
                PaperEngramTrainingMode::EngramLora => "next-token-loss+paper-engram-lora",
                PaperEngramTrainingMode::FullFinetune => "next-token-loss+paper-engram-full",
                _ => unreachable!(),
            }
            .to_string();
            recipe.frozen_base_model = false;
            recipe.lora_enabled = matches!(mode, PaperEngramTrainingMode::EngramLora);
            recipe.checksum = paper_engram_training_recipe_manifest_checksum(&recipe);
            recipe
                .validate()
                .expect("finetune recipe mode remains valid");
            let report = sample_paper_engram_eval_report_manifest();
            let mut module = sample_paper_engram_module_manifest();
            module.quality_claim = PaperEngramQualityClaim::Finetune;
            module.training_recipe_ref =
                Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
            module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
            module.checksum = paper_engram_module_manifest_checksum(&module);

            service
                .register_paper_engram_tokenizer_projection(projection)
                .expect("register projection");
            service
                .register_paper_engram_hash_config(hash_config)
                .expect("register hash config");
            service
                .register_paper_engram_training_recipe(recipe)
                .expect("register finetune recipe");
            service
                .register_paper_engram_eval_report(report)
                .expect("register eval report");
            service
                .register_paper_engram_table_shard(shard)
                .expect("register shard");
            service
                .register_paper_engram_gate(gate)
                .expect("register gate");
            service
                .register_paper_engram_module(module.clone())
                .expect("register finetune module");
            service
                .validate_paper_engram_module_quality(&module.module_id)
                .expect("validate finetune module quality claim");
        }
    }

    #[test]
    fn paper_engram_quality_claim_rejects_recipe_shape_mismatch() {
        for case in ["layers", "orders", "heads_per_order", "table_rows"] {
            let mut service = LingquMemoryService::new();
            let projection = sample_paper_engram_tokenizer_projection_manifest();
            let mut hash_config = sample_paper_engram_hash_config_manifest();
            let mut shard = sample_paper_engram_table_shard_manifest();
            let gate = sample_paper_engram_gate_manifest();
            let mut recipe = sample_paper_engram_training_recipe_manifest();
            let report = sample_paper_engram_eval_report_manifest();
            let mut module = sample_paper_engram_module_manifest();
            let (expected_field, expected_reason) = match case {
                "layers" => {
                    recipe.layers = vec![4];
                    (
                        "paper_engram_training_recipe.layers",
                        "training recipe layers must match module layers",
                    )
                }
                "orders" => {
                    recipe.orders = vec![3];
                    (
                        "paper_engram_training_recipe.orders",
                        "training recipe orders must match module and hash config orders",
                    )
                }
                "heads_per_order" => {
                    recipe.heads_per_order = 2;
                    (
                        "paper_engram_training_recipe.heads_per_order",
                        "training recipe heads_per_order must match module and hash config",
                    )
                }
                "table_rows" => {
                    hash_config.table_specs[0].table_rows = 512;
                    hash_config.checksum = paper_engram_hash_config_manifest_checksum(&hash_config);
                    shard.row_end = 512;
                    shard.shape[0] = 512;
                    shard.block_payload_refs = vec![LingquBlockPayloadRef::new(
                        "block/pe-shard-0",
                        0,
                        512 * 512 * 4,
                        0xabc,
                    )];
                    shard.checksum = paper_engram_table_shard_manifest_checksum(&shard);
                    (
                        "paper_engram_training_recipe.table_rows",
                        "training recipe table_rows must match hash config table specs",
                    )
                }
                _ => unreachable!(),
            };
            recipe.checksum = paper_engram_training_recipe_manifest_checksum(&recipe);
            recipe.validate().expect("mutated recipe remains valid");
            hash_config
                .validate()
                .expect("mutated hash config remains valid");
            shard.validate().expect("mutated shard remains valid");
            module.quality_claim = PaperEngramQualityClaim::Posttrain;
            module.training_recipe_ref =
                Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
            module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
            module.checksum = paper_engram_module_manifest_checksum(&module);

            service
                .register_paper_engram_tokenizer_projection(projection)
                .expect("register projection");
            service
                .register_paper_engram_hash_config(hash_config)
                .expect("register hash config");
            service
                .register_paper_engram_training_recipe(recipe)
                .expect("register recipe");
            service
                .register_paper_engram_eval_report(report)
                .expect("register eval report");
            service
                .register_paper_engram_table_shard(shard)
                .expect("register shard");
            service
                .register_paper_engram_gate(gate)
                .expect("register gate");

            assert_eq!(
                service
                    .register_paper_engram_module(module)
                    .expect_err("quality claim must reject shape mismatch"),
                LingquMemoryError::InvalidValue {
                    field: expected_field,
                    reason: expected_reason
                }
            );
        }
    }

    #[test]
    fn paper_engram_quality_claim_rejects_wrong_training_mode() {
        assert!(
            !paper_engram_quality_claim_accepts_training_mode(
                PaperEngramQualityClaim::Posttrain,
                PaperEngramTrainingMode::FullFinetune
            ),
            "posttrain quality must not accept full-finetune recipe evidence"
        );
        assert!(
            !paper_engram_quality_claim_accepts_training_mode(
                PaperEngramQualityClaim::Imported,
                PaperEngramTrainingMode::EngramLora
            ),
            "imported quality must not accept local finetune recipe evidence"
        );
    }

    #[test]
    fn paper_engram_quality_claim_requires_acceptance_evidence() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let recipe = sample_paper_engram_training_recipe_manifest();
        let mut report = sample_paper_engram_eval_report_manifest();
        report.paper_engram_decode_policy_loss_milli = None;
        report.zero_table_hidden_checksum = None;
        report.paper_engram_hidden_checksum = None;
        report.zero_table_output_checksum = None;
        report.cpu_backend_output_match = None;
        report.row_prefetch_requests = None;
        report.row_prefetch_hits = None;
        report.max_backend_latency_us = None;
        report.max_allowed_backend_latency_us = None;
        report.checksum = paper_engram_eval_report_manifest_checksum(&report);
        report
            .validate()
            .expect("plain eval report can be registered before quality acceptance");
        let mut module = sample_paper_engram_module_manifest();
        module.quality_claim = PaperEngramQualityClaim::Posttrain;
        module.training_recipe_ref = Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
        module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
        module.checksum = paper_engram_module_manifest_checksum(&module);

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_training_recipe(recipe)
            .expect("register recipe");
        service
            .register_paper_engram_eval_report(report)
            .expect("register eval report without acceptance evidence");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("quality claim requires runtime acceptance evidence"),
            LingquMemoryError::MissingField(
                "paper_engram_eval_report.paper_engram_decode_policy_loss_milli"
            )
        );
    }

    #[test]
    fn paper_engram_quality_claim_requires_decode_policy_loss_variant() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let recipe = sample_paper_engram_training_recipe_manifest();
        let mut report = sample_paper_engram_eval_report_manifest();
        report.decode_policy_loss_milli = None;
        report.checksum = paper_engram_eval_report_manifest_checksum(&report);
        report
            .validate()
            .expect("plain eval report may omit decode-policy-only loss before quality claim");
        let mut module = sample_paper_engram_module_manifest();
        module.quality_claim = PaperEngramQualityClaim::Posttrain;
        module.training_recipe_ref = Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
        module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
        module.checksum = paper_engram_module_manifest_checksum(&module);

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_training_recipe(recipe)
            .expect("register recipe");
        service
            .register_paper_engram_eval_report(report)
            .expect("register eval report without decode-policy-only loss");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("quality claim requires all four loss variants"),
            LingquMemoryError::MissingField("paper_engram_eval_report.decode_policy_loss_milli")
        );
    }

    #[test]
    fn paper_engram_quality_claim_requires_phase6_summary_provenance() {
        let mut report = sample_paper_engram_eval_report_manifest();
        report
            .evidence_refs
            .retain(|evidence_ref| !evidence_ref.starts_with("w5-summary:base:"));
        report.checksum = paper_engram_eval_report_manifest_checksum(&report);
        report
            .validate()
            .expect("plain eval report may omit Phase 6 W5 refs before quality claim");

        assert_eq!(
            validate_paper_engram_phase6_summary_provenance(&report)
                .expect_err("quality acceptance requires Phase 6 base summary provenance"),
            LingquMemoryError::MissingField(
                "paper_engram_eval_report.evidence_refs.phase6_base_summary"
            )
        );
    }

    #[test]
    fn paper_engram_quality_claim_requires_complete_row_prefetch_locality() {
        let service = LingquMemoryService::new();
        let mut report = sample_paper_engram_eval_report_manifest();
        report.row_prefetch_hits = Some(7);
        report.checksum = paper_engram_eval_report_manifest_checksum(&report);
        report
            .validate()
            .expect("plain eval report may record partial row prefetch locality");

        assert_eq!(
            service
                .validate_paper_engram_eval_acceptance_evidence(&report)
                .expect_err("quality acceptance requires complete row prefetch locality"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.row_prefetch_hits",
                reason:
                    "trained paper Engram quality requires complete row prefetch locality evidence"
            }
        );
    }

    #[test]
    fn paper_engram_quality_claim_requires_complete_runtime_context_coverage() {
        let service = LingquMemoryService::new();
        let mut report = sample_paper_engram_eval_report_manifest();
        report.runtime_context_steps_expected = Some(8);
        report.runtime_context_steps_observed = Some(7);
        report.checksum = paper_engram_eval_report_manifest_checksum(&report);
        report
            .validate()
            .expect("plain eval report may record partial runtime context coverage");

        assert_eq!(
            service
                .validate_paper_engram_eval_acceptance_evidence(&report)
                .expect_err("quality acceptance requires complete runtime context coverage"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.runtime_context_steps_observed",
                reason: "trained paper Engram quality requires runtime context evidence for every decode step"
            }
        );
    }

    #[test]
    fn paper_engram_quality_claim_rejects_fixture_artifact_provenance() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let mut shard = sample_paper_engram_table_shard_manifest();
        shard.source_ref = Some("fixture://sim-memory/paper-engram/table".to_string());
        let shard =
            PaperEngramTableShardManifest::new(shard).expect("fixture table shard manifest");
        let gate = sample_paper_engram_gate_manifest();
        let recipe = sample_paper_engram_training_recipe_manifest();
        let report = sample_paper_engram_eval_report_manifest();
        let mut module = sample_paper_engram_module_manifest();
        module.quality_claim = PaperEngramQualityClaim::Posttrain;
        module.training_recipe_ref = Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
        module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
        module.checksum = paper_engram_module_manifest_checksum(&module);

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_training_recipe(recipe)
            .expect("register recipe");
        service
            .register_paper_engram_eval_report(report)
            .expect("register eval report");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register fixture shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("fixture table must not back a trained quality claim"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_table_shard.source_ref",
                reason:
                    "trained paper Engram quality requires non-fixture table and gate provenance"
            }
        );
    }

    #[test]
    fn paper_engram_quality_source_ref_rejects_relative_fixture_path() {
        for source_ref in [
            "fixture/table.bin",
            "tests/fixture/table.bin",
            "/tmp/paper-engram/fixtures/table.bin",
        ] {
            assert_eq!(
                validate_trained_paper_engram_source_ref(
                    "paper_engram_table_shard.source_ref",
                    Some(source_ref),
                    true,
                )
                .expect_err("fixture path must not back a trained quality claim"),
                LingquMemoryError::InvalidValue {
                    field: "paper_engram_table_shard.source_ref",
                    reason:
                        "trained paper Engram quality requires non-fixture table and gate provenance"
                }
            );
        }
        validate_trained_paper_engram_source_ref(
            "paper_engram_table_shard.source_ref",
            Some("dfs://runs/qwen3-quality-train/table.safetensors"),
            true,
        )
        .expect("DFS provenance can back trained quality claim");
    }

    #[test]
    fn paper_engram_quality_claim_rejects_fixture_training_and_eval_provenance() {
        #[derive(Clone, Copy)]
        enum Case {
            RecipeDataset,
            RecipeEvidence,
            ReportValidationSet,
            ReportEvidence,
        }

        let cases = [
            (
                Case::RecipeDataset,
                "paper_engram_training_recipe.dataset_refs",
            ),
            (
                Case::RecipeEvidence,
                "paper_engram_training_recipe.evidence_refs",
            ),
            (
                Case::ReportValidationSet,
                "paper_engram_eval_report.validation_set_refs",
            ),
            (
                Case::ReportEvidence,
                "paper_engram_eval_report.evidence_refs",
            ),
        ];

        for (case, field) in cases {
            let mut service = LingquMemoryService::new();
            let projection = sample_paper_engram_tokenizer_projection_manifest();
            let hash_config = sample_paper_engram_hash_config_manifest();
            let shard = sample_paper_engram_table_shard_manifest();
            let gate = sample_paper_engram_gate_manifest();
            let mut recipe = sample_paper_engram_training_recipe_manifest();
            let mut report = sample_paper_engram_eval_report_manifest();
            match case {
                Case::RecipeDataset => {
                    recipe.dataset_refs = vec!["fixture/training-set.jsonl".to_string()];
                    recipe.checksum = paper_engram_training_recipe_manifest_checksum(&recipe);
                }
                Case::RecipeEvidence => {
                    recipe.evidence_refs = vec!["fixture://paper-engram/train-log".to_string()];
                    recipe.checksum = paper_engram_training_recipe_manifest_checksum(&recipe);
                }
                Case::ReportValidationSet => {
                    report.validation_set_refs = vec!["fixture/validation-set.jsonl".to_string()];
                    report.checksum = paper_engram_eval_report_manifest_checksum(&report);
                }
                Case::ReportEvidence => {
                    report.evidence_refs = vec!["fixture://paper-engram/eval-report".to_string()];
                    report.checksum = paper_engram_eval_report_manifest_checksum(&report);
                }
            }
            recipe
                .validate()
                .expect("fixture provenance can still be registered as a plain recipe");
            report
                .validate()
                .expect("fixture provenance can still be registered as a plain eval report");

            let mut module = sample_paper_engram_module_manifest();
            module.quality_claim = PaperEngramQualityClaim::Posttrain;
            module.training_recipe_ref =
                Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
            module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
            module.checksum = paper_engram_module_manifest_checksum(&module);

            service
                .register_paper_engram_tokenizer_projection(projection)
                .expect("register projection");
            service
                .register_paper_engram_hash_config(hash_config)
                .expect("register hash config");
            service
                .register_paper_engram_training_recipe(recipe)
                .expect("register recipe");
            service
                .register_paper_engram_eval_report(report)
                .expect("register eval report");
            service
                .register_paper_engram_table_shard(shard)
                .expect("register shard");
            service
                .register_paper_engram_gate(gate)
                .expect("register gate");

            assert_eq!(
                service
                    .register_paper_engram_module(module)
                    .expect_err("fixture provenance must not back a trained quality claim"),
                LingquMemoryError::InvalidValue {
                    field,
                    reason: "trained paper Engram quality requires non-fixture training and eval provenance"
                }
            );
        }
    }

    #[test]
    fn paper_engram_quality_claim_rejects_training_validation_overlap() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let recipe = sample_paper_engram_training_recipe_manifest();
        let mut report = sample_paper_engram_eval_report_manifest();
        report.validation_set_refs = recipe.dataset_refs.clone();
        report.checksum = paper_engram_eval_report_manifest_checksum(&report);
        report
            .validate()
            .expect("plain eval report may still reference overlapping data before quality claim");
        let mut module = sample_paper_engram_module_manifest();
        module.quality_claim = PaperEngramQualityClaim::Posttrain;
        module.training_recipe_ref = Some(paper_engram_training_recipe_dfs_path(&recipe.recipe_id));
        module.eval_report_ref = Some(paper_engram_eval_report_dfs_path(&report.report_id));
        module.checksum = paper_engram_module_manifest_checksum(&module);

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_training_recipe(recipe)
            .expect("register recipe");
        service
            .register_paper_engram_eval_report(report)
            .expect("register eval report");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("quality claim must reject train/eval data leakage"),
            LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.validation_set_refs",
                reason: "trained paper Engram quality requires validation sets distinct from training datasets"
            }
        );
    }

    #[test]
    fn paper_engram_eval_report_rejects_decode_policy_combo_regression() {
        let mut report = sample_paper_engram_eval_report_manifest();
        report.baseline_loss_milli = 1200;
        report.decode_policy_loss_milli = Some(1185);
        report.paper_engram_decode_policy_loss_milli = Some(1191);
        report.max_allowed_regression_milli = 5;
        report.checksum = paper_engram_eval_report_manifest_checksum(&report);

        let err = report
            .validate()
            .expect_err("paper Engram plus decode policy must not regress");
        assert_eq!(
            err,
            LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.paper_engram_decode_policy_loss_milli",
                reason: "paper Engram plus decode policy eval must not regress versus decode policy beyond max_allowed_regression_milli"
            }
        );
    }

    #[test]
    fn paper_engram_eval_report_rejects_decode_policy_regression() {
        let mut report = sample_paper_engram_eval_report_manifest();
        report.baseline_loss_milli = 1200;
        report.decode_policy_loss_milli = Some(1185);
        report.paper_engram_loss_milli = 1191;
        report.max_allowed_regression_milli = 5;
        report.checksum = paper_engram_eval_report_manifest_checksum(&report);

        let err = report
            .validate()
            .expect_err("paper Engram must not regress versus decode policy baseline");
        assert_eq!(
            err,
            LingquMemoryError::InvalidValue {
                field: "paper_engram_eval_report.paper_engram_loss_milli",
                reason: "paper Engram eval must not regress versus decode policy beyond max_allowed_regression_milli"
            }
        );
    }

    #[test]
    fn paper_engram_module_registration_rejects_incomplete_table_coverage() {
        let mut service = LingquMemoryService::new();
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let mut hash_config = sample_paper_engram_hash_config_manifest();
        hash_config.orders = vec![2, 3];
        hash_config.heads_per_order = 1;
        hash_config.table_specs = paper_engram_default_hash_table_specs(
            &hash_config.orders,
            hash_config.heads_per_order,
            hash_config.table_rows,
            hash_config.seed,
        )
        .expect("build complete hash table specs");
        hash_config.checksum = paper_engram_hash_config_manifest_checksum(&hash_config);
        let shard = sample_paper_engram_table_shard_manifest();
        let gate = sample_paper_engram_gate_manifest();
        let mut module = sample_paper_engram_module_manifest();
        module.orders = vec![2, 3];
        module.checksum = paper_engram_module_manifest_checksum(&module);

        service
            .register_paper_engram_tokenizer_projection(projection)
            .expect("register projection");
        service
            .register_paper_engram_hash_config(hash_config)
            .expect("register hash config");
        service
            .register_paper_engram_table_shard(shard)
            .expect("register shard");
        service
            .register_paper_engram_gate(gate)
            .expect("register gate");

        assert_eq!(
            service
                .register_paper_engram_module(module)
                .expect_err("missing order=3 table operand should fail at registration"),
            LingquMemoryError::MissingField("paper_engram_runtime.table_operand")
        );
    }

    #[test]
    fn boundary_lookup_requires_matching_hidden_fingerprint() {
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
        let mut mismatched_hidden_ref = hidden_ref.clone();
        mismatched_hidden_ref.checksum ^= 1;
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
        service
            .register_execution_artifact(ExecutionArtifactObject {
                artifact_id: "artifact/logits/step3/node4".to_string(),
                kind: ExecutionArtifactKind::Logits,
                model: sample_model_binding(),
                producer_boundary: sample_range_boundary(),
                boundary_hidden_fingerprint: BoundaryTensorFingerprint::from_hot_ref(&hidden_ref),
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
            })
            .unwrap();

        let response = service
            .boundary_lookup(
                BoundaryLookupRequest {
                    request_id: "boundary/hidden-mismatch".to_string(),
                    model: sample_model_binding(),
                    boundary: sample_range_boundary(),
                    hidden_state: mismatched_hidden_ref,
                    engram_state_id: None,
                    min_confidence_milli: 900,
                    allowed_actions: vec![ShortpathAction::JumpToTerminal],
                    created_at_us: 13,
                },
                14,
            )
            .unwrap();

        assert_eq!(response.support.supported_action, ShortpathAction::Continue);
        assert_eq!(response.artifact, None);
        assert_eq!(
            response.support.reason,
            "no_verified_execution_artifact_support"
        );
    }

    #[test]
    fn boundary_lookup_requires_exact_range_boundary_identity() {
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
        service
            .register_execution_artifact(ExecutionArtifactObject {
                artifact_id: "artifact/logits/step3/node4".to_string(),
                kind: ExecutionArtifactKind::Logits,
                model: sample_model_binding(),
                producer_boundary: sample_range_boundary(),
                boundary_hidden_fingerprint: BoundaryTensorFingerprint::from_hot_ref(&hidden_ref),
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
            })
            .unwrap();

        for boundary in [
            RangeBoundary {
                step_index: 4,
                ..sample_range_boundary()
            },
            RangeBoundary {
                node_index: 5,
                ..sample_range_boundary()
            },
            RangeBoundary {
                layer_start: 0,
                ..sample_range_boundary()
            },
            RangeBoundary {
                position: 13,
                ..sample_range_boundary()
            },
        ] {
            let response = service
                .boundary_lookup(
                    BoundaryLookupRequest {
                        request_id: format!(
                            "boundary/exact-identity/step{}/node{}",
                            boundary.step_index, boundary.node_index
                        ),
                        model: sample_model_binding(),
                        boundary,
                        hidden_state: hidden_ref.clone(),
                        engram_state_id: None,
                        min_confidence_milli: 900,
                        allowed_actions: vec![ShortpathAction::JumpToTerminal],
                        created_at_us: 13,
                    },
                    14,
                )
                .unwrap();

            assert_eq!(response.support.supported_action, ShortpathAction::Continue);
            assert_eq!(response.artifact, None);
            assert_eq!(
                response.support.reason,
                "no_verified_execution_artifact_support"
            );
        }
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
            boundary_hidden_fingerprint: BoundaryTensorFingerprint {
                bytes: 16,
                checksum: 0x6666,
                dtype: TensorDType::F32,
                shape: vec![1, 4],
            },
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
        assert_eq!(
            boundary.support.supported_action,
            ShortpathAction::JumpToTerminal
        );
        assert_eq!(
            boundary.support.artifact_id.as_deref(),
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
            .persist_shortpath_supports_to_dfs(&mut restored)
            .expect("persist shortpath support audit");
        restored_service
            .persist_prefetch_plans_to_dfs(&mut restored)
            .expect("persist prefetch audit");
        restored_service
            .persist_prefix_cache_reuse_plans_to_dfs(&mut restored)
            .expect("persist prefix cache reuse audit");
        restored_service
            .persist_shortpath_supports_to_dfs(&mut restored)
            .expect("persist shortpath support audit idempotently");
        restored_service
            .persist_prefetch_plans_to_dfs(&mut restored)
            .expect("persist prefetch audit idempotently");
        restored_service
            .persist_prefix_cache_reuse_plans_to_dfs(&mut restored)
            .expect("persist prefix cache reuse audit idempotently");
        let audit_snapshot = restored
            .export_durable_sim_snapshot()
            .expect("export audit durable snapshot");
        let shortpath_support_log_records = audit_snapshot
            .dfs
            .append_logs
            .iter()
            .filter(|record| record.path == LINGQU_SHORTPATH_SUPPORT_AUDIT_LOG_PATH)
            .collect::<Vec<_>>();
        let prefetch_log_records = audit_snapshot
            .dfs
            .append_logs
            .iter()
            .filter(|record| record.path == LINGQU_PREFETCH_PLAN_AUDIT_LOG_PATH)
            .collect::<Vec<_>>();
        let prefix_reuse_log_records = audit_snapshot
            .dfs
            .append_logs
            .iter()
            .filter(|record| record.path == LINGQU_PREFIX_CACHE_REUSE_AUDIT_LOG_PATH)
            .collect::<Vec<_>>();
        assert_eq!(shortpath_support_log_records.len(), 1);
        assert_eq!(shortpath_support_log_records[0].seq, 1);
        assert_eq!(prefetch_log_records.len(), 1);
        assert_eq!(prefetch_log_records[0].seq, 1);
        assert_eq!(prefix_reuse_log_records.len(), 1);
        assert_eq!(prefix_reuse_log_records[0].seq, 1);
        assert!(audit_snapshot.dfs.files.iter().all(|record| record.path
            != LINGQU_SHORTPATH_DECISION_MANIFEST_PATH
            && record.path != LINGQU_SHORTPATH_SUPPORT_MANIFEST_PATH
            && record.path != LINGQU_PREFETCH_PLAN_MANIFEST_PATH));
        let audit_json = audit_snapshot.to_json_bytes().expect("audit json");
        let audit_decoded = durable_sim::LingquDurableSimSnapshot::from_json_bytes(&audit_json)
            .expect("decode audit durable snapshot");
        let mut audit_store = LingquMemoryDurableStore::import_durable_sim_snapshot(audit_decoded)
            .expect("import audit store");
        let mut audit_service = LingquMemoryService::new();
        audit_service
            .rebuild_shortpath_supports_from_dfs(&mut audit_store)
            .expect("rebuild shortpath support audit");
        audit_service
            .rebuild_prefetch_plans_from_dfs(&mut audit_store)
            .expect("rebuild prefetch audit");
        audit_service
            .rebuild_prefix_cache_reuse_plans_from_dfs(&mut audit_store)
            .expect("rebuild prefix cache reuse audit");
        assert!(audit_service
            .shortpath_support("shortpath-support/boundary/restart")
            .is_some());
        assert!(audit_service
            .prefetch_plan("prefetch-plan/prefetch/restart")
            .is_some());
        assert!(audit_service
            .prefix_cache_reuse_plan("prefix-cache-reuse/prefix/restart")
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
        durable
            .persist_query_result(&result)
            .expect("persist query result idempotently");
        let audit_results = durable
            .load_query_result_audit_manifest()
            .expect("load query result audit");
        assert_eq!(audit_results, vec![result.clone()]);
        let audit_snapshot = durable
            .export_durable_sim_snapshot()
            .expect("export query audit snapshot");
        let query_log_records = audit_snapshot
            .dfs
            .append_logs
            .iter()
            .filter(|record| record.path == LINGQU_QUERY_RESULT_AUDIT_LOG_PATH)
            .collect::<Vec<_>>();
        assert_eq!(query_log_records.len(), 1);
        assert_eq!(query_log_records[0].seq, 1);
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
        assert_eq!(durable.load_query_result(&result_path).unwrap(), result);
        restored
            .rebuild_query_results_from_dfs(&mut durable)
            .expect("rebuild query results from audit");

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
    fn object_service_checkpoint_dedupes_identical_payload_blocks() {
        let mut durable = LingquMemoryDurableStore::new();
        let mut object_service =
            LingquObjectServiceStub::new(LingquObjectServiceProfile::default());
        let key = "kvcache/qwen3-test/node1/layers-0-4/decode-step0";
        let payload = vec![0x44; 128];
        let checksum = checksum64(&payload) ^ 0x1234;
        for _ in 0..2 {
            object_service
                .submit_publish(
                    LingquObjectPublishReq {
                        task: None,
                        key: key.to_string(),
                        kind: LingquObjectKind::KvCacheBlock,
                        producer_entity: 1,
                        owner_entity: Some(1),
                        expected_version: None,
                        metadata: LingquObjectMetadata {
                            bytes: payload.len() as u64,
                            checksum,
                            dtype: None,
                            shape: vec![payload.len() as u64],
                            layout: None,
                            expires_at_us: None,
                        },
                        placements: vec![LingquPayloadPlacement {
                            backend: LingquPayloadBackend::ObmmShmem,
                            storage_ref: format!("{key}/payload"),
                            segment: None,
                            offset: 0,
                            bytes: payload.len() as u64,
                            checksum,
                            locality: LingquObjectLocality::DomainShared(0),
                        }],
                        payload_bytes: payload.clone(),
                    },
                    1,
                )
                .expect("publish duplicate object payload version");
        }

        durable
            .persist_object_service_checkpoint(&object_service)
            .expect("persist duplicate object checkpoint");
        durable
            .persist_object_service_checkpoint(&object_service)
            .expect("persist duplicate object checkpoint again");
        let snapshot = durable
            .export_durable_sim_snapshot()
            .expect("export durable snapshot");
        let object_payload_blocks = snapshot
            .block
            .blocks
            .iter()
            .filter(|record| record.block.0.starts_with("block/object-service/payload/"))
            .count();
        assert_eq!(object_payload_blocks, 1);
        let restored = durable
            .load_object_service_checkpoint()
            .expect("load object checkpoint");
        assert_eq!(restored.records.len(), 2);
        assert!(restored
            .records
            .iter()
            .all(|record| record.payload_bytes == payload));
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

    fn sample_paper_engram_tokenizer_projection_manifest() -> PaperEngramTokenizerProjectionManifest
    {
        let mut manifest = PaperEngramTokenizerProjectionManifest {
            projection_id: "pe-projection-0".to_string(),
            model_id: "Qwen3-0.6B".to_string(),
            tokenizer_id: "tok/qwen3-14b".to_string(),
            projection_ref: LingquDfsPath::new("/lingqu/memory/engram/tokenizer-proj.json"),
            projection_checksum: 0x1357,
            source_ref: Some("dfs://pe/tokenizer/run-0".to_string()),
            checksum: 1,
            version: 1,
            created_at_us: 8,
            expires_at_us: Some(800),
        };
        manifest.checksum = paper_engram_tokenizer_projection_manifest_checksum(&manifest);
        manifest
            .validate()
            .expect("build paper engram tokenizer projection");
        manifest
    }

    fn sample_paper_engram_hash_config_manifest() -> PaperEngramHashConfigManifest {
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        PaperEngramHashConfigManifest::new(PaperEngramHashConfigManifest {
            hash_config_id: "pe-hash-config-0".to_string(),
            model_id: "Qwen3-0.6B".to_string(),
            tokenizer_projection_id: projection.projection_id,
            tokenizer_projection_checksum: projection.projection_checksum,
            hash_config_ref: LingquDfsPath::new("/lingqu/memory/engram/hash-config.json"),
            hash_config_checksum: 0x2468,
            orders: vec![2],
            heads_per_order: 1,
            table_rows: 1024,
            table_specs: Vec::new(),
            seed: 0x1234_5678,
            algorithm: ENGRAM_HASH_ALGORITHM_VERSION.to_string(),
            source_ref: Some("dfs://pe/hash/run-0".to_string()),
            checksum: 1,
            version: 1,
            created_at_us: 9,
            expires_at_us: Some(900),
        })
        .expect("build paper engram hash config")
    }

    fn sample_paper_engram_table_shard_manifest() -> PaperEngramTableShardManifest {
        let mut manifest = PaperEngramTableShardManifest {
            shard_id: "pe-shard-0".to_string(),
            model_id: "Qwen3-0.6B".to_string(),
            layer: 3,
            order: 2,
            head: 0,
            row_start: 0,
            row_end: 1024,
            dtype: TensorDType::F32,
            shape: vec![1024, 512],
            block_payload_refs: vec![LingquBlockPayloadRef::new(
                "block/pe-shard-0",
                0,
                1024 * 512 * 4,
                0xabc,
            )],
            source_ref: Some("dfs://pe/loader/run-0".to_string()),
            checksum: 1,
            version: 1,
            created_at_us: 10,
            expires_at_us: Some(1000),
        };
        manifest.checksum = paper_engram_table_shard_manifest_checksum(&manifest);
        manifest.validate().expect("build paper engram table shard");
        manifest
    }

    fn sample_paper_engram_gate_manifest() -> PaperEngramGateManifest {
        let mut manifest = PaperEngramGateManifest {
            gate_id: "pe-gate-0".to_string(),
            model_id: "Qwen3-0.6B".to_string(),
            layer: 3,
            dtype: TensorDType::F32,
            shape: vec![4096],
            payload_ref: Some(LingquBlockPayloadRef::new(
                "block/pe-gate-0",
                0,
                4096,
                0xdef,
            )),
            source_ref: None,
            checksum: 1,
            version: 1,
            created_at_us: 11,
            expires_at_us: Some(1100),
        };
        manifest.checksum = paper_engram_gate_manifest_checksum(&manifest);
        manifest.validate().expect("build paper engram gate");
        manifest
    }

    fn sample_paper_engram_training_recipe_manifest() -> PaperEngramTrainingRecipeManifest {
        let projection = sample_paper_engram_tokenizer_projection_manifest();
        let hash_config = sample_paper_engram_hash_config_manifest();
        let mut manifest = PaperEngramTrainingRecipeManifest {
            recipe_id: "pe-recipe-0".to_string(),
            model: sample_model_binding(),
            mode: PaperEngramTrainingMode::EngramOnlyContinuedPretrain,
            base_checkpoint_checksum: 0x2026,
            tokenizer_projection_ref: projection.projection_ref,
            hash_config_ref: hash_config.hash_config_ref,
            dataset_refs: vec!["dfs://datasets/qwen3-pe-train-0".to_string()],
            objective: "next-token-loss+engram-context".to_string(),
            frozen_base_model: true,
            lora_enabled: false,
            table_init: "hash-ngram-random-normal".to_string(),
            gate_init: "zero".to_string(),
            layers: vec![3],
            orders: vec![2],
            heads_per_order: 1,
            table_rows: 1024,
            evidence_refs: vec!["dfs://runs/pe-recipe-0/config.json".to_string()],
            checksum: 1,
            version: 1,
            created_at_us: 13,
            expires_at_us: Some(1300),
        };
        manifest.checksum = paper_engram_training_recipe_manifest_checksum(&manifest);
        manifest.validate().expect("build paper engram recipe");
        manifest
    }

    fn sample_paper_engram_eval_report_manifest() -> PaperEngramEvalReportManifest {
        let mut manifest = PaperEngramEvalReportManifest {
            report_id: "pe-eval-0".to_string(),
            recipe_id: "pe-recipe-0".to_string(),
            module_id: "pe-module-0".to_string(),
            model: sample_model_binding(),
            validation_set_refs: vec!["dfs://datasets/qwen3-pe-val-0".to_string()],
            sample_count: 128,
            baseline_loss_milli: 1200,
            paper_engram_loss_milli: 1180,
            decode_policy_loss_milli: Some(1185),
            paper_engram_decode_policy_loss_milli: Some(1178),
            max_allowed_regression_milli: 5,
            output_checksum: 0x5151,
            zero_table_hidden_checksum: Some(0x4141),
            paper_engram_hidden_checksum: Some(0x4242),
            zero_table_output_checksum: Some(0x5050),
            cpu_backend_output_match: Some(true),
            row_prefetch_requests: Some(8),
            row_prefetch_hits: Some(8),
            runtime_context_steps_expected: Some(1),
            runtime_context_steps_observed: Some(1),
            max_backend_latency_us: Some(100),
            max_allowed_backend_latency_us: Some(1000),
            evidence_refs: vec![
                "dfs://runs/pe-eval-0/report.json".to_string(),
                "w5-summary:base:dfs://runs/pe-eval-0/base_summary.txt".to_string(),
                "w5-summary:base_decode_policy:dfs://runs/pe-eval-0/decode_policy_summary.txt"
                    .to_string(),
                "w5-summary:paper_engram:dfs://runs/pe-eval-0/paper_engram_summary.txt"
                    .to_string(),
                "w5-summary:paper_engram_decode_policy:dfs://runs/pe-eval-0/paper_engram_decode_policy_summary.txt".to_string(),
            ],
            checksum: 1,
            version: 1,
            created_at_us: 14,
            expires_at_us: Some(1400),
        };
        manifest.checksum = paper_engram_eval_report_manifest_checksum(&manifest);
        manifest.validate().expect("build paper engram eval report");
        manifest
    }

    fn sample_paper_engram_module_manifest() -> PaperEngramModuleManifest {
        let mut manifest = PaperEngramModuleManifest {
            module_id: "pe-module-0".to_string(),
            module_name: "Qwen3-14B-PE".to_string(),
            model: sample_model_binding(),
            base_checkpoint_checksum: 0x2026,
            tokenizer_id: "tok/qwen3-14b".to_string(),
            tokenizer_projection_ref: LingquDfsPath::new(
                "/lingqu/memory/engram/tokenizer-proj.json",
            ),
            hash_config_ref: LingquDfsPath::new("/lingqu/memory/engram/hash-config.json"),
            table_shard_ids: vec!["pe-shard-0".to_string()],
            gate_ids: vec!["pe-gate-0".to_string()],
            layers: vec![3],
            orders: vec![2],
            heads_per_order: 1,
            hidden_size: 4096,
            memory_dim: 512,
            table_dtype: TensorDType::F32,
            table_layout: "squad".to_string(),
            gate_kind: "context".to_string(),
            training_recipe_ref: None,
            eval_report_ref: None,
            quality_claim: PaperEngramQualityClaim::None,
            payload_checksums: vec![0x1111, 0x2222],
            checksum: 1,
            version: 1,
            created_at_us: 12,
            expires_at_us: Some(1200),
        };
        manifest.checksum = paper_engram_module_manifest_checksum(&manifest);
        manifest.validate().expect("build paper engram module");
        manifest
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

    fn sample_boundary_hidden_fingerprint() -> BoundaryTensorFingerprint {
        BoundaryTensorFingerprint {
            bytes: 16,
            checksum: 0x4444,
            dtype: TensorDType::F32,
            shape: vec![1, 4],
        }
    }

    fn sample_logits_execution_artifact(artifact_id: &str) -> ExecutionArtifactObject {
        ExecutionArtifactObject {
            artifact_id: artifact_id.to_string(),
            kind: ExecutionArtifactKind::Logits,
            model: sample_model_binding(),
            producer_boundary: sample_range_boundary(),
            boundary_hidden_fingerprint: sample_boundary_hidden_fingerprint(),
            target_layer_start: 8,
            target_layer_end: 8,
            dtype: TensorDType::F32,
            shape: vec![1, 128],
            durable_payload_ref: None,
            hot_object_ref: Some(HotTensorObjectRef {
                object_key: format!("hot/{artifact_id}"),
                version: 1,
                backend: HotObjectBackend::ObmmShmem,
                storage_ref: format!("obmm://hot/{artifact_id}"),
                segment: None,
                offset: 0,
                bytes: 512,
                checksum: 0x9999,
                dtype: TensorDType::F32,
                shape: vec![1, 128],
            }),
            source_query_result_id: None,
            source_engram_state_id: None,
            confidence_milli: 980,
            state: ExecutionArtifactState::Verified,
            checksum: 0x8888,
            version: 1,
            created_at_us: 10,
            expires_at_us: Some(100),
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
