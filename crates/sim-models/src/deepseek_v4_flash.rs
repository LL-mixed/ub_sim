//! DeepSeek V4 Flash model geometry.
//!
//! Stage 1 scope: geometry only (layer count, hidden size, range nodes, MoE
//! expert counts). Real MoE routing / expert aggregation / expert cache is
//! stage 2. The values mirror DwarfStar (ds4) `DS4_SHAPE_FLASH`
//! (`/Volumes/repos/ds4/ds4.c:177-212`), the algorithmic reference.
//!
//! Flash is a Mixture-of-Experts transformer: 43 layers, hidden 4096, 256
//! routed experts (top-6 active) + 1 shared expert, compressed sparse
//! attention (128-token raw sliding window, ratio-4 / ratio-128 compressed
//! layers). For the pipeline-parallel sharding modeled here only the layer
//! count and range node count matter; per-layer KV coefficients differ by
//! layer type but stay constants (see the plan §3.4).

/// DeepSeek V4 Flash reference geometry.
///
/// `is_moe`, `num_experts`, `num_experts_used`, `num_experts_shared` are
/// declared here so stage 2 can pick them up; stage 1 does not consume them
/// beyond presence/existence checks.
#[derive(Clone, Copy, Debug)]
pub struct DeepseekV4FlashProfile {
    /// Total transformer layers (Flash = 43).
    pub num_hidden_layers: u64,
    /// Hidden dimension per layer (Flash = 4096).
    pub hidden_size: u64,
    /// Vocabulary size (Flash = 129280).
    pub vocab_size: u64,
    /// Number of attention heads (Flash = 64).
    pub num_attention_heads: u64,
    /// Number of KV heads (Flash heavily-compressed attention = 1).
    pub num_key_value_heads: u64,
    /// Dimension per head (Flash = 512).
    pub head_dim: u64,
    /// KV streams per layer (K + V).
    pub kv_streams: u64,
    /// Bytes per KV element in the current mem_service object contract.
    pub kv_elem_bytes: u64,
    /// Raw sliding-window attention width (Flash = 128 tokens).
    pub sliding_window: u64,
    /// Pipeline-parallel node count for 8-node full-mesh (8).
    pub tp_nodes: u64,
    /// Prefill tokens per step (matches the simulator prefill budget).
    pub prefill_tokens: u64,
    /// Decode tokens per step (single-token autoregressive).
    pub decode_tokens: u64,
    /// Whether this model is Mixture-of-Experts (Flash = true).
    pub is_moe: bool,
    /// Total routed experts (Flash = 256).
    pub num_experts: u64,
    /// Active routed experts per token (Flash = 6, top-6).
    pub num_experts_used: u64,
    /// Shared (always-active) experts (Flash = 1).
    pub num_experts_shared: u64,
}

pub const DEEPSEEK_V4_FLASH_PROFILE: DeepseekV4FlashProfile = DeepseekV4FlashProfile {
    num_hidden_layers: 43,
    hidden_size: 4_096,
    vocab_size: 129_280,
    num_attention_heads: 64,
    num_key_value_heads: 1,
    head_dim: 512,
    kv_streams: 2,
    kv_elem_bytes: 4,
    sliding_window: 128,
    tp_nodes: 8,
    prefill_tokens: 128,
    decode_tokens: 1,
    is_moe: true,
    num_experts: 256,
    num_experts_used: 6,
    num_experts_shared: 1,
};

/// DeepSeek V4 Flash model key (namespace for object-store keys).
pub const DEEPSEEK_V4_FLASH_MODEL_KEY: &str = "deepseek-v4-flash";

/// Layer range for one pipeline node, using the same base/rem contiguous split
/// as the guest C helper. For 43 layers / 8 nodes this yields nodes 0-2 owning
/// 6 layers and nodes 3-7 owning 5 layers.
pub fn deepseek_v4_flash_layer_range_for_node(node_id: u64) -> Option<(u64, u64)> {
    let profile = DEEPSEEK_V4_FLASH_PROFILE;
    if node_id >= profile.tp_nodes {
        return None;
    }
    let base = profile.num_hidden_layers / profile.tp_nodes;
    let rem = profile.num_hidden_layers % profile.tp_nodes;
    let start = node_id * base + node_id.min(rem);
    let count = base + u64::from(node_id < rem);
    Some((start, start + count))
}

/// Layer-id -> owning pipeline node.
pub fn deepseek_v4_flash_hidden_layer_owner_node(layer_id: u64) -> u64 {
    for node_id in 0..DEEPSEEK_V4_FLASH_PROFILE.tp_nodes {
        if let Some((start, end)) = deepseek_v4_flash_layer_range_for_node(node_id) {
            if layer_id >= start && layer_id < end {
                return node_id;
            }
        }
    }
    DEEPSEEK_V4_FLASH_PROFILE.tp_nodes.saturating_sub(1)
}

/// KV object bytes for one contiguous layer range.
pub fn deepseek_v4_flash_range_kv_state_bytes(layer_start: u64, layer_end: u64) -> Option<u64> {
    let profile = DEEPSEEK_V4_FLASH_PROFILE;
    if layer_end <= layer_start || layer_end > profile.num_hidden_layers {
        return None;
    }
    Some(
        (layer_end - layer_start)
            * profile.num_key_value_heads
            * profile.head_dim
            * profile.kv_streams
            * profile.kv_elem_bytes,
    )
}

/// Compute the per-node layer counts for the full 8-node pipeline.
/// Returns a vector indexed by node id; entry value is the number of layers
/// assigned to that node.
pub fn deepseek_v4_flash_layers_per_node() -> Vec<u64> {
    let mut counts = vec![0u64; DEEPSEEK_V4_FLASH_PROFILE.tp_nodes as usize];
    for layer_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        let node_id = deepseek_v4_flash_hidden_layer_owner_node(layer_id) as usize;
        if let Some(count) = counts.get_mut(node_id) {
            *count += 1;
        }
    }
    counts
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Stage 1 geometry smoke: 43 layers over 8 nodes, using the same
    /// base/rem split as the guest C helper.
    #[test]
    fn flash_43_layer_8_node_sharding_balanced() {
        let counts = deepseek_v4_flash_layers_per_node();
        assert_eq!(counts, vec![6, 6, 6, 5, 5, 5, 5, 5]);
        let min = *counts.iter().min().unwrap();
        let max = *counts.iter().max().unwrap();
        assert_eq!(min, 5, "minimum layers per node must be 5");
        assert_eq!(max, 6, "maximum layers per node must be 6");
        assert_eq!(max - min, 1, "layer split must be balanced (max diff 1)");
    }

    #[test]
    fn flash_layer_ranges_match_guest_base_rem_split() {
        assert_eq!(deepseek_v4_flash_layer_range_for_node(0), Some((0, 6)));
        assert_eq!(deepseek_v4_flash_layer_range_for_node(1), Some((6, 12)));
        assert_eq!(deepseek_v4_flash_layer_range_for_node(2), Some((12, 18)));
        assert_eq!(deepseek_v4_flash_layer_range_for_node(3), Some((18, 23)));
        assert_eq!(deepseek_v4_flash_layer_range_for_node(7), Some((38, 43)));
        assert_eq!(deepseek_v4_flash_layer_range_for_node(8), None);
    }

    /// Layer ownership is monotonic non-decreasing in node id (contiguous slices).
    #[test]
    fn flash_layer_ownership_is_contiguous() {
        let mut prev_node = 0u64;
        for layer_id in 0..DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
            let node = deepseek_v4_flash_hidden_layer_owner_node(layer_id);
            assert!(
                node >= prev_node,
                "layer {layer_id} node {node} < prev {prev_node}: not contiguous"
            );
            prev_node = node;
        }
        assert_eq!(prev_node, 7, "last layer must be owned by node 7");
    }

    /// MoE fields are present for stage 2; stage 1 only checks they are sane.
    #[test]
    fn flash_moe_geometry_matches_ds4_reference() {
        assert!(DEEPSEEK_V4_FLASH_PROFILE.is_moe);
        assert_eq!(DEEPSEEK_V4_FLASH_PROFILE.num_experts, 256);
        assert_eq!(DEEPSEEK_V4_FLASH_PROFILE.num_experts_used, 6);
        assert_eq!(DEEPSEEK_V4_FLASH_PROFILE.num_experts_shared, 1);
        assert_eq!(DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers, 43);
        assert_eq!(DEEPSEEK_V4_FLASH_PROFILE.hidden_size, 4_096);
        assert_eq!(DEEPSEEK_V4_FLASH_PROFILE.sliding_window, 128);
        assert_eq!(DEEPSEEK_V4_FLASH_PROFILE.kv_streams, 2);
        assert_eq!(DEEPSEEK_V4_FLASH_PROFILE.kv_elem_bytes, 4);
        assert_eq!(
            deepseek_v4_flash_range_kv_state_bytes(0, 6),
            Some(6 * 1 * 512 * 2 * 4)
        );
    }
}
