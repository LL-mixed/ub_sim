//! A5 vector-kernel dispatches for the official DeepSeek V4 Flash checkpoint.

use std::path::Path;

use serde::Serialize;
use sim_core::{
    BackendDispatchOperation, BufferUsage, CompletionStatus, DispatchBackendProfile,
    DispatchBackendSpec, DispatchRuntimeVariant, FunctionLabel, MemoryEndpoint, PlLevel,
    SegmentHandle, SimplerRuntimeArg, SimplerRuntimeArtifacts, TaskKey,
};
use sim_models::deepseek_v4_flash_checkpoint_reference::DeepseekV4ReferenceTokenLogit;
use sim_models::deepseek_v4_flash_lowering::{DeepseekV4FlashHcSplit, DeepseekV4FlashRouterOutput};
use sim_runtime::{LocalRuntimeEngine, VecEventSink};
use sim_topology::SimTopology;

use super::{
    bytes_to_f32s, f32s_to_bytes, host_vector_dispatch_lock_guard, kvcache_host_matmul_request,
    opaque_binding, scenario_config_for_chipbackend, simpler_host_artifact_producer_path,
    with_suppressed_stdio, SimplerRuntimeManifestEnvelope,
};

const RMS_NORM: u64 = 1;
const HC_SPLIT: u64 = 2;
const HC_WEIGHTED_SUM: u64 = 3;
const HC_POST: u64 = 4;
const ROPE: u64 = 5;
const KV_FP8_ROUNDTRIP: u64 = 6;
const SINK_ATTENTION: u64 = 7;
const INDEXER_QAT: u64 = 8;
const SCALE: u64 = 9;
const SWIGLU: u64 = 10;
const ADD: u64 = 11;
const ROUTER: u64 = 12;
const TOP_K: u64 = 13;
const HC_HEAD_WEIGHTS: u64 = 14;
const COMPRESSOR_POOL: u64 = 15;

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialVectorExecution {
    pub output: Vec<f32>,
    pub dispatch_count: usize,
    pub peak_payload_bytes: usize,
}

pub fn ensure_simpler_host_deepseek_vector_manifest(manifest_path: &Path) -> Result<(), String> {
    if let Ok(text) = std::fs::read_to_string(manifest_path) {
        if let (Ok(manifest), Ok(value)) = (
            serde_json::from_str::<SimplerRuntimeManifestEnvelope>(&text),
            serde_json::from_str::<serde_json::Value>(&text),
        ) {
            if value["host_deepseek_vector_manifest_version"].as_u64() == Some(13)
                && manifest.platform.as_deref() == Some("a5sim")
                && manifest.simpler_runtime.orch_function_name == "build_deepseek_vector_graph"
            {
                return Ok(());
            }
        }
    }
    let output_dir = manifest_path.parent().ok_or_else(|| {
        format!(
            "host_deepseek_vector_manifest_has_no_parent:{}",
            manifest_path.display()
        )
    })?;
    let status = std::process::Command::new("python3")
        .arg(simpler_host_artifact_producer_path())
        .arg("--profile")
        .arg("host_deepseek_vector")
        .arg("--output-dir")
        .arg(output_dir)
        .arg("--platform")
        .arg("a5sim")
        .status()
        .map_err(|err| format!("run_simpler_host_deepseek_vector_producer_failed:{err}"))?;
    if !status.success() || !manifest_path.exists() {
        return Err(format!(
            "simpler_host_deepseek_vector_producer_failed:status={status}"
        ));
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn execute_vector(
    topology: &SimTopology,
    task: &TaskKey,
    manifest_path: &Path,
    segment_base: u64,
    operation: u64,
    input0: &[f32],
    input1: &[f32],
    input2: &[f32],
    output_len: usize,
    params: [u64; 4],
    floats: [f32; 2],
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if output_len == 0
        || input0
            .iter()
            .chain(input1)
            .chain(input2)
            .any(|value| !value.is_finite())
    {
        return Err(format!(
            "deepseek_v4_vector_request_invalid:operation={operation}:output={output_len}"
        ));
    }
    ensure_simpler_host_deepseek_vector_manifest(manifest_path)?;
    let _dispatch_lock = host_vector_dispatch_lock_guard()?;
    let scenario_config = scenario_config_for_chipbackend()?;
    let host_node = topology
        .hosts
        .first()
        .map(|host| host.node_id)
        .ok_or_else(|| "missing_host_node".to_string())?;
    let ubpu_node = topology
        .ubpus
        .first()
        .map(|ubpu| ubpu.node_id)
        .ok_or_else(|| "missing_ubpu_node".to_string())?;
    let mut runtime = LocalRuntimeEngine::from_config(&scenario_config);
    let mut sink = VecEventSink::default();
    let handles = [
        SegmentHandle(segment_base + 1),
        SegmentHandle(segment_base + 2),
        SegmentHandle(segment_base + 3),
        SegmentHandle(segment_base + 4),
    ];
    let padded = |values: &[f32]| {
        if values.is_empty() {
            f32s_to_bytes(&[0.0])
        } else {
            f32s_to_bytes(values)
        }
    };
    runtime.seed_host_segment(host_node, handles[0], padded(input0));
    runtime.seed_host_segment(host_node, handles[1], padded(input1));
    runtime.seed_host_segment(host_node, handles[2], padded(input2));
    runtime.seed_host_segment(host_node, handles[3], vec![0; output_len * 4]);
    let endpoint = |segment| MemoryEndpoint {
        node: host_node,
        segment,
        offset: 0,
    };
    let endpoints = handles.map(endpoint);
    let text = std::fs::read_to_string(manifest_path).map_err(|err| {
        format!(
            "read_simpler_deepseek_vector_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let manifest: SimplerRuntimeManifestEnvelope = serde_json::from_str(&text).map_err(|err| {
        format!(
            "parse_simpler_deepseek_vector_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    if manifest.platform.as_deref() != Some("a5sim")
        || manifest.simpler_runtime.orch_function_name != "build_deepseek_vector_graph"
    {
        return Err("simpler_deepseek_vector_manifest_contract_mismatch".to_string());
    }
    let input_bytes = [
        input0.len().max(1) * 4,
        input1.len().max(1) * 4,
        input2.len().max(1) * 4,
    ];
    let output_bytes = output_len * 4;
    let mut args = Vec::with_capacity(15);
    for index in 0..3 {
        args.push(SimplerRuntimeArg::InputSegment {
            endpoint: endpoints[index].clone(),
            bytes: input_bytes[index] as u64,
        });
    }
    args.push(SimplerRuntimeArg::OutputSegment {
        endpoint: endpoints[3].clone(),
        bytes: output_bytes as u64,
    });
    for scalar in [
        operation,
        input0.len() as u64,
        input1.len() as u64,
        input2.len() as u64,
        output_len as u64,
        params[0],
        params[1],
        params[2],
        params[3],
        floats[0].to_bits() as u64,
        floats[1].to_bits() as u64,
    ] {
        args.push(SimplerRuntimeArg::ScalarU64(scalar));
    }
    let simpler_runtime = SimplerRuntimeArtifacts {
        host_runtime_library: manifest.simpler_runtime.host_runtime_library,
        orch_shared_object: manifest.simpler_runtime.orch_shared_object,
        orch_function_name: manifest.simpler_runtime.orch_function_name,
        aicpu_binary: manifest.simpler_runtime.aicpu_binary,
        aicore_binary: manifest.simpler_runtime.aicore_binary,
        kernels: manifest.simpler_runtime.kernels,
        launch: manifest.simpler_runtime.launch,
        runtime_env: manifest.simpler_runtime.runtime_env,
        args,
    };
    let backend_spec = DispatchBackendSpec {
        profile: DispatchBackendProfile::HostVector,
        platform: "a5sim".to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_deepseek_vector".to_string()),
        simpler_runtime: Some(simpler_runtime),
        context: None,
    };
    let mut bindings = Vec::with_capacity(4);
    for index in 0..3 {
        bindings.push(opaque_binding(
            format!("deepseek_vector_input{index}"),
            BufferUsage::Input,
            endpoints[index].clone(),
            input_bytes[index] as u64,
        ));
    }
    bindings.push(opaque_binding(
        "deepseek_vector_output",
        BufferUsage::Output,
        endpoints[3].clone(),
        output_bytes as u64,
    ));
    let dispatch = BackendDispatchOperation {
        task: task.clone(),
        function: FunctionLabel {
            name: "host_deepseek_vector".into(),
            level: PlLevel::L2,
        },
        backend_spec,
        request: kvcache_host_matmul_request(task.task_id, bindings),
        target_level: PlLevel::L2,
        target_node: ubpu_node,
        legacy_input_segments: handles[..3].to_vec(),
    };
    let now_us = scenario_config
        .pypto
        .simpler_boundary
        .dispatch_latency_us
        .unwrap_or(15);
    let completion = with_suppressed_stdio(|| {
        runtime
            .submit_backend_dispatch(dispatch, &mut sink)
            .map_err(|err| err.to_string())?;
        runtime.advance_to(now_us, &mut sink);
        runtime
            .poll_completions(now_us, &mut sink)
            .into_iter()
            .next()
            .ok_or_else(|| "deepseek_vector_dispatch_did_not_complete".to_string())
    })?;
    if completion.status != CompletionStatus::Success {
        return Err(format!(
            "deepseek_vector_dispatch_failed:operation={operation}:status={:?}",
            completion.status
        ));
    }
    let payload = runtime
        .host_segment_payload(host_node, handles[3])
        .ok_or_else(|| "missing_deepseek_vector_output_payload".to_string())?;
    let output = bytes_to_f32s(payload);
    if output.len() != output_len || output.iter().any(|value| !value.is_finite()) {
        return Err(format!(
            "deepseek_vector_output_invalid:operation={operation}:actual={}:expected={output_len}",
            output.len()
        ));
    }
    Ok(DeepseekV4OfficialVectorExecution {
        output,
        dispatch_count: 1,
        peak_payload_bytes: input_bytes.iter().sum::<usize>() + output_bytes,
    })
}

pub fn execute_rms_norm_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    input: &[f32],
    weight: Option<&[f32]>,
    groups: usize,
    width: usize,
    eps: f32,
    bf16: bool,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if groups.checked_mul(width) != Some(input.len())
        || weight.is_some_and(|weight| weight.len() != width)
    {
        return Err("deepseek_vector_rms_norm_shape_invalid".to_string());
    }
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        RMS_NORM,
        input,
        weight.unwrap_or(&[]),
        &[],
        input.len(),
        [
            groups as u64,
            width as u64,
            u64::from(weight.is_some()),
            u64::from(bf16),
        ],
        [eps, 0.0],
    )
}

pub fn execute_hc_split_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    mix: &[f32],
    scale: &[f32],
    base: &[f32],
    hc: usize,
    iterations: usize,
    eps: f32,
) -> Result<(DeepseekV4FlashHcSplit, DeepseekV4OfficialVectorExecution), String> {
    let expected = (2 + hc) * hc;
    if mix.len() != expected || scale.len() != 3 || base.len() != expected {
        return Err("deepseek_vector_hc_split_shape_invalid".to_string());
    }
    let execution = execute_vector(
        topology,
        task,
        manifest,
        segment,
        HC_SPLIT,
        mix,
        scale,
        base,
        expected,
        [hc as u64, iterations as u64, 0, 0],
        [eps, 0.0],
    )?;
    let split = DeepseekV4FlashHcSplit {
        pre: execution.output[..hc].to_vec(),
        post: execution.output[hc..2 * hc].to_vec(),
        combine: execution.output[2 * hc..].to_vec(),
    };
    Ok((split, execution))
}

pub fn execute_hc_weighted_sum_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    hidden_hc: &[f32],
    weights: &[f32],
    hidden: usize,
    bf16: bool,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if hidden_hc.len() != hidden * weights.len() {
        return Err("deepseek_vector_hc_weighted_sum_shape_invalid".to_string());
    }
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        HC_WEIGHTED_SUM,
        hidden_hc,
        weights,
        &[],
        hidden,
        [hidden as u64, weights.len() as u64, u64::from(bf16), 0],
        [0.0, 0.0],
    )
}

pub fn execute_hc_post_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    branch: &[f32],
    residual: &[f32],
    post: &[f32],
    combine: &[f32],
    hidden: usize,
    bf16: bool,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    let hc = post.len();
    if branch.len() != hidden || residual.len() != hidden * hc || combine.len() != hc * hc {
        return Err("deepseek_vector_hc_post_shape_invalid".to_string());
    }
    let mut controls = post.to_vec();
    controls.extend_from_slice(combine);
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        HC_POST,
        branch,
        residual,
        &controls,
        residual.len(),
        [hidden as u64, hc as u64, u64::from(bf16), 0],
        [0.0, 0.0],
    )
}

pub fn execute_rope_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    input: &[f32],
    cos: &[f32],
    sin: &[f32],
    heads: usize,
    head_dim: usize,
    rope_dim: usize,
    inverse: bool,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if input.len() != heads * head_dim || cos.len() != rope_dim / 2 || sin.len() != cos.len() {
        return Err("deepseek_vector_rope_shape_invalid".to_string());
    }
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        ROPE,
        input,
        cos,
        sin,
        input.len(),
        [
            heads as u64,
            head_dim as u64,
            rope_dim as u64,
            u64::from(inverse),
        ],
        [0.0, 0.0],
    )
}

pub fn execute_kv_fp8_roundtrip_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    input: &[f32],
    quantized_len: usize,
    block: usize,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if quantized_len > input.len() || block == 0 || !quantized_len.is_multiple_of(block) {
        return Err("deepseek_vector_kv_fp8_shape_invalid".to_string());
    }
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        KV_FP8_ROUNDTRIP,
        input,
        &[],
        &[],
        input.len(),
        [quantized_len as u64, block as u64, 0, 0],
        [0.0, 0.0],
    )
}

pub fn execute_sink_attention_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    q: &[f32],
    rows: &[f32],
    sinks: &[f32],
    heads: usize,
    head_dim: usize,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if q.len() != heads * head_dim
        || sinks.len() != heads
        || rows.is_empty()
        || !rows.len().is_multiple_of(head_dim)
        || rows.len() / head_dim > 1024
    {
        return Err("deepseek_vector_sink_attention_shape_invalid".to_string());
    }
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        SINK_ATTENTION,
        q,
        rows,
        sinks,
        q.len(),
        [heads as u64, head_dim as u64, 0, 0],
        [0.0, 0.0],
    )
}

pub fn execute_indexer_qat_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    input: &[f32],
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if input.is_empty() || !input.len().is_multiple_of(128) {
        return Err("deepseek_vector_indexer_qat_shape_invalid".to_string());
    }
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        INDEXER_QAT,
        input,
        &[],
        &[],
        input.len(),
        [0; 4],
        [0.0, 0.0],
    )
}

#[allow(clippy::too_many_arguments)]
pub fn execute_compressor_pool_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    kv_state: &[f32],
    score_state: &[f32],
    norm: &[f32],
    cos: &[f32],
    sin: &[f32],
    head_dim: usize,
    ratio: usize,
    width: usize,
    rope_dim: usize,
    eps: f32,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    let rows = ratio
        .checked_mul(if ratio == 4 { 2 } else { 1 })
        .ok_or_else(|| "deepseek_vector_compressor_rows_overflow".to_string())?;
    if !matches!(ratio, 4 | 128)
        || width != head_dim * if ratio == 4 { 2 } else { 1 }
        || kv_state.len() != rows * width
        || score_state.len() != kv_state.len()
        || norm.len() != head_dim
        || rope_dim > head_dim
        || !rope_dim.is_multiple_of(2)
        || cos.len() != rope_dim / 2
        || sin.len() != cos.len()
        || !eps.is_finite()
        || eps <= 0.0
        || score_state
            .iter()
            .any(|value| value.is_nan() || *value == f32::INFINITY)
    {
        return Err("deepseek_vector_compressor_pool_shape_invalid".to_string());
    }
    let sanitized_scores = score_state
        .iter()
        .map(|value| {
            if *value == f32::NEG_INFINITY {
                -f32::MAX
            } else {
                *value
            }
        })
        .collect::<Vec<_>>();
    let mut controls = norm.to_vec();
    controls.extend_from_slice(cos);
    controls.extend_from_slice(sin);
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        COMPRESSOR_POOL,
        kv_state,
        &sanitized_scores,
        &controls,
        head_dim,
        [head_dim as u64, ratio as u64, width as u64, rope_dim as u64],
        [eps, 0.0],
    )
}

pub fn execute_scale_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    input: &[f32],
    scale: f32,
    bf16: bool,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        SCALE,
        input,
        &[],
        &[],
        input.len(),
        [u64::from(bf16), 0, 0, 0],
        [scale, 0.0],
    )
}

pub fn execute_swiglu_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    gate: &[f32],
    up: &[f32],
    limit: f32,
    bf16: bool,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if gate.len() != up.len() {
        return Err("deepseek_vector_swiglu_shape_invalid".to_string());
    }
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        SWIGLU,
        gate,
        up,
        &[],
        gate.len(),
        [u64::from(bf16), 0, 0, 0],
        [limit, 0.0],
    )
}

pub fn execute_add_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    left: &[f32],
    right: &[f32],
    bf16: bool,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if left.len() != right.len() {
        return Err("deepseek_vector_add_shape_invalid".to_string());
    }
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        ADD,
        left,
        right,
        &[],
        left.len(),
        [u64::from(bf16), 0, 0, 0],
        [0.0, 0.0],
    )
}

pub fn execute_router_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    logits: &[f32],
    bias: Option<&[f32]>,
    hash: Option<&[usize]>,
    top_k: usize,
    weight_scale: f32,
) -> Result<
    (
        DeepseekV4FlashRouterOutput,
        DeepseekV4OfficialVectorExecution,
    ),
    String,
> {
    if logits.is_empty()
        || top_k == 0
        || top_k > logits.len()
        || bias.is_some_and(|bias| bias.len() != logits.len())
        || hash.is_some_and(|hash| hash.len() != top_k)
    {
        return Err("deepseek_vector_router_shape_invalid".to_string());
    }
    let hash_values = hash
        .map(|values| values.iter().map(|value| *value as f32).collect::<Vec<_>>())
        .unwrap_or_default();
    let execution = execute_vector(
        topology,
        task,
        manifest,
        segment,
        ROUTER,
        logits,
        bias.unwrap_or(&[]),
        &hash_values,
        logits.len() + top_k * 2,
        [
            logits.len() as u64,
            top_k as u64,
            u64::from(hash.is_some()),
            0,
        ],
        [weight_scale, 0.0],
    )?;
    let expert_indices = execution.output[logits.len()..logits.len() + top_k]
        .iter()
        .map(|value| *value as usize)
        .collect();
    let output = DeepseekV4FlashRouterOutput {
        probabilities: execution.output[..logits.len()].to_vec(),
        expert_indices,
        expert_weights: execution.output[logits.len() + top_k..].to_vec(),
    };
    Ok((output, execution))
}

pub fn execute_top_k_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    logits: &[f32],
    top_k: usize,
) -> Result<
    (
        Vec<DeepseekV4ReferenceTokenLogit>,
        DeepseekV4OfficialVectorExecution,
    ),
    String,
> {
    if top_k == 0 || top_k > logits.len() {
        return Err("deepseek_vector_top_k_shape_invalid".to_string());
    }
    let execution = execute_vector(
        topology,
        task,
        manifest,
        segment,
        TOP_K,
        logits,
        &[],
        &[],
        top_k * 2,
        [top_k as u64, 0, 0, 0],
        [0.0, 0.0],
    )?;
    let values = (0..top_k)
        .map(|index| DeepseekV4ReferenceTokenLogit {
            token_id: execution.output[index] as u64,
            logit: execution.output[top_k + index],
        })
        .collect();
    Ok((values, execution))
}

pub fn execute_hc_head_weights_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest: &Path,
    segment: u64,
    hc: &[f32],
    base: &[f32],
    scale: f32,
    eps: f32,
) -> Result<DeepseekV4OfficialVectorExecution, String> {
    if hc.len() != base.len() {
        return Err("deepseek_vector_hc_head_weights_shape_invalid".to_string());
    }
    execute_vector(
        topology,
        task,
        manifest,
        segment,
        HC_HEAD_WEIGHTS,
        hc,
        base,
        &[],
        hc.len(),
        [0; 4],
        [scale, eps],
    )
}
