//! Out-of-core loader for the official DeepSeek V4 Flash checkpoint.
//!
//! Shard payloads are never read wholesale. Opening a checkpoint reads only
//! the small JSON files and Safetensors headers; tensor data is fetched with
//! positioned reads and retained only in explicitly bounded caches.

use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::fs::{self, File};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

const MAX_SAFETENSORS_HEADER_BYTES: u64 = 64 * 1024 * 1024;
const OFFICIAL_SHARD_COUNT: usize = 46;
const FNV1A64_OFFSET_BASIS: u64 = 0xcbf29ce484222325;
const FNV1A64_PRIME: u64 = 0x100000001b3;

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct DeepseekV4QuantizationConfig {
    pub activation_scheme: String,
    pub fmt: String,
    pub quant_method: String,
    pub scale_fmt: String,
    pub weight_block_size: [u64; 2],
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct DeepseekV4RopeScalingConfig {
    pub beta_fast: f64,
    pub beta_slow: f64,
    pub factor: f64,
    pub original_max_position_embeddings: u64,
    #[serde(rename = "type")]
    pub kind: String,
}

#[derive(Clone, Debug, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct DeepseekV4Config {
    pub architectures: Vec<String>,
    pub attention_bias: bool,
    pub attention_dropout: f64,
    pub bos_token_id: u64,
    pub eos_token_id: u64,
    pub expert_dtype: String,
    pub hc_eps: f64,
    pub hc_mult: u64,
    pub hc_sinkhorn_iters: u64,
    pub head_dim: u64,
    pub hidden_act: String,
    pub hidden_size: u64,
    pub index_head_dim: u64,
    pub index_n_heads: u64,
    pub index_topk: u64,
    pub initializer_range: f64,
    pub max_position_embeddings: u64,
    pub model_type: String,
    pub moe_intermediate_size: u64,
    pub n_routed_experts: u64,
    pub n_shared_experts: u64,
    pub norm_topk_prob: bool,
    pub num_attention_heads: u64,
    pub num_experts_per_tok: u64,
    pub num_hash_layers: u64,
    pub num_hidden_layers: u64,
    pub num_key_value_heads: u64,
    pub num_nextn_predict_layers: u64,
    pub o_groups: u64,
    pub o_lora_rank: u64,
    pub q_lora_rank: u64,
    pub qk_rope_head_dim: u64,
    pub quantization_config: DeepseekV4QuantizationConfig,
    pub rms_norm_eps: f64,
    pub rope_scaling: DeepseekV4RopeScalingConfig,
    pub rope_theta: f64,
    pub routed_scaling_factor: f64,
    pub scoring_func: String,
    pub sliding_window: u64,
    pub swiglu_limit: f64,
    pub tie_word_embeddings: bool,
    pub topk_method: String,
    pub torch_dtype: String,
    pub transformers_version: String,
    pub use_cache: bool,
    pub vocab_size: u64,
    pub compress_rope_theta: u64,
    pub compress_ratios: Vec<u32>,
}

impl DeepseekV4Config {
    pub fn validate_official_flash(&self) -> Result<(), String> {
        let required = [
            ("hidden_size", self.hidden_size, 4_096),
            ("num_hidden_layers", self.num_hidden_layers, 43),
            ("vocab_size", self.vocab_size, 129_280),
            ("num_attention_heads", self.num_attention_heads, 64),
            ("num_key_value_heads", self.num_key_value_heads, 1),
            ("head_dim", self.head_dim, 512),
            ("moe_intermediate_size", self.moe_intermediate_size, 2_048),
            ("n_routed_experts", self.n_routed_experts, 256),
            ("n_shared_experts", self.n_shared_experts, 1),
            ("num_experts_per_tok", self.num_experts_per_tok, 6),
            ("num_hash_layers", self.num_hash_layers, 3),
            ("num_nextn_predict_layers", self.num_nextn_predict_layers, 1),
        ];
        for (name, actual, expected) in required {
            if actual != expected {
                return Err(format!(
                    "deepseek_v4_config_value_mismatch:{name}:actual={actual}:expected={expected}"
                ));
            }
        }
        if self.architectures != ["DeepseekV4ForCausalLM"] {
            return Err("deepseek_v4_config_architecture_mismatch".to_string());
        }
        if self.model_type != "deepseek_v4"
            || self.expert_dtype != "fp4"
            || self.torch_dtype != "bfloat16"
        {
            return Err("deepseek_v4_config_model_or_dtype_mismatch".to_string());
        }
        let quant = &self.quantization_config;
        if quant.activation_scheme != "dynamic"
            || quant.fmt != "e4m3"
            || quant.quant_method != "fp8"
            || quant.scale_fmt != "ue8m0"
            || quant.weight_block_size != [128, 128]
        {
            return Err("deepseek_v4_config_quantization_mismatch".to_string());
        }
        let expected_compress_ratios = self
            .num_hidden_layers
            .checked_add(self.num_nextn_predict_layers)
            .ok_or_else(|| "deepseek_v4_config_layer_count_overflow".to_string())?;
        if self.compress_ratios.len() != expected_compress_ratios as usize
            || self
                .compress_ratios
                .iter()
                .any(|ratio| !matches!(ratio, 0 | 4 | 128))
        {
            return Err("deepseek_v4_config_compress_ratios_invalid".to_string());
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, Deserialize, Serialize, PartialEq, Eq, PartialOrd, Ord)]
pub enum DeepseekV4TensorDType {
    #[serde(rename = "BF16")]
    Bf16,
    #[serde(rename = "F32")]
    F32,
    #[serde(rename = "F8_E4M3")]
    F8E4M3,
    #[serde(rename = "F8_E8M0")]
    F8E8M0,
    #[serde(rename = "I8")]
    I8,
    #[serde(rename = "I64")]
    I64,
}

impl DeepseekV4TensorDType {
    pub fn safetensors_name(self) -> &'static str {
        match self {
            Self::Bf16 => "BF16",
            Self::F32 => "F32",
            Self::F8E4M3 => "F8_E4M3",
            Self::F8E8M0 => "F8_E8M0",
            Self::I8 => "I8",
            Self::I64 => "I64",
        }
    }

    fn storage_bytes(self) -> u64 {
        match self {
            Self::Bf16 => 2,
            Self::F32 => 4,
            Self::F8E4M3 | Self::F8E8M0 | Self::I8 => 1,
            Self::I64 => 8,
        }
    }
}

impl TryFrom<&str> for DeepseekV4TensorDType {
    type Error = String;

    fn try_from(value: &str) -> Result<Self, Self::Error> {
        match value {
            "BF16" => Ok(Self::Bf16),
            "F32" => Ok(Self::F32),
            "F8_E4M3" => Ok(Self::F8E4M3),
            "F8_E8M0" => Ok(Self::F8E8M0),
            "I8" => Ok(Self::I8),
            "I64" => Ok(Self::I64),
            other => Err(format!("deepseek_v4_safetensors_dtype_unsupported:{other}")),
        }
    }
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct DeepseekV4TensorMetadata {
    pub name: String,
    pub shard: String,
    pub dtype: DeepseekV4TensorDType,
    pub shape: Vec<u64>,
    pub data_offsets: [u64; 2],
    pub data_base_offset: u64,
    pub file_len: u64,
    pub scale_tensor: Option<String>,
}

impl DeepseekV4TensorMetadata {
    pub fn payload_bytes(&self) -> u64 {
        self.data_offsets[1] - self.data_offsets[0]
    }

    pub fn absolute_offset(&self) -> Result<u64, String> {
        self.data_base_offset
            .checked_add(self.data_offsets[0])
            .ok_or_else(|| format!("deepseek_v4_tensor_absolute_offset_overflow:{}", self.name))
    }

    pub fn is_routed_expert(&self) -> bool {
        is_routed_expert_name(&self.name)
    }
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct DeepseekV4ShardMetadata {
    pub file_name: String,
    pub file_len: u64,
    pub header_len: u64,
    pub tensor_count: usize,
}

#[derive(Clone, Debug, Serialize, PartialEq, Eq)]
pub struct DeepseekV4CheckpointIdentity {
    pub revision: String,
    pub config_checksum: u64,
    pub index_checksum: u64,
}

#[derive(Clone, Copy, Debug, Default, Serialize, PartialEq, Eq)]
pub struct DeepseekV4CacheStats {
    pub capacity_bytes: u64,
    pub resident_bytes: u64,
    pub peak_resident_bytes: u64,
    pub disk_read_bytes: u64,
    pub hits: u64,
    pub misses: u64,
    pub evictions: u64,
}

#[derive(Clone, Copy, Debug, Serialize, PartialEq, Eq)]
pub struct DeepseekV4CacheLimits {
    pub tensor_bytes: u64,
    pub expert_bytes: u64,
}

impl Default for DeepseekV4CacheLimits {
    fn default() -> Self {
        Self {
            tensor_bytes: 64 * 1024 * 1024,
            expert_bytes: 256 * 1024 * 1024,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
struct CacheKey {
    tensor: String,
    offset: u64,
    bytes: u64,
}

#[derive(Debug)]
struct BoundedCache {
    entries: BTreeMap<CacheKey, Arc<[u8]>>,
    lru: VecDeque<CacheKey>,
    stats: DeepseekV4CacheStats,
}

impl BoundedCache {
    fn new(capacity_bytes: u64) -> Self {
        Self {
            entries: BTreeMap::new(),
            lru: VecDeque::new(),
            stats: DeepseekV4CacheStats {
                capacity_bytes,
                ..DeepseekV4CacheStats::default()
            },
        }
    }
}

#[derive(Debug)]
struct CacheState {
    tensor: BoundedCache,
    expert: BoundedCache,
}

#[derive(Debug)]
pub struct DeepseekV4Checkpoint {
    root: PathBuf,
    pub config: DeepseekV4Config,
    pub identity: DeepseekV4CheckpointIdentity,
    pub tensors: BTreeMap<String, DeepseekV4TensorMetadata>,
    pub shards: Vec<DeepseekV4ShardMetadata>,
    pub total_payload_bytes: u64,
    pub metadata_resident_bytes: u64,
    cache: Mutex<CacheState>,
}

#[derive(Deserialize)]
struct SafetensorsIndexMetadata {
    total_size: u64,
}

#[derive(Deserialize)]
struct SafetensorsIndex {
    metadata: SafetensorsIndexMetadata,
    weight_map: BTreeMap<String, String>,
}

#[derive(Deserialize)]
struct RawTensorMetadata {
    dtype: String,
    shape: Vec<u64>,
    data_offsets: [u64; 2],
}

pub fn checksum64(bytes: &[u8]) -> u64 {
    bytes.iter().fold(FNV1A64_OFFSET_BASIS, |hash, byte| {
        (hash ^ u64::from(*byte)).wrapping_mul(FNV1A64_PRIME)
    })
}

fn is_routed_expert_name(name: &str) -> bool {
    name.contains(".ffn.experts.")
}

impl DeepseekV4Checkpoint {
    pub fn open(
        model_dir: impl AsRef<Path>,
        cache_limits: DeepseekV4CacheLimits,
    ) -> Result<Self, String> {
        let root = fs::canonicalize(model_dir.as_ref()).map_err(|err| {
            format!(
                "deepseek_v4_checkpoint_directory_missing:{}:{err}",
                model_dir.as_ref().display()
            )
        })?;
        if !root.is_dir() {
            return Err(format!(
                "deepseek_v4_checkpoint_not_directory:{}",
                root.display()
            ));
        }

        let config_path = root.join("config.json");
        let index_path = root.join("model.safetensors.index.json");
        let config_bytes = read_small_file(&config_path, 1024 * 1024)?;
        let index_bytes = read_small_file(&index_path, 64 * 1024 * 1024)?;
        let config: DeepseekV4Config = serde_json::from_slice(&config_bytes).map_err(|err| {
            format!(
                "deepseek_v4_config_parse_failed:{}:{err}",
                config_path.display()
            )
        })?;
        config.validate_official_flash()?;
        let index: SafetensorsIndex = serde_json::from_slice(&index_bytes).map_err(|err| {
            format!(
                "deepseek_v4_safetensors_index_parse_failed:{}:{err}",
                index_path.display()
            )
        })?;
        if index.weight_map.is_empty() {
            return Err("deepseek_v4_safetensors_index_empty".to_string());
        }

        let shard_names: BTreeSet<String> = index.weight_map.values().cloned().collect();
        validate_shard_names(&shard_names)?;
        let mut tensors = BTreeMap::new();
        let mut shards = Vec::with_capacity(shard_names.len());
        let mut header_bytes = 0u64;
        for shard_name in &shard_names {
            let (shard, loaded) = load_shard_header(&root, shard_name)?;
            header_bytes = header_bytes
                .checked_add(shard.header_len)
                .ok_or_else(|| "deepseek_v4_header_bytes_overflow".to_string())?;
            for (name, tensor) in loaded {
                match index.weight_map.get(&name) {
                    Some(index_shard) if index_shard == shard_name => {}
                    Some(index_shard) => {
                        return Err(format!(
                            "deepseek_v4_index_header_shard_mismatch:{name}:index={index_shard}:header={shard_name}"
                        ));
                    }
                    None => {
                        return Err(format!(
                            "deepseek_v4_header_tensor_missing_from_index:{name}:shard={shard_name}"
                        ));
                    }
                }
                if tensors.insert(name.clone(), tensor).is_some() {
                    return Err(format!("deepseek_v4_tensor_duplicate_across_shards:{name}"));
                }
            }
            shards.push(shard);
        }
        for (name, shard_name) in &index.weight_map {
            if !tensors.contains_key(name) {
                return Err(format!(
                    "deepseek_v4_index_tensor_missing_from_header:{name}:shard={shard_name}"
                ));
            }
        }
        if tensors.len() != index.weight_map.len() {
            return Err(format!(
                "deepseek_v4_index_header_tensor_count_mismatch:index={}:headers={}",
                index.weight_map.len(),
                tensors.len()
            ));
        }

        validate_quantized_scale_associations(&mut tensors)?;
        validate_model_tensor_schema(&config, &tensors)?;
        let total_payload_bytes = tensors.values().try_fold(0u64, |total, tensor| {
            total
                .checked_add(tensor.payload_bytes())
                .ok_or_else(|| "deepseek_v4_total_payload_bytes_overflow".to_string())
        })?;
        if total_payload_bytes != index.metadata.total_size {
            return Err(format!(
                "deepseek_v4_index_total_size_mismatch:index={}:headers={total_payload_bytes}",
                index.metadata.total_size
            ));
        }

        let config_checksum = checksum64(&config_bytes);
        let index_checksum = checksum64(&index_bytes);
        let revision = read_revision(&root, config_checksum, index_checksum)?;
        let metadata_resident_bytes = (config_bytes.len() as u64)
            .checked_add(index_bytes.len() as u64)
            .and_then(|value| value.checked_add(header_bytes))
            .ok_or_else(|| "deepseek_v4_metadata_resident_bytes_overflow".to_string())?;
        Ok(Self {
            root,
            config,
            identity: DeepseekV4CheckpointIdentity {
                revision,
                config_checksum,
                index_checksum,
            },
            tensors,
            shards,
            total_payload_bytes,
            metadata_resident_bytes,
            cache: Mutex::new(CacheState {
                tensor: BoundedCache::new(cache_limits.tensor_bytes),
                expert: BoundedCache::new(cache_limits.expert_bytes),
            }),
        })
    }

    pub fn tensor(&self, name: &str) -> Result<&DeepseekV4TensorMetadata, String> {
        self.tensors
            .get(name)
            .ok_or_else(|| format!("deepseek_v4_tensor_not_found:{name}"))
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn tensor_names_for_layer_range(
        &self,
        layer_start: u64,
        layer_end: u64,
    ) -> Result<Vec<&str>, String> {
        tensor_names_for_layer_range(
            &self.tensors,
            layer_start,
            layer_end,
            self.config.num_hidden_layers,
        )
    }

    pub fn read_tensor_slice(
        &self,
        name: &str,
        byte_offset: u64,
        bytes: u64,
    ) -> Result<Arc<[u8]>, String> {
        let tensor = self.tensor(name)?;
        if bytes == 0 {
            return Err(format!("deepseek_v4_tensor_slice_empty:{name}"));
        }
        let end = byte_offset.checked_add(bytes).ok_or_else(|| {
            format!("deepseek_v4_tensor_slice_range_overflow:{name}:{byte_offset}:{bytes}")
        })?;
        if end > tensor.payload_bytes() {
            return Err(format!(
                "deepseek_v4_tensor_slice_oob:{name}:offset={byte_offset}:bytes={bytes}:tensor_bytes={}",
                tensor.payload_bytes()
            ));
        }
        let key = CacheKey {
            tensor: name.to_string(),
            offset: byte_offset,
            bytes,
        };
        let expert = tensor.is_routed_expert();
        {
            let mut state = self
                .cache
                .lock()
                .map_err(|_| "deepseek_v4_tensor_cache_poisoned".to_string())?;
            let cache = if expert {
                &mut state.expert
            } else {
                &mut state.tensor
            };
            if let Some(payload) = cache_get(cache, &key) {
                return Ok(payload);
            }
        }

        let absolute_offset = tensor
            .absolute_offset()?
            .checked_add(byte_offset)
            .ok_or_else(|| format!("deepseek_v4_tensor_slice_absolute_offset_overflow:{name}"))?;
        let payload_len = usize::try_from(bytes)
            .map_err(|_| format!("deepseek_v4_tensor_slice_too_large:{name}:{bytes}"))?;
        let shard_path = self.root.join(&tensor.shard);
        let mut payload = vec![0u8; payload_len];
        read_exact_at(&shard_path, absolute_offset, &mut payload)?;
        let payload: Arc<[u8]> = payload.into();

        let mut state = self
            .cache
            .lock()
            .map_err(|_| "deepseek_v4_tensor_cache_poisoned".to_string())?;
        let cache = if expert {
            &mut state.expert
        } else {
            &mut state.tensor
        };
        cache.stats.disk_read_bytes = cache
            .stats
            .disk_read_bytes
            .checked_add(bytes)
            .ok_or_else(|| "deepseek_v4_cache_disk_read_bytes_overflow".to_string())?;
        cache_insert(cache, key, Arc::clone(&payload))?;
        Ok(payload)
    }

    pub fn read_expert_slice(
        &self,
        name: &str,
        byte_offset: u64,
        bytes: u64,
    ) -> Result<Arc<[u8]>, String> {
        if !is_routed_expert_name(name) {
            return Err(format!("deepseek_v4_tensor_is_not_routed_expert:{name}"));
        }
        self.read_tensor_slice(name, byte_offset, bytes)
    }

    pub fn cache_stats(&self) -> Result<(DeepseekV4CacheStats, DeepseekV4CacheStats), String> {
        let state = self
            .cache
            .lock()
            .map_err(|_| "deepseek_v4_tensor_cache_poisoned".to_string())?;
        Ok((state.tensor.stats, state.expert.stats))
    }
}

fn read_small_file(path: &Path, max_bytes: u64) -> Result<Vec<u8>, String> {
    let metadata = fs::metadata(path)
        .map_err(|err| format!("deepseek_v4_required_file_missing:{}:{err}", path.display()))?;
    if metadata.len() > max_bytes {
        return Err(format!(
            "deepseek_v4_metadata_file_too_large:{}:bytes={}:limit={max_bytes}",
            path.display(),
            metadata.len()
        ));
    }
    fs::read(path).map_err(|err| {
        format!(
            "deepseek_v4_metadata_file_read_failed:{}:{err}",
            path.display()
        )
    })
}

fn read_revision(root: &Path, config_checksum: u64, index_checksum: u64) -> Result<String, String> {
    let revision_path = root.join(".mv");
    if revision_path.is_file() {
        let bytes = read_small_file(&revision_path, 4096)?;
        let revision = std::str::from_utf8(&bytes)
            .map_err(|err| format!("deepseek_v4_revision_utf8_failed:{err}"))?
            .trim()
            .to_string();
        if revision.is_empty() {
            return Err("deepseek_v4_revision_empty".to_string());
        }
        return Ok(revision);
    }
    Ok(format!(
        "content-fnv1a64-{config_checksum:016x}-{index_checksum:016x}"
    ))
}

fn validate_shard_names(shards: &BTreeSet<String>) -> Result<(), String> {
    if shards.len() != OFFICIAL_SHARD_COUNT {
        return Err(format!(
            "deepseek_v4_official_shard_count_mismatch:actual={}:expected={OFFICIAL_SHARD_COUNT}",
            shards.len()
        ));
    }
    for shard_id in 1..=OFFICIAL_SHARD_COUNT {
        let expected = format!("model-{shard_id:05}-of-{OFFICIAL_SHARD_COUNT:05}.safetensors");
        if !shards.contains(&expected) {
            return Err(format!("deepseek_v4_official_shard_missing:{expected}"));
        }
    }
    Ok(())
}

fn load_shard_header(
    root: &Path,
    shard_name: &str,
) -> Result<
    (
        DeepseekV4ShardMetadata,
        BTreeMap<String, DeepseekV4TensorMetadata>,
    ),
    String,
> {
    if Path::new(shard_name).components().count() != 1 {
        return Err(format!("deepseek_v4_shard_name_unsafe:{shard_name}"));
    }
    let path = root.join(shard_name);
    let mut file = File::open(&path)
        .map_err(|err| format!("deepseek_v4_shard_open_failed:{}:{err}", path.display()))?;
    let file_len = file
        .metadata()
        .map_err(|err| format!("deepseek_v4_shard_metadata_failed:{}:{err}", path.display()))?
        .len();
    let mut header_len_bytes = [0u8; 8];
    file.read_exact(&mut header_len_bytes).map_err(|err| {
        format!(
            "deepseek_v4_shard_header_len_read_failed:{}:{err}",
            path.display()
        )
    })?;
    let header_len = u64::from_le_bytes(header_len_bytes);
    if header_len == 0 || header_len > MAX_SAFETENSORS_HEADER_BYTES {
        return Err(format!(
            "deepseek_v4_shard_header_len_invalid:{}:{header_len}",
            path.display()
        ));
    }
    let data_base_offset = 8u64
        .checked_add(header_len)
        .ok_or_else(|| format!("deepseek_v4_shard_data_base_overflow:{}", path.display()))?;
    if data_base_offset > file_len {
        return Err(format!(
            "deepseek_v4_shard_header_oob:{}:header_len={header_len}:file_len={file_len}",
            path.display()
        ));
    }
    let header_len_usize = usize::try_from(header_len).map_err(|_| {
        format!(
            "deepseek_v4_shard_header_len_platform_overflow:{}:{header_len}",
            path.display()
        )
    })?;
    let mut header = vec![0u8; header_len_usize];
    file.read_exact(&mut header).map_err(|err| {
        format!(
            "deepseek_v4_shard_header_read_failed:{}:{err}",
            path.display()
        )
    })?;
    let raw: BTreeMap<String, RawTensorMetadata> =
        serde_json::from_slice(&header).map_err(|err| {
            format!(
                "deepseek_v4_shard_header_parse_failed:{}:{err}",
                path.display()
            )
        })?;
    if raw.is_empty() {
        return Err(format!("deepseek_v4_shard_header_empty:{}", path.display()));
    }

    let mut ranges = Vec::with_capacity(raw.len());
    let mut tensors = BTreeMap::new();
    for (name, raw_tensor) in raw {
        if name == "__metadata__" {
            return Err(format!(
                "deepseek_v4_shard_metadata_object_unsupported:{}",
                path.display()
            ));
        }
        let dtype = DeepseekV4TensorDType::try_from(raw_tensor.dtype.as_str())?;
        validate_tensor_layout(
            &name,
            dtype,
            &raw_tensor.shape,
            raw_tensor.data_offsets,
            file_len,
            data_base_offset,
        )?;
        ranges.push((
            raw_tensor.data_offsets[0],
            raw_tensor.data_offsets[1],
            name.clone(),
        ));
        tensors.insert(
            name.clone(),
            DeepseekV4TensorMetadata {
                name,
                shard: shard_name.to_string(),
                dtype,
                shape: raw_tensor.shape,
                data_offsets: raw_tensor.data_offsets,
                data_base_offset,
                file_len,
                scale_tensor: None,
            },
        );
    }
    ranges.sort_by_key(|range| range.0);
    let mut expected_start = 0u64;
    for (start, end, name) in &ranges {
        if *start != expected_start {
            return Err(format!(
                "deepseek_v4_shard_payload_not_contiguous:{}:{name}:start={start}:expected={expected_start}",
                path.display()
            ));
        }
        expected_start = *end;
    }
    let payload_bytes = file_len - data_base_offset;
    if expected_start != payload_bytes {
        return Err(format!(
            "deepseek_v4_shard_payload_size_mismatch:{}:tensor_end={expected_start}:payload_bytes={payload_bytes}",
            path.display()
        ));
    }
    Ok((
        DeepseekV4ShardMetadata {
            file_name: shard_name.to_string(),
            file_len,
            header_len,
            tensor_count: tensors.len(),
        },
        tensors,
    ))
}

fn validate_tensor_layout(
    name: &str,
    dtype: DeepseekV4TensorDType,
    shape: &[u64],
    offsets: [u64; 2],
    file_len: u64,
    data_base_offset: u64,
) -> Result<(), String> {
    if shape.is_empty() || shape.contains(&0) {
        return Err(format!("deepseek_v4_tensor_shape_invalid:{name}:{shape:?}"));
    }
    if offsets[0] >= offsets[1] {
        return Err(format!(
            "deepseek_v4_tensor_offsets_invalid:{name}:{}:{}",
            offsets[0], offsets[1]
        ));
    }
    let elements = shape.iter().try_fold(1u64, |total, dimension| {
        total
            .checked_mul(*dimension)
            .ok_or_else(|| format!("deepseek_v4_tensor_element_count_overflow:{name}"))
    })?;
    let expected_bytes = elements
        .checked_mul(dtype.storage_bytes())
        .ok_or_else(|| format!("deepseek_v4_tensor_payload_bytes_overflow:{name}"))?;
    let actual_bytes = offsets[1] - offsets[0];
    if actual_bytes != expected_bytes {
        return Err(format!(
            "deepseek_v4_tensor_payload_size_mismatch:{name}:dtype={}:shape={shape:?}:actual={actual_bytes}:expected={expected_bytes}",
            dtype.safetensors_name()
        ));
    }
    let absolute_end = data_base_offset
        .checked_add(offsets[1])
        .ok_or_else(|| format!("deepseek_v4_tensor_absolute_end_overflow:{name}"))?;
    if absolute_end > file_len {
        return Err(format!(
            "deepseek_v4_tensor_payload_oob:{name}:end={absolute_end}:file_len={file_len}"
        ));
    }
    Ok(())
}

fn validate_quantized_scale_associations(
    tensors: &mut BTreeMap<String, DeepseekV4TensorMetadata>,
) -> Result<(), String> {
    let mut associations = Vec::new();
    let mut associated_scales = BTreeSet::new();
    for (name, tensor) in tensors.iter() {
        let expected_scale_shape = match tensor.dtype {
            DeepseekV4TensorDType::F8E4M3 => {
                if !name.ends_with(".weight") || tensor.shape.len() != 2 {
                    return Err(format!("deepseek_v4_fp8_weight_schema_invalid:{name}"));
                }
                vec![
                    div_ceil(tensor.shape[0], 128),
                    div_ceil(tensor.shape[1], 128),
                ]
            }
            DeepseekV4TensorDType::I8 => {
                if !is_routed_expert_name(name)
                    || !name.ends_with(".weight")
                    || tensor.shape.len() != 2
                {
                    return Err(format!("deepseek_v4_fp4_weight_schema_invalid:{name}"));
                }
                let logical_k = tensor.shape[1]
                    .checked_mul(2)
                    .ok_or_else(|| format!("deepseek_v4_fp4_logical_k_overflow:{name}"))?;
                if logical_k % 32 != 0 {
                    return Err(format!(
                        "deepseek_v4_fp4_logical_k_not_block_aligned:{name}:{logical_k}"
                    ));
                }
                vec![tensor.shape[0], logical_k / 32]
            }
            _ => continue,
        };
        let scale_name = name
            .strip_suffix(".weight")
            .map(|prefix| format!("{prefix}.scale"))
            .ok_or_else(|| format!("deepseek_v4_quantized_weight_name_invalid:{name}"))?;
        let scale = tensors.get(&scale_name).ok_or_else(|| {
            format!("deepseek_v4_quantized_weight_scale_missing:{name}:{scale_name}")
        })?;
        if scale.dtype != DeepseekV4TensorDType::F8E8M0 {
            return Err(format!(
                "deepseek_v4_quantized_weight_scale_dtype_mismatch:{name}:{scale_name}:actual={}",
                scale.dtype.safetensors_name()
            ));
        }
        if scale.shape != expected_scale_shape {
            return Err(format!(
                "deepseek_v4_quantized_weight_scale_shape_mismatch:{name}:{scale_name}:actual={:?}:expected={expected_scale_shape:?}",
                scale.shape
            ));
        }
        associated_scales.insert(scale_name.clone());
        associations.push((name.clone(), scale_name));
    }
    for (name, tensor) in tensors.iter() {
        if tensor.dtype == DeepseekV4TensorDType::F8E8M0 && !associated_scales.contains(name) {
            return Err(format!("deepseek_v4_orphan_scale_tensor:{name}"));
        }
    }
    for (weight_name, scale_name) in associations {
        tensors
            .get_mut(&weight_name)
            .expect("weight collected from the same map")
            .scale_tensor = Some(scale_name);
    }
    Ok(())
}

fn div_ceil(value: u64, divisor: u64) -> u64 {
    value / divisor + u64::from(value % divisor != 0)
}

fn validate_model_tensor_schema(
    config: &DeepseekV4Config,
    tensors: &BTreeMap<String, DeepseekV4TensorMetadata>,
) -> Result<(), String> {
    require_tensor(
        tensors,
        "embed.weight",
        DeepseekV4TensorDType::Bf16,
        &[config.vocab_size, config.hidden_size],
    )?;
    require_tensor(
        tensors,
        "head.weight",
        DeepseekV4TensorDType::Bf16,
        &[config.vocab_size, config.hidden_size],
    )?;
    require_tensor(
        tensors,
        "norm.weight",
        DeepseekV4TensorDType::Bf16,
        &[config.hidden_size],
    )?;

    let mut seen_layers = BTreeSet::new();
    for name in tensors.keys() {
        if name.starts_with("layers.") {
            let layer = parse_layer_id(name)
                .ok_or_else(|| format!("deepseek_v4_layer_tensor_name_invalid:{name}"))?;
            if layer >= config.num_hidden_layers {
                return Err(format!(
                    "deepseek_v4_layer_tensor_out_of_range:{name}:layers={}",
                    config.num_hidden_layers
                ));
            }
            seen_layers.insert(layer);
        }
    }
    let expected_layers: BTreeSet<u64> = (0..config.num_hidden_layers).collect();
    if seen_layers != expected_layers {
        return Err(format!(
            "deepseek_v4_layer_coverage_mismatch:actual={seen_layers:?}:expected={expected_layers:?}"
        ));
    }

    for layer in 0..config.num_hidden_layers {
        let prefix = format!("layers.{layer}");
        validate_block_schema(config, tensors, &prefix, layer < config.num_hash_layers)?;
    }
    for mtp in 0..config.num_nextn_predict_layers {
        let prefix = format!("mtp.{mtp}");
        validate_block_schema(config, tensors, &prefix, false)?;
        for name in ["e_proj.weight", "h_proj.weight"] {
            require_quantized_weight(tensors, &format!("{prefix}.{name}"))?;
        }
        for name in ["enorm.weight", "hnorm.weight", "norm.weight"] {
            require_tensor(
                tensors,
                &format!("{prefix}.{name}"),
                DeepseekV4TensorDType::Bf16,
                &[config.hidden_size],
            )?;
        }
    }
    Ok(())
}

fn validate_block_schema(
    config: &DeepseekV4Config,
    tensors: &BTreeMap<String, DeepseekV4TensorMetadata>,
    prefix: &str,
    hash_routed: bool,
) -> Result<(), String> {
    for name in ["attn_norm.weight", "ffn_norm.weight"] {
        require_tensor(
            tensors,
            &format!("{prefix}.{name}"),
            DeepseekV4TensorDType::Bf16,
            &[config.hidden_size],
        )?;
    }
    for name in [
        "hc_attn_base",
        "hc_attn_fn",
        "hc_attn_scale",
        "hc_ffn_base",
        "hc_ffn_fn",
        "hc_ffn_scale",
    ] {
        require_dtype(
            tensors,
            &format!("{prefix}.{name}"),
            DeepseekV4TensorDType::F32,
        )?;
    }
    for name in [
        "attn.wq_a.weight",
        "attn.wq_b.weight",
        "attn.wkv.weight",
        "attn.wo_a.weight",
        "attn.wo_b.weight",
    ] {
        require_quantized_weight(tensors, &format!("{prefix}.{name}"))?;
    }
    require_tensor(
        tensors,
        &format!("{prefix}.attn.attn_sink"),
        DeepseekV4TensorDType::F32,
        &[config.num_attention_heads],
    )?;
    require_tensor(
        tensors,
        &format!("{prefix}.attn.q_norm.weight"),
        DeepseekV4TensorDType::Bf16,
        &[config.q_lora_rank],
    )?;
    require_tensor(
        tensors,
        &format!("{prefix}.attn.kv_norm.weight"),
        DeepseekV4TensorDType::Bf16,
        &[config.head_dim],
    )?;

    if hash_routed {
        require_tensor(
            tensors,
            &format!("{prefix}.ffn.gate.tid2eid"),
            DeepseekV4TensorDType::I64,
            &[config.vocab_size, config.num_experts_per_tok],
        )?;
    } else {
        require_tensor(
            tensors,
            &format!("{prefix}.ffn.gate.weight"),
            DeepseekV4TensorDType::Bf16,
            &[config.n_routed_experts, config.hidden_size],
        )?;
        require_tensor(
            tensors,
            &format!("{prefix}.ffn.gate.bias"),
            DeepseekV4TensorDType::F32,
            &[config.n_routed_experts],
        )?;
    }

    for projection in ["w1", "w2", "w3"] {
        require_quantized_weight(
            tensors,
            &format!("{prefix}.ffn.shared_experts.{projection}.weight"),
        )?;
    }
    for expert in 0..config.n_routed_experts {
        for projection in ["w1", "w2", "w3"] {
            let name = format!("{prefix}.ffn.experts.{expert}.{projection}.weight");
            let tensor = require_quantized_weight(tensors, &name)?;
            if tensor.dtype != DeepseekV4TensorDType::I8 {
                return Err(format!(
                    "deepseek_v4_routed_expert_not_fp4:{name}:dtype={}",
                    tensor.dtype.safetensors_name()
                ));
            }
        }
    }
    Ok(())
}

fn require_quantized_weight<'a>(
    tensors: &'a BTreeMap<String, DeepseekV4TensorMetadata>,
    name: &str,
) -> Result<&'a DeepseekV4TensorMetadata, String> {
    let tensor = tensors
        .get(name)
        .ok_or_else(|| format!("deepseek_v4_required_tensor_missing:{name}"))?;
    if tensor.scale_tensor.is_none() {
        return Err(format!("deepseek_v4_required_weight_scale_missing:{name}"));
    }
    Ok(tensor)
}

fn require_dtype<'a>(
    tensors: &'a BTreeMap<String, DeepseekV4TensorMetadata>,
    name: &str,
    dtype: DeepseekV4TensorDType,
) -> Result<&'a DeepseekV4TensorMetadata, String> {
    let tensor = tensors
        .get(name)
        .ok_or_else(|| format!("deepseek_v4_required_tensor_missing:{name}"))?;
    if tensor.dtype != dtype {
        return Err(format!(
            "deepseek_v4_required_tensor_dtype_mismatch:{name}:actual={}:expected={}",
            tensor.dtype.safetensors_name(),
            dtype.safetensors_name()
        ));
    }
    Ok(tensor)
}

fn require_tensor<'a>(
    tensors: &'a BTreeMap<String, DeepseekV4TensorMetadata>,
    name: &str,
    dtype: DeepseekV4TensorDType,
    shape: &[u64],
) -> Result<&'a DeepseekV4TensorMetadata, String> {
    let tensor = require_dtype(tensors, name, dtype)?;
    if tensor.shape != shape {
        return Err(format!(
            "deepseek_v4_required_tensor_shape_mismatch:{name}:actual={:?}:expected={shape:?}",
            tensor.shape
        ));
    }
    Ok(tensor)
}

fn parse_layer_id(name: &str) -> Option<u64> {
    let suffix = name.strip_prefix("layers.")?;
    suffix.split('.').next()?.parse().ok()
}

fn tensor_names_for_layer_range<'a>(
    tensors: &'a BTreeMap<String, DeepseekV4TensorMetadata>,
    layer_start: u64,
    layer_end: u64,
    total_layers: u64,
) -> Result<Vec<&'a str>, String> {
    if layer_start >= layer_end || layer_end > total_layers {
        return Err(format!(
            "deepseek_v4_layer_range_invalid:{layer_start}:{layer_end}:total={total_layers}"
        ));
    }
    let names = tensors
        .keys()
        .filter(|name| {
            parse_layer_id(name)
                .map(|layer| layer >= layer_start && layer < layer_end)
                .unwrap_or(false)
        })
        .map(String::as_str)
        .collect::<Vec<_>>();
    if names.is_empty() {
        return Err(format!(
            "deepseek_v4_layer_range_has_no_tensors:{layer_start}:{layer_end}"
        ));
    }
    Ok(names)
}

fn cache_get(cache: &mut BoundedCache, key: &CacheKey) -> Option<Arc<[u8]>> {
    match cache.entries.get(key).cloned() {
        Some(payload) => {
            cache.stats.hits = cache.stats.hits.saturating_add(1);
            if let Some(position) = cache.lru.iter().position(|candidate| candidate == key) {
                cache.lru.remove(position);
            }
            cache.lru.push_back(key.clone());
            Some(payload)
        }
        None => {
            cache.stats.misses = cache.stats.misses.saturating_add(1);
            None
        }
    }
}

fn cache_insert(cache: &mut BoundedCache, key: CacheKey, payload: Arc<[u8]>) -> Result<(), String> {
    let payload_bytes = u64::try_from(payload.len())
        .map_err(|_| "deepseek_v4_cache_payload_len_overflow".to_string())?;
    if payload_bytes > cache.stats.capacity_bytes || cache.stats.capacity_bytes == 0 {
        return Ok(());
    }
    if cache.entries.contains_key(&key) {
        return Ok(());
    }
    while cache
        .stats
        .resident_bytes
        .checked_add(payload_bytes)
        .ok_or_else(|| "deepseek_v4_cache_resident_bytes_overflow".to_string())?
        > cache.stats.capacity_bytes
    {
        let evicted_key = cache
            .lru
            .pop_front()
            .ok_or_else(|| "deepseek_v4_cache_lru_empty_during_eviction".to_string())?;
        let evicted = cache
            .entries
            .remove(&evicted_key)
            .ok_or_else(|| "deepseek_v4_cache_lru_entry_missing".to_string())?;
        cache.stats.resident_bytes = cache
            .stats
            .resident_bytes
            .checked_sub(evicted.len() as u64)
            .ok_or_else(|| "deepseek_v4_cache_resident_bytes_underflow".to_string())?;
        cache.stats.evictions = cache.stats.evictions.saturating_add(1);
    }
    cache.stats.resident_bytes = cache
        .stats
        .resident_bytes
        .checked_add(payload_bytes)
        .ok_or_else(|| "deepseek_v4_cache_resident_bytes_overflow".to_string())?;
    cache.stats.peak_resident_bytes = cache
        .stats
        .peak_resident_bytes
        .max(cache.stats.resident_bytes);
    cache.lru.push_back(key.clone());
    cache.entries.insert(key, payload);
    Ok(())
}

fn read_exact_at(path: &Path, offset: u64, payload: &mut [u8]) -> Result<(), String> {
    let file = File::open(path).map_err(|err| {
        format!(
            "deepseek_v4_tensor_payload_open_failed:{}:{err}",
            path.display()
        )
    })?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::FileExt;
        let mut completed = 0usize;
        while completed < payload.len() {
            let current_offset = offset.checked_add(completed as u64).ok_or_else(|| {
                format!(
                    "deepseek_v4_tensor_payload_read_offset_overflow:{}",
                    path.display()
                )
            })?;
            let bytes = file
                .read_at(&mut payload[completed..], current_offset)
                .map_err(|err| {
                    format!(
                        "deepseek_v4_tensor_payload_read_failed:{}:{current_offset}:{err}",
                        path.display()
                    )
                })?;
            if bytes == 0 {
                return Err(format!(
                    "deepseek_v4_tensor_payload_unexpected_eof:{}:{current_offset}",
                    path.display()
                ));
            }
            completed += bytes;
        }
    }
    #[cfg(not(unix))]
    {
        use std::io::{Seek, SeekFrom};
        let mut file = file;
        file.seek(SeekFrom::Start(offset)).map_err(|err| {
            format!(
                "deepseek_v4_tensor_payload_seek_failed:{}:{offset}:{err}",
                path.display()
            )
        })?;
        file.read_exact(payload).map_err(|err| {
            format!(
                "deepseek_v4_tensor_payload_read_failed:{}:{offset}:{err}",
                path.display()
            )
        })?;
    }
    Ok(())
}

pub fn process_peak_resident_bytes() -> Option<u64> {
    let mut usage = std::mem::MaybeUninit::<libc::rusage>::zeroed();
    let result = unsafe { libc::getrusage(libc::RUSAGE_SELF, usage.as_mut_ptr()) };
    if result != 0 {
        return None;
    }
    let usage = unsafe { usage.assume_init() };
    let raw = u64::try_from(usage.ru_maxrss).ok()?;
    #[cfg(target_os = "macos")]
    {
        Some(raw)
    }
    #[cfg(not(target_os = "macos"))]
    {
        raw.checked_mul(1024)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tensor(
        name: &str,
        dtype: DeepseekV4TensorDType,
        shape: Vec<u64>,
    ) -> DeepseekV4TensorMetadata {
        let bytes = shape.iter().product::<u64>() * dtype.storage_bytes();
        DeepseekV4TensorMetadata {
            name: name.to_string(),
            shard: "model-00001-of-00046.safetensors".to_string(),
            dtype,
            shape,
            data_offsets: [0, bytes],
            data_base_offset: 128,
            file_len: 128 + bytes,
            scale_tensor: None,
        }
    }

    #[test]
    fn official_dtypes_are_explicit_and_unknown_dtype_fails_closed() {
        for name in ["BF16", "F32", "F8_E4M3", "F8_E8M0", "I8", "I64"] {
            let dtype = DeepseekV4TensorDType::try_from(name).expect("official dtype");
            assert_eq!(dtype.safetensors_name(), name);
        }
        let error =
            DeepseekV4TensorDType::try_from("F8_E5M2").expect_err("unsupported dtype must fail");
        assert!(error.contains("dtype_unsupported"));
    }

    #[test]
    fn tensor_layout_rejects_bad_offset_shape_and_payload_size() {
        let bad_offset = validate_tensor_layout(
            "bad.offset",
            DeepseekV4TensorDType::F32,
            &[1],
            [4, 4],
            16,
            8,
        )
        .expect_err("empty range must fail");
        assert!(bad_offset.contains("offsets_invalid"));

        let bad_shape = validate_tensor_layout(
            "bad.shape",
            DeepseekV4TensorDType::F32,
            &[0, 1],
            [0, 4],
            12,
            8,
        )
        .expect_err("zero dimension must fail");
        assert!(bad_shape.contains("shape_invalid"));

        let bad_size =
            validate_tensor_layout("bad.size", DeepseekV4TensorDType::Bf16, &[2], [0, 2], 10, 8)
                .expect_err("payload size mismatch must fail");
        assert!(bad_size.contains("payload_size_mismatch"));
    }

    #[test]
    fn quantized_weights_bind_fp8_and_fp4_scales() {
        let fp8 = "layers.0.attn.wkv.weight";
        let fp8_scale = "layers.0.attn.wkv.scale";
        let fp4 = "layers.0.ffn.experts.7.w1.weight";
        let fp4_scale = "layers.0.ffn.experts.7.w1.scale";
        let mut tensors = BTreeMap::from([
            (
                fp8.to_string(),
                tensor(fp8, DeepseekV4TensorDType::F8E4M3, vec![129, 257]),
            ),
            (
                fp8_scale.to_string(),
                tensor(fp8_scale, DeepseekV4TensorDType::F8E8M0, vec![2, 3]),
            ),
            (
                fp4.to_string(),
                tensor(fp4, DeepseekV4TensorDType::I8, vec![2, 16]),
            ),
            (
                fp4_scale.to_string(),
                tensor(fp4_scale, DeepseekV4TensorDType::F8E8M0, vec![2, 1]),
            ),
        ]);
        validate_quantized_scale_associations(&mut tensors).expect("valid scale association");
        assert_eq!(tensors[fp8].scale_tensor.as_deref(), Some(fp8_scale));
        assert_eq!(tensors[fp4].scale_tensor.as_deref(), Some(fp4_scale));
    }

    #[test]
    fn quantized_weight_missing_or_bad_scale_fails_closed() {
        let weight_name = "layers.0.attn.wkv.weight";
        let scale_name = "layers.0.attn.wkv.scale";
        let weight = tensor(weight_name, DeepseekV4TensorDType::F8E4M3, vec![128, 128]);
        let missing = validate_quantized_scale_associations(&mut BTreeMap::from([(
            weight_name.to_string(),
            weight.clone(),
        )]))
        .expect_err("missing scale must fail");
        assert!(missing.contains("scale_missing"));

        let mut bad_shape = BTreeMap::from([
            (weight_name.to_string(), weight),
            (
                scale_name.to_string(),
                tensor(scale_name, DeepseekV4TensorDType::F8E8M0, vec![1, 2]),
            ),
        ]);
        let error = validate_quantized_scale_associations(&mut bad_shape)
            .expect_err("bad scale shape must fail");
        assert!(error.contains("scale_shape_mismatch"));
    }

    #[test]
    fn official_shard_set_rejects_missing_shard() {
        let mut shards = (1..=OFFICIAL_SHARD_COUNT)
            .map(|id| format!("model-{id:05}-of-{OFFICIAL_SHARD_COUNT:05}.safetensors"))
            .collect::<BTreeSet<_>>();
        validate_shard_names(&shards).expect("complete shard set");
        shards.remove("model-00017-of-00046.safetensors");
        let error = validate_shard_names(&shards).expect_err("missing shard must fail");
        assert!(error.contains("shard_count_mismatch") || error.contains("shard_missing"));
    }

    #[test]
    fn cache_enforces_capacity_and_evicts_lru_entry() {
        let mut cache = BoundedCache::new(6);
        let first = CacheKey {
            tensor: "first".to_string(),
            offset: 0,
            bytes: 4,
        };
        let second = CacheKey {
            tensor: "second".to_string(),
            offset: 0,
            bytes: 4,
        };
        cache_insert(&mut cache, first.clone(), Arc::from([1u8, 2, 3, 4])).expect("insert first");
        cache_insert(&mut cache, second.clone(), Arc::from([5u8, 6, 7, 8])).expect("insert second");
        assert!(!cache.entries.contains_key(&first));
        assert!(cache.entries.contains_key(&second));
        assert_eq!(cache.stats.resident_bytes, 4);
        assert_eq!(cache.stats.peak_resident_bytes, 4);
        assert_eq!(cache.stats.evictions, 1);
        assert!(cache_get(&mut cache, &second).is_some());
        assert_eq!(cache.stats.hits, 1);
    }

    #[test]
    fn positioned_read_returns_only_requested_bytes() {
        let path = std::env::temp_dir().join(format!(
            "deepseek_v4_positioned_read_{}.bin",
            std::process::id()
        ));
        let _ = fs::remove_file(&path);
        fs::write(&path, (0u8..32).collect::<Vec<_>>()).expect("write payload fixture");
        let mut payload = [0u8; 5];
        read_exact_at(&path, 11, &mut payload).expect("positioned read");
        assert_eq!(payload, [11, 12, 13, 14, 15]);
        fs::remove_file(path).expect("remove payload fixture");
    }

    #[test]
    fn layer_name_parser_is_strict() {
        assert_eq!(parse_layer_id("layers.42.attn.wkv.weight"), Some(42));
        assert_eq!(parse_layer_id("mtp.0.attn.wkv.weight"), None);
        assert_eq!(parse_layer_id("layers.bad.attn.wkv.weight"), None);
    }

    #[test]
    fn layer_range_selects_only_owned_layers() {
        let tensors = BTreeMap::from([
            (
                "embed.weight".to_string(),
                tensor("embed.weight", DeepseekV4TensorDType::Bf16, vec![1]),
            ),
            (
                "layers.0.attn.wkv.weight".to_string(),
                tensor(
                    "layers.0.attn.wkv.weight",
                    DeepseekV4TensorDType::F8E4M3,
                    vec![1, 1],
                ),
            ),
            (
                "layers.1.attn.wkv.weight".to_string(),
                tensor(
                    "layers.1.attn.wkv.weight",
                    DeepseekV4TensorDType::F8E4M3,
                    vec![1, 1],
                ),
            ),
            (
                "layers.2.attn.wkv.weight".to_string(),
                tensor(
                    "layers.2.attn.wkv.weight",
                    DeepseekV4TensorDType::F8E4M3,
                    vec![1, 1],
                ),
            ),
        ]);
        assert_eq!(
            tensor_names_for_layer_range(&tensors, 1, 3, 3).expect("valid layer range"),
            ["layers.1.attn.wkv.weight", "layers.2.attn.wkv.weight"]
        );
        let error = tensor_names_for_layer_range(&tensors, 2, 4, 3)
            .expect_err("range beyond configured layers must fail");
        assert!(error.contains("layer_range_invalid"));
    }
}
