//! DeepSeek V4 Flash decode lowering for host-backed operator execution.
//!
//! The model semantics mirror both `vendor/pypto-lib/models/deepseek/v4` and
//! the DS4 C reference. Backends consume this plan and execute its operator
//! groups; the plan itself does not hide a Python or DS4 runtime invocation.

use crate::deepseek_v4_flash::{deepseek_v4_flash_layer_compress_ratio, DEEPSEEK_V4_FLASH_PROFILE};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeepseekV4FlashAttentionKind {
    SlidingWindow,
    CompressedSparseRatio4,
    HeavilyCompressedRatio128,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeepseekV4FlashCompressedSelection {
    None,
    LearnedIndexer,
    DeterministicPrefix,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeepseekV4FlashOp {
    HyperConnectionPreAttention,
    AttentionNorm,
    QueryLowRankProjection,
    QueryLowRankNorm,
    QueryProjection,
    KeyValueProjection,
    RotaryEmbedding,
    RawKvCacheAppend,
    Ratio4Compressor,
    Ratio128Compressor,
    SparseAttentionIndexer,
    SlidingWindowAttention,
    CompressedSparseAttention,
    HeavilyCompressedAttention,
    GroupedAttentionOutputProjection,
    HyperConnectionPostAttention,
    HyperConnectionPreFfn,
    FfnNormAndActivationQuantize,
    ExpertRouter,
    ExpertDispatch,
    SharedExpertSwiGlu,
    RoutedExpertSwiGlu,
    ExpertCombine,
    HyperConnectionPostFfn,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeepseekV4FlashDtype {
    F32,
    Bf16,
    I32,
    I8,
    Fp4,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TensorContract {
    pub dimensions: Vec<u32>,
    pub dtype: DeepseekV4FlashDtype,
}

impl TensorContract {
    pub fn new(dimensions: impl Into<Vec<u32>>, dtype: DeepseekV4FlashDtype) -> Self {
        Self {
            dimensions: dimensions.into(),
            dtype,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeepseekV4FlashLayerPlan {
    pub layer_id: u32,
    pub attention_kind: DeepseekV4FlashAttentionKind,
    pub compress_ratio: u32,
    pub hidden_hc: TensorContract,
    pub raw_kv_row: TensorContract,
    pub routed_expert_input: TensorContract,
    pub routed_expert_intermediate: TensorContract,
    pub routed_expert_output: TensorContract,
    pub route_indices: TensorContract,
    pub route_weights: TensorContract,
    pub operations: Vec<DeepseekV4FlashOp>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeepseekV4FlashQkvProjectionPlan {
    pub normalized_hidden: TensorContract,
    pub q_lora_weight: TensorContract,
    pub q_lora: TensorContract,
    pub q_lora_quantized: TensorContract,
    pub q_lora_scale: TensorContract,
    pub q_projection_weight: TensorContract,
    pub q: TensorContract,
    pub kv_projection_weight: TensorContract,
    pub kv: TensorContract,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeepseekV4FlashGemmKind {
    HyperConnectionControl,
    QueryLowRank,
    QueryExpansion,
    KeyValue,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeepseekV4FlashGemmPlan {
    pub kind: DeepseekV4FlashGemmKind,
    pub logical_m: u32,
    pub artifact_m: u32,
    pub k: u32,
    pub n: u32,
    pub input_dtype: DeepseekV4FlashDtype,
    pub weight_dtype: DeepseekV4FlashDtype,
    pub output_dtype: DeepseekV4FlashDtype,
}

impl DeepseekV4FlashGemmPlan {
    pub fn hc_control(token_count: u32) -> Result<Self, String> {
        if token_count == 0 {
            return Err("deepseek GEMM token count must be non-zero".to_string());
        }
        let p = DEEPSEEK_V4_FLASH_PROFILE;
        let artifact_m = token_count
            .checked_add(127)
            .ok_or_else(|| "deepseek GEMM M padding overflow".to_string())?
            / 128
            * 128;
        Ok(Self {
            kind: DeepseekV4FlashGemmKind::HyperConnectionControl,
            logical_m: token_count,
            artifact_m,
            k: (p.hidden_size * p.hc_mult) as u32,
            n: 128,
            input_dtype: DeepseekV4FlashDtype::F32,
            weight_dtype: DeepseekV4FlashDtype::F32,
            output_dtype: DeepseekV4FlashDtype::F32,
        })
    }

    pub fn qkv_decode(token_count: u32) -> Result<Vec<Self>, String> {
        if token_count == 0 {
            return Err("deepseek GEMM token count must be non-zero".to_string());
        }
        let p = DEEPSEEK_V4_FLASH_PROFILE;
        let artifact_m = token_count
            .checked_add(127)
            .ok_or_else(|| "deepseek GEMM M padding overflow".to_string())?
            / 128
            * 128;
        Ok(vec![
            Self {
                kind: DeepseekV4FlashGemmKind::QueryLowRank,
                logical_m: token_count,
                artifact_m,
                k: p.hidden_size as u32,
                n: p.q_lora_rank as u32,
                input_dtype: DeepseekV4FlashDtype::Bf16,
                weight_dtype: DeepseekV4FlashDtype::Bf16,
                output_dtype: DeepseekV4FlashDtype::F32,
            },
            Self {
                kind: DeepseekV4FlashGemmKind::QueryExpansion,
                logical_m: token_count,
                artifact_m,
                k: p.q_lora_rank as u32,
                n: (p.num_attention_heads * p.head_dim) as u32,
                input_dtype: DeepseekV4FlashDtype::I8,
                weight_dtype: DeepseekV4FlashDtype::I8,
                output_dtype: DeepseekV4FlashDtype::I32,
            },
            Self {
                kind: DeepseekV4FlashGemmKind::KeyValue,
                logical_m: token_count,
                artifact_m,
                k: p.hidden_size as u32,
                n: p.head_dim as u32,
                input_dtype: DeepseekV4FlashDtype::Bf16,
                weight_dtype: DeepseekV4FlashDtype::Bf16,
                output_dtype: DeepseekV4FlashDtype::F32,
            },
        ])
    }

    pub fn supports_host_bf16_gemm(&self) -> bool {
        self.input_dtype == DeepseekV4FlashDtype::Bf16
            && self.weight_dtype == DeepseekV4FlashDtype::Bf16
            && self.output_dtype == DeepseekV4FlashDtype::F32
            && self.artifact_m % 128 == 0
            && self.k % 128 == 0
            && self.n % 128 == 0
    }

    pub fn supports_host_fp32_gemm(&self) -> bool {
        self.input_dtype == DeepseekV4FlashDtype::F32
            && self.weight_dtype == DeepseekV4FlashDtype::F32
            && self.output_dtype == DeepseekV4FlashDtype::F32
            && self.artifact_m % 128 == 0
            && self.k % 128 == 0
            && self.n % 128 == 0
    }

    pub fn supports_host_quantized_gemm(&self) -> bool {
        self.input_dtype == DeepseekV4FlashDtype::I8
            && self.weight_dtype == DeepseekV4FlashDtype::I8
            && self.output_dtype == DeepseekV4FlashDtype::I32
            && self.artifact_m % 128 == 0
            && self.k % 128 == 0
            && self.n % 128 == 0
    }
}

pub fn deepseek_v4_flash_dequantize_gemm_reference(
    accumulators: &[i32],
    rows: usize,
    columns: usize,
    row_scales: &[f32],
    column_scales: &[f32],
) -> Result<Vec<f32>, String> {
    let elements = rows
        .checked_mul(columns)
        .ok_or_else(|| "deepseek GEMM dequant shape overflow".to_string())?;
    if rows == 0
        || columns == 0
        || accumulators.len() != elements
        || row_scales.len() != rows
        || column_scales.len() != columns
    {
        return Err(format!(
            "deepseek GEMM dequant shape mismatch: accumulators={} rows={rows} columns={columns} row_scales={} column_scales={}",
            accumulators.len(),
            row_scales.len(),
            column_scales.len()
        ));
    }
    if row_scales
        .iter()
        .chain(column_scales)
        .any(|scale| !scale.is_finite())
    {
        return Err("deepseek GEMM dequant scale contains non-finite value".to_string());
    }

    Ok(accumulators
        .chunks_exact(columns)
        .zip(row_scales)
        .flat_map(|(row, row_scale)| {
            row.iter()
                .zip(column_scales)
                .map(move |(value, column_scale)| *value as f32 * row_scale * column_scale)
        })
        .collect())
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeepseekV4FlashAttentionCachePlan {
    pub attention_kind: DeepseekV4FlashAttentionKind,
    pub sequence_len: u32,
    pub raw_start: u32,
    pub raw_rows: u32,
    pub compress_ratio: u32,
    pub compressed_rows: u32,
    pub compressed_selection: DeepseekV4FlashCompressedSelection,
}

impl DeepseekV4FlashAttentionCachePlan {
    pub fn decode(layer_id: u32, sequence_len: u32) -> Result<Self, String> {
        if sequence_len == 0 {
            return Err("deepseek attention sequence length must be non-zero".to_string());
        }
        let compress_ratio = deepseek_v4_flash_layer_compress_ratio(u64::from(layer_id))
            .ok_or_else(|| format!("deepseek layer out of range: {layer_id}"))?;
        let profile = DEEPSEEK_V4_FLASH_PROFILE;
        let raw_rows = sequence_len.min(profile.sliding_window as u32);
        let raw_start = sequence_len - raw_rows;
        let (attention_kind, compressed_rows, compressed_selection) = match compress_ratio {
            0 => (
                DeepseekV4FlashAttentionKind::SlidingWindow,
                0,
                DeepseekV4FlashCompressedSelection::None,
            ),
            4 => (
                DeepseekV4FlashAttentionKind::CompressedSparseRatio4,
                (sequence_len / 4).min(profile.indexer_top_k as u32),
                DeepseekV4FlashCompressedSelection::LearnedIndexer,
            ),
            128 => (
                DeepseekV4FlashAttentionKind::HeavilyCompressedRatio128,
                (sequence_len / 128).min(profile.indexer_top_k as u32),
                DeepseekV4FlashCompressedSelection::DeterministicPrefix,
            ),
            other => return Err(format!("unsupported deepseek compression ratio: {other}")),
        };
        Ok(Self {
            attention_kind,
            sequence_len,
            raw_start,
            raw_rows,
            compress_ratio,
            compressed_rows,
            compressed_selection,
        })
    }
}

impl DeepseekV4FlashQkvProjectionPlan {
    pub fn decode(token_count: u32) -> Result<Self, String> {
        if token_count == 0 {
            return Err("deepseek QKV token count must be non-zero".to_string());
        }
        let p = DEEPSEEK_V4_FLASH_PROFILE;
        Ok(Self {
            normalized_hidden: TensorContract::new(
                [token_count, p.hidden_size as u32],
                DeepseekV4FlashDtype::Bf16,
            ),
            q_lora_weight: TensorContract::new(
                [p.hidden_size as u32, p.q_lora_rank as u32],
                DeepseekV4FlashDtype::Bf16,
            ),
            q_lora: TensorContract::new(
                [token_count, p.q_lora_rank as u32],
                DeepseekV4FlashDtype::F32,
            ),
            q_lora_quantized: TensorContract::new(
                [token_count, p.q_lora_rank as u32],
                DeepseekV4FlashDtype::I8,
            ),
            q_lora_scale: TensorContract::new([token_count, 1], DeepseekV4FlashDtype::F32),
            q_projection_weight: TensorContract::new(
                [
                    p.q_lora_rank as u32,
                    (p.num_attention_heads * p.head_dim) as u32,
                ],
                DeepseekV4FlashDtype::I8,
            ),
            q: TensorContract::new(
                [token_count, p.num_attention_heads as u32, p.head_dim as u32],
                DeepseekV4FlashDtype::Bf16,
            ),
            kv_projection_weight: TensorContract::new(
                [p.hidden_size as u32, p.head_dim as u32],
                DeepseekV4FlashDtype::Bf16,
            ),
            kv: TensorContract::new(
                [token_count, p.num_key_value_heads as u32, p.head_dim as u32],
                DeepseekV4FlashDtype::Bf16,
            ),
        })
    }
}

impl DeepseekV4FlashLayerPlan {
    pub fn decode(layer_id: u32, token_count: u32) -> Result<Self, String> {
        if token_count == 0 {
            return Err("deepseek decode token count must be non-zero".to_string());
        }
        let compress_ratio = deepseek_v4_flash_layer_compress_ratio(u64::from(layer_id))
            .ok_or_else(|| format!("deepseek layer out of range: {layer_id}"))?;
        let attention_kind = match compress_ratio {
            0 => DeepseekV4FlashAttentionKind::SlidingWindow,
            4 => DeepseekV4FlashAttentionKind::CompressedSparseRatio4,
            128 => DeepseekV4FlashAttentionKind::HeavilyCompressedRatio128,
            other => return Err(format!("unsupported deepseek compression ratio: {other}")),
        };

        let p = DEEPSEEK_V4_FLASH_PROFILE;
        let mut operations = vec![
            DeepseekV4FlashOp::HyperConnectionPreAttention,
            DeepseekV4FlashOp::AttentionNorm,
            DeepseekV4FlashOp::QueryLowRankProjection,
            DeepseekV4FlashOp::QueryLowRankNorm,
            DeepseekV4FlashOp::QueryProjection,
            DeepseekV4FlashOp::KeyValueProjection,
            DeepseekV4FlashOp::RotaryEmbedding,
            DeepseekV4FlashOp::RawKvCacheAppend,
        ];
        match attention_kind {
            DeepseekV4FlashAttentionKind::SlidingWindow => {
                operations.push(DeepseekV4FlashOp::SlidingWindowAttention);
            }
            DeepseekV4FlashAttentionKind::CompressedSparseRatio4 => {
                operations.extend([
                    DeepseekV4FlashOp::Ratio4Compressor,
                    DeepseekV4FlashOp::SparseAttentionIndexer,
                    DeepseekV4FlashOp::CompressedSparseAttention,
                ]);
            }
            DeepseekV4FlashAttentionKind::HeavilyCompressedRatio128 => {
                operations.extend([
                    DeepseekV4FlashOp::Ratio128Compressor,
                    DeepseekV4FlashOp::HeavilyCompressedAttention,
                ]);
            }
        }
        operations.extend([
            DeepseekV4FlashOp::GroupedAttentionOutputProjection,
            DeepseekV4FlashOp::HyperConnectionPostAttention,
            DeepseekV4FlashOp::HyperConnectionPreFfn,
            DeepseekV4FlashOp::FfnNormAndActivationQuantize,
            DeepseekV4FlashOp::ExpertRouter,
            DeepseekV4FlashOp::ExpertDispatch,
            DeepseekV4FlashOp::SharedExpertSwiGlu,
            DeepseekV4FlashOp::RoutedExpertSwiGlu,
            DeepseekV4FlashOp::ExpertCombine,
            DeepseekV4FlashOp::HyperConnectionPostFfn,
        ]);

        Ok(Self {
            layer_id,
            attention_kind,
            compress_ratio,
            hidden_hc: TensorContract::new(
                [token_count, p.hc_mult as u32, p.hidden_size as u32],
                DeepseekV4FlashDtype::F32,
            ),
            raw_kv_row: TensorContract::new(
                [token_count, p.num_key_value_heads as u32, p.head_dim as u32],
                DeepseekV4FlashDtype::Bf16,
            ),
            routed_expert_input: TensorContract::new(
                [token_count, p.hidden_size as u32],
                DeepseekV4FlashDtype::I8,
            ),
            routed_expert_intermediate: TensorContract::new(
                [token_count, p.moe_intermediate_size as u32],
                DeepseekV4FlashDtype::I8,
            ),
            routed_expert_output: TensorContract::new(
                [token_count, p.hidden_size as u32],
                DeepseekV4FlashDtype::Bf16,
            ),
            route_indices: TensorContract::new(
                [token_count, p.num_experts_used as u32],
                DeepseekV4FlashDtype::I32,
            ),
            route_weights: TensorContract::new(
                [token_count, p.num_experts_used as u32],
                DeepseekV4FlashDtype::F32,
            ),
            operations,
        })
    }

    pub fn validate(&self) -> Result<(), String> {
        let p = DEEPSEEK_V4_FLASH_PROFILE;
        let tokens = *self
            .hidden_hc
            .dimensions
            .first()
            .ok_or_else(|| "deepseek hidden tensor rank is zero".to_string())?;
        if self.hidden_hc
            != TensorContract::new(
                [tokens, p.hc_mult as u32, p.hidden_size as u32],
                DeepseekV4FlashDtype::F32,
            )
        {
            return Err("deepseek hyper-connection hidden contract mismatch".to_string());
        }
        if self.route_indices.dimensions != [tokens, p.num_experts_used as u32]
            || self.route_weights.dimensions != [tokens, p.num_experts_used as u32]
        {
            return Err("deepseek moe routing contract mismatch".to_string());
        }
        if !self
            .operations
            .contains(&DeepseekV4FlashOp::RawKvCacheAppend)
            || !self
                .operations
                .contains(&DeepseekV4FlashOp::RoutedExpertSwiGlu)
            || !self
                .operations
                .contains(&DeepseekV4FlashOp::HyperConnectionPostFfn)
        {
            return Err(
                "deepseek layer is missing stateful attention, moe, or HC output".to_string(),
            );
        }
        Ok(())
    }
}

pub fn deepseek_v4_flash_head_rms_norm_reference(
    values: &[f32],
    num_heads: usize,
    head_dim: usize,
    weight: Option<&[f32]>,
    eps: f32,
) -> Result<Vec<f32>, String> {
    let expected = num_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "deepseek head RMSNorm shape overflow".to_string())?;
    if num_heads == 0 || head_dim == 0 || values.len() != expected {
        return Err(format!(
            "deepseek head RMSNorm shape mismatch: values={} expected={expected}",
            values.len()
        ));
    }
    if let Some(weight) = weight {
        if weight.len() != head_dim {
            return Err(format!(
                "deepseek head RMSNorm weight mismatch: weight={} head_dim={head_dim}",
                weight.len()
            ));
        }
    }

    let mut output = Vec::with_capacity(values.len());
    for head in values.chunks_exact(head_dim) {
        output.extend(deepseek_v4_flash_rms_norm_reference(head, weight, eps)?);
    }
    Ok(output)
}

pub fn deepseek_v4_flash_rope_tail_reference(
    values: &[f32],
    num_heads: usize,
    head_dim: usize,
    rope_dim: usize,
    cos: &[f32],
    sin: &[f32],
    inverse: bool,
) -> Result<Vec<f32>, String> {
    let expected = num_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "deepseek RoPE shape overflow".to_string())?;
    if num_heads == 0
        || head_dim == 0
        || rope_dim == 0
        || rope_dim > head_dim
        || rope_dim % 2 != 0
        || values.len() != expected
    {
        return Err(format!(
            "deepseek RoPE shape mismatch: values={} heads={num_heads} head_dim={head_dim} rope_dim={rope_dim}",
            values.len()
        ));
    }
    if cos.len() != rope_dim / 2 || sin.len() != rope_dim / 2 {
        return Err(format!(
            "deepseek RoPE table mismatch: cos={} sin={} expected={}",
            cos.len(),
            sin.len(),
            rope_dim / 2
        ));
    }
    if values
        .iter()
        .chain(cos)
        .chain(sin)
        .any(|value| !value.is_finite())
    {
        return Err("deepseek RoPE input contains non-finite value".to_string());
    }

    let mut output = values.to_vec();
    let tail_start = head_dim - rope_dim;
    let sin_sign = if inverse { -1.0 } else { 1.0 };
    for head in output.chunks_exact_mut(head_dim) {
        for pair in 0..rope_dim / 2 {
            let index = tail_start + pair * 2;
            let x0 = head[index];
            let x1 = head[index + 1];
            let c = cos[pair];
            let s = sin_sign * sin[pair];
            head[index] = x0 * c - x1 * s;
            head[index + 1] = x0 * s + x1 * c;
        }
    }
    Ok(output)
}

pub fn deepseek_v4_flash_write_kv_row_reference(
    cache: &mut [f32],
    head_dim: usize,
    slot: usize,
    kv: &[f32],
) -> Result<(), String> {
    if head_dim == 0 || kv.len() != head_dim || cache.len() % head_dim != 0 {
        return Err(format!(
            "deepseek KV write shape mismatch: cache={} kv={} head_dim={head_dim}",
            cache.len(),
            kv.len()
        ));
    }
    let start = slot
        .checked_mul(head_dim)
        .ok_or_else(|| "deepseek KV slot offset overflow".to_string())?;
    let end = start
        .checked_add(head_dim)
        .ok_or_else(|| "deepseek KV slot end overflow".to_string())?;
    let destination = cache
        .get_mut(start..end)
        .ok_or_else(|| format!("deepseek KV slot out of range: slot={slot}"))?;
    if kv.iter().any(|value| !value.is_finite()) {
        return Err("deepseek KV row contains non-finite value".to_string());
    }
    destination.copy_from_slice(kv);
    Ok(())
}

pub fn deepseek_v4_flash_gather_kv_rows_reference(
    cache: &[f32],
    head_dim: usize,
    row_indices: &[usize],
) -> Result<Vec<f32>, String> {
    if head_dim == 0 || cache.len() % head_dim != 0 {
        return Err(format!(
            "deepseek KV gather shape mismatch: cache={} head_dim={head_dim}",
            cache.len()
        ));
    }
    let mut output = Vec::with_capacity(
        row_indices
            .len()
            .checked_mul(head_dim)
            .ok_or_else(|| "deepseek KV gather size overflow".to_string())?,
    );
    for &row in row_indices {
        let start = row
            .checked_mul(head_dim)
            .ok_or_else(|| "deepseek KV row offset overflow".to_string())?;
        let end = start
            .checked_add(head_dim)
            .ok_or_else(|| "deepseek KV row end overflow".to_string())?;
        output.extend_from_slice(
            cache
                .get(start..end)
                .ok_or_else(|| format!("deepseek KV row out of range: row={row}"))?,
        );
    }
    Ok(output)
}

/// DS4-compatible attention over shared KV rows. The learned sink contributes
/// to the softmax denominator, but has no value row to add to the output.
pub fn deepseek_v4_flash_sink_attention_reference(
    q: &[f32],
    kv_rows: &[f32],
    sinks: &[f32],
    num_heads: usize,
    head_dim: usize,
) -> Result<Vec<f32>, String> {
    let q_len = num_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "deepseek attention Q shape overflow".to_string())?;
    if num_heads == 0
        || head_dim == 0
        || q.len() != q_len
        || sinks.len() != num_heads
        || kv_rows.is_empty()
        || kv_rows.len() % head_dim != 0
    {
        return Err(format!(
            "deepseek attention shape mismatch: q={} kv={} sinks={} heads={num_heads} head_dim={head_dim}",
            q.len(),
            kv_rows.len(),
            sinks.len()
        ));
    }
    if q.iter()
        .chain(kv_rows)
        .chain(sinks)
        .any(|value| !value.is_finite())
    {
        return Err("deepseek attention input contains non-finite value".to_string());
    }

    let row_count = kv_rows.len() / head_dim;
    let scale = (head_dim as f32).sqrt().recip();
    let mut output = vec![0.0f32; q_len];
    let mut scores = vec![0.0f32; row_count];
    for head in 0..num_heads {
        let q_head = &q[head * head_dim..(head + 1) * head_dim];
        let mut max_score = sinks[head];
        for (row, kv) in kv_rows.chunks_exact(head_dim).enumerate() {
            let score = q_head
                .iter()
                .zip(kv)
                .map(|(q_value, kv_value)| q_value * kv_value)
                .sum::<f32>()
                * scale;
            scores[row] = score;
            max_score = max_score.max(score);
        }

        let mut denominator = (sinks[head] - max_score).exp();
        let output_head = &mut output[head * head_dim..(head + 1) * head_dim];
        for (row, kv) in kv_rows.chunks_exact(head_dim).enumerate() {
            let weight = (scores[row] - max_score).exp();
            denominator += weight;
            for (output_value, kv_value) in output_head.iter_mut().zip(kv) {
                *output_value += weight * kv_value;
            }
        }
        let inverse_denominator = denominator.recip();
        for output_value in output_head {
            *output_value *= inverse_denominator;
        }
    }
    Ok(output)
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashCompressorState {
    head_dim: usize,
    compress_ratio: usize,
    width: usize,
    kv: Vec<f32>,
    score: Vec<f32>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashCompressorSnapshot {
    pub head_dim: usize,
    pub compress_ratio: usize,
    pub kv: Vec<f32>,
    pub score: Vec<f32>,
}

impl DeepseekV4FlashCompressorState {
    pub fn new(head_dim: usize, compress_ratio: usize) -> Result<Self, String> {
        if head_dim == 0 || !matches!(compress_ratio, 4 | 128) {
            return Err(format!(
                "deepseek compressor geometry mismatch: head_dim={head_dim} ratio={compress_ratio}"
            ));
        }
        let lanes = if compress_ratio == 4 { 2usize } else { 1usize };
        let width = lanes
            .checked_mul(head_dim)
            .ok_or_else(|| "deepseek compressor width overflow".to_string())?;
        let rows = compress_ratio
            .checked_mul(lanes)
            .ok_or_else(|| "deepseek compressor row count overflow".to_string())?;
        let state_len = rows
            .checked_mul(width)
            .ok_or_else(|| "deepseek compressor state size overflow".to_string())?;
        Ok(Self {
            head_dim,
            compress_ratio,
            width,
            kv: vec![0.0; state_len],
            score: vec![f32::NEG_INFINITY; state_len],
        })
    }

    pub fn width(&self) -> usize {
        self.width
    }

    pub fn head_dim(&self) -> usize {
        self.head_dim
    }

    pub fn compress_ratio(&self) -> usize {
        self.compress_ratio
    }

    pub fn snapshot(&self) -> DeepseekV4FlashCompressorSnapshot {
        DeepseekV4FlashCompressorSnapshot {
            head_dim: self.head_dim,
            compress_ratio: self.compress_ratio,
            kv: self.kv.clone(),
            score: self.score.clone(),
        }
    }

    pub fn restore(snapshot: DeepseekV4FlashCompressorSnapshot) -> Result<Self, String> {
        let mut state = Self::new(snapshot.head_dim, snapshot.compress_ratio)?;
        if snapshot.kv.len() != state.kv.len()
            || snapshot.score.len() != state.score.len()
            || snapshot.kv.iter().any(|value| !value.is_finite())
            || snapshot.score.iter().any(|value| value.is_nan())
        {
            return Err(format!(
                "deepseek compressor snapshot mismatch:kv={}:score={}:expected={}",
                snapshot.kv.len(),
                snapshot.score.len(),
                state.kv.len()
            ));
        }
        state.kv = snapshot.kv;
        state.score = snapshot.score;
        Ok(state)
    }

    pub fn update_projected(
        &mut self,
        position: u32,
        projected_kv: &[f32],
        projected_score: &[f32],
        ape: &[f32],
        norm_weight: &[f32],
        rope_dim: usize,
        cos: &[f32],
        sin: &[f32],
    ) -> Result<Option<Vec<f32>>, String> {
        if projected_kv.len() != self.width
            || projected_score.len() != self.width
            || ape.len() != self.width
            || norm_weight.len() != self.head_dim
            || projected_kv
                .iter()
                .chain(projected_score)
                .chain(ape)
                .any(|value| !value.is_finite())
        {
            return Err(format!(
                "deepseek compressor projected shape mismatch: kv={} score={} ape={} norm={} width={} head_dim={}",
                projected_kv.len(),
                projected_score.len(),
                ape.len(),
                norm_weight.len(),
                self.width,
                self.head_dim
            ));
        }

        let position_mod = position as usize % self.compress_ratio;
        let row = if self.compress_ratio == 4 {
            self.compress_ratio + position_mod
        } else {
            position_mod
        };
        let row_start = row * self.width;
        self.kv[row_start..row_start + self.width].copy_from_slice(projected_kv);
        for index in 0..self.width {
            self.score[row_start + index] = projected_score[index] + ape[index];
        }

        if (position as usize + 1) % self.compress_ratio != 0 {
            return Ok(None);
        }

        let pooled = self.pool()?;
        let mut output = deepseek_v4_flash_rms_norm_reference(&pooled, Some(norm_weight), 1.0e-6)?;
        output = deepseek_v4_flash_rope_tail_reference(
            &output,
            1,
            self.head_dim,
            rope_dim,
            cos,
            sin,
            false,
        )?;

        if self.head_dim == DEEPSEEK_V4_FLASH_PROFILE.head_dim as usize {
            deepseek_v4_flash_fp8_kv_roundtrip_reference(&mut output, rope_dim)?;
        } else if self.head_dim == DEEPSEEK_V4_FLASH_PROFILE.indexer_head_dim as usize {
            deepseek_v4_flash_indexer_qat_reference(&mut output)?;
        }

        if self.compress_ratio == 4 {
            self.advance_ratio4_window();
        }
        Ok(Some(output))
    }

    fn pool(&self) -> Result<Vec<f32>, String> {
        let mut output = vec![0.0f32; self.head_dim];
        for dimension in 0..self.head_dim {
            let mut pairs = Vec::with_capacity(if self.compress_ratio == 4 {
                self.compress_ratio * 2
            } else {
                self.compress_ratio
            });
            if self.compress_ratio == 4 {
                for row in 0..self.compress_ratio {
                    let index = row * self.width + dimension;
                    pairs.push((self.kv[index], self.score[index]));
                    let current_index =
                        (self.compress_ratio + row) * self.width + self.head_dim + dimension;
                    pairs.push((self.kv[current_index], self.score[current_index]));
                }
            } else {
                for row in 0..self.compress_ratio {
                    let index = row * self.width + dimension;
                    pairs.push((self.kv[index], self.score[index]));
                }
            }
            let max_score = pairs
                .iter()
                .map(|(_, score)| *score)
                .fold(f32::NEG_INFINITY, f32::max);
            if !max_score.is_finite() {
                continue;
            }
            let mut denominator = 0.0f32;
            let mut weighted_sum = 0.0f32;
            for (value, score) in pairs {
                let weight = (score - max_score).exp();
                denominator += weight;
                weighted_sum += value * weight;
            }
            if denominator <= 0.0 || !denominator.is_finite() {
                return Err("deepseek compressor softmax denominator is invalid".to_string());
            }
            output[dimension] = weighted_sum / denominator;
        }
        Ok(output)
    }

    fn advance_ratio4_window(&mut self) {
        for row in 0..self.compress_ratio {
            let previous = row * self.width;
            let current = (self.compress_ratio + row) * self.width;
            self.kv.copy_within(current..current + self.width, previous);
            self.score
                .copy_within(current..current + self.width, previous);
        }
        for row in 0..self.compress_ratio {
            let previous = row * self.width;
            let current = (self.compress_ratio + row) * self.width;
            self.kv
                .copy_within(previous..previous + self.width, current);
            self.score
                .copy_within(previous..previous + self.width, current);
        }
    }
}

pub fn deepseek_v4_flash_fp8_kv_roundtrip_reference(
    values: &mut [f32],
    rope_dim: usize,
) -> Result<(), String> {
    if rope_dim > values.len() || (values.len() - rope_dim) % 64 != 0 {
        return Err(format!(
            "deepseek FP8 KV shape mismatch: width={} rope_dim={rope_dim}",
            values.len()
        ));
    }
    let nope_dim = values.len() - rope_dim;
    for block in values[..nope_dim].chunks_exact_mut(64) {
        let amax = block
            .iter()
            .map(|value| value.abs())
            .fold(0.0f32, f32::max)
            .max(1.0e-4);
        let scale = 2.0f32.powf((amax / 448.0).log2().ceil());
        for value in block {
            *value =
                deepseek_v4_flash_e4m3fn_roundtrip((*value / scale).clamp(-448.0, 448.0)) * scale;
        }
    }
    Ok(())
}

fn deepseek_v4_flash_e4m3fn_roundtrip(value: f32) -> f32 {
    fn positive_value(index: usize) -> f32 {
        const EXP_SCALE: [f32; 16] = [
            0.0, 0.015625, 0.03125, 0.0625, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
            128.0, 256.0,
        ];
        let exponent = (index >> 3) & 0x0f;
        let mantissa = index & 0x07;
        if exponent == 0 {
            mantissa as f32 * 0.001953125
        } else {
            (1.0 + mantissa as f32 * 0.125) * EXP_SCALE[exponent]
        }
    }

    let sign = if value < 0.0 { -1.0 } else { 1.0 };
    let magnitude = value.abs().min(448.0);
    let mut low = 0usize;
    let mut high = 126usize;
    while low < high {
        let middle = (low + high + 1) >> 1;
        if positive_value(middle) <= magnitude {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    let mut best = low;
    if best < 126 {
        let best_difference = (magnitude - positive_value(best)).abs();
        let next_difference = (magnitude - positive_value(best + 1)).abs();
        if next_difference < best_difference
            || (next_difference == best_difference && (best + 1) % 2 == 0 && best % 2 != 0)
        {
            best += 1;
        }
    }
    sign * positive_value(best)
}

pub fn deepseek_v4_flash_indexer_qat_reference(values: &mut [f32]) -> Result<(), String> {
    const HEAD_DIM: usize = 128;
    if values.is_empty() || values.len() % HEAD_DIM != 0 {
        return Err(format!(
            "deepseek indexer QAT expects 128 values per head, got {}",
            values.len()
        ));
    }
    for head in values.chunks_exact_mut(HEAD_DIM) {
        let mut stride = 1;
        while stride < head.len() {
            for base in (0..head.len()).step_by(2 * stride) {
                for index in 0..stride {
                    let a = head[base + index];
                    let b = head[base + stride + index];
                    head[base + index] = a + b;
                    head[base + stride + index] = a - b;
                }
            }
            stride *= 2;
        }
        for value in head.iter_mut() {
            *value *= 0.08838834764831845;
        }

        const FP4_VALUES: [f32; 8] = [0.0, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0];
        for block in head.chunks_exact_mut(32) {
            let amax = block
                .iter()
                .map(|value| value.abs())
                .fold(0.0f32, f32::max)
                .max(7.052966104933725e-38);
            let scale = 2.0f32.powf((amax / 6.0).log2().ceil());
            for value in block {
                let sign = if *value < 0.0 { -1.0 } else { 1.0 };
                let magnitude = (*value / scale).abs().min(6.0);
                let mut best = 0usize;
                for candidate in 1..FP4_VALUES.len() {
                    let difference = (magnitude - FP4_VALUES[candidate]).abs();
                    let best_difference = (magnitude - FP4_VALUES[best]).abs();
                    if difference < best_difference
                        || (difference == best_difference && candidate % 2 == 0 && best % 2 != 0)
                    {
                        best = candidate;
                    }
                }
                *value = sign * FP4_VALUES[best] * scale;
            }
        }
    }
    Ok(())
}

pub fn deepseek_v4_flash_sparse_indexer_reference(
    q: &[f32],
    compressed_kv: &[f32],
    projected_head_weights: &[f32],
    num_heads: usize,
    head_dim: usize,
    top_k: usize,
) -> Result<Vec<usize>, String> {
    let q_len = num_heads
        .checked_mul(head_dim)
        .ok_or_else(|| "deepseek indexer Q shape overflow".to_string())?;
    if num_heads == 0
        || head_dim == 0
        || top_k == 0
        || q.len() != q_len
        || projected_head_weights.len() != num_heads
        || compressed_kv.is_empty()
        || compressed_kv.len() % head_dim != 0
        || q.iter()
            .chain(compressed_kv)
            .chain(projected_head_weights)
            .any(|value| !value.is_finite())
    {
        return Err(format!(
            "deepseek indexer shape mismatch: q={} kv={} weights={} heads={num_heads} head_dim={head_dim} top_k={top_k}",
            q.len(),
            compressed_kv.len(),
            projected_head_weights.len()
        ));
    }

    let row_count = compressed_kv.len() / head_dim;
    let selection_count = top_k.min(row_count);
    if selection_count == row_count {
        return Ok((0..row_count).collect());
    }
    let scale = ((num_heads * head_dim) as f32).sqrt().recip();
    let mut scores = vec![0.0f32; row_count];
    for (row, kv) in compressed_kv.chunks_exact(head_dim).enumerate() {
        for head in 0..num_heads {
            let q_head = &q[head * head_dim..(head + 1) * head_dim];
            let dot = q_head
                .iter()
                .zip(kv)
                .map(|(q_value, kv_value)| q_value * kv_value)
                .sum::<f32>()
                .max(0.0);
            scores[row] += dot * projected_head_weights[head] * scale;
        }
    }

    let mut selected = Vec::with_capacity(selection_count);
    let mut allowed = vec![false; row_count];
    for _ in 0..selection_count {
        let mut best_row = None;
        let mut best_score = f32::NEG_INFINITY;
        for row in 0..row_count {
            if !allowed[row] && scores[row] > best_score {
                best_row = Some(row);
                best_score = scores[row];
            }
        }
        let row = best_row.ok_or_else(|| "deepseek indexer selection failed".to_string())?;
        allowed[row] = true;
        selected.push(row);
    }
    Ok(selected)
}

pub fn deepseek_v4_flash_mixed_attention_reference(
    q: &[f32],
    raw_kv: &[f32],
    compressed_kv: &[f32],
    compressed_indices: &[usize],
    sinks: &[f32],
    num_heads: usize,
    head_dim: usize,
) -> Result<Vec<f32>, String> {
    if raw_kv.is_empty() || raw_kv.len() % head_dim != 0 {
        return Err("deepseek mixed attention raw KV shape mismatch".to_string());
    }
    let selected_compressed =
        deepseek_v4_flash_gather_kv_rows_reference(compressed_kv, head_dim, compressed_indices)?;
    let mut rows = Vec::with_capacity(raw_kv.len() + selected_compressed.len());
    rows.extend_from_slice(raw_kv);
    rows.extend_from_slice(&selected_compressed);
    deepseek_v4_flash_sink_attention_reference(q, &rows, sinks, num_heads, head_dim)
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashHcSplit {
    pub pre: Vec<f32>,
    pub post: Vec<f32>,
    /// DS4 Sinkhorn storage, row-major `[destination_hc, source_hc]`.
    /// HC post intentionally reads this buffer transposed.
    pub combine: Vec<f32>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashHcPreOutput {
    pub normalized_control_input: Vec<f32>,
    pub mixed_hidden: Vec<f32>,
    pub normalized_hidden: Vec<f32>,
    pub post: Vec<f32>,
    pub combine: Vec<f32>,
}

pub fn deepseek_v4_flash_rms_norm_reference(
    input: &[f32],
    weight: Option<&[f32]>,
    eps: f32,
) -> Result<Vec<f32>, String> {
    if input.is_empty() {
        return Err("deepseek rmsnorm input must be non-empty".to_string());
    }
    if !eps.is_finite() || eps <= 0.0 {
        return Err("deepseek rmsnorm epsilon must be finite and positive".to_string());
    }
    if let Some(weight) = weight {
        if weight.len() != input.len() {
            return Err(format!(
                "deepseek rmsnorm weight length mismatch: input={} weight={}",
                input.len(),
                weight.len()
            ));
        }
    }
    if input.iter().any(|value| !value.is_finite()) {
        return Err("deepseek rmsnorm input contains non-finite value".to_string());
    }

    let mean_square = input.iter().map(|value| value * value).sum::<f32>() / input.len() as f32;
    let inv_rms = (mean_square + eps).sqrt().recip();
    Ok(input
        .iter()
        .enumerate()
        .map(|(index, value)| value * inv_rms * weight.map_or(1.0, |values| values[index]))
        .collect())
}

pub fn deepseek_v4_flash_hc_split_reference(
    mix: &[f32],
    scale: &[f32],
    base: &[f32],
    hc_mult: usize,
    sinkhorn_iters: usize,
    eps: f32,
) -> Result<DeepseekV4FlashHcSplit, String> {
    let mix_hc = (2 + hc_mult)
        .checked_mul(hc_mult)
        .ok_or_else(|| "deepseek HC width overflow".to_string())?;
    if hc_mult == 0 || sinkhorn_iters == 0 {
        return Err("deepseek HC width and Sinkhorn iterations must be non-zero".to_string());
    }
    if mix.len() != mix_hc || base.len() != mix_hc || scale.len() != 3 {
        return Err(format!(
            "deepseek HC split shape mismatch: mix={} base={} scale={} expected_mix={mix_hc}",
            mix.len(),
            base.len(),
            scale.len()
        ));
    }
    if !eps.is_finite() || eps <= 0.0 {
        return Err("deepseek HC epsilon must be finite and positive".to_string());
    }

    let sigmoid = |value: f32| 1.0 / (1.0 + (-value).exp());
    let pre = (0..hc_mult)
        .map(|index| sigmoid(mix[index] * scale[0] + base[index]) + eps)
        .collect();
    let post = (0..hc_mult)
        .map(|index| {
            let offset = hc_mult + index;
            2.0 * sigmoid(mix[offset] * scale[1] + base[offset])
        })
        .collect();

    let mut combine = vec![0.0f32; hc_mult * hc_mult];
    let combine_offset = 2 * hc_mult;
    for destination in 0..hc_mult {
        let mut row_max = f32::NEG_INFINITY;
        for source in 0..hc_mult {
            let index = destination * hc_mult + source;
            let value = mix[combine_offset + index] * scale[2] + base[combine_offset + index];
            combine[index] = value;
            row_max = row_max.max(value);
        }
        let mut row_sum = 0.0;
        for source in 0..hc_mult {
            let index = destination * hc_mult + source;
            combine[index] = (combine[index] - row_max).exp();
            row_sum += combine[index];
        }
        for source in 0..hc_mult {
            let index = destination * hc_mult + source;
            combine[index] = combine[index] / row_sum + eps;
        }
    }

    normalize_hc_source_columns(&mut combine, hc_mult, eps);
    for _ in 1..sinkhorn_iters {
        normalize_hc_destination_rows(&mut combine, hc_mult, eps);
        normalize_hc_source_columns(&mut combine, hc_mult, eps);
    }

    Ok(DeepseekV4FlashHcSplit { pre, post, combine })
}

fn normalize_hc_source_columns(combine: &mut [f32], hc_mult: usize, eps: f32) {
    for source in 0..hc_mult {
        let sum = (0..hc_mult)
            .map(|destination| combine[destination * hc_mult + source])
            .sum::<f32>();
        let inv = (sum + eps).recip();
        for destination in 0..hc_mult {
            combine[destination * hc_mult + source] *= inv;
        }
    }
}

fn normalize_hc_destination_rows(combine: &mut [f32], hc_mult: usize, eps: f32) {
    for destination in 0..hc_mult {
        let start = destination * hc_mult;
        let sum = combine[start..start + hc_mult].iter().sum::<f32>();
        let inv = (sum + eps).recip();
        for value in &mut combine[start..start + hc_mult] {
            *value *= inv;
        }
    }
}

pub fn deepseek_v4_flash_hc_weighted_sum_reference(
    residual_hc: &[f32],
    pre: &[f32],
    hidden_size: usize,
) -> Result<Vec<f32>, String> {
    let expected = pre
        .len()
        .checked_mul(hidden_size)
        .ok_or_else(|| "deepseek HC residual size overflow".to_string())?;
    if hidden_size == 0 || residual_hc.len() != expected {
        return Err(format!(
            "deepseek HC residual shape mismatch: actual={} expected={expected}",
            residual_hc.len()
        ));
    }
    let mut output = vec![0.0f32; hidden_size];
    for (source, weight) in pre.iter().copied().enumerate() {
        let source_row = &residual_hc[source * hidden_size..(source + 1) * hidden_size];
        for (output, value) in output.iter_mut().zip(source_row) {
            *output += value * weight;
        }
    }
    Ok(output)
}

pub fn deepseek_v4_flash_hc_control_input_reference(
    residual_hc: &[f32],
    hidden_size: usize,
    hc_mult: usize,
    eps: f32,
) -> Result<Vec<f32>, String> {
    let expected = hidden_size
        .checked_mul(hc_mult)
        .ok_or_else(|| "deepseek HC control input size overflow".to_string())?;
    if hidden_size == 0 || hc_mult == 0 || residual_hc.len() != expected {
        return Err(format!(
            "deepseek HC control input shape mismatch: actual={} expected={expected}",
            residual_hc.len()
        ));
    }
    deepseek_v4_flash_rms_norm_reference(residual_hc, None, eps)
}

pub fn deepseek_v4_flash_hc_attention_input_from_mix_reference(
    residual_hc: &[f32],
    mix: &[f32],
    scale: &[f32],
    base: &[f32],
    attention_norm_weight: &[f32],
    hidden_size: usize,
    hc_mult: usize,
    sinkhorn_iters: usize,
    eps: f32,
) -> Result<DeepseekV4FlashHcPreOutput, String> {
    if attention_norm_weight.len() != hidden_size {
        return Err(format!(
            "deepseek attention norm shape mismatch: actual={} expected={hidden_size}",
            attention_norm_weight.len()
        ));
    }
    let normalized_control_input =
        deepseek_v4_flash_hc_control_input_reference(residual_hc, hidden_size, hc_mult, eps)?;
    let split =
        deepseek_v4_flash_hc_split_reference(mix, scale, base, hc_mult, sinkhorn_iters, eps)?;
    let mixed_hidden =
        deepseek_v4_flash_hc_weighted_sum_reference(residual_hc, &split.pre, hidden_size)?;
    let normalized_hidden =
        deepseek_v4_flash_rms_norm_reference(&mixed_hidden, Some(attention_norm_weight), eps)?;
    Ok(DeepseekV4FlashHcPreOutput {
        normalized_control_input,
        mixed_hidden,
        normalized_hidden,
        post: split.post,
        combine: split.combine,
    })
}

pub fn deepseek_v4_flash_hc_post_reference(
    sublayer_output: &[f32],
    residual_hc: &[f32],
    post: &[f32],
    combine: &[f32],
) -> Result<Vec<f32>, String> {
    let hc_mult = post.len();
    if hc_mult == 0 || combine.len() != hc_mult * hc_mult {
        return Err("deepseek HC post combine shape mismatch".to_string());
    }
    let hidden_size = sublayer_output.len();
    if hidden_size == 0 || residual_hc.len() != hc_mult * hidden_size {
        return Err("deepseek HC post residual shape mismatch".to_string());
    }

    let mut output = vec![0.0f32; hc_mult * hidden_size];
    for destination in 0..hc_mult {
        let output_row = &mut output[destination * hidden_size..(destination + 1) * hidden_size];
        for (value, sublayer) in output_row.iter_mut().zip(sublayer_output) {
            *value = sublayer * post[destination];
        }
        for source in 0..hc_mult {
            let weight = combine[source * hc_mult + destination];
            let residual_row = &residual_hc[source * hidden_size..(source + 1) * hidden_size];
            for (value, residual) in output_row.iter_mut().zip(residual_row) {
                *value += residual * weight;
            }
        }
    }
    Ok(output)
}

/// DS4-compatible SwiGLU used by both shared and routed experts.
pub fn deepseek_v4_flash_swiglu_reference(
    gate: &[f32],
    up: &[f32],
    clamp: f32,
) -> Result<Vec<f32>, String> {
    if gate.is_empty() || gate.len() != up.len() {
        return Err(format!(
            "deepseek SwiGLU shape mismatch:gate={} up={}",
            gate.len(),
            up.len()
        ));
    }
    if !clamp.is_finite() || clamp < 0.0 {
        return Err(format!("deepseek SwiGLU clamp invalid:{clamp}"));
    }
    if gate.iter().chain(up).any(|value| !value.is_finite()) {
        return Err("deepseek SwiGLU input contains non-finite value".to_string());
    }
    Ok(gate
        .iter()
        .zip(up)
        .map(|(gate, up)| {
            let gate = if clamp > 1.0e-6 {
                gate.min(clamp)
            } else {
                *gate
            };
            let up = if clamp > 1.0e-6 {
                up.clamp(-clamp, clamp)
            } else {
                *up
            };
            gate * (1.0 + (-gate).exp()).recip() * up
        })
        .collect())
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashRouterOutput {
    pub probabilities: Vec<f32>,
    pub expert_indices: Vec<usize>,
    pub expert_weights: Vec<f32>,
}

/// DS4-compatible router decision. Early hash-routed layers supply explicit
/// expert IDs; later layers select top-k after adding the optional bias.
pub fn deepseek_v4_flash_router_reference(
    logits: &[f32],
    selection_bias: Option<&[f32]>,
    selected_experts: Option<&[usize]>,
    top_k: usize,
    weight_scale: f32,
) -> Result<DeepseekV4FlashRouterOutput, String> {
    if logits.is_empty()
        || top_k == 0
        || top_k > logits.len()
        || !weight_scale.is_finite()
        || weight_scale <= 0.0
        || logits.iter().any(|value| !value.is_finite())
    {
        return Err(format!(
            "deepseek router arguments invalid:experts={} top_k={top_k} scale={weight_scale}",
            logits.len()
        ));
    }
    if selection_bias.is_some_and(|bias| {
        bias.len() != logits.len() || bias.iter().any(|value| !value.is_finite())
    }) {
        return Err("deepseek router selection bias shape invalid".to_string());
    }
    let probabilities: Vec<f32> = logits
        .iter()
        .map(|logit| {
            let softplus = if *logit > 20.0 {
                *logit
            } else if *logit < -20.0 {
                logit.exp()
            } else {
                logit.exp().ln_1p()
            };
            softplus.sqrt()
        })
        .collect();
    let expert_indices = if let Some(selected) = selected_experts {
        if selected.len() != top_k || selected.iter().any(|expert| *expert >= logits.len()) {
            return Err("deepseek hash router selection invalid".to_string());
        }
        selected.to_vec()
    } else {
        let scores: Vec<f32> = probabilities
            .iter()
            .enumerate()
            .map(|(index, probability)| {
                probability + selection_bias.map_or(0.0, |bias| bias[index])
            })
            .collect();
        let mut selected = Vec::with_capacity(top_k);
        for expert in 0..scores.len() {
            let insertion = selected
                .iter()
                .position(|current| scores[expert] > scores[*current])
                .unwrap_or(selected.len());
            if insertion < top_k {
                selected.insert(insertion, expert);
                selected.truncate(top_k);
            }
        }
        selected
    };
    let sum = expert_indices
        .iter()
        .map(|expert| probabilities[*expert])
        .sum::<f32>()
        .max(6.103_515_6e-5);
    let expert_weights = expert_indices
        .iter()
        .map(|expert| probabilities[*expert] / sum * weight_scale)
        .collect();
    Ok(DeepseekV4FlashRouterOutput {
        probabilities,
        expert_indices,
        expert_weights,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn swiglu_matches_ds4_clamp_semantics() {
        let output =
            deepseek_v4_flash_swiglu_reference(&[20.0, -2.0, 1.0], &[20.0, -20.0, 3.0], 10.0)
                .expect("execute SwiGLU reference");
        let expected = [
            10.0 * (1.0 + (-10.0f32).exp()).recip() * 10.0,
            -2.0 * (1.0 + 2.0f32.exp()).recip() * -10.0,
            1.0 * (1.0 + (-1.0f32).exp()).recip() * 3.0,
        ];
        assert_eq!(output, expected);
        assert!(deepseek_v4_flash_swiglu_reference(&[1.0], &[], 10.0).is_err());
        assert!(deepseek_v4_flash_swiglu_reference(&[f32::NAN], &[1.0], 10.0).is_err());
    }

    #[test]
    fn router_supports_topk_bias_and_hash_selection() {
        let logits = [0.0, 1.0, 2.0, 3.0];
        let topk =
            deepseek_v4_flash_router_reference(&logits, Some(&[10.0, 0.0, 0.0, 0.0]), None, 2, 1.5)
                .expect("route top-k experts");
        assert_eq!(topk.expert_indices, vec![0, 3]);
        assert!((topk.expert_weights.iter().sum::<f32>() - 1.5).abs() < 1.0e-6);
        let unbiased_sum = topk.probabilities[0] + topk.probabilities[3];
        assert_eq!(
            topk.expert_weights,
            vec![
                topk.probabilities[0] / unbiased_sum * 1.5,
                topk.probabilities[3] / unbiased_sum * 1.5,
            ]
        );

        let hash = deepseek_v4_flash_router_reference(&logits, None, Some(&[2, 1]), 2, 1.5)
            .expect("route hash-selected experts");
        assert_eq!(hash.expert_indices, vec![2, 1]);
        assert!((hash.expert_weights.iter().sum::<f32>() - 1.5).abs() < 1.0e-6);
    }

    #[test]
    fn decode_plan_uses_hyper_connection_hidden_and_real_moe_shapes() {
        let plan = DeepseekV4FlashLayerPlan::decode(0, 1).expect("layer 0");
        plan.validate().expect("valid Flash layer plan");
        assert_eq!(
            plan.hidden_hc,
            TensorContract::new([1, 4, 4096], DeepseekV4FlashDtype::F32)
        );
        assert_eq!(
            plan.routed_expert_intermediate,
            TensorContract::new([1, 2048], DeepseekV4FlashDtype::I8)
        );
        assert_eq!(
            plan.route_indices,
            TensorContract::new([1, 6], DeepseekV4FlashDtype::I32)
        );
    }

    #[test]
    fn decode_plan_selects_swa_csa_and_hca_from_compression_metadata() {
        let swa = DeepseekV4FlashLayerPlan::decode(1, 1).expect("SWA");
        let csa = DeepseekV4FlashLayerPlan::decode(2, 1).expect("CSA");
        let hca = DeepseekV4FlashLayerPlan::decode(3, 1).expect("HCA");
        assert_eq!(
            swa.attention_kind,
            DeepseekV4FlashAttentionKind::SlidingWindow
        );
        assert_eq!(
            csa.attention_kind,
            DeepseekV4FlashAttentionKind::CompressedSparseRatio4
        );
        assert_eq!(
            hca.attention_kind,
            DeepseekV4FlashAttentionKind::HeavilyCompressedRatio128
        );
        assert!(csa
            .operations
            .contains(&DeepseekV4FlashOp::SparseAttentionIndexer));
        assert!(!hca
            .operations
            .contains(&DeepseekV4FlashOp::SparseAttentionIndexer));
    }

    #[test]
    fn decode_plan_rejects_invalid_layer_or_empty_batch() {
        assert!(DeepseekV4FlashLayerPlan::decode(43, 1).is_err());
        assert!(DeepseekV4FlashLayerPlan::decode(0, 0).is_err());
    }

    #[test]
    fn attention_cache_plan_distinguishes_swa_csa_and_hca_reads() {
        let swa = DeepseekV4FlashAttentionCachePlan::decode(1, 1_024).expect("SWA cache");
        assert_eq!(swa.raw_start, 896);
        assert_eq!(swa.raw_rows, 128);
        assert_eq!(swa.compressed_rows, 0);
        assert_eq!(
            swa.compressed_selection,
            DeepseekV4FlashCompressedSelection::None
        );

        let csa = DeepseekV4FlashAttentionCachePlan::decode(2, 1_024).expect("CSA cache");
        assert_eq!(csa.raw_start, 896);
        assert_eq!(csa.compressed_rows, 256);
        assert_eq!(
            csa.compressed_selection,
            DeepseekV4FlashCompressedSelection::LearnedIndexer
        );

        let hca = DeepseekV4FlashAttentionCachePlan::decode(3, 1_024).expect("HCA cache");
        assert_eq!(hca.raw_start, 896);
        assert_eq!(hca.compressed_rows, 8);
        assert_eq!(
            hca.compressed_selection,
            DeepseekV4FlashCompressedSelection::DeterministicPrefix
        );
    }

    #[test]
    fn attention_cache_plan_caps_compressed_rows_at_model_topk() {
        let csa = DeepseekV4FlashAttentionCachePlan::decode(2, 16_384).expect("long CSA cache");
        let hca = DeepseekV4FlashAttentionCachePlan::decode(3, 131_072).expect("long HCA cache");
        assert_eq!(csa.compressed_rows, 512);
        assert_eq!(hca.compressed_rows, 512);
        assert!(DeepseekV4FlashAttentionCachePlan::decode(1, 0).is_err());
    }

    #[test]
    fn qkv_projection_plan_matches_flash_shapes() {
        let plan = DeepseekV4FlashQkvProjectionPlan::decode(8).expect("QKV plan");
        assert_eq!(
            plan.q_lora_weight,
            TensorContract::new([4096, 1024], DeepseekV4FlashDtype::Bf16)
        );
        assert_eq!(
            plan.q_projection_weight,
            TensorContract::new([1024, 32768], DeepseekV4FlashDtype::I8)
        );
        assert_eq!(
            plan.q,
            TensorContract::new([8, 64, 512], DeepseekV4FlashDtype::Bf16)
        );
        assert_eq!(
            plan.kv,
            TensorContract::new([8, 1, 512], DeepseekV4FlashDtype::Bf16)
        );
    }

    #[test]
    fn qkv_gemm_plan_maps_bf16_and_quantized_ops_explicitly() {
        let plans = DeepseekV4FlashGemmPlan::qkv_decode(1).expect("QKV GEMM plans");
        assert_eq!(plans.len(), 3);
        assert_eq!(plans[0].kind, DeepseekV4FlashGemmKind::QueryLowRank);
        assert_eq!((plans[0].logical_m, plans[0].artifact_m), (1, 128));
        assert_eq!((plans[0].k, plans[0].n), (4096, 1024));
        assert!(plans[0].supports_host_bf16_gemm());

        assert_eq!(plans[1].kind, DeepseekV4FlashGemmKind::QueryExpansion);
        assert_eq!((plans[1].k, plans[1].n), (1024, 32768));
        assert_eq!(plans[1].input_dtype, DeepseekV4FlashDtype::I8);
        assert_eq!(plans[1].output_dtype, DeepseekV4FlashDtype::I32);
        assert!(!plans[1].supports_host_bf16_gemm());
        assert!(plans[1].supports_host_quantized_gemm());

        assert_eq!(plans[2].kind, DeepseekV4FlashGemmKind::KeyValue);
        assert_eq!((plans[2].k, plans[2].n), (4096, 512));
        assert!(plans[2].supports_host_bf16_gemm());
    }

    #[test]
    fn hc_control_gemm_plan_pads_tokens_and_mix_width_for_fp32_backend() {
        let plan = DeepseekV4FlashGemmPlan::hc_control(1).expect("HC control GEMM plan");
        assert_eq!(plan.kind, DeepseekV4FlashGemmKind::HyperConnectionControl);
        assert_eq!((plan.logical_m, plan.artifact_m), (1, 128));
        assert_eq!((plan.k, plan.n), (16_384, 128));
        assert!(plan.supports_host_fp32_gemm());
        assert!(!plan.supports_host_bf16_gemm());
    }

    #[test]
    fn query_expansion_dequant_applies_row_and_column_scales() {
        let output = deepseek_v4_flash_dequantize_gemm_reference(
            &[2, -4, 6, -8],
            2,
            2,
            &[0.5, 0.25],
            &[2.0, 4.0],
        )
        .expect("query expansion dequant");
        assert_eq!(output, vec![2.0, -8.0, 3.0, -8.0]);
    }

    #[test]
    fn query_expansion_dequant_rejects_wrong_scale_shape() {
        let error =
            deepseek_v4_flash_dequantize_gemm_reference(&[1, 2, 3, 4], 2, 2, &[1.0], &[1.0, 1.0])
                .expect_err("row scale shape must match rows");
        assert!(error.contains("shape mismatch"));
    }

    #[test]
    fn head_rms_norm_is_independent_per_head() {
        let output =
            deepseek_v4_flash_head_rms_norm_reference(&[3.0, 4.0, 0.0, 5.0], 2, 2, None, 1.0e-6)
                .expect("head RMSNorm");
        let first_inv = ((25.0f32 / 2.0) + 1.0e-6).sqrt().recip();
        let second_inv = ((25.0f32 / 2.0) + 1.0e-6).sqrt().recip();
        assert!((output[0] - 3.0 * first_inv).abs() < 1.0e-6);
        assert!((output[1] - 4.0 * first_inv).abs() < 1.0e-6);
        assert!((output[2] - 0.0).abs() < 1.0e-6);
        assert!((output[3] - 5.0 * second_inv).abs() < 1.0e-6);
    }

    #[test]
    fn rope_rotates_only_head_tail_and_inverse_restores_input() {
        let values = [10.0, 20.0, 1.0, 2.0, 3.0, 4.0];
        let angle0 = 0.25f32;
        let angle1 = -0.5f32;
        let cos = [angle0.cos(), angle1.cos()];
        let sin = [angle0.sin(), angle1.sin()];
        let rotated = deepseek_v4_flash_rope_tail_reference(&values, 1, 6, 4, &cos, &sin, false)
            .expect("forward RoPE");
        assert_eq!(&rotated[..2], &values[..2]);
        let restored = deepseek_v4_flash_rope_tail_reference(&rotated, 1, 6, 4, &cos, &sin, true)
            .expect("inverse RoPE");
        for (actual, expected) in restored.iter().zip(values) {
            assert!((actual - expected).abs() < 1.0e-5);
        }
    }

    #[test]
    fn kv_write_and_gather_follow_explicit_physical_rows() {
        let mut cache = vec![0.0; 8];
        deepseek_v4_flash_write_kv_row_reference(&mut cache, 2, 2, &[5.0, 6.0])
            .expect("write KV row");
        deepseek_v4_flash_write_kv_row_reference(&mut cache, 2, 0, &[1.0, 2.0])
            .expect("write KV row");
        assert_eq!(cache, vec![1.0, 2.0, 0.0, 0.0, 5.0, 6.0, 0.0, 0.0]);
        let gathered =
            deepseek_v4_flash_gather_kv_rows_reference(&cache, 2, &[2, 0]).expect("gather KV rows");
        assert_eq!(gathered, vec![5.0, 6.0, 1.0, 2.0]);
        assert!(deepseek_v4_flash_write_kv_row_reference(&mut cache, 2, 4, &[1.0, 2.0]).is_err());
        assert!(deepseek_v4_flash_gather_kv_rows_reference(&cache, 2, &[4]).is_err());
    }

    #[test]
    fn sink_attention_matches_stable_softmax_with_no_sink_value() {
        let q = [1.0, 0.0, 0.0, 1.0];
        let kv = [1.0, 0.0, 0.0, 1.0];
        let output = deepseek_v4_flash_sink_attention_reference(&q, &kv, &[0.0, 0.0], 2, 2)
            .expect("sink attention");
        let high = (1.0f32 / 2.0f32.sqrt()).exp();
        let denominator = high + 2.0;
        let expected = [
            high / denominator,
            1.0 / denominator,
            1.0 / denominator,
            high / denominator,
        ];
        for (actual, expected) in output.iter().zip(expected) {
            assert!((actual - expected).abs() < 1.0e-6);
        }
    }

    #[test]
    fn sink_attention_is_numerically_stable_for_large_scores() {
        let output = deepseek_v4_flash_sink_attention_reference(
            &[10_000.0, 10_000.0],
            &[10_000.0, 10_000.0],
            &[100_000_000.0],
            1,
            2,
        )
        .expect("stable sink attention");
        assert!(output.iter().all(|value| value.is_finite()));
    }

    #[test]
    fn ratio4_compressor_emits_only_at_boundaries_and_pools_both_lanes() {
        let mut state = DeepseekV4FlashCompressorState::new(2, 4).expect("ratio-4 state");
        assert_eq!(state.width(), 4);
        let projected = [
            [10.0, 20.0, 1.0, 2.0],
            [30.0, 40.0, 3.0, 4.0],
            [50.0, 60.0, 5.0, 6.0],
            [70.0, 80.0, 7.0, 8.0],
        ];
        for (position, row) in projected.iter().enumerate() {
            let result = state
                .update_projected(
                    position as u32,
                    row,
                    &[0.0; 4],
                    &[0.0; 4],
                    &[1.0; 2],
                    2,
                    &[1.0],
                    &[0.0],
                )
                .expect("ratio-4 update");
            if position < 3 {
                assert!(result.is_none());
            } else {
                let output = result.expect("ratio-4 boundary output");
                let inv_rms = ((4.0f32 * 4.0 + 5.0 * 5.0) / 2.0 + 1.0e-6).sqrt().recip();
                assert!((output[0] - 4.0 * inv_rms).abs() < 1.0e-6);
                assert!((output[1] - 5.0 * inv_rms).abs() < 1.0e-6);
            }
        }
    }

    #[test]
    fn compressor_snapshot_round_trips_partial_window() {
        let mut state = DeepseekV4FlashCompressorState::new(2, 4).expect("ratio-4 state");
        state
            .update_projected(
                0,
                &[1.0, 2.0, 3.0, 4.0],
                &[0.0; 4],
                &[0.0; 4],
                &[1.0; 2],
                2,
                &[1.0],
                &[0.0],
            )
            .expect("partial compressor update");

        let restored = DeepseekV4FlashCompressorState::restore(state.snapshot())
            .expect("restore compressor state");
        assert_eq!(restored, state);
    }

    #[test]
    fn ratio128_compressor_keeps_state_until_full_boundary() {
        let mut state = DeepseekV4FlashCompressorState::new(2, 128).expect("ratio-128 state");
        for position in 0..128 {
            let result = state
                .update_projected(
                    position,
                    &[position as f32, 1.0],
                    &[0.0; 2],
                    &[0.0; 2],
                    &[1.0; 2],
                    2,
                    &[1.0],
                    &[0.0],
                )
                .expect("ratio-128 update");
            assert_eq!(result.is_some(), position == 127);
        }
    }

    #[test]
    fn fp8_kv_roundtrip_quantizes_nope_blocks_and_preserves_rope_tail() {
        let mut values = vec![0.0f32; 128];
        for (index, value) in values.iter_mut().enumerate() {
            *value = index as f32 * 0.013 - 0.7;
        }
        let rope_before = values[64..].to_vec();
        let nope_before = values[..64].to_vec();
        deepseek_v4_flash_fp8_kv_roundtrip_reference(&mut values, 64).expect("FP8 KV roundtrip");
        assert_ne!(&values[..64], nope_before.as_slice());
        assert_eq!(&values[64..], rope_before.as_slice());
    }

    #[test]
    fn indexer_qat_runs_hadamard_and_fp4_roundtrip() {
        let mut values = [0.0f32; 128];
        values[0] = 1.0;
        deepseek_v4_flash_indexer_qat_reference(&mut values).expect("indexer QAT");
        assert!(values.iter().all(|value| value.is_finite()));
        assert!(values.iter().all(|value| *value != 0.0));
    }

    #[test]
    fn indexer_qat_processes_each_head_independently() {
        let mut values = vec![0.0f32; 256];
        values[0] = 1.0;
        values[128] = 2.0;
        deepseek_v4_flash_indexer_qat_reference(&mut values).expect("two-head indexer QAT");
        assert!(values.iter().all(|value| value.is_finite()));
        assert_ne!(&values[..128], &values[128..]);
        let mut first = vec![0.0f32; 128];
        first[0] = 1.0;
        deepseek_v4_flash_indexer_qat_reference(&mut first).expect("single-head QAT");
        assert_eq!(&values[..128], first.as_slice());
    }

    #[test]
    fn sparse_indexer_selects_highest_weighted_relu_scores() {
        let q = [1.0, 0.0, 0.0, 2.0];
        let compressed_kv = [
            1.0, 0.0, // row 0: first head
            0.0, 1.0, // row 1: second head
            -1.0, -1.0, // row 2: both dots are clamped to zero
        ];
        let selected =
            deepseek_v4_flash_sparse_indexer_reference(&q, &compressed_kv, &[1.0, 2.0], 2, 2, 2)
                .expect("sparse indexer");
        assert_eq!(selected, vec![1, 0]);
    }

    #[test]
    fn mixed_attention_consumes_raw_and_selected_compressed_rows() {
        let output = deepseek_v4_flash_mixed_attention_reference(
            &[1.0, 0.0],
            &[1.0, 0.0],
            &[0.0, 1.0, 10.0, 10.0],
            &[0],
            &[0.0],
            1,
            2,
        )
        .expect("mixed attention");
        assert!(output[0] > output[1]);

        let different = deepseek_v4_flash_mixed_attention_reference(
            &[1.0, 0.0],
            &[1.0, 0.0],
            &[0.0, 1.0, 10.0, 10.0],
            &[1],
            &[0.0],
            1,
            2,
        )
        .expect("mixed attention with another compressed row");
        assert_ne!(output, different);
    }

    #[test]
    fn rms_norm_reference_matches_known_vector() {
        let output = deepseek_v4_flash_rms_norm_reference(&[3.0, 4.0], Some(&[2.0, 0.5]), 1.0e-6)
            .expect("rmsnorm");
        let inv_rms = ((25.0f32 / 2.0) + 1.0e-6).sqrt().recip();
        assert!((output[0] - 6.0 * inv_rms).abs() < 1.0e-6);
        assert!((output[1] - 2.0 * inv_rms).abs() < 1.0e-6);
    }

    #[test]
    fn hc_attention_input_chains_control_norm_mix_and_attention_norm() {
        let residual = vec![3.0, 4.0, 6.0, 8.0];
        let output = deepseek_v4_flash_hc_attention_input_from_mix_reference(
            &residual,
            &[0.0; 8],
            &[1.0; 3],
            &[0.0; 8],
            &[2.0, 0.5],
            2,
            2,
            3,
            1.0e-6,
        )
        .expect("HC attention input");

        let control_rms = ((9.0f32 + 16.0 + 36.0 + 64.0) / 4.0 + 1.0e-6).sqrt();
        assert_eq!(
            output.normalized_control_input,
            residual
                .iter()
                .map(|value| value / control_rms)
                .collect::<Vec<_>>()
        );
        for (actual, expected) in output
            .mixed_hidden
            .iter()
            .zip([4.5 + 9.0e-6, 6.0 + 12.0e-6])
        {
            assert!((actual - expected).abs() < 1.0e-6);
        }
        let expected_norm =
            deepseek_v4_flash_rms_norm_reference(&output.mixed_hidden, Some(&[2.0, 0.5]), 1.0e-6)
                .unwrap();
        assert_eq!(output.normalized_hidden, expected_norm);
        assert_eq!(output.post, vec![1.0, 1.0]);
        assert_eq!(output.combine.len(), 4);
    }

    #[test]
    fn zero_logits_hc_split_is_uniform_and_post_is_one() {
        let split =
            deepseek_v4_flash_hc_split_reference(&[0.0; 24], &[1.0; 3], &[0.0; 24], 4, 20, 1.0e-6)
                .expect("HC split");
        assert!(split
            .pre
            .iter()
            .all(|value| (*value - 0.500001).abs() < 1.0e-6));
        assert!(split.post.iter().all(|value| (*value - 1.0).abs() < 1.0e-6));
        assert!(split
            .combine
            .iter()
            .all(|value| (*value - 0.25).abs() < 2.0e-6));
    }

    #[test]
    fn hc_pre_reduction_and_post_preserve_expected_layout() {
        let residual = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0];
        let mixed =
            deepseek_v4_flash_hc_weighted_sum_reference(&residual, &[0.25, 0.25, 0.25, 0.25], 2)
                .expect("HC weighted sum");
        assert_eq!(mixed, vec![4.0, 5.0]);

        let output =
            deepseek_v4_flash_hc_post_reference(&[10.0, 20.0], &residual, &[1.0; 4], &[0.25; 16])
                .expect("HC post");
        assert_eq!(output, vec![14.0, 25.0, 14.0, 25.0, 14.0, 25.0, 14.0, 25.0]);
    }
}
