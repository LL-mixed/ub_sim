use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use serde::{Deserialize, Serialize};
use sim_core::{SegmentHandle, TaskKey};
use sim_services::weights::{
    WeightMetadataPut, WeightPayloadWrite, WeightStorageKind, WeightsLoadReq,
};
use sim_topology::SimTopology;

#[derive(Clone, Copy, Debug)]
pub struct Qwen3Dense06bProfile {
    pub vocab_size: u64,
    pub hidden_size: u64,
    pub intermediate_size: u64,
    pub num_hidden_layers: u64,
    pub num_attention_heads: u64,
    pub num_key_value_heads: u64,
    pub head_dim: u64,
    pub max_position_embeddings: u64,
    pub rope_theta: u64,
    pub prefill_tokens: u64,
    pub decode_tokens: u64,
    pub tp_nodes: u64,
}

pub const QWEN3_DENSE_0_6B_PROFILE: Qwen3Dense06bProfile = Qwen3Dense06bProfile {
    vocab_size: 151_936,
    hidden_size: 1_024,
    intermediate_size: 3_072,
    num_hidden_layers: 28,
    num_attention_heads: 16,
    num_key_value_heads: 8,
    head_dim: 128,
    max_position_embeddings: 40_960,
    rope_theta: 1_000_000,
    prefill_tokens: 128,
    decode_tokens: 1,
    tp_nodes: 8,
};

#[derive(Clone, Copy, Debug)]
pub struct Qwen3Dense06bShard {
    pub shard_id: u64,
    pub owner_node: u64,
    pub target_node: u64,
    pub head_start: u64,
    pub head_end: u64,
    pub kv_block_start: u64,
    pub kv_block_end: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Qwen3Dense06bLayerOpKind {
    RmsNorm,
    QkvProjection,
    Rope,
    AttentionScore,
    AttentionSoftmax,
    AttentionValue,
    OProjection,
    AttentionResidualAdd,
    MlpUpGateProjection,
    MlpSwiGlu,
    MlpDownProjection,
    MlpResidualAdd,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Qwen3Dense06bLoweringKind {
    TiledMatmul,
    MissingRmsNorm,
    MissingRope,
    MissingSoftmax,
    MissingSiluSwiglu,
    MissingAdd,
    MissingCollective,
}

#[derive(Clone, Copy, Debug)]
pub struct Qwen3Dense06bLayerOp {
    pub kind: Qwen3Dense06bLayerOpKind,
    pub input_width: u64,
    pub output_width: u64,
    pub sharded: bool,
    pub requires_collective: bool,
    pub lowering: Qwen3Dense06bLoweringKind,
}

#[derive(Clone, Debug)]
pub struct Qwen3Dense06bLayerGraphIr {
    pub layer_id: u64,
    pub hidden_size: u64,
    pub intermediate_size: u64,
    pub attention_heads: u64,
    pub kv_heads: u64,
    pub head_dim: u64,
    pub prefill_tokens: u64,
    pub ops: Vec<Qwen3Dense06bLayerOp>,
}

#[derive(Clone, Copy, Debug)]
pub struct Qwen3Dense06bTensorParallelShard {
    pub shard_id: u64,
    pub owner_node: u64,
    pub target_node: u64,
    pub q_head_start: u64,
    pub q_head_end: u64,
    pub kv_head_start: u64,
    pub kv_head_end: u64,
    pub local_q_heads: u64,
    pub local_kv_heads: u64,
    pub local_q_width: u64,
    pub local_kv_width: u64,
    pub local_o_input_width: u64,
    pub local_mlp_intermediate_width: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bLoweringSummary {
    pub tiled_matmul_ops: usize,
    pub missing_ops: usize,
    pub missing_rmsnorm: usize,
    pub missing_rope: usize,
    pub missing_softmax: usize,
    pub missing_silu_swiglu: usize,
    pub missing_add: usize,
    pub missing_collective: usize,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub enum Qwen3Dense06bWeightDType {
    F32,
    F16,
    BF16,
    I8,
    U8,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub enum Qwen3Dense06bWeightTensorKind {
    InputLayerNorm,
    QProj,
    KProj,
    VProj,
    OProj,
    PostAttentionLayerNorm,
    GateProj,
    UpProj,
    DownProj,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub enum Qwen3Dense06bWeightStorageKind {
    Block,
    Shmem,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bWeightTensorMetadata {
    pub dtype: Qwen3Dense06bWeightDType,
    pub shape: Vec<u64>,
    pub data_offsets: Option<[u64; 2]>,
    #[serde(default)]
    pub source_file: Option<String>,
    #[serde(default)]
    pub data_base_offset: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bWeightStorageRef {
    pub kind: Qwen3Dense06bWeightStorageKind,
    pub storage_ref: String,
    pub segment: u64,
    pub offset: u64,
    pub bytes: u64,
    pub checksum: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bWeightSlice {
    pub layer_id: u64,
    pub shard_id: u64,
    pub tensor_kind: Qwen3Dense06bWeightTensorKind,
    pub tensor_name: String,
    pub dtype: Qwen3Dense06bWeightDType,
    pub global_shape: Vec<u64>,
    pub slice_axis: Option<u64>,
    pub slice_start: u64,
    pub slice_end: u64,
    pub local_shape: Vec<u64>,
    pub storage: Qwen3Dense06bWeightStorageRef,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bWeightManifest {
    pub model_id: String,
    pub source: String,
    pub format: String,
    pub profile: Qwen3Dense06bWeightManifestProfile,
    pub slices: Vec<Qwen3Dense06bWeightSlice>,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bWeightManifestProfile {
    pub hidden_size: u64,
    pub intermediate_size: u64,
    pub num_hidden_layers: u64,
    pub num_attention_heads: u64,
    pub num_key_value_heads: u64,
    pub head_dim: u64,
    pub tp_nodes: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bWeightDbPut {
    pub key: String,
    pub bytes: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bWeightPayloadWrite {
    pub storage_ref: String,
    pub storage_kind: Qwen3Dense06bWeightStorageKind,
    pub segment: u64,
    pub offset: u64,
    pub bytes: u64,
    pub checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bWeightServiceLoadPlan {
    pub manifest: Qwen3Dense06bWeightManifest,
    pub metadata_db_puts: Vec<Qwen3Dense06bWeightDbPut>,
    pub payload_writes: Vec<Qwen3Dense06bWeightPayloadWrite>,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bSafetensorsIndex {
    pub metadata: Option<BTreeMap<String, serde_json::Value>>,
    pub weight_map: BTreeMap<String, String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bLoadedWeights {
    pub source: String,
    pub tensors: BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
}

pub fn shard_plan(
    topology: &SimTopology,
    profile: Qwen3Dense06bProfile,
) -> Result<Vec<Qwen3Dense06bShard>, String> {
    let shard_count = profile.tp_nodes as usize;
    if shard_count == 0 {
        return Err("qwen3_dense_0_6b_empty_shard_plan".to_string());
    }
    let heads_per_shard = profile
        .num_attention_heads
        .checked_div(profile.tp_nodes)
        .ok_or_else(|| "qwen3_dense_0_6b_invalid_tp_nodes".to_string())?;
    if heads_per_shard == 0 || heads_per_shard * profile.tp_nodes != profile.num_attention_heads {
        return Err(format!(
            "qwen3_dense_0_6b_heads_not_divisible:heads={}:tp={}",
            profile.num_attention_heads, profile.tp_nodes
        ));
    }
    let hosts: Vec<u64> = topology.hosts.iter().map(|host| host.node_id).collect();
    let ubpus: Vec<u64> = topology.ubpus.iter().map(|ubpu| ubpu.node_id).collect();
    if hosts.is_empty() || ubpus.is_empty() {
        return Err("qwen3_dense_0_6b_missing_topology_nodes".to_string());
    }
    let mut shards = Vec::with_capacity(shard_count);
    for shard_index in 0..shard_count {
        let shard_id = shard_index as u64;
        shards.push(Qwen3Dense06bShard {
            shard_id,
            owner_node: hosts[shard_index % hosts.len()],
            target_node: ubpus[shard_index % ubpus.len()],
            head_start: shard_id * heads_per_shard,
            head_end: (shard_id + 1) * heads_per_shard,
            kv_block_start: shard_id * 2,
            kv_block_end: shard_id * 2 + 2,
        });
    }
    Ok(shards)
}

pub fn layer_graph_ir(
    profile: Qwen3Dense06bProfile,
    layer_id: u64,
) -> Qwen3Dense06bLayerGraphIr {
    Qwen3Dense06bLayerGraphIr {
        layer_id,
        hidden_size: profile.hidden_size,
        intermediate_size: profile.intermediate_size,
        attention_heads: profile.num_attention_heads,
        kv_heads: profile.num_key_value_heads,
        head_dim: profile.head_dim,
        prefill_tokens: profile.prefill_tokens,
        ops: vec![
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::RmsNorm,
                input_width: profile.hidden_size,
                output_width: profile.hidden_size,
                sharded: false,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::MissingRmsNorm,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::QkvProjection,
                input_width: profile.hidden_size,
                output_width: (profile.num_attention_heads + profile.num_key_value_heads * 2)
                    * profile.head_dim,
                sharded: true,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::TiledMatmul,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::Rope,
                input_width: (profile.num_attention_heads + profile.num_key_value_heads)
                    * profile.head_dim,
                output_width: (profile.num_attention_heads + profile.num_key_value_heads)
                    * profile.head_dim,
                sharded: true,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::MissingRope,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::AttentionScore,
                input_width: profile.num_attention_heads * profile.head_dim,
                output_width: profile.num_attention_heads * profile.prefill_tokens,
                sharded: true,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::TiledMatmul,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::AttentionSoftmax,
                input_width: profile.num_attention_heads * profile.prefill_tokens,
                output_width: profile.num_attention_heads * profile.prefill_tokens,
                sharded: true,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::MissingSoftmax,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::AttentionValue,
                input_width: profile.num_attention_heads * profile.prefill_tokens,
                output_width: profile.num_attention_heads * profile.head_dim,
                sharded: true,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::TiledMatmul,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::OProjection,
                input_width: profile.num_attention_heads * profile.head_dim,
                output_width: profile.hidden_size,
                sharded: true,
                requires_collective: true,
                lowering: Qwen3Dense06bLoweringKind::MissingCollective,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::AttentionResidualAdd,
                input_width: profile.hidden_size,
                output_width: profile.hidden_size,
                sharded: false,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::MissingAdd,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::MlpUpGateProjection,
                input_width: profile.hidden_size,
                output_width: profile.intermediate_size * 2,
                sharded: true,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::TiledMatmul,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::MlpSwiGlu,
                input_width: profile.intermediate_size * 2,
                output_width: profile.intermediate_size,
                sharded: true,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::MissingSiluSwiglu,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::MlpDownProjection,
                input_width: profile.intermediate_size,
                output_width: profile.hidden_size,
                sharded: true,
                requires_collective: true,
                lowering: Qwen3Dense06bLoweringKind::MissingCollective,
            },
            Qwen3Dense06bLayerOp {
                kind: Qwen3Dense06bLayerOpKind::MlpResidualAdd,
                input_width: profile.hidden_size,
                output_width: profile.hidden_size,
                sharded: false,
                requires_collective: false,
                lowering: Qwen3Dense06bLoweringKind::MissingAdd,
            },
        ],
    }
}

pub fn tensor_parallel_plan(
    topology: &SimTopology,
    profile: Qwen3Dense06bProfile,
) -> Result<Vec<Qwen3Dense06bTensorParallelShard>, String> {
    let shards = shard_plan(topology, profile)?;
    if profile.num_key_value_heads % profile.tp_nodes != 0 {
        return Err(format!(
            "qwen3_dense_0_6b_kv_heads_not_divisible:kv_heads={}:tp={}",
            profile.num_key_value_heads, profile.tp_nodes
        ));
    }
    if profile.intermediate_size % profile.tp_nodes != 0 {
        return Err(format!(
            "qwen3_dense_0_6b_mlp_intermediate_not_divisible:intermediate={}:tp={}",
            profile.intermediate_size, profile.tp_nodes
        ));
    }
    let kv_heads_per_shard = profile.num_key_value_heads / profile.tp_nodes;
    let mlp_intermediate_per_shard = profile.intermediate_size / profile.tp_nodes;
    Ok(shards
        .into_iter()
        .map(|shard| {
            let local_q_heads = shard.head_end - shard.head_start;
            let kv_head_start = shard.shard_id * kv_heads_per_shard;
            let kv_head_end = kv_head_start + kv_heads_per_shard;
            Qwen3Dense06bTensorParallelShard {
                shard_id: shard.shard_id,
                owner_node: shard.owner_node,
                target_node: shard.target_node,
                q_head_start: shard.head_start,
                q_head_end: shard.head_end,
                kv_head_start,
                kv_head_end,
                local_q_heads,
                local_kv_heads: kv_heads_per_shard,
                local_q_width: local_q_heads * profile.head_dim,
                local_kv_width: kv_heads_per_shard * profile.head_dim,
                local_o_input_width: local_q_heads * profile.head_dim,
                local_mlp_intermediate_width: mlp_intermediate_per_shard,
            }
        })
        .collect())
}

pub fn lowering_summary(graph: &Qwen3Dense06bLayerGraphIr) -> Qwen3Dense06bLoweringSummary {
    let mut summary = Qwen3Dense06bLoweringSummary {
        tiled_matmul_ops: 0,
        missing_ops: 0,
        missing_rmsnorm: 0,
        missing_rope: 0,
        missing_softmax: 0,
        missing_silu_swiglu: 0,
        missing_add: 0,
        missing_collective: 0,
    };
    for op in graph.ops.iter() {
        match op.lowering {
            Qwen3Dense06bLoweringKind::TiledMatmul => summary.tiled_matmul_ops += 1,
            Qwen3Dense06bLoweringKind::MissingRmsNorm => {
                summary.missing_ops += 1;
                summary.missing_rmsnorm += 1;
            }
            Qwen3Dense06bLoweringKind::MissingRope => {
                summary.missing_ops += 1;
                summary.missing_rope += 1;
            }
            Qwen3Dense06bLoweringKind::MissingSoftmax => {
                summary.missing_ops += 1;
                summary.missing_softmax += 1;
            }
            Qwen3Dense06bLoweringKind::MissingSiluSwiglu => {
                summary.missing_ops += 1;
                summary.missing_silu_swiglu += 1;
            }
            Qwen3Dense06bLoweringKind::MissingAdd => {
                summary.missing_ops += 1;
                summary.missing_add += 1;
            }
            Qwen3Dense06bLoweringKind::MissingCollective => {
                summary.missing_ops += 1;
                summary.missing_collective += 1;
            }
        }
    }
    summary
}

pub fn profile_from_config_json(config_json: &str) -> Result<Qwen3Dense06bProfile, String> {
    let value: serde_json::Value = serde_json::from_str(config_json)
        .map_err(|err| format!("qwen3_config_json_parse_failed:{err}"))?;
    let get_u64 = |key: &str| -> Result<u64, String> {
        value
            .get(key)
            .and_then(serde_json::Value::as_u64)
            .ok_or_else(|| format!("qwen3_config_missing_u64:{key}"))
    };
    let profile = Qwen3Dense06bProfile {
        vocab_size: get_u64("vocab_size")?,
        hidden_size: get_u64("hidden_size")?,
        intermediate_size: get_u64("intermediate_size")?,
        num_hidden_layers: get_u64("num_hidden_layers")?,
        num_attention_heads: get_u64("num_attention_heads")?,
        num_key_value_heads: get_u64("num_key_value_heads")?,
        head_dim: get_u64("head_dim")?,
        max_position_embeddings: get_u64("max_position_embeddings")?,
        rope_theta: get_u64("rope_theta")?,
        prefill_tokens: QWEN3_DENSE_0_6B_PROFILE.prefill_tokens,
        decode_tokens: QWEN3_DENSE_0_6B_PROFILE.decode_tokens,
        tp_nodes: QWEN3_DENSE_0_6B_PROFILE.tp_nodes,
    };
    validate_profile(profile)?;
    Ok(profile)
}

pub fn validate_profile(profile: Qwen3Dense06bProfile) -> Result<(), String> {
    let expected = QWEN3_DENSE_0_6B_PROFILE;
    let checks = [
        ("vocab_size", profile.vocab_size, expected.vocab_size),
        ("hidden_size", profile.hidden_size, expected.hidden_size),
        (
            "intermediate_size",
            profile.intermediate_size,
            expected.intermediate_size,
        ),
        (
            "num_hidden_layers",
            profile.num_hidden_layers,
            expected.num_hidden_layers,
        ),
        (
            "num_attention_heads",
            profile.num_attention_heads,
            expected.num_attention_heads,
        ),
        (
            "num_key_value_heads",
            profile.num_key_value_heads,
            expected.num_key_value_heads,
        ),
        ("head_dim", profile.head_dim, expected.head_dim),
        (
            "max_position_embeddings",
            profile.max_position_embeddings,
            expected.max_position_embeddings,
        ),
        ("rope_theta", profile.rope_theta, expected.rope_theta),
    ];
    for (name, got, expected_value) in checks {
        if got != expected_value {
            return Err(format!(
                "qwen3_dense_0_6b_config_mismatch:{name}:got={got}:expected={expected_value}"
            ));
        }
    }
    Ok(())
}

pub fn parse_safetensors_metadata_json(
    metadata_json: &str,
) -> Result<BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>, String> {
    let value: serde_json::Value = serde_json::from_str(metadata_json)
        .map_err(|err| format!("qwen3_safetensors_metadata_parse_failed:{err}"))?;
    let object = value
        .as_object()
        .ok_or_else(|| "qwen3_safetensors_metadata_not_object".to_string())?;
    let mut tensors = BTreeMap::new();
    for (name, tensor) in object {
        if name == "__metadata__" {
            continue;
        }
        let dtype = tensor
            .get("dtype")
            .and_then(serde_json::Value::as_str)
            .ok_or_else(|| format!("qwen3_safetensors_tensor_missing_dtype:{name}"))
            .and_then(parse_weight_dtype)?;
        let shape = tensor
            .get("shape")
            .and_then(serde_json::Value::as_array)
            .ok_or_else(|| format!("qwen3_safetensors_tensor_missing_shape:{name}"))?
            .iter()
            .map(|item| {
                item.as_u64()
                    .ok_or_else(|| format!("qwen3_safetensors_tensor_shape_not_u64:{name}"))
            })
            .collect::<Result<Vec<_>, _>>()?;
        let data_offsets = match tensor.get("data_offsets").and_then(serde_json::Value::as_array) {
            Some(offsets) => {
                if offsets.len() != 2 {
                    return Err(format!(
                        "qwen3_safetensors_tensor_bad_data_offsets:{name}:len={}",
                        offsets.len()
                    ));
                }
                Some([
                    offsets[0]
                        .as_u64()
                        .ok_or_else(|| format!("qwen3_safetensors_tensor_offset_not_u64:{name}"))?,
                    offsets[1]
                        .as_u64()
                        .ok_or_else(|| format!("qwen3_safetensors_tensor_offset_not_u64:{name}"))?,
                ])
            }
            None => None,
        };
        tensors.insert(
            name.clone(),
            Qwen3Dense06bWeightTensorMetadata {
                dtype,
                shape,
                data_offsets,
                source_file: None,
                data_base_offset: 0,
            },
        );
    }
    Ok(tensors)
}

pub fn parse_weight_dtype(dtype: &str) -> Result<Qwen3Dense06bWeightDType, String> {
    match dtype {
        "F32" => Ok(Qwen3Dense06bWeightDType::F32),
        "F16" => Ok(Qwen3Dense06bWeightDType::F16),
        "BF16" => Ok(Qwen3Dense06bWeightDType::BF16),
        "I8" => Ok(Qwen3Dense06bWeightDType::I8),
        "U8" => Ok(Qwen3Dense06bWeightDType::U8),
        other => Err(format!("qwen3_safetensors_unsupported_dtype:{other}")),
    }
}

pub fn load_safetensors_file_metadata(
    path: impl AsRef<Path>,
) -> Result<Qwen3Dense06bLoadedWeights, String> {
    let path = path.as_ref();
    let mut file = File::open(path).map_err(|err| {
        format!(
            "qwen3_safetensors_open_failed:{}:{err}",
            path.display()
        )
    })?;
    let file_len = file
        .metadata()
        .map_err(|err| {
            format!(
                "qwen3_safetensors_metadata_failed:{}:{err}",
                path.display()
            )
        })?
        .len();
    let mut header_len_bytes = [0u8; 8];
    file.read_exact(&mut header_len_bytes).map_err(|err| {
        format!(
            "qwen3_safetensors_header_len_read_failed:{}:{err}",
            path.display()
        )
    })?;
    let header_len = u64::from_le_bytes(header_len_bytes);
    let data_base_offset = 8u64
        .checked_add(header_len)
        .ok_or_else(|| "qwen3_safetensors_header_offset_overflow".to_string())?;
    if data_base_offset > file_len {
        return Err(format!(
            "qwen3_safetensors_header_oob:{}:header_len={header_len}:file_len={file_len}",
            path.display()
        ));
    }
    let mut header = vec![0u8; header_len as usize];
    file.read_exact(&mut header).map_err(|err| {
        format!(
            "qwen3_safetensors_header_read_failed:{}:{err}",
            path.display()
        )
    })?;
    let header = std::str::from_utf8(&header).map_err(|err| {
        format!(
            "qwen3_safetensors_header_utf8_failed:{}:{err}",
            path.display()
        )
    })?;
    let mut tensors = parse_safetensors_metadata_json(header)?;
    let source_file = path.display().to_string();
    for (name, metadata) in tensors.iter_mut() {
        metadata.source_file = Some(source_file.clone());
        metadata.data_base_offset = data_base_offset;
        validate_safetensors_tensor_range(name, metadata, file_len)?;
    }
    Ok(Qwen3Dense06bLoadedWeights {
        source: source_file,
        tensors,
    })
}

pub fn load_safetensors_index_metadata(
    index_path: impl AsRef<Path>,
) -> Result<Qwen3Dense06bLoadedWeights, String> {
    let index_path = index_path.as_ref();
    let index_text = fs::read_to_string(index_path).map_err(|err| {
        format!(
            "qwen3_safetensors_index_read_failed:{}:{err}",
            index_path.display()
        )
    })?;
    let index: Qwen3Dense06bSafetensorsIndex = serde_json::from_str(&index_text).map_err(|err| {
        format!(
            "qwen3_safetensors_index_parse_failed:{}:{err}",
            index_path.display()
        )
    })?;
    let base_dir = index_path.parent().unwrap_or_else(|| Path::new("."));
    let unique_files: BTreeSet<String> = index.weight_map.values().cloned().collect();
    let mut file_tensors = BTreeMap::new();
    for file_name in unique_files {
        let file_path = base_dir.join(&file_name);
        let loaded = load_safetensors_file_metadata(&file_path)?;
        file_tensors.insert(file_name, loaded.tensors);
    }
    let mut tensors = BTreeMap::new();
    for (tensor_name, file_name) in index.weight_map.iter() {
        let tensor = file_tensors
            .get(file_name)
            .and_then(|items| items.get(tensor_name))
            .ok_or_else(|| {
                format!(
                    "qwen3_safetensors_index_tensor_missing:{tensor_name}:file={file_name}"
                )
            })?;
        tensors.insert(tensor_name.clone(), tensor.clone());
    }
    Ok(Qwen3Dense06bLoadedWeights {
        source: index_path.display().to_string(),
        tensors,
    })
}

pub fn load_safetensors_path_metadata(
    path: impl AsRef<Path>,
) -> Result<Qwen3Dense06bLoadedWeights, String> {
    let path = path.as_ref();
    if path.is_dir() {
        let index_path = path.join("model.safetensors.index.json");
        if index_path.exists() {
            return load_safetensors_index_metadata(index_path);
        }
        let single_path = path.join("model.safetensors");
        if single_path.exists() {
            return load_safetensors_file_metadata(single_path);
        }
        return Err(format!(
            "qwen3_safetensors_dir_missing_model_files:{}",
            path.display()
        ));
    }
    let file_name = path
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or_default();
    if file_name.ends_with(".safetensors.index.json") {
        load_safetensors_index_metadata(path)
    } else {
        load_safetensors_file_metadata(path)
    }
}

pub fn weight_manifest_from_metadata(
    topology: &SimTopology,
    profile: Qwen3Dense06bProfile,
    source: impl Into<String>,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
) -> Result<Qwen3Dense06bWeightManifest, String> {
    validate_profile(profile)?;
    validate_required_weight_tensors(profile, tensors)?;
    let tp_plan = tensor_parallel_plan(topology, profile)?;
    let mut slices = Vec::new();
    for layer_id in 0..profile.num_hidden_layers {
        for shard in tp_plan.iter() {
            push_layer_weight_slices(profile, layer_id, shard, tensors, &mut slices)?;
        }
    }
    Ok(Qwen3Dense06bWeightManifest {
        model_id: "Qwen/Qwen3-0.6B".to_string(),
        source: source.into(),
        format: "safetensors".to_string(),
        profile: Qwen3Dense06bWeightManifestProfile {
            hidden_size: profile.hidden_size,
            intermediate_size: profile.intermediate_size,
            num_hidden_layers: profile.num_hidden_layers,
            num_attention_heads: profile.num_attention_heads,
            num_key_value_heads: profile.num_key_value_heads,
            head_dim: profile.head_dim,
            tp_nodes: profile.tp_nodes,
        },
        slices,
    })
}

pub fn weight_manifest_from_safetensors_path(
    topology: &SimTopology,
    profile: Qwen3Dense06bProfile,
    path: impl AsRef<Path>,
) -> Result<Qwen3Dense06bWeightManifest, String> {
    let loaded = load_safetensors_path_metadata(path)?;
    weight_manifest_from_metadata(topology, profile, loaded.source, &loaded.tensors)
}

pub fn validate_required_weight_tensors(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
) -> Result<(), String> {
    for layer_id in 0..profile.num_hidden_layers {
        let layer_prefix = format!("model.layers.{layer_id}");
        let required = [
            (
                format!("{layer_prefix}.input_layernorm.weight"),
                vec![profile.hidden_size],
            ),
            (
                format!("{layer_prefix}.self_attn.q_proj.weight"),
                vec![profile.num_attention_heads * profile.head_dim, profile.hidden_size],
            ),
            (
                format!("{layer_prefix}.self_attn.k_proj.weight"),
                vec![profile.num_key_value_heads * profile.head_dim, profile.hidden_size],
            ),
            (
                format!("{layer_prefix}.self_attn.v_proj.weight"),
                vec![profile.num_key_value_heads * profile.head_dim, profile.hidden_size],
            ),
            (
                format!("{layer_prefix}.self_attn.o_proj.weight"),
                vec![profile.hidden_size, profile.num_attention_heads * profile.head_dim],
            ),
            (
                format!("{layer_prefix}.post_attention_layernorm.weight"),
                vec![profile.hidden_size],
            ),
            (
                format!("{layer_prefix}.mlp.gate_proj.weight"),
                vec![profile.intermediate_size, profile.hidden_size],
            ),
            (
                format!("{layer_prefix}.mlp.up_proj.weight"),
                vec![profile.intermediate_size, profile.hidden_size],
            ),
            (
                format!("{layer_prefix}.mlp.down_proj.weight"),
                vec![profile.hidden_size, profile.intermediate_size],
            ),
        ];
        for (name, expected_shape) in required {
            let metadata = tensors
                .get(&name)
                .ok_or_else(|| format!("qwen3_dense_0_6b_missing_weight_tensor:{name}"))?;
            if metadata.shape != expected_shape {
                return Err(format!(
                    "qwen3_dense_0_6b_weight_shape_mismatch:{name}:got={:?}:expected={:?}",
                    metadata.shape, expected_shape
                ));
            }
        }
    }
    Ok(())
}

fn push_layer_weight_slices(
    profile: Qwen3Dense06bProfile,
    layer_id: u64,
    shard: &Qwen3Dense06bTensorParallelShard,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    out: &mut Vec<Qwen3Dense06bWeightSlice>,
) -> Result<(), String> {
    let specs = [
        (
            Qwen3Dense06bWeightTensorKind::InputLayerNorm,
            format!("model.layers.{layer_id}.input_layernorm.weight"),
            vec![profile.hidden_size],
            None,
            0,
            profile.hidden_size,
        ),
        (
            Qwen3Dense06bWeightTensorKind::QProj,
            format!("model.layers.{layer_id}.self_attn.q_proj.weight"),
            vec![profile.num_attention_heads * profile.head_dim, profile.hidden_size],
            Some(0),
            shard.q_head_start * profile.head_dim,
            shard.q_head_end * profile.head_dim,
        ),
        (
            Qwen3Dense06bWeightTensorKind::KProj,
            format!("model.layers.{layer_id}.self_attn.k_proj.weight"),
            vec![profile.num_key_value_heads * profile.head_dim, profile.hidden_size],
            Some(0),
            shard.kv_head_start * profile.head_dim,
            shard.kv_head_end * profile.head_dim,
        ),
        (
            Qwen3Dense06bWeightTensorKind::VProj,
            format!("model.layers.{layer_id}.self_attn.v_proj.weight"),
            vec![profile.num_key_value_heads * profile.head_dim, profile.hidden_size],
            Some(0),
            shard.kv_head_start * profile.head_dim,
            shard.kv_head_end * profile.head_dim,
        ),
        (
            Qwen3Dense06bWeightTensorKind::OProj,
            format!("model.layers.{layer_id}.self_attn.o_proj.weight"),
            vec![profile.hidden_size, profile.num_attention_heads * profile.head_dim],
            Some(1),
            shard.q_head_start * profile.head_dim,
            shard.q_head_end * profile.head_dim,
        ),
        (
            Qwen3Dense06bWeightTensorKind::PostAttentionLayerNorm,
            format!("model.layers.{layer_id}.post_attention_layernorm.weight"),
            vec![profile.hidden_size],
            None,
            0,
            profile.hidden_size,
        ),
        (
            Qwen3Dense06bWeightTensorKind::GateProj,
            format!("model.layers.{layer_id}.mlp.gate_proj.weight"),
            vec![profile.intermediate_size, profile.hidden_size],
            Some(0),
            shard.shard_id * shard.local_mlp_intermediate_width,
            (shard.shard_id + 1) * shard.local_mlp_intermediate_width,
        ),
        (
            Qwen3Dense06bWeightTensorKind::UpProj,
            format!("model.layers.{layer_id}.mlp.up_proj.weight"),
            vec![profile.intermediate_size, profile.hidden_size],
            Some(0),
            shard.shard_id * shard.local_mlp_intermediate_width,
            (shard.shard_id + 1) * shard.local_mlp_intermediate_width,
        ),
        (
            Qwen3Dense06bWeightTensorKind::DownProj,
            format!("model.layers.{layer_id}.mlp.down_proj.weight"),
            vec![profile.hidden_size, profile.intermediate_size],
            Some(1),
            shard.shard_id * shard.local_mlp_intermediate_width,
            (shard.shard_id + 1) * shard.local_mlp_intermediate_width,
        ),
    ];
    for (ordinal, (kind, name, expected_shape, slice_axis, slice_start, slice_end)) in
        specs.into_iter().enumerate()
    {
        let metadata = tensors
            .get(&name)
            .ok_or_else(|| format!("qwen3_dense_0_6b_missing_weight_tensor:{name}"))?;
        if metadata.shape != expected_shape {
            return Err(format!(
                "qwen3_dense_0_6b_weight_shape_mismatch:{name}:got={:?}:expected={:?}",
                metadata.shape, expected_shape
            ));
        }
        out.push(weight_slice_from_spec(
            layer_id,
            shard.shard_id,
            ordinal as u64,
            kind,
            name,
            metadata,
            slice_axis,
            slice_start,
            slice_end,
        )?);
    }
    Ok(())
}

fn weight_slice_from_spec(
    layer_id: u64,
    shard_id: u64,
    ordinal: u64,
    tensor_kind: Qwen3Dense06bWeightTensorKind,
    tensor_name: String,
    metadata: &Qwen3Dense06bWeightTensorMetadata,
    slice_axis: Option<u64>,
    slice_start: u64,
    slice_end: u64,
) -> Result<Qwen3Dense06bWeightSlice, String> {
    let mut local_shape = metadata.shape.clone();
    if let Some(axis) = slice_axis {
        let axis_index = axis as usize;
        if axis_index >= local_shape.len() {
            return Err(format!(
                "qwen3_dense_0_6b_weight_slice_axis_oob:{tensor_name}:axis={axis}"
            ));
        }
        if slice_start >= slice_end || slice_end > local_shape[axis_index] {
            return Err(format!(
                "qwen3_dense_0_6b_weight_slice_range_invalid:{tensor_name}:start={slice_start}:end={slice_end}:dim={}",
                local_shape[axis_index]
            ));
        }
        local_shape[axis_index] = slice_end - slice_start;
    }
    let bytes = dtype_size(metadata.dtype) * local_shape.iter().product::<u64>();
    let segment = 50_000 + layer_id * 1_000 + shard_id * 100 + ordinal;
    Ok(Qwen3Dense06bWeightSlice {
        layer_id,
        shard_id,
        tensor_kind,
        tensor_name,
        dtype: metadata.dtype,
        global_shape: metadata.shape.clone(),
        slice_axis,
        slice_start,
        slice_end,
        local_shape,
        storage: Qwen3Dense06bWeightStorageRef {
            kind: Qwen3Dense06bWeightStorageKind::Block,
            storage_ref: format!(
                "qwen3_dense_0_6b/layer/{layer_id}/shard/{shard_id}/weight/{ordinal}"
            ),
            segment,
            offset: 0,
            bytes,
            checksum: weight_metadata_checksum(
                layer_id,
                shard_id,
                ordinal,
                slice_start,
                slice_end,
                bytes,
            ),
        },
    })
}

pub fn dtype_size(dtype: Qwen3Dense06bWeightDType) -> u64 {
    match dtype {
        Qwen3Dense06bWeightDType::F32 => 4,
        Qwen3Dense06bWeightDType::F16 | Qwen3Dense06bWeightDType::BF16 => 2,
        Qwen3Dense06bWeightDType::I8 | Qwen3Dense06bWeightDType::U8 => 1,
    }
}

fn weight_metadata_checksum(
    layer_id: u64,
    shard_id: u64,
    ordinal: u64,
    slice_start: u64,
    slice_end: u64,
    bytes: u64,
) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for value in [layer_id, shard_id, ordinal, slice_start, slice_end, bytes] {
        acc ^= value;
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

pub fn weight_db_key(slice: &Qwen3Dense06bWeightSlice) -> String {
    format!(
        "qwen3_dense_0_6b/layer/{}/shard/{}/{:?}",
        slice.layer_id, slice.shard_id, slice.tensor_kind
    )
}

pub fn weight_db_value(slice: &Qwen3Dense06bWeightSlice) -> Result<Vec<u8>, String> {
    serde_json::to_vec(slice).map_err(|err| format!("qwen3_weight_slice_json_encode_failed:{err}"))
}

pub fn weight_db_puts(
    manifest: &Qwen3Dense06bWeightManifest,
) -> Result<Vec<Qwen3Dense06bWeightDbPut>, String> {
    manifest
        .slices
        .iter()
        .map(|slice| {
            let value = weight_db_value(slice)?;
            Ok(Qwen3Dense06bWeightDbPut {
                key: weight_db_key(slice),
                bytes: value.len() as u64,
            })
        })
        .collect()
}

pub fn weight_service_load_plan(
    manifest: Qwen3Dense06bWeightManifest,
) -> Result<Qwen3Dense06bWeightServiceLoadPlan, String> {
    let metadata_db_puts = weight_db_puts(&manifest)?;
    let payload_writes = manifest
        .slices
        .iter()
        .map(|slice| Qwen3Dense06bWeightPayloadWrite {
            storage_ref: slice.storage.storage_ref.clone(),
            storage_kind: slice.storage.kind,
            segment: slice.storage.segment,
            offset: slice.storage.offset,
            bytes: slice.storage.bytes,
            checksum: slice.storage.checksum,
        })
        .collect();
    Ok(Qwen3Dense06bWeightServiceLoadPlan {
        manifest,
        metadata_db_puts,
        payload_writes,
    })
}

pub fn weight_service_load_req(
    plan: &Qwen3Dense06bWeightServiceLoadPlan,
    task: Option<TaskKey>,
    requester_entity: u32,
) -> WeightsLoadReq {
    WeightsLoadReq {
        task,
        requester_entity,
        metadata_puts: plan
            .metadata_db_puts
            .iter()
            .map(|put| WeightMetadataPut {
                key: put.key.clone(),
                bytes: put.bytes,
            })
            .collect(),
        payload_writes: plan
            .payload_writes
            .iter()
            .map(|write| WeightPayloadWrite {
                storage_ref: write.storage_ref.clone(),
                storage_kind: match write.storage_kind {
                    Qwen3Dense06bWeightStorageKind::Block => WeightStorageKind::Block,
                    Qwen3Dense06bWeightStorageKind::Shmem => WeightStorageKind::Shmem,
                },
                segment: SegmentHandle(write.segment),
                offset: write.offset,
                bytes: write.bytes,
                checksum: write.checksum,
            })
            .collect(),
    }
}

pub fn materialize_weight_slice_payload(
    slice: &Qwen3Dense06bWeightSlice,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
) -> Result<Vec<u8>, String> {
    let metadata = tensors
        .get(&slice.tensor_name)
        .ok_or_else(|| format!("qwen3_weight_payload_tensor_missing:{}", slice.tensor_name))?;
    if metadata.dtype != slice.dtype || metadata.shape != slice.global_shape {
        return Err(format!(
            "qwen3_weight_payload_tensor_mismatch:{}",
            slice.tensor_name
        ));
    }
    let source_file = metadata.source_file.as_ref().ok_or_else(|| {
        format!(
            "qwen3_weight_payload_source_file_missing:{}",
            slice.tensor_name
        )
    })?;
    let offsets = metadata.data_offsets.ok_or_else(|| {
        format!(
            "qwen3_weight_payload_offsets_missing:{}",
            slice.tensor_name
        )
    })?;
    let element_size = dtype_size(metadata.dtype);
    let tensor_base = metadata
        .data_base_offset
        .checked_add(offsets[0])
        .ok_or_else(|| format!("qwen3_weight_payload_base_overflow:{}", slice.tensor_name))?;
    match slice.slice_axis {
        None => {
            let bytes = offsets[1]
                .checked_sub(offsets[0])
                .ok_or_else(|| format!("qwen3_weight_payload_bad_offsets:{}", slice.tensor_name))?;
            read_file_range(source_file, tensor_base, bytes)
        }
        Some(0) => {
            if metadata.shape.is_empty() {
                return Err(format!(
                    "qwen3_weight_payload_axis0_empty_shape:{}",
                    slice.tensor_name
                ));
            }
            let inner_elems = metadata.shape[1..].iter().product::<u64>();
            let row_bytes = inner_elems
                .checked_mul(element_size)
                .ok_or_else(|| format!("qwen3_weight_payload_row_overflow:{}", slice.tensor_name))?;
            let start = tensor_base
                .checked_add(slice.slice_start.checked_mul(row_bytes).ok_or_else(|| {
                    format!("qwen3_weight_payload_start_overflow:{}", slice.tensor_name)
                })?)
                .ok_or_else(|| format!("qwen3_weight_payload_start_overflow:{}", slice.tensor_name))?;
            let bytes = (slice.slice_end - slice.slice_start)
                .checked_mul(row_bytes)
                .ok_or_else(|| format!("qwen3_weight_payload_bytes_overflow:{}", slice.tensor_name))?;
            read_file_range(source_file, start, bytes)
        }
        Some(1) => {
            if metadata.shape.len() != 2 {
                return Err(format!(
                    "qwen3_weight_payload_axis1_requires_2d:{}:rank={}",
                    slice.tensor_name,
                    metadata.shape.len()
                ));
            }
            let rows = metadata.shape[0];
            let cols = metadata.shape[1];
            let slice_cols = slice.slice_end - slice.slice_start;
            let row_stride = cols
                .checked_mul(element_size)
                .ok_or_else(|| format!("qwen3_weight_payload_stride_overflow:{}", slice.tensor_name))?;
            let slice_bytes = slice_cols
                .checked_mul(element_size)
                .ok_or_else(|| format!("qwen3_weight_payload_slice_overflow:{}", slice.tensor_name))?;
            let mut file = File::open(source_file)
                .map_err(|err| format!("qwen3_weight_payload_open_failed:{source_file}:{err}"))?;
            let mut out = Vec::with_capacity((rows * slice_bytes) as usize);
            for row in 0..rows {
                let offset = tensor_base
                    .checked_add(row.checked_mul(row_stride).ok_or_else(|| {
                        format!("qwen3_weight_payload_row_offset_overflow:{}", slice.tensor_name)
                    })?)
                    .and_then(|base| {
                        base.checked_add(slice.slice_start.checked_mul(element_size)?)
                    })
                    .ok_or_else(|| {
                        format!("qwen3_weight_payload_row_offset_overflow:{}", slice.tensor_name)
                    })?;
                let mut buf = vec![0u8; slice_bytes as usize];
                file.seek(SeekFrom::Start(offset)).map_err(|err| {
                    format!("qwen3_weight_payload_seek_failed:{source_file}:{offset}:{err}")
                })?;
                file.read_exact(&mut buf).map_err(|err| {
                    format!("qwen3_weight_payload_read_failed:{source_file}:{offset}:{err}")
                })?;
                out.extend_from_slice(&buf);
            }
            Ok(out)
        }
        Some(axis) => Err(format!(
            "qwen3_weight_payload_unsupported_slice_axis:{}:axis={axis}",
            slice.tensor_name
        )),
    }
}

fn validate_safetensors_tensor_range(
    name: &str,
    metadata: &Qwen3Dense06bWeightTensorMetadata,
    file_len: u64,
) -> Result<(), String> {
    let [start, end] = metadata
        .data_offsets
        .ok_or_else(|| format!("qwen3_safetensors_tensor_missing_data_offsets:{name}"))?;
    if start >= end {
        return Err(format!(
            "qwen3_safetensors_tensor_bad_range:{name}:start={start}:end={end}"
        ));
    }
    let expected = dtype_size(metadata.dtype)
        .checked_mul(metadata.shape.iter().product::<u64>())
        .ok_or_else(|| format!("qwen3_safetensors_tensor_bytes_overflow:{name}"))?;
    let actual = end - start;
    if actual != expected {
        return Err(format!(
            "qwen3_safetensors_tensor_size_mismatch:{name}:got={actual}:expected={expected}"
        ));
    }
    let absolute_end = metadata
        .data_base_offset
        .checked_add(end)
        .ok_or_else(|| format!("qwen3_safetensors_tensor_absolute_end_overflow:{name}"))?;
    if absolute_end > file_len {
        return Err(format!(
            "qwen3_safetensors_tensor_oob:{name}:end={absolute_end}:file_len={file_len}"
        ));
    }
    Ok(())
}

fn read_file_range(path: &str, offset: u64, bytes: u64) -> Result<Vec<u8>, String> {
    let mut file =
        File::open(path).map_err(|err| format!("qwen3_weight_payload_open_failed:{path}:{err}"))?;
    file.seek(SeekFrom::Start(offset))
        .map_err(|err| format!("qwen3_weight_payload_seek_failed:{path}:{offset}:{err}"))?;
    let mut out = vec![0u8; bytes as usize];
    file.read_exact(&mut out)
        .map_err(|err| format!("qwen3_weight_payload_read_failed:{path}:{offset}:{err}"))?;
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn test_topology() -> SimTopology {
        let config = sim_config::ScenarioConfig::from_yaml_str(TEST_YAML).expect("valid config");
        SimTopology::from_config(&config).expect("topology")
    }

    const TEST_YAML: &str = r#"
scenario:
  name: qwen3_dense_0_6b_test
  group: W
  variant: qwen3_tp8
  seed: 42
  duration_us: 1000000
  logical_system: llm-serving-qwen3
platform:
  backend: qemu
  machine_profile: ub-host-minimal
  cpu_model: host
  memory_model: numa-sim
  device_model_mode: mixed
topology:
  hosts: 8
  ubpus_per_host: 1
  entities_per_ubpu: 1
  ub_domains:
    - id: w4
      hosts: [0, 1, 2, 3, 4, 5, 6, 7]
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
    pe_count: 8
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
  profile: qwen3_dense_0_6b
  qps: 1
  unique_prefixes: 1
  blocks_per_request: 4
  function_label_mode: host_orchestration
faults: []
outputs:
  trace: true
  metrics_csv: true
  summary_json: true
  emit_task_coord_trace: true
  emit_data_service_trace: true
  emit_qemu_platform_trace: true
"#;

    #[test]
    fn layer_ir_and_tp_partition_are_explicit() {
        let topology = test_topology();
        let graph = layer_graph_ir(QWEN3_DENSE_0_6B_PROFILE, 0);
        let op_kinds: Vec<Qwen3Dense06bLayerOpKind> =
            graph.ops.iter().map(|op| op.kind).collect();
        assert_eq!(
            op_kinds,
            vec![
                Qwen3Dense06bLayerOpKind::RmsNorm,
                Qwen3Dense06bLayerOpKind::QkvProjection,
                Qwen3Dense06bLayerOpKind::Rope,
                Qwen3Dense06bLayerOpKind::AttentionScore,
                Qwen3Dense06bLayerOpKind::AttentionSoftmax,
                Qwen3Dense06bLayerOpKind::AttentionValue,
                Qwen3Dense06bLayerOpKind::OProjection,
                Qwen3Dense06bLayerOpKind::AttentionResidualAdd,
                Qwen3Dense06bLayerOpKind::MlpUpGateProjection,
                Qwen3Dense06bLayerOpKind::MlpSwiGlu,
                Qwen3Dense06bLayerOpKind::MlpDownProjection,
                Qwen3Dense06bLayerOpKind::MlpResidualAdd,
            ]
        );
        assert_eq!(graph.hidden_size, 1024);
        assert_eq!(graph.intermediate_size, 3072);
        assert_eq!(graph.attention_heads, 16);
        assert_eq!(graph.kv_heads, 8);
        assert_eq!(graph.head_dim, 128);
        assert_eq!(graph.prefill_tokens, 128);
        assert_eq!(graph.ops[1].output_width, 4096);
        assert_eq!(graph.ops[7].lowering, Qwen3Dense06bLoweringKind::MissingAdd);
        assert_eq!(graph.ops[8].output_width, 6144);
        assert_eq!(graph.ops[8].lowering, Qwen3Dense06bLoweringKind::TiledMatmul);
        assert_eq!(graph.ops[9].output_width, 3072);
        assert_eq!(
            graph.ops[9].lowering,
            Qwen3Dense06bLoweringKind::MissingSiluSwiglu
        );
        assert!(graph.ops[6].requires_collective);
        assert!(graph.ops[10].requires_collective);

        let summary = lowering_summary(&graph);
        assert_eq!(summary.tiled_matmul_ops, 4);
        assert_eq!(summary.missing_ops, 8);
        assert_eq!(summary.missing_rmsnorm, 1);
        assert_eq!(summary.missing_rope, 1);
        assert_eq!(summary.missing_softmax, 1);
        assert_eq!(summary.missing_silu_swiglu, 1);
        assert_eq!(summary.missing_add, 2);
        assert_eq!(summary.missing_collective, 2);

        let tp_plan =
            tensor_parallel_plan(&topology, QWEN3_DENSE_0_6B_PROFILE).expect("tp plan");
        assert_eq!(tp_plan.len(), 8);
        for (index, shard) in tp_plan.iter().enumerate() {
            assert_eq!(shard.shard_id, index as u64);
            assert_eq!(shard.local_q_heads, 2);
            assert_eq!(shard.local_kv_heads, 1);
            assert_eq!(shard.local_q_width, 256);
            assert_eq!(shard.local_kv_width, 128);
            assert_eq!(shard.local_o_input_width, 256);
            assert_eq!(shard.local_mlp_intermediate_width, 384);
            assert_eq!(shard.q_head_start, index as u64 * 2);
            assert_eq!(shard.q_head_end, index as u64 * 2 + 2);
            assert_eq!(shard.kv_head_start, index as u64);
            assert_eq!(shard.kv_head_end, index as u64 + 1);
        }
    }

    #[test]
    fn weight_manifest_builds_tp_slices_and_db_refs() {
        let topology = test_topology();
        let config = r#"{
            "vocab_size": 151936,
            "hidden_size": 1024,
            "intermediate_size": 3072,
            "num_hidden_layers": 28,
            "num_attention_heads": 16,
            "num_key_value_heads": 8,
            "head_dim": 128,
            "max_position_embeddings": 40960,
            "rope_theta": 1000000
        }"#;
        let profile = profile_from_config_json(config).expect("qwen3 config parser");
        let header = r#"{
            "__metadata__": {"format": "pt"},
            "model.layers.0.self_attn.q_proj.weight": {
                "dtype": "BF16",
                "shape": [2048, 1024],
                "data_offsets": [0, 4194304]
            }
        }"#;
        let parsed = parse_safetensors_metadata_json(header).expect("metadata parser");
        assert_eq!(
            parsed["model.layers.0.self_attn.q_proj.weight"].dtype,
            Qwen3Dense06bWeightDType::BF16
        );
        assert_eq!(
            parsed["model.layers.0.self_attn.q_proj.weight"].shape,
            vec![2048, 1024]
        );

        let tensors = test_weight_metadata(profile);
        let manifest = weight_manifest_from_metadata(
            &topology,
            profile,
            "/models/qwen3-0.6b/model.safetensors",
            &tensors,
        )
        .expect("weight manifest");
        assert_eq!(manifest.profile.hidden_size, 1024);
        assert_eq!(manifest.profile.tp_nodes, 8);
        assert_eq!(manifest.slices.len(), 28 * 8 * 9);

        let q_proj = manifest
            .slices
            .iter()
            .find(|slice| {
                slice.layer_id == 0
                    && slice.shard_id == 3
                    && slice.tensor_kind == Qwen3Dense06bWeightTensorKind::QProj
            })
            .expect("layer0 shard3 q_proj slice");
        assert_eq!(q_proj.global_shape, vec![2048, 1024]);
        assert_eq!(q_proj.slice_axis, Some(0));
        assert_eq!(q_proj.slice_start, 768);
        assert_eq!(q_proj.slice_end, 1024);
        assert_eq!(q_proj.local_shape, vec![256, 1024]);
        assert_eq!(q_proj.storage.bytes, 256 * 1024 * 2);

        let down_proj = manifest
            .slices
            .iter()
            .find(|slice| {
                slice.layer_id == 0
                    && slice.shard_id == 7
                    && slice.tensor_kind == Qwen3Dense06bWeightTensorKind::DownProj
            })
            .expect("layer0 shard7 down_proj slice");
        assert_eq!(down_proj.global_shape, vec![1024, 3072]);
        assert_eq!(down_proj.slice_axis, Some(1));
        assert_eq!(down_proj.slice_start, 2688);
        assert_eq!(down_proj.slice_end, 3072);
        assert_eq!(down_proj.local_shape, vec![1024, 384]);

        assert_eq!(weight_db_key(q_proj), "qwen3_dense_0_6b/layer/0/shard/3/QProj");
        let db_value = weight_db_value(q_proj).expect("db value");
        assert!(db_value.len() > 128);
        assert!(String::from_utf8_lossy(&db_value).contains("storage_ref"));
        let db_puts = weight_db_puts(&manifest).expect("db puts");
        assert_eq!(db_puts.len(), manifest.slices.len());
    }

    fn test_weight_metadata(
        profile: Qwen3Dense06bProfile,
    ) -> BTreeMap<String, Qwen3Dense06bWeightTensorMetadata> {
        let mut tensors = BTreeMap::new();
        for layer_id in 0..profile.num_hidden_layers {
            let layer_prefix = format!("model.layers.{layer_id}");
            let entries = [
                (
                    format!("{layer_prefix}.input_layernorm.weight"),
                    vec![profile.hidden_size],
                ),
                (
                    format!("{layer_prefix}.self_attn.q_proj.weight"),
                    vec![profile.num_attention_heads * profile.head_dim, profile.hidden_size],
                ),
                (
                    format!("{layer_prefix}.self_attn.k_proj.weight"),
                    vec![profile.num_key_value_heads * profile.head_dim, profile.hidden_size],
                ),
                (
                    format!("{layer_prefix}.self_attn.v_proj.weight"),
                    vec![profile.num_key_value_heads * profile.head_dim, profile.hidden_size],
                ),
                (
                    format!("{layer_prefix}.self_attn.o_proj.weight"),
                    vec![profile.hidden_size, profile.num_attention_heads * profile.head_dim],
                ),
                (
                    format!("{layer_prefix}.post_attention_layernorm.weight"),
                    vec![profile.hidden_size],
                ),
                (
                    format!("{layer_prefix}.mlp.gate_proj.weight"),
                    vec![profile.intermediate_size, profile.hidden_size],
                ),
                (
                    format!("{layer_prefix}.mlp.up_proj.weight"),
                    vec![profile.intermediate_size, profile.hidden_size],
                ),
                (
                    format!("{layer_prefix}.mlp.down_proj.weight"),
                    vec![profile.hidden_size, profile.intermediate_size],
                ),
            ];
            for (name, shape) in entries {
                tensors.insert(
                    name,
                    Qwen3Dense06bWeightTensorMetadata {
                        dtype: Qwen3Dense06bWeightDType::BF16,
                        shape,
                        data_offsets: None,
                        source_file: None,
                        data_base_offset: 0,
                    },
                );
            }
        }
        tensors
    }

    #[test]
    fn safetensors_file_loader_reads_header_and_materializes_slices() {
        let path = std::env::temp_dir().join(format!(
            "qwen3_weight_loader_test_{}.safetensors",
            std::process::id()
        ));
        let header = r#"{"tensor.weight":{"dtype":"U8","shape":[2,4],"data_offsets":[0,8]}}"#;
        let payload = [0u8, 1, 2, 3, 4, 5, 6, 7];
        write_test_safetensors(&path, header, &payload);

        let loaded = load_safetensors_file_metadata(&path).expect("load safetensors metadata");
        let metadata = loaded.tensors.get("tensor.weight").expect("tensor metadata");
        assert_eq!(metadata.dtype, Qwen3Dense06bWeightDType::U8);
        assert_eq!(metadata.shape, vec![2, 4]);
        assert_eq!(metadata.data_offsets, Some([0, 8]));
        assert!(metadata.source_file.as_ref().unwrap().ends_with(".safetensors"));

        let axis0 = Qwen3Dense06bWeightSlice {
            layer_id: 0,
            shard_id: 0,
            tensor_kind: Qwen3Dense06bWeightTensorKind::QProj,
            tensor_name: "tensor.weight".to_string(),
            dtype: Qwen3Dense06bWeightDType::U8,
            global_shape: vec![2, 4],
            slice_axis: Some(0),
            slice_start: 1,
            slice_end: 2,
            local_shape: vec![1, 4],
            storage: Qwen3Dense06bWeightStorageRef {
                kind: Qwen3Dense06bWeightStorageKind::Block,
                storage_ref: "test/axis0".to_string(),
                segment: 1,
                offset: 0,
                bytes: 4,
                checksum: 0,
            },
        };
        assert_eq!(
            materialize_weight_slice_payload(&axis0, &loaded.tensors).expect("axis0 payload"),
            vec![4, 5, 6, 7]
        );

        let axis1 = Qwen3Dense06bWeightSlice {
            slice_axis: Some(1),
            slice_start: 1,
            slice_end: 3,
            local_shape: vec![2, 2],
            storage: Qwen3Dense06bWeightStorageRef {
                storage_ref: "test/axis1".to_string(),
                bytes: 4,
                ..axis0.storage.clone()
            },
            ..axis0
        };
        assert_eq!(
            materialize_weight_slice_payload(&axis1, &loaded.tensors).expect("axis1 payload"),
            vec![1, 2, 5, 6]
        );

        let _ = std::fs::remove_file(path);
    }

    fn write_test_safetensors(path: &std::path::Path, header: &str, payload: &[u8]) {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&(header.len() as u64).to_le_bytes());
        bytes.extend_from_slice(header.as_bytes());
        bytes.extend_from_slice(payload);
        std::fs::write(path, bytes).expect("write test safetensors");
    }
}
