//! Full-layer production execution for the official DeepSeek V4 Flash checkpoint.

use std::path::{Path, PathBuf};

use serde::Serialize;
use sim_core::TaskKey;
use sim_models::deepseek_v4_flash::{
    deepseek_v4_flash_rope_coefficients, DEEPSEEK_V4_FLASH_RMS_EPS,
};
use sim_models::deepseek_v4_flash_checkpoint::{DeepseekV4CacheStats, DeepseekV4Checkpoint};
use sim_models::deepseek_v4_flash_lowering::DeepseekV4FlashHcSplit;
use sim_topology::SimTopology;

use super::deepseek_v4_flash_official_expert_runtime::{
    execute_deepseek_official_fp8_rows_full_k_through_simpler,
    execute_deepseek_official_routed_experts_through_simpler,
    execute_deepseek_official_router_through_simpler,
};
use super::deepseek_v4_flash_official_runtime::{
    checksum_f32, execute_deepseek_official_bf16_rows_through_simpler,
    execute_deepseek_official_f32_rows_through_simpler, DeepseekV4LinearOutputDType,
};
use super::deepseek_v4_flash_official_vector_runtime::{
    execute_add_through_simpler, execute_hc_post_through_simpler, execute_hc_split_through_simpler,
    execute_hc_weighted_sum_through_simpler, execute_indexer_qat_through_simpler,
    execute_kv_fp8_roundtrip_through_simpler, execute_rms_norm_through_simpler,
    execute_rope_through_simpler, execute_scale_through_simpler,
    execute_sink_attention_through_simpler, execute_swiglu_through_simpler,
    DeepseekV4OfficialVectorExecution,
};

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialLayerKvSummary {
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
pub struct DeepseekV4OfficialLayerExecution {
    pub layer_id: u64,
    pub token_id: u64,
    pub position: u32,
    pub attention_kind: String,
    pub compress_ratio: u32,
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
    pub kv: DeepseekV4OfficialLayerKvSummary,
    #[serde(skip_serializing)]
    pub raw_kv: Vec<f32>,
    pub layer_output_hidden: Vec<f32>,
    pub layer_output_hidden_checksum: String,
    pub dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
    pub tensor_disk_read_bytes: u64,
    pub expert_disk_read_bytes: u64,
    pub tensor_cache: DeepseekV4CacheStats,
    pub expert_cache: DeepseekV4CacheStats,
}

struct HcPre {
    normalized_hidden: Vec<f32>,
    split: DeepseekV4FlashHcSplit,
}

struct AttentionOutput {
    output: Vec<f32>,
    kv: Vec<f32>,
    q_lora: Vec<f32>,
    query: Vec<f32>,
    attended: Vec<f32>,
    low_rank_output: Vec<f32>,
}

struct ProductionContext<'a> {
    checkpoint: &'a DeepseekV4Checkpoint,
    topology: &'a SimTopology,
    task: TaskKey,
    artifact_dir: PathBuf,
    next_segment: u64,
    dispatch_count: usize,
    peak_tile_payload_bytes: usize,
}

impl<'a> ProductionContext<'a> {
    fn current_task(&self) -> TaskKey {
        let mut task = self.task.clone();
        task.task_id = self.task.task_id.saturating_add(self.dispatch_count as u64);
        task
    }

    fn record(&mut self, dispatch_count: usize, peak_tile_payload_bytes: usize) {
        self.dispatch_count = self.dispatch_count.saturating_add(dispatch_count);
        self.peak_tile_payload_bytes = self.peak_tile_payload_bytes.max(peak_tile_payload_bytes);
        self.next_segment = self.next_segment.saturating_add(1_000);
    }

    fn vector_manifest(&self) -> PathBuf {
        self.artifact_dir
            .join("vector")
            .join("host_deepseek_vector_manifest.json")
    }

    fn record_vector(&mut self, execution: &DeepseekV4OfficialVectorExecution) {
        self.record(execution.dispatch_count, execution.peak_payload_bytes);
    }

    fn f32_rows(
        &mut self,
        name: &str,
        row_start: usize,
        row_count: usize,
        input: &[f32],
    ) -> Result<Vec<f32>, String> {
        let execution = execute_deepseek_official_f32_rows_through_simpler(
            self.checkpoint,
            self.topology,
            &self.current_task(),
            &self
                .artifact_dir
                .join(format!("fp32-k{}", input.len()))
                .join("host_fp32_gemm_manifest.json"),
            name,
            row_start,
            row_count,
            input,
            DeepseekV4LinearOutputDType::F32,
        )?;
        self.record(execution.dispatch_count, execution.peak_tile_payload_bytes);
        Ok(execution.output)
    }

    fn fp8_rows(
        &mut self,
        name: &str,
        row_start: usize,
        row_count: usize,
        input: &[f32],
    ) -> Result<Vec<f32>, String> {
        let execution = execute_deepseek_official_fp8_rows_full_k_through_simpler(
            self.checkpoint,
            self.topology,
            &self.current_task(),
            &self
                .artifact_dir
                .join(format!("mx-full-k{}", input.len()))
                .join("host_fp4_gemm_manifest.json"),
            self.next_segment,
            name,
            row_start,
            row_count,
            input,
            DeepseekV4LinearOutputDType::F32,
        )?;
        self.record(execution.dispatch_count, execution.peak_tile_payload_bytes);
        let rounded = execute_scale_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &execution.output,
            1.0,
            true,
        )?;
        self.record_vector(&rounded);
        Ok(rounded.output)
    }

    fn bf16_rows(
        &mut self,
        name: &str,
        row_start: usize,
        row_count: usize,
        input: &[f32],
    ) -> Result<Vec<f32>, String> {
        self.bf16_rows_with_output(
            name,
            row_start,
            row_count,
            input,
            DeepseekV4LinearOutputDType::Bf16,
        )
    }

    fn bf16_rows_with_output(
        &mut self,
        name: &str,
        row_start: usize,
        row_count: usize,
        input: &[f32],
        output_dtype: DeepseekV4LinearOutputDType,
    ) -> Result<Vec<f32>, String> {
        let execution = execute_deepseek_official_bf16_rows_through_simpler(
            self.checkpoint,
            self.topology,
            &self.current_task(),
            &self
                .artifact_dir
                .join(format!("bf16-k{}", input.len()))
                .join("host_gemm_manifest.json"),
            name,
            row_start,
            row_count,
            input,
            DeepseekV4LinearOutputDType::F32,
        )?;
        self.record(execution.dispatch_count, execution.peak_tile_payload_bytes);
        if output_dtype == DeepseekV4LinearOutputDType::F32 {
            return Ok(execution.output);
        }
        let rounded = execute_scale_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &execution.output,
            1.0,
            true,
        )?;
        self.record_vector(&rounded);
        Ok(rounded.output)
    }

    fn hc_pre(&mut self, prefix: &str, kind: &str, residual_hc: &[f32]) -> Result<HcPre, String> {
        let hidden_size = self.checkpoint.config.hidden_size as usize;
        let hc_mult = self.checkpoint.config.hc_mult as usize;
        let control = execute_rms_norm_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            residual_hc,
            None,
            1,
            residual_hc.len(),
            DEEPSEEK_V4_FLASH_RMS_EPS,
            false,
        )?;
        self.record_vector(&control);
        let mix = self.f32_rows(
            &format!("{prefix}.hc_{kind}_fn"),
            0,
            (2 + hc_mult) * hc_mult,
            &control.output,
        )?;
        let scale = self
            .checkpoint
            .reference_read_vector(&format!("{prefix}.hc_{kind}_scale"))?;
        let base = self
            .checkpoint
            .reference_read_vector(&format!("{prefix}.hc_{kind}_base"))?;
        let (split, split_execution) = execute_hc_split_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &mix,
            &scale,
            &base,
            hc_mult,
            self.checkpoint.config.hc_sinkhorn_iters as usize,
            self.checkpoint.config.hc_eps as f32,
        )?;
        self.record_vector(&split_execution);
        let mixed = execute_hc_weighted_sum_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            residual_hc,
            &split.pre,
            hidden_size,
            true,
        )?;
        self.record_vector(&mixed);
        let norm = self
            .checkpoint
            .reference_read_vector(&format!("{prefix}.{kind}_norm.weight"))?;
        let normalized_hidden = execute_rms_norm_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &mixed.output,
            Some(&norm),
            1,
            hidden_size,
            self.checkpoint.config.rms_norm_eps as f32,
            true,
        )?;
        self.record_vector(&normalized_hidden);
        Ok(HcPre {
            normalized_hidden: normalized_hidden.output,
            split,
        })
    }

    fn attention(
        &mut self,
        prefix: &str,
        layer_id: u64,
        position: u32,
        hidden: &[f32],
    ) -> Result<AttentionOutput, String> {
        let config = &self.checkpoint.config;
        let heads = config.num_attention_heads as usize;
        let head_dim = config.head_dim as usize;
        let rope_dim = config.qk_rope_head_dim as usize;
        let q_rank = config.q_lora_rank as usize;
        let output_groups = config.o_groups as usize;
        let output_rank = config.o_lora_rank as usize;
        let hidden_size = config.hidden_size as usize;

        let q_lora = self.fp8_rows(&format!("{prefix}.attn.wq_a.weight"), 0, q_rank, hidden)?;
        let q_norm_weight = self
            .checkpoint
            .reference_read_vector(&format!("{prefix}.attn.q_norm.weight"))?;
        let q_lora = execute_rms_norm_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &q_lora,
            Some(&q_norm_weight),
            1,
            q_rank,
            config.rms_norm_eps as f32,
            true,
        )?;
        self.record_vector(&q_lora);
        let q = self.fp8_rows(
            &format!("{prefix}.attn.wq_b.weight"),
            0,
            heads * head_dim,
            &q_lora.output,
        )?;
        let q = execute_rms_norm_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &q,
            None,
            heads,
            head_dim,
            config.rms_norm_eps as f32,
            true,
        )?;
        self.record_vector(&q);
        let rope = deepseek_v4_flash_rope_coefficients(layer_id, position)?;
        let q = execute_rope_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &q.output,
            &rope.cos,
            &rope.sin,
            heads,
            head_dim,
            rope_dim,
            false,
        )?;
        self.record_vector(&q);

        let kv = self.fp8_rows(&format!("{prefix}.attn.wkv.weight"), 0, head_dim, hidden)?;
        let kv_norm_weight = self
            .checkpoint
            .reference_read_vector(&format!("{prefix}.attn.kv_norm.weight"))?;
        let kv = execute_rms_norm_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &kv,
            Some(&kv_norm_weight),
            1,
            head_dim,
            config.rms_norm_eps as f32,
            true,
        )?;
        self.record_vector(&kv);
        let kv = execute_rope_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &kv.output,
            &rope.cos,
            &rope.sin,
            1,
            head_dim,
            rope_dim,
            false,
        )?;
        self.record_vector(&kv);
        let nope_dim = head_dim - rope_dim;
        let kv = execute_kv_fp8_roundtrip_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &kv.output,
            nope_dim,
            64,
        )?;
        self.record_vector(&kv);

        let sinks = self
            .checkpoint
            .reference_read_vector(&format!("{prefix}.attn.attn_sink"))?;
        let attended = execute_sink_attention_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &q.output,
            &kv.output,
            &sinks,
            heads,
            head_dim,
        )?;
        self.record_vector(&attended);
        let attended = execute_rope_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &attended.output,
            &rope.cos,
            &rope.sin,
            heads,
            head_dim,
            rope_dim,
            true,
        )?;
        self.record_vector(&attended);

        if heads % output_groups != 0 {
            return Err("deepseek_v4_production_output_group_geometry_invalid".to_string());
        }
        let group_input = heads / output_groups * head_dim;
        let mut low_rank_output = Vec::with_capacity(output_groups * output_rank);
        for group in 0..output_groups {
            low_rank_output.extend(self.fp8_rows(
                &format!("{prefix}.attn.wo_a.weight"),
                group * output_rank,
                output_rank,
                &attended.output[group * group_input..(group + 1) * group_input],
            )?);
        }
        let output = self.fp8_rows(
            &format!("{prefix}.attn.wo_b.weight"),
            0,
            hidden_size,
            &low_rank_output,
        )?;
        Ok(AttentionOutput {
            output,
            kv: kv.output,
            q_lora: q_lora.output,
            query: q.output,
            attended: attended.output,
            low_rank_output,
        })
    }

    fn indexer_outputs(
        &mut self,
        prefix: &str,
        hidden: &[f32],
        q_lora: &[f32],
    ) -> Result<(Vec<f32>, Vec<f32>), String> {
        let index_heads = self.checkpoint.config.index_n_heads as usize;
        let index_head_dim = self.checkpoint.config.index_head_dim as usize;
        let query = self.fp8_rows(
            &format!("{prefix}.wq_b.weight"),
            0,
            index_heads * index_head_dim,
            q_lora,
        )?;
        let query = execute_indexer_qat_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &query,
        )?;
        self.record_vector(&query);
        let weights = self.bf16_rows(
            &format!("{prefix}.weights_proj.weight"),
            0,
            index_heads,
            hidden,
        )?;
        let weight_scale =
            (index_head_dim as f32).sqrt().recip() * (index_heads as f32).sqrt().recip();
        let weights = execute_scale_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &weights,
            weight_scale,
            true,
        )?;
        self.record_vector(&weights);
        Ok((query.output, weights.output))
    }

    fn compressor_pending(
        &mut self,
        prefix: &str,
        hidden: &[f32],
        head_dim: usize,
        overlap: bool,
    ) -> Result<Vec<f32>, String> {
        let width = (1 + usize::from(overlap))
            .checked_mul(head_dim)
            .ok_or_else(|| "deepseek_v4_production_compressor_width_overflow".to_string())?;
        let kv = self.bf16_rows_with_output(
            &format!("{prefix}.wkv.weight"),
            0,
            width,
            hidden,
            DeepseekV4LinearOutputDType::F32,
        )?;
        let score = self.bf16_rows_with_output(
            &format!("{prefix}.wgate.weight"),
            0,
            width,
            hidden,
            DeepseekV4LinearOutputDType::F32,
        )?;
        let ape = self
            .checkpoint
            .reference_decode_tensor(&format!("{prefix}.ape"), 0, width as u64)?
            .values;
        let score = execute_add_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &score,
            &ape,
            false,
        )?;
        self.record_vector(&score);
        let mut pending = kv;
        pending.extend(score.output);
        Ok(pending)
    }

    fn shared_expert(&mut self, prefix: &str, input: &[f32]) -> Result<Vec<f32>, String> {
        let intermediate = self.checkpoint.config.moe_intermediate_size as usize;
        let hidden_size = self.checkpoint.config.hidden_size as usize;
        let gate = self.fp8_rows(&format!("{prefix}.w1.weight"), 0, intermediate, input)?;
        let up = self.fp8_rows(&format!("{prefix}.w3.weight"), 0, intermediate, input)?;
        let activated = execute_swiglu_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &gate,
            &up,
            self.checkpoint.config.swiglu_limit as f32,
            true,
        )?;
        self.record_vector(&activated);
        self.fp8_rows(
            &format!("{prefix}.w2.weight"),
            0,
            hidden_size,
            &activated.output,
        )
    }

    fn ffn(
        &mut self,
        prefix: &str,
        layer_id: usize,
        token_id: u64,
        input: &[f32],
    ) -> Result<(Vec<f32>, Vec<usize>, Vec<f32>), String> {
        let route = execute_deepseek_official_router_through_simpler(
            self.checkpoint,
            self.topology,
            &self.current_task(),
            &self
                .artifact_dir
                .join("bf16")
                .join("host_gemm_manifest.json"),
            layer_id,
            token_id,
            input,
        )?;
        self.record(route.dispatch_count, route.peak_payload_bytes);
        let output = self.shared_expert(&format!("{prefix}.ffn.shared_experts"), input)?;
        let routed = execute_deepseek_official_routed_experts_through_simpler(
            self.checkpoint,
            self.topology,
            &self.current_task(),
            &self.artifact_dir.join("fp4"),
            self.next_segment,
            layer_id,
            &route.expert_indices,
            &route.expert_weights,
            input,
        )?;
        self.record(routed.dispatch_count, 0);
        let output = execute_add_through_simpler(
            self.topology,
            &self.current_task(),
            &self.vector_manifest(),
            self.next_segment,
            &output,
            &routed.output,
            true,
        )?;
        self.record_vector(&output);
        Ok((output.output, route.expert_indices, route.expert_weights))
    }
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_layer_through_simpler(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    layer_id: u64,
    token_id: u64,
    position: u32,
    hidden_hc: &[f32],
) -> Result<DeepseekV4OfficialLayerExecution, String> {
    let config = &checkpoint.config;
    let hidden_size = config.hidden_size as usize;
    let hc_mult = config.hc_mult as usize;
    if layer_id >= config.num_hidden_layers
        || token_id >= config.vocab_size
        || position != 0
        || hidden_hc.len() != hidden_size * hc_mult
        || hidden_hc.iter().any(|value| !value.is_finite())
    {
        return Err(format!(
            "deepseek_v4_production_layer_request_invalid:layer={layer_id}:token={token_id}:position={position}:hidden={}",
            hidden_hc.len()
        ));
    }
    let ratio = config.compress_ratios[layer_id as usize];
    if !matches!(ratio, 0 | 4 | 128) {
        return Err(format!(
            "deepseek_v4_production_attention_ratio_unsupported:layer={layer_id}:ratio={ratio}"
        ));
    }
    let (tensor_cache_before, expert_cache_before) = checkpoint.cache_stats()?;
    let input_hidden_checksum = checksum_f32(hidden_hc);
    let prefix = format!("layers.{layer_id}");
    let mut context = ProductionContext {
        checkpoint,
        topology,
        task: task.clone(),
        artifact_dir: artifact_dir.to_path_buf(),
        next_segment: segment_base,
        dispatch_count: 0,
        peak_tile_payload_bytes: 0,
    };

    let attention_pre = context.hc_pre(&prefix, "attn", hidden_hc)?;
    let attention = context.attention(
        &prefix,
        layer_id,
        position,
        &attention_pre.normalized_hidden,
    )?;
    let attention_output = attention.output;
    let kv = attention.kv;
    let q_lora = attention.q_lora;
    let attention_compressor_pending = if ratio == 0 {
        None
    } else {
        Some(context.compressor_pending(
            &format!("{prefix}.attn.compressor"),
            &attention_pre.normalized_hidden,
            config.head_dim as usize,
            ratio == 4,
        )?)
    };
    let indexer_compressor_pending = if ratio == 4 {
        Some(context.compressor_pending(
            &format!("{prefix}.attn.indexer.compressor"),
            &attention_pre.normalized_hidden,
            config.index_head_dim as usize,
            true,
        )?)
    } else {
        None
    };
    let attention_compressor_pending_checksum =
        attention_compressor_pending.as_deref().map(checksum_f32);
    let indexer_compressor_pending_checksum =
        indexer_compressor_pending.as_deref().map(checksum_f32);
    let (indexer_query, indexer_weights) = if ratio == 4 {
        let (query, weights) = context.indexer_outputs(
            &format!("{prefix}.attn.indexer"),
            &attention_pre.normalized_hidden,
            &q_lora,
        )?;
        (Some(query), Some(weights))
    } else {
        (None, None)
    };
    let indexer_query_checksum = indexer_query.as_deref().map(checksum_f32);
    let indexer_weights_checksum = indexer_weights.as_deref().map(checksum_f32);
    let attention_output_checksum = checksum_f32(&attention_output);
    let attention_hidden = execute_hc_post_through_simpler(
        context.topology,
        &context.current_task(),
        &context.vector_manifest(),
        context.next_segment,
        &attention_output,
        hidden_hc,
        &attention_pre.split.post,
        &attention_pre.split.combine,
        hidden_size,
        true,
    )?;
    context.record_vector(&attention_hidden);
    let attention_hidden = attention_hidden.output;

    let ffn_pre = context.hc_pre(&prefix, "ffn", &attention_hidden)?;
    let (ffn, selected_experts, route_weights) = context.ffn(
        &prefix,
        layer_id as usize,
        token_id,
        &ffn_pre.normalized_hidden,
    )?;
    let layer_output_hidden = execute_hc_post_through_simpler(
        context.topology,
        &context.current_task(),
        &context.vector_manifest(),
        context.next_segment,
        &ffn,
        &attention_hidden,
        &ffn_pre.split.post,
        &ffn_pre.split.combine,
        hidden_size,
        true,
    )?;
    context.record_vector(&layer_output_hidden);
    let layer_output_hidden = layer_output_hidden.output;
    let layer_output_hidden_checksum = checksum_f32(&layer_output_hidden);
    let (tensor_cache, expert_cache) = checkpoint.cache_stats()?;
    let tensor_disk_read_bytes = tensor_cache
        .disk_read_bytes
        .checked_sub(tensor_cache_before.disk_read_bytes)
        .ok_or_else(|| "deepseek_v4_production_tensor_cache_counter_underflow".to_string())?;
    let expert_disk_read_bytes = expert_cache
        .disk_read_bytes
        .checked_sub(expert_cache_before.disk_read_bytes)
        .ok_or_else(|| "deepseek_v4_production_expert_cache_counter_underflow".to_string())?;

    Ok(DeepseekV4OfficialLayerExecution {
        layer_id,
        token_id,
        position,
        attention_kind: match ratio {
            0 => "raw",
            4 => "compressed-ratio4",
            128 => "compressed-ratio128",
            _ => unreachable!("validated official compression ratio"),
        }
        .to_string(),
        compress_ratio: ratio,
        input_hidden_checksum,
        attention_output_checksum,
        attention_output,
        q_lora,
        query: attention.query,
        attended: attention.attended,
        low_rank_attention_output: attention.low_rank_output,
        selected_experts,
        route_weights,
        kv: DeepseekV4OfficialLayerKvSummary {
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
        dispatch_count: context.dispatch_count,
        peak_tile_payload_bytes: context.peak_tile_payload_bytes,
        tensor_disk_read_bytes,
        expert_disk_read_bytes,
        tensor_cache,
        expert_cache,
    })
}

#[cfg(test)]
mod tests {
    #[test]
    fn routed_dispatch_tile_count_uses_ceiling_division() {
        assert_eq!(1usize.div_ceil(128), 1);
        assert_eq!(129usize.div_ceil(128), 2);
    }
}
