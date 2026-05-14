use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use std::sync::{Mutex, OnceLock};

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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bTokenizerPolicy {
    pub model_id: &'static str,
    pub tokenizer_family: &'static str,
    pub vocab_size: u64,
    pub synthetic_piece_prefix: &'static str,
    pub synthetic_piece_digits: u64,
    pub synthetic_piece_bytes: u64,
    pub policy_hash: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bTokenPiece {
    pub token_id: u64,
    pub byte_len: u64,
    pub word0: u64,
    pub word1: u64,
    pub checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bPromptTokenization {
    pub token_ids: Vec<u64>,
    pub token_count: u64,
    pub token_checksum: u64,
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

pub const QWEN3_DENSE_0_6B_TOKENIZER_POLICY_KIND: u64 = 1;
pub const QWEN3_DENSE_0_6B_TOKENIZER_ASSET_POLICY_KIND: u64 = 2;

pub fn profile_from_dense_profile(
    profile: &crate::qwen3_dense::Qwen3DenseProfile,
) -> Qwen3Dense06bProfile {
    Qwen3Dense06bProfile {
        vocab_size: profile.vocab_size,
        hidden_size: profile.hidden_size,
        intermediate_size: profile.intermediate_size,
        num_hidden_layers: profile.num_hidden_layers,
        num_attention_heads: profile.num_attention_heads,
        num_key_value_heads: profile.num_key_value_heads,
        head_dim: profile.head_dim,
        max_position_embeddings: profile.max_position_embeddings,
        rope_theta: profile.rope_theta,
        prefill_tokens: profile.prefill_tokens,
        decode_tokens: profile.decode_tokens,
        tp_nodes: profile.tp_nodes,
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bTokenizerAssetFileSummary {
    pub name: String,
    pub bytes: u64,
    pub checksum: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bTokenizerAssetSummary {
    pub model_id: String,
    pub source: String,
    pub vocab_size: u64,
    pub vocab_entries: u64,
    pub added_tokens: u64,
    pub merge_rules: u64,
    pub files: Vec<Qwen3Dense06bTokenizerAssetFileSummary>,
    pub aggregate_checksum: u64,
}

pub fn tokenizer_policy(profile: Qwen3Dense06bProfile) -> Qwen3Dense06bTokenizerPolicy {
    let mut policy = Qwen3Dense06bTokenizerPolicy {
        model_id: "Qwen/Qwen3-0.6B",
        tokenizer_family: "qwen3-tiktoken-compatible-synthetic-piece",
        vocab_size: profile.vocab_size,
        synthetic_piece_prefix: "q3_",
        synthetic_piece_digits: 6,
        synthetic_piece_bytes: 9,
        policy_hash: 0,
    };
    policy.policy_hash = tokenizer_policy_hash(&policy);
    policy
}

pub fn tokenizer_policy_hash(policy: &Qwen3Dense06bTokenizerPolicy) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for byte in policy
        .model_id
        .bytes()
        .chain([0])
        .chain(policy.tokenizer_family.bytes())
        .chain([0])
        .chain(policy.vocab_size.to_le_bytes())
        .chain(policy.synthetic_piece_prefix.bytes())
        .chain([0])
        .chain(policy.synthetic_piece_digits.to_le_bytes())
        .chain(policy.synthetic_piece_bytes.to_le_bytes())
    {
        acc ^= byte as u64;
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

pub fn token_piece_from_policy(
    policy: Qwen3Dense06bTokenizerPolicy,
    token_id: u64,
) -> Qwen3Dense06bTokenPiece {
    debug_assert_eq!(policy.synthetic_piece_prefix, "q3_");
    debug_assert_eq!(policy.synthetic_piece_digits, 6);
    let piece = token_piece_bytes_from_policy(policy, token_id);
    debug_assert_eq!(piece.len() as u64, policy.synthetic_piece_bytes);
    token_piece_from_bytes(token_id, &piece)
}

pub fn token_piece_bytes_from_policy(
    policy: Qwen3Dense06bTokenizerPolicy,
    token_id: u64,
) -> Vec<u8> {
    debug_assert_eq!(policy.synthetic_piece_prefix, "q3_");
    debug_assert_eq!(policy.synthetic_piece_digits, 6);
    format!("{}{token_id:06}", policy.synthetic_piece_prefix).into_bytes()
}

pub fn load_tokenizer_asset_summary(
    tokenizer_path: &Path,
) -> Result<Qwen3Dense06bTokenizerAssetSummary, String> {
    let tokenizer_dir = tokenizer_path;
    let tokenizer_config = read_tokenizer_asset_file(tokenizer_dir, "tokenizer_config.json")?;
    let tokenizer_json = read_tokenizer_asset_file(tokenizer_dir, "tokenizer.json")?;
    let vocab_json = read_tokenizer_asset_file(tokenizer_dir, "vocab.json")?;
    let merges = read_tokenizer_asset_file(tokenizer_dir, "merges.txt")?;
    let generation_config = read_tokenizer_asset_file(tokenizer_dir, "generation_config.json")?;
    let vocab_value: serde_json::Value = serde_json::from_slice(&vocab_json).map_err(|err| {
        format!(
            "qwen3_tokenizer_vocab_json_parse_failed:{}:{err}",
            tokenizer_dir.display()
        )
    })?;
    let vocab_entries = vocab_value
        .as_object()
        .ok_or_else(|| "qwen3_tokenizer_vocab_json_not_object".to_string())?
        .len() as u64;
    let config_value: serde_json::Value =
        serde_json::from_slice(&tokenizer_config).map_err(|err| {
            format!(
                "qwen3_tokenizer_config_json_parse_failed:{}:{err}",
                tokenizer_dir.display()
            )
        })?;
    let added_tokens = config_value
        .get("added_tokens_decoder")
        .and_then(serde_json::Value::as_object)
        .map(|tokens| tokens.len() as u64)
        .unwrap_or(0);
    let merge_rules = merges
        .split(|byte| *byte == b'\n')
        .filter(|line| !line.is_empty() && !line.starts_with(b"#"))
        .count() as u64;
    let files = vec![
        tokenizer_asset_file_summary("tokenizer_config.json", &tokenizer_config),
        tokenizer_asset_file_summary("tokenizer.json", &tokenizer_json),
        tokenizer_asset_file_summary("vocab.json", &vocab_json),
        tokenizer_asset_file_summary("merges.txt", &merges),
        tokenizer_asset_file_summary("generation_config.json", &generation_config),
    ];
    let aggregate_words = [
        QWEN3_DENSE_0_6B_PROFILE.vocab_size,
        vocab_entries,
        added_tokens,
        merge_rules,
        files.iter().map(|file| file.bytes).sum(),
        files.iter().fold(0xcbf2_9ce4_8422_2325u64, |acc, file| {
            acc.wrapping_mul(0x0000_0100_0000_01b3)
                ^ file.bytes.rotate_left(11)
                ^ file.checksum.rotate_left(23)
        }),
    ];
    Ok(Qwen3Dense06bTokenizerAssetSummary {
        model_id: "Qwen/Qwen3-0.6B".to_string(),
        source: tokenizer_dir.display().to_string(),
        vocab_size: QWEN3_DENSE_0_6B_PROFILE.vocab_size,
        vocab_entries,
        added_tokens,
        merge_rules,
        files,
        aggregate_checksum: checksum_words(&aggregate_words),
    })
}

pub fn token_piece_from_tokenizer_path(
    tokenizer_path: &Path,
    token_id: u64,
) -> Result<Qwen3Dense06bTokenPiece, String> {
    let piece_bytes = token_piece_bytes_from_tokenizer_path(tokenizer_path, token_id)?;
    Ok(token_piece_from_bytes(token_id, &piece_bytes))
}

pub fn token_piece_bytes_from_tokenizer_path(
    tokenizer_path: &Path,
    token_id: u64,
) -> Result<Vec<u8>, String> {
    let tokenizer_config = read_tokenizer_asset_file(tokenizer_path, "tokenizer_config.json")?;
    let tokenizer_json = read_tokenizer_asset_file(tokenizer_path, "tokenizer.json")?;
    let vocab_json = read_tokenizer_asset_file(tokenizer_path, "vocab.json")?;
    tokenizer_piece_bytes(&tokenizer_config, &tokenizer_json, &vocab_json, token_id)
}

pub fn token_piece_decode_bytes(piece: &[u8]) -> Vec<u8> {
    let Ok(piece) = std::str::from_utf8(piece) else {
        return piece.to_vec();
    };
    let byte_map = qwen3_tokenizer_bytes_to_unicode();
    let unicode_to_byte: BTreeMap<char, u8> = byte_map
        .iter()
        .enumerate()
        .map(|(byte, ch)| (*ch, byte as u8))
        .collect();
    let mut decoded = Vec::with_capacity(piece.len());
    for ch in piece.chars() {
        if let Some(byte) = unicode_to_byte.get(&ch) {
            decoded.push(*byte);
        } else {
            let mut buf = [0u8; 4];
            decoded.extend_from_slice(ch.encode_utf8(&mut buf).as_bytes());
        }
    }
    decoded
}

pub fn tokenize_prompt_from_tokenizer_path(
    tokenizer_path: &Path,
    prompt: &str,
) -> Result<Qwen3Dense06bPromptTokenization, String> {
    let tokenizer_json = read_tokenizer_asset_file(tokenizer_path, "tokenizer.json")?;
    let tokenizer_value: serde_json::Value = serde_json::from_slice(&tokenizer_json)
        .map_err(|err| format!("qwen3_tokenizer_json_parse_failed:{err}"))?;
    let model = tokenizer_value
        .get("model")
        .ok_or_else(|| "qwen3_tokenizer_model_missing".to_string())?;
    let vocab_value = model
        .get("vocab")
        .ok_or_else(|| "qwen3_tokenizer_model_vocab_missing".to_string())?;
    let vocab: BTreeMap<String, u64> = serde_json::from_value(vocab_value.clone())
        .map_err(|err| format!("qwen3_tokenizer_model_vocab_parse_failed:{err}"))?;
    let merge_ranks = qwen3_tokenizer_merge_ranks(model)?;
    let mut token_ids = Vec::new();
    for segment in qwen3_tokenizer_pretokenize(prompt) {
        let encoded = qwen3_tokenizer_byte_level_encode(&segment);
        let pieces = qwen3_tokenizer_bpe_pieces(&encoded, &merge_ranks);
        for piece in pieces {
            let token_id = vocab
                .get(&piece)
                .copied()
                .ok_or_else(|| format!("qwen3_tokenizer_piece_missing:{piece}"))?;
            token_ids.push(token_id);
        }
    }
    let token_checksum = prompt_token_ids_checksum(&token_ids);
    Ok(Qwen3Dense06bPromptTokenization {
        token_count: token_ids.len() as u64,
        token_ids,
        token_checksum,
    })
}

pub fn prompt_token_ids_checksum(token_ids: &[u64]) -> u64 {
    let mut words = Vec::with_capacity(token_ids.len() * 2 + 1);
    words.push(token_ids.len() as u64);
    for (index, token_id) in token_ids.iter().enumerate() {
        words.extend_from_slice(&[index as u64, *token_id]);
    }
    checksum_words(&words)
}

fn read_tokenizer_asset_file(tokenizer_dir: &Path, name: &str) -> Result<Vec<u8>, String> {
    let path = tokenizer_dir.join(name);
    fs::read(&path)
        .map_err(|err| format!("qwen3_tokenizer_asset_read_failed:{}:{err}", path.display()))
}

fn qwen3_tokenizer_merge_ranks(
    model: &serde_json::Value,
) -> Result<BTreeMap<(String, String), usize>, String> {
    let merges = model
        .get("merges")
        .and_then(serde_json::Value::as_array)
        .ok_or_else(|| "qwen3_tokenizer_model_merges_missing".to_string())?;
    let mut ranks = BTreeMap::new();
    for (rank, merge) in merges.iter().enumerate() {
        let pair = merge
            .as_array()
            .ok_or_else(|| "qwen3_tokenizer_merge_not_pair".to_string())?;
        if pair.len() != 2 {
            return Err("qwen3_tokenizer_merge_pair_len".to_string());
        }
        let left = pair[0]
            .as_str()
            .ok_or_else(|| "qwen3_tokenizer_merge_left_not_string".to_string())?;
        let right = pair[1]
            .as_str()
            .ok_or_else(|| "qwen3_tokenizer_merge_right_not_string".to_string())?;
        ranks.insert((left.to_string(), right.to_string()), rank);
    }
    Ok(ranks)
}

fn qwen3_tokenizer_pretokenize(prompt: &str) -> Vec<String> {
    let chars: Vec<char> = prompt.chars().collect();
    let mut segments = Vec::new();
    let mut index = 0usize;
    while index < chars.len() {
        if chars[index] == '\'' {
            let remaining: String = chars[index..].iter().take(4).collect();
            let lower = remaining.to_ascii_lowercase();
            if let Some(len) = ["'s", "'t", "'re", "'ve", "'m", "'ll", "'d"]
                .iter()
                .find_map(|suffix| lower.starts_with(suffix).then_some(suffix.len()))
            {
                segments.push(chars[index..index + len].iter().collect());
                index += len;
                continue;
            }
        }
        let mut start = index;
        if chars[index] == ' '
            && index + 1 < chars.len()
            && (chars[index + 1].is_alphabetic()
                || (!chars[index + 1].is_whitespace() && !chars[index + 1].is_numeric()))
        {
            index += 1;
        }
        if index < chars.len() && chars[index].is_alphabetic() {
            while index < chars.len() && chars[index].is_alphabetic() {
                index += 1;
            }
            segments.push(chars[start..index].iter().collect());
            continue;
        }
        if index < chars.len() && chars[index].is_numeric() {
            segments.push(chars[index..index + 1].iter().collect());
            index += 1;
            continue;
        }
        if index < chars.len() && chars[index].is_whitespace() {
            start = index;
            while index < chars.len() && chars[index].is_whitespace() {
                index += 1;
            }
            segments.push(chars[start..index].iter().collect());
            continue;
        }
        while index < chars.len()
            && !chars[index].is_whitespace()
            && !chars[index].is_alphabetic()
            && !chars[index].is_numeric()
        {
            index += 1;
        }
        segments.push(chars[start..index].iter().collect());
    }
    segments
}

fn qwen3_tokenizer_byte_level_encode(segment: &str) -> String {
    let byte_map = qwen3_tokenizer_bytes_to_unicode();
    segment
        .as_bytes()
        .iter()
        .map(|byte| byte_map[*byte as usize])
        .collect()
}

fn qwen3_tokenizer_bytes_to_unicode() -> [char; 256] {
    let mut chars = ['\0'; 256];
    let mut visible = Vec::new();
    visible.extend(33u32..=126u32);
    visible.extend(161u32..=172u32);
    visible.extend(174u32..=255u32);
    for byte in &visible {
        chars[*byte as usize] = char::from_u32(*byte).expect("visible byte unicode");
    }
    let mut extra = 0u32;
    for byte in 0u32..=255u32 {
        if !visible.contains(&byte) {
            chars[byte as usize] =
                char::from_u32(256 + extra).expect("byte-level unicode extension");
            extra += 1;
        }
    }
    chars
}

fn qwen3_tokenizer_bpe_pieces(
    encoded: &str,
    merge_ranks: &BTreeMap<(String, String), usize>,
) -> Vec<String> {
    let mut pieces: Vec<String> = encoded.chars().map(|ch| ch.to_string()).collect();
    while pieces.len() > 1 {
        let Some((best_index, _)) = pieces
            .windows(2)
            .enumerate()
            .filter_map(|(index, pair)| {
                merge_ranks
                    .get(&(pair[0].clone(), pair[1].clone()))
                    .map(|rank| (index, *rank))
            })
            .min_by_key(|(_, rank)| *rank)
        else {
            break;
        };
        let merged = format!("{}{}", pieces[best_index], pieces[best_index + 1]);
        pieces.splice(best_index..best_index + 2, [merged]);
    }
    pieces
}

fn tokenizer_asset_file_summary(
    name: impl Into<String>,
    bytes: &[u8],
) -> Qwen3Dense06bTokenizerAssetFileSummary {
    Qwen3Dense06bTokenizerAssetFileSummary {
        name: name.into(),
        bytes: bytes.len() as u64,
        checksum: weight_bytes_checksum(bytes),
    }
}

fn tokenizer_piece_bytes(
    tokenizer_config: &[u8],
    tokenizer_json: &[u8],
    vocab_json: &[u8],
    token_id: u64,
) -> Result<Vec<u8>, String> {
    let tokenizer_value: serde_json::Value = serde_json::from_slice(tokenizer_json)
        .map_err(|err| format!("qwen3_tokenizer_json_parse_failed:{err}"))?;
    if let Some(piece) = tokenizer_value
        .get("model")
        .and_then(|model| model.get("vocab"))
        .and_then(serde_json::Value::as_object)
        .and_then(|vocab| {
            vocab
                .iter()
                .find_map(|(piece, id)| (id.as_u64() == Some(token_id)).then_some(piece.as_str()))
        })
    {
        return Ok(piece.as_bytes().to_vec());
    }
    if let Some(piece) = tokenizer_value
        .get("added_tokens")
        .and_then(serde_json::Value::as_array)
        .and_then(|tokens| {
            tokens.iter().find_map(|token| {
                (token.get("id").and_then(serde_json::Value::as_u64) == Some(token_id))
                    .then(|| token.get("content").and_then(serde_json::Value::as_str))
                    .flatten()
            })
        })
    {
        return Ok(piece.as_bytes().to_vec());
    }
    let vocab: BTreeMap<String, u64> = serde_json::from_slice(vocab_json)
        .map_err(|err| format!("qwen3_tokenizer_vocab_json_parse_failed:{err}"))?;
    if let Some((piece, _)) = vocab.iter().find(|(_, id)| **id == token_id) {
        return Ok(piece.as_bytes().to_vec());
    }
    let config_value: serde_json::Value = serde_json::from_slice(tokenizer_config)
        .map_err(|err| format!("qwen3_tokenizer_config_json_parse_failed:{err}"))?;
    let token_key = token_id.to_string();
    config_value
        .get("added_tokens_decoder")
        .and_then(|decoder| decoder.get(&token_key))
        .and_then(|token| token.get("content"))
        .and_then(serde_json::Value::as_str)
        .map(|piece| piece.as_bytes().to_vec())
        .ok_or_else(|| format!("qwen3_tokenizer_token_missing:{token_id}"))
}

fn token_piece_from_bytes(token_id: u64, piece: &[u8]) -> Qwen3Dense06bTokenPiece {
    let mut bytes = [0u8; 16];
    let copy_len = piece.len().min(bytes.len());
    bytes[..copy_len].copy_from_slice(&piece[..copy_len]);
    Qwen3Dense06bTokenPiece {
        token_id,
        byte_len: piece.len() as u64,
        word0: u64::from_le_bytes(bytes[0..8].try_into().expect("token piece word0")),
        word1: u64::from_le_bytes(bytes[8..16].try_into().expect("token piece word1")),
        checksum: token_piece_checksum(token_id, piece),
    }
}

fn token_piece_checksum(token_id: u64, piece: &[u8]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64 ^ token_id;
    for byte in piece {
        acc ^= *byte as u64;
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc ^ (piece.len() as u64).rotate_left(17)
}

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
    QNorm,
    KProj,
    KNorm,
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

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bWeightSliceValidation {
    pub bytes: u64,
    pub checksum: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bQkvReferenceValidation {
    pub layer_id: u64,
    pub shard_id: u64,
    pub hidden_size: u64,
    pub rmsnorm_checksum: u64,
    pub rmsnorm_sample_words: [u64; 4],
    pub q_weight_checksum: u64,
    pub k_weight_checksum: u64,
    pub v_weight_checksum: u64,
    pub q_output_checksum: u64,
    pub q_output_sample_words: [u64; 4],
    pub k_output_checksum: u64,
    pub k_output_sample_words: [u64; 4],
    pub v_output_checksum: u64,
    pub v_output_sample_words: [u64; 4],
    pub q_rows: u64,
    pub k_rows: u64,
    pub v_rows: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bReferenceWeightSliceValidation {
    pub kind: Qwen3Dense06bWeightTensorKind,
    pub shape: Vec<u64>,
    pub slice_axis: Option<u64>,
    pub slice_start: u64,
    pub slice_end: u64,
    pub bytes: u64,
    pub checksum: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bQkvReferenceShardSummary {
    pub shard_id: u64,
    pub hidden_size: u64,
    pub rmsnorm_checksum: u64,
    pub rmsnorm_sample_words: [u64; 4],
    pub q_weight_checksum: u64,
    pub k_weight_checksum: u64,
    pub v_weight_checksum: u64,
    pub q_output_checksum: u64,
    pub q_output_sample_words: [u64; 4],
    pub k_output_checksum: u64,
    pub k_output_sample_words: [u64; 4],
    pub v_output_checksum: u64,
    pub v_output_sample_words: [u64; 4],
    pub q_rows: u64,
    pub k_rows: u64,
    pub v_rows: u64,
    pub weight_slices: Vec<Qwen3Dense06bReferenceWeightSliceValidation>,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bQkvReferenceLayerSummary {
    pub layer_id: u64,
    pub shards: Vec<Qwen3Dense06bQkvReferenceShardSummary>,
    pub shard_count: u64,
    pub total_weight_bytes: u64,
    pub total_q_rows: u64,
    pub total_k_rows: u64,
    pub total_v_rows: u64,
    pub aggregate_checksum: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bQkvReferenceShardValues {
    pub shard_id: u64,
    pub hidden_size: u64,
    pub q_rows: u64,
    pub k_rows: u64,
    pub v_rows: u64,
    pub rmsnorm: Vec<f32>,
    pub q_output: Vec<f32>,
    pub k_output: Vec<f32>,
    pub v_output: Vec<f32>,
    pub rmsnorm_checksum: u64,
    pub q_output_checksum: u64,
    pub k_output_checksum: u64,
    pub v_output_checksum: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bQkvReferenceLayerValues {
    pub layer_id: u64,
    pub shard_count: u64,
    pub shards: Vec<Qwen3Dense06bQkvReferenceShardValues>,
    pub aggregate_checksum: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Qwen3Dense06bMlpReferenceValidation {
    pub layer_id: u64,
    pub shard_id: u64,
    pub hidden_size: u64,
    pub intermediate_rows: u64,
    pub gate_weight_checksum: u64,
    pub up_weight_checksum: u64,
    pub down_weight_checksum: u64,
    pub gate_output_checksum: u64,
    pub gate_output_sample_words: [u64; 4],
    pub up_output_checksum: u64,
    pub up_output_sample_words: [u64; 4],
    pub activation_checksum: u64,
    pub activation_sample_words: [u64; 4],
    pub down_output_checksum: u64,
    pub down_output_sample_words: [u64; 4],
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bMlpReferenceShardSummary {
    pub shard_id: u64,
    pub hidden_size: u64,
    pub intermediate_rows: u64,
    pub gate_weight_checksum: u64,
    pub up_weight_checksum: u64,
    pub down_weight_checksum: u64,
    pub gate_output_checksum: u64,
    pub gate_output_sample_words: [u64; 4],
    pub up_output_checksum: u64,
    pub up_output_sample_words: [u64; 4],
    pub activation_checksum: u64,
    pub activation_sample_words: [u64; 4],
    pub down_output_checksum: u64,
    pub down_output_sample_words: [u64; 4],
    pub weight_slices: Vec<Qwen3Dense06bReferenceWeightSliceValidation>,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bMlpReferenceLayerSummary {
    pub layer_id: u64,
    pub shards: Vec<Qwen3Dense06bMlpReferenceShardSummary>,
    pub shard_count: u64,
    pub total_weight_bytes: u64,
    pub total_intermediate_rows: u64,
    pub aggregate_checksum: u64,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bLogitsReferenceTokenSummary {
    pub step_index: u64,
    pub token_id: u64,
    pub row_bytes: u64,
    pub row_checksum: u64,
    pub logit_bits: u64,
    pub logit_checksum: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bLogitsReferenceSummary {
    pub model_id: String,
    pub source: String,
    pub vocab_size: u64,
    pub hidden_size: u64,
    pub final_norm_bytes: u64,
    pub final_norm_checksum: u64,
    pub token_count: u64,
    pub aggregate_checksum: u64,
    pub tokens: Vec<Qwen3Dense06bLogitsReferenceTokenSummary>,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bEmbeddingReferenceTokenSummary {
    pub sequence_index: u64,
    pub token_id: u64,
    pub row_bytes: u64,
    pub row_checksum: u64,
    pub value_checksum: u64,
    pub sample_words: [u64; 4],
}

#[derive(Clone, Debug, Serialize, Deserialize, Eq, PartialEq)]
pub struct Qwen3Dense06bEmbeddingReferenceSummary {
    pub model_id: String,
    pub source: String,
    pub vocab_size: u64,
    pub hidden_size: u64,
    pub token_count: u64,
    pub row_byte_count: u64,
    pub row_checksum: u64,
    pub value_checksum: u64,
    pub aggregate_checksum: u64,
    pub tokens: Vec<Qwen3Dense06bEmbeddingReferenceTokenSummary>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bLayerForwardReference {
    pub layer_id: u64,
    pub position: u64,
    pub hidden_size: u64,
    pub input_checksum: u64,
    pub q_checksum: u64,
    pub k_checksum: u64,
    pub v_checksum: u64,
    pub rope_q_checksum: u64,
    pub rope_k_checksum: u64,
    pub attention_context_checksum: u64,
    pub attention_output_checksum: u64,
    pub attention_residual_checksum: u64,
    pub mlp_gate_checksum: u64,
    pub mlp_up_checksum: u64,
    pub mlp_activation_checksum: u64,
    pub mlp_down_checksum: u64,
    pub output_checksum: u64,
    pub output_sample_words: [u64; 4],
    pub output: Vec<f32>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bForwardReference {
    pub layer_count: u64,
    pub position: u64,
    pub hidden_size: u64,
    pub input_checksum: u64,
    pub final_hidden_checksum: u64,
    pub final_hidden_sample_words: [u64; 4],
    pub aggregate_checksum: u64,
    pub final_hidden: Vec<f32>,
    pub layers: Vec<Qwen3Dense06bLayerForwardReference>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bLayerKvCache {
    pub layer_id: u64,
    pub token_count: u64,
    pub rope_k_states: Vec<Vec<f32>>,
    pub v_states: Vec<Vec<f32>>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bForwardWithKvCache {
    pub forward: Qwen3Dense06bForwardReference,
    pub kv_cache: Vec<Qwen3Dense06bLayerKvCache>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bLogitCandidate {
    pub rank: u64,
    pub token_id: u64,
    pub logit_bits: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bFullVocabLogitsSummary {
    pub vocab_size: u64,
    pub hidden_size: u64,
    pub final_norm_checksum: u64,
    pub checked_token_count: u64,
    pub top_token_id: u64,
    pub top_logit_bits: u64,
    pub runner_up_token_id: u64,
    pub runner_up_logit_bits: u64,
    pub top_candidates: Vec<Qwen3Dense06bLogitCandidate>,
    pub logits_checksum: u64,
    pub aggregate_checksum: u64,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bSampledTextReference {
    pub token_id: u64,
    pub byte_len: u64,
    pub byte_checksum: u64,
    pub text_lossy: String,
    pub bytes: Vec<u8>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Qwen3Dense06bRealInferenceReference {
    pub token_ids: Vec<u64>,
    pub forward: Qwen3Dense06bForwardReference,
    pub logits: Qwen3Dense06bFullVocabLogitsSummary,
    pub sampled_text: Qwen3Dense06bSampledTextReference,
    pub aggregate_checksum: u64,
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

pub fn layer_graph_ir(profile: Qwen3Dense06bProfile, layer_id: u64) -> Qwen3Dense06bLayerGraphIr {
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
    if profile.num_key_value_heads % profile.tp_nodes != 0 {
        return Err(format!(
            "qwen3_dense_kv_heads_not_divisible:kv_heads={}:tp={}",
            profile.num_key_value_heads, profile.tp_nodes
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
        let data_offsets = match tensor
            .get("data_offsets")
            .and_then(serde_json::Value::as_array)
        {
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
    let mut file = File::open(path)
        .map_err(|err| format!("qwen3_safetensors_open_failed:{}:{err}", path.display()))?;
    let file_len = file
        .metadata()
        .map_err(|err| format!("qwen3_safetensors_metadata_failed:{}:{err}", path.display()))?
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
    let index: Qwen3Dense06bSafetensorsIndex =
        serde_json::from_str(&index_text).map_err(|err| {
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
                format!("qwen3_safetensors_index_tensor_missing:{tensor_name}:file={file_name}")
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
    weight_manifest_from_metadata_for_model(topology, "Qwen/Qwen3-0.6B", profile, source, tensors)
}

pub fn weight_manifest_from_metadata_for_model(
    topology: &SimTopology,
    model_id: impl Into<String>,
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
        model_id: model_id.into(),
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
                vec![
                    profile.num_attention_heads * profile.head_dim,
                    profile.hidden_size,
                ],
            ),
            (
                format!("{layer_prefix}.self_attn.q_norm.weight"),
                vec![profile.head_dim],
            ),
            (
                format!("{layer_prefix}.self_attn.k_proj.weight"),
                vec![
                    profile.num_key_value_heads * profile.head_dim,
                    profile.hidden_size,
                ],
            ),
            (
                format!("{layer_prefix}.self_attn.k_norm.weight"),
                vec![profile.head_dim],
            ),
            (
                format!("{layer_prefix}.self_attn.v_proj.weight"),
                vec![
                    profile.num_key_value_heads * profile.head_dim,
                    profile.hidden_size,
                ],
            ),
            (
                format!("{layer_prefix}.self_attn.o_proj.weight"),
                vec![
                    profile.hidden_size,
                    profile.num_attention_heads * profile.head_dim,
                ],
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
            vec![
                profile.num_attention_heads * profile.head_dim,
                profile.hidden_size,
            ],
            Some(0),
            shard.q_head_start * profile.head_dim,
            shard.q_head_end * profile.head_dim,
        ),
        (
            Qwen3Dense06bWeightTensorKind::QNorm,
            format!("model.layers.{layer_id}.self_attn.q_norm.weight"),
            vec![profile.head_dim],
            None,
            0,
            profile.head_dim,
        ),
        (
            Qwen3Dense06bWeightTensorKind::KProj,
            format!("model.layers.{layer_id}.self_attn.k_proj.weight"),
            vec![
                profile.num_key_value_heads * profile.head_dim,
                profile.hidden_size,
            ],
            Some(0),
            shard.kv_head_start * profile.head_dim,
            shard.kv_head_end * profile.head_dim,
        ),
        (
            Qwen3Dense06bWeightTensorKind::KNorm,
            format!("model.layers.{layer_id}.self_attn.k_norm.weight"),
            vec![profile.head_dim],
            None,
            0,
            profile.head_dim,
        ),
        (
            Qwen3Dense06bWeightTensorKind::VProj,
            format!("model.layers.{layer_id}.self_attn.v_proj.weight"),
            vec![
                profile.num_key_value_heads * profile.head_dim,
                profile.hidden_size,
            ],
            Some(0),
            shard.kv_head_start * profile.head_dim,
            shard.kv_head_end * profile.head_dim,
        ),
        (
            Qwen3Dense06bWeightTensorKind::OProj,
            format!("model.layers.{layer_id}.self_attn.o_proj.weight"),
            vec![
                profile.hidden_size,
                profile.num_attention_heads * profile.head_dim,
            ],
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
    let offsets = metadata
        .data_offsets
        .ok_or_else(|| format!("qwen3_weight_payload_offsets_missing:{}", slice.tensor_name))?;
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
            let row_bytes = inner_elems.checked_mul(element_size).ok_or_else(|| {
                format!("qwen3_weight_payload_row_overflow:{}", slice.tensor_name)
            })?;
            let start = tensor_base
                .checked_add(slice.slice_start.checked_mul(row_bytes).ok_or_else(|| {
                    format!("qwen3_weight_payload_start_overflow:{}", slice.tensor_name)
                })?)
                .ok_or_else(|| {
                    format!("qwen3_weight_payload_start_overflow:{}", slice.tensor_name)
                })?;
            let bytes = (slice.slice_end - slice.slice_start)
                .checked_mul(row_bytes)
                .ok_or_else(|| {
                    format!("qwen3_weight_payload_bytes_overflow:{}", slice.tensor_name)
                })?;
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
            let row_stride = cols.checked_mul(element_size).ok_or_else(|| {
                format!("qwen3_weight_payload_stride_overflow:{}", slice.tensor_name)
            })?;
            let slice_bytes = slice_cols.checked_mul(element_size).ok_or_else(|| {
                format!("qwen3_weight_payload_slice_overflow:{}", slice.tensor_name)
            })?;
            let mut file = File::open(source_file)
                .map_err(|err| format!("qwen3_weight_payload_open_failed:{source_file}:{err}"))?;
            let mut out = Vec::with_capacity((rows * slice_bytes) as usize);
            for row in 0..rows {
                let offset = tensor_base
                    .checked_add(row.checked_mul(row_stride).ok_or_else(|| {
                        format!(
                            "qwen3_weight_payload_row_offset_overflow:{}",
                            slice.tensor_name
                        )
                    })?)
                    .and_then(|base| base.checked_add(slice.slice_start.checked_mul(element_size)?))
                    .ok_or_else(|| {
                        format!(
                            "qwen3_weight_payload_row_offset_overflow:{}",
                            slice.tensor_name
                        )
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

pub fn materialize_full_weight_tensor_payload(
    tensor_name: &str,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
) -> Result<Vec<u8>, String> {
    let tensor = tensors
        .get(tensor_name)
        .ok_or_else(|| format!("qwen3_dense_0_6b_missing_weight_tensor:{tensor_name}"))?;
    materialize_full_tensor_payload(tensor_name, tensor)
}

pub fn weight_slice_validation(
    slice: &Qwen3Dense06bWeightSlice,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
) -> Result<Qwen3Dense06bWeightSliceValidation, String> {
    let payload = materialize_weight_slice_payload(slice, tensors)?;
    Ok(Qwen3Dense06bWeightSliceValidation {
        bytes: payload.len() as u64,
        checksum: weight_bytes_checksum(&payload),
    })
}

pub fn qkv_reference_validation(
    manifest: &Qwen3Dense06bWeightManifest,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
    shard_id: u64,
) -> Result<Qwen3Dense06bQkvReferenceValidation, String> {
    let norm_slice = find_weight_slice(
        manifest,
        layer_id,
        shard_id,
        Qwen3Dense06bWeightTensorKind::InputLayerNorm,
    )?;
    let q_slice = find_weight_slice(
        manifest,
        layer_id,
        shard_id,
        Qwen3Dense06bWeightTensorKind::QProj,
    )?;
    let k_slice = find_weight_slice(
        manifest,
        layer_id,
        shard_id,
        Qwen3Dense06bWeightTensorKind::KProj,
    )?;
    let v_slice = find_weight_slice(
        manifest,
        layer_id,
        shard_id,
        Qwen3Dense06bWeightTensorKind::VProj,
    )?;
    let norm_payload = materialize_weight_slice_payload(norm_slice, tensors)?;
    let q_payload = materialize_weight_slice_payload(q_slice, tensors)?;
    let k_payload = materialize_weight_slice_payload(k_slice, tensors)?;
    let v_payload = materialize_weight_slice_payload(v_slice, tensors)?;
    let reference = qkv_reference_from_payloads(
        norm_slice.dtype,
        q_slice.dtype,
        k_slice.dtype,
        v_slice.dtype,
        norm_slice.local_shape[0],
        q_slice.local_shape[0],
        k_slice.local_shape[0],
        v_slice.local_shape[0],
        &norm_payload,
        &q_payload,
        &k_payload,
        &v_payload,
    )?;
    Ok(Qwen3Dense06bQkvReferenceValidation {
        layer_id,
        shard_id,
        hidden_size: norm_slice.local_shape[0],
        rmsnorm_checksum: reference.rmsnorm_checksum,
        rmsnorm_sample_words: reference.rmsnorm_sample_words,
        q_weight_checksum: weight_bytes_checksum(&q_payload),
        k_weight_checksum: weight_bytes_checksum(&k_payload),
        v_weight_checksum: weight_bytes_checksum(&v_payload),
        q_output_checksum: reference.q_output_checksum,
        q_output_sample_words: reference.q_output_sample_words,
        k_output_checksum: reference.k_output_checksum,
        k_output_sample_words: reference.k_output_sample_words,
        v_output_checksum: reference.v_output_checksum,
        v_output_sample_words: reference.v_output_sample_words,
        q_rows: q_slice.local_shape[0],
        k_rows: k_slice.local_shape[0],
        v_rows: v_slice.local_shape[0],
    })
}

pub fn qkv_reference_layer_summary(
    manifest: &Qwen3Dense06bWeightManifest,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
) -> Result<Qwen3Dense06bQkvReferenceLayerSummary, String> {
    let mut shards = Vec::new();
    let mut aggregate_words = Vec::new();
    let mut total_weight_bytes = 0u64;
    let mut total_q_rows = 0u64;
    let mut total_k_rows = 0u64;
    let mut total_v_rows = 0u64;
    for shard_id in 0..manifest.profile.tp_nodes {
        let reference = qkv_reference_validation(manifest, tensors, layer_id, shard_id)?;
        total_q_rows += reference.q_rows;
        total_k_rows += reference.k_rows;
        total_v_rows += reference.v_rows;
        aggregate_words.extend_from_slice(&[
            reference.rmsnorm_checksum,
            reference.q_weight_checksum,
            reference.k_weight_checksum,
            reference.v_weight_checksum,
            reference.q_output_checksum,
            reference.k_output_checksum,
            reference.v_output_checksum,
        ]);
        aggregate_words.extend_from_slice(&reference.rmsnorm_sample_words);
        aggregate_words.extend_from_slice(&reference.q_output_sample_words);
        aggregate_words.extend_from_slice(&reference.k_output_sample_words);
        aggregate_words.extend_from_slice(&reference.v_output_sample_words);
        let mut weight_slices = Vec::new();
        for kind in [
            Qwen3Dense06bWeightTensorKind::InputLayerNorm,
            Qwen3Dense06bWeightTensorKind::QProj,
            Qwen3Dense06bWeightTensorKind::KProj,
            Qwen3Dense06bWeightTensorKind::VProj,
        ] {
            let slice = find_weight_slice(manifest, layer_id, shard_id, kind)?;
            let validation = weight_slice_validation(slice, tensors)?;
            total_weight_bytes += validation.bytes;
            weight_slices.push(Qwen3Dense06bReferenceWeightSliceValidation {
                kind,
                shape: slice.local_shape.clone(),
                slice_axis: slice.slice_axis,
                slice_start: slice.slice_start,
                slice_end: slice.slice_end,
                bytes: validation.bytes,
                checksum: validation.checksum,
            });
        }
        shards.push(Qwen3Dense06bQkvReferenceShardSummary {
            shard_id: reference.shard_id,
            hidden_size: reference.hidden_size,
            rmsnorm_checksum: reference.rmsnorm_checksum,
            rmsnorm_sample_words: reference.rmsnorm_sample_words,
            q_weight_checksum: reference.q_weight_checksum,
            k_weight_checksum: reference.k_weight_checksum,
            v_weight_checksum: reference.v_weight_checksum,
            q_output_checksum: reference.q_output_checksum,
            q_output_sample_words: reference.q_output_sample_words,
            k_output_checksum: reference.k_output_checksum,
            k_output_sample_words: reference.k_output_sample_words,
            v_output_checksum: reference.v_output_checksum,
            v_output_sample_words: reference.v_output_sample_words,
            q_rows: reference.q_rows,
            k_rows: reference.k_rows,
            v_rows: reference.v_rows,
            weight_slices,
        });
    }
    Ok(Qwen3Dense06bQkvReferenceLayerSummary {
        layer_id,
        shard_count: manifest.profile.tp_nodes,
        total_weight_bytes,
        total_q_rows,
        total_k_rows,
        total_v_rows,
        aggregate_checksum: checksum_words(&aggregate_words),
        shards,
    })
}

pub fn qkv_reference_layer_values(
    manifest: &Qwen3Dense06bWeightManifest,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
) -> Result<Qwen3Dense06bQkvReferenceLayerValues, String> {
    qkv_reference_layer_values_with_hidden(manifest, tensors, layer_id, None)
}

pub fn qkv_reference_layer_values_with_hidden(
    manifest: &Qwen3Dense06bWeightManifest,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
    hidden: Option<&[f32]>,
) -> Result<Qwen3Dense06bQkvReferenceLayerValues, String> {
    let mut shards = Vec::new();
    let mut aggregate_words = Vec::new();
    for shard_id in 0..manifest.profile.tp_nodes {
        let norm_slice = find_weight_slice(
            manifest,
            layer_id,
            shard_id,
            Qwen3Dense06bWeightTensorKind::InputLayerNorm,
        )?;
        let q_slice = find_weight_slice(
            manifest,
            layer_id,
            shard_id,
            Qwen3Dense06bWeightTensorKind::QProj,
        )?;
        let k_slice = find_weight_slice(
            manifest,
            layer_id,
            shard_id,
            Qwen3Dense06bWeightTensorKind::KProj,
        )?;
        let v_slice = find_weight_slice(
            manifest,
            layer_id,
            shard_id,
            Qwen3Dense06bWeightTensorKind::VProj,
        )?;
        let norm_payload = materialize_weight_slice_payload(norm_slice, tensors)?;
        let q_payload = materialize_weight_slice_payload(q_slice, tensors)?;
        let k_payload = materialize_weight_slice_payload(k_slice, tensors)?;
        let v_payload = materialize_weight_slice_payload(v_slice, tensors)?;
        let values = qkv_reference_values_from_payloads(
            norm_slice.dtype,
            q_slice.dtype,
            k_slice.dtype,
            v_slice.dtype,
            norm_slice.local_shape[0],
            q_slice.local_shape[0],
            k_slice.local_shape[0],
            v_slice.local_shape[0],
            &norm_payload,
            &q_payload,
            &k_payload,
            &v_payload,
            hidden,
        )?;
        aggregate_words.extend_from_slice(&[
            values.rmsnorm_checksum,
            values.q_output_checksum,
            values.k_output_checksum,
            values.v_output_checksum,
        ]);
        shards.push(Qwen3Dense06bQkvReferenceShardValues {
            shard_id,
            hidden_size: norm_slice.local_shape[0],
            q_rows: q_slice.local_shape[0],
            k_rows: k_slice.local_shape[0],
            v_rows: v_slice.local_shape[0],
            rmsnorm: values.rmsnorm,
            q_output: values.q_output,
            k_output: values.k_output,
            v_output: values.v_output,
            rmsnorm_checksum: values.rmsnorm_checksum,
            q_output_checksum: values.q_output_checksum,
            k_output_checksum: values.k_output_checksum,
            v_output_checksum: values.v_output_checksum,
        });
    }
    Ok(Qwen3Dense06bQkvReferenceLayerValues {
        layer_id,
        shard_count: manifest.profile.tp_nodes,
        aggregate_checksum: checksum_words(&aggregate_words),
        shards,
    })
}

pub fn mlp_reference_validation(
    manifest: &Qwen3Dense06bWeightManifest,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
    shard_id: u64,
) -> Result<Qwen3Dense06bMlpReferenceValidation, String> {
    mlp_reference_validation_with_hidden(manifest, tensors, layer_id, shard_id, None)
}

fn mlp_reference_validation_with_hidden(
    manifest: &Qwen3Dense06bWeightManifest,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
    shard_id: u64,
    hidden: Option<&[f32]>,
) -> Result<Qwen3Dense06bMlpReferenceValidation, String> {
    let gate_slice = find_weight_slice(
        manifest,
        layer_id,
        shard_id,
        Qwen3Dense06bWeightTensorKind::GateProj,
    )?;
    let up_slice = find_weight_slice(
        manifest,
        layer_id,
        shard_id,
        Qwen3Dense06bWeightTensorKind::UpProj,
    )?;
    let down_slice = find_weight_slice(
        manifest,
        layer_id,
        shard_id,
        Qwen3Dense06bWeightTensorKind::DownProj,
    )?;
    let gate_payload = materialize_weight_slice_payload(gate_slice, tensors)?;
    let up_payload = materialize_weight_slice_payload(up_slice, tensors)?;
    let down_payload = materialize_weight_slice_payload(down_slice, tensors)?;
    let reference = mlp_reference_from_payloads(
        gate_slice.dtype,
        up_slice.dtype,
        down_slice.dtype,
        gate_slice.local_shape[1],
        gate_slice.local_shape[0],
        &gate_payload,
        &up_payload,
        &down_payload,
        hidden,
    )?;
    Ok(Qwen3Dense06bMlpReferenceValidation {
        layer_id,
        shard_id,
        hidden_size: gate_slice.local_shape[1],
        intermediate_rows: gate_slice.local_shape[0],
        gate_weight_checksum: weight_bytes_checksum(&gate_payload),
        up_weight_checksum: weight_bytes_checksum(&up_payload),
        down_weight_checksum: weight_bytes_checksum(&down_payload),
        gate_output_checksum: reference.gate_output_checksum,
        gate_output_sample_words: reference.gate_output_sample_words,
        up_output_checksum: reference.up_output_checksum,
        up_output_sample_words: reference.up_output_sample_words,
        activation_checksum: reference.activation_checksum,
        activation_sample_words: reference.activation_sample_words,
        down_output_checksum: reference.down_output_checksum,
        down_output_sample_words: reference.down_output_sample_words,
    })
}

pub fn mlp_reference_layer_summary(
    manifest: &Qwen3Dense06bWeightManifest,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
) -> Result<Qwen3Dense06bMlpReferenceLayerSummary, String> {
    mlp_reference_layer_summary_with_hidden(manifest, tensors, layer_id, None)
}

pub fn mlp_reference_layer_summary_with_hidden(
    manifest: &Qwen3Dense06bWeightManifest,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
    hidden: Option<&[f32]>,
) -> Result<Qwen3Dense06bMlpReferenceLayerSummary, String> {
    let mut shards = Vec::new();
    let mut aggregate_words = Vec::new();
    let mut total_weight_bytes = 0u64;
    let mut total_intermediate_rows = 0u64;
    for shard_id in 0..manifest.profile.tp_nodes {
        let reference =
            mlp_reference_validation_with_hidden(manifest, tensors, layer_id, shard_id, hidden)?;
        total_intermediate_rows += reference.intermediate_rows;
        aggregate_words.extend_from_slice(&[
            reference.gate_weight_checksum,
            reference.up_weight_checksum,
            reference.down_weight_checksum,
            reference.gate_output_checksum,
            reference.up_output_checksum,
            reference.activation_checksum,
            reference.down_output_checksum,
        ]);
        aggregate_words.extend_from_slice(&reference.gate_output_sample_words);
        aggregate_words.extend_from_slice(&reference.up_output_sample_words);
        aggregate_words.extend_from_slice(&reference.activation_sample_words);
        aggregate_words.extend_from_slice(&reference.down_output_sample_words);
        let mut weight_slices = Vec::new();
        for kind in [
            Qwen3Dense06bWeightTensorKind::GateProj,
            Qwen3Dense06bWeightTensorKind::UpProj,
            Qwen3Dense06bWeightTensorKind::DownProj,
        ] {
            let slice = find_weight_slice(manifest, layer_id, shard_id, kind)?;
            let validation = weight_slice_validation(slice, tensors)?;
            total_weight_bytes += validation.bytes;
            weight_slices.push(Qwen3Dense06bReferenceWeightSliceValidation {
                kind,
                shape: slice.local_shape.clone(),
                slice_axis: slice.slice_axis,
                slice_start: slice.slice_start,
                slice_end: slice.slice_end,
                bytes: validation.bytes,
                checksum: validation.checksum,
            });
        }
        shards.push(Qwen3Dense06bMlpReferenceShardSummary {
            shard_id: reference.shard_id,
            hidden_size: reference.hidden_size,
            intermediate_rows: reference.intermediate_rows,
            gate_weight_checksum: reference.gate_weight_checksum,
            up_weight_checksum: reference.up_weight_checksum,
            down_weight_checksum: reference.down_weight_checksum,
            gate_output_checksum: reference.gate_output_checksum,
            gate_output_sample_words: reference.gate_output_sample_words,
            up_output_checksum: reference.up_output_checksum,
            up_output_sample_words: reference.up_output_sample_words,
            activation_checksum: reference.activation_checksum,
            activation_sample_words: reference.activation_sample_words,
            down_output_checksum: reference.down_output_checksum,
            down_output_sample_words: reference.down_output_sample_words,
            weight_slices,
        });
    }
    Ok(Qwen3Dense06bMlpReferenceLayerSummary {
        layer_id,
        shard_count: manifest.profile.tp_nodes,
        total_weight_bytes,
        total_intermediate_rows,
        aggregate_checksum: checksum_words(&aggregate_words),
        shards,
    })
}

pub fn logits_reference_summary(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_requests: &[(u64, u64)],
) -> Result<Qwen3Dense06bLogitsReferenceSummary, String> {
    logits_reference_summary_with_hidden(tensors, token_requests, None)
}

pub fn logits_reference_summary_with_hidden(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_requests: &[(u64, u64)],
    hidden: Option<&[f32]>,
) -> Result<Qwen3Dense06bLogitsReferenceSummary, String> {
    logits_reference_summary_with_hidden_and_payloads(tensors, None, token_requests, hidden)
}

pub fn logits_reference_summary_with_hidden_and_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    token_requests: &[(u64, u64)],
    hidden: Option<&[f32]>,
) -> Result<Qwen3Dense06bLogitsReferenceSummary, String> {
    let norm = tensors
        .get("model.norm.weight")
        .ok_or_else(|| "qwen3_dense_0_6b_missing_weight_tensor:model.norm.weight".to_string())?;
    let lm_head = tensors
        .get("lm_head.weight")
        .ok_or_else(|| "qwen3_dense_0_6b_missing_weight_tensor:lm_head.weight".to_string())?;
    if norm.shape != vec![QWEN3_DENSE_0_6B_PROFILE.hidden_size] {
        return Err(format!(
            "qwen3_dense_0_6b_weight_shape_mismatch:model.norm.weight:got={:?}:expected={:?}",
            norm.shape,
            vec![QWEN3_DENSE_0_6B_PROFILE.hidden_size]
        ));
    }
    if lm_head.shape
        != vec![
            QWEN3_DENSE_0_6B_PROFILE.vocab_size,
            QWEN3_DENSE_0_6B_PROFILE.hidden_size,
        ]
    {
        return Err(format!(
            "qwen3_dense_0_6b_weight_shape_mismatch:lm_head.weight:got={:?}:expected={:?}",
            lm_head.shape,
            vec![
                QWEN3_DENSE_0_6B_PROFILE.vocab_size,
                QWEN3_DENSE_0_6B_PROFILE.hidden_size
            ]
        ));
    }
    let hidden_size = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize;
    let norm_payload =
        materialize_full_tensor_payload_with_payloads("model.norm.weight", norm, tensor_payloads)?;
    let norm_weight = decode_weight_vector(norm.dtype, &norm_payload, hidden_size)?;
    let hidden = match hidden {
        Some(hidden) if hidden.len() == hidden_size => Some(hidden),
        Some(hidden) => {
            return Err(format!(
                "qwen3_logits_reference_hidden_size_mismatch:got={}:expected={hidden_size}",
                hidden.len()
            ));
        }
        None => None,
    };
    let final_norm_checksum = weight_bytes_checksum(&norm_payload);
    let mut tokens = Vec::with_capacity(token_requests.len());
    let mut aggregate_words = vec![
        QWEN3_DENSE_0_6B_PROFILE.vocab_size,
        QWEN3_DENSE_0_6B_PROFILE.hidden_size,
        final_norm_checksum,
    ];
    for (step_index, token_id) in token_requests {
        let row_payload = materialize_tensor_row_payload_with_payloads(
            "lm_head.weight",
            lm_head,
            *token_id,
            tensor_payloads,
        )?;
        let row_weights = decode_weight_vector(lm_head.dtype, &row_payload, hidden_size)?;
        let fallback_hidden;
        let hidden = match hidden {
            Some(hidden) => hidden,
            None => {
                fallback_hidden = deterministic_logits_hidden(hidden_size, *step_index);
                &fallback_hidden
            }
        };
        let normalized = rmsnorm_reference(&hidden, &norm_weight);
        let logit = normalized
            .iter()
            .zip(row_weights.iter())
            .map(|(value, weight)| value * weight)
            .sum::<f32>();
        let row_checksum = weight_bytes_checksum(&row_payload);
        let logit_bits = logit.to_bits() as u64;
        let logit_checksum = checksum_words(&[
            *step_index,
            *token_id,
            row_payload.len() as u64,
            row_checksum,
            logit_bits,
        ]);
        aggregate_words.extend_from_slice(&[
            *step_index,
            *token_id,
            row_payload.len() as u64,
            row_checksum,
            logit_bits,
            logit_checksum,
        ]);
        tokens.push(Qwen3Dense06bLogitsReferenceTokenSummary {
            step_index: *step_index,
            token_id: *token_id,
            row_bytes: row_payload.len() as u64,
            row_checksum,
            logit_bits,
            logit_checksum,
        });
    }
    Ok(Qwen3Dense06bLogitsReferenceSummary {
        model_id: "Qwen/Qwen3-0.6B".to_string(),
        source: lm_head
            .source_file
            .clone()
            .unwrap_or_else(|| "<memory>".to_string()),
        vocab_size: QWEN3_DENSE_0_6B_PROFILE.vocab_size,
        hidden_size: QWEN3_DENSE_0_6B_PROFILE.hidden_size,
        final_norm_bytes: norm_payload.len() as u64,
        final_norm_checksum,
        token_count: tokens.len() as u64,
        aggregate_checksum: checksum_words(&aggregate_words),
        tokens,
    })
}

pub fn embedding_reference_summary(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_ids: &[u64],
) -> Result<Qwen3Dense06bEmbeddingReferenceSummary, String> {
    embedding_reference_summary_with_payloads(tensors, None, token_ids)
}

pub fn embedding_reference_summary_with_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    token_ids: &[u64],
) -> Result<Qwen3Dense06bEmbeddingReferenceSummary, String> {
    let embedding = tensors.get("model.embed_tokens.weight").ok_or_else(|| {
        "qwen3_dense_0_6b_missing_weight_tensor:model.embed_tokens.weight".to_string()
    })?;
    if embedding.shape
        != vec![
            QWEN3_DENSE_0_6B_PROFILE.vocab_size,
            QWEN3_DENSE_0_6B_PROFILE.hidden_size,
        ]
    {
        return Err(format!(
            "qwen3_dense_0_6b_weight_shape_mismatch:model.embed_tokens.weight:got={:?}:expected={:?}",
            embedding.shape,
            vec![
                QWEN3_DENSE_0_6B_PROFILE.vocab_size,
                QWEN3_DENSE_0_6B_PROFILE.hidden_size
            ]
        ));
    }
    let hidden_size = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize;
    let mut tokens = Vec::with_capacity(token_ids.len());
    let mut aggregate_words = vec![
        QWEN3_DENSE_0_6B_PROFILE.vocab_size,
        QWEN3_DENSE_0_6B_PROFILE.hidden_size,
        token_ids.len() as u64,
    ];
    let mut row_byte_count = 0u64;
    let mut row_checksums = Vec::with_capacity(token_ids.len());
    let mut value_checksums = Vec::with_capacity(token_ids.len());
    for (sequence_index, token_id) in token_ids.iter().copied().enumerate() {
        let row_payload = materialize_tensor_row_payload_with_payloads(
            "model.embed_tokens.weight",
            embedding,
            token_id,
            tensor_payloads,
        )?;
        let row_values = decode_weight_vector(embedding.dtype, &row_payload, hidden_size)?;
        let row_checksum = weight_bytes_checksum(&row_payload);
        let value_checksum = f32_vector_checksum(&row_values);
        let sample_words = f32_vector_sample_words(&row_values);
        row_byte_count += row_payload.len() as u64;
        row_checksums.push(row_checksum);
        value_checksums.push(value_checksum);
        aggregate_words.extend_from_slice(&[
            sequence_index as u64,
            token_id,
            row_payload.len() as u64,
            row_checksum,
            value_checksum,
        ]);
        aggregate_words.extend_from_slice(&sample_words);
        tokens.push(Qwen3Dense06bEmbeddingReferenceTokenSummary {
            sequence_index: sequence_index as u64,
            token_id,
            row_bytes: row_payload.len() as u64,
            row_checksum,
            value_checksum,
            sample_words,
        });
    }
    Ok(Qwen3Dense06bEmbeddingReferenceSummary {
        model_id: "Qwen/Qwen3-0.6B".to_string(),
        source: embedding
            .source_file
            .clone()
            .unwrap_or_else(|| "<memory>".to_string()),
        vocab_size: QWEN3_DENSE_0_6B_PROFILE.vocab_size,
        hidden_size: QWEN3_DENSE_0_6B_PROFILE.hidden_size,
        token_count: token_ids.len() as u64,
        row_byte_count,
        row_checksum: checksum_words(&row_checksums),
        value_checksum: checksum_words(&value_checksums),
        aggregate_checksum: checksum_words(&aggregate_words),
        tokens,
    })
}

pub fn embedding_reference_last_hidden(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_ids: &[u64],
) -> Result<Vec<f32>, String> {
    embedding_reference_last_hidden_for_profile(QWEN3_DENSE_0_6B_PROFILE, tensors, token_ids)
}

pub fn embedding_reference_last_hidden_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_ids: &[u64],
) -> Result<Vec<f32>, String> {
    embedding_reference_last_hidden_with_payloads_for_profile(profile, tensors, None, token_ids)
}

pub fn embedding_reference_last_hidden_with_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    token_ids: &[u64],
) -> Result<Vec<f32>, String> {
    embedding_reference_last_hidden_with_payloads_for_profile(
        QWEN3_DENSE_0_6B_PROFILE,
        tensors,
        tensor_payloads,
        token_ids,
    )
}

pub fn embedding_reference_last_hidden_with_payloads_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    token_ids: &[u64],
) -> Result<Vec<f32>, String> {
    validate_profile(profile)?;
    let token_id = token_ids
        .last()
        .copied()
        .ok_or_else(|| "qwen3_embedding_reference_no_tokens".to_string())?;
    let embedding = tensors.get("model.embed_tokens.weight").ok_or_else(|| {
        "qwen3_dense_0_6b_missing_weight_tensor:model.embed_tokens.weight".to_string()
    })?;
    if embedding.shape != vec![profile.vocab_size, profile.hidden_size] {
        return Err(format!(
            "qwen3_dense_0_6b_weight_shape_mismatch:model.embed_tokens.weight:got={:?}:expected={:?}",
            embedding.shape,
            vec![
                profile.vocab_size,
                profile.hidden_size
            ]
        ));
    }
    let row_payload = materialize_tensor_row_payload_with_payloads(
        "model.embed_tokens.weight",
        embedding,
        token_id,
        tensor_payloads,
    )?;
    decode_weight_vector(embedding.dtype, &row_payload, profile.hidden_size as usize)
}

pub fn embedding_reference_hidden_sequence(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_ids: &[u64],
) -> Result<Vec<Vec<f32>>, String> {
    embedding_reference_hidden_sequence_for_profile(QWEN3_DENSE_0_6B_PROFILE, tensors, token_ids)
}

pub fn embedding_reference_hidden_sequence_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_ids: &[u64],
) -> Result<Vec<Vec<f32>>, String> {
    embedding_reference_hidden_sequence_with_payloads_for_profile(profile, tensors, None, token_ids)
}

pub fn embedding_reference_hidden_sequence_with_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    token_ids: &[u64],
) -> Result<Vec<Vec<f32>>, String> {
    embedding_reference_hidden_sequence_with_payloads_for_profile(
        QWEN3_DENSE_0_6B_PROFILE,
        tensors,
        tensor_payloads,
        token_ids,
    )
}

pub fn embedding_reference_hidden_sequence_with_payloads_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    token_ids: &[u64],
) -> Result<Vec<Vec<f32>>, String> {
    validate_profile(profile)?;
    if token_ids.is_empty() {
        return Err("qwen3_embedding_reference_no_tokens".to_string());
    }
    let embedding = tensors.get("model.embed_tokens.weight").ok_or_else(|| {
        "qwen3_dense_0_6b_missing_weight_tensor:model.embed_tokens.weight".to_string()
    })?;
    if embedding.shape != vec![profile.vocab_size, profile.hidden_size] {
        return Err(format!(
            "qwen3_dense_0_6b_weight_shape_mismatch:model.embed_tokens.weight:got={:?}:expected={:?}",
            embedding.shape,
            vec![
                profile.vocab_size,
                profile.hidden_size
            ]
        ));
    }
    let hidden_size = profile.hidden_size as usize;
    token_ids
        .iter()
        .copied()
        .map(|token_id| {
            let row_payload = materialize_tensor_row_payload_with_payloads(
                "model.embed_tokens.weight",
                embedding,
                token_id,
                tensor_payloads,
            )?;
            decode_weight_vector(embedding.dtype, &row_payload, hidden_size)
        })
        .collect()
}

pub fn layer_forward_reference(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
    position: u64,
    hidden: &[f32],
) -> Result<Qwen3Dense06bLayerForwardReference, String> {
    layer_forward_reference_for_profile(
        QWEN3_DENSE_0_6B_PROFILE,
        tensors,
        layer_id,
        position,
        hidden,
    )
}

pub fn layer_forward_reference_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
    position: u64,
    hidden: &[f32],
) -> Result<Qwen3Dense06bLayerForwardReference, String> {
    validate_profile(profile)?;
    if layer_id >= profile.num_hidden_layers {
        return Err(format!(
            "qwen3_layer_forward_layer_oob:layer={layer_id}:layers={}",
            profile.num_hidden_layers
        ));
    }
    let hidden_size = profile.hidden_size as usize;
    if hidden.len() != hidden_size {
        return Err(format!(
            "qwen3_layer_forward_hidden_size_mismatch:got={}:expected={hidden_size}",
            hidden.len()
        ));
    }

    let prefix = format!("model.layers.{layer_id}");
    let input_norm_weight = load_weight_vector(
        tensors,
        &format!("{prefix}.input_layernorm.weight"),
        hidden_size,
    )?;
    let normed = rmsnorm_reference(hidden, &input_norm_weight);
    let q_projection = matmul_full_tensor(
        tensors,
        &format!("{prefix}.self_attn.q_proj.weight"),
        (profile.num_attention_heads * profile.head_dim) as usize,
        hidden_size,
        &normed,
    )?;
    let k_projection = matmul_full_tensor(
        tensors,
        &format!("{prefix}.self_attn.k_proj.weight"),
        (profile.num_key_value_heads * profile.head_dim) as usize,
        hidden_size,
        &normed,
    )?;
    let v = matmul_full_tensor(
        tensors,
        &format!("{prefix}.self_attn.v_proj.weight"),
        (profile.num_key_value_heads * profile.head_dim) as usize,
        hidden_size,
        &normed,
    )?;
    let q_norm_weight = load_weight_vector(
        tensors,
        &format!("{prefix}.self_attn.q_norm.weight"),
        profile.head_dim as usize,
    )?;
    let k_norm_weight = load_weight_vector(
        tensors,
        &format!("{prefix}.self_attn.k_norm.weight"),
        profile.head_dim as usize,
    )?;
    let q = rmsnorm_per_head_reference(
        &q_projection,
        profile.num_attention_heads as usize,
        profile.head_dim as usize,
        &q_norm_weight,
    )?;
    let k = rmsnorm_per_head_reference(
        &k_projection,
        profile.num_key_value_heads as usize,
        profile.head_dim as usize,
        &k_norm_weight,
    )?;
    let rope_q = apply_rope(
        &q,
        profile.num_attention_heads as usize,
        profile.head_dim as usize,
        position,
        profile.rope_theta as f32,
    )?;
    let rope_k = apply_rope(
        &k,
        profile.num_key_value_heads as usize,
        profile.head_dim as usize,
        position,
        profile.rope_theta as f32,
    )?;
    let attention_context = single_token_gqa_attention_context(
        &rope_q,
        &rope_k,
        &v,
        profile.num_attention_heads as usize,
        profile.num_key_value_heads as usize,
        profile.head_dim as usize,
    )?;
    let attention_output = matmul_full_tensor(
        tensors,
        &format!("{prefix}.self_attn.o_proj.weight"),
        hidden_size,
        (profile.num_attention_heads * profile.head_dim) as usize,
        &attention_context,
    )?;
    let attention_residual = vector_add(hidden, &attention_output)?;

    let post_norm_weight = load_weight_vector(
        tensors,
        &format!("{prefix}.post_attention_layernorm.weight"),
        hidden_size,
    )?;
    let post_normed = rmsnorm_reference(&attention_residual, &post_norm_weight);
    let gate = matmul_full_tensor(
        tensors,
        &format!("{prefix}.mlp.gate_proj.weight"),
        profile.intermediate_size as usize,
        hidden_size,
        &post_normed,
    )?;
    let up = matmul_full_tensor(
        tensors,
        &format!("{prefix}.mlp.up_proj.weight"),
        profile.intermediate_size as usize,
        hidden_size,
        &post_normed,
    )?;
    let activation = swiglu_activation(&gate, &up)?;
    let mlp_down = matmul_full_tensor(
        tensors,
        &format!("{prefix}.mlp.down_proj.weight"),
        hidden_size,
        profile.intermediate_size as usize,
        &activation,
    )?;
    let output = vector_add(&attention_residual, &mlp_down)?;

    Ok(Qwen3Dense06bLayerForwardReference {
        layer_id,
        position,
        hidden_size: profile.hidden_size,
        input_checksum: f32_vector_checksum(hidden),
        q_checksum: f32_vector_checksum(&q),
        k_checksum: f32_vector_checksum(&k),
        v_checksum: f32_vector_checksum(&v),
        rope_q_checksum: f32_vector_checksum(&rope_q),
        rope_k_checksum: f32_vector_checksum(&rope_k),
        attention_context_checksum: f32_vector_checksum(&attention_context),
        attention_output_checksum: f32_vector_checksum(&attention_output),
        attention_residual_checksum: f32_vector_checksum(&attention_residual),
        mlp_gate_checksum: f32_vector_checksum(&gate),
        mlp_up_checksum: f32_vector_checksum(&up),
        mlp_activation_checksum: f32_vector_checksum(&activation),
        mlp_down_checksum: f32_vector_checksum(&mlp_down),
        output_checksum: f32_vector_checksum(&output),
        output_sample_words: f32_vector_sample_words(&output),
        output,
    })
}

fn layer_forward_reference_sequence_with_cache(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    layer_id: u64,
    hidden_states: &[Vec<f32>],
) -> Result<
    (
        Vec<Vec<f32>>,
        Qwen3Dense06bLayerForwardReference,
        Qwen3Dense06bLayerKvCache,
    ),
    String,
> {
    layer_forward_reference_sequence_with_cache_for_profile(
        QWEN3_DENSE_0_6B_PROFILE,
        tensors,
        layer_payloads,
        layer_id,
        hidden_states,
    )
}

fn layer_forward_reference_sequence_with_cache_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    layer_id: u64,
    hidden_states: &[Vec<f32>],
) -> Result<
    (
        Vec<Vec<f32>>,
        Qwen3Dense06bLayerForwardReference,
        Qwen3Dense06bLayerKvCache,
    ),
    String,
> {
    validate_profile(profile)?;
    if hidden_states.is_empty() {
        return Err("qwen3_layer_forward_sequence_empty".to_string());
    }
    if layer_id >= profile.num_hidden_layers {
        return Err(format!(
            "qwen3_layer_forward_layer_oob:layer={layer_id}:layers={}",
            profile.num_hidden_layers
        ));
    }
    let hidden_size = profile.hidden_size as usize;
    if let Some((index, hidden)) = hidden_states
        .iter()
        .enumerate()
        .find(|(_, hidden)| hidden.len() != hidden_size)
    {
        return Err(format!(
            "qwen3_layer_forward_hidden_size_mismatch:index={index}:got={}:expected={hidden_size}",
            hidden.len()
        ));
    }

    let prefix = format!("model.layers.{layer_id}");
    let input_norm_weight = load_weight_vector_with_payloads(
        tensors,
        layer_payloads,
        &format!("{prefix}.input_layernorm.weight"),
        hidden_size,
    )?;
    let mut q_states = Vec::with_capacity(hidden_states.len());
    let mut k_states = Vec::with_capacity(hidden_states.len());
    let mut v_states = Vec::with_capacity(hidden_states.len());
    let mut rope_q_states = Vec::with_capacity(hidden_states.len());
    let mut rope_k_states = Vec::with_capacity(hidden_states.len());
    let q_norm_weight = load_weight_vector_with_payloads(
        tensors,
        layer_payloads,
        &format!("{prefix}.self_attn.q_norm.weight"),
        profile.head_dim as usize,
    )?;
    let k_norm_weight = load_weight_vector_with_payloads(
        tensors,
        layer_payloads,
        &format!("{prefix}.self_attn.k_norm.weight"),
        profile.head_dim as usize,
    )?;
    for (position, hidden) in hidden_states.iter().enumerate() {
        let normed = rmsnorm_reference(hidden, &input_norm_weight);
        let q_projection = matmul_full_tensor_with_payloads(
            tensors,
            layer_payloads,
            &format!("{prefix}.self_attn.q_proj.weight"),
            (profile.num_attention_heads * profile.head_dim) as usize,
            hidden_size,
            &normed,
        )?;
        let k_projection = matmul_full_tensor_with_payloads(
            tensors,
            layer_payloads,
            &format!("{prefix}.self_attn.k_proj.weight"),
            (profile.num_key_value_heads * profile.head_dim) as usize,
            hidden_size,
            &normed,
        )?;
        let v = matmul_full_tensor_with_payloads(
            tensors,
            layer_payloads,
            &format!("{prefix}.self_attn.v_proj.weight"),
            (profile.num_key_value_heads * profile.head_dim) as usize,
            hidden_size,
            &normed,
        )?;
        let q = rmsnorm_per_head_reference(
            &q_projection,
            profile.num_attention_heads as usize,
            profile.head_dim as usize,
            &q_norm_weight,
        )?;
        let k = rmsnorm_per_head_reference(
            &k_projection,
            profile.num_key_value_heads as usize,
            profile.head_dim as usize,
            &k_norm_weight,
        )?;
        let rope_q = apply_rope(
            &q,
            profile.num_attention_heads as usize,
            profile.head_dim as usize,
            position as u64,
            profile.rope_theta as f32,
        )?;
        let rope_k = apply_rope(
            &k,
            profile.num_key_value_heads as usize,
            profile.head_dim as usize,
            position as u64,
            profile.rope_theta as f32,
        )?;
        q_states.push(q);
        k_states.push(k);
        v_states.push(v);
        rope_q_states.push(rope_q);
        rope_k_states.push(rope_k);
    }

    let post_norm_weight = load_weight_vector_with_payloads(
        tensors,
        layer_payloads,
        &format!("{prefix}.post_attention_layernorm.weight"),
        hidden_size,
    )?;
    let mut outputs = Vec::with_capacity(hidden_states.len());
    let mut last_reference = None;
    for position in 0..hidden_states.len() {
        let attention_context = causal_gqa_attention_context_for_position(
            &rope_q_states,
            &rope_k_states,
            &v_states,
            position,
            profile.num_attention_heads as usize,
            profile.num_key_value_heads as usize,
            profile.head_dim as usize,
        )?;
        let attention_output = matmul_full_tensor_with_payloads(
            tensors,
            layer_payloads,
            &format!("{prefix}.self_attn.o_proj.weight"),
            hidden_size,
            (profile.num_attention_heads * profile.head_dim) as usize,
            &attention_context,
        )?;
        let attention_residual = vector_add(&hidden_states[position], &attention_output)?;
        let post_normed = rmsnorm_reference(&attention_residual, &post_norm_weight);
        let gate = matmul_full_tensor_with_payloads(
            tensors,
            layer_payloads,
            &format!("{prefix}.mlp.gate_proj.weight"),
            profile.intermediate_size as usize,
            hidden_size,
            &post_normed,
        )?;
        let up = matmul_full_tensor_with_payloads(
            tensors,
            layer_payloads,
            &format!("{prefix}.mlp.up_proj.weight"),
            profile.intermediate_size as usize,
            hidden_size,
            &post_normed,
        )?;
        let activation = swiglu_activation(&gate, &up)?;
        let mlp_down = matmul_full_tensor_with_payloads(
            tensors,
            layer_payloads,
            &format!("{prefix}.mlp.down_proj.weight"),
            hidden_size,
            profile.intermediate_size as usize,
            &activation,
        )?;
        let output = vector_add(&attention_residual, &mlp_down)?;
        if position + 1 == hidden_states.len() {
            last_reference = Some(Qwen3Dense06bLayerForwardReference {
                layer_id,
                position: position as u64,
                hidden_size: profile.hidden_size,
                input_checksum: f32_vector_checksum(&hidden_states[position]),
                q_checksum: f32_vector_checksum(&q_states[position]),
                k_checksum: f32_vector_checksum(&k_states[position]),
                v_checksum: f32_vector_checksum(&v_states[position]),
                rope_q_checksum: f32_vector_checksum(&rope_q_states[position]),
                rope_k_checksum: f32_vector_checksum(&rope_k_states[position]),
                attention_context_checksum: f32_vector_checksum(&attention_context),
                attention_output_checksum: f32_vector_checksum(&attention_output),
                attention_residual_checksum: f32_vector_checksum(&attention_residual),
                mlp_gate_checksum: f32_vector_checksum(&gate),
                mlp_up_checksum: f32_vector_checksum(&up),
                mlp_activation_checksum: f32_vector_checksum(&activation),
                mlp_down_checksum: f32_vector_checksum(&mlp_down),
                output_checksum: f32_vector_checksum(&output),
                output_sample_words: f32_vector_sample_words(&output),
                output: output.clone(),
            });
        }
        outputs.push(output);
    }
    Ok((
        outputs,
        last_reference.ok_or_else(|| "qwen3_layer_forward_missing_last_reference".to_string())?,
        Qwen3Dense06bLayerKvCache {
            layer_id,
            token_count: hidden_states.len() as u64,
            rope_k_states,
            v_states,
        },
    ))
}

fn layer_forward_reference_sequence(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
    hidden_states: &[Vec<f32>],
) -> Result<(Vec<Vec<f32>>, Qwen3Dense06bLayerForwardReference), String> {
    let (outputs, reference, _) =
        layer_forward_reference_sequence_with_cache(tensors, None, layer_id, hidden_states)?;
    Ok((outputs, reference))
}

pub fn layer_forward_reference_sequence_with_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_payloads: &BTreeMap<String, Vec<u8>>,
    layer_id: u64,
    hidden_states: &[Vec<f32>],
) -> Result<(Vec<Vec<f32>>, Qwen3Dense06bLayerForwardReference), String> {
    let (outputs, reference, _) = layer_forward_reference_sequence_with_cache(
        tensors,
        Some(layer_payloads),
        layer_id,
        hidden_states,
    )?;
    Ok((outputs, reference))
}

fn layer_forward_incremental_with_cache(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_id: u64,
    position: u64,
    hidden: &[f32],
    previous_cache: &Qwen3Dense06bLayerKvCache,
) -> Result<
    (
        Vec<f32>,
        Qwen3Dense06bLayerForwardReference,
        Qwen3Dense06bLayerKvCache,
    ),
    String,
> {
    let profile = QWEN3_DENSE_0_6B_PROFILE;
    if layer_id != previous_cache.layer_id {
        return Err(format!(
            "qwen3_incremental_cache_layer_id_mismatch:got={}:expected={layer_id}",
            previous_cache.layer_id
        ));
    }
    if previous_cache.token_count != position {
        return Err(format!(
            "qwen3_incremental_cache_position_mismatch:cache_tokens={}:position={position}",
            previous_cache.token_count
        ));
    }
    let hidden_size = profile.hidden_size as usize;
    if hidden.len() != hidden_size {
        return Err(format!(
            "qwen3_layer_forward_hidden_size_mismatch:got={}:expected={hidden_size}",
            hidden.len()
        ));
    }

    let prefix = format!("model.layers.{layer_id}");
    let input_norm_weight = load_weight_vector(
        tensors,
        &format!("{prefix}.input_layernorm.weight"),
        hidden_size,
    )?;
    let normed = rmsnorm_reference(hidden, &input_norm_weight);
    let q_projection = matmul_full_tensor(
        tensors,
        &format!("{prefix}.self_attn.q_proj.weight"),
        (profile.num_attention_heads * profile.head_dim) as usize,
        hidden_size,
        &normed,
    )?;
    let k_projection = matmul_full_tensor(
        tensors,
        &format!("{prefix}.self_attn.k_proj.weight"),
        (profile.num_key_value_heads * profile.head_dim) as usize,
        hidden_size,
        &normed,
    )?;
    let v = matmul_full_tensor(
        tensors,
        &format!("{prefix}.self_attn.v_proj.weight"),
        (profile.num_key_value_heads * profile.head_dim) as usize,
        hidden_size,
        &normed,
    )?;
    let q_norm_weight = load_weight_vector(
        tensors,
        &format!("{prefix}.self_attn.q_norm.weight"),
        profile.head_dim as usize,
    )?;
    let k_norm_weight = load_weight_vector(
        tensors,
        &format!("{prefix}.self_attn.k_norm.weight"),
        profile.head_dim as usize,
    )?;
    let q = rmsnorm_per_head_reference(
        &q_projection,
        profile.num_attention_heads as usize,
        profile.head_dim as usize,
        &q_norm_weight,
    )?;
    let k = rmsnorm_per_head_reference(
        &k_projection,
        profile.num_key_value_heads as usize,
        profile.head_dim as usize,
        &k_norm_weight,
    )?;
    let rope_q = apply_rope(
        &q,
        profile.num_attention_heads as usize,
        profile.head_dim as usize,
        position,
        profile.rope_theta as f32,
    )?;
    let rope_k = apply_rope(
        &k,
        profile.num_key_value_heads as usize,
        profile.head_dim as usize,
        position,
        profile.rope_theta as f32,
    )?;
    let mut rope_k_states = previous_cache.rope_k_states.clone();
    let mut v_states = previous_cache.v_states.clone();
    rope_k_states.push(rope_k.clone());
    v_states.push(v.clone());
    let attention_context = causal_gqa_attention_context_for_query(
        &rope_q,
        &rope_k_states,
        &v_states,
        profile.num_attention_heads as usize,
        profile.num_key_value_heads as usize,
        profile.head_dim as usize,
    )?;
    let attention_output = matmul_full_tensor(
        tensors,
        &format!("{prefix}.self_attn.o_proj.weight"),
        hidden_size,
        (profile.num_attention_heads * profile.head_dim) as usize,
        &attention_context,
    )?;
    let attention_residual = vector_add(hidden, &attention_output)?;

    let post_norm_weight = load_weight_vector(
        tensors,
        &format!("{prefix}.post_attention_layernorm.weight"),
        hidden_size,
    )?;
    let post_normed = rmsnorm_reference(&attention_residual, &post_norm_weight);
    let gate = matmul_full_tensor(
        tensors,
        &format!("{prefix}.mlp.gate_proj.weight"),
        profile.intermediate_size as usize,
        hidden_size,
        &post_normed,
    )?;
    let up = matmul_full_tensor(
        tensors,
        &format!("{prefix}.mlp.up_proj.weight"),
        profile.intermediate_size as usize,
        hidden_size,
        &post_normed,
    )?;
    let activation = swiglu_activation(&gate, &up)?;
    let mlp_down = matmul_full_tensor(
        tensors,
        &format!("{prefix}.mlp.down_proj.weight"),
        hidden_size,
        profile.intermediate_size as usize,
        &activation,
    )?;
    let output = vector_add(&attention_residual, &mlp_down)?;
    Ok((
        output.clone(),
        Qwen3Dense06bLayerForwardReference {
            layer_id,
            position,
            hidden_size: profile.hidden_size,
            input_checksum: f32_vector_checksum(hidden),
            q_checksum: f32_vector_checksum(&q),
            k_checksum: f32_vector_checksum(&k),
            v_checksum: f32_vector_checksum(&v),
            rope_q_checksum: f32_vector_checksum(&rope_q),
            rope_k_checksum: f32_vector_checksum(&rope_k),
            attention_context_checksum: f32_vector_checksum(&attention_context),
            attention_output_checksum: f32_vector_checksum(&attention_output),
            attention_residual_checksum: f32_vector_checksum(&attention_residual),
            mlp_gate_checksum: f32_vector_checksum(&gate),
            mlp_up_checksum: f32_vector_checksum(&up),
            mlp_activation_checksum: f32_vector_checksum(&activation),
            mlp_down_checksum: f32_vector_checksum(&mlp_down),
            output_checksum: f32_vector_checksum(&output),
            output_sample_words: f32_vector_sample_words(&output),
            output: output.clone(),
        },
        Qwen3Dense06bLayerKvCache {
            layer_id,
            token_count: position + 1,
            rope_k_states,
            v_states,
        },
    ))
}

pub fn forward_reference_from_hidden(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    position: u64,
    hidden: &[f32],
) -> Result<Qwen3Dense06bForwardReference, String> {
    forward_reference_from_hidden_range_for_profile(
        QWEN3_DENSE_0_6B_PROFILE,
        tensors,
        0,
        QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers,
        position,
        hidden,
    )
}

pub fn forward_reference_from_hidden_range_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_start: u64,
    layer_end: u64,
    position: u64,
    hidden: &[f32],
) -> Result<Qwen3Dense06bForwardReference, String> {
    validate_profile(profile)?;
    if layer_start > layer_end || layer_end > profile.num_hidden_layers {
        return Err(format!(
            "qwen3_forward_hidden_range_invalid:start={layer_start}:end={layer_end}:layers={}",
            profile.num_hidden_layers
        ));
    }
    let mut current = hidden.to_vec();
    let mut layers = Vec::with_capacity((layer_end - layer_start) as usize);
    let mut aggregate_words = vec![
        position,
        layer_start,
        layer_end,
        f32_vector_checksum(hidden),
    ];
    for layer_id in layer_start..layer_end {
        let layer =
            layer_forward_reference_for_profile(profile, tensors, layer_id, position, &current)?;
        aggregate_words.extend_from_slice(&[
            layer.layer_id,
            layer.input_checksum,
            layer.q_checksum,
            layer.k_checksum,
            layer.v_checksum,
            layer.attention_output_checksum,
            layer.mlp_down_checksum,
            layer.output_checksum,
        ]);
        aggregate_words.extend_from_slice(&layer.output_sample_words);
        current = layer.output.clone();
        layers.push(layer);
    }
    let final_hidden_checksum = f32_vector_checksum(&current);
    let final_hidden_sample_words = f32_vector_sample_words(&current);
    aggregate_words.push(final_hidden_checksum);
    aggregate_words.extend_from_slice(&final_hidden_sample_words);
    Ok(Qwen3Dense06bForwardReference {
        layer_count: layer_end - layer_start,
        position,
        hidden_size: profile.hidden_size,
        input_checksum: f32_vector_checksum(hidden),
        final_hidden_checksum,
        final_hidden_sample_words,
        aggregate_checksum: checksum_words(&aggregate_words),
        final_hidden: current,
        layers,
    })
}

pub fn forward_reference_from_hidden_sequence_range_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_start: u64,
    layer_end: u64,
    hidden_states: &[Vec<f32>],
) -> Result<(Qwen3Dense06bForwardReference, Vec<Vec<f32>>), String> {
    validate_profile(profile)?;
    if layer_start > layer_end || layer_end > profile.num_hidden_layers {
        return Err(format!(
            "qwen3_forward_hidden_sequence_range_invalid:start={layer_start}:end={layer_end}:layers={}",
            profile.num_hidden_layers
        ));
    }
    if hidden_states.is_empty() {
        return Err("qwen3_forward_hidden_sequence_empty".to_string());
    }
    let position = hidden_states.len().saturating_sub(1) as u64;
    let input_checksum = f32_vector_checksum(
        hidden_states
            .last()
            .ok_or_else(|| "qwen3_forward_reference_no_hidden_sequence".to_string())?,
    );
    let mut sequence = hidden_states.to_vec();
    let mut layers = Vec::with_capacity((layer_end - layer_start) as usize);
    let mut aggregate_words = vec![
        position,
        hidden_states.len() as u64,
        layer_start,
        layer_end,
        input_checksum,
    ];
    for layer_id in layer_start..layer_end {
        let (next_sequence, layer, _) = layer_forward_reference_sequence_with_cache_for_profile(
            profile, tensors, None, layer_id, &sequence,
        )?;
        aggregate_words.extend_from_slice(&[
            layer.layer_id,
            layer.input_checksum,
            layer.q_checksum,
            layer.k_checksum,
            layer.v_checksum,
            layer.attention_context_checksum,
            layer.attention_output_checksum,
            layer.mlp_down_checksum,
            layer.output_checksum,
        ]);
        aggregate_words.extend_from_slice(&layer.output_sample_words);
        sequence = next_sequence;
        layers.push(layer);
    }
    let final_hidden = sequence
        .last()
        .ok_or_else(|| "qwen3_forward_reference_no_final_hidden".to_string())?
        .clone();
    let final_hidden_checksum = f32_vector_checksum(&final_hidden);
    let final_hidden_sample_words = f32_vector_sample_words(&final_hidden);
    aggregate_words.push(final_hidden_checksum);
    aggregate_words.extend_from_slice(&final_hidden_sample_words);
    Ok((
        Qwen3Dense06bForwardReference {
            layer_count: layer_end - layer_start,
            position,
            hidden_size: profile.hidden_size,
            input_checksum,
            final_hidden_checksum,
            final_hidden_sample_words,
            aggregate_checksum: checksum_words(&aggregate_words),
            final_hidden,
            layers,
        },
        sequence,
    ))
}

pub fn forward_reference_from_token_ids(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_ids: &[u64],
) -> Result<Qwen3Dense06bForwardReference, String> {
    let mut sequence = embedding_reference_hidden_sequence(tensors, token_ids)?;
    let position = token_ids.len().saturating_sub(1) as u64;
    let input_checksum = f32_vector_checksum(
        sequence
            .last()
            .ok_or_else(|| "qwen3_forward_reference_no_hidden_sequence".to_string())?,
    );
    let mut layers = Vec::with_capacity(QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as usize);
    let mut aggregate_words = vec![position, token_ids.len() as u64, input_checksum];
    for layer_id in 0..QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        let (next_sequence, layer) =
            layer_forward_reference_sequence(tensors, layer_id, &sequence)?;
        aggregate_words.extend_from_slice(&[
            layer.layer_id,
            layer.input_checksum,
            layer.q_checksum,
            layer.k_checksum,
            layer.v_checksum,
            layer.attention_context_checksum,
            layer.attention_output_checksum,
            layer.mlp_down_checksum,
            layer.output_checksum,
        ]);
        aggregate_words.extend_from_slice(&layer.output_sample_words);
        sequence = next_sequence;
        layers.push(layer);
    }
    let final_hidden = sequence
        .last()
        .ok_or_else(|| "qwen3_forward_reference_no_final_hidden".to_string())?
        .clone();
    let final_hidden_checksum = f32_vector_checksum(&final_hidden);
    let final_hidden_sample_words = f32_vector_sample_words(&final_hidden);
    aggregate_words.push(final_hidden_checksum);
    aggregate_words.extend_from_slice(&final_hidden_sample_words);
    Ok(Qwen3Dense06bForwardReference {
        layer_count: QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers,
        position,
        hidden_size: QWEN3_DENSE_0_6B_PROFILE.hidden_size,
        input_checksum,
        final_hidden_checksum,
        final_hidden_sample_words,
        aggregate_checksum: checksum_words(&aggregate_words),
        final_hidden,
        layers,
    })
}

pub fn forward_from_token_ids(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_ids: &[u64],
) -> Result<Qwen3Dense06bForwardReference, String> {
    forward_reference_from_token_ids(tensors, token_ids)
}

pub fn forward_from_token_ids_with_layer_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_payloads: &BTreeMap<String, Vec<u8>>,
    token_ids: &[u64],
) -> Result<Qwen3Dense06bForwardReference, String> {
    let mut sequence = embedding_reference_hidden_sequence_with_payloads(
        tensors,
        Some(layer_payloads),
        token_ids,
    )?;
    let position = token_ids.len().saturating_sub(1) as u64;
    let input_checksum = f32_vector_checksum(
        sequence
            .last()
            .ok_or_else(|| "qwen3_forward_reference_no_hidden_sequence".to_string())?,
    );
    let mut layers = Vec::with_capacity(QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as usize);
    let mut aggregate_words = vec![position, token_ids.len() as u64, input_checksum];
    for layer_id in 0..QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        let (next_sequence, layer) = layer_forward_reference_sequence_with_payloads(
            tensors,
            layer_payloads,
            layer_id,
            &sequence,
        )?;
        aggregate_words.extend_from_slice(&[
            layer.layer_id,
            layer.input_checksum,
            layer.q_checksum,
            layer.k_checksum,
            layer.v_checksum,
            layer.attention_context_checksum,
            layer.attention_output_checksum,
            layer.mlp_down_checksum,
            layer.output_checksum,
        ]);
        aggregate_words.extend_from_slice(&layer.output_sample_words);
        sequence = next_sequence;
        layers.push(layer);
    }
    let final_hidden = sequence
        .last()
        .ok_or_else(|| "qwen3_forward_reference_no_final_hidden".to_string())?
        .clone();
    let final_hidden_checksum = f32_vector_checksum(&final_hidden);
    let final_hidden_sample_words = f32_vector_sample_words(&final_hidden);
    aggregate_words.push(final_hidden_checksum);
    aggregate_words.extend_from_slice(&final_hidden_sample_words);
    Ok(Qwen3Dense06bForwardReference {
        layer_count: QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers,
        position,
        hidden_size: QWEN3_DENSE_0_6B_PROFILE.hidden_size,
        input_checksum,
        final_hidden_checksum,
        final_hidden_sample_words,
        aggregate_checksum: checksum_words(&aggregate_words),
        final_hidden,
        layers,
    })
}

pub fn forward_with_kv_cache_from_token_ids(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    token_ids: &[u64],
) -> Result<Qwen3Dense06bForwardWithKvCache, String> {
    let mut sequence = embedding_reference_hidden_sequence(tensors, token_ids)?;
    let position = token_ids.len().saturating_sub(1) as u64;
    let input_checksum = f32_vector_checksum(
        sequence
            .last()
            .ok_or_else(|| "qwen3_forward_reference_no_hidden_sequence".to_string())?,
    );
    let mut layers = Vec::with_capacity(QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as usize);
    let mut kv_cache = Vec::with_capacity(QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as usize);
    let mut aggregate_words = vec![position, token_ids.len() as u64, input_checksum];
    for layer_id in 0..QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        let (next_sequence, layer, layer_cache) =
            layer_forward_reference_sequence_with_cache(tensors, None, layer_id, &sequence)?;
        aggregate_words.extend_from_slice(&[
            layer.layer_id,
            layer.input_checksum,
            layer.q_checksum,
            layer.k_checksum,
            layer.v_checksum,
            layer.attention_context_checksum,
            layer.attention_output_checksum,
            layer.mlp_down_checksum,
            layer.output_checksum,
        ]);
        aggregate_words.extend_from_slice(&layer.output_sample_words);
        sequence = next_sequence;
        layers.push(layer);
        kv_cache.push(layer_cache);
    }
    let final_hidden = sequence
        .last()
        .ok_or_else(|| "qwen3_forward_reference_no_final_hidden".to_string())?
        .clone();
    let final_hidden_checksum = f32_vector_checksum(&final_hidden);
    let final_hidden_sample_words = f32_vector_sample_words(&final_hidden);
    aggregate_words.push(final_hidden_checksum);
    aggregate_words.extend_from_slice(&final_hidden_sample_words);
    Ok(Qwen3Dense06bForwardWithKvCache {
        forward: Qwen3Dense06bForwardReference {
            layer_count: QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers,
            position,
            hidden_size: QWEN3_DENSE_0_6B_PROFILE.hidden_size,
            input_checksum,
            final_hidden_checksum,
            final_hidden_sample_words,
            aggregate_checksum: checksum_words(&aggregate_words),
            final_hidden,
            layers,
        },
        kv_cache,
    })
}

pub fn forward_incremental_with_kv_cache_from_hidden(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    previous_cache: &[Qwen3Dense06bLayerKvCache],
    position: u64,
    hidden: &[f32],
) -> Result<Qwen3Dense06bForwardWithKvCache, String> {
    if previous_cache.len() != QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as usize {
        return Err(format!(
            "qwen3_incremental_cache_layer_count_mismatch:got={}:expected={}",
            previous_cache.len(),
            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
        ));
    }
    let mut current = hidden.to_vec();
    let mut layers = Vec::with_capacity(QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as usize);
    let mut kv_cache = Vec::with_capacity(QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as usize);
    let mut aggregate_words = vec![position, position + 1, f32_vector_checksum(hidden)];
    for layer_id in 0..QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers {
        let (next_hidden, layer, layer_cache) = layer_forward_incremental_with_cache(
            tensors,
            layer_id,
            position,
            &current,
            &previous_cache[layer_id as usize],
        )?;
        aggregate_words.extend_from_slice(&[
            layer.layer_id,
            layer.input_checksum,
            layer.q_checksum,
            layer.k_checksum,
            layer.v_checksum,
            layer.attention_context_checksum,
            layer.attention_output_checksum,
            layer.mlp_down_checksum,
            layer.output_checksum,
        ]);
        aggregate_words.extend_from_slice(&layer.output_sample_words);
        current = next_hidden;
        layers.push(layer);
        kv_cache.push(layer_cache);
    }
    let final_hidden_checksum = f32_vector_checksum(&current);
    let final_hidden_sample_words = f32_vector_sample_words(&current);
    aggregate_words.push(final_hidden_checksum);
    aggregate_words.extend_from_slice(&final_hidden_sample_words);
    Ok(Qwen3Dense06bForwardWithKvCache {
        forward: Qwen3Dense06bForwardReference {
            layer_count: QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers,
            position,
            hidden_size: QWEN3_DENSE_0_6B_PROFILE.hidden_size,
            input_checksum: f32_vector_checksum(hidden),
            final_hidden_checksum,
            final_hidden_sample_words,
            aggregate_checksum: checksum_words(&aggregate_words),
            final_hidden: current,
            layers,
        },
        kv_cache,
    })
}

pub fn full_vocab_logits_from_hidden(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    hidden: &[f32],
) -> Result<Qwen3Dense06bFullVocabLogitsSummary, String> {
    full_vocab_logits_from_hidden_with_chunk(tensors, hidden, 4096)
}

pub fn full_vocab_logits_from_hidden_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    hidden: &[f32],
) -> Result<Qwen3Dense06bFullVocabLogitsSummary, String> {
    full_vocab_logits_from_hidden_with_chunk_for_profile(profile, tensors, hidden, 4096)
}

pub fn full_vocab_logits_from_hidden_with_chunk_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    hidden: &[f32],
    chunk_rows: usize,
) -> Result<Qwen3Dense06bFullVocabLogitsSummary, String> {
    full_vocab_logits_from_hidden_with_chunk_and_payloads_for_profile(
        profile, tensors, None, hidden, chunk_rows,
    )
}

pub fn full_vocab_logits_from_hidden_with_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tensor_payloads: &BTreeMap<String, Vec<u8>>,
    hidden: &[f32],
) -> Result<Qwen3Dense06bFullVocabLogitsSummary, String> {
    full_vocab_logits_from_hidden_with_chunk_and_payloads(
        tensors,
        Some(tensor_payloads),
        hidden,
        4096,
    )
}

pub fn full_vocab_logits_from_hidden_with_chunk(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    hidden: &[f32],
    chunk_rows: usize,
) -> Result<Qwen3Dense06bFullVocabLogitsSummary, String> {
    full_vocab_logits_from_hidden_with_chunk_and_payloads(tensors, None, hidden, chunk_rows)
}

pub fn full_vocab_logits_from_hidden_with_chunk_and_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    hidden: &[f32],
    chunk_rows: usize,
) -> Result<Qwen3Dense06bFullVocabLogitsSummary, String> {
    full_vocab_logits_from_hidden_with_chunk_and_payloads_for_profile(
        QWEN3_DENSE_0_6B_PROFILE,
        tensors,
        tensor_payloads,
        hidden,
        chunk_rows,
    )
}

pub fn full_vocab_logits_from_hidden_with_chunk_and_payloads_for_profile(
    profile: Qwen3Dense06bProfile,
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    hidden: &[f32],
    chunk_rows: usize,
) -> Result<Qwen3Dense06bFullVocabLogitsSummary, String> {
    const TOP_CANDIDATE_COUNT: usize = 4;

    validate_profile(profile)?;
    let hidden_size = profile.hidden_size as usize;
    if hidden.len() != hidden_size {
        return Err(format!(
            "qwen3_full_vocab_logits_hidden_size_mismatch:got={}:expected={hidden_size}",
            hidden.len()
        ));
    }
    if chunk_rows == 0 {
        return Err("qwen3_full_vocab_logits_zero_chunk_rows".to_string());
    }
    let norm_weight = load_weight_vector_with_payloads(
        tensors,
        tensor_payloads,
        "model.norm.weight",
        hidden_size,
    )?;
    let normalized = rmsnorm_reference(hidden, &norm_weight);
    let final_norm_checksum = f32_vector_checksum(&normalized);
    let (head_name, head) = logits_head_tensor(tensors)?;
    let expected_shape = vec![profile.vocab_size, profile.hidden_size];
    if head.shape != expected_shape {
        return Err(format!(
            "qwen3_dense_0_6b_weight_shape_mismatch:{head_name}:got={:?}:expected={:?}",
            head.shape, expected_shape
        ));
    }

    let mut top_token_id = 0u64;
    let mut top_logit = f32::NEG_INFINITY;
    let mut runner_up_token_id = 0u64;
    let mut runner_up_logit = f32::NEG_INFINITY;
    let mut top_candidates = Vec::<(u64, f32)>::new();
    let mut aggregate_words = vec![profile.vocab_size, profile.hidden_size, final_norm_checksum];
    let mut logits_words = Vec::new();
    let vocab_size = profile.vocab_size as usize;
    for start in (0..vocab_size).step_by(chunk_rows) {
        let rows = chunk_rows.min(vocab_size - start);
        let payload = materialize_tensor_row_range_payload_with_payloads(
            head_name,
            head,
            start as u64,
            rows,
            tensor_payloads,
        )?;
        let weights = decode_weight_vector(
            head.dtype,
            &payload,
            rows.checked_mul(hidden_size)
                .ok_or_else(|| "qwen3_full_vocab_logits_chunk_elems_overflow".to_string())?,
        )?;
        let chunk_checksum = weight_bytes_checksum(&payload);
        let mut chunk_logit_checksum_words = Vec::with_capacity(rows);
        for row in 0..rows {
            let token_id = (start + row) as u64;
            let row_base = row * hidden_size;
            let logit = normalized
                .iter()
                .enumerate()
                .map(|(col, value)| value * weights[row_base + col])
                .sum::<f32>();
            if logit > top_logit {
                runner_up_token_id = top_token_id;
                runner_up_logit = top_logit;
                top_token_id = token_id;
                top_logit = logit;
            } else if logit > runner_up_logit {
                runner_up_token_id = token_id;
                runner_up_logit = logit;
            }
            qwen3_insert_top_logit_candidate(
                &mut top_candidates,
                TOP_CANDIDATE_COUNT,
                token_id,
                logit,
            );
            chunk_logit_checksum_words.push(logit.to_bits() as u64 ^ token_id.rotate_left(19));
        }
        let chunk_logit_checksum = checksum_words(&chunk_logit_checksum_words);
        aggregate_words.extend_from_slice(&[
            start as u64,
            rows as u64,
            payload.len() as u64,
            chunk_checksum,
            chunk_logit_checksum,
        ]);
        logits_words.push(chunk_logit_checksum);
    }
    let logits_checksum = checksum_words(&logits_words);
    aggregate_words.extend_from_slice(&[
        top_token_id,
        top_logit.to_bits() as u64,
        runner_up_token_id,
        runner_up_logit.to_bits() as u64,
        logits_checksum,
    ]);
    for (rank, (token_id, logit)) in top_candidates.iter().enumerate() {
        aggregate_words.extend_from_slice(&[rank as u64, *token_id, logit.to_bits() as u64]);
    }
    Ok(Qwen3Dense06bFullVocabLogitsSummary {
        vocab_size: profile.vocab_size,
        hidden_size: profile.hidden_size,
        final_norm_checksum,
        checked_token_count: profile.vocab_size,
        top_token_id,
        top_logit_bits: top_logit.to_bits() as u64,
        runner_up_token_id,
        runner_up_logit_bits: runner_up_logit.to_bits() as u64,
        top_candidates: top_candidates
            .into_iter()
            .enumerate()
            .map(|(rank, (token_id, logit))| Qwen3Dense06bLogitCandidate {
                rank: rank as u64,
                token_id,
                logit_bits: logit.to_bits() as u64,
            })
            .collect(),
        logits_checksum,
        aggregate_checksum: checksum_words(&aggregate_words),
    })
}

fn qwen3_insert_top_logit_candidate(
    candidates: &mut Vec<(u64, f32)>,
    capacity: usize,
    token_id: u64,
    logit: f32,
) {
    candidates.push((token_id, logit));
    candidates.sort_by(|(left_token, left_logit), (right_token, right_logit)| {
        right_logit
            .partial_cmp(left_logit)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| left_token.cmp(right_token))
    });
    candidates.truncate(capacity);
}

pub fn sampled_text_reference_from_logits(
    tokenizer_path: &Path,
    logits: &Qwen3Dense06bFullVocabLogitsSummary,
) -> Result<Qwen3Dense06bSampledTextReference, String> {
    let bytes = token_piece_bytes_from_tokenizer_path(tokenizer_path, logits.top_token_id)?;
    let decoded = token_piece_decode_bytes(&bytes);
    let byte_checksum = weight_bytes_checksum(&decoded);
    Ok(Qwen3Dense06bSampledTextReference {
        token_id: logits.top_token_id,
        byte_len: decoded.len() as u64,
        byte_checksum,
        text_lossy: String::from_utf8_lossy(&decoded).into_owned(),
        bytes: decoded,
    })
}

pub fn real_inference_reference_from_token_ids(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    tokenizer_path: &Path,
    token_ids: &[u64],
) -> Result<Qwen3Dense06bRealInferenceReference, String> {
    let forward = forward_reference_from_token_ids(tensors, token_ids)?;
    let logits = full_vocab_logits_from_hidden(tensors, &forward.final_hidden)?;
    let sampled_text = sampled_text_reference_from_logits(tokenizer_path, &logits)?;
    let mut aggregate_words = vec![
        token_ids.len() as u64,
        forward.aggregate_checksum,
        logits.aggregate_checksum,
        sampled_text.token_id,
        sampled_text.byte_len,
        sampled_text.byte_checksum,
    ];
    aggregate_words.extend_from_slice(token_ids);
    Ok(Qwen3Dense06bRealInferenceReference {
        token_ids: token_ids.to_vec(),
        forward,
        logits,
        sampled_text,
        aggregate_checksum: checksum_words(&aggregate_words),
    })
}

pub fn weight_bytes_checksum(bytes: &[u8]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for (index, byte) in bytes.iter().enumerate() {
        acc ^= (*byte as u64) | ((index as u64) << 8);
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

pub fn checksum_words(words: &[u64]) -> u64 {
    let mut bytes = Vec::with_capacity(words.len() * 8);
    for word in words {
        bytes.extend_from_slice(&word.to_le_bytes());
    }
    weight_bytes_checksum(&bytes)
}

fn find_weight_slice(
    manifest: &Qwen3Dense06bWeightManifest,
    layer_id: u64,
    shard_id: u64,
    tensor_kind: Qwen3Dense06bWeightTensorKind,
) -> Result<&Qwen3Dense06bWeightSlice, String> {
    manifest
        .slices
        .iter()
        .find(|slice| {
            slice.layer_id == layer_id
                && slice.shard_id == shard_id
                && slice.tensor_kind == tensor_kind
        })
        .ok_or_else(|| {
            format!(
                "qwen3_weight_slice_missing:layer={layer_id}:shard={shard_id}:kind={tensor_kind:?}"
            )
        })
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct QkvReferenceChecksums {
    rmsnorm_checksum: u64,
    rmsnorm_sample_words: [u64; 4],
    q_output_checksum: u64,
    q_output_sample_words: [u64; 4],
    k_output_checksum: u64,
    k_output_sample_words: [u64; 4],
    v_output_checksum: u64,
    v_output_sample_words: [u64; 4],
}

#[derive(Clone, Debug, PartialEq)]
struct QkvReferenceValues {
    rmsnorm: Vec<f32>,
    q_output: Vec<f32>,
    k_output: Vec<f32>,
    v_output: Vec<f32>,
    rmsnorm_checksum: u64,
    q_output_checksum: u64,
    k_output_checksum: u64,
    v_output_checksum: u64,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct MlpReferenceChecksums {
    gate_output_checksum: u64,
    gate_output_sample_words: [u64; 4],
    up_output_checksum: u64,
    up_output_sample_words: [u64; 4],
    activation_checksum: u64,
    activation_sample_words: [u64; 4],
    down_output_checksum: u64,
    down_output_sample_words: [u64; 4],
}

#[allow(clippy::too_many_arguments)]
fn qkv_reference_from_payloads(
    norm_dtype: Qwen3Dense06bWeightDType,
    q_dtype: Qwen3Dense06bWeightDType,
    k_dtype: Qwen3Dense06bWeightDType,
    v_dtype: Qwen3Dense06bWeightDType,
    hidden_size: u64,
    q_rows: u64,
    k_rows: u64,
    v_rows: u64,
    norm_payload: &[u8],
    q_payload: &[u8],
    k_payload: &[u8],
    v_payload: &[u8],
) -> Result<QkvReferenceChecksums, String> {
    let values = qkv_reference_values_from_payloads(
        norm_dtype,
        q_dtype,
        k_dtype,
        v_dtype,
        hidden_size,
        q_rows,
        k_rows,
        v_rows,
        norm_payload,
        q_payload,
        k_payload,
        v_payload,
        None,
    )?;
    Ok(QkvReferenceChecksums {
        rmsnorm_checksum: values.rmsnorm_checksum,
        rmsnorm_sample_words: f32_vector_sample_words(&values.rmsnorm),
        q_output_checksum: values.q_output_checksum,
        q_output_sample_words: f32_vector_sample_words(&values.q_output),
        k_output_checksum: values.k_output_checksum,
        k_output_sample_words: f32_vector_sample_words(&values.k_output),
        v_output_checksum: values.v_output_checksum,
        v_output_sample_words: f32_vector_sample_words(&values.v_output),
    })
}

#[allow(clippy::too_many_arguments)]
fn qkv_reference_values_from_payloads(
    norm_dtype: Qwen3Dense06bWeightDType,
    q_dtype: Qwen3Dense06bWeightDType,
    k_dtype: Qwen3Dense06bWeightDType,
    v_dtype: Qwen3Dense06bWeightDType,
    hidden_size: u64,
    q_rows: u64,
    k_rows: u64,
    v_rows: u64,
    norm_payload: &[u8],
    q_payload: &[u8],
    k_payload: &[u8],
    v_payload: &[u8],
    hidden: Option<&[f32]>,
) -> Result<QkvReferenceValues, String> {
    let hidden_size = hidden_size as usize;
    let fallback_hidden;
    let hidden = match hidden {
        Some(hidden) if hidden.len() == hidden_size => hidden,
        Some(hidden) => {
            return Err(format!(
                "qwen3_qkv_reference_hidden_size_mismatch:got={}:expected={hidden_size}",
                hidden.len()
            ));
        }
        None => {
            fallback_hidden = deterministic_reference_hidden(hidden_size);
            &fallback_hidden
        }
    };
    let norm_weight = decode_weight_vector(norm_dtype, norm_payload, hidden_size)?;
    let rmsnorm = rmsnorm_reference(&hidden, &norm_weight);
    let q_output = matmul_reference_values(q_dtype, q_payload, q_rows as usize, &rmsnorm)?;
    let k_output = matmul_reference_values(k_dtype, k_payload, k_rows as usize, &rmsnorm)?;
    let v_output = matmul_reference_values(v_dtype, v_payload, v_rows as usize, &rmsnorm)?;
    Ok(QkvReferenceValues {
        rmsnorm_checksum: f32_vector_checksum(&rmsnorm),
        q_output_checksum: f32_vector_checksum(&q_output),
        k_output_checksum: f32_vector_checksum(&k_output),
        v_output_checksum: f32_vector_checksum(&v_output),
        rmsnorm,
        q_output,
        k_output,
        v_output,
    })
}

fn mlp_reference_from_payloads(
    gate_dtype: Qwen3Dense06bWeightDType,
    up_dtype: Qwen3Dense06bWeightDType,
    down_dtype: Qwen3Dense06bWeightDType,
    hidden_size: u64,
    intermediate_rows: u64,
    gate_payload: &[u8],
    up_payload: &[u8],
    down_payload: &[u8],
    hidden: Option<&[f32]>,
) -> Result<MlpReferenceChecksums, String> {
    let hidden_size = hidden_size as usize;
    let intermediate_rows = intermediate_rows as usize;
    let fallback_hidden;
    let hidden = match hidden {
        Some(hidden) if hidden.len() == hidden_size => hidden,
        Some(hidden) => {
            return Err(format!(
                "qwen3_mlp_reference_hidden_size_mismatch:got={}:expected={hidden_size}",
                hidden.len()
            ));
        }
        None => {
            fallback_hidden = deterministic_reference_hidden(hidden_size);
            &fallback_hidden
        }
    };
    let gate_output =
        matmul_reference_values(gate_dtype, gate_payload, intermediate_rows, &hidden)?;
    let up_output = matmul_reference_values(up_dtype, up_payload, intermediate_rows, &hidden)?;
    let activation = gate_output
        .iter()
        .zip(up_output.iter())
        .map(|(gate, up)| {
            let silu_gate = *gate / (1.0 + (-*gate).exp());
            silu_gate * *up
        })
        .collect::<Vec<_>>();
    let down_output = matmul_reference_values(down_dtype, down_payload, hidden_size, &activation)?;
    Ok(MlpReferenceChecksums {
        gate_output_checksum: f32_vector_checksum(&gate_output),
        gate_output_sample_words: f32_vector_sample_words(&gate_output),
        up_output_checksum: f32_vector_checksum(&up_output),
        up_output_sample_words: f32_vector_sample_words(&up_output),
        activation_checksum: f32_vector_checksum(&activation),
        activation_sample_words: f32_vector_sample_words(&activation),
        down_output_checksum: f32_vector_checksum(&down_output),
        down_output_sample_words: f32_vector_sample_words(&down_output),
    })
}

fn deterministic_reference_hidden(hidden_size: usize) -> Vec<f32> {
    (0..hidden_size)
        .map(|index| (((index * 17 + 3) % 251) as f32 - 125.0) / 64.0)
        .collect()
}

fn deterministic_logits_hidden(hidden_size: usize, step_index: u64) -> Vec<f32> {
    let step = (step_index as usize).wrapping_add(1);
    (0..hidden_size)
        .map(|index| (((index * 31 + step * 19 + 7) % 509) as f32 - 254.0) / 96.0)
        .collect()
}

fn rmsnorm_reference(hidden: &[f32], weight: &[f32]) -> Vec<f32> {
    let mean_square = hidden.iter().map(|value| value * value).sum::<f32>() / hidden.len() as f32;
    let inv_rms = 1.0 / (mean_square + 1.0e-6).sqrt();
    hidden
        .iter()
        .zip(weight.iter())
        .map(|(value, gamma)| value * inv_rms * gamma)
        .collect()
}

fn load_weight_vector(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    name: &str,
    expected_elems: usize,
) -> Result<Vec<f32>, String> {
    load_weight_vector_with_payloads(tensors, None, name, expected_elems)
}

fn load_weight_vector_with_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    name: &str,
    expected_elems: usize,
) -> Result<Vec<f32>, String> {
    let tensor = tensors
        .get(name)
        .ok_or_else(|| format!("qwen3_dense_0_6b_missing_weight_tensor:{name}"))?;
    let actual_elems = tensor.shape.iter().try_fold(1usize, |acc, dim| {
        acc.checked_mul(*dim as usize)
            .ok_or_else(|| format!("qwen3_weight_vector_elems_overflow:{name}"))
    })?;
    if actual_elems != expected_elems {
        return Err(format!(
            "qwen3_weight_vector_elems_mismatch:{name}:got={actual_elems}:expected={expected_elems}"
        ));
    }
    let payload = materialize_full_tensor_payload_with_payloads(name, tensor, layer_payloads)?;
    decode_weight_vector(tensor.dtype, &payload, expected_elems)
}

fn matmul_full_tensor(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    name: &str,
    rows: usize,
    cols: usize,
    input: &[f32],
) -> Result<Vec<f32>, String> {
    matmul_full_tensor_with_payloads(tensors, None, name, rows, cols, input)
}

fn matmul_full_tensor_with_payloads(
    tensors: &BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
    layer_payloads: Option<&BTreeMap<String, Vec<u8>>>,
    name: &str,
    rows: usize,
    cols: usize,
    input: &[f32],
) -> Result<Vec<f32>, String> {
    if input.len() != cols {
        return Err(format!(
            "qwen3_matmul_input_cols_mismatch:{name}:got={}:expected={cols}",
            input.len()
        ));
    }
    let tensor = tensors
        .get(name)
        .ok_or_else(|| format!("qwen3_dense_0_6b_missing_weight_tensor:{name}"))?;
    let expected_shape = vec![rows as u64, cols as u64];
    if tensor.shape != expected_shape {
        return Err(format!(
            "qwen3_dense_0_6b_weight_shape_mismatch:{name}:got={:?}:expected={:?}",
            tensor.shape, expected_shape
        ));
    }
    let payload = materialize_full_tensor_payload_with_payloads(name, tensor, layer_payloads)?;
    matmul_reference_values(tensor.dtype, &payload, rows, input)
}

fn logits_head_tensor<'a>(
    tensors: &'a BTreeMap<String, Qwen3Dense06bWeightTensorMetadata>,
) -> Result<(&'static str, &'a Qwen3Dense06bWeightTensorMetadata), String> {
    if let Some(tensor) = tensors.get("lm_head.weight") {
        return Ok(("lm_head.weight", tensor));
    }
    tensors
        .get("model.embed_tokens.weight")
        .map(|tensor| ("model.embed_tokens.weight", tensor))
        .ok_or_else(|| "qwen3_dense_0_6b_missing_logits_head_tensor".to_string())
}

fn apply_rope(
    values: &[f32],
    heads: usize,
    head_dim: usize,
    position: u64,
    theta: f32,
) -> Result<Vec<f32>, String> {
    let expected = heads
        .checked_mul(head_dim)
        .ok_or_else(|| "qwen3_rope_elems_overflow".to_string())?;
    if values.len() != expected {
        return Err(format!(
            "qwen3_rope_len_mismatch:got={}:expected={expected}",
            values.len()
        ));
    }
    if head_dim % 2 != 0 {
        return Err(format!("qwen3_rope_head_dim_odd:head_dim={head_dim}"));
    }
    let mut out = values.to_vec();
    let half_dim = head_dim / 2;
    for head in 0..heads {
        let head_base = head * head_dim;
        for pair in 0..half_dim {
            let angle = position as f32 / theta.powf((pair * 2) as f32 / head_dim as f32);
            let (sin, cos) = angle.sin_cos();
            let x0 = values[head_base + pair];
            let x1 = values[head_base + pair + half_dim];
            out[head_base + pair] = x0 * cos - x1 * sin;
            out[head_base + pair + half_dim] = x1 * cos + x0 * sin;
        }
    }
    Ok(out)
}

fn single_token_gqa_attention_context(
    q: &[f32],
    k: &[f32],
    v: &[f32],
    q_heads: usize,
    kv_heads: usize,
    head_dim: usize,
) -> Result<Vec<f32>, String> {
    if kv_heads == 0 || q_heads == 0 || q_heads % kv_heads != 0 {
        return Err(format!(
            "qwen3_gqa_bad_heads:q_heads={q_heads}:kv_heads={kv_heads}"
        ));
    }
    let q_len = q_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "qwen3_gqa_q_len_overflow".to_string())?;
    let kv_len = kv_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "qwen3_gqa_kv_len_overflow".to_string())?;
    if q.len() != q_len {
        return Err(format!(
            "qwen3_gqa_q_len_mismatch:got={}:expected={q_len}",
            q.len()
        ));
    }
    if k.len() != kv_len {
        return Err(format!(
            "qwen3_gqa_k_len_mismatch:got={}:expected={kv_len}",
            k.len()
        ));
    }
    if v.len() != kv_len {
        return Err(format!(
            "qwen3_gqa_v_len_mismatch:got={}:expected={kv_len}",
            v.len()
        ));
    }
    if !q.iter().chain(k.iter()).all(|value| value.is_finite()) {
        return Err("qwen3_gqa_qk_non_finite".to_string());
    }
    let group_size = q_heads / kv_heads;
    let mut out = vec![0.0f32; q_len];
    for q_head in 0..q_heads {
        let kv_head = q_head / group_size;
        let out_base = q_head * head_dim;
        let v_base = kv_head * head_dim;
        out[out_base..out_base + head_dim].copy_from_slice(&v[v_base..v_base + head_dim]);
    }
    Ok(out)
}

fn causal_gqa_attention_context_for_position(
    q_states: &[Vec<f32>],
    k_states: &[Vec<f32>],
    v_states: &[Vec<f32>],
    position: usize,
    q_heads: usize,
    kv_heads: usize,
    head_dim: usize,
) -> Result<Vec<f32>, String> {
    if q_states.len() != k_states.len() || q_states.len() != v_states.len() {
        return Err(format!(
            "qwen3_causal_gqa_sequence_len_mismatch:q={}:k={}:v={}",
            q_states.len(),
            k_states.len(),
            v_states.len()
        ));
    }
    if position >= q_states.len() {
        return Err(format!(
            "qwen3_causal_gqa_position_oob:position={position}:len={}",
            q_states.len()
        ));
    }
    if kv_heads == 0 || q_heads == 0 || q_heads % kv_heads != 0 {
        return Err(format!(
            "qwen3_causal_gqa_bad_heads:q_heads={q_heads}:kv_heads={kv_heads}"
        ));
    }
    let q_len = q_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "qwen3_causal_gqa_q_len_overflow".to_string())?;
    let kv_len = kv_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "qwen3_causal_gqa_kv_len_overflow".to_string())?;
    for (index, q) in q_states.iter().enumerate() {
        if q.len() != q_len {
            return Err(format!(
                "qwen3_causal_gqa_q_len_mismatch:index={index}:got={}:expected={q_len}",
                q.len()
            ));
        }
    }
    for (index, k) in k_states.iter().enumerate() {
        if k.len() != kv_len {
            return Err(format!(
                "qwen3_causal_gqa_k_len_mismatch:index={index}:got={}:expected={kv_len}",
                k.len()
            ));
        }
    }
    for (index, v) in v_states.iter().enumerate() {
        if v.len() != kv_len {
            return Err(format!(
                "qwen3_causal_gqa_v_len_mismatch:index={index}:got={}:expected={kv_len}",
                v.len()
            ));
        }
    }
    let group_size = q_heads / kv_heads;
    let scale = (head_dim as f32).sqrt().recip();
    let mut out = vec![0.0f32; q_len];
    for q_head in 0..q_heads {
        let kv_head = q_head / group_size;
        let q_base = q_head * head_dim;
        let kv_base = kv_head * head_dim;
        let mut scores = Vec::with_capacity(position + 1);
        let mut max_score = f32::NEG_INFINITY;
        for key_position in 0..=position {
            let dot = (0..head_dim)
                .map(|dim| q_states[position][q_base + dim] * k_states[key_position][kv_base + dim])
                .sum::<f32>()
                * scale;
            if !dot.is_finite() {
                return Err("qwen3_causal_gqa_score_non_finite".to_string());
            }
            max_score = max_score.max(dot);
            scores.push(dot);
        }
        let mut denom = 0.0f32;
        for score in &mut scores {
            *score = (*score - max_score).exp();
            denom += *score;
        }
        if !denom.is_finite() || denom <= 0.0 {
            return Err("qwen3_causal_gqa_softmax_denominator_invalid".to_string());
        }
        for dim in 0..head_dim {
            let mut value = 0.0f32;
            for key_position in 0..=position {
                let prob = scores[key_position] / denom;
                value += prob * v_states[key_position][kv_base + dim];
            }
            out[q_base + dim] = value;
        }
    }
    Ok(out)
}

fn causal_gqa_attention_context_for_query(
    q: &[f32],
    k_states: &[Vec<f32>],
    v_states: &[Vec<f32>],
    q_heads: usize,
    kv_heads: usize,
    head_dim: usize,
) -> Result<Vec<f32>, String> {
    if k_states.len() != v_states.len() {
        return Err(format!(
            "qwen3_incremental_gqa_sequence_len_mismatch:k={}:v={}",
            k_states.len(),
            v_states.len()
        ));
    }
    if k_states.is_empty() {
        return Err("qwen3_incremental_gqa_empty_cache".to_string());
    }
    if kv_heads == 0 || q_heads == 0 || q_heads % kv_heads != 0 {
        return Err(format!(
            "qwen3_incremental_gqa_bad_heads:q_heads={q_heads}:kv_heads={kv_heads}"
        ));
    }
    let q_len = q_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "qwen3_incremental_gqa_q_len_overflow".to_string())?;
    let kv_len = kv_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "qwen3_incremental_gqa_kv_len_overflow".to_string())?;
    if q.len() != q_len {
        return Err(format!(
            "qwen3_incremental_gqa_q_len_mismatch:got={}:expected={q_len}",
            q.len()
        ));
    }
    for (index, k) in k_states.iter().enumerate() {
        if k.len() != kv_len {
            return Err(format!(
                "qwen3_incremental_gqa_k_len_mismatch:index={index}:got={}:expected={kv_len}",
                k.len()
            ));
        }
    }
    for (index, v) in v_states.iter().enumerate() {
        if v.len() != kv_len {
            return Err(format!(
                "qwen3_incremental_gqa_v_len_mismatch:index={index}:got={}:expected={kv_len}",
                v.len()
            ));
        }
    }
    let group_size = q_heads / kv_heads;
    let scale = (head_dim as f32).sqrt().recip();
    let mut out = vec![0.0f32; q_len];
    for q_head in 0..q_heads {
        let kv_head = q_head / group_size;
        let q_base = q_head * head_dim;
        let kv_base = kv_head * head_dim;
        let mut scores = Vec::with_capacity(k_states.len());
        let mut max_score = f32::NEG_INFINITY;
        for key in k_states {
            let dot = (0..head_dim)
                .map(|dim| q[q_base + dim] * key[kv_base + dim])
                .sum::<f32>()
                * scale;
            if !dot.is_finite() {
                return Err("qwen3_incremental_gqa_score_non_finite".to_string());
            }
            max_score = max_score.max(dot);
            scores.push(dot);
        }
        let mut denom = 0.0f32;
        for score in &mut scores {
            *score = (*score - max_score).exp();
            denom += *score;
        }
        if !denom.is_finite() || denom <= 0.0 {
            return Err("qwen3_incremental_gqa_softmax_denominator_invalid".to_string());
        }
        for dim in 0..head_dim {
            let mut value = 0.0f32;
            for key_position in 0..v_states.len() {
                let prob = scores[key_position] / denom;
                value += prob * v_states[key_position][kv_base + dim];
            }
            out[q_base + dim] = value;
        }
    }
    Ok(out)
}

fn vector_add(lhs: &[f32], rhs: &[f32]) -> Result<Vec<f32>, String> {
    if lhs.len() != rhs.len() {
        return Err(format!(
            "qwen3_vector_add_len_mismatch:lhs={}:rhs={}",
            lhs.len(),
            rhs.len()
        ));
    }
    Ok(lhs
        .iter()
        .zip(rhs.iter())
        .map(|(left, right)| left + right)
        .collect())
}

fn swiglu_activation(gate: &[f32], up: &[f32]) -> Result<Vec<f32>, String> {
    if gate.len() != up.len() {
        return Err(format!(
            "qwen3_swiglu_len_mismatch:gate={}:up={}",
            gate.len(),
            up.len()
        ));
    }
    Ok(gate
        .iter()
        .zip(up.iter())
        .map(|(gate, up)| {
            let silu = gate / (1.0 + (-gate).exp());
            silu * up
        })
        .collect())
}

fn rmsnorm_per_head_reference(
    values: &[f32],
    heads: usize,
    head_dim: usize,
    weight: &[f32],
) -> Result<Vec<f32>, String> {
    if heads == 0 || head_dim == 0 {
        return Err(format!(
            "qwen3_qk_norm_bad_shape:heads={heads}:head_dim={head_dim}"
        ));
    }
    if weight.len() != head_dim {
        return Err(format!(
            "qwen3_qk_norm_weight_len_mismatch:got={}:expected={head_dim}",
            weight.len()
        ));
    }
    let expected = heads
        .checked_mul(head_dim)
        .ok_or_else(|| "qwen3_qk_norm_elems_overflow".to_string())?;
    if values.len() != expected {
        return Err(format!(
            "qwen3_qk_norm_values_len_mismatch:got={}:expected={expected}",
            values.len()
        ));
    }
    let mut out = vec![0.0f32; values.len()];
    for head in 0..heads {
        let base = head * head_dim;
        let mean_square = values[base..base + head_dim]
            .iter()
            .map(|value| value * value)
            .sum::<f32>()
            / head_dim as f32;
        let scale = (mean_square + 1.0e-6).sqrt().recip();
        for dim in 0..head_dim {
            out[base + dim] = values[base + dim] * scale * weight[dim];
        }
    }
    Ok(out)
}

fn matmul_reference_values(
    dtype: Qwen3Dense06bWeightDType,
    payload: &[u8],
    rows: usize,
    input: &[f32],
) -> Result<Vec<f32>, String> {
    let expected = rows
        .checked_mul(input.len())
        .ok_or_else(|| "qwen3_qkv_reference_matrix_elems_overflow".to_string())?;
    let weights = decode_weight_vector(dtype, payload, expected)?;
    let mut outputs = Vec::with_capacity(rows);
    for row in 0..rows {
        let row_base = row * input.len();
        let dot = input
            .iter()
            .enumerate()
            .map(|(col, value)| value * weights[row_base + col])
            .sum::<f32>();
        outputs.push(dot);
    }
    Ok(outputs)
}

fn decode_weight_vector(
    dtype: Qwen3Dense06bWeightDType,
    payload: &[u8],
    expected_elems: usize,
) -> Result<Vec<f32>, String> {
    let elem_size = dtype_size(dtype) as usize;
    let expected_bytes = expected_elems
        .checked_mul(elem_size)
        .ok_or_else(|| "qwen3_weight_decode_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "qwen3_weight_decode_len_mismatch:dtype={dtype:?}:got={}:expected={expected_bytes}",
            payload.len()
        ));
    }
    let mut out = Vec::with_capacity(expected_elems);
    for index in 0..expected_elems {
        let base = index * elem_size;
        let value = match dtype {
            Qwen3Dense06bWeightDType::F32 => {
                f32::from_le_bytes(payload[base..base + 4].try_into().expect("f32 weight"))
            }
            Qwen3Dense06bWeightDType::F16 => f16_bits_to_f32(u16::from_le_bytes(
                payload[base..base + 2].try_into().expect("f16 weight"),
            )),
            Qwen3Dense06bWeightDType::BF16 => f32::from_bits(
                (u16::from_le_bytes(payload[base..base + 2].try_into().expect("bf16 weight"))
                    as u32)
                    << 16,
            ),
            Qwen3Dense06bWeightDType::I8 => payload[base] as i8 as f32,
            Qwen3Dense06bWeightDType::U8 => payload[base] as f32,
        };
        out.push(value);
    }
    Ok(out)
}

fn f16_bits_to_f32(bits: u16) -> f32 {
    let sign = ((bits & 0x8000) as u32) << 16;
    let exponent = (bits >> 10) & 0x1f;
    let fraction = bits & 0x03ff;
    let f32_bits = match exponent {
        0 => {
            if fraction == 0 {
                sign
            } else {
                let mut mantissa = fraction as u32;
                let mut exp = -14i32;
                while (mantissa & 0x0400) == 0 {
                    mantissa <<= 1;
                    exp -= 1;
                }
                mantissa &= 0x03ff;
                sign | (((exp + 127) as u32) << 23) | (mantissa << 13)
            }
        }
        0x1f => sign | 0x7f80_0000 | ((fraction as u32) << 13),
        _ => sign | (((exponent as u32) + 112) << 23) | ((fraction as u32) << 13),
    };
    f32::from_bits(f32_bits)
}

fn f32_vector_checksum(values: &[f32]) -> u64 {
    let mut acc = 0xcbf2_9ce4_8422_2325u64;
    for (index, value) in values.iter().enumerate() {
        acc ^= value.to_bits() as u64;
        acc ^= (index as u64).rotate_left(17);
        acc = acc.wrapping_mul(0x0000_0100_0000_01b3);
    }
    acc
}

fn f32_vector_sample_words(values: &[f32]) -> [u64; 4] {
    if values.is_empty() {
        return [0; 4];
    }
    let offsets = [0, values.len() / 3, values.len() * 2 / 3, values.len() - 1];
    let mut words = [0u64; 4];
    for (word, offset) in words.iter_mut().zip(offsets) {
        *word = values[offset].to_bits() as u64 ^ (offset as u64).rotate_left(32);
    }
    words
}

fn materialize_full_tensor_payload(
    tensor_name: &str,
    metadata: &Qwen3Dense06bWeightTensorMetadata,
) -> Result<Vec<u8>, String> {
    materialize_full_tensor_payload_with_payloads(tensor_name, metadata, None)
}

fn materialize_full_tensor_payload_with_payloads(
    tensor_name: &str,
    metadata: &Qwen3Dense06bWeightTensorMetadata,
    layer_payloads: Option<&BTreeMap<String, Vec<u8>>>,
) -> Result<Vec<u8>, String> {
    if let Some(payload) = layer_payloads.and_then(|payloads| payloads.get(tensor_name)) {
        let expected_bytes = metadata
            .shape
            .iter()
            .try_fold(dtype_size(metadata.dtype), |acc, dim| acc.checked_mul(*dim))
            .ok_or_else(|| format!("qwen3_weight_payload_bytes_overflow:{tensor_name}"))?;
        if payload.len() as u64 != expected_bytes {
            return Err(format!(
                "qwen3_weight_payload_override_len_mismatch:{tensor_name}:got={}:expected={expected_bytes}",
                payload.len()
            ));
        }
        return Ok(payload.clone());
    }
    let source_file = metadata
        .source_file
        .as_ref()
        .ok_or_else(|| format!("qwen3_weight_payload_source_file_missing:{tensor_name}"))?;
    let offsets = metadata
        .data_offsets
        .ok_or_else(|| format!("qwen3_weight_payload_offsets_missing:{tensor_name}"))?;
    let bytes = offsets[1]
        .checked_sub(offsets[0])
        .ok_or_else(|| format!("qwen3_weight_payload_bad_offsets:{tensor_name}"))?;
    let expected_bytes = metadata
        .shape
        .iter()
        .try_fold(dtype_size(metadata.dtype), |acc, dim| acc.checked_mul(*dim))
        .ok_or_else(|| format!("qwen3_weight_payload_bytes_overflow:{tensor_name}"))?;
    if bytes != expected_bytes {
        return Err(format!(
            "qwen3_weight_payload_len_mismatch:{tensor_name}:got={bytes}:expected={expected_bytes}"
        ));
    }
    let tensor_base = metadata
        .data_base_offset
        .checked_add(offsets[0])
        .ok_or_else(|| format!("qwen3_weight_payload_base_overflow:{tensor_name}"))?;
    read_file_range(source_file, tensor_base, bytes)
}

fn materialize_tensor_row_payload_with_payloads(
    tensor_name: &str,
    metadata: &Qwen3Dense06bWeightTensorMetadata,
    row: u64,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
) -> Result<Vec<u8>, String> {
    if metadata.shape.len() != 2 {
        return Err(format!(
            "qwen3_weight_payload_row_requires_2d:{tensor_name}:rank={}",
            metadata.shape.len()
        ));
    }
    if row >= metadata.shape[0] {
        return Err(format!(
            "qwen3_weight_payload_row_oob:{tensor_name}:row={row}:rows={}",
            metadata.shape[0]
        ));
    }
    let row_bytes = metadata.shape[1]
        .checked_mul(dtype_size(metadata.dtype))
        .ok_or_else(|| format!("qwen3_weight_payload_row_overflow:{tensor_name}"))?;
    if let Some(payload) = tensor_payloads.and_then(|payloads| payloads.get(tensor_name)) {
        let expected_bytes = metadata
            .shape
            .iter()
            .try_fold(dtype_size(metadata.dtype), |acc, dim| acc.checked_mul(*dim))
            .ok_or_else(|| format!("qwen3_weight_payload_bytes_overflow:{tensor_name}"))?;
        if payload.len() as u64 != expected_bytes {
            return Err(format!(
                "qwen3_weight_payload_override_len_mismatch:{tensor_name}:got={}:expected={expected_bytes}",
                payload.len()
            ));
        }
        let start = row
            .checked_mul(row_bytes)
            .ok_or_else(|| format!("qwen3_weight_payload_row_offset_overflow:{tensor_name}"))?
            as usize;
        let end = start
            .checked_add(row_bytes as usize)
            .ok_or_else(|| format!("qwen3_weight_payload_row_end_overflow:{tensor_name}"))?;
        return payload
            .get(start..end)
            .map(Vec::from)
            .ok_or_else(|| format!("qwen3_weight_payload_row_oob:{tensor_name}:row={row}"));
    }
    let source_file = metadata
        .source_file
        .as_ref()
        .ok_or_else(|| format!("qwen3_weight_payload_source_file_missing:{tensor_name}"))?;
    let offsets = metadata
        .data_offsets
        .ok_or_else(|| format!("qwen3_weight_payload_offsets_missing:{tensor_name}"))?;
    let tensor_base = metadata
        .data_base_offset
        .checked_add(offsets[0])
        .ok_or_else(|| format!("qwen3_weight_payload_base_overflow:{tensor_name}"))?;
    let start = tensor_base
        .checked_add(
            row.checked_mul(row_bytes)
                .ok_or_else(|| format!("qwen3_weight_payload_row_offset_overflow:{tensor_name}"))?,
        )
        .ok_or_else(|| format!("qwen3_weight_payload_row_offset_overflow:{tensor_name}"))?;
    read_file_range(source_file, start, row_bytes)
}

fn materialize_tensor_row_range_payload_with_payloads(
    tensor_name: &str,
    metadata: &Qwen3Dense06bWeightTensorMetadata,
    start_row: u64,
    rows: usize,
    tensor_payloads: Option<&BTreeMap<String, Vec<u8>>>,
) -> Result<Vec<u8>, String> {
    if metadata.shape.len() != 2 {
        return Err(format!(
            "qwen3_weight_payload_row_range_requires_2d:{tensor_name}:rank={}",
            metadata.shape.len()
        ));
    }
    if rows == 0 {
        return Err(format!(
            "qwen3_weight_payload_row_range_empty:{tensor_name}"
        ));
    }
    let end_row = start_row
        .checked_add(rows as u64)
        .ok_or_else(|| format!("qwen3_weight_payload_row_range_end_overflow:{tensor_name}"))?;
    if end_row > metadata.shape[0] {
        return Err(format!(
            "qwen3_weight_payload_row_range_oob:{tensor_name}:start={start_row}:rows={rows}:total_rows={}",
            metadata.shape[0]
        ));
    }
    let row_bytes = metadata.shape[1]
        .checked_mul(dtype_size(metadata.dtype))
        .ok_or_else(|| format!("qwen3_weight_payload_row_range_row_overflow:{tensor_name}"))?;
    let bytes = row_bytes
        .checked_mul(rows as u64)
        .ok_or_else(|| format!("qwen3_weight_payload_row_range_bytes_overflow:{tensor_name}"))?;
    if let Some(payload) = tensor_payloads.and_then(|payloads| payloads.get(tensor_name)) {
        let expected_bytes = metadata
            .shape
            .iter()
            .try_fold(dtype_size(metadata.dtype), |acc, dim| acc.checked_mul(*dim))
            .ok_or_else(|| format!("qwen3_weight_payload_bytes_overflow:{tensor_name}"))?;
        if payload.len() as u64 != expected_bytes {
            return Err(format!(
                "qwen3_weight_payload_override_len_mismatch:{tensor_name}:got={}:expected={expected_bytes}",
                payload.len()
            ));
        }
        let start = start_row.checked_mul(row_bytes).ok_or_else(|| {
            format!("qwen3_weight_payload_row_range_offset_overflow:{tensor_name}")
        })? as usize;
        let end = start
            .checked_add(bytes as usize)
            .ok_or_else(|| format!("qwen3_weight_payload_row_range_end_overflow:{tensor_name}"))?;
        return payload
            .get(start..end)
            .map(Vec::from)
            .ok_or_else(|| {
                format!(
                    "qwen3_weight_payload_row_range_oob:{tensor_name}:start={start_row}:rows={rows}:total_rows={}",
                    metadata.shape[0]
                )
            });
    }
    let source_file = metadata
        .source_file
        .as_ref()
        .ok_or_else(|| format!("qwen3_weight_payload_source_file_missing:{tensor_name}"))?;
    let offsets = metadata
        .data_offsets
        .ok_or_else(|| format!("qwen3_weight_payload_offsets_missing:{tensor_name}"))?;
    let tensor_base = metadata
        .data_base_offset
        .checked_add(offsets[0])
        .ok_or_else(|| format!("qwen3_weight_payload_base_overflow:{tensor_name}"))?;
    let start = tensor_base
        .checked_add(start_row.checked_mul(row_bytes).ok_or_else(|| {
            format!("qwen3_weight_payload_row_range_offset_overflow:{tensor_name}")
        })?)
        .ok_or_else(|| format!("qwen3_weight_payload_row_range_start_overflow:{tensor_name}"))?;
    read_file_range(source_file, start, bytes)
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
    static CACHE: OnceLock<Mutex<BTreeMap<(String, u64, u64), Vec<u8>>>> = OnceLock::new();
    let cache = CACHE.get_or_init(|| Mutex::new(BTreeMap::new()));
    let key = (path.to_string(), offset, bytes);
    {
        let cache = cache
            .lock()
            .map_err(|_| "qwen3_weight_payload_cache_poisoned".to_string())?;
        if let Some(payload) = cache.get(&key) {
            return Ok(payload.clone());
        }
    }
    let mut file =
        File::open(path).map_err(|err| format!("qwen3_weight_payload_open_failed:{path}:{err}"))?;
    file.seek(SeekFrom::Start(offset))
        .map_err(|err| format!("qwen3_weight_payload_seek_failed:{path}:{offset}:{err}"))?;
    let mut out = vec![0u8; bytes as usize];
    file.read_exact(&mut out)
        .map_err(|err| format!("qwen3_weight_payload_read_failed:{path}:{offset}:{err}"))?;
    cache
        .lock()
        .map_err(|_| "qwen3_weight_payload_cache_poisoned".to_string())?
        .insert(key, out.clone());
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
        let op_kinds: Vec<Qwen3Dense06bLayerOpKind> = graph.ops.iter().map(|op| op.kind).collect();
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
        assert_eq!(
            graph.ops[8].lowering,
            Qwen3Dense06bLoweringKind::TiledMatmul
        );
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

        let tp_plan = tensor_parallel_plan(&topology, QWEN3_DENSE_0_6B_PROFILE).expect("tp plan");
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
    fn tokenizer_policy_is_explicit_and_stable() {
        let policy = tokenizer_policy(QWEN3_DENSE_0_6B_PROFILE);
        assert_eq!(policy.model_id, "Qwen/Qwen3-0.6B");
        assert_eq!(
            policy.tokenizer_family,
            "qwen3-tiktoken-compatible-synthetic-piece"
        );
        assert_eq!(policy.vocab_size, 151_936);
        assert_eq!(policy.synthetic_piece_prefix, "q3_");
        assert_eq!(policy.synthetic_piece_digits, 6);
        assert_eq!(policy.synthetic_piece_bytes, 9);
        assert_eq!(policy.policy_hash, tokenizer_policy_hash(&policy));

        let piece = token_piece_from_policy(policy, 123);
        assert_eq!(piece.token_id, 123);
        assert_eq!(piece.byte_len, 9);
        assert_eq!(piece.word0, u64::from_le_bytes(*b"q3_00012"));
        assert_eq!(piece.word1, u64::from_le_bytes([b'3', 0, 0, 0, 0, 0, 0, 0]));
        assert_ne!(piece.checksum, 0);
        assert_eq!(token_piece_bytes_from_policy(policy, 123), b"q3_000123");
    }

    #[test]
    fn tokenizer_asset_summary_and_piece_use_real_files() {
        let temp =
            std::env::temp_dir().join(format!("qwen3_tokenizer_asset_test_{}", std::process::id()));
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).expect("create tokenizer temp dir");
        fs::write(
            temp.join("tokenizer_config.json"),
            r#"{"added_tokens_decoder":{"151643":{"content":"<|endoftext|>"}}}"#,
        )
        .expect("write tokenizer config");
        fs::write(temp.join("tokenizer.json"), r#"{"model":{"type":"BPE"}}"#)
            .expect("write tokenizer json");
        fs::write(temp.join("vocab.json"), r#"{"hello":0," world":1}"#).expect("write vocab json");
        fs::write(temp.join("merges.txt"), b"#version\nh e\n").expect("write merges");
        fs::write(
            temp.join("generation_config.json"),
            r#"{"eos_token_id":151643}"#,
        )
        .expect("write generation config");

        let summary = load_tokenizer_asset_summary(&temp).expect("tokenizer summary");
        assert_eq!(summary.vocab_size, 151_936);
        assert_eq!(summary.vocab_entries, 2);
        assert_eq!(summary.added_tokens, 1);
        assert_eq!(summary.merge_rules, 1);
        assert_eq!(summary.files.len(), 5);
        assert_ne!(summary.aggregate_checksum, 0);

        let vocab_piece = token_piece_from_tokenizer_path(&temp, 1).expect("vocab token piece");
        assert_eq!(vocab_piece.byte_len, 6);
        assert_eq!(vocab_piece.word0 & 0x0000_ffff_ffff_ffff, 0x646c_726f_7720);
        assert_ne!(vocab_piece.checksum, 0);
        assert_eq!(
            token_piece_bytes_from_tokenizer_path(&temp, 1).expect("vocab token bytes"),
            b" world"
        );
        let added_piece =
            token_piece_from_tokenizer_path(&temp, 151_643).expect("added token piece");
        assert_eq!(added_piece.byte_len, 13);
        assert_eq!(added_piece.word0, u64::from_le_bytes(*b"<|endoft"));
        assert_eq!(
            token_piece_bytes_from_tokenizer_path(&temp, 151_643).expect("added token bytes"),
            b"<|endoftext|>"
        );

        fs::remove_dir_all(&temp).expect("remove tokenizer temp dir");
    }

    #[test]
    fn tokenizer_piece_decode_restores_byte_level_space_marker() {
        assert_eq!(token_piece_decode_bytes("ĠI".as_bytes()), b" I");
        assert_eq!(
            String::from_utf8(token_piece_decode_bytes("Ġconfused".as_bytes()))
                .expect("decoded text"),
            " confused"
        );
    }

    #[test]
    fn tokenizer_prompt_encoding_uses_real_vocab_and_merges() {
        let temp = std::env::temp_dir().join(format!(
            "qwen3_tokenizer_encode_test_{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&temp);
        fs::create_dir_all(&temp).expect("create tokenizer temp dir");
        fs::write(
            temp.join("tokenizer_config.json"),
            r#"{"added_tokens_decoder":{}}"#,
        )
        .expect("write tokenizer config");
        fs::write(
            temp.join("tokenizer.json"),
            r#"{"model":{"type":"BPE","vocab":{"H":0,"e":1,"l":2,"o":3,"He":4,"ll":5,"Hell":6,"Hello":7,"Ġ":8,"Q":9,"w":10,"n":11,"ĠQ":12,"we":13,"wen":14,"ĠQwen":15,"3":16},"merges":[["H","e"],["l","l"],["He","ll"],["Hell","o"],["Ġ","Q"],["w","e"],["we","n"],["ĠQ","wen"]]}}"#,
        )
        .expect("write tokenizer json");
        fs::write(temp.join("vocab.json"), r#"{"Hello":7,"ĠQwen":15,"3":16}"#)
            .expect("write vocab json");
        fs::write(temp.join("merges.txt"), b"#version\nH e\n").expect("write merges");
        fs::write(
            temp.join("generation_config.json"),
            r#"{"eos_token_id":151643}"#,
        )
        .expect("write generation config");

        let encoded =
            tokenize_prompt_from_tokenizer_path(&temp, "Hello Qwen3").expect("tokenize prompt");
        assert_eq!(encoded.token_ids, vec![7, 15, 16]);
        assert_eq!(encoded.token_count, 3);
        assert_eq!(
            encoded.token_checksum,
            prompt_token_ids_checksum(&encoded.token_ids)
        );
        assert_ne!(encoded.token_checksum, 0);

        fs::remove_dir_all(&temp).expect("remove tokenizer temp dir");
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
        assert_eq!(manifest.slices.len(), 28 * 8 * 11);

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

        assert_eq!(
            weight_db_key(q_proj),
            "qwen3_dense_0_6b/layer/0/shard/3/QProj"
        );
        let db_value = weight_db_value(q_proj).expect("db value");
        assert!(db_value.len() > 128);
        assert!(String::from_utf8_lossy(&db_value).contains("storage_ref"));
        let db_puts = weight_db_puts(&manifest).expect("db puts");
        assert_eq!(db_puts.len(), manifest.slices.len());
    }

    #[test]
    fn weight_manifest_accepts_qwen3_14b_shape() {
        let topology = test_topology();
        let profile = Qwen3Dense06bProfile {
            vocab_size: 151_936,
            hidden_size: 5_120,
            intermediate_size: 17_408,
            num_hidden_layers: 40,
            num_attention_heads: 40,
            num_key_value_heads: 8,
            head_dim: 128,
            max_position_embeddings: 40_960,
            rope_theta: 1_000_000,
            prefill_tokens: 128,
            decode_tokens: 1,
            tp_nodes: 8,
        };
        let tensors = test_weight_metadata(profile);
        let manifest = weight_manifest_from_metadata_for_model(
            &topology,
            "Qwen/Qwen3-14B",
            profile,
            "/models/qwen3-14b/model.safetensors.index.json",
            &tensors,
        )
        .expect("14B weight manifest");

        assert_eq!(manifest.model_id, "Qwen/Qwen3-14B");
        assert_eq!(manifest.profile.hidden_size, 5_120);
        assert_eq!(manifest.profile.num_hidden_layers, 40);
        assert_eq!(manifest.profile.intermediate_size, 17_408);
        assert_eq!(manifest.slices.len(), 40 * 8 * 11);
        let q_proj = manifest
            .slices
            .iter()
            .find(|slice| {
                slice.layer_id == 0
                    && slice.shard_id == 7
                    && slice.tensor_kind == Qwen3Dense06bWeightTensorKind::QProj
            })
            .expect("layer0 shard7 q_proj slice");
        assert_eq!(q_proj.global_shape, vec![5_120, 5_120]);
        assert_eq!(q_proj.local_shape, vec![640, 5_120]);
        assert_eq!(q_proj.slice_axis, Some(0));
        assert_eq!(q_proj.slice_start, 4_480);
        assert_eq!(q_proj.slice_end, 5_120);

        let down_proj = manifest
            .slices
            .iter()
            .find(|slice| {
                slice.layer_id == 39
                    && slice.shard_id == 7
                    && slice.tensor_kind == Qwen3Dense06bWeightTensorKind::DownProj
            })
            .expect("layer39 shard7 down_proj slice");
        assert_eq!(down_proj.global_shape, vec![5_120, 17_408]);
        assert_eq!(down_proj.local_shape, vec![5_120, 2_176]);
        assert_eq!(down_proj.slice_axis, Some(1));
        assert_eq!(down_proj.slice_start, 15_232);
        assert_eq!(down_proj.slice_end, 17_408);
    }

    #[test]
    fn generic_profile_sequence_range_forward_runs_real_layers() {
        let profile = Qwen3Dense06bProfile {
            vocab_size: 8,
            hidden_size: 4,
            intermediate_size: 4,
            num_hidden_layers: 2,
            num_attention_heads: 2,
            num_key_value_heads: 2,
            head_dim: 2,
            max_position_embeddings: 32,
            rope_theta: 10_000,
            prefill_tokens: 4,
            decode_tokens: 1,
            tp_nodes: 1,
        };
        let path = std::env::temp_dir().join(format!(
            "qwen3_generic_profile_forward_{}_{}.bin",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time")
                .as_nanos()
        ));
        let mut raw = Vec::new();
        let mut tensors = BTreeMap::new();
        let source_file = path.to_string_lossy().to_string();
        let mut add_tensor = |name: String, shape: Vec<u64>, values: Vec<f32>| {
            let start = raw.len() as u64;
            raw.extend_from_slice(&f32_payload(&values));
            let end = raw.len() as u64;
            tensors.insert(
                name,
                Qwen3Dense06bWeightTensorMetadata {
                    dtype: Qwen3Dense06bWeightDType::F32,
                    shape,
                    data_offsets: Some([start, end]),
                    source_file: Some(source_file.clone()),
                    data_base_offset: 0,
                },
            );
        };
        add_tensor(
            "model.embed_tokens.weight".to_string(),
            vec![profile.vocab_size, profile.hidden_size],
            (0..profile.vocab_size * profile.hidden_size)
                .map(|index| 0.01 + index as f32 * 0.001)
                .collect(),
        );
        for layer_id in 0..profile.num_hidden_layers {
            let prefix = format!("model.layers.{layer_id}");
            add_tensor(
                format!("{prefix}.input_layernorm.weight"),
                vec![profile.hidden_size],
                vec![1.0, 0.9, 1.1, 1.0],
            );
            add_tensor(
                format!("{prefix}.self_attn.q_proj.weight"),
                vec![
                    profile.num_attention_heads * profile.head_dim,
                    profile.hidden_size,
                ],
                vec![0.05 + layer_id as f32 * 0.01; 16],
            );
            add_tensor(
                format!("{prefix}.self_attn.q_norm.weight"),
                vec![profile.head_dim],
                vec![1.0, 1.0],
            );
            add_tensor(
                format!("{prefix}.self_attn.k_proj.weight"),
                vec![
                    profile.num_key_value_heads * profile.head_dim,
                    profile.hidden_size,
                ],
                vec![0.04 + layer_id as f32 * 0.01; 16],
            );
            add_tensor(
                format!("{prefix}.self_attn.k_norm.weight"),
                vec![profile.head_dim],
                vec![1.0, 1.0],
            );
            add_tensor(
                format!("{prefix}.self_attn.v_proj.weight"),
                vec![
                    profile.num_key_value_heads * profile.head_dim,
                    profile.hidden_size,
                ],
                vec![0.03 + layer_id as f32 * 0.01; 16],
            );
            add_tensor(
                format!("{prefix}.self_attn.o_proj.weight"),
                vec![
                    profile.hidden_size,
                    profile.num_attention_heads * profile.head_dim,
                ],
                vec![0.02 + layer_id as f32 * 0.01; 16],
            );
            add_tensor(
                format!("{prefix}.post_attention_layernorm.weight"),
                vec![profile.hidden_size],
                vec![1.0, 1.0, 0.95, 1.05],
            );
            add_tensor(
                format!("{prefix}.mlp.gate_proj.weight"),
                vec![profile.intermediate_size, profile.hidden_size],
                vec![0.025 + layer_id as f32 * 0.01; 16],
            );
            add_tensor(
                format!("{prefix}.mlp.up_proj.weight"),
                vec![profile.intermediate_size, profile.hidden_size],
                vec![0.035 + layer_id as f32 * 0.01; 16],
            );
            add_tensor(
                format!("{prefix}.mlp.down_proj.weight"),
                vec![profile.hidden_size, profile.intermediate_size],
                vec![0.045 + layer_id as f32 * 0.01; 16],
            );
        }
        std::fs::write(&path, raw).expect("write generic profile raw weights");

        let sequence =
            embedding_reference_hidden_sequence_for_profile(profile, &tensors, &[1, 2, 3])
                .expect("generic embedding sequence");
        let (forward, output_sequence) = forward_reference_from_hidden_sequence_range_for_profile(
            profile, &tensors, 0, 2, &sequence,
        )
        .expect("generic sequence range forward");

        assert_eq!(forward.layer_count, 2);
        assert_eq!(forward.layers.len(), 2);
        assert_eq!(forward.hidden_size, 4);
        assert_eq!(forward.position, 2);
        assert_eq!(output_sequence.len(), 3);
        assert_eq!(forward.final_hidden, output_sequence[2]);
        assert_ne!(forward.final_hidden_checksum, 0);
        assert_ne!(forward.aggregate_checksum, 0);
        assert_ne!(
            forward.layers[0].output_checksum,
            forward.layers[1].output_checksum
        );

        let _ = std::fs::remove_file(path);
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
                    vec![
                        profile.num_attention_heads * profile.head_dim,
                        profile.hidden_size,
                    ],
                ),
                (
                    format!("{layer_prefix}.self_attn.q_norm.weight"),
                    vec![profile.head_dim],
                ),
                (
                    format!("{layer_prefix}.self_attn.k_proj.weight"),
                    vec![
                        profile.num_key_value_heads * profile.head_dim,
                        profile.hidden_size,
                    ],
                ),
                (
                    format!("{layer_prefix}.self_attn.k_norm.weight"),
                    vec![profile.head_dim],
                ),
                (
                    format!("{layer_prefix}.self_attn.v_proj.weight"),
                    vec![
                        profile.num_key_value_heads * profile.head_dim,
                        profile.hidden_size,
                    ],
                ),
                (
                    format!("{layer_prefix}.self_attn.o_proj.weight"),
                    vec![
                        profile.hidden_size,
                        profile.num_attention_heads * profile.head_dim,
                    ],
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
        let metadata = loaded
            .tensors
            .get("tensor.weight")
            .expect("tensor metadata");
        assert_eq!(metadata.dtype, Qwen3Dense06bWeightDType::U8);
        assert_eq!(metadata.shape, vec![2, 4]);
        assert_eq!(metadata.data_offsets, Some([0, 8]));
        assert!(metadata
            .source_file
            .as_ref()
            .unwrap()
            .ends_with(".safetensors"));

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

    #[test]
    fn qkv_reference_validation_consumes_real_weight_bytes() {
        let hidden_size = 4;
        let norm_payload = f32_payload(&[1.0, 0.5, 1.5, 2.0]);
        let q_payload = f32_payload(&[
            0.25, 0.5, 0.75, 1.0, //
            1.0, 0.75, 0.5, 0.25,
        ]);
        let k_payload = f32_payload(&[0.125, 0.25, 0.5, 1.0]);
        let v_payload = f32_payload(&[1.0, -0.5, 0.25, -0.125]);

        let reference = qkv_reference_from_payloads(
            Qwen3Dense06bWeightDType::F32,
            Qwen3Dense06bWeightDType::F32,
            Qwen3Dense06bWeightDType::F32,
            Qwen3Dense06bWeightDType::F32,
            hidden_size,
            2,
            1,
            1,
            &norm_payload,
            &q_payload,
            &k_payload,
            &v_payload,
        )
        .expect("qkv reference");
        let values = qkv_reference_values_from_payloads(
            Qwen3Dense06bWeightDType::F32,
            Qwen3Dense06bWeightDType::F32,
            Qwen3Dense06bWeightDType::F32,
            Qwen3Dense06bWeightDType::F32,
            hidden_size,
            2,
            1,
            1,
            &norm_payload,
            &q_payload,
            &k_payload,
            &v_payload,
            None,
        )
        .expect("qkv reference values");
        let real_hidden_values = qkv_reference_values_from_payloads(
            Qwen3Dense06bWeightDType::F32,
            Qwen3Dense06bWeightDType::F32,
            Qwen3Dense06bWeightDType::F32,
            Qwen3Dense06bWeightDType::F32,
            hidden_size,
            2,
            1,
            1,
            &norm_payload,
            &q_payload,
            &k_payload,
            &v_payload,
            Some(&[0.125, -0.25, 0.5, -1.0]),
        )
        .expect("qkv reference values from hidden");
        assert_ne!(reference.rmsnorm_checksum, 0);
        assert_ne!(reference.rmsnorm_sample_words, [0; 4]);
        assert_ne!(reference.q_output_checksum, 0);
        assert_ne!(reference.q_output_sample_words, [0; 4]);
        assert_ne!(reference.k_output_checksum, 0);
        assert_ne!(reference.k_output_sample_words, [0; 4]);
        assert_ne!(reference.v_output_checksum, 0);
        assert_ne!(reference.v_output_sample_words, [0; 4]);
        assert_ne!(reference.q_output_checksum, reference.k_output_checksum);
        assert_ne!(
            weight_bytes_checksum(&q_payload),
            weight_bytes_checksum(&k_payload)
        );
        assert_eq!(values.rmsnorm.len(), hidden_size as usize);
        assert_eq!(values.q_output.len(), 2);
        assert_eq!(values.k_output.len(), 1);
        assert_eq!(values.v_output.len(), 1);
        assert_eq!(values.rmsnorm_checksum, reference.rmsnorm_checksum);
        assert_eq!(values.q_output_checksum, reference.q_output_checksum);
        assert_eq!(values.k_output_checksum, reference.k_output_checksum);
        assert_eq!(values.v_output_checksum, reference.v_output_checksum);
        assert_ne!(
            values.rmsnorm,
            deterministic_reference_hidden(hidden_size as usize)
        );
        assert_ne!(
            values.q_output_checksum, real_hidden_values.q_output_checksum,
            "QKV reference values must depend on supplied real hidden input"
        );

        assert_eq!(f16_bits_to_f32(0x3c00), 1.0);
        assert_eq!(f16_bits_to_f32(0xc000), -2.0);
    }

    #[test]
    fn qkv_reference_layer_summary_covers_all_tp_shards() {
        let path = std::env::temp_dir().join(format!(
            "qwen3_reference_summary_{}_{}.bin",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time")
                .as_nanos()
        ));
        let mut tensors = BTreeMap::new();
        let mut slices = Vec::new();
        let mut raw = Vec::new();
        for shard_id in 0..2 {
            let norm = f32_payload(&[1.0 + shard_id as f32, 0.5, 1.5, 2.0]);
            let q = f32_payload(&[0.25 + shard_id as f32, 0.5, 0.75, 1.0, 1.0, 0.75, 0.5, 0.25]);
            let k = f32_payload(&[0.125, 0.25 + shard_id as f32, 0.5, 1.0]);
            let v = f32_payload(&[1.0, -0.5, 0.25 + shard_id as f32, -0.125]);
            for (kind, name_suffix, shape, payload) in [
                (
                    Qwen3Dense06bWeightTensorKind::InputLayerNorm,
                    "input_layernorm",
                    vec![4],
                    norm,
                ),
                (
                    Qwen3Dense06bWeightTensorKind::QProj,
                    "q_proj",
                    vec![2, 4],
                    q,
                ),
                (
                    Qwen3Dense06bWeightTensorKind::KProj,
                    "k_proj",
                    vec![1, 4],
                    k,
                ),
                (
                    Qwen3Dense06bWeightTensorKind::VProj,
                    "v_proj",
                    vec![1, 4],
                    v,
                ),
            ] {
                let tensor_name = format!("test.layer0.shard{shard_id}.{name_suffix}");
                let start = raw.len() as u64;
                raw.extend_from_slice(&payload);
                let end = raw.len() as u64;
                tensors.insert(
                    tensor_name.clone(),
                    Qwen3Dense06bWeightTensorMetadata {
                        dtype: Qwen3Dense06bWeightDType::F32,
                        shape: shape.clone(),
                        data_offsets: Some([start, end]),
                        source_file: Some(path.to_string_lossy().to_string()),
                        data_base_offset: 0,
                    },
                );
                slices.push(Qwen3Dense06bWeightSlice {
                    layer_id: 0,
                    shard_id,
                    tensor_kind: kind,
                    tensor_name,
                    dtype: Qwen3Dense06bWeightDType::F32,
                    global_shape: shape.clone(),
                    slice_axis: None,
                    slice_start: 0,
                    slice_end: shape[0],
                    local_shape: shape,
                    storage: Qwen3Dense06bWeightStorageRef {
                        kind: Qwen3Dense06bWeightStorageKind::Block,
                        storage_ref: format!("test/shard{shard_id}/{kind:?}"),
                        segment: 0,
                        offset: start,
                        bytes: end - start,
                        checksum: weight_bytes_checksum(&payload),
                    },
                });
            }
        }
        std::fs::write(&path, raw).expect("write test raw weights");
        let manifest = Qwen3Dense06bWeightManifest {
            model_id: "Qwen/Qwen3-0.6B".to_string(),
            source: path.to_string_lossy().to_string(),
            format: "test-raw".to_string(),
            profile: Qwen3Dense06bWeightManifestProfile {
                hidden_size: 4,
                intermediate_size: 8,
                num_hidden_layers: 1,
                num_attention_heads: 2,
                num_key_value_heads: 2,
                head_dim: 1,
                tp_nodes: 2,
            },
            slices,
        };

        let summary =
            qkv_reference_layer_summary(&manifest, &tensors, 0).expect("layer reference summary");
        let values =
            qkv_reference_layer_values(&manifest, &tensors, 0).expect("layer reference values");
        assert_eq!(summary.layer_id, 0);
        assert_eq!(summary.shard_count, 2);
        assert_eq!(summary.shards.len(), 2);
        assert_eq!(values.layer_id, 0);
        assert_eq!(values.shard_count, 2);
        assert_eq!(values.shards.len(), 2);
        assert_eq!(summary.total_weight_bytes, (4 + 8 + 4 + 4) * 4 * 2);
        assert_eq!(summary.total_q_rows, 4);
        assert_eq!(summary.total_k_rows, 2);
        assert_eq!(summary.total_v_rows, 2);
        assert_ne!(summary.aggregate_checksum, 0);
        assert_ne!(values.aggregate_checksum, 0);
        assert_eq!(
            values.shards[0].rmsnorm_checksum,
            summary.shards[0].rmsnorm_checksum
        );
        assert_eq!(
            values.shards[0].q_output_checksum,
            summary.shards[0].q_output_checksum
        );
        assert_eq!(values.shards[0].rmsnorm.len(), 4);
        assert_eq!(values.shards[0].q_output.len(), 2);
        assert_eq!(values.shards[0].k_output.len(), 1);
        assert_eq!(values.shards[0].v_output.len(), 1);
        assert_ne!(
            summary.shards[0].q_output_checksum,
            summary.shards[1].q_output_checksum
        );
        assert_ne!(summary.shards[0].rmsnorm_sample_words, [0; 4]);
        assert_ne!(summary.shards[0].q_output_sample_words, [0; 4]);
        assert_ne!(summary.shards[0].k_output_sample_words, [0; 4]);
        assert_ne!(summary.shards[0].v_output_sample_words, [0; 4]);
        assert_eq!(summary.shards[0].weight_slices.len(), 4);
        assert_eq!(
            summary.shards[0].weight_slices[1].kind,
            Qwen3Dense06bWeightTensorKind::QProj
        );

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn mlp_reference_layer_summary_covers_gate_up_down_slices() {
        let path = std::env::temp_dir().join(format!(
            "qwen3_mlp_reference_summary_{}_{}.bin",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time")
                .as_nanos()
        ));
        let mut tensors = BTreeMap::new();
        let mut slices = Vec::new();
        let mut raw = Vec::new();
        for shard_id in 0..2 {
            let gate = f32_payload(&[
                0.25 + shard_id as f32,
                0.50,
                0.75,
                1.00,
                1.00,
                0.75,
                0.50,
                0.25,
            ]);
            let up = f32_payload(&[
                0.125,
                0.25 + shard_id as f32,
                0.50,
                1.00,
                0.50,
                0.25,
                0.125,
                0.0625,
            ]);
            let down = f32_payload(&[
                1.00,
                -0.50,
                0.25 + shard_id as f32,
                -0.125,
                0.50,
                0.25,
                -0.25,
                0.125,
            ]);
            for (kind, name_suffix, shape, payload) in [
                (
                    Qwen3Dense06bWeightTensorKind::GateProj,
                    "gate_proj",
                    vec![2, 4],
                    gate,
                ),
                (
                    Qwen3Dense06bWeightTensorKind::UpProj,
                    "up_proj",
                    vec![2, 4],
                    up,
                ),
                (
                    Qwen3Dense06bWeightTensorKind::DownProj,
                    "down_proj",
                    vec![4, 2],
                    down,
                ),
            ] {
                let tensor_name = format!("test.layer0.shard{shard_id}.{name_suffix}");
                let start = raw.len() as u64;
                raw.extend_from_slice(&payload);
                let end = raw.len() as u64;
                tensors.insert(
                    tensor_name.clone(),
                    Qwen3Dense06bWeightTensorMetadata {
                        dtype: Qwen3Dense06bWeightDType::F32,
                        shape: shape.clone(),
                        data_offsets: Some([start, end]),
                        source_file: Some(path.to_string_lossy().to_string()),
                        data_base_offset: 0,
                    },
                );
                slices.push(Qwen3Dense06bWeightSlice {
                    layer_id: 0,
                    shard_id,
                    tensor_kind: kind,
                    tensor_name,
                    dtype: Qwen3Dense06bWeightDType::F32,
                    global_shape: shape.clone(),
                    slice_axis: None,
                    slice_start: 0,
                    slice_end: shape[0],
                    local_shape: shape,
                    storage: Qwen3Dense06bWeightStorageRef {
                        kind: Qwen3Dense06bWeightStorageKind::Block,
                        storage_ref: format!("test/shard{shard_id}/{kind:?}"),
                        segment: 0,
                        offset: start,
                        bytes: end - start,
                        checksum: weight_bytes_checksum(&payload),
                    },
                });
            }
        }
        std::fs::write(&path, raw).expect("write test raw MLP weights");
        let manifest = Qwen3Dense06bWeightManifest {
            model_id: "Qwen/Qwen3-0.6B".to_string(),
            source: path.to_string_lossy().to_string(),
            format: "test-raw".to_string(),
            profile: Qwen3Dense06bWeightManifestProfile {
                hidden_size: 4,
                intermediate_size: 4,
                num_hidden_layers: 1,
                num_attention_heads: 2,
                num_key_value_heads: 2,
                head_dim: 1,
                tp_nodes: 2,
            },
            slices,
        };

        let summary =
            mlp_reference_layer_summary(&manifest, &tensors, 0).expect("MLP reference summary");
        let real_hidden = [0.125, -0.25, 0.375, -0.50];
        let real_hidden_summary =
            mlp_reference_layer_summary_with_hidden(&manifest, &tensors, 0, Some(&real_hidden))
                .expect("MLP reference summary with real hidden");
        assert_eq!(summary.layer_id, 0);
        assert_eq!(summary.shard_count, 2);
        assert_eq!(summary.shards.len(), 2);
        assert_eq!(summary.total_weight_bytes, (8 + 8 + 8) * 4 * 2);
        assert_eq!(summary.total_intermediate_rows, 4);
        assert_ne!(summary.aggregate_checksum, 0);
        assert_ne!(
            summary.shards[0].activation_checksum,
            summary.shards[1].activation_checksum
        );
        assert_ne!(summary.shards[0].activation_sample_words, [0; 4]);
        assert_ne!(summary.shards[0].down_output_sample_words, [0; 4]);
        assert_eq!(summary.shards[0].weight_slices.len(), 3);
        assert_eq!(
            summary.shards[0].weight_slices[2].kind,
            Qwen3Dense06bWeightTensorKind::DownProj
        );
        assert_ne!(
            real_hidden_summary.shards[0].activation_checksum,
            summary.shards[0].activation_checksum,
            "MLP activation reference must depend on supplied real hidden input"
        );
        assert_ne!(
            real_hidden_summary.shards[0].down_output_checksum,
            summary.shards[0].down_output_checksum,
            "MLP down output reference must depend on supplied real hidden input"
        );
        let hidden_mismatch = mlp_reference_layer_summary_with_hidden(
            &manifest,
            &tensors,
            0,
            Some(&real_hidden[..3]),
        )
        .expect_err("wrong hidden length must fail");
        assert!(
            hidden_mismatch.starts_with("qwen3_mlp_reference_hidden_size_mismatch"),
            "{hidden_mismatch}"
        );

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn logits_reference_summary_reads_real_lm_head_rows() {
        let path = std::env::temp_dir().join(format!(
            "qwen3_logits_reference_{}_{}.bin",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time")
                .as_nanos()
        ));
        let hidden_size = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize;
        let mut raw = Vec::new();
        let norm_offset = raw.len() as u64;
        raw.extend_from_slice(&f32_payload(&vec![1.0; hidden_size]));
        let norm_end = raw.len() as u64;
        let lm_offset = raw.len() as u64;
        raw.extend_from_slice(&f32_payload(&vec![0.25; hidden_size]));
        raw.extend_from_slice(&f32_payload(&vec![0.5; hidden_size]));
        raw.extend_from_slice(&f32_payload(&vec![0.75; hidden_size]));
        std::fs::write(&path, raw).expect("write logits reference weights");

        let source_file = path.to_string_lossy().to_string();
        let mut tensors = BTreeMap::new();
        tensors.insert(
            "model.norm.weight".to_string(),
            Qwen3Dense06bWeightTensorMetadata {
                dtype: Qwen3Dense06bWeightDType::F32,
                shape: vec![QWEN3_DENSE_0_6B_PROFILE.hidden_size],
                data_offsets: Some([norm_offset, norm_end]),
                source_file: Some(source_file.clone()),
                data_base_offset: 0,
            },
        );
        tensors.insert(
            "lm_head.weight".to_string(),
            Qwen3Dense06bWeightTensorMetadata {
                dtype: Qwen3Dense06bWeightDType::F32,
                shape: vec![
                    QWEN3_DENSE_0_6B_PROFILE.vocab_size,
                    QWEN3_DENSE_0_6B_PROFILE.hidden_size,
                ],
                data_offsets: Some([
                    lm_offset,
                    lm_offset
                        + QWEN3_DENSE_0_6B_PROFILE.vocab_size
                            * QWEN3_DENSE_0_6B_PROFILE.hidden_size
                            * 4,
                ]),
                source_file: Some(source_file),
                data_base_offset: 0,
            },
        );

        let summary =
            logits_reference_summary(&tensors, &[(0, 0), (1, 2)]).expect("logits reference");
        let real_hidden = vec![0.125; hidden_size];
        let real_hidden_summary =
            logits_reference_summary_with_hidden(&tensors, &[(0, 0), (1, 2)], Some(&real_hidden))
                .expect("logits reference with real hidden");
        assert_eq!(summary.vocab_size, QWEN3_DENSE_0_6B_PROFILE.vocab_size);
        assert_eq!(summary.hidden_size, QWEN3_DENSE_0_6B_PROFILE.hidden_size);
        assert_eq!(summary.final_norm_bytes, (hidden_size * 4) as u64);
        assert_eq!(summary.token_count, 2);
        assert_eq!(summary.tokens.len(), 2);
        assert_eq!(summary.tokens[0].row_bytes, (hidden_size * 4) as u64);
        assert_eq!(summary.tokens[1].token_id, 2);
        assert_ne!(summary.final_norm_checksum, 0);
        assert_ne!(summary.aggregate_checksum, 0);
        assert_ne!(
            summary.tokens[0].row_checksum,
            summary.tokens[1].row_checksum
        );
        assert_ne!(
            summary.tokens[0].logit_checksum,
            summary.tokens[1].logit_checksum
        );
        assert_ne!(
            real_hidden_summary.tokens[0].logit_checksum, summary.tokens[0].logit_checksum,
            "logits reference must depend on supplied real hidden input"
        );
        let hidden_mismatch =
            logits_reference_summary_with_hidden(&tensors, &[(0, 0)], Some(&real_hidden[..3]))
                .expect_err("wrong hidden length must fail");
        assert!(
            hidden_mismatch.starts_with("qwen3_logits_reference_hidden_size_mismatch"),
            "{hidden_mismatch}"
        );

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn embedding_reference_summary_reads_real_token_rows() {
        let path = std::env::temp_dir().join(format!(
            "qwen3_embedding_reference_{}_{}.bin",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time")
                .as_nanos()
        ));
        let hidden_size = QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize;
        let mut raw = Vec::new();
        let embedding_offset = raw.len() as u64;
        raw.extend_from_slice(&f32_payload(&vec![0.125; hidden_size]));
        raw.extend_from_slice(&f32_payload(&vec![0.25; hidden_size]));
        raw.extend_from_slice(&f32_payload(&vec![0.5; hidden_size]));
        std::fs::write(&path, raw).expect("write embedding reference weights");

        let mut tensors = BTreeMap::new();
        tensors.insert(
            "model.embed_tokens.weight".to_string(),
            Qwen3Dense06bWeightTensorMetadata {
                dtype: Qwen3Dense06bWeightDType::F32,
                shape: vec![
                    QWEN3_DENSE_0_6B_PROFILE.vocab_size,
                    QWEN3_DENSE_0_6B_PROFILE.hidden_size,
                ],
                data_offsets: Some([
                    embedding_offset,
                    embedding_offset
                        + QWEN3_DENSE_0_6B_PROFILE.vocab_size
                            * QWEN3_DENSE_0_6B_PROFILE.hidden_size
                            * 4,
                ]),
                source_file: Some(path.to_string_lossy().to_string()),
                data_base_offset: 0,
            },
        );

        let summary = embedding_reference_summary(&tensors, &[0, 2]).expect("embedding reference");
        assert_eq!(summary.vocab_size, QWEN3_DENSE_0_6B_PROFILE.vocab_size);
        assert_eq!(summary.hidden_size, QWEN3_DENSE_0_6B_PROFILE.hidden_size);
        assert_eq!(summary.token_count, 2);
        assert_eq!(summary.tokens.len(), 2);
        assert_eq!(summary.row_byte_count, (hidden_size * 4 * 2) as u64);
        assert_eq!(summary.tokens[0].row_bytes, (hidden_size * 4) as u64);
        assert_eq!(summary.tokens[1].token_id, 2);
        assert_ne!(summary.row_checksum, 0);
        assert_ne!(summary.value_checksum, 0);
        assert_ne!(summary.aggregate_checksum, 0);
        assert_ne!(
            summary.tokens[0].row_checksum,
            summary.tokens[1].row_checksum
        );
        assert_ne!(
            summary.tokens[0].value_checksum,
            summary.tokens[1].value_checksum
        );
        assert_ne!(summary.tokens[0].sample_words, [0; 4]);

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn real_inference_reference_runs_real_layer_stack_full_vocab_and_text_when_assets_are_available(
    ) {
        let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
            return;
        };
        let tokenizer_path = Path::new(&weights_path);
        let prompt_tokens =
            tokenize_prompt_from_tokenizer_path(tokenizer_path, "Hello").expect("tokenize prompt");
        assert!(!prompt_tokens.token_ids.is_empty());
        let loaded = load_safetensors_path_metadata(Path::new(&weights_path))
            .expect("load Qwen3 real weight metadata");

        let reference = real_inference_reference_from_token_ids(
            &loaded.tensors,
            tokenizer_path,
            &prompt_tokens.token_ids,
        )
        .expect("real inference reference");

        assert_eq!(
            reference.forward.layer_count,
            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers
        );
        assert_eq!(
            reference.forward.layers.len(),
            QWEN3_DENSE_0_6B_PROFILE.num_hidden_layers as usize
        );
        assert_eq!(
            reference.forward.final_hidden.len(),
            QWEN3_DENSE_0_6B_PROFILE.hidden_size as usize
        );
        assert_ne!(reference.forward.final_hidden_checksum, 0);
        assert_eq!(
            reference.logits.checked_token_count,
            QWEN3_DENSE_0_6B_PROFILE.vocab_size
        );
        assert_eq!(
            reference.logits.vocab_size,
            QWEN3_DENSE_0_6B_PROFILE.vocab_size
        );
        assert_ne!(reference.logits.final_norm_checksum, 0);
        assert_ne!(reference.logits.logits_checksum, 0);
        assert!(reference.logits.top_token_id < QWEN3_DENSE_0_6B_PROFILE.vocab_size);
        assert_ne!(
            reference.logits.top_token_id,
            reference.logits.runner_up_token_id
        );
        assert_eq!(reference.logits.top_candidates.len(), 4);
        assert_eq!(
            reference.logits.top_candidates[0].token_id,
            reference.logits.top_token_id
        );
        assert_eq!(
            reference.logits.top_candidates[1].token_id,
            reference.logits.runner_up_token_id
        );
        assert!(reference
            .logits
            .top_candidates
            .windows(2)
            .all(|pair| f32::from_bits(pair[0].logit_bits as u32)
                >= f32::from_bits(pair[1].logit_bits as u32)));
        assert_eq!(
            reference.sampled_text.token_id,
            reference.logits.top_token_id
        );
        assert_eq!(
            reference.sampled_text.byte_len as usize,
            reference.sampled_text.bytes.len()
        );
        assert_ne!(reference.sampled_text.byte_checksum, 0);
        assert_ne!(reference.aggregate_checksum, 0);
    }

    #[test]
    fn incremental_kv_cache_forward_matches_full_forward_when_assets_are_available() {
        let Ok(weights_path) = std::env::var("SIM_QWEN3_0_6B_WEIGHTS_PATH") else {
            return;
        };
        let tokenizer_path = Path::new(&weights_path);
        let prompt_tokens = tokenize_prompt_from_tokenizer_path(tokenizer_path, "Hello Qwen3")
            .expect("tokenize prompt");
        if prompt_tokens.token_ids.len() < 2 {
            return;
        }
        let loaded = load_safetensors_path_metadata(Path::new(&weights_path))
            .expect("load Qwen3 real weight metadata");
        let prefix_len = prompt_tokens.token_ids.len() - 1;
        let prefix = &prompt_tokens.token_ids[..prefix_len];
        let full_tokens = &prompt_tokens.token_ids;
        let prefix_cached = forward_with_kv_cache_from_token_ids(&loaded.tensors, prefix)
            .expect("prefix cached forward");
        let next_hidden = embedding_reference_last_hidden(&loaded.tensors, full_tokens)
            .expect("next token embedding");
        let incremental = forward_incremental_with_kv_cache_from_hidden(
            &loaded.tensors,
            &prefix_cached.kv_cache,
            prefix_len as u64,
            &next_hidden,
        )
        .expect("incremental cached forward");
        let full = forward_from_token_ids(&loaded.tensors, full_tokens).expect("full forward");

        assert_eq!(incremental.forward.position, full.position);
        assert_eq!(
            incremental.forward.final_hidden_checksum,
            full.final_hidden_checksum
        );
        assert_eq!(
            incremental.forward.aggregate_checksum,
            full.aggregate_checksum
        );
        assert_eq!(
            incremental.kv_cache[0].token_count,
            full_tokens.len() as u64
        );
    }

    fn f32_payload(values: &[f32]) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(values.len() * 4);
        for value in values {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
        bytes
    }

    fn write_test_safetensors(path: &std::path::Path, header: &str, payload: &[u8]) {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&(header.len() as u64).to_le_bytes());
        bytes.extend_from_slice(header.as_bytes());
        bytes.extend_from_slice(payload);
        std::fs::write(path, bytes).expect("write test safetensors");
    }
}
