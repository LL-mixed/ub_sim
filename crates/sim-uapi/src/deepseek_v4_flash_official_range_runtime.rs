//! Stateful layer-range execution for the official DeepSeek V4 Flash checkpoint.

use std::path::Path;

use sim_core::TaskKey;
use sim_models::deepseek_v4_flash_checkpoint::DeepseekV4Checkpoint;
use sim_topology::SimTopology;

use super::{
    execute_deepseek_official_output_head_through_simpler,
    execute_deepseek_official_stateful_layer_through_simpler, DeepseekV4FlashModelState,
};

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4OfficialRangeExecution {
    pub hidden_hc: Vec<f32>,
    pub logits: Option<Vec<f32>>,
    pub routed_expert_bytes: u64,
    pub layer_routes: Vec<Vec<usize>>,
    pub dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4OfficialSequenceExecution {
    pub hidden_hc: Vec<f32>,
    pub logits: Option<Vec<f32>>,
    pub routed_expert_bytes: u64,
    pub token_layer_routes: Vec<Vec<Vec<usize>>>,
    pub dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeepseekV4OfficialRangeProgress {
    pub layer_id: u64,
    pub compression_ratio: u32,
    pub routed_experts: Vec<usize>,
    pub routed_expert_bytes: u64,
    pub raw_rows: usize,
    pub compressed_rows: usize,
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_range_with_progress_through_simpler<F>(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    state: &mut DeepseekV4FlashModelState,
    layer_start: u64,
    layer_end: u64,
    token_id: usize,
    position: u32,
    input_hc: Option<&[f32]>,
    output_logits: bool,
    mut on_layer_complete: F,
) -> Result<DeepseekV4OfficialRangeExecution, String>
where
    F: FnMut(&DeepseekV4OfficialRangeProgress),
{
    let config = &checkpoint.config;
    if layer_start >= layer_end || layer_end > config.num_hidden_layers {
        return Err(format!(
            "deepseek_v4_official_range_invalid:start={layer_start}:end={layer_end}"
        ));
    }
    if token_id as u64 >= config.vocab_size {
        return Err(format!(
            "deepseek_v4_official_token_out_of_range:{token_id}"
        ));
    }
    if output_logits && layer_end != config.num_hidden_layers {
        return Err(format!(
            "deepseek_v4_official_logits_require_terminal_range:end={layer_end}"
        ));
    }
    let expected_hc = usize::try_from(config.hidden_size * config.hc_mult)
        .map_err(|_| "deepseek_v4_official_hc_size_overflow".to_string())?;
    let mut hidden_hc = if layer_start == 0 {
        if input_hc.is_some() {
            return Err("deepseek_v4_official_first_range_rejects_input".to_string());
        }
        checkpoint.reference_embedding_hc(token_id as u64)?
    } else {
        input_hc
            .ok_or_else(|| format!("deepseek_v4_official_range_input_missing:start={layer_start}"))?
            .to_vec()
    };
    if hidden_hc.len() != expected_hc || hidden_hc.iter().any(|value| !value.is_finite()) {
        return Err(format!(
            "deepseek_v4_official_range_input_shape_invalid:actual={}:expected={expected_hc}",
            hidden_hc.len()
        ));
    }

    let mut routed_expert_bytes = 0u64;
    let mut layer_routes = Vec::with_capacity((layer_end - layer_start) as usize);
    let mut dispatch_count = 0usize;
    let mut peak_tile_payload_bytes = 0usize;
    for layer_id in layer_start..layer_end {
        let ratio = config.compress_ratios[layer_id as usize];
        let mut layer_task = task.clone();
        layer_task.task_id = task
            .task_id
            .saturating_add(layer_id.saturating_mul(100_000));
        let execution = execute_deepseek_official_stateful_layer_through_simpler(
            checkpoint,
            topology,
            &layer_task,
            &artifact_dir.join(format!("layer-kind-{ratio}")),
            segment_base.saturating_add(layer_id.saturating_mul(10_000_000)),
            state.layer_mut(layer_id)?,
            layer_id,
            token_id as u64,
            position,
            &hidden_hc,
        )?;
        routed_expert_bytes = routed_expert_bytes.saturating_add(execution.expert_disk_read_bytes);
        dispatch_count = dispatch_count.saturating_add(execution.dispatch_count);
        peak_tile_payload_bytes = peak_tile_payload_bytes.max(execution.peak_tile_payload_bytes);
        layer_routes.push(execution.selected_experts.clone());
        hidden_hc = execution.layer_output_hidden;
        eprintln!(
            "deepseek-v4-official-range-progress: layer={} position={} ratio={} raw_rows={} compressed_rows={} indexer_rows={} routes={:?} hidden_checksum={} dispatches={} routed_expert_bytes={} status=ok",
            layer_id,
            position,
            ratio,
            execution.kv.raw_rows,
            execution.kv.compressed_rows,
            execution.kv.indexer_rows,
            execution.selected_experts,
            execution.layer_output_hidden_checksum,
            execution.dispatch_count,
            routed_expert_bytes,
        );
        on_layer_complete(&DeepseekV4OfficialRangeProgress {
            layer_id,
            compression_ratio: ratio,
            routed_experts: execution.selected_experts,
            routed_expert_bytes,
            raw_rows: execution.kv.raw_rows,
            compressed_rows: execution.kv.compressed_rows,
        });
    }
    let logits = if output_logits {
        let mut head_task = task.clone();
        head_task.task_id = task.task_id.saturating_add(9_000_000);
        let head = execute_deepseek_official_output_head_through_simpler(
            checkpoint,
            topology,
            &head_task,
            &artifact_dir.join("output-head"),
            &hidden_hc,
            4,
        )?;
        dispatch_count = dispatch_count.saturating_add(head.dispatch_count);
        peak_tile_payload_bytes = peak_tile_payload_bytes.max(head.peak_tile_payload_bytes);
        Some(head.logits)
    } else {
        None
    };
    Ok(DeepseekV4OfficialRangeExecution {
        hidden_hc,
        logits,
        routed_expert_bytes,
        layer_routes,
        dispatch_count,
        peak_tile_payload_bytes,
    })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_sequence_range_through_simpler(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    state: &mut DeepseekV4FlashModelState,
    layer_start: u64,
    layer_end: u64,
    token_ids: &[usize],
    position_start: u32,
    input_hc: Option<&[f32]>,
    output_logits: bool,
) -> Result<DeepseekV4OfficialSequenceExecution, String> {
    if token_ids.is_empty() {
        return Err("deepseek_v4_official_sequence_tokens_empty".to_string());
    }
    let expected_hc = usize::try_from(checkpoint.config.hidden_size * checkpoint.config.hc_mult)
        .map_err(|_| "deepseek_v4_official_hc_size_overflow".to_string())?;
    if layer_start == 0 && input_hc.is_some() {
        return Err("deepseek_v4_official_first_range_rejects_input".to_string());
    }
    if layer_start != 0 {
        let expected = token_ids
            .len()
            .checked_mul(expected_hc)
            .ok_or_else(|| "deepseek_v4_official_sequence_hidden_size_overflow".to_string())?;
        if input_hc.map_or(0, <[f32]>::len) != expected {
            return Err(format!(
                "deepseek_v4_official_sequence_hidden_shape_invalid:actual={}:expected={expected}",
                input_hc.map_or(0, <[f32]>::len)
            ));
        }
    }

    let mut hidden_hc = Vec::with_capacity(token_ids.len() * expected_hc);
    let mut logits = None;
    let mut routed_expert_bytes = 0u64;
    let mut token_layer_routes = Vec::with_capacity(token_ids.len());
    let mut dispatch_count = 0usize;
    let mut peak_tile_payload_bytes = 0usize;
    for (token_index, token_id) in token_ids.iter().copied().enumerate() {
        let position = position_start
            .checked_add(token_index as u32)
            .ok_or_else(|| "deepseek_v4_official_sequence_position_overflow".to_string())?;
        let token_input = input_hc.map(|values| {
            let start = token_index * expected_hc;
            &values[start..start + expected_hc]
        });
        let mut token_task = task.clone();
        token_task.task_id = task
            .task_id
            .saturating_add((token_index as u64).saturating_mul(10_000_000));
        let execution = execute_deepseek_official_range_with_progress_through_simpler(
            checkpoint,
            topology,
            &token_task,
            artifact_dir,
            segment_base.saturating_add((token_index as u64).saturating_mul(500_000_000)),
            state,
            layer_start,
            layer_end,
            token_id,
            position,
            token_input,
            output_logits && token_index + 1 == token_ids.len(),
            |_| {},
        )?;
        routed_expert_bytes = routed_expert_bytes.saturating_add(execution.routed_expert_bytes);
        dispatch_count = dispatch_count.saturating_add(execution.dispatch_count);
        peak_tile_payload_bytes = peak_tile_payload_bytes.max(execution.peak_tile_payload_bytes);
        token_layer_routes.push(execution.layer_routes);
        hidden_hc.extend_from_slice(&execution.hidden_hc);
        if execution.logits.is_some() {
            logits = execution.logits;
        }
    }
    Ok(DeepseekV4OfficialSequenceExecution {
        hidden_hc,
        logits,
        routed_expert_bytes,
        token_layer_routes,
        dispatch_count,
        peak_tile_payload_bytes,
    })
}
