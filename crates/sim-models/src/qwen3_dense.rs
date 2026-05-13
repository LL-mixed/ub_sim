use serde::{Deserialize, Serialize};

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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Qwen3DenseVariant {
    Dense0_6B,
    ConfigDriven,
}

pub const QWEN3_DENSE_DEFAULT_PREFILL_TOKENS: u64 = 128;
pub const QWEN3_DENSE_DEFAULT_DECODE_TOKENS: u64 = 1;
pub const QWEN3_DENSE_DEFAULT_TP_NODES: u64 = 8;

pub fn qwen3_dense_0_6b_profile() -> Qwen3DenseProfile {
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

pub fn validate_profile(
    profile: &Qwen3DenseProfile,
    variant: Qwen3DenseVariant,
) -> Result<(), String> {
    validate_structural_profile(profile)?;
    if variant == Qwen3DenseVariant::Dense0_6B && *profile != qwen3_dense_0_6b_profile() {
        return Err("qwen3_dense_0_6b_config_mismatch".to_string());
    }
    Ok(())
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

#[cfg(test)]
mod tests {
    use super::*;

    const QWEN3_0_6B_CONFIG: &str = r#"{
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
    fn parses_existing_qwen3_0_6b_config_as_generic_profile() {
        let profile = profile_from_config_json(
            "Qwen/Qwen3-0.6B",
            QWEN3_0_6B_CONFIG,
            QWEN3_DENSE_DEFAULT_TP_NODES,
            QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
            QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
        )
        .expect("parse 0.6B profile");

        assert_eq!(profile, qwen3_dense_0_6b_profile());
        validate_profile(&profile, Qwen3DenseVariant::Dense0_6B).expect("0.6B profile");
    }

    #[test]
    fn accepts_qwen3_14b_shaped_config_without_0_6b_equality_check() {
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
    }
}
