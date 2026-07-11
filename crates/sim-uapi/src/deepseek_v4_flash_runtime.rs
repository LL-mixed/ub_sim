use std::path::Path;

use sim_core::TaskKey;
use sim_models::deepseek_v4_flash_lowering::{
    deepseek_v4_flash_fp8_kv_roundtrip_reference, deepseek_v4_flash_hc_control_input_reference,
    deepseek_v4_flash_hc_post_reference, deepseek_v4_flash_hc_split_reference,
    deepseek_v4_flash_hc_weighted_sum_reference, deepseek_v4_flash_head_rms_norm_reference,
    deepseek_v4_flash_mixed_attention_reference, deepseek_v4_flash_rms_norm_reference,
    deepseek_v4_flash_rope_tail_reference, DeepseekV4FlashCompressorState,
};
use sim_topology::SimTopology;

use super::{
    execute_deepseek_compressor_update_through_simpler,
    execute_deepseek_f16_projection_through_simpler, execute_deepseek_ffn_through_simpler,
    execute_deepseek_grouped_q8_projection_through_simpler,
    execute_deepseek_indexer_through_simpler, execute_deepseek_q8_projection_through_simpler,
    f16_bits_to_f32, f32_to_f16_bits, DeepseekV4FlashAttentionWeights,
    DeepseekV4FlashCompressorWeights, DeepseekV4FlashFfnExecution, DeepseekV4FlashFfnWeights,
    DeepseekV4FlashIndexerWeights,
};

pub struct DeepseekV4FlashRatio4AttentionWeights<'a> {
    pub attention: DeepseekV4FlashAttentionWeights<'a>,
    pub attention_compressor: DeepseekV4FlashCompressorWeights<'a>,
    pub indexer_compressor: DeepseekV4FlashCompressorWeights<'a>,
    pub indexer: DeepseekV4FlashIndexerWeights<'a>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashRatio4AttentionState {
    raw_capacity: usize,
    compressed_capacity: usize,
    head_dim: usize,
    indexer_head_dim: usize,
    raw_kv: Vec<f32>,
    compressed_kv: Vec<f32>,
    indexer_compressed_kv: Vec<f32>,
    attention_compressor: DeepseekV4FlashCompressorState,
    indexer_compressor: DeepseekV4FlashCompressorState,
}

impl DeepseekV4FlashRatio4AttentionState {
    pub fn new(
        head_dim: usize,
        indexer_head_dim: usize,
        raw_capacity: usize,
        compressed_capacity: usize,
    ) -> Result<Self, String> {
        if head_dim == 0 || indexer_head_dim == 0 || raw_capacity == 0 || compressed_capacity == 0 {
            return Err(format!(
                "deepseek ratio-4 attention state geometry invalid:head_dim={head_dim}:indexer_head_dim={indexer_head_dim}:raw_capacity={raw_capacity}:compressed_capacity={compressed_capacity}"
            ));
        }
        Ok(Self {
            raw_capacity,
            compressed_capacity,
            head_dim,
            indexer_head_dim,
            raw_kv: Vec::new(),
            compressed_kv: Vec::new(),
            indexer_compressed_kv: Vec::new(),
            attention_compressor: DeepseekV4FlashCompressorState::new(head_dim, 4)?,
            indexer_compressor: DeepseekV4FlashCompressorState::new(indexer_head_dim, 4)?,
        })
    }

    pub fn raw_rows(&self) -> usize {
        self.raw_kv.len() / self.head_dim
    }

    pub fn compressed_rows(&self) -> usize {
        self.compressed_kv.len() / self.head_dim
    }

    pub fn indexer_compressed_rows(&self) -> usize {
        self.indexer_compressed_kv.len() / self.indexer_head_dim
    }

    pub fn raw_kv(&self) -> &[f32] {
        &self.raw_kv
    }

    pub fn compressed_kv(&self) -> &[f32] {
        &self.compressed_kv
    }

    pub fn indexer_compressed_kv(&self) -> &[f32] {
        &self.indexer_compressed_kv
    }

    fn push_raw(&mut self, row: &[f32]) -> Result<(), String> {
        push_bounded_row(&mut self.raw_kv, row, self.head_dim, self.raw_capacity)
    }

    fn push_compressed(&mut self, row: &[f32]) -> Result<(), String> {
        push_bounded_row(
            &mut self.compressed_kv,
            row,
            self.head_dim,
            self.compressed_capacity,
        )
    }

    fn push_indexer_compressed(&mut self, row: &[f32]) -> Result<(), String> {
        push_bounded_row(
            &mut self.indexer_compressed_kv,
            row,
            self.indexer_head_dim,
            self.compressed_capacity,
        )
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashRatio4AttentionExecution {
    pub current_kv: Vec<f32>,
    pub emitted_compressed_kv: Option<Vec<f32>>,
    pub emitted_indexer_kv: Option<Vec<f32>>,
    pub selected_compressed_rows: Vec<usize>,
    pub attention_output: Vec<f32>,
    pub output_hc: Vec<f32>,
}

pub struct DeepseekV4FlashRatio128AttentionWeights<'a> {
    pub attention: DeepseekV4FlashAttentionWeights<'a>,
    pub attention_compressor: DeepseekV4FlashCompressorWeights<'a>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashRatio128AttentionState {
    raw_capacity: usize,
    compressed_capacity: usize,
    head_dim: usize,
    raw_kv: Vec<f32>,
    compressed_kv: Vec<f32>,
    attention_compressor: DeepseekV4FlashCompressorState,
}

impl DeepseekV4FlashRatio128AttentionState {
    pub fn new(
        head_dim: usize,
        raw_capacity: usize,
        compressed_capacity: usize,
    ) -> Result<Self, String> {
        if head_dim == 0 || raw_capacity == 0 || compressed_capacity == 0 {
            return Err(format!(
                "deepseek ratio-128 attention state geometry invalid:head_dim={head_dim}:raw_capacity={raw_capacity}:compressed_capacity={compressed_capacity}"
            ));
        }
        Ok(Self {
            raw_capacity,
            compressed_capacity,
            head_dim,
            raw_kv: Vec::new(),
            compressed_kv: Vec::new(),
            attention_compressor: DeepseekV4FlashCompressorState::new(head_dim, 128)?,
        })
    }

    pub fn raw_rows(&self) -> usize {
        self.raw_kv.len() / self.head_dim
    }

    pub fn compressed_rows(&self) -> usize {
        self.compressed_kv.len() / self.head_dim
    }

    pub fn raw_kv(&self) -> &[f32] {
        &self.raw_kv
    }

    pub fn compressed_kv(&self) -> &[f32] {
        &self.compressed_kv
    }

    fn push_raw(&mut self, row: &[f32]) -> Result<(), String> {
        push_bounded_row(&mut self.raw_kv, row, self.head_dim, self.raw_capacity)
    }

    fn push_compressed(&mut self, row: &[f32]) -> Result<(), String> {
        push_bounded_row(
            &mut self.compressed_kv,
            row,
            self.head_dim,
            self.compressed_capacity,
        )
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashRatio128AttentionExecution {
    pub current_kv: Vec<f32>,
    pub emitted_compressed_kv: Option<Vec<f32>>,
    pub attention_output: Vec<f32>,
    pub output_hc: Vec<f32>,
}

pub struct DeepseekV4FlashRatio4LayerWeights<'a> {
    pub attention: DeepseekV4FlashRatio4AttentionWeights<'a>,
    pub ffn: DeepseekV4FlashFfnWeights<'a>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashRatio4LayerExecution {
    pub attention: DeepseekV4FlashRatio4AttentionExecution,
    pub ffn: DeepseekV4FlashFfnExecution,
}

pub struct DeepseekV4FlashRatio128LayerWeights<'a> {
    pub attention: DeepseekV4FlashRatio128AttentionWeights<'a>,
    pub ffn: DeepseekV4FlashFfnWeights<'a>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashRatio128LayerExecution {
    pub attention: DeepseekV4FlashRatio128AttentionExecution,
    pub ffn: DeepseekV4FlashFfnExecution,
}

fn push_bounded_row(
    cache: &mut Vec<f32>,
    row: &[f32],
    row_width: usize,
    capacity: usize,
) -> Result<(), String> {
    if row.len() != row_width {
        return Err(format!(
            "deepseek attention cache row shape mismatch:actual={}:expected={row_width}",
            row.len()
        ));
    }
    if cache.len() / row_width == capacity {
        cache.drain(..row_width);
    }
    cache.extend_from_slice(row);
    Ok(())
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_ratio4_layer_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    state: &mut DeepseekV4FlashRatio4AttentionState,
    weights: &DeepseekV4FlashRatio4LayerWeights<'_>,
    residual_hc: &[f32],
    position: u32,
    rope_cos: &[f32],
    rope_sin: &[f32],
    compressed_rope_cos: &[f32],
    compressed_rope_sin: &[f32],
    selection_bias: Option<&[f32]>,
    hash_selected_experts: Option<&[usize]>,
    hc_mult: usize,
    sinkhorn_iters: usize,
    num_heads: usize,
    head_dim: usize,
    rope_dim: usize,
    output_groups: usize,
    indexer_heads: usize,
    indexer_head_dim: usize,
    indexer_top_k: usize,
    eps: f32,
    clamp: f32,
) -> Result<DeepseekV4FlashRatio4LayerExecution, String> {
    let attention = execute_deepseek_ratio4_attention_through_simpler(
        topology,
        task,
        &artifact_dir.join("attention"),
        segment_base,
        state,
        &weights.attention,
        residual_hc,
        position,
        rope_cos,
        rope_sin,
        compressed_rope_cos,
        compressed_rope_sin,
        hc_mult,
        sinkhorn_iters,
        num_heads,
        head_dim,
        rope_dim,
        output_groups,
        indexer_heads,
        indexer_head_dim,
        indexer_top_k,
        eps,
    )?;
    let mut ffn_task = task.clone();
    ffn_task.task_id = task.task_id.saturating_add(1_000);
    let ffn = execute_deepseek_ffn_through_simpler(
        topology,
        &ffn_task,
        &artifact_dir.join("ffn"),
        segment_base.saturating_add(100_000),
        &weights.ffn,
        &attention.output_hc,
        selection_bias,
        hash_selected_experts,
        hc_mult,
        sinkhorn_iters,
        eps,
        clamp,
    )?;
    Ok(DeepseekV4FlashRatio4LayerExecution { attention, ffn })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_ratio128_layer_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    state: &mut DeepseekV4FlashRatio128AttentionState,
    weights: &DeepseekV4FlashRatio128LayerWeights<'_>,
    residual_hc: &[f32],
    position: u32,
    rope_cos: &[f32],
    rope_sin: &[f32],
    compressed_rope_cos: &[f32],
    compressed_rope_sin: &[f32],
    selection_bias: Option<&[f32]>,
    hash_selected_experts: Option<&[usize]>,
    hc_mult: usize,
    sinkhorn_iters: usize,
    num_heads: usize,
    head_dim: usize,
    rope_dim: usize,
    output_groups: usize,
    eps: f32,
    clamp: f32,
) -> Result<DeepseekV4FlashRatio128LayerExecution, String> {
    let attention = execute_deepseek_ratio128_attention_through_simpler(
        topology,
        task,
        &artifact_dir.join("attention"),
        segment_base,
        state,
        &weights.attention,
        residual_hc,
        position,
        rope_cos,
        rope_sin,
        compressed_rope_cos,
        compressed_rope_sin,
        hc_mult,
        sinkhorn_iters,
        num_heads,
        head_dim,
        rope_dim,
        output_groups,
        eps,
    )?;
    let mut ffn_task = task.clone();
    ffn_task.task_id = task.task_id.saturating_add(1_000);
    let ffn = execute_deepseek_ffn_through_simpler(
        topology,
        &ffn_task,
        &artifact_dir.join("ffn"),
        segment_base.saturating_add(100_000),
        &weights.ffn,
        &attention.output_hc,
        selection_bias,
        hash_selected_experts,
        hc_mult,
        sinkhorn_iters,
        eps,
        clamp,
    )?;
    Ok(DeepseekV4FlashRatio128LayerExecution { attention, ffn })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_ratio128_attention_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    state: &mut DeepseekV4FlashRatio128AttentionState,
    weights: &DeepseekV4FlashRatio128AttentionWeights<'_>,
    residual_hc: &[f32],
    position: u32,
    rope_cos: &[f32],
    rope_sin: &[f32],
    compressed_rope_cos: &[f32],
    compressed_rope_sin: &[f32],
    hc_mult: usize,
    sinkhorn_iters: usize,
    num_heads: usize,
    head_dim: usize,
    rope_dim: usize,
    output_groups: usize,
    eps: f32,
) -> Result<DeepseekV4FlashRatio128AttentionExecution, String> {
    if head_dim != state.head_dim {
        return Err(format!(
            "deepseek ratio-128 state geometry mismatch:head_dim={head_dim}/{}",
            state.head_dim
        ));
    }
    let hidden_size = weights.attention.attention_norm.len();
    let hc_size = hidden_size
        .checked_mul(hc_mult)
        .ok_or_else(|| "deepseek ratio-128 HC size overflow".to_string())?;
    let mix_size = hc_mult
        .checked_mul(hc_mult.saturating_add(2))
        .ok_or_else(|| "deepseek ratio-128 HC mix size overflow".to_string())?;
    if hidden_size == 0
        || residual_hc.len() != hc_size
        || weights.attention.hc_function_dimensions != [hc_size as u64, mix_size as u64]
    {
        return Err(format!(
            "deepseek ratio-128 HC shape mismatch:hidden={hidden_size}:residual={}:function={:?}",
            residual_hc.len(),
            weights.attention.hc_function_dimensions
        ));
    }

    let control_input =
        deepseek_v4_flash_hc_control_input_reference(residual_hc, hidden_size, hc_mult, eps)?;
    let control = execute_deepseek_f16_projection_through_simpler(
        topology,
        task,
        &artifact_dir.join("hc/host_fp32_gemm_manifest.json"),
        weights.attention.hc_function,
        weights.attention.hc_function_dimensions,
        &control_input,
    )?;
    let split = deepseek_v4_flash_hc_split_reference(
        &control,
        weights.attention.hc_scale,
        weights.attention.hc_base,
        hc_mult,
        sinkhorn_iters,
        eps,
    )?;
    let mixed_hidden =
        deepseek_v4_flash_hc_weighted_sum_reference(residual_hc, &split.pre, hidden_size)?;
    let normalized_hidden = deepseek_v4_flash_rms_norm_reference(
        &mixed_hidden,
        Some(weights.attention.attention_norm),
        eps,
    )?;

    let projection_manifest = |name: &str| {
        artifact_dir
            .join(name)
            .join("host_q8_block_dot_manifest.json")
    };
    let mut q_a_task = task.clone();
    q_a_task.task_id = task.task_id.saturating_add(1);
    let q_lora = execute_deepseek_q8_projection_through_simpler(
        topology,
        &q_a_task,
        &projection_manifest("q-a"),
        segment_base.saturating_add(100),
        weights.attention.q_a,
        weights.attention.q_a_dimensions,
        &normalized_hidden,
    )?;
    let q_lora =
        deepseek_v4_flash_rms_norm_reference(&q_lora, Some(weights.attention.q_a_norm), eps)?;
    let mut q_b_task = task.clone();
    q_b_task.task_id = task.task_id.saturating_add(2);
    let q = execute_deepseek_q8_projection_through_simpler(
        topology,
        &q_b_task,
        &projection_manifest("q-b"),
        segment_base.saturating_add(200),
        weights.attention.q_b,
        weights.attention.q_b_dimensions,
        &q_lora,
    )?;
    let q = deepseek_v4_flash_head_rms_norm_reference(&q, num_heads, head_dim, None, eps)?;
    let q = deepseek_v4_flash_rope_tail_reference(
        &q, num_heads, head_dim, rope_dim, rope_cos, rope_sin, false,
    )?;

    let mut kv_task = task.clone();
    kv_task.task_id = task.task_id.saturating_add(3);
    let kv = execute_deepseek_q8_projection_through_simpler(
        topology,
        &kv_task,
        &projection_manifest("kv"),
        segment_base.saturating_add(300),
        weights.attention.kv,
        weights.attention.kv_dimensions,
        &normalized_hidden,
    )?;
    let current_kv =
        deepseek_v4_flash_rms_norm_reference(&kv, Some(weights.attention.kv_norm), eps)?;
    let mut current_kv = deepseek_v4_flash_rope_tail_reference(
        &current_kv,
        1,
        head_dim,
        rope_dim,
        rope_cos,
        rope_sin,
        false,
    )?;
    deepseek_v4_flash_fp8_kv_roundtrip_reference(&mut current_kv, rope_dim)?;
    for value in &mut current_kv {
        *value = f16_bits_to_f32(f32_to_f16_bits(*value));
    }
    state.push_raw(&current_kv)?;

    let mut compressor_task = task.clone();
    compressor_task.task_id = task.task_id.saturating_add(10);
    let emitted_compressed_kv = execute_deepseek_compressor_update_through_simpler(
        topology,
        &compressor_task,
        &artifact_dir.join("attention-compressor"),
        &mut state.attention_compressor,
        &weights.attention_compressor,
        position,
        &normalized_hidden,
        rope_dim,
        compressed_rope_cos,
        compressed_rope_sin,
    )?;
    if let Some(row) = emitted_compressed_kv.as_deref() {
        state.push_compressed(row)?;
    }

    let compressed_rows = (0..state.compressed_rows()).collect::<Vec<_>>();
    let heads = deepseek_v4_flash_mixed_attention_reference(
        &q,
        &state.raw_kv,
        &state.compressed_kv,
        &compressed_rows,
        weights.attention.sinks,
        num_heads,
        head_dim,
    )?;
    let heads = deepseek_v4_flash_rope_tail_reference(
        &heads, num_heads, head_dim, rope_dim, rope_cos, rope_sin, true,
    )?;

    let mut output_a_task = task.clone();
    output_a_task.task_id = task.task_id.saturating_add(40);
    let output_a = execute_deepseek_grouped_q8_projection_through_simpler(
        topology,
        &output_a_task,
        &projection_manifest("output-a"),
        segment_base.saturating_add(4_000),
        weights.attention.output_a,
        weights.attention.output_a_dimensions,
        &heads,
        output_groups,
    )?;
    let mut output_b_task = task.clone();
    output_b_task.task_id = task.task_id.saturating_add(40 + output_groups as u64);
    let attention_output = execute_deepseek_q8_projection_through_simpler(
        topology,
        &output_b_task,
        &projection_manifest("output-b"),
        segment_base.saturating_add(5_000),
        weights.attention.output_b,
        weights.attention.output_b_dimensions,
        &output_a,
    )?;
    let output_hc = deepseek_v4_flash_hc_post_reference(
        &attention_output,
        residual_hc,
        &split.post,
        &split.combine,
    )?;
    Ok(DeepseekV4FlashRatio128AttentionExecution {
        current_kv,
        emitted_compressed_kv,
        attention_output,
        output_hc,
    })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_ratio4_attention_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    state: &mut DeepseekV4FlashRatio4AttentionState,
    weights: &DeepseekV4FlashRatio4AttentionWeights<'_>,
    residual_hc: &[f32],
    position: u32,
    rope_cos: &[f32],
    rope_sin: &[f32],
    compressed_rope_cos: &[f32],
    compressed_rope_sin: &[f32],
    hc_mult: usize,
    sinkhorn_iters: usize,
    num_heads: usize,
    head_dim: usize,
    rope_dim: usize,
    output_groups: usize,
    indexer_heads: usize,
    indexer_head_dim: usize,
    indexer_top_k: usize,
    eps: f32,
) -> Result<DeepseekV4FlashRatio4AttentionExecution, String> {
    if head_dim != state.head_dim || indexer_head_dim != state.indexer_head_dim {
        return Err(format!(
            "deepseek ratio-4 state geometry mismatch:head_dim={head_dim}/{}:indexer_head_dim={indexer_head_dim}/{}",
            state.head_dim, state.indexer_head_dim
        ));
    }
    let hidden_size = weights.attention.attention_norm.len();
    let hc_size = hidden_size
        .checked_mul(hc_mult)
        .ok_or_else(|| "deepseek ratio-4 HC size overflow".to_string())?;
    let mix_size = hc_mult
        .checked_mul(hc_mult.saturating_add(2))
        .ok_or_else(|| "deepseek ratio-4 HC mix size overflow".to_string())?;
    if hidden_size == 0
        || residual_hc.len() != hc_size
        || weights.attention.hc_function_dimensions != [hc_size as u64, mix_size as u64]
    {
        return Err(format!(
            "deepseek ratio-4 HC shape mismatch:hidden={hidden_size}:residual={}:function={:?}",
            residual_hc.len(),
            weights.attention.hc_function_dimensions
        ));
    }

    let control_input =
        deepseek_v4_flash_hc_control_input_reference(residual_hc, hidden_size, hc_mult, eps)?;
    let control = execute_deepseek_f16_projection_through_simpler(
        topology,
        task,
        &artifact_dir.join("hc/host_fp32_gemm_manifest.json"),
        weights.attention.hc_function,
        weights.attention.hc_function_dimensions,
        &control_input,
    )?;
    let split = deepseek_v4_flash_hc_split_reference(
        &control,
        weights.attention.hc_scale,
        weights.attention.hc_base,
        hc_mult,
        sinkhorn_iters,
        eps,
    )?;
    let mixed_hidden =
        deepseek_v4_flash_hc_weighted_sum_reference(residual_hc, &split.pre, hidden_size)?;
    let normalized_hidden = deepseek_v4_flash_rms_norm_reference(
        &mixed_hidden,
        Some(weights.attention.attention_norm),
        eps,
    )?;

    let projection_manifest = |name: &str| {
        artifact_dir
            .join(name)
            .join("host_q8_block_dot_manifest.json")
    };
    let mut q_a_task = task.clone();
    q_a_task.task_id = task.task_id.saturating_add(1);
    let q_lora = execute_deepseek_q8_projection_through_simpler(
        topology,
        &q_a_task,
        &projection_manifest("q-a"),
        segment_base.saturating_add(100),
        weights.attention.q_a,
        weights.attention.q_a_dimensions,
        &normalized_hidden,
    )?;
    let q_lora =
        deepseek_v4_flash_rms_norm_reference(&q_lora, Some(weights.attention.q_a_norm), eps)?;
    let mut q_b_task = task.clone();
    q_b_task.task_id = task.task_id.saturating_add(2);
    let q = execute_deepseek_q8_projection_through_simpler(
        topology,
        &q_b_task,
        &projection_manifest("q-b"),
        segment_base.saturating_add(200),
        weights.attention.q_b,
        weights.attention.q_b_dimensions,
        &q_lora,
    )?;
    let q = deepseek_v4_flash_head_rms_norm_reference(&q, num_heads, head_dim, None, eps)?;
    let q = deepseek_v4_flash_rope_tail_reference(
        &q, num_heads, head_dim, rope_dim, rope_cos, rope_sin, false,
    )?;

    let mut kv_task = task.clone();
    kv_task.task_id = task.task_id.saturating_add(3);
    let kv = execute_deepseek_q8_projection_through_simpler(
        topology,
        &kv_task,
        &projection_manifest("kv"),
        segment_base.saturating_add(300),
        weights.attention.kv,
        weights.attention.kv_dimensions,
        &normalized_hidden,
    )?;
    let current_kv =
        deepseek_v4_flash_rms_norm_reference(&kv, Some(weights.attention.kv_norm), eps)?;
    let mut current_kv = deepseek_v4_flash_rope_tail_reference(
        &current_kv,
        1,
        head_dim,
        rope_dim,
        rope_cos,
        rope_sin,
        false,
    )?;
    deepseek_v4_flash_fp8_kv_roundtrip_reference(&mut current_kv, rope_dim)?;
    for value in &mut current_kv {
        *value = f16_bits_to_f32(f32_to_f16_bits(*value));
    }
    state.push_raw(&current_kv)?;

    let mut attention_compressor_task = task.clone();
    attention_compressor_task.task_id = task.task_id.saturating_add(10);
    let emitted_compressed_kv = execute_deepseek_compressor_update_through_simpler(
        topology,
        &attention_compressor_task,
        &artifact_dir.join("attention-compressor"),
        &mut state.attention_compressor,
        &weights.attention_compressor,
        position,
        &normalized_hidden,
        rope_dim,
        compressed_rope_cos,
        compressed_rope_sin,
    )?;
    if let Some(row) = emitted_compressed_kv.as_deref() {
        state.push_compressed(row)?;
    }

    let mut indexer_compressor_task = task.clone();
    indexer_compressor_task.task_id = task.task_id.saturating_add(20);
    let emitted_indexer_kv = execute_deepseek_compressor_update_through_simpler(
        topology,
        &indexer_compressor_task,
        &artifact_dir.join("indexer-compressor"),
        &mut state.indexer_compressor,
        &weights.indexer_compressor,
        position,
        &normalized_hidden,
        rope_dim,
        compressed_rope_cos,
        compressed_rope_sin,
    )?;
    if let Some(row) = emitted_indexer_kv.as_deref() {
        state.push_indexer_compressed(row)?;
    }
    if state.compressed_rows() != state.indexer_compressed_rows() {
        return Err(format!(
            "deepseek ratio-4 compressed cache frontier mismatch:attention={}:indexer={}",
            state.compressed_rows(),
            state.indexer_compressed_rows()
        ));
    }

    let selected_compressed_rows = if state.indexer_compressed_kv.is_empty() {
        Vec::new()
    } else {
        let mut indexer_task = task.clone();
        indexer_task.task_id = task.task_id.saturating_add(30);
        execute_deepseek_indexer_through_simpler(
            topology,
            &indexer_task,
            &artifact_dir.join("indexer"),
            segment_base.saturating_add(3_000),
            &weights.indexer,
            &q_lora,
            &normalized_hidden,
            &state.indexer_compressed_kv,
            rope_dim,
            rope_cos,
            rope_sin,
            indexer_heads,
            indexer_head_dim,
            indexer_top_k,
        )?
    };
    let heads = deepseek_v4_flash_mixed_attention_reference(
        &q,
        &state.raw_kv,
        &state.compressed_kv,
        &selected_compressed_rows,
        weights.attention.sinks,
        num_heads,
        head_dim,
    )?;
    let heads = deepseek_v4_flash_rope_tail_reference(
        &heads, num_heads, head_dim, rope_dim, rope_cos, rope_sin, true,
    )?;

    let mut output_a_task = task.clone();
    output_a_task.task_id = task.task_id.saturating_add(40);
    let output_a = execute_deepseek_grouped_q8_projection_through_simpler(
        topology,
        &output_a_task,
        &projection_manifest("output-a"),
        segment_base.saturating_add(4_000),
        weights.attention.output_a,
        weights.attention.output_a_dimensions,
        &heads,
        output_groups,
    )?;
    let mut output_b_task = task.clone();
    output_b_task.task_id = task.task_id.saturating_add(40 + output_groups as u64);
    let attention_output = execute_deepseek_q8_projection_through_simpler(
        topology,
        &output_b_task,
        &projection_manifest("output-b"),
        segment_base.saturating_add(5_000),
        weights.attention.output_b,
        weights.attention.output_b_dimensions,
        &output_a,
    )?;
    let output_hc = deepseek_v4_flash_hc_post_reference(
        &attention_output,
        residual_hc,
        &split.post,
        &split.combine,
    )?;
    Ok(DeepseekV4FlashRatio4AttentionExecution {
        current_kv,
        emitted_compressed_kv,
        emitted_indexer_kv,
        selected_compressed_rows,
        attention_output,
        output_hc,
    })
}
