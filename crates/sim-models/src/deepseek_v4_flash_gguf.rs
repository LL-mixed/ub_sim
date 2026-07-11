use crate::deepseek_v4_flash::DEEPSEEK_V4_FLASH_PROFILE;
use crate::deepseek_v4_flash_iq2_tables::{IQ2_XS_SIGNS, IQ2_XXS_GRID};
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
        self.read_tensor_byte_range(name, 0, tensor.bytes)
    }

    pub fn read_tensor_byte_range(
        &self,
        name: &str,
        offset: u64,
        bytes: u64,
    ) -> Result<Vec<u8>, String> {
        let tensor = self.tensor(name)?;
        let end = offset
            .checked_add(bytes)
            .ok_or_else(|| format!("deepseek_gguf_tensor_range_overflow:{name}"))?;
        if end > tensor.bytes {
            return Err(format!(
                "deepseek_gguf_tensor_range_out_of_bounds:{name}:offset={offset}:bytes={bytes}:tensor_bytes={}",
                tensor.bytes
            ));
        }
        let bytes = usize::try_from(bytes)
            .map_err(|_| format!("deepseek_gguf_tensor_range_too_large:{name}"))?;
        let absolute_offset = tensor
            .absolute_offset
            .checked_add(offset)
            .ok_or_else(|| format!("deepseek_gguf_tensor_absolute_offset_overflow:{name}"))?;
        let mut reader = File::open(&self.path).map_err(|err| {
            format!(
                "deepseek_gguf_tensor_open_failed:{}:{err}",
                self.path.display()
            )
        })?;
        reader
            .seek(SeekFrom::Start(absolute_offset))
            .map_err(|err| format!("deepseek_gguf_tensor_seek_failed:{name}:{err}"))?;
        let mut payload = vec![0u8; bytes];
        reader
            .read_exact(&mut payload)
            .map_err(|err| format!("deepseek_gguf_tensor_read_failed:{name}:{err}"))?;
        Ok(payload)
    }

    pub fn read_expert_tensor_slice(
        &self,
        name: &str,
        expert: usize,
    ) -> Result<(Vec<u8>, Vec<u64>), String> {
        let tensor = self.tensor(name)?;
        if tensor.dimensions.len() != 3 {
            return Err(format!(
                "deepseek_gguf_expert_tensor_rank_invalid:{name}:rank={}",
                tensor.dimensions.len()
            ));
        }
        let experts = usize::try_from(tensor.dimensions[2])
            .map_err(|_| format!("deepseek_gguf_expert_count_too_large:{name}"))?;
        if expert >= experts {
            return Err(format!(
                "deepseek_gguf_expert_out_of_range:{name}:expert={expert}:experts={experts}"
            ));
        }
        if tensor.bytes % tensor.dimensions[2] != 0 {
            return Err(format!(
                "deepseek_gguf_expert_tensor_bytes_unaligned:{name}:bytes={}:experts={experts}",
                tensor.bytes
            ));
        }
        let expert_bytes = tensor.bytes / tensor.dimensions[2];
        let offset = (expert as u64)
            .checked_mul(expert_bytes)
            .ok_or_else(|| format!("deepseek_gguf_expert_offset_overflow:{name}"))?;
        let payload = self.read_tensor_byte_range(name, offset, expert_bytes)?;
        Ok((payload, vec![tensor.dimensions[0], tensor.dimensions[1], 1]))
    }

    pub fn read_f16_matrix_row(&self, name: &str, row: usize) -> Result<Vec<f32>, String> {
        let tensor = self.tensor(name)?;
        if tensor.tensor_type.name != "f16" || tensor.dimensions.len() != 2 {
            return Err(format!(
                "deepseek_f16_matrix_row_type_mismatch:{name}:type={}:rank={}",
                tensor.tensor_type.name,
                tensor.dimensions.len()
            ));
        }
        let columns = usize::try_from(tensor.dimensions[0])
            .map_err(|_| format!("deepseek_f16_matrix_columns_too_large:{name}"))?;
        let rows = usize::try_from(tensor.dimensions[1])
            .map_err(|_| format!("deepseek_f16_matrix_rows_too_large:{name}"))?;
        if row >= rows {
            return Err(format!(
                "deepseek_f16_matrix_row_out_of_range:{name}:row={row}:rows={rows}"
            ));
        }
        let row_bytes = columns
            .checked_mul(2)
            .ok_or_else(|| format!("deepseek_f16_matrix_row_bytes_overflow:{name}"))?;
        let row_offset = row
            .checked_mul(row_bytes)
            .and_then(|offset| u64::try_from(offset).ok())
            .and_then(|offset| tensor.absolute_offset.checked_add(offset))
            .ok_or_else(|| format!("deepseek_f16_matrix_row_offset_overflow:{name}"))?;
        let mut reader = File::open(&self.path).map_err(|err| {
            format!(
                "deepseek_gguf_tensor_open_failed:{}:{err}",
                self.path.display()
            )
        })?;
        reader
            .seek(SeekFrom::Start(row_offset))
            .map_err(|err| format!("deepseek_f16_matrix_row_seek_failed:{name}:{err}"))?;
        let mut payload = vec![0u8; row_bytes];
        reader
            .read_exact(&mut payload)
            .map_err(|err| format!("deepseek_f16_matrix_row_read_failed:{name}:{err}"))?;
        payload
            .chunks_exact(2)
            .enumerate()
            .map(|(column, bytes)| {
                let value = f16_to_f32(u16::from_le_bytes([bytes[0], bytes[1]]));
                value.is_finite().then_some(value).ok_or_else(|| {
                    format!(
                        "deepseek_f16_matrix_row_value_non_finite:{name}:row={row}:column={column}"
                    )
                })
            })
            .collect()
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

pub fn lower_f16_matrix_to_f32_kxn_padded(
    payload: &[u8],
    dimensions: &[u64],
    padded_n: usize,
) -> Result<Vec<f32>, String> {
    if dimensions.len() != 2 {
        return Err(format!(
            "deepseek_f16_matrix_rank_mismatch:{}",
            dimensions.len()
        ));
    }
    let k = usize::try_from(dimensions[0])
        .map_err(|_| "deepseek_f16_matrix_k_too_large".to_string())?;
    let n = usize::try_from(dimensions[1])
        .map_err(|_| "deepseek_f16_matrix_n_too_large".to_string())?;
    if k == 0 || n == 0 || padded_n < n {
        return Err(format!(
            "deepseek_f16_matrix_shape_invalid:{k}x{n}:padded_n={padded_n}"
        ));
    }
    let expected_bytes = k
        .checked_mul(n)
        .and_then(|elements| elements.checked_mul(2))
        .ok_or_else(|| "deepseek_f16_matrix_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "deepseek_f16_matrix_payload_size_mismatch:actual={}:expected={expected_bytes}",
            payload.len()
        ));
    }

    let output_elements = k
        .checked_mul(padded_n)
        .ok_or_else(|| "deepseek_f16_matrix_output_size_overflow".to_string())?;
    let mut output = vec![0.0f32; output_elements];
    for output_column in 0..n {
        for input_row in 0..k {
            let source = (output_column * k + input_row) * 2;
            let value = f16_to_f32(u16::from_le_bytes([payload[source], payload[source + 1]]));
            if !value.is_finite() {
                return Err(format!(
                    "deepseek_f16_matrix_value_non_finite:row={input_row}:column={output_column}"
                ));
            }
            output[input_row * padded_n + output_column] = value;
        }
    }
    Ok(output)
}

pub fn project_f16_matrix(
    payload: &[u8],
    dimensions: &[u64],
    input: &[f32],
) -> Result<Vec<f32>, String> {
    if dimensions.len() != 2 {
        return Err(format!(
            "deepseek_f16_matrix_rank_mismatch:{}",
            dimensions.len()
        ));
    }
    let k = usize::try_from(dimensions[0])
        .map_err(|_| "deepseek_f16_matrix_k_too_large".to_string())?;
    let n = usize::try_from(dimensions[1])
        .map_err(|_| "deepseek_f16_matrix_n_too_large".to_string())?;
    if k == 0 || n == 0 || input.len() != k {
        return Err(format!(
            "deepseek_f16_projection_shape_mismatch:matrix={k}x{n}:input={}",
            input.len()
        ));
    }
    let expected_bytes = k
        .checked_mul(n)
        .and_then(|elements| elements.checked_mul(2))
        .ok_or_else(|| "deepseek_f16_matrix_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "deepseek_f16_matrix_payload_size_mismatch:actual={}:expected={expected_bytes}",
            payload.len()
        ));
    }
    if input.iter().any(|value| !value.is_finite()) {
        return Err("deepseek_f16_projection_input_non_finite".to_string());
    }

    let mut output = vec![0.0f32; n];
    for (column, output_value) in output.iter_mut().enumerate() {
        let row = &payload[column * k * 2..(column + 1) * k * 2];
        let mut accumulator = 0.0f32;
        for (source, input_value) in row.chunks_exact(2).zip(input) {
            let weight = f16_to_f32(u16::from_le_bytes([source[0], source[1]]));
            accumulator += input_value * weight;
        }
        *output_value = accumulator;
    }
    Ok(output)
}

pub fn decode_f16_tensor(payload: &[u8], dimensions: &[u64]) -> Result<Vec<f32>, String> {
    let elements = dimensions.iter().try_fold(1usize, |total, dimension| {
        let dimension = usize::try_from(*dimension)
            .map_err(|_| "deepseek_f16_tensor_dimension_too_large".to_string())?;
        total
            .checked_mul(dimension)
            .ok_or_else(|| "deepseek_f16_tensor_elements_overflow".to_string())
    })?;
    if dimensions.is_empty() || elements == 0 {
        return Err("deepseek_f16_tensor_shape_invalid".to_string());
    }
    let expected_bytes = elements
        .checked_mul(2)
        .ok_or_else(|| "deepseek_f16_tensor_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "deepseek_f16_tensor_payload_size_mismatch:actual={}:expected={expected_bytes}",
            payload.len()
        ));
    }
    payload
        .chunks_exact(2)
        .enumerate()
        .map(|(index, bytes)| {
            let value = f16_to_f32(u16::from_le_bytes([bytes[0], bytes[1]]));
            value
                .is_finite()
                .then_some(value)
                .ok_or_else(|| format!("deepseek_f16_tensor_value_non_finite:index={index}"))
        })
        .collect()
}

pub fn decode_f32_tensor(payload: &[u8], dimensions: &[u64]) -> Result<Vec<f32>, String> {
    let elements = dimensions.iter().try_fold(1usize, |total, dimension| {
        let dimension = usize::try_from(*dimension)
            .map_err(|_| "deepseek_f32_tensor_dimension_too_large".to_string())?;
        total
            .checked_mul(dimension)
            .ok_or_else(|| "deepseek_f32_tensor_elements_overflow".to_string())
    })?;
    if dimensions.is_empty() || elements == 0 {
        return Err("deepseek_f32_tensor_shape_invalid".to_string());
    }
    let expected_bytes = elements
        .checked_mul(4)
        .ok_or_else(|| "deepseek_f32_tensor_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "deepseek_f32_tensor_payload_size_mismatch:actual={}:expected={expected_bytes}",
            payload.len()
        ));
    }
    payload
        .chunks_exact(4)
        .enumerate()
        .map(|(index, bytes)| {
            let value = f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]);
            value
                .is_finite()
                .then_some(value)
                .ok_or_else(|| format!("deepseek_f32_tensor_value_non_finite:index={index}"))
        })
        .collect()
}

#[derive(Clone, Debug, PartialEq)]
pub struct Q8_0Activation {
    pub values: Vec<i8>,
    pub scales: Vec<f32>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Q8KActivation {
    pub values: Vec<i8>,
    pub scales: Vec<f32>,
    pub block_sums: Vec<i16>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Q2KExpertDotPlan {
    pub activation_values: Vec<i8>,
    pub weight_values_kxn: Vec<u8>,
    pub weight_scales: Vec<f32>,
    pub minimum_scales: Vec<f32>,
    pub minimum_sums: Vec<i32>,
    pub dot_blocks: usize,
    pub output_size: usize,
}

#[derive(Clone, Debug, PartialEq)]
pub struct Iq2XxsExpertDotPlan {
    pub activation_values: Vec<i8>,
    pub weight_values_kxn: Vec<u8>,
    pub dot_scales: Vec<f32>,
    pub dot_blocks: usize,
    pub output_size: usize,
}

impl Iq2XxsExpertDotPlan {
    pub fn finish(&self, dots: &[i32]) -> Result<Vec<f32>, String> {
        let expected = self
            .dot_blocks
            .checked_mul(self.output_size)
            .ok_or_else(|| "deepseek_iq2_xxs_dot_count_overflow".to_string())?;
        if dots.len() != expected || self.dot_scales.len() != expected {
            return Err(format!(
                "deepseek_iq2_xxs_dot_size_mismatch:dots={}:scales={}:expected={expected}",
                dots.len(),
                self.dot_scales.len()
            ));
        }
        let mut output = vec![0.0f32; self.output_size];
        for block in 0..self.dot_blocks {
            for row in 0..self.output_size {
                let index = block * self.output_size + row;
                output[row] += self.dot_scales[index] * dots[index] as f32;
            }
        }
        Ok(output)
    }
}

impl Q2KExpertDotPlan {
    pub fn finish(&self, dots: &[i32]) -> Result<Vec<f32>, String> {
        let expected_dots = self
            .dot_blocks
            .checked_mul(self.output_size)
            .ok_or_else(|| "deepseek_q2_k_dot_count_overflow".to_string())?;
        if dots.len() != expected_dots {
            return Err(format!(
                "deepseek_q2_k_dot_size_mismatch:actual={}:expected={expected_dots}",
                dots.len()
            ));
        }
        let quant_blocks = self.weight_scales.len() / self.output_size;
        let mut output = vec![0.0f32; self.output_size];
        for block in 0..quant_blocks {
            for row in 0..self.output_size {
                let metadata_index = block * self.output_size + row;
                let quant_sum: i32 = (0..8)
                    .map(|group| dots[(block * 8 + group) * self.output_size + row])
                    .sum();
                output[row] += self.weight_scales[metadata_index] * quant_sum as f32
                    - self.minimum_scales[metadata_index]
                        * self.minimum_sums[metadata_index] as f32;
            }
        }
        Ok(output)
    }
}

pub fn quantize_q8_k_activation(input: &[f32]) -> Result<Q8KActivation, String> {
    const BLOCK_ELEMENTS: usize = 256;
    const SUM_GROUP_ELEMENTS: usize = 16;

    if input.is_empty() || input.len() % BLOCK_ELEMENTS != 0 {
        return Err(format!(
            "deepseek_q8_k_activation_shape_invalid:{}",
            input.len()
        ));
    }
    let mut values = vec![0i8; input.len()];
    let mut scales = Vec::with_capacity(input.len() / BLOCK_ELEMENTS);
    let mut block_sums = Vec::with_capacity(input.len() / SUM_GROUP_ELEMENTS);
    for (block, source) in input.chunks_exact(BLOCK_ELEMENTS).enumerate() {
        if source.iter().any(|value| !value.is_finite()) {
            return Err(format!("deepseek_q8_k_activation_non_finite:block={block}"));
        }
        let mut signed_max = 0.0f32;
        let mut absolute_max = 0.0f32;
        for value in source {
            if value.abs() > absolute_max {
                absolute_max = value.abs();
                signed_max = *value;
            }
        }
        if absolute_max == 0.0 {
            scales.push(0.0);
        } else {
            let inverse_scale = -127.0 / signed_max;
            scales.push(inverse_scale.recip());
            for (destination, value) in values[block * BLOCK_ELEMENTS..(block + 1) * BLOCK_ELEMENTS]
                .iter_mut()
                .zip(source)
            {
                *destination = (value * inverse_scale)
                    .round_ties_even()
                    .clamp(-128.0, 127.0) as i8;
            }
        }
        for group in values[block * BLOCK_ELEMENTS..(block + 1) * BLOCK_ELEMENTS]
            .chunks_exact(SUM_GROUP_ELEMENTS)
        {
            block_sums.push(group.iter().map(|value| i16::from(*value)).sum());
        }
    }
    Ok(Q8KActivation {
        values,
        scales,
        block_sums,
    })
}

pub fn project_q2_k_expert_matrix(
    payload: &[u8],
    dimensions: &[u64],
    expert: usize,
    input: &[f32],
) -> Result<Vec<f32>, String> {
    let plan = lower_q2_k_expert_for_block_dot(payload, dimensions, expert, input)?;
    let mut dots = vec![0i32; plan.dot_blocks * plan.output_size];
    for block in 0..plan.dot_blocks {
        let activation = &plan.activation_values[block * 32..(block + 1) * 32];
        for row in 0..plan.output_size {
            dots[block * plan.output_size + row] = (0..32)
                .map(|lane| {
                    let weight = i8::from_le_bytes([
                        plan.weight_values_kxn[(block * 32 + lane) * plan.output_size + row]
                    ]);
                    i32::from(activation[lane]) * i32::from(weight)
                })
                .sum();
        }
    }
    plan.finish(&dots)
}

pub fn lower_q2_k_expert_for_block_dot(
    payload: &[u8],
    dimensions: &[u64],
    expert: usize,
    input: &[f32],
) -> Result<Q2KExpertDotPlan, String> {
    const BLOCK_ELEMENTS: usize = 256;
    const BLOCK_BYTES: usize = 84;

    let [k, n, experts] = dimensions else {
        return Err(format!(
            "deepseek_q2_k_expert_matrix_rank_mismatch:{}",
            dimensions.len()
        ));
    };
    let k = usize::try_from(*k).map_err(|_| "deepseek_q2_k_matrix_k_too_large".to_string())?;
    let n = usize::try_from(*n).map_err(|_| "deepseek_q2_k_matrix_n_too_large".to_string())?;
    let experts = usize::try_from(*experts)
        .map_err(|_| "deepseek_q2_k_matrix_experts_too_large".to_string())?;
    if k == 0 || n == 0 || experts == 0 || k % BLOCK_ELEMENTS != 0 {
        return Err(format!(
            "deepseek_q2_k_expert_matrix_shape_invalid:{k}x{n}x{experts}"
        ));
    }
    if expert >= experts {
        return Err(format!(
            "deepseek_q2_k_expert_out_of_range:expert={expert}:experts={experts}"
        ));
    }
    if input.len() != k {
        return Err(format!(
            "deepseek_q2_k_activation_size_mismatch:actual={}:expected={k}",
            input.len()
        ));
    }
    let blocks_per_row = k / BLOCK_ELEMENTS;
    let row_bytes = blocks_per_row
        .checked_mul(BLOCK_BYTES)
        .ok_or_else(|| "deepseek_q2_k_row_bytes_overflow".to_string())?;
    let expert_bytes = n
        .checked_mul(row_bytes)
        .ok_or_else(|| "deepseek_q2_k_expert_bytes_overflow".to_string())?;
    let expected_bytes = experts
        .checked_mul(expert_bytes)
        .ok_or_else(|| "deepseek_q2_k_payload_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "deepseek_q2_k_payload_size_mismatch:actual={}:expected={expected_bytes}",
            payload.len()
        ));
    }

    let activation = quantize_q8_k_activation(input)?;
    let expert_start = expert * expert_bytes;
    let expert_payload = &payload[expert_start..expert_start + expert_bytes];
    let dot_blocks = blocks_per_row
        .checked_mul(8)
        .ok_or_else(|| "deepseek_q2_k_dot_blocks_overflow".to_string())?;
    let mut weight_values_kxn = vec![0u8; dot_blocks * 32 * n];
    let mut weight_scales = vec![0.0f32; blocks_per_row * n];
    let mut minimum_scales = vec![0.0f32; blocks_per_row * n];
    let mut minimum_sums = vec![0i32; blocks_per_row * n];
    for (row_index, row) in expert_payload.chunks_exact(row_bytes).enumerate() {
        for block_index in 0..blocks_per_row {
            let block = &row[block_index * BLOCK_BYTES..(block_index + 1) * BLOCK_BYTES];
            let scales = &block[..16];
            let quants = &block[16..80];
            let metadata_index = block_index * n + row_index;
            let weight_scale = f16_to_f32(u16::from_le_bytes([block[80], block[81]]));
            let weight_min = f16_to_f32(u16::from_le_bytes([block[82], block[83]]));
            if !weight_scale.is_finite() || !weight_min.is_finite() {
                return Err(format!(
                    "deepseek_q2_k_scale_non_finite:row={row_index}:block={block_index}"
                ));
            }
            weight_scales[metadata_index] = activation.scales[block_index] * weight_scale;
            minimum_scales[metadata_index] = activation.scales[block_index] * weight_min;
            minimum_sums[metadata_index] = activation.block_sums
                [block_index * 16..(block_index + 1) * 16]
                .iter()
                .zip(scales)
                .map(|(sum, scale)| i32::from(*sum) * i32::from(scale >> 4))
                .sum();
            for group in 0..8 {
                let quant_base = (group / 4) * 32;
                let shift = (group % 4) * 2;
                for lane in 0..32 {
                    let scale = scales[group * 2 + lane / 16] & 0x0f;
                    let quant = (quants[quant_base + lane] >> shift) & 0x03;
                    let destination = ((block_index * 8 + group) * 32 + lane) * n + row_index;
                    weight_values_kxn[destination] = quant * scale;
                }
            }
        }
    }
    Ok(Q2KExpertDotPlan {
        activation_values: activation.values,
        weight_values_kxn,
        weight_scales,
        minimum_scales,
        minimum_sums,
        dot_blocks,
        output_size: n,
    })
}

pub fn project_iq2_xxs_expert_matrix(
    payload: &[u8],
    dimensions: &[u64],
    expert: usize,
    input: &[f32],
) -> Result<Vec<f32>, String> {
    let plan = lower_iq2_xxs_expert_for_block_dot(payload, dimensions, expert, input)?;
    let mut dots = vec![0i32; plan.dot_blocks * plan.output_size];
    for block in 0..plan.dot_blocks {
        let activation = &plan.activation_values[block * 32..(block + 1) * 32];
        for row in 0..plan.output_size {
            dots[block * plan.output_size + row] = (0..32)
                .map(|lane| {
                    let weight = i8::from_le_bytes([
                        plan.weight_values_kxn[(block * 32 + lane) * plan.output_size + row]
                    ]);
                    i32::from(activation[lane]) * i32::from(weight)
                })
                .sum();
        }
    }
    plan.finish(&dots)
}

pub fn lower_iq2_xxs_expert_for_block_dot(
    payload: &[u8],
    dimensions: &[u64],
    expert: usize,
    input: &[f32],
) -> Result<Iq2XxsExpertDotPlan, String> {
    const BLOCK_ELEMENTS: usize = 256;
    const BLOCK_BYTES: usize = 66;

    let [k, n, experts] = dimensions else {
        return Err(format!(
            "deepseek_iq2_xxs_expert_matrix_rank_mismatch:{}",
            dimensions.len()
        ));
    };
    let k = usize::try_from(*k).map_err(|_| "deepseek_iq2_xxs_matrix_k_too_large".to_string())?;
    let n = usize::try_from(*n).map_err(|_| "deepseek_iq2_xxs_matrix_n_too_large".to_string())?;
    let experts = usize::try_from(*experts)
        .map_err(|_| "deepseek_iq2_xxs_matrix_experts_too_large".to_string())?;
    if k == 0 || n == 0 || experts == 0 || k % BLOCK_ELEMENTS != 0 {
        return Err(format!(
            "deepseek_iq2_xxs_expert_matrix_shape_invalid:{k}x{n}x{experts}"
        ));
    }
    if expert >= experts {
        return Err(format!(
            "deepseek_iq2_xxs_expert_out_of_range:expert={expert}:experts={experts}"
        ));
    }
    if input.len() != k {
        return Err(format!(
            "deepseek_iq2_xxs_activation_size_mismatch:actual={}:expected={k}",
            input.len()
        ));
    }
    let blocks_per_row = k / BLOCK_ELEMENTS;
    let row_bytes = blocks_per_row
        .checked_mul(BLOCK_BYTES)
        .ok_or_else(|| "deepseek_iq2_xxs_row_bytes_overflow".to_string())?;
    let expert_bytes = n
        .checked_mul(row_bytes)
        .ok_or_else(|| "deepseek_iq2_xxs_expert_bytes_overflow".to_string())?;
    let expected_bytes = experts
        .checked_mul(expert_bytes)
        .ok_or_else(|| "deepseek_iq2_xxs_payload_bytes_overflow".to_string())?;
    if payload.len() != expected_bytes {
        return Err(format!(
            "deepseek_iq2_xxs_payload_size_mismatch:actual={}:expected={expected_bytes}",
            payload.len()
        ));
    }

    let activation = quantize_q8_k_activation(input)?;
    let dot_blocks = blocks_per_row
        .checked_mul(8)
        .ok_or_else(|| "deepseek_iq2_xxs_dot_blocks_overflow".to_string())?;
    let mut weight_values_kxn = vec![0u8; dot_blocks * 32 * n];
    let mut dot_scales = vec![0.0f32; dot_blocks * n];
    let expert_start = expert * expert_bytes;
    let expert_payload = &payload[expert_start..expert_start + expert_bytes];
    for (row_index, row) in expert_payload.chunks_exact(row_bytes).enumerate() {
        for block_index in 0..blocks_per_row {
            let block = &row[block_index * BLOCK_BYTES..(block_index + 1) * BLOCK_BYTES];
            let block_scale = f16_to_f32(u16::from_le_bytes([block[0], block[1]]));
            if !block_scale.is_finite() {
                return Err(format!(
                    "deepseek_iq2_xxs_scale_non_finite:row={row_index}:block={block_index}"
                ));
            }
            for group in 0..8 {
                let encoded = &block[2 + group * 8..2 + (group + 1) * 8];
                let sign_and_scale = u32::from_le_bytes(encoded[4..8].try_into().unwrap());
                let dot_block = block_index * 8 + group;
                let local_scale = 2 * (sign_and_scale >> 28) + 1;
                dot_scales[dot_block * n + row_index] =
                    0.125 * activation.scales[block_index] * block_scale * local_scale as f32;
                for pair in 0..4 {
                    let grid = IQ2_XXS_GRID[encoded[pair] as usize].to_le_bytes();
                    let signs = IQ2_XS_SIGNS[((sign_and_scale >> (7 * pair)) & 127) as usize];
                    for lane in 0..8 {
                        let magnitude = grid[lane] as i8;
                        let value = if signs & (1 << lane) == 0 {
                            magnitude
                        } else {
                            -magnitude
                        };
                        let destination = ((dot_block * 32 + pair * 8 + lane) * n) + row_index;
                        weight_values_kxn[destination] = value as u8;
                    }
                }
            }
        }
    }
    Ok(Iq2XxsExpertDotPlan {
        activation_values: activation.values,
        weight_values_kxn,
        dot_scales,
        dot_blocks,
        output_size: n,
    })
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
        output.extend_from_slice(&3u64.to_le_bytes());
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
        output.extend_from_slice(&1u32.to_le_bytes());
        output.extend_from_slice(&0u64.to_le_bytes());

        push_string(&mut output, "blk.0.attn_q_b.weight");
        output.extend_from_slice(&2u32.to_le_bytes());
        output.extend_from_slice(&2u64.to_le_bytes());
        output.extend_from_slice(&3u64.to_le_bytes());
        output.extend_from_slice(&24u32.to_le_bytes());
        output.extend_from_slice(&32u64.to_le_bytes());

        push_string(&mut output, "blk.0.ffn_gate_exps.weight");
        output.extend_from_slice(&3u32.to_le_bytes());
        output.extend_from_slice(&2u64.to_le_bytes());
        output.extend_from_slice(&2u64.to_le_bytes());
        output.extend_from_slice(&3u64.to_le_bytes());
        output.extend_from_slice(&24u32.to_le_bytes());
        output.extend_from_slice(&64u64.to_le_bytes());

        while output.len() % 32 != 0 {
            output.push(0);
        }
        let tensor_data_start = output.len();
        for value in [0x3c00u16, 0x4000, 0x4200, 0x4400, 0xbc00, 0x3800, 0, 0x4800] {
            output.extend_from_slice(&value.to_le_bytes());
        }
        output.resize(output.len() + 16, 0);
        output.extend_from_slice(&[9, 8, 7, 6, 5, 4]);
        output.resize(tensor_data_start + 64, 0);
        output.extend_from_slice(&[10, 11, 12, 13, 20, 21, 22, 23, 30, 31, 32, 33]);
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
            assert_eq!(q_a.tensor_type.name, "f16");
            assert_eq!(q_a.bytes, 16);
            assert_eq!(
                catalog
                    .read_f16_matrix_row("blk.0.attn_q_a.weight", 0)
                    .expect("Q A row 0"),
                vec![1.0, 2.0, 3.0, 4.0]
            );
            assert_eq!(
                catalog
                    .read_f16_matrix_row("blk.0.attn_q_a.weight", 1)
                    .expect("Q A row 1"),
                vec![-1.0, 0.5, 0.0, 8.0]
            );
            assert!(catalog
                .read_f16_matrix_row("blk.0.attn_q_a.weight", 2)
                .unwrap_err()
                .contains("out_of_range"));
            let q_b = catalog.tensor("blk.0.attn_q_b.weight").expect("Q B tensor");
            assert_eq!(q_b.tensor_type.name, "i8");
            assert_eq!(
                catalog.read_tensor(&q_b.name).unwrap(),
                vec![9, 8, 7, 6, 5, 4]
            );
        });
    }

    #[test]
    fn reads_only_the_selected_expert_tensor_slice() {
        with_fixture(&fixture_bytes(), |path| {
            let catalog = GgufCatalog::open(path).expect("parse GGUF fixture");
            let name = "blk.0.ffn_gate_exps.weight";
            assert_eq!(
                catalog.read_tensor_byte_range(name, 4, 4).unwrap(),
                vec![20, 21, 22, 23]
            );
            let (payload, dimensions) = catalog
                .read_expert_tensor_slice(name, 2)
                .expect("read expert 2 only");
            assert_eq!(payload, vec![30, 31, 32, 33]);
            assert_eq!(dimensions, vec![2, 2, 1]);
            assert!(catalog
                .read_expert_tensor_slice(name, 3)
                .unwrap_err()
                .contains("expert_out_of_range"));
            assert!(catalog
                .read_tensor_byte_range(name, 10, 3)
                .unwrap_err()
                .contains("range_out_of_bounds"));
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
    fn lowers_f16_rows_to_padded_f32_kxn_layout() {
        let payload = [
            0x3c00u16.to_le_bytes(),
            0x4000u16.to_le_bytes(),
            0xbc00u16.to_le_bytes(),
            0x3800u16.to_le_bytes(),
        ]
        .concat();
        let output =
            lower_f16_matrix_to_f32_kxn_padded(&payload, &[2, 2], 4).expect("lower F16 matrix");
        assert_eq!(output, vec![1.0, -1.0, 0.0, 0.0, 2.0, 0.5, 0.0, 0.0]);
        assert_eq!(
            project_f16_matrix(&payload, &[2, 2], &[3.0, 4.0]).expect("project F16 matrix"),
            vec![11.0, -1.0]
        );
    }

    #[test]
    fn decodes_f32_tensor_and_rejects_non_finite_values() {
        let payload = [1.5f32.to_le_bytes(), (-2.0f32).to_le_bytes()].concat();
        assert_eq!(
            decode_f32_tensor(&payload, &[2]).expect("decode F32 tensor"),
            vec![1.5, -2.0]
        );
        assert!(decode_f32_tensor(&f32::NAN.to_le_bytes(), &[1])
            .unwrap_err()
            .contains("non_finite"));
    }

    #[test]
    fn decodes_f16_tensor_and_rejects_non_finite_values() {
        let payload = [0x3e00u16.to_le_bytes(), 0xc000u16.to_le_bytes()].concat();
        assert_eq!(
            decode_f16_tensor(&payload, &[2]).expect("decode F16 tensor"),
            vec![1.5, -2.0]
        );
        assert!(decode_f16_tensor(&0x7e00u16.to_le_bytes(), &[1])
            .unwrap_err()
            .contains("non_finite"));
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

    fn q2_k_block(scales: [u8; 16], quant: u8, scale: u16, minimum: u16) -> Vec<u8> {
        let mut block = Vec::with_capacity(84);
        block.extend_from_slice(&scales);
        block.extend(std::iter::repeat_n(quant, 64));
        block.extend_from_slice(&scale.to_le_bytes());
        block.extend_from_slice(&minimum.to_le_bytes());
        block
    }

    #[test]
    fn q8_k_activation_quantization_matches_ds4_signed_scale_rules() {
        let mut input = vec![0.0f32; 256];
        input[0] = 2.0;
        input[1] = -2.0;
        let activation = quantize_q8_k_activation(&input).expect("quantize Q8_K activation");
        assert_eq!(activation.scales, vec![-2.0 / 127.0]);
        assert_eq!(&activation.values[..2], &[-127, 127]);
        assert_eq!(activation.block_sums[0], 0);

        let zeros = quantize_q8_k_activation(&[0.0; 256]).expect("quantize zero Q8_K");
        assert_eq!(zeros.scales, vec![0.0]);
        assert!(zeros.values.iter().all(|value| *value == 0));
        assert!(zeros.block_sums.iter().all(|value| *value == 0));
    }

    #[test]
    fn q2_k_projection_selects_expert_and_applies_scale_and_minimum() {
        let zero_row = q2_k_block([0; 16], 0, 0x3c00, 0);
        let quant_row = q2_k_block([2; 16], 0xff, 0x3c00, 0);
        let min_row = q2_k_block([0x10; 16], 0, 0, 0x3c00);
        let payload = [
            zero_row.as_slice(),
            zero_row.as_slice(),
            quant_row.as_slice(),
            min_row.as_slice(),
        ]
        .concat();
        let input = vec![1.0f32; 256];

        assert_eq!(
            project_q2_k_expert_matrix(&payload, &[256, 2, 2], 0, &input)
                .expect("project expert zero"),
            vec![0.0, 0.0]
        );
        assert_eq!(
            project_q2_k_expert_matrix(&payload, &[256, 2, 2], 1, &input)
                .expect("project expert one"),
            vec![1536.0, -256.0]
        );
    }

    #[test]
    fn q2_k_projection_fails_closed_on_invalid_layout() {
        assert!(quantize_q8_k_activation(&[0.0; 255])
            .unwrap_err()
            .contains("shape_invalid"));
        assert!(
            project_q2_k_expert_matrix(&[0; 84], &[256, 1], 0, &[0.0; 256])
                .unwrap_err()
                .contains("rank_mismatch")
        );
        assert!(
            project_q2_k_expert_matrix(&[0; 84], &[256, 1, 1], 1, &[0.0; 256])
                .unwrap_err()
                .contains("out_of_range")
        );
        assert!(
            project_q2_k_expert_matrix(&[0; 83], &[256, 1, 1], 0, &[0.0; 256])
                .unwrap_err()
                .contains("payload_size_mismatch")
        );
    }

    #[test]
    fn iq2_xxs_projection_decodes_grid_signs_and_local_scale() {
        let mut positive = Vec::with_capacity(66);
        positive.extend_from_slice(&0x3c00u16.to_le_bytes());
        positive.extend(std::iter::repeat_n(0u8, 64));
        let mut signed = positive.clone();
        for group in 0..8 {
            let sign_and_scale = 127u32 | (15u32 << 28);
            signed[2 + group * 8 + 4..2 + group * 8 + 8]
                .copy_from_slice(&sign_and_scale.to_le_bytes());
        }
        let payload = [positive, signed].concat();
        let input = vec![1.0f32; 256];

        assert_eq!(
            project_iq2_xxs_expert_matrix(&payload, &[256, 1, 2], 0, &input)
                .expect("project positive IQ2_XXS expert"),
            vec![256.0]
        );
        let selected = project_iq2_xxs_expert_matrix(&payload, &[256, 1, 2], 1, &input)
            .expect("project signed IQ2_XXS expert");
        assert!(selected[0].is_finite());
        assert_ne!(selected, vec![256.0]);
    }

    #[test]
    fn iq2_xxs_projection_fails_closed_on_invalid_layout() {
        assert!(
            project_iq2_xxs_expert_matrix(&[0; 66], &[256, 1], 0, &[0.0; 256])
                .unwrap_err()
                .contains("rank_mismatch")
        );
        assert!(
            project_iq2_xxs_expert_matrix(&[0; 65], &[256, 1, 1], 0, &[0.0; 256])
                .unwrap_err()
                .contains("payload_size_mismatch")
        );
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
