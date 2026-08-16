//! Independent scalar CPU oracle for the official DeepSeek V4 Flash checkpoint.
//!
//! This module intentionally does not call the simpler production backend or
//! DS4. Quantized payloads are decoded directly from Safetensors slices. MX dot
//! products reproduce the A5 kernel's 128-wide `f64` partial sums and `f32`
//! accumulation boundaries before the model's `bfloat16` boundaries.

use serde::Serialize;

use crate::deepseek_v4_flash::{deepseek_v4_flash_rope_coefficients, DEEPSEEK_V4_FLASH_RMS_EPS};
use crate::deepseek_v4_flash_checkpoint::{
    checksum64, DeepseekV4CacheStats, DeepseekV4Checkpoint, DeepseekV4TensorDType,
};
use crate::deepseek_v4_flash_lowering::{
    deepseek_v4_flash_hc_control_input_reference, deepseek_v4_flash_hc_post_reference,
    deepseek_v4_flash_hc_split_reference, deepseek_v4_flash_hc_weighted_sum_reference,
    deepseek_v4_flash_head_rms_norm_reference, deepseek_v4_flash_rms_norm_reference,
    deepseek_v4_flash_rope_tail_reference, deepseek_v4_flash_router_reference,
    deepseek_v4_flash_sink_attention_reference, deepseek_v4_flash_swiglu_reference,
    DeepseekV4FlashHcSplit,
};

pub const FP8_ACTIVATION_BLOCK_SIZE: usize = 128;
pub const FP8_WEIGHT_BLOCK_SIZE: usize = 128;
pub const FP4_WEIGHT_BLOCK_SIZE: usize = 32;

#[derive(Clone, Debug, PartialEq)]
pub struct DynamicFp8Activation {
    pub values: Vec<u8>,
    pub scales: Vec<u8>,
    pub block_size: usize,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4DecodedTensor {
    pub name: String,
    pub dtype: String,
    pub shape: Vec<u64>,
    pub logical_shape: Vec<u64>,
    pub element_offset: u64,
    pub values: Vec<f32>,
    pub values_checksum: String,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4ReferenceKvSummary {
    pub raw_rows: usize,
    pub raw_row_checksum: String,
    pub compressed_rows: usize,
    pub indexer_rows: usize,
    pub attention_compressor_pending_checksum: Option<String>,
    pub indexer_compressor_pending_checksum: Option<String>,
    #[serde(skip_serializing)]
    pub attention_compressor_pending: Option<Vec<f32>>,
    #[serde(skip_serializing)]
    pub indexer_compressor_pending: Option<Vec<f32>>,
    pub indexer_query_checksum: Option<String>,
    pub indexer_weights_checksum: Option<String>,
    #[serde(skip_serializing)]
    pub indexer_query: Option<Vec<f32>>,
    #[serde(skip_serializing)]
    pub indexer_weights: Option<Vec<f32>>,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4ReferenceLayerOutput {
    pub model_revision: String,
    pub config_checksum: String,
    pub index_checksum: String,
    pub layer_id: u64,
    pub token_id: u64,
    pub tensor_metadata_checksum: String,
    pub input_hidden_checksum: String,
    pub attention_output_checksum: String,
    #[serde(skip_serializing)]
    pub attention_output: Vec<f32>,
    #[serde(skip_serializing)]
    pub q_lora: Vec<f32>,
    #[serde(skip_serializing)]
    pub query: Vec<f32>,
    #[serde(skip_serializing)]
    pub attended: Vec<f32>,
    #[serde(skip_serializing)]
    pub low_rank_attention_output: Vec<f32>,
    pub selected_experts: Vec<usize>,
    pub route_weights: Vec<f32>,
    pub kv: DeepseekV4ReferenceKvSummary,
    #[serde(skip_serializing)]
    pub raw_kv: Vec<f32>,
    pub layer_output_hidden: Vec<f32>,
    pub layer_output_hidden_checksum: String,
    pub tensor_cache: DeepseekV4CacheStats,
    pub expert_cache: DeepseekV4CacheStats,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4ReferenceTokenLogit {
    pub token_id: u64,
    pub logit: f32,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4ReferenceHeadOutput {
    pub hc_head_checksum: String,
    pub normalized_hidden_checksum: String,
    #[serde(skip_serializing)]
    pub logits: Vec<f32>,
    pub logits_checksum: String,
    pub top_k: Vec<DeepseekV4ReferenceTokenLogit>,
}

pub fn bf16_to_f32(bits: u16) -> f32 {
    f32::from_bits(u32::from(bits) << 16)
}

pub fn f32_to_bf16_rne(value: f32) -> u16 {
    let bits = value.to_bits();
    if !value.is_finite() {
        return (bits >> 16) as u16;
    }
    let rounding_bias = 0x7fffu32 + ((bits >> 16) & 1);
    bits.wrapping_add(rounding_bias).wrapping_shr(16) as u16
}

pub fn round_to_bf16(value: f32) -> f32 {
    bf16_to_f32(f32_to_bf16_rne(value))
}

pub fn round_slice_to_bf16(values: &mut [f32]) -> Result<(), String> {
    if values.iter().any(|value| !value.is_finite()) {
        return Err("deepseek_v4_reference_bf16_non_finite".to_string());
    }
    for value in values {
        *value = round_to_bf16(*value);
    }
    Ok(())
}

pub fn decode_fp8_e4m3(bits: u8) -> Result<f32, String> {
    let sign = if bits & 0x80 == 0 { 1.0 } else { -1.0 };
    let magnitude = bits & 0x7f;
    let exponent = magnitude >> 3;
    let mantissa = magnitude & 0x07;
    if exponent == 0x0f && mantissa == 0x07 {
        return Err(format!("deepseek_v4_reference_fp8_nan:0x{bits:02x}"));
    }
    let value = if exponent == 0 {
        f32::from(mantissa) * 2.0f32.powi(-9)
    } else {
        (1.0 + f32::from(mantissa) / 8.0) * 2.0f32.powi(i32::from(exponent) - 7)
    };
    Ok(sign * value)
}

/// Round-to-nearest-even conversion matching PyTorch's finite E4M3 format.
pub fn encode_fp8_e4m3(value: f32) -> Result<u8, String> {
    if !value.is_finite() {
        return Err("deepseek_v4_reference_fp8_encode_non_finite".to_string());
    }
    let mut bits = value.to_bits();
    let sign = bits & 0x8000_0000;
    bits ^= sign;
    let encoded = if bits >= (543u32 << 21) {
        0x7e
    } else if bits < (121u32 << 23) {
        let denorm_mask = 141u32 << 23;
        let shifted = (f32::from_bits(bits) + f32::from_bits(denorm_mask)).to_bits();
        shifted.wrapping_sub(denorm_mask) as u8
    } else {
        let mantissa_odd = (bits >> 20) & 1;
        bits = bits.wrapping_add(((7i32 - 127) as u32) << 23);
        bits = bits.wrapping_add(0x7f_fff + mantissa_odd);
        (bits >> 20) as u8
    };
    Ok(encoded | (sign >> 24) as u8)
}

pub fn decode_fp4_e2m1(nibble: u8) -> Result<f32, String> {
    if nibble > 0x0f {
        return Err(format!(
            "deepseek_v4_reference_fp4_nibble_invalid:0x{nibble:02x}"
        ));
    }
    const MAGNITUDES: [f32; 8] = [0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0];
    let sign = if nibble & 0x08 == 0 { 1.0 } else { -1.0 };
    Ok(sign * MAGNITUDES[usize::from(nibble & 0x07)])
}

pub fn decode_ue8m0(bits: u8) -> Result<f32, String> {
    if bits == 0xff {
        return Err("deepseek_v4_reference_ue8m0_nan:0xff".to_string());
    }
    Ok(2.0f32.powi(i32::from(bits) - 127))
}

fn encode_ue8m0_ceil(value: f32) -> Result<u8, String> {
    if !value.is_finite() || value <= 0.0 {
        return Err(format!(
            "deepseek_v4_reference_ue8m0_encode_invalid:{value}"
        ));
    }
    let exponent = value.log2().ceil() as i32;
    Ok((exponent.clamp(-127, 127) + 127) as u8)
}

pub fn quantize_dynamic_fp8_reference(
    input: &[f32],
    block_size: usize,
) -> Result<DynamicFp8Activation, String> {
    if input.is_empty() || block_size == 0 || !input.len().is_multiple_of(block_size) {
        return Err(format!(
            "deepseek_v4_reference_activation_shape_invalid:elements={}:block={block_size}",
            input.len()
        ));
    }
    if input.iter().any(|value| !value.is_finite()) {
        return Err("deepseek_v4_reference_activation_non_finite".to_string());
    }
    let mut values = Vec::with_capacity(input.len());
    let mut scales = Vec::with_capacity(input.len() / block_size);
    for block in input.chunks_exact(block_size) {
        let absolute_max = block
            .iter()
            .map(|value| value.abs())
            .fold(1.0e-4f32, f32::max);
        let scale_bits = encode_ue8m0_ceil(absolute_max / 448.0)?;
        let scale = decode_ue8m0(scale_bits)?;
        scales.push(scale_bits);
        for value in block {
            values.push(encode_fp8_e4m3((value / scale).clamp(-448.0, 448.0))?);
        }
    }
    Ok(DynamicFp8Activation {
        values,
        scales,
        block_size,
    })
}

pub fn dequantize_dynamic_fp8_reference(
    quantized: &DynamicFp8Activation,
) -> Result<Vec<f32>, String> {
    if quantized.block_size == 0
        || quantized.values.is_empty()
        || !quantized.values.len().is_multiple_of(quantized.block_size)
        || quantized.scales.len() != quantized.values.len() / quantized.block_size
    {
        return Err("deepseek_v4_reference_activation_metadata_invalid".to_string());
    }
    quantized
        .values
        .iter()
        .enumerate()
        .map(|(index, value)| {
            Ok(decode_fp8_e4m3(*value)?
                * decode_ue8m0(quantized.scales[index / quantized.block_size])?)
        })
        .collect()
}

pub fn checksum_f32(values: &[f32]) -> String {
    let mut bytes = Vec::with_capacity(values.len() * 4);
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    format!("fnv1a64:{:016x}", checksum64(&bytes))
}

pub fn deterministic_hidden_fixture(seed: u64, elements: usize) -> Vec<f32> {
    let mut state = seed ^ 0x9e37_79b9_7f4a_7c15;
    (0..elements)
        .map(|_| {
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            let word = state.wrapping_mul(0x2545_f491_4f6c_dd1d);
            let centered = ((word >> 40) as i32 - (1 << 23)) as f32 / (1 << 23) as f32;
            round_to_bf16(centered * 0.125)
        })
        .collect()
}

fn stable_sigmoid(value: f32) -> f32 {
    if value >= 0.0 {
        1.0 / (1.0 + crate::deepseek_v4_flash_lowering::deterministic_exp_f32(-value))
    } else {
        let exp = crate::deepseek_v4_flash_lowering::deterministic_exp_f32(value);
        exp / (1.0 + exp)
    }
}

pub fn top_k_logits(
    logits: &[f32],
    top_k: usize,
) -> Result<Vec<DeepseekV4ReferenceTokenLogit>, String> {
    if top_k == 0 || top_k > logits.len() || logits.iter().any(|value| !value.is_finite()) {
        return Err(format!(
            "deepseek_v4_reference_logits_invalid:len={}:top_k={top_k}",
            logits.len()
        ));
    }
    let mut ranked = logits.iter().copied().enumerate().collect::<Vec<_>>();
    ranked.sort_by(|left, right| {
        right
            .1
            .total_cmp(&left.1)
            .then_with(|| left.0.cmp(&right.0))
    });
    ranked
        .into_iter()
        .take(top_k)
        .map(|(token_id, logit)| {
            Ok(DeepseekV4ReferenceTokenLogit {
                token_id: u64::try_from(token_id)
                    .map_err(|_| "deepseek_v4_reference_token_id_overflow".to_string())?,
                logit,
            })
        })
        .collect()
}

fn validate_matrix_inputs(
    output_size: usize,
    input_size: usize,
    row_start: usize,
    row_count: usize,
    input: &[f32],
) -> Result<(), String> {
    if output_size == 0
        || input_size == 0
        || row_count == 0
        || input.len() != input_size
        || row_start
            .checked_add(row_count)
            .is_none_or(|end| end > output_size)
        || input.iter().any(|value| !value.is_finite())
    {
        return Err(format!(
            "deepseek_v4_reference_matvec_shape_invalid:out={output_size}:in={input_size}:row={row_start}+{row_count}:input={}",
            input.len()
        ));
    }
    Ok(())
}

pub fn fp8_matvec_rows_reference(
    weight: &[u8],
    scales: &[u8],
    output_size: usize,
    input_size: usize,
    row_start: usize,
    row_count: usize,
    input: &[f32],
) -> Result<Vec<f32>, String> {
    validate_matrix_inputs(output_size, input_size, row_start, row_count, input)?;
    if !input_size.is_multiple_of(FP8_ACTIVATION_BLOCK_SIZE)
        || weight.len() != row_count * input_size
    {
        return Err("deepseek_v4_reference_fp8_weight_shape_invalid".to_string());
    }
    let scale_rows = output_size.div_ceil(FP8_WEIGHT_BLOCK_SIZE);
    let scale_columns = input_size.div_ceil(FP8_WEIGHT_BLOCK_SIZE);
    if scales.len() != scale_rows * scale_columns {
        return Err(format!(
            "deepseek_v4_reference_fp8_scale_shape_invalid:actual={}:expected={}",
            scales.len(),
            scale_rows * scale_columns
        ));
    }
    let activation = quantize_dynamic_fp8_reference(input, FP8_ACTIVATION_BLOCK_SIZE)?;
    let mut output = Vec::with_capacity(row_count);
    for local_row in 0..row_count {
        let global_row = row_start + local_row;
        let row = &weight[local_row * input_size..(local_row + 1) * input_size];
        let mut accumulator = 0.0f32;
        for tile_start in (0..input_size).step_by(FP8_WEIGHT_BLOCK_SIZE) {
            let block = tile_start / FP8_WEIGHT_BLOCK_SIZE;
            let activation_scale = f64::from(decode_ue8m0(activation.scales[block])?);
            let weight_scale = f64::from(decode_ue8m0(
                scales[(global_row / FP8_WEIGHT_BLOCK_SIZE) * scale_columns + block],
            )?);
            let mut partial = 0.0f64;
            for index in tile_start..tile_start + FP8_WEIGHT_BLOCK_SIZE {
                partial += f64::from(decode_fp8_e4m3(activation.values[index])?)
                    * f64::from(decode_fp8_e4m3(row[index])?)
                    * activation_scale
                    * weight_scale;
            }
            accumulator = (f64::from(accumulator) + partial) as f32;
        }
        output.push(round_to_bf16(accumulator));
    }
    Ok(output)
}

pub fn fp4_matvec_rows_reference(
    packed_weight: &[u8],
    scales: &[u8],
    output_size: usize,
    input_size: usize,
    row_start: usize,
    row_count: usize,
    input: &[f32],
) -> Result<Vec<f32>, String> {
    validate_matrix_inputs(output_size, input_size, row_start, row_count, input)?;
    if !input_size.is_multiple_of(FP8_ACTIVATION_BLOCK_SIZE)
        || !input_size.is_multiple_of(2)
        || packed_weight.len() != row_count * (input_size / 2)
    {
        return Err("deepseek_v4_reference_fp4_weight_shape_invalid".to_string());
    }
    let scale_columns = input_size.div_ceil(FP4_WEIGHT_BLOCK_SIZE);
    if scales.len() != output_size * scale_columns {
        return Err(format!(
            "deepseek_v4_reference_fp4_scale_shape_invalid:actual={}:expected={}",
            scales.len(),
            output_size * scale_columns
        ));
    }
    let activation = quantize_dynamic_fp8_reference(input, FP8_ACTIVATION_BLOCK_SIZE)?;
    let packed_input = input_size / 2;
    let mut output = Vec::with_capacity(row_count);
    for local_row in 0..row_count {
        let global_row = row_start + local_row;
        let row = &packed_weight[local_row * packed_input..(local_row + 1) * packed_input];
        let mut accumulator = 0.0f32;
        for tile_start in (0..input_size).step_by(FP8_ACTIVATION_BLOCK_SIZE) {
            let activation_scale = f64::from(decode_ue8m0(
                activation.scales[tile_start / FP8_ACTIVATION_BLOCK_SIZE],
            )?);
            let mut partial = 0.0f64;
            for index in tile_start..tile_start + FP8_ACTIVATION_BLOCK_SIZE {
                let block = index / FP4_WEIGHT_BLOCK_SIZE;
                let byte = row[index / 2];
                let nibble = if index % 2 == 0 {
                    byte & 0x0f
                } else {
                    byte >> 4
                };
                let weight_scale =
                    f64::from(decode_ue8m0(scales[global_row * scale_columns + block])?);
                partial += f64::from(decode_fp8_e4m3(activation.values[index])?)
                    * f64::from(decode_fp4_e2m1(nibble)?)
                    * activation_scale
                    * weight_scale;
            }
            accumulator = (f64::from(accumulator) + partial) as f32;
        }
        output.push(round_to_bf16(accumulator));
    }
    Ok(output)
}

#[derive(Debug)]
struct ReferenceHcPre {
    normalized_hidden: Vec<f32>,
    split: DeepseekV4FlashHcSplit,
}

#[derive(Debug)]
struct ReferenceAttentionOutput {
    output: Vec<f32>,
    kv: Vec<f32>,
    q_lora: Vec<f32>,
    query: Vec<f32>,
    attended: Vec<f32>,
    low_rank_output: Vec<f32>,
}

#[derive(Debug)]
struct ReferenceFfnOutput {
    values: Vec<f32>,
    expert_indices: Vec<usize>,
    expert_weights: Vec<f32>,
}

fn bf16_boundary(mut values: Vec<f32>) -> Result<Vec<f32>, String> {
    round_slice_to_bf16(&mut values)?;
    Ok(values)
}

impl DeepseekV4Checkpoint {
    pub fn reference_embedding_hc(&self, token_id: u64) -> Result<Vec<f32>, String> {
        if token_id >= self.config.vocab_size {
            return Err(format!(
                "deepseek_v4_reference_embedding_token_out_of_range:{token_id}"
            ));
        }
        let hidden_size = usize::try_from(self.config.hidden_size)
            .map_err(|_| "deepseek_v4_reference_hidden_size_overflow".to_string())?;
        let hc_mult = usize::try_from(self.config.hc_mult)
            .map_err(|_| "deepseek_v4_reference_hc_mult_overflow".to_string())?;
        let element_offset = token_id
            .checked_mul(self.config.hidden_size)
            .ok_or_else(|| "deepseek_v4_reference_embedding_offset_overflow".to_string())?;
        let embedding = self
            .reference_decode_tensor("embed.weight", element_offset, self.config.hidden_size)?
            .values;
        if embedding.len() != hidden_size {
            return Err(format!(
                "deepseek_v4_reference_embedding_width_invalid:actual={}:expected={hidden_size}",
                embedding.len()
            ));
        }
        let mut hidden_hc = Vec::with_capacity(hidden_size * hc_mult);
        for _ in 0..hc_mult {
            hidden_hc.extend_from_slice(&embedding);
        }
        Ok(hidden_hc)
    }

    pub fn reference_output_head(
        &self,
        hidden_hc: &[f32],
        top_k: usize,
    ) -> Result<DeepseekV4ReferenceHeadOutput, String> {
        let hidden_size = usize::try_from(self.config.hidden_size)
            .map_err(|_| "deepseek_v4_reference_hidden_size_overflow".to_string())?;
        let hc_mult = usize::try_from(self.config.hc_mult)
            .map_err(|_| "deepseek_v4_reference_hc_mult_overflow".to_string())?;
        let vocab_size = usize::try_from(self.config.vocab_size)
            .map_err(|_| "deepseek_v4_reference_vocab_size_overflow".to_string())?;
        if hidden_hc.len() != hidden_size * hc_mult
            || hidden_hc.iter().any(|value| !value.is_finite())
        {
            return Err(format!(
                "deepseek_v4_reference_output_head_input_invalid:actual={}:expected={}",
                hidden_hc.len(),
                hidden_size * hc_mult
            ));
        }
        if top_k == 0 || top_k > vocab_size {
            return Err(format!(
                "deepseek_v4_reference_output_head_topk_invalid:{top_k}"
            ));
        }

        let control = deepseek_v4_flash_hc_control_input_reference(
            hidden_hc,
            hidden_size,
            hc_mult,
            self.config.rms_norm_eps as f32,
        )?;
        let mixes = self.reference_matvec_rows("hc_head_fn", &control, 0, hc_mult)?;
        let scale = self.reference_read_vector("hc_head_scale")?;
        let base = self.reference_read_vector("hc_head_base")?;
        if scale.len() != 1 || base.len() != hc_mult {
            return Err(format!(
                "deepseek_v4_reference_output_hc_control_invalid:scale={}:base={}:expected_base={hc_mult}",
                scale.len(),
                base.len()
            ));
        }
        let weights = mixes
            .iter()
            .zip(base)
            .map(|(value, base)| {
                stable_sigmoid(value * scale[0] + base) + self.config.hc_eps as f32
            })
            .collect::<Vec<_>>();
        let mut hc_head =
            deepseek_v4_flash_hc_weighted_sum_reference(hidden_hc, &weights, hidden_size)?;
        round_slice_to_bf16(&mut hc_head)?;
        let hc_head_checksum = checksum_f32(&hc_head);
        let norm = self.reference_read_vector("norm.weight")?;
        let mut normalized_hidden = deepseek_v4_flash_rms_norm_reference(
            &hc_head,
            Some(&norm),
            self.config.rms_norm_eps as f32,
        )?;
        round_slice_to_bf16(&mut normalized_hidden)?;
        let normalized_hidden_checksum = checksum_f32(&normalized_hidden);
        let logits =
            self.reference_matvec_rows("head.weight", &normalized_hidden, 0, vocab_size)?;
        let logits_checksum = checksum_f32(&logits);
        let top_k = top_k_logits(&logits, top_k)?;
        Ok(DeepseekV4ReferenceHeadOutput {
            hc_head_checksum,
            normalized_hidden_checksum,
            logits,
            logits_checksum,
            top_k,
        })
    }

    pub fn reference_decode_tensor(
        &self,
        name: &str,
        element_offset: u64,
        elements: u64,
    ) -> Result<DeepseekV4DecodedTensor, String> {
        let metadata = self.tensor(name)?;
        if elements == 0 {
            return Err(format!("deepseek_v4_reference_tensor_empty:{name}"));
        }
        if metadata.dtype == DeepseekV4TensorDType::I64 {
            return Err(format!(
                "deepseek_v4_reference_tensor_numeric_decode_unsupported:{name}:I64"
            ));
        }
        let logical_shape = if metadata.dtype == DeepseekV4TensorDType::I8 {
            let mut shape = metadata.shape.clone();
            let last = shape
                .last_mut()
                .ok_or_else(|| format!("deepseek_v4_reference_fp4_shape_empty:{name}"))?;
            *last = last
                .checked_mul(2)
                .ok_or_else(|| format!("deepseek_v4_reference_fp4_shape_overflow:{name}"))?;
            shape
        } else {
            metadata.shape.clone()
        };
        let logical_elements = logical_shape.iter().try_fold(1u64, |total, dimension| {
            total
                .checked_mul(*dimension)
                .ok_or_else(|| format!("deepseek_v4_reference_tensor_elements_overflow:{name}"))
        })?;
        let logical_end = element_offset
            .checked_add(elements)
            .ok_or_else(|| format!("deepseek_v4_reference_tensor_range_overflow:{name}"))?;
        if logical_end > logical_elements {
            return Err(format!(
                "deepseek_v4_reference_tensor_range_oob:{name}:offset={element_offset}:elements={elements}:tensor_elements={logical_elements}"
            ));
        }
        let bytes_per_element = metadata.dtype.storage_bytes();
        let (byte_offset, bytes) = if metadata.dtype == DeepseekV4TensorDType::I8 {
            let first_byte = element_offset / 2;
            let last_byte = logical_end.div_ceil(2);
            (first_byte, last_byte - first_byte)
        } else {
            (
                element_offset
                    .checked_mul(bytes_per_element)
                    .ok_or_else(|| {
                        format!("deepseek_v4_reference_tensor_offset_overflow:{name}")
                    })?,
                elements
                    .checked_mul(bytes_per_element)
                    .ok_or_else(|| format!("deepseek_v4_reference_tensor_bytes_overflow:{name}"))?,
            )
        };
        let payload = self.read_tensor_slice(name, byte_offset, bytes)?;
        let mut values = match metadata.dtype {
            DeepseekV4TensorDType::Bf16 => payload
                .chunks_exact(2)
                .map(|chunk| bf16_to_f32(u16::from_le_bytes([chunk[0], chunk[1]])))
                .collect(),
            DeepseekV4TensorDType::F32 => payload
                .chunks_exact(4)
                .map(|chunk| f32::from_le_bytes([chunk[0], chunk[1], chunk[2], chunk[3]]))
                .collect(),
            DeepseekV4TensorDType::F8E4M3 => payload
                .iter()
                .map(|value| decode_fp8_e4m3(*value))
                .collect::<Result<Vec<_>, _>>()?,
            DeepseekV4TensorDType::F8E8M0 => payload
                .iter()
                .map(|value| decode_ue8m0(*value))
                .collect::<Result<Vec<_>, _>>()?,
            DeepseekV4TensorDType::I8 => {
                let mut decoded = Vec::with_capacity(payload.len() * 2);
                for byte in payload.iter().copied() {
                    decoded.push(decode_fp4_e2m1(byte & 0x0f)?);
                    decoded.push(decode_fp4_e2m1(byte >> 4)?);
                }
                decoded
            }
            DeepseekV4TensorDType::I64 => unreachable!(),
        };
        if metadata.dtype == DeepseekV4TensorDType::I8 {
            let skip = usize::try_from(element_offset % 2).unwrap();
            let count = usize::try_from(elements)
                .map_err(|_| format!("deepseek_v4_reference_tensor_count_overflow:{name}"))?;
            values = values[skip..skip + count].to_vec();
        }
        Ok(DeepseekV4DecodedTensor {
            name: name.to_string(),
            dtype: metadata.dtype.safetensors_name().to_string(),
            shape: metadata.shape.clone(),
            logical_shape,
            element_offset,
            values_checksum: checksum_f32(&values),
            values,
        })
    }

    pub fn reference_read_vector(&self, name: &str) -> Result<Vec<f32>, String> {
        let metadata = self.tensor(name)?;
        if metadata.shape.len() != 1
            || !matches!(
                metadata.dtype,
                DeepseekV4TensorDType::Bf16 | DeepseekV4TensorDType::F32
            )
        {
            return Err(format!(
                "deepseek_v4_reference_vector_contract_invalid:{name}:dtype={}:shape={:?}",
                metadata.dtype.safetensors_name(),
                metadata.shape
            ));
        }
        self.reference_decode_tensor(name, 0, metadata.shape[0])
            .map(|decoded| decoded.values)
    }

    pub fn reference_matvec_rows(
        &self,
        name: &str,
        input: &[f32],
        row_start: usize,
        row_count: usize,
    ) -> Result<Vec<f32>, String> {
        let metadata = self.tensor(name)?;
        if metadata.shape.len() != 2 {
            return Err(format!(
                "deepseek_v4_reference_matrix_rank_invalid:{name}:shape={:?}",
                metadata.shape
            ));
        }
        let output_size = usize::try_from(metadata.shape[0])
            .map_err(|_| format!("deepseek_v4_reference_matrix_output_overflow:{name}"))?;
        let stored_input = usize::try_from(metadata.shape[1])
            .map_err(|_| format!("deepseek_v4_reference_matrix_input_overflow:{name}"))?;
        let input_size = if metadata.dtype == DeepseekV4TensorDType::I8 {
            stored_input
                .checked_mul(2)
                .ok_or_else(|| format!("deepseek_v4_reference_fp4_input_overflow:{name}"))?
        } else {
            stored_input
        };
        validate_matrix_inputs(output_size, input_size, row_start, row_count, input)?;

        let row_bytes = stored_input
            .checked_mul(usize::try_from(metadata.dtype.storage_bytes()).unwrap())
            .ok_or_else(|| format!("deepseek_v4_reference_matrix_row_bytes_overflow:{name}"))?;
        let byte_offset = row_start
            .checked_mul(row_bytes)
            .ok_or_else(|| format!("deepseek_v4_reference_matrix_offset_overflow:{name}"))?;
        let byte_count = row_count
            .checked_mul(row_bytes)
            .ok_or_else(|| format!("deepseek_v4_reference_matrix_bytes_overflow:{name}"))?;
        let weight = self.read_tensor_slice(name, byte_offset as u64, byte_count as u64)?;

        match metadata.dtype {
            DeepseekV4TensorDType::F8E4M3 | DeepseekV4TensorDType::I8 => {
                let scale_name = metadata
                    .scale_tensor
                    .as_deref()
                    .ok_or_else(|| format!("deepseek_v4_reference_matrix_scale_missing:{name}"))?;
                let scale_metadata = self.tensor(scale_name)?;
                if scale_metadata.dtype != DeepseekV4TensorDType::F8E8M0 {
                    return Err(format!(
                        "deepseek_v4_reference_matrix_scale_dtype_invalid:{scale_name}:{}",
                        scale_metadata.dtype.safetensors_name()
                    ));
                }
                let scales =
                    self.read_tensor_slice(scale_name, 0, scale_metadata.payload_bytes())?;
                if metadata.dtype == DeepseekV4TensorDType::F8E4M3 {
                    fp8_matvec_rows_reference(
                        &weight,
                        &scales,
                        output_size,
                        input_size,
                        row_start,
                        row_count,
                        input,
                    )
                } else {
                    fp4_matvec_rows_reference(
                        &weight,
                        &scales,
                        output_size,
                        input_size,
                        row_start,
                        row_count,
                        input,
                    )
                }
            }
            DeepseekV4TensorDType::Bf16 | DeepseekV4TensorDType::F32 => {
                let mut output = Vec::with_capacity(row_count);
                for row in 0..row_count {
                    let mut accumulator = 0.0f32;
                    for block_start in (0..input_size).step_by(128) {
                        let block_end = (block_start + 128).min(input_size);
                        let mut block_sum = 0.0f64;
                        for (column, input_value) in input
                            .iter()
                            .copied()
                            .enumerate()
                            .take(block_end)
                            .skip(block_start)
                        {
                            let index = row * row_bytes
                                + column * usize::try_from(metadata.dtype.storage_bytes()).unwrap();
                            let value = if metadata.dtype == DeepseekV4TensorDType::Bf16 {
                                bf16_to_f32(u16::from_le_bytes([weight[index], weight[index + 1]]))
                            } else {
                                f32::from_le_bytes([
                                    weight[index],
                                    weight[index + 1],
                                    weight[index + 2],
                                    weight[index + 3],
                                ])
                            };
                            block_sum += f64::from(input_value * value);
                        }
                        accumulator = (f64::from(accumulator) + block_sum) as f32;
                    }
                    if !accumulator.is_finite() {
                        return Err(format!(
                            "deepseek_v4_reference_matvec_non_finite:{name}:row={}",
                            row_start + row
                        ));
                    }
                    output.push(accumulator);
                }
                Ok(output)
            }
            other => Err(format!(
                "deepseek_v4_reference_matrix_dtype_unsupported:{name}:{}",
                other.safetensors_name()
            )),
        }
    }

    fn reference_hc_pre(
        &self,
        prefix: &str,
        kind: &str,
        residual_hc: &[f32],
    ) -> Result<ReferenceHcPre, String> {
        let hidden_size = usize::try_from(self.config.hidden_size)
            .map_err(|_| "deepseek_v4_reference_hidden_size_overflow".to_string())?;
        let hc_mult = usize::try_from(self.config.hc_mult)
            .map_err(|_| "deepseek_v4_reference_hc_mult_overflow".to_string())?;
        let control = deepseek_v4_flash_hc_control_input_reference(
            residual_hc,
            hidden_size,
            hc_mult,
            DEEPSEEK_V4_FLASH_RMS_EPS,
        )?;
        let function_name = format!("{prefix}.hc_{kind}_fn");
        let mix_hc = (2 + hc_mult) * hc_mult;
        let mix = self.reference_matvec_rows(&function_name, &control, 0, mix_hc)?;
        let scale = self.reference_read_vector(&format!("{prefix}.hc_{kind}_scale"))?;
        let base = self.reference_read_vector(&format!("{prefix}.hc_{kind}_base"))?;
        let split = deepseek_v4_flash_hc_split_reference(
            &mix,
            &scale,
            &base,
            hc_mult,
            usize::try_from(self.config.hc_sinkhorn_iters)
                .map_err(|_| "deepseek_v4_reference_sinkhorn_iters_overflow".to_string())?,
            self.config.hc_eps as f32,
        )?;
        let mixed = bf16_boundary(deepseek_v4_flash_hc_weighted_sum_reference(
            residual_hc,
            &split.pre,
            hidden_size,
        )?)?;
        let norm = self.reference_read_vector(&format!("{prefix}.{kind}_norm.weight"))?;
        let normalized_hidden = bf16_boundary(deepseek_v4_flash_rms_norm_reference(
            &mixed,
            Some(&norm),
            self.config.rms_norm_eps as f32,
        )?)?;
        Ok(ReferenceHcPre {
            normalized_hidden,
            split,
        })
    }

    fn reference_attention(
        &self,
        prefix: &str,
        layer_id: u64,
        position: u32,
        hidden: &[f32],
    ) -> Result<ReferenceAttentionOutput, String> {
        let heads = usize::try_from(self.config.num_attention_heads)
            .map_err(|_| "deepseek_v4_reference_attention_heads_overflow".to_string())?;
        let head_dim = usize::try_from(self.config.head_dim)
            .map_err(|_| "deepseek_v4_reference_head_dim_overflow".to_string())?;
        let rope_dim = usize::try_from(self.config.qk_rope_head_dim)
            .map_err(|_| "deepseek_v4_reference_rope_dim_overflow".to_string())?;
        let q_rank = usize::try_from(self.config.q_lora_rank)
            .map_err(|_| "deepseek_v4_reference_q_rank_overflow".to_string())?;
        let output_groups = usize::try_from(self.config.o_groups)
            .map_err(|_| "deepseek_v4_reference_output_groups_overflow".to_string())?;
        let output_rank = usize::try_from(self.config.o_lora_rank)
            .map_err(|_| "deepseek_v4_reference_output_rank_overflow".to_string())?;
        let hidden_size = usize::try_from(self.config.hidden_size)
            .map_err(|_| "deepseek_v4_reference_hidden_size_overflow".to_string())?;

        let q_lora =
            self.reference_matvec_rows(&format!("{prefix}.attn.wq_a.weight"), hidden, 0, q_rank)?;
        let q_norm_weight = self.reference_read_vector(&format!("{prefix}.attn.q_norm.weight"))?;
        let q_lora = bf16_boundary(deepseek_v4_flash_rms_norm_reference(
            &q_lora,
            Some(&q_norm_weight),
            self.config.rms_norm_eps as f32,
        )?)?;
        let q = self.reference_matvec_rows(
            &format!("{prefix}.attn.wq_b.weight"),
            &q_lora,
            0,
            heads * head_dim,
        )?;
        let q = bf16_boundary(deepseek_v4_flash_head_rms_norm_reference(
            &q,
            heads,
            head_dim,
            None,
            self.config.rms_norm_eps as f32,
        )?)?;
        let rope = deepseek_v4_flash_rope_coefficients(layer_id, position)?;
        let q = bf16_boundary(deepseek_v4_flash_rope_tail_reference(
            &q, heads, head_dim, rope_dim, &rope.cos, &rope.sin, false,
        )?)?;

        let kv =
            self.reference_matvec_rows(&format!("{prefix}.attn.wkv.weight"), hidden, 0, head_dim)?;
        let kv_norm_weight =
            self.reference_read_vector(&format!("{prefix}.attn.kv_norm.weight"))?;
        let kv = bf16_boundary(deepseek_v4_flash_rms_norm_reference(
            &kv,
            Some(&kv_norm_weight),
            self.config.rms_norm_eps as f32,
        )?)?;
        let mut kv = bf16_boundary(deepseek_v4_flash_rope_tail_reference(
            &kv, 1, head_dim, rope_dim, &rope.cos, &rope.sin, false,
        )?)?;
        let nope_dim = head_dim - rope_dim;
        let quantized_nope = quantize_dynamic_fp8_reference(&kv[..nope_dim], 64)?;
        let dequantized_nope = dequantize_dynamic_fp8_reference(&quantized_nope)?;
        kv[..nope_dim].copy_from_slice(&dequantized_nope);
        round_slice_to_bf16(&mut kv)?;

        let sinks = self.reference_read_vector(&format!("{prefix}.attn.attn_sink"))?;
        let attended = bf16_boundary(deepseek_v4_flash_sink_attention_reference(
            &q, &kv, &sinks, heads, head_dim,
        )?)?;
        let attended = bf16_boundary(deepseek_v4_flash_rope_tail_reference(
            &attended, heads, head_dim, rope_dim, &rope.cos, &rope.sin, true,
        )?)?;

        if heads % output_groups != 0 {
            return Err("deepseek_v4_reference_output_group_geometry_invalid".to_string());
        }
        let group_input = heads / output_groups * head_dim;
        let mut low_rank_output = Vec::with_capacity(output_groups * output_rank);
        for group in 0..output_groups {
            let input = &attended[group * group_input..(group + 1) * group_input];
            low_rank_output.extend(self.reference_matvec_rows(
                &format!("{prefix}.attn.wo_a.weight"),
                input,
                group * output_rank,
                output_rank,
            )?);
        }
        let output = self.reference_matvec_rows(
            &format!("{prefix}.attn.wo_b.weight"),
            &low_rank_output,
            0,
            hidden_size,
        )?;
        Ok(ReferenceAttentionOutput {
            output,
            kv,
            q_lora,
            query: q,
            attended,
            low_rank_output,
        })
    }

    fn reference_compressor_pending(
        &self,
        prefix: &str,
        hidden: &[f32],
        head_dim: usize,
        overlap: bool,
    ) -> Result<Vec<f32>, String> {
        let coefficient = 1 + usize::from(overlap);
        let width = coefficient
            .checked_mul(head_dim)
            .ok_or_else(|| "deepseek_v4_reference_compressor_width_overflow".to_string())?;
        let kv = self.reference_matvec_rows(&format!("{prefix}.wkv.weight"), hidden, 0, width)?;
        let mut score =
            self.reference_matvec_rows(&format!("{prefix}.wgate.weight"), hidden, 0, width)?;
        let ape = self
            .reference_decode_tensor(&format!("{prefix}.ape"), 0, width as u64)?
            .values;
        for (score, ape) in score.iter_mut().zip(ape) {
            *score += ape;
        }
        let mut pending = Vec::with_capacity(width * 2);
        pending.extend(kv);
        pending.extend(score);
        Ok(pending)
    }

    fn reference_expert(
        &self,
        prefix: &str,
        input: &[f32],
        route_weight: f32,
    ) -> Result<Vec<f32>, String> {
        if !route_weight.is_finite() || route_weight <= 0.0 {
            return Err(format!(
                "deepseek_v4_reference_route_weight_invalid:{route_weight}"
            ));
        }
        let intermediate = usize::try_from(self.config.moe_intermediate_size)
            .map_err(|_| "deepseek_v4_reference_moe_intermediate_overflow".to_string())?;
        let hidden_size = usize::try_from(self.config.hidden_size)
            .map_err(|_| "deepseek_v4_reference_hidden_size_overflow".to_string())?;
        let gate =
            self.reference_matvec_rows(&format!("{prefix}.w1.weight"), input, 0, intermediate)?;
        let up =
            self.reference_matvec_rows(&format!("{prefix}.w3.weight"), input, 0, intermediate)?;
        let mut activated =
            deepseek_v4_flash_swiglu_reference(&gate, &up, self.config.swiglu_limit as f32)?;
        for value in &mut activated {
            *value *= route_weight;
        }
        let activated = bf16_boundary(activated)?;
        self.reference_matvec_rows(&format!("{prefix}.w2.weight"), &activated, 0, hidden_size)
    }

    fn reference_ffn(
        &self,
        prefix: &str,
        layer_id: u64,
        token_id: u64,
        input: &[f32],
    ) -> Result<ReferenceFfnOutput, String> {
        let expert_count = usize::try_from(self.config.n_routed_experts)
            .map_err(|_| "deepseek_v4_reference_expert_count_overflow".to_string())?;
        let router_logits = self.reference_matvec_rows(
            &format!("{prefix}.ffn.gate.weight"),
            input,
            0,
            expert_count,
        )?;
        let hash_route = if layer_id < self.config.num_hash_layers {
            Some(self.reference_hash_route(layer_id, token_id)?)
        } else {
            None
        };
        let bias = if hash_route.is_none() {
            Some(self.reference_read_vector(&format!("{prefix}.ffn.gate.bias"))?)
        } else {
            None
        };
        let route = deepseek_v4_flash_router_reference(
            &router_logits,
            bias.as_deref(),
            hash_route.as_deref(),
            usize::try_from(self.config.num_experts_per_tok)
                .map_err(|_| "deepseek_v4_reference_topk_overflow".to_string())?,
            self.config.routed_scaling_factor as f32,
        )?;
        let mut output =
            self.reference_expert(&format!("{prefix}.ffn.shared_experts"), input, 1.0)?;
        for (&expert, &weight) in route.expert_indices.iter().zip(&route.expert_weights) {
            let routed =
                self.reference_expert(&format!("{prefix}.ffn.experts.{expert}"), input, weight)?;
            for (value, routed) in output.iter_mut().zip(routed) {
                *value += routed;
            }
        }
        let output = bf16_boundary(output)?;
        Ok(ReferenceFfnOutput {
            values: output,
            expert_indices: route.expert_indices,
            expert_weights: route.expert_weights,
        })
    }

    fn reference_layer_tensor_metadata_checksum(&self, layer_id: u64) -> Result<String, String> {
        let mut bytes = Vec::new();
        for name in self.tensor_names_for_layer_range(layer_id, layer_id + 1)? {
            let tensor = self.tensor(name)?;
            bytes.extend_from_slice(name.as_bytes());
            bytes.push(0);
            bytes.extend_from_slice(tensor.shard.as_bytes());
            bytes.push(0);
            bytes.extend_from_slice(tensor.dtype.safetensors_name().as_bytes());
            bytes.push(0);
            for dimension in &tensor.shape {
                bytes.extend_from_slice(&dimension.to_le_bytes());
            }
            bytes.extend_from_slice(&tensor.data_offsets[0].to_le_bytes());
            bytes.extend_from_slice(&tensor.data_offsets[1].to_le_bytes());
        }
        Ok(format!("fnv1a64:{:016x}", checksum64(&bytes)))
    }

    pub fn reference_layer_forward(
        &self,
        layer_id: u64,
        token_id: u64,
        position: u32,
        hidden_hc: &[f32],
    ) -> Result<DeepseekV4ReferenceLayerOutput, String> {
        if layer_id >= self.config.num_hidden_layers || token_id >= self.config.vocab_size {
            return Err(format!(
                "deepseek_v4_reference_layer_request_invalid:layer={layer_id}:token={token_id}"
            ));
        }
        if position != 0 {
            return Err(format!(
                "deepseek_v4_reference_layer_requires_kv_fixture:position={position}"
            ));
        }
        let hidden_size = usize::try_from(self.config.hidden_size)
            .map_err(|_| "deepseek_v4_reference_hidden_size_overflow".to_string())?;
        let hc_mult = usize::try_from(self.config.hc_mult)
            .map_err(|_| "deepseek_v4_reference_hc_mult_overflow".to_string())?;
        if hidden_hc.len() != hidden_size * hc_mult
            || hidden_hc.iter().any(|value| !value.is_finite())
        {
            return Err(format!(
                "deepseek_v4_reference_hidden_fixture_invalid:actual={}:expected={}",
                hidden_hc.len(),
                hidden_size * hc_mult
            ));
        }
        let input_hidden_checksum = checksum_f32(hidden_hc);
        let tensor_metadata_checksum = self.reference_layer_tensor_metadata_checksum(layer_id)?;
        let prefix = format!("layers.{layer_id}");

        let attention_pre = self.reference_hc_pre(&prefix, "attn", hidden_hc)?;
        let attention = self.reference_attention(
            &prefix,
            layer_id,
            position,
            &attention_pre.normalized_hidden,
        )?;
        let attention_output = attention.output;
        let kv = attention.kv;
        let q_lora = attention.q_lora;
        let ratio = self.config.compress_ratios[layer_id as usize];
        let attention_compressor_pending = if ratio == 0 {
            None
        } else {
            Some(
                self.reference_compressor_pending(
                    &format!("{prefix}.attn.compressor"),
                    &attention_pre.normalized_hidden,
                    usize::try_from(self.config.head_dim)
                        .map_err(|_| "deepseek_v4_reference_head_dim_overflow".to_string())?,
                    ratio == 4,
                )?,
            )
        };
        let indexer_compressor_pending = if ratio == 4 {
            Some(
                self.reference_compressor_pending(
                    &format!("{prefix}.attn.indexer.compressor"),
                    &attention_pre.normalized_hidden,
                    usize::try_from(self.config.index_head_dim)
                        .map_err(|_| "deepseek_v4_reference_index_head_dim_overflow".to_string())?,
                    true,
                )?,
            )
        } else {
            None
        };
        let attention_compressor_pending_checksum =
            attention_compressor_pending.as_deref().map(checksum_f32);
        let indexer_compressor_pending_checksum =
            indexer_compressor_pending.as_deref().map(checksum_f32);
        let (indexer_query, indexer_weights) = if ratio == 4 {
            let index_heads = usize::try_from(self.config.index_n_heads)
                .map_err(|_| "deepseek_v4_reference_index_heads_overflow".to_string())?;
            let index_head_dim = usize::try_from(self.config.index_head_dim)
                .map_err(|_| "deepseek_v4_reference_index_head_dim_overflow".to_string())?;
            let mut query = bf16_boundary(self.reference_matvec_rows(
                &format!("{prefix}.attn.indexer.wq_b.weight"),
                &q_lora,
                0,
                index_heads * index_head_dim,
            )?)?;
            crate::deepseek_v4_flash_lowering::deepseek_v4_flash_indexer_qat_reference(&mut query)?;
            round_slice_to_bf16(&mut query)?;
            let mut weights = bf16_boundary(self.reference_matvec_rows(
                &format!("{prefix}.attn.indexer.weights_proj.weight"),
                &attention_pre.normalized_hidden,
                0,
                index_heads,
            )?)?;
            let weight_scale =
                (index_head_dim as f32).sqrt().recip() * (index_heads as f32).sqrt().recip();
            for weight in &mut weights {
                *weight *= weight_scale;
            }
            round_slice_to_bf16(&mut weights)?;
            (Some(query), Some(weights))
        } else {
            (None, None)
        };
        let indexer_query_checksum = indexer_query.as_deref().map(checksum_f32);
        let indexer_weights_checksum = indexer_weights.as_deref().map(checksum_f32);
        let attention_output_checksum = checksum_f32(&attention_output);
        let attention_hidden = bf16_boundary(deepseek_v4_flash_hc_post_reference(
            &attention_output,
            hidden_hc,
            &attention_pre.split.post,
            &attention_pre.split.combine,
        )?)?;

        let ffn_pre = self.reference_hc_pre(&prefix, "ffn", &attention_hidden)?;
        let ffn = self.reference_ffn(&prefix, layer_id, token_id, &ffn_pre.normalized_hidden)?;
        let layer_output_hidden = bf16_boundary(deepseek_v4_flash_hc_post_reference(
            &ffn.values,
            &attention_hidden,
            &ffn_pre.split.post,
            &ffn_pre.split.combine,
        )?)?;
        let layer_output_hidden_checksum = checksum_f32(&layer_output_hidden);
        let (tensor_cache, expert_cache) = self.cache_stats()?;
        Ok(DeepseekV4ReferenceLayerOutput {
            model_revision: self.identity.revision.clone(),
            config_checksum: format!("fnv1a64:{:016x}", self.identity.config_checksum),
            index_checksum: format!("fnv1a64:{:016x}", self.identity.index_checksum),
            layer_id,
            token_id,
            tensor_metadata_checksum,
            input_hidden_checksum,
            attention_output_checksum,
            attention_output,
            q_lora,
            query: attention.query,
            attended: attention.attended,
            low_rank_attention_output: attention.low_rank_output,
            selected_experts: ffn.expert_indices,
            route_weights: ffn.expert_weights,
            kv: DeepseekV4ReferenceKvSummary {
                raw_rows: 1,
                raw_row_checksum: checksum_f32(&kv),
                compressed_rows: usize::from(ratio != 0 && 1 >= ratio),
                indexer_rows: usize::from(ratio == 4 && 1 >= ratio),
                attention_compressor_pending_checksum,
                indexer_compressor_pending_checksum,
                attention_compressor_pending,
                indexer_compressor_pending,
                indexer_query_checksum,
                indexer_weights_checksum,
                indexer_query,
                indexer_weights,
            },
            raw_kv: kv,
            layer_output_hidden,
            layer_output_hidden_checksum,
            tensor_cache,
            expert_cache,
        })
    }

    fn reference_hash_route(&self, layer_id: u64, token_id: u64) -> Result<Vec<usize>, String> {
        let name = format!("layers.{layer_id}.ffn.gate.tid2eid");
        let metadata = self.tensor(&name)?;
        let top_k = usize::try_from(self.config.num_experts_per_tok)
            .map_err(|_| "deepseek_v4_reference_topk_overflow".to_string())?;
        if metadata.dtype != DeepseekV4TensorDType::I64
            || metadata.shape != [self.config.vocab_size, self.config.num_experts_per_tok]
            || token_id >= self.config.vocab_size
        {
            return Err(format!(
                "deepseek_v4_reference_hash_route_contract_invalid:{name}:token={token_id}"
            ));
        }
        let bytes = self.read_tensor_slice(&name, token_id * top_k as u64 * 8, top_k as u64 * 8)?;
        bytes
            .chunks_exact(8)
            .map(|chunk| {
                let expert = i64::from_le_bytes(chunk.try_into().unwrap());
                usize::try_from(expert)
                    .map_err(|_| format!("deepseek_v4_reference_hash_expert_invalid:{expert}"))
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fixed_fp8_fp4_and_ue8m0_patterns_decode() {
        assert_eq!(decode_fp8_e4m3(0x00).unwrap(), 0.0);
        assert_eq!(decode_fp8_e4m3(0x01).unwrap(), 2.0f32.powi(-9));
        assert_eq!(decode_fp8_e4m3(0x38).unwrap(), 1.0);
        assert_eq!(decode_fp8_e4m3(0x7e).unwrap(), 448.0);
        assert_eq!(decode_fp8_e4m3(0xfe).unwrap(), -448.0);
        assert!(decode_fp8_e4m3(0x7f).is_err());
        assert_eq!(decode_fp4_e2m1(0x00).unwrap(), 0.0);
        assert_eq!(decode_fp4_e2m1(0x07).unwrap(), 6.0);
        assert_eq!(decode_fp4_e2m1(0x0f).unwrap(), -6.0);
        assert_eq!(decode_ue8m0(127).unwrap(), 1.0);
        assert_eq!(decode_ue8m0(128).unwrap(), 2.0);
        assert!(decode_ue8m0(255).is_err());
    }

    #[test]
    fn fp8_encoding_rounds_and_saturates() {
        for bits in [0x00, 0x01, 0x38, 0x3c, 0x7e, 0x80, 0xb8, 0xfe] {
            let value = decode_fp8_e4m3(bits).unwrap();
            assert_eq!(encode_fp8_e4m3(value).unwrap(), bits);
        }
        assert_eq!(encode_fp8_e4m3(1.0625).unwrap(), 0x38);
        assert_eq!(encode_fp8_e4m3(10_000.0).unwrap(), 0x7e);
        assert!(encode_fp8_e4m3(f32::NAN).is_err());
    }

    #[test]
    fn dynamic_activation_uses_power_of_two_scales() {
        let mut input = vec![0.0; 128];
        input[0] = 448.0;
        input[1] = -224.0;
        let quantized = quantize_dynamic_fp8_reference(&input, 128).unwrap();
        assert_eq!(quantized.scales, vec![127]);
        assert_eq!(quantized.values[0], 0x7e);
        assert_eq!(quantized.values[1], 0xf6);
        let decoded = dequantize_dynamic_fp8_reference(&quantized).unwrap();
        assert_eq!(decoded[0], 448.0);
        assert_eq!(decoded[1], -224.0);
        assert!(quantize_dynamic_fp8_reference(&[0.0; 127], 128).is_err());
    }

    #[test]
    fn fp8_matvec_applies_128_by_128_scales() {
        let input = vec![1.0; 128];
        let weight = vec![encode_fp8_e4m3(1.0).unwrap(); 2 * 128];
        let scales = [127, 128];
        let output = fp8_matvec_rows_reference(&weight, &scales, 256, 128, 0, 2, &input).unwrap();
        assert_eq!(output, vec![128.0, 128.0]);

        let second =
            fp8_matvec_rows_reference(&weight[..128], &scales, 256, 128, 128, 1, &input).unwrap();
        assert_eq!(second, vec![256.0]);
    }

    #[test]
    fn fp4_matvec_uses_low_nibble_first_and_per_32_k_scale() {
        let input = vec![1.0; 128];
        let packed = vec![0x21; 64];
        let mut scales = vec![127; 4];
        scales[1] = 128;
        let output = fp4_matvec_rows_reference(&packed, &scales, 1, 128, 0, 1, &input).unwrap();
        assert_eq!(output, vec![120.0]);
    }

    #[test]
    fn bf16_rounding_is_ties_to_even() {
        assert_eq!(f32_to_bf16_rne(f32::from_bits(0x3f80_8000)), 0x3f80);
        assert_eq!(f32_to_bf16_rne(f32::from_bits(0x3f81_8000)), 0x3f82);
    }

    #[test]
    fn top_k_logits_is_stable_and_fail_closed() {
        let top = top_k_logits(&[1.0, 3.0, 3.0, -2.0], 3).unwrap();
        assert_eq!(
            top.iter().map(|entry| entry.token_id).collect::<Vec<_>>(),
            vec![1, 2, 0]
        );
        assert!(top_k_logits(&[1.0], 0).is_err());
        assert!(top_k_logits(&[f32::NAN], 1).is_err());
    }

    #[test]
    #[ignore = "requires SIM_DEEPSEEK_V4_FLASH_WEIGHTS_PATH official checkpoint"]
    fn official_tensor_and_ratio4_layer_fixture_is_stable() {
        let model = std::env::var("SIM_DEEPSEEK_V4_FLASH_WEIGHTS_PATH")
            .expect("SIM_DEEPSEEK_V4_FLASH_WEIGHTS_PATH");
        let checkpoint = DeepseekV4Checkpoint::open(
            model,
            crate::deepseek_v4_flash_checkpoint::DeepseekV4CacheLimits {
                tensor_bytes: 64 * 1024 * 1024,
                expert_bytes: 256 * 1024 * 1024,
            },
        )
        .expect("open official checkpoint");
        assert_eq!(checkpoint.identity.config_checksum, 0x1f21_a253_6706_f3b8);
        assert_eq!(checkpoint.identity.index_checksum, 0x9085_917e_69b6_8077);

        let fp8_input = deterministic_hidden_fixture(7, 4096);
        let fp8 = checkpoint
            .reference_matvec_rows("layers.0.attn.wkv.weight", &fp8_input, 0, 8)
            .expect("official FP8 projection");
        assert_eq!(checksum_f32(&fp8), "fnv1a64:3f29ddf3d0033c55");
        let fp4 = checkpoint
            .reference_matvec_rows("layers.0.ffn.experts.17.w1.weight", &fp8_input, 0, 8)
            .expect("official FP4 projection");
        assert_eq!(checksum_f32(&fp4), "fnv1a64:8d1e67e5155c7a4c");

        let hidden = deterministic_hidden_fixture(7, 4 * 4096);
        let layer = checkpoint
            .reference_layer_forward(2, 1, 0, &hidden)
            .expect("official ratio-4 layer");
        assert_eq!(layer.selected_experts, vec![217, 221, 240, 26, 247, 39]);
        assert_eq!(layer.attention_output_checksum, "fnv1a64:a2b0cc4eba8a7035");
        assert_eq!(
            layer.kv.attention_compressor_pending_checksum.as_deref(),
            Some("fnv1a64:a37c315a79772c51")
        );
        assert_eq!(
            layer.kv.indexer_compressor_pending_checksum.as_deref(),
            Some("fnv1a64:3b93346b272846be")
        );
        assert_eq!(
            layer.kv.indexer_query_checksum.as_deref(),
            Some("fnv1a64:867b089f871b9188")
        );
        assert_eq!(
            layer.kv.indexer_weights_checksum.as_deref(),
            Some("fnv1a64:088d75e4a51846fa")
        );
        assert_eq!(
            layer.layer_output_hidden_checksum,
            "fnv1a64:98d13306567c6696"
        );
    }
}
