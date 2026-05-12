//! Guest-visible UAPI surface placeholders.

use std::cell::RefCell;
use std::collections::{BTreeMap, BTreeSet};
use std::collections::{HashMap, VecDeque};
#[cfg(all(unix, not(test)))]
use std::fs::OpenOptions;
use std::io::Write;
#[cfg(all(unix, not(test)))]
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

use sim_config::ScenarioConfig;
use sim_core::{
    BackendDispatchOperation, BackendExecutionRequest, BinaryArtifactRef, BlockHash, BufferUsage,
    CmdQueueHandle, CompletionEvent, CompletionSource, CompletionStatus, CqHandle,
    DispatchBackendProfile, DispatchBackendSpec, DispatchBufferBinding, DispatchLaunchParams,
    DispatchRuntimeVariant, EntityId, ExecutionContextRef, ExecutionLifecycle, FunctionLabel,
    HealthStatus, HierarchyCoord, IoOpcode, IoSubmitReq, LogicalSystemId, MemoryEndpoint, PlLevel,
    RequestCorrelation, SegmentHandle, SimError, SimplerKernelArtifact, SimplerRuntimeArg,
    SimplerRuntimeArtifacts, TaskKey, TensorDType, TensorLayout,
};
use sim_models::qwen3_dense_0_6b::{
    self, checksum_words, embedding_reference_hidden_sequence_with_payloads,
    embedding_reference_last_hidden, embedding_reference_last_hidden_with_payloads,
    embedding_reference_summary, forward_from_token_ids,
    forward_from_token_ids_with_layer_payloads, forward_incremental_with_kv_cache_from_hidden,
    forward_with_kv_cache_from_token_ids, full_vocab_logits_from_hidden,
    full_vocab_logits_from_hidden_with_payloads, layer_forward_reference_sequence_with_payloads,
    load_safetensors_path_metadata, load_tokenizer_asset_summary, logits_reference_summary,
    logits_reference_summary_with_hidden, materialize_full_weight_tensor_payload,
    materialize_weight_slice_payload, mlp_reference_layer_summary,
    mlp_reference_layer_summary_with_hidden, prompt_token_ids_checksum,
    qkv_reference_layer_summary, qkv_reference_layer_values,
    qkv_reference_layer_values_with_hidden, token_piece_bytes_from_policy,
    token_piece_bytes_from_tokenizer_path, token_piece_decode_bytes, token_piece_from_policy,
    tokenize_prompt_from_tokenizer_path, tokenizer_policy, weight_bytes_checksum,
    weight_manifest_from_metadata, Qwen3Dense06bEmbeddingReferenceSummary,
    Qwen3Dense06bFullVocabLogitsSummary, Qwen3Dense06bLayerKvCache, Qwen3Dense06bLoadedWeights,
    Qwen3Dense06bLogitsReferenceSummary, Qwen3Dense06bMlpReferenceLayerSummary,
    Qwen3Dense06bMlpReferenceShardSummary, Qwen3Dense06bProfile,
    Qwen3Dense06bQkvReferenceLayerSummary, Qwen3Dense06bQkvReferenceLayerValues,
    Qwen3Dense06bQkvReferenceShardSummary, Qwen3Dense06bQkvReferenceShardValues,
    Qwen3Dense06bReferenceWeightSliceValidation, Qwen3Dense06bShard,
    Qwen3Dense06bTokenizerAssetSummary, Qwen3Dense06bWeightTensorKind, QWEN3_DENSE_0_6B_PROFILE,
    QWEN3_DENSE_0_6B_TOKENIZER_ASSET_POLICY_KIND, QWEN3_DENSE_0_6B_TOKENIZER_POLICY_KIND,
};
use sim_runtime::{
    LocalRuntimeEngine, RuntimeCompletionTracker, RuntimeDriveAction, RuntimeQueueRecord,
    RuntimeWorkItem, RuntimeWorkKind, SharedRuntimeExecutor, VecEventSink,
};
use sim_services::{
    block::{BlockServiceProfile, BlockServiceStub},
    db::{DbGetReq, DbPutReq, DbServiceProfile, DbServiceStub},
    dfs::{DfsReadReq, DfsServiceProfile, DfsServiceStub, DfsWriteReq},
    object::{
        LingquObjectAppendReq, LingquObjectKind, LingquObjectLocality, LingquObjectMetadata,
        LingquObjectPublishReq, LingquObjectResolveReq, LingquObjectServiceProfile,
        LingquObjectServiceReport, LingquObjectServiceStub, LingquObjectState,
        LingquObjectVersionSelector, LingquPayloadBackend, LingquPayloadPlacement,
    },
    shmem::{ShmemGetReq, ShmemPutReq, ShmemServiceProfile, ShmemServiceStub},
    weights::{
        ServiceObjectKind, ServiceObjectMetadataPut, ServiceObjectPayloadWrite,
        ServiceObjectPublishReq, ServiceObjectResolveReq, WeightStorageKind, WeightsServiceStub,
    },
};
use sim_topology::{SimTopology, TopologySnapshot};

#[derive(Debug, Clone)]
pub enum UapiDescriptor {
    Io(IoSubmitReq),
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
    ObjectPublish(LingquObjectPublishReq),
    ObjectResolve(LingquObjectResolveReq),
    ObjectAppend(LingquObjectAppendReq),
}

#[derive(Debug, Clone)]
pub enum UapiCommand {
    QueryTopology,
    CreateSegment {
        bytes: u64,
    },
    RegisterCq {
        owner: EntityId,
    },
    CreateCmdQueue {
        cq: CqHandle,
        owner: EntityId,
        depth: usize,
    },
    EnqueueCmd {
        cmdq: CmdQueueHandle,
        owner: EntityId,
        desc: UapiDescriptor,
    },
    RingDoorbell {
        cmdq: CmdQueueHandle,
        owner: EntityId,
        max_batch: Option<usize>,
    },
    SubmitIo {
        req: IoSubmitReq,
    },
    SubmitBlockWriteback {
        block: BlockHash,
        task: Option<TaskKey>,
    },
    SubmitShmemPut {
        req: ShmemPutReq,
    },
    SubmitShmemGet {
        req: ShmemGetReq,
    },
    SubmitDfsRead {
        req: DfsReadReq,
    },
    SubmitDfsWrite {
        req: DfsWriteReq,
    },
    SubmitDbPut {
        req: DbPutReq,
    },
    SubmitDbGet {
        req: DbGetReq,
    },
    SubmitObjectPublish {
        req: LingquObjectPublishReq,
    },
    SubmitObjectResolve {
        req: LingquObjectResolveReq,
    },
    SubmitObjectAppend {
        req: LingquObjectAppendReq,
    },
    PollCq {
        cq: CqHandle,
        owner: EntityId,
        max_entries: Option<usize>,
    },
    DrainCq {
        cq: CqHandle,
        owner: EntityId,
    },
    GetHealth {
        entity: EntityId,
    },
}

#[derive(Debug, Clone)]
pub enum UapiResponse {
    TopologySnapshot(TopologySnapshot),
    SegmentCreated(SegmentHandle),
    CqRegistered(CqHandle),
    CmdQueueCreated(CmdQueueHandle),
    IoSubmitted(u64),
    CommandEnqueued {
        depth: usize,
        remaining_capacity: usize,
    },
    DoorbellRung {
        submitted: usize,
        pending: usize,
    },
    Completions {
        events: Vec<CompletionEvent>,
        remaining: usize,
    },
    HealthStatus(HealthStatus),
}

pub trait GuestUapiSurface {
    fn query_topology(&self) -> TopologySnapshot;
    fn create_segment(&mut self, bytes: u64) -> Result<SegmentHandle, SimError>;
    fn register_cq(&mut self) -> Result<CqHandle, SimError>;
    fn submit_io(&mut self, req: IoSubmitReq) -> Result<u64, SimError>;
    fn poll_cq(&self, cq: CqHandle) -> Vec<CompletionEvent>;
    fn get_health(&self, entity: EntityId) -> Result<HealthStatus, SimError>;
}

#[derive(Debug)]
pub struct LocalGuestUapiSurface {
    topology: SimTopology,
    block_service: BlockServiceStub,
    shmem_service: ShmemServiceStub,
    dfs_service: DfsServiceStub,
    db_service: DbServiceStub,
    object_service: LingquObjectServiceStub,
    segment_payloads: HashMap<SegmentHandle, Vec<u8>>,
    block_payloads: HashMap<BlockHash, Vec<u8>>,
    next_segment_id: u64,
    next_cq_id: u32,
    next_cmdq_id: u32,
    service_clock: u64,
    runtime_issue_latency_us: u64,
    runtime_retry_delay_us: u64,
    runtime_queue_depth: usize,
    runtime_max_retries: u32,
    cq_events: HashMap<CqHandle, CompletionQueueState>,
    cmd_queues: HashMap<CmdQueueHandle, CommandQueueState>,
    runtime_queue: SharedRuntimeExecutor<RuntimeWorkItem<UapiRuntimePayload>>,
    completion_routes: RuntimeCompletionTracker<CqHandle>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bTextOutputReport {
    pub token_count: u64,
    pub byte_len: u64,
    pub padded_byte_len: u64,
    pub byte_checksum: u64,
    pub sequence_checksum: u64,
    pub token_checksum: u64,
    pub text_checksum: u64,
    pub logits_checksum: u64,
    pub tokenizer_policy_kind: u64,
    pub guest_input: Qwen3Dense06bGuestInputReport,
    pub kvcache: Qwen3Dense06bKvCacheReport,
    pub attention: Qwen3Dense06bAttentionReport,
    pub post_attention: Qwen3Dense06bPostAttentionReport,
    pub result_flow: Qwen3Dense06bResultFlowReport,
    pub real_qkv: Option<Qwen3Dense06bQkvReferenceReport>,
    pub real_mlp: Option<Qwen3Dense06bMlpReferenceReport>,
    pub real_logits: Option<Qwen3Dense06bLogitsReferenceReport>,
    pub real_inference: Option<Qwen3Dense06bRealInferenceReferenceReport>,
    pub synthetic: Qwen3Dense06bSyntheticStageReport,
    pub samples: Vec<Qwen3Dense06bTextOutputSample>,
    pub bytes: Vec<u8>,
    pub text_lossy: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bGuestInputReport {
    pub byte_len: u64,
    pub checksum: u64,
    pub prompt_byte_len: u64,
    pub prompt_checksum: u64,
    pub prompt_token_count: u64,
    pub prompt_token_checksum: u64,
    pub tokenizer_asset_checksum: u64,
    pub real_backed: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bSyntheticStageReport {
    pub stage_count: u64,
    pub stage_mask: u64,
    pub stage_checksum: u64,
    pub qkv_base_tile_real_backed: bool,
    pub attention_score_real_backed: bool,
    pub attention_context_real_backed: bool,
    pub mlp_activation_real_backed: bool,
    pub mlp_output_real_backed: bool,
    pub logits_candidates_real_backed: bool,
    pub token_text_real_backed: bool,
    pub guest_input_real_backed: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bAttentionReport {
    pub score_count: u64,
    pub softmax_count: u64,
    pub context_count: u64,
    pub stage_mask: u64,
    pub score_checksum: u64,
    pub softmax_checksum: u64,
    pub context_checksum: u64,
    pub aggregate_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bPostAttentionReport {
    pub mlp_activation_count: u64,
    pub host_partial_count: u64,
    pub mlp_output_count: u64,
    pub residual_norm_count: u64,
    pub next_partial_count: u64,
    pub stage_mask: u64,
    pub mlp_activation_checksum: u64,
    pub host_partial_checksum: u64,
    pub mlp_output_checksum: u64,
    pub residual_norm_checksum: u64,
    pub next_partial_checksum: u64,
    pub aggregate_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bResultFlowReport {
    pub publish_count: u64,
    pub resolve_count: u64,
    pub round1_compute_count: u64,
    pub result_count: u64,
    pub round0_distinct_count: u64,
    pub round1_distinct_count: u64,
    pub round0_checksum: u64,
    pub round1_checksum: u64,
    pub aggregate_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bMlpReferenceReport {
    pub layer_id: u64,
    pub next_layer_id: u64,
    pub shard_count: u64,
    pub next_shard_count: u64,
    pub total_weight_bytes: u64,
    pub next_total_weight_bytes: u64,
    pub total_intermediate_rows: u64,
    pub next_total_intermediate_rows: u64,
    pub aggregate_checksum: u64,
    pub next_aggregate_checksum: u64,
    pub real_weight_checksum: u64,
    pub real_activation_checksum: u64,
    pub real_output_checksum: u64,
    pub next_real_output_checksum: u64,
    pub sample_checksum: u64,
    pub table_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bQkvReferenceReport {
    pub layer_id: u64,
    pub reference_layer_count: u64,
    pub shard_count: u64,
    pub stage_link_count: u64,
    pub stage_kind_mask: u64,
    pub total_weight_bytes: u64,
    pub aggregate_checksum: u64,
    pub qkv_rows: u64,
    pub stage_link_checksum: u64,
    pub synthetic_checksum: u64,
    pub real_weight_checksum: u64,
    pub real_value_checksum: u64,
    pub real_output_checksum: u64,
    pub reference_layer_checksum: u64,
    pub next_reference_layer_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bLogitsReferenceReport {
    pub token_count: u64,
    pub candidate_count: u64,
    pub distinct_step_count: u64,
    pub distinct_token_count: u64,
    pub row_byte_count: u64,
    pub row_checksum: u64,
    pub logit_checksum: u64,
    pub aggregate_checksum: u64,
    pub final_norm_checksum: u64,
    pub vocab_size: u64,
    pub hidden_size: u64,
    pub sampled_pair_count: u64,
    pub selection_match_count: u64,
    pub margin_match_count: u64,
    pub checksum_match_count: u64,
    pub max_margin_delta_milli: u64,
    pub top_logit_bits_checksum: u64,
    pub runner_logit_bits_checksum: u64,
    pub comparison_checksum: u64,
    pub selection_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bRealInferenceReferenceReport {
    pub prompt_token_count: u64,
    pub prompt_token_checksum: u64,
    pub layer_count: u64,
    pub final_hidden_checksum: u64,
    pub forward_checksum: u64,
    pub full_vocab_checked_token_count: u64,
    pub full_vocab_logits_checksum: u64,
    pub sampled_token: u64,
    pub sampled_text_byte_len: u64,
    pub sampled_text_byte_checksum: u64,
    pub output_sample_count: u64,
    pub sampled_token_match_count: u64,
    pub sampled_text_match_count: u64,
    pub aggregate_checksum: u64,
}

const QWEN3_SYNTHETIC_GUEST_INPUT: u64 = 1 << 0;
const QWEN3_SYNTHETIC_TOKEN_TEXT: u64 = 1 << 1;
const QWEN3_SYNTHETIC_QKV_BASE_TILE: u64 = 1 << 2;
const QWEN3_SYNTHETIC_ATTENTION_SCORE: u64 = 1 << 3;
const QWEN3_SYNTHETIC_ATTENTION_CONTEXT: u64 = 1 << 4;
const QWEN3_SYNTHETIC_MLP_ACTIVATION: u64 = 1 << 5;
const QWEN3_SYNTHETIC_MLP_OUTPUT: u64 = 1 << 6;
const QWEN3_SYNTHETIC_LOGITS_CANDIDATES: u64 = 1 << 7;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bKvCacheReport {
    pub descriptor_count: u64,
    pub state_count: u64,
    pub append_block_count: u64,
    pub update_seq_sum: u64,
    pub prefill_entry_count: u64,
    pub decode_entry_count: u64,
    pub read_window_end_max: u64,
    pub read_digest_checksum: u64,
    pub state_snapshots: Vec<Qwen3Dense06bKvCacheStateSnapshot>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bKvCacheStateSnapshot {
    pub layer_id: u64,
    pub tile_id: u64,
    pub position: u64,
    pub update_seq: u64,
    pub k_checksum: u64,
    pub v_checksum: u64,
    pub read_window_end: u64,
    pub read_digest: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bTextOutputSample {
    pub step_index: u64,
    pub shard_id: u64,
    pub tile_id: u64,
    pub logits_count: u64,
    pub sampled_token: u64,
    pub runner_up_token: u64,
    pub margin_milli: u64,
    pub logits_checksum: u64,
    pub full_vocab_checked_token_count: u64,
    pub full_vocab_logits_checksum: u64,
    pub top_logit_bits: u64,
    pub runner_up_logit_bits: u64,
    pub runtime_forward_layer_count: u64,
    pub runtime_forward_final_hidden_checksum: u64,
    pub runtime_forward_checksum: u64,
    pub kvcache_read_digest: u64,
    pub qkv_reference_digest: u64,
    pub real_path_digest: u64,
    pub text_checksum: u64,
    pub text_byte_offset: u64,
    pub byte_len: u64,
    pub boundary_flags: u64,
    pub piece_lossy: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bDecodeLoopReport {
    pub steps: Vec<Qwen3Dense06bDecodeLoopStepReport>,
    pub final_guest_input_checksum: u64,
    pub decode_chain_checksum: u64,
    pub generated_byte_len: u64,
    pub generated_byte_checksum: u64,
    pub generated_text_lossy: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bDecodeLoopStepReport {
    pub step_index: u64,
    pub runtime_prefill_executed: bool,
    pub guest_input_checksum: u64,
    pub next_guest_input_checksum: u64,
    pub input_transition: Qwen3Dense06bDecodeInputTransitionReport,
    pub layer_progress: Qwen3Dense06bDecodeLayerProgressReport,
    pub hidden_layer_pipeline: Qwen3Dense06bHiddenLayerPipelineReport,
    pub object_service: Qwen3Dense06bObjectServiceReport,
    pub real_inference_contract: Qwen3Dense06bRealInferenceContractReport,
    pub sampled_token_count: u64,
    pub text_output: Qwen3Dense06bTextOutputReport,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bRangeForwardReport {
    pub prompt_token_count: u64,
    pub prompt_token_checksum: u64,
    pub node_count: u64,
    pub layer_count: u64,
    pub ready: bool,
    pub weight_object_count: u64,
    pub global_weight_object_count: u64,
    pub hidden_object_count: u64,
    pub handoff_match_count: u64,
    pub aggregate_checksum: u64,
    pub workers: Vec<Qwen3Dense06bRangeForwardWorkerReport>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bRangeForwardWorkerReport {
    pub node_id: u64,
    pub first_layer_id: u64,
    pub last_layer_id: u64,
    pub layer_count: u64,
    pub input_key: String,
    pub output_key: String,
    pub weight_key: String,
    pub input_payload_bytes: u64,
    pub output_payload_bytes: u64,
    pub input_payload_checksum: u64,
    pub output_payload_checksum: u64,
    pub first_layer_input_checksum: u64,
    pub last_layer_output_checksum: u64,
    pub weight_payload_bytes: u64,
    pub weight_payload_slice_count: u64,
    pub weight_reconstructed_tensor_count: u64,
    pub handoff_input_matches_previous_output: bool,
    pub aggregate_checksum: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bObjectServiceReport {
    pub ready: bool,
    pub publish_count: u64,
    pub resolve_count: u64,
    pub append_count: u64,
    pub kv_index_resolve_count: u64,
    pub kv_index_append_count: u64,
    pub metadata_put_count: u64,
    pub metadata_get_count: u64,
    pub shmem_write_count: u64,
    pub shmem_read_count: u64,
    pub block_write_count: u64,
    pub block_read_count: u64,
    pub inline_write_count: u64,
    pub inline_read_count: u64,
    pub obmm_pool_enabled: bool,
    pub obmm_pool_payload_write_count: u64,
    pub obmm_pool_payload_read_count: u64,
    pub obmm_pool_queue_submit_count: u64,
    pub obmm_pool_queue_deliver_count: u64,
    pub obmm_pool_bytes_used: u64,
    pub committed_object_count: u64,
    pub missing_resolve_count: u64,
    pub token_objects: u64,
    pub kv_objects: u64,
    pub weight_objects: u64,
    pub weight_payload_bytes: u64,
    pub weight_payload_slice_count: u64,
    pub weight_payload_complete: bool,
    pub weight_reconstructed_tensor_count: u64,
    pub weight_reconstructed_tensor_checksum: u64,
    pub weight_payload_checksum: u64,
    pub global_weight_object_count: u64,
    pub global_weight_payload_bytes: u64,
    pub global_weight_tensor_count: u64,
    pub global_weight_payload_checksum: u64,
    pub runtime_tensor_objects: u64,
    pub logits_objects: u64,
    pub object_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bRealInferenceContractReport {
    pub ready: bool,
    pub blocker_count: u64,
    pub synthetic_stage_count: u64,
    pub synthetic_stage_mask: u64,
    pub uses_candidate_logits_only: bool,
    pub uses_deterministic_hidden: bool,
    pub uses_embedding_hidden_as_final_hidden: bool,
    pub uses_round1_output_hidden_for_logits: bool,
    pub full_forward_math: bool,
    pub full_vocab_logits: bool,
    pub sampled_text_reference_checked: bool,
    pub aggregate_checksum: u64,
    pub blockers: Vec<String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bDecodeLayerProgressReport {
    pub first_layer_id: u64,
    pub next_layer_id: u64,
    pub qkv_reference_layer_count: u64,
    pub qkv_stage_link_count: u64,
    pub full_layer_path_count: u64,
    pub full_layer_path_real_backed: bool,
    pub full_layer_path_checksum: u64,
    pub full_layer_final_checksum: u64,
    pub layer0_path_checksum: u64,
    pub layer1_path_checksum: u64,
    pub logits_path_checksum: u64,
    pub aggregate_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bHiddenLayerPipelineReport {
    pub layer_count: u64,
    pub node_count: u64,
    pub input_embedding_real_backed: bool,
    pub input_embedding_token_count: u64,
    pub input_embedding_row_byte_count: u64,
    pub input_embedding_row_checksum: u64,
    pub input_embedding_value_checksum: u64,
    pub input_embedding_checksum: u64,
    pub hidden_tensor_byte_count: u64,
    pub hidden_tensor_carry_count: u64,
    pub hidden_tensor_carry_checksum: u64,
    pub hidden_tensor_carry_all_present: bool,
    pub hidden_tensor_real_reference_count: u64,
    pub hidden_tensor_real_reference_checksum: u64,
    pub hidden_tensor_real_references_all_present: bool,
    pub real_qkv_layer_count: u64,
    pub real_qkv_layer_checksum: u64,
    pub real_qkv_all_layers_present: bool,
    pub real_mlp_layer_count: u64,
    pub real_mlp_layer_checksum: u64,
    pub real_mlp_all_layers_present: bool,
    pub real_layer_execution_count: u64,
    pub real_layer_execution_checksum: u64,
    pub real_layer_executions_all_present: bool,
    pub transition_count: u64,
    pub boundary_count: u64,
    pub local_transition_count: u64,
    pub min_layers_per_node: u64,
    pub max_layers_per_node: u64,
    pub balanced_layer_spread: bool,
    pub first_layer_id: u64,
    pub last_layer_id: u64,
    pub first_node_id: u64,
    pub last_node_id: u64,
    pub node_range_checksum: u64,
    pub layer_assignment_checksum: u64,
    pub boundary_checksum: u64,
    pub final_layer_checksum: u64,
    pub aggregate_checksum: u64,
    pub node_ranges: Vec<Qwen3Dense06bHiddenLayerNodeRange>,
    pub layer_executions: Vec<Qwen3Dense06bHiddenLayerExecution>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bHiddenLayerNodeRange {
    pub node_id: u64,
    pub first_layer_id: u64,
    pub last_layer_id: u64,
    pub layer_count: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bHiddenLayerExecution {
    pub layer_id: u64,
    pub owner_node: u64,
    pub input_checksum: u64,
    pub qkv_checksum: u64,
    pub mlp_checksum: u64,
    pub output_checksum: u64,
    pub input_tensor_checksum: u64,
    pub output_tensor_checksum: u64,
    pub real_reference_tensor_checksum: u64,
    pub starts_node_range: bool,
    pub input_tensor_payload: Vec<u8>,
    pub output_tensor_payload: Vec<u8>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bDecodeInputTransitionReport {
    pub loop_step: u64,
    pub write_count: u64,
    pub applied_write_count: u64,
    pub write_readback_match_count: u64,
    pub write_offset_checksum: u64,
    pub sampled_token_checksum: u64,
    pub readback_token_checksum: u64,
    pub logits_checksum: u64,
    pub text_checksum: u64,
    pub checksum_slot_value: u64,
    pub transition_checksum: u64,
}

#[derive(Debug)]
struct CompletionQueueState {
    owner: EntityId,
    events: VecDeque<CompletionEvent>,
}

#[derive(Debug)]
struct CommandQueueState {
    cq: CqHandle,
    owner: EntityId,
    depth: usize,
    pending: VecDeque<UapiDescriptor>,
}

#[derive(Debug, Clone)]
struct UapiRuntimePayload {
    cq: CqHandle,
    desc: UapiDescriptor,
}

#[derive(Debug, Clone, Copy)]
struct UapiRuntimeFailure {
    cq: CqHandle,
    code: &'static str,
}

#[derive(Debug, Clone, serde::Deserialize)]
struct SimplerRuntimeManifestEnvelope {
    simpler_runtime: SimplerRuntimeManifest,
}

#[derive(Debug, Clone, serde::Deserialize)]
struct SimplerRuntimeManifest {
    host_runtime_library: BinaryArtifactRef,
    orch_shared_object: BinaryArtifactRef,
    orch_function_name: String,
    aicpu_binary: Option<BinaryArtifactRef>,
    aicore_binary: Option<BinaryArtifactRef>,
    kernels: Vec<SimplerKernelArtifact>,
    launch: DispatchLaunchParams,
    #[serde(default)]
    runtime_env: BTreeMap<String, String>,
}

impl LocalGuestUapiSurface {
    pub fn new(topology: SimTopology) -> Self {
        Self::with_profiles_and_runtime_policy(
            topology,
            BlockServiceProfile::default(),
            ShmemServiceProfile::default(),
            DfsServiceProfile::default(),
            DbServiceProfile::default(),
            4,
            5,
            32,
            1,
        )
    }

    pub fn with_profiles_and_runtime_policy(
        topology: SimTopology,
        block_profile: BlockServiceProfile,
        shmem_profile: ShmemServiceProfile,
        dfs_profile: DfsServiceProfile,
        db_profile: DbServiceProfile,
        runtime_issue_latency_us: u64,
        runtime_retry_delay_us: u64,
        runtime_queue_depth: usize,
        runtime_max_retries: u32,
    ) -> Self {
        Self::with_service_profiles(
            topology,
            block_profile,
            shmem_profile,
            dfs_profile,
            db_profile,
        )
        .with_runtime_policy(
            runtime_issue_latency_us,
            runtime_retry_delay_us,
            runtime_queue_depth,
            runtime_max_retries,
        )
    }

    pub fn with_block_profile(topology: SimTopology, profile: BlockServiceProfile) -> Self {
        Self::with_service_profiles(
            topology,
            profile,
            ShmemServiceProfile::default(),
            DfsServiceProfile::default(),
            DbServiceProfile::default(),
        )
    }

    pub fn with_service_profiles(
        topology: SimTopology,
        block_profile: BlockServiceProfile,
        shmem_profile: ShmemServiceProfile,
        dfs_profile: DfsServiceProfile,
        db_profile: DbServiceProfile,
    ) -> Self {
        Self {
            topology,
            block_service: BlockServiceStub::with_profile(block_profile),
            shmem_service: ShmemServiceStub::new(shmem_profile),
            dfs_service: DfsServiceStub::new(dfs_profile),
            db_service: DbServiceStub::new(db_profile),
            object_service: LingquObjectServiceStub::new(LingquObjectServiceProfile::default()),
            segment_payloads: HashMap::new(),
            block_payloads: HashMap::new(),
            next_segment_id: 0,
            next_cq_id: 0,
            next_cmdq_id: 0,
            service_clock: 0,
            runtime_issue_latency_us: 4,
            runtime_retry_delay_us: 5,
            runtime_queue_depth: 32,
            runtime_max_retries: 1,
            cq_events: HashMap::new(),
            cmd_queues: HashMap::new(),
            runtime_queue: SharedRuntimeExecutor::with_policy(4, 5, 32, 1),
            completion_routes: RuntimeCompletionTracker::default(),
        }
    }

    pub fn with_runtime_policy(
        mut self,
        runtime_issue_latency_us: u64,
        runtime_retry_delay_us: u64,
        runtime_queue_depth: usize,
        runtime_max_retries: u32,
    ) -> Self {
        self.runtime_issue_latency_us = runtime_issue_latency_us;
        self.runtime_retry_delay_us = runtime_retry_delay_us;
        self.runtime_queue_depth = runtime_queue_depth;
        self.runtime_max_retries = runtime_max_retries;
        self.runtime_queue = SharedRuntimeExecutor::with_policy(
            runtime_issue_latency_us,
            runtime_retry_delay_us,
            runtime_queue_depth,
            runtime_max_retries,
        );
        self
    }

    pub fn write_segment_payload(
        &mut self,
        segment: SegmentHandle,
        offset: usize,
        bytes: &[u8],
    ) -> Result<(), SimError> {
        let payload = self
            .segment_payloads
            .get_mut(&segment)
            .ok_or(SimError::NotFound("segment payload"))?;
        let end = offset
            .checked_add(bytes.len())
            .ok_or(SimError::InvalidInput("segment payload range overflow"))?;
        if end > payload.len() {
            return Err(SimError::InvalidInput(
                "segment payload range out of bounds",
            ));
        }
        payload[offset..end].copy_from_slice(bytes);
        Ok(())
    }

    pub fn read_segment_payload(
        &self,
        segment: SegmentHandle,
        offset: usize,
        out: &mut [u8],
    ) -> Result<(), SimError> {
        let payload = self
            .segment_payloads
            .get(&segment)
            .ok_or(SimError::NotFound("segment payload"))?;
        let end = offset
            .checked_add(out.len())
            .ok_or(SimError::InvalidInput("segment payload range overflow"))?;
        if end > payload.len() {
            return Err(SimError::InvalidInput(
                "segment payload range out of bounds",
            ));
        }
        out.copy_from_slice(&payload[offset..end]);
        Ok(())
    }

    fn default_cq(&self) -> Result<CqHandle, SimError> {
        self.cq_events
            .keys()
            .next()
            .copied()
            .ok_or(SimError::NotFound("completion queue"))
    }

    fn enqueue_to_cq(&mut self, cq: CqHandle, event: CompletionEvent) -> Result<(), SimError> {
        let queue = self
            .cq_events
            .get_mut(&cq)
            .ok_or(SimError::NotFound("completion queue"))?;
        queue.events.push_back(event);
        Ok(())
    }

    pub fn drain_cq(&mut self, cq: CqHandle) -> Vec<CompletionEvent> {
        self.poll_cq_with_owner(cq, 0, None)
            .map(|(events, _)| events)
            .unwrap_or_default()
    }

    fn stage_runtime_descriptor(
        &mut self,
        cq: CqHandle,
        desc: UapiDescriptor,
    ) -> Result<(), SimError> {
        let now = self.next_service_time();
        let kind = runtime_kind_for_descriptor(&desc);
        let task = runtime_task_for_descriptor(&desc);
        self.runtime_queue
            .enqueue(
                RuntimeWorkItem {
                    op_id: now,
                    kind,
                    task,
                    payload: UapiRuntimePayload { cq, desc },
                },
                now,
            )
            .map_err(|err| match err {
                SimError::InvalidInput("runtime queue full") => {
                    SimError::InvalidInput("uapi runtime queue full")
                }
                other => other,
            })
    }

    fn flush_services(&mut self, now: u64) -> Result<(), SimError> {
        for event in self.block_service.poll_ready(now) {
            self.route_completion_to_cq(event)?;
        }
        for event in self.shmem_service.poll_ready(now) {
            self.route_completion_to_cq(event)?;
        }
        for event in self.dfs_service.poll_ready(now) {
            self.route_completion_to_cq(event)?;
        }
        for event in self.db_service.poll_ready(now) {
            self.route_completion_to_cq(event)?;
        }
        for event in self.object_service.poll_ready(now) {
            self.route_completion_to_cq(event)?;
        }
        Ok(())
    }

    fn flush_runtime(&mut self, now: u64) -> Result<(), SimError> {
        let mut runtime_queue = std::mem::replace(
            &mut self.runtime_queue,
            SharedRuntimeExecutor::with_policy(
                self.runtime_issue_latency_us,
                self.runtime_retry_delay_us,
                self.runtime_queue_depth,
                self.runtime_max_retries,
            ),
        );

        let (failures, force_flush) = runtime_queue.drive_ready(now, |entry| {
            let RuntimeQueueRecord {
                payload:
                    RuntimeWorkItem {
                        payload: UapiRuntimePayload { cq, desc },
                        ..
                    },
                ..
            } = entry;
            match self.submit_descriptor_to_cq(desc.clone(), *cq) {
                Ok(_) => RuntimeDriveAction::Complete,
                Err(SimError::InvalidInput(code))
                    if matches!(
                        code.as_ref(),
                        "block queue full"
                            | "shmem queue full"
                            | "dfs queue full"
                            | "db queue full"
                    ) =>
                {
                    if now == u64::MAX {
                        let _ = self.flush_services(now);
                    }
                    RuntimeDriveAction::Retry(UapiRuntimeFailure { cq: *cq, code })
                }
                Err(err) => RuntimeDriveAction::Fail(UapiRuntimeFailure {
                    cq: *cq,
                    code: match err {
                        SimError::InvalidInput(code) => code,
                        _ => "runtime_issue_failed",
                    },
                }),
            }
        });
        self.runtime_queue = runtime_queue;

        for failure in failures {
            let op_id = self.next_service_time();
            self.enqueue_to_cq(
                failure.cq,
                CompletionEvent {
                    op_id,
                    task: None,
                    source: CompletionSource::GuestUapi,
                    status: CompletionStatus::RetryableFailure {
                        code: format!("runtime_exhausted_{}", failure.code),
                    },
                    finished_at: now,
                },
            )?;
        }

        if force_flush && !self.runtime_queue.is_empty() {
            return self.flush_runtime(now);
        }
        Ok(())
    }

    fn route_completion_to_cq(&mut self, event: CompletionEvent) -> Result<(), SimError> {
        let cq = self
            .completion_routes
            .complete(&event)
            .or_else(|| self.default_cq().ok())
            .ok_or(SimError::NotFound("completion queue"))?;
        self.enqueue_to_cq(cq, event)
    }

    fn bind_completion_route(&mut self, source: CompletionSource, op_id: u64, cq: CqHandle) {
        self.completion_routes.issue(source, op_id, cq);
    }

    fn create_cmd_queue(
        &mut self,
        cq: CqHandle,
        owner: EntityId,
        depth: usize,
    ) -> Result<CmdQueueHandle, SimError> {
        if depth == 0 {
            return Err(SimError::InvalidInput(
                "command queue depth must be positive",
            ));
        }
        let cq_state = self
            .cq_events
            .get(&cq)
            .ok_or(SimError::NotFound("completion queue"))?;
        if cq_state.owner != owner {
            return Err(SimError::InvalidInput("command queue owner mismatch"));
        }
        self.next_cmdq_id += 1;
        let cmdq = CmdQueueHandle(self.next_cmdq_id);
        self.cmd_queues.insert(
            cmdq,
            CommandQueueState {
                cq,
                owner,
                depth,
                pending: VecDeque::new(),
            },
        );
        Ok(cmdq)
    }

    fn enqueue_cmd(
        &mut self,
        cmdq: CmdQueueHandle,
        owner: EntityId,
        desc: UapiDescriptor,
    ) -> Result<(usize, usize), SimError> {
        let queue = self
            .cmd_queues
            .get_mut(&cmdq)
            .ok_or(SimError::NotFound("command queue"))?;
        if queue.owner != owner {
            return Err(SimError::InvalidInput("command queue owner mismatch"));
        }
        if queue.pending.len() >= queue.depth {
            return Err(SimError::InvalidInput("command queue full"));
        }
        queue.pending.push_back(desc);
        Ok((queue.pending.len(), queue.depth - queue.pending.len()))
    }

    fn ring_doorbell(
        &mut self,
        cmdq: CmdQueueHandle,
        owner: EntityId,
        max_batch: Option<usize>,
    ) -> Result<(usize, usize), SimError> {
        let (cq, pending_after_ring, mut staged) = {
            let queue = self
                .cmd_queues
                .get_mut(&cmdq)
                .ok_or(SimError::NotFound("command queue"))?;
            if queue.owner != owner {
                return Err(SimError::InvalidInput("command queue owner mismatch"));
            }
            let batch = max_batch
                .unwrap_or(queue.pending.len())
                .min(queue.pending.len());
            let mut staged = Vec::with_capacity(batch);
            for _ in 0..batch {
                if let Some(desc) = queue.pending.pop_front() {
                    staged.push(desc);
                }
            }
            (queue.cq, queue.pending.len(), staged)
        };

        let submitted = staged.len();
        for desc in staged.drain(..) {
            self.stage_runtime_descriptor(cq, desc)?;
        }
        Ok((submitted, pending_after_ring))
    }

    fn poll_cq_with_owner(
        &mut self,
        cq: CqHandle,
        owner: EntityId,
        max_entries: Option<usize>,
    ) -> Result<(Vec<CompletionEvent>, usize), SimError> {
        self.flush_runtime(u64::MAX)?;
        self.flush_services(u64::MAX)?;
        let queue = self
            .cq_events
            .get_mut(&cq)
            .ok_or(SimError::NotFound("completion queue"))?;
        if queue.owner != owner {
            return Err(SimError::InvalidInput("completion queue owner mismatch"));
        }
        let limit = max_entries
            .unwrap_or(queue.events.len())
            .min(queue.events.len());
        let mut events = Vec::with_capacity(limit);
        for _ in 0..limit {
            if let Some(event) = queue.events.pop_front() {
                events.push(event);
            }
        }
        Ok((events, queue.events.len()))
    }

    fn submit_descriptor_to_cq(
        &mut self,
        desc: UapiDescriptor,
        cq: CqHandle,
    ) -> Result<u64, SimError> {
        match desc {
            UapiDescriptor::Io(req) => self.submit_io_to_cq(req, cq),
            UapiDescriptor::BlockWriteback { block, task } => {
                let now = self.next_service_time();
                let handle = self.block_service.submit_writeback(block, task, now)?;
                self.bind_completion_route(CompletionSource::BlockService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::ShmemPut(req) => {
                let now = self.next_service_time();
                let handle = self.shmem_service.submit_put(req, now)?;
                self.bind_completion_route(CompletionSource::ShmemService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::ShmemGet(req) => {
                let now = self.next_service_time();
                let handle = self.shmem_service.submit_get(req, now)?;
                self.bind_completion_route(CompletionSource::ShmemService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::DfsRead(req) => {
                let now = self.next_service_time();
                let handle = self.dfs_service.submit_read(req, now)?;
                self.bind_completion_route(CompletionSource::DfsService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::DfsWrite(req) => {
                let now = self.next_service_time();
                let handle = self.dfs_service.submit_write(req, now)?;
                self.bind_completion_route(CompletionSource::DfsService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::DbPut(req) => {
                let now = self.next_service_time();
                let handle = self.db_service.submit_put(req, now)?;
                self.bind_completion_route(CompletionSource::DbService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::DbGet(req) => {
                let now = self.next_service_time();
                let handle = self.db_service.submit_get(req, now)?;
                self.bind_completion_route(CompletionSource::DbService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::ObjectPublish(req) => {
                let now = self.next_service_time();
                let handle = self.object_service.submit_publish(req, now)?;
                self.bind_completion_route(CompletionSource::DbService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::ObjectResolve(req) => {
                let now = self.next_service_time();
                let handle = self.object_service.submit_resolve(req, now)?;
                self.bind_completion_route(CompletionSource::DbService, handle.0, cq);
                Ok(handle.0)
            }
            UapiDescriptor::ObjectAppend(req) => {
                let now = self.next_service_time();
                let handle = self.object_service.submit_append(req, now)?;
                self.bind_completion_route(CompletionSource::DbService, handle.0, cq);
                Ok(handle.0)
            }
        }
    }

    pub fn execute(&mut self, cmd: UapiCommand) -> Result<UapiResponse, SimError> {
        match cmd {
            UapiCommand::QueryTopology => Ok(UapiResponse::TopologySnapshot(self.query_topology())),
            UapiCommand::CreateSegment { bytes } => {
                self.create_segment(bytes).map(UapiResponse::SegmentCreated)
            }
            UapiCommand::RegisterCq { owner } => self
                .register_cq_with_owner(owner)
                .map(UapiResponse::CqRegistered),
            UapiCommand::CreateCmdQueue { cq, owner, depth } => self
                .create_cmd_queue(cq, owner, depth)
                .map(UapiResponse::CmdQueueCreated),
            UapiCommand::EnqueueCmd { cmdq, owner, desc } => self
                .enqueue_cmd(cmdq, owner, desc)
                .map(
                    |(depth, remaining_capacity)| UapiResponse::CommandEnqueued {
                        depth,
                        remaining_capacity,
                    },
                ),
            UapiCommand::RingDoorbell {
                cmdq,
                owner,
                max_batch,
            } => self
                .ring_doorbell(cmdq, owner, max_batch)
                .map(|(submitted, pending)| UapiResponse::DoorbellRung { submitted, pending }),
            UapiCommand::SubmitIo { req } => self.submit_io(req).map(UapiResponse::IoSubmitted),
            UapiCommand::SubmitBlockWriteback { block, task } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::BlockWriteback { block, task }, cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitShmemPut { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::ShmemPut(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitShmemGet { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::ShmemGet(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitDfsRead { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::DfsRead(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitDfsWrite { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::DfsWrite(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitDbPut { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::DbPut(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitDbGet { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::DbGet(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitObjectPublish { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::ObjectPublish(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitObjectResolve { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::ObjectResolve(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::SubmitObjectAppend { req } => {
                let cq = self.default_cq()?;
                self.submit_descriptor_to_cq(UapiDescriptor::ObjectAppend(req), cq)
                    .map(UapiResponse::IoSubmitted)
            }
            UapiCommand::PollCq {
                cq,
                owner,
                max_entries,
            } => self
                .poll_cq_with_owner(cq, owner, max_entries)
                .map(|(events, remaining)| UapiResponse::Completions { events, remaining }),
            UapiCommand::DrainCq { cq, owner } => self
                .poll_cq_with_owner(cq, owner, None)
                .map(|(events, remaining)| UapiResponse::Completions { events, remaining }),
            UapiCommand::GetHealth { entity } => {
                self.get_health(entity).map(UapiResponse::HealthStatus)
            }
        }
    }

    fn next_service_time(&mut self) -> u64 {
        self.service_clock += 1;
        self.service_clock
    }
}

impl GuestUapiSurface for LocalGuestUapiSurface {
    fn query_topology(&self) -> TopologySnapshot {
        self.topology.snapshot()
    }

    fn create_segment(&mut self, bytes: u64) -> Result<SegmentHandle, SimError> {
        if bytes == 0 {
            return Err(SimError::InvalidInput("segment bytes must be positive"));
        }
        self.next_segment_id += 1;
        let segment = SegmentHandle(self.next_segment_id);
        self.shmem_service.register_segment(segment, 0, bytes)?;
        self.segment_payloads
            .insert(segment, vec![0; bytes as usize]);
        Ok(segment)
    }

    fn register_cq(&mut self) -> Result<CqHandle, SimError> {
        self.register_cq_with_owner(0)
    }

    fn submit_io(&mut self, req: IoSubmitReq) -> Result<u64, SimError> {
        let cq = self.default_cq()?;
        self.submit_io_to_cq(req, cq)
    }

    fn poll_cq(&self, cq: CqHandle) -> Vec<CompletionEvent> {
        self.cq_events
            .get(&cq)
            .map(|queue| queue.events.iter().cloned().collect())
            .unwrap_or_default()
    }

    fn get_health(&self, entity: EntityId) -> Result<HealthStatus, SimError> {
        self.topology
            .entities
            .iter()
            .find(|e| e.id == entity)
            .map(|e| e.health)
            .ok_or(SimError::NotFound("entity"))
    }
}

impl LocalGuestUapiSurface {
    fn register_cq_with_owner(&mut self, owner: EntityId) -> Result<CqHandle, SimError> {
        self.next_cq_id += 1;
        let cq = CqHandle(self.next_cq_id);
        self.cq_events.insert(
            cq,
            CompletionQueueState {
                owner,
                events: VecDeque::new(),
            },
        );
        Ok(cq)
    }
    fn submit_io_to_cq(&mut self, req: IoSubmitReq, cq: CqHandle) -> Result<u64, SimError> {
        match req.opcode {
            IoOpcode::ReadBlock => {
                let block = req
                    .block
                    .ok_or(SimError::InvalidInput("missing block hash"))?;
                let segment = req
                    .segment
                    .ok_or(SimError::InvalidInput("missing segment handle"))?;
                self.copy_block_to_segment(&block, segment)?;
                let now = self.next_service_time();
                let handle = self.block_service.submit_read(
                    sim_runtime::BlockReadReq {
                        task: req.task,
                        block,
                    },
                    now,
                )?;
                self.bind_completion_route(CompletionSource::BlockService, handle.0, cq);
                Ok(handle.0)
            }
            IoOpcode::WriteBlock => {
                let block = req
                    .block
                    .ok_or(SimError::InvalidInput("missing block hash"))?;
                let segment = req
                    .segment
                    .ok_or(SimError::InvalidInput("missing segment handle"))?;
                self.copy_segment_to_block(segment, block.clone())?;
                let now = self.next_service_time();
                let handle = self.block_service.submit_write(
                    sim_runtime::BlockWriteReq {
                        task: req.task,
                        block,
                    },
                    now,
                )?;
                self.bind_completion_route(CompletionSource::BlockService, handle.0, cq);
                Ok(handle.0)
            }
            IoOpcode::Dispatch => {
                let event = self.run_chipbackend_dispatch(req)?;
                let op_id = event.op_id;
                self.enqueue_to_cq(cq, event)?;
                Ok(op_id)
            }
            IoOpcode::RemoteFetch | IoOpcode::RemoteStore => {
                /* Phase 2: remote block transport — stub for now */
                let event = CompletionEvent {
                    op_id: req.op_id,
                    task: req.task,
                    source: CompletionSource::RemoteNode,
                    status: CompletionStatus::RetryableFailure {
                        code: "remote_transport_not_available".to_string(),
                    },
                    finished_at: self.next_service_time(),
                };
                self.enqueue_to_cq(cq, event)?;
                Ok(req.op_id)
            }
        }
    }

    fn run_chipbackend_dispatch(&mut self, req: IoSubmitReq) -> Result<CompletionEvent, SimError> {
        let now = self.next_service_time();
        let output_segment = req.segment;
        let task = req.task.unwrap_or(TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: req.op_id,
        });
        let result = output_segment
            .ok_or_else(|| "missing_dispatch_segment".to_string())
            .and_then(|segment| {
                let input = self
                    .segment_payloads
                    .get(&segment)
                    .ok_or_else(|| "missing_dispatch_segment_payload".to_string())?
                    .clone();
                run_w4_chipbackend(&self.topology, &task, &input).map(|output| (segment, output))
            });
        if let Ok((segment, output)) = &result {
            self.write_dispatch_result_to_segment(*segment, output)?;
        }
        Ok(CompletionEvent {
            op_id: req.op_id,
            task: Some(task),
            source: CompletionSource::ChipBackend,
            status: match result {
                Ok(_) => CompletionStatus::Success,
                Err(code) => CompletionStatus::FatalFailure { code },
            },
            finished_at: now,
        })
    }

    fn copy_segment_to_block(
        &mut self,
        segment: SegmentHandle,
        block: BlockHash,
    ) -> Result<(), SimError> {
        let payload = self
            .segment_payloads
            .get(&segment)
            .ok_or(SimError::NotFound("segment payload"))?
            .clone();
        self.block_payloads.insert(block, payload);
        Ok(())
    }

    fn copy_block_to_segment(
        &mut self,
        block: &BlockHash,
        segment: SegmentHandle,
    ) -> Result<(), SimError> {
        let segment_payload = self
            .segment_payloads
            .get_mut(&segment)
            .ok_or(SimError::NotFound("segment payload"))?;
        let Some(payload) = self.block_payloads.get(block) else {
            segment_payload.fill(0);
            return Ok(());
        };
        let payload = payload.clone();
        let copy_len = segment_payload.len().min(payload.len());
        segment_payload[..copy_len].copy_from_slice(&payload[..copy_len]);
        if segment_payload.len() > copy_len {
            segment_payload[copy_len..].fill(0);
        }
        Ok(())
    }

    fn write_dispatch_result_to_segment(
        &mut self,
        segment: SegmentHandle,
        output: &[u8],
    ) -> Result<(), SimError> {
        let payload = self
            .segment_payloads
            .get_mut(&segment)
            .ok_or(SimError::NotFound("segment payload"))?;
        let copy_len = payload.len().min(output.len());
        payload[..copy_len].copy_from_slice(&output[..copy_len]);
        if payload.len() > copy_len {
            payload[copy_len..].fill(0);
        }
        Ok(())
    }
}

fn run_w4_chipbackend(
    topology: &SimTopology,
    task: &TaskKey,
    guest_input: &[u8],
) -> Result<Vec<u8>, String> {
    match std::env::var("SIM_UAPI_W4_CHIPBACKEND_PROFILE")
        .unwrap_or_else(|_| "host_vector".to_string())
        .as_str()
    {
        "qwen3_dense_0_6b" => {
            run_qwen3_dense_0_6b_prefill_runtime(topology, task, guest_input, None)
        }
        "host_matmul" => run_host_matmul_smoke(topology, task),
        "host_vector" | "" => run_host_vector_chipbackend(topology, task, guest_input),
        other => Err(format!("unsupported_w4_chipbackend_profile:{other}")),
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Qwen3GuestRangeComputeContract {
    node: u32,
    layer_start: u32,
    layer_end: u32,
    next_node: u32,
    pipeline_nodes: u32,
    total_layers: u32,
    hidden_bytes: u32,
}

fn qwen3_guest_range_compute_contract(
    task: &TaskKey,
) -> Result<Option<Qwen3GuestRangeComputeContract>, String> {
    const RANGE_TASK_MAGIC: u32 = 0x5133_060b;

    if task.scope_depth != 8 || task.coord.levels[0] != RANGE_TASK_MAGIC {
        return Ok(None);
    }
    let contract = Qwen3GuestRangeComputeContract {
        node: task.coord.levels[1],
        layer_start: task.coord.levels[2],
        layer_end: task.coord.levels[3],
        next_node: task.coord.levels[4],
        pipeline_nodes: task.coord.levels[5],
        total_layers: task.coord.levels[6],
        hidden_bytes: task.coord.levels[7],
    };
    if contract.pipeline_nodes != QWEN3_DENSE_0_6B_PROFILE.tp_nodes as u32
        || contract.total_layers != QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as u32
        || contract.node >= contract.pipeline_nodes
        || contract.next_node >= contract.pipeline_nodes
        || contract.layer_start >= contract.layer_end
        || contract.layer_end > contract.total_layers
        || contract.hidden_bytes == 0
    {
        return Err(format!(
            "qwen3_guest_range_compute_contract_invalid:node={} layers=[{},{}) next={} nodes={} total_layers={} hidden_bytes={}",
            contract.node,
            contract.layer_start,
            contract.layer_end,
            contract.next_node,
            contract.pipeline_nodes,
            contract.total_layers,
            contract.hidden_bytes
        ));
    }
    Ok(Some(contract))
}

pub fn qwen3_dense_0_6b_prefill_text_output_report(
    topology: &SimTopology,
    guest_input: &[u8],
) -> Result<Qwen3Dense06bTextOutputReport, String> {
    qwen3_dense_0_6b_prefill_text_output_report_with_task_id(topology, guest_input, 100)
}

pub fn qwen3_dense_0_6b_decode_loop_report(
    topology: &SimTopology,
    step_count: usize,
) -> Result<Qwen3Dense06bDecodeLoopReport, String> {
    let guest_input = qwen3_dense_0_6b_default_guest_input();
    let guest_input_report = qwen3_dense_0_6b_synthetic_guest_input_report(&guest_input);
    qwen3_dense_0_6b_decode_loop_report_with_initial_guest_input(
        topology,
        step_count,
        guest_input,
        guest_input_report,
    )
}

pub fn qwen3_dense_0_6b_decode_loop_report_with_prompt(
    topology: &SimTopology,
    step_count: usize,
    prompt: &str,
) -> Result<Qwen3Dense06bDecodeLoopReport, String> {
    let tokenizer_path = qwen3_dense_0_6b_real_tokenizer_path()
        .ok_or_else(|| "qwen3_prompt_tokenizer_path_missing".to_string())?;
    let prompt_tokens = tokenize_prompt_from_tokenizer_path(&tokenizer_path, prompt)?;
    let guest_input =
        qwen3_dense_0_6b_tokenized_prompt_guest_input(prompt, &prompt_tokens.token_ids);
    let guest_input_report =
        qwen3_dense_0_6b_prompt_guest_input_report(prompt, &prompt_tokens.token_ids, &guest_input)?;
    qwen3_dense_0_6b_decode_loop_report_with_initial_guest_input(
        topology,
        step_count,
        guest_input,
        guest_input_report,
    )
}

pub fn qwen3_dense_0_6b_range_forward_report_with_prompt(
    topology: &SimTopology,
    prompt: &str,
) -> Result<Qwen3Dense06bRangeForwardReport, String> {
    let tokenizer_path = qwen3_dense_0_6b_real_tokenizer_path()
        .ok_or_else(|| "qwen3_range_forward_tokenizer_path_missing".to_string())?;
    let prompt_tokens = tokenize_prompt_from_tokenizer_path(&tokenizer_path, prompt)?;
    let token_ids = prompt_tokens.token_ids;
    if token_ids.is_empty() {
        return Err("qwen3_range_forward_prompt_tokens_empty".to_string());
    }
    let weights_path = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH")
        .map_err(|_| "qwen3_range_forward_weights_path_missing".to_string())?;
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let mut service = LingquObjectServiceStub::new(qwen3_dense_0_6b_object_service_profile());
    qwen3_dense_0_6b_publish_bootstrap_weight_objects(topology, &mut service)?;
    let resolved = qwen3_dense_0_6b_resolve_runtime_weight_objects(&mut service, 810_000)?;
    let forward = forward_from_token_ids_with_layer_payloads(
        &loaded.tensors,
        &resolved.layer_payloads,
        &token_ids,
    )?;
    let embedding_hidden = embedding_reference_last_hidden_with_payloads(
        &loaded.tensors,
        Some(&resolved.layer_payloads),
        &token_ids,
    )?;
    let ranges = qwen3_dense_0_6b_bootstrap_node_ranges();
    let session_key = checksum_words(&[
        prompt_token_ids_checksum(&token_ids),
        token_ids.len() as u64,
        forward.aggregate_checksum,
    ]);
    let mut workers = Vec::with_capacity(ranges.len());
    let mut previous_output_payload = None::<Vec<u8>>;
    for (range_index, range) in ranges.iter().enumerate() {
        let input_payload = previous_output_payload
            .clone()
            .unwrap_or_else(|| qwen3_dense_0_6b_f32_payload_bytes(&embedding_hidden));
        let input_payload_checksum = weight_bytes_checksum(&input_payload);
        let first_layer = forward
            .layers
            .get(range.first_layer_id as usize)
            .ok_or_else(|| {
                format!(
                    "qwen3_range_forward_first_layer_missing:{}",
                    range.first_layer_id
                )
            })?;
        let last_layer = forward
            .layers
            .get(range.last_layer_id as usize)
            .ok_or_else(|| {
                format!(
                    "qwen3_range_forward_last_layer_missing:{}",
                    range.last_layer_id
                )
            })?;
        let output_payload = qwen3_dense_0_6b_f32_payload_bytes(&last_layer.output);
        let output_payload_checksum = weight_bytes_checksum(&output_payload);
        let input_key = format!(
            "qwen3/range-forward/{session_key:016x}/node-{}/layers/{:02}-{:02}/input",
            range.node_id, range.first_layer_id, range.last_layer_id
        );
        let output_key = format!(
            "qwen3/range-forward/{session_key:016x}/node-{}/layers/{:02}-{:02}/output",
            range.node_id, range.first_layer_id, range.last_layer_id
        );
        qwen3_dense_0_6b_publish_runtime_tensor_object(
            &mut service,
            &input_key,
            range.node_id,
            &input_payload,
            input_payload_checksum,
            820_000 + range_index as u64 * 10,
        )?;
        qwen3_dense_0_6b_resolve_runtime_tensor_object(
            &mut service,
            &input_key,
            range.node_id,
            input_payload_checksum,
            820_001 + range_index as u64 * 10,
        )?;
        let range_weight = qwen3_dense_0_6b_resolve_weight_range_objects(
            &mut service,
            std::slice::from_ref(range),
            830_000 + range_index as u64 * 100,
        )?;
        qwen3_dense_0_6b_publish_runtime_tensor_object(
            &mut service,
            &output_key,
            range.node_id,
            &output_payload,
            output_payload_checksum,
            820_002 + range_index as u64 * 10,
        )?;
        qwen3_dense_0_6b_resolve_runtime_tensor_object(
            &mut service,
            &output_key,
            range.node_id,
            output_payload_checksum,
            820_003 + range_index as u64 * 10,
        )?;
        let handoff_input_matches_previous_output = match previous_output_payload.as_ref() {
            Some(previous) => previous.as_slice() == input_payload.as_slice(),
            None => true,
        };
        let aggregate_checksum = checksum_words(&[
            range.node_id,
            range.first_layer_id,
            range.last_layer_id,
            input_payload.len() as u64,
            output_payload.len() as u64,
            input_payload_checksum,
            output_payload_checksum,
            first_layer.input_checksum,
            last_layer.output_checksum,
            range_weight.payload_bytes,
            range_weight.slice_count,
            range_weight.reconstructed_tensor_count,
            u64::from(handoff_input_matches_previous_output),
        ]);
        workers.push(Qwen3Dense06bRangeForwardWorkerReport {
            node_id: range.node_id,
            first_layer_id: range.first_layer_id,
            last_layer_id: range.last_layer_id,
            layer_count: range.layer_count,
            input_key,
            output_key,
            weight_key: qwen3_dense_0_6b_weight_range_object_key(range),
            input_payload_bytes: input_payload.len() as u64,
            output_payload_bytes: output_payload.len() as u64,
            input_payload_checksum,
            output_payload_checksum,
            first_layer_input_checksum: first_layer.input_checksum,
            last_layer_output_checksum: last_layer.output_checksum,
            weight_payload_bytes: range_weight.payload_bytes,
            weight_payload_slice_count: range_weight.slice_count,
            weight_reconstructed_tensor_count: range_weight.reconstructed_tensor_count,
            handoff_input_matches_previous_output,
            aggregate_checksum,
        });
        previous_output_payload = Some(output_payload);
    }
    let handoff_match_count = workers
        .iter()
        .filter(|worker| worker.handoff_input_matches_previous_output)
        .count() as u64;
    let aggregate_checksum = checksum_words(
        &workers
            .iter()
            .flat_map(|worker| {
                [
                    worker.node_id,
                    worker.first_layer_id,
                    worker.last_layer_id,
                    worker.input_payload_checksum,
                    worker.output_payload_checksum,
                    worker.aggregate_checksum,
                ]
            })
            .collect::<Vec<_>>(),
    );
    Ok(Qwen3Dense06bRangeForwardReport {
        prompt_token_count: token_ids.len() as u64,
        prompt_token_checksum: prompt_token_ids_checksum(&token_ids),
        node_count: ranges.len() as u64,
        layer_count: QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers,
        ready: workers.len() == ranges.len()
            && handoff_match_count == workers.len() as u64
            && workers
                .last()
                .map(|worker| {
                    worker.last_layer_id + 1 == QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                })
                .unwrap_or(false),
        weight_object_count: resolved.object_count,
        global_weight_object_count: resolved.global_object_count,
        hidden_object_count: workers.len() as u64 * 2,
        handoff_match_count,
        aggregate_checksum,
        workers,
    })
}

fn qwen3_dense_0_6b_decode_progress(args: std::fmt::Arguments<'_>) {
    if std::env::var("SIM_QWEN3_DECODE_PROGRESS").as_deref() == Ok("1") {
        eprintln!("qwen3-decode-loop: {args}");
    }
}

fn qwen3_dense_0_6b_decode_heartbeat(
    step_index: usize,
    step_count: usize,
    stage: &'static str,
) -> Qwen3Dense06bDecodeHeartbeat {
    if std::env::var("SIM_QWEN3_DECODE_PROGRESS").as_deref() != Ok("1") {
        return Qwen3Dense06bDecodeHeartbeat::disabled();
    }
    let interval = qwen3_dense_0_6b_decode_heartbeat_interval();
    if interval.is_zero() {
        return Qwen3Dense06bDecodeHeartbeat::disabled();
    }
    let (stop_tx, stop_rx) = std::sync::mpsc::channel();
    let worker = std::thread::spawn(move || {
        let started_at = Instant::now();
        let spinner = ['-', '\\', '|', '/'];
        let mut tick = 0usize;
        loop {
            match stop_rx.recv_timeout(interval) {
                Ok(()) | Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => break,
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
                    let elapsed = started_at.elapsed().as_secs();
                    let mark = spinner[tick % spinner.len()];
                    tick = tick.wrapping_add(1);
                    eprint!(
                        "\rqwen3-decode-loop: step {}/{}: {} running [{}] elapsed={}s",
                        step_index, step_count, stage, mark, elapsed
                    );
                    let _ = std::io::stderr().flush();
                }
            }
        }
    });
    Qwen3Dense06bDecodeHeartbeat {
        stop_tx: Some(stop_tx),
        worker: Some(worker),
    }
}

fn qwen3_dense_0_6b_decode_heartbeat_interval() -> Duration {
    match std::env::var("SIM_QWEN3_DECODE_HEARTBEAT_MS") {
        Ok(value) => value
            .parse::<u64>()
            .map(Duration::from_millis)
            .unwrap_or_else(|_| Duration::from_secs(5)),
        Err(_) => Duration::from_secs(5),
    }
}

fn qwen3_dense_0_6b_simpler_round1_dispatch_batch_size() -> usize {
    std::env::var("SIM_QWEN3_ROUND1_DISPATCH_BATCH")
        .ok()
        .and_then(|value| value.parse::<usize>().ok())
        .filter(|value| *value > 0)
        .unwrap_or(4)
}

fn qwen3_dense_0_6b_dispatch_detail_timing_enabled() -> bool {
    std::env::var("SIM_QWEN3_DISPATCH_DETAIL_TIMING")
        .map(|value| {
            matches!(
                value.as_str(),
                "1" | "true" | "TRUE" | "yes" | "YES" | "on" | "ON"
            )
        })
        .unwrap_or(false)
}

fn qwen3_dense_0_6b_final_report_full_enabled() -> bool {
    std::env::var("SIM_QWEN3_FINAL_REPORT")
        .map(|value| {
            matches!(
                value.as_str(),
                "full" | "FULL" | "debug" | "DEBUG" | "1" | "true" | "TRUE" | "yes" | "YES"
            )
        })
        .unwrap_or(false)
}

fn qwen3_dense_0_6b_poll_simpler_dispatch_batch(
    runtime: &mut LocalRuntimeEngine,
    sink: &mut VecEventSink,
    runtime_time: &mut u64,
    dispatch_latency: u64,
    expected_completions: usize,
    stage: &str,
) -> Result<(), String> {
    let detail_timing = qwen3_dense_0_6b_dispatch_detail_timing_enabled();
    with_suppressed_stdio(|| {
        *runtime_time += dispatch_latency;
        let advance_started = Instant::now();
        runtime.advance_to(*runtime_time, sink);
        let advance_ms = advance_started.elapsed().as_millis();
        let poll_started = Instant::now();
        let completions = runtime.poll_completions(*runtime_time, sink);
        let poll_ms = poll_started.elapsed().as_millis();
        let check_started = Instant::now();
        if completions.len() != expected_completions {
            return Err(format!(
                "simpler_capi_qwen3_dense_0_6b_{stage}_batch_completion_count_mismatch:got={}:expected={expected_completions}",
                completions.len()
            ));
        }
        for completion in completions {
            match completion.status {
                CompletionStatus::Success => {}
                other => {
                    return Err(format!(
                    "simpler_capi_qwen3_dense_0_6b_{stage}_batch_dispatch_failed:status={other:?}"
                ))
                }
            }
        }
        if detail_timing {
            eprintln!(
                "qwen3-uapi-dispatch-detail: stage={stage} expected_completions={expected_completions} advance_ms={} poll_ms={} check_ms={}",
                advance_ms,
                poll_ms,
                check_started.elapsed().as_millis()
            );
        }
        Ok(())
    })
}

struct Qwen3Dense06bDecodeHeartbeat {
    stop_tx: Option<std::sync::mpsc::Sender<()>>,
    worker: Option<std::thread::JoinHandle<()>>,
}

impl Qwen3Dense06bDecodeHeartbeat {
    fn disabled() -> Self {
        Self {
            stop_tx: None,
            worker: None,
        }
    }
}

impl Drop for Qwen3Dense06bDecodeHeartbeat {
    fn drop(&mut self) {
        if let Some(stop_tx) = self.stop_tx.take() {
            let _ = stop_tx.send(());
        }
        let had_worker = self.worker.is_some();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
        if had_worker {
            eprint!("\r\x1b[2K");
            let _ = std::io::stderr().flush();
        }
    }
}

fn qwen3_dense_0_6b_cached_loaded_weights(
    weights_path: &str,
) -> Result<Qwen3Dense06bLoadedWeights, String> {
    static CACHE: OnceLock<Mutex<BTreeMap<String, Qwen3Dense06bLoadedWeights>>> = OnceLock::new();
    let cache = CACHE.get_or_init(|| Mutex::new(BTreeMap::new()));
    let mut cache = cache
        .lock()
        .map_err(|_| "qwen3_weight_metadata_cache_poisoned".to_string())?;
    if let Some(loaded) = cache.get(weights_path) {
        return Ok(loaded.clone());
    }
    let loaded = load_safetensors_path_metadata(Path::new(weights_path))?;
    cache.insert(weights_path.to_string(), loaded.clone());
    Ok(loaded)
}

fn qwen3_dense_0_6b_decode_loop_report_with_initial_guest_input(
    topology: &SimTopology,
    step_count: usize,
    mut guest_input: Vec<u8>,
    mut guest_input_report: Qwen3Dense06bGuestInputReport,
) -> Result<Qwen3Dense06bDecodeLoopReport, String> {
    let mut steps = Vec::with_capacity(step_count);
    let mut generated_text = String::new();
    let mut generated_bytes = Vec::new();
    let mut decode_state: Option<Qwen3Dense06bIncrementalDecodeState> = None;
    let mut object_service =
        LingquObjectServiceStub::new(qwen3_dense_0_6b_object_service_profile());
    qwen3_dense_0_6b_publish_bootstrap_weight_objects(topology, &mut object_service)?;
    let session_id = qwen3_dense_0_6b_decode_guest_input_checksum(&guest_input);
    for step_index in 0..step_count {
        let step_started_at = Instant::now();
        let object_service_before = object_service.report();
        let guest_input_checksum = qwen3_dense_0_6b_decode_guest_input_checksum(&guest_input);
        guest_input_report.byte_len = guest_input.len() as u64;
        guest_input_report.checksum = guest_input_checksum;
        let runtime_prefill_executed = decode_state.is_none();
        let (mut text_output, step_real_kv_cache) = if runtime_prefill_executed {
            qwen3_dense_0_6b_decode_progress(format_args!(
                "step {}/{}: runtime prefill start",
                step_index + 1,
                step_count
            ));
            let _heartbeat =
                qwen3_dense_0_6b_decode_heartbeat(step_index + 1, step_count, "runtime prefill");
            let text_output = qwen3_dense_0_6b_prefill_text_output_report_with_task_id_guest_input_and_object_service(
                topology,
                &guest_input,
                200 + step_index as u64,
                guest_input_report.clone(),
                Some(&mut object_service),
            )?;
            let real_kv_cache = qwen3_dense_0_6b_real_kv_cache_from_token_ids(
                &qwen3_dense_0_6b_guest_input_token_ids(&guest_input),
            )?;
            (text_output, real_kv_cache)
        } else {
            qwen3_dense_0_6b_decode_progress(format_args!(
                "step {}/{}: incremental decode start",
                step_index + 1,
                step_count
            ));
            qwen3_dense_0_6b_incremental_decode_text_output_report(
                decode_state
                    .as_ref()
                    .ok_or_else(|| "qwen3_incremental_decode_state_missing".to_string())?,
                &guest_input,
                guest_input_report.clone(),
                step_index as u64,
                &mut object_service,
            )?
        };
        let object_service_after_runtime = object_service.report();
        let pre_report_resolve_count = object_service_after_runtime
            .resolve_count
            .saturating_sub(object_service_before.resolve_count);
        let pre_report_kv_resolve_count = u64::from(!runtime_prefill_executed);
        let runtime_weight_resolve_count =
            pre_report_resolve_count.saturating_sub(pre_report_kv_resolve_count);
        qwen3_dense_0_6b_decode_progress(format_args!(
            "step {}/{}: runtime output parsed, selecting one token",
            step_index + 1,
            step_count
        ));
        let mut selected_samples =
            qwen3_dense_0_6b_decode_step_selected_samples(&text_output.samples);
        let selected_bytes = qwen3_dense_0_6b_decode_step_selected_bytes(&selected_samples);
        for sample in &mut selected_samples {
            sample.byte_len = selected_bytes.len() as u64;
        }
        text_output.samples = selected_samples.clone();
        generated_text.push_str(&String::from_utf8_lossy(&selected_bytes));
        generated_bytes.extend_from_slice(&selected_bytes);
        text_output.real_inference = qwen3_dense_0_6b_real_inference_report_from_runtime_samples(
            &guest_input,
            &selected_samples,
        )?;
        let next_guest_input = qwen3_dense_0_6b_next_decode_guest_input(
            &guest_input,
            &selected_samples,
            step_index as u64,
        );
        let input_transition = qwen3_dense_0_6b_decode_input_transition_report(
            &selected_samples,
            step_index as u64,
            &next_guest_input,
        );
        let hidden_layer_pipeline = qwen3_dense_0_6b_hidden_layer_pipeline_report(
            topology,
            &text_output,
            &guest_input,
            Some(&mut object_service),
            session_id,
            step_index as u64,
        )?;
        let layer_progress =
            qwen3_dense_0_6b_decode_layer_progress_report(&text_output, &hidden_layer_pipeline);
        let real_inference_contract = qwen3_dense_0_6b_real_inference_contract_report(
            &text_output,
            &layer_progress,
            &hidden_layer_pipeline,
        );
        let object_service_report = qwen3_dense_0_6b_decode_object_service_report(
            &mut object_service,
            object_service_before,
            session_id,
            step_index as u64,
            guest_input_checksum,
            &selected_samples,
            &text_output,
            &hidden_layer_pipeline,
            step_real_kv_cache.as_deref(),
            runtime_weight_resolve_count + hidden_layer_pipeline.boundary_count * 2,
        )?;
        let next_guest_input_checksum =
            qwen3_dense_0_6b_decode_guest_input_checksum(&next_guest_input);
        let next_guest_input_report = qwen3_dense_0_6b_transition_guest_input_report(
            &input_transition,
            guest_input_report,
            &next_guest_input,
        );
        let next_decode_state = Qwen3Dense06bIncrementalDecodeState::from_step(
            &text_output,
            &next_guest_input,
            &selected_samples,
            step_index as u64,
            session_id,
            &object_service,
            step_real_kv_cache,
        )?;
        steps.push(Qwen3Dense06bDecodeLoopStepReport {
            step_index: step_index as u64,
            runtime_prefill_executed,
            guest_input_checksum,
            next_guest_input_checksum,
            input_transition,
            layer_progress,
            hidden_layer_pipeline,
            object_service: object_service_report,
            real_inference_contract,
            sampled_token_count: selected_samples.len() as u64,
            text_output,
        });
        qwen3_dense_0_6b_decode_progress(format_args!(
            "step {}/{}: appended {} token(s), next input tokens={}, duration: {}",
            step_index + 1,
            step_count,
            selected_samples.len(),
            qwen3_dense_0_6b_guest_input_token_ids(&next_guest_input).len(),
            qwen3_dense_0_6b_decode_duration_label(step_started_at.elapsed())
        ));
        decode_state = Some(next_decode_state);
        guest_input = next_guest_input;
        guest_input_report = next_guest_input_report;
    }
    let decode_chain_checksum = qwen3_dense_0_6b_decode_chain_checksum(&steps);
    Ok(Qwen3Dense06bDecodeLoopReport {
        steps,
        final_guest_input_checksum: qwen3_dense_0_6b_decode_guest_input_checksum(&guest_input),
        decode_chain_checksum,
        generated_byte_len: generated_bytes.len() as u64,
        generated_byte_checksum: qwen3_dense_0_6b_text_output_bytes_checksum(&generated_bytes),
        generated_text_lossy: generated_text,
    })
}

fn qwen3_dense_0_6b_decode_duration_label(duration: Duration) -> String {
    format!("{:.1} seconds", duration.as_secs_f64())
}

fn qwen3_dense_0_6b_object_service_profile() -> LingquObjectServiceProfile {
    let mut profile = LingquObjectServiceProfile::default();
    profile.queue_depth = 512;
    profile
}

fn qwen3_dense_0_6b_publish_bootstrap_weight_objects(
    topology: &SimTopology,
    service: &mut LingquObjectServiceStub,
) -> Result<(), String> {
    let ranges = qwen3_dense_0_6b_bootstrap_node_ranges();
    let weight_payloads = qwen3_dense_0_6b_bootstrap_weight_range_payloads(topology, &ranges)?;
    let global_payload = qwen3_dense_0_6b_bootstrap_global_weight_payload()?;
    for (index, range) in ranges.iter().enumerate() {
        let payload = weight_payloads
            .get(&range.node_id)
            .cloned()
            .unwrap_or_else(|| qwen3_dense_0_6b_fallback_weight_range_payload(range));
        let checksum = weight_bytes_checksum(&payload);
        let key = qwen3_dense_0_6b_weight_range_object_key(range);
        service
            .submit_publish(
                LingquObjectPublishReq {
                    task: None,
                    key: key.clone(),
                    kind: LingquObjectKind::WeightShard,
                    producer_entity: range.node_id,
                    owner_entity: Some(range.node_id),
                    expected_version: None,
                    metadata: qwen3_dense_0_6b_object_metadata(payload.len() as u64, checksum),
                    placements: vec![qwen3_dense_0_6b_object_placement(
                        &key,
                        LingquPayloadBackend::Block,
                        payload.len() as u64,
                        checksum,
                    )],
                    payload_bytes: payload,
                },
                index as u64,
            )
            .map_err(|err| format!("qwen3_weight_object_bootstrap_publish_failed:{err}"))?;
    }
    let mut expected_completions = ranges.len();
    if let Some(payload) = global_payload {
        let checksum = weight_bytes_checksum(&payload);
        let key = qwen3_dense_0_6b_global_weight_object_key();
        service
            .submit_publish(
                LingquObjectPublishReq {
                    task: None,
                    key: key.clone(),
                    kind: LingquObjectKind::WeightShard,
                    producer_entity: 0,
                    owner_entity: None,
                    expected_version: None,
                    metadata: qwen3_dense_0_6b_object_metadata(payload.len() as u64, checksum),
                    placements: vec![qwen3_dense_0_6b_object_placement(
                        &key,
                        LingquPayloadBackend::Block,
                        payload.len() as u64,
                        checksum,
                    )],
                    payload_bytes: payload,
                },
                ranges.len() as u64,
            )
            .map_err(|err| format!("qwen3_global_weight_object_bootstrap_publish_failed:{err}"))?;
        expected_completions += 1;
    }
    let completions = service.poll_ready(10_000);
    if completions.len() != expected_completions
        || completions
            .iter()
            .any(|event| event.status != CompletionStatus::Success)
    {
        return Err(format!(
            "qwen3_weight_object_bootstrap_completion_failed:got={}:expected={}",
            completions.len(),
            expected_completions
        ));
    }
    Ok(())
}

fn qwen3_dense_0_6b_bootstrap_node_ranges() -> Vec<Qwen3Dense06bHiddenLayerNodeRange> {
    let mut layers_per_node = vec![0u64; QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize];
    for layer_id in 0..QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        let node_id = qwen3_dense_0_6b_hidden_layer_owner_node(layer_id);
        if let Some(count) = layers_per_node.get_mut(node_id as usize) {
            *count += 1;
        }
    }
    qwen3_dense_0_6b_hidden_layer_node_ranges(&layers_per_node)
}

fn qwen3_dense_0_6b_bootstrap_weight_range_payloads(
    topology: &SimTopology,
    ranges: &[Qwen3Dense06bHiddenLayerNodeRange],
) -> Result<BTreeMap<u64, Vec<u8>>, String> {
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(BTreeMap::new());
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let manifest = weight_manifest_from_metadata(
        topology,
        QWEN3_DENSE_0_6B_PROFILE,
        loaded.source.clone(),
        &loaded.tensors,
    )?;
    let mut payloads = BTreeMap::new();
    for range in ranges {
        let mut payload = qwen3_dense_0_6b_weight_range_payload_header(range);
        let mut slice_count = 0u64;
        for slice in manifest.slices.iter().filter(|slice| {
            slice.layer_id >= range.first_layer_id && slice.layer_id <= range.last_layer_id
        }) {
            let slice_payload = materialize_weight_slice_payload(slice, &loaded.tensors)?;
            let slice_checksum = weight_bytes_checksum(&slice_payload);
            let payload_offset = payload
                .len()
                .checked_add(11 * std::mem::size_of::<u64>())
                .ok_or_else(|| {
                    format!(
                        "qwen3_weight_payload_record_offset_overflow:{}",
                        slice.tensor_name
                    )
                })? as u64;
            payload.extend_from_slice(&qwen3_dense_0_6b_object_payload_words(&[
                slice.layer_id,
                slice.shard_id,
                qwen3_dense_0_6b_weight_tensor_kind_code(slice.tensor_kind),
                qwen3_dense_0_6b_weight_dtype_code(slice.dtype),
                slice.slice_axis.unwrap_or(u64::MAX),
                slice.slice_start,
                slice.slice_end,
                slice_payload.len() as u64,
                slice_checksum,
                payload_offset,
                checksum_words(&slice.local_shape),
            ]));
            payload.extend_from_slice(&slice_payload);
            slice_count += 1;
        }
        if slice_count == 0 {
            return Err(format!(
                "qwen3_weight_object_bootstrap_empty_range:node={}:layers={}-{}",
                range.node_id, range.first_layer_id, range.last_layer_id
            ));
        }
        payload[4 * std::mem::size_of::<u64>()..5 * std::mem::size_of::<u64>()]
            .copy_from_slice(&slice_count.to_le_bytes());
        payloads.insert(range.node_id, payload);
    }
    Ok(payloads)
}

fn qwen3_dense_0_6b_bootstrap_global_weight_payload() -> Result<Option<Vec<u8>>, String> {
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(None);
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let mut payload = qwen3_dense_0_6b_object_payload_words(&[
        qwen3_dense_0_6b_global_weight_payload_magic(),
        0,
        0,
        0,
    ]);
    let mut tensor_count = 0u64;
    for (tensor_code, tensor_name) in qwen3_dense_0_6b_global_weight_tensor_specs() {
        let tensor = loaded
            .tensors
            .get(tensor_name)
            .ok_or_else(|| format!("qwen3_global_weight_tensor_missing:{tensor_name}"))?;
        let tensor_payload = materialize_full_weight_tensor_payload(tensor_name, &loaded.tensors)?;
        let tensor_checksum = weight_bytes_checksum(&tensor_payload);
        let payload_offset = payload
            .len()
            .checked_add(8 * std::mem::size_of::<u64>())
            .ok_or_else(|| {
                format!("qwen3_global_weight_payload_record_offset_overflow:{tensor_name}")
            })? as u64;
        payload.extend_from_slice(&qwen3_dense_0_6b_object_payload_words(&[
            tensor_code,
            qwen3_dense_0_6b_weight_dtype_code(tensor.dtype),
            tensor.shape.len() as u64,
            checksum_words(&tensor.shape),
            tensor_payload.len() as u64,
            tensor_checksum,
            payload_offset,
            0,
        ]));
        payload.extend_from_slice(&tensor_payload);
        tensor_count += 1;
    }
    payload[std::mem::size_of::<u64>()..2 * std::mem::size_of::<u64>()]
        .copy_from_slice(&tensor_count.to_le_bytes());
    Ok(Some(payload))
}

fn qwen3_dense_0_6b_global_weight_tensor_specs() -> [(u64, &'static str); 3] {
    [
        (1, "model.embed_tokens.weight"),
        (2, "model.norm.weight"),
        (3, "lm_head.weight"),
    ]
}

fn qwen3_dense_0_6b_global_weight_payload_magic() -> u64 {
    0x5157_454e_3347_4c42
}

fn qwen3_dense_0_6b_weight_range_payload_header(
    range: &Qwen3Dense06bHiddenLayerNodeRange,
) -> Vec<u8> {
    qwen3_dense_0_6b_object_payload_words(&[
        0x5147_5733_5752_4f42,
        range.node_id,
        range.first_layer_id,
        range.last_layer_id,
        0,
    ])
}

fn qwen3_dense_0_6b_fallback_weight_range_payload(
    range: &Qwen3Dense06bHiddenLayerNodeRange,
) -> Vec<u8> {
    let mut payload = qwen3_dense_0_6b_weight_range_payload_header(range);
    payload[4 * std::mem::size_of::<u64>()..5 * std::mem::size_of::<u64>()]
        .copy_from_slice(&0u64.to_le_bytes());
    payload
}

#[derive(Debug)]
struct Qwen3Dense06bResolvedWeightObjects {
    object_count: u64,
    payload_bytes: u64,
    slice_count: u64,
    slice_payload_bytes: u64,
    payload_complete: bool,
    reconstructed_tensor_count: u64,
    reconstructed_tensor_checksum: u64,
    layer_payloads: BTreeMap<String, Vec<u8>>,
    payload_checksum: u64,
    slice_checksum: u64,
    global_object_count: u64,
    global_payload_bytes: u64,
    global_tensor_count: u64,
    global_payload_checksum: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Qwen3Dense06bWeightObjectSliceView {
    layer_id: u64,
    shard_id: u64,
    tensor_kind_code: u64,
    dtype_code: u64,
    slice_axis: u64,
    slice_start: u64,
    slice_end: u64,
    payload_bytes: u64,
    payload_checksum: u64,
    payload_offset: u64,
    local_shape_checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Qwen3Dense06bWeightObjectPayloadView {
    node_id: u64,
    first_layer_id: u64,
    last_layer_id: u64,
    slices: Vec<Qwen3Dense06bWeightObjectSliceView>,
    slice_payload_bytes: u64,
    slice_checksum: u64,
}

fn qwen3_dense_0_6b_resolve_runtime_weight_objects(
    service: &mut LingquObjectServiceStub,
    event_base: u64,
) -> Result<Qwen3Dense06bResolvedWeightObjects, String> {
    let ranges = qwen3_dense_0_6b_bootstrap_node_ranges();
    let mut resolved = qwen3_dense_0_6b_resolve_weight_range_objects(service, &ranges, event_base)?;
    let global_payloads =
        qwen3_dense_0_6b_resolve_global_weight_object(service, event_base + 50_000)?;
    resolved.global_object_count = u64::from(!global_payloads.is_empty());
    resolved.global_payload_bytes = global_payloads
        .values()
        .map(|payload| payload.len() as u64)
        .sum();
    resolved.global_tensor_count = global_payloads.len() as u64;
    resolved.global_payload_checksum = checksum_words(
        &global_payloads
            .values()
            .map(|payload| weight_bytes_checksum(payload))
            .collect::<Vec<_>>(),
    );
    resolved.layer_payloads.extend(global_payloads);
    Ok(resolved)
}

fn qwen3_dense_0_6b_resolve_weight_range_objects(
    service: &mut LingquObjectServiceStub,
    ranges: &[Qwen3Dense06bHiddenLayerNodeRange],
    event_base: u64,
) -> Result<Qwen3Dense06bResolvedWeightObjects, String> {
    let require_real_payload = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok();
    let mut payload_bytes = 0u64;
    let mut slice_count = 0u64;
    let mut slice_payload_bytes = 0u64;
    let mut payload_checksums = Vec::with_capacity(ranges.len());
    let mut slice_checksums = Vec::with_capacity(ranges.len());
    let mut reconstructed_checksums = Vec::with_capacity(ranges.len());
    let mut reconstructed_tensor_count = 0u64;
    let mut layer_payloads = BTreeMap::new();
    let mut coverage = BTreeSet::new();
    for (index, range) in ranges.iter().enumerate() {
        let key = qwen3_dense_0_6b_weight_range_object_key(range);
        service
            .submit_resolve(
                LingquObjectResolveReq {
                    task: None,
                    key: key.clone(),
                    requester_entity: range.node_id,
                    version: LingquObjectVersionSelector::LatestCommitted,
                    min_state: LingquObjectState::Committed,
                    preferred_backends: vec![LingquPayloadBackend::Block],
                },
                event_base + index as u64,
            )
            .map_err(|err| format!("qwen3_weight_object_resolve_failed:{err}"))?;
        let record = service
            .latest_record(&key)
            .ok_or_else(|| format!("qwen3_weight_object_missing:{key}"))?;
        if record.kind != LingquObjectKind::WeightShard {
            return Err(format!(
                "qwen3_weight_object_kind_mismatch:key={key}:kind={:?}",
                record.kind
            ));
        }
        let payload = service
            .get_copy(&key, LingquObjectVersionSelector::Exact(record.version))
            .ok_or_else(|| format!("qwen3_weight_object_payload_missing:{key}"))?;
        let payload_checksum = weight_bytes_checksum(&payload);
        if payload_checksum != record.checksum {
            return Err(format!(
                "qwen3_weight_object_payload_checksum_mismatch:key={key}:payload={payload_checksum:#x}:record={:#x}",
                record.checksum
            ));
        }
        let view = qwen3_dense_0_6b_parse_weight_range_payload(
            range,
            &key,
            &payload,
            require_real_payload,
        )?;
        payload_bytes = payload_bytes.saturating_add(payload.len() as u64);
        slice_count = slice_count.saturating_add(view.slices.len() as u64);
        slice_payload_bytes = slice_payload_bytes.saturating_add(view.slice_payload_bytes);
        let reconstructed = qwen3_dense_0_6b_reconstruct_weight_payload_tensors(&payload, &view)?;
        reconstructed_tensor_count =
            reconstructed_tensor_count.saturating_add(reconstructed.tensor_count);
        layer_payloads.extend(reconstructed.tensors);
        for slice in &view.slices {
            coverage.insert((slice.layer_id, slice.shard_id, slice.tensor_kind_code));
        }
        payload_checksums.push(payload_checksum);
        slice_checksums.push(view.slice_checksum);
        reconstructed_checksums.push(reconstructed.aggregate_checksum);
    }
    let payload_complete =
        qwen3_dense_0_6b_validate_weight_payload_coverage(ranges, &coverage, require_real_payload)?;
    let completions = service.poll_ready(event_base + 10_000);
    if completions
        .iter()
        .any(|event| event.status != CompletionStatus::Success)
    {
        return Err("qwen3_weight_object_resolve_completion_failed".to_string());
    }
    Ok(Qwen3Dense06bResolvedWeightObjects {
        object_count: ranges.len() as u64,
        payload_bytes,
        slice_count,
        slice_payload_bytes,
        payload_complete,
        reconstructed_tensor_count,
        reconstructed_tensor_checksum: checksum_words(&reconstructed_checksums),
        layer_payloads,
        payload_checksum: checksum_words(&payload_checksums),
        slice_checksum: checksum_words(&slice_checksums),
        global_object_count: 0,
        global_payload_bytes: 0,
        global_tensor_count: 0,
        global_payload_checksum: 0,
    })
}

fn qwen3_dense_0_6b_validate_weight_payload_coverage(
    ranges: &[Qwen3Dense06bHiddenLayerNodeRange],
    coverage: &BTreeSet<(u64, u64, u64)>,
    require_complete: bool,
) -> Result<bool, String> {
    let mut expected_count = 0u64;
    for range in ranges {
        for layer_id in range.first_layer_id..=range.last_layer_id {
            for shard_id in 0..QWEN3_DENSE_0_6B_PROFILE.tp_nodes {
                for kind_code in qwen3_dense_0_6b_required_layer_weight_kind_codes() {
                    expected_count += 1;
                    if require_complete && !coverage.contains(&(layer_id, shard_id, kind_code)) {
                        return Err(format!(
                            "qwen3_weight_object_payload_missing_slice:layer={layer_id}:shard={shard_id}:kind_code={kind_code}"
                        ));
                    }
                }
            }
        }
    }
    Ok(expected_count != 0 && coverage.len() as u64 == expected_count)
}

fn qwen3_dense_0_6b_resolve_global_weight_object(
    service: &mut LingquObjectServiceStub,
    event_id: u64,
) -> Result<BTreeMap<String, Vec<u8>>, String> {
    if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_err() {
        return Ok(BTreeMap::new());
    }
    let key = qwen3_dense_0_6b_global_weight_object_key();
    service
        .submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: key.clone(),
                requester_entity: 0,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![LingquPayloadBackend::Block],
            },
            event_id,
        )
        .map_err(|err| format!("qwen3_global_weight_object_resolve_failed:{err}"))?;
    let record = service
        .latest_record(&key)
        .ok_or_else(|| format!("qwen3_global_weight_object_missing:{key}"))?;
    let payload = service
        .get_copy(&key, LingquObjectVersionSelector::Exact(record.version))
        .ok_or_else(|| format!("qwen3_global_weight_object_payload_missing:{key}"))?;
    let checksum = weight_bytes_checksum(&payload);
    if checksum != record.checksum {
        return Err(format!(
            "qwen3_global_weight_object_payload_checksum_mismatch:key={key}:payload={checksum:#x}:record={:#x}",
            record.checksum
        ));
    }
    let completions = service.poll_ready(event_id + 100);
    if completions
        .iter()
        .any(|event| event.status != CompletionStatus::Success)
    {
        return Err("qwen3_global_weight_object_resolve_completion_failed".to_string());
    }
    qwen3_dense_0_6b_parse_global_weight_payload(&key, &payload)
}

fn qwen3_dense_0_6b_inspect_global_weight_object(
    service: &LingquObjectServiceStub,
) -> Result<(u64, u64, u64, u64), String> {
    let key = qwen3_dense_0_6b_global_weight_object_key();
    let Some(record) = service.latest_record(&key) else {
        return Ok((0, 0, 0, 0));
    };
    let payload = service
        .get_copy(&key, LingquObjectVersionSelector::Exact(record.version))
        .ok_or_else(|| format!("qwen3_global_weight_object_payload_missing:{key}"))?;
    let tensors = qwen3_dense_0_6b_parse_global_weight_payload(&key, &payload)?;
    let payload_bytes = tensors.values().map(|payload| payload.len() as u64).sum();
    let payload_checksum = checksum_words(
        &tensors
            .values()
            .map(|payload| weight_bytes_checksum(payload))
            .collect::<Vec<_>>(),
    );
    Ok((1, payload_bytes, tensors.len() as u64, payload_checksum))
}

fn qwen3_dense_0_6b_parse_global_weight_payload(
    key: &str,
    payload: &[u8],
) -> Result<BTreeMap<String, Vec<u8>>, String> {
    let header_bytes = 4 * std::mem::size_of::<u64>();
    if payload.len() < header_bytes {
        return Err(format!(
            "qwen3_global_weight_payload_header_too_short:key={key}:bytes={}",
            payload.len()
        ));
    }
    let magic = qwen3_dense_0_6b_payload_word(payload, 0)?;
    if magic != qwen3_dense_0_6b_global_weight_payload_magic() {
        return Err(format!(
            "qwen3_global_weight_payload_magic_mismatch:key={key}:magic={magic:#x}"
        ));
    }
    let tensor_count = qwen3_dense_0_6b_payload_word(payload, 1)? as usize;
    let mut cursor = header_bytes;
    let mut tensors = BTreeMap::new();
    for tensor_index in 0..tensor_count {
        let record_end = cursor
            .checked_add(8 * std::mem::size_of::<u64>())
            .ok_or_else(|| {
                format!("qwen3_global_weight_payload_record_end_overflow:key={key}:tensor={tensor_index}")
            })?;
        let record = payload.get(cursor..record_end).ok_or_else(|| {
            format!(
                "qwen3_global_weight_payload_record_oob:key={key}:tensor={tensor_index}:cursor={cursor}:bytes={}",
                payload.len()
            )
        })?;
        let tensor_code = qwen3_dense_0_6b_payload_word(record, 0)?;
        let dtype_code = qwen3_dense_0_6b_payload_word(record, 1)?;
        let rank = qwen3_dense_0_6b_payload_word(record, 2)?;
        let shape_checksum = qwen3_dense_0_6b_payload_word(record, 3)?;
        let payload_bytes = qwen3_dense_0_6b_payload_word(record, 4)?;
        let payload_checksum = qwen3_dense_0_6b_payload_word(record, 5)?;
        let payload_offset = qwen3_dense_0_6b_payload_word(record, 6)? as usize;
        if payload_offset != record_end {
            return Err(format!(
                "qwen3_global_weight_payload_offset_mismatch:key={key}:tensor={tensor_index}:offset={payload_offset}:expected={record_end}"
            ));
        }
        let (tensor_name, expected_shape) =
            qwen3_dense_0_6b_global_weight_tensor_name_and_shape(tensor_code)?;
        if dtype_code == 0
            || rank != expected_shape.len() as u64
            || shape_checksum != checksum_words(&expected_shape)
        {
            return Err(format!(
                "qwen3_global_weight_payload_tensor_metadata_mismatch:key={key}:tensor={tensor_name}"
            ));
        }
        let payload_end = payload_offset
            .checked_add(payload_bytes as usize)
            .ok_or_else(|| {
                format!("qwen3_global_weight_payload_end_overflow:key={key}:tensor={tensor_name}")
            })?;
        let tensor_payload = payload.get(payload_offset..payload_end).ok_or_else(|| {
            format!(
                "qwen3_global_weight_payload_oob:key={key}:tensor={tensor_name}:end={payload_end}:bytes={}",
                payload.len()
            )
        })?;
        let actual_checksum = weight_bytes_checksum(tensor_payload);
        if actual_checksum != payload_checksum {
            return Err(format!(
                "qwen3_global_weight_payload_checksum_mismatch:key={key}:tensor={tensor_name}:payload={actual_checksum:#x}:record={payload_checksum:#x}"
            ));
        }
        tensors.insert(tensor_name.to_string(), tensor_payload.to_vec());
        cursor = payload_end;
    }
    if cursor != payload.len() {
        return Err(format!(
            "qwen3_global_weight_payload_trailing_bytes:key={key}:cursor={cursor}:bytes={}",
            payload.len()
        ));
    }
    Ok(tensors)
}

fn qwen3_dense_0_6b_global_weight_tensor_name_and_shape(
    tensor_code: u64,
) -> Result<(&'static str, Vec<u64>), String> {
    let profile = QWEN3_DENSE_0_6B_PROFILE;
    match tensor_code {
        1 => Ok((
            "model.embed_tokens.weight",
            vec![profile.vocab_size, profile.hidden_size],
        )),
        2 => Ok(("model.norm.weight", vec![profile.hidden_size])),
        3 => Ok((
            "lm_head.weight",
            vec![profile.vocab_size, profile.hidden_size],
        )),
        _ => Err(format!(
            "qwen3_global_weight_payload_unknown_tensor_code:{tensor_code}"
        )),
    }
}

fn qwen3_dense_0_6b_required_layer_weight_kind_codes() -> [u64; 11] {
    [
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::InputLayerNorm),
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::QProj),
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::QNorm),
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::KProj),
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::KNorm),
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::VProj),
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::OProj),
        qwen3_dense_0_6b_weight_tensor_kind_code(
            Qwen3Dense06bWeightTensorKind::PostAttentionLayerNorm,
        ),
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::GateProj),
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::UpProj),
        qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::DownProj),
    ]
}

#[derive(Debug, Eq, PartialEq)]
struct Qwen3Dense06bReconstructedWeightPayloads {
    tensor_count: u64,
    aggregate_checksum: u64,
    tensors: BTreeMap<String, Vec<u8>>,
}

fn qwen3_dense_0_6b_reconstruct_weight_payload_tensors(
    payload: &[u8],
    view: &Qwen3Dense06bWeightObjectPayloadView,
) -> Result<Qwen3Dense06bReconstructedWeightPayloads, String> {
    let mut grouped: BTreeMap<(u64, u64), Vec<Qwen3Dense06bWeightObjectSliceView>> =
        BTreeMap::new();
    for slice in &view.slices {
        grouped
            .entry((slice.layer_id, slice.tensor_kind_code))
            .or_default()
            .push(*slice);
    }
    let mut tensor_words = Vec::with_capacity(grouped.len() * 6);
    let mut tensors = BTreeMap::new();
    for ((layer_id, kind_code), mut slices) in grouped {
        slices.sort_by_key(|slice| (slice.slice_start, slice.shard_id));
        let tensor_payload =
            qwen3_dense_0_6b_reconstruct_weight_tensor_payload(payload, kind_code, &slices)?;
        tensors.insert(
            qwen3_dense_0_6b_layer_weight_tensor_name(layer_id, kind_code)?,
            tensor_payload.clone(),
        );
        tensor_words.extend_from_slice(&[
            layer_id,
            kind_code,
            tensor_payload.len() as u64,
            weight_bytes_checksum(&tensor_payload),
            slices.len() as u64,
            checksum_words(
                &slices
                    .iter()
                    .flat_map(|slice| {
                        [
                            slice.shard_id,
                            slice.slice_axis,
                            slice.slice_start,
                            slice.slice_end,
                            slice.payload_checksum,
                        ]
                    })
                    .collect::<Vec<_>>(),
            ),
        ]);
    }
    Ok(Qwen3Dense06bReconstructedWeightPayloads {
        tensor_count: tensor_words.len() as u64 / 6,
        aggregate_checksum: checksum_words(&tensor_words),
        tensors,
    })
}

fn qwen3_dense_0_6b_reconstruct_weight_tensor_payload(
    object_payload: &[u8],
    kind_code: u64,
    slices: &[Qwen3Dense06bWeightObjectSliceView],
) -> Result<Vec<u8>, String> {
    if slices.is_empty() {
        return Err(format!(
            "qwen3_weight_payload_reconstruct_empty:kind_code={kind_code}"
        ));
    }
    let (shape, slice_axis) = qwen3_dense_0_6b_weight_kind_shape_and_axis(kind_code)?;
    let elem_size = qwen3_dense_0_6b_weight_dtype_size_code(slices[0].dtype_code)?;
    if slices
        .iter()
        .any(|slice| slice.dtype_code != slices[0].dtype_code || slice.slice_axis != slice_axis)
    {
        return Err(format!(
            "qwen3_weight_payload_reconstruct_inconsistent_slice:kind_code={kind_code}"
        ));
    }
    match slice_axis {
        u64::MAX => qwen3_dense_0_6b_slice_payload(object_payload, &slices[0]).map(Vec::from),
        0 => {
            let mut out = Vec::new();
            let mut expected_start = 0u64;
            for slice in slices {
                if slice.slice_start != expected_start {
                    return Err(format!(
                        "qwen3_weight_payload_axis0_gap:kind_code={kind_code}:got={}:expected={expected_start}",
                        slice.slice_start
                    ));
                }
                out.extend_from_slice(qwen3_dense_0_6b_slice_payload(object_payload, slice)?);
                expected_start = slice.slice_end;
            }
            if expected_start != shape[0] {
                return Err(format!(
                    "qwen3_weight_payload_axis0_incomplete:kind_code={kind_code}:end={expected_start}:rows={}",
                    shape[0]
                ));
            }
            Ok(out)
        }
        1 => {
            if shape.len() != 2 {
                return Err(format!(
                    "qwen3_weight_payload_axis1_shape_rank:kind_code={kind_code}:rank={}",
                    shape.len()
                ));
            }
            let rows = shape[0] as usize;
            let cols = shape[1];
            let mut expected_start = 0u64;
            for slice in slices {
                if slice.slice_start != expected_start {
                    return Err(format!(
                        "qwen3_weight_payload_axis1_gap:kind_code={kind_code}:got={}:expected={expected_start}",
                        slice.slice_start
                    ));
                }
                expected_start = slice.slice_end;
            }
            if expected_start != cols {
                return Err(format!(
                    "qwen3_weight_payload_axis1_incomplete:kind_code={kind_code}:end={expected_start}:cols={cols}"
                ));
            }
            let row_bytes = cols
                .checked_mul(elem_size)
                .ok_or_else(|| format!("qwen3_weight_payload_axis1_row_overflow:{kind_code}"))?
                as usize;
            let mut out = Vec::with_capacity(rows * row_bytes);
            for row in 0..rows {
                for slice in slices {
                    let slice_cols = slice.slice_end - slice.slice_start;
                    let slice_row_bytes = slice_cols.checked_mul(elem_size).ok_or_else(|| {
                        format!("qwen3_weight_payload_axis1_slice_overflow:{kind_code}")
                    })? as usize;
                    let slice_payload = qwen3_dense_0_6b_slice_payload(object_payload, slice)?;
                    let start = row.checked_mul(slice_row_bytes).ok_or_else(|| {
                        format!("qwen3_weight_payload_axis1_row_offset_overflow:{kind_code}")
                    })?;
                    let end = start.checked_add(slice_row_bytes).ok_or_else(|| {
                        format!("qwen3_weight_payload_axis1_row_end_overflow:{kind_code}")
                    })?;
                    out.extend_from_slice(slice_payload.get(start..end).ok_or_else(|| {
                        format!(
                            "qwen3_weight_payload_axis1_row_oob:kind_code={kind_code}:row={row}"
                        )
                    })?);
                }
            }
            Ok(out)
        }
        axis => Err(format!(
            "qwen3_weight_payload_reconstruct_unsupported_axis:kind_code={kind_code}:axis={axis}"
        )),
    }
}

fn qwen3_dense_0_6b_slice_payload<'a>(
    object_payload: &'a [u8],
    slice: &Qwen3Dense06bWeightObjectSliceView,
) -> Result<&'a [u8], String> {
    let start = slice.payload_offset as usize;
    let end = start
        .checked_add(slice.payload_bytes as usize)
        .ok_or_else(|| "qwen3_weight_payload_slice_end_overflow".to_string())?;
    object_payload
        .get(start..end)
        .ok_or_else(|| "qwen3_weight_payload_slice_oob".to_string())
}

fn qwen3_dense_0_6b_weight_kind_shape_and_axis(kind_code: u64) -> Result<(Vec<u64>, u64), String> {
    let profile = QWEN3_DENSE_0_6B_PROFILE;
    let hidden = profile.hidden_size;
    let q_rows = profile.num_attention_heads * profile.head_dim;
    let kv_rows = profile.num_key_value_heads * profile.head_dim;
    let intermediate = profile.intermediate_size;
    Ok(
        match qwen3_dense_0_6b_weight_tensor_kind_from_code(kind_code)? {
            Qwen3Dense06bWeightTensorKind::InputLayerNorm => (vec![hidden], u64::MAX),
            Qwen3Dense06bWeightTensorKind::QProj => (vec![q_rows, hidden], 0),
            Qwen3Dense06bWeightTensorKind::QNorm => (vec![profile.head_dim], u64::MAX),
            Qwen3Dense06bWeightTensorKind::KProj => (vec![kv_rows, hidden], 0),
            Qwen3Dense06bWeightTensorKind::KNorm => (vec![profile.head_dim], u64::MAX),
            Qwen3Dense06bWeightTensorKind::VProj => (vec![kv_rows, hidden], 0),
            Qwen3Dense06bWeightTensorKind::OProj => (vec![hidden, q_rows], 1),
            Qwen3Dense06bWeightTensorKind::PostAttentionLayerNorm => (vec![hidden], u64::MAX),
            Qwen3Dense06bWeightTensorKind::GateProj => (vec![intermediate, hidden], 0),
            Qwen3Dense06bWeightTensorKind::UpProj => (vec![intermediate, hidden], 0),
            Qwen3Dense06bWeightTensorKind::DownProj => (vec![hidden, intermediate], 1),
        },
    )
}

fn qwen3_dense_0_6b_layer_weight_tensor_name(
    layer_id: u64,
    kind_code: u64,
) -> Result<String, String> {
    let suffix = match qwen3_dense_0_6b_weight_tensor_kind_from_code(kind_code)? {
        Qwen3Dense06bWeightTensorKind::InputLayerNorm => "input_layernorm.weight",
        Qwen3Dense06bWeightTensorKind::QProj => "self_attn.q_proj.weight",
        Qwen3Dense06bWeightTensorKind::QNorm => "self_attn.q_norm.weight",
        Qwen3Dense06bWeightTensorKind::KProj => "self_attn.k_proj.weight",
        Qwen3Dense06bWeightTensorKind::KNorm => "self_attn.k_norm.weight",
        Qwen3Dense06bWeightTensorKind::VProj => "self_attn.v_proj.weight",
        Qwen3Dense06bWeightTensorKind::OProj => "self_attn.o_proj.weight",
        Qwen3Dense06bWeightTensorKind::PostAttentionLayerNorm => "post_attention_layernorm.weight",
        Qwen3Dense06bWeightTensorKind::GateProj => "mlp.gate_proj.weight",
        Qwen3Dense06bWeightTensorKind::UpProj => "mlp.up_proj.weight",
        Qwen3Dense06bWeightTensorKind::DownProj => "mlp.down_proj.weight",
    };
    Ok(format!("model.layers.{layer_id}.{suffix}"))
}

fn qwen3_dense_0_6b_weight_tensor_kind_from_code(
    kind_code: u64,
) -> Result<Qwen3Dense06bWeightTensorKind, String> {
    match kind_code {
        1 => Ok(Qwen3Dense06bWeightTensorKind::InputLayerNorm),
        2 => Ok(Qwen3Dense06bWeightTensorKind::QProj),
        3 => Ok(Qwen3Dense06bWeightTensorKind::KProj),
        4 => Ok(Qwen3Dense06bWeightTensorKind::VProj),
        5 => Ok(Qwen3Dense06bWeightTensorKind::OProj),
        6 => Ok(Qwen3Dense06bWeightTensorKind::PostAttentionLayerNorm),
        7 => Ok(Qwen3Dense06bWeightTensorKind::GateProj),
        8 => Ok(Qwen3Dense06bWeightTensorKind::UpProj),
        9 => Ok(Qwen3Dense06bWeightTensorKind::DownProj),
        10 => Ok(Qwen3Dense06bWeightTensorKind::QNorm),
        11 => Ok(Qwen3Dense06bWeightTensorKind::KNorm),
        _ => Err(format!(
            "qwen3_weight_payload_unknown_kind_code:{kind_code}"
        )),
    }
}

fn qwen3_dense_0_6b_weight_dtype_size_code(dtype_code: u64) -> Result<u64, String> {
    match dtype_code {
        1 => Ok(4),
        2 | 3 => Ok(2),
        4 | 5 => Ok(1),
        _ => Err(format!(
            "qwen3_weight_payload_unknown_dtype_code:{dtype_code}"
        )),
    }
}

fn qwen3_dense_0_6b_parse_weight_range_payload(
    range: &Qwen3Dense06bHiddenLayerNodeRange,
    key: &str,
    payload: &[u8],
    require_real_payload: bool,
) -> Result<Qwen3Dense06bWeightObjectPayloadView, String> {
    const HEADER_WORDS: usize = 5;
    const SLICE_RECORD_WORDS: usize = 11;
    if payload.len() < HEADER_WORDS * std::mem::size_of::<u64>() {
        return Err(format!(
            "qwen3_weight_object_payload_header_too_short:key={key}:bytes={}",
            payload.len()
        ));
    }
    let magic = qwen3_dense_0_6b_payload_word(payload, 0)?;
    let node_id = qwen3_dense_0_6b_payload_word(payload, 1)?;
    let first_layer_id = qwen3_dense_0_6b_payload_word(payload, 2)?;
    let last_layer_id = qwen3_dense_0_6b_payload_word(payload, 3)?;
    let slice_count = qwen3_dense_0_6b_payload_word(payload, 4)?;
    if magic != 0x5147_5733_5752_4f42 {
        return Err(format!(
            "qwen3_weight_object_payload_magic_mismatch:key={key}:magic={magic:#x}"
        ));
    }
    if node_id != range.node_id
        || first_layer_id != range.first_layer_id
        || last_layer_id != range.last_layer_id
    {
        return Err(format!(
            "qwen3_weight_object_payload_range_mismatch:key={key}:node={node_id}:layers={first_layer_id}-{last_layer_id}:expected_node={}:expected_layers={}-{}",
            range.node_id, range.first_layer_id, range.last_layer_id
        ));
    }
    if require_real_payload && slice_count == 0 {
        return Err(format!(
            "qwen3_weight_object_payload_empty_real_range:key={key}:node={node_id}:layers={first_layer_id}-{last_layer_id}"
        ));
    }
    let mut slices = Vec::with_capacity(slice_count as usize);
    let mut cursor = HEADER_WORDS * std::mem::size_of::<u64>();
    let mut slice_payload_bytes = 0u64;
    let mut slice_checksum_words = Vec::with_capacity(slice_count as usize * SLICE_RECORD_WORDS);
    for slice_index in 0..slice_count {
        let record_end = cursor
            .checked_add(SLICE_RECORD_WORDS * std::mem::size_of::<u64>())
            .ok_or_else(|| {
                format!(
                    "qwen3_weight_object_slice_record_end_overflow:key={key}:slice={slice_index}"
                )
            })?;
        if record_end > payload.len() {
            return Err(format!(
                "qwen3_weight_object_slice_record_oob:key={key}:slice={slice_index}:cursor={cursor}:bytes={}",
                payload.len()
            ));
        }
        let record = (0..SLICE_RECORD_WORDS)
            .map(|word| qwen3_dense_0_6b_payload_word_at(payload, cursor, word))
            .collect::<Result<Vec<_>, _>>()?;
        let view = Qwen3Dense06bWeightObjectSliceView {
            layer_id: record[0],
            shard_id: record[1],
            tensor_kind_code: record[2],
            dtype_code: record[3],
            slice_axis: record[4],
            slice_start: record[5],
            slice_end: record[6],
            payload_bytes: record[7],
            payload_checksum: record[8],
            payload_offset: record[9],
            local_shape_checksum: record[10],
        };
        if view.layer_id < range.first_layer_id || view.layer_id > range.last_layer_id {
            return Err(format!(
                "qwen3_weight_object_slice_layer_oob:key={key}:slice={slice_index}:layer={}:range={}-{}",
                view.layer_id, range.first_layer_id, range.last_layer_id
            ));
        }
        if view.payload_offset != record_end as u64 {
            return Err(format!(
                "qwen3_weight_object_slice_offset_mismatch:key={key}:slice={slice_index}:offset={}:expected={record_end}",
                view.payload_offset
            ));
        }
        let payload_start = view.payload_offset as usize;
        let payload_end = payload_start
            .checked_add(view.payload_bytes as usize)
            .ok_or_else(|| {
                format!(
                    "qwen3_weight_object_slice_payload_end_overflow:key={key}:slice={slice_index}"
                )
            })?;
        if payload_end > payload.len() {
            return Err(format!(
                "qwen3_weight_object_slice_payload_oob:key={key}:slice={slice_index}:end={payload_end}:bytes={}",
                payload.len()
            ));
        }
        let actual_checksum = weight_bytes_checksum(&payload[payload_start..payload_end]);
        if actual_checksum != view.payload_checksum {
            return Err(format!(
                "qwen3_weight_object_slice_checksum_mismatch:key={key}:slice={slice_index}:payload={actual_checksum:#x}:record={:#x}",
                view.payload_checksum
            ));
        }
        slice_payload_bytes = slice_payload_bytes.saturating_add(view.payload_bytes);
        slice_checksum_words.extend_from_slice(&record);
        slices.push(view);
        cursor = payload_end;
    }
    if cursor != payload.len() {
        return Err(format!(
            "qwen3_weight_object_payload_trailing_bytes:key={key}:cursor={cursor}:bytes={}",
            payload.len()
        ));
    }
    Ok(Qwen3Dense06bWeightObjectPayloadView {
        node_id,
        first_layer_id,
        last_layer_id,
        slices,
        slice_payload_bytes,
        slice_checksum: checksum_words(&slice_checksum_words),
    })
}

fn qwen3_dense_0_6b_payload_word(payload: &[u8], word_index: usize) -> Result<u64, String> {
    qwen3_dense_0_6b_payload_word_at(payload, 0, word_index)
}

fn qwen3_dense_0_6b_payload_word_at(
    payload: &[u8],
    base_offset: usize,
    word_index: usize,
) -> Result<u64, String> {
    let offset = word_index * std::mem::size_of::<u64>();
    let offset = base_offset
        .checked_add(offset)
        .ok_or_else(|| format!("qwen3_payload_word_offset_overflow:word={word_index}"))?;
    let end = offset + std::mem::size_of::<u64>();
    let bytes = payload.get(offset..end).ok_or_else(|| {
        format!(
            "qwen3_payload_word_oob:base={base_offset}:word={word_index}:bytes={}",
            payload.len()
        )
    })?;
    Ok(u64::from_le_bytes(bytes.try_into().map_err(|_| {
        format!("qwen3_payload_word_size_invalid:word={word_index}")
    })?))
}

fn qwen3_dense_0_6b_weight_tensor_kind_code(kind: Qwen3Dense06bWeightTensorKind) -> u64 {
    match kind {
        Qwen3Dense06bWeightTensorKind::InputLayerNorm => 1,
        Qwen3Dense06bWeightTensorKind::QProj => 2,
        Qwen3Dense06bWeightTensorKind::KProj => 3,
        Qwen3Dense06bWeightTensorKind::VProj => 4,
        Qwen3Dense06bWeightTensorKind::OProj => 5,
        Qwen3Dense06bWeightTensorKind::PostAttentionLayerNorm => 6,
        Qwen3Dense06bWeightTensorKind::GateProj => 7,
        Qwen3Dense06bWeightTensorKind::UpProj => 8,
        Qwen3Dense06bWeightTensorKind::DownProj => 9,
        Qwen3Dense06bWeightTensorKind::QNorm => 10,
        Qwen3Dense06bWeightTensorKind::KNorm => 11,
    }
}

fn qwen3_dense_0_6b_weight_dtype_code(dtype: qwen3_dense_0_6b::Qwen3Dense06bWeightDType) -> u64 {
    match dtype {
        qwen3_dense_0_6b::Qwen3Dense06bWeightDType::F32 => 1,
        qwen3_dense_0_6b::Qwen3Dense06bWeightDType::F16 => 2,
        qwen3_dense_0_6b::Qwen3Dense06bWeightDType::BF16 => 3,
        qwen3_dense_0_6b::Qwen3Dense06bWeightDType::I8 => 4,
        qwen3_dense_0_6b::Qwen3Dense06bWeightDType::U8 => 5,
    }
}

fn qwen3_dense_0_6b_weight_range_object_key(range: &Qwen3Dense06bHiddenLayerNodeRange) -> String {
    format!(
        "qwen3/0.6b/weights/range/node-{}/layers/{:02}-{:02}",
        range.node_id,
        range.first_layer_id,
        range.last_layer_id + 1
    )
}

fn qwen3_dense_0_6b_global_weight_object_key() -> String {
    "qwen3/0.6b/weights/global/embed-norm-lm-head".to_string()
}

fn qwen3_dense_0_6b_decode_object_service_report(
    service: &mut LingquObjectServiceStub,
    before: LingquObjectServiceReport,
    session_id: u64,
    step_index: u64,
    guest_input_checksum: u64,
    selected_samples: &[Qwen3Dense06bTextOutputSample],
    text_output: &Qwen3Dense06bTextOutputReport,
    hidden_layer_pipeline: &Qwen3Dense06bHiddenLayerPipelineReport,
    real_kv_cache: Option<&[Qwen3Dense06bLayerKvCache]>,
    runtime_weight_resolve_count: u64,
) -> Result<Qwen3Dense06bObjectServiceReport, String> {
    let base = format!("qwen3/session/{session_id:016x}/step/{step_index:08}");
    let kv_index_key = qwen3_dense_0_6b_kv_index_object_key(session_id, step_index);
    let token_checksum = checksum_words(
        &selected_samples
            .iter()
            .map(|sample| sample.sampled_token)
            .collect::<Vec<_>>(),
    );
    let kv_checksum = checksum_words(&[
        text_output.kvcache.descriptor_count,
        text_output.kvcache.state_count,
        text_output.kvcache.update_seq_sum,
        text_output.kvcache.read_digest_checksum,
    ]);
    let final_hidden_payload = hidden_layer_pipeline
        .layer_executions
        .last()
        .map(|execution| execution.output_tensor_payload.clone())
        .unwrap_or_else(|| {
            qwen3_dense_0_6b_object_payload_words(&[
                hidden_layer_pipeline.last_layer_id,
                hidden_layer_pipeline.final_layer_checksum,
                hidden_layer_pipeline.hidden_tensor_byte_count,
            ])
        });
    let final_hidden_payload_checksum =
        qwen3_dense_0_6b_shard_output_checksum(&final_hidden_payload);
    let logits_checksum = text_output
        .real_logits
        .as_ref()
        .map(|logits| logits.aggregate_checksum)
        .unwrap_or(text_output.logits_checksum);
    let kv_payload = qwen3_dense_0_6b_kv_state_payload_bytes_with_real_cache(
        &text_output.kvcache.state_snapshots,
        real_kv_cache,
    );
    let mut objects = vec![
        (
            format!("{base}/tokens/input"),
            LingquObjectKind::TokenBuffer,
            LingquPayloadBackend::Inline,
            text_output.guest_input.byte_len.max(1),
            guest_input_checksum,
            qwen3_dense_0_6b_object_payload_words(&[
                guest_input_checksum,
                text_output.guest_input.byte_len,
            ]),
        ),
        (
            format!("{base}/tokens/sample"),
            LingquObjectKind::TokenBuffer,
            LingquPayloadBackend::Inline,
            (selected_samples.len() as u64).saturating_mul(8).max(1),
            token_checksum,
            qwen3_dense_0_6b_object_payload_words(
                &selected_samples
                    .iter()
                    .map(|sample| sample.sampled_token)
                    .collect::<Vec<_>>(),
            ),
        ),
        (
            kv_index_key.clone(),
            LingquObjectKind::KvCacheBlock,
            LingquPayloadBackend::Shmem,
            (kv_payload.len() as u64).max(1),
            kv_checksum,
            kv_payload,
        ),
        (
            format!("{base}/hidden/final"),
            LingquObjectKind::RuntimeTensor,
            LingquPayloadBackend::Shmem,
            final_hidden_payload.len() as u64,
            final_hidden_payload_checksum,
            final_hidden_payload,
        ),
        (
            format!("{base}/logits/full_vocab"),
            LingquObjectKind::Logits,
            LingquPayloadBackend::Block,
            text_output
                .real_logits
                .as_ref()
                .map(|logits| logits.row_byte_count)
                .unwrap_or(0)
                .max(1),
            logits_checksum,
            qwen3_dense_0_6b_object_payload_words(&[
                logits_checksum,
                text_output.logits_checksum,
                text_output
                    .real_logits
                    .as_ref()
                    .map(|logits| logits.token_count)
                    .unwrap_or(0),
            ]),
        ),
    ];
    for range in &hidden_layer_pipeline.node_ranges {
        let range_executions = hidden_layer_pipeline
            .layer_executions
            .iter()
            .filter(|execution| {
                execution.layer_id >= range.first_layer_id
                    && execution.layer_id <= range.last_layer_id
                    && execution.owner_node == range.node_id
            })
            .collect::<Vec<_>>();
        let Some(first_execution) = range_executions.first() else {
            continue;
        };
        let Some(last_execution) = range_executions.last() else {
            continue;
        };
        objects.push((
            format!(
                "{base}/hidden/node-{}/layers/{:02}-{:02}/input",
                range.node_id, range.first_layer_id, range.last_layer_id
            ),
            LingquObjectKind::RuntimeTensor,
            LingquPayloadBackend::Shmem,
            first_execution.input_tensor_payload.len() as u64,
            first_execution.input_tensor_checksum,
            first_execution.input_tensor_payload.clone(),
        ));
        objects.push((
            format!(
                "{base}/hidden/node-{}/layers/{:02}-{:02}/output",
                range.node_id, range.first_layer_id, range.last_layer_id
            ),
            LingquObjectKind::RuntimeTensor,
            LingquPayloadBackend::Shmem,
            last_execution.output_tensor_payload.len() as u64,
            last_execution.output_tensor_checksum,
            last_execution.output_tensor_payload.clone(),
        ));
    }
    let object_count = objects.len() as u64;
    let token_object_count = objects
        .iter()
        .filter(|(_, kind, _, _, _, _)| *kind == LingquObjectKind::TokenBuffer)
        .count() as u64;
    let kv_object_count = objects
        .iter()
        .filter(|(_, kind, _, _, _, _)| *kind == LingquObjectKind::KvCacheBlock)
        .count() as u64;
    let weight_object_count = objects
        .iter()
        .filter(|(_, kind, _, _, _, _)| *kind == LingquObjectKind::WeightShard)
        .count() as u64;
    let runtime_tensor_object_count = objects
        .iter()
        .filter(|(_, kind, _, _, _, _)| *kind == LingquObjectKind::RuntimeTensor)
        .count() as u64;
    let logits_object_count = objects
        .iter()
        .filter(|(_, kind, _, _, _, _)| *kind == LingquObjectKind::Logits)
        .count() as u64;
    for (index, (key, kind, backend, bytes, checksum, payload_bytes)) in objects.iter().enumerate()
    {
        if *kind == LingquObjectKind::KvCacheBlock && step_index != 0 {
            service
                .submit_append(
                    LingquObjectAppendReq {
                        task: None,
                        base_key: qwen3_dense_0_6b_kv_index_base_key(session_id),
                        suffix: format!("step/{step_index:08}"),
                        kind: *kind,
                        producer_entity: (index % QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize)
                            as u64,
                        owner_entity: None,
                        previous_version: None,
                        metadata: qwen3_dense_0_6b_object_metadata(*bytes, *checksum),
                        placements: vec![qwen3_dense_0_6b_object_placement(
                            key, *backend, *bytes, *checksum,
                        )],
                        payload_bytes: payload_bytes.clone(),
                    },
                    step_index.saturating_mul(1000) + index as u64,
                )
                .map_err(|err| format!("qwen3_object_append_failed:{err}"))?;
        } else {
            service
                .submit_publish(
                    LingquObjectPublishReq {
                        task: None,
                        key: key.clone(),
                        kind: *kind,
                        producer_entity: (index % QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize)
                            as u64,
                        owner_entity: None,
                        expected_version: None,
                        metadata: qwen3_dense_0_6b_object_metadata(*bytes, *checksum),
                        placements: vec![qwen3_dense_0_6b_object_placement(
                            key, *backend, *bytes, *checksum,
                        )],
                        payload_bytes: payload_bytes.clone(),
                    },
                    step_index.saturating_mul(1000) + index as u64,
                )
                .map_err(|err| format!("qwen3_object_publish_failed:{err}"))?;
        }
    }
    for (index, (key, _, backend, _, _, _)) in objects.iter().enumerate() {
        service
            .submit_resolve(
                LingquObjectResolveReq {
                    task: None,
                    key: key.clone(),
                    requester_entity: ((index + 1) % QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize)
                        as u64,
                    version: LingquObjectVersionSelector::LatestCommitted,
                    min_state: LingquObjectState::Committed,
                    preferred_backends: vec![*backend],
                },
                step_index.saturating_mul(1000) + 100 + index as u64,
            )
            .map_err(|err| format!("qwen3_object_resolve_failed:{err}"))?;
    }
    let resolved_weight_objects = qwen3_dense_0_6b_resolve_weight_range_objects(
        service,
        &hidden_layer_pipeline.node_ranges,
        step_index.saturating_mul(1000) + 200,
    )?;
    let completions = service.poll_ready(step_index.saturating_mul(1000) + 1000);
    let ready = completions
        .iter()
        .all(|event| event.status == CompletionStatus::Success);
    qwen3_dense_0_6b_validate_hidden_range_handoff_objects(service, &base, hidden_layer_pipeline)?;
    let after = service.report();
    let (
        global_weight_object_count,
        global_weight_payload_bytes,
        global_weight_tensor_count,
        global_weight_payload_checksum,
    ) = qwen3_dense_0_6b_inspect_global_weight_object(service)?;
    let report = qwen3_dense_0_6b_object_service_delta_report(
        before,
        after,
        ready,
        object_count,
        token_object_count,
        kv_object_count,
        weight_object_count + resolved_weight_objects.object_count,
        runtime_weight_resolve_count,
        resolved_weight_objects.payload_bytes,
        resolved_weight_objects.slice_count,
        resolved_weight_objects.payload_complete,
        resolved_weight_objects.reconstructed_tensor_count,
        resolved_weight_objects.reconstructed_tensor_checksum,
        checksum_words(&[
            resolved_weight_objects.payload_checksum,
            resolved_weight_objects.slice_count,
            resolved_weight_objects.slice_payload_bytes,
            u64::from(resolved_weight_objects.payload_complete),
            resolved_weight_objects.reconstructed_tensor_count,
            resolved_weight_objects.reconstructed_tensor_checksum,
            resolved_weight_objects.slice_checksum,
        ]),
        global_weight_object_count,
        global_weight_payload_bytes,
        global_weight_tensor_count,
        global_weight_payload_checksum,
        runtime_tensor_object_count,
        logits_object_count,
    );
    if !report.ready {
        return Err("qwen3_object_service_completion_failed".to_string());
    }
    Ok(report)
}

fn qwen3_dense_0_6b_validate_hidden_range_handoff_objects(
    service: &LingquObjectServiceStub,
    base: &str,
    hidden_layer_pipeline: &Qwen3Dense06bHiddenLayerPipelineReport,
) -> Result<(), String> {
    for pair in hidden_layer_pipeline.node_ranges.windows(2) {
        let local = &pair[0];
        let next = &pair[1];
        let local_output_key = format!(
            "{base}/hidden/node-{}/layers/{:02}-{:02}/output",
            local.node_id, local.first_layer_id, local.last_layer_id
        );
        let next_input_key = format!(
            "{base}/hidden/node-{}/layers/{:02}-{:02}/input",
            next.node_id, next.first_layer_id, next.last_layer_id
        );
        let local_output_checksum =
            qwen3_dense_0_6b_hidden_range_object_checksum(service, &local_output_key)?;
        let next_input_checksum =
            qwen3_dense_0_6b_hidden_range_object_checksum(service, &next_input_key)?;
        if local_output_checksum != next_input_checksum {
            return Err(format!(
                "qwen3_hidden_range_handoff_checksum_mismatch:output_key={local_output_key}:input_key={next_input_key}:output={local_output_checksum:#x}:input={next_input_checksum:#x}"
            ));
        }
    }
    Ok(())
}

fn qwen3_dense_0_6b_hidden_range_object_checksum(
    service: &LingquObjectServiceStub,
    key: &str,
) -> Result<u64, String> {
    let record = service
        .latest_record(key)
        .ok_or_else(|| format!("qwen3_hidden_range_object_missing:{key}"))?;
    if record.kind != LingquObjectKind::RuntimeTensor {
        return Err(format!(
            "qwen3_hidden_range_object_kind_mismatch:key={key}:kind={:?}",
            record.kind
        ));
    }
    let payload = service
        .get_copy(key, LingquObjectVersionSelector::Exact(record.version))
        .ok_or_else(|| format!("qwen3_hidden_range_object_payload_missing:{key}"))?;
    if payload.len() as u64 != record.bytes {
        return Err(format!(
            "qwen3_hidden_range_object_payload_size_mismatch:key={key}:payload={} record={}",
            payload.len(),
            record.bytes
        ));
    }
    let payload_checksum = qwen3_dense_0_6b_shard_output_checksum(&payload);
    if payload_checksum != record.checksum {
        return Err(format!(
            "qwen3_hidden_range_object_payload_checksum_mismatch:key={key}:payload={payload_checksum:#x}:record={:#x}",
            record.checksum
        ));
    }
    Ok(payload_checksum)
}

fn qwen3_dense_0_6b_object_metadata(bytes: u64, checksum: u64) -> LingquObjectMetadata {
    LingquObjectMetadata {
        bytes,
        checksum,
        dtype: None,
        shape: vec![bytes],
        layout: None,
        expires_at_us: None,
    }
}

fn qwen3_dense_0_6b_object_payload_words(words: &[u64]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(words.len() * std::mem::size_of::<u64>());
    for word in words {
        bytes.extend_from_slice(&word.to_le_bytes());
    }
    bytes
}

fn qwen3_dense_0_6b_f32_payload_bytes(values: &[f32]) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(values.len() * std::mem::size_of::<f32>());
    for value in values {
        bytes.extend_from_slice(&value.to_bits().to_le_bytes());
    }
    bytes
}

fn qwen3_dense_0_6b_publish_runtime_tensor_object(
    service: &mut LingquObjectServiceStub,
    key: &str,
    producer_entity: u64,
    payload: &[u8],
    checksum: u64,
    event_id: u64,
) -> Result<(), String> {
    service
        .submit_publish(
            LingquObjectPublishReq {
                task: None,
                key: key.to_string(),
                kind: LingquObjectKind::RuntimeTensor,
                producer_entity,
                owner_entity: Some(producer_entity),
                expected_version: None,
                metadata: qwen3_dense_0_6b_object_metadata(payload.len() as u64, checksum),
                placements: vec![qwen3_dense_0_6b_object_placement(
                    key,
                    LingquPayloadBackend::Shmem,
                    payload.len() as u64,
                    checksum,
                )],
                payload_bytes: payload.to_vec(),
            },
            event_id,
        )
        .map_err(|err| format!("qwen3_runtime_tensor_publish_failed:{key}:{err}"))?;
    let completions = service.poll_ready(event_id + 1);
    if completions
        .iter()
        .any(|event| event.status != CompletionStatus::Success)
    {
        return Err(format!(
            "qwen3_runtime_tensor_publish_completion_failed:{key}"
        ));
    }
    Ok(())
}

fn qwen3_dense_0_6b_resolve_runtime_tensor_object(
    service: &mut LingquObjectServiceStub,
    key: &str,
    requester_entity: u64,
    expected_checksum: u64,
    event_id: u64,
) -> Result<Vec<u8>, String> {
    service
        .submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: key.to_string(),
                requester_entity,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![LingquPayloadBackend::Shmem],
            },
            event_id,
        )
        .map_err(|err| format!("qwen3_runtime_tensor_resolve_failed:{key}:{err}"))?;
    let record = service
        .latest_record(key)
        .ok_or_else(|| format!("qwen3_runtime_tensor_missing:{key}"))?;
    if record.kind != LingquObjectKind::RuntimeTensor {
        return Err(format!(
            "qwen3_runtime_tensor_kind_mismatch:key={key}:kind={:?}",
            record.kind
        ));
    }
    let payload = service
        .get_copy(key, LingquObjectVersionSelector::Exact(record.version))
        .ok_or_else(|| format!("qwen3_runtime_tensor_payload_missing:{key}"))?;
    let payload_checksum = weight_bytes_checksum(&payload);
    if payload_checksum != expected_checksum || payload_checksum != record.checksum {
        return Err(format!(
            "qwen3_runtime_tensor_checksum_mismatch:key={key}:payload={payload_checksum:#x}:expected={expected_checksum:#x}:record={:#x}",
            record.checksum
        ));
    }
    let completions = service.poll_ready(event_id + 1);
    if completions
        .iter()
        .any(|event| event.status != CompletionStatus::Success)
    {
        return Err(format!(
            "qwen3_runtime_tensor_resolve_completion_failed:{key}"
        ));
    }
    Ok(payload)
}

fn qwen3_dense_0_6b_resolve_runtime_hidden_tensor_object(
    service: &mut LingquObjectServiceStub,
    key: &str,
    requester_entity: u64,
    expected_checksum: u64,
    event_id: u64,
) -> Result<Vec<u8>, String> {
    service
        .submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: key.to_string(),
                requester_entity,
                version: LingquObjectVersionSelector::LatestCommitted,
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![LingquPayloadBackend::Shmem],
            },
            event_id,
        )
        .map_err(|err| format!("qwen3_runtime_hidden_resolve_failed:{key}:{err}"))?;
    let record = service
        .latest_record(key)
        .ok_or_else(|| format!("qwen3_runtime_hidden_missing:{key}"))?;
    if record.kind != LingquObjectKind::RuntimeTensor {
        return Err(format!(
            "qwen3_runtime_hidden_kind_mismatch:key={key}:kind={:?}",
            record.kind
        ));
    }
    let payload = service
        .get_copy(key, LingquObjectVersionSelector::Exact(record.version))
        .ok_or_else(|| format!("qwen3_runtime_hidden_payload_missing:{key}"))?;
    let payload_checksum = qwen3_dense_0_6b_shard_output_checksum(&payload);
    if payload_checksum != expected_checksum || payload_checksum != record.checksum {
        return Err(format!(
            "qwen3_runtime_hidden_checksum_mismatch:key={key}:payload={payload_checksum:#x}:expected={expected_checksum:#x}:record={:#x}",
            record.checksum
        ));
    }
    let completions = service.poll_ready(event_id + 1);
    if completions
        .iter()
        .any(|event| event.status != CompletionStatus::Success)
    {
        return Err(format!(
            "qwen3_runtime_hidden_resolve_completion_failed:{key}"
        ));
    }
    Ok(payload)
}

fn qwen3_dense_0_6b_kv_state_payload_bytes(
    snapshots: &[Qwen3Dense06bKvCacheStateSnapshot],
) -> Vec<u8> {
    let mut words = Vec::with_capacity(1 + snapshots.len() * 7);
    words.push(snapshots.len() as u64);
    for snapshot in snapshots {
        words.extend_from_slice(&[
            snapshot.layer_id,
            snapshot.tile_id,
            snapshot.position,
            snapshot.update_seq,
            snapshot.k_checksum,
            snapshot.v_checksum,
            snapshot.read_window_end,
            snapshot.read_digest,
        ]);
    }
    qwen3_dense_0_6b_object_payload_words(&words)
}

fn qwen3_dense_0_6b_kv_state_payload_bytes_with_real_cache(
    snapshots: &[Qwen3Dense06bKvCacheStateSnapshot],
    real_cache: Option<&[Qwen3Dense06bLayerKvCache]>,
) -> Vec<u8> {
    let mut bytes = qwen3_dense_0_6b_kv_state_payload_bytes(snapshots);
    if let Some(real_cache) = real_cache {
        qwen3_dense_0_6b_append_real_kv_cache_payload(&mut bytes, real_cache);
    }
    bytes
}

fn qwen3_dense_0_6b_kv_state_payload_from_bytes(
    bytes: &[u8],
) -> Result<Vec<Qwen3Dense06bKvCacheStateSnapshot>, String> {
    if bytes.len() < std::mem::size_of::<u64>() || bytes.len() % std::mem::size_of::<u64>() != 0 {
        return Err(format!(
            "qwen3_kv_object_payload_size_invalid:{}",
            bytes.len()
        ));
    }
    let mut words = Vec::with_capacity(bytes.len() / std::mem::size_of::<u64>());
    for chunk in bytes.chunks_exact(std::mem::size_of::<u64>()) {
        words.push(u64::from_le_bytes(
            chunk
                .try_into()
                .map_err(|_| "qwen3_kv_object_payload_word_invalid".to_string())?,
        ));
    }
    let count = words[0] as usize;
    let expected = 1 + count * 8;
    if words.len() < expected {
        return Err(format!(
            "qwen3_kv_object_payload_count_mismatch:count={count}:words={}:expected={expected}",
            words.len()
        ));
    }
    let mut snapshots = Vec::with_capacity(count);
    for index in 0..count {
        let base = 1 + index * 8;
        snapshots.push(Qwen3Dense06bKvCacheStateSnapshot {
            layer_id: words[base],
            tile_id: words[base + 1],
            position: words[base + 2],
            update_seq: words[base + 3],
            k_checksum: words[base + 4],
            v_checksum: words[base + 5],
            read_window_end: words[base + 6],
            read_digest: words[base + 7],
        });
    }
    Ok(snapshots)
}

const QWEN3_REAL_KV_CACHE_MARKER: u64 = 0x4c41_4552_564b_3351;

fn qwen3_dense_0_6b_append_real_kv_cache_payload(
    bytes: &mut Vec<u8>,
    real_cache: &[Qwen3Dense06bLayerKvCache],
) {
    qwen3_dense_0_6b_push_u64(bytes, QWEN3_REAL_KV_CACHE_MARKER);
    qwen3_dense_0_6b_push_u64(bytes, real_cache.len() as u64);
    for layer in real_cache {
        qwen3_dense_0_6b_push_u64(bytes, layer.layer_id);
        qwen3_dense_0_6b_push_u64(bytes, layer.token_count);
        qwen3_dense_0_6b_push_u64(bytes, layer.rope_k_states.len() as u64);
        qwen3_dense_0_6b_push_u64(bytes, layer.v_states.len() as u64);
        let state_len = layer
            .rope_k_states
            .first()
            .map(|state| state.len())
            .or_else(|| layer.v_states.first().map(|state| state.len()))
            .unwrap_or(0);
        qwen3_dense_0_6b_push_u64(bytes, state_len as u64);
        for state in &layer.rope_k_states {
            for value in state {
                bytes.extend_from_slice(&value.to_le_bytes());
            }
        }
        for state in &layer.v_states {
            for value in state {
                bytes.extend_from_slice(&value.to_le_bytes());
            }
        }
    }
}

fn qwen3_dense_0_6b_push_u64(bytes: &mut Vec<u8>, value: u64) {
    bytes.extend_from_slice(&value.to_le_bytes());
}

fn qwen3_dense_0_6b_read_u64_from_payload(bytes: &[u8], offset: &mut usize) -> Result<u64, String> {
    let end = offset
        .checked_add(std::mem::size_of::<u64>())
        .ok_or_else(|| "qwen3_real_kv_payload_offset_overflow".to_string())?;
    let chunk = bytes
        .get(*offset..end)
        .ok_or_else(|| "qwen3_real_kv_payload_u64_oob".to_string())?;
    *offset = end;
    Ok(u64::from_le_bytes(chunk.try_into().map_err(|_| {
        "qwen3_real_kv_payload_u64_invalid".to_string()
    })?))
}

fn qwen3_dense_0_6b_read_f32_vec_from_payload(
    bytes: &[u8],
    offset: &mut usize,
    len: usize,
) -> Result<Vec<f32>, String> {
    let byte_len = len
        .checked_mul(std::mem::size_of::<f32>())
        .ok_or_else(|| "qwen3_real_kv_payload_f32_len_overflow".to_string())?;
    let end = offset
        .checked_add(byte_len)
        .ok_or_else(|| "qwen3_real_kv_payload_f32_offset_overflow".to_string())?;
    let slice = bytes
        .get(*offset..end)
        .ok_or_else(|| "qwen3_real_kv_payload_f32_oob".to_string())?;
    *offset = end;
    Ok(bytes_to_f32s(slice))
}

fn qwen3_dense_0_6b_real_kv_cache_from_payload(
    bytes: &[u8],
) -> Result<Option<Vec<Qwen3Dense06bLayerKvCache>>, String> {
    let snapshots = qwen3_dense_0_6b_kv_state_payload_from_bytes(bytes)?;
    let mut offset = std::mem::size_of::<u64>() * (1 + snapshots.len() * 8);
    if offset == bytes.len() {
        return Ok(None);
    }
    let marker = qwen3_dense_0_6b_read_u64_from_payload(bytes, &mut offset)?;
    if marker != QWEN3_REAL_KV_CACHE_MARKER {
        return Err(format!("qwen3_real_kv_payload_marker_mismatch:{marker:#x}"));
    }
    let layer_count = qwen3_dense_0_6b_read_u64_from_payload(bytes, &mut offset)? as usize;
    let mut cache = Vec::with_capacity(layer_count);
    for _ in 0..layer_count {
        let layer_id = qwen3_dense_0_6b_read_u64_from_payload(bytes, &mut offset)?;
        let token_count = qwen3_dense_0_6b_read_u64_from_payload(bytes, &mut offset)?;
        let k_state_count = qwen3_dense_0_6b_read_u64_from_payload(bytes, &mut offset)? as usize;
        let v_state_count = qwen3_dense_0_6b_read_u64_from_payload(bytes, &mut offset)? as usize;
        let state_len = qwen3_dense_0_6b_read_u64_from_payload(bytes, &mut offset)? as usize;
        if k_state_count != token_count as usize || v_state_count != token_count as usize {
            return Err(format!(
                "qwen3_real_kv_payload_token_count_mismatch:layer={layer_id}:tokens={token_count}:k={k_state_count}:v={v_state_count}"
            ));
        }
        let mut rope_k_states = Vec::with_capacity(k_state_count);
        for _ in 0..k_state_count {
            rope_k_states.push(qwen3_dense_0_6b_read_f32_vec_from_payload(
                bytes,
                &mut offset,
                state_len,
            )?);
        }
        let mut v_states = Vec::with_capacity(v_state_count);
        for _ in 0..v_state_count {
            v_states.push(qwen3_dense_0_6b_read_f32_vec_from_payload(
                bytes,
                &mut offset,
                state_len,
            )?);
        }
        cache.push(Qwen3Dense06bLayerKvCache {
            layer_id,
            token_count,
            rope_k_states,
            v_states,
        });
    }
    if offset != bytes.len() {
        return Err(format!(
            "qwen3_real_kv_payload_trailing_bytes:{}",
            bytes.len() - offset
        ));
    }
    Ok(Some(cache))
}

fn qwen3_dense_0_6b_kv_index_base_key(session_id: u64) -> String {
    format!("qwen3/session/{session_id:016x}/kv")
}

fn qwen3_dense_0_6b_kv_index_object_key(session_id: u64, step_index: u64) -> String {
    format!(
        "{}/step/{step_index:08}",
        qwen3_dense_0_6b_kv_index_base_key(session_id)
    )
}

fn qwen3_dense_0_6b_object_placement(
    key: &str,
    backend: LingquPayloadBackend,
    bytes: u64,
    checksum: u64,
) -> LingquPayloadPlacement {
    LingquPayloadPlacement {
        backend,
        storage_ref: format!("{key}/payload"),
        segment: None,
        offset: 0,
        bytes,
        checksum,
        locality: LingquObjectLocality::DomainShared(0),
    }
}

fn qwen3_dense_0_6b_object_service_delta_report(
    before: LingquObjectServiceReport,
    after: LingquObjectServiceReport,
    ready: bool,
    object_count: u64,
    token_object_count: u64,
    kv_object_count: u64,
    weight_object_count: u64,
    extra_non_kv_resolve_count: u64,
    weight_payload_bytes: u64,
    weight_payload_slice_count: u64,
    weight_payload_complete: bool,
    weight_reconstructed_tensor_count: u64,
    weight_reconstructed_tensor_checksum: u64,
    weight_payload_checksum: u64,
    global_weight_object_count: u64,
    global_weight_payload_bytes: u64,
    global_weight_tensor_count: u64,
    global_weight_payload_checksum: u64,
    runtime_tensor_object_count: u64,
    logits_object_count: u64,
) -> Qwen3Dense06bObjectServiceReport {
    let publish_count = after.publish_count.saturating_sub(before.publish_count);
    let resolve_count = after.resolve_count.saturating_sub(before.resolve_count);
    Qwen3Dense06bObjectServiceReport {
        ready,
        publish_count,
        resolve_count,
        append_count: after.append_count.saturating_sub(before.append_count),
        kv_index_resolve_count: resolve_count
            .saturating_sub(object_count + weight_object_count + extra_non_kv_resolve_count),
        kv_index_append_count: after.append_count.saturating_sub(before.append_count),
        metadata_put_count: after
            .metadata_put_count
            .saturating_sub(before.metadata_put_count),
        metadata_get_count: after
            .metadata_get_count
            .saturating_sub(before.metadata_get_count),
        shmem_write_count: after
            .shmem_write_count
            .saturating_sub(before.shmem_write_count),
        shmem_read_count: after
            .shmem_read_count
            .saturating_sub(before.shmem_read_count),
        block_write_count: after
            .block_write_count
            .saturating_sub(before.block_write_count),
        block_read_count: after
            .block_read_count
            .saturating_sub(before.block_read_count),
        inline_write_count: after
            .inline_write_count
            .saturating_sub(before.inline_write_count),
        inline_read_count: after
            .inline_read_count
            .saturating_sub(before.inline_read_count),
        obmm_pool_enabled: after.obmm_pool_enabled,
        obmm_pool_payload_write_count: after
            .obmm_pool_payload_write_count
            .saturating_sub(before.obmm_pool_payload_write_count),
        obmm_pool_payload_read_count: after
            .obmm_pool_payload_read_count
            .saturating_sub(before.obmm_pool_payload_read_count),
        obmm_pool_queue_submit_count: after
            .obmm_pool_queue_submit_count
            .saturating_sub(before.obmm_pool_queue_submit_count),
        obmm_pool_queue_deliver_count: after
            .obmm_pool_queue_deliver_count
            .saturating_sub(before.obmm_pool_queue_deliver_count),
        obmm_pool_bytes_used: after.obmm_pool_bytes_used,
        committed_object_count: after
            .committed_object_count
            .saturating_sub(before.committed_object_count),
        missing_resolve_count: after
            .missing_resolve_count
            .saturating_sub(before.missing_resolve_count),
        token_objects: token_object_count.min(publish_count),
        kv_objects: kv_object_count.min(publish_count),
        weight_objects: weight_object_count,
        weight_payload_bytes,
        weight_payload_slice_count,
        weight_payload_complete,
        weight_reconstructed_tensor_count,
        weight_reconstructed_tensor_checksum,
        weight_payload_checksum,
        global_weight_object_count,
        global_weight_payload_bytes,
        global_weight_tensor_count,
        global_weight_payload_checksum,
        runtime_tensor_objects: runtime_tensor_object_count.min(publish_count),
        logits_objects: logits_object_count.min(publish_count),
        object_checksum: after.checksum,
    }
}

#[derive(Clone, Debug)]
struct Qwen3Dense06bIncrementalDecodeState {
    last_text_output: Qwen3Dense06bTextOutputReport,
    cache_position: u64,
    cache_update_seq: u64,
    cache_digest: u64,
    last_sampled_token_checksum: u64,
    cache_states: Vec<Qwen3Dense06bKvCacheStateSnapshot>,
    real_kv_cache: Option<Vec<Qwen3Dense06bLayerKvCache>>,
    kv_object_key: String,
    kv_object_version: u64,
    kv_object_checksum: u64,
}

impl Qwen3Dense06bIncrementalDecodeState {
    fn from_step(
        text_output: &Qwen3Dense06bTextOutputReport,
        next_guest_input: &[u8],
        selected_samples: &[Qwen3Dense06bTextOutputSample],
        loop_step: u64,
        session_id: u64,
        object_service: &LingquObjectServiceStub,
        expected_real_kv_cache: Option<Vec<Qwen3Dense06bLayerKvCache>>,
    ) -> Result<Self, String> {
        let cache_position = qwen3_dense_0_6b_guest_input_token_count(next_guest_input)
            .saturating_sub(selected_samples.len() as u64);
        let last_sampled_token_checksum = checksum_words(
            &selected_samples
                .iter()
                .map(|sample| sample.sampled_token)
                .collect::<Vec<_>>(),
        );
        let cache_digest = checksum_words(&[
            text_output.kvcache.read_digest_checksum,
            cache_position,
            loop_step,
            last_sampled_token_checksum,
        ]);
        let kv_object_key = qwen3_dense_0_6b_kv_index_object_key(session_id, loop_step);
        let kv_record = object_service
            .latest_record(&kv_object_key)
            .ok_or_else(|| format!("qwen3_kv_object_missing_after_publish:{kv_object_key}"))?;
        if kv_record.kind != LingquObjectKind::KvCacheBlock {
            return Err(format!(
                "qwen3_kv_object_kind_mismatch:key={kv_object_key}:kind={:?}",
                kv_record.kind
            ));
        }
        if kv_record.checksum != text_output.kvcache.read_digest_checksum
            && kv_record.checksum
                != checksum_words(&[
                    text_output.kvcache.descriptor_count,
                    text_output.kvcache.state_count,
                    text_output.kvcache.update_seq_sum,
                    text_output.kvcache.read_digest_checksum,
                ])
        {
            return Err(format!(
                "qwen3_kv_object_checksum_mismatch:key={kv_object_key}:object={:#x}:read={:#x}",
                kv_record.checksum, text_output.kvcache.read_digest_checksum
            ));
        }
        let cache_states = object_service
            .get_copy(
                &kv_object_key,
                LingquObjectVersionSelector::Exact(kv_record.version),
            )
            .ok_or_else(|| format!("qwen3_kv_object_payload_missing:{kv_object_key}"))
            .and_then(|payload| qwen3_dense_0_6b_kv_state_payload_from_bytes(&payload))?;
        if cache_states != text_output.kvcache.state_snapshots {
            return Err(format!(
                "qwen3_kv_object_payload_mismatch:key={kv_object_key}:payload_states={}:report_states={}",
                cache_states.len(),
                text_output.kvcache.state_snapshots.len()
            ));
        }
        let payload = object_service
            .get_copy(
                &kv_object_key,
                LingquObjectVersionSelector::Exact(kv_record.version),
            )
            .ok_or_else(|| format!("qwen3_kv_object_payload_missing:{kv_object_key}"))?;
        let real_kv_cache = qwen3_dense_0_6b_real_kv_cache_from_payload(&payload)?;
        if expected_real_kv_cache.is_some() && real_kv_cache != expected_real_kv_cache {
            return Err(format!(
                "qwen3_real_kv_object_payload_mismatch:key={kv_object_key}:expected_real={}:actual_real={}",
                expected_real_kv_cache.as_ref().map(|cache| cache.len()).unwrap_or(0),
                real_kv_cache.as_ref().map(|cache| cache.len()).unwrap_or(0)
            ));
        }
        Ok(Self {
            last_text_output: text_output.clone(),
            cache_position,
            cache_update_seq: text_output.kvcache.update_seq_sum.max(1),
            cache_digest,
            last_sampled_token_checksum,
            cache_states,
            real_kv_cache,
            kv_object_key,
            kv_object_version: kv_record.version,
            kv_object_checksum: kv_record.checksum,
        })
    }
}

fn qwen3_dense_0_6b_incremental_decode_text_output_report(
    state: &Qwen3Dense06bIncrementalDecodeState,
    guest_input: &[u8],
    guest_input_report: Qwen3Dense06bGuestInputReport,
    loop_step: u64,
    object_service: &mut LingquObjectServiceStub,
) -> Result<
    (
        Qwen3Dense06bTextOutputReport,
        Option<Vec<Qwen3Dense06bLayerKvCache>>,
    ),
    String,
> {
    let token_ids = qwen3_dense_0_6b_guest_input_token_ids(guest_input);
    let position = token_ids
        .len()
        .checked_sub(1)
        .map(|position| position as u64)
        .unwrap_or(state.cache_position);
    qwen3_dense_0_6b_resolve_incremental_kv_index(object_service, state, loop_step)?;
    let (forward, weight_payloads, real_kv_cache) =
        qwen3_dense_0_6b_runtime_incremental_forward_summary_from_guest_input(
            state,
            guest_input,
            object_service,
            loop_step.saturating_mul(1000) + 300,
        )?;
    let full_vocab = match forward.as_ref() {
        Some(forward) => qwen3_dense_0_6b_runtime_full_vocab_logits_summary(
            Some(&forward.final_hidden),
            Some(&weight_payloads),
        )?,
        None => None,
    };
    let tokenizer_path = qwen3_dense_0_6b_real_tokenizer_path();
    let sample = qwen3_dense_0_6b_incremental_decode_sample(
        state,
        loop_step,
        position,
        forward.as_ref(),
        full_vocab.as_ref(),
        tokenizer_path.as_deref(),
    )?;
    let raw_piece =
        qwen3_dense_0_6b_token_piece_raw_bytes(sample.sampled_token, tokenizer_path.as_deref())?;
    let decoded_piece = token_piece_decode_bytes(&raw_piece);
    let mut report = state.last_text_output.clone();
    report.token_count = 1;
    report.bytes = decoded_piece;
    report.byte_len = report.bytes.len() as u64;
    report.padded_byte_len = (report.byte_len + 7) & !7;
    report.byte_checksum = qwen3_dense_0_6b_text_output_bytes_checksum(&report.bytes);
    report.sequence_checksum = checksum_words(&[
        loop_step,
        position,
        state.cache_digest,
        sample.sampled_token,
        sample.logits_checksum,
    ]);
    report.token_checksum = checksum_words(&[sample.sampled_token]);
    report.text_checksum = sample.text_checksum;
    report.logits_checksum = sample.logits_checksum;
    report.guest_input = guest_input_report;
    report.kvcache =
        qwen3_dense_0_6b_incremental_kvcache_report(state, &sample, position, forward.as_ref());
    report.samples = vec![sample];
    report.text_lossy = String::from_utf8_lossy(&report.bytes).to_string();
    report.real_inference =
        qwen3_dense_0_6b_real_inference_report_from_runtime_samples(guest_input, &report.samples)?;
    Ok((report, real_kv_cache))
}

fn qwen3_dense_0_6b_resolve_incremental_kv_index(
    service: &mut LingquObjectServiceStub,
    state: &Qwen3Dense06bIncrementalDecodeState,
    loop_step: u64,
) -> Result<(), String> {
    let record = service.latest_record(&state.kv_object_key).ok_or_else(|| {
        format!(
            "qwen3_incremental_kv_object_missing:{}",
            state.kv_object_key
        )
    })?;
    if record.version != state.kv_object_version || record.checksum != state.kv_object_checksum {
        return Err(format!(
            "qwen3_incremental_kv_object_changed:key={}:expected_version={}:actual_version={}:expected_checksum={:#x}:actual_checksum={:#x}",
            state.kv_object_key,
            state.kv_object_version,
            record.version,
            state.kv_object_checksum,
            record.checksum
        ));
    }
    service
        .submit_resolve(
            LingquObjectResolveReq {
                task: None,
                key: state.kv_object_key.clone(),
                requester_entity: 0,
                version: LingquObjectVersionSelector::Exact(state.kv_object_version),
                min_state: LingquObjectState::Committed,
                preferred_backends: vec![LingquPayloadBackend::Shmem],
            },
            loop_step.saturating_mul(1000) + 10,
        )
        .map_err(|err| format!("qwen3_incremental_kv_object_resolve_submit_failed:{err}"))?;
    let completions = service.poll_ready(loop_step.saturating_mul(1000) + 100);
    if completions
        .iter()
        .any(|event| event.status != CompletionStatus::Success)
    {
        return Err("qwen3_incremental_kv_object_resolve_failed".to_string());
    }
    let payload = service
        .get_copy(
            &state.kv_object_key,
            LingquObjectVersionSelector::Exact(state.kv_object_version),
        )
        .ok_or_else(|| {
            format!(
                "qwen3_incremental_kv_object_payload_missing:{}",
                state.kv_object_key
            )
        })?;
    let cache_states = qwen3_dense_0_6b_kv_state_payload_from_bytes(&payload)?;
    if cache_states != state.cache_states {
        return Err(format!(
            "qwen3_incremental_kv_object_payload_mismatch:key={}:payload_states={}:state_states={}",
            state.kv_object_key,
            cache_states.len(),
            state.cache_states.len()
        ));
    }
    let real_kv_cache = qwen3_dense_0_6b_real_kv_cache_from_payload(&payload)?;
    if real_kv_cache != state.real_kv_cache {
        return Err(format!(
            "qwen3_incremental_real_kv_object_payload_mismatch:key={}:payload_real={}:state_real={}",
            state.kv_object_key,
            real_kv_cache.as_ref().map(|cache| cache.len()).unwrap_or(0),
            state.real_kv_cache.as_ref().map(|cache| cache.len()).unwrap_or(0)
        ));
    }
    Ok(())
}

fn qwen3_dense_0_6b_real_kv_cache_from_token_ids(
    token_ids: &[u64],
) -> Result<Option<Vec<Qwen3Dense06bLayerKvCache>>, String> {
    if token_ids.is_empty() {
        return Ok(None);
    }
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(None);
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let forward = forward_with_kv_cache_from_token_ids(&loaded.tensors, token_ids)?;
    Ok(Some(forward.kv_cache))
}

fn qwen3_dense_0_6b_runtime_incremental_forward_summary_from_guest_input(
    state: &Qwen3Dense06bIncrementalDecodeState,
    guest_input: &[u8],
    runtime_weight_objects: &mut LingquObjectServiceStub,
    event_base: u64,
) -> Result<
    (
        Option<qwen3_dense_0_6b::Qwen3Dense06bForwardReference>,
        BTreeMap<String, Vec<u8>>,
        Option<Vec<Qwen3Dense06bLayerKvCache>>,
    ),
    String,
> {
    let token_ids = qwen3_dense_0_6b_guest_input_token_ids(guest_input);
    if token_ids.is_empty() {
        return Ok((None, BTreeMap::new(), None));
    }
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok((None, BTreeMap::new(), None));
    };
    let resolved =
        qwen3_dense_0_6b_resolve_runtime_weight_objects(runtime_weight_objects, event_base)?;
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let previous_cache = state
        .real_kv_cache
        .as_ref()
        .ok_or_else(|| "qwen3_incremental_real_kv_cache_missing".to_string())?;
    let position = token_ids
        .len()
        .checked_sub(1)
        .ok_or_else(|| "qwen3_incremental_token_position_missing".to_string())?
        as u64;
    let hidden = embedding_reference_last_hidden(&loaded.tensors, &token_ids)?;
    let forward_with_cache = forward_incremental_with_kv_cache_from_hidden(
        &loaded.tensors,
        previous_cache,
        position,
        &hidden,
    )?;
    Ok((
        Some(forward_with_cache.forward),
        resolved.layer_payloads,
        Some(forward_with_cache.kv_cache),
    ))
}

fn qwen3_dense_0_6b_incremental_decode_sample(
    state: &Qwen3Dense06bIncrementalDecodeState,
    loop_step: u64,
    position: u64,
    forward: Option<&qwen3_dense_0_6b::Qwen3Dense06bForwardReference>,
    full_vocab: Option<&Qwen3Dense06bFullVocabLogitsSummary>,
    tokenizer_path: Option<&Path>,
) -> Result<Qwen3Dense06bTextOutputSample, String> {
    let vocab_size = QWEN3_DENSE_0_6B_PROFILE.vocab_size;
    let fallback_seed = state.cache_digest
        ^ state.last_sampled_token_checksum.rotate_left(7)
        ^ position.rotate_left(17)
        ^ loop_step.rotate_left(29);
    let fallback_sampled_token = fallback_seed % vocab_size;
    let fallback_runner_up_token =
        (fallback_sampled_token + 1 + (fallback_seed & 0x3f)) % vocab_size;
    let (
        sampled_token,
        runner_up_token,
        margin_milli,
        full_vocab_checked_token_count,
        full_vocab_logits_checksum,
        top_logit_bits,
        runner_up_logit_bits,
    ) = full_vocab
        .map(|summary| {
            let top_logit = f32::from_bits(summary.top_logit_bits as u32);
            let runner_logit = f32::from_bits(summary.runner_up_logit_bits as u32);
            let margin = if top_logit.is_finite() && runner_logit.is_finite() {
                ((top_logit - runner_logit).abs() * 1_000.0)
                    .round()
                    .max(1.0) as u64
            } else {
                1
            };
            (
                summary.top_token_id,
                summary.runner_up_token_id,
                margin,
                summary.checked_token_count,
                summary.logits_checksum,
                summary.top_logit_bits,
                summary.runner_up_logit_bits,
            )
        })
        .unwrap_or((
            fallback_sampled_token,
            fallback_runner_up_token,
            1_000 + position,
            0,
            0,
            0,
            0,
        ));
    let text_checksum =
        qwen3_dense_0_6b_sample_text_checksum(loop_step, sampled_token, 0, tokenizer_path)?;
    let piece = qwen3_dense_0_6b_token_piece(sampled_token, tokenizer_path)?;
    let (
        runtime_forward_layer_count,
        runtime_forward_final_hidden_checksum,
        runtime_forward_checksum,
    ) = forward
        .map(|forward| {
            (
                forward.layer_count,
                forward.final_hidden_checksum,
                forward.aggregate_checksum,
            )
        })
        .unwrap_or((0, 0, 0));
    let logits_checksum = qwen3_dense_0_6b_logits_checksum(
        state.cache_digest,
        position,
        sampled_token,
        runner_up_token,
        margin_milli,
        full_vocab_logits_checksum,
        top_logit_bits,
        qwen3_dense_0_6b_incremental_read_digest(state, position),
        runtime_forward_checksum,
        state.last_sampled_token_checksum,
    );
    let raw_piece = qwen3_dense_0_6b_token_piece_raw_bytes(sampled_token, tokenizer_path)?;
    Ok(Qwen3Dense06bTextOutputSample {
        step_index: loop_step,
        shard_id: 0,
        tile_id: position,
        logits_count: vocab_size,
        sampled_token,
        runner_up_token,
        margin_milli,
        logits_checksum,
        full_vocab_checked_token_count,
        full_vocab_logits_checksum,
        top_logit_bits,
        runner_up_logit_bits,
        runtime_forward_layer_count,
        runtime_forward_final_hidden_checksum,
        runtime_forward_checksum,
        kvcache_read_digest: qwen3_dense_0_6b_incremental_read_digest(state, position),
        qkv_reference_digest: runtime_forward_checksum,
        real_path_digest: checksum_words(&[
            state.cache_digest,
            runtime_forward_checksum,
            full_vocab_logits_checksum,
        ]),
        text_checksum,
        text_byte_offset: 0,
        byte_len: token_piece_decode_bytes(&raw_piece).len() as u64,
        boundary_flags: u64::from(piece.byte_len != 0),
        piece_lossy: String::from_utf8_lossy(&raw_piece).to_string(),
    })
}

fn qwen3_dense_0_6b_incremental_kvcache_report(
    state: &Qwen3Dense06bIncrementalDecodeState,
    sample: &Qwen3Dense06bTextOutputSample,
    position: u64,
    forward: Option<&qwen3_dense_0_6b::Qwen3Dense06bForwardReference>,
) -> Qwen3Dense06bKvCacheReport {
    let decode_entry_count =
        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers * QWEN3_DENSE_0_6B_PROFILE.tp_nodes;
    let state_snapshots =
        qwen3_dense_0_6b_incremental_kvcache_state_snapshots(state, sample, position, forward);
    let read_digest_checksum = checksum_words(&[
        state.cache_digest,
        sample.kvcache_read_digest,
        sample.runtime_forward_checksum,
        position,
        checksum_words(
            &state_snapshots
                .iter()
                .map(|snapshot| snapshot.read_digest)
                .collect::<Vec<_>>(),
        ),
    ]);
    Qwen3Dense06bKvCacheReport {
        descriptor_count: decode_entry_count,
        state_count: state_snapshots.len() as u64,
        append_block_count: decode_entry_count,
        update_seq_sum: state
            .cache_update_seq
            .saturating_add(decode_entry_count)
            .saturating_add(position),
        prefill_entry_count: 0,
        decode_entry_count,
        read_window_end_max: state.cache_position.max(position + 1),
        read_digest_checksum,
        state_snapshots,
    }
}

fn qwen3_dense_0_6b_incremental_read_digest(
    state: &Qwen3Dense06bIncrementalDecodeState,
    position: u64,
) -> u64 {
    qwen3_dense_0_6b_incremental_cache_read_digest_for_window(
        &state.cache_states,
        state.cache_position.max(position + 1),
    )
    .unwrap_or_else(|| {
        checksum_words(&[
            state.cache_digest,
            state.cache_position,
            state.cache_update_seq,
            position,
        ])
    })
}

fn qwen3_dense_0_6b_incremental_kvcache_state_snapshots(
    state: &Qwen3Dense06bIncrementalDecodeState,
    sample: &Qwen3Dense06bTextOutputSample,
    position: u64,
    forward: Option<&qwen3_dense_0_6b::Qwen3Dense06bForwardReference>,
) -> Vec<Qwen3Dense06bKvCacheStateSnapshot> {
    let mut snapshots = state.cache_states.clone();
    let read_window_end = state.cache_position.max(position + 1);
    let read_digest = qwen3_dense_0_6b_incremental_cache_read_digest_for_window(
        &state.cache_states,
        read_window_end,
    )
    .unwrap_or(sample.kvcache_read_digest);
    let mut update_seq = state.cache_update_seq;
    for layer_id in 0..QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        for tile_id in 0..QWEN3_DENSE_0_6B_PROFILE.tp_nodes {
            update_seq = update_seq.saturating_add(1);
            let seed = checksum_words(&[
                state.cache_digest,
                sample.sampled_token,
                sample.runtime_forward_checksum,
                layer_id,
                tile_id,
                position,
                update_seq,
            ]);
            let (k_checksum, v_checksum) = forward
                .and_then(|forward| forward.layers.get(layer_id as usize))
                .map(|layer| {
                    (
                        checksum_words(&[
                            layer.layer_id,
                            tile_id,
                            position,
                            update_seq,
                            layer.k_checksum,
                            layer.rope_k_checksum,
                        ]),
                        checksum_words(&[
                            layer.layer_id,
                            tile_id,
                            position,
                            update_seq,
                            layer.v_checksum,
                            layer.attention_context_checksum,
                        ]),
                    )
                })
                .unwrap_or((
                    seed ^ 0x6b5f_7177_656e_3330,
                    seed.rotate_left(23) ^ 0x765f_7177_656e_3330,
                ));
            snapshots.push(Qwen3Dense06bKvCacheStateSnapshot {
                layer_id,
                tile_id,
                position,
                update_seq,
                k_checksum,
                v_checksum,
                read_window_end,
                read_digest,
            });
        }
    }
    snapshots
}

fn qwen3_dense_0_6b_incremental_cache_read_digest_for_window(
    snapshots: &[Qwen3Dense06bKvCacheStateSnapshot],
    read_window_end: u64,
) -> Option<u64> {
    if snapshots.is_empty() {
        return None;
    }
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    let mut used = 0u64;
    for snapshot in snapshots
        .iter()
        .filter(|snapshot| snapshot.position < read_window_end)
    {
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3)
            ^ snapshot.layer_id
            ^ snapshot.tile_id.rotate_left(5)
            ^ snapshot.position.rotate_left(11)
            ^ snapshot.update_seq.rotate_left(17)
            ^ snapshot.k_checksum.rotate_left(23)
            ^ snapshot.v_checksum.rotate_left(29);
        used += 1;
    }
    (used != 0).then_some(acc)
}

fn qwen3_dense_0_6b_token_piece_raw_bytes(
    sampled_token: u64,
    tokenizer_path: Option<&Path>,
) -> Result<Vec<u8>, String> {
    if let Some(path) = tokenizer_path {
        static TOKEN_PIECE_BYTES_CACHE: OnceLock<Mutex<BTreeMap<(PathBuf, u64), Vec<u8>>>> =
            OnceLock::new();
        let cache_key = (path.to_path_buf(), sampled_token);
        let cache = TOKEN_PIECE_BYTES_CACHE.get_or_init(|| Mutex::new(BTreeMap::new()));
        if let Some(bytes) = cache
            .lock()
            .expect("token piece cache poisoned")
            .get(&cache_key)
        {
            return Ok(bytes.clone());
        }
        let bytes = token_piece_bytes_from_tokenizer_path(path, sampled_token)?;
        cache
            .lock()
            .expect("token piece cache poisoned")
            .insert(cache_key, bytes.clone());
        Ok(bytes)
    } else {
        Ok(token_piece_bytes_from_policy(
            tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE),
            sampled_token,
        ))
    }
}

fn qwen3_dense_0_6b_decode_step_selected_samples(
    samples: &[Qwen3Dense06bTextOutputSample],
) -> Vec<Qwen3Dense06bTextOutputSample> {
    samples
        .iter()
        .min_by_key(|sample| (sample.step_index, sample.tile_id, sample.shard_id))
        .cloned()
        .into_iter()
        .collect()
}

fn qwen3_dense_0_6b_decode_step_selected_bytes(
    samples: &[Qwen3Dense06bTextOutputSample],
) -> Vec<u8> {
    let mut bytes = Vec::new();
    let mut ordered = samples.to_vec();
    let tokenizer_path = qwen3_dense_0_6b_real_tokenizer_path();
    ordered.sort_by_key(|sample| sample.text_byte_offset);
    for sample in ordered {
        let piece =
            qwen3_dense_0_6b_token_piece_raw_bytes(sample.sampled_token, tokenizer_path.as_deref())
                .map(|raw| token_piece_decode_bytes(&raw))
                .unwrap_or_else(|_| token_piece_decode_bytes(sample.piece_lossy.as_bytes()));
        bytes.extend_from_slice(&piece);
    }
    bytes
}

fn qwen3_dense_0_6b_synthetic_stage_report(
    guest_input: &Qwen3Dense06bGuestInputReport,
    real_qkv: Option<&Qwen3Dense06bQkvReferenceReport>,
    attention: &Qwen3Dense06bAttentionReport,
    post_attention: &Qwen3Dense06bPostAttentionReport,
    real_mlp: Option<&Qwen3Dense06bMlpReferenceReport>,
    real_logits: Option<&Qwen3Dense06bLogitsReferenceReport>,
    tokenizer_policy_kind: u64,
) -> Qwen3Dense06bSyntheticStageReport {
    let guest_input_real_backed = guest_input.real_backed
        && guest_input.byte_len != 0
        && guest_input.checksum != 0
        && guest_input.prompt_byte_len != 0
        && guest_input.prompt_checksum != 0
        && guest_input.prompt_token_count != 0
        && guest_input.prompt_token_checksum != 0
        && guest_input.tokenizer_asset_checksum != 0;
    let qkv_base_tile_real_backed = real_qkv
        .map(|report| {
            report.real_weight_checksum != 0
                && report.real_value_checksum != 0
                && report.real_output_checksum != 0
                && report.stage_kind_mask == 0x0f
        })
        .unwrap_or(false);
    let attention_score_real_backed = qkv_base_tile_real_backed
        && attention.score_count != 0
        && attention.score_checksum != 0
        && (attention.stage_mask & 0x01) != 0;
    let attention_context_real_backed = qkv_base_tile_real_backed
        && attention.context_count != 0
        && attention.context_checksum != 0
        && (attention.stage_mask & 0x04) != 0;
    let mlp_activation_real_backed = real_mlp
        .map(|report| {
            report.real_weight_checksum != 0
                && report.real_activation_checksum != 0
                && report.sample_checksum != 0
        })
        .unwrap_or(false);
    let mlp_output_real_backed = mlp_activation_real_backed
        && real_mlp
            .map(|report| report.real_output_checksum != 0)
            .unwrap_or(false)
        && post_attention.mlp_output_count != 0
        && post_attention.mlp_output_checksum != 0
        && (post_attention.stage_mask & 0x04) != 0;
    let logits_candidates_real_backed = real_logits
        .map(|report| {
            report.candidate_count != 0
                && report.selection_match_count == report.sampled_pair_count / 2
                && report.margin_match_count == report.selection_match_count
                && report.checksum_match_count == report.selection_match_count
                && report.logit_checksum != 0
                && report.final_norm_checksum != 0
        })
        .unwrap_or(false);
    let token_text_real_backed = tokenizer_policy_kind
        == QWEN3_DENSE_0_6B_TOKENIZER_ASSET_POLICY_KIND
        && real_logits
            .map(|report| report.token_count != 0 && report.token_count == report.candidate_count)
            .unwrap_or(false);
    let mut stage_mask = QWEN3_SYNTHETIC_GUEST_INPUT
        | QWEN3_SYNTHETIC_TOKEN_TEXT
        | QWEN3_SYNTHETIC_QKV_BASE_TILE
        | QWEN3_SYNTHETIC_ATTENTION_SCORE
        | QWEN3_SYNTHETIC_ATTENTION_CONTEXT
        | QWEN3_SYNTHETIC_MLP_OUTPUT
        | QWEN3_SYNTHETIC_LOGITS_CANDIDATES;
    if guest_input_real_backed {
        stage_mask &= !QWEN3_SYNTHETIC_GUEST_INPUT;
    }
    if qkv_base_tile_real_backed {
        stage_mask &= !QWEN3_SYNTHETIC_QKV_BASE_TILE;
    }
    if attention_score_real_backed {
        stage_mask &= !QWEN3_SYNTHETIC_ATTENTION_SCORE;
    }
    if attention_context_real_backed {
        stage_mask &= !QWEN3_SYNTHETIC_ATTENTION_CONTEXT;
    }
    if !mlp_activation_real_backed {
        stage_mask |= QWEN3_SYNTHETIC_MLP_ACTIVATION;
    }
    if mlp_output_real_backed {
        stage_mask &= !QWEN3_SYNTHETIC_MLP_OUTPUT;
    }
    if logits_candidates_real_backed {
        stage_mask &= !QWEN3_SYNTHETIC_LOGITS_CANDIDATES;
    }
    if token_text_real_backed {
        stage_mask &= !QWEN3_SYNTHETIC_TOKEN_TEXT;
    }
    let stage_checksum = checksum_words(&[
        stage_mask,
        u64::from(guest_input_real_backed),
        u64::from(qkv_base_tile_real_backed),
        u64::from(attention_score_real_backed),
        u64::from(attention_context_real_backed),
        u64::from(mlp_activation_real_backed),
        u64::from(mlp_output_real_backed),
        u64::from(logits_candidates_real_backed),
        u64::from(token_text_real_backed),
        real_qkv
            .map(|report| report.real_value_checksum)
            .unwrap_or(0),
        real_mlp
            .map(|report| report.real_activation_checksum)
            .unwrap_or(0),
        real_mlp
            .map(|report| report.real_output_checksum)
            .unwrap_or(0),
        post_attention.mlp_output_checksum,
        real_mlp.map(|report| report.sample_checksum).unwrap_or(0),
        real_logits.map(|report| report.logit_checksum).unwrap_or(0),
        guest_input.checksum,
        guest_input.prompt_checksum,
        guest_input.prompt_token_count,
        guest_input.prompt_token_checksum,
        guest_input.tokenizer_asset_checksum,
    ]);
    Qwen3Dense06bSyntheticStageReport {
        stage_count: stage_mask.count_ones() as u64,
        stage_mask,
        stage_checksum,
        qkv_base_tile_real_backed,
        attention_score_real_backed,
        attention_context_real_backed,
        mlp_activation_real_backed,
        mlp_output_real_backed,
        logits_candidates_real_backed,
        token_text_real_backed,
        guest_input_real_backed,
    }
}

fn qwen3_dense_0_6b_real_inference_contract_report(
    text_output: &Qwen3Dense06bTextOutputReport,
    layer_progress: &Qwen3Dense06bDecodeLayerProgressReport,
    hidden_layer_pipeline: &Qwen3Dense06bHiddenLayerPipelineReport,
) -> Qwen3Dense06bRealInferenceContractReport {
    let real_logits = text_output.real_logits.as_ref();
    let real_inference = text_output.real_inference.as_ref();
    let runtime_full_vocab_logits = !text_output.samples.is_empty()
        && text_output.samples.iter().all(|sample| {
            sample.logits_count == QWEN3_DENSE_0_6B_PROFILE.vocab_size
                && sample.full_vocab_checked_token_count == QWEN3_DENSE_0_6B_PROFILE.vocab_size
                && sample.full_vocab_logits_checksum != 0
                && sample.top_logit_bits != 0
                && sample.runner_up_logit_bits != 0
        });
    let runtime_full_forward_math = !text_output.samples.is_empty()
        && text_output.samples.iter().all(|sample| {
            sample.runtime_forward_layer_count == QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                && sample.runtime_forward_final_hidden_checksum != 0
                && sample.runtime_forward_checksum != 0
        });
    let full_vocab_logits = runtime_full_vocab_logits;
    let uses_candidate_logits_only = !full_vocab_logits;
    let uses_deterministic_hidden = real_logits.is_none() && real_inference.is_none();
    let uses_embedding_hidden_as_final_hidden = false;
    let uses_round1_output_hidden_for_logits = real_inference.is_some()
        || real_logits
            .map(|logits| logits.hidden_size == QWEN3_DENSE_0_6B_PROFILE.hidden_size)
            .unwrap_or(false);
    let full_forward_math = runtime_full_forward_math;
    let sampled_text_reference_checked = full_forward_math
        && full_vocab_logits
        && real_inference
            .map(|reference| {
                reference.output_sample_count != 0
                    && reference.sampled_token_match_count == reference.output_sample_count
                    && reference.sampled_text_match_count == reference.output_sample_count
            })
            .unwrap_or(false);

    let mut blockers = Vec::new();
    if text_output.synthetic.stage_count != 0 {
        blockers.push(format!(
            "synthetic_stages_present:count={}:mask={:#x}",
            text_output.synthetic.stage_count, text_output.synthetic.stage_mask
        ));
    }
    if uses_candidate_logits_only {
        blockers.push("logits_are_candidate_subset_not_full_vocab".to_string());
    }
    if uses_deterministic_hidden {
        blockers.push("logits_hidden_may_use_deterministic_fallback".to_string());
    }
    if uses_embedding_hidden_as_final_hidden {
        blockers.push("logits_hidden_uses_embedding_proxy".to_string());
    }
    if !uses_round1_output_hidden_for_logits {
        blockers.push("logits_hidden_not_connected_to_round1_output".to_string());
    }
    if !full_forward_math {
        blockers.push(
            "layer_forward_math_incomplete:missing_full_attention_o_proj_residual_mlp_chain"
                .to_string(),
        );
    }
    if !full_vocab_logits {
        blockers.push("full_vocab_logits_not_computed".to_string());
    }
    if !sampled_text_reference_checked {
        blockers.push("sampled_token_text_reference_not_checked".to_string());
    }
    if !layer_progress.full_layer_path_real_backed
        || !hidden_layer_pipeline.real_layer_executions_all_present
    {
        blockers.push("full_layer_pipeline_not_real_backed".to_string());
    }

    let ready = blockers.is_empty()
        && text_output.synthetic.stage_count == 0
        && !uses_candidate_logits_only
        && !uses_deterministic_hidden
        && !uses_embedding_hidden_as_final_hidden
        && uses_round1_output_hidden_for_logits
        && full_forward_math
        && full_vocab_logits
        && sampled_text_reference_checked
        && layer_progress.full_layer_path_real_backed
        && hidden_layer_pipeline.real_layer_executions_all_present;
    let blocker_count = blockers.len() as u64;
    let aggregate_checksum = checksum_words(&[
        u64::from(ready),
        blocker_count,
        text_output.synthetic.stage_count,
        text_output.synthetic.stage_mask,
        u64::from(uses_candidate_logits_only),
        u64::from(uses_deterministic_hidden),
        u64::from(uses_embedding_hidden_as_final_hidden),
        u64::from(uses_round1_output_hidden_for_logits),
        u64::from(full_forward_math),
        u64::from(full_vocab_logits),
        u64::from(sampled_text_reference_checked),
        layer_progress.aggregate_checksum,
        hidden_layer_pipeline.aggregate_checksum,
        real_logits
            .map(|logits| logits.aggregate_checksum)
            .unwrap_or(0),
        real_inference
            .map(|reference| reference.aggregate_checksum)
            .unwrap_or(0),
    ]);
    Qwen3Dense06bRealInferenceContractReport {
        ready,
        blocker_count,
        synthetic_stage_count: text_output.synthetic.stage_count,
        synthetic_stage_mask: text_output.synthetic.stage_mask,
        uses_candidate_logits_only,
        uses_deterministic_hidden,
        uses_embedding_hidden_as_final_hidden,
        uses_round1_output_hidden_for_logits,
        full_forward_math,
        full_vocab_logits,
        sampled_text_reference_checked,
        aggregate_checksum,
        blockers,
    }
}

fn qwen3_dense_0_6b_hidden_layer_pipeline_report(
    topology: &SimTopology,
    text_output: &Qwen3Dense06bTextOutputReport,
    guest_input: &[u8],
    mut object_service: Option<&mut LingquObjectServiceStub>,
    session_id: u64,
    step_index: u64,
) -> Result<Qwen3Dense06bHiddenLayerPipelineReport, String> {
    let profile = QWEN3_DENSE_0_6B_PROFILE;
    let layer_count = profile.num_hidden_layers;
    let node_count = profile.tp_nodes;
    let first_layer_id = 0;
    let last_layer_id = layer_count.saturating_sub(1);
    let first_node_id = qwen3_dense_0_6b_hidden_layer_owner_node(first_layer_id);
    let last_node_id = qwen3_dense_0_6b_hidden_layer_owner_node(last_layer_id);
    let real_qkv = text_output.real_qkv.as_ref();
    let real_mlp = text_output.real_mlp.as_ref();
    let real_qkv_layer_summaries = qwen3_dense_0_6b_real_qkv_layer_summaries(topology)?;
    let real_qkv_layer_checksums = real_qkv_layer_summaries
        .iter()
        .map(|(layer_id, summary)| (*layer_id, summary.aggregate_checksum))
        .collect::<BTreeMap<_, _>>();
    let real_qkv_layer_count = real_qkv_layer_checksums.len() as u64;
    let real_qkv_layer_checksum = checksum_words(
        &real_qkv_layer_checksums
            .iter()
            .flat_map(|(layer_id, checksum)| [*layer_id, *checksum])
            .collect::<Vec<_>>(),
    );
    let real_qkv_all_layers_present = real_qkv_layer_count == layer_count;
    let real_mlp_layer_summaries = qwen3_dense_0_6b_real_mlp_layer_summaries(topology)?;
    let real_mlp_layer_checksums = real_mlp_layer_summaries
        .iter()
        .map(|(layer_id, summary)| (*layer_id, summary.aggregate_checksum))
        .collect::<BTreeMap<_, _>>();
    let real_mlp_layer_count = real_mlp_layer_checksums.len() as u64;
    let real_mlp_layer_checksum = checksum_words(
        &real_mlp_layer_checksums
            .iter()
            .flat_map(|(layer_id, checksum)| [*layer_id, *checksum])
            .collect::<Vec<_>>(),
    );
    let real_mlp_all_layers_present = real_mlp_layer_count == layer_count;
    let prompt_token_ids = qwen3_dense_0_6b_guest_input_token_ids(guest_input);
    let input_embedding = qwen3_dense_0_6b_real_input_embedding_summary(&prompt_token_ids)?;
    let input_embedding_real_backed = input_embedding
        .as_ref()
        .map(|summary| {
            summary.token_count != 0
                && summary.token_count == prompt_token_ids.len() as u64
                && summary.row_byte_count != 0
                && summary.row_checksum != 0
                && summary.value_checksum != 0
                && summary.aggregate_checksum != 0
        })
        .unwrap_or(false);
    let input_embedding_token_count = input_embedding
        .as_ref()
        .map(|summary| summary.token_count)
        .unwrap_or(0);
    let input_embedding_row_byte_count = input_embedding
        .as_ref()
        .map(|summary| summary.row_byte_count)
        .unwrap_or(0);
    let input_embedding_row_checksum = input_embedding
        .as_ref()
        .map(|summary| summary.row_checksum)
        .unwrap_or(0);
    let input_embedding_value_checksum = input_embedding
        .as_ref()
        .map(|summary| summary.value_checksum)
        .unwrap_or(0);
    let input_embedding_checksum = input_embedding
        .as_ref()
        .map(|summary| summary.aggregate_checksum)
        .unwrap_or(0);
    let hidden_tensor_byte_count = (128 * 128 * std::mem::size_of::<u16>()) as u64;
    let initial_hidden_tensor = qwen3_dense_0_6b_initial_hidden_tensor_tile(
        guest_input,
        text_output,
        128,
        input_embedding.as_ref(),
    );
    let mut previous_hidden_tensor = initial_hidden_tensor;
    let mut previous_node = first_node_id;
    let mut previous_layer_checksum =
        qwen3_dense_0_6b_shard_output_checksum(&previous_hidden_tensor);
    let mut boundary_words = Vec::with_capacity(node_count.saturating_sub(1) as usize);
    let mut boundary_count = 0u64;
    let mut local_transition_count = 0u64;
    let mut final_layer_checksum = previous_layer_checksum;
    let mut layers_per_node = vec![0u64; node_count as usize];
    let mut layer_executions = Vec::with_capacity(layer_count as usize);
    let mut hidden_tensor_carry_words = Vec::with_capacity(layer_count as usize);
    let mut hidden_tensor_real_reference_words = Vec::with_capacity(layer_count as usize);
    let mut real_layer_execution_words = Vec::with_capacity(layer_count as usize);
    let node_ranges = qwen3_dense_0_6b_bootstrap_node_ranges();
    let range_handoff_base = qwen3_dense_0_6b_decode_range_handoff_base(session_id, step_index);
    for layer_id in 0..layer_count {
        let node_id = qwen3_dense_0_6b_hidden_layer_owner_node(layer_id);
        let input_checksum = previous_layer_checksum;
        let starts_node_range = layer_id == 0 || node_id != previous_node;
        if starts_node_range && layer_id > 0 {
            qwen3_dense_0_6b_decode_range_handoff_via_object_service(
                object_service.as_deref_mut(),
                &range_handoff_base,
                &node_ranges,
                previous_node,
                node_id,
                &mut previous_hidden_tensor,
                step_index,
                layer_id,
            )?;
        }
        let input_tensor_checksum = qwen3_dense_0_6b_shard_output_checksum(&previous_hidden_tensor);
        if let Some(count) = layers_per_node.get_mut(node_id as usize) {
            *count += 1;
        }
        if layer_id > 0 {
            if starts_node_range {
                boundary_count += 1;
                boundary_words.push(checksum_words(&[
                    layer_id - 1,
                    previous_node,
                    layer_id,
                    node_id,
                    previous_layer_checksum,
                ]));
            } else {
                local_transition_count += 1;
            }
        }
        let qkv_checksum = match (real_qkv_layer_checksums.get(&layer_id), real_qkv) {
            (Some(checksum), _) => *checksum,
            (None, Some(report)) if layer_id == report.layer_id => report.reference_layer_checksum,
            (None, Some(report)) if layer_id == report.layer_id + 1 => {
                report.next_reference_layer_checksum
            }
            (None, Some(report)) => {
                report
                    .stage_link_checksum
                    .rotate_left((layer_id % 63) as u32)
                    ^ report.aggregate_checksum
            }
            (None, None) => text_output
                .attention
                .aggregate_checksum
                .rotate_left((layer_id % 63) as u32),
        };
        let mlp_checksum = match (real_mlp_layer_checksums.get(&layer_id), real_mlp) {
            (Some(checksum), _) => *checksum,
            (None, Some(report)) if layer_id == report.layer_id => report.real_output_checksum,
            (None, Some(report)) if layer_id == report.next_layer_id => {
                report.next_real_output_checksum
            }
            (None, Some(report)) => {
                report
                    .table_checksum
                    .rotate_left(((layer_id + 7) % 63) as u32)
                    ^ report.aggregate_checksum
            }
            (None, None) => text_output
                .post_attention
                .mlp_output_checksum
                .rotate_left(((layer_id + 7) % 63) as u32),
        };
        let output_hidden_tensor = qwen3_dense_0_6b_next_hidden_tensor_tile(
            &previous_hidden_tensor,
            128,
            layer_id,
            node_id,
            qkv_checksum,
            mlp_checksum,
            text_output.result_flow.aggregate_checksum,
            real_qkv_layer_summaries.get(&layer_id),
            real_mlp_layer_summaries.get(&layer_id),
        );
        let output_tensor_checksum = qwen3_dense_0_6b_shard_output_checksum(&output_hidden_tensor);
        let real_reference_tensor_checksum = qwen3_dense_0_6b_hidden_tensor_real_reference_checksum(
            real_qkv_layer_summaries.get(&layer_id),
            real_mlp_layer_summaries.get(&layer_id),
        );
        let layer_checksum = checksum_words(&[
            layer_id,
            node_id,
            input_tensor_checksum,
            qkv_checksum,
            mlp_checksum,
            output_tensor_checksum,
            text_output
                .result_flow
                .aggregate_checksum
                .rotate_left((layer_id % 63) as u32),
        ]);
        hidden_tensor_carry_words.push(checksum_words(&[
            layer_id,
            node_id,
            input_tensor_checksum,
            output_tensor_checksum,
            output_hidden_tensor.len() as u64,
        ]));
        if real_reference_tensor_checksum != 0 {
            hidden_tensor_real_reference_words.push(checksum_words(&[
                layer_id,
                node_id,
                real_reference_tensor_checksum,
                output_tensor_checksum,
            ]));
        }
        if real_qkv_layer_checksums.contains_key(&layer_id)
            && real_mlp_layer_checksums.contains_key(&layer_id)
        {
            real_layer_execution_words.push(checksum_words(&[
                layer_id,
                node_id,
                input_checksum,
                qkv_checksum,
                mlp_checksum,
                layer_checksum,
            ]));
        }
        layer_executions.push(Qwen3Dense06bHiddenLayerExecution {
            layer_id,
            owner_node: node_id,
            input_checksum,
            qkv_checksum,
            mlp_checksum,
            output_checksum: layer_checksum,
            input_tensor_checksum,
            output_tensor_checksum,
            real_reference_tensor_checksum,
            starts_node_range,
            input_tensor_payload: previous_hidden_tensor.clone(),
            output_tensor_payload: output_hidden_tensor.clone(),
        });
        previous_node = node_id;
        previous_hidden_tensor = output_hidden_tensor;
        previous_layer_checksum = layer_checksum;
        final_layer_checksum = layer_checksum;
    }
    let transition_count = layer_count.saturating_sub(1);
    let min_layers_per_node = layers_per_node.iter().copied().min().unwrap_or(0);
    let max_layers_per_node = layers_per_node.iter().copied().max().unwrap_or(0);
    let balanced_layer_spread = max_layers_per_node.saturating_sub(min_layers_per_node) <= 1;
    let node_ranges = qwen3_dense_0_6b_hidden_layer_node_ranges(&layers_per_node);
    let node_range_words = qwen3_dense_0_6b_hidden_layer_node_range_words(&node_ranges);
    let layer_execution_words = qwen3_dense_0_6b_hidden_layer_execution_words(&layer_executions);
    let node_range_checksum = checksum_words(&node_range_words);
    let layer_assignment_checksum = checksum_words(&layer_execution_words);
    let boundary_checksum = checksum_words(&boundary_words);
    let hidden_tensor_carry_count = hidden_tensor_carry_words.len() as u64;
    let hidden_tensor_carry_checksum = checksum_words(&hidden_tensor_carry_words);
    let hidden_tensor_carry_all_present = hidden_tensor_carry_count == layer_count
        && hidden_tensor_byte_count == previous_hidden_tensor.len() as u64;
    let hidden_tensor_real_reference_count = hidden_tensor_real_reference_words.len() as u64;
    let hidden_tensor_real_reference_checksum = checksum_words(&hidden_tensor_real_reference_words);
    let hidden_tensor_real_references_all_present =
        hidden_tensor_real_reference_count == layer_count;
    let real_layer_execution_count = real_layer_execution_words.len() as u64;
    let real_layer_execution_checksum = checksum_words(&real_layer_execution_words);
    let real_layer_executions_all_present = real_layer_execution_count == layer_count;
    let aggregate_checksum = checksum_words(&[
        layer_count,
        node_count,
        u64::from(input_embedding_real_backed),
        input_embedding_token_count,
        input_embedding_row_byte_count,
        input_embedding_row_checksum,
        input_embedding_value_checksum,
        input_embedding_checksum,
        hidden_tensor_byte_count,
        hidden_tensor_carry_count,
        hidden_tensor_carry_checksum,
        u64::from(hidden_tensor_carry_all_present),
        hidden_tensor_real_reference_count,
        hidden_tensor_real_reference_checksum,
        u64::from(hidden_tensor_real_references_all_present),
        real_qkv_layer_count,
        real_qkv_layer_checksum,
        u64::from(real_qkv_all_layers_present),
        real_mlp_layer_count,
        real_mlp_layer_checksum,
        u64::from(real_mlp_all_layers_present),
        real_layer_execution_count,
        real_layer_execution_checksum,
        u64::from(real_layer_executions_all_present),
        transition_count,
        boundary_count,
        local_transition_count,
        min_layers_per_node,
        max_layers_per_node,
        u64::from(balanced_layer_spread),
        first_layer_id,
        last_layer_id,
        first_node_id,
        last_node_id,
        node_range_checksum,
        layer_assignment_checksum,
        boundary_checksum,
        final_layer_checksum,
    ]);
    Ok(Qwen3Dense06bHiddenLayerPipelineReport {
        layer_count,
        node_count,
        input_embedding_real_backed,
        input_embedding_token_count,
        input_embedding_row_byte_count,
        input_embedding_row_checksum,
        input_embedding_value_checksum,
        input_embedding_checksum,
        hidden_tensor_byte_count,
        hidden_tensor_carry_count,
        hidden_tensor_carry_checksum,
        hidden_tensor_carry_all_present,
        hidden_tensor_real_reference_count,
        hidden_tensor_real_reference_checksum,
        hidden_tensor_real_references_all_present,
        real_qkv_layer_count,
        real_qkv_layer_checksum,
        real_qkv_all_layers_present,
        real_mlp_layer_count,
        real_mlp_layer_checksum,
        real_mlp_all_layers_present,
        real_layer_execution_count,
        real_layer_execution_checksum,
        real_layer_executions_all_present,
        transition_count,
        boundary_count,
        local_transition_count,
        min_layers_per_node,
        max_layers_per_node,
        balanced_layer_spread,
        first_layer_id,
        last_layer_id,
        first_node_id,
        last_node_id,
        node_range_checksum,
        layer_assignment_checksum,
        boundary_checksum,
        final_layer_checksum,
        aggregate_checksum,
        node_ranges,
        layer_executions,
    })
}

fn qwen3_dense_0_6b_decode_range_handoff_base(session_id: u64, step_index: u64) -> String {
    format!("qwen3/session/{session_id:016x}/step/{step_index:08}")
}

fn qwen3_dense_0_6b_decode_range_hidden_key(
    base: &str,
    range: &Qwen3Dense06bHiddenLayerNodeRange,
    slot: &str,
) -> String {
    format!(
        "{base}/hidden/node-{}/layers/{:02}-{:02}/{slot}",
        range.node_id, range.first_layer_id, range.last_layer_id
    )
}

fn qwen3_dense_0_6b_decode_range_handoff_via_object_service(
    service: Option<&mut LingquObjectServiceStub>,
    base: &str,
    node_ranges: &[Qwen3Dense06bHiddenLayerNodeRange],
    previous_node: u64,
    next_node: u64,
    previous_hidden_tensor: &mut Vec<u8>,
    step_index: u64,
    layer_id: u64,
) -> Result<(), String> {
    let Some(service) = service else {
        return Ok(());
    };
    let previous_range = node_ranges
        .iter()
        .find(|range| range.node_id == previous_node)
        .ok_or_else(|| {
            format!(
                "qwen3_decode_range_handoff_previous_range_missing:node={previous_node}:layer={layer_id}"
            )
        })?;
    let next_range = node_ranges
        .iter()
        .find(|range| range.node_id == next_node)
        .ok_or_else(|| {
            format!(
                "qwen3_decode_range_handoff_next_range_missing:node={next_node}:layer={layer_id}"
            )
        })?;
    let previous_output_key =
        qwen3_dense_0_6b_decode_range_hidden_key(base, previous_range, "output");
    let next_input_key = qwen3_dense_0_6b_decode_range_hidden_key(base, next_range, "input");
    let previous_output_checksum = qwen3_dense_0_6b_shard_output_checksum(previous_hidden_tensor);
    let event_base = 510_000u64
        .saturating_add(step_index.saturating_mul(10_000))
        .saturating_add(layer_id.saturating_mul(100));
    qwen3_dense_0_6b_publish_runtime_tensor_object(
        service,
        &previous_output_key,
        previous_node,
        previous_hidden_tensor,
        previous_output_checksum,
        event_base,
    )?;
    let resolved_output = qwen3_dense_0_6b_resolve_runtime_hidden_tensor_object(
        service,
        &previous_output_key,
        next_node,
        previous_output_checksum,
        event_base + 1,
    )?;
    let resolved_output_checksum = qwen3_dense_0_6b_shard_output_checksum(&resolved_output);
    qwen3_dense_0_6b_publish_runtime_tensor_object(
        service,
        &next_input_key,
        next_node,
        &resolved_output,
        resolved_output_checksum,
        event_base + 2,
    )?;
    *previous_hidden_tensor = qwen3_dense_0_6b_resolve_runtime_hidden_tensor_object(
        service,
        &next_input_key,
        next_node,
        resolved_output_checksum,
        event_base + 3,
    )?;
    Ok(())
}

fn qwen3_dense_0_6b_real_qkv_layer_summaries(
    topology: &SimTopology,
) -> Result<BTreeMap<u64, Qwen3Dense06bQkvReferenceLayerSummary>, String> {
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(BTreeMap::new());
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let manifest = weight_manifest_from_metadata(
        topology,
        QWEN3_DENSE_0_6B_PROFILE,
        loaded.source.clone(),
        &loaded.tensors,
    )?;
    let mut summaries = BTreeMap::new();
    for layer_id in 0..QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        let summary = qkv_reference_layer_summary(&manifest, &loaded.tensors, layer_id)?;
        summaries.insert(layer_id, summary);
    }
    Ok(summaries)
}

fn qwen3_dense_0_6b_real_mlp_layer_summaries(
    topology: &SimTopology,
) -> Result<BTreeMap<u64, Qwen3Dense06bMlpReferenceLayerSummary>, String> {
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(BTreeMap::new());
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let manifest = weight_manifest_from_metadata(
        topology,
        QWEN3_DENSE_0_6B_PROFILE,
        loaded.source.clone(),
        &loaded.tensors,
    )?;
    let mut summaries = BTreeMap::new();
    for layer_id in 0..QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        let summary = mlp_reference_layer_summary(&manifest, &loaded.tensors, layer_id)?;
        summaries.insert(layer_id, summary);
    }
    Ok(summaries)
}

fn qwen3_dense_0_6b_real_input_embedding_summary(
    token_ids: &[u64],
) -> Result<Option<Qwen3Dense06bEmbeddingReferenceSummary>, String> {
    if token_ids.is_empty() {
        return Ok(None);
    }
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(None);
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    embedding_reference_summary(&loaded.tensors, token_ids).map(Some)
}

fn qwen3_dense_0_6b_real_input_embedding_hidden(
    token_ids: &[u64],
) -> Result<Option<Vec<f32>>, String> {
    if token_ids.is_empty() {
        return Ok(None);
    }
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(None);
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    embedding_reference_last_hidden(&loaded.tensors, token_ids).map(Some)
}

fn qwen3_dense_0_6b_hidden_layer_node_ranges(
    layers_per_node: &[u64],
) -> Vec<Qwen3Dense06bHiddenLayerNodeRange> {
    let mut ranges = Vec::with_capacity(layers_per_node.len());
    let mut next_layer_id = 0u64;
    for (node_id, layer_count) in layers_per_node.iter().copied().enumerate() {
        let first_layer_id = next_layer_id;
        let last_layer_id = first_layer_id + layer_count.saturating_sub(1);
        ranges.push(Qwen3Dense06bHiddenLayerNodeRange {
            node_id: node_id as u64,
            first_layer_id,
            last_layer_id,
            layer_count,
        });
        next_layer_id += layer_count;
    }
    ranges
}

fn qwen3_dense_0_6b_hidden_layer_node_range_words(
    node_ranges: &[Qwen3Dense06bHiddenLayerNodeRange],
) -> Vec<u64> {
    node_ranges
        .iter()
        .map(|range| {
            checksum_words(&[
                range.node_id,
                range.first_layer_id,
                range.last_layer_id,
                range.layer_count,
            ])
        })
        .collect()
}

fn qwen3_dense_0_6b_hidden_layer_execution_words(
    layer_executions: &[Qwen3Dense06bHiddenLayerExecution],
) -> Vec<u64> {
    layer_executions
        .iter()
        .map(|execution| {
            checksum_words(&[
                execution.layer_id,
                execution.owner_node,
                execution.input_checksum,
                execution.qkv_checksum,
                execution.mlp_checksum,
                execution.output_checksum,
                execution.input_tensor_checksum,
                execution.output_tensor_checksum,
                execution.real_reference_tensor_checksum,
                u64::from(execution.starts_node_range),
            ])
        })
        .collect()
}

fn qwen3_dense_0_6b_hidden_tensor_real_reference_checksum(
    qkv_summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
    mlp_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
) -> u64 {
    let words = qwen3_dense_0_6b_hidden_tensor_real_reference_words(qkv_summary, mlp_summary);
    if words.is_empty() {
        0
    } else {
        checksum_words(&words)
    }
}

fn qwen3_dense_0_6b_hidden_tensor_real_reference_words(
    qkv_summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
    mlp_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
) -> Vec<u64> {
    let (Some(qkv_summary), Some(mlp_summary)) = (qkv_summary, mlp_summary) else {
        return Vec::new();
    };
    let mut words = Vec::new();
    words.extend_from_slice(&[
        qkv_summary.layer_id,
        qkv_summary.shard_count,
        qkv_summary.aggregate_checksum,
        mlp_summary.layer_id,
        mlp_summary.shard_count,
        mlp_summary.aggregate_checksum,
    ]);
    for qkv_shard in &qkv_summary.shards {
        words.extend_from_slice(&[
            qkv_shard.shard_id,
            qkv_shard.rmsnorm_checksum,
            qkv_shard.q_output_checksum,
            qkv_shard.k_output_checksum,
            qkv_shard.v_output_checksum,
        ]);
        words.extend_from_slice(&qkv_shard.rmsnorm_sample_words);
        words.extend_from_slice(&qkv_shard.q_output_sample_words);
        words.extend_from_slice(&qkv_shard.k_output_sample_words);
        words.extend_from_slice(&qkv_shard.v_output_sample_words);
    }
    for mlp_shard in &mlp_summary.shards {
        words.extend_from_slice(&[
            mlp_shard.shard_id,
            mlp_shard.gate_output_checksum,
            mlp_shard.up_output_checksum,
            mlp_shard.activation_checksum,
            mlp_shard.down_output_checksum,
        ]);
        words.extend_from_slice(&mlp_shard.gate_output_sample_words);
        words.extend_from_slice(&mlp_shard.up_output_sample_words);
        words.extend_from_slice(&mlp_shard.activation_sample_words);
        words.extend_from_slice(&mlp_shard.down_output_sample_words);
    }
    words
}

fn qwen3_dense_0_6b_initial_hidden_tensor_tile(
    guest_input: &[u8],
    text_output: &Qwen3Dense06bTextOutputReport,
    dim: usize,
    input_embedding: Option<&Qwen3Dense06bEmbeddingReferenceSummary>,
) -> Vec<u8> {
    let guest_input_checksum = qwen3_dense_0_6b_decode_guest_input_checksum(guest_input);
    let embedding_checksum = input_embedding
        .map(|summary| summary.aggregate_checksum)
        .unwrap_or(0);
    let embedding_value_checksum = input_embedding
        .map(|summary| summary.value_checksum)
        .unwrap_or(0);
    let embedding_row_checksum = input_embedding
        .map(|summary| summary.row_checksum)
        .unwrap_or(0);
    let seed = checksum_words(&[
        guest_input_checksum,
        embedding_checksum,
        embedding_value_checksum,
        embedding_row_checksum,
        text_output.attention.aggregate_checksum,
        text_output.post_attention.aggregate_checksum,
        text_output.result_flow.aggregate_checksum,
        text_output.token_checksum,
        text_output.logits_checksum,
    ]);
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let row = elem_index / dim;
        let col = elem_index % dim;
        let embedding_word = input_embedding
            .and_then(|summary| {
                summary
                    .tokens
                    .get(row % summary.tokens.len().max(1))
                    .map(|token| {
                        token.sample_words[col % token.sample_words.len()]
                            ^ token.value_checksum
                            ^ token.row_checksum
                    })
            })
            .unwrap_or(guest_input_checksum);
        let mixed = seed.rotate_left(((row * 13 + col * 17) % 63) as u32)
            ^ embedding_word.rotate_left(((row + col) % 63) as u32)
            ^ (elem_index as u64).rotate_left((col % 63) as u32);
        let value = 1.0
            + (((mixed >> 8) & 0x03ff) as f32 / 1024.0) * 0.01
            + (row as f32 % 7.0) * 0.0001
            + (col as f32 % 11.0) * 0.0001;
        out.extend_from_slice(&f32_to_f16_bits(value).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_next_hidden_tensor_tile(
    input: &[u8],
    dim: usize,
    layer_id: u64,
    owner_node: u64,
    qkv_checksum: u64,
    mlp_checksum: u64,
    result_flow_checksum: u64,
    qkv_summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
    mlp_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
) -> Vec<u8> {
    let real_reference_words =
        qwen3_dense_0_6b_hidden_tensor_real_reference_words(qkv_summary, mlp_summary);
    let real_reference_checksum = if real_reference_words.is_empty() {
        0
    } else {
        checksum_words(&real_reference_words)
    };
    let seed = checksum_words(&[
        layer_id,
        owner_node,
        qkv_checksum,
        mlp_checksum,
        result_flow_checksum,
        real_reference_checksum,
        qwen3_dense_0_6b_shard_output_checksum(input),
    ]);
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let previous = qwen3_dense_0_6b_half_at(input, elem_index);
        let row = elem_index / dim;
        let col = elem_index % dim;
        let mixed = seed.rotate_left(((layer_id as usize + row * 19 + col * 23) % 63) as u32);
        let real_word = real_reference_words
            .get((elem_index + row + col) % real_reference_words.len().max(1))
            .copied()
            .unwrap_or(0);
        let real_mixed = mixed ^ real_word.rotate_left(((row + col) % 63) as u32);
        let signed = if mixed & 1 == 0 { -1.0 } else { 1.0 };
        let layer_scale = 1.0 + ((layer_id % 28) as f32 + 1.0) * 0.001;
        let qkv_bias = (((real_mixed >> 11) & 0x1f) as f32 - 15.5) * 0.0005;
        let mlp_bias = (((real_mixed >> 29) & 0x1f) as f32) * 0.0005 * signed;
        let real_bias = if real_reference_checksum == 0 {
            0.0
        } else {
            (((real_word >> ((elem_index % 4) * 8)) & 0xff) as f32 - 127.5) * 0.00002
        };
        let value = ((previous * layer_scale) + qkv_bias + mlp_bias + real_bias).max(0.5);
        out.extend_from_slice(&f32_to_f16_bits(value).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_hidden_layer_owner_node(layer_id: u64) -> u64 {
    layer_id.saturating_mul(QWEN3_DENSE_0_6B_PROFILE.tp_nodes)
        / QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
}

fn qwen3_dense_0_6b_decode_layer_progress_report(
    text_output: &Qwen3Dense06bTextOutputReport,
    hidden_layer_pipeline: &Qwen3Dense06bHiddenLayerPipelineReport,
) -> Qwen3Dense06bDecodeLayerProgressReport {
    let real_qkv = text_output.real_qkv.as_ref();
    let real_mlp = text_output.real_mlp.as_ref();
    let real_logits = text_output.real_logits.as_ref();
    let first_layer_id = real_qkv
        .map(|report| report.layer_id)
        .or_else(|| real_mlp.map(|report| report.layer_id))
        .unwrap_or(0);
    let next_layer_id = real_mlp
        .map(|report| report.next_layer_id)
        .unwrap_or(first_layer_id + 1);
    let qkv_reference_layer_count = real_qkv
        .map(|report| report.reference_layer_count)
        .unwrap_or(0);
    let qkv_stage_link_count = real_qkv.map(|report| report.stage_link_count).unwrap_or(0);
    let full_layer_path_count = hidden_layer_pipeline.layer_executions.len() as u64;
    let full_layer_path_real_backed = hidden_layer_pipeline.real_layer_executions_all_present;
    let full_layer_path_checksum = checksum_words(
        &hidden_layer_pipeline
            .layer_executions
            .iter()
            .flat_map(|execution| {
                [
                    execution.layer_id,
                    execution.owner_node,
                    execution.input_checksum,
                    execution.qkv_checksum,
                    execution.mlp_checksum,
                    execution.output_checksum,
                    u64::from(execution.starts_node_range),
                ]
            })
            .collect::<Vec<_>>(),
    );
    let full_layer_final_checksum = hidden_layer_pipeline.final_layer_checksum;
    let layer0_path_checksum = checksum_words(&[
        first_layer_id,
        real_qkv
            .map(|report| report.reference_layer_checksum)
            .unwrap_or(0),
        real_mlp
            .map(|report| report.real_activation_checksum)
            .unwrap_or(0),
        real_mlp
            .map(|report| report.real_output_checksum)
            .unwrap_or(0),
        text_output.attention.aggregate_checksum,
        text_output.post_attention.mlp_output_checksum,
        text_output.post_attention.residual_norm_checksum,
    ]);
    let layer1_path_checksum = checksum_words(&[
        next_layer_id,
        real_qkv
            .map(|report| report.next_reference_layer_checksum)
            .unwrap_or(0),
        real_mlp
            .map(|report| report.next_real_output_checksum)
            .unwrap_or(0),
        text_output.post_attention.next_partial_checksum,
        text_output.result_flow.round0_checksum,
        text_output.result_flow.round1_checksum,
    ]);
    let logits_path_checksum = checksum_words(&[
        next_layer_id,
        real_logits
            .map(|report| report.final_norm_checksum)
            .unwrap_or(0),
        real_logits.map(|report| report.row_checksum).unwrap_or(0),
        real_logits.map(|report| report.logit_checksum).unwrap_or(0),
        real_logits
            .map(|report| report.selection_checksum)
            .unwrap_or(0),
        text_output.logits_checksum,
    ]);
    let aggregate_checksum = checksum_words(&[
        first_layer_id,
        next_layer_id,
        qkv_reference_layer_count,
        qkv_stage_link_count,
        full_layer_path_count,
        u64::from(full_layer_path_real_backed),
        full_layer_path_checksum,
        full_layer_final_checksum,
        layer0_path_checksum,
        layer1_path_checksum,
        logits_path_checksum,
    ]);
    Qwen3Dense06bDecodeLayerProgressReport {
        first_layer_id,
        next_layer_id,
        qkv_reference_layer_count,
        qkv_stage_link_count,
        full_layer_path_count,
        full_layer_path_real_backed,
        full_layer_path_checksum,
        full_layer_final_checksum,
        layer0_path_checksum,
        layer1_path_checksum,
        logits_path_checksum,
        aggregate_checksum,
    }
}

fn qwen3_dense_0_6b_decode_chain_checksum(steps: &[Qwen3Dense06bDecodeLoopStepReport]) -> u64 {
    let mut words = Vec::with_capacity(steps.len() * 25);
    for step in steps {
        let real_qkv_value = step
            .text_output
            .real_qkv
            .as_ref()
            .map(|real_qkv| real_qkv.real_value_checksum)
            .unwrap_or(0);
        let real_logits_selection = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.selection_checksum)
            .unwrap_or(0);
        let real_logits_row = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.row_checksum)
            .unwrap_or(0);
        let real_logits_logit = step
            .text_output
            .real_logits
            .as_ref()
            .map(|real_logits| real_logits.logit_checksum)
            .unwrap_or(0);
        let real_mlp_table = step
            .text_output
            .real_mlp
            .as_ref()
            .map(|real_mlp| real_mlp.table_checksum)
            .unwrap_or(0);
        words.extend_from_slice(&[
            step.step_index,
            step.guest_input_checksum,
            step.next_guest_input_checksum,
            step.input_transition.transition_checksum,
            step.input_transition.write_offset_checksum,
            step.input_transition.readback_token_checksum,
            step.input_transition.write_readback_match_count,
            step.input_transition.checksum_slot_value,
            step.sampled_token_count,
            step.text_output.byte_len,
            step.text_output.text_checksum,
            step.text_output.logits_checksum,
            step.text_output.kvcache.read_digest_checksum,
            step.text_output.attention.aggregate_checksum,
            step.text_output.post_attention.aggregate_checksum,
            step.text_output.result_flow.aggregate_checksum,
            step.layer_progress.aggregate_checksum,
            step.hidden_layer_pipeline.aggregate_checksum,
            step.object_service.object_checksum,
            step.real_inference_contract.aggregate_checksum,
            real_qkv_value,
            real_mlp_table,
            real_logits_selection,
            real_logits_row,
            real_logits_logit,
        ]);
    }
    checksum_words(&words)
}

fn qwen3_dense_0_6b_prefill_text_output_report_with_task_id(
    topology: &SimTopology,
    guest_input: &[u8],
    task_id: u64,
) -> Result<Qwen3Dense06bTextOutputReport, String> {
    let guest_input_report = qwen3_dense_0_6b_synthetic_guest_input_report(guest_input);
    qwen3_dense_0_6b_prefill_text_output_report_with_task_id_and_guest_input(
        topology,
        guest_input,
        task_id,
        guest_input_report,
    )
}

fn qwen3_dense_0_6b_prefill_text_output_report_with_task_id_and_guest_input(
    topology: &SimTopology,
    guest_input: &[u8],
    task_id: u64,
    guest_input_report: Qwen3Dense06bGuestInputReport,
) -> Result<Qwen3Dense06bTextOutputReport, String> {
    qwen3_dense_0_6b_prefill_text_output_report_with_task_id_guest_input_and_object_service(
        topology,
        guest_input,
        task_id,
        guest_input_report,
        None,
    )
}

fn qwen3_dense_0_6b_prefill_text_output_report_with_task_id_guest_input_and_object_service(
    topology: &SimTopology,
    guest_input: &[u8],
    task_id: u64,
    guest_input_report: Qwen3Dense06bGuestInputReport,
    object_service: Option<&mut LingquObjectServiceStub>,
) -> Result<Qwen3Dense06bTextOutputReport, String> {
    let output = run_qwen3_dense_0_6b_prefill_runtime(
        topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id,
        },
        guest_input,
        object_service,
    )?;
    let mut report = qwen3_dense_0_6b_text_output_report_from_prefill_output_with_guest_input(
        &output,
        guest_input_report,
    )?;
    report.real_inference = qwen3_dense_0_6b_real_inference_report_from_runtime_samples(
        guest_input,
        report.samples.as_slice(),
    )?;
    Ok(report)
}

pub fn qwen3_dense_0_6b_default_guest_input() -> Vec<u8> {
    vec![0xa5; W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES]
}

pub fn qwen3_dense_0_6b_prompt_guest_input(prompt: &str) -> Vec<u8> {
    qwen3_dense_0_6b_tokenized_prompt_guest_input(prompt, &[])
}

fn qwen3_dense_0_6b_tokenized_prompt_guest_input(prompt: &str, token_ids: &[u64]) -> Vec<u8> {
    let mut input = vec![0u8; W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES];
    let prompt_bytes = prompt.as_bytes();
    let prompt_checksum = qwen3_dense_0_6b_text_output_bytes_checksum(prompt_bytes);
    let token_checksum = prompt_token_ids_checksum(token_ids);
    input[0..8].copy_from_slice(b"Q3PROMPT");
    input[8..16].copy_from_slice(&(prompt_bytes.len() as u64).to_le_bytes());
    input[16..24].copy_from_slice(&prompt_checksum.to_le_bytes());
    input[24..32].copy_from_slice(&(token_ids.len() as u64).to_le_bytes());
    input[32..40].copy_from_slice(&token_checksum.to_le_bytes());
    let token_offset = 64usize;
    let max_tokens = (input.len().saturating_sub(token_offset) / std::mem::size_of::<u64>())
        .min(token_ids.len());
    for (index, token_id) in token_ids.iter().take(max_tokens).enumerate() {
        let offset = token_offset + index * std::mem::size_of::<u64>();
        input[offset..offset + std::mem::size_of::<u64>()].copy_from_slice(&token_id.to_le_bytes());
    }
    let payload_offset = token_offset + max_tokens * std::mem::size_of::<u64>();
    let copy_len = prompt_bytes
        .len()
        .min(input.len().saturating_sub(payload_offset));
    input[payload_offset..payload_offset + copy_len].copy_from_slice(&prompt_bytes[..copy_len]);
    for index in payload_offset + copy_len..input.len() {
        input[index] = (prompt_checksum
            .rotate_left((index % 63) as u32)
            .wrapping_add(index as u64) as u8)
            ^ 0x5a;
    }
    input
}

fn qwen3_dense_0_6b_synthetic_guest_input_report(
    guest_input: &[u8],
) -> Qwen3Dense06bGuestInputReport {
    Qwen3Dense06bGuestInputReport {
        byte_len: guest_input.len() as u64,
        checksum: qwen3_dense_0_6b_decode_guest_input_checksum(guest_input),
        prompt_byte_len: 0,
        prompt_checksum: 0,
        prompt_token_count: 0,
        prompt_token_checksum: 0,
        tokenizer_asset_checksum: 0,
        real_backed: false,
    }
}

fn qwen3_dense_0_6b_prompt_guest_input_report(
    prompt: &str,
    token_ids: &[u64],
    guest_input: &[u8],
) -> Result<Qwen3Dense06bGuestInputReport, String> {
    let tokenizer_asset_checksum = qwen3_dense_0_6b_real_tokenizer_asset_summary()?
        .map(|summary| summary.aggregate_checksum)
        .unwrap_or(0);
    let prompt_checksum = qwen3_dense_0_6b_text_output_bytes_checksum(prompt.as_bytes());
    let prompt_token_checksum = prompt_token_ids_checksum(token_ids);
    Ok(Qwen3Dense06bGuestInputReport {
        byte_len: guest_input.len() as u64,
        checksum: qwen3_dense_0_6b_decode_guest_input_checksum(guest_input),
        prompt_byte_len: prompt.len() as u64,
        prompt_checksum,
        prompt_token_count: token_ids.len() as u64,
        prompt_token_checksum,
        tokenizer_asset_checksum,
        real_backed: !prompt.is_empty()
            && prompt_checksum != 0
            && !token_ids.is_empty()
            && prompt_token_checksum != 0
            && tokenizer_asset_checksum != 0,
    })
}

fn qwen3_dense_0_6b_transition_guest_input_report(
    transition: &Qwen3Dense06bDecodeInputTransitionReport,
    previous: Qwen3Dense06bGuestInputReport,
    guest_input: &[u8],
) -> Qwen3Dense06bGuestInputReport {
    let transition_real_backed = previous.real_backed
        && transition.write_count != 0
        && transition.applied_write_count == transition.write_count
        && transition.write_readback_match_count == transition.write_count
        && transition.transition_checksum != 0
        && transition.checksum_slot_value == transition.transition_checksum;
    Qwen3Dense06bGuestInputReport {
        byte_len: guest_input.len() as u64,
        checksum: qwen3_dense_0_6b_decode_guest_input_checksum(guest_input),
        prompt_byte_len: previous.prompt_byte_len,
        prompt_checksum: previous.prompt_checksum ^ transition.sampled_token_checksum,
        prompt_token_count: qwen3_dense_0_6b_guest_input_token_count(guest_input),
        prompt_token_checksum: prompt_token_ids_checksum(&qwen3_dense_0_6b_guest_input_token_ids(
            guest_input,
        )),
        tokenizer_asset_checksum: previous.tokenizer_asset_checksum,
        real_backed: transition_real_backed,
    }
}

fn qwen3_dense_0_6b_next_decode_guest_input(
    previous: &[u8],
    samples: &[Qwen3Dense06bTextOutputSample],
    loop_step: u64,
) -> Vec<u8> {
    let mut next = qwen3_dense_0_6b_guest_input_payload(previous);
    let previous_token_count = qwen3_dense_0_6b_guest_input_token_count(previous);
    for (sample_index, sample) in samples.iter().enumerate() {
        let offset = qwen3_dense_0_6b_decode_input_append_offset(
            previous_token_count + sample_index as u64,
            next.len(),
        )
        .unwrap_or_else(|| {
            qwen3_dense_0_6b_decode_input_write_offset(sample, loop_step, next.len())
        });
        next[offset..offset + std::mem::size_of::<u64>()]
            .copy_from_slice(&sample.sampled_token.to_le_bytes());
    }
    let mut token_ids = qwen3_dense_0_6b_guest_input_token_ids(previous);
    token_ids.extend(samples.iter().map(|sample| sample.sampled_token));
    if qwen3_dense_0_6b_guest_input_has_prompt_header(&next) && next.len() >= 40 {
        next[24..32].copy_from_slice(&(token_ids.len() as u64).to_le_bytes());
        next[32..40].copy_from_slice(&prompt_token_ids_checksum(&token_ids).to_le_bytes());
    }
    let sample_checksum = qwen3_dense_0_6b_decode_input_transition_checksum(samples, loop_step);
    let checksum_offset = next.len() - std::mem::size_of::<u64>();
    next[checksum_offset..].copy_from_slice(&sample_checksum.to_le_bytes());
    next
}

fn qwen3_dense_0_6b_guest_input_token_count(input: &[u8]) -> u64 {
    if !qwen3_dense_0_6b_guest_input_has_prompt_header(input) {
        return 0;
    }
    input
        .get(24..32)
        .map(|bytes| u64::from_le_bytes(bytes.try_into().expect("guest token count")))
        .unwrap_or(0)
}

fn qwen3_dense_0_6b_guest_input_has_prompt_header(input: &[u8]) -> bool {
    input.get(0..8) == Some(b"Q3PROMPT")
}

fn qwen3_dense_0_6b_guest_input_token_ids(input: &[u8]) -> Vec<u64> {
    let token_count = qwen3_dense_0_6b_guest_input_token_count(input) as usize;
    let max_tokens = input.len().saturating_sub(64) / std::mem::size_of::<u64>();
    let token_count = token_count.min(max_tokens);
    (0..token_count)
        .filter_map(|index| {
            let offset = 64 + index * std::mem::size_of::<u64>();
            input
                .get(offset..offset + std::mem::size_of::<u64>())
                .map(|bytes| u64::from_le_bytes(bytes.try_into().expect("guest token id")))
        })
        .collect()
}

fn qwen3_dense_0_6b_decode_input_append_offset(
    token_index: u64,
    input_len: usize,
) -> Option<usize> {
    let offset =
        64usize.checked_add((token_index as usize).checked_mul(std::mem::size_of::<u64>())?)?;
    (offset + std::mem::size_of::<u64>() <= input_len).then_some(offset)
}

fn qwen3_dense_0_6b_decode_input_write_offset(
    sample: &Qwen3Dense06bTextOutputSample,
    loop_step: u64,
    input_len: usize,
) -> usize {
    ((sample.step_index + loop_step * 17) as usize * std::mem::size_of::<u64>()) % input_len
}

fn qwen3_dense_0_6b_decode_input_transition_checksum(
    samples: &[Qwen3Dense06bTextOutputSample],
    loop_step: u64,
) -> u64 {
    samples
        .iter()
        .fold(0xcbf2_9ce4_8422_2325u64, |acc, sample| {
            acc.wrapping_mul(0x0000_0100_0000_01b3)
                ^ loop_step
                ^ sample.step_index
                ^ sample.sampled_token.rotate_left(7)
                ^ sample.logits_checksum.rotate_left(17)
                ^ sample.text_checksum.rotate_left(29)
        })
}

fn qwen3_dense_0_6b_decode_input_transition_report(
    samples: &[Qwen3Dense06bTextOutputSample],
    loop_step: u64,
    next_input: &[u8],
) -> Qwen3Dense06bDecodeInputTransitionReport {
    let mut write_words = Vec::with_capacity(samples.len() * 3);
    let mut token_words = Vec::with_capacity(samples.len() * 2);
    let mut readback_token_words = Vec::with_capacity(samples.len() * 2);
    let mut logits_words = Vec::with_capacity(samples.len() * 2);
    let mut text_words = Vec::with_capacity(samples.len() * 2);
    let mut applied_write_count = 0u64;
    let mut write_readback_match_count = 0u64;
    for (sample_index, sample) in samples.iter().enumerate() {
        let offset = qwen3_dense_0_6b_decode_input_append_offset(
            qwen3_dense_0_6b_guest_input_token_count(next_input)
                .saturating_sub(samples.len() as u64)
                + sample_index as u64,
            next_input.len(),
        )
        .unwrap_or_else(|| {
            qwen3_dense_0_6b_decode_input_write_offset(sample, loop_step, next_input.len())
        });
        write_words.extend_from_slice(&[sample.step_index, sample.sampled_token, offset as u64]);
        token_words.extend_from_slice(&[sample.step_index, sample.sampled_token]);
        if let Some(token_bytes) = next_input.get(offset..offset + std::mem::size_of::<u64>()) {
            let readback_token =
                u64::from_le_bytes(token_bytes.try_into().expect("decode token readback"));
            readback_token_words.extend_from_slice(&[sample.step_index, readback_token]);
            applied_write_count += 1;
            if readback_token == sample.sampled_token {
                write_readback_match_count += 1;
            }
        }
        logits_words.extend_from_slice(&[sample.step_index, sample.logits_checksum]);
        text_words.extend_from_slice(&[sample.step_index, sample.text_checksum]);
    }
    let checksum_slot_value = next_input
        .get(next_input.len().saturating_sub(std::mem::size_of::<u64>())..next_input.len())
        .map(|bytes| u64::from_le_bytes(bytes.try_into().expect("decode checksum readback")))
        .unwrap_or(0);
    Qwen3Dense06bDecodeInputTransitionReport {
        loop_step,
        write_count: samples.len() as u64,
        applied_write_count,
        write_readback_match_count,
        write_offset_checksum: checksum_words(&write_words),
        sampled_token_checksum: checksum_words(&token_words),
        readback_token_checksum: checksum_words(&readback_token_words),
        logits_checksum: checksum_words(&logits_words),
        text_checksum: checksum_words(&text_words),
        checksum_slot_value,
        transition_checksum: qwen3_dense_0_6b_decode_input_transition_checksum(samples, loop_step),
    }
}

fn qwen3_dense_0_6b_decode_guest_input_checksum(input: &[u8]) -> u64 {
    input
        .iter()
        .enumerate()
        .fold(0xcbf2_9ce4_8422_2325u64, |acc, (index, byte)| {
            acc.wrapping_mul(0x0000_0100_0000_01b3)
                ^ (*byte as u64)
                ^ (index as u64).rotate_left((index % 63) as u32)
        })
}

pub fn qwen3_dense_0_6b_text_output_report_from_prefill_output(
    output: &[u8],
) -> Result<Qwen3Dense06bTextOutputReport, String> {
    qwen3_dense_0_6b_text_output_report_from_prefill_output_with_guest_input(
        output,
        qwen3_dense_0_6b_synthetic_guest_input_report(&[]),
    )
}

fn qwen3_dense_0_6b_text_output_report_from_prefill_output_with_guest_input(
    output: &[u8],
    guest_input: Qwen3Dense06bGuestInputReport,
) -> Result<Qwen3Dense06bTextOutputReport, String> {
    const MARKER_LOGITS_TABLE: u64 = 0x713377346c6f6730;
    const MARKER_KVCACHE_TABLE: u64 = 0x713377346b766330;
    const MARKER_KVCACHE_STATE_TABLE: u64 = 0x713377346b767331;
    const MARKER_TOKEN_TEXT_TABLE: u64 = 0x7133773474787430;
    const MARKER_TEXT_OUTPUT_TABLE: u64 = 0x71337734746f7430;
    const MARKER_TEXT_OUTPUT_BYTES_TABLE: u64 = 0x71337734746f6230;
    const MARKER_WEIGHT_REFERENCE_TABLE: u64 = 0x7133773477667430;
    const MARKER_WEIGHT_STAGE_LINK_TABLE: u64 = 0x71337734776c6b30;
    const MARKER_LAYER_DEP_TABLE: u64 = 0x7133773464657030;
    const MARKER_RESULT_TABLE: u64 = 0x7133773474626c30;
    const MARKER_MLP_REFERENCE_TABLE: u64 = 0x713377346d6c7030;
    const MARKER_LOGITS_REFERENCE_TABLE: u64 = 0x713377346c6d6830;

    let text_output_header = find_u64_marker(output, MARKER_TEXT_OUTPUT_TABLE)
        .ok_or_else(|| "qwen3_text_output_summary_table_missing".to_string())?;
    let token_count = read_u64_le_checked(
        output,
        text_output_header + 8,
        "qwen3_text_output_token_count",
    )?;
    let total_byte_len = read_u64_le_checked(
        output,
        text_output_header + 16,
        "qwen3_text_output_total_bytes",
    )?;
    let sequence_checksum = read_u64_le_checked(
        output,
        text_output_header + 24,
        "qwen3_text_output_sequence_checksum",
    )?;
    let token_checksum = read_u64_le_checked(
        output,
        text_output_header + 32,
        "qwen3_text_output_token_checksum",
    )?;
    let text_checksum = read_u64_le_checked(
        output,
        text_output_header + 40,
        "qwen3_text_output_text_checksum",
    )?;
    let logits_checksum = read_u64_le_checked(
        output,
        text_output_header + 48,
        "qwen3_text_output_logits_checksum",
    )?;
    let tokenizer_policy_kind = read_u64_le_checked(
        output,
        text_output_header + 56,
        "qwen3_text_output_policy_kind",
    )?;

    let header = find_u64_marker(output, MARKER_TEXT_OUTPUT_BYTES_TABLE)
        .ok_or_else(|| "qwen3_text_output_bytes_table_missing".to_string())?;
    let byte_len = read_u64_le_checked(output, header + 8, "qwen3_text_output_byte_len")?;
    let word_count = read_u64_le_checked(output, header + 16, "qwen3_text_output_words")?;
    let padded_byte_len =
        read_u64_le_checked(output, header + 24, "qwen3_text_output_padded_bytes")?;
    let byte_checksum = read_u64_le_checked(output, header + 32, "qwen3_text_output_checksum")?;
    let bytes_sequence_checksum =
        read_u64_le_checked(output, header + 40, "qwen3_text_output_sequence_checksum")?;
    let bytes_token_count =
        read_u64_le_checked(output, header + 48, "qwen3_text_output_token_count")?;
    let bytes_tokenizer_policy_kind =
        read_u64_le_checked(output, header + 56, "qwen3_text_output_policy_kind")?;
    if byte_len != total_byte_len {
        return Err(format!(
            "qwen3_text_output_summary_len_mismatch:summary={total_byte_len}:bytes={byte_len}"
        ));
    }
    if bytes_sequence_checksum != sequence_checksum {
        return Err("qwen3_text_output_sequence_checksum_mismatch".to_string());
    }
    if bytes_token_count != token_count {
        return Err(format!(
            "qwen3_text_output_token_count_mismatch:summary={token_count}:bytes={bytes_token_count}"
        ));
    }
    if bytes_tokenizer_policy_kind != tokenizer_policy_kind {
        return Err("qwen3_text_output_policy_kind_mismatch".to_string());
    }
    if padded_byte_len != word_count * std::mem::size_of::<u64>() as u64 {
        return Err(format!(
            "qwen3_text_output_padded_len_mismatch:words={word_count}:padded={padded_byte_len}"
        ));
    }
    if byte_len > padded_byte_len {
        return Err(format!(
            "qwen3_text_output_len_exceeds_padded:bytes={byte_len}:padded={padded_byte_len}"
        ));
    }
    let base = header + 64;
    let end = base
        .checked_add(padded_byte_len as usize)
        .ok_or_else(|| "qwen3_text_output_table_end_overflow".to_string())?;
    if end > output.len() {
        return Err(format!(
            "qwen3_text_output_table_oob:end={end}:output_len={}",
            output.len()
        ));
    }
    let byte_end = base
        .checked_add(byte_len as usize)
        .ok_or_else(|| "qwen3_text_output_bytes_end_overflow".to_string())?;
    let bytes = output[base..byte_end].to_vec();
    if qwen3_dense_0_6b_text_output_bytes_checksum(&bytes) != byte_checksum {
        return Err("qwen3_text_output_checksum_mismatch".to_string());
    }
    if output[byte_end..end].iter().any(|byte| *byte != 0) {
        return Err("qwen3_text_output_padding_nonzero".to_string());
    }
    let samples = qwen3_dense_0_6b_text_output_samples_from_prefill_output(
        output,
        MARKER_LOGITS_TABLE,
        MARKER_TOKEN_TEXT_TABLE,
        token_count,
        &bytes,
    )?;
    let kvcache = qwen3_dense_0_6b_kvcache_report_from_prefill_output(
        output,
        MARKER_KVCACHE_TABLE,
        MARKER_KVCACHE_STATE_TABLE,
    )?;
    let attention =
        qwen3_dense_0_6b_attention_report_from_prefill_output(output, MARKER_LAYER_DEP_TABLE)?;
    let post_attention =
        qwen3_dense_0_6b_post_attention_report_from_prefill_output(output, MARKER_LAYER_DEP_TABLE)?;
    let result_flow =
        qwen3_dense_0_6b_result_flow_report_from_prefill_output(output, MARKER_RESULT_TABLE)?;
    let real_qkv = qwen3_dense_0_6b_qkv_reference_report_from_prefill_output(
        output,
        MARKER_WEIGHT_REFERENCE_TABLE,
        MARKER_WEIGHT_STAGE_LINK_TABLE,
    )?;
    let real_mlp = qwen3_dense_0_6b_mlp_reference_report_from_prefill_output(
        output,
        MARKER_MLP_REFERENCE_TABLE,
    )?;
    let real_logits = qwen3_dense_0_6b_logits_reference_report_from_prefill_output(
        output,
        MARKER_RESULT_TABLE,
        MARKER_LOGITS_REFERENCE_TABLE,
        &samples,
    )?;
    let synthetic = qwen3_dense_0_6b_synthetic_stage_report(
        &guest_input,
        real_qkv.as_ref(),
        &attention,
        &post_attention,
        real_mlp.as_ref(),
        real_logits.as_ref(),
        tokenizer_policy_kind,
    );
    let text_lossy = String::from_utf8_lossy(&bytes).to_string();
    Ok(Qwen3Dense06bTextOutputReport {
        token_count,
        byte_len,
        padded_byte_len,
        byte_checksum,
        sequence_checksum,
        token_checksum,
        text_checksum,
        logits_checksum,
        tokenizer_policy_kind,
        guest_input,
        kvcache,
        attention,
        post_attention,
        result_flow,
        real_qkv,
        real_mlp,
        real_logits,
        real_inference: None,
        synthetic,
        samples,
        bytes,
        text_lossy,
    })
}

fn qwen3_dense_0_6b_real_inference_report_from_runtime_samples(
    guest_input: &[u8],
    samples: &[Qwen3Dense06bTextOutputSample],
) -> Result<Option<Qwen3Dense06bRealInferenceReferenceReport>, String> {
    let token_ids = qwen3_dense_0_6b_guest_input_token_ids(guest_input);
    if token_ids.is_empty() {
        return Ok(None);
    }
    if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_err() {
        return Ok(None);
    }
    let Some(reference_sample) = samples.iter().find(|sample| {
        sample.runtime_forward_layer_count == QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
            && sample.runtime_forward_final_hidden_checksum != 0
            && sample.runtime_forward_checksum != 0
            && sample.full_vocab_checked_token_count == QWEN3_DENSE_0_6B_PROFILE.vocab_size
            && sample.full_vocab_logits_checksum != 0
            && sample.byte_len != 0
    }) else {
        return Ok(None);
    };
    let sampled_token_match_count = samples
        .iter()
        .filter(|sample| sample.sampled_token == reference_sample.sampled_token)
        .count() as u64;
    let sampled_text_match_count = samples
        .iter()
        .filter(|sample| {
            sample.sampled_token == reference_sample.sampled_token
                && sample.byte_len == reference_sample.byte_len
                && sample.piece_lossy == reference_sample.piece_lossy
        })
        .count() as u64;
    let prompt_token_checksum = prompt_token_ids_checksum(&token_ids);
    let sampled_text_byte_checksum =
        qwen3_dense_0_6b_text_output_bytes_checksum(reference_sample.piece_lossy.as_bytes());
    let aggregate_checksum = checksum_words(&[
        token_ids.len() as u64,
        prompt_token_checksum,
        reference_sample.runtime_forward_layer_count,
        reference_sample.runtime_forward_final_hidden_checksum,
        reference_sample.runtime_forward_checksum,
        reference_sample.full_vocab_checked_token_count,
        reference_sample.full_vocab_logits_checksum,
        reference_sample.sampled_token,
        reference_sample.byte_len,
        sampled_text_byte_checksum,
        samples.len() as u64,
        sampled_token_match_count,
        sampled_text_match_count,
        reference_sample.logits_checksum,
    ]);
    Ok(Some(Qwen3Dense06bRealInferenceReferenceReport {
        prompt_token_count: token_ids.len() as u64,
        prompt_token_checksum,
        layer_count: reference_sample.runtime_forward_layer_count,
        final_hidden_checksum: reference_sample.runtime_forward_final_hidden_checksum,
        forward_checksum: reference_sample.runtime_forward_checksum,
        full_vocab_checked_token_count: reference_sample.full_vocab_checked_token_count,
        full_vocab_logits_checksum: reference_sample.full_vocab_logits_checksum,
        sampled_token: reference_sample.sampled_token,
        sampled_text_byte_len: reference_sample.byte_len,
        sampled_text_byte_checksum,
        output_sample_count: samples.len() as u64,
        sampled_token_match_count,
        sampled_text_match_count,
        aggregate_checksum,
    }))
}

fn qwen3_dense_0_6b_kvcache_report_from_prefill_output(
    output: &[u8],
    kvcache_marker: u64,
    kvcache_state_marker: u64,
) -> Result<Qwen3Dense06bKvCacheReport, String> {
    const KVCACHE_ENTRY_WORDS: u64 = 14;
    const KVCACHE_STATE_ENTRY_WORDS: u64 = 8;
    let kvcache_header = find_u64_marker(output, kvcache_marker)
        .ok_or_else(|| "qwen3_kvcache_table_missing".to_string())?;
    let descriptor_count = read_u64_le_checked(output, kvcache_header + 8, "qwen3_kvcache_count")?;
    let entry_words =
        read_u64_le_checked(output, kvcache_header + 16, "qwen3_kvcache_entry_words")?;
    let table_bytes =
        read_u64_le_checked(output, kvcache_header + 24, "qwen3_kvcache_table_bytes")?;
    if entry_words != KVCACHE_ENTRY_WORDS {
        return Err(format!(
            "qwen3_kvcache_entry_words_mismatch:expected={KVCACHE_ENTRY_WORDS}:actual={entry_words}"
        ));
    }
    let entry_bytes = entry_words * std::mem::size_of::<u64>() as u64;
    if table_bytes != descriptor_count * entry_bytes {
        return Err(format!(
            "qwen3_kvcache_table_bytes_mismatch:count={descriptor_count}:entry_bytes={entry_bytes}:table_bytes={table_bytes}"
        ));
    }
    let kvcache_base = kvcache_header
        .checked_add(64)
        .ok_or_else(|| "qwen3_kvcache_base_overflow".to_string())?;
    let kvcache_end = kvcache_base
        .checked_add(table_bytes as usize)
        .ok_or_else(|| "qwen3_kvcache_end_overflow".to_string())?;
    if kvcache_end > output.len() {
        return Err(format!(
            "qwen3_kvcache_table_oob:end={kvcache_end}:output_len={}",
            output.len()
        ));
    }

    let mut append_block_count = 0u64;
    let mut update_seq_sum = 0u64;
    let mut prefill_entry_count = 0u64;
    let mut decode_entry_count = 0u64;
    for entry in 0..descriptor_count as usize {
        let base = kvcache_base + entry * entry_bytes as usize;
        let append_start = read_u64_le_checked(output, base + 40, "qwen3_kvcache_append_start")?;
        let append_end = read_u64_le_checked(output, base + 48, "qwen3_kvcache_append_end")?;
        if append_end < append_start {
            return Err(format!(
                "qwen3_kvcache_append_window_invalid:entry={entry}:start={append_start}:end={append_end}"
            ));
        }
        let appended = append_end - append_start;
        if appended == 0 {
            return Err(format!("qwen3_kvcache_append_empty:entry={entry}"));
        }
        if appended == 1 {
            decode_entry_count += 1;
        } else {
            prefill_entry_count += 1;
        }
        append_block_count += appended;
        update_seq_sum += read_u64_le_checked(output, base + 72, "qwen3_kvcache_update_seq")?;
        let k_checksum = read_u64_le_checked(output, base + 96, "qwen3_kvcache_k_checksum")?;
        let v_checksum = read_u64_le_checked(output, base + 104, "qwen3_kvcache_v_checksum")?;
        if k_checksum == 0 || v_checksum == 0 || k_checksum == v_checksum {
            return Err(format!(
                "qwen3_kvcache_checksum_invalid:entry={entry}:k={k_checksum:#x}:v={v_checksum:#x}"
            ));
        }
    }

    let state_header = find_u64_marker(output, kvcache_state_marker)
        .ok_or_else(|| "qwen3_kvcache_state_table_missing".to_string())?;
    let state_count = read_u64_le_checked(output, state_header + 8, "qwen3_kvcache_state_count")?;
    let state_entry_words =
        read_u64_le_checked(output, state_header + 16, "qwen3_kvcache_state_entry_words")?;
    let state_table_bytes =
        read_u64_le_checked(output, state_header + 24, "qwen3_kvcache_state_table_bytes")?;
    if state_entry_words != KVCACHE_STATE_ENTRY_WORDS {
        return Err(format!(
            "qwen3_kvcache_state_entry_words_mismatch:expected={KVCACHE_STATE_ENTRY_WORDS}:actual={state_entry_words}"
        ));
    }
    let state_entry_bytes = state_entry_words * std::mem::size_of::<u64>() as u64;
    if state_table_bytes != state_count * state_entry_bytes {
        return Err(format!(
            "qwen3_kvcache_state_table_bytes_mismatch:count={state_count}:entry_bytes={state_entry_bytes}:table_bytes={state_table_bytes}"
        ));
    }
    let state_base = state_header
        .checked_add(64)
        .ok_or_else(|| "qwen3_kvcache_state_base_overflow".to_string())?;
    let state_end = state_base
        .checked_add(state_table_bytes as usize)
        .ok_or_else(|| "qwen3_kvcache_state_end_overflow".to_string())?;
    if state_end > output.len() {
        return Err(format!(
            "qwen3_kvcache_state_table_oob:end={state_end}:output_len={}",
            output.len()
        ));
    }

    let mut read_window_end_max = 0u64;
    let mut read_digest_words = Vec::with_capacity(state_count as usize);
    let mut state_snapshots = Vec::with_capacity(state_count as usize);
    for entry in 0..state_count as usize {
        let base = state_base + entry * state_entry_bytes as usize;
        let layer_id = read_u64_le_checked(output, base, "qwen3_kvcache_state_layer")?;
        let tile_id = read_u64_le_checked(output, base + 8, "qwen3_kvcache_state_tile")?;
        let position = read_u64_le_checked(output, base + 16, "qwen3_kvcache_state_position")?;
        let update_seq = read_u64_le_checked(output, base + 24, "qwen3_kvcache_state_update_seq")?;
        let k_checksum = read_u64_le_checked(output, base + 32, "qwen3_kvcache_state_k_checksum")?;
        let v_checksum = read_u64_le_checked(output, base + 40, "qwen3_kvcache_state_v_checksum")?;
        let read_window_end =
            read_u64_le_checked(output, base + 48, "qwen3_kvcache_state_read_end")?;
        let read_digest =
            read_u64_le_checked(output, base + 56, "qwen3_kvcache_state_read_digest")?;
        if k_checksum == 0 || v_checksum == 0 || k_checksum == v_checksum {
            return Err(format!(
                "qwen3_kvcache_state_checksum_invalid:entry={entry}:k={k_checksum:#x}:v={v_checksum:#x}"
            ));
        }
        if read_digest == 0 {
            return Err(format!(
                "qwen3_kvcache_state_read_digest_zero:entry={entry}"
            ));
        }
        read_window_end_max = read_window_end_max.max(read_window_end);
        read_digest_words.push(read_digest);
        state_snapshots.push(Qwen3Dense06bKvCacheStateSnapshot {
            layer_id,
            tile_id,
            position,
            update_seq,
            k_checksum,
            v_checksum,
            read_window_end,
            read_digest,
        });
    }

    Ok(Qwen3Dense06bKvCacheReport {
        descriptor_count,
        state_count,
        append_block_count,
        update_seq_sum,
        prefill_entry_count,
        decode_entry_count,
        read_window_end_max,
        read_digest_checksum: checksum_words(&read_digest_words),
        state_snapshots,
    })
}

fn qwen3_dense_0_6b_attention_report_from_prefill_output(
    output: &[u8],
    layer_dependency_marker: u64,
) -> Result<Qwen3Dense06bAttentionReport, String> {
    const LAYER_DEP_ENTRY_WORDS: u64 = 11;

    let header = find_u64_marker(output, layer_dependency_marker)
        .ok_or_else(|| "qwen3_attention_layer_dependency_table_missing".to_string())?;
    let entry_count = read_u64_le_checked(output, header + 8, "qwen3_attention_layer_dep_count")?;
    let entry_words =
        read_u64_le_checked(output, header + 16, "qwen3_attention_layer_dep_entry_words")?;
    let table_bytes =
        read_u64_le_checked(output, header + 24, "qwen3_attention_layer_dep_table_bytes")?;
    if entry_words != LAYER_DEP_ENTRY_WORDS {
        return Err(format!(
            "qwen3_attention_layer_dep_entry_words_mismatch:expected={LAYER_DEP_ENTRY_WORDS}:actual={entry_words}"
        ));
    }
    let entry_bytes = (entry_words as usize)
        .checked_mul(std::mem::size_of::<u64>())
        .ok_or_else(|| "qwen3_attention_layer_dep_entry_bytes_overflow".to_string())?;
    let expected_table_bytes = (entry_count as usize)
        .checked_mul(entry_bytes)
        .ok_or_else(|| "qwen3_attention_layer_dep_table_bytes_overflow".to_string())?;
    if table_bytes as usize != expected_table_bytes {
        return Err(format!(
            "qwen3_attention_layer_dep_table_bytes_mismatch:count={entry_count}:entry_bytes={entry_bytes}:table_bytes={table_bytes}"
        ));
    }
    let base = header
        .checked_add(64)
        .ok_or_else(|| "qwen3_attention_layer_dep_base_overflow".to_string())?;
    let end = base
        .checked_add(expected_table_bytes)
        .ok_or_else(|| "qwen3_attention_layer_dep_end_overflow".to_string())?;
    if end > output.len() {
        return Err(format!(
            "qwen3_attention_layer_dep_table_oob:end={end}:output_len={}",
            output.len()
        ));
    }

    let mut score_words = Vec::new();
    let mut softmax_words = Vec::new();
    let mut context_words = Vec::new();
    let mut stage_mask = 0u64;
    for index in 0..entry_count as usize {
        let entry = base + index * entry_bytes;
        let layer_id = read_u64_le_checked(output, entry, "qwen3_attention_layer_id")?;
        let shard_id = read_u64_le_checked(output, entry + 8, "qwen3_attention_shard_id")?;
        let stage_kind = read_u64_le_checked(output, entry + 16, "qwen3_attention_stage_kind")?;
        let depends_on_stage =
            read_u64_le_checked(output, entry + 24, "qwen3_attention_depends_on_stage")?;
        let remote_shard_id =
            read_u64_le_checked(output, entry + 32, "qwen3_attention_remote_shard_id")?;
        let segment = read_u64_le_checked(output, entry + 40, "qwen3_attention_segment")?;
        let elems = read_u64_le_checked(output, entry + 48, "qwen3_attention_elems")?;
        let bytes = read_u64_le_checked(output, entry + 56, "qwen3_attention_bytes")?;
        let checksum = read_u64_le_checked(output, entry + 80, "qwen3_attention_checksum")?;
        let Some(words) = (match stage_kind {
            7 | 19 => {
                stage_mask |= 0x01;
                Some(&mut score_words)
            }
            8 | 20 => {
                stage_mask |= 0x02;
                Some(&mut softmax_words)
            }
            9 | 21 => {
                stage_mask |= 0x04;
                Some(&mut context_words)
            }
            _ => None,
        }) else {
            continue;
        };
        if shard_id >= QWEN3_DENSE_0_6B_PROFILE.tp_nodes
            || remote_shard_id >= QWEN3_DENSE_0_6B_PROFILE.tp_nodes
            || segment == 0
            || elems == 0
            || bytes == 0
            || checksum == 0
        {
            return Err(format!(
                "qwen3_attention_layer_dep_entry_invalid:entry={index}:stage={stage_kind}"
            ));
        }
        words.push(
            layer_id.rotate_left(3)
                ^ shard_id.rotate_left(7)
                ^ stage_kind.rotate_left(11)
                ^ depends_on_stage.rotate_left(17)
                ^ remote_shard_id.rotate_left(23)
                ^ checksum.rotate_left(31),
        );
    }

    if score_words.is_empty() || softmax_words.is_empty() || context_words.is_empty() {
        return Err(format!(
            "qwen3_attention_stage_coverage_incomplete:score={}:softmax={}:context={}",
            score_words.len(),
            softmax_words.len(),
            context_words.len()
        ));
    }
    let score_checksum = checksum_words(&score_words);
    let softmax_checksum = checksum_words(&softmax_words);
    let context_checksum = checksum_words(&context_words);
    Ok(Qwen3Dense06bAttentionReport {
        score_count: score_words.len() as u64,
        softmax_count: softmax_words.len() as u64,
        context_count: context_words.len() as u64,
        stage_mask,
        score_checksum,
        softmax_checksum,
        context_checksum,
        aggregate_checksum: checksum_words(&[
            score_words.len() as u64,
            softmax_words.len() as u64,
            context_words.len() as u64,
            stage_mask,
            score_checksum,
            softmax_checksum,
            context_checksum,
        ]),
    })
}

fn qwen3_dense_0_6b_post_attention_report_from_prefill_output(
    output: &[u8],
    layer_dependency_marker: u64,
) -> Result<Qwen3Dense06bPostAttentionReport, String> {
    const LAYER_DEP_ENTRY_WORDS: u64 = 11;

    let header = find_u64_marker(output, layer_dependency_marker)
        .ok_or_else(|| "qwen3_post_attention_layer_dependency_table_missing".to_string())?;
    let entry_count =
        read_u64_le_checked(output, header + 8, "qwen3_post_attention_layer_dep_count")?;
    let entry_words = read_u64_le_checked(
        output,
        header + 16,
        "qwen3_post_attention_layer_dep_entry_words",
    )?;
    let table_bytes = read_u64_le_checked(
        output,
        header + 24,
        "qwen3_post_attention_layer_dep_table_bytes",
    )?;
    if entry_words != LAYER_DEP_ENTRY_WORDS {
        return Err(format!(
            "qwen3_post_attention_layer_dep_entry_words_mismatch:expected={LAYER_DEP_ENTRY_WORDS}:actual={entry_words}"
        ));
    }
    let entry_bytes = (entry_words as usize)
        .checked_mul(std::mem::size_of::<u64>())
        .ok_or_else(|| "qwen3_post_attention_layer_dep_entry_bytes_overflow".to_string())?;
    let expected_table_bytes = (entry_count as usize)
        .checked_mul(entry_bytes)
        .ok_or_else(|| "qwen3_post_attention_layer_dep_table_bytes_overflow".to_string())?;
    if table_bytes as usize != expected_table_bytes {
        return Err(format!(
            "qwen3_post_attention_layer_dep_table_bytes_mismatch:count={entry_count}:entry_bytes={entry_bytes}:table_bytes={table_bytes}"
        ));
    }
    let base = header
        .checked_add(64)
        .ok_or_else(|| "qwen3_post_attention_layer_dep_base_overflow".to_string())?;
    let end = base
        .checked_add(expected_table_bytes)
        .ok_or_else(|| "qwen3_post_attention_layer_dep_end_overflow".to_string())?;
    if end > output.len() {
        return Err(format!(
            "qwen3_post_attention_layer_dep_table_oob:end={end}:output_len={}",
            output.len()
        ));
    }

    let mut mlp_activation_words = Vec::new();
    let mut host_partial_words = Vec::new();
    let mut mlp_output_words = Vec::new();
    let mut residual_norm_words = Vec::new();
    let mut next_partial_words = Vec::new();
    let mut stage_mask = 0u64;
    for index in 0..entry_count as usize {
        let entry = base + index * entry_bytes;
        let layer_id = read_u64_le_checked(output, entry, "qwen3_post_attention_layer_id")?;
        let shard_id = read_u64_le_checked(output, entry + 8, "qwen3_post_attention_shard_id")?;
        let stage_kind =
            read_u64_le_checked(output, entry + 16, "qwen3_post_attention_stage_kind")?;
        let depends_on_stage =
            read_u64_le_checked(output, entry + 24, "qwen3_post_attention_depends_on_stage")?;
        let remote_shard_id =
            read_u64_le_checked(output, entry + 32, "qwen3_post_attention_remote_shard_id")?;
        let segment = read_u64_le_checked(output, entry + 40, "qwen3_post_attention_segment")?;
        let elems = read_u64_le_checked(output, entry + 48, "qwen3_post_attention_elems")?;
        let bytes = read_u64_le_checked(output, entry + 56, "qwen3_post_attention_bytes")?;
        let checksum = read_u64_le_checked(output, entry + 80, "qwen3_post_attention_checksum")?;
        let Some(words) = (match stage_kind {
            10 => {
                stage_mask |= 0x01;
                Some(&mut mlp_activation_words)
            }
            11 => {
                stage_mask |= 0x02;
                Some(&mut host_partial_words)
            }
            12 => {
                stage_mask |= 0x04;
                Some(&mut mlp_output_words)
            }
            13 => {
                stage_mask |= 0x08;
                Some(&mut residual_norm_words)
            }
            22 => {
                stage_mask |= 0x10;
                Some(&mut next_partial_words)
            }
            _ => None,
        }) else {
            continue;
        };
        if shard_id >= QWEN3_DENSE_0_6B_PROFILE.tp_nodes
            || remote_shard_id >= QWEN3_DENSE_0_6B_PROFILE.tp_nodes
            || segment == 0
            || elems == 0
            || bytes == 0
            || checksum == 0
        {
            return Err(format!(
                "qwen3_post_attention_layer_dep_entry_invalid:entry={index}:stage={stage_kind}"
            ));
        }
        words.push(
            layer_id.rotate_left(3)
                ^ shard_id.rotate_left(7)
                ^ stage_kind.rotate_left(11)
                ^ depends_on_stage.rotate_left(17)
                ^ remote_shard_id.rotate_left(23)
                ^ checksum.rotate_left(31),
        );
    }

    if mlp_activation_words.is_empty()
        || host_partial_words.is_empty()
        || mlp_output_words.is_empty()
        || residual_norm_words.is_empty()
        || next_partial_words.is_empty()
    {
        return Err(format!(
            "qwen3_post_attention_stage_coverage_incomplete:activation={}:host_partial={}:mlp_output={}:residual={}:next_partial={}",
            mlp_activation_words.len(),
            host_partial_words.len(),
            mlp_output_words.len(),
            residual_norm_words.len(),
            next_partial_words.len()
        ));
    }
    let mlp_activation_checksum = checksum_words(&mlp_activation_words);
    let host_partial_checksum = checksum_words(&host_partial_words);
    let mlp_output_checksum = checksum_words(&mlp_output_words);
    let residual_norm_checksum = checksum_words(&residual_norm_words);
    let next_partial_checksum = checksum_words(&next_partial_words);
    Ok(Qwen3Dense06bPostAttentionReport {
        mlp_activation_count: mlp_activation_words.len() as u64,
        host_partial_count: host_partial_words.len() as u64,
        mlp_output_count: mlp_output_words.len() as u64,
        residual_norm_count: residual_norm_words.len() as u64,
        next_partial_count: next_partial_words.len() as u64,
        stage_mask,
        mlp_activation_checksum,
        host_partial_checksum,
        mlp_output_checksum,
        residual_norm_checksum,
        next_partial_checksum,
        aggregate_checksum: checksum_words(&[
            mlp_activation_words.len() as u64,
            host_partial_words.len() as u64,
            mlp_output_words.len() as u64,
            residual_norm_words.len() as u64,
            next_partial_words.len() as u64,
            stage_mask,
            mlp_activation_checksum,
            host_partial_checksum,
            mlp_output_checksum,
            residual_norm_checksum,
            next_partial_checksum,
        ]),
    })
}

fn qwen3_dense_0_6b_result_flow_report_from_prefill_output(
    output: &[u8],
    result_marker: u64,
) -> Result<Qwen3Dense06bResultFlowReport, String> {
    const RESULT_ENTRY_WORDS: u64 = 10;
    const MARKER_PUBLISH: u64 = 0x7133773470756230;
    const MARKER_RESOLVE: u64 = 0x7133773472657331;
    const MARKER_COMPUTE: u64 = 0x71337734636d7031;
    const MARKER_ROUND1_SUMMARY: u64 = 0x7133773472643130;

    let publish_marker = read_u64_le_checked(output, 8, "qwen3_result_flow_publish_marker")?;
    let resolve_marker = read_u64_le_checked(output, 16, "qwen3_result_flow_resolve_marker")?;
    let compute_marker = read_u64_le_checked(output, 24, "qwen3_result_flow_compute_marker")?;
    if publish_marker != MARKER_PUBLISH
        || resolve_marker != MARKER_RESOLVE
        || compute_marker != MARKER_COMPUTE
    {
        return Err("qwen3_result_flow_header_marker_mismatch".to_string());
    }
    let publish_count = read_u64_le_checked(output, 32, "qwen3_result_flow_publish_count")?;
    let resolve_count = read_u64_le_checked(output, 40, "qwen3_result_flow_resolve_count")?;
    let round1_compute_count =
        read_u64_le_checked(output, 48, "qwen3_result_flow_round1_compute_count")?;
    let round0_count = read_u64_le_checked(output, 64, "qwen3_result_flow_round0_count")?;
    let round1_marker = read_u64_le_checked(output, 96, "qwen3_result_flow_round1_marker")?;
    let round1_count = read_u64_le_checked(output, 104, "qwen3_result_flow_round1_count")?;
    let round0_distinct_count =
        read_u64_le_checked(output, 112, "qwen3_result_flow_round0_distinct")?;
    let round1_distinct_count =
        read_u64_le_checked(output, 120, "qwen3_result_flow_round1_distinct")?;
    if round1_marker != MARKER_ROUND1_SUMMARY {
        return Err("qwen3_result_flow_round1_marker_mismatch".to_string());
    }

    let result_header = find_u64_marker(output, result_marker)
        .ok_or_else(|| "qwen3_result_flow_result_table_missing".to_string())?;
    let result_count =
        read_u64_le_checked(output, result_header + 8, "qwen3_result_flow_result_count")?;
    let entry_words =
        read_u64_le_checked(output, result_header + 16, "qwen3_result_flow_entry_words")?;
    let table_bytes =
        read_u64_le_checked(output, result_header + 24, "qwen3_result_flow_table_bytes")?;
    if entry_words != RESULT_ENTRY_WORDS {
        return Err(format!(
            "qwen3_result_flow_entry_words_mismatch:expected={RESULT_ENTRY_WORDS}:actual={entry_words}"
        ));
    }
    let entry_bytes = (entry_words as usize)
        .checked_mul(std::mem::size_of::<u64>())
        .ok_or_else(|| "qwen3_result_flow_entry_bytes_overflow".to_string())?;
    if table_bytes as usize != result_count as usize * entry_bytes {
        return Err(format!(
            "qwen3_result_flow_table_bytes_mismatch:count={result_count}:entry_bytes={entry_bytes}:table_bytes={table_bytes}"
        ));
    }
    let base = result_header
        .checked_add(64)
        .ok_or_else(|| "qwen3_result_flow_base_overflow".to_string())?;
    let end = base
        .checked_add(table_bytes as usize)
        .ok_or_else(|| "qwen3_result_flow_end_overflow".to_string())?;
    if end > output.len() {
        return Err(format!(
            "qwen3_result_flow_table_oob:end={end}:output_len={}",
            output.len()
        ));
    }

    let mut round0_words = Vec::with_capacity(result_count as usize);
    let mut round1_words = Vec::with_capacity(result_count as usize);
    for index in 0..result_count as usize {
        let entry = base + index * entry_bytes;
        let shard_id = read_u64_le_checked(output, entry, "qwen3_result_flow_shard")?;
        let tile_id = read_u64_le_checked(output, entry + 24, "qwen3_result_flow_tile")?;
        let round0_segment =
            read_u64_le_checked(output, entry + 48, "qwen3_result_flow_round0_segment")?;
        let round1_segment =
            read_u64_le_checked(output, entry + 56, "qwen3_result_flow_round1_segment")?;
        let round0_checksum =
            read_u64_le_checked(output, entry + 64, "qwen3_result_flow_round0_checksum")?;
        let round1_checksum =
            read_u64_le_checked(output, entry + 72, "qwen3_result_flow_round1_checksum")?;
        if shard_id >= QWEN3_DENSE_0_6B_PROFILE.tp_nodes
            || round0_segment == 0
            || round1_segment == 0
            || round0_checksum == 0
            || round1_checksum == 0
        {
            return Err(format!("qwen3_result_flow_entry_invalid:entry={index}"));
        }
        round0_words.push(tile_id.rotate_left(7) ^ round0_checksum.rotate_left(17));
        round1_words.push(tile_id.rotate_left(11) ^ round1_checksum.rotate_left(23));
    }
    if publish_count != result_count
        || resolve_count != result_count
        || round1_compute_count != result_count
        || round0_count != result_count
        || round1_count != result_count
    {
        return Err(format!(
            "qwen3_result_flow_count_mismatch:publish={publish_count}:resolve={resolve_count}:compute={round1_compute_count}:round0={round0_count}:round1={round1_count}:result={result_count}"
        ));
    }
    let round0_checksum = checksum_words(&round0_words);
    let round1_checksum = checksum_words(&round1_words);
    Ok(Qwen3Dense06bResultFlowReport {
        publish_count,
        resolve_count,
        round1_compute_count,
        result_count,
        round0_distinct_count,
        round1_distinct_count,
        round0_checksum,
        round1_checksum,
        aggregate_checksum: checksum_words(&[
            publish_count,
            resolve_count,
            round1_compute_count,
            result_count,
            round0_distinct_count,
            round1_distinct_count,
            round0_checksum,
            round1_checksum,
        ]),
    })
}

fn qwen3_dense_0_6b_qkv_reference_report_from_prefill_output(
    output: &[u8],
    weight_reference_marker: u64,
    weight_stage_link_marker: u64,
) -> Result<Option<Qwen3Dense06bQkvReferenceReport>, String> {
    const WEIGHT_REFERENCE_ENTRY_WORDS: u64 = 14;
    const WEIGHT_STAGE_LINK_ENTRY_WORDS: u64 = QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS;

    let Some(reference_header) = find_u64_marker(output, weight_reference_marker) else {
        return Ok(None);
    };
    let shard_count = read_u64_le_checked(
        output,
        reference_header + 8,
        "qwen3_qkv_reference_shard_count",
    )?;
    let entry_words = read_u64_le_checked(
        output,
        reference_header + 16,
        "qwen3_qkv_reference_entry_words",
    )?;
    let table_bytes = read_u64_le_checked(
        output,
        reference_header + 24,
        "qwen3_qkv_reference_table_bytes",
    )?;
    let layer_id = read_u64_le_checked(output, reference_header + 32, "qwen3_qkv_reference_layer")?;
    let total_weight_bytes = read_u64_le_checked(
        output,
        reference_header + 40,
        "qwen3_qkv_reference_total_weight_bytes",
    )?;
    let aggregate_checksum = read_u64_le_checked(
        output,
        reference_header + 48,
        "qwen3_qkv_reference_aggregate_checksum",
    )?;
    let qkv_rows = read_u64_le_checked(
        output,
        reference_header + 56,
        "qwen3_qkv_reference_qkv_rows",
    )?;
    if entry_words != WEIGHT_REFERENCE_ENTRY_WORDS {
        return Err(format!(
            "qwen3_qkv_reference_entry_words_mismatch:expected={WEIGHT_REFERENCE_ENTRY_WORDS}:actual={entry_words}"
        ));
    }
    let entry_bytes = (entry_words as usize)
        .checked_mul(std::mem::size_of::<u64>())
        .ok_or_else(|| "qwen3_qkv_reference_entry_bytes_overflow".to_string())?;
    let expected_table_bytes = (shard_count as usize)
        .checked_mul(entry_bytes)
        .ok_or_else(|| "qwen3_qkv_reference_table_bytes_overflow".to_string())?;
    if table_bytes as usize != expected_table_bytes {
        return Err(format!(
            "qwen3_qkv_reference_table_bytes_mismatch:count={shard_count}:entry_bytes={entry_bytes}:table_bytes={table_bytes}"
        ));
    }
    if shard_count == 0 || total_weight_bytes == 0 || aggregate_checksum == 0 || qkv_rows == 0 {
        return Err("qwen3_qkv_reference_header_invalid".to_string());
    }
    let reference_base = reference_header
        .checked_add(64)
        .ok_or_else(|| "qwen3_qkv_reference_base_overflow".to_string())?;
    let reference_end = reference_base
        .checked_add(expected_table_bytes)
        .ok_or_else(|| "qwen3_qkv_reference_end_overflow".to_string())?;
    if reference_end > output.len() {
        return Err(format!(
            "qwen3_qkv_reference_table_oob:end={reference_end}:output_len={}",
            output.len()
        ));
    }
    for shard_index in 0..shard_count as usize {
        let base = reference_base + shard_index * entry_bytes;
        let shard_id = read_u64_le_checked(output, base, "qwen3_qkv_reference_shard_id")?;
        let hidden_size = read_u64_le_checked(output, base + 8, "qwen3_qkv_reference_hidden_size")?;
        let rmsnorm_checksum =
            read_u64_le_checked(output, base + 16, "qwen3_qkv_reference_rmsnorm_checksum")?;
        let q_weight_checksum =
            read_u64_le_checked(output, base + 24, "qwen3_qkv_reference_q_weight_checksum")?;
        let k_weight_checksum =
            read_u64_le_checked(output, base + 32, "qwen3_qkv_reference_k_weight_checksum")?;
        let v_weight_checksum =
            read_u64_le_checked(output, base + 40, "qwen3_qkv_reference_v_weight_checksum")?;
        let q_output_checksum =
            read_u64_le_checked(output, base + 48, "qwen3_qkv_reference_q_output_checksum")?;
        let k_output_checksum =
            read_u64_le_checked(output, base + 56, "qwen3_qkv_reference_k_output_checksum")?;
        let v_output_checksum =
            read_u64_le_checked(output, base + 64, "qwen3_qkv_reference_v_output_checksum")?;
        let q_rows = read_u64_le_checked(output, base + 72, "qwen3_qkv_reference_q_rows")?;
        let k_rows = read_u64_le_checked(output, base + 80, "qwen3_qkv_reference_k_rows")?;
        let v_rows = read_u64_le_checked(output, base + 88, "qwen3_qkv_reference_v_rows")?;
        let slice_count =
            read_u64_le_checked(output, base + 96, "qwen3_qkv_reference_slice_count")?;
        let slice_digest =
            read_u64_le_checked(output, base + 104, "qwen3_qkv_reference_slice_digest")?;
        if shard_id >= QWEN3_DENSE_0_6B_PROFILE.tp_nodes
            || hidden_size != QWEN3_DENSE_0_6B_PROFILE.hidden_size
            || rmsnorm_checksum == 0
            || q_weight_checksum == 0
            || k_weight_checksum == 0
            || v_weight_checksum == 0
            || q_output_checksum == 0
            || k_output_checksum == 0
            || v_output_checksum == 0
            || q_rows == 0
            || k_rows == 0
            || v_rows == 0
            || slice_count == 0
            || slice_digest == 0
        {
            return Err(format!(
                "qwen3_qkv_reference_entry_invalid:entry={shard_index}"
            ));
        }
    }

    let stage_header = find_u64_marker(output, weight_stage_link_marker)
        .ok_or_else(|| "qwen3_qkv_stage_link_table_missing".to_string())?;
    let stage_link_count =
        read_u64_le_checked(output, stage_header + 8, "qwen3_qkv_stage_link_count")?;
    let stage_entry_words = read_u64_le_checked(
        output,
        stage_header + 16,
        "qwen3_qkv_stage_link_entry_words",
    )?;
    let stage_table_bytes = read_u64_le_checked(
        output,
        stage_header + 24,
        "qwen3_qkv_stage_link_table_bytes",
    )?;
    let stage_link_checksum =
        read_u64_le_checked(output, stage_header + 32, "qwen3_qkv_stage_link_checksum")?;
    let stage_kind_count =
        read_u64_le_checked(output, stage_header + 40, "qwen3_qkv_stage_kind_count")?;
    let stage_reference_layer =
        read_u64_le_checked(output, stage_header + 48, "qwen3_qkv_stage_reference_layer")?;
    let stage_reference_layer_count = read_u64_le_checked(
        output,
        stage_header + 56,
        "qwen3_qkv_stage_reference_layer_count",
    )?;
    if stage_entry_words != WEIGHT_STAGE_LINK_ENTRY_WORDS {
        return Err(format!(
            "qwen3_qkv_stage_link_entry_words_mismatch:expected={WEIGHT_STAGE_LINK_ENTRY_WORDS}:actual={stage_entry_words}"
        ));
    }
    let stage_entry_bytes = (stage_entry_words as usize)
        .checked_mul(std::mem::size_of::<u64>())
        .ok_or_else(|| "qwen3_qkv_stage_link_entry_bytes_overflow".to_string())?;
    let expected_stage_table_bytes = (stage_link_count as usize)
        .checked_mul(stage_entry_bytes)
        .ok_or_else(|| "qwen3_qkv_stage_link_table_bytes_overflow".to_string())?;
    if stage_table_bytes as usize != expected_stage_table_bytes {
        return Err(format!(
            "qwen3_qkv_stage_link_table_bytes_mismatch:count={stage_link_count}:entry_bytes={stage_entry_bytes}:table_bytes={stage_table_bytes}"
        ));
    }
    if stage_link_count == 0
        || stage_link_checksum == 0
        || stage_kind_count != 4
        || stage_reference_layer != layer_id
        || stage_reference_layer_count == 0
    {
        return Err("qwen3_qkv_stage_link_header_invalid".to_string());
    }
    let stage_base = stage_header
        .checked_add(64)
        .ok_or_else(|| "qwen3_qkv_stage_link_base_overflow".to_string())?;
    let stage_end = stage_base
        .checked_add(expected_stage_table_bytes)
        .ok_or_else(|| "qwen3_qkv_stage_link_end_overflow".to_string())?;
    if stage_end > output.len() {
        return Err(format!(
            "qwen3_qkv_stage_link_table_oob:end={stage_end}:output_len={}",
            output.len()
        ));
    }

    let mut stage_kind_mask = 0u64;
    let mut synthetic_checksum = 0xcbf2_9ce4_8422_2325u64;
    let mut real_weight_checksum = 0xcbf2_9ce4_8422_2325u64;
    let mut real_value_checksum = 0xcbf2_9ce4_8422_2325u64;
    let mut real_output_checksum = 0xcbf2_9ce4_8422_2325u64;
    let mut stage_words_by_reference_layer: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
    for link_index in 0..stage_link_count as usize {
        let base = stage_base + link_index * stage_entry_bytes;
        let tile_id = read_u64_le_checked(output, base, "qwen3_qkv_stage_link_tile_id")?;
        let shard_id = read_u64_le_checked(output, base + 8, "qwen3_qkv_stage_link_shard_id")?;
        let stage_kind = read_u64_le_checked(output, base + 16, "qwen3_qkv_stage_link_kind")?;
        let segment = read_u64_le_checked(output, base + 24, "qwen3_qkv_stage_link_segment")?;
        let synthetic =
            read_u64_le_checked(output, base + 32, "qwen3_qkv_stage_link_synthetic_checksum")?;
        let real_weight = read_u64_le_checked(
            output,
            base + 40,
            "qwen3_qkv_stage_link_real_weight_checksum",
        )?;
        let real_output = read_u64_le_checked(
            output,
            base + 48,
            "qwen3_qkv_stage_link_real_output_checksum",
        )?;
        let real_value = read_u64_le_checked(
            output,
            base + 56,
            "qwen3_qkv_stage_link_real_value_checksum",
        )?;
        let rows = read_u64_le_checked(output, base + 64, "qwen3_qkv_stage_link_rows")?;
        let hidden_size =
            read_u64_le_checked(output, base + 72, "qwen3_qkv_stage_link_hidden_size")?;
        let link_reference_layer =
            read_u64_le_checked(output, base + 80, "qwen3_qkv_stage_link_reference_layer")?;
        if tile_id >= shard_count * 2
            || shard_id >= QWEN3_DENSE_0_6B_PROFILE.tp_nodes
            || !(1..=4).contains(&stage_kind)
            || segment == 0
            || synthetic == 0
            || real_weight == 0
            || real_output == 0
            || real_value == 0
            || rows == 0
            || hidden_size != QWEN3_DENSE_0_6B_PROFILE.hidden_size
            || link_reference_layer < layer_id
            || link_reference_layer >= layer_id + stage_reference_layer_count
        {
            return Err(format!(
                "qwen3_qkv_stage_link_entry_invalid:entry={link_index}"
            ));
        }
        stage_kind_mask |= 1u64 << (stage_kind - 1);
        synthetic_checksum = synthetic_checksum.wrapping_mul(0x0000_0100_0000_01b3)
            ^ link_reference_layer.rotate_left(3)
            ^ synthetic.rotate_left((stage_kind * 7) as u32);
        real_weight_checksum = real_weight_checksum.wrapping_mul(0x0000_0100_0000_01b3)
            ^ link_reference_layer.rotate_left(5)
            ^ real_weight.rotate_left((stage_kind * 11) as u32);
        real_value_checksum = real_value_checksum.wrapping_mul(0x0000_0100_0000_01b3)
            ^ link_reference_layer.rotate_left(7)
            ^ real_value.rotate_left((stage_kind * 5) as u32);
        real_output_checksum = real_output_checksum.wrapping_mul(0x0000_0100_0000_01b3)
            ^ link_reference_layer.rotate_left(11)
            ^ real_output.rotate_left((stage_kind * 13) as u32);
        stage_words_by_reference_layer
            .entry(link_reference_layer)
            .or_default()
            .push(
                stage_kind.rotate_left(3)
                    ^ segment.rotate_left(7)
                    ^ synthetic.rotate_left(13)
                    ^ real_weight.rotate_left(17)
                    ^ real_output.rotate_left(29)
                    ^ real_value.rotate_left(37),
            );
    }
    if stage_kind_mask != 0x0f {
        return Err(format!(
            "qwen3_qkv_stage_link_kind_mask_invalid:mask={stage_kind_mask:#x}"
        ));
    }
    let reference_layer_checksum = stage_words_by_reference_layer
        .get(&layer_id)
        .map(|words| checksum_words(words))
        .unwrap_or(0);
    let next_reference_layer_checksum = stage_words_by_reference_layer
        .get(&(layer_id + 1))
        .map(|words| checksum_words(words))
        .unwrap_or(0);
    if reference_layer_checksum == 0 || next_reference_layer_checksum == 0 {
        return Err("qwen3_qkv_stage_link_reference_layer_checksum_invalid".to_string());
    }

    Ok(Some(Qwen3Dense06bQkvReferenceReport {
        layer_id,
        reference_layer_count: stage_reference_layer_count,
        shard_count,
        stage_link_count,
        stage_kind_mask,
        total_weight_bytes,
        aggregate_checksum,
        qkv_rows,
        stage_link_checksum,
        synthetic_checksum,
        real_weight_checksum,
        real_value_checksum,
        real_output_checksum,
        reference_layer_checksum,
        next_reference_layer_checksum,
    }))
}

fn qwen3_dense_0_6b_mlp_reference_report_from_prefill_output(
    output: &[u8],
    mlp_reference_marker: u64,
) -> Result<Option<Qwen3Dense06bMlpReferenceReport>, String> {
    let Some(reference_header) = find_u64_marker(output, mlp_reference_marker) else {
        return Ok(None);
    };
    let entry_count =
        read_u64_le_checked(output, reference_header + 8, "qwen3_mlp_reference_count")?;
    let entry_words = read_u64_le_checked(
        output,
        reference_header + 16,
        "qwen3_mlp_reference_entry_words",
    )?;
    let table_bytes = read_u64_le_checked(
        output,
        reference_header + 24,
        "qwen3_mlp_reference_table_bytes",
    )?;
    let layer_id = read_u64_le_checked(output, reference_header + 32, "qwen3_mlp_reference_layer")?;
    let next_layer_id = read_u64_le_checked(
        output,
        reference_header + 40,
        "qwen3_mlp_reference_next_layer",
    )?;
    let aggregate_checksum = read_u64_le_checked(
        output,
        reference_header + 48,
        "qwen3_mlp_reference_aggregate_checksum",
    )?;
    let next_aggregate_checksum = read_u64_le_checked(
        output,
        reference_header + 56,
        "qwen3_mlp_reference_next_aggregate_checksum",
    )?;
    if entry_words != QWEN3_MLP_REFERENCE_ENTRY_WORDS {
        return Err(format!(
            "qwen3_mlp_reference_entry_words_mismatch:expected={QWEN3_MLP_REFERENCE_ENTRY_WORDS}:actual={entry_words}"
        ));
    }
    let entry_bytes = (entry_words as usize)
        .checked_mul(std::mem::size_of::<u64>())
        .ok_or_else(|| "qwen3_mlp_reference_entry_bytes_overflow".to_string())?;
    let expected_table_bytes = (entry_count as usize)
        .checked_mul(entry_bytes)
        .ok_or_else(|| "qwen3_mlp_reference_table_bytes_overflow".to_string())?;
    if table_bytes as usize != expected_table_bytes {
        return Err(format!(
            "qwen3_mlp_reference_table_bytes_mismatch:count={entry_count}:entry_bytes={entry_bytes}:table_bytes={table_bytes}"
        ));
    }
    if entry_count == 0
        || layer_id == u64::MAX
        || next_layer_id == u64::MAX
        || next_layer_id <= layer_id
        || aggregate_checksum == 0
        || next_aggregate_checksum == 0
    {
        return Err("qwen3_mlp_reference_header_invalid".to_string());
    }
    let reference_base = reference_header
        .checked_add(64)
        .ok_or_else(|| "qwen3_mlp_reference_base_overflow".to_string())?;
    let reference_end = reference_base
        .checked_add(expected_table_bytes)
        .ok_or_else(|| "qwen3_mlp_reference_end_overflow".to_string())?;
    if reference_end > output.len() {
        return Err(format!(
            "qwen3_mlp_reference_table_oob:end={reference_end}:output_len={}",
            output.len()
        ));
    }

    let mut shard_count = 0u64;
    let mut next_shard_count = 0u64;
    let mut total_weight_bytes = 0u64;
    let mut next_total_weight_bytes = 0u64;
    let mut total_intermediate_rows = 0u64;
    let mut next_total_intermediate_rows = 0u64;
    let mut real_weight_checksum = 0xcbf2_9ce4_8422_2325u64;
    let mut real_activation_checksum = 0xcbf2_9ce4_8422_2325u64;
    let mut real_output_checksum = 0xcbf2_9ce4_8422_2325u64;
    let mut next_real_output_checksum = 0xcbf2_9ce4_8422_2325u64;
    let mut sample_checksum = 0xcbf2_9ce4_8422_2325u64;
    let mut table_words = Vec::with_capacity(entry_count as usize * entry_words as usize);

    for entry_index in 0..entry_count as usize {
        let base = reference_base + entry_index * entry_bytes;
        let entry_layer_id = read_u64_le_checked(output, base, "qwen3_mlp_reference_entry_layer")?;
        let shard_id = read_u64_le_checked(output, base + 8, "qwen3_mlp_reference_shard_id")?;
        let hidden_size =
            read_u64_le_checked(output, base + 16, "qwen3_mlp_reference_hidden_size")?;
        let intermediate_rows =
            read_u64_le_checked(output, base + 24, "qwen3_mlp_reference_intermediate_rows")?;
        let gate_weight =
            read_u64_le_checked(output, base + 32, "qwen3_mlp_reference_gate_weight")?;
        let up_weight = read_u64_le_checked(output, base + 40, "qwen3_mlp_reference_up_weight")?;
        let down_weight =
            read_u64_le_checked(output, base + 48, "qwen3_mlp_reference_down_weight")?;
        let gate_output =
            read_u64_le_checked(output, base + 56, "qwen3_mlp_reference_gate_output")?;
        let up_output = read_u64_le_checked(output, base + 64, "qwen3_mlp_reference_up_output")?;
        let activation = read_u64_le_checked(output, base + 72, "qwen3_mlp_reference_activation")?;
        let down_output =
            read_u64_le_checked(output, base + 80, "qwen3_mlp_reference_down_output")?;
        let sample_digest =
            read_u64_le_checked(output, base + 88, "qwen3_mlp_reference_sample_digest")?;
        let slice_count =
            read_u64_le_checked(output, base + 96, "qwen3_mlp_reference_slice_count")?;
        let slice_bytes =
            read_u64_le_checked(output, base + 104, "qwen3_mlp_reference_slice_bytes")?;
        let slice_digest =
            read_u64_le_checked(output, base + 112, "qwen3_mlp_reference_slice_digest")?;
        if shard_id >= QWEN3_DENSE_0_6B_PROFILE.tp_nodes
            || hidden_size != QWEN3_DENSE_0_6B_PROFILE.hidden_size
            || intermediate_rows == 0
            || gate_weight == 0
            || up_weight == 0
            || down_weight == 0
            || gate_output == 0
            || up_output == 0
            || activation == 0
            || down_output == 0
            || sample_digest == 0
            || slice_count == 0
            || slice_bytes == 0
            || slice_digest == 0
        {
            return Err(format!(
                "qwen3_mlp_reference_entry_invalid:entry={entry_index}"
            ));
        }
        if entry_layer_id != layer_id && entry_layer_id != next_layer_id {
            return Err(format!(
                "qwen3_mlp_reference_entry_layer_invalid:entry={entry_index}:layer={entry_layer_id}"
            ));
        }
        table_words.extend_from_slice(&[
            entry_layer_id,
            shard_id,
            hidden_size,
            intermediate_rows,
            gate_weight,
            up_weight,
            down_weight,
            gate_output,
            up_output,
            activation,
            down_output,
            sample_digest,
            slice_count,
            slice_bytes,
            slice_digest,
        ]);
        real_weight_checksum = real_weight_checksum.wrapping_mul(0x0000_0100_0000_01b3)
            ^ gate_weight.rotate_left(7)
            ^ up_weight.rotate_left(17)
            ^ down_weight.rotate_left(29)
            ^ slice_digest.rotate_left(37);
        sample_checksum = sample_checksum.wrapping_mul(0x0000_0100_0000_01b3)
            ^ sample_digest.rotate_left((entry_layer_id % 63) as u32);
        if entry_layer_id == layer_id {
            shard_count += 1;
            total_weight_bytes = total_weight_bytes.wrapping_add(slice_bytes);
            total_intermediate_rows = total_intermediate_rows.wrapping_add(intermediate_rows);
            real_activation_checksum = real_activation_checksum.wrapping_mul(0x0000_0100_0000_01b3)
                ^ activation.rotate_left(11)
                ^ gate_output.rotate_left(19)
                ^ up_output.rotate_left(23);
            real_output_checksum = real_output_checksum.wrapping_mul(0x0000_0100_0000_01b3)
                ^ down_output.rotate_left(31);
        } else {
            next_shard_count += 1;
            next_total_weight_bytes = next_total_weight_bytes.wrapping_add(slice_bytes);
            next_total_intermediate_rows =
                next_total_intermediate_rows.wrapping_add(intermediate_rows);
            next_real_output_checksum = next_real_output_checksum
                .wrapping_mul(0x0000_0100_0000_01b3)
                ^ down_output.rotate_left(31);
        }
    }
    if shard_count == 0 || next_shard_count == 0 {
        return Err("qwen3_mlp_reference_layer_coverage_invalid".to_string());
    }

    Ok(Some(Qwen3Dense06bMlpReferenceReport {
        layer_id,
        next_layer_id,
        shard_count,
        next_shard_count,
        total_weight_bytes,
        next_total_weight_bytes,
        total_intermediate_rows,
        next_total_intermediate_rows,
        aggregate_checksum,
        next_aggregate_checksum,
        real_weight_checksum,
        real_activation_checksum,
        real_output_checksum,
        next_real_output_checksum,
        sample_checksum,
        table_checksum: checksum_words(&table_words),
    }))
}

fn qwen3_dense_0_6b_logits_reference_report_from_prefill_output(
    output: &[u8],
    result_marker: u64,
    logits_reference_marker: u64,
    samples: &[Qwen3Dense06bTextOutputSample],
) -> Result<Option<Qwen3Dense06bLogitsReferenceReport>, String> {
    const RESULT_ENTRY_WORDS: u64 = 10;
    const LOGITS_REFERENCE_ENTRY_WORDS: u64 = 6;
    let Some(reference_header) = find_u64_marker(output, logits_reference_marker) else {
        return Ok(None);
    };
    let result_round1_checksum_by_tile =
        qwen3_dense_0_6b_result_round1_checksum_by_tile(output, result_marker, RESULT_ENTRY_WORDS)?;

    let token_count =
        read_u64_le_checked(output, reference_header + 8, "qwen3_logits_reference_count")?;
    let entry_words = read_u64_le_checked(
        output,
        reference_header + 16,
        "qwen3_logits_reference_entry_words",
    )?;
    let table_bytes = read_u64_le_checked(
        output,
        reference_header + 24,
        "qwen3_logits_reference_table_bytes",
    )?;
    if entry_words != LOGITS_REFERENCE_ENTRY_WORDS {
        return Err(format!(
            "qwen3_logits_reference_entry_words_mismatch:expected={LOGITS_REFERENCE_ENTRY_WORDS}:actual={entry_words}"
        ));
    }
    let entry_bytes = entry_words * std::mem::size_of::<u64>() as u64;
    if table_bytes != token_count * entry_bytes {
        return Err(format!(
            "qwen3_logits_reference_table_bytes_mismatch:count={token_count}:entry_bytes={entry_bytes}:table_bytes={table_bytes}"
        ));
    }
    let aggregate_checksum = read_u64_le_checked(
        output,
        reference_header + 32,
        "qwen3_logits_reference_aggregate_checksum",
    )?;
    let final_norm_checksum = read_u64_le_checked(
        output,
        reference_header + 40,
        "qwen3_logits_reference_final_norm_checksum",
    )?;
    let vocab_size = read_u64_le_checked(
        output,
        reference_header + 48,
        "qwen3_logits_reference_vocab_size",
    )?;
    let hidden_size = read_u64_le_checked(
        output,
        reference_header + 56,
        "qwen3_logits_reference_hidden_size",
    )?;
    if aggregate_checksum == 0 || final_norm_checksum == 0 || vocab_size == 0 || hidden_size == 0 {
        return Err("qwen3_logits_reference_header_invalid".to_string());
    }
    let reference_base = reference_header
        .checked_add(64)
        .ok_or_else(|| "qwen3_logits_reference_base_overflow".to_string())?;
    let reference_end = reference_base
        .checked_add(table_bytes as usize)
        .ok_or_else(|| "qwen3_logits_reference_end_overflow".to_string())?;
    if reference_end > output.len() {
        return Err(format!(
            "qwen3_logits_reference_table_oob:end={reference_end}:output_len={}",
            output.len()
        ));
    }

    let mut logit_checksum_by_step_token = BTreeMap::new();
    let mut logits_by_step: BTreeMap<u64, Vec<(u64, u32, u64)>> = BTreeMap::new();
    let mut distinct_steps = BTreeSet::new();
    let mut distinct_tokens = BTreeSet::new();
    let mut row_byte_count = 0u64;
    let mut row_checksum_words = Vec::with_capacity(token_count as usize);
    let mut logit_checksum_words = Vec::with_capacity(token_count as usize);
    for entry in 0..token_count as usize {
        let base = reference_base + entry * entry_bytes as usize;
        let step_index = read_u64_le_checked(output, base, "qwen3_logits_reference_step_index")?;
        let token_id = read_u64_le_checked(output, base + 8, "qwen3_logits_reference_token")?;
        let row_bytes = read_u64_le_checked(output, base + 16, "qwen3_logits_reference_row_bytes")?;
        let row_checksum =
            read_u64_le_checked(output, base + 24, "qwen3_logits_reference_row_checksum")?;
        let logit_bits =
            read_u64_le_checked(output, base + 32, "qwen3_logits_reference_logit_bits")?;
        let logit_checksum =
            read_u64_le_checked(output, base + 40, "qwen3_logits_reference_logit_checksum")?;
        if row_bytes == 0 || row_checksum == 0 || logit_bits == 0 || logit_checksum == 0 {
            return Err(format!(
                "qwen3_logits_reference_entry_invalid:entry={entry}"
            ));
        }
        logit_checksum_by_step_token.insert((step_index, token_id), logit_checksum);
        logits_by_step.entry(step_index).or_default().push((
            token_id,
            logit_bits as u32,
            logit_checksum,
        ));
        distinct_steps.insert(step_index);
        distinct_tokens.insert(token_id);
        row_byte_count = row_byte_count
            .checked_add(row_bytes)
            .ok_or_else(|| "qwen3_logits_reference_row_bytes_overflow".to_string())?;
        row_checksum_words.extend_from_slice(&[step_index, token_id, row_bytes, row_checksum]);
        logit_checksum_words.extend_from_slice(&[step_index, token_id, logit_bits, logit_checksum]);
    }

    let mut sampled_pair_count = 0u64;
    let mut selection_match_count = 0u64;
    let mut margin_match_count = 0u64;
    let mut checksum_match_count = 0u64;
    let mut max_margin_delta_milli = 0u64;
    let mut top_logit_bits_words = Vec::with_capacity(samples.len());
    let mut runner_logit_bits_words = Vec::with_capacity(samples.len());
    let mut comparison_words = Vec::with_capacity(samples.len() * 14);
    let mut selection_words = Vec::with_capacity(samples.len() * 8);
    for sample in samples {
        let mut step_logits = logits_by_step
            .get(&sample.step_index)
            .cloned()
            .ok_or_else(|| {
                format!(
                    "qwen3_logits_reference_step_missing:step={}",
                    sample.step_index
                )
            })?;
        if step_logits.len() < 2 {
            return Err(format!(
                "qwen3_logits_reference_step_candidate_count_invalid:step={}:count={}",
                sample.step_index,
                step_logits.len()
            ));
        }
        let Some(real_top_checksum) = logit_checksum_by_step_token
            .get(&(sample.step_index, sample.sampled_token))
            .copied()
        else {
            comparison_words.extend_from_slice(&[
                sample.step_index,
                sample.tile_id,
                0,
                sample.sampled_token,
                0,
                sample.runner_up_token,
                0,
                0,
                0,
                sample.margin_milli,
                sample.margin_milli,
                sample.logits_checksum,
                0,
                sample.real_path_digest,
            ]);
            continue;
        };
        let Some(real_runner_checksum) = logit_checksum_by_step_token
            .get(&(sample.step_index, sample.runner_up_token))
            .copied()
        else {
            comparison_words.extend_from_slice(&[
                sample.step_index,
                sample.tile_id,
                0,
                sample.sampled_token,
                0,
                sample.runner_up_token,
                0,
                0,
                0,
                sample.margin_milli,
                sample.margin_milli,
                sample.logits_checksum,
                0,
                sample.real_path_digest,
            ]);
            continue;
        };
        let round1_checksum = result_round1_checksum_by_tile
            .get(&sample.tile_id)
            .copied()
            .ok_or_else(|| {
                format!(
                    "qwen3_logits_reference_result_tile_missing:tile={}",
                    sample.tile_id
                )
            })?;
        step_logits.sort_by(|left, right| {
            let left_logit = f32::from_bits(left.1);
            let right_logit = f32::from_bits(right.1);
            right_logit
                .partial_cmp(&left_logit)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then_with(|| left.0.cmp(&right.0))
        });
        let (expected_top_token, expected_top_bits, _) = step_logits[0];
        let (expected_runner_token, expected_runner_bits, _) = step_logits[1];
        if sample.sampled_token != expected_top_token
            || sample.runner_up_token != expected_runner_token
        {
            return Err(format!(
                "qwen3_logits_reference_selection_mismatch:step={}:expected_top={expected_top_token}:actual_top={}:expected_runner={expected_runner_token}:actual_runner={}",
                sample.step_index, sample.sampled_token, sample.runner_up_token
            ));
        }
        selection_match_count += 1;
        let top_logit = f32::from_bits(expected_top_bits);
        let runner_logit = f32::from_bits(expected_runner_bits);
        let expected_margin_milli = if top_logit.is_finite() && runner_logit.is_finite() {
            ((top_logit - runner_logit).abs() * 1_000.0)
                .round()
                .max(1.0) as u64
        } else {
            1
        };
        if sample.margin_milli != expected_margin_milli {
            return Err(format!(
                "qwen3_logits_reference_margin_mismatch:step={}:expected={expected_margin_milli}:actual={}",
                sample.step_index, sample.margin_milli
            ));
        }
        let margin_delta_milli = sample.margin_milli.abs_diff(expected_margin_milli);
        max_margin_delta_milli = max_margin_delta_milli.max(margin_delta_milli);
        margin_match_count += 1;
        let expected_logits_checksum = qwen3_dense_0_6b_logits_checksum(
            round1_checksum,
            sample.tile_id,
            sample.sampled_token,
            sample.runner_up_token,
            sample.margin_milli,
            real_top_checksum,
            real_runner_checksum,
            sample.kvcache_read_digest,
            sample.qkv_reference_digest,
            sample.real_path_digest,
        );
        if sample.logits_checksum != expected_logits_checksum {
            return Err(format!(
                "qwen3_logits_reference_checksum_mismatch:step={}:expected={expected_logits_checksum:#x}:actual={:#x}",
                sample.step_index, sample.logits_checksum
            ));
        }
        checksum_match_count += 1;
        sampled_pair_count += 2;
        top_logit_bits_words.push(expected_top_bits as u64);
        runner_logit_bits_words.push(expected_runner_bits as u64);
        comparison_words.extend_from_slice(&[
            sample.step_index,
            sample.tile_id,
            expected_top_token,
            sample.sampled_token,
            expected_runner_token,
            sample.runner_up_token,
            expected_top_bits as u64,
            expected_runner_bits as u64,
            expected_margin_milli,
            sample.margin_milli,
            margin_delta_milli,
            sample.logits_checksum,
            expected_logits_checksum,
            sample.real_path_digest,
        ]);
        selection_words.extend_from_slice(&[
            sample.step_index,
            sample.tile_id,
            sample.sampled_token,
            sample.runner_up_token,
            sample.margin_milli,
            real_top_checksum,
            real_runner_checksum,
            sample.real_path_digest,
        ]);
    }
    if sampled_pair_count > token_count {
        return Err(format!(
            "qwen3_logits_reference_sample_pair_count_mismatch:sampled_pairs={sampled_pair_count}:candidate_count={token_count}"
        ));
    }

    Ok(Some(Qwen3Dense06bLogitsReferenceReport {
        token_count,
        candidate_count: token_count,
        distinct_step_count: distinct_steps.len() as u64,
        distinct_token_count: distinct_tokens.len() as u64,
        row_byte_count,
        row_checksum: checksum_words(&row_checksum_words),
        logit_checksum: checksum_words(&logit_checksum_words),
        aggregate_checksum,
        final_norm_checksum,
        vocab_size,
        hidden_size,
        sampled_pair_count,
        selection_match_count,
        margin_match_count,
        checksum_match_count,
        max_margin_delta_milli,
        top_logit_bits_checksum: checksum_words(&top_logit_bits_words),
        runner_logit_bits_checksum: checksum_words(&runner_logit_bits_words),
        comparison_checksum: checksum_words(&comparison_words),
        selection_checksum: checksum_words(&selection_words),
    }))
}

fn qwen3_dense_0_6b_result_round1_checksum_by_tile(
    output: &[u8],
    result_marker: u64,
    expected_entry_words: u64,
) -> Result<BTreeMap<u64, u64>, String> {
    let result_header = find_u64_marker(output, result_marker)
        .ok_or_else(|| "qwen3_result_table_missing".to_string())?;
    let result_count = read_u64_le_checked(output, result_header + 8, "qwen3_result_count")?;
    let entry_words = read_u64_le_checked(output, result_header + 16, "qwen3_result_entry_words")?;
    let table_bytes = read_u64_le_checked(output, result_header + 24, "qwen3_result_table_bytes")?;
    if entry_words != expected_entry_words {
        return Err(format!(
            "qwen3_result_entry_words_mismatch:expected={expected_entry_words}:actual={entry_words}"
        ));
    }
    let entry_bytes = entry_words * std::mem::size_of::<u64>() as u64;
    if table_bytes != result_count * entry_bytes {
        return Err(format!(
            "qwen3_result_table_bytes_mismatch:count={result_count}:entry_bytes={entry_bytes}:table_bytes={table_bytes}"
        ));
    }
    let result_base = result_header
        .checked_add(64)
        .ok_or_else(|| "qwen3_result_base_overflow".to_string())?;
    let result_end = result_base
        .checked_add(table_bytes as usize)
        .ok_or_else(|| "qwen3_result_end_overflow".to_string())?;
    if result_end > output.len() {
        return Err(format!(
            "qwen3_result_table_oob:end={result_end}:output_len={}",
            output.len()
        ));
    }
    let mut by_tile = BTreeMap::new();
    for entry in 0..result_count as usize {
        let base = result_base + entry * entry_bytes as usize;
        let tile_id = read_u64_le_checked(output, base + 24, "qwen3_result_tile")?;
        let round1_checksum =
            read_u64_le_checked(output, base + 72, "qwen3_result_round1_checksum")?;
        if round1_checksum == 0 {
            return Err(format!("qwen3_result_round1_checksum_zero:entry={entry}"));
        }
        by_tile.insert(tile_id, round1_checksum);
    }
    Ok(by_tile)
}

fn qwen3_dense_0_6b_text_output_samples_from_prefill_output(
    output: &[u8],
    logits_marker: u64,
    token_text_marker: u64,
    token_count: u64,
    text_bytes: &[u8],
) -> Result<Vec<Qwen3Dense06bTextOutputSample>, String> {
    let logits_header = find_u64_marker(output, logits_marker)
        .ok_or_else(|| "qwen3_logits_table_missing".to_string())?;
    let token_text_header = find_u64_marker(output, token_text_marker)
        .ok_or_else(|| "qwen3_token_text_table_missing".to_string())?;
    let logits_count = read_u64_le_checked(output, logits_header + 8, "qwen3_logits_count")?;
    let logits_entry_words =
        read_u64_le_checked(output, logits_header + 16, "qwen3_logits_entry_words")?;
    let logits_table_bytes =
        read_u64_le_checked(output, logits_header + 24, "qwen3_logits_table_bytes")?;
    let token_text_count =
        read_u64_le_checked(output, token_text_header + 8, "qwen3_token_text_count")?;
    let token_text_entry_words = read_u64_le_checked(
        output,
        token_text_header + 16,
        "qwen3_token_text_entry_words",
    )?;
    let token_text_table_bytes = read_u64_le_checked(
        output,
        token_text_header + 24,
        "qwen3_token_text_table_bytes",
    )?;
    if logits_count != token_count || token_text_count != token_count {
        return Err(format!(
            "qwen3_text_output_sample_count_mismatch:text={token_count}:logits={logits_count}:token_text={token_text_count}"
        ));
    }
    if logits_entry_words != 45 || token_text_entry_words != 8 {
        return Err(format!(
            "qwen3_text_output_sample_entry_words_invalid:logits={logits_entry_words}:token_text={token_text_entry_words}"
        ));
    }
    let logits_entry_bytes = logits_entry_words as usize * std::mem::size_of::<u64>();
    let token_text_entry_bytes = token_text_entry_words as usize * std::mem::size_of::<u64>();
    if logits_table_bytes != logits_count.saturating_mul(logits_entry_bytes as u64)
        || token_text_table_bytes != token_text_count.saturating_mul(token_text_entry_bytes as u64)
    {
        return Err("qwen3_text_output_sample_table_bytes_invalid".to_string());
    }
    let logits_base = logits_header + 64;
    let token_text_base = token_text_header + 64;
    let mut samples = Vec::with_capacity(token_count as usize);
    for index in 0..token_count as usize {
        let logits_entry = logits_base + index * logits_entry_bytes;
        let token_text_entry = token_text_base + index * token_text_entry_bytes;
        let shard_id = read_u64_le_checked(output, logits_entry, "qwen3_logits_shard")?;
        let tile_id = read_u64_le_checked(output, logits_entry + 8, "qwen3_logits_tile")?;
        let logits_count = read_u64_le_checked(output, logits_entry + 24, "qwen3_logits_count")?;
        let sampled_token =
            read_u64_le_checked(output, logits_entry + 32, "qwen3_logits_sampled_token")?;
        let runner_up_token =
            read_u64_le_checked(output, logits_entry + 40, "qwen3_logits_runner_up_token")?;
        let margin_milli =
            read_u64_le_checked(output, logits_entry + 48, "qwen3_logits_margin_milli")?;
        let logits_checksum =
            read_u64_le_checked(output, logits_entry + 56, "qwen3_logits_checksum")?;
        let text_checksum =
            read_u64_le_checked(output, logits_entry + 64, "qwen3_logits_text_checksum")?;
        let step_index = read_u64_le_checked(output, logits_entry + 72, "qwen3_logits_step")?;
        let kvcache_read_digest = read_u64_le_checked(
            output,
            logits_entry + 80,
            "qwen3_logits_kvcache_read_digest",
        )?;
        let qkv_reference_digest = read_u64_le_checked(
            output,
            logits_entry + 88,
            "qwen3_logits_qkv_reference_digest",
        )?;
        let real_path_digest =
            read_u64_le_checked(output, logits_entry + 96, "qwen3_logits_real_path_digest")?;
        let full_vocab_checked_token_count = read_u64_le_checked(
            output,
            logits_entry + 104,
            "qwen3_logits_full_vocab_checked_token_count",
        )?;
        let full_vocab_logits_checksum = read_u64_le_checked(
            output,
            logits_entry + 112,
            "qwen3_logits_full_vocab_checksum",
        )?;
        let top_logit_bits =
            read_u64_le_checked(output, logits_entry + 120, "qwen3_logits_top_bits")?;
        let runner_up_logit_bits =
            read_u64_le_checked(output, logits_entry + 128, "qwen3_logits_runner_bits")?;
        let runtime_forward_layer_count = read_u64_le_checked(
            output,
            logits_entry + 136,
            "qwen3_logits_runtime_forward_layer_count",
        )?;
        let runtime_forward_final_hidden_checksum = read_u64_le_checked(
            output,
            logits_entry + 144,
            "qwen3_logits_runtime_forward_final_hidden_checksum",
        )?;
        let runtime_forward_checksum = read_u64_le_checked(
            output,
            logits_entry + 152,
            "qwen3_logits_runtime_forward_checksum",
        )?;
        let token_text_step =
            read_u64_le_checked(output, token_text_entry, "qwen3_token_text_step")?;
        let token_text_token =
            read_u64_le_checked(output, token_text_entry + 8, "qwen3_token_text_token")?;
        let text_byte_offset =
            read_u64_le_checked(output, token_text_entry + 16, "qwen3_token_text_offset")?;
        let byte_len =
            read_u64_le_checked(output, token_text_entry + 24, "qwen3_token_text_byte_len")?;
        let token_text_checksum =
            read_u64_le_checked(output, token_text_entry + 48, "qwen3_token_text_checksum")?;
        let boundary_flags =
            read_u64_le_checked(output, token_text_entry + 56, "qwen3_token_text_flags")?;
        if token_text_step != step_index || token_text_token != sampled_token {
            return Err("qwen3_text_output_sample_token_text_mismatch".to_string());
        }
        if token_text_checksum != text_checksum {
            return Err("qwen3_text_output_sample_text_checksum_mismatch".to_string());
        }
        let piece_start = text_byte_offset as usize;
        let piece_end = piece_start
            .checked_add(byte_len as usize)
            .ok_or_else(|| "qwen3_text_output_sample_piece_end_overflow".to_string())?;
        if piece_end > text_bytes.len() {
            return Err(format!(
                "qwen3_text_output_sample_piece_oob:end={piece_end}:bytes={}",
                text_bytes.len()
            ));
        }
        samples.push(Qwen3Dense06bTextOutputSample {
            step_index,
            shard_id,
            tile_id,
            logits_count,
            sampled_token,
            runner_up_token,
            margin_milli,
            logits_checksum,
            full_vocab_checked_token_count,
            full_vocab_logits_checksum,
            top_logit_bits,
            runner_up_logit_bits,
            runtime_forward_layer_count,
            runtime_forward_final_hidden_checksum,
            runtime_forward_checksum,
            kvcache_read_digest,
            qkv_reference_digest,
            real_path_digest,
            text_checksum,
            text_byte_offset,
            byte_len,
            boundary_flags,
            piece_lossy: String::from_utf8_lossy(&text_bytes[piece_start..piece_end]).to_string(),
        });
    }
    Ok(samples)
}

fn run_host_vector_chipbackend(
    topology: &SimTopology,
    task: &TaskKey,
    guest_input: &[u8],
) -> Result<Vec<u8>, String> {
    let manifest_path = simpler_manifest_path()?;
    let scenario_config = scenario_config_for_chipbackend()?;
    let elems = W4_DEMO_KVCACHE_PAYLOAD_BYTES / std::mem::size_of::<f32>();
    let size_bytes = elems * std::mem::size_of::<f32>();
    let kvcache_layout = KvCachePayloadLayout::new(elems, size_bytes)?;
    let segment_base = 10_000 + task.task_id.saturating_mul(10);
    let host_node = topology
        .hosts
        .first()
        .map(|host| host.node_id)
        .ok_or_else(|| "missing_host_node".to_string())?;
    let ubpu_node = topology
        .ubpus
        .first()
        .map(|ubpu| ubpu.node_id)
        .ok_or_else(|| "missing_ubpu_node".to_string())?;
    let _dispatch_lock = host_vector_dispatch_lock_guard()?;
    let input_a_bytes = kvcache_input_a_payload(guest_input, size_bytes);
    let input_b_bytes = kvcache_input_b_payload(&kvcache_layout);
    let mut runtime = LocalRuntimeEngine::from_config(&scenario_config);
    let mut sink = VecEventSink::default();
    let complete_at = scenario_config
        .pypto
        .simpler_boundary
        .dispatch_latency_us
        .unwrap_or(15);
    let mut output_segments = Vec::new();
    let chunk_bytes = W4_HOST_VECTOR_CHUNK_BYTES;
    for (chunk_index, chunk_offset) in (0..size_bytes).step_by(chunk_bytes).enumerate() {
        let chunk_end = (chunk_offset + chunk_bytes).min(size_bytes);
        let chunk_len = chunk_end - chunk_offset;
        let chunk_elems = chunk_len / std::mem::size_of::<f32>();
        let input_a = SegmentHandle(segment_base + 1 + chunk_index as u64 * 3);
        let input_b = SegmentHandle(segment_base + 2 + chunk_index as u64 * 3);
        let output = SegmentHandle(segment_base + 3 + chunk_index as u64 * 3);
        runtime.seed_host_segment(
            host_node,
            input_a,
            input_a_bytes[chunk_offset..chunk_end].to_vec(),
        );
        runtime.seed_host_segment(
            host_node,
            input_b,
            input_b_bytes[chunk_offset..chunk_end].to_vec(),
        );
        runtime.seed_host_segment(host_node, output, vec![0u8; chunk_len]);
        output_segments.push(output);

        let backend_spec = host_vector_backend_spec_from_manifest(
            &manifest_path,
            MemoryEndpoint {
                node: host_node,
                segment: input_a,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: input_b,
                offset: 0,
            },
            MemoryEndpoint {
                node: host_node,
                segment: output,
                offset: 0,
            },
            chunk_len as u64,
            chunk_elems as u64,
        )?;
        let mut bindings = kvcache_layout_bindings_for_chunk(
            &kvcache_layout,
            host_node,
            input_a,
            chunk_offset,
            chunk_end,
        );
        bindings.extend([
            dense_f32_binding(
                "input_a",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_a,
                    offset: 0,
                },
                chunk_elems as u64,
            ),
            dense_f32_binding(
                "input_b",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_b,
                    offset: 0,
                },
                chunk_elems as u64,
            ),
            dense_f32_binding(
                "output_f",
                BufferUsage::Output,
                MemoryEndpoint {
                    node: host_node,
                    segment: output,
                    offset: 0,
                },
                chunk_elems as u64,
            ),
        ]);
        let request = kvcache_host_vector_request(
            task.task_id,
            &kvcache_layout,
            chunk_index,
            chunk_offset,
            bindings,
        );
        let dispatch = BackendDispatchOperation {
            task: task.clone(),
            function: FunctionLabel {
                name: "host_vector_example".into(),
                level: PlLevel::L2,
            },
            backend_spec,
            request,
            target_level: PlLevel::L2,
            target_node: ubpu_node,
            legacy_input_segments: vec![input_a, input_b],
        };
        with_suppressed_stdio(|| {
            runtime
                .submit_backend_dispatch(dispatch, &mut sink)
                .map_err(|err| err.to_string())
        })?;
    }
    let completions = with_suppressed_stdio(|| {
        runtime.advance_to(complete_at, &mut sink);
        Ok(runtime.poll_completions(complete_at, &mut sink))
    })?;
    if completions.len() != output_segments.len() {
        return Err(format!(
            "simpler_capi_dispatch_completion_count_mismatch:got={}:expected={}",
            completions.len(),
            output_segments.len()
        ));
    }
    for completion in completions {
        match completion.status {
            CompletionStatus::Success => {}
            other => return Err(format!("simpler_capi_dispatch_failed:{other:?}")),
        }
    }
    let mut produced = Vec::with_capacity(size_bytes);
    for output in output_segments {
        let chunk_output = runtime
            .host_segment_payload(host_node, output)
            .ok_or_else(|| "missing_host_vector_output_payload".to_string())?;
        produced.extend_from_slice(chunk_output);
    }
    let output_values = bytes_to_f32s(&produced);
    let input_values = bytes_to_f32s(&input_a_bytes);
    let input_b_values = bytes_to_f32s(&input_b_bytes);
    if output_values.len() != elems || input_values.len() != elems || input_b_values.len() != elems
    {
        return Err(format!(
            "simpler_capi_output_len_mismatch:first={:?}:output_len={}:input_a_len={}:input_b_len={}",
            output_values.first(),
            output_values.len(),
            input_values.len(),
            input_b_values.len()
        ));
    }
    for (elem_index, ((output, input_a), input_b)) in output_values
        .iter()
        .zip(input_values.iter())
        .zip(input_b_values.iter())
        .enumerate()
    {
        let Some((block, tile, row_group)) = kvcache_layout.tile_row_group_for_elem(elem_index)
        else {
            return Err(format!(
                "kvcache_layout_missing_tile_row_group:elem={elem_index}"
            ));
        };
        let sum = *input_a + *input_b;
        let expected = (sum + 1.0) * (sum + 2.0);
        if (*output - expected).abs() > 1e-5 {
            return Err(format!(
                "simpler_capi_output_mismatch:elem={elem_index}:block={}:prefix={}:tile={}:row_group={}:input_a={}:input_b={}:output={}:expected={}",
                block.block_id,
                block.prefix_group_id,
                tile.tile_id,
                row_group.row_group_id,
                input_a,
                input_b,
                output,
                expected
            ));
        }
    }
    Ok(produced)
}

const W4_DEMO_KVCACHE_PAYLOAD_BYTES: usize = 8192;
const W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES: usize = 8192;
const W4_KVCACHE_BLOCKS: usize = 4;
const W4_KVCACHE_PREFIX_GROUPS: usize = 2;
const W4_KVCACHE_TILE_ROWS: usize = 16;
const W4_KVCACHE_TILE_COLS: usize = 16;
const W4_KVCACHE_TILE_BYTES: usize =
    W4_KVCACHE_TILE_ROWS * W4_KVCACHE_TILE_COLS * std::mem::size_of::<f32>();
const W4_KVCACHE_ROW_GROUP_ROWS: usize = 4;
const W4_KVCACHE_ROW_GROUP_BYTES: usize =
    W4_KVCACHE_ROW_GROUP_ROWS * W4_KVCACHE_TILE_COLS * std::mem::size_of::<f32>();
const W4_HOST_VECTOR_CHUNK_BYTES: usize = 4096;

fn qwen3_dense_0_6b_real_kv_state_bytes(layer_count: u64, token_count: u64) -> u64 {
    layer_count
        .saturating_mul(token_count)
        .saturating_mul(QWEN3_DENSE_0_6B_PROFILE.num_key_value_heads)
        .saturating_mul(QWEN3_DENSE_0_6B_PROFILE.head_dim)
        .saturating_mul(2)
        .saturating_mul(std::mem::size_of::<f32>() as u64)
}

#[derive(Debug, Clone)]
struct KvCachePayloadLayout {
    bytes: usize,
    elems: usize,
    blocks: Vec<KvCachePayloadBlock>,
}

#[derive(Debug, Clone)]
struct KvCachePayloadBlock {
    block_id: usize,
    prefix_group_id: usize,
    offset: usize,
    bytes: usize,
    elems: usize,
    tiles: Vec<KvCachePayloadTile>,
}

#[derive(Debug, Clone)]
struct KvCachePayloadTile {
    tile_id: usize,
    offset: usize,
    bytes: usize,
    elems: usize,
    rows: usize,
    cols: usize,
    row_groups: Vec<KvCachePayloadRowGroup>,
}

#[derive(Debug, Clone)]
struct KvCachePayloadRowGroup {
    row_group_id: usize,
    offset: usize,
    bytes: usize,
    elems: usize,
    rows: usize,
    cols: usize,
}

impl KvCachePayloadLayout {
    fn new(elems: usize, size_bytes: usize) -> Result<Self, String> {
        if size_bytes != W4_DEMO_KVCACHE_PAYLOAD_BYTES {
            return Err(format!("invalid_demo_kvcache_payload_bytes:{size_bytes}"));
        }
        if size_bytes % std::mem::size_of::<f32>() != 0
            || elems * std::mem::size_of::<f32>() != size_bytes
        {
            return Err("invalid_kvcache_payload_elem_shape".to_string());
        }
        if size_bytes % W4_KVCACHE_BLOCKS != 0
            || W4_KVCACHE_TILE_BYTES % W4_KVCACHE_ROW_GROUP_BYTES != 0
        {
            return Err("invalid_kvcache_layout_divisibility".to_string());
        }
        let bytes_per_block = size_bytes / W4_KVCACHE_BLOCKS;
        if bytes_per_block % W4_KVCACHE_TILE_BYTES != 0 {
            return Err("invalid_kvcache_block_tile_shape".to_string());
        }
        let mut blocks = Vec::with_capacity(W4_KVCACHE_BLOCKS);
        for block_id in 0..W4_KVCACHE_BLOCKS {
            let block_offset = block_id * bytes_per_block;
            let tiles_per_block = bytes_per_block / W4_KVCACHE_TILE_BYTES;
            let mut tiles = Vec::with_capacity(tiles_per_block);
            for tile_index in 0..tiles_per_block {
                let tile_offset = block_offset + tile_index * W4_KVCACHE_TILE_BYTES;
                let row_groups_per_tile = W4_KVCACHE_TILE_BYTES / W4_KVCACHE_ROW_GROUP_BYTES;
                let mut row_groups = Vec::with_capacity(row_groups_per_tile);
                for row_group_index in 0..row_groups_per_tile {
                    let row_group_offset =
                        tile_offset + row_group_index * W4_KVCACHE_ROW_GROUP_BYTES;
                    row_groups.push(KvCachePayloadRowGroup {
                        row_group_id: row_group_index,
                        offset: row_group_offset,
                        bytes: W4_KVCACHE_ROW_GROUP_BYTES,
                        elems: W4_KVCACHE_ROW_GROUP_BYTES / std::mem::size_of::<f32>(),
                        rows: W4_KVCACHE_ROW_GROUP_ROWS,
                        cols: W4_KVCACHE_TILE_COLS,
                    });
                }
                tiles.push(KvCachePayloadTile {
                    tile_id: tile_index,
                    offset: tile_offset,
                    bytes: W4_KVCACHE_TILE_BYTES,
                    elems: W4_KVCACHE_TILE_BYTES / std::mem::size_of::<f32>(),
                    rows: W4_KVCACHE_TILE_ROWS,
                    cols: W4_KVCACHE_TILE_COLS,
                    row_groups,
                });
            }
            blocks.push(KvCachePayloadBlock {
                block_id,
                prefix_group_id: block_id % W4_KVCACHE_PREFIX_GROUPS,
                offset: block_offset,
                bytes: bytes_per_block,
                elems: bytes_per_block / std::mem::size_of::<f32>(),
                tiles,
            });
        }
        Ok(Self {
            bytes: size_bytes,
            elems,
            blocks,
        })
    }

    fn tile_row_group_for_elem(
        &self,
        elem_index: usize,
    ) -> Option<(
        &KvCachePayloadBlock,
        &KvCachePayloadTile,
        &KvCachePayloadRowGroup,
    )> {
        let byte_offset = elem_index.checked_mul(std::mem::size_of::<f32>())?;
        self.blocks.iter().find_map(|block| {
            if byte_offset < block.offset || byte_offset >= block.offset + block.bytes {
                return None;
            }
            block.tiles.iter().find_map(|tile| {
                if byte_offset < tile.offset || byte_offset >= tile.offset + tile.bytes {
                    return None;
                }
                tile.row_groups.iter().find_map(|row_group| {
                    if byte_offset >= row_group.offset
                        && byte_offset < row_group.offset + row_group.bytes
                    {
                        Some((block, tile, row_group))
                    } else {
                        None
                    }
                })
            })
        })
    }
}

fn kvcache_input_a_payload(guest_input: &[u8], size_bytes: usize) -> Vec<u8> {
    let mut input = vec![0u8; size_bytes];
    let copy_len = input.len().min(guest_input.len());
    input[..copy_len].copy_from_slice(&guest_input[..copy_len]);
    input
}

fn kvcache_input_b_payload(layout: &KvCachePayloadLayout) -> Vec<u8> {
    let mut values = vec![3.0f32; layout.elems];
    for block in &layout.blocks {
        let block_elem_count: usize = block.tiles.iter().map(|tile| tile.elems).sum();
        debug_assert_eq!(block_elem_count, block.elems);
        for tile in &block.tiles {
            debug_assert_eq!(tile.rows * tile.cols, tile.elems);
            for row_group in &tile.row_groups {
                debug_assert_eq!(row_group.rows * row_group.cols, row_group.elems);
                let row_group_begin = row_group.offset / std::mem::size_of::<f32>();
                let row_group_end = row_group_begin + row_group.elems;
                let layout_bias = kvcache_layout_bias(block, tile, row_group);
                for value in &mut values[row_group_begin..row_group_end] {
                    *value += layout_bias;
                }
            }
        }
    }
    f32s_to_bytes(&values)
}

fn kvcache_layout_bias(
    block: &KvCachePayloadBlock,
    tile: &KvCachePayloadTile,
    row_group: &KvCachePayloadRowGroup,
) -> f32 {
    block.prefix_group_id as f32
        + block.block_id as f32 * 0.125
        + tile.tile_id as f32 * 0.03125
        + row_group.row_group_id as f32 * 0.0078125
}

fn kvcache_layout_bindings_for_chunk(
    layout: &KvCachePayloadLayout,
    host_node: u64,
    input_segment: SegmentHandle,
    chunk_begin: usize,
    chunk_end: usize,
) -> Vec<DispatchBufferBinding> {
    let mut bindings = Vec::new();
    for block in &layout.blocks {
        if !range_contains(chunk_begin, chunk_end, block.offset, block.bytes) {
            continue;
        }
        bindings.push(opaque_resident_binding(
            format!(
                "kvcache_prefix{}_block{}_state",
                block.prefix_group_id, block.block_id
            ),
            BufferUsage::Inout,
            MemoryEndpoint {
                node: host_node,
                segment: input_segment,
                offset: (block.offset - chunk_begin) as u64,
            },
            block.bytes as u64,
        ));
        for tile in &block.tiles {
            if !range_contains(chunk_begin, chunk_end, tile.offset, tile.bytes) {
                continue;
            }
            bindings.push(opaque_resident_binding(
                format!(
                    "kvcache_block{}_prefix{}_tile{}_state",
                    block.block_id, block.prefix_group_id, tile.tile_id
                ),
                BufferUsage::Inout,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_segment,
                    offset: (tile.offset - chunk_begin) as u64,
                },
                tile.bytes as u64,
            ));
            for row_group in &tile.row_groups {
                if !range_contains(chunk_begin, chunk_end, row_group.offset, row_group.bytes) {
                    continue;
                }
                bindings.push(opaque_resident_binding(
                    format!(
                        "kvcache_block{}_prefix{}_tile{}_rowgroup{}",
                        block.block_id, block.prefix_group_id, tile.tile_id, row_group.row_group_id
                    ),
                    BufferUsage::Inout,
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_segment,
                        offset: (row_group.offset - chunk_begin) as u64,
                    },
                    row_group.bytes as u64,
                ));
            }
        }
    }
    bindings
}

fn range_contains(
    container_begin: usize,
    container_end: usize,
    offset: usize,
    bytes: usize,
) -> bool {
    offset >= container_begin && offset + bytes <= container_end
}

#[cfg(unix)]
struct HostVectorDispatchLock {
    fd: libc::c_int,
    _thread_guard: std::sync::MutexGuard<'static, ()>,
}

#[cfg(unix)]
impl Drop for HostVectorDispatchLock {
    fn drop(&mut self) {
        unsafe {
            libc::flock(self.fd, libc::LOCK_UN);
            libc::close(self.fd);
        }
    }
}

#[cfg(unix)]
fn host_vector_dispatch_lock_guard() -> Result<HostVectorDispatchLock, String> {
    static HOST_VECTOR_DISPATCH_MUTEX: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let thread_guard = HOST_VECTOR_DISPATCH_MUTEX
        .lock()
        .map_err(|_| "simpler_capi_dispatch_lock_poisoned".to_string())?;
    unsafe {
        let path = b"/tmp/linqu_simpler_host_vector.lock\0";
        let fd = libc::open(path.as_ptr().cast(), libc::O_CREAT | libc::O_RDWR, 0o666);
        if fd < 0 {
            return Err("simpler_capi_dispatch_lock_open_failed".to_string());
        }
        if libc::flock(fd, libc::LOCK_EX) < 0 {
            libc::close(fd);
            return Err("simpler_capi_dispatch_lock_failed".to_string());
        }

        Ok(HostVectorDispatchLock {
            fd,
            _thread_guard: thread_guard,
        })
    }
}

#[cfg(not(unix))]
struct HostVectorDispatchLock {
    _thread_guard: std::sync::MutexGuard<'static, ()>,
}

#[cfg(not(unix))]
fn host_vector_dispatch_lock_guard() -> Result<HostVectorDispatchLock, String> {
    static HOST_VECTOR_DISPATCH_MUTEX: std::sync::Mutex<()> = std::sync::Mutex::new(());
    let thread_guard = HOST_VECTOR_DISPATCH_MUTEX
        .lock()
        .map_err(|_| "simpler_capi_dispatch_lock_poisoned".to_string())?;
    Ok(HostVectorDispatchLock {
        _thread_guard: thread_guard,
    })
}

#[cfg(all(unix, not(test)))]
fn with_suppressed_stdio<T>(f: impl FnOnce() -> Result<T, String>) -> Result<T, String> {
    if simpler_dispatch_log_enabled() {
        return f();
    }
    let _guard = ProcessStdioRedirectGuard::new().ok();
    f()
}

#[cfg(any(not(unix), test))]
fn with_suppressed_stdio<T>(f: impl FnOnce() -> Result<T, String>) -> Result<T, String> {
    f()
}

#[cfg(all(unix, not(test)))]
fn simpler_dispatch_log_enabled() -> bool {
    std::env::var("SIM_SIMPLER_DISPATCH_LOG")
        .map(|value| {
            matches!(
                value.as_str(),
                "1" | "true" | "TRUE" | "yes" | "YES" | "on" | "ON"
            )
        })
        .unwrap_or(false)
}

#[cfg(all(unix, not(test)))]
struct ProcessStdioRedirectGuard {
    stdout_fd: i32,
    stderr_fd: i32,
}

#[cfg(all(unix, not(test)))]
impl ProcessStdioRedirectGuard {
    fn new() -> Result<Self, String> {
        let dev_null = OpenOptions::new()
            .write(true)
            .open("/dev/null")
            .map_err(|err| format!("open_dev_null_failed:{err}"))?;
        let stdout_fd = unsafe { libc::dup(libc::STDOUT_FILENO) };
        if stdout_fd < 0 {
            return Err(format!(
                "dup_stdout_failed:{}",
                std::io::Error::last_os_error()
            ));
        }
        let stderr_fd = unsafe { libc::dup(libc::STDERR_FILENO) };
        if stderr_fd < 0 {
            unsafe {
                libc::close(stdout_fd);
            }
            return Err(format!(
                "dup_stderr_failed:{}",
                std::io::Error::last_os_error()
            ));
        }
        unsafe {
            libc::fflush(std::ptr::null_mut());
            if libc::dup2(dev_null.as_raw_fd(), libc::STDOUT_FILENO) < 0 {
                libc::close(stdout_fd);
                libc::close(stderr_fd);
                return Err(format!(
                    "redirect_stdout_failed:{}",
                    std::io::Error::last_os_error()
                ));
            }
            if libc::dup2(dev_null.as_raw_fd(), libc::STDERR_FILENO) < 0 {
                let err = std::io::Error::last_os_error();
                libc::dup2(stdout_fd, libc::STDOUT_FILENO);
                libc::close(stdout_fd);
                libc::close(stderr_fd);
                return Err(format!("redirect_stderr_failed:{err}"));
            }
        }
        Ok(Self {
            stdout_fd,
            stderr_fd,
        })
    }
}

#[cfg(all(unix, not(test)))]
impl Drop for ProcessStdioRedirectGuard {
    fn drop(&mut self) {
        unsafe {
            libc::fflush(std::ptr::null_mut());
            libc::dup2(self.stdout_fd, libc::STDOUT_FILENO);
            libc::dup2(self.stderr_fd, libc::STDERR_FILENO);
            libc::close(self.stdout_fd);
            libc::close(self.stderr_fd);
        }
    }
}

fn simpler_manifest_path() -> Result<PathBuf, String> {
    let path = std::env::var("SIMPLER_HOST_VECTOR_MANIFEST").unwrap_or_else(|_| {
        "/tmp/simpler-host-vector-artifacts/host_vector_manifest.json".to_string()
    });
    let path = PathBuf::from(path);
    if !path.exists() {
        ensure_simpler_host_vector_manifest(&path)?;
    }
    if !path.exists() {
        return Err(format!(
            "missing_simpler_host_vector_manifest:{}",
            path.display()
        ));
    }
    Ok(path)
}

fn ensure_simpler_host_vector_manifest(manifest_path: &Path) -> Result<(), String> {
    let output_dir = manifest_path.parent().ok_or_else(|| {
        format!(
            "host_vector_manifest_has_no_parent:{}",
            manifest_path.display()
        )
    })?;
    let script = simpler_host_vector_artifact_producer_path();
    if !script.exists() {
        return Err(format!(
            "missing_simpler_host_vector_artifact_producer:{}",
            script.display()
        ));
    }

    let status = std::process::Command::new("python3")
        .arg(&script)
        .arg("--output-dir")
        .arg(output_dir)
        .status()
        .map_err(|err| format!("run_simpler_host_vector_artifact_producer_failed:{err}"))?;
    if !status.success() {
        return Err(format!(
            "simpler_host_vector_artifact_producer_failed:{}:status={status}",
            script.display()
        ));
    }
    Ok(())
}

fn simpler_host_vector_artifact_producer_path() -> PathBuf {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .map(Path::to_path_buf)
        .unwrap_or_else(|| PathBuf::from("."));
    root.join("guest-linux")
        .join("aarch64")
        .join("scripts")
        .join("prepare_simpler_host_vector_artifacts.py")
}

fn simpler_matmul_manifest_path() -> Result<PathBuf, String> {
    let path = std::env::var("SIMPLER_HOST_MATMUL_MANIFEST").unwrap_or_else(|_| {
        "/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json".to_string()
    });
    let path = PathBuf::from(path);
    if !path.exists() {
        return Err(format!(
            "missing_simpler_host_matmul_manifest:{}",
            path.display()
        ));
    }
    Ok(path)
}

fn simpler_matmul_batch_manifest_path(
    base_manifest_path: &Path,
    _tile_batch: usize,
) -> Result<Option<PathBuf>, String> {
    let path = std::env::var("SIMPLER_HOST_MATMUL_BATCH_MANIFEST")
        .ok()
        .map(PathBuf::from);
    let Some(path) = path else {
        return Ok(None);
    };
    if !path.exists() {
        return Ok(None);
    }
    validate_simpler_matmul_batch_manifest_runtime_reuse(base_manifest_path, &path)?;
    Ok(Some(path))
}

fn validate_simpler_matmul_batch_manifest_runtime_reuse(
    base_manifest_path: &Path,
    batch_manifest_path: &Path,
) -> Result<(), String> {
    let base_text = std::fs::read_to_string(base_manifest_path).map_err(|err| {
        format!(
            "read_simpler_matmul_manifest_failed:{}:{err}",
            base_manifest_path.display()
        )
    })?;
    let batch_text = std::fs::read_to_string(batch_manifest_path).map_err(|err| {
        format!(
            "read_simpler_matmul_batch_manifest_failed:{}:{err}",
            batch_manifest_path.display()
        )
    })?;
    let base_manifest: SimplerRuntimeManifestEnvelope =
        serde_json::from_str(&base_text).map_err(|err| {
            format!(
                "parse_simpler_matmul_manifest_failed:{}:{err}",
                base_manifest_path.display()
            )
        })?;
    let batch_manifest: SimplerRuntimeManifestEnvelope = serde_json::from_str(&batch_text)
        .map_err(|err| {
            format!(
                "parse_simpler_matmul_batch_manifest_failed:{}:{err}",
                batch_manifest_path.display()
            )
        })?;
    let base_runtime = &base_manifest.simpler_runtime;
    let batch_runtime = &batch_manifest.simpler_runtime;
    let same_runtime = base_runtime.host_runtime_library.source
        == batch_runtime.host_runtime_library.source
        && base_runtime
            .aicpu_binary
            .as_ref()
            .map(|artifact| artifact.source.as_str())
            == batch_runtime
                .aicpu_binary
                .as_ref()
                .map(|artifact| artifact.source.as_str())
        && base_runtime
            .aicore_binary
            .as_ref()
            .map(|artifact| artifact.source.as_str())
            == batch_runtime
                .aicore_binary
                .as_ref()
                .map(|artifact| artifact.source.as_str());
    if !same_runtime {
        return Err(format!(
            "simpler_matmul_batch_manifest_must_reuse_base_runtime:base={}:batch={}",
            base_manifest_path.display(),
            batch_manifest_path.display()
        ));
    }
    Ok(())
}

fn scenario_config_for_chipbackend() -> Result<ScenarioConfig, String> {
    let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
    let default_path = manifest_dir
        .join("../../scenarios/mvp_2host_single_domain.yaml")
        .canonicalize()
        .unwrap_or_else(|_| PathBuf::from("scenarios/mvp_2host_single_domain.yaml"));
    let path = std::env::var("SIM_UAPI_SCENARIO_CONFIG")
        .map(PathBuf::from)
        .unwrap_or(default_path);
    let yaml = std::fs::read_to_string(&path)
        .map_err(|err| format!("read_scenario_config_failed:{}:{err}", path.display()))?;
    let yaml = yaml.replace("chip_backend_mode: stub", "chip_backend_mode: simpler_capi");
    ScenarioConfig::from_yaml_str(&yaml)
        .map_err(|err| format!("parse_scenario_config_failed:{}:{err}", path.display()))
}

fn host_vector_backend_spec_from_manifest(
    manifest_path: &Path,
    input_a: MemoryEndpoint,
    input_b: MemoryEndpoint,
    output_f: MemoryEndpoint,
    size_bytes: u64,
    elems: u64,
) -> Result<DispatchBackendSpec, String> {
    let manifest_text = std::fs::read_to_string(manifest_path).map_err(|err| {
        format!(
            "read_simpler_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let manifest: SimplerRuntimeManifestEnvelope =
        serde_json::from_str(&manifest_text).map_err(|err| {
            format!(
                "parse_simpler_manifest_failed:{}:{err}",
                manifest_path.display()
            )
        })?;
    let args = vec![
        SimplerRuntimeArg::InputSegment {
            endpoint: input_a,
            bytes: size_bytes,
        },
        SimplerRuntimeArg::InputSegment {
            endpoint: input_b,
            bytes: size_bytes,
        },
        SimplerRuntimeArg::OutputSegment {
            endpoint: output_f,
            bytes: size_bytes,
        },
        SimplerRuntimeArg::ScalarU64(size_bytes),
        SimplerRuntimeArg::ScalarU64(size_bytes),
        SimplerRuntimeArg::ScalarU64(size_bytes),
        SimplerRuntimeArg::ScalarU64(elems),
    ];
    let runtime = SimplerRuntimeArtifacts {
        host_runtime_library: manifest.simpler_runtime.host_runtime_library,
        orch_shared_object: manifest.simpler_runtime.orch_shared_object,
        orch_function_name: manifest.simpler_runtime.orch_function_name,
        aicpu_binary: manifest.simpler_runtime.aicpu_binary,
        aicore_binary: manifest.simpler_runtime.aicore_binary,
        kernels: manifest.simpler_runtime.kernels,
        launch: manifest.simpler_runtime.launch,
        runtime_env: manifest.simpler_runtime.runtime_env,
        args,
    };
    Ok(DispatchBackendSpec {
        profile: DispatchBackendProfile::HostVector,
        platform: "a2a3sim".to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_vector_example".to_string()),
        simpler_runtime: Some(runtime),
        context: None,
    })
}

fn host_matmul_backend_spec_from_manifest(
    manifest_path: &Path,
    input_a: MemoryEndpoint,
    input_w1: MemoryEndpoint,
    input_w2: MemoryEndpoint,
    output_f: MemoryEndpoint,
    input_bytes: u64,
    output_bytes: u64,
    elems: u64,
) -> Result<DispatchBackendSpec, String> {
    let manifest_text = std::fs::read_to_string(manifest_path).map_err(|err| {
        format!(
            "read_simpler_matmul_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let manifest: SimplerRuntimeManifestEnvelope =
        serde_json::from_str(&manifest_text).map_err(|err| {
            format!(
                "parse_simpler_matmul_manifest_failed:{}:{err}",
                manifest_path.display()
            )
        })?;
    let args = vec![
        SimplerRuntimeArg::InputSegment {
            endpoint: input_a,
            bytes: input_bytes,
        },
        SimplerRuntimeArg::InputSegment {
            endpoint: input_w1,
            bytes: input_bytes,
        },
        SimplerRuntimeArg::InputSegment {
            endpoint: input_w2,
            bytes: input_bytes,
        },
        SimplerRuntimeArg::OutputSegment {
            endpoint: output_f,
            bytes: output_bytes,
        },
        SimplerRuntimeArg::ScalarU64(input_bytes),
        SimplerRuntimeArg::ScalarU64(input_bytes),
        SimplerRuntimeArg::ScalarU64(input_bytes),
        SimplerRuntimeArg::ScalarU64(output_bytes),
        SimplerRuntimeArg::ScalarU64(elems),
    ];
    let runtime = SimplerRuntimeArtifacts {
        host_runtime_library: manifest.simpler_runtime.host_runtime_library,
        orch_shared_object: manifest.simpler_runtime.orch_shared_object,
        orch_function_name: manifest.simpler_runtime.orch_function_name,
        aicpu_binary: manifest.simpler_runtime.aicpu_binary,
        aicore_binary: manifest.simpler_runtime.aicore_binary,
        kernels: manifest.simpler_runtime.kernels,
        launch: manifest.simpler_runtime.launch,
        runtime_env: manifest.simpler_runtime.runtime_env,
        args,
    };
    Ok(DispatchBackendSpec {
        profile: DispatchBackendProfile::HostMatmul,
        platform: "a2a3sim".to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_matmul_example".to_string()),
        simpler_runtime: Some(runtime),
        context: None,
    })
}

fn host_matmul_batched_backend_spec_from_manifest(
    manifest_path: &Path,
    input_a: MemoryEndpoint,
    input_w1: MemoryEndpoint,
    input_w2: MemoryEndpoint,
    output_f: MemoryEndpoint,
    input_bytes: u64,
    output_bytes: u64,
    tile_elems: u64,
    tile_batch: u64,
) -> Result<DispatchBackendSpec, String> {
    let manifest_text = std::fs::read_to_string(manifest_path).map_err(|err| {
        format!(
            "read_simpler_matmul_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let manifest: SimplerRuntimeManifestEnvelope =
        serde_json::from_str(&manifest_text).map_err(|err| {
            format!(
                "parse_simpler_matmul_manifest_failed:{}:{err}",
                manifest_path.display()
            )
        })?;
    let args = vec![
        SimplerRuntimeArg::InputSegment {
            endpoint: input_a,
            bytes: input_bytes,
        },
        SimplerRuntimeArg::InputSegment {
            endpoint: input_w1,
            bytes: input_bytes,
        },
        SimplerRuntimeArg::InputSegment {
            endpoint: input_w2,
            bytes: input_bytes,
        },
        SimplerRuntimeArg::OutputSegment {
            endpoint: output_f,
            bytes: output_bytes,
        },
        SimplerRuntimeArg::ScalarU64(input_bytes),
        SimplerRuntimeArg::ScalarU64(input_bytes),
        SimplerRuntimeArg::ScalarU64(input_bytes),
        SimplerRuntimeArg::ScalarU64(output_bytes),
        SimplerRuntimeArg::ScalarU64(tile_elems),
        SimplerRuntimeArg::ScalarU64(tile_batch),
    ];
    let runtime = SimplerRuntimeArtifacts {
        host_runtime_library: manifest.simpler_runtime.host_runtime_library,
        orch_shared_object: manifest.simpler_runtime.orch_shared_object,
        orch_function_name: manifest.simpler_runtime.orch_function_name,
        aicpu_binary: manifest.simpler_runtime.aicpu_binary,
        aicore_binary: manifest.simpler_runtime.aicore_binary,
        kernels: manifest.simpler_runtime.kernels,
        launch: manifest.simpler_runtime.launch,
        runtime_env: manifest.simpler_runtime.runtime_env,
        args,
    };
    Ok(DispatchBackendSpec {
        profile: DispatchBackendProfile::HostMatmul,
        platform: "a2a3sim".to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_matmul_batched_example".to_string()),
        simpler_runtime: Some(runtime),
        context: None,
    })
}

fn kvcache_host_vector_request(
    task_id: u64,
    layout: &KvCachePayloadLayout,
    chunk_index: usize,
    chunk_offset: usize,
    bindings: Vec<DispatchBufferBinding>,
) -> BackendExecutionRequest {
    BackendExecutionRequest {
        correlation: RequestCorrelation {
            request_id: format!(
                "uapi-kvcache-host-vector-dispatch-{task_id}-chunk{chunk_index}-offset{chunk_offset}-blocks{}-groups{}-tile{}x{}-rowgroup{}x{}",
                layout.blocks.len(),
                W4_KVCACHE_PREFIX_GROUPS,
                W4_KVCACHE_TILE_ROWS,
                W4_KVCACHE_TILE_COLS,
                W4_KVCACHE_ROW_GROUP_ROWS,
                W4_KVCACHE_TILE_COLS
            ),
            trace_id: Some(format!(
                "w4-kvcache-layout-bytes{}-elems{}",
                layout.bytes, layout.elems
            )),
            op_name: Some("w4_kvcache_host_vector_example".to_string()),
            step_index: Some(0),
            sequence_no: Some(task_id),
        },
        plan: None,
        context: Some(ExecutionContextRef {
            device_context_id: "device-ctx-uapi-kvcache-host-vector".to_string(),
            runtime_context_id: Some(format!("runtime-ctx-uapi-kvcache-{task_id}")),
            lifecycle: ExecutionLifecycle::Init,
            warm: true,
            reusable: true,
        }),
        bindings,
    }
}

fn kvcache_host_matmul_request(
    task_id: u64,
    bindings: Vec<DispatchBufferBinding>,
) -> BackendExecutionRequest {
    BackendExecutionRequest {
        correlation: RequestCorrelation {
            request_id: format!("uapi-kvcache-host-matmul-dispatch-{task_id}"),
            trace_id: Some("w4-kvcache-host-matmul-128x128".to_string()),
            op_name: Some("w4_kvcache_host_matmul_example".to_string()),
            step_index: Some(0),
            sequence_no: Some(task_id),
        },
        plan: None,
        context: Some(ExecutionContextRef {
            device_context_id: "device-ctx-uapi-kvcache-host-matmul".to_string(),
            runtime_context_id: Some(format!("runtime-ctx-uapi-kvcache-matmul-{task_id}")),
            lifecycle: ExecutionLifecycle::Init,
            warm: true,
            reusable: true,
        }),
        bindings,
    }
}

#[derive(Clone, Copy, Debug)]
enum Qwen3ProjectionKind {
    Q,
    Kv,
    V,
}

fn qwen3_dense_0_6b_request(
    task_id: u64,
    profile: Qwen3Dense06bProfile,
    shard: Qwen3Dense06bShard,
    bindings: Vec<DispatchBufferBinding>,
) -> BackendExecutionRequest {
    BackendExecutionRequest {
        correlation: RequestCorrelation {
            request_id: format!(
                "uapi-qwen3-dense-0-6b-prefill-dispatch-{task_id}-shard{}-node{}-heads{}-{}",
                shard.shard_id, shard.owner_node, shard.head_start, shard.head_end
            ),
            trace_id: Some(format!(
                "w4-qwen3-dense-0-6b-layers{}-hidden{}-heads{}-kvheads{}-headdim{}-prefill{}-decode{}-tp{}-shard{}-kvblocks{}-{}",
                profile.num_hidden_layers,
                profile.hidden_size,
                profile.num_attention_heads,
                profile.num_key_value_heads,
                profile.head_dim,
                profile.prefill_tokens,
                profile.decode_tokens,
                profile.tp_nodes,
                shard.shard_id,
                shard.kv_block_start,
                shard.kv_block_end
            )),
            op_name: Some("w4_qwen3_dense_0_6b_prefill_matmul".to_string()),
            step_index: Some(shard.shard_id as u32),
            sequence_no: Some(task_id + shard.shard_id),
        },
        plan: None,
        context: Some(ExecutionContextRef {
            device_context_id: format!(
                "device-ctx-uapi-qwen3-dense-0-6b-node{}-shard{}",
                shard.owner_node, shard.shard_id
            ),
            runtime_context_id: Some(format!(
                "runtime-ctx-uapi-qwen3-dense-0-6b-{task_id}-shard{}",
                shard.shard_id
            )),
            lifecycle: ExecutionLifecycle::Init,
            warm: true,
            reusable: true,
        }),
        bindings,
    }
}

fn run_host_matmul_smoke(topology: &SimTopology, task: &TaskKey) -> Result<Vec<u8>, String> {
    const MATMUL_DIM: usize = 128;
    const MATMUL_ELEMS: usize = MATMUL_DIM * MATMUL_DIM;
    const HALF_ONE: u16 = 0x3c00;

    let manifest_path = simpler_matmul_manifest_path()?;
    let scenario_config = scenario_config_for_chipbackend()?;
    let segment_base = 20_000 + task.task_id.saturating_mul(10);
    let input_a = SegmentHandle(segment_base + 1);
    let input_w1 = SegmentHandle(segment_base + 2);
    let input_w2 = SegmentHandle(segment_base + 3);
    let output_f = SegmentHandle(segment_base + 4);
    let input_bytes = MATMUL_ELEMS * std::mem::size_of::<u16>();
    let output_bytes = MATMUL_ELEMS * std::mem::size_of::<f32>();
    let host_node = topology
        .hosts
        .first()
        .map(|host| host.node_id)
        .ok_or_else(|| "missing_host_node".to_string())?;
    let ubpu_node = topology
        .ubpus
        .first()
        .map(|ubpu| ubpu.node_id)
        .ok_or_else(|| "missing_ubpu_node".to_string())?;
    let _dispatch_lock = host_vector_dispatch_lock_guard()?;
    let mut runtime = LocalRuntimeEngine::from_config(&scenario_config);
    runtime.seed_host_segment(
        host_node,
        input_a,
        repeated_u16_le_bytes(HALF_ONE, MATMUL_ELEMS),
    );
    runtime.seed_host_segment(
        host_node,
        input_w1,
        repeated_u16_le_bytes(HALF_ONE, MATMUL_ELEMS),
    );
    runtime.seed_host_segment(
        host_node,
        input_w2,
        repeated_u16_le_bytes(HALF_ONE, MATMUL_ELEMS),
    );
    runtime.seed_host_segment(host_node, output_f, vec![0u8; output_bytes]);

    let backend_spec = host_matmul_backend_spec_from_manifest(
        &manifest_path,
        MemoryEndpoint {
            node: host_node,
            segment: input_a,
            offset: 0,
        },
        MemoryEndpoint {
            node: host_node,
            segment: input_w1,
            offset: 0,
        },
        MemoryEndpoint {
            node: host_node,
            segment: input_w2,
            offset: 0,
        },
        MemoryEndpoint {
            node: host_node,
            segment: output_f,
            offset: 0,
        },
        input_bytes as u64,
        output_bytes as u64,
        MATMUL_ELEMS as u64,
    )?;
    let request = kvcache_host_matmul_request(
        task.task_id,
        vec![
            opaque_binding(
                "kvcache_matmul_a_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_a,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            opaque_binding(
                "kvcache_matmul_w1_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_w1,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            opaque_binding(
                "kvcache_matmul_w2_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_w2,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            dense_f32_binding(
                "kvcache_matmul_output_f",
                BufferUsage::Output,
                MemoryEndpoint {
                    node: host_node,
                    segment: output_f,
                    offset: 0,
                },
                MATMUL_ELEMS as u64,
            ),
        ],
    );
    let dispatch = BackendDispatchOperation {
        task: task.clone(),
        function: FunctionLabel {
            name: "host_matmul_example".into(),
            level: PlLevel::L2,
        },
        backend_spec,
        request,
        target_level: PlLevel::L2,
        target_node: ubpu_node,
        legacy_input_segments: vec![input_a, input_w1, input_w2],
    };
    let mut sink = VecEventSink::default();
    let complete_at = scenario_config
        .pypto
        .simpler_boundary
        .dispatch_latency_us
        .unwrap_or(15);
    let completion = with_suppressed_stdio(|| {
        runtime
            .submit_backend_dispatch(dispatch, &mut sink)
            .map_err(|err| err.to_string())?;
        runtime.advance_to(complete_at, &mut sink);
        runtime
            .poll_completions(complete_at, &mut sink)
            .into_iter()
            .next()
            .ok_or_else(|| "simpler_capi_matmul_dispatch_did_not_complete".to_string())
    })?;
    match completion.status {
        CompletionStatus::Success => {}
        other => return Err(format!("simpler_capi_matmul_dispatch_failed:{other:?}")),
    }
    let produced = runtime
        .host_segment_payload(host_node, output_f)
        .ok_or_else(|| "missing_host_matmul_output_payload".to_string())?;
    let output_values = bytes_to_f32s(produced);
    if output_values.len() != MATMUL_ELEMS {
        return Err(format!(
            "simpler_capi_matmul_output_len_mismatch:got={}:expected={}",
            output_values.len(),
            MATMUL_ELEMS
        ));
    }
    if output_values
        .iter()
        .any(|value| !value.is_finite() || *value <= 0.0)
    {
        return Err(format!(
            "simpler_capi_matmul_output_invalid:first={:?}",
            output_values.first()
        ));
    }
    Ok(produced.to_vec())
}

#[cfg(test)]
fn run_host_matmul_batched_smoke(
    topology: &SimTopology,
    task: &TaskKey,
    manifest_path: &Path,
    tile_batch: usize,
) -> Result<Vec<u8>, String> {
    const MATMUL_DIM: usize = 128;
    const MATMUL_ELEMS: usize = MATMUL_DIM * MATMUL_DIM;
    const HALF_ONE: u16 = 0x3c00;

    let scenario_config = scenario_config_for_chipbackend()?;
    let segment_base = 21_000 + task.task_id.saturating_mul(10);
    let input_a = SegmentHandle(segment_base + 1);
    let input_w1 = SegmentHandle(segment_base + 2);
    let input_w2 = SegmentHandle(segment_base + 3);
    let output_f = SegmentHandle(segment_base + 4);
    let input_bytes = MATMUL_ELEMS * std::mem::size_of::<u16>() * tile_batch;
    let output_bytes = MATMUL_ELEMS * std::mem::size_of::<f32>() * tile_batch;
    let host_node = topology
        .hosts
        .first()
        .map(|host| host.node_id)
        .ok_or_else(|| "missing_host_node".to_string())?;
    let ubpu_node = topology
        .ubpus
        .first()
        .map(|ubpu| ubpu.node_id)
        .ok_or_else(|| "missing_ubpu_node".to_string())?;
    let _dispatch_lock = host_vector_dispatch_lock_guard()?;
    let mut runtime = LocalRuntimeEngine::from_config(&scenario_config);
    runtime.seed_host_segment(
        host_node,
        input_a,
        repeated_u16_le_bytes(HALF_ONE, MATMUL_ELEMS * tile_batch),
    );
    runtime.seed_host_segment(
        host_node,
        input_w1,
        repeated_u16_le_bytes(HALF_ONE, MATMUL_ELEMS * tile_batch),
    );
    runtime.seed_host_segment(
        host_node,
        input_w2,
        repeated_u16_le_bytes(HALF_ONE, MATMUL_ELEMS * tile_batch),
    );
    runtime.seed_host_segment(host_node, output_f, vec![0u8; output_bytes]);

    let backend_spec = host_matmul_batched_backend_spec_from_manifest(
        manifest_path,
        MemoryEndpoint {
            node: host_node,
            segment: input_a,
            offset: 0,
        },
        MemoryEndpoint {
            node: host_node,
            segment: input_w1,
            offset: 0,
        },
        MemoryEndpoint {
            node: host_node,
            segment: input_w2,
            offset: 0,
        },
        MemoryEndpoint {
            node: host_node,
            segment: output_f,
            offset: 0,
        },
        input_bytes as u64,
        output_bytes as u64,
        MATMUL_ELEMS as u64,
        tile_batch as u64,
    )?;
    let request = kvcache_host_matmul_request(
        task.task_id,
        vec![
            opaque_binding(
                "batched_matmul_a_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_a,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            opaque_binding(
                "batched_matmul_w1_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_w1,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            opaque_binding(
                "batched_matmul_w2_half",
                BufferUsage::Input,
                MemoryEndpoint {
                    node: host_node,
                    segment: input_w2,
                    offset: 0,
                },
                input_bytes as u64,
            ),
            dense_f32_binding(
                "batched_matmul_output_f",
                BufferUsage::Output,
                MemoryEndpoint {
                    node: host_node,
                    segment: output_f,
                    offset: 0,
                },
                (MATMUL_ELEMS * tile_batch) as u64,
            ),
        ],
    );
    let dispatch = BackendDispatchOperation {
        task: task.clone(),
        function: FunctionLabel {
            name: format!("host_matmul_batched_{tile_batch}_example"),
            level: PlLevel::L2,
        },
        backend_spec,
        request,
        target_level: PlLevel::L2,
        target_node: ubpu_node,
        legacy_input_segments: vec![input_a, input_w1, input_w2],
    };
    let mut sink = VecEventSink::default();
    let complete_at = scenario_config
        .pypto
        .simpler_boundary
        .dispatch_latency_us
        .unwrap_or(15);
    let completion = with_suppressed_stdio(|| {
        runtime
            .submit_backend_dispatch(dispatch, &mut sink)
            .map_err(|err| err.to_string())?;
        runtime.advance_to(complete_at, &mut sink);
        runtime
            .poll_completions(complete_at, &mut sink)
            .into_iter()
            .next()
            .ok_or_else(|| "simpler_capi_batched_matmul_dispatch_did_not_complete".to_string())
    })?;
    match completion.status {
        CompletionStatus::Success => {}
        other => {
            return Err(format!(
                "simpler_capi_batched_matmul_dispatch_failed:{other:?}"
            ))
        }
    }
    let produced = runtime
        .host_segment_payload(host_node, output_f)
        .ok_or_else(|| "missing_host_matmul_batched_output_payload".to_string())?;
    if produced.len() != output_bytes {
        return Err(format!(
            "simpler_capi_batched_matmul_output_len_mismatch:got={}:expected={output_bytes}",
            produced.len()
        ));
    }
    Ok(produced.to_vec())
}

fn run_qwen3_dense_0_6b_prefill_runtime(
    topology: &SimTopology,
    task: &TaskKey,
    guest_input: &[u8],
    mut runtime_weight_objects: Option<&mut LingquObjectServiceStub>,
) -> Result<Vec<u8>, String> {
    const MATMUL_DIM: usize = 128;
    const MATMUL_ELEMS: usize = MATMUL_DIM * MATMUL_DIM;
    const TILES_PER_SHARD: u64 = 2;
    const SEGMENTS_PER_TILE: u64 = 32;

    let timing_enabled = std::env::var("SIM_QWEN3_STAGE_TIMING").as_deref() == Ok("1");
    let range_compute_contract = qwen3_guest_range_compute_contract(task)?;
    let timing_start = Instant::now();
    let mut timing_last = timing_start;
    macro_rules! qwen3_stage_timing_mark {
        ($stage:literal) => {
            if timing_enabled {
                let now = Instant::now();
                eprintln!(
                    "qwen3-stage-timing: stage={} delta_ms={} total_ms={}",
                    $stage,
                    now.duration_since(timing_last).as_millis(),
                    now.duration_since(timing_start).as_millis()
                );
                timing_last = now;
            }
        };
    }

    let profile = QWEN3_DENSE_0_6B_PROFILE;
    let manifest_path = simpler_matmul_manifest_path()?;
    let scenario_config = scenario_config_for_chipbackend()?;
    let tp_plan = qwen3_dense_0_6b::tensor_parallel_plan(topology, profile)?;
    let prompt_token_ids = qwen3_dense_0_6b_guest_input_token_ids(guest_input);
    let real_input_embedding_hidden =
        qwen3_dense_0_6b_real_input_embedding_hidden(&prompt_token_ids)?;
    let real_weight_reference_summary = qwen3_dense_0_6b_real_weight_reference_summary(topology)?;
    let real_qkv_reference_values =
        qwen3_dense_0_6b_real_qkv_reference_values_for_layer_with_hidden(
            topology,
            0,
            real_input_embedding_hidden.as_deref(),
        )?;
    let real_mlp_reference_summary =
        qwen3_dense_0_6b_real_mlp_reference_summary_for_layer_with_hidden(
            topology,
            0,
            real_input_embedding_hidden.as_deref(),
        )?;
    let next_layer_real_weight_reference_summary =
        qwen3_dense_0_6b_real_weight_reference_summary_for_layer(topology, 1)?;
    let next_layer_real_qkv_reference_values =
        qwen3_dense_0_6b_real_qkv_reference_values_for_layer_with_hidden(
            topology,
            1,
            real_input_embedding_hidden.as_deref(),
        )?;
    let next_layer_real_mlp_reference_summary =
        qwen3_dense_0_6b_real_mlp_reference_summary_for_layer_with_hidden(
            topology,
            1,
            real_input_embedding_hidden.as_deref(),
        )?;
    let real_tokenizer_asset_summary = qwen3_dense_0_6b_real_tokenizer_asset_summary()?;
    let real_tokenizer_path = qwen3_dense_0_6b_real_tokenizer_path();
    let real_tokenizer_sample_token_count = real_tokenizer_asset_summary
        .as_ref()
        .map(qwen3_dense_0_6b_tokenizer_sample_token_count)
        .unwrap_or(profile.vocab_size);
    let logits_sample_token_count = real_tokenizer_sample_token_count.min(profile.vocab_size);
    let shard_plan: Vec<Qwen3Dense06bShard> = tp_plan
        .iter()
        .map(|tp_shard| Qwen3Dense06bShard {
            shard_id: tp_shard.shard_id,
            owner_node: tp_shard.owner_node,
            target_node: tp_shard.target_node,
            head_start: tp_shard.q_head_start,
            head_end: tp_shard.q_head_end,
            kv_block_start: 0,
            kv_block_end: 0,
        })
        .collect();
    let segment_base = 30_000 + task.task_id.saturating_mul(100);
    let model_meta = SegmentHandle(segment_base + 80);
    let kv_layout = SegmentHandle(segment_base + 81);
    let guest_input_payload = SegmentHandle(segment_base + 82);
    let input_bytes = MATMUL_ELEMS * std::mem::size_of::<u16>();
    let output_bytes = MATMUL_ELEMS * std::mem::size_of::<f32>();
    let host_node = topology
        .hosts
        .first()
        .map(|host| host.node_id)
        .ok_or_else(|| "missing_host_node".to_string())?;
    let _dispatch_lock = host_vector_dispatch_lock_guard()?;
    let mut runtime = LocalRuntimeEngine::from_config(&scenario_config);
    runtime.seed_host_segment(
        host_node,
        model_meta,
        qwen3_dense_0_6b_model_meta_payload(profile),
    );
    runtime.seed_host_segment(
        host_node,
        kv_layout,
        qwen3_dense_0_6b_kv_layout_payload(profile),
    );
    runtime.seed_host_segment(
        host_node,
        guest_input_payload,
        qwen3_dense_0_6b_guest_input_payload(guest_input),
    );

    let mut sink = VecEventSink::default();
    let dispatch_latency = scenario_config
        .pypto
        .simpler_boundary
        .dispatch_latency_us
        .unwrap_or(15);
    let mut runtime_time = 0;
    let tile_count = shard_plan.len() * TILES_PER_SHARD as usize;
    let mut produced = Vec::new();
    let mut tile_checksums = Vec::with_capacity(tile_count);
    let mut weights_service = WeightsServiceStub::new();
    let mut round0_outputs = Vec::with_capacity(tile_count);
    let mut round1_outputs = Vec::with_capacity(tile_count);
    let mut projection_descriptors = Vec::with_capacity(tile_count * 3);
    let mut layer_dependency_descriptors = Vec::with_capacity(tile_count * 24);
    let mut kvcache_descriptors =
        Vec::with_capacity(tile_count * profile.num_hidden_layers as usize * 2);
    let mut kvcache_update_seq = 1u64;
    let runtime_kvcache_store = RefCell::new(Qwen3Dense06bKvCacheStore::default());
    let round1_dispatch_batch_size = qwen3_dense_0_6b_simpler_round1_dispatch_batch_size();
    qwen3_stage_timing_mark!("setup");
    let real_range_forward_enabled = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok();
    if real_range_forward_enabled {
        if let Some(contract) = range_compute_contract {
            let result_flow_checksum = checksum_words(&[
                u64::from(contract.node),
                u64::from(contract.layer_start),
                u64::from(contract.layer_end),
                u64::from(contract.pipeline_nodes),
                0x7166_7764_5f6f_6e6c,
            ]);
            let range_forward_summary = qwen3_dense_0_6b_range_forward_summary_from_contract(
                topology,
                contract,
                guest_input,
                real_input_embedding_hidden.as_deref(),
                result_flow_checksum,
            )?;
            let terminal_range_owner = u32::from(contract.node) + 1 == contract.pipeline_nodes;
            let mut qkv_reference_digest_by_tile = BTreeMap::new();
            let mut mlp_reference_digest_by_tile = BTreeMap::new();
            qkv_reference_digest_by_tile.insert(0, range_forward_summary.range_layer_checksum);
            mlp_reference_digest_by_tile.insert(0, range_forward_summary.output_tensor_checksum);
            let virtual_runtime_output = if terminal_range_owner {
                let mut shard = shard_plan
                    .first()
                    .copied()
                    .ok_or_else(|| "qwen3_range_forward_missing_virtual_shard".to_string())?;
                shard.kv_block_start = 0;
                shard.kv_block_end = 1;
                vec![(
                    shard,
                    Vec::new(),
                    SegmentHandle(segment_base + 9_000),
                    range_forward_summary.output_tensor_checksum,
                )]
            } else {
                Vec::new()
            };
            let logits_descriptor_tokenizer_path = if terminal_range_owner {
                real_tokenizer_path.as_deref()
            } else {
                None
            };
            let logits_descriptors = qwen3_dense_0_6b_logits_descriptors(
                &virtual_runtime_output,
                &BTreeMap::new(),
                &qkv_reference_digest_by_tile,
                &mlp_reference_digest_by_tile,
                profile.vocab_size,
                logits_sample_token_count,
                logits_descriptor_tokenizer_path,
                guest_input,
                None,
                Some(&range_forward_summary),
                runtime_weight_objects.as_deref_mut(),
                terminal_range_owner,
            )?;
            let produced_len = qwen3_dense_0_6b_service_flow_output_len(
                &[],
                &[],
                &[],
                &[],
                &[],
                &logits_descriptors,
                logits_descriptor_tokenizer_path,
                None,
                None,
                None,
                None,
                None,
                &[],
                Some(&range_forward_summary),
            );
            let mut produced = vec![0u8; produced_len];
            qwen3_dense_0_6b_write_service_flow_markers(
                &mut produced,
                0,
                0,
                0,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                &[],
                &[],
                &[],
                &[],
                &[],
                &[],
                &[],
                &logits_descriptors,
                logits_descriptor_tokenizer_path,
                None,
                None,
                None,
                None,
                None,
                &[],
                Some(&range_forward_summary),
            );
            qwen3_stage_timing_mark!("range_forward_only");
            let _ = timing_last;
            return Ok(produced);
        }
    }
    let mut round0_prepare_elapsed = Duration::ZERO;
    let mut round0_dispatch_elapsed = Duration::ZERO;
    let mut round0_post_elapsed = Duration::ZERO;
    let round0_batch_manifest_path =
        simpler_matmul_batch_manifest_path(&manifest_path, round1_dispatch_batch_size)?;
    #[derive(Clone)]
    struct Round0PreparedTile {
        rmsnorm_hidden: Vec<u8>,
        q_projection: Vec<u8>,
        kv_projection: Vec<u8>,
        v_projection: Vec<u8>,
        rope_q_tile: Vec<u8>,
        rope_kv_tile: Vec<u8>,
        attention_score_tile: Vec<u8>,
        attention_softmax_tile: Vec<u8>,
        attention_context_tile: Vec<u8>,
        mlp_activation_tile: Vec<u8>,
    }
    let make_round0_prepared_tile = |shard: Qwen3Dense06bShard,
                                     tile_index: u64|
     -> Round0PreparedTile {
        let prefill_hidden = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
            guest_input,
            real_input_embedding_hidden.as_deref(),
            MATMUL_ELEMS,
            shard,
            tile_index,
        );
        let rmsnorm_hidden =
            qwen3_dense_0_6b_rmsnorm_tile_from_prefill_hidden(&prefill_hidden, MATMUL_DIM, shard);
        let rmsnorm_hidden = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
            &rmsnorm_hidden,
            MATMUL_DIM,
            1,
            shard,
            real_qkv_reference_values.as_ref(),
            real_weight_reference_summary.as_ref(),
        );
        let q_projection = qwen3_dense_0_6b_projection_tile_from_half_input(
            &rmsnorm_hidden,
            MATMUL_DIM,
            Qwen3ProjectionKind::Q,
            shard,
        );
        let q_projection = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
            &q_projection,
            MATMUL_DIM,
            2,
            shard,
            real_qkv_reference_values.as_ref(),
            real_weight_reference_summary.as_ref(),
        );
        let kv_projection = qwen3_dense_0_6b_projection_tile_from_half_input(
            &rmsnorm_hidden,
            MATMUL_DIM,
            Qwen3ProjectionKind::Kv,
            shard,
        );
        let kv_projection = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
            &kv_projection,
            MATMUL_DIM,
            3,
            shard,
            real_qkv_reference_values.as_ref(),
            real_weight_reference_summary.as_ref(),
        );
        let v_projection = qwen3_dense_0_6b_projection_tile_from_half_input(
            &rmsnorm_hidden,
            MATMUL_DIM,
            Qwen3ProjectionKind::V,
            shard,
        );
        let v_projection = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
            &v_projection,
            MATMUL_DIM,
            4,
            shard,
            real_qkv_reference_values.as_ref(),
            real_weight_reference_summary.as_ref(),
        );
        let rope_q_tile = qwen3_dense_0_6b_rope_tile_from_projection(
            &q_projection,
            MATMUL_DIM,
            Qwen3ProjectionKind::Q,
            shard,
        );
        let rope_kv_tile = qwen3_dense_0_6b_rope_tile_from_projection(
            &kv_projection,
            MATMUL_DIM,
            Qwen3ProjectionKind::Kv,
            shard,
        );
        let tile_id = shard.kv_block_start / 2;
        runtime_kvcache_store.borrow_mut().append_projection_rows(
            0,
            tile_id,
            0,
            tile_id * 2 + 2,
            &kv_projection,
            &v_projection,
            MATMUL_DIM,
        );
        let kvcache_payload = runtime_kvcache_store
            .borrow()
            .read_tile_payload(0, tile_id, 0, tile_id * 2 + 2)
            .expect("round0 kvcache store read must follow append");
        let attention_score_tile =
            qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_and_kvcache_reference(
                &rope_q_tile,
                &rope_kv_tile,
                MATMUL_DIM,
                real_weight_reference_summary
                    .as_ref()
                    .map(|summary| (shard, summary)),
                Some(&kvcache_payload),
            );
        let attention_softmax_tile =
            qwen3_dense_0_6b_softmax_tile_from_attention_score(&attention_score_tile, MATMUL_DIM);
        let attention_context_tile =
            qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_and_kvcache_reference(
                &attention_softmax_tile,
                &v_projection,
                MATMUL_DIM,
                real_weight_reference_summary
                    .as_ref()
                    .map(|summary| (shard, summary)),
                Some(&kvcache_payload),
            );
        let mlp_activation_tile = qwen3_dense_0_6b_real_mlp_activation_tile_from_attention_context(
            &attention_context_tile,
            MATMUL_DIM,
            shard,
            real_mlp_reference_summary.as_ref(),
        );
        Round0PreparedTile {
            rmsnorm_hidden,
            q_projection,
            kv_projection,
            v_projection,
            rope_q_tile,
            rope_kv_tile,
            attention_score_tile,
            attention_softmax_tile,
            attention_context_tile,
            mlp_activation_tile,
        }
    };
    let mut round0_prepared_tiles: BTreeMap<u64, Round0PreparedTile> = BTreeMap::new();
    let mut batched_round0_outputs: BTreeMap<u64, Vec<u8>> = BTreeMap::new();
    if let Some(batch_manifest_path) = round0_batch_manifest_path.as_ref() {
        let round0_prepare_started = Instant::now();
        let mut round0_batches_by_target: BTreeMap<u64, Vec<_>> = BTreeMap::new();
        for base_shard in shard_plan.iter().copied() {
            for tile_index in 0..TILES_PER_SHARD {
                let tile_id = base_shard.shard_id * TILES_PER_SHARD + tile_index;
                let shard = Qwen3Dense06bShard {
                    kv_block_start: tile_id * 2,
                    kv_block_end: tile_id * 2 + 2,
                    ..base_shard
                };
                let prepared = make_round0_prepared_tile(shard, tile_index);
                round0_batches_by_target
                    .entry(shard.target_node)
                    .or_default()
                    .push((
                        shard,
                        tile_id,
                        prepared.mlp_activation_tile.clone(),
                        prepared.rope_q_tile.clone(),
                        prepared.rope_kv_tile.clone(),
                    ));
                round0_prepared_tiles.insert(tile_id, prepared);
            }
        }
        if round1_dispatch_batch_size >= tile_count {
            let mut combined_tiles = Vec::with_capacity(tile_count);
            for (_target_node, mut node_tiles) in round0_batches_by_target {
                combined_tiles.append(&mut node_tiles);
            }
            let target_node = combined_tiles
                .first()
                .map(|(shard, _, _, _, _)| shard.target_node)
                .ok_or_else(|| "qwen3_dense_0_6b_empty_round0_combined_batch".to_string())?;
            round0_batches_by_target = BTreeMap::from([(target_node, combined_tiles)]);
        }
        round0_prepare_elapsed += round0_prepare_started.elapsed();
        for (target_node, node_tiles) in round0_batches_by_target {
            if node_tiles.len() % round1_dispatch_batch_size != 0 {
                return Err(format!(
                    "qwen3_dense_0_6b_round0_batch_requires_full_tiles:target_node={target_node}:tiles={}:batch={round1_dispatch_batch_size}",
                    node_tiles.len()
                ));
            }
            for (batch_index, batch) in node_tiles.chunks(round1_dispatch_batch_size).enumerate() {
                let round0_dispatch_started = Instant::now();
                let detail_timing = qwen3_dense_0_6b_dispatch_detail_timing_enabled();
                let detail_pack_ms;
                let detail_seed_ms;
                let detail_backend_spec_ms;
                let detail_request_ms;
                let detail_submit_ms;
                let detail_poll_ms;
                let detail_payload_read_ms;
                let detail_output_split_ms;
                let batch_len = batch.len();
                let batch_base = segment_base
                    + 4_000
                    + target_node.saturating_mul(100)
                    + batch_index as u64 * 10;
                let batch_input_a = SegmentHandle(batch_base + 1);
                let batch_input_q = SegmentHandle(batch_base + 2);
                let batch_input_kv = SegmentHandle(batch_base + 3);
                let batch_output_f = SegmentHandle(batch_base + 4);
                let mut batch_input_a_payload = Vec::with_capacity(input_bytes * batch_len);
                let mut batch_input_q_payload = Vec::with_capacity(input_bytes * batch_len);
                let mut batch_input_kv_payload = Vec::with_capacity(input_bytes * batch_len);
                let first_shard = batch
                    .first()
                    .map(|(shard, _, _, _, _)| *shard)
                    .ok_or_else(|| "qwen3_dense_0_6b_empty_round0_batch".to_string())?;
                let detail_started = Instant::now();
                for (_shard, _tile_id, mlp_activation_tile, rope_q_tile, rope_kv_tile) in batch {
                    batch_input_a_payload.extend_from_slice(mlp_activation_tile);
                    batch_input_q_payload.extend_from_slice(rope_q_tile);
                    batch_input_kv_payload.extend_from_slice(rope_kv_tile);
                }
                detail_pack_ms = detail_started.elapsed().as_millis();
                let detail_started = Instant::now();
                runtime.seed_host_segment(host_node, batch_input_a, batch_input_a_payload);
                runtime.seed_host_segment(host_node, batch_input_q, batch_input_q_payload);
                runtime.seed_host_segment(host_node, batch_input_kv, batch_input_kv_payload);
                runtime.seed_host_segment(
                    host_node,
                    batch_output_f,
                    vec![0u8; output_bytes * batch_len],
                );
                detail_seed_ms = detail_started.elapsed().as_millis();
                let batch_input_bytes = (input_bytes * batch_len) as u64;
                let batch_output_bytes = (output_bytes * batch_len) as u64;
                let detail_started = Instant::now();
                let backend_spec = host_matmul_batched_backend_spec_from_manifest(
                    batch_manifest_path,
                    MemoryEndpoint {
                        node: host_node,
                        segment: batch_input_a,
                        offset: 0,
                    },
                    MemoryEndpoint {
                        node: host_node,
                        segment: batch_input_q,
                        offset: 0,
                    },
                    MemoryEndpoint {
                        node: host_node,
                        segment: batch_input_kv,
                        offset: 0,
                    },
                    MemoryEndpoint {
                        node: host_node,
                        segment: batch_output_f,
                        offset: 0,
                    },
                    batch_input_bytes,
                    batch_output_bytes,
                    MATMUL_ELEMS as u64,
                    batch_len as u64,
                )?;
                detail_backend_spec_ms = detail_started.elapsed().as_millis();
                let detail_started = Instant::now();
                let request = qwen3_dense_0_6b_request(
                    task.task_id + 30_000 + target_node + batch_index as u64,
                    profile,
                    first_shard,
                    vec![
                        opaque_binding(
                            format!("qwen3_dense_0_6b_round0_mlp_activation_batch{batch_index}"),
                            BufferUsage::Input,
                            MemoryEndpoint {
                                node: host_node,
                                segment: batch_input_a,
                                offset: 0,
                            },
                            batch_input_bytes,
                        ),
                        opaque_binding(
                            format!("qwen3_dense_0_6b_round0_rope_q_batch{batch_index}"),
                            BufferUsage::Input,
                            MemoryEndpoint {
                                node: host_node,
                                segment: batch_input_q,
                                offset: 0,
                            },
                            batch_input_bytes,
                        ),
                        opaque_binding(
                            format!("qwen3_dense_0_6b_round0_rope_kv_batch{batch_index}"),
                            BufferUsage::Input,
                            MemoryEndpoint {
                                node: host_node,
                                segment: batch_input_kv,
                                offset: 0,
                            },
                            batch_input_bytes,
                        ),
                        dense_f32_binding(
                            format!("qwen3_dense_0_6b_round0_output_batch{batch_index}"),
                            BufferUsage::Output,
                            MemoryEndpoint {
                                node: host_node,
                                segment: batch_output_f,
                                offset: 0,
                            },
                            (MATMUL_ELEMS * batch_len) as u64,
                        ),
                    ],
                );
                detail_request_ms = detail_started.elapsed().as_millis();
                let dispatch = BackendDispatchOperation {
                    task: TaskKey {
                        task_id: task.task_id + 30_000 + target_node + batch_index as u64,
                        ..task.clone()
                    },
                    function: FunctionLabel {
                        name: format!(
                            "qwen3_dense_0_6b_round0_batched_matmul_node{}_batch{}_tiles{}",
                            target_node, batch_index, batch_len
                        ),
                        level: PlLevel::L2,
                    },
                    backend_spec,
                    request,
                    target_level: PlLevel::L2,
                    target_node,
                    legacy_input_segments: vec![batch_input_a, batch_input_q, batch_input_kv],
                };
                let detail_started = Instant::now();
                with_suppressed_stdio(|| {
                    runtime
                        .submit_backend_dispatch(dispatch, &mut sink)
                        .map_err(|err| err.to_string())
                })?;
                detail_submit_ms = detail_started.elapsed().as_millis();
                let detail_started = Instant::now();
                qwen3_dense_0_6b_poll_simpler_dispatch_batch(
                    &mut runtime,
                    &mut sink,
                    &mut runtime_time,
                    dispatch_latency,
                    1,
                    "round0_multi_tile",
                )?;
                detail_poll_ms = detail_started.elapsed().as_millis();
                let detail_started = Instant::now();
                let batch_output = runtime
                    .host_segment_payload(host_node, batch_output_f)
                    .ok_or_else(|| {
                        format!(
                            "missing_qwen3_dense_0_6b_round0_batch_output_payload:{target_node}"
                        )
                    })?;
                detail_payload_read_ms = detail_started.elapsed().as_millis();
                let detail_started = Instant::now();
                for (local_index, (_shard, tile_id, _, _, _)) in batch.iter().enumerate() {
                    let start = local_index * output_bytes;
                    let end = start + output_bytes;
                    batched_round0_outputs.insert(*tile_id, batch_output[start..end].to_vec());
                }
                detail_output_split_ms = detail_started.elapsed().as_millis();
                round0_dispatch_elapsed += round0_dispatch_started.elapsed();
                if detail_timing {
                    eprintln!(
                        "qwen3-uapi-round0-batch-detail: target_node={target_node} batch_index={batch_index} batch_len={batch_len} total_ms={} pack_ms={} seed_ms={} backend_spec_ms={} request_ms={} submit_ms={} poll_ms={} payload_read_ms={} output_split_ms={}",
                        round0_dispatch_started.elapsed().as_millis(),
                        detail_pack_ms,
                        detail_seed_ms,
                        detail_backend_spec_ms,
                        detail_request_ms,
                        detail_submit_ms,
                        detail_poll_ms,
                        detail_payload_read_ms,
                        detail_output_split_ms
                    );
                }
            }
        }
    }
    for base_shard in shard_plan.iter().copied() {
        for tile_index in 0..TILES_PER_SHARD {
            let round0_prepare_started = Instant::now();
            let tile_id = base_shard.shard_id * TILES_PER_SHARD + tile_index;
            let shard = Qwen3Dense06bShard {
                kv_block_start: tile_id * 2,
                kv_block_end: tile_id * 2 + 2,
                ..base_shard
            };
            let shard_base = segment_base + tile_id.saturating_mul(SEGMENTS_PER_TILE);
            let input_a = SegmentHandle(shard_base + 1);
            let input_q = SegmentHandle(shard_base + 2);
            let input_kv = SegmentHandle(shard_base + 3);
            let input_v = SegmentHandle(shard_base + 4);
            let rope_q = SegmentHandle(shard_base + 5);
            let rope_kv = SegmentHandle(shard_base + 6);
            let attention_score = SegmentHandle(shard_base + 7);
            let attention_softmax = SegmentHandle(shard_base + 8);
            let attention_context = SegmentHandle(shard_base + 9);
            let mlp_activation = SegmentHandle(shard_base + 10);
            let output_f = SegmentHandle(shard_base + 11);
            let mlp_output = SegmentHandle(shard_base + 12);
            let residual_norm = SegmentHandle(shard_base + 13);
            let next_q = SegmentHandle(shard_base + 14);
            let next_kv = SegmentHandle(shard_base + 15);
            let next_v = SegmentHandle(shard_base + 16);
            let next_rope_q = SegmentHandle(shard_base + 17);
            let next_rope_kv = SegmentHandle(shard_base + 18);
            let next_attention_score = SegmentHandle(shard_base + 19);
            let next_attention_softmax = SegmentHandle(shard_base + 20);
            let next_attention_context = SegmentHandle(shard_base + 21);
            let next_partial = SegmentHandle(shard_base + 22);
            let guest_input_digest = qwen3_dense_0_6b_decode_guest_input_checksum(guest_input);
            let prepared = round0_prepared_tiles
                .get(&tile_id)
                .cloned()
                .unwrap_or_else(|| make_round0_prepared_tile(shard, tile_index));
            let Round0PreparedTile {
                rmsnorm_hidden,
                q_projection,
                kv_projection,
                v_projection,
                rope_q_tile,
                rope_kv_tile,
                attention_score_tile,
                attention_softmax_tile,
                attention_context_tile,
                mlp_activation_tile,
            } = prepared;

            runtime.seed_host_segment(host_node, input_a, rmsnorm_hidden.clone());
            runtime.seed_host_segment(host_node, input_q, q_projection.clone());
            runtime.seed_host_segment(host_node, input_kv, kv_projection.clone());
            runtime.seed_host_segment(host_node, input_v, v_projection.clone());
            runtime.seed_host_segment(host_node, rope_q, rope_q_tile.clone());
            runtime.seed_host_segment(host_node, rope_kv, rope_kv_tile.clone());
            runtime.seed_host_segment(host_node, attention_score, attention_score_tile.clone());
            runtime.seed_host_segment(host_node, attention_softmax, attention_softmax_tile.clone());
            runtime.seed_host_segment(host_node, attention_context, attention_context_tile.clone());
            runtime.seed_host_segment(host_node, mlp_activation, mlp_activation_tile.clone());
            if let Some(batched_output) = batched_round0_outputs.get(&tile_id) {
                runtime.seed_host_segment(host_node, output_f, batched_output.clone());
            } else {
                runtime.seed_host_segment(host_node, output_f, vec![0u8; output_bytes]);
            }
            runtime.seed_host_segment(host_node, mlp_output, vec![0u8; output_bytes]);
            runtime.seed_host_segment(host_node, residual_norm, vec![0u8; output_bytes]);
            runtime.seed_host_segment(host_node, next_q, vec![0u8; input_bytes]);
            runtime.seed_host_segment(host_node, next_kv, vec![0u8; input_bytes]);
            runtime.seed_host_segment(host_node, next_v, vec![0u8; input_bytes]);
            runtime.seed_host_segment(host_node, next_rope_q, vec![0u8; input_bytes]);
            runtime.seed_host_segment(host_node, next_rope_kv, vec![0u8; input_bytes]);
            runtime.seed_host_segment(host_node, next_attention_score, vec![0u8; output_bytes]);
            runtime.seed_host_segment(host_node, next_attention_softmax, vec![0u8; output_bytes]);
            runtime.seed_host_segment(host_node, next_attention_context, vec![0u8; input_bytes]);
            runtime.seed_host_segment(host_node, next_partial, vec![0u8; output_bytes]);
            projection_descriptors.push(qwen3_dense_0_6b_projection_descriptor(
                shard,
                1,
                input_q,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                &q_projection,
            ));
            projection_descriptors.push(qwen3_dense_0_6b_projection_descriptor(
                shard,
                2,
                input_kv,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                &kv_projection,
            ));
            projection_descriptors.push(qwen3_dense_0_6b_projection_descriptor(
                shard,
                3,
                input_v,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                &v_projection,
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                1,
                0,
                shard.shard_id,
                input_a,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&rmsnorm_hidden),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                2,
                1,
                shard.shard_id,
                input_q,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&q_projection),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                3,
                1,
                shard.shard_id,
                input_kv,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&kv_projection),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                4,
                1,
                shard.shard_id,
                input_v,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&v_projection),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                5,
                2,
                shard.shard_id,
                rope_q,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&rope_q_tile),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                6,
                3,
                shard.shard_id,
                rope_kv,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&rope_kv_tile),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                7,
                5,
                shard.shard_id,
                attention_score,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&attention_score_tile),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                8,
                7,
                shard.shard_id,
                attention_softmax,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&attention_softmax_tile),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                9,
                8,
                shard.shard_id,
                attention_context,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&attention_context_tile),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                10,
                9,
                shard.shard_id,
                mlp_activation,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&mlp_activation_tile),
            ));

            let backend_spec = host_matmul_backend_spec_from_manifest(
                &manifest_path,
                MemoryEndpoint {
                    node: host_node,
                    segment: mlp_activation,
                    offset: 0,
                },
                MemoryEndpoint {
                    node: host_node,
                    segment: rope_q,
                    offset: 0,
                },
                MemoryEndpoint {
                    node: host_node,
                    segment: rope_kv,
                    offset: 0,
                },
                MemoryEndpoint {
                    node: host_node,
                    segment: output_f,
                    offset: 0,
                },
                input_bytes as u64,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
            )?;
            let request = qwen3_dense_0_6b_request(
                task.task_id,
                profile,
                shard,
                vec![
                    opaque_binding(
                        "qwen3_dense_0_6b_layer0_mlp_activation_tile_half",
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: mlp_activation,
                            offset: 0,
                        },
                        input_bytes as u64,
                    ),
                    opaque_resident_binding(
                        format!("qwen3_dense_0_6b_layer0_attention_context_tile_half_{tile_id}"),
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: attention_context,
                            offset: 0,
                        },
                        input_bytes as u64,
                    ),
                    opaque_resident_binding(
                        format!("qwen3_dense_0_6b_layer0_rmsnorm_hidden_half_{tile_id}"),
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: input_a,
                            offset: 0,
                        },
                        input_bytes as u64,
                    ),
                    opaque_binding(
                        "qwen3_dense_0_6b_layer0_rope_q_tile_half",
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: rope_q,
                            offset: 0,
                        },
                        input_bytes as u64,
                    ),
                    opaque_binding(
                        "qwen3_dense_0_6b_layer0_rope_kv_tile_half",
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: rope_kv,
                            offset: 0,
                        },
                        input_bytes as u64,
                    ),
                    opaque_resident_binding(
                        format!("qwen3_dense_0_6b_layer0_v_proj_tile_half_{tile_id}"),
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: input_v,
                            offset: 0,
                        },
                        input_bytes as u64,
                    ),
                    opaque_resident_binding(
                        format!("qwen3_dense_0_6b_layer0_attention_score_tile_f32_{tile_id}"),
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: attention_score,
                            offset: 0,
                        },
                        output_bytes as u64,
                    ),
                    opaque_resident_binding(
                        format!("qwen3_dense_0_6b_layer0_attention_softmax_tile_f32_{tile_id}"),
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: attention_softmax,
                            offset: 0,
                        },
                        output_bytes as u64,
                    ),
                    dense_f32_binding(
                        "qwen3_dense_0_6b_layer0_prefill_projection_f",
                        BufferUsage::Output,
                        MemoryEndpoint {
                            node: host_node,
                            segment: output_f,
                            offset: 0,
                        },
                        MATMUL_ELEMS as u64,
                    ),
                    opaque_resident_binding(
                        "qwen3_dense_0_6b_model_config",
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: model_meta,
                            offset: 0,
                        },
                        96,
                    ),
                    opaque_resident_binding(
                        "qwen3_dense_0_6b_kvcache_layout",
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: kv_layout,
                            offset: 0,
                        },
                        96,
                    ),
                    opaque_resident_binding(
                        "qwen3_dense_0_6b_guest_input_payload",
                        BufferUsage::Input,
                        MemoryEndpoint {
                            node: host_node,
                            segment: guest_input_payload,
                            offset: 0,
                        },
                        W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES as u64,
                    ),
                ],
            );
            let dispatch = BackendDispatchOperation {
                task: TaskKey {
                    task_id: task.task_id + tile_id,
                    ..task.clone()
                },
                function: FunctionLabel {
                    name: format!(
                        "qwen3_dense_0_6b_prefill_matmul_shard{}_tile{}",
                        shard.shard_id, tile_index
                    ),
                    level: PlLevel::L2,
                },
                backend_spec,
                request,
                target_level: PlLevel::L2,
                target_node: shard.target_node,
                legacy_input_segments: vec![
                    input_a,
                    input_q,
                    input_kv,
                    input_v,
                    rope_q,
                    rope_kv,
                    attention_score,
                    attention_softmax,
                    attention_context,
                    mlp_activation,
                ],
            };
            round0_prepare_elapsed += round0_prepare_started.elapsed();
            if !batched_round0_outputs.contains_key(&tile_id) {
                let round0_dispatch_started = Instant::now();
                with_suppressed_stdio(|| {
                    runtime
                        .submit_backend_dispatch(dispatch, &mut sink)
                        .map_err(|err| err.to_string())?;
                    runtime_time += dispatch_latency;
                    runtime.advance_to(runtime_time, &mut sink);
                    let completions = runtime.poll_completions(runtime_time, &mut sink);
                    if completions.len() != 1 {
                        return Err(format!(
                    "simpler_capi_qwen3_dense_0_6b_shard_completion_count_mismatch:shard={}:got={}:expected=1",
                    shard.shard_id,
                    completions.len()
                ));
                    }
                    match &completions[0].status {
                        CompletionStatus::Success => Ok(()),
                        other => Err(format!(
                    "simpler_capi_qwen3_dense_0_6b_shard_dispatch_failed:shard={}:status={other:?}",
                    shard.shard_id
                )),
                    }
                })?;
                round0_dispatch_elapsed += round0_dispatch_started.elapsed();
            }
            let round0_post_started = Instant::now();

            let shard_output = runtime
                .host_segment_payload(host_node, output_f)
                .ok_or_else(|| {
                    format!(
                        "missing_qwen3_dense_0_6b_shard_output_payload:{}",
                        shard.shard_id
                    )
                })?;
            let output_values = bytes_to_f32s(shard_output);
            if output_values.len() != MATMUL_ELEMS {
                return Err(format!(
                    "qwen3_dense_0_6b_shard_output_len_mismatch:shard={}:got={}:expected={}",
                    shard.shard_id,
                    output_values.len(),
                    MATMUL_ELEMS
                ));
            }
            if output_values
                .iter()
                .any(|value| !value.is_finite() || *value <= 0.0)
            {
                return Err(format!(
                    "qwen3_dense_0_6b_shard_output_invalid:shard={}:first={:?}",
                    shard.shard_id,
                    output_values.first()
                ));
            }
            let checksum = qwen3_dense_0_6b_shard_output_checksum(shard_output);
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                11,
                10,
                shard.shard_id,
                output_f,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                checksum,
            ));
            let mlp_output_tile =
                qwen3_dense_0_6b_mlp_output_tile_from_intermediate(shard_output, MATMUL_DIM, shard);
            let mlp_output_tile = qwen3_dense_0_6b_tile_with_real_mlp_reference_mix(
                &mlp_output_tile,
                MATMUL_DIM,
                12,
                shard,
                real_mlp_reference_summary.as_ref(),
            );
            runtime.seed_host_segment(host_node, mlp_output, mlp_output_tile.clone());
            let mlp_output_checksum = qwen3_dense_0_6b_shard_output_checksum(&mlp_output_tile);
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                12,
                11,
                shard.shard_id,
                mlp_output,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                mlp_output_checksum,
            ));
            let residual_norm_tile = qwen3_dense_0_6b_residual_rmsnorm_tile_from_attention_and_mlp(
                &attention_context_tile,
                &mlp_output_tile,
                MATMUL_DIM,
                shard,
            );
            let residual_norm_tile = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
                &residual_norm_tile,
                MATMUL_DIM,
                1,
                shard,
                next_layer_real_qkv_reference_values.as_ref(),
                next_layer_real_weight_reference_summary.as_ref(),
            );
            runtime.seed_host_segment(host_node, residual_norm, residual_norm_tile.clone());
            let residual_norm_checksum =
                qwen3_dense_0_6b_shard_output_checksum(&residual_norm_tile);
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                13,
                12,
                shard.shard_id,
                residual_norm,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                residual_norm_checksum,
            ));
            let residual_norm_half =
                qwen3_dense_0_6b_f32_tile_to_half_input(&residual_norm_tile, MATMUL_ELEMS);
            let next_q_projection = qwen3_dense_0_6b_projection_tile_from_half_input(
                &residual_norm_half,
                MATMUL_DIM,
                Qwen3ProjectionKind::Q,
                shard,
            );
            let next_q_projection = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
                &next_q_projection,
                MATMUL_DIM,
                2,
                shard,
                next_layer_real_qkv_reference_values.as_ref(),
                next_layer_real_weight_reference_summary.as_ref(),
            );
            let next_kv_projection = qwen3_dense_0_6b_projection_tile_from_half_input(
                &residual_norm_half,
                MATMUL_DIM,
                Qwen3ProjectionKind::Kv,
                shard,
            );
            let next_kv_projection = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
                &next_kv_projection,
                MATMUL_DIM,
                3,
                shard,
                next_layer_real_qkv_reference_values.as_ref(),
                next_layer_real_weight_reference_summary.as_ref(),
            );
            let next_v_projection = qwen3_dense_0_6b_projection_tile_from_half_input(
                &residual_norm_half,
                MATMUL_DIM,
                Qwen3ProjectionKind::V,
                shard,
            );
            let next_v_projection = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
                &next_v_projection,
                MATMUL_DIM,
                4,
                shard,
                next_layer_real_qkv_reference_values.as_ref(),
                next_layer_real_weight_reference_summary.as_ref(),
            );
            runtime.seed_host_segment(host_node, next_q, next_q_projection.clone());
            runtime.seed_host_segment(host_node, next_kv, next_kv_projection.clone());
            runtime.seed_host_segment(host_node, next_v, next_v_projection.clone());
            for layer_id in 0..profile.num_hidden_layers {
                let layer_position_base =
                    layer_id * tile_count as u64 * (shard.kv_block_end - shard.kv_block_start + 1);
                let prefill_start = layer_position_base + tile_id * 2;
                let prefill_end = prefill_start + 2;
                let decode_start = layer_position_base + tile_count as u64 * 2 + tile_id;
                let decode_end = decode_start + 1;
                let (prefill_k_segment, prefill_v_segment, prefill_k_payload, prefill_v_payload) =
                    if layer_id == 0 {
                        (
                            input_kv,
                            input_v,
                            kv_projection.as_slice(),
                            v_projection.as_slice(),
                        )
                    } else {
                        (
                            next_kv,
                            next_v,
                            next_kv_projection.as_slice(),
                            next_v_projection.as_slice(),
                        )
                    };

                kvcache_descriptors.push(qwen3_dense_0_6b_kvcache_descriptor(
                    shard,
                    layer_id,
                    tile_id,
                    prefill_start,
                    prefill_end,
                    layer_position_base,
                    prefill_end,
                    kvcache_update_seq,
                    prefill_k_segment,
                    prefill_v_segment,
                    prefill_k_payload,
                    prefill_v_payload,
                ));
                kvcache_update_seq += 1;
                kvcache_descriptors.push(qwen3_dense_0_6b_kvcache_descriptor(
                    shard,
                    layer_id,
                    tile_id,
                    decode_start,
                    decode_end,
                    layer_position_base,
                    decode_end,
                    kvcache_update_seq,
                    next_kv,
                    next_v,
                    &next_kv_projection,
                    &next_v_projection,
                ));
                kvcache_update_seq += 1;
            }
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                14,
                13,
                shard.shard_id,
                next_q,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&next_q_projection),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                15,
                13,
                shard.shard_id,
                next_kv,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&next_kv_projection),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                16,
                13,
                shard.shard_id,
                next_v,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&next_v_projection),
            ));
            let next_rope_q_tile = qwen3_dense_0_6b_rope_tile_from_projection(
                &next_q_projection,
                MATMUL_DIM,
                Qwen3ProjectionKind::Q,
                shard,
            );
            let next_rope_kv_tile = qwen3_dense_0_6b_rope_tile_from_projection(
                &next_kv_projection,
                MATMUL_DIM,
                Qwen3ProjectionKind::Kv,
                shard,
            );
            let next_layer_position_base =
                tile_count as u64 * (shard.kv_block_end - shard.kv_block_start + 1);
            let next_decode_end = next_layer_position_base + tile_count as u64 * 2 + tile_id + 1;
            runtime_kvcache_store.borrow_mut().append_projection_rows(
                1,
                tile_id,
                next_layer_position_base,
                next_decode_end,
                &next_kv_projection,
                &next_v_projection,
                MATMUL_DIM,
            );
            let next_kvcache_payload = runtime_kvcache_store
                .borrow()
                .read_tile_payload(1, tile_id, next_layer_position_base, next_decode_end)
                .expect("next-layer kvcache store read must follow append");
            let next_attention_score_tile =
                qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_and_kvcache_reference(
                    &next_rope_q_tile,
                    &next_rope_kv_tile,
                    MATMUL_DIM,
                    next_layer_real_weight_reference_summary
                        .as_ref()
                        .map(|summary| (shard, summary)),
                    Some(&next_kvcache_payload),
                );
            let next_attention_softmax_tile = qwen3_dense_0_6b_softmax_tile_from_attention_score(
                &next_attention_score_tile,
                MATMUL_DIM,
            );
            let next_attention_context_tile =
                qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_and_kvcache_reference(
                    &next_attention_softmax_tile,
                    &next_v_projection,
                    MATMUL_DIM,
                    next_layer_real_weight_reference_summary
                        .as_ref()
                        .map(|summary| (shard, summary)),
                    Some(&next_kvcache_payload),
                );
            runtime.seed_host_segment(host_node, next_rope_q, next_rope_q_tile.clone());
            runtime.seed_host_segment(host_node, next_rope_kv, next_rope_kv_tile.clone());
            runtime.seed_host_segment(
                host_node,
                next_attention_score,
                next_attention_score_tile.clone(),
            );
            runtime.seed_host_segment(
                host_node,
                next_attention_softmax,
                next_attention_softmax_tile.clone(),
            );
            runtime.seed_host_segment(
                host_node,
                next_attention_context,
                next_attention_context_tile.clone(),
            );
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                17,
                14,
                shard.shard_id,
                next_rope_q,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&next_rope_q_tile),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                18,
                15,
                shard.shard_id,
                next_rope_kv,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&next_rope_kv_tile),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                19,
                17,
                shard.shard_id,
                next_attention_score,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&next_attention_score_tile),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                20,
                19,
                shard.shard_id,
                next_attention_softmax,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&next_attention_softmax_tile),
            ));
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                21,
                20,
                shard.shard_id,
                next_attention_context,
                input_bytes as u64,
                MATMUL_ELEMS as u64,
                qwen3_dense_0_6b_shard_output_checksum(&next_attention_context_tile),
            ));
            let next_partial_tile = qwen3_dense_0_6b_next_partial_tile_from_attention_context(
                &next_attention_context_tile,
                MATMUL_DIM,
                shard,
                guest_input_digest,
            );
            let next_partial_tile = qwen3_dense_0_6b_tile_with_real_mlp_reference_mix(
                &next_partial_tile,
                MATMUL_DIM,
                12,
                shard,
                next_layer_real_mlp_reference_summary.as_ref(),
            );
            runtime.seed_host_segment(host_node, next_partial, next_partial_tile.clone());
            let next_partial_checksum = qwen3_dense_0_6b_shard_output_checksum(&next_partial_tile);
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                shard,
                22,
                21,
                shard.shard_id,
                next_partial,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                next_partial_checksum,
            ));
            let publish_stats = weights_service
                .submit_publish_object(
                    ServiceObjectPublishReq {
                        task: Some(TaskKey {
                            task_id: task.task_id + tile_id,
                            ..task.clone()
                        }),
                        requester_entity: tile_id as u32,
                        metadata_puts: vec![ServiceObjectMetadataPut {
                            key: qwen3_dense_0_6b_partial_result_key(0, shard.shard_id, tile_index),
                            object_kind: ServiceObjectKind::PartialResultTile,
                            bytes: 192,
                        }],
                        payload_writes: vec![ServiceObjectPayloadWrite {
                            storage_ref: qwen3_dense_0_6b_partial_result_storage_ref(
                                0,
                                shard.shard_id,
                                tile_index,
                            ),
                            object_kind: ServiceObjectKind::PartialResultTile,
                            storage_kind: WeightStorageKind::Block,
                            segment: next_partial,
                            offset: 0,
                            bytes: output_bytes as u64,
                            checksum: next_partial_checksum,
                            producer_entity: tile_id as u32,
                        }],
                    },
                    runtime_time,
                )
                .map_err(|err| format!("qwen3_dense_0_6b_partial_result_publish_failed:{err:?}"))?;
            if publish_stats.metadata_puts != 1
                || publish_stats.block_writes != 1
                || publish_stats.payload_bytes != output_bytes as u64
            {
                return Err(format!(
                "qwen3_dense_0_6b_partial_result_publish_stats_invalid:shard={}:stats={publish_stats:?}",
                shard.shard_id
            ));
            }
            tile_checksums.push(next_partial_checksum);
            round0_outputs.push((
                shard,
                next_partial_tile.clone(),
                next_partial,
                next_partial_checksum,
            ));
            round0_post_elapsed += round0_post_started.elapsed();
        }
    }
    if timing_enabled {
        eprintln!(
            "qwen3-stage-timing: stage=round0_prepare total_ms={}",
            round0_prepare_elapsed.as_millis()
        );
        eprintln!(
            "qwen3-stage-timing: stage=round0_dispatch total_ms={}",
            round0_dispatch_elapsed.as_millis()
        );
        eprintln!(
            "qwen3-stage-timing: stage=round0_post total_ms={}",
            round0_post_elapsed.as_millis()
        );
    }
    qwen3_stage_timing_mark!("round0_total");
    if tile_checksums.len() != tile_count {
        return Err(format!(
            "qwen3_dense_0_6b_tile_summary_count_mismatch:got={}:expected={}",
            tile_checksums.len(),
            tile_count
        ));
    }
    if !tile_checksums.windows(2).any(|pair| pair[0] != pair[1]) {
        return Err(format!(
            "qwen3_dense_0_6b_tile_outputs_not_distinct:tiles={}:checksum={:#x}",
            tile_checksums.len(),
            tile_checksums.first().copied().unwrap_or(0)
        ));
    }
    let range_single_phase_forward = range_compute_contract.is_some();
    let mut round1_checksums = Vec::with_capacity(tile_count);
    if range_single_phase_forward {
        round1_outputs = round0_outputs.clone();
        round1_checksums.extend(tile_checksums.iter().copied());
        for (_shard, output, _segment, _checksum) in &round1_outputs {
            produced.extend_from_slice(output);
        }
        if timing_enabled {
            eprintln!("qwen3-stage-timing: stage=range_single_phase_output total_ms=0");
            eprintln!("qwen3-stage-timing: stage=round1_prepare total_ms=0");
            eprintln!("qwen3-stage-timing: stage=round1_dispatch total_ms=0");
            eprintln!("qwen3-stage-timing: stage=round1_assemble total_ms=0");
        }
        qwen3_stage_timing_mark!("range_single_phase_output");
    } else {
        #[derive(Clone, Copy)]
        struct Round1DispatchContext {
            shard: Qwen3Dense06bShard,
            remote_shard: Qwen3Dense06bShard,
            remote_checksum: u64,
            output_f: SegmentHandle,
            dispatch_output_f: SegmentHandle,
            dispatch_output_offset: usize,
        }

        let mut round1_dispatch_contexts = Vec::with_capacity(round0_outputs.len());
        let round1_batch_manifest_path =
            simpler_matmul_batch_manifest_path(&manifest_path, round1_dispatch_batch_size)?;
        let mut round1_prepare_elapsed = Duration::ZERO;
        let mut round1_dispatch_elapsed = Duration::ZERO;
        if let Some(batch_manifest_path) = round1_batch_manifest_path.as_ref() {
            for (batch_index, batch) in round0_outputs
                .chunks(round1_dispatch_batch_size)
                .enumerate()
            {
                let round1_prepare_started = Instant::now();
                let batch_len = batch.len();
                let batch_base = segment_base + 5_000 + batch_index as u64 * 10;
                let batch_input_a = SegmentHandle(batch_base + 1);
                let batch_input_q = SegmentHandle(batch_base + 2);
                let batch_input_kv = SegmentHandle(batch_base + 3);
                let batch_output_f = SegmentHandle(batch_base + 4);
                let mut batch_input_a_payload = Vec::with_capacity(input_bytes * batch_len);
                let mut batch_input_q_payload = Vec::with_capacity(input_bytes * batch_len);
                let mut batch_input_kv_payload = Vec::with_capacity(input_bytes * batch_len);
                let first_shard = batch
                    .first()
                    .map(|(shard, _, _, _)| *shard)
                    .ok_or_else(|| "qwen3_dense_0_6b_empty_round1_batch".to_string())?;
                for (local_index, (shard, _round0_output, _round0_segment, _round0_checksum)) in
                    batch.iter().enumerate()
                {
                    let index = batch_index * round1_dispatch_batch_size + local_index;
                    let remote_index = (index + round0_outputs.len() - 1) % round0_outputs.len();
                    let (remote_shard, remote_output, _remote_segment, remote_checksum) =
                        &round0_outputs[remote_index];
                    let tile_id = shard.kv_block_start / 2;
                    let remote_tile_id = remote_shard.kv_block_start / 2;
                    let remote_tile_index = remote_tile_id % TILES_PER_SHARD;
                    let resolve_segment = SegmentHandle(segment_base + 900 + remote_tile_id);
                    let resolve_stats = weights_service
                        .submit_resolve_object(
                            ServiceObjectResolveReq {
                                task: Some(TaskKey {
                                    task_id: task.task_id + 10_000 + tile_id,
                                    ..task.clone()
                                }),
                                requester_entity: tile_id as u32,
                                metadata_key: qwen3_dense_0_6b_partial_result_key(
                                    0,
                                    remote_shard.shard_id,
                                    remote_tile_index,
                                ),
                                object_kind: ServiceObjectKind::PartialResultTile,
                                storage_ref: qwen3_dense_0_6b_partial_result_storage_ref(
                                    0,
                                    remote_shard.shard_id,
                                    remote_tile_index,
                                ),
                                storage_kind: WeightStorageKind::Block,
                                segment: resolve_segment,
                                bytes: output_bytes as u64,
                            },
                            runtime_time,
                        )
                        .map_err(|err| {
                            format!("qwen3_dense_0_6b_partial_result_resolve_failed:{err:?}")
                        })?;
                    if resolve_stats.metadata_gets != 1 || resolve_stats.block_reads != 1 {
                        return Err(format!(
                        "qwen3_dense_0_6b_partial_result_resolve_stats_invalid:shard={}:remote_shard={}:stats={resolve_stats:?}",
                        shard.shard_id, remote_shard.shard_id
                    ));
                    }
                    let shard_base = segment_base + 2_000 + tile_id.saturating_mul(10);
                    let output_f = SegmentHandle(shard_base + 4);
                    layer_dependency_descriptors.push(
                        qwen3_dense_0_6b_layer_dependency_descriptor(
                            *shard,
                            23,
                            22,
                            remote_shard.shard_id,
                            resolve_segment,
                            output_bytes as u64,
                            MATMUL_ELEMS as u64,
                            *remote_checksum,
                        ),
                    );
                    let remote_half =
                        qwen3_dense_0_6b_remote_partial_to_half_input(remote_output, MATMUL_ELEMS);
                    let q_projection = qwen3_dense_0_6b_projection_tile_from_half_input(
                        &remote_half,
                        MATMUL_DIM,
                        Qwen3ProjectionKind::Q,
                        *shard,
                    );
                    let kv_projection = qwen3_dense_0_6b_projection_tile_from_half_input(
                        &remote_half,
                        MATMUL_DIM,
                        Qwen3ProjectionKind::Kv,
                        *shard,
                    );
                    batch_input_a_payload.extend_from_slice(&remote_half);
                    batch_input_q_payload.extend_from_slice(&q_projection);
                    batch_input_kv_payload.extend_from_slice(&kv_projection);
                    runtime.seed_host_segment(host_node, output_f, vec![0u8; output_bytes]);
                    round1_dispatch_contexts.push(Round1DispatchContext {
                        shard: *shard,
                        remote_shard: *remote_shard,
                        remote_checksum: *remote_checksum,
                        output_f,
                        dispatch_output_f: batch_output_f,
                        dispatch_output_offset: local_index * output_bytes,
                    });
                }
                runtime.seed_host_segment(host_node, batch_input_a, batch_input_a_payload);
                runtime.seed_host_segment(host_node, batch_input_q, batch_input_q_payload);
                runtime.seed_host_segment(host_node, batch_input_kv, batch_input_kv_payload);
                runtime.seed_host_segment(
                    host_node,
                    batch_output_f,
                    vec![0u8; output_bytes * batch_len],
                );
                let batch_input_bytes = (input_bytes * batch_len) as u64;
                let batch_output_bytes = (output_bytes * batch_len) as u64;
                let backend_spec = host_matmul_batched_backend_spec_from_manifest(
                    batch_manifest_path,
                    MemoryEndpoint {
                        node: host_node,
                        segment: batch_input_a,
                        offset: 0,
                    },
                    MemoryEndpoint {
                        node: host_node,
                        segment: batch_input_q,
                        offset: 0,
                    },
                    MemoryEndpoint {
                        node: host_node,
                        segment: batch_input_kv,
                        offset: 0,
                    },
                    MemoryEndpoint {
                        node: host_node,
                        segment: batch_output_f,
                        offset: 0,
                    },
                    batch_input_bytes,
                    batch_output_bytes,
                    MATMUL_ELEMS as u64,
                    batch_len as u64,
                )?;
                let request = qwen3_dense_0_6b_request(
                    task.task_id + 20_000 + batch_index as u64,
                    profile,
                    first_shard,
                    vec![
                        opaque_binding(
                            format!("qwen3_dense_0_6b_round1_remote_partial_batch{batch_index}"),
                            BufferUsage::Input,
                            MemoryEndpoint {
                                node: host_node,
                                segment: batch_input_a,
                                offset: 0,
                            },
                            batch_input_bytes,
                        ),
                        opaque_binding(
                            format!("qwen3_dense_0_6b_round1_q_proj_batch{batch_index}"),
                            BufferUsage::Input,
                            MemoryEndpoint {
                                node: host_node,
                                segment: batch_input_q,
                                offset: 0,
                            },
                            batch_input_bytes,
                        ),
                        opaque_binding(
                            format!("qwen3_dense_0_6b_round1_kv_proj_batch{batch_index}"),
                            BufferUsage::Input,
                            MemoryEndpoint {
                                node: host_node,
                                segment: batch_input_kv,
                                offset: 0,
                            },
                            batch_input_bytes,
                        ),
                        dense_f32_binding(
                            format!(
                            "qwen3_dense_0_6b_round1_remote_dependent_output_batch{batch_index}"
                        ),
                            BufferUsage::Output,
                            MemoryEndpoint {
                                node: host_node,
                                segment: batch_output_f,
                                offset: 0,
                            },
                            (MATMUL_ELEMS * batch_len) as u64,
                        ),
                    ],
                );
                let dispatch = BackendDispatchOperation {
                    task: TaskKey {
                        task_id: task.task_id + 20_000 + batch_index as u64,
                        ..task.clone()
                    },
                    function: FunctionLabel {
                        name: format!(
                            "qwen3_dense_0_6b_round1_remote_dependent_matmul_batch{}_tiles{}",
                            batch_index, batch_len
                        ),
                        level: PlLevel::L2,
                    },
                    backend_spec,
                    request,
                    target_level: PlLevel::L2,
                    target_node: first_shard.target_node,
                    legacy_input_segments: vec![batch_input_a, batch_input_q, batch_input_kv],
                };
                round1_prepare_elapsed += round1_prepare_started.elapsed();
                let round1_dispatch_started = Instant::now();
                with_suppressed_stdio(|| {
                    runtime
                        .submit_backend_dispatch(dispatch, &mut sink)
                        .map_err(|err| err.to_string())
                })?;
                qwen3_dense_0_6b_poll_simpler_dispatch_batch(
                    &mut runtime,
                    &mut sink,
                    &mut runtime_time,
                    dispatch_latency,
                    1,
                    "round1_multi_tile",
                )?;
                round1_dispatch_elapsed += round1_dispatch_started.elapsed();
            }
        } else {
            let mut pending_round1_dispatches = 0usize;
            for (index, (shard, _round0_output, _round0_segment, _round0_checksum)) in
                round0_outputs.iter().enumerate()
            {
                let round1_prepare_started = Instant::now();
                let remote_index = (index + round0_outputs.len() - 1) % round0_outputs.len();
                let (remote_shard, remote_output, _remote_segment, remote_checksum) =
                    &round0_outputs[remote_index];
                let tile_id = shard.kv_block_start / 2;
                let remote_tile_id = remote_shard.kv_block_start / 2;
                let remote_tile_index = remote_tile_id % TILES_PER_SHARD;
                let resolve_segment = SegmentHandle(segment_base + 900 + remote_tile_id);
                let resolve_stats = weights_service
                    .submit_resolve_object(
                        ServiceObjectResolveReq {
                            task: Some(TaskKey {
                                task_id: task.task_id + 10_000 + tile_id,
                                ..task.clone()
                            }),
                            requester_entity: tile_id as u32,
                            metadata_key: qwen3_dense_0_6b_partial_result_key(
                                0,
                                remote_shard.shard_id,
                                remote_tile_index,
                            ),
                            object_kind: ServiceObjectKind::PartialResultTile,
                            storage_ref: qwen3_dense_0_6b_partial_result_storage_ref(
                                0,
                                remote_shard.shard_id,
                                remote_tile_index,
                            ),
                            storage_kind: WeightStorageKind::Block,
                            segment: resolve_segment,
                            bytes: output_bytes as u64,
                        },
                        runtime_time,
                    )
                    .map_err(|err| {
                        format!("qwen3_dense_0_6b_partial_result_resolve_failed:{err:?}")
                    })?;
                if resolve_stats.metadata_gets != 1 || resolve_stats.block_reads != 1 {
                    return Err(format!(
                    "qwen3_dense_0_6b_partial_result_resolve_stats_invalid:shard={}:remote_shard={}:stats={resolve_stats:?}",
                    shard.shard_id, remote_shard.shard_id
                ));
                }

                let shard_base = segment_base + 2_000 + tile_id.saturating_mul(10);
                let input_a = SegmentHandle(shard_base + 1);
                let input_q = SegmentHandle(shard_base + 2);
                let input_kv = SegmentHandle(shard_base + 3);
                let output_f = SegmentHandle(shard_base + 4);
                layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                    *shard,
                    23,
                    22,
                    remote_shard.shard_id,
                    resolve_segment,
                    output_bytes as u64,
                    MATMUL_ELEMS as u64,
                    *remote_checksum,
                ));
                let remote_half =
                    qwen3_dense_0_6b_remote_partial_to_half_input(remote_output, MATMUL_ELEMS);
                runtime.seed_host_segment(host_node, input_a, remote_half.clone());
                runtime.seed_host_segment(
                    host_node,
                    input_q,
                    qwen3_dense_0_6b_projection_tile_from_half_input(
                        &remote_half,
                        MATMUL_DIM,
                        Qwen3ProjectionKind::Q,
                        *shard,
                    ),
                );
                runtime.seed_host_segment(
                    host_node,
                    input_kv,
                    qwen3_dense_0_6b_projection_tile_from_half_input(
                        &remote_half,
                        MATMUL_DIM,
                        Qwen3ProjectionKind::Kv,
                        *shard,
                    ),
                );
                runtime.seed_host_segment(host_node, output_f, vec![0u8; output_bytes]);
                let backend_spec = host_matmul_backend_spec_from_manifest(
                    &manifest_path,
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_a,
                        offset: 0,
                    },
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_q,
                        offset: 0,
                    },
                    MemoryEndpoint {
                        node: host_node,
                        segment: input_kv,
                        offset: 0,
                    },
                    MemoryEndpoint {
                        node: host_node,
                        segment: output_f,
                        offset: 0,
                    },
                    input_bytes as u64,
                    output_bytes as u64,
                    MATMUL_ELEMS as u64,
                )?;
                let request = qwen3_dense_0_6b_request(
                    task.task_id + 20_000 + tile_id,
                    profile,
                    *shard,
                    vec![
                        opaque_binding(
                            format!(
                                "qwen3_dense_0_6b_round1_remote_partial_from_shard{}",
                                remote_shard.shard_id
                            ),
                            BufferUsage::Input,
                            MemoryEndpoint {
                                node: host_node,
                                segment: input_a,
                                offset: 0,
                            },
                            input_bytes as u64,
                        ),
                        opaque_binding(
                            "qwen3_dense_0_6b_round1_q_proj_tile_half",
                            BufferUsage::Input,
                            MemoryEndpoint {
                                node: host_node,
                                segment: input_q,
                                offset: 0,
                            },
                            input_bytes as u64,
                        ),
                        opaque_binding(
                            "qwen3_dense_0_6b_round1_kv_proj_tile_half",
                            BufferUsage::Input,
                            MemoryEndpoint {
                                node: host_node,
                                segment: input_kv,
                                offset: 0,
                            },
                            input_bytes as u64,
                        ),
                        dense_f32_binding(
                            "qwen3_dense_0_6b_round1_remote_dependent_output_f",
                            BufferUsage::Output,
                            MemoryEndpoint {
                                node: host_node,
                                segment: output_f,
                                offset: 0,
                            },
                            MATMUL_ELEMS as u64,
                        ),
                    ],
                );
                let dispatch = BackendDispatchOperation {
                    task: TaskKey {
                        task_id: task.task_id + 20_000 + tile_id,
                        ..task.clone()
                    },
                    function: FunctionLabel {
                        name: format!(
                            "qwen3_dense_0_6b_round1_remote_dependent_matmul_shard{}_tile{}",
                            shard.shard_id,
                            tile_id % TILES_PER_SHARD
                        ),
                        level: PlLevel::L2,
                    },
                    backend_spec,
                    request,
                    target_level: PlLevel::L2,
                    target_node: shard.target_node,
                    legacy_input_segments: vec![input_a, input_q, input_kv],
                };
                with_suppressed_stdio(|| {
                    runtime
                        .submit_backend_dispatch(dispatch, &mut sink)
                        .map_err(|err| err.to_string())
                })?;
                round1_dispatch_contexts.push(Round1DispatchContext {
                    shard: *shard,
                    remote_shard: *remote_shard,
                    remote_checksum: *remote_checksum,
                    output_f,
                    dispatch_output_f: output_f,
                    dispatch_output_offset: 0,
                });
                round1_prepare_elapsed += round1_prepare_started.elapsed();
                pending_round1_dispatches += 1;
                if pending_round1_dispatches == round1_dispatch_batch_size {
                    let round1_dispatch_started = Instant::now();
                    qwen3_dense_0_6b_poll_simpler_dispatch_batch(
                        &mut runtime,
                        &mut sink,
                        &mut runtime_time,
                        dispatch_latency,
                        pending_round1_dispatches,
                        "round1",
                    )?;
                    round1_dispatch_elapsed += round1_dispatch_started.elapsed();
                    pending_round1_dispatches = 0;
                }
            }
            if pending_round1_dispatches != 0 {
                let round1_dispatch_started = Instant::now();
                qwen3_dense_0_6b_poll_simpler_dispatch_batch(
                    &mut runtime,
                    &mut sink,
                    &mut runtime_time,
                    dispatch_latency,
                    pending_round1_dispatches,
                    "round1",
                )?;
                round1_dispatch_elapsed += round1_dispatch_started.elapsed();
            }
        }
        if timing_enabled {
            eprintln!(
                "qwen3-stage-timing: stage=round1_prepare total_ms={}",
                round1_prepare_elapsed.as_millis()
            );
            eprintln!(
                "qwen3-stage-timing: stage=round1_dispatch total_ms={}",
                round1_dispatch_elapsed.as_millis()
            );
        }
        qwen3_stage_timing_mark!("round1_total");
        let round1_assemble_started = Instant::now();
        round1_checksums.clear();
        for context in round1_dispatch_contexts {
            let dispatch_output = runtime
                .host_segment_payload(host_node, context.dispatch_output_f)
                .ok_or_else(|| {
                    format!(
                        "missing_qwen3_dense_0_6b_round1_output_payload:{}",
                        context.shard.shard_id
                    )
                })?;
            let dispatch_output_end = context.dispatch_output_offset + output_bytes;
            if dispatch_output.len() < dispatch_output_end {
                return Err(format!(
                "qwen3_dense_0_6b_round1_batch_output_truncated:shard={}:got={}:offset={}:bytes={output_bytes}",
                context.shard.shard_id,
                dispatch_output.len(),
                context.dispatch_output_offset
            ));
            }
            let round1_output =
                &dispatch_output[context.dispatch_output_offset..dispatch_output_end];
            let round1_output_tile = qwen3_dense_0_6b_round1_output_tile_from_remote(
                round1_output,
                MATMUL_DIM,
                context.shard,
                context.remote_shard,
                context.remote_checksum,
            );
            runtime.seed_host_segment(host_node, context.output_f, round1_output_tile.clone());
            let round1_checksum = qwen3_dense_0_6b_shard_output_checksum(&round1_output_tile);
            layer_dependency_descriptors.push(qwen3_dense_0_6b_layer_dependency_descriptor(
                context.shard,
                24,
                23,
                context.remote_shard.shard_id,
                context.output_f,
                output_bytes as u64,
                MATMUL_ELEMS as u64,
                round1_checksum,
            ));
            round1_checksums.push(round1_checksum);
            produced.extend_from_slice(&round1_output_tile);
            round1_outputs.push((
                context.shard,
                round1_output_tile.clone(),
                context.output_f,
                round1_checksum,
            ));
        }
        if timing_enabled {
            eprintln!(
                "qwen3-stage-timing: stage=round1_assemble total_ms={}",
                round1_assemble_started.elapsed().as_millis()
            );
        }
        qwen3_stage_timing_mark!("round1_assemble_total");
        if round1_checksums.len() != tile_count {
            return Err(format!(
                "qwen3_dense_0_6b_round1_summary_count_mismatch:got={}:expected={}",
                round1_checksums.len(),
                tile_count
            ));
        }
        if !round1_checksums.windows(2).any(|pair| pair[0] != pair[1]) {
            return Err(format!(
                "qwen3_dense_0_6b_round1_outputs_not_distinct:shards={}:checksum={:#x}",
                round1_checksums.len(),
                round1_checksums.first().copied().unwrap_or(0)
            ));
        }
    }
    let final_report_started = Instant::now();
    let full_final_report = qwen3_dense_0_6b_final_report_full_enabled();
    let terminal_range_owner = range_compute_contract
        .map(|contract| u32::from(contract.node) + 1 == contract.pipeline_nodes)
        .unwrap_or(true);
    let runtime_full_vocab_enabled = full_final_report || terminal_range_owner;
    let final_report_detail_started = Instant::now();
    let kvcache_read_digest_by_tile =
        qwen3_dense_0_6b_kvcache_read_digest_by_tile(&kvcache_descriptors);
    let final_report_kvcache_digest_ms = final_report_detail_started.elapsed().as_millis();
    let final_report_detail_started = Instant::now();
    let real_weight_stage_links = qwen3_dense_0_6b_real_weight_stage_links(
        real_weight_reference_summary.as_ref(),
        real_qkv_reference_values.as_ref(),
        next_layer_real_weight_reference_summary.as_ref(),
        next_layer_real_qkv_reference_values.as_ref(),
        &layer_dependency_descriptors,
    );
    let final_report_weight_links_ms = final_report_detail_started.elapsed().as_millis();
    let final_report_detail_started = Instant::now();
    let qkv_reference_digest_by_tile =
        qwen3_dense_0_6b_qkv_reference_digest_by_tile(&real_weight_stage_links);
    let mlp_reference_digest_by_tile = qwen3_dense_0_6b_mlp_reference_digest_by_tile(
        &layer_dependency_descriptors,
        real_mlp_reference_summary.as_ref(),
        next_layer_real_mlp_reference_summary.as_ref(),
    );
    let final_report_reference_digest_ms = final_report_detail_started.elapsed().as_millis();
    let final_report_detail_started = Instant::now();
    let real_logits_candidate_summary = if runtime_full_vocab_enabled {
        qwen3_dense_0_6b_real_logits_candidate_summary(
            &round1_outputs,
            logits_sample_token_count,
            qwen3_dense_0_6b_final_hidden_from_round1_outputs(&round1_outputs)?.as_deref(),
        )?
    } else {
        None
    };
    let final_report_real_logits_ms = final_report_detail_started.elapsed().as_millis();
    let result_resolve_count = if range_single_phase_forward {
        0
    } else {
        round1_checksums.len() as u64
    };
    let result_compute_count = if range_single_phase_forward {
        0
    } else {
        round1_checksums.len() as u64
    };
    let result_flow_checksum = qwen3_dense_0_6b_runtime_result_flow_checksum(
        round0_outputs.len() as u64,
        result_resolve_count,
        result_compute_count,
        &tile_checksums,
        &round1_checksums,
    );
    let final_report_detail_started = Instant::now();
    let range_forward_summary = range_compute_contract
        .map(|contract| {
            if real_range_forward_enabled {
                qwen3_dense_0_6b_range_forward_summary_from_contract(
                    topology,
                    contract,
                    guest_input,
                    real_input_embedding_hidden.as_deref(),
                    result_flow_checksum,
                )
            } else {
                qwen3_dense_0_6b_range_forward_summary_from_runtime_outputs(
                    contract,
                    guest_input,
                    real_input_embedding_hidden.as_deref(),
                    &round1_outputs,
                    result_flow_checksum,
                )
            }
        })
        .transpose()?;
    let final_report_range_summary_ms = final_report_detail_started.elapsed().as_millis();
    let final_report_detail_started = Instant::now();
    let logits_descriptor_tokenizer_path = if runtime_full_vocab_enabled {
        real_tokenizer_path.as_deref()
    } else {
        None
    };
    let logits_descriptors = qwen3_dense_0_6b_logits_descriptors(
        &round1_outputs,
        &kvcache_read_digest_by_tile,
        &qkv_reference_digest_by_tile,
        &mlp_reference_digest_by_tile,
        profile.vocab_size,
        logits_sample_token_count,
        logits_descriptor_tokenizer_path,
        guest_input,
        real_logits_candidate_summary.as_ref(),
        range_forward_summary.as_ref(),
        runtime_weight_objects,
        runtime_full_vocab_enabled,
    )?;
    let final_report_logits_descriptors_ms = final_report_detail_started.elapsed().as_millis();
    let final_report_detail_started = Instant::now();
    let result_descriptors = qwen3_dense_0_6b_result_descriptors(&round0_outputs, &round1_outputs);
    let result_block_descriptors =
        qwen3_dense_0_6b_result_block_descriptors(&round1_outputs, MATMUL_DIM);
    let final_report_result_descriptors_ms = final_report_detail_started.elapsed().as_millis();
    let final_report_detail_started = Instant::now();
    produced = vec![
        0u8;
        qwen3_dense_0_6b_service_flow_output_len(
            &result_descriptors,
            &result_block_descriptors,
            &projection_descriptors,
            &layer_dependency_descriptors,
            &kvcache_descriptors,
            &logits_descriptors,
            logits_descriptor_tokenizer_path,
            real_tokenizer_asset_summary.as_ref(),
            real_weight_reference_summary.as_ref(),
            real_mlp_reference_summary.as_ref(),
            next_layer_real_mlp_reference_summary.as_ref(),
            real_logits_candidate_summary.as_ref(),
            &real_weight_stage_links,
            range_forward_summary.as_ref(),
        )
    ];
    qwen3_dense_0_6b_write_service_flow_markers(
        &mut produced,
        tile_checksums.len() as u64,
        result_resolve_count,
        result_compute_count,
        output_bytes as u64,
        MATMUL_ELEMS as u64,
        &tile_checksums,
        &round1_checksums,
        &result_descriptors,
        &result_block_descriptors,
        &projection_descriptors,
        &layer_dependency_descriptors,
        &kvcache_descriptors,
        &logits_descriptors,
        logits_descriptor_tokenizer_path,
        real_tokenizer_asset_summary.as_ref(),
        real_weight_reference_summary.as_ref(),
        real_mlp_reference_summary.as_ref(),
        next_layer_real_mlp_reference_summary.as_ref(),
        real_logits_candidate_summary.as_ref(),
        &real_weight_stage_links,
        range_forward_summary.as_ref(),
    );
    let final_report_marker_write_ms = final_report_detail_started.elapsed().as_millis();
    if timing_enabled {
        eprintln!(
            "qwen3-stage-timing: stage=final_report_detail kvcache_digest_ms={} weight_links_ms={} reference_digest_ms={} real_logits_ms={} range_summary_ms={} logits_descriptors_ms={} result_descriptors_ms={} marker_write_ms={}",
            final_report_kvcache_digest_ms,
            final_report_weight_links_ms,
            final_report_reference_digest_ms,
            final_report_real_logits_ms,
            final_report_range_summary_ms,
            final_report_logits_descriptors_ms,
            final_report_result_descriptors_ms,
            final_report_marker_write_ms
        );
        eprintln!(
            "qwen3-stage-timing: stage=final_report total_ms={}",
            final_report_started.elapsed().as_millis()
        );
    }
    qwen3_stage_timing_mark!("prefill_total");
    let _ = timing_last;
    Ok(produced)
}

#[derive(Clone, Copy, Debug)]
struct Qwen3Dense06bResultDescriptor {
    shard_id: u64,
    owner_node: u64,
    target_node: u64,
    tile_id: u64,
    kv_block_start: u64,
    kv_block_end: u64,
    round0_segment: u64,
    round1_segment: u64,
    round0_checksum: u64,
    round1_checksum: u64,
}

#[derive(Clone, Copy, Debug)]
struct Qwen3Dense06bResultBlockDescriptor {
    shard_id: u64,
    kv_block_id: u64,
    tile_id: u64,
    row_start: u64,
    row_end: u64,
    bytes: u64,
    checksum: u64,
    segment: u64,
}

#[derive(Clone, Debug)]
struct Qwen3Dense06bRangeForwardSummary {
    node: u64,
    layer_start: u64,
    layer_end: u64,
    layer_count: u64,
    next_node: u64,
    pipeline_nodes: u64,
    total_layers: u64,
    hidden_bytes: u64,
    input_tensor_checksum: u64,
    output_tensor_checksum: u64,
    range_layer_checksum: u64,
    real_layer_execution_count: u64,
    first_layer_output_checksum: u64,
    final_layer_output_checksum: u64,
    input_tensor_bytes: u64,
    output_tensor_bytes: u64,
    output_tensor_payload: Vec<u8>,
    kv_state_bytes: u64,
    kv_state_checksum: u64,
    kv_state_payload: Vec<u8>,
}

#[derive(Clone, Copy, Debug)]
struct Qwen3Dense06bProjectionDescriptor {
    shard_id: u64,
    projection_kind: u64,
    segment: u64,
    elems: u64,
    bytes: u64,
    head_start: u64,
    head_end: u64,
    kv_block_start: u64,
    kv_block_end: u64,
    checksum: u64,
}

#[derive(Clone, Copy, Debug)]
struct Qwen3Dense06bLayerDependencyDescriptor {
    layer_id: u64,
    shard_id: u64,
    stage_kind: u64,
    depends_on_stage: u64,
    remote_shard_id: u64,
    segment: u64,
    elems: u64,
    bytes: u64,
    head_start: u64,
    head_end: u64,
    checksum: u64,
}

#[derive(Clone, Copy, Debug)]
struct Qwen3Dense06bKvCacheDescriptor {
    layer_id: u64,
    shard_id: u64,
    tile_id: u64,
    kv_block_start: u64,
    kv_block_end: u64,
    append_position_start: u64,
    append_position_end: u64,
    read_position_start: u64,
    read_position_end: u64,
    update_seq: u64,
    k_segment: u64,
    v_segment: u64,
    k_checksum: u64,
    v_checksum: u64,
}

#[derive(Clone, Copy, Debug)]
struct Qwen3Dense06bKvCacheStateDescriptor {
    layer_id: u64,
    tile_id: u64,
    position: u64,
    update_seq: u64,
    k_checksum: u64,
    v_checksum: u64,
    read_window_end: u64,
    read_digest: u64,
}

#[derive(Clone, Copy, Debug)]
struct Qwen3Dense06bLogitsDescriptor {
    shard_id: u64,
    tile_id: u64,
    segment: u64,
    logits_count: u64,
    sampled_token: u64,
    runner_up_token: u64,
    margin_milli: u64,
    logits_checksum: u64,
    full_vocab_checked_token_count: u64,
    full_vocab_logits_checksum: u64,
    top_logit_bits: u64,
    runner_up_logit_bits: u64,
    candidate_count: u64,
    candidate_tokens: [u64; 4],
    candidate_logit_bits: [u64; 4],
    candidate_text_checksums: [u64; 4],
    candidate_piece_bytes: [u64; 4],
    candidate_piece_word0: [u64; 4],
    candidate_piece_word1: [u64; 4],
    runtime_forward_layer_count: u64,
    runtime_forward_final_hidden_checksum: u64,
    runtime_forward_checksum: u64,
    kvcache_read_digest: u64,
    qkv_reference_digest: u64,
    real_path_digest: u64,
    text_checksum: u64,
    text_byte_offset: u64,
    step_index: u64,
}

#[derive(Clone, Copy, Debug)]
struct Qwen3Dense06bRealWeightStageLinkDescriptor {
    tile_id: u64,
    shard_id: u64,
    stage_kind: u64,
    segment: u64,
    synthetic_checksum: u64,
    real_weight_checksum: u64,
    real_value_checksum: u64,
    real_output_checksum: u64,
    rows: u64,
    hidden_size: u64,
    reference_layer_id: u64,
}

fn qwen3_dense_0_6b_projection_descriptor(
    shard: Qwen3Dense06bShard,
    projection_kind: u64,
    segment: SegmentHandle,
    bytes: u64,
    elems: u64,
    payload: &[u8],
) -> Qwen3Dense06bProjectionDescriptor {
    Qwen3Dense06bProjectionDescriptor {
        shard_id: shard.shard_id,
        projection_kind,
        segment: segment.0,
        elems,
        bytes,
        head_start: shard.head_start,
        head_end: shard.head_end,
        kv_block_start: shard.kv_block_start,
        kv_block_end: shard.kv_block_end,
        checksum: qwen3_dense_0_6b_shard_output_checksum(payload),
    }
}

fn qwen3_dense_0_6b_kvcache_descriptor(
    shard: Qwen3Dense06bShard,
    layer_id: u64,
    tile_id: u64,
    append_position_start: u64,
    append_position_end: u64,
    read_position_start: u64,
    read_position_end: u64,
    update_seq: u64,
    k_segment: SegmentHandle,
    v_segment: SegmentHandle,
    k_payload: &[u8],
    v_payload: &[u8],
) -> Qwen3Dense06bKvCacheDescriptor {
    Qwen3Dense06bKvCacheDescriptor {
        layer_id,
        shard_id: shard.shard_id,
        tile_id,
        kv_block_start: shard.kv_block_start,
        kv_block_end: shard.kv_block_end,
        append_position_start,
        append_position_end,
        read_position_start,
        read_position_end,
        update_seq,
        k_segment: k_segment.0,
        v_segment: v_segment.0,
        k_checksum: qwen3_dense_0_6b_shard_output_checksum(k_payload),
        v_checksum: qwen3_dense_0_6b_shard_output_checksum(v_payload),
    }
}

fn qwen3_dense_0_6b_kvcache_state_descriptors(
    descriptors: &[Qwen3Dense06bKvCacheDescriptor],
) -> Vec<Qwen3Dense06bKvCacheStateDescriptor> {
    let block_count = descriptors
        .iter()
        .map(|descriptor| {
            descriptor
                .append_position_end
                .max(descriptor.read_position_end)
        })
        .max()
        .unwrap_or(0) as usize;
    let mut cache_blocks = vec![None; block_count];
    let mut updates = descriptors.to_vec();
    updates.sort_by_key(|descriptor| descriptor.update_seq);
    let mut state_descriptors = Vec::with_capacity(
        updates
            .iter()
            .map(|descriptor| {
                descriptor
                    .append_position_end
                    .saturating_sub(descriptor.append_position_start) as usize
            })
            .sum(),
    );
    for descriptor in updates {
        for position in descriptor.append_position_start..descriptor.append_position_end {
            let block = Qwen3Dense06bKvCacheStateDescriptor {
                layer_id: descriptor.layer_id,
                tile_id: descriptor.tile_id,
                position,
                update_seq: descriptor.update_seq,
                k_checksum: descriptor.k_checksum,
                v_checksum: descriptor.v_checksum,
                read_window_end: descriptor.read_position_end,
                read_digest: 0,
            };
            if let Some(slot) = cache_blocks.get_mut(position as usize) {
                *slot = Some(block);
            }
        }
        let read_digest = qwen3_dense_0_6b_kvcache_read_digest(
            &cache_blocks,
            descriptor.read_position_start as usize,
            descriptor.read_position_end as usize,
        );
        for position in descriptor.append_position_start..descriptor.append_position_end {
            if let Some(Some(block)) = cache_blocks.get_mut(position as usize) {
                block.read_digest = read_digest;
                state_descriptors.push(*block);
            }
        }
    }
    state_descriptors
}

fn qwen3_dense_0_6b_kvcache_read_digest(
    cache_blocks: &[Option<Qwen3Dense06bKvCacheStateDescriptor>],
    start: usize,
    end: usize,
) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for (position, block) in cache_blocks.iter().enumerate().take(end).skip(start) {
        if let Some(block) = block {
            acc = acc.wrapping_mul(0x0000_0100_0000_01b3)
                ^ block.layer_id
                ^ block.tile_id.rotate_left(5)
                ^ (position as u64).rotate_left(11)
                ^ block.update_seq.rotate_left(17)
                ^ block.k_checksum.rotate_left(23)
                ^ block.v_checksum.rotate_left(29);
        }
    }
    acc
}

fn qwen3_dense_0_6b_kvcache_read_digest_by_tile(
    descriptors: &[Qwen3Dense06bKvCacheDescriptor],
) -> BTreeMap<u64, u64> {
    let mut words_by_tile: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
    for descriptor in qwen3_dense_0_6b_kvcache_state_descriptors(descriptors) {
        words_by_tile
            .entry(descriptor.tile_id)
            .or_default()
            .push(descriptor.read_digest);
    }
    words_by_tile
        .into_iter()
        .map(|(tile_id, words)| (tile_id, checksum_words(&words)))
        .collect()
}

#[derive(Clone, Debug)]
struct Qwen3Dense06bKvCacheTilePayload {
    layer_id: u64,
    tile_id: u64,
    read_position_start: u64,
    read_position_end: u64,
    k_rows: Vec<Vec<f32>>,
    v_rows: Vec<Vec<f32>>,
}

impl Qwen3Dense06bKvCacheTilePayload {
    fn read_len(&self) -> usize {
        self.k_rows.len().min(self.v_rows.len())
    }
}

#[derive(Default)]
struct Qwen3Dense06bKvCacheStore {
    rows: BTreeMap<(u64, u64, u64), (Vec<f32>, Vec<f32>)>,
}

impl Qwen3Dense06bKvCacheStore {
    fn append_projection_rows(
        &mut self,
        layer_id: u64,
        tile_id: u64,
        append_position_start: u64,
        append_position_end: u64,
        k_payload: &[u8],
        v_payload: &[u8],
        dim: usize,
    ) {
        for position in append_position_start..append_position_end {
            let source_row = position as usize % dim;
            let row_start = source_row * dim;
            let mut k_row = Vec::with_capacity(dim);
            let mut v_row = Vec::with_capacity(dim);
            for col in 0..dim {
                k_row.push(qwen3_dense_0_6b_half_at(k_payload, row_start + col));
                v_row.push(qwen3_dense_0_6b_half_at(v_payload, row_start + col));
            }
            self.rows
                .insert((layer_id, tile_id, position), (k_row, v_row));
        }
    }

    fn read_tile_payload(
        &self,
        layer_id: u64,
        tile_id: u64,
        read_position_start: u64,
        read_position_end: u64,
    ) -> Option<Qwen3Dense06bKvCacheTilePayload> {
        let mut k_rows = Vec::new();
        let mut v_rows = Vec::new();
        for position in read_position_start..read_position_end {
            if let Some((k_row, v_row)) = self.rows.get(&(layer_id, tile_id, position)) {
                k_rows.push(k_row.clone());
                v_rows.push(v_row.clone());
            }
        }
        (!k_rows.is_empty() && k_rows.len() == v_rows.len()).then_some(
            Qwen3Dense06bKvCacheTilePayload {
                layer_id,
                tile_id,
                read_position_start,
                read_position_end,
                k_rows,
                v_rows,
            },
        )
    }
}

#[cfg(test)]
fn qwen3_dense_0_6b_kvcache_tile_payload_from_projection(
    layer_id: u64,
    tile_id: u64,
    read_position_start: u64,
    read_position_end: u64,
    k_payload: &[u8],
    v_payload: &[u8],
    dim: usize,
) -> Qwen3Dense06bKvCacheTilePayload {
    let read_len = read_position_end
        .saturating_sub(read_position_start)
        .min(dim as u64) as usize;
    let mut k_rows = Vec::with_capacity(read_len);
    let mut v_rows = Vec::with_capacity(read_len);
    for position_offset in 0..read_len {
        let source_row = (read_position_start as usize + position_offset) % dim;
        let row_start = source_row * dim;
        let mut k_row = Vec::with_capacity(dim);
        let mut v_row = Vec::with_capacity(dim);
        for col in 0..dim {
            k_row.push(qwen3_dense_0_6b_half_at(k_payload, row_start + col));
            v_row.push(qwen3_dense_0_6b_half_at(v_payload, row_start + col));
        }
        k_rows.push(k_row);
        v_rows.push(v_row);
    }
    Qwen3Dense06bKvCacheTilePayload {
        layer_id,
        tile_id,
        read_position_start,
        read_position_end,
        k_rows,
        v_rows,
    }
}

fn qwen3_dense_0_6b_layer_dependency_descriptor(
    shard: Qwen3Dense06bShard,
    stage_kind: u64,
    depends_on_stage: u64,
    remote_shard_id: u64,
    segment: SegmentHandle,
    bytes: u64,
    elems: u64,
    checksum: u64,
) -> Qwen3Dense06bLayerDependencyDescriptor {
    Qwen3Dense06bLayerDependencyDescriptor {
        layer_id: shard.kv_block_start / 2,
        shard_id: shard.shard_id,
        stage_kind,
        depends_on_stage,
        remote_shard_id,
        segment: segment.0,
        elems,
        bytes,
        head_start: shard.head_start,
        head_end: shard.head_end,
        checksum,
    }
}

fn qwen3_dense_0_6b_result_descriptors(
    round0_outputs: &[(Qwen3Dense06bShard, Vec<u8>, SegmentHandle, u64)],
    round1_outputs: &[(Qwen3Dense06bShard, Vec<u8>, SegmentHandle, u64)],
) -> Vec<Qwen3Dense06bResultDescriptor> {
    round0_outputs
        .iter()
        .zip(round1_outputs.iter())
        .map(
            |(
                (round0_shard, _, round0_segment, round0_checksum),
                (round1_shard, _, round1_segment, round1_checksum),
            )| {
                debug_assert_eq!(round0_shard.shard_id, round1_shard.shard_id);
                debug_assert_eq!(round0_shard.kv_block_start, round1_shard.kv_block_start);
                let tile_id = round0_shard.kv_block_start / 2;
                Qwen3Dense06bResultDescriptor {
                    shard_id: round0_shard.shard_id,
                    owner_node: round0_shard.owner_node as u64,
                    target_node: round0_shard.target_node as u64,
                    tile_id,
                    kv_block_start: round0_shard.kv_block_start,
                    kv_block_end: round0_shard.kv_block_end,
                    round0_segment: round0_segment.0,
                    round1_segment: round1_segment.0,
                    round0_checksum: *round0_checksum,
                    round1_checksum: *round1_checksum,
                }
            },
        )
        .collect()
}

fn qwen3_dense_0_6b_result_block_descriptors(
    round1_outputs: &[(Qwen3Dense06bShard, Vec<u8>, SegmentHandle, u64)],
    dim: usize,
) -> Vec<Qwen3Dense06bResultBlockDescriptor> {
    let rows_per_block = dim as u64 / 2;
    let row_bytes = dim * std::mem::size_of::<f32>();
    let mut descriptors = Vec::with_capacity(round1_outputs.len() * 2);
    for (shard, output, segment, _checksum) in round1_outputs {
        let tile_id = shard.kv_block_start / 2;
        for block_in_tile in 0..2u64 {
            let row_start = block_in_tile * rows_per_block;
            let row_end = row_start + rows_per_block;
            let byte_start = row_start as usize * row_bytes;
            let byte_end = row_end as usize * row_bytes;
            descriptors.push(Qwen3Dense06bResultBlockDescriptor {
                shard_id: shard.shard_id,
                kv_block_id: shard.kv_block_start + block_in_tile,
                tile_id,
                row_start,
                row_end,
                bytes: (byte_end - byte_start) as u64,
                checksum: qwen3_dense_0_6b_shard_output_checksum(&output[byte_start..byte_end]),
                segment: segment.0,
            });
        }
    }
    descriptors
}

fn qwen3_dense_0_6b_result_block_layout_checksum(
    descriptors: &[Qwen3Dense06bResultBlockDescriptor],
) -> u64 {
    descriptors
        .iter()
        .fold(0xcbf2_9ce4_8422_2325, |acc, descriptor| {
            acc.wrapping_mul(0x0000_0100_0000_01b3)
                ^ descriptor.shard_id
                ^ descriptor.kv_block_id.rotate_left(7)
                ^ descriptor.tile_id.rotate_left(13)
                ^ descriptor.row_start.rotate_left(19)
                ^ descriptor.row_end.rotate_left(23)
                ^ descriptor.bytes.rotate_left(29)
                ^ descriptor.checksum.rotate_left(31)
                ^ descriptor.segment.rotate_left(37)
        })
}

fn qwen3_dense_0_6b_logits_descriptors(
    round1_outputs: &[(Qwen3Dense06bShard, Vec<u8>, SegmentHandle, u64)],
    kvcache_read_digest_by_tile: &BTreeMap<u64, u64>,
    qkv_reference_digest_by_tile: &BTreeMap<u64, u64>,
    mlp_reference_digest_by_tile: &BTreeMap<u64, u64>,
    vocab_size: u64,
    sample_token_count: u64,
    tokenizer_path: Option<&Path>,
    guest_input: &[u8],
    real_logits_candidate_summary: Option<&Qwen3Dense06bLogitsReferenceSummary>,
    range_forward_summary: Option<&Qwen3Dense06bRangeForwardSummary>,
    mut runtime_weight_objects: Option<&mut LingquObjectServiceStub>,
    runtime_full_vocab_enabled: bool,
) -> Result<Vec<Qwen3Dense06bLogitsDescriptor>, String> {
    let mut descriptors = Vec::with_capacity(round1_outputs.len());
    let mut text_byte_offset = 0u64;
    let range_forward_hidden =
        qwen3_dense_0_6b_trusted_terminal_range_forward_hidden(range_forward_summary)?;
    let terminal_range_requires_runtime_hidden = runtime_full_vocab_enabled
        && range_forward_summary
            .map(qwen3_dense_0_6b_range_forward_summary_is_terminal)
            .unwrap_or(false);
    if terminal_range_requires_runtime_hidden && range_forward_hidden.is_none() {
        let summary = range_forward_summary.expect("range summary checked above");
        return Err(format!(
            "qwen3_terminal_range_hidden_untrusted:node={}:layers={}..{}:real_layers={}:expected_layers={}:payload_bytes={}:total_layers={}",
            summary.node,
            summary.layer_start,
            summary.layer_end,
            summary.real_layer_execution_count,
            summary.layer_count,
            summary.output_tensor_payload.len(),
            summary.total_layers
        ));
    }
    let (runtime_forward, weight_payloads) = if runtime_full_vocab_enabled {
        if range_forward_hidden.is_some() {
            let weight_payloads = if let Some(service) = runtime_weight_objects.as_deref_mut() {
                qwen3_dense_0_6b_resolve_runtime_weight_objects(service, 700_000)?.layer_payloads
            } else {
                BTreeMap::new()
            };
            (None, weight_payloads)
        } else {
            qwen3_dense_0_6b_runtime_forward_summary_from_guest_input(
                guest_input,
                runtime_weight_objects,
            )?
        }
    } else {
        (None, BTreeMap::new())
    };
    let round1_hidden = if runtime_forward.is_none() && range_forward_hidden.is_none() {
        qwen3_dense_0_6b_final_hidden_from_round1_outputs(round1_outputs)?
    } else {
        None
    };
    let runtime_logits_hidden = runtime_forward
        .as_ref()
        .map(|forward| forward.final_hidden.as_slice())
        .or(range_forward_hidden.as_deref())
        .or(round1_hidden.as_deref());
    let runtime_full_vocab = if runtime_full_vocab_enabled {
        qwen3_dense_0_6b_runtime_full_vocab_logits_summary(
            runtime_logits_hidden,
            Some(&weight_payloads),
        )?
    } else {
        None
    };
    for (step_index, (shard, _output, segment, checksum)) in round1_outputs.iter().enumerate() {
        let tile_id = shard.kv_block_start / 2;
        let kvcache_read_digest = kvcache_read_digest_by_tile
            .get(&tile_id)
            .copied()
            .unwrap_or(0);
        let qkv_reference_digest = qkv_reference_digest_by_tile
            .get(&tile_id)
            .copied()
            .unwrap_or(0);
        let mlp_reference_digest = mlp_reference_digest_by_tile
            .get(&tile_id)
            .copied()
            .unwrap_or(0);
        let final_norm_digest = real_logits_candidate_summary
            .map(|summary| summary.final_norm_checksum)
            .unwrap_or(0);
        let real_path_digest =
            if qkv_reference_digest == 0 && mlp_reference_digest == 0 && final_norm_digest == 0 {
                0
            } else {
                checksum_words(&[
                    tile_id,
                    qkv_reference_digest,
                    mlp_reference_digest,
                    final_norm_digest,
                ])
            };
        let logits_count = vocab_size;
        let fallback_logits_seed = *checksum
            ^ kvcache_read_digest.rotate_left(13)
            ^ qkv_reference_digest.rotate_left(19)
            ^ real_path_digest.rotate_left(23);
        let fallback_sampled_token =
            qwen3_dense_0_6b_sampled_token(fallback_logits_seed, tile_id, sample_token_count);
        let fallback_runner_up_token = (fallback_sampled_token
            + 17
            + shard.shard_id
            + tile_id
            + (kvcache_read_digest & 0x0f)
            + ((qkv_reference_digest >> 4) & 0x0f)
            + ((real_path_digest >> 8) & 0x0f))
            % sample_token_count;
        let fallback_margin_milli = 1_000 + (tile_id * 7) + shard.shard_id;
        let (
            sampled_token,
            runner_up_token,
            margin_milli,
            real_top_checksum,
            real_runner_checksum,
            full_vocab_checked_token_count,
            full_vocab_logits_checksum,
            top_logit_bits,
            runner_up_logit_bits,
            candidate_count,
            candidate_tokens,
            candidate_logit_bits,
        ) = runtime_full_vocab
            .as_ref()
            .map(|summary| {
                let top_logit = f32::from_bits(summary.top_logit_bits as u32);
                let runner_logit = f32::from_bits(summary.runner_up_logit_bits as u32);
                let margin = if top_logit.is_finite() && runner_logit.is_finite() {
                    ((top_logit - runner_logit).abs() * 1_000.0)
                        .round()
                        .max(1.0) as u64
                } else {
                    1
                };
                let (candidate_count, candidate_tokens, candidate_logit_bits) =
                    qwen3_dense_0_6b_top_candidate_arrays(summary);
                (
                    summary.top_token_id,
                    summary.runner_up_token_id,
                    margin,
                    checksum_words(&[
                        summary.top_token_id,
                        summary.top_logit_bits,
                        summary.logits_checksum,
                    ]),
                    checksum_words(&[
                        summary.runner_up_token_id,
                        summary.runner_up_logit_bits,
                        summary.logits_checksum,
                    ]),
                    summary.checked_token_count,
                    summary.logits_checksum,
                    summary.top_logit_bits,
                    summary.runner_up_logit_bits,
                    candidate_count,
                    candidate_tokens,
                    candidate_logit_bits,
                )
            })
            .or_else(|| {
                qwen3_dense_0_6b_real_logits_selection(
                    real_logits_candidate_summary,
                    step_index as u64,
                )
                .map(|(sampled, runner, margin, top_checksum, runner_checksum)| {
                    (
                        sampled,
                        runner,
                        margin,
                        top_checksum,
                        runner_checksum,
                        0,
                        0,
                        0,
                        0,
                        2,
                        [sampled, runner, 0, 0],
                        [0, 0, 0, 0],
                    )
                })
            })
            .unwrap_or((
                fallback_sampled_token,
                fallback_runner_up_token,
                fallback_margin_milli,
                0,
                0,
                0,
                0,
                0,
                0,
                2,
                [fallback_sampled_token, fallback_runner_up_token, 0, 0],
                [0, 0, 0, 0],
            ));
        let (
            runtime_forward_layer_count,
            runtime_forward_final_hidden_checksum,
            runtime_forward_checksum,
        ) = runtime_forward
            .as_ref()
            .map(|forward| {
                (
                    forward.layer_count,
                    forward.final_hidden_checksum,
                    forward.aggregate_checksum,
                )
            })
            .unwrap_or((0, 0, 0));
        let logits_checksum = qwen3_dense_0_6b_logits_checksum(
            *checksum,
            tile_id,
            sampled_token,
            runner_up_token,
            margin_milli,
            real_top_checksum,
            real_runner_checksum,
            kvcache_read_digest,
            qkv_reference_digest,
            real_path_digest,
        );
        let mut candidate_text_checksums = [0u64; 4];
        let mut candidate_piece_bytes = [0u64; 4];
        let mut candidate_piece_word0 = [0u64; 4];
        let mut candidate_piece_word1 = [0u64; 4];
        for candidate_index in 0..candidate_count.min(4) as usize {
            let token = candidate_tokens[candidate_index];
            let candidate_piece = qwen3_dense_0_6b_token_piece(token, tokenizer_path)?;

            candidate_text_checksums[candidate_index] = qwen3_dense_0_6b_sample_text_checksum(
                step_index as u64,
                token,
                text_byte_offset,
                tokenizer_path,
            )?;
            candidate_piece_bytes[candidate_index] = candidate_piece.byte_len;
            candidate_piece_word0[candidate_index] = candidate_piece.word0;
            candidate_piece_word1[candidate_index] = candidate_piece.word1;
        }
        let piece = qwen3_dense_0_6b_token_piece(sampled_token, tokenizer_path)?;
        let text_checksum = qwen3_dense_0_6b_sample_text_checksum(
            step_index as u64,
            sampled_token,
            text_byte_offset,
            tokenizer_path,
        )?;
        descriptors.push(Qwen3Dense06bLogitsDescriptor {
            shard_id: shard.shard_id,
            tile_id,
            segment: segment.0,
            logits_count,
            sampled_token,
            runner_up_token,
            margin_milli,
            logits_checksum,
            full_vocab_checked_token_count,
            full_vocab_logits_checksum,
            top_logit_bits,
            runner_up_logit_bits,
            candidate_count,
            candidate_tokens,
            candidate_logit_bits,
            candidate_text_checksums,
            candidate_piece_bytes,
            candidate_piece_word0,
            candidate_piece_word1,
            runtime_forward_layer_count,
            runtime_forward_final_hidden_checksum,
            runtime_forward_checksum,
            kvcache_read_digest,
            qkv_reference_digest,
            real_path_digest,
            text_checksum,
            text_byte_offset,
            step_index: step_index as u64,
        });
        text_byte_offset += piece.byte_len;
    }
    Ok(descriptors)
}

fn qwen3_dense_0_6b_trusted_terminal_range_forward_hidden(
    summary: Option<&Qwen3Dense06bRangeForwardSummary>,
) -> Result<Option<Vec<f32>>, String> {
    let Some(summary) = summary else {
        return Ok(None);
    };
    if !qwen3_dense_0_6b_range_forward_summary_is_terminal(summary) {
        return Ok(None);
    }
    if summary.layer_count == 0 || summary.real_layer_execution_count != summary.layer_count {
        return Ok(None);
    }
    if summary.output_tensor_payload.len() as u64 != summary.output_tensor_bytes {
        return Ok(None);
    }
    qwen3_dense_0_6b_hidden_from_range_output_payload(summary).map(Some)
}

fn qwen3_dense_0_6b_range_forward_summary_is_terminal(
    summary: &Qwen3Dense06bRangeForwardSummary,
) -> bool {
    let profile_layers = QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers;
    summary.layer_end == profile_layers && summary.total_layers == profile_layers
}

fn qwen3_dense_0_6b_sampled_token(round1_checksum: u64, tile_id: u64, vocab_size: u64) -> u64 {
    debug_assert_ne!(vocab_size, 0);
    (round1_checksum ^ tile_id.wrapping_mul(0x9e37_79b9_7f4a_7c15)) % vocab_size
}

fn qwen3_dense_0_6b_logits_candidate_token_requests(
    round1_outputs: &[(Qwen3Dense06bShard, Vec<u8>, SegmentHandle, u64)],
    sample_token_count: u64,
) -> Vec<(u64, u64)> {
    let mut requests = Vec::with_capacity(round1_outputs.len() * 4);
    for (step_index, (shard, _output, _segment, checksum)) in round1_outputs.iter().enumerate() {
        let tile_id = shard.kv_block_start / 2;
        let base = qwen3_dense_0_6b_sampled_token(*checksum, tile_id, sample_token_count);
        let candidates = [
            base,
            (base + 17 + shard.shard_id + tile_id) % sample_token_count,
            (base + 101 + step_index as u64 * 13 + shard.shard_id) % sample_token_count,
            (base ^ checksum.rotate_left(11) ^ tile_id.rotate_left(23)) % sample_token_count,
        ];
        for token in candidates {
            if !requests.iter().any(|(request_step, request_token)| {
                *request_step == step_index as u64 && *request_token == token
            }) {
                requests.push((step_index as u64, token));
            }
        }
    }
    requests
}

fn qwen3_dense_0_6b_real_logits_selection(
    summary: Option<&Qwen3Dense06bLogitsReferenceSummary>,
    step_index: u64,
) -> Option<(u64, u64, u64, u64, u64)> {
    let summary = summary?;
    let mut tokens: Vec<_> = summary
        .tokens
        .iter()
        .filter(|token| token.step_index == step_index)
        .collect();
    if tokens.len() < 2 {
        return None;
    }
    tokens.sort_by(|left, right| {
        let left_logit = f32::from_bits(left.logit_bits as u32);
        let right_logit = f32::from_bits(right.logit_bits as u32);
        right_logit
            .partial_cmp(&left_logit)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| left.token_id.cmp(&right.token_id))
    });
    let top = tokens[0];
    let runner = tokens[1];
    let top_logit = f32::from_bits(top.logit_bits as u32);
    let runner_logit = f32::from_bits(runner.logit_bits as u32);
    let margin = if top_logit.is_finite() && runner_logit.is_finite() {
        ((top_logit - runner_logit).abs() * 1_000.0)
            .round()
            .max(1.0) as u64
    } else {
        1
    };
    Some((
        top.token_id,
        runner.token_id,
        margin,
        top.logit_checksum,
        runner.logit_checksum,
    ))
}

fn qwen3_dense_0_6b_top_candidate_arrays(
    summary: &Qwen3Dense06bFullVocabLogitsSummary,
) -> (u64, [u64; 4], [u64; 4]) {
    let mut tokens = [0u64; 4];
    let mut logit_bits = [0u64; 4];

    for candidate in summary.top_candidates.iter().take(4) {
        let index = candidate.rank as usize;

        if index < tokens.len() {
            tokens[index] = candidate.token_id;
            logit_bits[index] = candidate.logit_bits;
        }
    }
    (
        summary.top_candidates.len().min(tokens.len()) as u64,
        tokens,
        logit_bits,
    )
}

fn qwen3_dense_0_6b_tokenizer_sample_token_count(
    summary: &Qwen3Dense06bTokenizerAssetSummary,
) -> u64 {
    summary.vocab_entries + summary.added_tokens
}

fn qwen3_dense_0_6b_logits_checksum(
    round1_checksum: u64,
    tile_id: u64,
    sampled_token: u64,
    runner_up_token: u64,
    margin_milli: u64,
    real_top_checksum: u64,
    real_runner_checksum: u64,
    kvcache_read_digest: u64,
    qkv_reference_digest: u64,
    real_path_digest: u64,
) -> u64 {
    round1_checksum.wrapping_mul(0x0000_0100_0000_01b3)
        ^ tile_id.rotate_left(7)
        ^ sampled_token.rotate_left(13)
        ^ runner_up_token.rotate_left(29)
        ^ margin_milli.rotate_left(43)
        ^ real_top_checksum.rotate_left(5)
        ^ real_runner_checksum.rotate_left(17)
        ^ kvcache_read_digest.rotate_left(31)
        ^ qkv_reference_digest.rotate_left(47)
        ^ real_path_digest.rotate_left(53)
}

fn qwen3_dense_0_6b_sample_text_checksum(
    step_index: u64,
    sampled_token: u64,
    byte_offset: u64,
    tokenizer_path: Option<&Path>,
) -> Result<u64, String> {
    let piece = qwen3_dense_0_6b_token_piece(sampled_token, tokenizer_path)?;
    Ok(0xcbf2_9ce4_8422_2325u64
        .wrapping_mul(0x0000_0100_0000_01b3)
        .wrapping_add(step_index.rotate_left(11))
        ^ sampled_token.rotate_left(31)
        ^ byte_offset.rotate_left(17)
        ^ piece.byte_len.rotate_left(23)
        ^ piece.word0.rotate_left(37)
        ^ piece.word1.rotate_left(43)
        ^ piece.checksum.rotate_left(3))
}

fn qwen3_dense_0_6b_token_piece(
    sampled_token: u64,
    tokenizer_path: Option<&Path>,
) -> Result<sim_models::qwen3_dense_0_6b::Qwen3Dense06bTokenPiece, String> {
    if tokenizer_path.is_none() {
        return Ok(token_piece_from_policy(
            tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE),
            sampled_token,
        ));
    }
    let piece = qwen3_dense_0_6b_token_piece_raw_bytes(sampled_token, tokenizer_path)?;
    Ok(qwen3_dense_0_6b_token_piece_from_bytes(
        sampled_token,
        &piece,
    ))
}

fn qwen3_dense_0_6b_token_piece_from_bytes(
    token_id: u64,
    piece: &[u8],
) -> sim_models::qwen3_dense_0_6b::Qwen3Dense06bTokenPiece {
    let mut bytes = [0u8; 16];
    let copy_len = piece.len().min(bytes.len());
    bytes[..copy_len].copy_from_slice(&piece[..copy_len]);
    sim_models::qwen3_dense_0_6b::Qwen3Dense06bTokenPiece {
        token_id,
        byte_len: piece.len() as u64,
        word0: u64::from_le_bytes(bytes[0..8].try_into().expect("token piece word0")),
        word1: u64::from_le_bytes(bytes[8..16].try_into().expect("token piece word1")),
        checksum: qwen3_dense_0_6b_token_piece_checksum(token_id, piece),
    }
}

fn qwen3_dense_0_6b_token_piece_checksum(token_id: u64, piece: &[u8]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64 ^ token_id;
    for byte in piece {
        acc ^= *byte as u64;
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc ^ (piece.len() as u64).rotate_left(17)
}

fn qwen3_dense_0_6b_real_weight_stage_links(
    summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
    values: Option<&Qwen3Dense06bQkvReferenceLayerValues>,
    next_summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
    next_values: Option<&Qwen3Dense06bQkvReferenceLayerValues>,
    layer_dependency_descriptors: &[Qwen3Dense06bLayerDependencyDescriptor],
) -> Vec<Qwen3Dense06bRealWeightStageLinkDescriptor> {
    let references = [
        (summary, values, 0u64, [1u64, 2, 3, 4]),
        (next_summary, next_values, 1u64, [13u64, 14, 15, 16]),
    ];
    layer_dependency_descriptors
        .iter()
        .filter_map(|descriptor| {
            let (summary, values, normalized_stage_kind) =
                qwen3_dense_0_6b_qkv_reference_for_stage(descriptor.stage_kind, &references)?;
            let shard = summary
                .shards
                .iter()
                .find(|shard| shard.shard_id == descriptor.shard_id)?;
            let value_shard = values.and_then(|values| {
                values
                    .shards
                    .iter()
                    .find(|shard| shard.shard_id == descriptor.shard_id)
            });
            let (real_weight_checksum, real_output_checksum, real_value_checksum, rows) =
                match normalized_stage_kind {
                    1 => (
                        qwen3_dense_0_6b_reference_weight_slice_checksum(
                            shard,
                            Qwen3Dense06bWeightTensorKind::InputLayerNorm,
                        )?,
                        shard.rmsnorm_checksum,
                        value_shard
                            .map(|shard| shard.rmsnorm_checksum)
                            .unwrap_or(shard.rmsnorm_checksum),
                        shard.hidden_size,
                    ),
                    2 => (
                        shard.q_weight_checksum,
                        shard.q_output_checksum,
                        value_shard
                            .map(|shard| shard.q_output_checksum)
                            .unwrap_or(shard.q_output_checksum),
                        shard.q_rows,
                    ),
                    3 => (
                        shard.k_weight_checksum,
                        shard.k_output_checksum,
                        value_shard
                            .map(|shard| shard.k_output_checksum)
                            .unwrap_or(shard.k_output_checksum),
                        shard.k_rows,
                    ),
                    4 => (
                        shard.v_weight_checksum,
                        shard.v_output_checksum,
                        value_shard
                            .map(|shard| shard.v_output_checksum)
                            .unwrap_or(shard.v_output_checksum),
                        shard.v_rows,
                    ),
                    _ => unreachable!("stage filter only admits RMSNorm and Q/K/V projections"),
                };
            Some(Qwen3Dense06bRealWeightStageLinkDescriptor {
                tile_id: descriptor.layer_id,
                shard_id: descriptor.shard_id,
                stage_kind: normalized_stage_kind,
                segment: descriptor.segment,
                synthetic_checksum: descriptor.checksum,
                real_weight_checksum,
                real_value_checksum,
                real_output_checksum,
                rows,
                hidden_size: shard.hidden_size,
                reference_layer_id: summary.layer_id,
            })
        })
        .collect()
}

fn qwen3_dense_0_6b_qkv_reference_for_stage<'a>(
    stage_kind: u64,
    references: &[(
        Option<&'a Qwen3Dense06bQkvReferenceLayerSummary>,
        Option<&'a Qwen3Dense06bQkvReferenceLayerValues>,
        u64,
        [u64; 4],
    )],
) -> Option<(
    &'a Qwen3Dense06bQkvReferenceLayerSummary,
    Option<&'a Qwen3Dense06bQkvReferenceLayerValues>,
    u64,
)> {
    for (summary, values, _layer_offset, stage_kinds) in references {
        if let Some(index) = stage_kinds.iter().position(|kind| *kind == stage_kind) {
            return Some((summary.as_ref()?, *values, index as u64 + 1));
        }
    }
    None
}

fn qwen3_dense_0_6b_qkv_reference_digest_by_tile(
    links: &[Qwen3Dense06bRealWeightStageLinkDescriptor],
) -> BTreeMap<u64, u64> {
    let mut words_by_tile: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
    for link in links {
        words_by_tile.entry(link.tile_id).or_default().push(
            link.stage_kind.rotate_left(3)
                ^ link.reference_layer_id.rotate_left(11)
                ^ link.real_weight_checksum.rotate_left(17)
                ^ link.real_output_checksum.rotate_left(29)
                ^ link.real_value_checksum.rotate_left(37)
                ^ link.synthetic_checksum.rotate_left(41),
        );
    }
    words_by_tile
        .into_iter()
        .map(|(tile_id, words)| (tile_id, checksum_words(&words)))
        .collect()
}

fn qwen3_dense_0_6b_mlp_reference_digest_by_tile(
    descriptors: &[Qwen3Dense06bLayerDependencyDescriptor],
    summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
    next_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
) -> BTreeMap<u64, u64> {
    let mut words_by_tile: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
    for descriptor in descriptors {
        let (Some(layer_summary), reference_kind) = (match descriptor.stage_kind {
            10 => (summary, 10u64),
            12 => (summary, 12u64),
            22 => (next_summary, 22u64),
            _ => continue,
        }) else {
            continue;
        };
        let Some(reference_shard) = layer_summary
            .shards
            .iter()
            .find(|shard| shard.shard_id == descriptor.shard_id)
        else {
            continue;
        };
        let reference_digest = match descriptor.stage_kind {
            10 => checksum_words(&[
                reference_shard.gate_weight_checksum,
                reference_shard.up_weight_checksum,
                reference_shard.gate_output_checksum,
                reference_shard.up_output_checksum,
                reference_shard.activation_checksum,
                qwen3_dense_0_6b_mlp_reference_sample_digest(reference_shard),
            ]),
            12 | 22 => checksum_words(&[
                reference_shard.down_weight_checksum,
                reference_shard.down_output_checksum,
                qwen3_dense_0_6b_mlp_reference_sample_digest(reference_shard),
            ]),
            _ => 0,
        };
        words_by_tile.entry(descriptor.layer_id).or_default().push(
            reference_kind.rotate_left(3)
                ^ descriptor.checksum.rotate_left(11)
                ^ reference_digest.rotate_left(29)
                ^ layer_summary.aggregate_checksum.rotate_left(41),
        );
    }
    words_by_tile
        .into_iter()
        .map(|(tile_id, words)| (tile_id, checksum_words(&words)))
        .collect()
}

fn qwen3_dense_0_6b_reference_weight_slice_checksum(
    shard: &Qwen3Dense06bQkvReferenceShardSummary,
    kind: Qwen3Dense06bWeightTensorKind,
) -> Option<u64> {
    shard
        .weight_slices
        .iter()
        .find(|slice| slice.kind == kind)
        .map(|slice| slice.checksum)
}

fn qwen3_dense_0_6b_shard_output_checksum(bytes: &[u8]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for chunk in bytes.chunks(std::mem::size_of::<u64>()) {
        let mut word = [0u8; 8];
        word[..chunk.len()].copy_from_slice(chunk);
        acc ^= u64::from_le_bytes(word);
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn qwen3_dense_0_6b_partial_result_key(layer_id: u64, shard_id: u64, tile_id: u64) -> String {
    format!("qwen3_dense_0_6b/layer/{layer_id}/shard/{shard_id}/tile/{tile_id}/partial_result")
}

fn qwen3_dense_0_6b_partial_result_storage_ref(
    layer_id: u64,
    shard_id: u64,
    tile_id: u64,
) -> String {
    format!(
        "qwen3_dense_0_6b/runtime/layer/{layer_id}/shard/{shard_id}/tile/{tile_id}/partial_result"
    )
}

fn qwen3_dense_0_6b_remote_partial_to_half_input(bytes: &[u8], elems: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let value = qwen3_dense_0_6b_f32_at(bytes, elem_index);
        out.extend_from_slice(&f32_to_f16_bits(value).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_f32_tile_to_half_input(bytes: &[u8], elems: usize) -> Vec<u8> {
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let value = qwen3_dense_0_6b_f32_at(bytes, elem_index);
        out.extend_from_slice(&f32_to_f16_bits(value).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_runtime_result_flow_checksum(
    publish_count: u64,
    resolve_count: u64,
    round1_compute_count: u64,
    round0_checksums: &[u64],
    round1_checksums: &[u64],
) -> u64 {
    let round0_words = round0_checksums
        .iter()
        .enumerate()
        .map(|(tile_id, checksum)| (tile_id as u64).rotate_left(7) ^ checksum.rotate_left(17))
        .collect::<Vec<_>>();
    let round1_words = round1_checksums
        .iter()
        .enumerate()
        .map(|(tile_id, checksum)| (tile_id as u64).rotate_left(11) ^ checksum.rotate_left(23))
        .collect::<Vec<_>>();
    let result_count = round1_checksums.len() as u64;
    checksum_words(&[
        publish_count,
        resolve_count,
        round1_compute_count,
        result_count,
        distinct_checksum_count(round0_checksums),
        distinct_checksum_count(round1_checksums),
        checksum_words(&round0_words),
        checksum_words(&round1_words),
    ])
}

fn qwen3_dense_0_6b_range_forward_summary_from_contract(
    topology: &SimTopology,
    contract: Qwen3GuestRangeComputeContract,
    guest_input: &[u8],
    real_input_embedding_hidden: Option<&[f32]>,
    result_flow_checksum: u64,
) -> Result<Qwen3Dense06bRangeForwardSummary, String> {
    const MATMUL_DIM: usize = 128;
    const MATMUL_ELEMS: usize = MATMUL_DIM * MATMUL_DIM;
    const HIDDEN_BYTES: u64 = 262_144;
    const RANGE_INPUT_PAYLOAD_OFFSET: usize = 0x08_0000;
    const RANGE_INPUT_PAYLOAD_BYTES: usize = HIDDEN_BYTES as usize;

    if contract.layer_start >= contract.layer_end {
        return Err(format!(
            "qwen3_range_forward_contract_empty:start={}:end={}",
            contract.layer_start, contract.layer_end
        ));
    }
    let layer_start = u64::from(contract.layer_start);
    let layer_end = u64::from(contract.layer_end);
    let node = u64::from(contract.node);
    let next_node = u64::from(contract.next_node);
    let pipeline_nodes = u64::from(contract.pipeline_nodes);
    let total_layers = u64::from(contract.total_layers);
    let hidden_bytes = u64::from(contract.hidden_bytes);
    if layer_end > QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        return Err(format!(
            "qwen3_range_forward_contract_layer_oob:end={}:layers={}",
            contract.layer_end, QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
        ));
    }

    let prompt_token_ids = qwen3_dense_0_6b_guest_input_token_ids(guest_input);
    let weights_path = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").ok();
    let loaded_weights = weights_path
        .as_deref()
        .map(qwen3_dense_0_6b_cached_loaded_weights)
        .transpose()?;
    let input_embedding = qwen3_dense_0_6b_real_input_embedding_summary(&prompt_token_ids)?;
    let range_input_payload = guest_input
        .get(RANGE_INPUT_PAYLOAD_OFFSET..RANGE_INPUT_PAYLOAD_OFFSET + RANGE_INPUT_PAYLOAD_BYTES)
        .filter(|payload| payload.iter().any(|byte| *byte != 0));
    let real_qkv_layer_summaries = qwen3_dense_0_6b_real_qkv_layer_summaries(topology)?;
    let real_mlp_layer_summaries = qwen3_dense_0_6b_real_mlp_layer_summaries(topology)?;
    let mut layer_words = Vec::new();
    let mut range_input_tensor_checksum = 0;
    let mut first_layer_output_checksum = 0;
    let mut final_layer_output_checksum = 0;
    let mut real_layer_execution_count = 0;
    let mut output_tensor_payload = Vec::new();
    let mut kv_state_payload = Vec::new();

    if let Some(loaded) = loaded_weights.as_ref() {
        let full_forward_with_kv =
            forward_with_kv_cache_from_token_ids(&loaded.tensors, &prompt_token_ids)?;
        kv_state_payload = qwen3_dense_0_6b_range_kv_payload_from_cache(
            &full_forward_with_kv.kv_cache,
            layer_start,
            layer_end,
        )?;
        let mut previous_sequence = if layer_start > 0 {
            let Some(payload) = range_input_payload else {
                return Err(format!(
                    "qwen3_range_forward_input_payload_missing:node={}:start={}:offset={}:bytes={}",
                    contract.node,
                    contract.layer_start,
                    RANGE_INPUT_PAYLOAD_OFFSET,
                    RANGE_INPUT_PAYLOAD_BYTES
                ));
            };
            qwen3_dense_0_6b_hidden_sequence_from_range_payload(payload, prompt_token_ids.len())?
        } else {
            embedding_reference_hidden_sequence_with_payloads(
                &loaded.tensors,
                None,
                &prompt_token_ids,
            )?
        };

        for layer_id in layer_start..layer_end {
            let owner_node = qwen3_dense_0_6b_hidden_layer_owner_node(layer_id);
            let qkv_summary = real_qkv_layer_summaries.get(&layer_id);
            let mlp_summary = real_mlp_layer_summaries.get(&layer_id);
            let qkv_checksum = qkv_summary
                .map(|summary| summary.aggregate_checksum)
                .unwrap_or(0);
            let mlp_checksum = mlp_summary
                .map(|summary| summary.aggregate_checksum)
                .unwrap_or(0);
            let input_tensor_checksum =
                qwen3_dense_0_6b_hidden_sequence_checksum(&previous_sequence);
            let (next_sequence, layer_reference) = layer_forward_reference_sequence_with_payloads(
                &loaded.tensors,
                &BTreeMap::new(),
                layer_id,
                &previous_sequence,
            )?;
            let output_tensor_checksum = qwen3_dense_0_6b_hidden_sequence_checksum(&next_sequence);

            if layer_id == layer_start {
                range_input_tensor_checksum = input_tensor_checksum;
            }
            real_layer_execution_count += 1;
            if layer_id == layer_start {
                first_layer_output_checksum = output_tensor_checksum;
            }
            final_layer_output_checksum = output_tensor_checksum;
            if layer_id + 1 == layer_end {
                output_tensor_payload = qwen3_dense_0_6b_range_payload_from_hidden_sequence(
                    &next_sequence,
                    RANGE_INPUT_PAYLOAD_BYTES,
                )?;
            }
            layer_words.extend_from_slice(&[
                layer_id,
                owner_node,
                layer_reference.input_checksum,
                qkv_checksum,
                mlp_checksum,
                layer_reference.output_checksum,
                output_tensor_checksum,
            ]);
            previous_sequence = next_sequence;
        }
    } else {
        let mut previous_hidden_tensor = if layer_start > 0 {
            let Some(payload) = range_input_payload else {
                return Err(format!(
                    "qwen3_range_forward_input_payload_missing:node={}:start={}:offset={}:bytes={}",
                    contract.node,
                    contract.layer_start,
                    RANGE_INPUT_PAYLOAD_OFFSET,
                    RANGE_INPUT_PAYLOAD_BYTES
                ));
            };
            payload.to_vec()
        } else if let Some(hidden) = real_input_embedding_hidden {
            let mut bytes = Vec::with_capacity(hidden.len() * std::mem::size_of::<f32>());
            for value in hidden {
                bytes.extend_from_slice(&value.to_le_bytes());
            }
            qwen3_dense_0_6b_f32_tile_to_half_input(&bytes, MATMUL_ELEMS)
        } else {
            let guest_input_checksum = qwen3_dense_0_6b_decode_guest_input_checksum(guest_input);
            let embedding_checksum = input_embedding
                .as_ref()
                .map(|summary| summary.aggregate_checksum)
                .unwrap_or(0);
            let seed = checksum_words(&[
                guest_input_checksum,
                embedding_checksum,
                result_flow_checksum,
            ]);
            let mut out = Vec::with_capacity(MATMUL_ELEMS * std::mem::size_of::<u16>());
            for elem_index in 0..MATMUL_ELEMS {
                let mixed = seed.rotate_left((elem_index % 63) as u32) ^ elem_index as u64;
                let value = 1.0 + (((mixed >> 8) & 0x03ff) as f32 / 1024.0) * 0.01;
                out.extend_from_slice(&f32_to_f16_bits(value).to_le_bytes());
            }
            out
        };
        for layer_id in layer_start..layer_end {
            let owner_node = qwen3_dense_0_6b_hidden_layer_owner_node(layer_id);
            let qkv_summary = real_qkv_layer_summaries.get(&layer_id);
            let mlp_summary = real_mlp_layer_summaries.get(&layer_id);
            let qkv_checksum = qkv_summary
                .map(|summary| summary.aggregate_checksum)
                .unwrap_or(0);
            let mlp_checksum = mlp_summary
                .map(|summary| summary.aggregate_checksum)
                .unwrap_or(0);
            let input_tensor_checksum =
                qwen3_dense_0_6b_shard_output_checksum(&previous_hidden_tensor);
            let output_hidden_tensor = qwen3_dense_0_6b_next_hidden_tensor_tile(
                &previous_hidden_tensor,
                MATMUL_DIM,
                layer_id,
                owner_node,
                qkv_checksum,
                mlp_checksum,
                result_flow_checksum,
                qkv_summary,
                mlp_summary,
            );
            let output_tensor_checksum =
                qwen3_dense_0_6b_shard_output_checksum(&output_hidden_tensor);

            if layer_id == layer_start {
                range_input_tensor_checksum = input_tensor_checksum;
            }
            if qkv_summary.is_some() && mlp_summary.is_some() {
                real_layer_execution_count += 1;
            }
            if layer_id == layer_start {
                first_layer_output_checksum = output_tensor_checksum;
            }
            final_layer_output_checksum = output_tensor_checksum;
            if layer_id + 1 == layer_end {
                output_tensor_payload = output_hidden_tensor.clone();
            }
            layer_words.extend_from_slice(&[
                layer_id,
                owner_node,
                input_tensor_checksum,
                qkv_checksum,
                mlp_checksum,
                output_tensor_checksum,
            ]);
            previous_hidden_tensor = output_hidden_tensor;
        }
    }

    Ok(Qwen3Dense06bRangeForwardSummary {
        node,
        layer_start,
        layer_end,
        layer_count: layer_end - layer_start,
        next_node,
        pipeline_nodes,
        total_layers,
        hidden_bytes,
        input_tensor_checksum: range_input_tensor_checksum,
        output_tensor_checksum: final_layer_output_checksum,
        range_layer_checksum: checksum_words(&layer_words),
        real_layer_execution_count,
        first_layer_output_checksum,
        final_layer_output_checksum,
        input_tensor_bytes: HIDDEN_BYTES,
        output_tensor_bytes: HIDDEN_BYTES,
        output_tensor_payload,
        kv_state_bytes: kv_state_payload.len() as u64,
        kv_state_checksum: qwen3_dense_0_6b_range_object_payload_checksum(&kv_state_payload),
        kv_state_payload,
    })
}

fn qwen3_dense_0_6b_range_forward_summary_from_runtime_outputs(
    contract: Qwen3GuestRangeComputeContract,
    guest_input: &[u8],
    real_input_embedding_hidden: Option<&[f32]>,
    round1_outputs: &[(Qwen3Dense06bShard, Vec<u8>, SegmentHandle, u64)],
    result_flow_checksum: u64,
) -> Result<Qwen3Dense06bRangeForwardSummary, String> {
    const HIDDEN_BYTES: u64 = 262_144;
    const RANGE_INPUT_PAYLOAD_OFFSET: usize = 0x08_0000;
    const RANGE_INPUT_PAYLOAD_BYTES: usize = HIDDEN_BYTES as usize;

    if contract.layer_start >= contract.layer_end {
        return Err(format!(
            "qwen3_range_forward_contract_empty:start={}:end={}",
            contract.layer_start, contract.layer_end
        ));
    }
    let layer_start = u64::from(contract.layer_start);
    let layer_end = u64::from(contract.layer_end);
    if layer_end > QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        return Err(format!(
            "qwen3_range_forward_contract_layer_oob:end={}:layers={}",
            contract.layer_end, QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
        ));
    }

    let prompt_token_ids = qwen3_dense_0_6b_guest_input_token_ids(guest_input);
    let token_count = prompt_token_ids.len().max(1);
    let output_tensor_payload = qwen3_dense_0_6b_range_payload_from_round_outputs(
        round1_outputs,
        token_count,
        RANGE_INPUT_PAYLOAD_BYTES,
    )?;
    let input_tensor_checksum = if layer_start > 0 {
        let payload = guest_input
            .get(RANGE_INPUT_PAYLOAD_OFFSET..RANGE_INPUT_PAYLOAD_OFFSET + RANGE_INPUT_PAYLOAD_BYTES)
            .filter(|payload| payload.iter().any(|byte| *byte != 0))
            .ok_or_else(|| {
                format!(
                    "qwen3_range_forward_input_payload_missing:node={}:start={}:offset={}:bytes={}",
                    contract.node,
                    contract.layer_start,
                    RANGE_INPUT_PAYLOAD_OFFSET,
                    RANGE_INPUT_PAYLOAD_BYTES
                )
            })?;
        qwen3_dense_0_6b_shard_output_checksum(payload)
    } else if let Some(hidden) = real_input_embedding_hidden {
        qwen3_dense_0_6b_f32_values_checksum(hidden)
    } else {
        checksum_words(&[
            qwen3_dense_0_6b_decode_guest_input_checksum(guest_input),
            result_flow_checksum,
            token_count as u64,
        ])
    };
    let output_tensor_checksum = qwen3_dense_0_6b_shard_output_checksum(&output_tensor_payload);
    let mut layer_words = Vec::with_capacity((layer_end - layer_start) as usize * 6 + 4);
    let mut previous_checksum = input_tensor_checksum;
    for layer_id in layer_start..layer_end {
        let owner_node = qwen3_dense_0_6b_hidden_layer_owner_node(layer_id);
        let qkv_checksum = checksum_words(&[
            previous_checksum,
            output_tensor_checksum,
            result_flow_checksum,
            layer_id,
            owner_node,
            0x716b_765f_7274_0001,
        ]);
        let mlp_checksum = checksum_words(&[
            previous_checksum,
            output_tensor_checksum,
            result_flow_checksum,
            layer_id,
            owner_node,
            0x6d6c_705f_7274_0001,
        ]);
        let layer_output_checksum = if layer_id + 1 == layer_end {
            output_tensor_checksum
        } else {
            checksum_words(&[
                previous_checksum,
                qkv_checksum,
                mlp_checksum,
                result_flow_checksum,
                layer_id,
            ])
        };
        layer_words.extend_from_slice(&[
            layer_id,
            owner_node,
            previous_checksum,
            qkv_checksum,
            mlp_checksum,
            layer_output_checksum,
        ]);
        previous_checksum = layer_output_checksum;
    }

    Ok(Qwen3Dense06bRangeForwardSummary {
        node: u64::from(contract.node),
        layer_start,
        layer_end,
        layer_count: layer_end - layer_start,
        next_node: u64::from(contract.next_node),
        pipeline_nodes: u64::from(contract.pipeline_nodes),
        total_layers: u64::from(contract.total_layers),
        hidden_bytes: u64::from(contract.hidden_bytes),
        input_tensor_checksum,
        output_tensor_checksum,
        range_layer_checksum: checksum_words(&layer_words),
        real_layer_execution_count: layer_end - layer_start,
        first_layer_output_checksum: layer_words
            .get(5)
            .copied()
            .unwrap_or(output_tensor_checksum),
        final_layer_output_checksum: output_tensor_checksum,
        input_tensor_bytes: HIDDEN_BYTES,
        output_tensor_bytes: HIDDEN_BYTES,
        output_tensor_payload,
        kv_state_bytes: 0,
        kv_state_checksum: 0,
        kv_state_payload: Vec::new(),
    })
}

fn qwen3_dense_0_6b_range_payload_from_round_outputs(
    round1_outputs: &[(Qwen3Dense06bShard, Vec<u8>, SegmentHandle, u64)],
    token_count: usize,
    capacity_bytes: usize,
) -> Result<Vec<u8>, String> {
    let hidden_size = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize;
    if round1_outputs.is_empty() {
        return Err("qwen3_range_forward_runtime_outputs_empty".to_string());
    }
    if hidden_size % round1_outputs.len() != 0 {
        return Err(format!(
            "qwen3_range_forward_runtime_tile_count_mismatch:hidden_size={hidden_size}:tiles={}",
            round1_outputs.len()
        ));
    }
    let values_per_tile = hidden_size / round1_outputs.len();
    let required_bytes = token_count * hidden_size * std::mem::size_of::<f32>();
    if required_bytes > capacity_bytes {
        return Err(format!(
            "qwen3_range_forward_runtime_payload_too_large:tokens={token_count}:bytes={required_bytes}:capacity={capacity_bytes}"
        ));
    }
    let mut outputs = round1_outputs.iter().collect::<Vec<_>>();
    outputs.sort_by_key(|(shard, _output, _segment, _checksum)| shard.kv_block_start / 2);

    let mut payload = Vec::with_capacity(capacity_bytes);
    for token_index in 0..token_count {
        let value_start = token_index * values_per_tile;
        let value_end = value_start + values_per_tile;
        for (shard, output, _segment, _checksum) in &outputs {
            let values = bytes_to_f32s(output);
            if values.len() < value_end {
                return Err(format!(
                    "qwen3_range_forward_runtime_tile_too_short:tile={}:token={token_index}:got={}:expected_at_least={value_end}",
                    shard.kv_block_start / 2,
                    values.len()
                ));
            }
            for value in &values[value_start..value_end] {
                if !value.is_finite() {
                    return Err(format!(
                        "qwen3_range_forward_runtime_tile_nonfinite:tile={}:token={token_index}",
                        shard.kv_block_start / 2
                    ));
                }
                payload.extend_from_slice(&value.to_le_bytes());
            }
        }
    }
    payload.resize(capacity_bytes, 0);
    Ok(payload)
}

fn qwen3_dense_0_6b_range_object_payload_checksum(bytes: &[u8]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for (index, byte) in bytes.iter().copied().enumerate() {
        acc ^= u64::from(byte) | ((index as u64) << 8);
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn qwen3_dense_0_6b_range_kv_payload_from_cache(
    cache: &[Qwen3Dense06bLayerKvCache],
    layer_start: u64,
    layer_end: u64,
) -> Result<Vec<u8>, String> {
    let mut payload = Vec::new();
    for layer_id in layer_start..layer_end {
        let layer = cache
            .get(layer_id as usize)
            .ok_or_else(|| format!("qwen3_range_kv_layer_missing:layer={layer_id}"))?;
        if layer.layer_id != layer_id {
            return Err(format!(
                "qwen3_range_kv_layer_id_mismatch:expected={layer_id}:got={}",
                layer.layer_id
            ));
        }
        payload.extend_from_slice(&layer.layer_id.to_le_bytes());
        payload.extend_from_slice(&layer.token_count.to_le_bytes());
        payload.extend_from_slice(&(layer.rope_k_states.len() as u64).to_le_bytes());
        payload.extend_from_slice(&(layer.v_states.len() as u64).to_le_bytes());
        let state_len = layer
            .rope_k_states
            .first()
            .map(|state| state.len())
            .or_else(|| layer.v_states.first().map(|state| state.len()))
            .unwrap_or(0);
        payload.extend_from_slice(&(state_len as u64).to_le_bytes());
        for state in &layer.rope_k_states {
            if state.len() != state_len {
                return Err(format!(
                    "qwen3_range_kv_k_state_len_mismatch:layer={layer_id}:got={}:expected={state_len}",
                    state.len()
                ));
            }
            for value in state {
                payload.extend_from_slice(&value.to_le_bytes());
            }
        }
        for state in &layer.v_states {
            if state.len() != state_len {
                return Err(format!(
                    "qwen3_range_kv_v_state_len_mismatch:layer={layer_id}:got={}:expected={state_len}",
                    state.len()
                ));
            }
            for value in state {
                payload.extend_from_slice(&value.to_le_bytes());
            }
        }
    }
    Ok(payload)
}

fn qwen3_dense_0_6b_write_service_flow_markers(
    output: &mut [u8],
    round0_publish_count: u64,
    round1_resolve_count: u64,
    round1_compute_count: u64,
    shard_output_bytes: u64,
    shard_output_elems: u64,
    round0_checksums: &[u64],
    round1_checksums: &[u64],
    result_descriptors: &[Qwen3Dense06bResultDescriptor],
    result_block_descriptors: &[Qwen3Dense06bResultBlockDescriptor],
    projection_descriptors: &[Qwen3Dense06bProjectionDescriptor],
    layer_dependency_descriptors: &[Qwen3Dense06bLayerDependencyDescriptor],
    kvcache_descriptors: &[Qwen3Dense06bKvCacheDescriptor],
    logits_descriptors: &[Qwen3Dense06bLogitsDescriptor],
    real_tokenizer_path: Option<&Path>,
    real_tokenizer_asset_summary: Option<&Qwen3Dense06bTokenizerAssetSummary>,
    real_weight_reference_summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
    real_mlp_reference_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
    next_layer_real_mlp_reference_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
    real_logits_reference_summary: Option<&Qwen3Dense06bLogitsReferenceSummary>,
    real_weight_stage_links: &[Qwen3Dense06bRealWeightStageLinkDescriptor],
    range_forward_summary: Option<&Qwen3Dense06bRangeForwardSummary>,
) {
    const TILES_PER_SHARD: usize = 2;
    const LAYER_DEP_STAGES_PER_TILE: usize = 24;
    const MARKER_PUBLISH: u64 = 0x7133773470756230;
    const MARKER_RESOLVE: u64 = 0x7133773472657331;
    const MARKER_COMPUTE: u64 = 0x71337734636d7031;
    const MARKER_SHARD_SUMMARY: u64 = 0x7133773473686430;
    const MARKER_ROUND1_SUMMARY: u64 = 0x7133773472643130;
    const MARKER_RESULT_TABLE: u64 = 0x7133773474626c30;
    const MARKER_PROJECTION_TABLE: u64 = 0x7133773471767430;
    const MARKER_LAYER_DEP_TABLE: u64 = 0x7133773464657030;
    const MARKER_RESULT_BLOCK_TABLE: u64 = 0x71337734626c6b30;
    const MARKER_KVCACHE_TABLE: u64 = 0x713377346b766330;
    const MARKER_KVCACHE_STATE_TABLE: u64 = 0x713377346b767331;
    const MARKER_LOGITS_TABLE: u64 = 0x713377346c6f6730;
    const MARKER_TOKEN_TEXT_TABLE: u64 = 0x7133773474787430;
    const MARKER_TEXT_OUTPUT_TABLE: u64 = 0x71337734746f7430;
    const MARKER_TEXT_OUTPUT_BYTES_TABLE: u64 = 0x71337734746f6230;
    const MARKER_TOKENIZER_ASSET_TABLE: u64 = 0x71337734746f6b30;
    const MARKER_WEIGHT_REFERENCE_TABLE: u64 = 0x7133773477667430;
    const MARKER_WEIGHT_STAGE_LINK_TABLE: u64 = 0x71337734776c6b30;
    const MARKER_MLP_REFERENCE_TABLE: u64 = 0x713377346d6c7030;
    const MARKER_LOGITS_REFERENCE_TABLE: u64 = 0x713377346c6d6830;
    const MARKER_RANGE_FORWARD_TABLE: u64 = 0x7133773472667430;
    const RESULT_TABLE_HEADER: usize = 320;
    const RESULT_TABLE_BASE: usize = 384;
    const RESULT_TABLE_ENTRY_WORDS: u64 = 10;
    const RESULT_TABLE_ENTRY_BYTES: usize =
        RESULT_TABLE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    const PROJECTION_TABLE_HEADER: usize = 1664;
    const PROJECTION_TABLE_BASE: usize = 1728;
    const PROJECTION_TABLE_ENTRY_WORDS: u64 = 10;
    const PROJECTION_TABLE_ENTRY_BYTES: usize =
        PROJECTION_TABLE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    const LAYER_DEP_TABLE_HEADER: usize = 5568;
    const LAYER_DEP_TABLE_BASE: usize = 5632;
    const LAYER_DEP_TABLE_ENTRY_WORDS: u64 = 11;
    const LAYER_DEP_TABLE_ENTRY_BYTES: usize =
        LAYER_DEP_TABLE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    const RESULT_BLOCK_TABLE_HEADER: usize = LAYER_DEP_TABLE_BASE
        + (QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize
            * TILES_PER_SHARD
            * LAYER_DEP_STAGES_PER_TILE
            * LAYER_DEP_TABLE_ENTRY_BYTES);
    const RESULT_BLOCK_TABLE_BASE: usize = RESULT_BLOCK_TABLE_HEADER + 64;
    const RESULT_BLOCK_TABLE_ENTRY_WORDS: u64 = 16;
    const RESULT_BLOCK_TABLE_ENTRY_BYTES: usize =
        RESULT_BLOCK_TABLE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    const KVCACHE_TABLE_HEADER: usize = RESULT_BLOCK_TABLE_BASE
        + (QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize
            * TILES_PER_SHARD
            * 2
            * RESULT_BLOCK_TABLE_ENTRY_BYTES);
    const KVCACHE_TABLE_BASE: usize = KVCACHE_TABLE_HEADER + 64;
    const KVCACHE_TABLE_ENTRY_WORDS: u64 = 14;
    const KVCACHE_TABLE_ENTRY_BYTES: usize =
        KVCACHE_TABLE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    const KVCACHE_STATE_TABLE_HEADER_BYTES: usize = 64;
    const KVCACHE_STATE_TABLE_ENTRY_WORDS: u64 = 8;
    const KVCACHE_STATE_TABLE_ENTRY_BYTES: usize =
        KVCACHE_STATE_TABLE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    const LOGITS_TABLE_HEADER_BYTES: usize = 64;
    const LOGITS_TABLE_ENTRY_WORDS: u64 = 45;
    const LOGITS_TABLE_ENTRY_BYTES: usize =
        LOGITS_TABLE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    const TOKEN_TEXT_TABLE_HEADER_BYTES: usize = 64;
    const TOKEN_TEXT_TABLE_ENTRY_WORDS: u64 = 8;
    const TOKEN_TEXT_TABLE_ENTRY_BYTES: usize =
        TOKEN_TEXT_TABLE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    const TEXT_OUTPUT_TABLE_HEADER_BYTES: usize = 64;
    const TEXT_OUTPUT_BYTES_TABLE_HEADER_BYTES: usize = 64;
    let result_block_table_bytes = result_block_descriptors.len() * RESULT_BLOCK_TABLE_ENTRY_BYTES;
    let result_block_table_end = RESULT_BLOCK_TABLE_BASE + result_block_table_bytes;
    let kvcache_table_bytes = kvcache_descriptors.len() * KVCACHE_TABLE_ENTRY_BYTES;
    let kvcache_state_descriptors = qwen3_dense_0_6b_kvcache_state_descriptors(kvcache_descriptors);
    let kvcache_state_table_header = KVCACHE_TABLE_BASE + kvcache_table_bytes;
    let kvcache_state_table_base = kvcache_state_table_header + KVCACHE_STATE_TABLE_HEADER_BYTES;
    let kvcache_state_table_bytes =
        kvcache_state_descriptors.len() * KVCACHE_STATE_TABLE_ENTRY_BYTES;
    let logits_table_header = kvcache_state_table_base + kvcache_state_table_bytes;
    let logits_table_base = logits_table_header + LOGITS_TABLE_HEADER_BYTES;
    let logits_table_bytes = logits_descriptors.len() * LOGITS_TABLE_ENTRY_BYTES;
    let token_text_table_header = logits_table_base + logits_table_bytes;
    let token_text_table_base = token_text_table_header + TOKEN_TEXT_TABLE_HEADER_BYTES;
    let token_text_table_bytes = logits_descriptors.len() * TOKEN_TEXT_TABLE_ENTRY_BYTES;
    let text_output_table_header = token_text_table_base + token_text_table_bytes;
    let text_output_bytes =
        qwen3_dense_0_6b_text_output_bytes(logits_descriptors, real_tokenizer_path);
    let text_output_bytes_table_header = text_output_table_header + TEXT_OUTPUT_TABLE_HEADER_BYTES;
    let text_output_bytes_table_base =
        text_output_bytes_table_header + TEXT_OUTPUT_BYTES_TABLE_HEADER_BYTES;
    let text_output_bytes_table_bytes = (text_output_bytes.len() + 7) & !7;
    let tokenizer_asset_table_header = text_output_bytes_table_base + text_output_bytes_table_bytes;
    let tokenizer_asset_table_end = qwen3_dense_0_6b_tokenizer_asset_table_end(
        tokenizer_asset_table_header,
        real_tokenizer_asset_summary,
    );
    let weight_reference_table_header = tokenizer_asset_table_end;
    let weight_reference_table_end = qwen3_dense_0_6b_weight_reference_table_end(
        weight_reference_table_header,
        real_weight_reference_summary,
    );
    let weight_stage_link_table_header = weight_reference_table_end;
    let weight_stage_link_table_end = qwen3_dense_0_6b_weight_stage_link_table_end(
        weight_stage_link_table_header,
        real_weight_stage_links,
    );
    let mlp_reference_table_header = weight_stage_link_table_end;
    let mlp_reference_table_end = qwen3_dense_0_6b_mlp_reference_table_end(
        mlp_reference_table_header,
        real_mlp_reference_summary,
        next_layer_real_mlp_reference_summary,
    );
    let logits_reference_table_header = mlp_reference_table_end;
    let logits_reference_table_end = qwen3_dense_0_6b_logits_reference_table_end(
        logits_reference_table_header,
        real_logits_reference_summary,
    );
    let range_forward_table_header = logits_reference_table_end;
    let range_forward_table_end =
        qwen3_dense_0_6b_range_forward_table_end(range_forward_table_header, range_forward_summary);
    let metadata_table_end = range_forward_table_end;

    write_u64_le_at(output, 8, MARKER_PUBLISH);
    write_u64_le_at(output, 16, MARKER_RESOLVE);
    write_u64_le_at(output, 24, MARKER_COMPUTE);
    write_u64_le_at(output, 32, round0_publish_count);
    write_u64_le_at(output, 40, round1_resolve_count);
    write_u64_le_at(output, 48, round1_compute_count);
    write_u64_le_at(output, 56, MARKER_SHARD_SUMMARY);
    write_u64_le_at(output, 64, round0_checksums.len() as u64);
    write_u64_le_at(output, 72, shard_output_bytes);
    write_u64_le_at(output, 80, shard_output_elems);
    write_u64_le_at(output, 88, 2);
    write_u64_le_at(output, 96, MARKER_ROUND1_SUMMARY);
    write_u64_le_at(output, 104, round1_checksums.len() as u64);
    write_u64_le_at(output, 112, distinct_checksum_count(round0_checksums));
    write_u64_le_at(output, 120, distinct_checksum_count(round1_checksums));
    for (index, checksum) in round0_checksums.iter().enumerate() {
        write_u64_le_at(output, 128 + index * std::mem::size_of::<u64>(), *checksum);
    }
    write_u64_le_at(output, RESULT_TABLE_HEADER, MARKER_RESULT_TABLE);
    write_u64_le_at(
        output,
        RESULT_TABLE_HEADER + 8,
        result_descriptors.len() as u64,
    );
    write_u64_le_at(output, RESULT_TABLE_HEADER + 16, RESULT_TABLE_ENTRY_WORDS);
    write_u64_le_at(
        output,
        RESULT_TABLE_HEADER + 24,
        (result_descriptors.len() * RESULT_TABLE_ENTRY_BYTES) as u64,
    );
    for (index, descriptor) in result_descriptors.iter().enumerate() {
        let base = RESULT_TABLE_BASE + index * RESULT_TABLE_ENTRY_BYTES;
        write_u64_le_at(output, base, descriptor.shard_id);
        write_u64_le_at(output, base + 8, descriptor.owner_node);
        write_u64_le_at(output, base + 16, descriptor.target_node);
        write_u64_le_at(output, base + 24, descriptor.tile_id);
        write_u64_le_at(output, base + 32, descriptor.kv_block_start);
        write_u64_le_at(output, base + 40, descriptor.kv_block_end);
        write_u64_le_at(output, base + 48, descriptor.round0_segment);
        write_u64_le_at(output, base + 56, descriptor.round1_segment);
        write_u64_le_at(output, base + 64, descriptor.round0_checksum);
        write_u64_le_at(output, base + 72, descriptor.round1_checksum);
    }
    write_u64_le_at(output, PROJECTION_TABLE_HEADER, MARKER_PROJECTION_TABLE);
    write_u64_le_at(
        output,
        PROJECTION_TABLE_HEADER + 8,
        projection_descriptors.len() as u64,
    );
    write_u64_le_at(
        output,
        PROJECTION_TABLE_HEADER + 16,
        PROJECTION_TABLE_ENTRY_WORDS,
    );
    write_u64_le_at(
        output,
        PROJECTION_TABLE_HEADER + 24,
        (projection_descriptors.len() * PROJECTION_TABLE_ENTRY_BYTES) as u64,
    );
    for (index, descriptor) in projection_descriptors.iter().enumerate() {
        let base = PROJECTION_TABLE_BASE + index * PROJECTION_TABLE_ENTRY_BYTES;
        write_u64_le_at(output, base, descriptor.shard_id);
        write_u64_le_at(output, base + 8, descriptor.projection_kind);
        write_u64_le_at(output, base + 16, descriptor.segment);
        write_u64_le_at(output, base + 24, descriptor.elems);
        write_u64_le_at(output, base + 32, descriptor.bytes);
        write_u64_le_at(output, base + 40, descriptor.head_start);
        write_u64_le_at(output, base + 48, descriptor.head_end);
        write_u64_le_at(output, base + 56, descriptor.kv_block_start);
        write_u64_le_at(output, base + 64, descriptor.kv_block_end);
        write_u64_le_at(output, base + 72, descriptor.checksum);
    }
    write_u64_le_at(output, LAYER_DEP_TABLE_HEADER, MARKER_LAYER_DEP_TABLE);
    write_u64_le_at(
        output,
        LAYER_DEP_TABLE_HEADER + 8,
        layer_dependency_descriptors.len() as u64,
    );
    write_u64_le_at(
        output,
        LAYER_DEP_TABLE_HEADER + 16,
        LAYER_DEP_TABLE_ENTRY_WORDS,
    );
    write_u64_le_at(
        output,
        LAYER_DEP_TABLE_HEADER + 24,
        (layer_dependency_descriptors.len() * LAYER_DEP_TABLE_ENTRY_BYTES) as u64,
    );
    for (index, descriptor) in layer_dependency_descriptors.iter().enumerate() {
        let base = LAYER_DEP_TABLE_BASE + index * LAYER_DEP_TABLE_ENTRY_BYTES;
        write_u64_le_at(output, base, descriptor.layer_id);
        write_u64_le_at(output, base + 8, descriptor.shard_id);
        write_u64_le_at(output, base + 16, descriptor.stage_kind);
        write_u64_le_at(output, base + 24, descriptor.depends_on_stage);
        write_u64_le_at(output, base + 32, descriptor.remote_shard_id);
        write_u64_le_at(output, base + 40, descriptor.segment);
        write_u64_le_at(output, base + 48, descriptor.elems);
        write_u64_le_at(output, base + 56, descriptor.bytes);
        write_u64_le_at(output, base + 64, descriptor.head_start);
        write_u64_le_at(output, base + 72, descriptor.head_end);
        write_u64_le_at(output, base + 80, descriptor.checksum);
    }
    debug_assert_ne!(
        qwen3_dense_0_6b_result_block_layout_checksum(result_block_descriptors),
        0
    );
    if result_block_table_end <= output.len() {
        output[RESULT_BLOCK_TABLE_HEADER..result_block_table_end].fill(0);
        write_u64_le_at(output, RESULT_BLOCK_TABLE_HEADER, MARKER_RESULT_BLOCK_TABLE);
        write_u64_le_at(
            output,
            RESULT_BLOCK_TABLE_HEADER + 8,
            result_block_descriptors.len() as u64,
        );
        write_u64_le_at(
            output,
            RESULT_BLOCK_TABLE_HEADER + 16,
            RESULT_BLOCK_TABLE_ENTRY_WORDS,
        );
        write_u64_le_at(
            output,
            RESULT_BLOCK_TABLE_HEADER + 24,
            result_block_table_bytes as u64,
        );
        write_u64_le_at(
            output,
            RESULT_BLOCK_TABLE_HEADER + 32,
            metadata_table_end as u64,
        );
        write_u64_le_at(
            output,
            RESULT_BLOCK_TABLE_HEADER + 40,
            range_forward_table_header as u64,
        );
        for (index, descriptor) in result_block_descriptors.iter().enumerate() {
            let base = RESULT_BLOCK_TABLE_BASE + index * RESULT_BLOCK_TABLE_ENTRY_BYTES;
            let byte_start = descriptor.tile_id as usize * shard_output_bytes as usize
                + descriptor.row_start as usize * 128 * std::mem::size_of::<f32>();
            let byte_end = byte_start + descriptor.bytes as usize;
            let checksum = qwen3_dense_0_6b_canonical_block_checksum(
                output,
                byte_start,
                byte_end,
                RESULT_BLOCK_TABLE_HEADER,
                metadata_table_end,
            );
            write_u64_le_at(output, base, descriptor.shard_id);
            write_u64_le_at(output, base + 8, descriptor.kv_block_id);
            write_u64_le_at(output, base + 16, descriptor.tile_id);
            write_u64_le_at(output, base + 24, descriptor.row_start);
            write_u64_le_at(output, base + 32, descriptor.row_end);
            write_u64_le_at(output, base + 40, descriptor.bytes);
            write_u64_le_at(output, base + 48, checksum);
            write_u64_le_at(output, base + 56, descriptor.segment);
            for (sample_index, sample_offset) in qwen3_dense_0_6b_result_block_sample_offsets()
                .iter()
                .enumerate()
            {
                debug_assert!(sample_offset + 8 <= descriptor.bytes as usize);
                let sample_source = byte_start + sample_offset;
                let sample_value =
                    if (RESULT_BLOCK_TABLE_HEADER..metadata_table_end).contains(&sample_source) {
                        0
                    } else {
                        read_u64_le_at(output, sample_source)
                    };
                write_u64_le_at(
                    output,
                    base + 64 + sample_index * std::mem::size_of::<u64>(),
                    sample_value,
                );
            }
        }
    }
    write_u64_le_at(output, KVCACHE_TABLE_HEADER, MARKER_KVCACHE_TABLE);
    write_u64_le_at(
        output,
        KVCACHE_TABLE_HEADER + 8,
        kvcache_descriptors.len() as u64,
    );
    write_u64_le_at(output, KVCACHE_TABLE_HEADER + 16, KVCACHE_TABLE_ENTRY_WORDS);
    write_u64_le_at(
        output,
        KVCACHE_TABLE_HEADER + 24,
        kvcache_table_bytes as u64,
    );
    for (index, descriptor) in kvcache_descriptors.iter().enumerate() {
        let base = KVCACHE_TABLE_BASE + index * KVCACHE_TABLE_ENTRY_BYTES;
        write_u64_le_at(output, base, descriptor.layer_id);
        write_u64_le_at(output, base + 8, descriptor.shard_id);
        write_u64_le_at(output, base + 16, descriptor.tile_id);
        write_u64_le_at(output, base + 24, descriptor.kv_block_start);
        write_u64_le_at(output, base + 32, descriptor.kv_block_end);
        write_u64_le_at(output, base + 40, descriptor.append_position_start);
        write_u64_le_at(output, base + 48, descriptor.append_position_end);
        write_u64_le_at(output, base + 56, descriptor.read_position_start);
        write_u64_le_at(output, base + 64, descriptor.read_position_end);
        write_u64_le_at(output, base + 72, descriptor.update_seq);
        write_u64_le_at(output, base + 80, descriptor.k_segment);
        write_u64_le_at(output, base + 88, descriptor.v_segment);
        write_u64_le_at(output, base + 96, descriptor.k_checksum);
        write_u64_le_at(output, base + 104, descriptor.v_checksum);
    }
    write_u64_le_at(
        output,
        kvcache_state_table_header,
        MARKER_KVCACHE_STATE_TABLE,
    );
    write_u64_le_at(
        output,
        kvcache_state_table_header + 8,
        kvcache_state_descriptors.len() as u64,
    );
    write_u64_le_at(
        output,
        kvcache_state_table_header + 16,
        KVCACHE_STATE_TABLE_ENTRY_WORDS,
    );
    write_u64_le_at(
        output,
        kvcache_state_table_header + 24,
        kvcache_state_table_bytes as u64,
    );
    for (index, descriptor) in kvcache_state_descriptors.iter().enumerate() {
        let base = kvcache_state_table_base + index * KVCACHE_STATE_TABLE_ENTRY_BYTES;
        write_u64_le_at(output, base, descriptor.layer_id);
        write_u64_le_at(output, base + 8, descriptor.tile_id);
        write_u64_le_at(output, base + 16, descriptor.position);
        write_u64_le_at(output, base + 24, descriptor.update_seq);
        write_u64_le_at(output, base + 32, descriptor.k_checksum);
        write_u64_le_at(output, base + 40, descriptor.v_checksum);
        write_u64_le_at(output, base + 48, descriptor.read_window_end);
        write_u64_le_at(output, base + 56, descriptor.read_digest);
    }
    write_u64_le_at(output, logits_table_header, MARKER_LOGITS_TABLE);
    write_u64_le_at(
        output,
        logits_table_header + 8,
        logits_descriptors.len() as u64,
    );
    write_u64_le_at(output, logits_table_header + 16, LOGITS_TABLE_ENTRY_WORDS);
    write_u64_le_at(output, logits_table_header + 24, logits_table_bytes as u64);
    for (index, descriptor) in logits_descriptors.iter().enumerate() {
        let base = logits_table_base + index * LOGITS_TABLE_ENTRY_BYTES;
        write_u64_le_at(output, base, descriptor.shard_id);
        write_u64_le_at(output, base + 8, descriptor.tile_id);
        write_u64_le_at(output, base + 16, descriptor.segment);
        write_u64_le_at(output, base + 24, descriptor.logits_count);
        write_u64_le_at(output, base + 32, descriptor.sampled_token);
        write_u64_le_at(output, base + 40, descriptor.runner_up_token);
        write_u64_le_at(output, base + 48, descriptor.margin_milli);
        write_u64_le_at(output, base + 56, descriptor.logits_checksum);
        write_u64_le_at(output, base + 64, descriptor.text_checksum);
        write_u64_le_at(output, base + 72, descriptor.step_index);
        write_u64_le_at(output, base + 80, descriptor.kvcache_read_digest);
        write_u64_le_at(output, base + 88, descriptor.qkv_reference_digest);
        write_u64_le_at(output, base + 96, descriptor.real_path_digest);
        write_u64_le_at(
            output,
            base + 104,
            descriptor.full_vocab_checked_token_count,
        );
        write_u64_le_at(output, base + 112, descriptor.full_vocab_logits_checksum);
        write_u64_le_at(output, base + 120, descriptor.top_logit_bits);
        write_u64_le_at(output, base + 128, descriptor.runner_up_logit_bits);
        write_u64_le_at(output, base + 136, descriptor.runtime_forward_layer_count);
        write_u64_le_at(
            output,
            base + 144,
            descriptor.runtime_forward_final_hidden_checksum,
        );
        write_u64_le_at(output, base + 152, descriptor.runtime_forward_checksum);
        write_u64_le_at(output, base + 160, descriptor.candidate_count);
        for candidate_index in 0..4 {
            let candidate_base = base + 168 + candidate_index * 48;
            write_u64_le_at(
                output,
                candidate_base,
                descriptor.candidate_tokens[candidate_index],
            );
            write_u64_le_at(
                output,
                candidate_base + 8,
                descriptor.candidate_logit_bits[candidate_index],
            );
            write_u64_le_at(
                output,
                candidate_base + 16,
                descriptor.candidate_text_checksums[candidate_index],
            );
            write_u64_le_at(
                output,
                candidate_base + 24,
                descriptor.candidate_piece_bytes[candidate_index],
            );
            write_u64_le_at(
                output,
                candidate_base + 32,
                descriptor.candidate_piece_word0[candidate_index],
            );
            write_u64_le_at(
                output,
                candidate_base + 40,
                descriptor.candidate_piece_word1[candidate_index],
            );
        }
    }
    write_u64_le_at(output, token_text_table_header, MARKER_TOKEN_TEXT_TABLE);
    write_u64_le_at(
        output,
        token_text_table_header + 8,
        logits_descriptors.len() as u64,
    );
    write_u64_le_at(
        output,
        token_text_table_header + 16,
        TOKEN_TEXT_TABLE_ENTRY_WORDS,
    );
    write_u64_le_at(
        output,
        token_text_table_header + 24,
        token_text_table_bytes as u64,
    );
    write_u64_le_at(
        output,
        token_text_table_header + 32,
        logits_descriptors
            .iter()
            .filter_map(|descriptor| {
                qwen3_dense_0_6b_token_piece(descriptor.sampled_token, real_tokenizer_path)
                    .ok()
                    .map(|piece| piece.byte_len)
            })
            .sum(),
    );
    let tokenizer_policy_hash = real_tokenizer_asset_summary
        .map(|summary| summary.aggregate_checksum)
        .unwrap_or_else(|| tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE).policy_hash);
    let tokenizer_policy_kind = if real_tokenizer_asset_summary.is_some() {
        QWEN3_DENSE_0_6B_TOKENIZER_ASSET_POLICY_KIND
    } else {
        QWEN3_DENSE_0_6B_TOKENIZER_POLICY_KIND
    };
    let text_output_summary =
        qwen3_dense_0_6b_text_output_summary(logits_descriptors, real_tokenizer_path);
    write_u64_le_at(output, token_text_table_header + 40, tokenizer_policy_hash);
    write_u64_le_at(output, token_text_table_header + 48, tokenizer_policy_kind);
    for (index, descriptor) in logits_descriptors.iter().enumerate() {
        let base = token_text_table_base + index * TOKEN_TEXT_TABLE_ENTRY_BYTES;
        let piece = qwen3_dense_0_6b_token_piece(descriptor.sampled_token, real_tokenizer_path)
            .unwrap_or_else(|_| {
                token_piece_from_policy(
                    tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE),
                    descriptor.sampled_token,
                )
            });
        let boundary_flags = (descriptor.step_index == 0) as u64
            | (((index + 1) == logits_descriptors.len()) as u64) << 1;
        write_u64_le_at(output, base, descriptor.step_index);
        write_u64_le_at(output, base + 8, descriptor.sampled_token);
        write_u64_le_at(output, base + 16, descriptor.text_byte_offset);
        write_u64_le_at(output, base + 24, piece.byte_len);
        write_u64_le_at(output, base + 32, piece.word0);
        write_u64_le_at(output, base + 40, piece.word1);
        write_u64_le_at(output, base + 48, descriptor.text_checksum);
        write_u64_le_at(output, base + 56, boundary_flags);
    }
    write_u64_le_at(output, text_output_table_header, MARKER_TEXT_OUTPUT_TABLE);
    write_u64_le_at(
        output,
        text_output_table_header + 8,
        logits_descriptors.len() as u64,
    );
    write_u64_le_at(
        output,
        text_output_table_header + 16,
        text_output_summary.total_bytes,
    );
    write_u64_le_at(
        output,
        text_output_table_header + 24,
        text_output_summary.sequence_checksum,
    );
    write_u64_le_at(
        output,
        text_output_table_header + 32,
        text_output_summary.token_checksum,
    );
    write_u64_le_at(
        output,
        text_output_table_header + 40,
        text_output_summary.text_checksum,
    );
    write_u64_le_at(
        output,
        text_output_table_header + 48,
        text_output_summary.logits_checksum,
    );
    write_u64_le_at(output, text_output_table_header + 56, tokenizer_policy_kind);
    write_u64_le_at(
        output,
        text_output_bytes_table_header,
        MARKER_TEXT_OUTPUT_BYTES_TABLE,
    );
    write_u64_le_at(
        output,
        text_output_bytes_table_header + 8,
        text_output_bytes.len() as u64,
    );
    write_u64_le_at(
        output,
        text_output_bytes_table_header + 16,
        (text_output_bytes_table_bytes / std::mem::size_of::<u64>()) as u64,
    );
    write_u64_le_at(
        output,
        text_output_bytes_table_header + 24,
        text_output_bytes_table_bytes as u64,
    );
    write_u64_le_at(
        output,
        text_output_bytes_table_header + 32,
        qwen3_dense_0_6b_text_output_bytes_checksum(&text_output_bytes),
    );
    write_u64_le_at(
        output,
        text_output_bytes_table_header + 40,
        text_output_summary.sequence_checksum,
    );
    write_u64_le_at(
        output,
        text_output_bytes_table_header + 48,
        logits_descriptors.len() as u64,
    );
    write_u64_le_at(
        output,
        text_output_bytes_table_header + 56,
        tokenizer_policy_kind,
    );
    if text_output_bytes_table_base + text_output_bytes_table_bytes <= output.len() {
        output[text_output_bytes_table_base
            ..text_output_bytes_table_base + text_output_bytes_table_bytes]
            .fill(0);
        output
            [text_output_bytes_table_base..text_output_bytes_table_base + text_output_bytes.len()]
            .copy_from_slice(&text_output_bytes);
    }
    qwen3_dense_0_6b_write_tokenizer_asset_table(
        output,
        tokenizer_asset_table_header,
        MARKER_TOKENIZER_ASSET_TABLE,
        real_tokenizer_asset_summary,
    );
    if let Some(summary) = real_weight_reference_summary {
        qwen3_dense_0_6b_write_weight_reference_table(
            output,
            weight_reference_table_header,
            MARKER_WEIGHT_REFERENCE_TABLE,
            summary,
        );
    }
    qwen3_dense_0_6b_write_weight_stage_link_table(
        output,
        weight_stage_link_table_header,
        MARKER_WEIGHT_STAGE_LINK_TABLE,
        real_weight_stage_links,
    );
    qwen3_dense_0_6b_write_mlp_reference_table(
        output,
        mlp_reference_table_header,
        MARKER_MLP_REFERENCE_TABLE,
        real_mlp_reference_summary,
        next_layer_real_mlp_reference_summary,
    );
    qwen3_dense_0_6b_write_logits_reference_table(
        output,
        logits_reference_table_header,
        MARKER_LOGITS_REFERENCE_TABLE,
        real_logits_reference_summary,
    );
    qwen3_dense_0_6b_write_range_forward_table(
        output,
        range_forward_table_header,
        MARKER_RANGE_FORWARD_TABLE,
        range_forward_summary,
    );
}

fn qwen3_dense_0_6b_service_flow_output_len(
    result_descriptors: &[Qwen3Dense06bResultDescriptor],
    result_block_descriptors: &[Qwen3Dense06bResultBlockDescriptor],
    projection_descriptors: &[Qwen3Dense06bProjectionDescriptor],
    layer_dependency_descriptors: &[Qwen3Dense06bLayerDependencyDescriptor],
    kvcache_descriptors: &[Qwen3Dense06bKvCacheDescriptor],
    logits_descriptors: &[Qwen3Dense06bLogitsDescriptor],
    real_tokenizer_path: Option<&Path>,
    real_tokenizer_asset_summary: Option<&Qwen3Dense06bTokenizerAssetSummary>,
    real_weight_reference_summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
    real_mlp_reference_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
    next_layer_real_mlp_reference_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
    real_logits_reference_summary: Option<&Qwen3Dense06bLogitsReferenceSummary>,
    real_weight_stage_links: &[Qwen3Dense06bRealWeightStageLinkDescriptor],
    range_forward_summary: Option<&Qwen3Dense06bRangeForwardSummary>,
) -> usize {
    const TILES_PER_SHARD: usize = 2;
    const LAYER_DEP_STAGES_PER_TILE: usize = 24;
    const RESULT_TABLE_BASE: usize = 384;
    const RESULT_TABLE_ENTRY_BYTES: usize = 10usize * std::mem::size_of::<u64>();
    const PROJECTION_TABLE_BASE: usize = 1728;
    const PROJECTION_TABLE_ENTRY_BYTES: usize = 10usize * std::mem::size_of::<u64>();
    const LAYER_DEP_TABLE_BASE: usize = 5632;
    const LAYER_DEP_TABLE_ENTRY_BYTES: usize = 11usize * std::mem::size_of::<u64>();
    const RESULT_BLOCK_TABLE_HEADER: usize = LAYER_DEP_TABLE_BASE
        + (QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize
            * TILES_PER_SHARD
            * LAYER_DEP_STAGES_PER_TILE
            * LAYER_DEP_TABLE_ENTRY_BYTES);
    const RESULT_BLOCK_TABLE_BASE: usize = RESULT_BLOCK_TABLE_HEADER + 64;
    const RESULT_BLOCK_TABLE_ENTRY_BYTES: usize = 16usize * std::mem::size_of::<u64>();
    const KVCACHE_TABLE_BASE: usize = RESULT_BLOCK_TABLE_BASE
        + (QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize
            * TILES_PER_SHARD
            * 2
            * RESULT_BLOCK_TABLE_ENTRY_BYTES
            + 64);
    const KVCACHE_TABLE_ENTRY_BYTES: usize = 14usize * std::mem::size_of::<u64>();
    const KVCACHE_STATE_TABLE_HEADER_BYTES: usize = 64;
    const KVCACHE_STATE_TABLE_ENTRY_BYTES: usize = 8usize * std::mem::size_of::<u64>();
    const LOGITS_TABLE_HEADER_BYTES: usize = 64;
    const LOGITS_TABLE_ENTRY_BYTES: usize = 45usize * std::mem::size_of::<u64>();
    const TOKEN_TEXT_TABLE_HEADER_BYTES: usize = 64;
    const TOKEN_TEXT_TABLE_ENTRY_BYTES: usize = 8usize * std::mem::size_of::<u64>();
    const TEXT_OUTPUT_TABLE_HEADER_BYTES: usize = 64;
    const TEXT_OUTPUT_BYTES_TABLE_HEADER_BYTES: usize = 64;

    let result_table_end = RESULT_TABLE_BASE + result_descriptors.len() * RESULT_TABLE_ENTRY_BYTES;
    let projection_table_end =
        PROJECTION_TABLE_BASE + projection_descriptors.len() * PROJECTION_TABLE_ENTRY_BYTES;
    let layer_dep_end =
        LAYER_DEP_TABLE_BASE + layer_dependency_descriptors.len() * LAYER_DEP_TABLE_ENTRY_BYTES;
    let result_block_table_bytes = result_block_descriptors.len() * RESULT_BLOCK_TABLE_ENTRY_BYTES;
    let result_block_table_end = RESULT_BLOCK_TABLE_BASE + result_block_table_bytes;
    let kvcache_table_bytes = kvcache_descriptors.len() * KVCACHE_TABLE_ENTRY_BYTES;
    let kvcache_table_end = KVCACHE_TABLE_BASE + kvcache_table_bytes;
    let kvcache_state_descriptors = qwen3_dense_0_6b_kvcache_state_descriptors(kvcache_descriptors);
    let kvcache_state_table_end = kvcache_table_end
        + KVCACHE_STATE_TABLE_HEADER_BYTES
        + kvcache_state_descriptors.len() * KVCACHE_STATE_TABLE_ENTRY_BYTES;
    let logits_table_end = kvcache_state_table_end
        + LOGITS_TABLE_HEADER_BYTES
        + logits_descriptors.len() * LOGITS_TABLE_ENTRY_BYTES;
    let token_text_table_end = logits_table_end
        + TOKEN_TEXT_TABLE_HEADER_BYTES
        + logits_descriptors.len() * TOKEN_TEXT_TABLE_ENTRY_BYTES;
    let text_output_bytes =
        qwen3_dense_0_6b_text_output_bytes(logits_descriptors, real_tokenizer_path);
    let text_output_bytes_table_bytes = (text_output_bytes.len() + 7) & !7;
    let tokenizer_asset_table_end = qwen3_dense_0_6b_tokenizer_asset_table_end(
        token_text_table_end
            + TEXT_OUTPUT_TABLE_HEADER_BYTES
            + TEXT_OUTPUT_BYTES_TABLE_HEADER_BYTES
            + text_output_bytes_table_bytes,
        real_tokenizer_asset_summary,
    );
    let weight_reference_table_end = qwen3_dense_0_6b_weight_reference_table_end(
        tokenizer_asset_table_end,
        real_weight_reference_summary,
    );
    let weight_stage_link_table_end = qwen3_dense_0_6b_weight_stage_link_table_end(
        weight_reference_table_end,
        real_weight_stage_links,
    );
    let mlp_reference_table_end = qwen3_dense_0_6b_mlp_reference_table_end(
        weight_stage_link_table_end,
        real_mlp_reference_summary,
        next_layer_real_mlp_reference_summary,
    );
    let logits_reference_table_end = qwen3_dense_0_6b_logits_reference_table_end(
        mlp_reference_table_end,
        real_logits_reference_summary,
    );
    let metadata_table_end =
        qwen3_dense_0_6b_range_forward_table_end(logits_reference_table_end, range_forward_summary);
    [
        result_table_end,
        projection_table_end,
        layer_dep_end,
        result_block_table_end,
        kvcache_table_end,
        kvcache_state_table_end,
        logits_table_end,
        token_text_table_end,
        metadata_table_end,
    ]
    .into_iter()
    .max()
    .unwrap_or(metadata_table_end)
}

fn qwen3_dense_0_6b_real_weight_reference_summary(
    topology: &SimTopology,
) -> Result<Option<Qwen3Dense06bQkvReferenceLayerSummary>, String> {
    qwen3_dense_0_6b_real_weight_reference_summary_for_layer(topology, 0)
}

fn qwen3_dense_0_6b_real_weight_reference_summary_for_layer(
    topology: &SimTopology,
    layer_id: u64,
) -> Result<Option<Qwen3Dense06bQkvReferenceLayerSummary>, String> {
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(None);
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let manifest = weight_manifest_from_metadata(
        topology,
        QWEN3_DENSE_0_6B_PROFILE,
        loaded.source.clone(),
        &loaded.tensors,
    )?;
    qkv_reference_layer_summary(&manifest, &loaded.tensors, layer_id).map(Some)
}

#[cfg(test)]
fn qwen3_dense_0_6b_real_qkv_reference_values(
    topology: &SimTopology,
) -> Result<Option<Qwen3Dense06bQkvReferenceLayerValues>, String> {
    qwen3_dense_0_6b_real_qkv_reference_values_for_layer_with_hidden(topology, 0, None)
}

fn qwen3_dense_0_6b_real_qkv_reference_values_for_layer_with_hidden(
    topology: &SimTopology,
    layer_id: u64,
    hidden: Option<&[f32]>,
) -> Result<Option<Qwen3Dense06bQkvReferenceLayerValues>, String> {
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(None);
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let manifest = weight_manifest_from_metadata(
        topology,
        QWEN3_DENSE_0_6B_PROFILE,
        loaded.source.clone(),
        &loaded.tensors,
    )?;
    match hidden {
        Some(hidden) => qkv_reference_layer_values_with_hidden(
            &manifest,
            &loaded.tensors,
            layer_id,
            Some(hidden),
        )
        .map(Some),
        None => qkv_reference_layer_values(&manifest, &loaded.tensors, layer_id).map(Some),
    }
}

#[cfg(test)]
fn qwen3_dense_0_6b_real_mlp_reference_summary(
    topology: &SimTopology,
) -> Result<Option<Qwen3Dense06bMlpReferenceLayerSummary>, String> {
    qwen3_dense_0_6b_real_mlp_reference_summary_for_layer_with_hidden(topology, 0, None)
}

fn qwen3_dense_0_6b_real_mlp_reference_summary_for_layer_with_hidden(
    topology: &SimTopology,
    layer_id: u64,
    hidden: Option<&[f32]>,
) -> Result<Option<Qwen3Dense06bMlpReferenceLayerSummary>, String> {
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(None);
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let manifest = weight_manifest_from_metadata(
        topology,
        QWEN3_DENSE_0_6B_PROFILE,
        loaded.source.clone(),
        &loaded.tensors,
    )?;
    match hidden {
        Some(hidden) => mlp_reference_layer_summary_with_hidden(
            &manifest,
            &loaded.tensors,
            layer_id,
            Some(hidden),
        )
        .map(Some),
        None => mlp_reference_layer_summary(&manifest, &loaded.tensors, layer_id).map(Some),
    }
}

fn qwen3_dense_0_6b_real_logits_candidate_summary(
    round1_outputs: &[(Qwen3Dense06bShard, Vec<u8>, SegmentHandle, u64)],
    sample_token_count: u64,
    hidden: Option<&[f32]>,
) -> Result<Option<Qwen3Dense06bLogitsReferenceSummary>, String> {
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(None);
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    let token_requests =
        qwen3_dense_0_6b_logits_candidate_token_requests(round1_outputs, sample_token_count);
    match hidden {
        Some(hidden) => {
            logits_reference_summary_with_hidden(&loaded.tensors, &token_requests, Some(hidden))
                .map(Some)
        }
        None => logits_reference_summary(&loaded.tensors, &token_requests).map(Some),
    }
}

fn qwen3_dense_0_6b_runtime_full_vocab_logits_summary(
    hidden: Option<&[f32]>,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
) -> Result<Option<Qwen3Dense06bFullVocabLogitsSummary>, String> {
    let Some(hidden) = hidden else {
        return Ok(None);
    };
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok(None);
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    match tensor_payloads {
        Some(tensor_payloads) => {
            full_vocab_logits_from_hidden_with_payloads(&loaded.tensors, tensor_payloads, hidden)
                .map(Some)
        }
        None => full_vocab_logits_from_hidden(&loaded.tensors, hidden).map(Some),
    }
}

fn qwen3_dense_0_6b_runtime_forward_summary_from_guest_input(
    guest_input: &[u8],
    runtime_weight_objects: Option<&mut LingquObjectServiceStub>,
) -> Result<
    (
        Option<qwen3_dense_0_6b::Qwen3Dense06bForwardReference>,
        BTreeMap<String, Vec<u8>>,
    ),
    String,
> {
    let token_ids = qwen3_dense_0_6b_guest_input_token_ids(guest_input);
    if token_ids.is_empty() {
        return Ok((None, BTreeMap::new()));
    }
    let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
        return Ok((None, BTreeMap::new()));
    };
    let loaded = qwen3_dense_0_6b_cached_loaded_weights(&weights_path)?;
    if let Some(service) = runtime_weight_objects {
        let resolved = qwen3_dense_0_6b_resolve_runtime_weight_objects(service, 700_000)?;
        let forward = forward_from_token_ids_with_layer_payloads(
            &loaded.tensors,
            &resolved.layer_payloads,
            &token_ids,
        )
        .map(Some)?;
        Ok((forward, resolved.layer_payloads))
    } else {
        forward_from_token_ids(&loaded.tensors, &token_ids)
            .map(Some)
            .map(|forward| (forward, BTreeMap::new()))
    }
}

fn qwen3_dense_0_6b_final_hidden_from_round1_outputs(
    round1_outputs: &[(Qwen3Dense06bShard, Vec<u8>, SegmentHandle, u64)],
) -> Result<Option<Vec<f32>>, String> {
    if round1_outputs.is_empty() {
        return Ok(None);
    }
    let hidden_size = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize;
    if hidden_size % round1_outputs.len() != 0 {
        return Err(format!(
            "qwen3_final_hidden_tile_count_mismatch:hidden_size={hidden_size}:tiles={}",
            round1_outputs.len()
        ));
    }
    let values_per_tile = hidden_size / round1_outputs.len();
    let mut outputs = round1_outputs.iter().collect::<Vec<_>>();
    outputs.sort_by_key(|(shard, _output, _segment, _checksum)| shard.kv_block_start / 2);

    let mut hidden = Vec::with_capacity(hidden_size);
    for (shard, output, _segment, _checksum) in outputs {
        let values = bytes_to_f32s(output);
        if values.len() < values_per_tile {
            return Err(format!(
                "qwen3_final_hidden_tile_too_short:tile={}:got={}:expected_at_least={values_per_tile}",
                shard.kv_block_start / 2,
                values.len()
            ));
        }
        if values
            .iter()
            .take(values_per_tile)
            .any(|value| !value.is_finite())
        {
            return Err(format!(
                "qwen3_final_hidden_tile_nonfinite:tile={}",
                shard.kv_block_start / 2
            ));
        }
        hidden.extend_from_slice(&values[..values_per_tile]);
    }
    if hidden.len() != hidden_size {
        return Err(format!(
            "qwen3_final_hidden_size_mismatch:got={}:expected={hidden_size}",
            hidden.len()
        ));
    }
    Ok(Some(hidden))
}

fn qwen3_dense_0_6b_hidden_from_range_output_payload(
    summary: &Qwen3Dense06bRangeForwardSummary,
) -> Result<Vec<f32>, String> {
    let token_count = qwen3_dense_0_6b_range_payload_token_count(&summary.output_tensor_payload)?;
    let sequence = qwen3_dense_0_6b_hidden_sequence_from_range_payload(
        &summary.output_tensor_payload,
        token_count,
    )?;
    sequence.last().cloned().ok_or_else(|| {
        format!(
            "qwen3_range_forward_logits_hidden_empty:node={}:layers={}..{}",
            summary.node, summary.layer_start, summary.layer_end
        )
    })
}

fn qwen3_dense_0_6b_range_payload_token_count(payload: &[u8]) -> Result<usize, String> {
    let hidden_size = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize;
    let bytes_per_hidden = hidden_size * std::mem::size_of::<f32>();
    let used_bytes = payload
        .chunks_exact(bytes_per_hidden)
        .take_while(|chunk| chunk.iter().any(|byte| *byte != 0))
        .count()
        * bytes_per_hidden;
    if used_bytes == 0 {
        return Err("qwen3_range_forward_payload_empty".to_string());
    }
    Ok(used_bytes / bytes_per_hidden)
}

fn qwen3_dense_0_6b_hidden_sequence_from_range_payload(
    payload: &[u8],
    token_count: usize,
) -> Result<Vec<Vec<f32>>, String> {
    let hidden_size = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize;
    let bytes_per_hidden = hidden_size * std::mem::size_of::<f32>();
    let required_bytes = token_count * bytes_per_hidden;
    if payload.len() < required_bytes {
        return Err(format!(
            "qwen3_range_forward_sequence_payload_too_short:tokens={token_count}:got={}:expected_at_least={required_bytes}",
            payload.len()
        ));
    }

    let mut sequence = Vec::with_capacity(token_count);
    for token_index in 0..token_count {
        let mut hidden = Vec::with_capacity(hidden_size);
        let token_offset = token_index * bytes_per_hidden;
        for elem_index in 0..hidden_size {
            let byte_index = token_offset + elem_index * std::mem::size_of::<f32>();
            let value = f32::from_le_bytes([
                payload[byte_index],
                payload[byte_index + 1],
                payload[byte_index + 2],
                payload[byte_index + 3],
            ]);
            if !value.is_finite() {
                return Err(format!(
                    "qwen3_range_forward_sequence_payload_nonfinite:token={token_index}:elem={elem_index}"
                ));
            }
            hidden.push(value);
        }
        sequence.push(hidden);
    }
    Ok(sequence)
}

fn qwen3_dense_0_6b_range_payload_from_hidden_sequence(
    sequence: &[Vec<f32>],
    capacity_bytes: usize,
) -> Result<Vec<u8>, String> {
    let hidden_size = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize;
    let required_bytes = sequence.len() * hidden_size * std::mem::size_of::<f32>();
    if required_bytes > capacity_bytes {
        return Err(format!(
            "qwen3_range_forward_sequence_payload_too_large:tokens={}:bytes={required_bytes}:capacity={capacity_bytes}",
            sequence.len()
        ));
    }
    let mut payload = Vec::with_capacity(capacity_bytes);
    for (token_index, hidden) in sequence.iter().enumerate() {
        if hidden.len() != hidden_size {
            return Err(format!(
                "qwen3_range_forward_sequence_hidden_size_mismatch:token={token_index}:got={}:expected={hidden_size}",
                hidden.len()
            ));
        }
        for value in hidden {
            if !value.is_finite() {
                return Err(format!(
                    "qwen3_range_forward_sequence_hidden_nonfinite:token={token_index}"
                ));
            }
            payload.extend_from_slice(&value.to_le_bytes());
        }
    }
    payload.resize(capacity_bytes, 0);
    Ok(payload)
}

fn qwen3_dense_0_6b_hidden_sequence_checksum(sequence: &[Vec<f32>]) -> u64 {
    let mut words = Vec::with_capacity(sequence.len() * 5 + 1);
    words.push(sequence.len() as u64);
    for (index, hidden) in sequence.iter().enumerate() {
        words.push(index as u64);
        words.push(qwen3_dense_0_6b_f32_values_checksum(hidden));
        words.extend_from_slice(&qwen3_dense_0_6b_f32_values_sample_words(hidden));
    }
    checksum_words(&words)
}

fn qwen3_dense_0_6b_f32_values_checksum(values: &[f32]) -> u64 {
    checksum_words(
        &values
            .iter()
            .enumerate()
            .map(|(index, value)| (index as u64).rotate_left(17) ^ value.to_bits() as u64)
            .collect::<Vec<_>>(),
    )
}

fn qwen3_dense_0_6b_f32_values_sample_words(values: &[f32]) -> [u64; 4] {
    if values.is_empty() {
        return [0; 4];
    }
    let last = values.len() - 1;
    [
        values[0].to_bits() as u64,
        values[last / 3].to_bits() as u64,
        values[(last * 2) / 3].to_bits() as u64,
        values[last].to_bits() as u64,
    ]
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Qwen3Dense06bTextOutputSummary {
    total_bytes: u64,
    sequence_checksum: u64,
    token_checksum: u64,
    text_checksum: u64,
    logits_checksum: u64,
}

fn qwen3_dense_0_6b_text_output_summary(
    logits_descriptors: &[Qwen3Dense06bLogitsDescriptor],
    tokenizer_path: Option<&Path>,
) -> Qwen3Dense06bTextOutputSummary {
    let mut token_words = Vec::with_capacity(logits_descriptors.len() * 4);
    let mut text_words = Vec::with_capacity(logits_descriptors.len() * 5);
    let mut logits_words = Vec::with_capacity(logits_descriptors.len() * 5);
    let mut sequence_words = Vec::with_capacity(logits_descriptors.len() * 9);
    let mut total_bytes = 0u64;
    for descriptor in logits_descriptors {
        let piece = qwen3_dense_0_6b_token_piece(descriptor.sampled_token, tokenizer_path)
            .unwrap_or_else(|_| {
                token_piece_from_policy(
                    tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE),
                    descriptor.sampled_token,
                )
            });
        token_words.extend_from_slice(&[
            descriptor.step_index,
            descriptor.sampled_token,
            descriptor.runner_up_token,
            piece.checksum,
        ]);
        text_words.extend_from_slice(&[
            descriptor.step_index,
            descriptor.text_byte_offset,
            piece.byte_len,
            piece.word0,
            piece.word1,
        ]);
        logits_words.extend_from_slice(&[
            descriptor.step_index,
            descriptor.logits_checksum,
            descriptor.margin_milli,
            descriptor.logits_count,
            descriptor.real_path_digest,
        ]);
        sequence_words.extend_from_slice(&[
            descriptor.step_index,
            descriptor.sampled_token,
            descriptor.runner_up_token,
            descriptor.text_byte_offset,
            piece.byte_len,
            piece.checksum,
            descriptor.text_checksum,
            descriptor.logits_checksum,
            descriptor.real_path_digest,
        ]);
        total_bytes = total_bytes.wrapping_add(piece.byte_len);
    }
    sequence_words.push(total_bytes);
    Qwen3Dense06bTextOutputSummary {
        total_bytes,
        sequence_checksum: checksum_words(&sequence_words),
        token_checksum: checksum_words(&token_words),
        text_checksum: checksum_words(&text_words),
        logits_checksum: checksum_words(&logits_words),
    }
}

fn qwen3_dense_0_6b_text_output_bytes(
    logits_descriptors: &[Qwen3Dense06bLogitsDescriptor],
    tokenizer_path: Option<&Path>,
) -> Vec<u8> {
    let mut bytes = Vec::new();
    for descriptor in logits_descriptors {
        let piece = qwen3_dense_0_6b_token_piece_bytes(descriptor.sampled_token, tokenizer_path)
            .unwrap_or_else(|_| {
                token_piece_bytes_from_policy(
                    tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE),
                    descriptor.sampled_token,
                )
            });
        bytes.extend_from_slice(&piece);
    }
    bytes
}

fn qwen3_dense_0_6b_token_piece_bytes(
    sampled_token: u64,
    tokenizer_path: Option<&Path>,
) -> Result<Vec<u8>, String> {
    qwen3_dense_0_6b_token_piece_raw_bytes(sampled_token, tokenizer_path)
}

fn qwen3_dense_0_6b_text_output_bytes_checksum(bytes: &[u8]) -> u64 {
    let mut words = Vec::with_capacity(bytes.len().div_ceil(8) + 1);
    for chunk in bytes.chunks(8) {
        let mut word = [0u8; 8];
        word[..chunk.len()].copy_from_slice(chunk);
        words.push(u64::from_le_bytes(word));
    }
    words.push(bytes.len() as u64);
    checksum_words(&words)
}

fn qwen3_dense_0_6b_real_tokenizer_path() -> Option<PathBuf> {
    std::env::var("SIM_QWEN3_0_6B_TOKENIZER_PATH")
        .or_else(|_| std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH"))
        .ok()
        .map(PathBuf::from)
}

fn qwen3_dense_0_6b_real_tokenizer_asset_summary(
) -> Result<Option<Qwen3Dense06bTokenizerAssetSummary>, String> {
    let Some(tokenizer_path) = qwen3_dense_0_6b_real_tokenizer_path() else {
        return Ok(None);
    };
    load_tokenizer_asset_summary(&tokenizer_path).map(Some)
}

fn qwen3_dense_0_6b_weight_reference_table_end(
    table_header: usize,
    summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
) -> usize {
    const TABLE_HEADER_BYTES: usize = 64;
    const ENTRY_BYTES: usize =
        QWEN3_WEIGHT_REFERENCE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    summary
        .map(|summary| table_header + TABLE_HEADER_BYTES + summary.shards.len() * ENTRY_BYTES)
        .unwrap_or(table_header)
}

fn qwen3_dense_0_6b_write_weight_reference_table(
    output: &mut [u8],
    table_header: usize,
    marker: u64,
    summary: &Qwen3Dense06bQkvReferenceLayerSummary,
) {
    const TABLE_HEADER_BYTES: usize = 64;
    const ENTRY_BYTES: usize =
        QWEN3_WEIGHT_REFERENCE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    let table_base = table_header + TABLE_HEADER_BYTES;
    let table_bytes = summary.shards.len() * ENTRY_BYTES;
    write_u64_le_at(output, table_header, marker);
    write_u64_le_at(output, table_header + 8, summary.shards.len() as u64);
    write_u64_le_at(
        output,
        table_header + 16,
        QWEN3_WEIGHT_REFERENCE_ENTRY_WORDS,
    );
    write_u64_le_at(output, table_header + 24, table_bytes as u64);
    write_u64_le_at(output, table_header + 32, summary.layer_id);
    write_u64_le_at(output, table_header + 40, summary.total_weight_bytes);
    write_u64_le_at(output, table_header + 48, summary.aggregate_checksum);
    write_u64_le_at(
        output,
        table_header + 56,
        summary.total_q_rows + summary.total_k_rows + summary.total_v_rows,
    );
    for (index, shard) in summary.shards.iter().enumerate() {
        let base = table_base + index * ENTRY_BYTES;
        let slice_bytes = shard
            .weight_slices
            .iter()
            .map(|slice| slice.bytes)
            .sum::<u64>();
        let slice_checksum =
            shard
                .weight_slices
                .iter()
                .fold(0xcbf2_9ce4_8422_2325u64, |acc, slice| {
                    acc.wrapping_mul(0x0000_0100_0000_01b3)
                        ^ (slice.kind as u64).rotate_left(5)
                        ^ slice.bytes.rotate_left(17)
                        ^ slice.checksum.rotate_left(29)
                });
        write_u64_le_at(output, base, shard.shard_id);
        write_u64_le_at(output, base + 8, shard.hidden_size);
        write_u64_le_at(output, base + 16, shard.rmsnorm_checksum);
        write_u64_le_at(output, base + 24, shard.q_weight_checksum);
        write_u64_le_at(output, base + 32, shard.k_weight_checksum);
        write_u64_le_at(output, base + 40, shard.v_weight_checksum);
        write_u64_le_at(output, base + 48, shard.q_output_checksum);
        write_u64_le_at(output, base + 56, shard.k_output_checksum);
        write_u64_le_at(output, base + 64, shard.v_output_checksum);
        write_u64_le_at(output, base + 72, shard.q_rows);
        write_u64_le_at(output, base + 80, shard.k_rows);
        write_u64_le_at(output, base + 88, shard.v_rows);
        write_u64_le_at(output, base + 96, shard.weight_slices.len() as u64);
        write_u64_le_at(output, base + 104, slice_bytes ^ slice_checksum);
    }
}

const QWEN3_WEIGHT_REFERENCE_ENTRY_WORDS: u64 = 14;
const QWEN3_TOKENIZER_ASSET_ENTRY_WORDS: u64 = 4;
const QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS: u64 = 11;
const QWEN3_MLP_REFERENCE_ENTRY_WORDS: u64 = 15;
const QWEN3_LOGITS_REFERENCE_ENTRY_WORDS: u64 = 6;

fn qwen3_dense_0_6b_tokenizer_asset_table_end(
    table_header: usize,
    summary: Option<&Qwen3Dense06bTokenizerAssetSummary>,
) -> usize {
    const TABLE_HEADER_BYTES: usize = 64;
    let entry_bytes = QWEN3_TOKENIZER_ASSET_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    summary
        .map(|summary| table_header + TABLE_HEADER_BYTES + summary.files.len() * entry_bytes)
        .unwrap_or(table_header)
}

fn qwen3_dense_0_6b_write_tokenizer_asset_table(
    output: &mut [u8],
    table_header: usize,
    marker: u64,
    summary: Option<&Qwen3Dense06bTokenizerAssetSummary>,
) {
    let Some(summary) = summary else {
        return;
    };
    const TABLE_HEADER_BYTES: usize = 64;
    let entry_bytes = QWEN3_TOKENIZER_ASSET_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    let table_base = table_header + TABLE_HEADER_BYTES;
    let table_bytes = summary.files.len() * entry_bytes;
    write_u64_le_at(output, table_header, marker);
    write_u64_le_at(output, table_header + 8, summary.files.len() as u64);
    write_u64_le_at(output, table_header + 16, QWEN3_TOKENIZER_ASSET_ENTRY_WORDS);
    write_u64_le_at(output, table_header + 24, table_bytes as u64);
    write_u64_le_at(output, table_header + 32, summary.aggregate_checksum);
    write_u64_le_at(output, table_header + 40, summary.vocab_size);
    write_u64_le_at(output, table_header + 48, summary.vocab_entries);
    write_u64_le_at(
        output,
        table_header + 56,
        summary.added_tokens ^ summary.merge_rules.rotate_left(32),
    );
    for (index, file) in summary.files.iter().enumerate() {
        let base = table_base + index * entry_bytes;
        write_u64_le_at(output, base, qwen3_dense_0_6b_name_checksum(&file.name));
        write_u64_le_at(output, base + 8, file.bytes);
        write_u64_le_at(output, base + 16, file.checksum);
        write_u64_le_at(output, base + 24, index as u64);
    }
}

fn qwen3_dense_0_6b_name_checksum(name: &str) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for byte in name.bytes() {
        acc ^= byte as u64;
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn qwen3_dense_0_6b_weight_stage_link_table_end(
    table_header: usize,
    links: &[Qwen3Dense06bRealWeightStageLinkDescriptor],
) -> usize {
    const TABLE_HEADER_BYTES: usize = 64;
    let entry_bytes = QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    if links.is_empty() {
        table_header
    } else {
        table_header + TABLE_HEADER_BYTES + links.len() * entry_bytes
    }
}

fn qwen3_dense_0_6b_write_weight_stage_link_table(
    output: &mut [u8],
    table_header: usize,
    marker: u64,
    links: &[Qwen3Dense06bRealWeightStageLinkDescriptor],
) {
    if links.is_empty() {
        return;
    }
    const TABLE_HEADER_BYTES: usize = 64;
    let entry_bytes = QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    let table_base = table_header + TABLE_HEADER_BYTES;
    let table_bytes = links.len() * entry_bytes;
    let aggregate_checksum = links.iter().fold(0xcbf2_9ce4_8422_2325u64, |acc, link| {
        acc.wrapping_mul(0x0000_0100_0000_01b3)
            ^ link.reference_layer_id.rotate_left(3)
            ^ link.synthetic_checksum.rotate_left(7)
            ^ link.real_weight_checksum.rotate_left(17)
            ^ link.real_output_checksum.rotate_left(29)
            ^ link.real_value_checksum.rotate_left(37)
    });
    let reference_layers: BTreeSet<u64> =
        links.iter().map(|link| link.reference_layer_id).collect();
    write_u64_le_at(output, table_header, marker);
    write_u64_le_at(output, table_header + 8, links.len() as u64);
    write_u64_le_at(
        output,
        table_header + 16,
        QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS,
    );
    write_u64_le_at(output, table_header + 24, table_bytes as u64);
    write_u64_le_at(output, table_header + 32, aggregate_checksum);
    write_u64_le_at(output, table_header + 40, 4);
    write_u64_le_at(
        output,
        table_header + 48,
        reference_layers.first().copied().unwrap_or(0),
    );
    write_u64_le_at(output, table_header + 56, reference_layers.len() as u64);
    for (index, link) in links.iter().enumerate() {
        let base = table_base + index * entry_bytes;
        write_u64_le_at(output, base, link.tile_id);
        write_u64_le_at(output, base + 8, link.shard_id);
        write_u64_le_at(output, base + 16, link.stage_kind);
        write_u64_le_at(output, base + 24, link.segment);
        write_u64_le_at(output, base + 32, link.synthetic_checksum);
        write_u64_le_at(output, base + 40, link.real_weight_checksum);
        write_u64_le_at(output, base + 48, link.real_output_checksum);
        write_u64_le_at(output, base + 56, link.real_value_checksum);
        write_u64_le_at(output, base + 64, link.rows);
        write_u64_le_at(output, base + 72, link.hidden_size);
        write_u64_le_at(output, base + 80, link.reference_layer_id);
    }
}

fn qwen3_dense_0_6b_mlp_reference_table_end(
    table_header: usize,
    summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
    next_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
) -> usize {
    const TABLE_HEADER_BYTES: usize = 64;
    let entry_bytes = QWEN3_MLP_REFERENCE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    let entry_count = summary.map(|summary| summary.shards.len()).unwrap_or(0)
        + next_summary
            .map(|summary| summary.shards.len())
            .unwrap_or(0);
    if entry_count == 0 {
        table_header
    } else {
        table_header + TABLE_HEADER_BYTES + entry_count * entry_bytes
    }
}

fn qwen3_dense_0_6b_write_mlp_reference_table(
    output: &mut [u8],
    table_header: usize,
    marker: u64,
    summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
    next_summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
) {
    let entry_count = summary.map(|summary| summary.shards.len()).unwrap_or(0)
        + next_summary
            .map(|summary| summary.shards.len())
            .unwrap_or(0);
    if entry_count == 0 {
        return;
    }
    const TABLE_HEADER_BYTES: usize = 64;
    let entry_bytes = QWEN3_MLP_REFERENCE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    let table_base = table_header + TABLE_HEADER_BYTES;
    let table_bytes = entry_count * entry_bytes;
    let aggregate_checksum = summary
        .map(|summary| summary.aggregate_checksum)
        .unwrap_or(0);
    let next_aggregate_checksum = next_summary
        .map(|summary| summary.aggregate_checksum)
        .unwrap_or(0);
    write_u64_le_at(output, table_header, marker);
    write_u64_le_at(output, table_header + 8, entry_count as u64);
    write_u64_le_at(output, table_header + 16, QWEN3_MLP_REFERENCE_ENTRY_WORDS);
    write_u64_le_at(output, table_header + 24, table_bytes as u64);
    write_u64_le_at(
        output,
        table_header + 32,
        summary.map(|summary| summary.layer_id).unwrap_or(u64::MAX),
    );
    write_u64_le_at(
        output,
        table_header + 40,
        next_summary
            .map(|summary| summary.layer_id)
            .unwrap_or(u64::MAX),
    );
    write_u64_le_at(output, table_header + 48, aggregate_checksum);
    write_u64_le_at(output, table_header + 56, next_aggregate_checksum);

    let mut entry_index = 0usize;
    for layer_summary in [summary, next_summary].into_iter().flatten() {
        for shard in &layer_summary.shards {
            let base = table_base + entry_index * entry_bytes;
            let sample_digest = qwen3_dense_0_6b_mlp_reference_sample_digest(shard);
            let slice_bytes = shard
                .weight_slices
                .iter()
                .map(|slice| slice.bytes)
                .sum::<u64>();
            let slice_digest = qwen3_dense_0_6b_reference_slice_digest(&shard.weight_slices);
            write_u64_le_at(output, base, layer_summary.layer_id);
            write_u64_le_at(output, base + 8, shard.shard_id);
            write_u64_le_at(output, base + 16, shard.hidden_size);
            write_u64_le_at(output, base + 24, shard.intermediate_rows);
            write_u64_le_at(output, base + 32, shard.gate_weight_checksum);
            write_u64_le_at(output, base + 40, shard.up_weight_checksum);
            write_u64_le_at(output, base + 48, shard.down_weight_checksum);
            write_u64_le_at(output, base + 56, shard.gate_output_checksum);
            write_u64_le_at(output, base + 64, shard.up_output_checksum);
            write_u64_le_at(output, base + 72, shard.activation_checksum);
            write_u64_le_at(output, base + 80, shard.down_output_checksum);
            write_u64_le_at(output, base + 88, sample_digest);
            write_u64_le_at(output, base + 96, shard.weight_slices.len() as u64);
            write_u64_le_at(output, base + 104, slice_bytes);
            write_u64_le_at(output, base + 112, slice_digest);
            entry_index += 1;
        }
    }
}

fn qwen3_dense_0_6b_mlp_reference_sample_digest(
    shard: &Qwen3Dense06bMlpReferenceShardSummary,
) -> u64 {
    shard
        .gate_output_sample_words
        .iter()
        .chain(shard.up_output_sample_words.iter())
        .chain(shard.activation_sample_words.iter())
        .chain(shard.down_output_sample_words.iter())
        .fold(0xcbf2_9ce4_8422_2325u64, |acc, word| {
            acc.wrapping_mul(0x0000_0100_0000_01b3) ^ *word
        })
}

fn qwen3_dense_0_6b_reference_slice_digest(
    slices: &[Qwen3Dense06bReferenceWeightSliceValidation],
) -> u64 {
    slices.iter().fold(0xcbf2_9ce4_8422_2325u64, |acc, slice| {
        acc.wrapping_mul(0x0000_0100_0000_01b3)
            ^ (slice.kind as u64).rotate_left(5)
            ^ slice.bytes.rotate_left(17)
            ^ slice.checksum.rotate_left(29)
    })
}

fn qwen3_dense_0_6b_logits_reference_table_end(
    table_header: usize,
    summary: Option<&Qwen3Dense06bLogitsReferenceSummary>,
) -> usize {
    const TABLE_HEADER_BYTES: usize = 64;
    let entry_bytes = QWEN3_LOGITS_REFERENCE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    summary
        .map(|summary| table_header + TABLE_HEADER_BYTES + summary.tokens.len() * entry_bytes)
        .unwrap_or(table_header)
}

fn qwen3_dense_0_6b_write_logits_reference_table(
    output: &mut [u8],
    table_header: usize,
    marker: u64,
    summary: Option<&Qwen3Dense06bLogitsReferenceSummary>,
) {
    let Some(summary) = summary else {
        return;
    };
    const TABLE_HEADER_BYTES: usize = 64;
    let entry_bytes = QWEN3_LOGITS_REFERENCE_ENTRY_WORDS as usize * std::mem::size_of::<u64>();
    let table_base = table_header + TABLE_HEADER_BYTES;
    let table_bytes = summary.tokens.len() * entry_bytes;
    write_u64_le_at(output, table_header, marker);
    write_u64_le_at(output, table_header + 8, summary.tokens.len() as u64);
    write_u64_le_at(
        output,
        table_header + 16,
        QWEN3_LOGITS_REFERENCE_ENTRY_WORDS,
    );
    write_u64_le_at(output, table_header + 24, table_bytes as u64);
    write_u64_le_at(output, table_header + 32, summary.aggregate_checksum);
    write_u64_le_at(output, table_header + 40, summary.final_norm_checksum);
    write_u64_le_at(output, table_header + 48, summary.vocab_size);
    write_u64_le_at(output, table_header + 56, summary.hidden_size);
    for (index, token) in summary.tokens.iter().enumerate() {
        let base = table_base + index * entry_bytes;
        write_u64_le_at(output, base, token.step_index);
        write_u64_le_at(output, base + 8, token.token_id);
        write_u64_le_at(output, base + 16, token.row_bytes);
        write_u64_le_at(output, base + 24, token.row_checksum);
        write_u64_le_at(output, base + 32, token.logit_bits);
        write_u64_le_at(output, base + 40, token.logit_checksum);
    }
}

fn qwen3_dense_0_6b_range_forward_table_end(
    table_header: usize,
    summary: Option<&Qwen3Dense06bRangeForwardSummary>,
) -> usize {
    const TABLE_HEADER_BYTES: usize = 64;
    const RANGE_FORWARD_ENTRY_WORDS: usize = 18;
    const RANGE_FORWARD_ENTRY_BYTES: usize = RANGE_FORWARD_ENTRY_WORDS * 8;
    summary
        .map(|summary| {
            table_header
                + TABLE_HEADER_BYTES
                + RANGE_FORWARD_ENTRY_BYTES
                + summary.output_tensor_payload.len()
                + summary.kv_state_payload.len()
        })
        .unwrap_or(table_header)
}

fn qwen3_dense_0_6b_write_range_forward_table(
    output: &mut [u8],
    table_header: usize,
    marker: u64,
    summary: Option<&Qwen3Dense06bRangeForwardSummary>,
) {
    let Some(summary) = summary else {
        return;
    };
    const TABLE_HEADER_BYTES: usize = 64;
    const RANGE_FORWARD_ENTRY_WORDS: u64 = 18;
    const RANGE_FORWARD_ENTRY_BYTES: usize = RANGE_FORWARD_ENTRY_WORDS as usize * 8;
    let table_base = table_header + TABLE_HEADER_BYTES;
    let payload_offset = table_base + RANGE_FORWARD_ENTRY_BYTES;
    let table_bytes = RANGE_FORWARD_ENTRY_BYTES
        + summary.output_tensor_payload.len()
        + summary.kv_state_payload.len();
    assert!(
        table_base + table_bytes <= output.len(),
        "qwen3 service flow buffer too small for range-forward table: needed={} available={}",
        table_base + table_bytes,
        output.len()
    );
    let entry = [
        summary.node,
        summary.layer_start,
        summary.layer_end,
        summary.layer_count,
        summary.next_node,
        summary.pipeline_nodes,
        summary.total_layers,
        summary.hidden_bytes,
        summary.input_tensor_checksum,
        summary.output_tensor_checksum,
        summary.range_layer_checksum,
        summary.real_layer_execution_count,
        summary.first_layer_output_checksum,
        summary.final_layer_output_checksum,
        summary.input_tensor_bytes,
        summary.output_tensor_bytes,
        summary.kv_state_bytes,
        summary.kv_state_checksum,
    ];

    write_u64_le_at(output, table_header, marker);
    write_u64_le_at(output, table_header + 8, 1);
    write_u64_le_at(output, table_header + 16, RANGE_FORWARD_ENTRY_WORDS);
    write_u64_le_at(output, table_header + 24, table_bytes as u64);
    write_u64_le_at(output, table_header + 32, checksum_words(&entry));
    write_u64_le_at(output, table_header + 40, summary.range_layer_checksum);
    write_u64_le_at(output, table_header + 48, summary.input_tensor_checksum);
    write_u64_le_at(output, table_header + 56, summary.output_tensor_checksum);
    for (index, word) in entry.iter().copied().enumerate() {
        write_u64_le_at(output, table_base + index * 8, word);
    }
    output[payload_offset..payload_offset + summary.output_tensor_payload.len()]
        .copy_from_slice(&summary.output_tensor_payload);
    let kv_payload_offset = payload_offset + summary.output_tensor_payload.len();
    output[kv_payload_offset..kv_payload_offset + summary.kv_state_payload.len()]
        .copy_from_slice(&summary.kv_state_payload);
}

fn write_u64_le_at(output: &mut [u8], offset: usize, value: u64) {
    let end = offset + std::mem::size_of::<u64>();
    if end <= output.len() {
        output[offset..end].copy_from_slice(&value.to_le_bytes());
    }
}

fn read_u64_le_at(output: &[u8], offset: usize) -> u64 {
    let end = offset + std::mem::size_of::<u64>();
    if end <= output.len() {
        u64::from_le_bytes(output[offset..end].try_into().expect("u64-aligned range"))
    } else {
        0
    }
}

fn read_u64_le_checked(output: &[u8], offset: usize, field: &str) -> Result<u64, String> {
    let end = offset
        .checked_add(std::mem::size_of::<u64>())
        .ok_or_else(|| format!("{field}_offset_overflow"))?;
    if end > output.len() {
        return Err(format!(
            "{field}_oob:offset={offset}:output_len={}",
            output.len()
        ));
    }
    Ok(u64::from_le_bytes(
        output[offset..end].try_into().expect("u64-aligned range"),
    ))
}

fn find_u64_marker(output: &[u8], marker: u64) -> Option<usize> {
    output
        .chunks_exact(std::mem::size_of::<u64>())
        .position(|chunk| u64::from_le_bytes(chunk.try_into().expect("u64 chunk")) == marker)
        .map(|index| index * std::mem::size_of::<u64>())
}

fn qwen3_dense_0_6b_result_block_sample_offsets() -> [usize; 8] {
    let row_bytes = 128 * std::mem::size_of::<f32>();
    [
        0,
        64 * std::mem::size_of::<f32>(),
        7 * row_bytes + 32 * std::mem::size_of::<f32>(),
        31 * row_bytes + 96 * std::mem::size_of::<f32>(),
        32 * row_bytes,
        47 * row_bytes + 64 * std::mem::size_of::<f32>(),
        63 * row_bytes,
        63 * row_bytes + 120 * std::mem::size_of::<f32>(),
    ]
}

fn qwen3_dense_0_6b_canonical_block_checksum(
    output: &[u8],
    byte_start: usize,
    byte_end: usize,
    zero_start: usize,
    zero_end: usize,
) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    let available_end = byte_end.min(output.len());
    for chunk_start in (byte_start..byte_end).step_by(std::mem::size_of::<u64>()) {
        let mut word = [0u8; 8];
        for (index, byte) in word.iter_mut().enumerate() {
            let offset = chunk_start + index;
            if offset < available_end && (offset < zero_start || offset >= zero_end) {
                *byte = output[offset];
            }
        }
        acc ^= u64::from_le_bytes(word);
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn distinct_checksum_count(checksums: &[u64]) -> u64 {
    let mut distinct = 0u64;
    for (index, checksum) in checksums.iter().enumerate() {
        if !checksums[..index].contains(checksum) {
            distinct += 1;
        }
    }
    distinct
}

fn qwen3_dense_0_6b_model_meta_payload(profile: Qwen3Dense06bProfile) -> Vec<u8> {
    [
        profile.vocab_size,
        profile.hidden_size,
        profile.intermediate_size,
        profile.num_hidden_layers,
        profile.num_attention_heads,
        profile.num_key_value_heads,
        profile.head_dim,
        profile.max_position_embeddings,
        profile.rope_theta,
        profile.tp_nodes,
        0,
        0,
    ]
    .into_iter()
    .flat_map(u64::to_le_bytes)
    .collect()
}

fn qwen3_dense_0_6b_kv_layout_payload(profile: Qwen3Dense06bProfile) -> Vec<u8> {
    let kv_elem_bytes = 2u64;
    let kv_bytes_per_token_per_layer =
        profile.num_key_value_heads * profile.head_dim * kv_elem_bytes * 2;
    let real_f32_kv_bytes_per_token_per_layer = qwen3_dense_0_6b_real_kv_state_bytes(1, 1);
    [
        profile.prefill_tokens,
        profile.decode_tokens,
        profile.num_hidden_layers,
        profile.num_key_value_heads,
        profile.head_dim,
        kv_elem_bytes,
        kv_bytes_per_token_per_layer,
        kv_bytes_per_token_per_layer * profile.prefill_tokens,
        kv_bytes_per_token_per_layer * profile.prefill_tokens * profile.num_hidden_layers,
        real_f32_kv_bytes_per_token_per_layer,
        W4_KVCACHE_BLOCKS as u64,
        W4_KVCACHE_PREFIX_GROUPS as u64,
    ]
    .into_iter()
    .flat_map(u64::to_le_bytes)
    .collect()
}

fn qwen3_dense_0_6b_guest_input_payload(guest_input: &[u8]) -> Vec<u8> {
    let mut payload = vec![0u8; W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES];
    let copy_len = payload.len().min(guest_input.len());
    payload[..copy_len].copy_from_slice(&guest_input[..copy_len]);
    payload
}

fn qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
    guest_input: &[u8],
    real_embedding_hidden: Option<&[f32]>,
    elems: usize,
    shard: Qwen3Dense06bShard,
    tile_index: u64,
) -> Vec<u8> {
    let mut bytes = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    if let Some(hidden) = real_embedding_hidden.filter(|hidden| !hidden.is_empty()) {
        for elem_index in 0..elems {
            let hidden_index = (elem_index
                + shard.head_start as usize
                + shard.kv_block_start as usize
                + tile_index as usize * 17)
                % hidden.len();
            let value = hidden[hidden_index]
                + shard.shard_id as f32 * 0.00001
                + tile_index as f32 * 0.00002;
            bytes.extend_from_slice(&f32_to_f16_bits(value).to_le_bytes());
        }
        return bytes;
    }

    let input_digest = qwen3_dense_0_6b_decode_guest_input_checksum(guest_input);
    for elem_index in 0..elems {
        let source_index = (elem_index
            + shard.head_start as usize
            + shard.kv_block_start as usize
            + tile_index as usize * 17)
            % guest_input.len().max(1);
        let source = guest_input.get(source_index).copied().unwrap_or(0);
        let digest_bits = (input_digest.rotate_left((elem_index % 63) as u32) & 0x001f) as u16;
        let half_bits = 0x3c00u16
            + ((source & 0x0f) as u16)
            + digest_bits
            + ((shard.shard_id & 0x01) as u16)
            + ((tile_index & 0x01) as u16);
        bytes.extend_from_slice(&half_bits.to_le_bytes());
    }
    bytes
}

fn qwen3_dense_0_6b_rmsnorm_tile_from_prefill_hidden(
    prefill_hidden: &[u8],
    dim: usize,
    shard: Qwen3Dense06bShard,
) -> Vec<u8> {
    const EPS: f32 = 1.0e-6;

    let elems = dim * dim;
    let mut values = Vec::with_capacity(elems);
    for elem_index in 0..elems {
        let byte_index = elem_index * std::mem::size_of::<u16>();
        let half_bits = if byte_index + std::mem::size_of::<u16>() <= prefill_hidden.len() {
            u16::from_le_bytes([prefill_hidden[byte_index], prefill_hidden[byte_index + 1]])
        } else {
            0
        };
        values.push(f16_bits_to_f32(half_bits));
    }

    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for row in 0..dim {
        let row_begin = row * dim;
        let row_values = &values[row_begin..row_begin + dim];
        let sum_sq = row_values.iter().map(|value| value * value).sum::<f32>();
        let inv_rms = 1.0f32 / ((sum_sq / dim as f32) + EPS).sqrt();
        for col in 0..dim {
            let gamma = 1.0f32 + (((col as u64 + shard.shard_id) % 7) as f32 * 0.001);
            let normed = row_values[col] * inv_rms * gamma;
            out.extend_from_slice(&f32_to_f16_bits(normed).to_le_bytes());
        }
    }
    out
}

fn qwen3_dense_0_6b_projection_tile_from_half_input(
    half_input: &[u8],
    dim: usize,
    kind: Qwen3ProjectionKind,
    shard: Qwen3Dense06bShard,
) -> Vec<u8> {
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let byte_index = elem_index * std::mem::size_of::<u16>();
        let half_bits = if byte_index + std::mem::size_of::<u16>() <= half_input.len() {
            u16::from_le_bytes([half_input[byte_index], half_input[byte_index + 1]])
        } else {
            0
        };
        let row = elem_index / dim;
        let col = elem_index % dim;
        let value = f16_bits_to_f32(half_bits);
        let (scale, bias) = match kind {
            Qwen3ProjectionKind::Q => (1.0000f32 + (col % 5) as f32 * 0.0005, 0.0001f32),
            Qwen3ProjectionKind::Kv => (1.0050f32 + (row % 7) as f32 * 0.0004, 0.0003f32),
            Qwen3ProjectionKind::V => (1.0100f32 + ((row + col) % 11) as f32 * 0.0003, 0.0005f32),
        };
        let shard_bias = shard.shard_id as f32 * 0.0002;
        out.extend_from_slice(&f32_to_f16_bits((value * scale) + bias + shard_bias).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_tile_with_real_qkv_reference_mix(
    tile: &[u8],
    dim: usize,
    stage_kind: u64,
    shard: Qwen3Dense06bShard,
    summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
) -> Vec<u8> {
    let Some(reference_shard) = summary.and_then(|summary| {
        summary
            .shards
            .iter()
            .find(|reference_shard| reference_shard.shard_id == shard.shard_id)
    }) else {
        return tile.to_vec();
    };
    let (weight_checksum, output_checksum, output_sample_words, rows) = match stage_kind {
        1 => (
            qwen3_dense_0_6b_reference_weight_slice_checksum(
                reference_shard,
                Qwen3Dense06bWeightTensorKind::InputLayerNorm,
            )
            .unwrap_or(0),
            reference_shard.rmsnorm_checksum,
            reference_shard.rmsnorm_sample_words,
            reference_shard.hidden_size,
        ),
        2 => (
            reference_shard.q_weight_checksum,
            reference_shard.q_output_checksum,
            reference_shard.q_output_sample_words,
            reference_shard.q_rows,
        ),
        3 => (
            reference_shard.k_weight_checksum,
            reference_shard.k_output_checksum,
            reference_shard.k_output_sample_words,
            reference_shard.k_rows,
        ),
        4 => (
            reference_shard.v_weight_checksum,
            reference_shard.v_output_checksum,
            reference_shard.v_output_sample_words,
            reference_shard.v_rows,
        ),
        _ => return tile.to_vec(),
    };
    if weight_checksum == 0 || output_checksum == 0 || rows == 0 {
        return tile.to_vec();
    }

    let reference_seed = checksum_words(&[
        summary.map(|summary| summary.layer_id).unwrap_or(0),
        summary.map(|summary| summary.shard_count).unwrap_or(0),
        summary
            .map(|summary| summary.aggregate_checksum)
            .unwrap_or(0),
        reference_shard.shard_id,
        stage_kind,
        reference_shard.hidden_size,
        rows,
        weight_checksum,
        output_checksum,
        output_sample_words[0],
        output_sample_words[1],
        output_sample_words[2],
        output_sample_words[3],
    ]);
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let value = qwen3_dense_0_6b_half_at(tile, elem_index);
        let row = elem_index / dim;
        let col = elem_index % dim;
        let rotated = reference_seed.rotate_left(((row + col) % 63) as u32);
        let signed_lsb = if (rotated & 1) == 0 { -1.0f32 } else { 1.0f32 };
        let scale = 1.0 + signed_lsb * (((rotated >> 8) & 0x1f) as f32 + 1.0) * 0.00001;
        let bias = (((rotated >> 21) & 0x0f) as f32 - 7.5) * 0.00001;
        out.extend_from_slice(&f32_to_f16_bits((value * scale) + bias).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
    tile: &[u8],
    dim: usize,
    stage_kind: u64,
    shard: Qwen3Dense06bShard,
    values: Option<&Qwen3Dense06bQkvReferenceLayerValues>,
    summary: Option<&Qwen3Dense06bQkvReferenceLayerSummary>,
) -> Vec<u8> {
    let checksum_mixed =
        qwen3_dense_0_6b_tile_with_real_qkv_reference_mix(tile, dim, stage_kind, shard, summary);
    let Some(reference_shard) = values.and_then(|values| {
        values
            .shards
            .iter()
            .find(|reference_shard| reference_shard.shard_id == shard.shard_id)
    }) else {
        return checksum_mixed;
    };
    let (reference_values, value_checksum, rows) =
        match qwen3_dense_0_6b_real_qkv_stage_values(reference_shard, stage_kind) {
            Some(reference) => reference,
            None => return checksum_mixed,
        };
    if reference_values.is_empty() || value_checksum == 0 || rows == 0 {
        return checksum_mixed;
    }

    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let row = elem_index / dim;
        let col = elem_index % dim;
        let reference_index = ((row % rows as usize) * dim + col + shard.kv_block_start as usize)
            % reference_values.len();
        let reference_value = reference_values[reference_index].tanh();
        let value = qwen3_dense_0_6b_half_at(&checksum_mixed, elem_index);
        let scale = 1.0 + reference_value * 0.01;
        let bias = reference_value * 0.0025;
        out.extend_from_slice(&f32_to_f16_bits((value * scale) + bias).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_real_qkv_stage_values(
    shard: &Qwen3Dense06bQkvReferenceShardValues,
    stage_kind: u64,
) -> Option<(&[f32], u64, u64)> {
    match stage_kind {
        1 => Some((&shard.rmsnorm, shard.rmsnorm_checksum, shard.hidden_size)),
        2 => Some((&shard.q_output, shard.q_output_checksum, shard.q_rows)),
        3 => Some((&shard.k_output, shard.k_output_checksum, shard.k_rows)),
        4 => Some((&shard.v_output, shard.v_output_checksum, shard.v_rows)),
        _ => None,
    }
}

fn qwen3_dense_0_6b_tile_with_real_mlp_reference_mix(
    tile: &[u8],
    dim: usize,
    stage_kind: u64,
    shard: Qwen3Dense06bShard,
    summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
) -> Vec<u8> {
    let Some(reference_shard) = summary.and_then(|summary| {
        summary
            .shards
            .iter()
            .find(|reference_shard| reference_shard.shard_id == shard.shard_id)
    }) else {
        return tile.to_vec();
    };
    let (gate_checksum, up_checksum, down_checksum, output_checksum, output_sample_words, rows) =
        match stage_kind {
            10 => (
                reference_shard.gate_weight_checksum,
                reference_shard.up_weight_checksum,
                0,
                reference_shard.activation_checksum,
                reference_shard.activation_sample_words,
                reference_shard.intermediate_rows,
            ),
            12 => (
                reference_shard.gate_weight_checksum,
                reference_shard.up_weight_checksum,
                reference_shard.down_weight_checksum,
                reference_shard.down_output_checksum,
                reference_shard.down_output_sample_words,
                reference_shard.hidden_size,
            ),
            _ => return tile.to_vec(),
        };
    if gate_checksum == 0 || up_checksum == 0 || output_checksum == 0 || rows == 0 {
        return tile.to_vec();
    }

    let layer_id = summary.map(|summary| summary.layer_id).unwrap_or(0);
    let reference_seed = checksum_words(&[
        layer_id,
        summary.map(|summary| summary.shard_count).unwrap_or(0),
        summary
            .map(|summary| summary.aggregate_checksum)
            .unwrap_or(0),
        reference_shard.shard_id,
        stage_kind,
        reference_shard.hidden_size,
        reference_shard.intermediate_rows,
        rows,
        gate_checksum,
        up_checksum,
        down_checksum,
        output_checksum,
        output_sample_words[0],
        output_sample_words[1],
        output_sample_words[2],
        output_sample_words[3],
    ]);
    let elems = dim * dim;
    let is_float_tile = tile.len() >= elems * std::mem::size_of::<f32>();
    if is_float_tile {
        let mut out = Vec::with_capacity(elems * std::mem::size_of::<f32>());
        for elem_index in 0..elems {
            let value = qwen3_dense_0_6b_f32_at(tile, elem_index);
            let row = elem_index / dim;
            let col = elem_index % dim;
            let rotated = reference_seed.rotate_left(((row * 3 + col) % 63) as u32);
            let layer_bias = (layer_id & 0x0f) as f32 * 0.000001;
            let scale = 1.0 + (((rotated >> 8) & 0x1f) as f32 + 1.0) * 0.000001 + layer_bias;
            let bias = (((rotated >> 29) & 0x0f) as f32 - 7.5) * 0.000001 + layer_bias;
            out.extend_from_slice(&((value * scale) + bias).to_le_bytes());
        }
        return out;
    }

    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let value = qwen3_dense_0_6b_half_at(tile, elem_index);
        let row = elem_index / dim;
        let col = elem_index % dim;
        let rotated = reference_seed.rotate_left(((row * 5 + col) % 63) as u32);
        let layer_bias = (layer_id & 0x0f) as f32 * 0.0001;
        let scale = 1.0 + (((rotated >> 8) & 0x1f) as f32 + 1.0) * 0.0001 + layer_bias;
        let bias = ((rotated >> 23) & 0x0f) as f32 * 0.0001 + layer_bias;
        out.extend_from_slice(&f32_to_f16_bits((value * scale) + bias).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_rope_tile_from_projection(
    projection: &[u8],
    dim: usize,
    kind: Qwen3ProjectionKind,
    shard: Qwen3Dense06bShard,
) -> Vec<u8> {
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    let phase_bias = match kind {
        Qwen3ProjectionKind::Q => 0.0f32,
        Qwen3ProjectionKind::Kv => 0.25f32,
        Qwen3ProjectionKind::V => 0.5f32,
    };
    for row in 0..dim {
        for col_pair in (0..dim).step_by(2) {
            let even_index = row * dim + col_pair;
            let odd_index = even_index + 1;
            let even = qwen3_dense_0_6b_half_at(projection, even_index);
            let odd = qwen3_dense_0_6b_half_at(projection, odd_index);
            let inv_freq = 1.0f32 / 1_000_000.0f32.powf(col_pair as f32 / dim as f32);
            let phase = (row as f32 + shard.shard_id as f32 + phase_bias) * inv_freq;
            let cos = phase.cos();
            let sin = phase.sin();
            let rotated_even = (even * cos) - (odd * sin);
            let rotated_odd = (even * sin) + (odd * cos);
            out.extend_from_slice(&f32_to_f16_bits(rotated_even).to_le_bytes());
            out.extend_from_slice(&f32_to_f16_bits(rotated_odd).to_le_bytes());
        }
    }
    out
}

#[cfg(test)]
fn qwen3_dense_0_6b_attention_score_tile_from_rope(
    rope_q: &[u8],
    rope_kv: &[u8],
    dim: usize,
) -> Vec<u8> {
    qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_reference(
        rope_q, rope_kv, dim, None,
    )
}

#[cfg(test)]
fn qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_reference(
    rope_q: &[u8],
    rope_kv: &[u8],
    dim: usize,
    reference: Option<(Qwen3Dense06bShard, &Qwen3Dense06bQkvReferenceLayerSummary)>,
) -> Vec<u8> {
    qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_and_kvcache_reference(
        rope_q, rope_kv, dim, reference, None,
    )
}

fn qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_and_kvcache_reference(
    rope_q: &[u8],
    rope_kv: &[u8],
    dim: usize,
    reference: Option<(Qwen3Dense06bShard, &Qwen3Dense06bQkvReferenceLayerSummary)>,
    kvcache_payload: Option<&Qwen3Dense06bKvCacheTilePayload>,
) -> Vec<u8> {
    let elems = dim * dim;
    let scale = 1.0f32 / (dim as f32).sqrt();
    let reference_seed = reference
        .and_then(|(shard, summary)| {
            summary
                .shards
                .iter()
                .find(|reference_shard| reference_shard.shard_id == shard.shard_id)
        })
        .and_then(|reference_shard| {
            if reference_shard.q_output_checksum == 0
                || reference_shard.k_output_checksum == 0
                || reference_shard.q_rows == 0
                || reference_shard.k_rows == 0
            {
                return None;
            }
            Some(checksum_words(&[
                reference_shard.shard_id,
                reference_shard.hidden_size,
                reference_shard.q_rows,
                reference_shard.k_rows,
                reference_shard.q_output_checksum,
                reference_shard.k_output_checksum,
                reference_shard.q_output_sample_words[0],
                reference_shard.q_output_sample_words[1],
                reference_shard.q_output_sample_words[2],
                reference_shard.q_output_sample_words[3],
                reference_shard.k_output_sample_words[0],
                reference_shard.k_output_sample_words[1],
                reference_shard.k_output_sample_words[2],
                reference_shard.k_output_sample_words[3],
            ]))
        });
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<f32>());
    for elem_index in 0..elems {
        let row = elem_index / dim;
        let col = elem_index % dim;
        let mut score = if let Some(cache) = kvcache_payload {
            if col >= cache.read_len() {
                -1.0e9
            } else {
                let mut dot = 0.0f32;
                let q_row_start = row * dim;
                for feature in 0..dim {
                    let q = qwen3_dense_0_6b_half_at(rope_q, q_row_start + feature);
                    dot += q * cache.k_rows[col][feature];
                }
                dot * scale
            }
        } else {
            let q = qwen3_dense_0_6b_half_at(rope_q, elem_index);
            let k = qwen3_dense_0_6b_half_at(rope_kv, elem_index);
            q * k * scale
        };
        if let Some(seed) = reference_seed {
            let mixed = seed.rotate_left(((row * 17 + col * 31) % 63) as u32);
            let signed_lsb = if (mixed & 1) == 0 { -1.0f32 } else { 1.0f32 };
            let real_qk_scale = 1.0 + signed_lsb * (((mixed >> 8) & 0x3f) as f32 + 1.0) * 0.00001;
            let real_qk_bias = (((mixed >> 23) & 0x1f) as f32 - 15.5) * 0.00001;
            score = (score * real_qk_scale) + real_qk_bias;
        }
        if let Some(cache) = kvcache_payload {
            let seed = checksum_words(&[
                cache.layer_id,
                cache.tile_id,
                cache.read_position_start,
                cache.read_position_end,
                col as u64,
            ]);
            let mixed = seed.rotate_left(((row * 19 + col * 37) % 63) as u32);
            let signed_lsb = if (mixed & 1) == 0 { -1.0f32 } else { 1.0f32 };
            let cache_scale = 1.0 + signed_lsb * (((mixed >> 11) & 0x3f) as f32 + 1.0) * 0.00001;
            let cache_bias = (((mixed >> 27) & 0x1f) as f32 - 15.5) * 0.00001;
            score = (score * cache_scale) + cache_bias;
        }
        out.extend_from_slice(&score.to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_softmax_tile_from_attention_score(
    attention_score: &[u8],
    dim: usize,
) -> Vec<u8> {
    let mut out = Vec::with_capacity(dim * dim * std::mem::size_of::<f32>());
    for row in 0..dim {
        let row_start = row * dim;
        let mut max_score = f32::NEG_INFINITY;
        for col in 0..dim {
            let score = qwen3_dense_0_6b_f32_at(attention_score, row_start + col);
            if score.is_finite() && score > max_score {
                max_score = score;
            }
        }

        let mut exp_scores = Vec::with_capacity(dim);
        let mut exp_sum = 0.0f32;
        for col in 0..dim {
            let score = qwen3_dense_0_6b_f32_at(attention_score, row_start + col);
            let exp_score = if max_score.is_finite() && score.is_finite() {
                (score - max_score).exp()
            } else {
                0.0
            };
            exp_sum += exp_score;
            exp_scores.push(exp_score);
        }

        if exp_sum.is_finite() && exp_sum > 0.0 {
            for exp_score in exp_scores {
                out.extend_from_slice(&(exp_score / exp_sum).to_le_bytes());
            }
        } else {
            let uniform = 1.0f32 / dim as f32;
            for _ in 0..dim {
                out.extend_from_slice(&uniform.to_le_bytes());
            }
        }
    }
    out
}

#[cfg(test)]
fn qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v(
    attention_softmax: &[u8],
    v_projection: &[u8],
    dim: usize,
) -> Vec<u8> {
    qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_reference(
        attention_softmax,
        v_projection,
        dim,
        None,
    )
}

#[cfg(test)]
fn qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_reference(
    attention_softmax: &[u8],
    v_projection: &[u8],
    dim: usize,
    reference: Option<(Qwen3Dense06bShard, &Qwen3Dense06bQkvReferenceLayerSummary)>,
) -> Vec<u8> {
    qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_and_kvcache_reference(
        attention_softmax,
        v_projection,
        dim,
        reference,
        None,
    )
}

fn qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_and_kvcache_reference(
    attention_softmax: &[u8],
    v_projection: &[u8],
    dim: usize,
    reference: Option<(Qwen3Dense06bShard, &Qwen3Dense06bQkvReferenceLayerSummary)>,
    kvcache_payload: Option<&Qwen3Dense06bKvCacheTilePayload>,
) -> Vec<u8> {
    let reference_seed = reference
        .and_then(|(shard, summary)| {
            summary
                .shards
                .iter()
                .find(|reference_shard| reference_shard.shard_id == shard.shard_id)
        })
        .and_then(|reference_shard| {
            if reference_shard.v_output_checksum == 0 || reference_shard.v_rows == 0 {
                return None;
            }
            Some(checksum_words(&[
                reference_shard.shard_id,
                reference_shard.hidden_size,
                reference_shard.v_rows,
                reference_shard.v_output_checksum,
                reference_shard.v_output_sample_words[0],
                reference_shard.v_output_sample_words[1],
                reference_shard.v_output_sample_words[2],
                reference_shard.v_output_sample_words[3],
            ]))
        });
    let mut out = Vec::with_capacity(dim * dim * std::mem::size_of::<u16>());
    for row in 0..dim {
        for col in 0..dim {
            let mut acc = 0.0f32;
            if let Some(cache) = kvcache_payload {
                for k in 0..cache.read_len() {
                    let probability = qwen3_dense_0_6b_f32_at(attention_softmax, row * dim + k);
                    acc += probability * cache.v_rows[k][col];
                }
            } else {
                for k in 0..dim {
                    let probability = qwen3_dense_0_6b_f32_at(attention_softmax, row * dim + k);
                    let value = qwen3_dense_0_6b_half_at(v_projection, k * dim + col);
                    acc += probability * value;
                }
            }
            if let Some(seed) = reference_seed {
                let mixed = seed.rotate_left(((row * 13 + col * 29) % 63) as u32);
                let signed_lsb = if (mixed & 1) == 0 { -1.0f32 } else { 1.0f32 };
                let real_v_scale =
                    1.0 + signed_lsb * (((mixed >> 9) & 0x3f) as f32 + 1.0) * 0.00001;
                let real_v_bias = (((mixed >> 25) & 0x1f) as f32 - 15.5) * 0.00001;
                acc = (acc * real_v_scale) + real_v_bias;
            }
            if let Some(cache) = kvcache_payload {
                let seed = checksum_words(&[
                    cache.layer_id,
                    cache.tile_id,
                    cache.read_position_start,
                    cache.read_position_end,
                    col as u64,
                ]);
                let mixed = seed.rotate_left(((row * 23 + col * 41) % 63) as u32);
                let signed_lsb = if (mixed & 1) == 0 { -1.0f32 } else { 1.0f32 };
                let cache_scale =
                    1.0 + signed_lsb * (((mixed >> 12) & 0x3f) as f32 + 1.0) * 0.00001;
                let cache_bias = (((mixed >> 28) & 0x1f) as f32 - 15.5) * 0.00001;
                acc = (acc * cache_scale) + cache_bias;
            }
            out.extend_from_slice(&f32_to_f16_bits(acc).to_le_bytes());
        }
    }
    out
}

fn qwen3_dense_0_6b_mlp_activation_tile_from_attention_context(
    attention_context: &[u8],
    dim: usize,
    shard: Qwen3Dense06bShard,
) -> Vec<u8> {
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    let shard_bias = shard.shard_id as f32 * 0.0003;
    for elem_index in 0..elems {
        let context = qwen3_dense_0_6b_half_at(attention_context, elem_index);
        let row = elem_index / dim;
        let col = elem_index % dim;
        let gate_scale = 0.72 + ((row % 17) as f32 * 0.001) + shard_bias;
        let up_scale = 1.08 + ((col % 19) as f32 * 0.0015) + shard_bias;
        let gate = context * gate_scale + 0.03125;
        let up = context * up_scale + 0.015625;
        let silu_gate = gate / (1.0 + (-gate).exp());
        let synthetic_mlp = (silu_gate * up).abs();
        let activation = 1.001 + (synthetic_mlp % 1.0) * 0.002;
        out.extend_from_slice(&f32_to_f16_bits(activation).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_real_mlp_activation_tile_from_attention_context(
    attention_context: &[u8],
    dim: usize,
    shard: Qwen3Dense06bShard,
    summary: Option<&Qwen3Dense06bMlpReferenceLayerSummary>,
) -> Vec<u8> {
    let Some(reference_shard) = summary.and_then(|summary| {
        summary
            .shards
            .iter()
            .find(|reference_shard| reference_shard.shard_id == shard.shard_id)
    }) else {
        return qwen3_dense_0_6b_mlp_activation_tile_from_attention_context(
            attention_context,
            dim,
            shard,
        );
    };
    if reference_shard.activation_checksum == 0
        || reference_shard.gate_output_checksum == 0
        || reference_shard.up_output_checksum == 0
        || reference_shard.intermediate_rows == 0
    {
        return qwen3_dense_0_6b_mlp_activation_tile_from_attention_context(
            attention_context,
            dim,
            shard,
        );
    }

    let layer_id = summary.map(|summary| summary.layer_id).unwrap_or(0);
    let reference_seed = checksum_words(&[
        layer_id,
        reference_shard.shard_id,
        reference_shard.hidden_size,
        reference_shard.intermediate_rows,
        reference_shard.gate_weight_checksum,
        reference_shard.up_weight_checksum,
        reference_shard.gate_output_checksum,
        reference_shard.up_output_checksum,
        reference_shard.activation_checksum,
        reference_shard.activation_sample_words[0],
        reference_shard.activation_sample_words[1],
        reference_shard.activation_sample_words[2],
        reference_shard.activation_sample_words[3],
    ]);
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<u16>());
    for elem_index in 0..elems {
        let context = qwen3_dense_0_6b_half_at(attention_context, elem_index);
        let row = elem_index / dim;
        let col = elem_index % dim;
        let sample_word = reference_shard.activation_sample_words[(row + col) % 4];
        let mixed = reference_seed ^ sample_word.rotate_left(((row * 11 + col * 7) % 63) as u32);
        let sign = if mixed & 1 == 0 { -1.0 } else { 1.0 };
        let activation_reference = ((mixed >> 8) & 0x03ff) as f32 / 1024.0;
        let gate_reference = ((reference_shard
            .gate_output_checksum
            .rotate_left((row % 63) as u32)
            >> 17)
            & 0xff) as f32
            / 255.0;
        let up_reference = ((reference_shard
            .up_output_checksum
            .rotate_left((col % 63) as u32)
            >> 23)
            & 0xff) as f32
            / 255.0;
        let value = 1.001
            + activation_reference * 0.004
            + gate_reference * 0.001
            + up_reference * 0.001
            + context.tanh() * 0.0005 * sign;
        out.extend_from_slice(&f32_to_f16_bits(value).to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_mlp_output_tile_from_intermediate(
    intermediate: &[u8],
    dim: usize,
    shard: Qwen3Dense06bShard,
) -> Vec<u8> {
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<f32>());
    let shard_scale = 0.83 + shard.shard_id as f32 * 0.0004;
    for elem_index in 0..elems {
        let value = qwen3_dense_0_6b_f32_at(intermediate, elem_index);
        let row = elem_index / dim;
        let col = elem_index % dim;
        let down_scale = shard_scale + ((row + col) % 23) as f32 * 0.0007;
        let residual_bias = 0.03125 + (row % 5) as f32 * 0.0005;
        let down = (value.ln_1p() * down_scale) + residual_bias;
        out.extend_from_slice(&down.to_le_bytes());
    }
    out
}

fn qwen3_dense_0_6b_residual_rmsnorm_tile_from_attention_and_mlp(
    attention_context: &[u8],
    mlp_output: &[u8],
    dim: usize,
    shard: Qwen3Dense06bShard,
) -> Vec<u8> {
    let elems = dim * dim;
    let mut residual = Vec::with_capacity(elems);
    for elem_index in 0..elems {
        let context = qwen3_dense_0_6b_half_at(attention_context, elem_index);
        let mlp = qwen3_dense_0_6b_f32_at(mlp_output, elem_index);
        residual.push(context + mlp);
    }

    let mut out = Vec::with_capacity(elems * std::mem::size_of::<f32>());
    for row in 0..dim {
        let row_begin = row * dim;
        let row_values = &residual[row_begin..row_begin + dim];
        let sum_sq = row_values.iter().map(|value| value * value).sum::<f32>();
        let inv_rms = 1.0f32 / ((sum_sq / dim as f32) + 1.0e-6).sqrt();
        for col in 0..dim {
            let gamma = 1.0 + (((col as u64 + shard.shard_id) % 11) as f32 * 0.0004);
            out.extend_from_slice(&(row_values[col] * inv_rms * gamma).to_le_bytes());
        }
    }
    out
}

fn qwen3_dense_0_6b_next_partial_tile_from_attention_context(
    attention_context: &[u8],
    dim: usize,
    shard: Qwen3Dense06bShard,
    guest_input_digest: u64,
) -> Vec<u8> {
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<f32>());
    let shard_scale = 1.0 + shard.shard_id as f32 * 0.0006;
    for elem_index in 0..elems {
        let context = qwen3_dense_0_6b_half_at(attention_context, elem_index);
        let row = elem_index / dim;
        let col = elem_index % dim;
        let position_bias = ((row * 3 + col * 5) % 31) as f32 * 0.00002;
        let input_bias = ((guest_input_digest.rotate_left((elem_index % 61) as u32) & 0x03ff)
            as f32)
            * 0.0000005;
        out.extend_from_slice(
            &((context.abs().ln_1p() * shard_scale) + position_bias + input_bias).to_le_bytes(),
        );
    }
    out
}

fn qwen3_dense_0_6b_round1_output_tile_from_remote(
    round1_output: &[u8],
    dim: usize,
    shard: Qwen3Dense06bShard,
    remote_shard: Qwen3Dense06bShard,
    remote_checksum: u64,
) -> Vec<u8> {
    let elems = dim * dim;
    let mut out = Vec::with_capacity(elems * std::mem::size_of::<f32>());
    let shard_scale = 1.0 + shard.shard_id as f32 * 0.0007;
    let remote_bias = remote_shard.shard_id as f32 * 0.0009;
    for elem_index in 0..elems {
        let value = qwen3_dense_0_6b_f32_at(round1_output, elem_index);
        let row = elem_index / dim;
        let col = elem_index % dim;
        let bounded = if value.is_finite() {
            value.abs().ln_1p() % 1.0
        } else {
            0.5
        };
        let position_bias = ((row + col) % 29) as f32 * 0.00003;
        let checksum_bias =
            ((remote_checksum.rotate_left((elem_index % 59) as u32) & 0x03ff) as f32) * 0.0000007;
        out.extend_from_slice(
            &(bounded * shard_scale + remote_bias + position_bias + checksum_bias).to_le_bytes(),
        );
    }
    out
}

fn qwen3_dense_0_6b_f32_at(bytes: &[u8], elem_index: usize) -> f32 {
    let byte_index = elem_index * std::mem::size_of::<f32>();
    if byte_index + std::mem::size_of::<f32>() <= bytes.len() {
        f32::from_le_bytes([
            bytes[byte_index],
            bytes[byte_index + 1],
            bytes[byte_index + 2],
            bytes[byte_index + 3],
        ])
    } else {
        0.0
    }
}

fn qwen3_dense_0_6b_half_at(bytes: &[u8], elem_index: usize) -> f32 {
    let byte_index = elem_index * std::mem::size_of::<u16>();
    let half_bits = if byte_index + std::mem::size_of::<u16>() <= bytes.len() {
        u16::from_le_bytes([bytes[byte_index], bytes[byte_index + 1]])
    } else {
        0
    };
    f16_bits_to_f32(half_bits)
}

fn f16_bits_to_f32(bits: u16) -> f32 {
    let sign = ((bits >> 15) & 0x1) as u32;
    let exponent = ((bits >> 10) & 0x1f) as i32;
    let fraction = (bits & 0x03ff) as u32;

    let f32_bits = if exponent == 0 {
        if fraction == 0 {
            sign << 31
        } else {
            let mut frac = fraction;
            let mut exp = -14i32;
            while (frac & 0x0400) == 0 {
                frac <<= 1;
                exp -= 1;
            }
            frac &= 0x03ff;
            (sign << 31) | (((exp + 127) as u32) << 23) | (frac << 13)
        }
    } else if exponent == 0x1f {
        (sign << 31) | 0x7f80_0000 | (fraction << 13)
    } else {
        (sign << 31) | (((exponent - 15 + 127) as u32) << 23) | (fraction << 13)
    };
    f32::from_bits(f32_bits)
}

fn f32_to_f16_bits(value: f32) -> u16 {
    let bits = value.to_bits();
    let sign = ((bits >> 16) & 0x8000) as u16;
    let exponent = ((bits >> 23) & 0xff) as i32;
    let fraction = bits & 0x007f_ffff;

    if exponent == 0xff {
        return sign | if fraction == 0 { 0x7c00 } else { 0x7e00 };
    }

    let half_exp = exponent - 127 + 15;
    if half_exp >= 0x1f {
        return sign | 0x7c00;
    }
    if half_exp <= 0 {
        if half_exp < -10 {
            return sign;
        }
        let mantissa = fraction | 0x0080_0000;
        let shift = (14 - half_exp) as u32;
        let mut half_fraction = (mantissa >> shift) as u16;
        if ((mantissa >> (shift - 1)) & 0x1) != 0 {
            half_fraction = half_fraction.wrapping_add(1);
        }
        return sign | half_fraction;
    }

    let mut half = sign | ((half_exp as u16) << 10) | ((fraction >> 13) as u16);
    if ((fraction >> 12) & 0x1) != 0 {
        half = half.wrapping_add(1);
    }
    half
}

fn repeated_u16_le_bytes(value: u16, count: usize) -> Vec<u8> {
    let bytes = value.to_le_bytes();
    let mut out = Vec::with_capacity(count * bytes.len());
    for _ in 0..count {
        out.extend_from_slice(&bytes);
    }
    out
}

fn dense_f32_binding(
    name: impl Into<String>,
    usage: BufferUsage,
    endpoint: MemoryEndpoint,
    elems: u64,
) -> DispatchBufferBinding {
    DispatchBufferBinding {
        name: name.into(),
        usage,
        endpoint,
        bytes: elems * std::mem::size_of::<f32>() as u64,
        dtype: TensorDType::F32,
        shape: vec![elems],
        layout: TensorLayout::Contiguous,
        strides: None,
        resident: false,
    }
}

fn opaque_resident_binding(
    name: impl Into<String>,
    usage: BufferUsage,
    endpoint: MemoryEndpoint,
    bytes: u64,
) -> DispatchBufferBinding {
    DispatchBufferBinding {
        name: name.into(),
        usage,
        endpoint,
        bytes,
        dtype: TensorDType::Opaque,
        shape: vec![bytes],
        layout: TensorLayout::Opaque,
        strides: None,
        resident: true,
    }
}

fn opaque_binding(
    name: impl Into<String>,
    usage: BufferUsage,
    endpoint: MemoryEndpoint,
    bytes: u64,
) -> DispatchBufferBinding {
    DispatchBufferBinding {
        name: name.into(),
        usage,
        endpoint,
        bytes,
        dtype: TensorDType::Opaque,
        shape: vec![bytes],
        layout: TensorLayout::Opaque,
        strides: None,
        resident: false,
    }
}

fn f32s_to_bytes(values: &[f32]) -> Vec<u8> {
    values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect()
}

fn bytes_to_f32s(bytes: &[u8]) -> Vec<f32> {
    bytes
        .chunks_exact(std::mem::size_of::<f32>())
        .map(|chunk| f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
        .collect()
}

fn runtime_kind_for_descriptor(desc: &UapiDescriptor) -> RuntimeWorkKind {
    match desc {
        UapiDescriptor::Io(_) => RuntimeWorkKind::GuestIo,
        UapiDescriptor::BlockWriteback { .. } => RuntimeWorkKind::BlockWriteback,
        UapiDescriptor::ShmemPut(_) => RuntimeWorkKind::ShmemPut,
        UapiDescriptor::ShmemGet(_) => RuntimeWorkKind::ShmemGet,
        UapiDescriptor::DfsRead(_) => RuntimeWorkKind::DfsRead,
        UapiDescriptor::DfsWrite(_) => RuntimeWorkKind::DfsWrite,
        UapiDescriptor::DbPut(_) => RuntimeWorkKind::DbPut,
        UapiDescriptor::DbGet(_) => RuntimeWorkKind::DbGet,
        UapiDescriptor::ObjectPublish(_) | UapiDescriptor::ObjectAppend(_) => {
            RuntimeWorkKind::DbPut
        }
        UapiDescriptor::ObjectResolve(_) => RuntimeWorkKind::DbGet,
    }
}

fn runtime_task_for_descriptor(desc: &UapiDescriptor) -> Option<TaskKey> {
    match desc {
        UapiDescriptor::Io(req) => req.task.clone(),
        UapiDescriptor::BlockWriteback { task, .. } => task.clone(),
        UapiDescriptor::ShmemPut(req) => req.task.clone(),
        UapiDescriptor::ShmemGet(req) => req.task.clone(),
        UapiDescriptor::DfsRead(req) => req.task.clone(),
        UapiDescriptor::DfsWrite(req) => req.task.clone(),
        UapiDescriptor::DbPut(req) => req.task.clone(),
        UapiDescriptor::DbGet(req) => req.task.clone(),
        UapiDescriptor::ObjectPublish(req) => req.task.clone(),
        UapiDescriptor::ObjectResolve(req) => req.task.clone(),
        UapiDescriptor::ObjectAppend(req) => req.task.clone(),
    }
}

#[cfg(test)]
mod tests {
    use super::{
        bytes_to_f32s, f32s_to_bytes, kvcache_input_b_payload,
        qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v,
        qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_and_kvcache_reference,
        qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_reference,
        qwen3_dense_0_6b_attention_score_tile_from_rope,
        qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_and_kvcache_reference,
        qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_reference,
        qwen3_dense_0_6b_bootstrap_node_ranges, qwen3_dense_0_6b_canonical_block_checksum,
        qwen3_dense_0_6b_decode_chain_checksum, qwen3_dense_0_6b_decode_duration_label,
        qwen3_dense_0_6b_decode_loop_report, qwen3_dense_0_6b_decode_loop_report_with_prompt,
        qwen3_dense_0_6b_decode_step_selected_samples,
        qwen3_dense_0_6b_fallback_weight_range_payload,
        qwen3_dense_0_6b_final_hidden_from_round1_outputs, qwen3_dense_0_6b_half_at,
        qwen3_dense_0_6b_hidden_layer_owner_node,
        qwen3_dense_0_6b_kvcache_tile_payload_from_projection, qwen3_dense_0_6b_logits_checksum,
        qwen3_dense_0_6b_mlp_activation_tile_from_attention_context,
        qwen3_dense_0_6b_object_metadata, qwen3_dense_0_6b_object_payload_words,
        qwen3_dense_0_6b_object_placement, qwen3_dense_0_6b_object_service_profile,
        qwen3_dense_0_6b_parse_weight_range_payload,
        qwen3_dense_0_6b_projection_tile_from_half_input,
        qwen3_dense_0_6b_publish_bootstrap_weight_objects,
        qwen3_dense_0_6b_range_forward_report_with_prompt,
        qwen3_dense_0_6b_real_mlp_activation_tile_from_attention_context,
        qwen3_dense_0_6b_real_mlp_reference_summary, qwen3_dense_0_6b_real_qkv_reference_values,
        qwen3_dense_0_6b_real_tokenizer_asset_summary, qwen3_dense_0_6b_real_tokenizer_path,
        qwen3_dense_0_6b_real_weight_reference_summary, qwen3_dense_0_6b_real_weight_stage_links,
        qwen3_dense_0_6b_required_layer_weight_kind_codes,
        qwen3_dense_0_6b_resolve_runtime_weight_objects,
        qwen3_dense_0_6b_resolve_weight_range_objects,
        qwen3_dense_0_6b_result_block_sample_offsets,
        qwen3_dense_0_6b_rmsnorm_tile_from_prefill_hidden,
        qwen3_dense_0_6b_rope_tile_from_projection, qwen3_dense_0_6b_sample_text_checksum,
        qwen3_dense_0_6b_sampled_token, qwen3_dense_0_6b_shard_output_checksum,
        qwen3_dense_0_6b_softmax_tile_from_attention_score,
        qwen3_dense_0_6b_text_output_bytes_checksum,
        qwen3_dense_0_6b_text_output_report_from_prefill_output,
        qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload,
        qwen3_dense_0_6b_tile_with_real_mlp_reference_mix,
        qwen3_dense_0_6b_tile_with_real_qkv_reference_mix,
        qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix, qwen3_dense_0_6b_token_piece,
        qwen3_dense_0_6b_tokenizer_sample_token_count,
        qwen3_dense_0_6b_validate_weight_payload_coverage,
        qwen3_dense_0_6b_weight_range_object_key, qwen3_dense_0_6b_weight_range_payload_header,
        qwen3_dense_0_6b_weight_reference_table_end, qwen3_dense_0_6b_weight_stage_link_table_end,
        qwen3_dense_0_6b_weight_tensor_kind_code, qwen3_dense_0_6b_write_weight_reference_table,
        qwen3_dense_0_6b_write_weight_stage_link_table, read_u64_le_at,
        run_host_matmul_batched_smoke, run_host_matmul_smoke, run_qwen3_dense_0_6b_prefill_runtime,
        GuestUapiSurface, KvCachePayloadLayout, LocalGuestUapiSurface,
        Qwen3Dense06bHiddenLayerNodeRange, Qwen3Dense06bLayerDependencyDescriptor,
        Qwen3Dense06bShard, Qwen3ProjectionKind, UapiCommand, UapiDescriptor, UapiResponse,
        QWEN3_DENSE_0_6B_PROFILE, QWEN3_DENSE_0_6B_TOKENIZER_POLICY_KIND,
        QWEN3_LOGITS_REFERENCE_ENTRY_WORDS, QWEN3_MLP_REFERENCE_ENTRY_WORDS,
        QWEN3_SYNTHETIC_ATTENTION_CONTEXT, QWEN3_SYNTHETIC_ATTENTION_SCORE,
        QWEN3_SYNTHETIC_GUEST_INPUT, QWEN3_SYNTHETIC_LOGITS_CANDIDATES,
        QWEN3_SYNTHETIC_MLP_ACTIVATION, QWEN3_SYNTHETIC_MLP_OUTPUT, QWEN3_SYNTHETIC_QKV_BASE_TILE,
        QWEN3_SYNTHETIC_TOKEN_TEXT, QWEN3_WEIGHT_REFERENCE_ENTRY_WORDS,
        QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS, W4_DEMO_KVCACHE_PAYLOAD_BYTES, W4_KVCACHE_BLOCKS,
        W4_KVCACHE_PREFIX_GROUPS, W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES,
    };
    use sim_config::ScenarioConfig;
    use sim_core::{
        BlockHash, CompletionStatus, HierarchyCoord, IoOpcode, IoSubmitReq, LogicalSystemId,
        SimError, TaskKey,
    };
    use sim_models::qwen3_dense_0_6b::{
        checksum_words, token_piece_bytes_from_policy, token_piece_bytes_from_tokenizer_path,
        token_piece_from_policy, token_piece_from_tokenizer_path, tokenizer_policy,
        weight_bytes_checksum, Qwen3Dense06bMlpReferenceLayerSummary,
        Qwen3Dense06bMlpReferenceShardSummary, Qwen3Dense06bQkvReferenceLayerSummary,
        Qwen3Dense06bQkvReferenceLayerValues, Qwen3Dense06bQkvReferenceShardSummary,
        Qwen3Dense06bQkvReferenceShardValues, Qwen3Dense06bReferenceWeightSliceValidation,
        Qwen3Dense06bWeightTensorKind, QWEN3_DENSE_0_6B_TOKENIZER_ASSET_POLICY_KIND,
    };
    use sim_services::block::BlockServiceProfile;
    use sim_services::{
        db::{DbGetReq, DbPutReq, DbServiceProfile},
        dfs::{DfsReadReq, DfsServiceProfile, DfsWriteReq},
        object::{
            LingquObjectKind, LingquObjectLocality, LingquObjectMetadata, LingquObjectPublishReq,
            LingquObjectResolveReq, LingquObjectServiceStub, LingquObjectState,
            LingquObjectVersionSelector, LingquPayloadBackend, LingquPayloadPlacement,
        },
        shmem::{ShmemGetReq, ShmemPutReq, ShmemServiceProfile},
    };
    use sim_topology::SimTopology;
    use std::collections::BTreeMap;
    use std::collections::BTreeSet;
    use std::path::PathBuf;
    use std::time::Duration;

    // The native simpler runtime owns process-global state, so run each workload in a fresh test process.
    fn run_simpler_native_test_isolated(test_name: &str, body: impl FnOnce()) {
        const CHILD_ENV: &str = "SIM_UAPI_SIMPLER_NATIVE_TEST_CHILD";

        if std::env::var(CHILD_ENV).as_deref() == Ok(test_name) {
            body();
            return;
        }

        let current_exe = std::env::current_exe().expect("current test binary");
        let status = std::process::Command::new(current_exe)
            .arg(test_name)
            .arg("--nocapture")
            .arg("--test-threads=1")
            .env(CHILD_ENV, test_name)
            .status()
            .expect("run isolated simpler native test");
        assert!(
            status.success(),
            "isolated simpler native test {test_name} failed with {status}"
        );
    }

    fn test_weight_reference_shard(
        shard_id: u64,
        checksum_base: u64,
    ) -> Qwen3Dense06bQkvReferenceShardSummary {
        Qwen3Dense06bQkvReferenceShardSummary {
            shard_id,
            hidden_size: 4,
            rmsnorm_checksum: checksum_base,
            rmsnorm_sample_words: [
                checksum_base + 11,
                checksum_base + 12,
                checksum_base + 13,
                checksum_base + 14,
            ],
            q_weight_checksum: checksum_base + 1,
            k_weight_checksum: checksum_base + 2,
            v_weight_checksum: checksum_base + 3,
            q_output_checksum: checksum_base + 4,
            q_output_sample_words: [
                checksum_base + 21,
                checksum_base + 22,
                checksum_base + 23,
                checksum_base + 24,
            ],
            k_output_checksum: checksum_base + 5,
            k_output_sample_words: [
                checksum_base + 31,
                checksum_base + 32,
                checksum_base + 33,
                checksum_base + 34,
            ],
            v_output_checksum: checksum_base + 6,
            v_output_sample_words: [
                checksum_base + 41,
                checksum_base + 42,
                checksum_base + 43,
                checksum_base + 44,
            ],
            q_rows: 2,
            k_rows: 1,
            v_rows: 1,
            weight_slices: vec![
                test_weight_reference_slice(Qwen3Dense06bWeightTensorKind::InputLayerNorm, 4),
                test_weight_reference_slice(Qwen3Dense06bWeightTensorKind::QProj, 8),
                test_weight_reference_slice(Qwen3Dense06bWeightTensorKind::KProj, 4),
                test_weight_reference_slice(Qwen3Dense06bWeightTensorKind::VProj, 4),
            ],
        }
    }

    fn test_weight_reference_values(
        shard_id: u64,
        checksum_base: u64,
    ) -> Qwen3Dense06bQkvReferenceShardValues {
        Qwen3Dense06bQkvReferenceShardValues {
            shard_id,
            hidden_size: 4,
            q_rows: 2,
            k_rows: 1,
            v_rows: 1,
            rmsnorm: vec![0.10, -0.20, 0.30, -0.40],
            q_output: vec![1.25, -1.50],
            k_output: vec![0.75],
            v_output: vec![-0.90],
            rmsnorm_checksum: checksum_base + 10,
            q_output_checksum: checksum_base + 20,
            k_output_checksum: checksum_base + 30,
            v_output_checksum: checksum_base + 40,
        }
    }

    fn test_weight_reference_slice(
        kind: Qwen3Dense06bWeightTensorKind,
        bytes: u64,
    ) -> Qwen3Dense06bReferenceWeightSliceValidation {
        Qwen3Dense06bReferenceWeightSliceValidation {
            kind,
            shape: vec![bytes / 4, 4],
            slice_axis: None,
            slice_start: 0,
            slice_end: bytes / 4,
            bytes,
            checksum: 0x1000 + bytes + kind as u64,
        }
    }

    fn test_mlp_reference_shard(
        shard_id: u64,
        checksum_base: u64,
    ) -> Qwen3Dense06bMlpReferenceShardSummary {
        Qwen3Dense06bMlpReferenceShardSummary {
            shard_id,
            hidden_size: 4,
            intermediate_rows: 3,
            gate_weight_checksum: checksum_base + 1,
            up_weight_checksum: checksum_base + 2,
            down_weight_checksum: checksum_base + 3,
            gate_output_checksum: checksum_base + 4,
            gate_output_sample_words: [
                checksum_base + 11,
                checksum_base + 12,
                checksum_base + 13,
                checksum_base + 14,
            ],
            up_output_checksum: checksum_base + 5,
            up_output_sample_words: [
                checksum_base + 21,
                checksum_base + 22,
                checksum_base + 23,
                checksum_base + 24,
            ],
            activation_checksum: checksum_base + 6,
            activation_sample_words: [
                checksum_base + 31,
                checksum_base + 32,
                checksum_base + 33,
                checksum_base + 34,
            ],
            down_output_checksum: checksum_base + 7,
            down_output_sample_words: [
                checksum_base + 41,
                checksum_base + 42,
                checksum_base + 43,
                checksum_base + 44,
            ],
            weight_slices: vec![
                test_weight_reference_slice(Qwen3Dense06bWeightTensorKind::GateProj, 8),
                test_weight_reference_slice(Qwen3Dense06bWeightTensorKind::UpProj, 8),
                test_weight_reference_slice(Qwen3Dense06bWeightTensorKind::DownProj, 8),
            ],
        }
    }

    #[test]
    fn kvcache_payload_layout_explicitly_maps_blocks_tiles_and_row_groups() {
        let elems = W4_DEMO_KVCACHE_PAYLOAD_BYTES / std::mem::size_of::<f32>();
        let layout = KvCachePayloadLayout::new(elems, W4_DEMO_KVCACHE_PAYLOAD_BYTES).unwrap();

        assert_eq!(layout.blocks.len(), W4_KVCACHE_BLOCKS);
        assert_eq!(layout.blocks[0].prefix_group_id, 0);
        assert_eq!(layout.blocks[1].prefix_group_id, 1);
        assert_eq!(layout.blocks[2].prefix_group_id, 0);
        assert_eq!(layout.blocks[3].prefix_group_id, 1);
        assert_eq!(W4_KVCACHE_PREFIX_GROUPS, 2);
        assert_eq!(layout.blocks[0].tiles.len(), 2);
        assert_eq!(layout.blocks[0].tiles[0].rows, 16);
        assert_eq!(layout.blocks[0].tiles[0].cols, 16);
        assert_eq!(layout.blocks[0].tiles[0].row_groups.len(), 4);
        assert_eq!(layout.blocks[0].tiles[0].row_groups[0].rows, 4);
        assert_eq!(layout.blocks[0].tiles[0].row_groups[0].cols, 16);
        assert!(layout.tile_row_group_for_elem(0).is_some());
        assert!(layout.tile_row_group_for_elem(elems - 1).is_some());
        assert!(layout.tile_row_group_for_elem(elems).is_none());
    }

    #[test]
    fn kvcache_input_b_encodes_prefix_block_tile_and_row_group_bias() {
        let elems = W4_DEMO_KVCACHE_PAYLOAD_BYTES / std::mem::size_of::<f32>();
        let layout = KvCachePayloadLayout::new(elems, W4_DEMO_KVCACHE_PAYLOAD_BYTES).unwrap();
        let values = bytes_to_f32s(&kvcache_input_b_payload(&layout));

        assert_eq!(values.len(), elems);
        assert!(values[0] < values[64]);
        assert!(values[64] < values[256]);
        assert!(values[512] > values[0]);
        assert!(values[1024] > values[0]);
        assert!(values[1536] > values[1024]);
    }

    #[test]
    fn qwen3_prefill_hidden_prefers_real_embedding_hidden_when_available() {
        let shard = Qwen3Dense06bShard {
            shard_id: 1,
            owner_node: 1,
            target_node: 1,
            head_start: 2,
            head_end: 4,
            kv_block_start: 2,
            kv_block_end: 4,
        };
        let guest_input = b"synthetic guest bytes should only feed fallback";
        let embedding_a = [0.10f32, -0.20, 0.30, -0.40, 0.50, -0.60, 0.70, -0.80];
        let embedding_b = [0.11f32, -0.21, 0.31, -0.41, 0.51, -0.61, 0.71, -0.81];

        let real_a = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
            guest_input,
            Some(&embedding_a),
            128,
            shard,
            0,
        );
        let real_b = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
            guest_input,
            Some(&embedding_b),
            128,
            shard,
            0,
        );
        let fallback = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
            guest_input,
            None,
            128,
            shard,
            0,
        );

        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&real_a),
            qwen3_dense_0_6b_shard_output_checksum(&real_b),
            "prefill hidden must depend on the real embedding vector"
        );
        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&real_a),
            qwen3_dense_0_6b_shard_output_checksum(&fallback),
            "real embedding path must replace the guest-byte fallback"
        );
    }

    #[test]
    fn qwen3_final_hidden_is_assembled_from_round1_tile_outputs() {
        let tile_count = 16usize;
        let values_per_tile = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize / tile_count;
        let mut outputs = Vec::with_capacity(tile_count);
        for tile in (0..tile_count).rev() {
            let shard = Qwen3Dense06bShard {
                shard_id: (tile / 2) as u64,
                owner_node: (tile / 2) as u64,
                target_node: (tile / 2) as u64,
                head_start: 0,
                head_end: 0,
                kv_block_start: (tile * 2) as u64,
                kv_block_end: (tile * 2 + 2) as u64,
            };
            let values = (0..128usize)
                .map(|index| tile as f32 * 1_000.0 + index as f32)
                .collect::<Vec<_>>();
            outputs.push((
                shard,
                f32s_to_bytes(&values),
                sim_core::SegmentHandle(tile as u64 + 1),
                tile as u64 + 11,
            ));
        }

        let hidden = qwen3_dense_0_6b_final_hidden_from_round1_outputs(&outputs)
            .expect("final hidden")
            .expect("hidden present");

        assert_eq!(hidden.len(), QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize);
        assert_eq!(hidden[0], 0.0);
        assert_eq!(hidden[values_per_tile - 1], (values_per_tile - 1) as f32);
        assert_eq!(hidden[values_per_tile], 1_000.0);
        assert_eq!(hidden[values_per_tile * 15], 15_000.0);
    }

    #[test]
    fn qwen3_weight_reference_table_encodes_real_weight_contract() {
        let summary = Qwen3Dense06bQkvReferenceLayerSummary {
            layer_id: 0,
            shard_count: 2,
            total_weight_bytes: 40,
            total_q_rows: 4,
            total_k_rows: 2,
            total_v_rows: 2,
            aggregate_checksum: 0xabc,
            shards: vec![
                test_weight_reference_shard(0, 0x10),
                test_weight_reference_shard(1, 0x20),
            ],
        };
        let table_header = 256;
        let marker = 0x7133773477667430;
        let table_end = qwen3_dense_0_6b_weight_reference_table_end(table_header, Some(&summary));
        let mut output = vec![0u8; table_end + 16];
        qwen3_dense_0_6b_write_weight_reference_table(&mut output, table_header, marker, &summary);

        assert_eq!(read_u64_le_at(&output, table_header), marker);
        assert_eq!(read_u64_le_at(&output, table_header + 8), 2);
        assert_eq!(
            read_u64_le_at(&output, table_header + 16),
            QWEN3_WEIGHT_REFERENCE_ENTRY_WORDS
        );
        assert_eq!(read_u64_le_at(&output, table_header + 24), 224);
        assert_eq!(read_u64_le_at(&output, table_header + 40), 40);
        assert_eq!(read_u64_le_at(&output, table_header + 48), 0xabc);
        assert_eq!(read_u64_le_at(&output, table_header + 56), 8);
        let first_entry = table_header + 64;
        assert_eq!(read_u64_le_at(&output, first_entry), 0);
        assert_eq!(read_u64_le_at(&output, first_entry + 8), 4);
        assert_eq!(read_u64_le_at(&output, first_entry + 16), 0x10);
        assert_eq!(read_u64_le_at(&output, first_entry + 72), 2);
        assert_eq!(read_u64_le_at(&output, first_entry + 80), 1);
        assert_eq!(read_u64_le_at(&output, first_entry + 88), 1);
        assert_eq!(read_u64_le_at(&output, first_entry + 96), 4);
        assert_ne!(read_u64_le_at(&output, first_entry + 104), 0);
    }

    #[test]
    fn qwen3_real_qkv_value_mix_depends_on_numeric_reference_values() {
        let shard = Qwen3Dense06bShard {
            shard_id: 0,
            owner_node: 0,
            target_node: 0,
            head_start: 0,
            head_end: 2,
            kv_block_start: 0,
            kv_block_end: 1,
        };
        let summary = Qwen3Dense06bQkvReferenceLayerSummary {
            layer_id: 0,
            shard_count: 1,
            total_weight_bytes: 20,
            total_q_rows: 2,
            total_k_rows: 1,
            total_v_rows: 1,
            aggregate_checksum: 0xabc,
            shards: vec![test_weight_reference_shard(0, 0x10)],
        };
        let values = Qwen3Dense06bQkvReferenceLayerValues {
            layer_id: 0,
            shard_count: 1,
            shards: vec![test_weight_reference_values(0, 0x100)],
            aggregate_checksum: 0xdef,
        };
        let mut changed_values = values.clone();
        changed_values.shards[0].q_output[1] = 3.5;

        let mut tile = Vec::new();
        for idx in 0..16u16 {
            tile.extend_from_slice(&(0x3c00 + idx).to_le_bytes());
        }

        let checksum_only =
            qwen3_dense_0_6b_tile_with_real_qkv_reference_mix(&tile, 4, 2, shard, Some(&summary));
        let numeric = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
            &tile,
            4,
            2,
            shard,
            Some(&values),
            Some(&summary),
        );
        let changed_numeric = qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
            &tile,
            4,
            2,
            shard,
            Some(&changed_values),
            Some(&summary),
        );

        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&numeric),
            qwen3_dense_0_6b_shard_output_checksum(&checksum_only),
            "Q projection tile must include the real numeric reference values"
        );
        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&changed_numeric),
            qwen3_dense_0_6b_shard_output_checksum(&numeric),
            "changing Q reference values must change the generated tile"
        );
    }

    #[test]
    fn qwen3_real_weight_stage_link_table_maps_dependency_stages() {
        let summary = Qwen3Dense06bQkvReferenceLayerSummary {
            layer_id: 0,
            shard_count: 2,
            total_weight_bytes: 40,
            total_q_rows: 4,
            total_k_rows: 2,
            total_v_rows: 2,
            aggregate_checksum: 0xabc,
            shards: vec![
                test_weight_reference_shard(0, 0x10),
                test_weight_reference_shard(1, 0x20),
            ],
        };
        let values = Qwen3Dense06bQkvReferenceLayerValues {
            layer_id: 0,
            shard_count: 2,
            shards: vec![
                test_weight_reference_values(0, 0x100),
                test_weight_reference_values(1, 0x200),
            ],
            aggregate_checksum: 0xdef,
        };
        let dependencies = vec![
            test_layer_dependency_descriptor(0, 0, 1, 0x1000, 0x2000),
            test_layer_dependency_descriptor(0, 0, 2, 0x1001, 0x2001),
            test_layer_dependency_descriptor(0, 0, 3, 0x1002, 0x2002),
            test_layer_dependency_descriptor(0, 0, 4, 0x1003, 0x2003),
            test_layer_dependency_descriptor(0, 0, 5, 0x1004, 0x2004),
            test_layer_dependency_descriptor(1, 1, 1, 0x1100, 0x2100),
        ];
        let links = qwen3_dense_0_6b_real_weight_stage_links(
            Some(&summary),
            Some(&values),
            None,
            None,
            &dependencies,
        );
        assert_eq!(links.len(), 5);
        assert_eq!(links[0].stage_kind, 1);
        assert_eq!(links[0].real_weight_checksum, 0x1004);
        assert_eq!(links[0].real_output_checksum, 0x10);
        assert_eq!(links[0].real_value_checksum, 0x10a);
        assert_eq!(links[0].rows, 4);
        assert_eq!(links[1].stage_kind, 2);
        assert_eq!(links[1].real_weight_checksum, 0x11);
        assert_eq!(links[1].real_output_checksum, 0x14);
        assert_eq!(links[1].real_value_checksum, 0x114);
        assert_eq!(links[1].rows, 2);
        assert_eq!(links[3].stage_kind, 4);
        assert_eq!(links[3].real_weight_checksum, 0x13);
        assert_eq!(links[3].real_output_checksum, 0x16);
        assert_eq!(links[3].real_value_checksum, 0x128);
        assert_eq!(links[4].shard_id, 1);
        assert_eq!(links[4].real_weight_checksum, 0x1004);
        assert_eq!(links[4].real_value_checksum, 0x20a);

        let table_header = 128usize;
        let table_base = table_header + 64;
        let table_end = qwen3_dense_0_6b_weight_stage_link_table_end(table_header, &links);
        let mut output = vec![0u8; table_end];
        qwen3_dense_0_6b_write_weight_stage_link_table(
            &mut output,
            table_header,
            0x71337734776c6b30,
            &links,
        );
        assert_eq!(read_u64_le_at(&output, table_header), 0x71337734776c6b30);
        assert_eq!(read_u64_le_at(&output, table_header + 8), 5);
        assert_eq!(
            read_u64_le_at(&output, table_header + 16),
            QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS
        );
        assert_eq!(
            read_u64_le_at(&output, table_header + 24),
            5 * QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS * 8
        );
        assert_ne!(read_u64_le_at(&output, table_header + 32), 0);
        assert_eq!(read_u64_le_at(&output, table_header + 40), 4);
        assert_eq!(read_u64_le_at(&output, table_base), 0);
        assert_eq!(read_u64_le_at(&output, table_base + 8), 0);
        assert_eq!(read_u64_le_at(&output, table_base + 16), 1);
        assert_eq!(read_u64_le_at(&output, table_base + 24), 0x1000);
        assert_eq!(read_u64_le_at(&output, table_base + 32), 0x2000);
        assert_eq!(read_u64_le_at(&output, table_base + 40), 0x1004);
        assert_eq!(read_u64_le_at(&output, table_base + 48), 0x10);
        assert_eq!(read_u64_le_at(&output, table_base + 56), 0x10a);
    }

    fn test_layer_dependency_descriptor(
        tile_id: u64,
        shard_id: u64,
        stage_kind: u64,
        segment: u64,
        checksum: u64,
    ) -> Qwen3Dense06bLayerDependencyDescriptor {
        Qwen3Dense06bLayerDependencyDescriptor {
            layer_id: tile_id,
            shard_id,
            stage_kind,
            depends_on_stage: stage_kind.saturating_sub(1),
            remote_shard_id: shard_id,
            segment,
            elems: 16_384,
            bytes: 32_768,
            head_start: shard_id * 2,
            head_end: shard_id * 2 + 2,
            checksum,
        }
    }

    #[test]
    fn host_matmul_dispatch_accepts_manifest_artifact() {
        run_simpler_native_test_isolated("host_matmul_dispatch_accepts_manifest_artifact", || {
            let topology = test_topology();
            let output = run_host_matmul_smoke(
                &topology,
                &TaskKey {
                    logical_system: LogicalSystemId(1),
                    coord: HierarchyCoord { levels: [0; 8] },
                    scope_depth: 0,
                    task_id: 99,
                },
            )
            .expect("host matmul dispatch");
            assert_eq!(output.len(), 128 * 128 * std::mem::size_of::<f32>());
        });
    }

    #[test]
    fn host_matmul_batched_dispatch_accepts_manifest_artifact() {
        run_simpler_native_test_isolated(
            "host_matmul_batched_dispatch_accepts_manifest_artifact",
            || {
                let Ok(manifest_path) =
                    std::env::var("SIMPLER_HOST_MATMUL_BATCH_MANIFEST").map(PathBuf::from)
                else {
                    return;
                };
                let topology = test_topology();
                let output = run_host_matmul_batched_smoke(
                    &topology,
                    &TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 199,
                    },
                    &manifest_path,
                    2,
                )
                .expect("host matmul batched dispatch");
                assert_eq!(output.len(), 2 * 128 * 128 * std::mem::size_of::<f32>());
            },
        );
    }

    #[test]
    fn host_matmul_repeated_single_then_batched_dispatch_accepts_manifest_artifact() {
        run_simpler_native_test_isolated(
            "host_matmul_repeated_single_then_batched_dispatch_accepts_manifest_artifact",
            || {
                let Ok(manifest_path) =
                    std::env::var("SIMPLER_HOST_MATMUL_BATCH_MANIFEST").map(PathBuf::from)
                else {
                    return;
                };
                let single_count = std::env::var("SIMPLER_HOST_MATMUL_SINGLE_BEFORE_BATCH")
                    .ok()
                    .and_then(|value| value.parse::<u64>().ok())
                    .unwrap_or(16);
                let topology = test_topology();
                for index in 0..single_count {
                    let output = run_host_matmul_smoke(
                        &topology,
                        &TaskKey {
                            logical_system: LogicalSystemId(1),
                            coord: HierarchyCoord { levels: [0; 8] },
                            scope_depth: 0,
                            task_id: 300 + index,
                        },
                    )
                    .expect("host matmul dispatch before batched dispatch");
                    assert_eq!(output.len(), 128 * 128 * std::mem::size_of::<f32>());
                }
                let output = run_host_matmul_batched_smoke(
                    &topology,
                    &TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 399,
                    },
                    &manifest_path,
                    2,
                )
                .expect("host matmul batched dispatch after repeated single dispatch");
                assert_eq!(output.len(), 2 * 128 * 128 * std::mem::size_of::<f32>());
            },
        );
    }

    #[test]
    fn host_matmul_batched_then_single_dispatch_accepts_manifest_artifact() {
        run_simpler_native_test_isolated(
            "host_matmul_batched_then_single_dispatch_accepts_manifest_artifact",
            || {
                let Ok(manifest_path) =
                    std::env::var("SIMPLER_HOST_MATMUL_BATCH_MANIFEST").map(PathBuf::from)
                else {
                    return;
                };
                let topology = test_topology();
                let output = run_host_matmul_batched_smoke(
                    &topology,
                    &TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 499,
                    },
                    &manifest_path,
                    2,
                )
                .expect("host matmul batched dispatch before single dispatch");
                assert_eq!(output.len(), 2 * 128 * 128 * std::mem::size_of::<f32>());
                let output = run_host_matmul_smoke(
                    &topology,
                    &TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 500,
                    },
                )
                .expect("host matmul dispatch after batched dispatch");
                assert_eq!(output.len(), 128 * 128 * std::mem::size_of::<f32>());
            },
        );
    }

    #[test]
    fn qwen3_dense_0_6b_prefill_profile_uses_host_matmul_artifact() {
        run_simpler_native_test_isolated(
            "qwen3_dense_0_6b_prefill_profile_uses_host_matmul_artifact",
            || {
                let topology = test_topology();
                let guest_input = [0xa5; W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES];
                let output = run_qwen3_dense_0_6b_prefill_runtime(
                    &topology,
                    &TaskKey {
                        logical_system: LogicalSystemId(1),
                        coord: HierarchyCoord { levels: [0; 8] },
                        scope_depth: 0,
                        task_id: 100,
                    },
                    &guest_input,
                    None,
                )
                .expect("qwen3 dense 0.6b shard-aware prefill dispatch");
                assert_qwen3_dense_0_6b_prefill_profile_output(&output, &guest_input);
            },
        );
    }

    #[test]
    fn qwen3_dense_0_6b_runtime_weight_resolve_requires_bootstrap_objects() {
        let mut service = LingquObjectServiceStub::new(qwen3_dense_0_6b_object_service_profile());
        let err = qwen3_dense_0_6b_resolve_runtime_weight_objects(&mut service, 900_000)
            .expect_err("runtime weight resolve should fail without bootstrap objects");
        assert!(
            err.contains("qwen3_weight_object_missing"),
            "unexpected error: {err}"
        );
    }

    #[test]
    fn qwen3_dense_0_6b_runtime_weight_resolve_validates_payload_checksum() {
        let range = qwen3_dense_0_6b_bootstrap_node_ranges()
            .into_iter()
            .next()
            .expect("first weight range");
        let key = qwen3_dense_0_6b_weight_range_object_key(&range);
        let payload = qwen3_dense_0_6b_fallback_weight_range_payload(&range);
        let checksum = weight_bytes_checksum(&payload);
        let mut service = LingquObjectServiceStub::new(qwen3_dense_0_6b_object_service_profile());
        service
            .submit_publish(
                LingquObjectPublishReq {
                    task: None,
                    key: key.clone(),
                    kind: LingquObjectKind::WeightShard,
                    producer_entity: range.node_id,
                    owner_entity: Some(range.node_id),
                    expected_version: None,
                    metadata: qwen3_dense_0_6b_object_metadata(payload.len() as u64, checksum ^ 1),
                    placements: vec![qwen3_dense_0_6b_object_placement(
                        &key,
                        LingquPayloadBackend::Block,
                        payload.len() as u64,
                        checksum ^ 1,
                    )],
                    payload_bytes: payload,
                },
                901_000,
            )
            .expect("publish corrupt weight object");
        assert_eq!(service.poll_ready(901_100).len(), 1);
        let err = qwen3_dense_0_6b_resolve_weight_range_objects(&mut service, &[range], 902_000)
            .expect_err("runtime weight resolve should reject checksum mismatch");
        assert!(
            err.contains("qwen3_weight_object_payload_checksum_mismatch"),
            "unexpected error: {err}"
        );
    }

    #[test]
    fn qwen3_dense_0_6b_weight_payload_view_parses_slice_records() {
        let range = qwen3_dense_0_6b_bootstrap_node_ranges()
            .into_iter()
            .next()
            .expect("first weight range");
        let key = qwen3_dense_0_6b_weight_range_object_key(&range);
        let slice_payload = vec![1u8, 2, 3, 4, 5, 6, 7, 8];
        let slice_checksum = weight_bytes_checksum(&slice_payload);
        let record_offset = 5 * std::mem::size_of::<u64>();
        let payload_offset = record_offset + 11 * std::mem::size_of::<u64>();
        let mut payload = qwen3_dense_0_6b_weight_range_payload_header(&range);
        payload[4 * std::mem::size_of::<u64>()..5 * std::mem::size_of::<u64>()]
            .copy_from_slice(&1u64.to_le_bytes());
        payload.extend_from_slice(&qwen3_dense_0_6b_object_payload_words(&[
            range.first_layer_id,
            0,
            qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::QProj),
            2,
            0,
            0,
            128,
            slice_payload.len() as u64,
            slice_checksum,
            payload_offset as u64,
            checksum_words(&[128, QWEN3_DENSE_0_6B_PROFILE.hidden_size]),
        ]));
        payload.extend_from_slice(&slice_payload);
        let view = qwen3_dense_0_6b_parse_weight_range_payload(&range, &key, &payload, true)
            .expect("parse weight object payload view");
        assert_eq!(view.node_id, range.node_id);
        assert_eq!(view.first_layer_id, range.first_layer_id);
        assert_eq!(view.last_layer_id, range.last_layer_id);
        assert_eq!(view.slices.len(), 1);
        assert_eq!(view.slice_payload_bytes, slice_payload.len() as u64);
        assert_ne!(view.slice_checksum, 0);
        assert_eq!(view.slices[0].layer_id, range.first_layer_id);
        assert_eq!(view.slices[0].payload_offset, payload_offset as u64);
        assert_eq!(view.slices[0].payload_checksum, slice_checksum);
    }

    #[test]
    fn qwen3_dense_0_6b_weight_payload_view_rejects_bad_slice_checksum() {
        let range = qwen3_dense_0_6b_bootstrap_node_ranges()
            .into_iter()
            .next()
            .expect("first weight range");
        let key = qwen3_dense_0_6b_weight_range_object_key(&range);
        let slice_payload = vec![9u8, 8, 7, 6];
        let record_offset = 5 * std::mem::size_of::<u64>();
        let payload_offset = record_offset + 11 * std::mem::size_of::<u64>();
        let mut payload = qwen3_dense_0_6b_weight_range_payload_header(&range);
        payload[4 * std::mem::size_of::<u64>()..5 * std::mem::size_of::<u64>()]
            .copy_from_slice(&1u64.to_le_bytes());
        payload.extend_from_slice(&qwen3_dense_0_6b_object_payload_words(&[
            range.first_layer_id,
            0,
            qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::QProj),
            2,
            0,
            0,
            128,
            slice_payload.len() as u64,
            weight_bytes_checksum(&slice_payload) ^ 1,
            payload_offset as u64,
            checksum_words(&[128, QWEN3_DENSE_0_6B_PROFILE.hidden_size]),
        ]));
        payload.extend_from_slice(&slice_payload);
        let err = qwen3_dense_0_6b_parse_weight_range_payload(&range, &key, &payload, true)
            .expect_err("bad slice checksum should fail");
        assert!(
            err.contains("qwen3_weight_object_slice_checksum_mismatch"),
            "unexpected error: {err}"
        );
    }

    #[test]
    fn qwen3_dense_0_6b_weight_payload_coverage_requires_every_layer_shard_kind() {
        let range = Qwen3Dense06bHiddenLayerNodeRange {
            node_id: 0,
            first_layer_id: 0,
            last_layer_id: 0,
            layer_count: 1,
        };
        let mut coverage = BTreeSet::new();
        for shard_id in 0..QWEN3_DENSE_0_6B_PROFILE.tp_nodes {
            for kind_code in qwen3_dense_0_6b_required_layer_weight_kind_codes() {
                coverage.insert((0, shard_id, kind_code));
            }
        }
        assert!(qwen3_dense_0_6b_validate_weight_payload_coverage(
            std::slice::from_ref(&range),
            &coverage,
            true,
        )
        .expect("complete payload coverage"));
        coverage.remove(&(
            0,
            3,
            qwen3_dense_0_6b_weight_tensor_kind_code(Qwen3Dense06bWeightTensorKind::KNorm),
        ));
        let err = qwen3_dense_0_6b_validate_weight_payload_coverage(
            std::slice::from_ref(&range),
            &coverage,
            true,
        )
        .expect_err("missing k_norm slice should fail coverage");
        assert!(
            err.contains("qwen3_weight_object_payload_missing_slice:layer=0:shard=3"),
            "unexpected error: {err}"
        );
    }

    #[test]
    fn qwen3_dense_0_6b_runtime_weight_resolve_accepts_bootstrap_objects() {
        let topology = test_topology();
        let mut service = LingquObjectServiceStub::new(qwen3_dense_0_6b_object_service_profile());
        qwen3_dense_0_6b_publish_bootstrap_weight_objects(&topology, &mut service)
            .expect("bootstrap weight objects");
        let resolved = qwen3_dense_0_6b_resolve_runtime_weight_objects(&mut service, 903_000)
            .expect("runtime weight resolve");
        assert_eq!(resolved.object_count, QWEN3_DENSE_0_6B_PROFILE.tp_nodes);
        assert_ne!(resolved.payload_bytes, 0);
        assert_ne!(resolved.payload_checksum, 0);
        if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok() {
            assert_eq!(resolved.global_object_count, 1);
            assert_ne!(resolved.global_payload_bytes, 0);
            assert_eq!(resolved.global_tensor_count, 3);
            assert_ne!(resolved.global_payload_checksum, 0);
            assert_ne!(resolved.slice_count, 0);
            assert_ne!(resolved.slice_payload_bytes, 0);
            assert_ne!(resolved.slice_checksum, 0);
            assert_eq!(
                resolved.slice_count,
                QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                    * QWEN3_DENSE_0_6B_PROFILE.tp_nodes
                    * qwen3_dense_0_6b_required_layer_weight_kind_codes().len() as u64
            );
            assert!(resolved.payload_complete);
            assert!(resolved
                .layer_payloads
                .contains_key("model.embed_tokens.weight"));
            assert!(resolved.layer_payloads.contains_key("model.norm.weight"));
            assert!(resolved.layer_payloads.contains_key("lm_head.weight"));
            assert_eq!(
                resolved.reconstructed_tensor_count,
                QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                    * qwen3_dense_0_6b_required_layer_weight_kind_codes().len() as u64
            );
            assert_ne!(resolved.reconstructed_tensor_checksum, 0);
        }
    }

    #[test]
    fn qwen3_dense_0_6b_range_forward_publishes_worker_handoffs() {
        let topology = test_topology();
        let result = qwen3_dense_0_6b_range_forward_report_with_prompt(&topology, "Hello Qwen3");
        if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_err() {
            let err = result.expect_err("missing weights path should fail range forward");
            assert!(
                err.contains("qwen3_range_forward_weights_path_missing")
                    || err.contains("qwen3_range_forward_tokenizer_path_missing"),
                "unexpected error: {err}"
            );
            return;
        }
        let report = result.expect("range forward report");
        assert!(report.ready);
        assert_eq!(report.node_count, QWEN3_DENSE_0_6B_PROFILE.tp_nodes);
        assert_eq!(
            report.layer_count,
            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
        );
        assert_eq!(
            report.workers.len() as u64,
            QWEN3_DENSE_0_6B_PROFILE.tp_nodes
        );
        assert_eq!(report.handoff_match_count, report.node_count);
        assert_eq!(report.hidden_object_count, report.node_count * 2);
        assert_eq!(
            report.weight_object_count,
            QWEN3_DENSE_0_6B_PROFILE.tp_nodes
        );
        assert_eq!(report.global_weight_object_count, 1);
        assert_ne!(report.aggregate_checksum, 0);
        for pair in report.workers.windows(2) {
            assert_eq!(pair[0].last_layer_id + 1, pair[1].first_layer_id);
            assert_eq!(
                pair[0].output_payload_checksum,
                pair[1].input_payload_checksum
            );
        }
        for worker in &report.workers {
            assert_ne!(worker.input_payload_bytes, 0);
            assert_ne!(worker.output_payload_bytes, 0);
            assert_ne!(worker.weight_payload_bytes, 0);
            assert_ne!(worker.weight_payload_slice_count, 0);
            assert_ne!(worker.weight_reconstructed_tensor_count, 0);
            assert!(worker.handoff_input_matches_previous_output);
        }
    }

    #[test]
    fn qwen3_dense_0_6b_decode_loop_feeds_sampled_tokens_forward() {
        run_simpler_native_test_isolated(
            "qwen3_dense_0_6b_decode_loop_feeds_sampled_tokens_forward",
            || {
                let topology = test_topology();
                let report =
                    qwen3_dense_0_6b_decode_loop_report(&topology, 2).expect("decode loop report");
                assert_eq!(report.steps.len(), 2);
                assert_eq!(
                    report.final_guest_input_checksum,
                    report
                        .steps
                        .last()
                        .expect("last decode step")
                        .next_guest_input_checksum
                );
                assert_ne!(report.decode_chain_checksum, 0);
                assert_eq!(
                    report.decode_chain_checksum,
                    qwen3_dense_0_6b_decode_chain_checksum(&report.steps)
                );
                assert_ne!(
                    report.decode_chain_checksum,
                    report.final_guest_input_checksum
                );
                assert_eq!(
                    report.generated_byte_len,
                    report
                        .steps
                        .iter()
                        .map(|step| {
                            qwen3_dense_0_6b_decode_step_selected_samples(&step.text_output.samples)
                                .iter()
                                .map(|sample| sample.byte_len)
                                .sum::<u64>()
                        })
                        .sum::<u64>()
                );
                assert_ne!(report.generated_byte_checksum, 0);
                assert_ne!(
                    report.steps[0].guest_input_checksum,
                    report.steps[0].next_guest_input_checksum
                );
                assert_eq!(
                    report.steps[1].guest_input_checksum,
                    report.steps[0].next_guest_input_checksum
                );
                assert!(report.steps[0].runtime_prefill_executed);
                assert!(!report.steps[1].runtime_prefill_executed);
                assert_ne!(
                    report.steps[1].guest_input_checksum,
                    report.steps[1].next_guest_input_checksum
                );
                assert_eq!(report.steps[0].sampled_token_count, 1);
                assert_eq!(report.steps[1].sampled_token_count, 1);
                assert_eq!(report.steps[0].input_transition.loop_step, 0);
                assert_eq!(report.steps[1].input_transition.loop_step, 1);
                assert_eq!(report.steps[0].input_transition.write_count, 1);
                assert_eq!(report.steps[1].input_transition.write_count, 1);
                assert_eq!(report.steps[0].input_transition.applied_write_count, 1);
                assert_eq!(report.steps[1].input_transition.applied_write_count, 1);
                assert_eq!(
                    report.steps[0].input_transition.write_readback_match_count,
                    1
                );
                assert_eq!(
                    report.steps[1].input_transition.write_readback_match_count,
                    1
                );
                assert_eq!(
                    report.steps[0].input_transition.write_count,
                    report.steps[0].sampled_token_count
                );
                assert_eq!(
                    report.steps[1].input_transition.write_count,
                    report.steps[1].sampled_token_count
                );
                assert_ne!(report.steps[0].input_transition.transition_checksum, 0);
                assert_ne!(report.steps[1].input_transition.transition_checksum, 0);
                assert_ne!(
                    report.steps[0].input_transition.transition_checksum,
                    report.steps[1].input_transition.transition_checksum
                );
                assert_ne!(
                    report.steps[0].input_transition.write_offset_checksum,
                    report.steps[1].input_transition.write_offset_checksum
                );
                assert_ne!(
                    report.steps[0].input_transition.sampled_token_checksum,
                    report.steps[1].input_transition.sampled_token_checksum
                );
                assert_eq!(
                    report.steps[0].input_transition.readback_token_checksum,
                    report.steps[0].input_transition.sampled_token_checksum
                );
                assert_eq!(
                    report.steps[1].input_transition.readback_token_checksum,
                    report.steps[1].input_transition.sampled_token_checksum
                );
                assert_eq!(
                    report.steps[0].input_transition.checksum_slot_value,
                    report.steps[0].input_transition.transition_checksum
                );
                assert_eq!(
                    report.steps[1].input_transition.checksum_slot_value,
                    report.steps[1].input_transition.transition_checksum
                );
                assert_eq!(
                    report.steps[0].input_transition.logits_checksum,
                    checksum_words(
                        &qwen3_dense_0_6b_decode_step_selected_samples(
                            &report.steps[0].text_output.samples
                        )
                        .iter()
                        .flat_map(|sample| [sample.step_index, sample.logits_checksum])
                        .collect::<Vec<_>>()
                    )
                );
                assert_eq!(
                    report.steps[1].input_transition.text_checksum,
                    checksum_words(
                        &qwen3_dense_0_6b_decode_step_selected_samples(
                            &report.steps[1].text_output.samples
                        )
                        .iter()
                        .flat_map(|sample| [sample.step_index, sample.text_checksum])
                        .collect::<Vec<_>>()
                    )
                );
                let prefill_tile_count = QWEN3_DENSE_0_6B_PROFILE.tp_nodes * 2;
                let prefill_descriptor_count =
                    prefill_tile_count * QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers * 2;
                let prefill_state_count =
                    prefill_tile_count * QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers * 3;
                let incremental_decode_entry_count =
                    QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers * QWEN3_DENSE_0_6B_PROFILE.tp_nodes;
                assert_eq!(
                    report.steps[0].text_output.kvcache.descriptor_count,
                    prefill_descriptor_count
                );
                assert_eq!(
                    report.steps[1].text_output.kvcache.descriptor_count,
                    incremental_decode_entry_count
                );
                assert_eq!(
                    report.steps[0].text_output.kvcache.state_count,
                    report.steps[0].text_output.kvcache.append_block_count
                );
                assert_eq!(
                    report.steps[0].text_output.kvcache.state_count,
                    prefill_state_count
                );
                assert!(
                    report.steps[1].text_output.kvcache.state_count
                        > report.steps[0].text_output.kvcache.state_count
                );
                assert_eq!(
                    report.steps[0].text_output.kvcache.prefill_entry_count,
                    prefill_tile_count * QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                );
                assert_eq!(
                    report.steps[0].text_output.kvcache.decode_entry_count,
                    prefill_tile_count * QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                );
                assert_eq!(report.steps[1].text_output.kvcache.prefill_entry_count, 0);
                assert_eq!(
                    report.steps[1].text_output.kvcache.decode_entry_count,
                    incremental_decode_entry_count
                );
                assert_eq!(
                    report.steps[0].text_output.kvcache.state_snapshots.len() as u64,
                    report.steps[0].text_output.kvcache.state_count
                );
                assert_eq!(
                    report.steps[1].text_output.kvcache.state_snapshots.len() as u64,
                    report.steps[1].text_output.kvcache.state_count
                );
                assert_eq!(
                    report.steps[1].text_output.kvcache.state_count,
                    report.steps[0].text_output.kvcache.state_count
                        + incremental_decode_entry_count
                );
                assert_ne!(
                    report.steps[0].text_output.kvcache.read_digest_checksum,
                    report.steps[1].text_output.kvcache.read_digest_checksum
                );
                assert_ne!(
                    report.steps[0].text_output.logits_checksum,
                    report.steps[1].text_output.logits_checksum
                );
                for step in &report.steps {
                    let range_object_count = QWEN3_DENSE_0_6B_PROFILE.tp_nodes * 2;
                    let decode_object_count = 5 + range_object_count;
                    let range_handoff_object_op_count =
                        step.hidden_layer_pipeline.boundary_count * 2;
                    let runtime_weight_resolve_count =
                        if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok()
                            && step.text_output.guest_input.prompt_token_count != 0
                        {
                            QWEN3_DENSE_0_6B_PROFILE.tp_nodes + 1
                        } else {
                            0
                        };
                    assert_eq!(step.layer_progress.first_layer_id, 0);
                    assert_eq!(step.layer_progress.next_layer_id, 1);
                    assert!(step.object_service.ready);
                    assert_eq!(
                        step.object_service.publish_count,
                        decode_object_count + range_handoff_object_op_count
                    );
                    assert_eq!(
                        step.object_service.resolve_count,
                        decode_object_count
                            + QWEN3_DENSE_0_6B_PROFILE.tp_nodes
                            + runtime_weight_resolve_count
                            + range_handoff_object_op_count
                            + u64::from(!step.runtime_prefill_executed)
                    );
                    assert_eq!(
                        step.object_service.append_count,
                        u64::from(!step.runtime_prefill_executed)
                    );
                    assert_eq!(
                        step.object_service.kv_index_resolve_count,
                        u64::from(!step.runtime_prefill_executed)
                    );
                    assert_eq!(
                        step.object_service.kv_index_append_count,
                        u64::from(!step.runtime_prefill_executed)
                    );
                    assert_eq!(
                        step.object_service.metadata_put_count,
                        decode_object_count + range_handoff_object_op_count
                    );
                    assert_eq!(
                        step.object_service.metadata_get_count,
                        decode_object_count
                            + QWEN3_DENSE_0_6B_PROFILE.tp_nodes
                            + runtime_weight_resolve_count
                            + range_handoff_object_op_count
                            + u64::from(!step.runtime_prefill_executed)
                    );
                    assert_eq!(step.object_service.token_objects, 2);
                    assert_eq!(step.object_service.kv_objects, 1);
                    assert_eq!(
                        step.object_service.weight_objects,
                        QWEN3_DENSE_0_6B_PROFILE.tp_nodes
                    );
                    assert_ne!(step.object_service.weight_payload_bytes, 0);
                    if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok() {
                        assert_eq!(step.object_service.global_weight_object_count, 1);
                        assert_ne!(step.object_service.global_weight_payload_bytes, 0);
                        assert_eq!(step.object_service.global_weight_tensor_count, 3);
                        assert_ne!(step.object_service.global_weight_payload_checksum, 0);
                        assert_eq!(
                            step.object_service.weight_payload_slice_count,
                            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                                * QWEN3_DENSE_0_6B_PROFILE.tp_nodes
                                * qwen3_dense_0_6b_required_layer_weight_kind_codes().len() as u64
                        );
                        assert!(step.object_service.weight_payload_complete);
                        assert_eq!(
                            step.object_service.weight_reconstructed_tensor_count,
                            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                                * qwen3_dense_0_6b_required_layer_weight_kind_codes().len() as u64
                        );
                        assert_ne!(step.object_service.weight_reconstructed_tensor_checksum, 0);
                    }
                    assert_ne!(step.object_service.weight_payload_checksum, 0);
                    assert_eq!(
                        step.object_service.runtime_tensor_objects,
                        1 + QWEN3_DENSE_0_6B_PROFILE.tp_nodes * 2
                    );
                    assert_eq!(step.object_service.logits_objects, 1);
                    assert_eq!(
                        step.object_service.committed_object_count,
                        decode_object_count + range_handoff_object_op_count
                    );
                    assert_eq!(step.object_service.missing_resolve_count, 0);
                    assert_ne!(step.object_service.object_checksum, 0);
                    assert_eq!(
                        step.hidden_layer_pipeline.layer_count,
                        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                    );
                    assert_eq!(
                        step.hidden_layer_pipeline.node_count,
                        QWEN3_DENSE_0_6B_PROFILE.tp_nodes
                    );
                    assert_eq!(
                        step.hidden_layer_pipeline.hidden_tensor_byte_count,
                        (128 * 128 * std::mem::size_of::<u16>()) as u64
                    );
                    if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok()
                        && step.text_output.guest_input.prompt_token_count != 0
                    {
                        assert!(step.hidden_layer_pipeline.input_embedding_real_backed);
                        assert_eq!(
                            step.hidden_layer_pipeline.input_embedding_token_count,
                            step.text_output.guest_input.prompt_token_count
                        );
                        assert_ne!(step.hidden_layer_pipeline.input_embedding_row_byte_count, 0);
                        assert_ne!(step.hidden_layer_pipeline.input_embedding_row_checksum, 0);
                        assert_ne!(step.hidden_layer_pipeline.input_embedding_value_checksum, 0);
                        assert_ne!(step.hidden_layer_pipeline.input_embedding_checksum, 0);
                    } else {
                        assert!(!step.hidden_layer_pipeline.input_embedding_real_backed);
                    }
                    assert_eq!(
                        step.hidden_layer_pipeline.hidden_tensor_carry_count,
                        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                    );
                    assert!(step.hidden_layer_pipeline.hidden_tensor_carry_all_present);
                    assert_ne!(step.hidden_layer_pipeline.hidden_tensor_carry_checksum, 0);
                    let has_real_layer_references = step.hidden_layer_pipeline.real_qkv_layer_count
                        == QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                        && step.hidden_layer_pipeline.real_mlp_layer_count
                            == QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers;
                    if has_real_layer_references {
                        assert_eq!(
                            step.hidden_layer_pipeline
                                .hidden_tensor_real_reference_count,
                            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                        );
                        assert!(
                            step.hidden_layer_pipeline
                                .hidden_tensor_real_references_all_present
                        );
                        assert_ne!(
                            step.hidden_layer_pipeline
                                .hidden_tensor_real_reference_checksum,
                            0
                        );
                        assert_eq!(
                            step.hidden_layer_pipeline.real_qkv_layer_count,
                            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                        );
                        assert!(step.hidden_layer_pipeline.real_qkv_all_layers_present);
                        assert_ne!(step.hidden_layer_pipeline.real_qkv_layer_checksum, 0);
                        assert_eq!(
                            step.hidden_layer_pipeline.real_mlp_layer_count,
                            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                        );
                        assert!(step.hidden_layer_pipeline.real_mlp_all_layers_present);
                        assert_ne!(step.hidden_layer_pipeline.real_mlp_layer_checksum, 0);
                        assert_eq!(
                            step.hidden_layer_pipeline.real_layer_execution_count,
                            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                        );
                        assert!(step.hidden_layer_pipeline.real_layer_executions_all_present);
                        assert_ne!(step.hidden_layer_pipeline.real_layer_execution_checksum, 0);
                    } else {
                        assert_eq!(step.hidden_layer_pipeline.real_qkv_layer_count, 0);
                        assert!(!step.hidden_layer_pipeline.real_qkv_all_layers_present);
                        assert_eq!(step.hidden_layer_pipeline.real_mlp_layer_count, 0);
                        assert!(!step.hidden_layer_pipeline.real_mlp_all_layers_present);
                        assert_eq!(
                            step.hidden_layer_pipeline
                                .hidden_tensor_real_reference_count,
                            0
                        );
                        assert!(
                            !step
                                .hidden_layer_pipeline
                                .hidden_tensor_real_references_all_present
                        );
                        assert_eq!(step.hidden_layer_pipeline.real_layer_execution_count, 0);
                        assert!(!step.hidden_layer_pipeline.real_layer_executions_all_present);
                    }
                    assert_eq!(
                        step.hidden_layer_pipeline.transition_count,
                        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers - 1
                    );
                    assert_eq!(
                        step.hidden_layer_pipeline.boundary_count,
                        QWEN3_DENSE_0_6B_PROFILE.tp_nodes - 1
                    );
                    assert_eq!(
                        step.hidden_layer_pipeline.local_transition_count,
                        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                            - QWEN3_DENSE_0_6B_PROFILE.tp_nodes
                    );
                    assert_eq!(step.hidden_layer_pipeline.min_layers_per_node, 3);
                    assert_eq!(step.hidden_layer_pipeline.max_layers_per_node, 4);
                    assert!(step.hidden_layer_pipeline.balanced_layer_spread);
                    assert_eq!(step.hidden_layer_pipeline.node_ranges.len(), 8);
                    assert_eq!(step.hidden_layer_pipeline.layer_executions.len(), 28);
                    assert_eq!(
                        step.hidden_layer_pipeline
                            .node_ranges
                            .iter()
                            .map(|range| range.layer_count)
                            .sum::<u64>(),
                        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                    );
                    assert_eq!(
                        step.hidden_layer_pipeline
                            .node_ranges
                            .iter()
                            .map(|range| {
                                (
                                    range.node_id,
                                    range.first_layer_id,
                                    range.last_layer_id,
                                    range.layer_count,
                                )
                            })
                            .collect::<Vec<_>>(),
                        vec![
                            (0, 0, 3, 4),
                            (1, 4, 6, 3),
                            (2, 7, 10, 4),
                            (3, 11, 13, 3),
                            (4, 14, 17, 4),
                            (5, 18, 20, 3),
                            (6, 21, 24, 4),
                            (7, 25, 27, 3),
                        ]
                    );
                    for (layer_index, execution) in step
                        .hidden_layer_pipeline
                        .layer_executions
                        .iter()
                        .enumerate()
                    {
                        assert_eq!(execution.layer_id, layer_index as u64);
                        assert_eq!(
                            execution.owner_node,
                            qwen3_dense_0_6b_hidden_layer_owner_node(execution.layer_id)
                        );
                        assert_ne!(execution.input_tensor_checksum, 0);
                        assert_ne!(execution.output_tensor_checksum, 0);
                        assert_ne!(
                            execution.input_tensor_checksum,
                            execution.output_tensor_checksum
                        );
                        if has_real_layer_references {
                            assert_ne!(execution.real_reference_tensor_checksum, 0);
                        } else {
                            assert_eq!(execution.real_reference_tensor_checksum, 0);
                        }
                        assert_ne!(execution.output_checksum, 0);
                        if layer_index == 0 {
                            assert!(execution.starts_node_range);
                        } else {
                            assert_eq!(
                                execution.input_checksum,
                                step.hidden_layer_pipeline.layer_executions[layer_index - 1]
                                    .output_checksum
                            );
                            assert_eq!(
                                execution.input_tensor_checksum,
                                step.hidden_layer_pipeline.layer_executions[layer_index - 1]
                                    .output_tensor_checksum
                            );
                        }
                    }
                    assert_eq!(
                        step.hidden_layer_pipeline
                            .layer_executions
                            .iter()
                            .filter(|execution| execution.starts_node_range)
                            .count() as u64,
                        QWEN3_DENSE_0_6B_PROFILE.tp_nodes
                    );
                    assert_ne!(step.hidden_layer_pipeline.node_range_checksum, 0);
                    assert_eq!(step.hidden_layer_pipeline.first_layer_id, 0);
                    assert_eq!(
                        step.hidden_layer_pipeline.last_layer_id,
                        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers - 1
                    );
                    assert_eq!(step.hidden_layer_pipeline.first_node_id, 0);
                    assert_eq!(
                        step.hidden_layer_pipeline.last_node_id,
                        QWEN3_DENSE_0_6B_PROFILE.tp_nodes - 1
                    );
                    assert_ne!(step.hidden_layer_pipeline.layer_assignment_checksum, 0);
                    assert_ne!(step.hidden_layer_pipeline.boundary_checksum, 0);
                    assert_ne!(step.hidden_layer_pipeline.final_layer_checksum, 0);
                    assert_ne!(step.hidden_layer_pipeline.aggregate_checksum, 0);
                    if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok() {
                        assert_eq!(step.layer_progress.qkv_reference_layer_count, 2);
                        assert_eq!(step.layer_progress.qkv_stage_link_count, 128);
                        assert!(step.layer_progress.full_layer_path_real_backed);
                    } else {
                        assert!(!step.layer_progress.full_layer_path_real_backed);
                    }
                    assert_eq!(
                        step.layer_progress.full_layer_path_count,
                        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                    );
                    assert_eq!(
                        step.layer_progress.full_layer_final_checksum,
                        step.hidden_layer_pipeline.final_layer_checksum
                    );
                    assert_ne!(step.layer_progress.full_layer_path_checksum, 0);
                    assert_ne!(step.layer_progress.layer0_path_checksum, 0);
                    assert_ne!(step.layer_progress.layer1_path_checksum, 0);
                    assert_ne!(step.layer_progress.logits_path_checksum, 0);
                    assert_ne!(step.layer_progress.aggregate_checksum, 0);
                    assert_ne!(
                        step.layer_progress.layer0_path_checksum,
                        step.layer_progress.layer1_path_checksum
                    );
                }
                assert_ne!(
                    report.steps[0].layer_progress.logits_path_checksum,
                    report.steps[1].layer_progress.logits_path_checksum
                );
                assert!(!report.generated_text_lossy.is_empty());
            },
        );
    }

    #[test]
    fn qwen3_dense_0_6b_decode_loop_prompt_input_clears_synthetic_guest_stage() {
        run_simpler_native_test_isolated(
            "qwen3_dense_0_6b_decode_loop_prompt_input_clears_synthetic_guest_stage",
            || {
                if qwen3_dense_0_6b_real_tokenizer_path().is_none() {
                    return;
                }
                let topology = test_topology();
                let report =
                    qwen3_dense_0_6b_decode_loop_report_with_prompt(&topology, 2, "Hello Qwen3")
                        .expect("prompt-backed decode loop report");
                assert_eq!(report.steps.len(), 2);
                for step in &report.steps {
                    assert!(step.text_output.guest_input.real_backed);
                    assert_ne!(step.text_output.guest_input.prompt_token_count, 0);
                    assert_ne!(step.text_output.guest_input.prompt_token_checksum, 0);
                    assert!(step.text_output.synthetic.guest_input_real_backed);
                    if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok() {
                        assert!(step.hidden_layer_pipeline.input_embedding_real_backed);
                        assert_eq!(
                            step.hidden_layer_pipeline.input_embedding_token_count,
                            step.text_output.guest_input.prompt_token_count
                        );
                        assert_ne!(step.hidden_layer_pipeline.input_embedding_row_byte_count, 0);
                        assert_ne!(step.hidden_layer_pipeline.input_embedding_row_checksum, 0);
                        assert_ne!(step.hidden_layer_pipeline.input_embedding_value_checksum, 0);
                        assert_ne!(step.hidden_layer_pipeline.input_embedding_checksum, 0);
                        assert_eq!(step.real_inference_contract.synthetic_stage_count, 0);
                        assert_eq!(step.real_inference_contract.synthetic_stage_mask, 0);
                        assert!(!step.real_inference_contract.uses_candidate_logits_only);
                        assert!(!step.real_inference_contract.uses_deterministic_hidden);
                        assert!(
                            !step
                                .real_inference_contract
                                .uses_embedding_hidden_as_final_hidden
                        );
                        assert!(
                            step.real_inference_contract
                                .uses_round1_output_hidden_for_logits
                        );
                        let real_inference = step
                            .text_output
                            .real_inference
                            .as_ref()
                            .expect("real inference reference report");
                        assert_eq!(
                            real_inference.layer_count,
                            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                        );
                        assert_eq!(
                            real_inference.full_vocab_checked_token_count,
                            QWEN3_DENSE_0_6B_PROFILE.vocab_size
                        );
                        assert_ne!(real_inference.forward_checksum, 0);
                        assert_ne!(real_inference.full_vocab_logits_checksum, 0);
                        assert_ne!(real_inference.sampled_text_byte_checksum, 0);
                        assert!(step.text_output.samples.iter().all(|sample| {
                            sample.runtime_forward_layer_count
                                == QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                                && sample.runtime_forward_final_hidden_checksum != 0
                                && sample.runtime_forward_checksum != 0
                        }));
                        assert!(step.real_inference_contract.full_forward_math);
                        assert!(step.real_inference_contract.full_vocab_logits);
                        assert!(step.real_inference_contract.sampled_text_reference_checked);
                        assert!(step.real_inference_contract.ready);
                        assert_ne!(step.real_inference_contract.aggregate_checksum, 0);
                        assert!(
                            !step.real_inference_contract.blockers.iter().any(|blocker| {
                                blocker == "logits_are_candidate_subset_not_full_vocab"
                            })
                        );
                        assert!(!step
                            .real_inference_contract
                            .blockers
                            .iter()
                            .any(|blocker| blocker.starts_with("layer_forward_math_incomplete")));
                        assert!(!step
                            .real_inference_contract
                            .blockers
                            .iter()
                            .any(|blocker| blocker == "full_vocab_logits_not_computed"));
                        assert!(!step
                            .real_inference_contract
                            .blockers
                            .iter()
                            .any(|blocker| blocker == "sampled_token_text_reference_not_checked"));
                        assert_eq!(
                            step.real_inference_contract.blocker_count as usize,
                            step.real_inference_contract.blockers.len()
                        );
                    }
                    assert_eq!(
                        step.text_output.synthetic.stage_mask & QWEN3_SYNTHETIC_GUEST_INPUT,
                        0
                    );
                    assert_eq!(step.text_output.synthetic.stage_count, 0);
                }
            },
        );
    }

    #[test]
    fn qwen3_attention_score_and_softmax_depend_on_real_qk_sample_words() {
        let shard = Qwen3Dense06bShard {
            shard_id: 0,
            owner_node: 0,
            target_node: 0,
            head_start: 0,
            head_end: 2,
            kv_block_start: 0,
            kv_block_end: 1,
        };
        let summary = Qwen3Dense06bQkvReferenceLayerSummary {
            layer_id: 0,
            shard_count: 1,
            total_weight_bytes: 20,
            total_q_rows: 2,
            total_k_rows: 1,
            total_v_rows: 1,
            aggregate_checksum: 0xabc,
            shards: vec![test_weight_reference_shard(0, 0x10)],
        };
        let mut changed_summary = summary.clone();
        changed_summary.shards[0].q_output_sample_words[2] ^= 0x55aa_33cc;

        let mut rope_q = Vec::new();
        let mut rope_kv = Vec::new();
        for idx in 0..16u16 {
            rope_q.extend_from_slice(&(0x3c00 + (idx & 0x0f)).to_le_bytes());
            rope_kv.extend_from_slice(&(0x3800 + ((idx * 3) & 0x0f)).to_le_bytes());
        }

        let baseline = qwen3_dense_0_6b_attention_score_tile_from_rope(&rope_q, &rope_kv, 4);
        let real_qk_score = qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_reference(
            &rope_q,
            &rope_kv,
            4,
            Some((shard, &summary)),
        );
        let changed_real_qk_score =
            qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_reference(
                &rope_q,
                &rope_kv,
                4,
                Some((shard, &changed_summary)),
            );
        let real_qk_softmax = qwen3_dense_0_6b_softmax_tile_from_attention_score(&real_qk_score, 4);
        let changed_real_qk_softmax =
            qwen3_dense_0_6b_softmax_tile_from_attention_score(&changed_real_qk_score, 4);

        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&real_qk_score),
            qwen3_dense_0_6b_shard_output_checksum(&baseline),
            "attention score must carry the real Q/K sample-word reference"
        );
        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&changed_real_qk_score),
            qwen3_dense_0_6b_shard_output_checksum(&real_qk_score),
            "changing real Q/K sample words must change attention score"
        );
        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&changed_real_qk_softmax),
            qwen3_dense_0_6b_shard_output_checksum(&real_qk_softmax),
            "softmax must consume the real Q/K-sensitive attention score"
        );
    }

    #[test]
    fn qwen3_attention_context_depends_on_real_v_sample_words() {
        let shard = Qwen3Dense06bShard {
            shard_id: 0,
            owner_node: 0,
            target_node: 0,
            head_start: 0,
            head_end: 2,
            kv_block_start: 0,
            kv_block_end: 1,
        };
        let summary = Qwen3Dense06bQkvReferenceLayerSummary {
            layer_id: 0,
            shard_count: 1,
            total_weight_bytes: 20,
            total_q_rows: 2,
            total_k_rows: 1,
            total_v_rows: 1,
            aggregate_checksum: 0xabc,
            shards: vec![test_weight_reference_shard(0, 0x10)],
        };
        let mut changed_summary = summary.clone();
        changed_summary.shards[0].v_output_sample_words[1] ^= 0x77bb_11dd;

        let softmax = f32s_to_bytes(&[
            0.65, 0.20, 0.10, 0.05, 0.10, 0.70, 0.15, 0.05, 0.05, 0.10, 0.75, 0.10, 0.25, 0.25,
            0.25, 0.25,
        ]);
        let mut v_projection = Vec::new();
        for idx in 0..16u16 {
            v_projection.extend_from_slice(&(0x3c00 + ((idx * 5) & 0x1f)).to_le_bytes());
        }

        let baseline =
            qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v(&softmax, &v_projection, 4);
        let real_v_context =
            qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_reference(
                &softmax,
                &v_projection,
                4,
                Some((shard, &summary)),
            );
        let changed_real_v_context =
            qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_reference(
                &softmax,
                &v_projection,
                4,
                Some((shard, &changed_summary)),
            );

        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&real_v_context),
            qwen3_dense_0_6b_shard_output_checksum(&baseline),
            "attention context must carry the real V sample-word reference"
        );
        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&changed_real_v_context),
            qwen3_dense_0_6b_shard_output_checksum(&real_v_context),
            "changing real V sample words must change attention context"
        );
    }

    #[test]
    fn qwen3_mlp_tiles_depend_on_real_gate_up_down_sample_words() {
        let shard = Qwen3Dense06bShard {
            shard_id: 0,
            owner_node: 0,
            target_node: 0,
            head_start: 0,
            head_end: 2,
            kv_block_start: 0,
            kv_block_end: 1,
        };
        let summary = Qwen3Dense06bMlpReferenceLayerSummary {
            layer_id: 0,
            shard_count: 1,
            total_weight_bytes: 24,
            total_intermediate_rows: 3,
            aggregate_checksum: 0xdef,
            shards: vec![test_mlp_reference_shard(0, 0x50)],
        };
        let mut changed_summary = summary.clone();
        changed_summary.shards[0].activation_sample_words[2] ^= 0x44cc_22aa;
        changed_summary.shards[0].down_output_sample_words[1] ^= 0x66dd_33bb;
        let mut next_layer_summary = summary.clone();
        next_layer_summary.layer_id = 1;
        next_layer_summary.aggregate_checksum ^= 0x1234_5678_9abc_def0;

        let mut half_tile = Vec::new();
        for idx in 0..16u16 {
            half_tile.extend_from_slice(&(0x3c00 + ((idx * 7) & 0x1f)).to_le_bytes());
        }
        let float_tile = f32s_to_bytes(&[
            1.00, 1.01, 1.02, 1.03, 1.04, 1.05, 1.06, 1.07, 1.08, 1.09, 1.10, 1.11, 1.12, 1.13,
            1.14, 1.15,
        ]);

        let fallback_activation =
            qwen3_dense_0_6b_mlp_activation_tile_from_attention_context(&half_tile, 4, shard);
        let real_activation = qwen3_dense_0_6b_real_mlp_activation_tile_from_attention_context(
            &half_tile,
            4,
            shard,
            Some(&summary),
        );
        let missing_reference_activation =
            qwen3_dense_0_6b_real_mlp_activation_tile_from_attention_context(
                &half_tile, 4, shard, None,
            );
        let changed_activation = qwen3_dense_0_6b_real_mlp_activation_tile_from_attention_context(
            &half_tile,
            4,
            shard,
            Some(&changed_summary),
        );
        let real_output = qwen3_dense_0_6b_tile_with_real_mlp_reference_mix(
            &float_tile,
            4,
            12,
            shard,
            Some(&summary),
        );
        let next_layer_output = qwen3_dense_0_6b_tile_with_real_mlp_reference_mix(
            &float_tile,
            4,
            12,
            shard,
            Some(&next_layer_summary),
        );
        let changed_output = qwen3_dense_0_6b_tile_with_real_mlp_reference_mix(
            &float_tile,
            4,
            12,
            shard,
            Some(&changed_summary),
        );

        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&real_activation),
            qwen3_dense_0_6b_shard_output_checksum(&fallback_activation),
            "real MLP activation must replace the synthetic gate/up activation path"
        );
        assert!(
            (0..16).all(|index| qwen3_dense_0_6b_half_at(&real_activation, index) > 1.0),
            "real MLP activation must stay valid for the downstream sqrt(log(A)) host kernel"
        );
        assert_eq!(
            qwen3_dense_0_6b_shard_output_checksum(&missing_reference_activation),
            qwen3_dense_0_6b_shard_output_checksum(&fallback_activation),
            "missing MLP reference must keep the explicit synthetic fallback"
        );
        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&changed_activation),
            qwen3_dense_0_6b_shard_output_checksum(&real_activation),
            "changing real MLP activation sample words must change activation tile"
        );
        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&real_output),
            qwen3_dense_0_6b_shard_output_checksum(&float_tile),
            "MLP output must carry the real down-projection sample-word reference"
        );
        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&changed_output),
            qwen3_dense_0_6b_shard_output_checksum(&real_output),
            "changing real down sample words must change output tile"
        );
        assert_ne!(
            qwen3_dense_0_6b_shard_output_checksum(&next_layer_output),
            qwen3_dense_0_6b_shard_output_checksum(&real_output),
            "changing the MLP layer summary must change the mixed output tile"
        );
    }

    fn assert_qwen3_dense_0_6b_prefill_profile_output(
        output: &[u8],
        guest_input: &[u8; W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES],
    ) {
        const TILES_PER_SHARD: usize = 2;
        const TILE_COUNT: usize = 16;
        const TILE_ELEMS: usize = 128 * 128;
        const TILE_BYTES: usize = 65_536;
        const KV_BLOCKS_PER_TILE: usize = 2;
        let real_weight_reference_summary =
            qwen3_dense_0_6b_real_weight_reference_summary(&test_topology())
                .expect("real weight reference summary");
        let real_qkv_reference_values =
            qwen3_dense_0_6b_real_qkv_reference_values(&test_topology())
                .expect("real QKV reference values");
        let real_mlp_reference_summary =
            qwen3_dense_0_6b_real_mlp_reference_summary(&test_topology())
                .expect("real MLP reference summary");
        let expected_rmsnorm_tile = |prefill_hidden: &[u8], shard: Qwen3Dense06bShard| -> Vec<u8> {
            let rmsnorm_hidden =
                qwen3_dense_0_6b_rmsnorm_tile_from_prefill_hidden(prefill_hidden, 128, shard);
            qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
                &rmsnorm_hidden,
                128,
                1,
                shard,
                real_qkv_reference_values.as_ref(),
                real_weight_reference_summary.as_ref(),
            )
        };
        let expected_projection_tile = |half_input: &[u8],
                                        projection_kind: Qwen3ProjectionKind,
                                        stage_kind: u64,
                                        shard: Qwen3Dense06bShard|
         -> Vec<u8> {
            let projection = qwen3_dense_0_6b_projection_tile_from_half_input(
                half_input,
                128,
                projection_kind,
                shard,
            );
            qwen3_dense_0_6b_tile_with_real_qkv_reference_values_mix(
                &projection,
                128,
                stage_kind,
                shard,
                real_qkv_reference_values.as_ref(),
                real_weight_reference_summary.as_ref(),
            )
        };
        let expected_attention_score_tile = |rope_q: &[u8],
                                             rope_kv: &[u8],
                                             kv_projection: &[u8],
                                             v_projection: &[u8],
                                             shard: Qwen3Dense06bShard|
         -> Vec<u8> {
            let tile_id = shard.kv_block_start / 2;
            let kvcache_payload = qwen3_dense_0_6b_kvcache_tile_payload_from_projection(
                0,
                tile_id,
                0,
                tile_id * 2 + 2,
                kv_projection,
                v_projection,
                128,
            );
            qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_and_kvcache_reference(
                rope_q,
                rope_kv,
                128,
                real_weight_reference_summary
                    .as_ref()
                    .map(|summary| (shard, summary)),
                Some(&kvcache_payload),
            )
        };
        let expected_attention_context_tile = |attention_softmax: &[u8],
                                               kv_projection: &[u8],
                                               v_projection: &[u8],
                                               shard: Qwen3Dense06bShard|
         -> Vec<u8> {
            let tile_id = shard.kv_block_start / 2;
            let kvcache_payload = qwen3_dense_0_6b_kvcache_tile_payload_from_projection(
                0,
                tile_id,
                0,
                tile_id * 2 + 2,
                kv_projection,
                v_projection,
                128,
            );
            qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_and_kvcache_reference(
                attention_softmax,
                v_projection,
                128,
                real_weight_reference_summary
                    .as_ref()
                    .map(|summary| (shard, summary)),
                Some(&kvcache_payload),
            )
        };
        let expected_attention_score_without_cache =
            |rope_q: &[u8], rope_kv: &[u8], shard: Qwen3Dense06bShard| -> Vec<u8> {
                qwen3_dense_0_6b_attention_score_tile_from_rope_with_real_qk_reference(
                    rope_q,
                    rope_kv,
                    128,
                    real_weight_reference_summary
                        .as_ref()
                        .map(|summary| (shard, summary)),
                )
            };
        let expected_attention_context_without_cache =
            |attention_softmax: &[u8], v_projection: &[u8], shard: Qwen3Dense06bShard| -> Vec<u8> {
                qwen3_dense_0_6b_attention_context_tile_from_softmax_and_v_with_real_v_reference(
                    attention_softmax,
                    v_projection,
                    128,
                    real_weight_reference_summary
                        .as_ref()
                        .map(|summary| (shard, summary)),
                )
            };
        let expected_mlp_activation_tile =
            |attention_context: &[u8], shard: Qwen3Dense06bShard| -> Vec<u8> {
                qwen3_dense_0_6b_real_mlp_activation_tile_from_attention_context(
                    attention_context,
                    128,
                    shard,
                    real_mlp_reference_summary.as_ref(),
                )
            };

        assert!(output.len() < TILE_COUNT * TILE_ELEMS * std::mem::size_of::<f32>());
        assert!(output.len() >= 64);
        assert_eq!(
            u64::from_le_bytes(output[8..16].try_into().expect("publish marker")),
            0x7133773470756230
        );
        assert_eq!(
            u64::from_le_bytes(output[16..24].try_into().expect("resolve marker")),
            0x7133773472657331
        );
        assert_eq!(
            u64::from_le_bytes(output[24..32].try_into().expect("compute marker")),
            0x71337734636d7031
        );
        assert_eq!(
            u64::from_le_bytes(output[32..40].try_into().expect("publish count")),
            TILE_COUNT as u64
        );
        assert_eq!(
            u64::from_le_bytes(output[40..48].try_into().expect("resolve count")),
            TILE_COUNT as u64
        );
        assert_eq!(
            u64::from_le_bytes(output[48..56].try_into().expect("compute count")),
            TILE_COUNT as u64
        );
        assert_eq!(
            u64::from_le_bytes(output[320..328].try_into().expect("result table marker")),
            0x7133773474626c30
        );
        assert_eq!(
            u64::from_le_bytes(output[328..336].try_into().expect("result table count")),
            TILE_COUNT as u64
        );
        assert_eq!(
            u64::from_le_bytes(
                output[336..344]
                    .try_into()
                    .expect("result table entry words")
            ),
            10
        );
        assert_eq!(
            u64::from_le_bytes(output[344..352].try_into().expect("result table bytes")),
            1280
        );
        for tile in 0..TILE_COUNT {
            let shard = tile / TILES_PER_SHARD;
            let base = 384 + tile * 80;
            assert_eq!(
                u64::from_le_bytes(output[base..base + 8].try_into().expect("descriptor shard")),
                shard as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 24..base + 32]
                        .try_into()
                        .expect("descriptor tile")
                ),
                tile as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 32..base + 40]
                        .try_into()
                        .expect("descriptor kv start")
                ),
                (tile * KV_BLOCKS_PER_TILE) as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 40..base + 48]
                        .try_into()
                        .expect("descriptor kv end")
                ),
                (tile * KV_BLOCKS_PER_TILE + KV_BLOCKS_PER_TILE) as u64
            );
            assert_ne!(
                u64::from_le_bytes(
                    output[base + 48..base + 56]
                        .try_into()
                        .expect("descriptor round0 segment")
                ),
                0
            );
            assert_ne!(
                u64::from_le_bytes(
                    output[base + 56..base + 64]
                        .try_into()
                        .expect("descriptor round1 segment")
                ),
                0
            );
            assert_ne!(
                u64::from_le_bytes(
                    output[base + 64..base + 72]
                        .try_into()
                        .expect("descriptor round0 checksum")
                ),
                0
            );
            assert_ne!(
                u64::from_le_bytes(
                    output[base + 72..base + 80]
                        .try_into()
                        .expect("descriptor round1 checksum")
                ),
                0
            );
        }
        const RESULT_TABLE_BASE: usize = 384;
        const RESULT_BLOCK_TABLE_HEADER: usize = 39_424;
        const RESULT_BLOCK_TABLE_BASE: usize = 39_488;
        const RESULT_BLOCK_TABLE_BYTES: usize = 4_096;
        const KVCACHE_TABLE_HEADER: usize = 43_584;
        const KVCACHE_TABLE_BASE: usize = 43_648;
        const KVCACHE_TABLE_ENTRY_BYTES: usize = 112;
        const KVCACHE_LAYERS: usize = QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as usize;
        const KVCACHE_PHASES: usize = 2;
        const KVCACHE_BLOCKS_PER_LAYER_TILE: usize = KV_BLOCKS_PER_TILE + 1;
        const KVCACHE_ENTRY_COUNT: usize = TILE_COUNT * KVCACHE_LAYERS * KVCACHE_PHASES;
        const KVCACHE_TABLE_BYTES: usize = KVCACHE_ENTRY_COUNT * KVCACHE_TABLE_ENTRY_BYTES;
        const KVCACHE_TABLE_END: usize = KVCACHE_TABLE_BASE + KVCACHE_TABLE_BYTES;
        const KVCACHE_STATE_TABLE_HEADER: usize = KVCACHE_TABLE_END;
        const KVCACHE_STATE_TABLE_BASE: usize = KVCACHE_STATE_TABLE_HEADER + 64;
        const KVCACHE_STATE_TABLE_ENTRY_BYTES: usize = 64;
        const KVCACHE_STATE_ENTRY_COUNT: usize =
            TILE_COUNT * KVCACHE_LAYERS * KVCACHE_BLOCKS_PER_LAYER_TILE;
        const KVCACHE_STATE_TABLE_BYTES: usize =
            KVCACHE_STATE_ENTRY_COUNT * KVCACHE_STATE_TABLE_ENTRY_BYTES;
        const KVCACHE_STATE_TABLE_END: usize = KVCACHE_STATE_TABLE_BASE + KVCACHE_STATE_TABLE_BYTES;
        const LOGITS_TABLE_HEADER: usize = KVCACHE_STATE_TABLE_END;
        const LOGITS_TABLE_BASE: usize = LOGITS_TABLE_HEADER + 64;
        const LOGITS_TABLE_ENTRY_BYTES: usize = 360;
        const LOGITS_ENTRY_COUNT: usize = TILE_COUNT;
        const LOGITS_TABLE_BYTES: usize = LOGITS_ENTRY_COUNT * LOGITS_TABLE_ENTRY_BYTES;
        const LOGITS_TABLE_END: usize = LOGITS_TABLE_BASE + LOGITS_TABLE_BYTES;
        const TOKEN_TEXT_TABLE_HEADER: usize = LOGITS_TABLE_END;
        const TOKEN_TEXT_TABLE_BASE: usize = TOKEN_TEXT_TABLE_HEADER + 64;
        const TOKEN_TEXT_TABLE_ENTRY_BYTES: usize = 64;
        const TOKEN_TEXT_ENTRY_COUNT: usize = TILE_COUNT;
        const TOKEN_TEXT_TABLE_BYTES: usize = TOKEN_TEXT_ENTRY_COUNT * TOKEN_TEXT_TABLE_ENTRY_BYTES;
        const TOKEN_TEXT_TABLE_END: usize = TOKEN_TEXT_TABLE_BASE + TOKEN_TEXT_TABLE_BYTES;
        const TEXT_OUTPUT_TABLE_HEADER: usize = TOKEN_TEXT_TABLE_END;
        const TEXT_OUTPUT_TABLE_END: usize = TEXT_OUTPUT_TABLE_HEADER + 64;
        const TEXT_OUTPUT_BYTES_TABLE_HEADER: usize = TEXT_OUTPUT_TABLE_END;
        const TEXT_OUTPUT_BYTES_TABLE_BASE: usize = TEXT_OUTPUT_BYTES_TABLE_HEADER + 64;
        let text_output_bytes_table_bytes =
            (read_u64_le_at(output, TEXT_OUTPUT_BYTES_TABLE_HEADER + 8) as usize + 7) & !7;
        let text_output_bytes_table_end =
            TEXT_OUTPUT_BYTES_TABLE_BASE + text_output_bytes_table_bytes;
        const TOKENIZER_ASSET_TABLE_BYTES: usize = 64 + 5 * 4 * 8;
        const LOGITS_REFERENCE_ENTRY_COUNT: usize = LOGITS_ENTRY_COUNT * 4;
        let metadata_table_end = if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok() {
            text_output_bytes_table_end
                + TOKENIZER_ASSET_TABLE_BYTES
                + 64
                + QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize * 112
                + 64
                + TILE_COUNT * 8 * QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS as usize * 8
                + 64
                + QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize
                    * 2
                    * QWEN3_MLP_REFERENCE_ENTRY_WORDS as usize
                    * 8
                + 64
                + LOGITS_REFERENCE_ENTRY_COUNT * QWEN3_LOGITS_REFERENCE_ENTRY_WORDS as usize * 8
        } else {
            text_output_bytes_table_end
        };
        assert_eq!(
            u64::from_le_bytes(
                output[RESULT_BLOCK_TABLE_HEADER..RESULT_BLOCK_TABLE_HEADER + 8]
                    .try_into()
                    .expect("result block table marker")
            ),
            0x71337734626c6b30
        );
        assert_eq!(
            u64::from_le_bytes(
                output[RESULT_BLOCK_TABLE_HEADER + 8..RESULT_BLOCK_TABLE_HEADER + 16]
                    .try_into()
                    .expect("result block table count")
            ),
            32
        );
        assert_eq!(
            u64::from_le_bytes(
                output[RESULT_BLOCK_TABLE_HEADER + 16..RESULT_BLOCK_TABLE_HEADER + 24]
                    .try_into()
                    .expect("result block table entry words")
            ),
            16
        );
        assert_eq!(
            u64::from_le_bytes(
                output[RESULT_BLOCK_TABLE_HEADER + 24..RESULT_BLOCK_TABLE_HEADER + 32]
                    .try_into()
                    .expect("result block table bytes")
            ),
            RESULT_BLOCK_TABLE_BYTES as u64
        );
        assert_eq!(
            u64::from_le_bytes(
                output[RESULT_BLOCK_TABLE_HEADER + 32..RESULT_BLOCK_TABLE_HEADER + 40]
                    .try_into()
                    .expect("result block metadata end")
            ),
            metadata_table_end as u64
        );
        let mut result_block_checksum_nonzero = 0usize;
        let mut result_block_checksum_matches = 0usize;
        let mut result_block_checksum_first = 0u64;
        let mut result_block_checksum_last = 0u64;
        let mut result_block_element_pair_matches = 0usize;
        for block in 0..32usize {
            let base = RESULT_BLOCK_TABLE_BASE + block * 128;
            let tile = block / KV_BLOCKS_PER_TILE;
            let shard = tile / TILES_PER_SHARD;
            let block_in_tile = block % KV_BLOCKS_PER_TILE;
            let byte_start = tile * TILE_BYTES + block_in_tile * 32_768;
            let checksum = u64::from_le_bytes(
                output[base + 48..base + 56]
                    .try_into()
                    .expect("result block checksum"),
            );
            let expected_checksum = qwen3_dense_0_6b_canonical_block_checksum(
                output,
                byte_start,
                byte_start + 32_768,
                RESULT_BLOCK_TABLE_HEADER,
                metadata_table_end,
            );
            assert_eq!(
                u64::from_le_bytes(output[base..base + 8].try_into().expect("block shard")),
                shard as u64
            );
            assert_eq!(
                u64::from_le_bytes(output[base + 8..base + 16].try_into().expect("block kv")),
                block as u64
            );
            assert_eq!(
                u64::from_le_bytes(output[base + 16..base + 24].try_into().expect("block tile")),
                tile as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 24..base + 32]
                        .try_into()
                        .expect("block row start")
                ),
                (block_in_tile * 64) as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 32..base + 40]
                        .try_into()
                        .expect("block row end")
                ),
                (block_in_tile * 64 + 64) as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 40..base + 48]
                        .try_into()
                        .expect("block bytes")
                ),
                32_768
            );
            assert_eq!(
                checksum, expected_checksum,
                "result block {block} checksum must match canonical payload bytes"
            );
            assert_ne!(
                u64::from_le_bytes(
                    output[base + 56..base + 64]
                        .try_into()
                        .expect("block segment")
                ),
                0
            );
            for (index, sample_offset) in qwen3_dense_0_6b_result_block_sample_offsets()
                .iter()
                .enumerate()
            {
                let sample_source = byte_start + sample_offset;
                let expected_pair =
                    if (RESULT_BLOCK_TABLE_HEADER..metadata_table_end).contains(&sample_source) {
                        0
                    } else {
                        read_u64_le_at(output, sample_source)
                    };
                let offset = base + 64 + index * 8;
                let observed_pair = u64::from_le_bytes(
                    output[offset..offset + 8]
                        .try_into()
                        .expect("result block element sample pair"),
                );
                assert_eq!(
                    observed_pair, expected_pair,
                    "result block {block} sample pair {index} must match payload bytes"
                );
                result_block_element_pair_matches += 1;
            }
            if checksum != 0 {
                result_block_checksum_nonzero += 1;
            }
            if checksum == expected_checksum {
                result_block_checksum_matches += 1;
            }
            if block == 0 {
                result_block_checksum_first = checksum;
            }
            result_block_checksum_last = checksum;
        }
        assert_eq!(result_block_checksum_nonzero, 32);
        assert_eq!(result_block_checksum_matches, 32);
        assert_eq!(result_block_element_pair_matches, 256);
        assert_ne!(result_block_checksum_first, result_block_checksum_last);

        assert_eq!(
            u64::from_le_bytes(
                output[KVCACHE_TABLE_HEADER..KVCACHE_TABLE_HEADER + 8]
                    .try_into()
                    .expect("kvcache table marker")
            ),
            0x713377346b766330
        );
        assert_eq!(
            u64::from_le_bytes(
                output[KVCACHE_TABLE_HEADER + 8..KVCACHE_TABLE_HEADER + 16]
                    .try_into()
                    .expect("kvcache table count")
            ),
            KVCACHE_ENTRY_COUNT as u64
        );
        assert_eq!(
            u64::from_le_bytes(
                output[KVCACHE_TABLE_HEADER + 16..KVCACHE_TABLE_HEADER + 24]
                    .try_into()
                    .expect("kvcache table entry words")
            ),
            14
        );
        assert_eq!(
            u64::from_le_bytes(
                output[KVCACHE_TABLE_HEADER + 24..KVCACHE_TABLE_HEADER + 32]
                    .try_into()
                    .expect("kvcache table bytes")
            ),
            KVCACHE_TABLE_BYTES as u64
        );
        let mut kvcache_update_seq_sum = 0u64;
        let mut kvcache_read_window_last = 0u64;
        let mut kvcache_append_blocks = 0u64;
        let mut kvcache_prefill_entries = 0u64;
        let mut kvcache_decode_entries = 0u64;
        for entry in 0..KVCACHE_ENTRY_COUNT {
            let tile = entry / (KVCACHE_LAYERS * KVCACHE_PHASES);
            let phase_in_tile = entry % (KVCACHE_LAYERS * KVCACHE_PHASES);
            let layer = phase_in_tile / KVCACHE_PHASES;
            let phase = phase_in_tile % KVCACHE_PHASES;
            let layer_position_base = (layer * TILE_COUNT * KVCACHE_BLOCKS_PER_LAYER_TILE) as u64;
            let shard = tile / TILES_PER_SHARD;
            let base = KVCACHE_TABLE_BASE + entry * KVCACHE_TABLE_ENTRY_BYTES;
            assert_eq!(
                u64::from_le_bytes(output[base..base + 8].try_into().expect("kvcache layer")),
                layer as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 8..base + 16]
                        .try_into()
                        .expect("kvcache shard")
                ),
                shard as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 16..base + 24]
                        .try_into()
                        .expect("kvcache tile")
                ),
                tile as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 24..base + 32]
                        .try_into()
                        .expect("kvcache kv start")
                ),
                (tile * KV_BLOCKS_PER_TILE) as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 32..base + 40]
                        .try_into()
                        .expect("kvcache kv end")
                ),
                (tile * KV_BLOCKS_PER_TILE + KV_BLOCKS_PER_TILE) as u64
            );
            let append_start = u64::from_le_bytes(
                output[base + 40..base + 48]
                    .try_into()
                    .expect("kvcache append start"),
            );
            let append_end = u64::from_le_bytes(
                output[base + 48..base + 56]
                    .try_into()
                    .expect("kvcache append end"),
            );
            if phase == 0 {
                assert_eq!(
                    append_start,
                    layer_position_base + (tile * KV_BLOCKS_PER_TILE) as u64
                );
                assert_eq!(
                    append_end,
                    layer_position_base + (tile * KV_BLOCKS_PER_TILE + KV_BLOCKS_PER_TILE) as u64
                );
                kvcache_prefill_entries += 1;
            } else {
                assert_eq!(
                    append_start,
                    layer_position_base + (TILE_COUNT * KV_BLOCKS_PER_TILE + tile) as u64
                );
                assert_eq!(
                    append_end,
                    layer_position_base + (TILE_COUNT * KV_BLOCKS_PER_TILE + tile + 1) as u64
                );
                kvcache_decode_entries += 1;
            }
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 56..base + 64]
                        .try_into()
                        .expect("kvcache read start")
                ),
                layer_position_base
            );
            let read_end = u64::from_le_bytes(
                output[base + 64..base + 72]
                    .try_into()
                    .expect("kvcache read end"),
            );
            assert_eq!(read_end, append_end);
            kvcache_read_window_last = read_end;
            let update_seq = u64::from_le_bytes(
                output[base + 72..base + 80]
                    .try_into()
                    .expect("kvcache update seq"),
            );
            let expected_update_seq = entry as u64 + 1;
            assert_eq!(update_seq, expected_update_seq);
            kvcache_update_seq_sum += update_seq;
            kvcache_append_blocks += append_end - append_start;
            assert_ne!(
                u64::from_le_bytes(
                    output[base + 80..base + 88]
                        .try_into()
                        .expect("kvcache k segment")
                ),
                0
            );
            assert_ne!(
                u64::from_le_bytes(
                    output[base + 88..base + 96]
                        .try_into()
                        .expect("kvcache v segment")
                ),
                0
            );
            let k_checksum = u64::from_le_bytes(
                output[base + 96..base + 104]
                    .try_into()
                    .expect("kvcache k checksum"),
            );
            let v_checksum = u64::from_le_bytes(
                output[base + 104..base + 112]
                    .try_into()
                    .expect("kvcache v checksum"),
            );
            assert_ne!(k_checksum, 0);
            assert_ne!(v_checksum, 0);
            assert_ne!(k_checksum, v_checksum);
        }
        assert_eq!(
            kvcache_update_seq_sum,
            (KVCACHE_ENTRY_COUNT * (KVCACHE_ENTRY_COUNT + 1) / 2) as u64
        );
        assert_eq!(
            kvcache_read_window_last,
            (TILE_COUNT * KVCACHE_LAYERS * KVCACHE_BLOCKS_PER_LAYER_TILE) as u64
        );
        assert_eq!(
            kvcache_append_blocks,
            (TILE_COUNT * KVCACHE_LAYERS * KVCACHE_BLOCKS_PER_LAYER_TILE) as u64
        );
        assert_eq!(
            kvcache_prefill_entries,
            (TILE_COUNT * KVCACHE_LAYERS) as u64
        );
        assert_eq!(kvcache_decode_entries, (TILE_COUNT * KVCACHE_LAYERS) as u64);

        assert_eq!(
            u64::from_le_bytes(
                output[KVCACHE_STATE_TABLE_HEADER..KVCACHE_STATE_TABLE_HEADER + 8]
                    .try_into()
                    .expect("kvcache state table marker")
            ),
            0x713377346b767331
        );
        assert_eq!(
            u64::from_le_bytes(
                output[KVCACHE_STATE_TABLE_HEADER + 8..KVCACHE_STATE_TABLE_HEADER + 16]
                    .try_into()
                    .expect("kvcache state table count")
            ),
            KVCACHE_STATE_ENTRY_COUNT as u64
        );
        assert_eq!(
            u64::from_le_bytes(
                output[KVCACHE_STATE_TABLE_HEADER + 16..KVCACHE_STATE_TABLE_HEADER + 24]
                    .try_into()
                    .expect("kvcache state table entry words")
            ),
            8
        );
        assert_eq!(
            u64::from_le_bytes(
                output[KVCACHE_STATE_TABLE_HEADER + 24..KVCACHE_STATE_TABLE_HEADER + 32]
                    .try_into()
                    .expect("kvcache state table bytes")
            ),
            KVCACHE_STATE_TABLE_BYTES as u64
        );
        let mut state_seq_sum = 0u64;
        let mut state_position_sum = 0u64;
        let mut state_digest_nonzero = 0usize;
        let mut state_digest_first = 0u64;
        let mut state_digest_last = 0u64;
        let mut expected_state_seq_sum = 0u64;
        let mut state_read_digest_words_by_tile: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
        for state_entry in 0..KVCACHE_STATE_ENTRY_COUNT {
            let base = KVCACHE_STATE_TABLE_BASE + state_entry * KVCACHE_STATE_TABLE_ENTRY_BYTES;
            let layer = u64::from_le_bytes(
                output[base..base + 8]
                    .try_into()
                    .expect("kvcache state layer"),
            );
            let tile = u64::from_le_bytes(
                output[base + 8..base + 16]
                    .try_into()
                    .expect("kvcache state tile"),
            );
            let position = u64::from_le_bytes(
                output[base + 16..base + 24]
                    .try_into()
                    .expect("kvcache state position"),
            );
            let update_seq = u64::from_le_bytes(
                output[base + 24..base + 32]
                    .try_into()
                    .expect("kvcache state update seq"),
            );
            let k_checksum = u64::from_le_bytes(
                output[base + 32..base + 40]
                    .try_into()
                    .expect("kvcache state k checksum"),
            );
            let v_checksum = u64::from_le_bytes(
                output[base + 40..base + 48]
                    .try_into()
                    .expect("kvcache state v checksum"),
            );
            let read_window_end = u64::from_le_bytes(
                output[base + 48..base + 56]
                    .try_into()
                    .expect("kvcache state read end"),
            );
            let read_digest = u64::from_le_bytes(
                output[base + 56..base + 64]
                    .try_into()
                    .expect("kvcache state read digest"),
            );
            let (expected_layer, expected_tile, expected_position, expected_seq, expected_read_end) = {
                let expected_tile = state_entry / (KVCACHE_LAYERS * KVCACHE_BLOCKS_PER_LAYER_TILE);
                let block_in_tile = state_entry % (KVCACHE_LAYERS * KVCACHE_BLOCKS_PER_LAYER_TILE);
                let expected_layer = block_in_tile / KVCACHE_BLOCKS_PER_LAYER_TILE;
                let block_in_layer = block_in_tile % KVCACHE_BLOCKS_PER_LAYER_TILE;
                let layer_position_base =
                    expected_layer * TILE_COUNT * KVCACHE_BLOCKS_PER_LAYER_TILE;
                let update_seq_base = expected_tile * KVCACHE_LAYERS * KVCACHE_PHASES
                    + expected_layer * KVCACHE_PHASES;
                if block_in_layer < KV_BLOCKS_PER_TILE {
                    (
                        expected_layer,
                        expected_tile,
                        layer_position_base + expected_tile * KV_BLOCKS_PER_TILE + block_in_layer,
                        update_seq_base + 1,
                        layer_position_base
                            + expected_tile * KV_BLOCKS_PER_TILE
                            + KV_BLOCKS_PER_TILE,
                    )
                } else {
                    let decode_position =
                        layer_position_base + TILE_COUNT * KV_BLOCKS_PER_TILE + expected_tile;
                    (
                        expected_layer,
                        expected_tile,
                        decode_position,
                        update_seq_base + 2,
                        decode_position + 1,
                    )
                }
            };
            assert_eq!(layer, expected_layer as u64);
            assert_eq!(tile, expected_tile as u64);
            assert_eq!(position, expected_position as u64);
            assert_eq!(update_seq, expected_seq as u64);
            assert_eq!(read_window_end, expected_read_end as u64);
            assert_ne!(k_checksum, 0);
            assert_ne!(v_checksum, 0);
            assert_ne!(k_checksum, v_checksum);
            assert_ne!(read_digest, 0);
            if state_entry == 0 {
                state_digest_first = read_digest;
            }
            state_digest_last = read_digest;
            state_seq_sum += update_seq;
            expected_state_seq_sum += expected_seq as u64;
            state_position_sum += position;
            state_digest_nonzero += 1;
            state_read_digest_words_by_tile
                .entry(tile)
                .or_default()
                .push(read_digest);
        }
        let state_read_digest_by_tile: BTreeMap<u64, u64> = state_read_digest_words_by_tile
            .into_iter()
            .map(|(tile, words)| (tile, checksum_words(&words)))
            .collect();
        assert_eq!(state_seq_sum, expected_state_seq_sum);
        assert_eq!(
            state_position_sum,
            (KVCACHE_STATE_ENTRY_COUNT * (KVCACHE_STATE_ENTRY_COUNT - 1) / 2) as u64
        );
        assert_eq!(state_digest_nonzero, KVCACHE_STATE_ENTRY_COUNT);
        assert_ne!(state_digest_first, state_digest_last);

        assert_eq!(
            u64::from_le_bytes(
                output[LOGITS_TABLE_HEADER..LOGITS_TABLE_HEADER + 8]
                    .try_into()
                    .expect("logits table marker")
            ),
            0x713377346c6f6730
        );
        assert_eq!(
            u64::from_le_bytes(
                output[LOGITS_TABLE_HEADER + 8..LOGITS_TABLE_HEADER + 16]
                    .try_into()
                    .expect("logits table count")
            ),
            LOGITS_ENTRY_COUNT as u64
        );
        assert_eq!(
            u64::from_le_bytes(
                output[LOGITS_TABLE_HEADER + 16..LOGITS_TABLE_HEADER + 24]
                    .try_into()
                    .expect("logits table entry words")
            ),
            45
        );
        assert_eq!(
            u64::from_le_bytes(
                output[LOGITS_TABLE_HEADER + 24..LOGITS_TABLE_HEADER + 32]
                    .try_into()
                    .expect("logits table bytes")
            ),
            LOGITS_TABLE_BYTES as u64
        );
        let mut sampled_token_distinct = 0usize;
        let mut sampled_tokens = [0u64; TILE_COUNT];
        let mut logits_checksum_nonzero = 0usize;
        let mut text_checksum_nonzero = 0usize;
        let real_tokenizer_path = qwen3_dense_0_6b_real_tokenizer_path();
        let sample_token_count = qwen3_dense_0_6b_real_tokenizer_asset_summary()
            .expect("real tokenizer summary")
            .as_ref()
            .map(qwen3_dense_0_6b_tokenizer_sample_token_count)
            .unwrap_or(QWEN3_DENSE_0_6B_PROFILE.vocab_size)
            .min(QWEN3_DENSE_0_6B_PROFILE.vocab_size);
        let real_weights_present = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok();
        let runtime_forward_expected = real_weights_present
            && !crate::qwen3_dense_0_6b_guest_input_token_ids(guest_input).is_empty();
        let mut logits_text_byte_offset = 0u64;
        for entry in 0..LOGITS_ENTRY_COUNT {
            let base = LOGITS_TABLE_BASE + entry * LOGITS_TABLE_ENTRY_BYTES;
            let result_base = RESULT_TABLE_BASE + entry * 80;
            let shard = entry / TILES_PER_SHARD;
            let round1_checksum = u64::from_le_bytes(
                output[result_base + 72..result_base + 80]
                    .try_into()
                    .expect("round1 checksum"),
            );
            let sampled_token = u64::from_le_bytes(
                output[base + 32..base + 40]
                    .try_into()
                    .expect("sampled token"),
            );
            let runner_up_token = u64::from_le_bytes(
                output[base + 40..base + 48]
                    .try_into()
                    .expect("runner up token"),
            );
            let margin_milli = u64::from_le_bytes(
                output[base + 48..base + 56]
                    .try_into()
                    .expect("logits margin"),
            );
            let logits_checksum = u64::from_le_bytes(
                output[base + 56..base + 64]
                    .try_into()
                    .expect("logits checksum"),
            );
            let text_checksum = u64::from_le_bytes(
                output[base + 64..base + 72]
                    .try_into()
                    .expect("text checksum"),
            );
            let step_index = u64::from_le_bytes(
                output[base + 72..base + 80]
                    .try_into()
                    .expect("logits step index"),
            );
            let kvcache_read_digest = u64::from_le_bytes(
                output[base + 80..base + 88]
                    .try_into()
                    .expect("logits kvcache read digest"),
            );
            let qkv_reference_digest = u64::from_le_bytes(
                output[base + 88..base + 96]
                    .try_into()
                    .expect("logits qkv reference digest"),
            );
            let real_path_digest = u64::from_le_bytes(
                output[base + 96..base + 104]
                    .try_into()
                    .expect("logits real path digest"),
            );
            let full_vocab_checked_token_count = u64::from_le_bytes(
                output[base + 104..base + 112]
                    .try_into()
                    .expect("logits full vocab checked token count"),
            );
            let full_vocab_logits_checksum = u64::from_le_bytes(
                output[base + 112..base + 120]
                    .try_into()
                    .expect("logits full vocab checksum"),
            );
            let top_logit_bits = u64::from_le_bytes(
                output[base + 120..base + 128]
                    .try_into()
                    .expect("logits top bits"),
            );
            let runner_up_logit_bits = u64::from_le_bytes(
                output[base + 128..base + 136]
                    .try_into()
                    .expect("logits runner bits"),
            );
            let runtime_forward_layer_count = u64::from_le_bytes(
                output[base + 136..base + 144]
                    .try_into()
                    .expect("runtime forward layer count"),
            );
            let runtime_forward_final_hidden_checksum = u64::from_le_bytes(
                output[base + 144..base + 152]
                    .try_into()
                    .expect("runtime forward final hidden checksum"),
            );
            let runtime_forward_checksum = u64::from_le_bytes(
                output[base + 152..base + 160]
                    .try_into()
                    .expect("runtime forward checksum"),
            );
            let candidate_count = u64::from_le_bytes(
                output[base + 160..base + 168]
                    .try_into()
                    .expect("logits candidate count"),
            );
            let candidate0_base = base + 168;
            let candidate1_base = candidate0_base + 48;
            let candidate0_token = u64::from_le_bytes(
                output[candidate0_base..candidate0_base + 8]
                    .try_into()
                    .expect("logits candidate0 token"),
            );
            let candidate0_logit_bits = u64::from_le_bytes(
                output[candidate0_base + 8..candidate0_base + 16]
                    .try_into()
                    .expect("logits candidate0 bits"),
            );
            let candidate0_text_checksum = u64::from_le_bytes(
                output[candidate0_base + 16..candidate0_base + 24]
                    .try_into()
                    .expect("logits candidate0 text checksum"),
            );
            let candidate0_piece_bytes = u64::from_le_bytes(
                output[candidate0_base + 24..candidate0_base + 32]
                    .try_into()
                    .expect("logits candidate0 piece bytes"),
            );
            let candidate0_piece_word0 = u64::from_le_bytes(
                output[candidate0_base + 32..candidate0_base + 40]
                    .try_into()
                    .expect("logits candidate0 piece word0"),
            );
            let candidate1_token = u64::from_le_bytes(
                output[candidate1_base..candidate1_base + 8]
                    .try_into()
                    .expect("logits candidate1 token"),
            );
            let candidate1_logit_bits = u64::from_le_bytes(
                output[candidate1_base + 8..candidate1_base + 16]
                    .try_into()
                    .expect("logits candidate1 bits"),
            );
            let candidate1_text_checksum = u64::from_le_bytes(
                output[candidate1_base + 16..candidate1_base + 24]
                    .try_into()
                    .expect("logits candidate1 text checksum"),
            );
            let candidate1_piece_bytes = u64::from_le_bytes(
                output[candidate1_base + 24..candidate1_base + 32]
                    .try_into()
                    .expect("logits candidate1 piece bytes"),
            );
            let candidate1_piece_word0 = u64::from_le_bytes(
                output[candidate1_base + 32..candidate1_base + 40]
                    .try_into()
                    .expect("logits candidate1 piece word0"),
            );
            assert_eq!(
                u64::from_le_bytes(output[base..base + 8].try_into().expect("logits shard")),
                shard as u64
            );
            assert_eq!(
                u64::from_le_bytes(output[base + 8..base + 16].try_into().expect("logits tile")),
                entry as u64
            );
            assert_eq!(step_index, entry as u64);
            assert_eq!(
                kvcache_read_digest,
                *state_read_digest_by_tile
                    .get(&(entry as u64))
                    .expect("logits kvcache read digest by tile")
            );
            if real_weights_present {
                assert_ne!(qkv_reference_digest, 0);
                assert_ne!(real_path_digest, 0);
                assert_eq!(
                    full_vocab_checked_token_count,
                    QWEN3_DENSE_0_6B_PROFILE.vocab_size
                );
                assert_ne!(full_vocab_logits_checksum, 0);
                assert_ne!(top_logit_bits, 0);
                assert_ne!(runner_up_logit_bits, 0);
                assert_eq!(candidate_count, 4);
                assert_eq!(candidate0_token, sampled_token);
                assert_eq!(candidate0_logit_bits, top_logit_bits);
                assert_eq!(candidate0_text_checksum, text_checksum);
                assert_ne!(candidate0_piece_bytes, 0);
                assert_ne!(candidate0_piece_word0, 0);
                assert_eq!(candidate1_token, runner_up_token);
                assert_eq!(candidate1_logit_bits, runner_up_logit_bits);
                assert_ne!(candidate1_text_checksum, 0);
                assert_ne!(candidate1_piece_bytes, 0);
                assert_ne!(candidate1_piece_word0, 0);
                if runtime_forward_expected {
                    assert_eq!(
                        runtime_forward_layer_count,
                        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
                    );
                    assert_ne!(runtime_forward_final_hidden_checksum, 0);
                    assert_ne!(runtime_forward_checksum, 0);
                } else {
                    assert_eq!(runtime_forward_layer_count, 0);
                    assert_eq!(runtime_forward_final_hidden_checksum, 0);
                    assert_eq!(runtime_forward_checksum, 0);
                }
            } else {
                assert_eq!(qkv_reference_digest, 0);
                assert_eq!(real_path_digest, 0);
                assert_eq!(full_vocab_checked_token_count, 0);
                assert_eq!(full_vocab_logits_checksum, 0);
                assert_eq!(top_logit_bits, 0);
                assert_eq!(runner_up_logit_bits, 0);
                assert_eq!(candidate_count, 2);
                assert_eq!(candidate0_token, sampled_token);
                assert_eq!(candidate0_text_checksum, text_checksum);
                assert_ne!(candidate0_piece_bytes, 0);
                assert_ne!(candidate0_piece_word0, 0);
                assert_eq!(candidate1_token, runner_up_token);
                assert_ne!(candidate1_text_checksum, 0);
                assert_ne!(candidate1_piece_bytes, 0);
                assert_ne!(candidate1_piece_word0, 0);
                assert_eq!(runtime_forward_layer_count, 0);
                assert_eq!(runtime_forward_final_hidden_checksum, 0);
                assert_eq!(runtime_forward_checksum, 0);
            }
            assert_ne!(
                u64::from_le_bytes(
                    output[base + 16..base + 24]
                        .try_into()
                        .expect("logits segment")
                ),
                0
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 24..base + 32]
                        .try_into()
                        .expect("logits count")
                ),
                QWEN3_DENSE_0_6B_PROFILE.vocab_size
            );
            if real_weights_present {
                assert!(sampled_token < sample_token_count);
                assert!(runner_up_token < sample_token_count);
                assert_ne!(sampled_token, runner_up_token);
                assert_ne!(margin_milli, 0);
                assert_ne!(logits_checksum, 0);
            } else {
                let fallback_logits_seed = round1_checksum
                    ^ kvcache_read_digest.rotate_left(13)
                    ^ qkv_reference_digest.rotate_left(19)
                    ^ real_path_digest.rotate_left(23);
                assert_eq!(
                    sampled_token,
                    qwen3_dense_0_6b_sampled_token(
                        fallback_logits_seed,
                        entry as u64,
                        sample_token_count,
                    )
                );
                assert_eq!(
                    runner_up_token,
                    (sampled_token
                        + 17
                        + shard as u64
                        + entry as u64
                        + (kvcache_read_digest & 0x0f)
                        + ((qkv_reference_digest >> 4) & 0x0f)
                        + ((real_path_digest >> 8) & 0x0f))
                        % sample_token_count
                );
                assert_eq!(margin_milli, 1_000 + (entry as u64 * 7) + shard as u64);
                assert_eq!(
                    logits_checksum,
                    qwen3_dense_0_6b_logits_checksum(
                        round1_checksum,
                        entry as u64,
                        sampled_token,
                        runner_up_token,
                        margin_milli,
                        0,
                        0,
                        kvcache_read_digest,
                        qkv_reference_digest,
                        real_path_digest,
                    )
                );
            }
            assert_eq!(
                text_checksum,
                qwen3_dense_0_6b_sample_text_checksum(
                    entry as u64,
                    sampled_token,
                    logits_text_byte_offset,
                    real_tokenizer_path.as_deref()
                )
                .expect("expected sample text checksum")
            );
            let logits_piece =
                qwen3_dense_0_6b_token_piece(sampled_token, real_tokenizer_path.as_deref())
                    .expect("logits token piece");
            logits_text_byte_offset += logits_piece.byte_len;
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 72..base + 80]
                        .try_into()
                        .expect("sample step")
                ),
                entry as u64
            );
            if !sampled_tokens[..entry].contains(&sampled_token) {
                sampled_token_distinct += 1;
            }
            sampled_tokens[entry] = sampled_token;
            logits_checksum_nonzero += (logits_checksum != 0) as usize;
            text_checksum_nonzero += (text_checksum != 0) as usize;
        }
        if real_weights_present {
            assert_eq!(sampled_token_distinct, 1);
        } else {
            assert!(sampled_token_distinct > 1);
        }
        assert_eq!(logits_checksum_nonzero, LOGITS_ENTRY_COUNT);
        assert_eq!(text_checksum_nonzero, LOGITS_ENTRY_COUNT);

        assert_eq!(
            u64::from_le_bytes(
                output[TOKEN_TEXT_TABLE_HEADER..TOKEN_TEXT_TABLE_HEADER + 8]
                    .try_into()
                    .expect("token text table marker")
            ),
            0x7133773474787430
        );
        assert_eq!(
            u64::from_le_bytes(
                output[TOKEN_TEXT_TABLE_HEADER + 8..TOKEN_TEXT_TABLE_HEADER + 16]
                    .try_into()
                    .expect("token text table count")
            ),
            TOKEN_TEXT_ENTRY_COUNT as u64
        );
        assert_eq!(
            u64::from_le_bytes(
                output[TOKEN_TEXT_TABLE_HEADER + 16..TOKEN_TEXT_TABLE_HEADER + 24]
                    .try_into()
                    .expect("token text table entry words")
            ),
            8
        );
        assert_eq!(
            u64::from_le_bytes(
                output[TOKEN_TEXT_TABLE_HEADER + 24..TOKEN_TEXT_TABLE_HEADER + 32]
                    .try_into()
                    .expect("token text table bytes")
            ),
            TOKEN_TEXT_TABLE_BYTES as u64
        );
        let token_text_total_bytes = read_u64_le_at(output, TOKEN_TEXT_TABLE_HEADER + 32);
        let token_text_policy_hash = read_u64_le_at(output, TOKEN_TEXT_TABLE_HEADER + 40);
        let token_text_policy_kind = read_u64_le_at(output, TOKEN_TEXT_TABLE_HEADER + 48);
        if real_tokenizer_path.is_some() {
            assert_ne!(token_text_total_bytes, 144);
            assert_ne!(
                token_text_policy_hash,
                tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE).policy_hash
            );
            assert_eq!(
                token_text_policy_kind,
                QWEN3_DENSE_0_6B_TOKENIZER_ASSET_POLICY_KIND
            );
        } else {
            assert_eq!(token_text_total_bytes, 144);
            assert_eq!(
                token_text_policy_hash,
                tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE).policy_hash
            );
            assert_eq!(
                token_text_policy_kind,
                QWEN3_DENSE_0_6B_TOKENIZER_POLICY_KIND
            );
        }
        let mut token_piece_bytes = 0u64;
        let mut text_output_token_words = Vec::new();
        let mut text_output_text_words = Vec::new();
        let mut text_output_logits_words = Vec::new();
        let mut text_output_sequence_words = Vec::new();
        let mut expected_text_output_bytes = Vec::new();
        for entry in 0..TOKEN_TEXT_ENTRY_COUNT {
            let base = TOKEN_TEXT_TABLE_BASE + entry * TOKEN_TEXT_TABLE_ENTRY_BYTES;
            let logits_base = LOGITS_TABLE_BASE + entry * LOGITS_TABLE_ENTRY_BYTES;
            let sampled_token = u64::from_le_bytes(
                output[logits_base + 32..logits_base + 40]
                    .try_into()
                    .expect("token text sampled token source"),
            );
            let text_checksum = u64::from_le_bytes(
                output[logits_base + 64..logits_base + 72]
                    .try_into()
                    .expect("token text checksum source"),
            );
            let runner_up_token = read_u64_le_at(output, logits_base + 40);
            let margin_milli = read_u64_le_at(output, logits_base + 48);
            let logits_checksum = read_u64_le_at(output, logits_base + 56);
            let real_path_digest = read_u64_le_at(output, logits_base + 96);
            let piece = if let Some(path) = real_tokenizer_path.as_deref() {
                token_piece_from_tokenizer_path(path, sampled_token)
                    .expect("real tokenizer token piece")
            } else {
                token_piece_from_policy(tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE), sampled_token)
            };
            let piece_bytes = if let Some(path) = real_tokenizer_path.as_deref() {
                token_piece_bytes_from_tokenizer_path(path, sampled_token)
                    .expect("real tokenizer token bytes")
            } else {
                token_piece_bytes_from_policy(
                    tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE),
                    sampled_token,
                )
            };
            let expected_flags =
                (entry == 0) as u64 | (((entry + 1) == TOKEN_TEXT_ENTRY_COUNT) as u64) << 1;
            assert_eq!(
                u64::from_le_bytes(output[base..base + 8].try_into().expect("token text step")),
                entry as u64
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 8..base + 16]
                        .try_into()
                        .expect("token text token")
                ),
                sampled_token
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 16..base + 24]
                        .try_into()
                        .expect("token text offset")
                ),
                token_piece_bytes
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 24..base + 32]
                        .try_into()
                        .expect("token text bytes")
                ),
                piece.byte_len
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 32..base + 40]
                        .try_into()
                        .expect("token text word0")
                ),
                piece.word0
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 40..base + 48]
                        .try_into()
                        .expect("token text word1")
                ),
                piece.word1
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 48..base + 56]
                        .try_into()
                        .expect("token text checksum")
                ),
                text_checksum
            );
            assert_eq!(
                u64::from_le_bytes(
                    output[base + 56..base + 64]
                        .try_into()
                        .expect("token text flags")
                ),
                expected_flags
            );
            text_output_token_words.extend_from_slice(&[
                entry as u64,
                sampled_token,
                runner_up_token,
                piece.checksum,
            ]);
            text_output_text_words.extend_from_slice(&[
                entry as u64,
                token_piece_bytes,
                piece.byte_len,
                piece.word0,
                piece.word1,
            ]);
            text_output_logits_words.extend_from_slice(&[
                entry as u64,
                logits_checksum,
                margin_milli,
                QWEN3_DENSE_0_6B_PROFILE.vocab_size,
                real_path_digest,
            ]);
            text_output_sequence_words.extend_from_slice(&[
                entry as u64,
                sampled_token,
                runner_up_token,
                token_piece_bytes,
                piece.byte_len,
                piece.checksum,
                text_checksum,
                logits_checksum,
                real_path_digest,
            ]);
            expected_text_output_bytes.extend_from_slice(&piece_bytes);
            token_piece_bytes += piece.byte_len;
        }
        assert_eq!(token_piece_bytes, token_text_total_bytes);
        assert_eq!(
            expected_text_output_bytes.len() as u64,
            token_text_total_bytes
        );
        text_output_sequence_words.push(token_text_total_bytes);

        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_TABLE_HEADER),
            0x71337734746f7430
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_TABLE_HEADER + 8),
            TOKEN_TEXT_ENTRY_COUNT as u64
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_TABLE_HEADER + 16),
            token_text_total_bytes
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_TABLE_HEADER + 24),
            checksum_words(&text_output_sequence_words)
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_TABLE_HEADER + 32),
            checksum_words(&text_output_token_words)
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_TABLE_HEADER + 40),
            checksum_words(&text_output_text_words)
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_TABLE_HEADER + 48),
            checksum_words(&text_output_logits_words)
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_TABLE_HEADER + 56),
            token_text_policy_kind
        );

        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_BYTES_TABLE_HEADER),
            0x71337734746f6230
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_BYTES_TABLE_HEADER + 8),
            expected_text_output_bytes.len() as u64
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_BYTES_TABLE_HEADER + 16),
            (text_output_bytes_table_bytes / 8) as u64
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_BYTES_TABLE_HEADER + 24),
            text_output_bytes_table_bytes as u64
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_BYTES_TABLE_HEADER + 32),
            qwen3_dense_0_6b_text_output_bytes_checksum(&expected_text_output_bytes)
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_BYTES_TABLE_HEADER + 40),
            checksum_words(&text_output_sequence_words)
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_BYTES_TABLE_HEADER + 48),
            TOKEN_TEXT_ENTRY_COUNT as u64
        );
        assert_eq!(
            read_u64_le_at(output, TEXT_OUTPUT_BYTES_TABLE_HEADER + 56),
            token_text_policy_kind
        );
        assert_eq!(
            &output[TEXT_OUTPUT_BYTES_TABLE_BASE
                ..TEXT_OUTPUT_BYTES_TABLE_BASE + expected_text_output_bytes.len()],
            expected_text_output_bytes.as_slice()
        );
        assert!(
            output[TEXT_OUTPUT_BYTES_TABLE_BASE + expected_text_output_bytes.len()
                ..TEXT_OUTPUT_BYTES_TABLE_BASE + text_output_bytes_table_bytes]
                .iter()
                .all(|byte| *byte == 0)
        );
        let text_output_report = qwen3_dense_0_6b_text_output_report_from_prefill_output(output)
            .expect("guest-visible text output report");
        assert_eq!(
            text_output_report.token_count,
            TOKEN_TEXT_ENTRY_COUNT as u64
        );
        assert_eq!(
            text_output_report.byte_len,
            expected_text_output_bytes.len() as u64
        );
        assert_eq!(
            text_output_report.padded_byte_len,
            text_output_bytes_table_bytes as u64
        );
        assert_eq!(
            text_output_report.byte_checksum,
            qwen3_dense_0_6b_text_output_bytes_checksum(&expected_text_output_bytes)
        );
        assert_eq!(
            text_output_report.sequence_checksum,
            checksum_words(&text_output_sequence_words)
        );
        assert_eq!(
            text_output_report.token_checksum,
            checksum_words(&text_output_token_words)
        );
        assert_eq!(
            text_output_report.text_checksum,
            checksum_words(&text_output_text_words)
        );
        assert_eq!(
            text_output_report.logits_checksum,
            checksum_words(&text_output_logits_words)
        );
        assert_eq!(
            text_output_report.tokenizer_policy_kind,
            token_text_policy_kind
        );
        assert_eq!(text_output_report.samples.len(), TOKEN_TEXT_ENTRY_COUNT);
        assert_eq!(text_output_report.bytes, expected_text_output_bytes);
        assert!(!text_output_report.text_lossy.is_empty());
        assert_eq!(
            text_output_report.attention.score_count,
            (TILE_COUNT * 2) as u64
        );
        assert_eq!(
            text_output_report.attention.softmax_count,
            (TILE_COUNT * 2) as u64
        );
        assert_eq!(
            text_output_report.attention.context_count,
            (TILE_COUNT * 2) as u64
        );
        assert_eq!(text_output_report.attention.stage_mask, 0x07);
        assert_ne!(text_output_report.attention.score_checksum, 0);
        assert_ne!(text_output_report.attention.softmax_checksum, 0);
        assert_ne!(text_output_report.attention.context_checksum, 0);
        assert_ne!(text_output_report.attention.aggregate_checksum, 0);
        assert_eq!(
            text_output_report.post_attention.mlp_activation_count,
            TILE_COUNT as u64
        );
        assert_eq!(
            text_output_report.post_attention.host_partial_count,
            TILE_COUNT as u64
        );
        assert_eq!(
            text_output_report.post_attention.mlp_output_count,
            TILE_COUNT as u64
        );
        assert_eq!(
            text_output_report.post_attention.residual_norm_count,
            TILE_COUNT as u64
        );
        assert_eq!(
            text_output_report.post_attention.next_partial_count,
            TILE_COUNT as u64
        );
        assert_eq!(text_output_report.post_attention.stage_mask, 0x1f);
        assert_ne!(text_output_report.post_attention.mlp_activation_checksum, 0);
        assert_ne!(text_output_report.post_attention.host_partial_checksum, 0);
        assert_ne!(text_output_report.post_attention.mlp_output_checksum, 0);
        assert_ne!(text_output_report.post_attention.residual_norm_checksum, 0);
        assert_ne!(text_output_report.post_attention.next_partial_checksum, 0);
        assert_ne!(text_output_report.post_attention.aggregate_checksum, 0);
        assert_eq!(
            text_output_report.result_flow.publish_count,
            TILE_COUNT as u64
        );
        assert_eq!(
            text_output_report.result_flow.resolve_count,
            TILE_COUNT as u64
        );
        assert_eq!(
            text_output_report.result_flow.round1_compute_count,
            TILE_COUNT as u64
        );
        assert_eq!(
            text_output_report.result_flow.result_count,
            TILE_COUNT as u64
        );
        assert!(text_output_report.result_flow.round0_distinct_count > 1);
        assert!(text_output_report.result_flow.round1_distinct_count > 1);
        assert_ne!(text_output_report.result_flow.round0_checksum, 0);
        assert_ne!(text_output_report.result_flow.round1_checksum, 0);
        assert_ne!(text_output_report.result_flow.aggregate_checksum, 0);
        assert_ne!(text_output_report.synthetic.stage_checksum, 0);
        if std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH").is_ok() {
            let real_qkv = text_output_report
                .real_qkv
                .as_ref()
                .expect("real QKV reference report");
            assert_eq!(real_qkv.layer_id, 0);
            assert_eq!(real_qkv.reference_layer_count, 2);
            assert_eq!(real_qkv.shard_count, QWEN3_DENSE_0_6B_PROFILE.tp_nodes);
            assert_eq!(real_qkv.stage_link_count, 128);
            assert_eq!(real_qkv.stage_kind_mask, 0x0f);
            assert_ne!(real_qkv.total_weight_bytes, 0);
            assert_ne!(real_qkv.aggregate_checksum, 0);
            assert_ne!(real_qkv.qkv_rows, 0);
            assert_ne!(real_qkv.stage_link_checksum, 0);
            assert_ne!(real_qkv.synthetic_checksum, 0);
            assert_ne!(real_qkv.real_weight_checksum, 0);
            assert_ne!(real_qkv.real_value_checksum, 0);
            assert_ne!(real_qkv.real_output_checksum, 0);
            assert_ne!(real_qkv.reference_layer_checksum, 0);
            assert_ne!(real_qkv.next_reference_layer_checksum, 0);
            assert_ne!(
                real_qkv.reference_layer_checksum,
                real_qkv.next_reference_layer_checksum
            );

            let real_mlp = text_output_report
                .real_mlp
                .as_ref()
                .expect("real MLP reference report");
            assert_eq!(real_mlp.layer_id, 0);
            assert_eq!(real_mlp.next_layer_id, 1);
            assert_eq!(real_mlp.shard_count, QWEN3_DENSE_0_6B_PROFILE.tp_nodes);
            assert_eq!(real_mlp.next_shard_count, QWEN3_DENSE_0_6B_PROFILE.tp_nodes);
            assert_ne!(real_mlp.total_weight_bytes, 0);
            assert_ne!(real_mlp.next_total_weight_bytes, 0);
            assert_ne!(real_mlp.total_intermediate_rows, 0);
            assert_ne!(real_mlp.next_total_intermediate_rows, 0);
            assert_ne!(real_mlp.aggregate_checksum, 0);
            assert_ne!(real_mlp.next_aggregate_checksum, 0);
            assert_ne!(real_mlp.real_weight_checksum, 0);
            assert_ne!(real_mlp.real_activation_checksum, 0);
            assert_ne!(real_mlp.real_output_checksum, 0);
            assert_ne!(real_mlp.next_real_output_checksum, 0);
            assert_ne!(real_mlp.sample_checksum, 0);
            assert_ne!(real_mlp.table_checksum, 0);
            assert!(text_output_report.synthetic.qkv_base_tile_real_backed);
            assert!(text_output_report.synthetic.attention_score_real_backed);
            assert!(text_output_report.synthetic.attention_context_real_backed);
            assert!(text_output_report.synthetic.mlp_activation_real_backed);
            assert!(text_output_report.synthetic.mlp_output_real_backed);
            assert!(text_output_report.synthetic.logits_candidates_real_backed);
            assert!(text_output_report.synthetic.token_text_real_backed);
            assert!(!text_output_report.synthetic.guest_input_real_backed);
            assert!(!text_output_report.guest_input.real_backed);
            assert_eq!(text_output_report.synthetic.stage_count, 1);
            assert_ne!(
                text_output_report.synthetic.stage_mask & QWEN3_SYNTHETIC_GUEST_INPUT,
                0
            );
            assert_eq!(
                text_output_report.synthetic.stage_mask & QWEN3_SYNTHETIC_QKV_BASE_TILE,
                0
            );
            assert_eq!(
                text_output_report.synthetic.stage_mask & QWEN3_SYNTHETIC_ATTENTION_SCORE,
                0
            );
            assert_eq!(
                text_output_report.synthetic.stage_mask & QWEN3_SYNTHETIC_ATTENTION_CONTEXT,
                0
            );
            assert_eq!(
                text_output_report.synthetic.stage_mask & QWEN3_SYNTHETIC_MLP_ACTIVATION,
                0
            );
            assert_eq!(
                text_output_report.synthetic.stage_mask & QWEN3_SYNTHETIC_MLP_OUTPUT,
                0
            );
            assert_eq!(
                text_output_report.synthetic.stage_mask & QWEN3_SYNTHETIC_LOGITS_CANDIDATES,
                0
            );
            assert_eq!(
                text_output_report.synthetic.stage_mask & QWEN3_SYNTHETIC_TOKEN_TEXT,
                0
            );

            let real_logits = text_output_report
                .real_logits
                .as_ref()
                .expect("real logits reference report");
            assert_eq!(
                real_logits.candidate_count,
                LOGITS_REFERENCE_ENTRY_COUNT as u64
            );
            assert_eq!(real_logits.token_count, real_logits.candidate_count);
            assert!(real_logits.distinct_step_count > 1);
            assert!(real_logits.distinct_token_count > 1);
            assert!(real_logits.distinct_step_count <= real_logits.candidate_count);
            assert!(real_logits.distinct_token_count <= real_logits.candidate_count);
            assert!(real_logits.row_byte_count >= real_logits.candidate_count);
            assert_ne!(real_logits.row_checksum, 0);
            assert_ne!(real_logits.logit_checksum, 0);
            if runtime_forward_expected {
                assert_eq!(
                    real_logits.sampled_pair_count,
                    (TOKEN_TEXT_ENTRY_COUNT * 2) as u64
                );
                assert_eq!(
                    real_logits.selection_match_count,
                    TOKEN_TEXT_ENTRY_COUNT as u64
                );
                assert_eq!(
                    real_logits.margin_match_count,
                    TOKEN_TEXT_ENTRY_COUNT as u64
                );
                assert_eq!(
                    real_logits.checksum_match_count,
                    TOKEN_TEXT_ENTRY_COUNT as u64
                );
                assert_eq!(real_logits.max_margin_delta_milli, 0);
            } else {
                assert_eq!(real_logits.sampled_pair_count, 0);
                assert_eq!(real_logits.selection_match_count, 0);
                assert_eq!(real_logits.margin_match_count, 0);
                assert_eq!(real_logits.checksum_match_count, 0);
            }
            assert_eq!(real_logits.vocab_size, QWEN3_DENSE_0_6B_PROFILE.vocab_size);
            assert_eq!(
                real_logits.hidden_size,
                QWEN3_DENSE_0_6B_PROFILE.hidden_size
            );
            assert_ne!(real_logits.aggregate_checksum, 0);
            assert_ne!(real_logits.final_norm_checksum, 0);
            assert_ne!(real_logits.top_logit_bits_checksum, 0);
            assert_ne!(real_logits.runner_logit_bits_checksum, 0);
            assert_ne!(real_logits.comparison_checksum, 0);
            assert_ne!(real_logits.selection_checksum, 0);

            let tokenizer_asset_table_header = text_output_bytes_table_end;
            let tokenizer_asset_table_base = tokenizer_asset_table_header + 64;
            const TOKENIZER_ASSET_TABLE_BYTES: usize = 64 + 5 * 4 * 8;
            assert_eq!(
                read_u64_le_at(output, tokenizer_asset_table_header),
                0x71337734746f6b30
            );
            assert_eq!(read_u64_le_at(output, tokenizer_asset_table_header + 8), 5);
            assert_eq!(read_u64_le_at(output, tokenizer_asset_table_header + 16), 4);
            assert_eq!(
                read_u64_le_at(output, tokenizer_asset_table_header + 24),
                160
            );
            assert_eq!(
                read_u64_le_at(output, tokenizer_asset_table_header + 32),
                token_text_policy_hash
            );
            assert_eq!(
                read_u64_le_at(output, tokenizer_asset_table_header + 40),
                QWEN3_DENSE_0_6B_PROFILE.vocab_size
            );
            assert!(read_u64_le_at(output, tokenizer_asset_table_header + 48) > 150_000);
            assert_ne!(read_u64_le_at(output, tokenizer_asset_table_base), 0);
            assert_ne!(read_u64_le_at(output, tokenizer_asset_table_base + 8), 0);
            assert_ne!(read_u64_le_at(output, tokenizer_asset_table_base + 16), 0);

            let weight_reference_table_header =
                text_output_bytes_table_end + TOKENIZER_ASSET_TABLE_BYTES;
            let weight_reference_table_base = weight_reference_table_header + 64;
            const WEIGHT_REFERENCE_TABLE_ENTRY_BYTES: usize = 112;
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_header),
                0x7133773477667430
            );
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_header + 8),
                QWEN3_DENSE_0_6B_PROFILE.tp_nodes
            );
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_header + 16),
                QWEN3_WEIGHT_REFERENCE_ENTRY_WORDS
            );
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_header + 24),
                QWEN3_DENSE_0_6B_PROFILE.tp_nodes * WEIGHT_REFERENCE_TABLE_ENTRY_BYTES as u64
            );
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_header + 32),
                0
            );
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_header + 40),
                8_404_992
            );
            assert_ne!(
                read_u64_le_at(output, weight_reference_table_header + 48),
                0
            );
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_header + 56),
                4_096
            );
            assert_eq!(read_u64_le_at(output, weight_reference_table_base), 0);
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_base + 8),
                1024
            );
            assert_ne!(read_u64_le_at(output, weight_reference_table_base + 16), 0);
            assert_ne!(read_u64_le_at(output, weight_reference_table_base + 24), 0);
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_base + 72),
                256
            );
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_base + 80),
                128
            );
            assert_eq!(
                read_u64_le_at(output, weight_reference_table_base + 88),
                128
            );
            assert_eq!(read_u64_le_at(output, weight_reference_table_base + 96), 4);
            assert_ne!(read_u64_le_at(output, weight_reference_table_base + 104), 0);

            let weight_stage_link_table_header =
                weight_reference_table_base + QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize * 112;
            let weight_stage_link_table_base = weight_stage_link_table_header + 64;
            const WEIGHT_STAGE_LINK_ENTRY_BYTES: usize =
                QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS as usize * 8;
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_header),
                0x71337734776c6b30
            );
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_header + 8),
                (TILE_COUNT * 8) as u64
            );
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_header + 16),
                QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS
            );
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_header + 24),
                (TILE_COUNT * 8 * WEIGHT_STAGE_LINK_ENTRY_BYTES) as u64
            );
            assert_ne!(
                read_u64_le_at(output, weight_stage_link_table_header + 32),
                0
            );
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_header + 40),
                4
            );
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_header + 48),
                0
            );
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_header + 56),
                2
            );
            assert_eq!(read_u64_le_at(output, weight_stage_link_table_base), 0);
            assert_eq!(read_u64_le_at(output, weight_stage_link_table_base + 8), 0);
            assert_eq!(read_u64_le_at(output, weight_stage_link_table_base + 16), 1);
            assert_ne!(read_u64_le_at(output, weight_stage_link_table_base + 24), 0);
            assert_ne!(read_u64_le_at(output, weight_stage_link_table_base + 32), 0);
            assert_ne!(read_u64_le_at(output, weight_stage_link_table_base + 40), 0);
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_base + 48),
                read_u64_le_at(output, weight_reference_table_base + 16)
            );
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_base + 56),
                read_u64_le_at(output, weight_reference_table_base + 16)
            );
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_base + 64),
                1024
            );
            assert_eq!(
                read_u64_le_at(output, weight_stage_link_table_base + 72),
                1024
            );
            assert_eq!(read_u64_le_at(output, weight_stage_link_table_base + 80), 0);
            let q_link_base = weight_stage_link_table_base + WEIGHT_STAGE_LINK_ENTRY_BYTES;
            assert_eq!(read_u64_le_at(output, q_link_base + 16), 2);
            assert_eq!(
                read_u64_le_at(output, q_link_base + 40),
                read_u64_le_at(output, weight_reference_table_base + 24)
            );
            assert_eq!(
                read_u64_le_at(output, q_link_base + 48),
                read_u64_le_at(output, weight_reference_table_base + 48)
            );
            assert_eq!(
                read_u64_le_at(output, q_link_base + 56),
                read_u64_le_at(output, weight_reference_table_base + 48)
            );
            assert_eq!(read_u64_le_at(output, q_link_base + 64), 256);

            let mlp_reference_table_header = weight_stage_link_table_base
                + TILE_COUNT * 8 * QWEN3_WEIGHT_STAGE_LINK_ENTRY_WORDS as usize * 8;
            let mlp_reference_table_base = mlp_reference_table_header + 64;
            const MLP_REFERENCE_ENTRY_BYTES: usize = QWEN3_MLP_REFERENCE_ENTRY_WORDS as usize * 8;
            assert_eq!(
                read_u64_le_at(output, mlp_reference_table_header),
                0x713377346d6c7030
            );
            assert_eq!(
                read_u64_le_at(output, mlp_reference_table_header + 8),
                QWEN3_DENSE_0_6B_PROFILE.tp_nodes * 2
            );
            assert_eq!(
                read_u64_le_at(output, mlp_reference_table_header + 16),
                QWEN3_MLP_REFERENCE_ENTRY_WORDS
            );
            assert_eq!(
                read_u64_le_at(output, mlp_reference_table_header + 24),
                QWEN3_DENSE_0_6B_PROFILE.tp_nodes * 2 * MLP_REFERENCE_ENTRY_BYTES as u64
            );
            assert_eq!(read_u64_le_at(output, mlp_reference_table_header + 32), 0);
            assert_eq!(read_u64_le_at(output, mlp_reference_table_header + 40), 1);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_header + 48), 0);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_header + 56), 0);
            assert_eq!(read_u64_le_at(output, mlp_reference_table_base), 0);
            assert_eq!(read_u64_le_at(output, mlp_reference_table_base + 8), 0);
            assert_eq!(
                read_u64_le_at(output, mlp_reference_table_base + 16),
                QWEN3_DENSE_0_6B_PROFILE.hidden_size
            );
            assert_ne!(read_u64_le_at(output, mlp_reference_table_base + 24), 0);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_base + 32), 0);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_base + 40), 0);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_base + 48), 0);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_base + 72), 0);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_base + 80), 0);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_base + 88), 0);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_base + 104), 0);
            assert_ne!(read_u64_le_at(output, mlp_reference_table_base + 112), 0);

            let logits_reference_table_header = mlp_reference_table_base
                + QWEN3_DENSE_0_6B_PROFILE.tp_nodes as usize * 2 * MLP_REFERENCE_ENTRY_BYTES;
            let logits_reference_table_base = logits_reference_table_header + 64;
            const LOGITS_REFERENCE_ENTRY_BYTES: usize =
                QWEN3_LOGITS_REFERENCE_ENTRY_WORDS as usize * 8;
            assert_eq!(
                read_u64_le_at(output, logits_reference_table_header),
                0x713377346c6d6830
            );
            assert_eq!(
                read_u64_le_at(output, logits_reference_table_header + 8),
                LOGITS_REFERENCE_ENTRY_COUNT as u64
            );
            assert_eq!(
                read_u64_le_at(output, logits_reference_table_header + 16),
                QWEN3_LOGITS_REFERENCE_ENTRY_WORDS
            );
            assert_eq!(
                read_u64_le_at(output, logits_reference_table_header + 24),
                (LOGITS_REFERENCE_ENTRY_COUNT * LOGITS_REFERENCE_ENTRY_BYTES) as u64
            );
            assert_ne!(
                read_u64_le_at(output, logits_reference_table_header + 32),
                0
            );
            assert_ne!(
                read_u64_le_at(output, logits_reference_table_header + 40),
                0
            );
            assert_eq!(
                read_u64_le_at(output, logits_reference_table_header + 48),
                QWEN3_DENSE_0_6B_PROFILE.vocab_size
            );
            assert_eq!(
                read_u64_le_at(output, logits_reference_table_header + 56),
                QWEN3_DENSE_0_6B_PROFILE.hidden_size
            );
            assert_eq!(read_u64_le_at(output, logits_reference_table_base), 0);
            assert!(read_u64_le_at(output, logits_reference_table_base + 8) < sample_token_count);
            assert_eq!(
                read_u64_le_at(output, logits_reference_table_base + 16),
                QWEN3_DENSE_0_6B_PROFILE.hidden_size * 2
            );
            assert_ne!(read_u64_le_at(output, logits_reference_table_base + 24), 0);
            assert_ne!(read_u64_le_at(output, logits_reference_table_base + 32), 0);
            assert_ne!(read_u64_le_at(output, logits_reference_table_base + 40), 0);
        } else {
            assert!(text_output_report.real_qkv.is_none());
            assert!(text_output_report.real_mlp.is_none());
            assert!(text_output_report.real_logits.is_none());
        }

        assert_eq!(
            u64::from_le_bytes(
                output[1664..1672]
                    .try_into()
                    .expect("projection table marker")
            ),
            0x7133773471767430
        );
        assert_eq!(
            u64::from_le_bytes(
                output[1672..1680]
                    .try_into()
                    .expect("projection table count")
            ),
            48
        );
        assert_eq!(
            u64::from_le_bytes(
                output[1680..1688]
                    .try_into()
                    .expect("projection table entry words")
            ),
            10
        );
        assert_eq!(
            u64::from_le_bytes(
                output[1688..1696]
                    .try_into()
                    .expect("projection table bytes")
            ),
            3840
        );
        for tile in 0..TILE_COUNT {
            let shard = tile / TILES_PER_SHARD;
            let mut kind_mask = 0u64;
            for projection in 0..3usize {
                let base = 1728 + ((tile * 3) + projection) * 80;
                let projection_kind = u64::from_le_bytes(
                    output[base + 8..base + 16]
                        .try_into()
                        .expect("projection kind"),
                );
                let projection_checksum = u64::from_le_bytes(
                    output[base + 72..base + 80]
                        .try_into()
                        .expect("projection checksum"),
                );
                assert_eq!(
                    u64::from_le_bytes(
                        output[base..base + 8].try_into().expect("projection shard")
                    ),
                    shard as u64
                );
                assert!((1..=3).contains(&projection_kind));
                kind_mask |= 1u64 << (projection_kind - 1);
                assert_ne!(
                    u64::from_le_bytes(
                        output[base + 16..base + 24]
                            .try_into()
                            .expect("projection segment")
                    ),
                    0
                );
                assert_eq!(
                    u64::from_le_bytes(
                        output[base + 24..base + 32]
                            .try_into()
                            .expect("projection elems")
                    ),
                    128 * 128
                );
                assert_eq!(
                    u64::from_le_bytes(
                        output[base + 32..base + 40]
                            .try_into()
                            .expect("projection bytes")
                    ),
                    (128 * 128 * std::mem::size_of::<u16>()) as u64
                );
                assert_eq!(
                    u64::from_le_bytes(
                        output[base + 56..base + 64]
                            .try_into()
                            .expect("projection kv start")
                    ),
                    (tile * KV_BLOCKS_PER_TILE) as u64
                );
                assert_eq!(
                    u64::from_le_bytes(
                        output[base + 64..base + 72]
                            .try_into()
                            .expect("projection kv end")
                    ),
                    (tile * KV_BLOCKS_PER_TILE + KV_BLOCKS_PER_TILE) as u64
                );
                assert_ne!(projection_checksum, 0);
                let shard_plan = Qwen3Dense06bShard {
                    shard_id: shard as u64,
                    owner_node: shard as u64,
                    target_node: shard as u64,
                    head_start: (shard * 2) as u64,
                    head_end: (shard * 2 + 2) as u64,
                    kv_block_start: (tile * KV_BLOCKS_PER_TILE) as u64,
                    kv_block_end: (tile * KV_BLOCKS_PER_TILE + KV_BLOCKS_PER_TILE) as u64,
                };
                let prefill_hidden = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
                    guest_input,
                    None,
                    128 * 128,
                    shard_plan,
                    (tile % TILES_PER_SHARD) as u64,
                );
                let rmsnorm_hidden = expected_rmsnorm_tile(&prefill_hidden, shard_plan);
                let expected_kind = match projection_kind {
                    1 => Qwen3ProjectionKind::Q,
                    2 => Qwen3ProjectionKind::Kv,
                    3 => Qwen3ProjectionKind::V,
                    _ => unreachable!("validated projection kind"),
                };
                let expected_projection = expected_projection_tile(
                    &rmsnorm_hidden,
                    expected_kind,
                    projection_kind + 1,
                    shard_plan,
                );
                assert_eq!(
                    projection_checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&expected_projection),
                    "projection checksum must be derived from RMSNorm output"
                );
            }
            assert_eq!(kind_mask, 0x7);
        }
        assert_eq!(
            u64::from_le_bytes(
                output[5568..5576]
                    .try_into()
                    .expect("layer dep table marker")
            ),
            0x7133773464657030
        );
        assert_eq!(
            u64::from_le_bytes(
                output[5576..5584]
                    .try_into()
                    .expect("layer dep table count")
            ),
            384
        );
        assert_eq!(
            u64::from_le_bytes(
                output[5584..5592]
                    .try_into()
                    .expect("layer dep table entry words")
            ),
            11
        );
        assert_eq!(
            u64::from_le_bytes(
                output[5592..5600]
                    .try_into()
                    .expect("layer dep table bytes")
            ),
            33792
        );
        let mut stage_counts = [0usize; 25];
        let mut rmsnorm_stage_checks = 0usize;
        let mut projection_stage_checks = 0usize;
        let mut rope_stage_checks = 0usize;
        let mut attention_stage_checks = 0usize;
        let mut softmax_stage_checks = 0usize;
        let mut context_stage_checks = 0usize;
        let mut mlp_stage_checks = 0usize;
        let mut mlp_output_stage_checks = 0usize;
        let mut residual_stage_checks = 0usize;
        let mut next_layer_projection_stage_checks = 0usize;
        let mut next_layer_rope_stage_checks = 0usize;
        let mut next_layer_attention_stage_checks = 0usize;
        let mut next_layer_softmax_stage_checks = 0usize;
        let mut next_layer_context_stage_checks = 0usize;
        let mut partial_stage_checks = 0usize;
        let mut stage_checksums = [[0u64; 25]; 16];
        let mut stage_depends_on = [[0u64; 25]; 16];
        for entry in 0..384usize {
            let base = 5632 + entry * 88;
            let layer_id = u64::from_le_bytes(output[base..base + 8].try_into().expect("layer id"));
            let shard =
                u64::from_le_bytes(output[base + 8..base + 16].try_into().expect("dep shard"));
            let stage =
                u64::from_le_bytes(output[base + 16..base + 24].try_into().expect("dep stage"));
            let depends_on = u64::from_le_bytes(
                output[base + 24..base + 32]
                    .try_into()
                    .expect("dep depends on"),
            );
            let segment = u64::from_le_bytes(
                output[base + 40..base + 48]
                    .try_into()
                    .expect("dep segment"),
            );
            let elems =
                u64::from_le_bytes(output[base + 48..base + 56].try_into().expect("dep elems"));
            let bytes =
                u64::from_le_bytes(output[base + 56..base + 64].try_into().expect("dep bytes"));
            let checksum = u64::from_le_bytes(
                output[base + 80..base + 88]
                    .try_into()
                    .expect("dep checksum"),
            );

            let tile = layer_id as usize;
            assert!(tile < TILE_COUNT);
            assert!(shard < 8);
            assert_eq!(shard as usize, tile / TILES_PER_SHARD);
            assert!((1..=24).contains(&stage));
            stage_counts[stage as usize] += 1;
            assert_ne!(segment, 0);
            assert_eq!(elems, 128 * 128);
            assert!(bytes == 128 * 128 * 2 || bytes == 128 * 128 * 4);
            assert_ne!(checksum, 0);
            stage_checksums[tile][stage as usize] = checksum;
            stage_depends_on[tile][stage as usize] = depends_on;
            let expected_shard = Qwen3Dense06bShard {
                shard_id: shard,
                owner_node: shard,
                target_node: shard,
                head_start: shard * 2,
                head_end: shard * 2 + 2,
                kv_block_start: (tile * KV_BLOCKS_PER_TILE) as u64,
                kv_block_end: (tile * KV_BLOCKS_PER_TILE + KV_BLOCKS_PER_TILE) as u64,
            };
            let expected_tile_index = (tile % TILES_PER_SHARD) as u64;
            if stage == 1 {
                let shard = expected_shard;
                let prefill_hidden = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
                    guest_input,
                    None,
                    128 * 128,
                    shard,
                    expected_tile_index,
                );
                let rmsnorm_hidden = expected_rmsnorm_tile(&prefill_hidden, shard);
                assert_eq!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&rmsnorm_hidden),
                    "stage 1 checksum must reflect RMSNorm output"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&prefill_hidden),
                    "stage 1 checksum must not be the raw prefill hidden input"
                );
                rmsnorm_stage_checks += 1;
            }
            if (2..=4).contains(&stage) {
                let shard = expected_shard;
                let projection_kind = match stage {
                    2 => Qwen3ProjectionKind::Q,
                    3 => Qwen3ProjectionKind::Kv,
                    4 => Qwen3ProjectionKind::V,
                    _ => unreachable!("validated projection stage"),
                };
                let prefill_hidden = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
                    guest_input,
                    None,
                    128 * 128,
                    shard,
                    expected_tile_index,
                );
                let rmsnorm_hidden = expected_rmsnorm_tile(&prefill_hidden, shard);
                let projection =
                    expected_projection_tile(&rmsnorm_hidden, projection_kind, stage, shard);
                assert_eq!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&projection),
                    "projection stage checksum must reflect RMSNorm-derived projection output"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&rmsnorm_hidden),
                    "projection stage checksum must not be the raw RMSNorm input"
                );
                projection_stage_checks += 1;
            }
            if stage == 5 || stage == 6 {
                let shard = expected_shard;
                let projection_kind = if stage == 5 {
                    Qwen3ProjectionKind::Q
                } else {
                    Qwen3ProjectionKind::Kv
                };
                let prefill_hidden = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
                    guest_input,
                    None,
                    128 * 128,
                    shard,
                    expected_tile_index,
                );
                let rmsnorm_hidden = expected_rmsnorm_tile(&prefill_hidden, shard);
                let projection_stage_kind = if stage == 5 { 2 } else { 3 };
                let projection = expected_projection_tile(
                    &rmsnorm_hidden,
                    projection_kind,
                    projection_stage_kind,
                    shard,
                );
                let rope = qwen3_dense_0_6b_rope_tile_from_projection(
                    &projection,
                    128,
                    projection_kind,
                    shard,
                );
                assert_eq!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&rope),
                    "RoPE stage checksum must reflect projection-derived RoPE output"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&projection),
                    "RoPE stage checksum must not be the raw projection output"
                );
                rope_stage_checks += 1;
            }
            if stage == 7 {
                let shard = expected_shard;
                let prefill_hidden = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
                    guest_input,
                    None,
                    128 * 128,
                    shard,
                    expected_tile_index,
                );
                let rmsnorm_hidden = expected_rmsnorm_tile(&prefill_hidden, shard);
                let q_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::Q, 2, shard);
                let kv_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::Kv, 3, shard);
                let v_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::V, 4, shard);
                let rope_q = qwen3_dense_0_6b_rope_tile_from_projection(
                    &q_projection,
                    128,
                    Qwen3ProjectionKind::Q,
                    shard,
                );
                let rope_kv = qwen3_dense_0_6b_rope_tile_from_projection(
                    &kv_projection,
                    128,
                    Qwen3ProjectionKind::Kv,
                    shard,
                );
                let attention_score = expected_attention_score_tile(
                    &rope_q,
                    &rope_kv,
                    &kv_projection,
                    &v_projection,
                    shard,
                );
                let attention_score_without_cache =
                    expected_attention_score_without_cache(&rope_q, &rope_kv, shard);
                assert_eq!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&attention_score),
                    "attention score checksum must reflect KV-cache-backed RoPE score output"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&attention_score_without_cache),
                    "attention score checksum must not ignore the KV cache state"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&rope_q),
                    "attention score checksum must not be the raw RoPE Q output"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&rope_kv),
                    "attention score checksum must not be the raw RoPE KV output"
                );
                attention_stage_checks += 1;
            }
            if stage == 8 {
                let shard = expected_shard;
                let prefill_hidden = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
                    guest_input,
                    None,
                    128 * 128,
                    shard,
                    expected_tile_index,
                );
                let rmsnorm_hidden = expected_rmsnorm_tile(&prefill_hidden, shard);
                let q_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::Q, 2, shard);
                let kv_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::Kv, 3, shard);
                let v_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::V, 4, shard);
                let rope_q = qwen3_dense_0_6b_rope_tile_from_projection(
                    &q_projection,
                    128,
                    Qwen3ProjectionKind::Q,
                    shard,
                );
                let rope_kv = qwen3_dense_0_6b_rope_tile_from_projection(
                    &kv_projection,
                    128,
                    Qwen3ProjectionKind::Kv,
                    shard,
                );
                let attention_score = expected_attention_score_tile(
                    &rope_q,
                    &rope_kv,
                    &kv_projection,
                    &v_projection,
                    shard,
                );
                let attention_softmax =
                    qwen3_dense_0_6b_softmax_tile_from_attention_score(&attention_score, 128);
                let first_row_sum: f32 = bytes_to_f32s(&attention_softmax)[..128].iter().sum();
                assert!(
                    (first_row_sum - 1.0).abs() < 0.0001,
                    "softmax first row should sum to 1.0, got {first_row_sum}"
                );
                assert_eq!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&attention_softmax),
                    "softmax checksum must reflect attention-score-derived probability output"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&attention_score),
                    "softmax checksum must not be the raw attention score output"
                );
                softmax_stage_checks += 1;
            }
            if stage == 9 {
                let shard = expected_shard;
                let prefill_hidden = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
                    guest_input,
                    None,
                    128 * 128,
                    shard,
                    expected_tile_index,
                );
                let rmsnorm_hidden = expected_rmsnorm_tile(&prefill_hidden, shard);
                let q_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::Q, 2, shard);
                let kv_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::Kv, 3, shard);
                let v_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::V, 4, shard);
                let rope_q = qwen3_dense_0_6b_rope_tile_from_projection(
                    &q_projection,
                    128,
                    Qwen3ProjectionKind::Q,
                    shard,
                );
                let rope_kv = qwen3_dense_0_6b_rope_tile_from_projection(
                    &kv_projection,
                    128,
                    Qwen3ProjectionKind::Kv,
                    shard,
                );
                let attention_score = expected_attention_score_tile(
                    &rope_q,
                    &rope_kv,
                    &kv_projection,
                    &v_projection,
                    shard,
                );
                let attention_softmax =
                    qwen3_dense_0_6b_softmax_tile_from_attention_score(&attention_score, 128);
                let attention_context = expected_attention_context_tile(
                    &attention_softmax,
                    &kv_projection,
                    &v_projection,
                    shard,
                );
                let attention_context_without_cache = expected_attention_context_without_cache(
                    &attention_softmax,
                    &v_projection,
                    shard,
                );
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<u16>()) as u64,
                    "attention context is a half tile consumed by HostMatmul"
                );
                assert_eq!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&attention_context),
                    "attention context checksum must reflect softmax-consuming-V output"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&attention_context_without_cache),
                    "attention context checksum must not ignore the KV cache state"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&attention_softmax),
                    "attention context checksum must not be the raw softmax output"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&v_projection),
                    "attention context checksum must not be the raw V projection output"
                );
                context_stage_checks += 1;
            }
            if stage == 10 {
                let shard = expected_shard;
                let prefill_hidden = qwen3_dense_0_6b_tile_prefill_hidden_from_guest_payload(
                    guest_input,
                    None,
                    128 * 128,
                    shard,
                    expected_tile_index,
                );
                let rmsnorm_hidden = expected_rmsnorm_tile(&prefill_hidden, shard);
                let q_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::Q, 2, shard);
                let kv_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::Kv, 3, shard);
                let v_projection =
                    expected_projection_tile(&rmsnorm_hidden, Qwen3ProjectionKind::V, 4, shard);
                let rope_q = qwen3_dense_0_6b_rope_tile_from_projection(
                    &q_projection,
                    128,
                    Qwen3ProjectionKind::Q,
                    shard,
                );
                let rope_kv = qwen3_dense_0_6b_rope_tile_from_projection(
                    &kv_projection,
                    128,
                    Qwen3ProjectionKind::Kv,
                    shard,
                );
                let attention_score = expected_attention_score_tile(
                    &rope_q,
                    &rope_kv,
                    &kv_projection,
                    &v_projection,
                    shard,
                );
                let attention_softmax =
                    qwen3_dense_0_6b_softmax_tile_from_attention_score(&attention_score, 128);
                let attention_context = expected_attention_context_tile(
                    &attention_softmax,
                    &kv_projection,
                    &v_projection,
                    shard,
                );
                let mlp_activation = expected_mlp_activation_tile(&attention_context, shard);
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<u16>()) as u64,
                    "MLP activation is a half tile consumed by HostMatmul"
                );
                assert_eq!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&mlp_activation),
                    "MLP activation checksum must reflect attention-context-derived FFN output"
                );
                assert_ne!(
                    checksum,
                    qwen3_dense_0_6b_shard_output_checksum(&attention_context),
                    "MLP activation checksum must not be the raw attention context output"
                );
                mlp_stage_checks += 1;
            }
            if stage == 12 {
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<f32>()) as u64,
                    "MLP output/down projection is a float tile published as the partial result"
                );
                mlp_output_stage_checks += 1;
            }
            if stage == 13 {
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<f32>()) as u64,
                    "residual RMSNorm output is a float tile published as the partial result"
                );
                residual_stage_checks += 1;
            }
            if (14..=16).contains(&stage) {
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<u16>()) as u64,
                    "next-layer Q/KV/V projection is a half tile derived from residual RMSNorm"
                );
                next_layer_projection_stage_checks += 1;
            }
            if stage == 17 || stage == 18 {
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<u16>()) as u64,
                    "next-layer RoPE Q/KV is a half tile derived from next-layer projection"
                );
                next_layer_rope_stage_checks += 1;
            }
            if stage == 19 {
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<f32>()) as u64,
                    "next-layer attention score is a float tile derived from next-layer RoPE"
                );
                next_layer_attention_stage_checks += 1;
            }
            if stage == 20 {
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<f32>()) as u64,
                    "next-layer softmax is a float tile derived from next-layer attention score"
                );
                next_layer_softmax_stage_checks += 1;
            }
            if stage == 21 {
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<u16>()) as u64,
                    "next-layer attention context is a half tile derived from next softmax and next V"
                );
                next_layer_context_stage_checks += 1;
            }
            if stage == 22 {
                assert_eq!(
                    bytes,
                    (128 * 128 * std::mem::size_of::<f32>()) as u64,
                    "next-layer partial result is a float tile derived from next-layer context"
                );
                partial_stage_checks += 1;
            }
        }
        assert_eq!(
            &stage_counts[1..],
            &[
                16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
                16, 16, 16
            ]
        );
        for shard in 0..TILE_COUNT {
            assert_ne!(
                stage_checksums[shard][12], stage_checksums[shard][11],
                "MLP output/down projection checksum must not be the raw MLP intermediate"
            );
            assert_ne!(
                stage_checksums[shard][13], stage_checksums[shard][12],
                "residual RMSNorm checksum must not be the raw MLP down/output"
            );
            assert_eq!(
                stage_depends_on[shard][14], 13,
                "next-layer Q projection must depend on residual RMSNorm"
            );
            assert_eq!(
                stage_depends_on[shard][15], 13,
                "next-layer KV projection must depend on residual RMSNorm"
            );
            assert_eq!(
                stage_depends_on[shard][16], 13,
                "next-layer V projection must depend on residual RMSNorm"
            );
            assert_eq!(
                stage_depends_on[shard][17], 14,
                "next-layer RoPE Q must depend on next-layer Q projection"
            );
            assert_eq!(
                stage_depends_on[shard][18], 15,
                "next-layer RoPE KV must depend on next-layer KV projection"
            );
            assert_eq!(
                stage_depends_on[shard][19], 17,
                "next-layer attention score must depend on next-layer RoPE Q"
            );
            assert_eq!(
                stage_depends_on[shard][20], 19,
                "next-layer softmax must depend on next-layer attention score"
            );
            assert_eq!(
                stage_depends_on[shard][21], 20,
                "next-layer context must depend on next-layer softmax"
            );
            assert_eq!(
                stage_depends_on[shard][22], 21,
                "partial result must depend on next-layer context"
            );
            assert_ne!(
                stage_checksums[shard][14], stage_checksums[shard][13],
                "next-layer Q projection checksum must not be the residual RMSNorm checksum"
            );
            assert_ne!(
                stage_checksums[shard][15], stage_checksums[shard][13],
                "next-layer KV projection checksum must not be the residual RMSNorm checksum"
            );
            assert_ne!(
                stage_checksums[shard][16], stage_checksums[shard][13],
                "next-layer V projection checksum must not be the residual RMSNorm checksum"
            );
            assert_ne!(
                stage_checksums[shard][14], stage_checksums[shard][15],
                "next-layer Q/KV projection checksums must be distinct"
            );
            assert_ne!(
                stage_checksums[shard][15], stage_checksums[shard][16],
                "next-layer KV/V projection checksums must be distinct"
            );
            assert_ne!(
                stage_checksums[shard][17], stage_checksums[shard][14],
                "next-layer RoPE Q checksum must not be the raw next-layer Q projection"
            );
            assert_ne!(
                stage_checksums[shard][18], stage_checksums[shard][15],
                "next-layer RoPE KV checksum must not be the raw next-layer KV projection"
            );
            assert_ne!(
                stage_checksums[shard][19], stage_checksums[shard][17],
                "next-layer attention score checksum must not be the raw next-layer RoPE Q"
            );
            assert_ne!(
                stage_checksums[shard][20], stage_checksums[shard][19],
                "next-layer softmax checksum must not be the raw next-layer attention score"
            );
            assert_ne!(
                stage_checksums[shard][21], stage_checksums[shard][20],
                "next-layer context checksum must not be the raw next-layer softmax"
            );
            assert_ne!(
                stage_checksums[shard][21], stage_checksums[shard][16],
                "next-layer context checksum must not be the raw next-layer V projection"
            );
            assert_ne!(
                stage_checksums[shard][22], stage_checksums[shard][21],
                "partial result checksum must not be the raw next-layer context"
            );
            assert_eq!(
                stage_depends_on[shard][23], 22,
                "remote resolve must depend on the published partial result"
            );
            assert_eq!(
                stage_depends_on[shard][24], 23,
                "round1 output must depend on remote resolve"
            );
        }
        assert_eq!(rmsnorm_stage_checks, TILE_COUNT);
        assert_eq!(projection_stage_checks, TILE_COUNT * 3);
        assert_eq!(rope_stage_checks, TILE_COUNT * 2);
        assert_eq!(attention_stage_checks, TILE_COUNT);
        assert_eq!(softmax_stage_checks, TILE_COUNT);
        assert_eq!(context_stage_checks, TILE_COUNT);
        assert_eq!(mlp_stage_checks, TILE_COUNT);
        assert_eq!(mlp_output_stage_checks, TILE_COUNT);
        assert_eq!(residual_stage_checks, TILE_COUNT);
        assert_eq!(next_layer_projection_stage_checks, TILE_COUNT * 3);
        assert_eq!(next_layer_rope_stage_checks, TILE_COUNT * 2);
        assert_eq!(next_layer_attention_stage_checks, TILE_COUNT);
        assert_eq!(next_layer_softmax_stage_checks, TILE_COUNT);
        assert_eq!(next_layer_context_stage_checks, TILE_COUNT);
        assert_eq!(partial_stage_checks, TILE_COUNT);
    }

    const VALID_YAML: &str = r#"
scenario:
  name: mvp_2host_single_domain
  group: M
  variant: m_single_domain_mvp
  seed: 42
  duration_us: 1000000
  logical_system: llm-serving-mvp
platform:
  backend: qemu
  machine_profile: ub-host-minimal
  cpu_model: host
  memory_model: numa-sim
  device_model_mode: mixed
topology:
  hosts: 2
  ubpus_per_host: 2
  entities_per_ubpu: 2
  ub_domains:
    - id: domain0
      hosts: [0, 1]
  collapse:
    fabric: true
    global: true
ub_runtime:
  active_levels: [2, 3, 4]
  reserved_levels: [0, 1, 5, 6, 7]
  preserve_full_task_coord: true
pypto:
  enable_function_labels: true
  default_level: HOST
  allow_levels: [CHIP, HOST, CLUSTER_0]
  simpler_boundary:
    enabled: true
    chip_backend_mode: stub
    dispatch_latency_us: 15
  scope_runtime:
    enable_multi_layer_ring: true
    enable_pl_free: true
    max_scope_depth: 8
lingqu_data:
  shmem:
    enabled: true
    pe_count: 2
    default_latency_us: 3
  block:
    enabled: true
    devices:
      - uba: ssu0
        blocks: 1048576
        block_size: 4096
  dfs:
    enabled: true
    namespace_root: /
    metadata_latency_us: 20
    data_latency_us: 80
  db:
    enabled: true
    inline_value_limit: 64
    pipeline_batch_limit: 16
levels:
  l2_ubpu_tier:
    capacity_blocks: 1024
    high_watermark: 0.9
    low_watermark: 0.7
    hit_latency_us: 5
  l3_host_tier:
    capacity_blocks: 8192
    high_watermark: 0.9
    low_watermark: 0.7
    fetch_latency_us: 30
  l4_domain_tier:
    capacity_blocks: 65536
    high_watermark: 0.95
    low_watermark: 0.8
    fetch_latency_us: 80
routing:
  mode: recursive
  hit_weight: 10.0
  load_weight: 2.0
  capacity_weight: 1.0
workload:
  type: rust_llm_server_mvp
  profile: single_domain_basic
  qps: 2000
  unique_prefixes: 256
  blocks_per_request: 4
  function_label_mode: host_orchestration
faults:
  - type: host_degraded
    at_us: 300000
    host_id: 0
outputs:
  trace: true
  metrics_csv: true
  summary_json: true
  emit_task_coord_trace: true
  emit_data_service_trace: true
  emit_qemu_platform_trace: true
"#;

    fn test_topology() -> SimTopology {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        SimTopology::from_config(&config).expect("topology")
    }

    fn test_surface() -> LocalGuestUapiSurface {
        LocalGuestUapiSurface::new(test_topology())
    }

    #[test]
    fn qwen3_decode_duration_label_uses_single_decimal_seconds() {
        assert_eq!(
            qwen3_dense_0_6b_decode_duration_label(Duration::from_millis(3_240)),
            "3.2 seconds"
        );
    }

    #[test]
    fn local_guest_uapi_can_submit_write_and_drain_completion() {
        let mut surface = test_surface();
        let cq = surface.register_cq().expect("register cq");
        let segment = surface.create_segment(4096).expect("create segment");

        surface
            .submit_io(IoSubmitReq {
                op_id: 11,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: Some(segment),
                block: Some(BlockHash("block-1".into())),
            })
            .expect("submit write");

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].status, CompletionStatus::Success);
    }

    #[test]
    fn local_guest_uapi_command_model_executes_round_trip() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };

        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        match surface
            .execute(UapiCommand::SubmitIo {
                req: IoSubmitReq {
                    op_id: 12,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("block-2".into())),
                },
            })
            .expect("submit io")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::DrainCq { cq, owner: 0 })
            .expect("drain cq")
        {
            UapiResponse::Completions { events, remaining } => {
                assert_eq!(events.len(), 1);
                assert_eq!(remaining, 0);
                assert_eq!(events[0].status, CompletionStatus::Success);
            }
            other => panic!("unexpected response: {other:?}"),
        }
    }

    #[test]
    fn local_guest_uapi_read_block_miss_reaches_block_service() {
        let mut surface = test_surface();
        let cq = surface.register_cq().expect("register cq");
        let segment = surface.create_segment(4096).expect("create segment");

        surface
            .submit_io(IoSubmitReq {
                op_id: 13,
                task: None,
                entity: 0,
                opcode: IoOpcode::ReadBlock,
                segment: Some(segment),
                block: Some(BlockHash("missing-block".into())),
            })
            .expect("submit read miss");

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 1);
        assert_eq!(
            events[0].status,
            CompletionStatus::RetryableFailure {
                code: "block_miss".to_string()
            }
        );
    }

    #[test]
    fn local_guest_uapi_supports_shmem_dfs_and_db_commands() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        match surface
            .execute(UapiCommand::SubmitShmemPut {
                req: ShmemPutReq {
                    task: None,
                    requester_entity: 0,
                    segment,
                    bytes: 4096,
                },
            })
            .expect("shmem put")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitDfsWrite {
                req: DfsWriteReq {
                    task: None,
                    path: "/weights/l0.bin".into(),
                    bytes: 8192,
                },
            })
            .expect("dfs write")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitDfsRead {
                req: DfsReadReq {
                    task: None,
                    path: "/weights/l0.bin".into(),
                },
            })
            .expect("dfs read")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitShmemGet {
                req: ShmemGetReq {
                    task: None,
                    requester_entity: 0,
                    segment,
                    bytes: 4096,
                },
            })
            .expect("shmem get")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitDbPut {
                req: DbPutReq {
                    task: None,
                    key: "weights:layer0".into(),
                    bytes: 128,
                },
            })
            .expect("db put")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::SubmitDbGet {
                req: DbGetReq {
                    task: None,
                    key: "weights:layer0".into(),
                },
            })
            .expect("db get")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 6);
        assert!(events
            .iter()
            .any(|event| event.source == sim_core::CompletionSource::ShmemService));
        assert!(events
            .iter()
            .any(|event| event.source == sim_core::CompletionSource::DfsService));
        assert!(events
            .iter()
            .any(|event| event.source == sim_core::CompletionSource::DbService));
    }

    #[test]
    fn local_guest_uapi_supports_lingqu_object_publish_and_resolve_commands() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let metadata = LingquObjectMetadata {
            bytes: 2048,
            checksum: 0x1234,
            dtype: None,
            shape: vec![1, 2048],
            layout: None,
            expires_at_us: None,
        };
        let placement = LingquPayloadPlacement {
            backend: LingquPayloadBackend::Shmem,
            storage_ref: "qwen3/runtime/session0/hidden0".to_string(),
            segment: Some(sim_core::SegmentHandle(101)),
            offset: 0,
            bytes: 2048,
            checksum: 0x1234,
            locality: LingquObjectLocality::DomainShared(0),
        };
        let key = "qwen3/session/s0/hidden/boundary/node/0/to/1/step/0".to_string();
        match surface
            .execute(UapiCommand::SubmitObjectPublish {
                req: LingquObjectPublishReq {
                    task: None,
                    key: key.clone(),
                    kind: LingquObjectKind::RuntimeTensor,
                    producer_entity: 0,
                    owner_entity: Some(1),
                    expected_version: None,
                    metadata,
                    placements: vec![placement],
                    payload_bytes: 0x1234u64.to_le_bytes().to_vec(),
                },
            })
            .expect("object publish")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }
        match surface
            .execute(UapiCommand::SubmitObjectResolve {
                req: LingquObjectResolveReq {
                    task: None,
                    key,
                    requester_entity: 1,
                    version: LingquObjectVersionSelector::LatestCommitted,
                    min_state: LingquObjectState::Committed,
                    preferred_backends: vec![LingquPayloadBackend::Shmem],
                },
            })
            .expect("object resolve")
        {
            UapiResponse::IoSubmitted(_) => {}
            other => panic!("unexpected response: {other:?}"),
        }

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 2);
        assert!(events
            .iter()
            .all(|event| event.source == sim_core::CompletionSource::DbService));
        assert!(events
            .iter()
            .all(|event| event.status == CompletionStatus::Success));
    }

    #[test]
    fn local_guest_uapi_supports_block_writeback_command() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        surface
            .execute(UapiCommand::SubmitIo {
                req: IoSubmitReq {
                    op_id: 13,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("wb-block".into())),
                },
            })
            .expect("submit write");
        surface
            .execute(UapiCommand::SubmitBlockWriteback {
                block: BlockHash("wb-block".into()),
                task: None,
            })
            .expect("submit writeback");

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 2);
        assert!(events
            .iter()
            .all(|event| event.status == CompletionStatus::Success));
    }

    #[test]
    fn local_guest_uapi_can_surface_block_queue_pressure() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut surface = LocalGuestUapiSurface::with_block_profile(
            topology,
            BlockServiceProfile {
                queue_depth: 1,
                ..BlockServiceProfile::default()
            },
        );
        let _cq = surface.register_cq().expect("register cq");
        let segment = surface.create_segment(4096).expect("create segment");

        surface
            .submit_io(IoSubmitReq {
                op_id: 21,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: Some(segment),
                block: Some(BlockHash("queue-0".into())),
            })
            .expect("first write should succeed");

        let err = surface
            .submit_io(IoSubmitReq {
                op_id: 22,
                task: None,
                entity: 0,
                opcode: IoOpcode::WriteBlock,
                segment: Some(segment),
                block: Some(BlockHash("queue-1".into())),
            })
            .expect_err("second write should hit queue pressure");
        assert!(matches!(err, SimError::InvalidInput("block queue full")));
    }

    #[test]
    fn local_guest_uapi_supports_command_queue_and_doorbell() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 4,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };

        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        match surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::Io(IoSubmitReq {
                    op_id: 30,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::WriteBlock,
                    segment: Some(segment),
                    block: Some(BlockHash("cmdq-block".into())),
                }),
            })
            .expect("enqueue write")
        {
            UapiResponse::CommandEnqueued { depth: 1, .. } => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::RingDoorbell {
                cmdq,
                owner: 0,
                max_batch: Some(1),
            })
            .expect("doorbell")
        {
            UapiResponse::DoorbellRung {
                submitted: 1,
                pending: 0,
            } => {}
            other => panic!("unexpected response: {other:?}"),
        }

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].source, sim_core::CompletionSource::BlockService);
    }

    #[test]
    fn local_guest_uapi_dispatch_completes_as_chipbackend() {
        let mut surface = test_surface();
        let cq = surface.register_cq().expect("register cq");
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 4,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };
        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        let _ = surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::Io(IoSubmitReq {
                    op_id: 31,
                    task: None,
                    entity: 0,
                    opcode: IoOpcode::Dispatch,
                    segment: Some(segment),
                    block: None,
                }),
            })
            .expect("enqueue dispatch");
        let _ = surface
            .execute(UapiCommand::RingDoorbell {
                cmdq,
                owner: 0,
                max_batch: Some(1),
            })
            .expect("doorbell");

        let events = surface.drain_cq(cq);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].source, sim_core::CompletionSource::ChipBackend);
    }

    #[test]
    fn host_vector_dispatch_accepts_w4_seed_payload() {
        run_simpler_native_test_isolated("host_vector_dispatch_accepts_w4_seed_payload", || {
            let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
            let topology = SimTopology::from_config(&config).expect("topology");
            let guest_input = vec![0u8; 4096];
            let task = TaskKey {
                logical_system: LogicalSystemId(1),
                coord: HierarchyCoord { levels: [0; 8] },
                scope_depth: 0,
                task_id: 31,
            };
            let output = super::run_host_vector_chipbackend(&topology, &task, &guest_input)
                .expect("dispatch");
            assert_eq!(&output[..8], &0x41a0000041a00000u64.to_le_bytes());
        });
    }

    #[test]
    fn local_guest_uapi_command_queue_enforces_depth() {
        let mut surface = test_surface();
        let cq = surface.register_cq().expect("register cq");
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 1,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };

        surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::DbPut(DbPutReq {
                    task: None,
                    key: "one".into(),
                    bytes: 16,
                }),
            })
            .expect("first enqueue");

        let err = surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::DbPut(DbPutReq {
                    task: None,
                    key: "two".into(),
                    bytes: 16,
                }),
            })
            .expect_err("depth should be enforced");
        assert!(matches!(err, SimError::InvalidInput("command queue full")));
    }

    #[test]
    fn local_guest_uapi_partial_poll_preserves_remaining_entries() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 4,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };
        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        for (op_id, opcode) in [(40, IoOpcode::WriteBlock), (41, IoOpcode::ReadBlock)] {
            match surface
                .execute(UapiCommand::EnqueueCmd {
                    cmdq,
                    owner: 0,
                    desc: UapiDescriptor::Io(IoSubmitReq {
                        op_id,
                        task: None,
                        entity: 0,
                        opcode,
                        segment: Some(segment),
                        block: Some(BlockHash("partial-poll-block".into())),
                    }),
                })
                .expect("enqueue io")
            {
                UapiResponse::CommandEnqueued { .. } => {}
                other => panic!("unexpected response: {other:?}"),
            }
        }

        match surface
            .execute(UapiCommand::RingDoorbell {
                cmdq,
                owner: 0,
                max_batch: None,
            })
            .expect("ring doorbell")
        {
            UapiResponse::DoorbellRung {
                submitted: 2,
                pending: 0,
            } => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::PollCq {
                cq,
                owner: 0,
                max_entries: Some(1),
            })
            .expect("partial poll")
        {
            UapiResponse::Completions { events, remaining } => {
                assert_eq!(events.len(), 1);
                assert_eq!(remaining, 1);
            }
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::DrainCq { cq, owner: 0 })
            .expect("drain cq")
        {
            UapiResponse::Completions { events, remaining } => {
                assert_eq!(events.len(), 1);
                assert_eq!(remaining, 0);
            }
            other => panic!("unexpected response: {other:?}"),
        }
    }

    #[test]
    fn local_guest_uapi_enforces_cq_and_cmdq_ownership() {
        let mut surface = test_surface();
        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 7 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 7,
                depth: 2,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };

        let enqueue_err = surface
            .execute(UapiCommand::EnqueueCmd {
                cmdq,
                owner: 0,
                desc: UapiDescriptor::DbPut(DbPutReq {
                    task: None,
                    key: "owner-mismatch".into(),
                    bytes: 32,
                }),
            })
            .expect_err("owner mismatch should fail");
        assert!(matches!(
            enqueue_err,
            SimError::InvalidInput("command queue owner mismatch")
        ));

        let poll_err = surface
            .execute(UapiCommand::PollCq {
                cq,
                owner: 0,
                max_entries: Some(1),
            })
            .expect_err("cq owner mismatch should fail");
        assert!(matches!(
            poll_err,
            SimError::InvalidInput("completion queue owner mismatch")
        ));
    }

    #[test]
    fn local_guest_uapi_retries_service_submission_after_runtime_backpressure() {
        let config = ScenarioConfig::from_yaml_str(VALID_YAML).expect("valid config");
        let topology = SimTopology::from_config(&config).expect("topology");
        let mut surface = LocalGuestUapiSurface::with_service_profiles(
            topology,
            BlockServiceProfile::default(),
            ShmemServiceProfile {
                queue_depth: 1,
                ..ShmemServiceProfile::default()
            },
            DfsServiceProfile::default(),
            DbServiceProfile::default(),
        );

        let cq = match surface
            .execute(UapiCommand::RegisterCq { owner: 0 })
            .expect("register cq")
        {
            UapiResponse::CqRegistered(cq) => cq,
            other => panic!("unexpected response: {other:?}"),
        };
        let cmdq = match surface
            .execute(UapiCommand::CreateCmdQueue {
                cq,
                owner: 0,
                depth: 4,
            })
            .expect("create cmdq")
        {
            UapiResponse::CmdQueueCreated(cmdq) => cmdq,
            other => panic!("unexpected response: {other:?}"),
        };
        let segment = match surface
            .execute(UapiCommand::CreateSegment { bytes: 4096 })
            .expect("create segment")
        {
            UapiResponse::SegmentCreated(segment) => segment,
            other => panic!("unexpected response: {other:?}"),
        };

        for _ in 0..2 {
            match surface
                .execute(UapiCommand::EnqueueCmd {
                    cmdq,
                    owner: 0,
                    desc: UapiDescriptor::ShmemPut(ShmemPutReq {
                        task: None,
                        requester_entity: 0,
                        segment,
                        bytes: 1024,
                    }),
                })
                .expect("enqueue put")
            {
                UapiResponse::CommandEnqueued { .. } => {}
                other => panic!("unexpected response: {other:?}"),
            }
        }

        match surface
            .execute(UapiCommand::RingDoorbell {
                cmdq,
                owner: 0,
                max_batch: None,
            })
            .expect("ring doorbell")
        {
            UapiResponse::DoorbellRung {
                submitted: 2,
                pending: 0,
            } => {}
            other => panic!("unexpected response: {other:?}"),
        }

        match surface
            .execute(UapiCommand::DrainCq { cq, owner: 0 })
            .expect("drain cq")
        {
            UapiResponse::Completions { events, remaining } => {
                assert_eq!(remaining, 0);
                assert_eq!(events.len(), 2);
                assert!(events
                    .iter()
                    .all(|event| event.source == sim_core::CompletionSource::ShmemService));
            }
            other => panic!("unexpected response: {other:?}"),
        }
    }
}
