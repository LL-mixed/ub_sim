use crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_PROFILE;
use serde::Serialize;
use std::collections::BTreeMap;
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

const GGUF_MAGIC: u32 = 0x4655_4747;
const GGUF_VERSION: u32 = 3;
const MAX_CATALOG_ENTRIES: u64 = 1_000_000;
const MAX_STRING_BYTES: u64 = 16 * 1024 * 1024;
const MAX_DIMS: u32 = 8;

const VALUE_UINT8: u32 = 0;
const VALUE_INT8: u32 = 1;
const VALUE_UINT16: u32 = 2;
const VALUE_INT16: u32 = 3;
const VALUE_UINT32: u32 = 4;
const VALUE_INT32: u32 = 5;
const VALUE_FLOAT32: u32 = 6;
const VALUE_BOOL: u32 = 7;
const VALUE_STRING: u32 = 8;
const VALUE_ARRAY: u32 = 9;
const VALUE_UINT64: u32 = 10;
const VALUE_INT64: u32 = 11;
const VALUE_FLOAT64: u32 = 12;

#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(untagged)]
pub enum GgufScalarValue {
    Unsigned(u64),
    Signed(i64),
    Float(f64),
    Bool(bool),
    String(String),
}

impl GgufScalarValue {
    fn as_u64(&self) -> Option<u64> {
        match self {
            Self::Unsigned(value) => Some(*value),
            Self::Signed(value) => u64::try_from(*value).ok(),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub struct GgufTensorType {
    pub code: u32,
    pub name: &'static str,
    pub block_elements: u64,
    pub block_bytes: u64,
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct GgufTensorInfo {
    pub name: String,
    pub dimensions: Vec<u64>,
    pub tensor_type: GgufTensorType,
    pub elements: u64,
    pub relative_offset: u64,
    pub absolute_offset: u64,
    pub bytes: u64,
}

#[derive(Clone, Debug, Serialize)]
pub struct GgufCatalog {
    pub path: PathBuf,
    pub version: u32,
    pub file_bytes: u64,
    pub alignment: u64,
    pub tensor_data_offset: u64,
    pub metadata_count: u64,
    pub metadata: BTreeMap<String, GgufScalarValue>,
    pub tensors: BTreeMap<String, GgufTensorInfo>,
}

impl GgufCatalog {
    pub fn open(path: &Path) -> Result<Self, String> {
        let mut reader = File::open(path)
            .map_err(|err| format!("deepseek_gguf_open_failed:{}:{err}", path.display()))?;
        let file_bytes = reader
            .metadata()
            .map_err(|err| format!("deepseek_gguf_stat_failed:{}:{err}", path.display()))?
            .len();
        if file_bytes < 32 {
            return Err(format!(
                "deepseek_gguf_file_too_small:{}:{file_bytes}",
                path.display()
            ));
        }

        let magic = read_u32(&mut reader)?;
        if magic != GGUF_MAGIC {
            return Err(format!("deepseek_gguf_bad_magic:{magic:#x}"));
        }
        let version = read_u32(&mut reader)?;
        if version != GGUF_VERSION {
            return Err(format!("deepseek_gguf_unsupported_version:{version}"));
        }
        let tensor_count = read_u64(&mut reader)?;
        let metadata_count = read_u64(&mut reader)?;
        validate_entry_count("tensor", tensor_count)?;
        validate_entry_count("metadata", metadata_count)?;

        let mut alignment = 32u64;
        let mut metadata = BTreeMap::new();
        for _ in 0..metadata_count {
            let key = read_string(&mut reader)?;
            let value_type = read_u32(&mut reader)?;
            if let Some(value) = read_metadata_value(&mut reader, value_type, &key)? {
                if key == "general.alignment" {
                    alignment = value
                        .as_u64()
                        .filter(|value| *value > 0)
                        .ok_or_else(|| "deepseek_gguf_invalid_alignment".to_string())?;
                }
                metadata.insert(key, value);
            }
        }

        let mut tensor_entries = Vec::with_capacity(
            usize::try_from(tensor_count)
                .map_err(|_| "deepseek_gguf_tensor_count_too_large".to_string())?,
        );
        for _ in 0..tensor_count {
            let name = read_string(&mut reader)?;
            let dimensions_count = read_u32(&mut reader)?;
            if dimensions_count == 0 || dimensions_count > MAX_DIMS {
                return Err(format!(
                    "deepseek_gguf_invalid_tensor_rank:{name}:{dimensions_count}"
                ));
            }
            let mut elements = 1u64;
            let mut dimensions = Vec::with_capacity(dimensions_count as usize);
            for _ in 0..dimensions_count {
                let dimension = read_u64(&mut reader)?;
                elements = elements
                    .checked_mul(dimension)
                    .ok_or_else(|| format!("deepseek_gguf_tensor_elements_overflow:{name}"))?;
                dimensions.push(dimension);
            }
            let tensor_type_code = read_u32(&mut reader)?;
            let tensor_type = gguf_tensor_type(tensor_type_code).ok_or_else(|| {
                format!("deepseek_gguf_unsupported_tensor_type:{name}:{tensor_type_code}")
            })?;
            let relative_offset = read_u64(&mut reader)?;
            let bytes = tensor_bytes(tensor_type, elements)?;
            tensor_entries.push((
                name,
                dimensions,
                tensor_type,
                elements,
                relative_offset,
                bytes,
            ));
        }

        let directory_end = reader
            .stream_position()
            .map_err(|err| format!("deepseek_gguf_position_failed:{err}"))?;
        let tensor_data_offset = align_up(directory_end, alignment)?;
        let mut tensors = BTreeMap::new();
        for (name, dimensions, tensor_type, elements, relative_offset, bytes) in tensor_entries {
            let absolute_offset = tensor_data_offset
                .checked_add(relative_offset)
                .ok_or_else(|| format!("deepseek_gguf_tensor_offset_overflow:{name}"))?;
            let end = absolute_offset
                .checked_add(bytes)
                .ok_or_else(|| format!("deepseek_gguf_tensor_end_overflow:{name}"))?;
            if end > file_bytes {
                return Err(format!(
                    "deepseek_gguf_tensor_out_of_file:{name}:end={end}:file={file_bytes}"
                ));
            }
            let info = GgufTensorInfo {
                name: name.clone(),
                dimensions,
                tensor_type,
                elements,
                relative_offset,
                absolute_offset,
                bytes,
            };
            if tensors.insert(name.clone(), info).is_some() {
                return Err(format!("deepseek_gguf_duplicate_tensor:{name}"));
            }
        }

        Ok(Self {
            path: path.to_path_buf(),
            version,
            file_bytes,
            alignment,
            tensor_data_offset,
            metadata_count,
            metadata,
            tensors,
        })
    }

    pub fn tensor(&self, name: &str) -> Result<&GgufTensorInfo, String> {
        self.tensors
            .get(name)
            .ok_or_else(|| format!("deepseek_gguf_tensor_missing:{name}"))
    }

    pub fn metadata_u64(&self, key: &str) -> Result<u64, String> {
        self.metadata
            .get(key)
            .and_then(GgufScalarValue::as_u64)
            .ok_or_else(|| format!("deepseek_gguf_unsigned_metadata_missing:{key}"))
    }

    pub fn read_tensor(&self, name: &str) -> Result<Vec<u8>, String> {
        let tensor = self.tensor(name)?;
        let bytes = usize::try_from(tensor.bytes)
            .map_err(|_| format!("deepseek_gguf_tensor_too_large_to_read:{name}"))?;
        let mut reader = File::open(&self.path).map_err(|err| {
            format!(
                "deepseek_gguf_tensor_open_failed:{}:{err}",
                self.path.display()
            )
        })?;
        reader
            .seek(SeekFrom::Start(tensor.absolute_offset))
            .map_err(|err| format!("deepseek_gguf_tensor_seek_failed:{name}:{err}"))?;
        let mut payload = vec![0u8; bytes];
        reader
            .read_exact(&mut payload)
            .map_err(|err| format!("deepseek_gguf_tensor_read_failed:{name}:{err}"))?;
        Ok(payload)
    }

    pub fn validate_deepseek_v4_flash(&self) -> Result<(), String> {
        let profile = DEEPSEEK_V4_FLASH_PROFILE;
        let expected = [
            ("deepseek4.block_count", profile.num_hidden_layers),
            ("deepseek4.embedding_length", profile.hidden_size),
            ("deepseek4.vocab_size", profile.vocab_size),
            (
                "deepseek4.attention.head_count",
                profile.num_attention_heads,
            ),
            (
                "deepseek4.attention.head_count_kv",
                profile.num_key_value_heads,
            ),
            ("deepseek4.attention.key_length", profile.head_dim),
            ("deepseek4.rope.dimension_count", profile.qk_rope_head_dim),
            ("deepseek4.attention.q_lora_rank", profile.q_lora_rank),
            ("deepseek4.attention.output_lora_rank", profile.o_lora_rank),
            (
                "deepseek4.attention.output_group_count",
                profile.output_groups,
            ),
            ("deepseek4.expert_count", profile.num_experts),
            ("deepseek4.expert_used_count", profile.num_experts_used),
            ("deepseek4.expert_shared_count", profile.num_experts_shared),
            (
                "deepseek4.expert_feed_forward_length",
                profile.moe_intermediate_size,
            ),
            ("deepseek4.hash_layer_count", profile.num_hash_layers),
            ("deepseek4.attention.sliding_window", profile.sliding_window),
            (
                "deepseek4.attention.indexer.head_count",
                profile.indexer_heads,
            ),
            (
                "deepseek4.attention.indexer.key_length",
                profile.indexer_head_dim,
            ),
            ("deepseek4.attention.indexer.top_k", profile.indexer_top_k),
            ("deepseek4.hyper_connection.count", profile.hc_mult),
            (
                "deepseek4.hyper_connection.sinkhorn_iterations",
                profile.hc_sinkhorn_iters,
            ),
        ];
        for (key, expected_value) in expected {
            let actual = self.metadata_u64(key)?;
            if actual != expected_value {
                return Err(format!(
                    "deepseek_gguf_flash_geometry_mismatch:{key}:actual={actual}:expected={expected_value}"
                ));
            }
        }
        for name in [
            "blk.0.attn_q_a.weight",
            "blk.0.attn_q_b.weight",
            "blk.0.attn_kv.weight",
        ] {
            self.tensor(name)?;
        }
        Ok(())
    }
}

fn validate_entry_count(kind: &str, count: u64) -> Result<(), String> {
    if count > MAX_CATALOG_ENTRIES {
        return Err(format!("deepseek_gguf_{kind}_count_too_large:{count}"));
    }
    Ok(())
}

fn read_metadata_value(
    reader: &mut File,
    value_type: u32,
    key: &str,
) -> Result<Option<GgufScalarValue>, String> {
    let value = match value_type {
        VALUE_UINT8 => GgufScalarValue::Unsigned(u64::from(read_u8(reader)?)),
        VALUE_INT8 => GgufScalarValue::Signed(i64::from(read_i8(reader)?)),
        VALUE_UINT16 => GgufScalarValue::Unsigned(u64::from(read_u16(reader)?)),
        VALUE_INT16 => GgufScalarValue::Signed(i64::from(read_i16(reader)?)),
        VALUE_UINT32 => GgufScalarValue::Unsigned(u64::from(read_u32(reader)?)),
        VALUE_INT32 => GgufScalarValue::Signed(i64::from(read_i32(reader)?)),
        VALUE_FLOAT32 => GgufScalarValue::Float(f64::from(read_f32(reader)?)),
        VALUE_BOOL => GgufScalarValue::Bool(read_u8(reader)? != 0),
        VALUE_UINT64 => GgufScalarValue::Unsigned(read_u64(reader)?),
        VALUE_INT64 => GgufScalarValue::Signed(read_i64(reader)?),
        VALUE_FLOAT64 => GgufScalarValue::Float(read_f64(reader)?),
        VALUE_STRING if matches!(key, "general.name" | "general.architecture") => {
            GgufScalarValue::String(read_string(reader)?)
        }
        VALUE_STRING => {
            skip_string(reader)?;
            return Ok(None);
        }
        VALUE_ARRAY => {
            let item_type = read_u32(reader)?;
            let count = read_u64(reader)?;
            validate_entry_count("metadata_array", count)?;
            skip_array(reader, item_type, count, 0)?;
            return Ok(None);
        }
        _ => {
            return Err(format!(
                "deepseek_gguf_unknown_metadata_type:{key}:{value_type}"
            ))
        }
    };
    Ok(Some(value))
}

fn skip_array(reader: &mut File, item_type: u32, count: u64, depth: u8) -> Result<(), String> {
    if depth > 8 {
        return Err("deepseek_gguf_metadata_array_too_deep".to_string());
    }
    if let Some(bytes) = scalar_bytes(item_type) {
        let total = count
            .checked_mul(bytes)
            .ok_or_else(|| "deepseek_gguf_metadata_array_size_overflow".to_string())?;
        return skip_bytes(reader, total);
    }
    match item_type {
        VALUE_STRING => {
            for _ in 0..count {
                skip_string(reader)?;
            }
        }
        VALUE_ARRAY => {
            for _ in 0..count {
                let nested_type = read_u32(reader)?;
                let nested_count = read_u64(reader)?;
                validate_entry_count("metadata_array", nested_count)?;
                skip_array(reader, nested_type, nested_count, depth + 1)?;
            }
        }
        _ => return Err(format!("deepseek_gguf_unknown_array_type:{item_type}")),
    }
    Ok(())
}

fn scalar_bytes(value_type: u32) -> Option<u64> {
    match value_type {
        VALUE_UINT8 | VALUE_INT8 | VALUE_BOOL => Some(1),
        VALUE_UINT16 | VALUE_INT16 => Some(2),
        VALUE_UINT32 | VALUE_INT32 | VALUE_FLOAT32 => Some(4),
        VALUE_UINT64 | VALUE_INT64 | VALUE_FLOAT64 => Some(8),
        _ => None,
    }
}

fn read_string(reader: &mut File) -> Result<String, String> {
    let len = read_u64(reader)?;
    if len > MAX_STRING_BYTES {
        return Err(format!("deepseek_gguf_string_too_large:{len}"));
    }
    let bytes = usize::try_from(len)
        .map_err(|_| "deepseek_gguf_string_size_unrepresentable".to_string())?;
    let mut payload = vec![0u8; bytes];
    reader
        .read_exact(&mut payload)
        .map_err(|err| format!("deepseek_gguf_string_read_failed:{err}"))?;
    String::from_utf8(payload).map_err(|err| format!("deepseek_gguf_string_not_utf8:{err}"))
}

fn skip_string(reader: &mut File) -> Result<(), String> {
    let len = read_u64(reader)?;
    if len > MAX_STRING_BYTES {
        return Err(format!("deepseek_gguf_string_too_large:{len}"));
    }
    skip_bytes(reader, len)
}

fn skip_bytes(reader: &mut File, bytes: u64) -> Result<(), String> {
    let offset =
        i64::try_from(bytes).map_err(|_| format!("deepseek_gguf_skip_too_large:{bytes}"))?;
    reader
        .seek(SeekFrom::Current(offset))
        .map_err(|err| format!("deepseek_gguf_skip_failed:{err}"))?;
    Ok(())
}

fn align_up(value: u64, alignment: u64) -> Result<u64, String> {
    if alignment == 0 {
        return Err("deepseek_gguf_zero_alignment".to_string());
    }
    let remainder = value % alignment;
    if remainder == 0 {
        return Ok(value);
    }
    value
        .checked_add(alignment - remainder)
        .ok_or_else(|| "deepseek_gguf_alignment_overflow".to_string())
}

fn tensor_bytes(tensor_type: GgufTensorType, elements: u64) -> Result<u64, String> {
    let blocks = elements
        .checked_add(tensor_type.block_elements - 1)
        .ok_or_else(|| "deepseek_gguf_tensor_block_count_overflow".to_string())?
        / tensor_type.block_elements;
    blocks
        .checked_mul(tensor_type.block_bytes)
        .ok_or_else(|| "deepseek_gguf_tensor_byte_count_overflow".to_string())
}

pub fn gguf_tensor_type(code: u32) -> Option<GgufTensorType> {
    let (name, block_elements, block_bytes) = match code {
        0 => ("f32", 1, 4),
        1 => ("f16", 1, 2),
        2 => ("q4_0", 32, 18),
        3 => ("q4_1", 32, 20),
        6 => ("q5_0", 32, 22),
        7 => ("q5_1", 32, 24),
        8 => ("q8_0", 32, 34),
        9 => ("q8_1", 32, 40),
        10 => ("q2_k", 256, 84),
        11 => ("q3_k", 256, 110),
        12 => ("q4_k", 256, 144),
        13 => ("q5_k", 256, 176),
        14 => ("q6_k", 256, 210),
        15 => ("q8_k", 256, 292),
        16 => ("iq2_xxs", 256, 66),
        17 => ("iq2_xs", 256, 74),
        18 => ("iq3_xxs", 256, 98),
        19 => ("iq1_s", 256, 110),
        20 => ("iq4_nl", 256, 50),
        21 => ("iq3_s", 256, 110),
        22 => ("iq2_s", 256, 82),
        23 => ("iq4_xs", 256, 136),
        24 => ("i8", 1, 1),
        25 => ("i16", 1, 2),
        26 => ("i32", 1, 4),
        27 => ("i64", 1, 8),
        28 => ("f64", 1, 8),
        29 => ("iq1_m", 256, 56),
        30 => ("bf16", 1, 2),
        _ => return None,
    };
    Some(GgufTensorType {
        code,
        name,
        block_elements,
        block_bytes,
    })
}

pub fn lower_q8_0_matrix_to_bf16_kxn(
    payload: &[u8],
    dimensions: &[u64],
) -> Result<Vec<u8>, String> {
    if dimensions.len() != 2 {
        return Err(format!(
            "deepseek_q8_0_matrix_rank_mismatch:{}",
            dimensions.len()
        ));
    }
    let k = usize::try_from(dimensions[0])
        .map_err(|_| "deepseek_q8_0_matrix_k_too_large".to_string())?;
    let n = usize::try_from(dimensions[1])
        .map_err(|_| "deepseek_q8_0_matrix_n_too_large".to_string())?;
    if k == 0 || n == 0 || k % 32 != 0 {
        return Err(format!("deepseek_q8_0_matrix_shape_invalid:{k}x{n}"));
    }
    let blocks_per_row = k / 32;
    let row_bytes = blocks_per_row
        .checked_mul(34)
        .ok_or_else(|| "deepseek_q8_0_row_bytes_overflow".to_string())?;
    let expected_bytes = n
        .checked_mul(row_bytes)
        .ok_or_else(|| "deepseek_q8_0_payload_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "deepseek_q8_0_payload_size_mismatch:actual={}:expected={expected_bytes}",
            payload.len()
        ));
    }
    let output_elements = k
        .checked_mul(n)
        .ok_or_else(|| "deepseek_q8_0_output_elements_overflow".to_string())?;
    let mut output = vec![0u8; output_elements * std::mem::size_of::<u16>()];
    for output_column in 0..n {
        let row = &payload[output_column * row_bytes..(output_column + 1) * row_bytes];
        for block in 0..blocks_per_row {
            let block_payload = &row[block * 34..(block + 1) * 34];
            let scale = f16_to_f32(u16::from_le_bytes([block_payload[0], block_payload[1]]));
            if !scale.is_finite() {
                return Err(format!(
                    "deepseek_q8_0_scale_non_finite:row={output_column}:block={block}"
                ));
            }
            for lane in 0..32 {
                let input_row = block * 32 + lane;
                let quantized = i8::from_le_bytes([block_payload[2 + lane]]);
                let bf16 = f32_to_bf16_rne(scale * f32::from(quantized));
                let destination = (input_row * n + output_column) * 2;
                output[destination..destination + 2].copy_from_slice(&bf16.to_le_bytes());
            }
        }
    }
    Ok(output)
}

#[derive(Clone, Debug, PartialEq)]
pub struct Q8_0Activation {
    pub values: Vec<i8>,
    pub scales: Vec<f32>,
}

pub fn quantize_q8_0_activation(input: &[f32]) -> Result<Q8_0Activation, String> {
    if input.is_empty() || input.len() % 32 != 0 {
        return Err(format!(
            "deepseek_q8_0_activation_shape_invalid:{}",
            input.len()
        ));
    }
    let mut values = vec![0i8; input.len()];
    let mut scales = Vec::with_capacity(input.len() / 32);
    for (block, source) in input.chunks_exact(32).enumerate() {
        if source.iter().any(|value| !value.is_finite()) {
            return Err(format!("deepseek_q8_0_activation_non_finite:block={block}"));
        }
        let amax = source
            .iter()
            .fold(0.0f32, |current, value| current.max(value.abs()));
        let scale = amax / 127.0;
        let inverse_scale = if scale == 0.0 { 0.0 } else { scale.recip() };
        scales.push(scale);
        for (destination, value) in values[block * 32..(block + 1) * 32].iter_mut().zip(source) {
            *destination = (value * inverse_scale)
                .round_ties_even()
                .clamp(-128.0, 127.0) as i8;
        }
    }
    Ok(Q8_0Activation { values, scales })
}

pub fn project_q8_0_matrix(
    payload: &[u8],
    dimensions: &[u64],
    input: &[f32],
) -> Result<Vec<f32>, String> {
    if dimensions.len() != 2 {
        return Err(format!(
            "deepseek_q8_0_matrix_rank_mismatch:{}",
            dimensions.len()
        ));
    }
    let k = usize::try_from(dimensions[0])
        .map_err(|_| "deepseek_q8_0_matrix_k_too_large".to_string())?;
    let n = usize::try_from(dimensions[1])
        .map_err(|_| "deepseek_q8_0_matrix_n_too_large".to_string())?;
    if k == 0 || n == 0 || k % 32 != 0 {
        return Err(format!("deepseek_q8_0_matrix_shape_invalid:{k}x{n}"));
    }
    if input.len() != k {
        return Err(format!(
            "deepseek_q8_0_activation_size_mismatch:actual={}:expected={k}",
            input.len()
        ));
    }
    let blocks_per_row = k / 32;
    let row_bytes = blocks_per_row
        .checked_mul(34)
        .ok_or_else(|| "deepseek_q8_0_row_bytes_overflow".to_string())?;
    let expected_bytes = n
        .checked_mul(row_bytes)
        .ok_or_else(|| "deepseek_q8_0_payload_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "deepseek_q8_0_payload_size_mismatch:actual={}:expected={expected_bytes}",
            payload.len()
        ));
    }

    let activation = quantize_q8_0_activation(input)?;
    let mut output = vec![0.0f32; n];
    for (output_index, row) in payload.chunks_exact(row_bytes).enumerate() {
        let mut accumulator = 0.0f32;
        for block in 0..blocks_per_row {
            let weight_block = &row[block * 34..(block + 1) * 34];
            let weight_scale = f16_to_f32(u16::from_le_bytes([weight_block[0], weight_block[1]]));
            if !weight_scale.is_finite() {
                return Err(format!(
                    "deepseek_q8_0_scale_non_finite:row={output_index}:block={block}"
                ));
            }
            let activation_values = &activation.values[block * 32..(block + 1) * 32];
            let dot = weight_block[2..].iter().zip(activation_values).fold(
                0i32,
                |sum, (weight, activation)| {
                    sum + i32::from(i8::from_le_bytes([*weight])) * i32::from(*activation)
                },
            );
            accumulator += weight_scale * activation.scales[block] * dot as f32;
        }
        output[output_index] = accumulator;
    }
    Ok(output)
}

pub fn q8_0_weight_block_kxn(
    payload: &[u8],
    dimensions: &[u64],
    block: usize,
) -> Result<(Vec<u8>, Vec<f32>), String> {
    if dimensions.len() != 2 {
        return Err(format!(
            "deepseek_q8_0_matrix_rank_mismatch:{}",
            dimensions.len()
        ));
    }
    let k = usize::try_from(dimensions[0])
        .map_err(|_| "deepseek_q8_0_matrix_k_too_large".to_string())?;
    let n = usize::try_from(dimensions[1])
        .map_err(|_| "deepseek_q8_0_matrix_n_too_large".to_string())?;
    if k == 0 || n == 0 || k % 32 != 0 {
        return Err(format!("deepseek_q8_0_matrix_shape_invalid:{k}x{n}"));
    }
    let blocks_per_row = k / 32;
    if block >= blocks_per_row {
        return Err(format!(
            "deepseek_q8_0_weight_block_out_of_range:block={block}:blocks={blocks_per_row}"
        ));
    }
    let row_bytes = blocks_per_row
        .checked_mul(34)
        .ok_or_else(|| "deepseek_q8_0_row_bytes_overflow".to_string())?;
    let expected_bytes = n
        .checked_mul(row_bytes)
        .ok_or_else(|| "deepseek_q8_0_payload_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "deepseek_q8_0_payload_size_mismatch:actual={}:expected={expected_bytes}",
            payload.len()
        ));
    }

    let mut weights = vec![0u8; 32 * n];
    let mut scales = vec![0.0f32; n];
    for output_index in 0..n {
        let source = output_index * row_bytes + block * 34;
        let scale = f16_to_f32(u16::from_le_bytes([payload[source], payload[source + 1]]));
        if !scale.is_finite() {
            return Err(format!(
                "deepseek_q8_0_scale_non_finite:row={output_index}:block={block}"
            ));
        }
        scales[output_index] = scale;
        for lane in 0..32 {
            weights[lane * n + output_index] = payload[source + 2 + lane];
        }
    }
    Ok((weights, scales))
}

fn f16_to_f32(bits: u16) -> f32 {
    let sign = u32::from(bits & 0x8000) << 16;
    let exponent = u32::from((bits >> 10) & 0x1f);
    let fraction = u32::from(bits & 0x03ff);
    let output = match exponent {
        0 if fraction == 0 => sign,
        0 => {
            let mut normalized = fraction;
            let mut exponent_adjust = 113u32;
            while normalized & 0x0400 == 0 {
                normalized <<= 1;
                exponent_adjust -= 1;
            }
            sign | (exponent_adjust << 23) | ((normalized & 0x03ff) << 13)
        }
        0x1f => sign | 0x7f80_0000 | (fraction << 13),
        _ => sign | ((exponent + 112) << 23) | (fraction << 13),
    };
    f32::from_bits(output)
}

fn f32_to_bf16_rne(value: f32) -> u16 {
    let bits = value.to_bits();
    let rounding_bias = 0x7fff + ((bits >> 16) & 1);
    ((bits.wrapping_add(rounding_bias)) >> 16) as u16
}

fn read_exact<const N: usize>(reader: &mut File) -> Result<[u8; N], String> {
    let mut bytes = [0u8; N];
    reader
        .read_exact(&mut bytes)
        .map_err(|err| format!("deepseek_gguf_read_failed:{err}"))?;
    Ok(bytes)
}

fn read_u8(reader: &mut File) -> Result<u8, String> {
    Ok(read_exact::<1>(reader)?[0])
}

fn read_i8(reader: &mut File) -> Result<i8, String> {
    Ok(i8::from_le_bytes(read_exact::<1>(reader)?))
}

fn read_u16(reader: &mut File) -> Result<u16, String> {
    Ok(u16::from_le_bytes(read_exact::<2>(reader)?))
}

fn read_i16(reader: &mut File) -> Result<i16, String> {
    Ok(i16::from_le_bytes(read_exact::<2>(reader)?))
}

fn read_u32(reader: &mut File) -> Result<u32, String> {
    Ok(u32::from_le_bytes(read_exact::<4>(reader)?))
}

fn read_i32(reader: &mut File) -> Result<i32, String> {
    Ok(i32::from_le_bytes(read_exact::<4>(reader)?))
}

fn read_f32(reader: &mut File) -> Result<f32, String> {
    Ok(f32::from_le_bytes(read_exact::<4>(reader)?))
}

fn read_u64(reader: &mut File) -> Result<u64, String> {
    Ok(u64::from_le_bytes(read_exact::<8>(reader)?))
}

fn read_i64(reader: &mut File) -> Result<i64, String> {
    Ok(i64::from_le_bytes(read_exact::<8>(reader)?))
}

fn read_f64(reader: &mut File) -> Result<f64, String> {
    Ok(f64::from_le_bytes(read_exact::<8>(reader)?))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT_FIXTURE_ID: AtomicU64 = AtomicU64::new(1);

    fn push_string(output: &mut Vec<u8>, value: &str) {
        output.extend_from_slice(&(value.len() as u64).to_le_bytes());
        output.extend_from_slice(value.as_bytes());
    }

    fn push_u32_metadata(output: &mut Vec<u8>, key: &str, value: u32) {
        push_string(output, key);
        output.extend_from_slice(&VALUE_UINT32.to_le_bytes());
        output.extend_from_slice(&value.to_le_bytes());
    }

    fn fixture_bytes() -> Vec<u8> {
        let mut output = Vec::new();
        output.extend_from_slice(&GGUF_MAGIC.to_le_bytes());
        output.extend_from_slice(&GGUF_VERSION.to_le_bytes());
        output.extend_from_slice(&2u64.to_le_bytes());
        output.extend_from_slice(&3u64.to_le_bytes());
        push_u32_metadata(&mut output, "general.alignment", 32);
        push_u32_metadata(&mut output, "deepseek4.block_count", 43);
        push_string(&mut output, "general.architecture");
        output.extend_from_slice(&VALUE_STRING.to_le_bytes());
        push_string(&mut output, "deepseek4");

        push_string(&mut output, "blk.0.attn_q_a.weight");
        output.extend_from_slice(&2u32.to_le_bytes());
        output.extend_from_slice(&4u64.to_le_bytes());
        output.extend_from_slice(&2u64.to_le_bytes());
        output.extend_from_slice(&30u32.to_le_bytes());
        output.extend_from_slice(&0u64.to_le_bytes());

        push_string(&mut output, "blk.0.attn_q_b.weight");
        output.extend_from_slice(&2u32.to_le_bytes());
        output.extend_from_slice(&2u64.to_le_bytes());
        output.extend_from_slice(&3u64.to_le_bytes());
        output.extend_from_slice(&24u32.to_le_bytes());
        output.extend_from_slice(&32u64.to_le_bytes());

        while output.len() % 32 != 0 {
            output.push(0);
        }
        output.extend(0u8..16);
        output.resize(output.len() + 16, 0);
        output.extend_from_slice(&[9, 8, 7, 6, 5, 4]);
        output
    }

    fn bf16_at(payload: &[u8], index: usize) -> f32 {
        let offset = index * 2;
        f32::from_bits(u32::from(u16::from_le_bytes([payload[offset], payload[offset + 1]])) << 16)
    }

    fn with_fixture<T>(bytes: &[u8], test: impl FnOnce(&Path) -> T) -> T {
        let id = NEXT_FIXTURE_ID.fetch_add(1, Ordering::Relaxed);
        let path = std::env::temp_dir().join(format!(
            "sim-models-deepseek-gguf-{}-{id}.gguf",
            std::process::id()
        ));
        fs::write(&path, bytes).expect("write GGUF fixture");
        let output = test(&path);
        fs::remove_file(path).expect("remove GGUF fixture");
        output
    }

    #[test]
    fn parses_tensor_directory_and_reads_selected_payload() {
        with_fixture(&fixture_bytes(), |path| {
            let catalog = GgufCatalog::open(path).expect("parse GGUF fixture");
            assert_eq!(catalog.version, 3);
            assert_eq!(catalog.alignment, 32);
            assert_eq!(catalog.metadata_u64("deepseek4.block_count"), Ok(43));
            let q_a = catalog.tensor("blk.0.attn_q_a.weight").expect("Q A tensor");
            assert_eq!(q_a.dimensions, vec![4, 2]);
            assert_eq!(q_a.tensor_type.name, "bf16");
            assert_eq!(q_a.bytes, 16);
            assert_eq!(
                catalog
                    .read_tensor("blk.0.attn_q_a.weight")
                    .expect("Q A payload"),
                (0u8..16).collect::<Vec<_>>()
            );
            let q_b = catalog.tensor("blk.0.attn_q_b.weight").expect("Q B tensor");
            assert_eq!(q_b.tensor_type.name, "i8");
            assert_eq!(
                catalog.read_tensor(&q_b.name).unwrap(),
                vec![9, 8, 7, 6, 5, 4]
            );
        });
    }

    #[test]
    fn rejects_tensor_payload_outside_file() {
        let mut bytes = fixture_bytes();
        bytes.truncate(bytes.len() - 1);
        with_fixture(&bytes, |path| {
            let error = GgufCatalog::open(path).expect_err("truncated GGUF must fail");
            assert!(error.contains("tensor_out_of_file"));
        });
    }

    #[test]
    fn flash_validation_fails_closed_on_incomplete_geometry() {
        with_fixture(&fixture_bytes(), |path| {
            let catalog = GgufCatalog::open(path).expect("parse GGUF fixture");
            let error = catalog
                .validate_deepseek_v4_flash()
                .expect_err("incomplete Flash metadata must fail");
            assert!(error.contains("unsigned_metadata_missing"));
        });
    }

    #[test]
    fn lowers_q8_0_rows_to_backend_kxn_bf16_layout() {
        let mut payload = Vec::new();
        payload.extend_from_slice(&0x3800u16.to_le_bytes());
        payload.extend((0..32).map(|value| (value as i8 - 16) as u8));
        payload.extend_from_slice(&0x4000u16.to_le_bytes());
        payload.extend(std::iter::repeat_n(1u8, 32));

        let output = lower_q8_0_matrix_to_bf16_kxn(&payload, &[32, 2]).expect("lower Q8_0 matrix");
        assert_eq!(output.len(), 32 * 2 * 2);
        assert_eq!(bf16_at(&output, 0), -8.0);
        assert_eq!(bf16_at(&output, 1), 2.0);
        assert_eq!(bf16_at(&output, 30 * 2), 7.0);
        assert_eq!(bf16_at(&output, 31 * 2 + 1), 2.0);
    }

    #[test]
    fn q8_0_lowering_rejects_unaligned_or_short_payload() {
        assert!(lower_q8_0_matrix_to_bf16_kxn(&[], &[31, 1])
            .unwrap_err()
            .contains("shape_invalid"));
        assert!(lower_q8_0_matrix_to_bf16_kxn(&[0; 33], &[32, 1])
            .unwrap_err()
            .contains("payload_size_mismatch"));
    }

    #[test]
    fn q8_0_projection_matches_ds4_block_quantization_rules() {
        let input: Vec<f32> = (-16..16).map(|value| value as f32 / 4.0).collect();
        let activation = quantize_q8_0_activation(&input).expect("quantize activation");
        assert_eq!(activation.scales, vec![4.0 / 127.0]);
        assert_eq!(activation.values[0], -127);
        assert_eq!(activation.values[16], 0);

        let mut payload = Vec::new();
        payload.extend_from_slice(&0x3800u16.to_le_bytes());
        payload.extend((0..32).map(|index| if index % 2 == 0 { 2u8 } else { (-3i8) as u8 }));
        payload.extend_from_slice(&0x4000u16.to_le_bytes());
        payload.extend(std::iter::repeat_n(1u8, 32));

        let projected =
            project_q8_0_matrix(&payload, &[32, 2], &input).expect("project Q8_0 matrix");
        let first_dot = (0..32).fold(0i32, |sum, index| {
            let weight = if index % 2 == 0 { 2 } else { -3 };
            sum + weight * i32::from(activation.values[index])
        });
        let second_dot: i32 = activation
            .values
            .iter()
            .map(|value| i32::from(*value))
            .sum();
        assert_eq!(projected[0], 0.5 * activation.scales[0] * first_dot as f32);
        assert_eq!(projected[1], 2.0 * activation.scales[0] * second_dot as f32);
    }

    #[test]
    fn q8_0_activation_quantization_fails_closed() {
        assert!(quantize_q8_0_activation(&[0.0; 31])
            .unwrap_err()
            .contains("shape_invalid"));
        let mut non_finite = [0.0; 32];
        non_finite[4] = f32::NAN;
        assert!(quantize_q8_0_activation(&non_finite)
            .unwrap_err()
            .contains("non_finite"));
        assert!(project_q8_0_matrix(&[0; 34], &[32, 1], &[0.0; 31])
            .unwrap_err()
            .contains("size_mismatch"));
    }

    #[test]
    fn extracts_q8_0_weight_block_in_simpler_kxn_layout() {
        let mut payload = Vec::new();
        for output in 0..2u8 {
            for block in 0..2u8 {
                payload.extend_from_slice(&(0x3800u16 + u16::from(block) * 0x0400).to_le_bytes());
                payload.extend((0..32u8).map(|lane| output * 64 + block * 32 + lane));
            }
        }
        let (weights, scales) =
            q8_0_weight_block_kxn(&payload, &[64, 2], 1).expect("extract block");
        assert_eq!(scales, vec![1.0, 1.0]);
        assert_eq!(&weights[0..4], &[32, 96, 33, 97]);
        assert_eq!(&weights[60..64], &[62, 126, 63, 127]);
        assert!(q8_0_weight_block_kxn(&payload, &[64, 2], 2)
            .unwrap_err()
            .contains("out_of_range"));
    }
}
