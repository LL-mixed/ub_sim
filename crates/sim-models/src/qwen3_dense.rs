use serde::{Deserialize, Serialize};
use sim_topology::SimTopology;
use std::fs;
use std::path::Path;

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Qwen3DenseProfile {
    pub model_id: String,
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

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Qwen3DenseLayerRange {
    pub node_index: u64,
    pub owner_node: u64,
    pub layer_start: u64,
    pub layer_end: u64,
    pub next_node_index: u64,
    pub next_owner_node: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Qwen3DenseTensorParallelShard {
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
pub enum Qwen3DenseVariant {
    ReferenceDefault,
    ConfigDriven,
}

pub const QWEN3_DENSE_DEFAULT_PREFILL_TOKENS: u64 = 128;
pub const QWEN3_DENSE_DEFAULT_DECODE_TOKENS: u64 = 1;
pub const QWEN3_DENSE_DEFAULT_TP_NODES: u64 = 8;
pub const QWEN3_DENSE_HIDDEN_ELEM_BYTES: u64 = 2;
pub const QWEN3_DENSE_KV_ELEM_BYTES: u64 = 4;
pub const QWEN3_DENSE_KV_STREAMS: u64 = 2;

pub fn qwen3_dense_reference_profile() -> Qwen3DenseProfile {
    Qwen3DenseProfile {
        model_id: "Qwen/Qwen3-0.6B".to_string(),
        vocab_size: 151_936,
        hidden_size: 1_024,
        intermediate_size: 3_072,
        num_hidden_layers: 28,
        num_attention_heads: 16,
        num_key_value_heads: 8,
        head_dim: 128,
        max_position_embeddings: 40_960,
        rope_theta: 1_000_000,
        prefill_tokens: QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
        decode_tokens: QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
        tp_nodes: QWEN3_DENSE_DEFAULT_TP_NODES,
    }
}

pub fn is_qwen3_dense_reference_shape(profile: &Qwen3DenseProfile) -> bool {
    let expected = qwen3_dense_reference_profile();
    profile.vocab_size == expected.vocab_size
        && profile.hidden_size == expected.hidden_size
        && profile.intermediate_size == expected.intermediate_size
        && profile.num_hidden_layers == expected.num_hidden_layers
        && profile.num_attention_heads == expected.num_attention_heads
        && profile.num_key_value_heads == expected.num_key_value_heads
        && profile.head_dim == expected.head_dim
        && profile.max_position_embeddings == expected.max_position_embeddings
        && profile.rope_theta == expected.rope_theta
        && profile.prefill_tokens == expected.prefill_tokens
        && profile.decode_tokens == expected.decode_tokens
        && profile.tp_nodes == expected.tp_nodes
}

pub fn profile_from_config_json(
    model_id: impl Into<String>,
    config_json: &str,
    tp_nodes: u64,
    prefill_tokens: u64,
    decode_tokens: u64,
) -> Result<Qwen3DenseProfile, String> {
    let value: serde_json::Value = serde_json::from_str(config_json)
        .map_err(|err| format!("qwen3_config_json_parse_failed:{err}"))?;
    let get_u64 = |key: &str| -> Result<u64, String> {
        value
            .get(key)
            .and_then(serde_json::Value::as_u64)
            .ok_or_else(|| format!("qwen3_config_missing_u64:{key}"))
    };
    let profile = Qwen3DenseProfile {
        model_id: model_id.into(),
        vocab_size: get_u64("vocab_size")?,
        hidden_size: get_u64("hidden_size")?,
        intermediate_size: get_u64("intermediate_size")?,
        num_hidden_layers: get_u64("num_hidden_layers")?,
        num_attention_heads: get_u64("num_attention_heads")?,
        num_key_value_heads: get_u64("num_key_value_heads")?,
        head_dim: get_u64("head_dim")?,
        max_position_embeddings: get_u64("max_position_embeddings")?,
        rope_theta: get_u64("rope_theta")?,
        prefill_tokens,
        decode_tokens,
        tp_nodes,
    };
    validate_profile(&profile, Qwen3DenseVariant::ConfigDriven)?;
    Ok(profile)
}

pub fn profile_from_weights_dir(
    weights_dir: &Path,
    model_id: Option<&str>,
    tp_nodes: u64,
    prefill_tokens: u64,
    decode_tokens: u64,
) -> Result<Qwen3DenseProfile, String> {
    let config_path = weights_dir.join("config.json");
    let config_json = fs::read_to_string(&config_path).map_err(|err| {
        format!(
            "qwen3_config_json_read_failed:path={}:error={err}",
            config_path.display()
        )
    })?;
    let resolved_model_id = model_id
        .map(ToOwned::to_owned)
        .or_else(|| model_id_from_config_json(&config_json))
        .unwrap_or_else(|| {
            weights_dir
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or("qwen3-dense")
                .to_string()
        });
    profile_from_config_json(
        resolved_model_id,
        &config_json,
        tp_nodes,
        prefill_tokens,
        decode_tokens,
    )
}

pub fn validate_profile(
    profile: &Qwen3DenseProfile,
    variant: Qwen3DenseVariant,
) -> Result<(), String> {
    validate_structural_profile(profile)?;
    if variant == Qwen3DenseVariant::ReferenceDefault && !is_qwen3_dense_reference_shape(profile) {
        return Err("qwen3_dense_reference_config_mismatch".to_string());
    }
    Ok(())
}

pub fn model_key(model_id: &str) -> String {
    let tail = model_id.rsplit('/').next().unwrap_or(model_id);
    let mut key = String::new();
    let mut previous_dash = false;
    for ch in tail.chars().flat_map(char::to_lowercase) {
        if ch.is_ascii_alphanumeric() {
            key.push(ch);
            previous_dash = false;
        } else if !previous_dash {
            key.push('-');
            previous_dash = true;
        }
    }
    while key.ends_with('-') {
        key.pop();
    }
    if key.is_empty() {
        "qwen3-dense".to_string()
    } else {
        key
    }
}

pub fn hidden_range_bytes(profile: &Qwen3DenseProfile) -> u64 {
    profile.prefill_tokens * profile.hidden_size * QWEN3_DENSE_HIDDEN_ELEM_BYTES
}

pub fn decode_hidden_bytes(profile: &Qwen3DenseProfile) -> u64 {
    profile.decode_tokens * profile.hidden_size * QWEN3_DENSE_HIDDEN_ELEM_BYTES
}

pub fn kv_state_bytes_for_layer_count(profile: &Qwen3DenseProfile, layer_count: u64) -> u64 {
    layer_count
        * profile.decode_tokens
        * profile.num_key_value_heads
        * profile.head_dim
        * QWEN3_DENSE_KV_STREAMS
        * QWEN3_DENSE_KV_ELEM_BYTES
}

pub fn balanced_layer_ranges(
    profile: &Qwen3DenseProfile,
) -> Result<Vec<Qwen3DenseLayerRange>, String> {
    validate_structural_profile(profile)?;
    let node_count = profile.tp_nodes;
    let base = profile.num_hidden_layers / node_count;
    let rem = profile.num_hidden_layers % node_count;
    let mut ranges = Vec::with_capacity(node_count as usize);
    let mut cursor = 0u64;

    for node_index in 0..node_count {
        let count = base + u64::from(node_index < rem);
        let layer_start = cursor;
        let layer_end = layer_start + count;
        let next_node_index = (node_index + 1) % node_count;
        ranges.push(Qwen3DenseLayerRange {
            node_index,
            owner_node: node_index + 1,
            layer_start,
            layer_end,
            next_node_index,
            next_owner_node: next_node_index + 1,
        });
        cursor = layer_end;
    }
    Ok(ranges)
}

pub fn tensor_parallel_plan(
    topology: &SimTopology,
    profile: &Qwen3DenseProfile,
) -> Result<Vec<Qwen3DenseTensorParallelShard>, String> {
    validate_structural_profile(profile)?;
    let hosts: Vec<u64> = topology.hosts.iter().map(|host| host.node_id).collect();
    let ubpus: Vec<u64> = topology.ubpus.iter().map(|ubpu| ubpu.node_id).collect();
    if hosts.is_empty() || ubpus.is_empty() {
        return Err("qwen3_dense_missing_topology_nodes".to_string());
    }
    if profile.num_key_value_heads % profile.tp_nodes != 0 {
        return Err(format!(
            "qwen3_dense_kv_heads_not_divisible:kv_heads={}:tp={}",
            profile.num_key_value_heads, profile.tp_nodes
        ));
    }
    let q_heads_per_shard = profile.num_attention_heads / profile.tp_nodes;
    let kv_heads_per_shard = profile.num_key_value_heads / profile.tp_nodes;
    let mlp_intermediate_per_shard = profile.intermediate_size / profile.tp_nodes;

    Ok((0..profile.tp_nodes)
        .map(|shard_id| {
            let q_head_start = shard_id * q_heads_per_shard;
            let kv_head_start = shard_id * kv_heads_per_shard;
            Qwen3DenseTensorParallelShard {
                shard_id,
                owner_node: hosts[shard_id as usize % hosts.len()],
                target_node: ubpus[shard_id as usize % ubpus.len()],
                q_head_start,
                q_head_end: q_head_start + q_heads_per_shard,
                kv_head_start,
                kv_head_end: kv_head_start + kv_heads_per_shard,
                local_q_heads: q_heads_per_shard,
                local_kv_heads: kv_heads_per_shard,
                local_q_width: q_heads_per_shard * profile.head_dim,
                local_kv_width: kv_heads_per_shard * profile.head_dim,
                local_o_input_width: q_heads_per_shard * profile.head_dim,
                local_mlp_intermediate_width: mlp_intermediate_per_shard,
            }
        })
        .collect())
}

fn validate_structural_profile(profile: &Qwen3DenseProfile) -> Result<(), String> {
    let checks = [
        ("vocab_size", profile.vocab_size),
        ("hidden_size", profile.hidden_size),
        ("intermediate_size", profile.intermediate_size),
        ("num_hidden_layers", profile.num_hidden_layers),
        ("num_attention_heads", profile.num_attention_heads),
        ("num_key_value_heads", profile.num_key_value_heads),
        ("head_dim", profile.head_dim),
        ("max_position_embeddings", profile.max_position_embeddings),
        ("rope_theta", profile.rope_theta),
        ("prefill_tokens", profile.prefill_tokens),
        ("decode_tokens", profile.decode_tokens),
        ("tp_nodes", profile.tp_nodes),
    ];
    for (name, value) in checks {
        if value == 0 {
            return Err(format!("qwen3_dense_profile_zero:{name}"));
        }
    }
    if profile.num_attention_heads % profile.tp_nodes != 0 {
        return Err(format!(
            "qwen3_dense_heads_not_divisible:heads={}:tp={}",
            profile.num_attention_heads, profile.tp_nodes
        ));
    }
    if profile.num_key_value_heads > profile.num_attention_heads {
        return Err(format!(
            "qwen3_dense_kv_heads_exceed_attention_heads:kv_heads={}:heads={}",
            profile.num_key_value_heads, profile.num_attention_heads
        ));
    }
    if profile.intermediate_size % profile.tp_nodes != 0 {
        return Err(format!(
            "qwen3_dense_intermediate_not_divisible:intermediate={}:tp={}",
            profile.intermediate_size, profile.tp_nodes
        ));
    }
    Ok(())
}

fn model_id_from_config_json(config_json: &str) -> Option<String> {
    let value: serde_json::Value = serde_json::from_str(config_json).ok()?;
    value
        .get("_name_or_path")
        .or_else(|| value.get("model_id"))
        .and_then(serde_json::Value::as_str)
        .filter(|value| !value.is_empty())
        .map(ToOwned::to_owned)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    const QWEN3_REFERENCE_CONFIG: &str = r#"{
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

    const QWEN3_14B_SHAPED_CONFIG: &str = r#"{
        "vocab_size": 151936,
        "hidden_size": 5120,
        "intermediate_size": 17408,
        "num_hidden_layers": 40,
        "num_attention_heads": 40,
        "num_key_value_heads": 8,
        "head_dim": 128,
        "max_position_embeddings": 40960,
        "rope_theta": 1000000
    }"#;

    #[test]
    fn parses_existing_qwen3_dense_reference_config_as_generic_profile() {
        let profile = profile_from_config_json(
            "Qwen/Qwen3-0.6B",
            QWEN3_REFERENCE_CONFIG,
            QWEN3_DENSE_DEFAULT_TP_NODES,
            QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
            QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
        )
        .expect("parse reference profile");

        assert_eq!(profile, qwen3_dense_reference_profile());
        assert!(is_qwen3_dense_reference_shape(&profile));
        validate_profile(&profile, Qwen3DenseVariant::ReferenceDefault).expect("reference profile");
    }

    #[test]
    fn accepts_reference_shape_with_local_directory_model_id() {
        let profile = profile_from_config_json(
            "Qwen3-reference",
            QWEN3_REFERENCE_CONFIG,
            QWEN3_DENSE_DEFAULT_TP_NODES,
            QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
            QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
        )
        .expect("parse local reference profile");

        assert_ne!(profile, qwen3_dense_reference_profile());
        assert!(is_qwen3_dense_reference_shape(&profile));
        validate_profile(&profile, Qwen3DenseVariant::ReferenceDefault)
            .expect("reference shape should not depend on model_id spelling");
    }

    #[test]
    fn accepts_qwen3_14b_shaped_config_without_reference_equality_check() {
        let profile = profile_from_config_json(
            "Qwen/Qwen3-14B",
            QWEN3_14B_SHAPED_CONFIG,
            QWEN3_DENSE_DEFAULT_TP_NODES,
            QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
            QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
        )
        .expect("parse 14B shaped profile");

        assert_eq!(profile.model_id, "Qwen/Qwen3-14B");
        assert_eq!(profile.hidden_size, 5120);
        assert_eq!(profile.num_hidden_layers, 40);
        assert_eq!(profile.num_attention_heads, 40);
        assert_eq!(profile.num_key_value_heads, 8);
        assert_eq!(profile.intermediate_size, 17408);
        assert_eq!(model_key(&profile.model_id), "qwen3-14b");
        assert_eq!(hidden_range_bytes(&profile), 1_310_720);
        assert_eq!(decode_hidden_bytes(&profile), 10_240);

        let ranges = balanced_layer_ranges(&profile).expect("14B layer ranges");
        assert_eq!(ranges.len(), 8);
        assert!(ranges
            .iter()
            .all(|range| range.layer_end - range.layer_start == 5));
        assert_eq!(
            kv_state_bytes_for_layer_count(&profile, ranges[0].layer_end - ranges[0].layer_start),
            40_960
        );
    }

    #[test]
    fn computes_existing_reference_layer_ranges_and_payload_sizes() {
        let profile = qwen3_dense_reference_profile();
        let ranges = balanced_layer_ranges(&profile).expect("reference layer ranges");

        assert_eq!(hidden_range_bytes(&profile), 262_144);
        assert_eq!(decode_hidden_bytes(&profile), 2_048);
        assert_eq!(
            ranges
                .iter()
                .map(|range| (range.owner_node, range.layer_start, range.layer_end))
                .collect::<Vec<_>>(),
            vec![
                (1, 0, 4),
                (2, 4, 8),
                (3, 8, 12),
                (4, 12, 16),
                (5, 16, 19),
                (6, 19, 22),
                (7, 22, 25),
                (8, 25, 28),
            ]
        );
        assert_eq!(kv_state_bytes_for_layer_count(&profile, 4), 32_768);
    }

    #[test]
    fn reads_profile_from_weights_dir_config() {
        let dir = std::env::temp_dir().join(format!(
            "sim_models_qwen3_dense_profile_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).expect("create qwen3 profile temp dir");
        let config = format!(
            "{{\"_name_or_path\":\"Qwen/Qwen3-14B\",{}",
            QWEN3_14B_SHAPED_CONFIG
                .trim_start()
                .strip_prefix('{')
                .expect("config starts with object")
        );
        fs::write(dir.join("config.json"), config).expect("write qwen3 config");

        let profile = profile_from_weights_dir(
            &dir,
            None,
            QWEN3_DENSE_DEFAULT_TP_NODES,
            QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
            QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
        )
        .expect("read profile from weights dir");
        assert_eq!(profile.model_id, "Qwen/Qwen3-14B");
        assert_eq!(profile.hidden_size, 5120);

        let _ = fs::remove_dir_all(&dir);
    }
}
