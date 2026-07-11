use std::path::Path;

use sim_core::TaskKey;
use sim_models::deepseek_v4_flash::{
    deepseek_v4_flash_layer_compress_ratio, deepseek_v4_flash_rope_coefficients,
    DEEPSEEK_V4_FLASH_PROFILE, DEEPSEEK_V4_FLASH_RMS_EPS, DEEPSEEK_V4_FLASH_SWIGLU_CLAMP,
};
use sim_models::deepseek_v4_flash_gguf::{decode_f16_tensor, decode_f32_tensor, GgufCatalog};
use sim_models::deepseek_v4_flash_lowering::{
    deepseek_v4_flash_hc_weighted_sum_reference, deepseek_v4_flash_rms_norm_reference,
};
use sim_topology::SimTopology;

use super::{
    execute_deepseek_dense_attention_through_simpler,
    execute_deepseek_f16_projection_through_simpler,
    execute_deepseek_q8_projection_through_simpler,
    execute_deepseek_ratio128_attention_through_simpler,
    execute_deepseek_ratio4_attention_through_simpler,
    finish_deepseek_ffn_with_expert_slices_through_simpler, prepare_deepseek_ffn_through_simpler,
    DeepseekV4FlashAttentionWeights, DeepseekV4FlashCompressorWeights,
    DeepseekV4FlashExpertSliceWeights, DeepseekV4FlashFfnExecution,
    DeepseekV4FlashFfnStaticWeights, DeepseekV4FlashIndexerWeights, DeepseekV4FlashLayerState,
    DeepseekV4FlashModelState, DeepseekV4FlashProjectionFormat,
    DeepseekV4FlashRatio128AttentionWeights, DeepseekV4FlashRatio4AttentionWeights,
};

struct OwnedTensor {
    payload: Vec<u8>,
    dimensions: Vec<u64>,
    tensor_type: String,
}

struct OwnedExpertSlice {
    logical_expert: usize,
    gate: Vec<u8>,
    gate_dimensions: Vec<u64>,
    up: Vec<u8>,
    up_dimensions: Vec<u64>,
    down: Vec<u8>,
    down_dimensions: Vec<u64>,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashGgufLayerExecution {
    pub current_kv: Vec<f32>,
    pub attention_output: Vec<f32>,
    pub attention_output_hc: Vec<f32>,
    pub ffn: DeepseekV4FlashFfnExecution,
    pub loaded_routed_expert_bytes: usize,
}

#[derive(Clone, Debug, PartialEq)]
pub struct DeepseekV4FlashGgufRangeExecution {
    pub hidden_hc: Vec<f32>,
    pub logits: Option<Vec<f32>>,
    pub loaded_routed_expert_bytes: usize,
    pub layer_routes: Vec<Vec<usize>>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeepseekV4FlashGgufRangeProgress {
    pub layer_id: u64,
    pub compression_ratio: u32,
    pub routed_experts: Vec<usize>,
    pub loaded_routed_expert_bytes: usize,
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_gguf_layer_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    catalog: &GgufCatalog,
    state: &mut DeepseekV4FlashLayerState,
    layer_id: u64,
    token_id: usize,
    position: u32,
    residual_hc: &[f32],
) -> Result<DeepseekV4FlashGgufLayerExecution, String> {
    let ratio = deepseek_v4_flash_layer_compress_ratio(layer_id)
        .ok_or_else(|| format!("deepseek GGUF layer out of range:{layer_id}"))?;
    let name = |suffix: &str| format!("blk.{layer_id}.{suffix}.weight");
    let tensor = |suffix: &str| -> Result<OwnedTensor, String> {
        let tensor_name = name(suffix);
        let metadata = catalog.tensor(&tensor_name)?;
        Ok(OwnedTensor {
            payload: catalog.read_tensor(&tensor_name)?,
            dimensions: metadata.dimensions.clone(),
            tensor_type: metadata.tensor_type.name.to_string(),
        })
    };
    let decode_f32 = |suffix: &str| -> Result<Vec<f32>, String> {
        let value = tensor(suffix)?;
        if value.tensor_type != "f32" {
            return Err(format!(
                "deepseek GGUF layer f32 tensor required:{suffix}:{}",
                value.tensor_type
            ));
        }
        decode_f32_tensor(&value.payload, &value.dimensions)
    };
    let decode_f16 = |suffix: &str| -> Result<Vec<f32>, String> {
        let value = tensor(suffix)?;
        if value.tensor_type != "f16" {
            return Err(format!(
                "deepseek GGUF layer f16 tensor required:{suffix}:{}",
                value.tensor_type
            ));
        }
        decode_f16_tensor(&value.payload, &value.dimensions)
    };

    let hc_attn = tensor("hc_attn_fn")?;
    let q_a = tensor("attn_q_a")?;
    let q_b = tensor("attn_q_b")?;
    let kv = tensor("attn_kv")?;
    let output_a = tensor("attn_output_a")?;
    let output_b = tensor("attn_output_b")?;
    for (label, value, expected) in [
        ("hc-attn", &hc_attn, "f16"),
        ("q-a", &q_a, "q8_0"),
        ("q-b", &q_b, "q8_0"),
        ("kv", &kv, "q8_0"),
        ("output-a", &output_a, "q8_0"),
        ("output-b", &output_b, "q8_0"),
    ] {
        if value.tensor_type != expected {
            return Err(format!(
                "deepseek GGUF attention tensor type invalid:{label}:actual={}:expected={expected}",
                value.tensor_type
            ));
        }
    }
    let hc_attn_scale = decode_f32("hc_attn_scale")?;
    let hc_attn_base = decode_f32("hc_attn_base")?;
    let attention_norm = decode_f32("attn_norm")?;
    let q_a_norm = decode_f32("attn_q_a_norm")?;
    let kv_norm = decode_f32("attn_kv_a_norm")?;
    let sinks = decode_f32("attn_sinks")?;
    let attention_weights = || DeepseekV4FlashAttentionWeights {
        hc_function: &hc_attn.payload,
        hc_function_dimensions: &hc_attn.dimensions,
        hc_scale: &hc_attn_scale,
        hc_base: &hc_attn_base,
        attention_norm: &attention_norm,
        q_a: &q_a.payload,
        q_a_dimensions: &q_a.dimensions,
        q_a_norm: &q_a_norm,
        q_b: &q_b.payload,
        q_b_dimensions: &q_b.dimensions,
        kv: &kv.payload,
        kv_dimensions: &kv.dimensions,
        kv_norm: &kv_norm,
        sinks: &sinks,
        output_a: &output_a.payload,
        output_a_dimensions: &output_a.dimensions,
        output_b: &output_b.payload,
        output_b_dimensions: &output_b.dimensions,
    };
    let rope = deepseek_v4_flash_rope_coefficients(layer_id, position)?;
    let compressed_position = position.saturating_add(1).saturating_sub(ratio.max(1));
    let compressed_rope = deepseek_v4_flash_rope_coefficients(layer_id, compressed_position)?;
    let profile = DEEPSEEK_V4_FLASH_PROFILE;

    let (current_kv, attention_output, attention_output_hc) = match (ratio, state) {
        (0, DeepseekV4FlashLayerState::Dense(state)) => {
            let execution = execute_deepseek_dense_attention_through_simpler(
                topology,
                task,
                &artifact_dir.join("attention"),
                segment_base,
                &attention_weights(),
                residual_hc,
                state.raw_kv(),
                &rope.cos,
                &rope.sin,
                profile.hc_mult as usize,
                profile.hc_sinkhorn_iters as usize,
                profile.num_attention_heads as usize,
                profile.head_dim as usize,
                profile.qk_rope_head_dim as usize,
                profile.output_groups as usize,
                DEEPSEEK_V4_FLASH_RMS_EPS,
            )?;
            state.push_raw(&execution.current_kv)?;
            (
                execution.current_kv,
                execution.attention_output,
                execution.output_hc,
            )
        }
        (4, DeepseekV4FlashLayerState::Ratio4(state)) => {
            let attention_compressor_kv = tensor("attn_compressor_kv")?;
            let attention_compressor_gate = tensor("attn_compressor_gate")?;
            let indexer_compressor_kv = tensor("indexer_compressor_kv")?;
            let indexer_compressor_gate = tensor("indexer_compressor_gate")?;
            let indexer_query = tensor("indexer.attn_q_b")?;
            let indexer_head_weights = tensor("indexer.proj")?;
            let query_format = match indexer_query.tensor_type.as_str() {
                "f16" => DeepseekV4FlashProjectionFormat::F16,
                "q8_0" => DeepseekV4FlashProjectionFormat::Q8_0,
                other => {
                    return Err(format!(
                        "deepseek GGUF indexer query type invalid:{other}"
                    ))
                }
            };
            let execution = execute_deepseek_ratio4_attention_through_simpler(
                topology,
                task,
                &artifact_dir.join("attention"),
                segment_base,
                state,
                &DeepseekV4FlashRatio4AttentionWeights {
                    attention: attention_weights(),
                    attention_compressor: DeepseekV4FlashCompressorWeights {
                        kv: &attention_compressor_kv.payload,
                        kv_dimensions: &attention_compressor_kv.dimensions,
                        gate: &attention_compressor_gate.payload,
                        gate_dimensions: &attention_compressor_gate.dimensions,
                        ape: &decode_f16("attn_compressor_ape")?,
                        norm: &decode_f32("attn_compressor_norm")?,
                    },
                    indexer_compressor: DeepseekV4FlashCompressorWeights {
                        kv: &indexer_compressor_kv.payload,
                        kv_dimensions: &indexer_compressor_kv.dimensions,
                        gate: &indexer_compressor_gate.payload,
                        gate_dimensions: &indexer_compressor_gate.dimensions,
                        ape: &decode_f16("indexer_compressor_ape")?,
                        norm: &decode_f32("indexer_compressor_norm")?,
                    },
                    indexer: DeepseekV4FlashIndexerWeights {
                        query: &indexer_query.payload,
                        query_dimensions: &indexer_query.dimensions,
                        query_format,
                        head_weights: &indexer_head_weights.payload,
                        head_weight_dimensions: &indexer_head_weights.dimensions,
                    },
                },
                residual_hc,
                position,
                &rope.cos,
                &rope.sin,
                &compressed_rope.cos,
                &compressed_rope.sin,
                profile.hc_mult as usize,
                profile.hc_sinkhorn_iters as usize,
                profile.num_attention_heads as usize,
                profile.head_dim as usize,
                profile.qk_rope_head_dim as usize,
                profile.output_groups as usize,
                profile.indexer_heads as usize,
                profile.indexer_head_dim as usize,
                profile.indexer_top_k as usize,
                DEEPSEEK_V4_FLASH_RMS_EPS,
            )?;
            (
                execution.current_kv,
                execution.attention_output,
                execution.output_hc,
            )
        }
        (128, DeepseekV4FlashLayerState::Ratio128(state)) => {
            let compressor_kv = tensor("attn_compressor_kv")?;
            let compressor_gate = tensor("attn_compressor_gate")?;
            let execution = execute_deepseek_ratio128_attention_through_simpler(
                topology,
                task,
                &artifact_dir.join("attention"),
                segment_base,
                state,
                &DeepseekV4FlashRatio128AttentionWeights {
                    attention: attention_weights(),
                    attention_compressor: DeepseekV4FlashCompressorWeights {
                        kv: &compressor_kv.payload,
                        kv_dimensions: &compressor_kv.dimensions,
                        gate: &compressor_gate.payload,
                        gate_dimensions: &compressor_gate.dimensions,
                        ape: &decode_f16("attn_compressor_ape")?,
                        norm: &decode_f32("attn_compressor_norm")?,
                    },
                },
                residual_hc,
                position,
                &rope.cos,
                &rope.sin,
                &compressed_rope.cos,
                &compressed_rope.sin,
                profile.hc_mult as usize,
                profile.hc_sinkhorn_iters as usize,
                profile.num_attention_heads as usize,
                profile.head_dim as usize,
                profile.qk_rope_head_dim as usize,
                profile.output_groups as usize,
                DEEPSEEK_V4_FLASH_RMS_EPS,
            )?;
            (
                execution.current_kv,
                execution.attention_output,
                execution.output_hc,
            )
        }
        (_, state) => {
            return Err(format!(
                "deepseek GGUF layer state kind mismatch:layer={layer_id}:ratio={ratio}:state={state:?}"
            ))
        }
    };

    let hc_ffn = tensor("hc_ffn_fn")?;
    let router = tensor("ffn_gate_inp")?;
    let shared_gate = tensor("ffn_gate_shexp")?;
    let shared_up = tensor("ffn_up_shexp")?;
    let shared_down = tensor("ffn_down_shexp")?;
    let hc_ffn_scale = decode_f32("hc_ffn_scale")?;
    let hc_ffn_base = decode_f32("hc_ffn_base")?;
    let ffn_norm = decode_f32("ffn_norm")?;
    let static_weights = DeepseekV4FlashFfnStaticWeights {
        hc_function: &hc_ffn.payload,
        hc_function_dimensions: &hc_ffn.dimensions,
        hc_scale: &hc_ffn_scale,
        hc_base: &hc_ffn_base,
        ffn_norm: &ffn_norm,
        router: &router.payload,
        router_dimensions: &router.dimensions,
        shared_gate: &shared_gate.payload,
        shared_gate_dimensions: &shared_gate.dimensions,
        shared_up: &shared_up.payload,
        shared_up_dimensions: &shared_up.dimensions,
        shared_down: &shared_down.payload,
        shared_down_dimensions: &shared_down.dimensions,
    };
    let selection_bias_name = name("ffn_exp_probs_b");
    let selection_bias = if catalog.tensors.contains_key(&selection_bias_name) {
        let metadata = catalog.tensor(&selection_bias_name)?;
        Some(decode_f32_tensor(
            &catalog.read_tensor(&selection_bias_name)?,
            &metadata.dimensions,
        )?)
    } else {
        None
    };
    let hash_selected = if layer_id < profile.num_hash_layers {
        Some(read_hash_router_experts(catalog, layer_id, token_id)?)
    } else {
        None
    };
    let mut ffn_task = task.clone();
    ffn_task.task_id = task.task_id.saturating_add(1_000);
    let ffn_artifacts = artifact_dir.join("ffn");
    let prepared = prepare_deepseek_ffn_through_simpler(
        topology,
        &ffn_task,
        &ffn_artifacts,
        &static_weights,
        &attention_output_hc,
        selection_bias.as_deref(),
        hash_selected.as_deref(),
        profile.hc_mult as usize,
        profile.hc_sinkhorn_iters as usize,
        DEEPSEEK_V4_FLASH_RMS_EPS,
    )?;
    let gate_name = name("ffn_gate_exps");
    let up_name = name("ffn_up_exps");
    let down_name = name("ffn_down_exps");
    let mut owned_experts = Vec::with_capacity(prepared.router.expert_indices.len());
    for &expert in &prepared.router.expert_indices {
        let (gate, gate_dimensions) = catalog.read_expert_tensor_slice(&gate_name, expert)?;
        let (up, up_dimensions) = catalog.read_expert_tensor_slice(&up_name, expert)?;
        let (down, down_dimensions) = catalog.read_expert_tensor_slice(&down_name, expert)?;
        owned_experts.push(OwnedExpertSlice {
            logical_expert: expert,
            gate,
            gate_dimensions,
            up,
            up_dimensions,
            down,
            down_dimensions,
        });
    }
    let loaded_routed_expert_bytes = owned_experts
        .iter()
        .map(|expert| expert.gate.len() + expert.up.len() + expert.down.len())
        .sum();
    let expert_slices = owned_experts
        .iter()
        .map(|expert| DeepseekV4FlashExpertSliceWeights {
            logical_expert: expert.logical_expert,
            gate: &expert.gate,
            gate_dimensions: &expert.gate_dimensions,
            up: &expert.up,
            up_dimensions: &expert.up_dimensions,
            down: &expert.down,
            down_dimensions: &expert.down_dimensions,
        })
        .collect::<Vec<_>>();
    let ffn = finish_deepseek_ffn_with_expert_slices_through_simpler(
        topology,
        &ffn_task,
        &ffn_artifacts,
        segment_base.saturating_add(100_000),
        &static_weights,
        prepared,
        &expert_slices,
        DEEPSEEK_V4_FLASH_SWIGLU_CLAMP,
    )?;
    Ok(DeepseekV4FlashGgufLayerExecution {
        current_kv,
        attention_output,
        attention_output_hc,
        ffn,
        loaded_routed_expert_bytes,
    })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_gguf_range_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    catalog: &GgufCatalog,
    state: &mut DeepseekV4FlashModelState,
    layer_start: u64,
    layer_end: u64,
    token_id: usize,
    position: u32,
    input_hc: Option<&[f32]>,
    output_logits: bool,
) -> Result<DeepseekV4FlashGgufRangeExecution, String> {
    execute_deepseek_gguf_range_with_progress_through_simpler(
        topology,
        task,
        artifact_dir,
        segment_base,
        catalog,
        state,
        layer_start,
        layer_end,
        token_id,
        position,
        input_hc,
        output_logits,
        |_| {},
    )
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_gguf_range_with_progress_through_simpler<F>(
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    catalog: &GgufCatalog,
    state: &mut DeepseekV4FlashModelState,
    layer_start: u64,
    layer_end: u64,
    token_id: usize,
    position: u32,
    input_hc: Option<&[f32]>,
    output_logits: bool,
    mut on_layer_complete: F,
) -> Result<DeepseekV4FlashGgufRangeExecution, String>
where
    F: FnMut(&DeepseekV4FlashGgufRangeProgress),
{
    if layer_start >= layer_end || layer_end > DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!(
            "deepseek GGUF range invalid:start={layer_start}:end={layer_end}"
        ));
    }
    if output_logits && layer_end != DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!(
            "deepseek GGUF logits require terminal range:end={layer_end}"
        ));
    }
    let mut hidden_hc = if layer_start == 0 {
        if input_hc.is_some() {
            return Err("deepseek GGUF first range rejects external hidden input".to_string());
        }
        deepseek_v4_flash_embedding_hc(catalog, token_id)?
    } else {
        input_hc
            .ok_or_else(|| format!("deepseek GGUF range input hidden missing:start={layer_start}"))?
            .to_vec()
    };
    let expected_hc =
        (DEEPSEEK_V4_FLASH_PROFILE.hidden_size * DEEPSEEK_V4_FLASH_PROFILE.hc_mult) as usize;
    if hidden_hc.len() != expected_hc {
        return Err(format!(
            "deepseek GGUF range input shape mismatch:actual={}:expected={expected_hc}",
            hidden_hc.len()
        ));
    }
    let mut loaded_routed_expert_bytes = 0usize;
    let mut layer_routes = Vec::with_capacity((layer_end - layer_start) as usize);
    for layer_id in layer_start..layer_end {
        let ratio = deepseek_v4_flash_layer_compress_ratio(layer_id)
            .ok_or_else(|| format!("deepseek GGUF range layer out of range:{layer_id}"))?;
        let mut layer_task = task.clone();
        layer_task.task_id = task.task_id.saturating_add(layer_id.saturating_mul(2_000));
        let execution = execute_deepseek_gguf_layer_through_simpler(
            topology,
            &layer_task,
            &artifact_dir.join(format!("layer-kind-{ratio}")),
            segment_base.saturating_add(layer_id.saturating_mul(200_000)),
            catalog,
            state.layer_mut(layer_id)?,
            layer_id,
            token_id,
            position,
            &hidden_hc,
        )?;
        loaded_routed_expert_bytes =
            loaded_routed_expert_bytes.saturating_add(execution.loaded_routed_expert_bytes);
        let routed_experts = execution.ffn.router.expert_indices.clone();
        layer_routes.push(routed_experts.clone());
        hidden_hc = execution.ffn.output_hc;
        on_layer_complete(&DeepseekV4FlashGgufRangeProgress {
            layer_id,
            compression_ratio: ratio,
            routed_experts,
            loaded_routed_expert_bytes,
        });
    }
    let logits = output_logits
        .then(|| {
            execute_deepseek_gguf_output_head_through_simpler(
                topology,
                task,
                &artifact_dir.join("output-head"),
                segment_base.saturating_add(10_000_000),
                catalog,
                &hidden_hc,
            )
        })
        .transpose()?;
    Ok(DeepseekV4FlashGgufRangeExecution {
        hidden_hc,
        logits,
        loaded_routed_expert_bytes,
        layer_routes,
    })
}

pub fn deepseek_v4_flash_embedding_hc(
    catalog: &GgufCatalog,
    token_id: usize,
) -> Result<Vec<f32>, String> {
    if token_id >= DEEPSEEK_V4_FLASH_PROFILE.vocab_size as usize {
        return Err(format!("deepseek embedding token out of range:{token_id}"));
    }
    let embedding = catalog.read_f16_matrix_row("token_embd.weight", token_id)?;
    if embedding.len() != DEEPSEEK_V4_FLASH_PROFILE.hidden_size as usize {
        return Err(format!(
            "deepseek embedding width mismatch:actual={}:expected={}",
            embedding.len(),
            DEEPSEEK_V4_FLASH_PROFILE.hidden_size
        ));
    }
    let mut hidden_hc =
        Vec::with_capacity(embedding.len() * DEEPSEEK_V4_FLASH_PROFILE.hc_mult as usize);
    for _ in 0..DEEPSEEK_V4_FLASH_PROFILE.hc_mult {
        hidden_hc.extend_from_slice(&embedding);
    }
    Ok(hidden_hc)
}

pub fn execute_deepseek_gguf_output_head_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    catalog: &GgufCatalog,
    hidden_hc: &[f32],
) -> Result<Vec<f32>, String> {
    let expected_hc =
        (DEEPSEEK_V4_FLASH_PROFILE.hidden_size * DEEPSEEK_V4_FLASH_PROFILE.hc_mult) as usize;
    if hidden_hc.len() != expected_hc {
        return Err(format!(
            "deepseek output head HC shape mismatch:actual={}:expected={expected_hc}",
            hidden_hc.len()
        ));
    }
    let hc_function = catalog.tensor("output_hc_fn.weight")?;
    if hc_function.tensor_type.name != "f16" {
        return Err(format!(
            "deepseek output HC function type invalid:{}",
            hc_function.tensor_type.name
        ));
    }
    let hc_function_payload = catalog.read_tensor(&hc_function.name)?;
    let decode_f32 = |name: &str| -> Result<Vec<f32>, String> {
        let tensor = catalog.tensor(name)?;
        if tensor.tensor_type.name != "f32" {
            return Err(format!(
                "deepseek output f32 tensor required:{name}:{}",
                tensor.tensor_type.name
            ));
        }
        decode_f32_tensor(&catalog.read_tensor(name)?, &tensor.dimensions)
    };
    let flat = deepseek_v4_flash_rms_norm_reference(hidden_hc, None, DEEPSEEK_V4_FLASH_RMS_EPS)?;
    let pre = execute_deepseek_f16_projection_through_simpler(
        topology,
        task,
        &artifact_dir.join("hc/host_fp32_gemm_manifest.json"),
        &hc_function_payload,
        &hc_function.dimensions,
        &flat,
    )?;
    let scale = decode_f32("output_hc_scale.weight")?;
    let base = decode_f32("output_hc_base.weight")?;
    if scale.len() != 1 || base.len() != DEEPSEEK_V4_FLASH_PROFILE.hc_mult as usize {
        return Err(format!(
            "deepseek output HC control shape mismatch:scale={}:base={}",
            scale.len(),
            base.len()
        ));
    }
    let weights = pre
        .iter()
        .zip(base)
        .map(|(value, base)| stable_sigmoid(value * scale[0] + base) + DEEPSEEK_V4_FLASH_RMS_EPS)
        .collect::<Vec<_>>();
    let embedding = deepseek_v4_flash_hc_weighted_sum_reference(
        hidden_hc,
        &weights,
        DEEPSEEK_V4_FLASH_PROFILE.hidden_size as usize,
    )?;
    let output_norm = decode_f32("output_norm.weight")?;
    let normalized = deepseek_v4_flash_rms_norm_reference(
        &embedding,
        Some(&output_norm),
        DEEPSEEK_V4_FLASH_RMS_EPS,
    )?;
    let output = catalog.tensor("output.weight")?;
    if output.tensor_type.name != "q8_0" {
        return Err(format!(
            "deepseek output projection type invalid:{}",
            output.tensor_type.name
        ));
    }
    execute_deepseek_q8_projection_through_simpler(
        topology,
        task,
        &artifact_dir.join("projection/host_q8_block_dot_manifest.json"),
        segment_base,
        &catalog.read_tensor(&output.name)?,
        &output.dimensions,
        &normalized,
    )
}

fn stable_sigmoid(value: f32) -> f32 {
    if value >= 0.0 {
        1.0 / (1.0 + (-value).exp())
    } else {
        let exp = value.exp();
        exp / (1.0 + exp)
    }
}

fn read_hash_router_experts(
    catalog: &GgufCatalog,
    layer_id: u64,
    token_id: usize,
) -> Result<Vec<usize>, String> {
    let name = format!("blk.{layer_id}.ffn_gate_tid2eid.weight");
    let tensor = catalog.tensor(&name)?;
    let top_k = DEEPSEEK_V4_FLASH_PROFILE.num_experts_used as usize;
    if tensor.tensor_type.name != "i32"
        || tensor.dimensions.first().copied() != Some(top_k as u64)
        || token_id as u64 >= tensor.dimensions.get(1).copied().unwrap_or(0)
    {
        return Err(format!(
            "deepseek hash router table invalid:{name}:token={token_id}:dimensions={:?}:type={}",
            tensor.dimensions, tensor.tensor_type.name
        ));
    }
    let offset = token_id
        .checked_mul(top_k)
        .and_then(|value| value.checked_mul(4))
        .ok_or_else(|| "deepseek hash router offset overflow".to_string())?;
    catalog
        .read_tensor_byte_range(&name, offset as u64, (top_k * 4) as u64)?
        .chunks_exact(4)
        .map(|chunk| {
            let expert = i32::from_le_bytes(chunk.try_into().expect("four bytes"));
            usize::try_from(expert)
                .map_err(|_| format!("deepseek hash router expert invalid:{expert}"))
        })
        .collect()
}
