//! First-token execution for the official DeepSeek V4 Flash checkpoint.

use std::path::Path;
use std::time::Instant;

use serde::Serialize;
use sim_core::TaskKey;
use sim_models::deepseek_v4_flash_checkpoint::{
    process_peak_resident_bytes, DeepseekV4CacheStats, DeepseekV4Checkpoint,
};
use sim_models::deepseek_v4_flash_checkpoint_reference::{
    checksum_f32, DeepseekV4ReferenceTokenLogit,
};
use sim_topology::SimTopology;

use super::deepseek_v4_flash_official_layer_runtime::{
    execute_deepseek_official_layer_through_simpler, DeepseekV4OfficialLayerExecution,
};
use super::deepseek_v4_flash_official_runtime::{
    execute_deepseek_official_bf16_rows_through_simpler,
    execute_deepseek_official_f32_rows_through_simpler, DeepseekV4LinearOutputDType,
};
use super::deepseek_v4_flash_official_vector_runtime::{
    execute_hc_head_weights_through_simpler, execute_hc_weighted_sum_through_simpler,
    execute_rms_norm_through_simpler, execute_top_k_through_simpler,
};

pub const DEEPSEEK_V4_FIRST_TOKEN_ATTENTION_TOLERANCE: f32 = 0.0;
pub const DEEPSEEK_V4_FIRST_TOKEN_KV_TOLERANCE: f32 = 0.0;
pub const DEEPSEEK_V4_FIRST_TOKEN_ROUTE_WEIGHT_TOLERANCE: f32 = 1.0e-7;
pub const DEEPSEEK_V4_FIRST_TOKEN_HIDDEN_TOLERANCE: f32 = 1.0e-5;
pub const DEEPSEEK_V4_FIRST_TOKEN_LOGIT_TOLERANCE: f32 = 0.0;

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialHeadExecution {
    pub hc_head_checksum: String,
    pub normalized_hidden_checksum: String,
    #[serde(skip_serializing)]
    pub logits: Vec<f32>,
    pub logits_checksum: String,
    pub top_k: Vec<DeepseekV4ReferenceTokenLogit>,
    pub dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
    pub tensor_disk_read_bytes: u64,
    pub tensor_cache: DeepseekV4CacheStats,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialLayerAlignment {
    pub layer_id: u64,
    pub attention_kind: String,
    pub compress_ratio: u32,
    pub reference_input_hidden_checksum: String,
    pub production_input_hidden_checksum: String,
    pub reference_output_hidden_checksum: String,
    pub production_output_hidden_checksum: String,
    pub selected_experts: Vec<usize>,
    pub reference_route_weights: Vec<f32>,
    pub production_route_weights: Vec<f32>,
    pub attention_max_abs_diff: f32,
    pub raw_kv_max_abs_diff: f32,
    pub route_weight_max_abs_diff: f32,
    pub output_hidden_max_abs_diff: f32,
    pub attention_compressor_pending_max_abs_diff: Option<f32>,
    pub indexer_compressor_pending_max_abs_diff: Option<f32>,
    pub indexer_query_max_abs_diff: Option<f32>,
    pub indexer_weights_max_abs_diff: Option<f32>,
    pub reference_raw_kv_checksum: String,
    pub production_raw_kv_checksum: String,
    pub reference_attention_compressor_pending_checksum: Option<String>,
    pub production_attention_compressor_pending_checksum: Option<String>,
    pub reference_indexer_compressor_pending_checksum: Option<String>,
    pub production_indexer_compressor_pending_checksum: Option<String>,
    pub reference_indexer_query_checksum: Option<String>,
    pub production_indexer_query_checksum: Option<String>,
    pub reference_indexer_weights_checksum: Option<String>,
    pub production_indexer_weights_checksum: Option<String>,
    pub dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
    pub tensor_disk_read_bytes: u64,
    pub expert_disk_read_bytes: u64,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialFirstTokenExecution {
    pub model_revision: String,
    pub config_checksum: String,
    pub index_checksum: String,
    pub prompt_token_ids: Vec<u64>,
    pub position: u32,
    pub num_hidden_layers: u64,
    pub reference_embedding_checksum: String,
    pub production_embedding_checksum: String,
    pub layers: Vec<DeepseekV4OfficialLayerAlignment>,
    pub reference_logits_checksum: String,
    pub production_logits_checksum: String,
    pub reference_top_k: Vec<DeepseekV4ReferenceTokenLogit>,
    pub production_top_k: Vec<DeepseekV4ReferenceTokenLogit>,
    pub top_1_token: u64,
    pub logits_max_abs_diff: f32,
    pub total_dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
    pub process_peak_resident_bytes: Option<u64>,
    pub elapsed_seconds: f64,
    pub tensor_disk_read_bytes: u64,
    pub expert_disk_read_bytes: u64,
    pub tensor_cache: DeepseekV4CacheStats,
    pub expert_cache: DeepseekV4CacheStats,
}

fn max_abs_diff(left: &[f32], right: &[f32], label: &str) -> Result<f32, String> {
    if left.len() != right.len() || left.iter().chain(right).any(|value| !value.is_finite()) {
        return Err(format!(
            "deepseek_v4_first_token_{label}_comparison_invalid:left={}:right={}",
            left.len(),
            right.len()
        ));
    }
    Ok(left
        .iter()
        .zip(right)
        .map(|(left, right)| (left - right).abs())
        .fold(0.0f32, f32::max))
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_output_head_through_simpler(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    hidden_hc: &[f32],
    top_k: usize,
) -> Result<DeepseekV4OfficialHeadExecution, String> {
    let hidden_size = usize::try_from(checkpoint.config.hidden_size)
        .map_err(|_| "deepseek_v4_production_hidden_size_overflow".to_string())?;
    let hc_mult = usize::try_from(checkpoint.config.hc_mult)
        .map_err(|_| "deepseek_v4_production_hc_mult_overflow".to_string())?;
    let vocab_size = usize::try_from(checkpoint.config.vocab_size)
        .map_err(|_| "deepseek_v4_production_vocab_size_overflow".to_string())?;
    if hidden_hc.len() != hidden_size * hc_mult || hidden_hc.iter().any(|value| !value.is_finite())
    {
        return Err(format!(
            "deepseek_v4_production_output_head_input_invalid:actual={}:expected={}",
            hidden_hc.len(),
            hidden_size * hc_mult
        ));
    }
    if top_k == 0 || top_k > vocab_size {
        return Err(format!(
            "deepseek_v4_production_output_head_topk_invalid:{top_k}"
        ));
    }
    let tensor_cache_before = checkpoint.cache_stats()?.0;
    let vector_manifest = artifact_dir.join("vector/host_deepseek_vector_manifest.json");
    let mut dispatch_count = 0usize;
    let mut peak_payload_bytes = 0usize;
    let segment_base = task.task_id.saturating_mul(10_000).saturating_add(8_000);
    let control = execute_rms_norm_through_simpler(
        topology,
        task,
        &vector_manifest,
        segment_base,
        hidden_hc,
        None,
        1,
        hidden_hc.len(),
        checkpoint.config.rms_norm_eps as f32,
        false,
    )?;
    dispatch_count += control.dispatch_count;
    peak_payload_bytes = peak_payload_bytes.max(control.peak_payload_bytes);
    let mut hc_task = task.clone();
    hc_task.task_id = task.task_id.saturating_add(dispatch_count as u64);
    let hc = execute_deepseek_official_f32_rows_through_simpler(
        checkpoint,
        topology,
        &hc_task,
        &artifact_dir.join("hc-head/host_fp32_gemm_manifest.json"),
        "hc_head_fn",
        0,
        hc_mult,
        &control.output,
        DeepseekV4LinearOutputDType::F32,
    )?;
    dispatch_count += hc.dispatch_count;
    peak_payload_bytes = peak_payload_bytes.max(hc.peak_tile_payload_bytes);
    let scale = checkpoint.reference_read_vector("hc_head_scale")?;
    let base = checkpoint.reference_read_vector("hc_head_base")?;
    if scale.len() != 1 || base.len() != hc_mult {
        return Err(format!(
            "deepseek_v4_production_output_hc_control_invalid:scale={}:base={}:expected_base={hc_mult}",
            scale.len(),
            base.len()
        ));
    }
    let mut weights_task = task.clone();
    weights_task.task_id = task.task_id.saturating_add(dispatch_count as u64);
    let weights = execute_hc_head_weights_through_simpler(
        topology,
        &weights_task,
        &vector_manifest,
        segment_base.saturating_add(100),
        &hc.output,
        &base,
        scale[0],
        checkpoint.config.hc_eps as f32,
    )?;
    dispatch_count += weights.dispatch_count;
    peak_payload_bytes = peak_payload_bytes.max(weights.peak_payload_bytes);
    let mut weighted_task = task.clone();
    weighted_task.task_id = task.task_id.saturating_add(dispatch_count as u64);
    let hc_head = execute_hc_weighted_sum_through_simpler(
        topology,
        &weighted_task,
        &vector_manifest,
        segment_base.saturating_add(200),
        hidden_hc,
        &weights.output,
        hidden_size,
        true,
    )?;
    dispatch_count += hc_head.dispatch_count;
    peak_payload_bytes = peak_payload_bytes.max(hc_head.peak_payload_bytes);
    let hc_head_checksum = checksum_f32(&hc_head.output);
    let norm = checkpoint.reference_read_vector("norm.weight")?;
    let mut norm_task = task.clone();
    norm_task.task_id = task.task_id.saturating_add(dispatch_count as u64);
    let normalized_hidden = execute_rms_norm_through_simpler(
        topology,
        &norm_task,
        &vector_manifest,
        segment_base.saturating_add(300),
        &hc_head.output,
        Some(&norm),
        1,
        hidden_size,
        checkpoint.config.rms_norm_eps as f32,
        true,
    )?;
    dispatch_count += normalized_hidden.dispatch_count;
    peak_payload_bytes = peak_payload_bytes.max(normalized_hidden.peak_payload_bytes);
    let normalized_hidden_checksum = checksum_f32(&normalized_hidden.output);
    let mut head_task = task.clone();
    head_task.task_id = task.task_id.saturating_add(dispatch_count as u64);
    let head = execute_deepseek_official_bf16_rows_through_simpler(
        checkpoint,
        topology,
        &head_task,
        &artifact_dir.join("head/host_gemm_manifest.json"),
        "head.weight",
        0,
        vocab_size,
        &normalized_hidden.output,
        DeepseekV4LinearOutputDType::F32,
    )?;
    dispatch_count += head.dispatch_count;
    peak_payload_bytes = peak_payload_bytes.max(head.peak_tile_payload_bytes);
    let logits_checksum = checksum_f32(&head.output);
    let mut topk_task = task.clone();
    topk_task.task_id = task.task_id.saturating_add(dispatch_count as u64);
    let (top_k, topk_execution) = execute_top_k_through_simpler(
        topology,
        &topk_task,
        &vector_manifest,
        segment_base.saturating_add(400),
        &head.output,
        top_k,
    )?;
    dispatch_count += topk_execution.dispatch_count;
    peak_payload_bytes = peak_payload_bytes.max(topk_execution.peak_payload_bytes);
    let tensor_cache = checkpoint.cache_stats()?.0;
    let tensor_disk_read_bytes = tensor_cache
        .disk_read_bytes
        .checked_sub(tensor_cache_before.disk_read_bytes)
        .ok_or_else(|| "deepseek_v4_production_head_cache_counter_underflow".to_string())?;
    Ok(DeepseekV4OfficialHeadExecution {
        hc_head_checksum,
        normalized_hidden_checksum,
        logits: head.output,
        logits_checksum,
        top_k,
        dispatch_count,
        peak_tile_payload_bytes: peak_payload_bytes,
        tensor_disk_read_bytes,
        tensor_cache,
    })
}

fn align_layer(
    reference: &sim_models::deepseek_v4_flash_checkpoint_reference::DeepseekV4ReferenceLayerOutput,
    production: &DeepseekV4OfficialLayerExecution,
) -> Result<DeepseekV4OfficialLayerAlignment, String> {
    if reference.layer_id != production.layer_id
        || reference.selected_experts != production.selected_experts
        || reference.kv.raw_rows != production.kv.raw_rows
        || reference.kv.compressed_rows != production.kv.compressed_rows
        || reference.kv.indexer_rows != production.kv.indexer_rows
    {
        return Err(format!(
            "deepseek_v4_first_token_layer_structure_mismatch:reference_layer={}:production_layer={}:reference_experts={:?}:production_experts={:?}",
            reference.layer_id,
            production.layer_id,
            reference.selected_experts,
            production.selected_experts
        ));
    }
    let attention_max_abs_diff = max_abs_diff(
        &reference.attention_output,
        &production.attention_output,
        "attention",
    )?;
    let raw_kv_max_abs_diff = max_abs_diff(&reference.raw_kv, &production.raw_kv, "raw_kv")?;
    let route_weight_max_abs_diff = max_abs_diff(
        &reference.route_weights,
        &production.route_weights,
        "route_weight",
    )?;
    let output_hidden_max_abs_diff = max_abs_diff(
        &reference.layer_output_hidden,
        &production.layer_output_hidden,
        "output_hidden",
    )?;
    let attention_compressor_pending_max_abs_diff = match (
        reference.kv.attention_compressor_pending.as_deref(),
        production.kv.attention_compressor_pending.as_deref(),
    ) {
        (None, None) => None,
        (Some(reference), Some(production)) => Some(max_abs_diff(
            reference,
            production,
            "attention_compressor_pending",
        )?),
        _ => {
            return Err(
                "deepseek_v4_first_token_attention_compressor_presence_mismatch".to_string(),
            )
        }
    };
    let indexer_compressor_pending_max_abs_diff = match (
        reference.kv.indexer_compressor_pending.as_deref(),
        production.kv.indexer_compressor_pending.as_deref(),
    ) {
        (None, None) => None,
        (Some(reference), Some(production)) => Some(max_abs_diff(
            reference,
            production,
            "indexer_compressor_pending",
        )?),
        _ => {
            return Err("deepseek_v4_first_token_indexer_compressor_presence_mismatch".to_string())
        }
    };
    let indexer_query_max_abs_diff = match (
        reference.kv.indexer_query.as_deref(),
        production.kv.indexer_query.as_deref(),
    ) {
        (None, None) => None,
        (Some(reference), Some(production)) => {
            Some(max_abs_diff(reference, production, "indexer_query")?)
        }
        _ => return Err("deepseek_v4_first_token_indexer_query_presence_mismatch".to_string()),
    };
    let indexer_weights_max_abs_diff = match (
        reference.kv.indexer_weights.as_deref(),
        production.kv.indexer_weights.as_deref(),
    ) {
        (None, None) => None,
        (Some(reference), Some(production)) => {
            Some(max_abs_diff(reference, production, "indexer_weights")?)
        }
        _ => return Err("deepseek_v4_first_token_indexer_weights_presence_mismatch".to_string()),
    };
    Ok(DeepseekV4OfficialLayerAlignment {
        layer_id: production.layer_id,
        attention_kind: production.attention_kind.clone(),
        compress_ratio: production.compress_ratio,
        reference_input_hidden_checksum: reference.input_hidden_checksum.clone(),
        production_input_hidden_checksum: production.input_hidden_checksum.clone(),
        reference_output_hidden_checksum: reference.layer_output_hidden_checksum.clone(),
        production_output_hidden_checksum: production.layer_output_hidden_checksum.clone(),
        selected_experts: production.selected_experts.clone(),
        reference_route_weights: reference.route_weights.clone(),
        production_route_weights: production.route_weights.clone(),
        attention_max_abs_diff,
        raw_kv_max_abs_diff,
        route_weight_max_abs_diff,
        output_hidden_max_abs_diff,
        attention_compressor_pending_max_abs_diff,
        indexer_compressor_pending_max_abs_diff,
        indexer_query_max_abs_diff,
        indexer_weights_max_abs_diff,
        reference_raw_kv_checksum: reference.kv.raw_row_checksum.clone(),
        production_raw_kv_checksum: production.kv.raw_row_checksum.clone(),
        reference_attention_compressor_pending_checksum: reference
            .kv
            .attention_compressor_pending_checksum
            .clone(),
        production_attention_compressor_pending_checksum: production
            .kv
            .attention_compressor_pending_checksum
            .clone(),
        reference_indexer_compressor_pending_checksum: reference
            .kv
            .indexer_compressor_pending_checksum
            .clone(),
        production_indexer_compressor_pending_checksum: production
            .kv
            .indexer_compressor_pending_checksum
            .clone(),
        reference_indexer_query_checksum: reference.kv.indexer_query_checksum.clone(),
        production_indexer_query_checksum: production.kv.indexer_query_checksum.clone(),
        reference_indexer_weights_checksum: reference.kv.indexer_weights_checksum.clone(),
        production_indexer_weights_checksum: production.kv.indexer_weights_checksum.clone(),
        dispatch_count: production.dispatch_count,
        peak_tile_payload_bytes: production.peak_tile_payload_bytes,
        tensor_disk_read_bytes: production.tensor_disk_read_bytes,
        expert_disk_read_bytes: production.expert_disk_read_bytes,
    })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_first_token_with_progress_through_simpler<F>(
    reference_checkpoint: &DeepseekV4Checkpoint,
    production_checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    prompt_token_ids: &[u64],
    top_k: usize,
    mut on_layer_complete: F,
) -> Result<DeepseekV4OfficialFirstTokenExecution, String>
where
    F: FnMut(&DeepseekV4OfficialLayerAlignment),
{
    let started_at = Instant::now();
    if prompt_token_ids.len() != 1 {
        return Err(format!(
            "deepseek_v4_first_token_requires_single_raw_prompt_token:actual={}",
            prompt_token_ids.len()
        ));
    }
    if reference_checkpoint.identity != production_checkpoint.identity {
        return Err("deepseek_v4_first_token_checkpoint_identity_mismatch".to_string());
    }
    let token_id = prompt_token_ids[0];
    let mut reference_hidden = reference_checkpoint.reference_embedding_hc(token_id)?;
    let mut production_hidden = production_checkpoint.reference_embedding_hc(token_id)?;
    let reference_embedding_checksum = checksum_f32(&reference_hidden);
    let production_embedding_checksum = checksum_f32(&production_hidden);
    if reference_embedding_checksum != production_embedding_checksum {
        return Err("deepseek_v4_first_token_embedding_mismatch".to_string());
    }

    let mut layers = Vec::with_capacity(production_checkpoint.config.num_hidden_layers as usize);
    let mut total_dispatch_count = 0usize;
    let mut peak_tile_payload_bytes = 0usize;
    let mut tensor_disk_read_bytes = 0u64;
    let mut expert_disk_read_bytes = 0u64;
    for layer_id in 0..production_checkpoint.config.num_hidden_layers {
        let reference = reference_checkpoint.reference_layer_forward(
            layer_id,
            token_id,
            0,
            &reference_hidden,
        )?;
        let mut layer_task = task.clone();
        layer_task.task_id = task.task_id.saturating_add(layer_id.saturating_mul(10_000));
        let production = execute_deepseek_official_layer_through_simpler(
            production_checkpoint,
            topology,
            &layer_task,
            &artifact_dir.join(format!(
                "layer-kind-{}",
                production_checkpoint.config.compress_ratios[layer_id as usize]
            )),
            layer_id.saturating_mul(1_000_000),
            layer_id,
            token_id,
            0,
            &production_hidden,
        )?;
        let alignment = align_layer(&reference, &production)?;
        on_layer_complete(&alignment);
        total_dispatch_count = total_dispatch_count.saturating_add(production.dispatch_count);
        peak_tile_payload_bytes = peak_tile_payload_bytes.max(production.peak_tile_payload_bytes);
        tensor_disk_read_bytes =
            tensor_disk_read_bytes.saturating_add(production.tensor_disk_read_bytes);
        expert_disk_read_bytes =
            expert_disk_read_bytes.saturating_add(production.expert_disk_read_bytes);
        reference_hidden = reference.layer_output_hidden;
        production_hidden = production.layer_output_hidden;
        layers.push(alignment);
    }
    if layers.len() != production_checkpoint.config.num_hidden_layers as usize
        || layers
            .iter()
            .enumerate()
            .any(|(index, layer)| layer.layer_id != index as u64)
    {
        return Err("deepseek_v4_first_token_layer_coverage_invalid".to_string());
    }

    let reference_head = reference_checkpoint.reference_output_head(&reference_hidden, top_k)?;
    let mut head_task = task.clone();
    head_task.task_id = task.task_id.saturating_add(1_000_000);
    let production_head = execute_deepseek_official_output_head_through_simpler(
        production_checkpoint,
        topology,
        &head_task,
        &artifact_dir.join("output-head"),
        &production_hidden,
        top_k,
    )?;
    total_dispatch_count = total_dispatch_count.saturating_add(production_head.dispatch_count);
    peak_tile_payload_bytes = peak_tile_payload_bytes.max(production_head.peak_tile_payload_bytes);
    tensor_disk_read_bytes =
        tensor_disk_read_bytes.saturating_add(production_head.tensor_disk_read_bytes);
    let logits_max_abs_diff =
        max_abs_diff(&reference_head.logits, &production_head.logits, "logits")?;
    let reference_top_1 = reference_head
        .top_k
        .first()
        .ok_or_else(|| "deepseek_v4_first_token_reference_top1_missing".to_string())?
        .token_id;
    let production_top_1 = production_head
        .top_k
        .first()
        .ok_or_else(|| "deepseek_v4_first_token_production_top1_missing".to_string())?
        .token_id;
    if reference_top_1 != production_top_1 {
        return Err(format!(
            "deepseek_v4_first_token_top1_mismatch:reference={reference_top_1}:production={production_top_1}"
        ));
    }
    let (tensor_cache, expert_cache) = production_checkpoint.cache_stats()?;
    Ok(DeepseekV4OfficialFirstTokenExecution {
        model_revision: production_checkpoint.identity.revision.clone(),
        config_checksum: format!(
            "fnv1a64:{:016x}",
            production_checkpoint.identity.config_checksum
        ),
        index_checksum: format!(
            "fnv1a64:{:016x}",
            production_checkpoint.identity.index_checksum
        ),
        prompt_token_ids: prompt_token_ids.to_vec(),
        position: 0,
        num_hidden_layers: production_checkpoint.config.num_hidden_layers,
        reference_embedding_checksum,
        production_embedding_checksum,
        layers,
        reference_logits_checksum: reference_head.logits_checksum,
        production_logits_checksum: production_head.logits_checksum,
        reference_top_k: reference_head.top_k,
        production_top_k: production_head.top_k,
        top_1_token: production_top_1,
        logits_max_abs_diff,
        total_dispatch_count,
        peak_tile_payload_bytes,
        process_peak_resident_bytes: process_peak_resident_bytes(),
        elapsed_seconds: started_at.elapsed().as_secs_f64(),
        tensor_disk_read_bytes,
        expert_disk_read_bytes,
        tensor_cache,
        expert_cache,
    })
}

pub fn validate_deepseek_official_first_token_alignment(
    execution: &DeepseekV4OfficialFirstTokenExecution,
) -> Result<(), String> {
    if execution.model_revision.is_empty()
        || !execution.config_checksum.starts_with("fnv1a64:")
        || !execution.index_checksum.starts_with("fnv1a64:")
        || execution.prompt_token_ids.len() != 1
        || execution.position != 0
        || execution.num_hidden_layers == 0
        || execution.layers.len() != execution.num_hidden_layers as usize
        || execution
            .layers
            .iter()
            .enumerate()
            .any(|(index, layer)| layer.layer_id != index as u64)
    {
        return Err("deepseek_v4_first_token_report_identity_or_coverage_invalid".to_string());
    }
    if execution.reference_embedding_checksum != execution.production_embedding_checksum {
        return Err("deepseek_v4_first_token_report_embedding_checksum_mismatch".to_string());
    }
    for layer in &execution.layers {
        let attention_contract_matches = matches!(
            (layer.attention_kind.as_str(), layer.compress_ratio),
            ("raw", 0) | ("compressed-ratio4", 4) | ("compressed-ratio128", 128)
        );
        if !attention_contract_matches
            || layer.selected_experts.len() != layer.reference_route_weights.len()
            || layer.selected_experts.len() != layer.production_route_weights.len()
            || layer.selected_experts.is_empty()
            || layer.reference_input_hidden_checksum.is_empty()
            || layer.production_input_hidden_checksum.is_empty()
            || layer.reference_output_hidden_checksum.is_empty()
            || layer.production_output_hidden_checksum.is_empty()
            || layer.reference_raw_kv_checksum != layer.production_raw_kv_checksum
            || layer.reference_attention_compressor_pending_checksum
                != layer.production_attention_compressor_pending_checksum
            || layer.reference_indexer_compressor_pending_checksum
                != layer.production_indexer_compressor_pending_checksum
            || layer.reference_indexer_query_checksum != layer.production_indexer_query_checksum
            || layer.reference_indexer_weights_checksum != layer.production_indexer_weights_checksum
            || layer.dispatch_count == 0
            || layer.peak_tile_payload_bytes == 0
            || layer.tensor_disk_read_bytes == 0
            || layer.expert_disk_read_bytes == 0
        {
            return Err(format!(
                "deepseek_v4_first_token_layer_evidence_invalid:layer={}",
                layer.layer_id
            ));
        }
        if layer.attention_max_abs_diff > DEEPSEEK_V4_FIRST_TOKEN_ATTENTION_TOLERANCE
            || layer.raw_kv_max_abs_diff > DEEPSEEK_V4_FIRST_TOKEN_KV_TOLERANCE
            || layer.route_weight_max_abs_diff > DEEPSEEK_V4_FIRST_TOKEN_ROUTE_WEIGHT_TOLERANCE
            || layer.output_hidden_max_abs_diff > DEEPSEEK_V4_FIRST_TOKEN_HIDDEN_TOLERANCE
            || layer
                .attention_compressor_pending_max_abs_diff
                .is_some_and(|difference| difference > DEEPSEEK_V4_FIRST_TOKEN_ATTENTION_TOLERANCE)
            || layer
                .indexer_compressor_pending_max_abs_diff
                .is_some_and(|difference| difference > DEEPSEEK_V4_FIRST_TOKEN_ATTENTION_TOLERANCE)
            || layer
                .indexer_query_max_abs_diff
                .is_some_and(|difference| difference > DEEPSEEK_V4_FIRST_TOKEN_ATTENTION_TOLERANCE)
            || layer
                .indexer_weights_max_abs_diff
                .is_some_and(|difference| {
                    difference > DEEPSEEK_V4_FIRST_TOKEN_ROUTE_WEIGHT_TOLERANCE
                })
        {
            return Err(format!(
                "deepseek_v4_first_token_layer_tolerance_exceeded:layer={}:attention={}:kv={}:route={}:hidden={}",
                layer.layer_id,
                layer.attention_max_abs_diff,
                layer.raw_kv_max_abs_diff,
                layer.route_weight_max_abs_diff,
                layer.output_hidden_max_abs_diff
            ));
        }
    }
    if execution.logits_max_abs_diff > DEEPSEEK_V4_FIRST_TOKEN_LOGIT_TOLERANCE {
        return Err(format!(
            "deepseek_v4_first_token_logits_tolerance_exceeded:{}",
            execution.logits_max_abs_diff
        ));
    }
    let reference_top_1 = execution
        .reference_top_k
        .first()
        .map(|entry| entry.token_id);
    let production_top_1 = execution
        .production_top_k
        .first()
        .map(|entry| entry.token_id);
    if execution.reference_logits_checksum != execution.production_logits_checksum
        || execution.reference_top_k != execution.production_top_k
        || reference_top_1 != Some(execution.top_1_token)
        || production_top_1 != Some(execution.top_1_token)
        || execution.total_dispatch_count == 0
        || execution.peak_tile_payload_bytes == 0
        || execution
            .process_peak_resident_bytes
            .is_none_or(|bytes| bytes == 0)
        || !execution.elapsed_seconds.is_finite()
        || execution.elapsed_seconds <= 0.0
        || execution.tensor_disk_read_bytes == 0
        || execution.expert_disk_read_bytes == 0
        || execution.tensor_cache.peak_resident_bytes > execution.tensor_cache.capacity_bytes
        || execution.expert_cache.peak_resident_bytes > execution.expert_cache.capacity_bytes
    {
        return Err("deepseek_v4_first_token_terminal_evidence_invalid".to_string());
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cache_stats() -> DeepseekV4CacheStats {
        DeepseekV4CacheStats {
            capacity_bytes: 2,
            resident_bytes: 1,
            peak_resident_bytes: 1,
            disk_read_bytes: 1,
            hits: 0,
            misses: 1,
            evictions: 0,
        }
    }

    fn aligned_layer() -> DeepseekV4OfficialLayerAlignment {
        DeepseekV4OfficialLayerAlignment {
            layer_id: 0,
            attention_kind: "raw".to_string(),
            compress_ratio: 0,
            reference_input_hidden_checksum: "input".to_string(),
            production_input_hidden_checksum: "input".to_string(),
            reference_output_hidden_checksum: "output".to_string(),
            production_output_hidden_checksum: "output".to_string(),
            selected_experts: vec![1],
            reference_route_weights: vec![1.0],
            production_route_weights: vec![1.0],
            attention_max_abs_diff: 0.0,
            raw_kv_max_abs_diff: 0.0,
            route_weight_max_abs_diff: 0.0,
            output_hidden_max_abs_diff: 0.0,
            attention_compressor_pending_max_abs_diff: None,
            indexer_compressor_pending_max_abs_diff: None,
            indexer_query_max_abs_diff: None,
            indexer_weights_max_abs_diff: None,
            reference_raw_kv_checksum: "kv".to_string(),
            production_raw_kv_checksum: "kv".to_string(),
            reference_attention_compressor_pending_checksum: None,
            production_attention_compressor_pending_checksum: None,
            reference_indexer_compressor_pending_checksum: None,
            production_indexer_compressor_pending_checksum: None,
            reference_indexer_query_checksum: None,
            production_indexer_query_checksum: None,
            reference_indexer_weights_checksum: None,
            production_indexer_weights_checksum: None,
            dispatch_count: 1,
            peak_tile_payload_bytes: 1,
            tensor_disk_read_bytes: 1,
            expert_disk_read_bytes: 1,
        }
    }

    fn aligned_execution() -> DeepseekV4OfficialFirstTokenExecution {
        let top_k = vec![DeepseekV4ReferenceTokenLogit {
            token_id: 7,
            logit: 1.0,
        }];
        DeepseekV4OfficialFirstTokenExecution {
            model_revision: "revision".to_string(),
            config_checksum: "fnv1a64:config".to_string(),
            index_checksum: "fnv1a64:index".to_string(),
            prompt_token_ids: vec![1],
            position: 0,
            num_hidden_layers: 1,
            reference_embedding_checksum: "embedding".to_string(),
            production_embedding_checksum: "embedding".to_string(),
            layers: vec![aligned_layer()],
            reference_logits_checksum: "logits".to_string(),
            production_logits_checksum: "logits".to_string(),
            reference_top_k: top_k.clone(),
            production_top_k: top_k,
            top_1_token: 7,
            logits_max_abs_diff: 0.0,
            total_dispatch_count: 1,
            peak_tile_payload_bytes: 1,
            process_peak_resident_bytes: Some(1),
            elapsed_seconds: 1.0,
            tensor_disk_read_bytes: 1,
            expert_disk_read_bytes: 1,
            tensor_cache: cache_stats(),
            expert_cache: cache_stats(),
        }
    }

    #[test]
    fn max_abs_diff_rejects_bad_inputs() {
        assert_eq!(max_abs_diff(&[1.0, 2.0], &[1.5, 1.0], "unit").unwrap(), 1.0);
        assert!(max_abs_diff(&[1.0], &[], "unit").is_err());
        assert!(max_abs_diff(&[f32::NAN], &[0.0], "unit").is_err());
    }

    #[test]
    fn first_token_report_validation_fails_closed() {
        let execution = aligned_execution();
        validate_deepseek_official_first_token_alignment(&execution).unwrap();

        let mut missing_layer = execution.clone();
        missing_layer.layers.clear();
        assert!(validate_deepseek_official_first_token_alignment(&missing_layer).is_err());

        let mut bad_checksum = execution.clone();
        bad_checksum.layers[0].production_raw_kv_checksum = "wrong".to_string();
        assert!(validate_deepseek_official_first_token_alignment(&bad_checksum).is_err());

        let mut within_tolerance = execution.clone();
        within_tolerance.layers[0].production_input_hidden_checksum = "input-rounded".to_string();
        within_tolerance.layers[0].production_output_hidden_checksum = "output-rounded".to_string();
        within_tolerance.layers[0].production_route_weights[0] += 1.0e-8;
        within_tolerance.layers[0].route_weight_max_abs_diff = 1.0e-8;
        within_tolerance.layers[0].output_hidden_max_abs_diff = 5.0e-6;
        validate_deepseek_official_first_token_alignment(&within_tolerance).unwrap();

        let mut over_tolerance = within_tolerance;
        over_tolerance.layers[0].route_weight_max_abs_diff = 2.0e-7;
        assert!(validate_deepseek_official_first_token_alignment(&over_tolerance).is_err());

        let mut bad_top_k = execution;
        bad_top_k.production_top_k[0].logit = 0.5;
        assert!(validate_deepseek_official_first_token_alignment(&bad_top_k).is_err());
    }
}
