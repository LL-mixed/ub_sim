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
}
