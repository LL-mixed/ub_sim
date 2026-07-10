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

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashHcSplit {
    pub pre: Vec<f32>,
    pub post: Vec<f32>,
    /// DS4 Sinkhorn storage, row-major `[destination_hc, source_hc]`.
    /// HC post intentionally reads this buffer transposed.
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

#[cfg(test)]
mod tests {
    use super::*;

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
    fn rms_norm_reference_matches_known_vector() {
        let output = deepseek_v4_flash_rms_norm_reference(&[3.0, 4.0], Some(&[2.0, 0.5]), 1.0e-6)
            .expect("rmsnorm");
        let inv_rms = ((25.0f32 / 2.0) + 1.0e-6).sqrt().recip();
        assert!((output[0] - 6.0 * inv_rms).abs() < 1.0e-6);
        assert!((output[1] - 2.0 * inv_rms).abs() < 1.0e-6);
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
