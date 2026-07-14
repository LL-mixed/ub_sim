//! Production packed-E2M1 routed-expert execution for the official checkpoint.

use std::path::{Path, PathBuf};

use serde::Serialize;
use sim_core::{
    BackendDispatchOperation, BufferUsage, CompletionStatus, DispatchBackendProfile,
    DispatchBackendSpec, DispatchRuntimeVariant, FunctionLabel, MemoryEndpoint, PlLevel,
    SegmentHandle, SimplerRuntimeArg, SimplerRuntimeArtifacts, TaskKey,
};
use sim_models::deepseek_v4_flash_checkpoint::{
    DeepseekV4CacheStats, DeepseekV4Checkpoint, DeepseekV4TensorDType,
};
use sim_models::deepseek_v4_flash_lowering::{
    deepseek_v4_flash_router_reference, deepseek_v4_flash_swiglu_reference,
};
use sim_runtime::{LocalRuntimeEngine, VecEventSink};
use sim_topology::SimTopology;

use super::deepseek_v4_flash_official_runtime::{
    checksum, checksum_f32, quantize_dynamic_fp8, round_to_bf16, DeepseekV4LinearOutputDType,
};
use super::{
    bytes_to_f32s, host_vector_dispatch_lock_guard, kvcache_host_matmul_request, opaque_binding,
    scenario_config_for_chipbackend, simpler_host_artifact_producer_path, with_suppressed_stdio,
    HostVectorDispatchLock, SimplerRuntimeManifestEnvelope,
};

const ARTIFACT_M: usize = 128;
const TILE_K: usize = 128;
const TILE_N: usize = 128;
const MX_SCALE_GROUP: usize = 32;

const E2M1_TO_E4M3: [u8; 16] = [
    0x00, 0x30, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x4c, 0x80, 0xb0, 0xb8, 0xbc, 0xc0, 0xc4, 0xc8, 0xcc,
];

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialFp4Execution {
    pub tensor_name: Option<String>,
    pub output_size: usize,
    pub input_size: usize,
    pub row_start: usize,
    pub row_count: usize,
    pub output_dtype: String,
    pub dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
    pub packed_weight_checksum: String,
    pub weight_scale_checksum: String,
    pub activation_values_checksum: String,
    pub activation_scales_checksum: String,
    pub output: Vec<f32>,
    pub output_checksum: String,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialRouterExecution {
    pub layer: usize,
    pub token_id: u64,
    pub hash_routed: bool,
    pub logits_checksum: String,
    pub probabilities: Vec<f32>,
    pub expert_indices: Vec<usize>,
    pub expert_weights: Vec<f32>,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialRoutedExpertExecution {
    pub layer: usize,
    pub expert: usize,
    pub route_weight: f32,
    pub gate_checksum: String,
    pub up_checksum: String,
    pub activated_checksum: String,
    pub output: Vec<f32>,
    pub output_checksum: String,
    pub dispatch_count: usize,
    pub expert_cache_before: DeepseekV4CacheStats,
    pub expert_cache_after: DeepseekV4CacheStats,
    pub expert_disk_read_bytes: u64,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialRoutedExpertsExecution {
    pub selected_experts: Vec<usize>,
    pub expert_weights: Vec<f32>,
    pub experts: Vec<DeepseekV4OfficialRoutedExpertExecution>,
    pub output: Vec<f32>,
    pub output_checksum: String,
    pub dispatch_count: usize,
}

fn combine_routed_expert_outputs<'a>(
    outputs: impl IntoIterator<Item = &'a [f32]>,
    expected_experts: usize,
    output_size: usize,
) -> Result<Vec<f32>, String> {
    let outputs = outputs.into_iter().collect::<Vec<_>>();
    if outputs.len() != expected_experts || output_size == 0 {
        return Err(format!(
            "deepseek_v4_production_routed_combine_shape_invalid:outputs={}:expected={expected_experts}:output_size={output_size}",
            outputs.len()
        ));
    }
    let mut combined = vec![0.0f32; output_size];
    for output in outputs {
        if output.len() != output_size || output.iter().any(|value| !value.is_finite()) {
            return Err(format!(
                "deepseek_v4_production_routed_combine_shape_invalid:expected={expected_experts}:output_size={output_size}:actual={}",
                output.len()
            ));
        }
        for (accumulator, value) in combined.iter_mut().zip(output) {
            *accumulator += *value;
        }
    }
    if combined.iter().any(|value| !value.is_finite()) {
        return Err("deepseek_v4_production_routed_combine_non_finite".to_string());
    }
    Ok(combined)
}

struct HostFp4GemmRunner {
    _dispatch_lock: HostVectorDispatchLock,
    scenario_config: sim_config::ScenarioConfig,
    runtime: LocalRuntimeEngine,
    sink: VecEventSink,
    host_node: u64,
    ubpu_node: u64,
    activation: SegmentHandle,
    weight: SegmentHandle,
    activation_scale: SegmentHandle,
    weight_scale: SegmentHandle,
    output: SegmentHandle,
    manifest_path: PathBuf,
    input_size: usize,
    now_us: u64,
}

pub fn ensure_simpler_host_fp4_gemm_manifest(
    manifest_path: &Path,
    input_size: usize,
) -> Result<(), String> {
    if input_size == 0 || !input_size.is_multiple_of(TILE_K) {
        return Err(format!("host_fp4_gemm_input_size_invalid:{input_size}"));
    }
    if let Ok(text) = std::fs::read_to_string(manifest_path) {
        if let (Ok(manifest), Ok(value)) = (
            serde_json::from_str::<SimplerRuntimeManifestEnvelope>(&text),
            serde_json::from_str::<serde_json::Value>(&text),
        ) {
            if value["host_fp4_gemm_manifest_version"].as_u64() == Some(2)
                && manifest.platform.as_deref() == Some("a5sim")
                && manifest.host_fp4_gemm.as_ref().is_some_and(|geometry| {
                    geometry.m == ARTIFACT_M as u64
                        && geometry.k == input_size as u64
                        && geometry.n == TILE_N as u64
                        && geometry.input_dtype == "fp8_e4m3+fp4_e2m1_lowered_fp8+ue8m0"
                        && geometry.output_dtype == "fp32"
                        && geometry.tile == TILE_N as u64
                })
            {
                return Ok(());
            }
        }
    }

    let output_dir = manifest_path.parent().ok_or_else(|| {
        format!(
            "host_fp4_gemm_manifest_has_no_parent:{}",
            manifest_path.display()
        )
    })?;
    let script = simpler_host_artifact_producer_path();
    let status = std::process::Command::new("python3")
        .arg(&script)
        .arg("--profile")
        .arg("host_fp4_gemm")
        .arg("--output-dir")
        .arg(output_dir)
        .arg("--platform")
        .arg("a5sim")
        .arg("--gemm-m")
        .arg(ARTIFACT_M.to_string())
        .arg("--gemm-k")
        .arg(input_size.to_string())
        .arg("--gemm-n")
        .arg(TILE_N.to_string())
        .status()
        .map_err(|err| format!("run_simpler_host_fp4_gemm_artifact_producer_failed:{err}"))?;
    if !status.success() || !manifest_path.exists() {
        return Err(format!(
            "simpler_host_fp4_gemm_artifact_producer_failed:{}:status={status}",
            script.display()
        ));
    }
    Ok(())
}

fn fp4_gemm_backend_spec_from_manifest(
    manifest_path: &Path,
    endpoints: &[MemoryEndpoint; 5],
    input_size: usize,
) -> Result<DispatchBackendSpec, String> {
    let text = std::fs::read_to_string(manifest_path).map_err(|err| {
        format!(
            "read_simpler_fp4_gemm_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let manifest: SimplerRuntimeManifestEnvelope = serde_json::from_str(&text).map_err(|err| {
        format!(
            "parse_simpler_fp4_gemm_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let geometry = manifest
        .host_fp4_gemm
        .ok_or_else(|| "simpler_fp4_gemm_manifest_missing_geometry".to_string())?;
    if manifest.platform.as_deref() != Some("a5sim")
        || geometry.m != ARTIFACT_M as u64
        || geometry.k != input_size as u64
        || geometry.n != TILE_N as u64
        || geometry.input_dtype != "fp8_e4m3+fp4_e2m1_lowered_fp8+ue8m0"
        || geometry.output_dtype != "fp32"
        || geometry.tile != TILE_N as u64
    {
        return Err(format!(
            "simpler_fp4_gemm_manifest_contract_mismatch:platform={:?}:geometry={}x{}x{}:{}/{}:tile{}",
            manifest.platform,
            geometry.m,
            geometry.k,
            geometry.n,
            geometry.input_dtype,
            geometry.output_dtype,
            geometry.tile
        ));
    }
    let activation_elements = ARTIFACT_M * input_size;
    let weight_elements = input_size * TILE_N;
    let activation_scale_elements = ARTIFACT_M * input_size / MX_SCALE_GROUP;
    let weight_scale_elements = input_size / MX_SCALE_GROUP * TILE_N;
    let output_elements = ARTIFACT_M * TILE_N;
    let runtime = SimplerRuntimeArtifacts {
        host_runtime_library: manifest.simpler_runtime.host_runtime_library,
        orch_shared_object: manifest.simpler_runtime.orch_shared_object,
        orch_function_name: manifest.simpler_runtime.orch_function_name,
        aicpu_binary: manifest.simpler_runtime.aicpu_binary,
        aicore_binary: manifest.simpler_runtime.aicore_binary,
        kernels: manifest.simpler_runtime.kernels,
        launch: manifest.simpler_runtime.launch,
        runtime_env: manifest.simpler_runtime.runtime_env,
        args: vec![
            SimplerRuntimeArg::InputSegment {
                endpoint: endpoints[0].clone(),
                bytes: activation_elements as u64,
            },
            SimplerRuntimeArg::InputSegment {
                endpoint: endpoints[1].clone(),
                bytes: weight_elements as u64,
            },
            SimplerRuntimeArg::InputSegment {
                endpoint: endpoints[2].clone(),
                bytes: activation_scale_elements as u64,
            },
            SimplerRuntimeArg::InputSegment {
                endpoint: endpoints[3].clone(),
                bytes: weight_scale_elements as u64,
            },
            SimplerRuntimeArg::OutputSegment {
                endpoint: endpoints[4].clone(),
                bytes: (output_elements * std::mem::size_of::<f32>()) as u64,
            },
            SimplerRuntimeArg::ScalarU64(ARTIFACT_M as u64),
            SimplerRuntimeArg::ScalarU64(input_size as u64),
            SimplerRuntimeArg::ScalarU64(TILE_N as u64),
        ],
    };
    Ok(DispatchBackendSpec {
        profile: DispatchBackendProfile::HostFp4Gemm,
        platform: "a5sim".to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_fp4_gemm".to_string()),
        simpler_runtime: Some(runtime),
        context: None,
    })
}

impl HostFp4GemmRunner {
    fn new(
        topology: &SimTopology,
        manifest_path: &Path,
        segment_base: u64,
        input_size: usize,
    ) -> Result<Self, String> {
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
        Ok(Self {
            _dispatch_lock: host_vector_dispatch_lock_guard()?,
            runtime: LocalRuntimeEngine::from_config(&scenario_config),
            scenario_config,
            sink: VecEventSink::default(),
            host_node,
            ubpu_node,
            activation: SegmentHandle(segment_base + 1),
            weight: SegmentHandle(segment_base + 2),
            activation_scale: SegmentHandle(segment_base + 3),
            weight_scale: SegmentHandle(segment_base + 4),
            output: SegmentHandle(segment_base + 5),
            manifest_path: manifest_path.to_path_buf(),
            input_size,
            now_us: 0,
        })
    }

    fn run_tile(
        &mut self,
        task: &TaskKey,
        activation: Vec<u8>,
        weight: Vec<u8>,
        activation_scale: Vec<u8>,
        weight_scale: Vec<u8>,
    ) -> Result<Vec<f32>, String> {
        let activation_elements = ARTIFACT_M * self.input_size;
        let weight_elements = self.input_size * TILE_N;
        let activation_scale_elements = ARTIFACT_M * self.input_size / MX_SCALE_GROUP;
        let weight_scale_elements = self.input_size / MX_SCALE_GROUP * TILE_N;
        let output_elements = ARTIFACT_M * TILE_N;
        if activation.len() != activation_elements
            || weight.len() != weight_elements
            || activation_scale.len() != activation_scale_elements
            || weight_scale.len() != weight_scale_elements
        {
            return Err(format!(
                "host_fp4_gemm_tile_size_mismatch:a={}:b={}:as={}:bs={}",
                activation.len(),
                weight.len(),
                activation_scale.len(),
                weight_scale.len()
            ));
        }
        let output_bytes = output_elements * std::mem::size_of::<f32>();
        self.runtime
            .seed_host_segment(self.host_node, self.activation, activation);
        self.runtime
            .seed_host_segment(self.host_node, self.weight, weight);
        self.runtime
            .seed_host_segment(self.host_node, self.activation_scale, activation_scale);
        self.runtime
            .seed_host_segment(self.host_node, self.weight_scale, weight_scale);
        self.runtime
            .seed_host_segment(self.host_node, self.output, vec![0u8; output_bytes]);
        let endpoint = |segment| MemoryEndpoint {
            node: self.host_node,
            segment,
            offset: 0,
        };
        let endpoints = [
            endpoint(self.activation),
            endpoint(self.weight),
            endpoint(self.activation_scale),
            endpoint(self.weight_scale),
            endpoint(self.output),
        ];
        let backend_spec =
            fp4_gemm_backend_spec_from_manifest(&self.manifest_path, &endpoints, self.input_size)?;
        let bindings = vec![
            opaque_binding(
                "fp4_activation_e4m3",
                BufferUsage::Input,
                endpoints[0].clone(),
                activation_elements as u64,
            ),
            opaque_binding(
                "fp4_weight_e2m1_lowered_e4m3",
                BufferUsage::Input,
                endpoints[1].clone(),
                weight_elements as u64,
            ),
            opaque_binding(
                "fp4_activation_scale_ue8m0",
                BufferUsage::Input,
                endpoints[2].clone(),
                activation_scale_elements as u64,
            ),
            opaque_binding(
                "fp4_weight_scale_ue8m0_per_32k",
                BufferUsage::Input,
                endpoints[3].clone(),
                weight_scale_elements as u64,
            ),
            opaque_binding(
                "fp4_output_fp32",
                BufferUsage::Output,
                endpoints[4].clone(),
                output_bytes as u64,
            ),
        ];
        let dispatch = BackendDispatchOperation {
            task: task.clone(),
            function: FunctionLabel {
                name: "host_fp4_gemm".into(),
                level: PlLevel::L2,
            },
            backend_spec,
            request: kvcache_host_matmul_request(task.task_id, bindings),
            target_level: PlLevel::L2,
            target_node: self.ubpu_node,
            legacy_input_segments: vec![
                self.activation,
                self.weight,
                self.activation_scale,
                self.weight_scale,
            ],
        };
        self.now_us = self
            .now_us
            .checked_add(
                self.scenario_config
                    .pypto
                    .simpler_boundary
                    .dispatch_latency_us
                    .unwrap_or(15),
            )
            .ok_or_else(|| "simpler_capi_fp4_gemm_time_overflow".to_string())?;
        let completion = with_suppressed_stdio(|| {
            self.runtime
                .submit_backend_dispatch(dispatch, &mut self.sink)
                .map_err(|err| err.to_string())?;
            self.runtime.advance_to(self.now_us, &mut self.sink);
            self.runtime
                .poll_completions(self.now_us, &mut self.sink)
                .into_iter()
                .next()
                .ok_or_else(|| "simpler_capi_fp4_gemm_dispatch_did_not_complete".to_string())
        })?;
        if completion.status != CompletionStatus::Success {
            return Err(format!(
                "simpler_capi_fp4_gemm_dispatch_failed:{:?}",
                completion.status
            ));
        }
        let payload = self
            .runtime
            .host_segment_payload(self.host_node, self.output)
            .ok_or_else(|| "missing_host_fp4_gemm_output_payload".to_string())?;
        let output = bytes_to_f32s(payload);
        if output.len() != output_elements || output.iter().any(|value| !value.is_finite()) {
            return Err(format!(
                "simpler_capi_fp4_gemm_output_invalid:len={}:first={:?}",
                output.len(),
                output.first()
            ));
        }
        Ok(output)
    }
}

#[allow(clippy::too_many_arguments)]
fn validate_fp4_rows(
    packed_weight: &[u8],
    scales: &[u8],
    output_size: usize,
    input_size: usize,
    row_start: usize,
    row_count: usize,
    input: &[f32],
) -> Result<usize, String> {
    if output_size == 0
        || input_size == 0
        || row_count == 0
        || input.len() != input_size
        || !input_size.is_multiple_of(TILE_K)
        || row_start
            .checked_add(row_count)
            .is_none_or(|end| end > output_size)
        || packed_weight.len() != row_count.checked_mul(input_size / 2).unwrap_or(usize::MAX)
    {
        return Err(format!(
            "deepseek_v4_production_fp4_shape_invalid:out={output_size}:in={input_size}:row={row_start}+{row_count}:weight={}:input={}",
            packed_weight.len(),
            input.len()
        ));
    }
    if input.iter().any(|value| !value.is_finite()) {
        return Err("deepseek_v4_production_fp4_activation_non_finite".to_string());
    }
    let scale_columns = input_size / MX_SCALE_GROUP;
    let expected_scales = row_count
        .checked_mul(scale_columns)
        .ok_or_else(|| "deepseek_v4_production_fp4_scale_size_overflow".to_string())?;
    if scales.len() != expected_scales || scales.contains(&0xff) {
        return Err(format!(
            "deepseek_v4_production_fp4_scale_invalid:actual={}:expected={expected_scales}",
            scales.len()
        ));
    }
    Ok(scale_columns)
}

#[allow(clippy::too_many_arguments)]
pub fn execute_fp4_rows_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest_path: &Path,
    segment_base: u64,
    packed_weight: &[u8],
    scales: &[u8],
    output_size: usize,
    input_size: usize,
    row_start: usize,
    row_count: usize,
    input: &[f32],
    output_dtype: DeepseekV4LinearOutputDType,
) -> Result<DeepseekV4OfficialFp4Execution, String> {
    let scale_columns = validate_fp4_rows(
        packed_weight,
        scales,
        output_size,
        input_size,
        row_start,
        row_count,
        input,
    )?;
    ensure_simpler_host_fp4_gemm_manifest(manifest_path, input_size)?;
    let activation = quantize_dynamic_fp8(input)?;
    let mut activation_matrix = vec![0u8; ARTIFACT_M * input_size];
    activation_matrix[..input_size].copy_from_slice(&activation.values);
    let mut activation_scale_matrix = vec![127u8; ARTIFACT_M * scale_columns];
    for group in 0..scale_columns {
        activation_scale_matrix[group] = activation.scales[group / (TILE_K / MX_SCALE_GROUP)];
    }
    let mut runner = HostFp4GemmRunner::new(topology, manifest_path, segment_base, input_size)?;
    let mut output = vec![0.0f32; row_count];
    let mut dispatch_count = 0usize;
    for output_tile_start in (0..row_count).step_by(TILE_N) {
        let valid_rows = (row_count - output_tile_start).min(TILE_N);
        let mut weight_matrix = vec![0u8; input_size * TILE_N];
        let mut weight_scale_matrix = vec![127u8; scale_columns * TILE_N];
        for column in 0..valid_rows {
            let source_row = output_tile_start + column;
            let packed_row =
                &packed_weight[source_row * (input_size / 2)..(source_row + 1) * (input_size / 2)];
            for k in 0..input_size {
                let packed = packed_row[k / 2];
                let nibble = if k % 2 == 0 {
                    packed & 0x0f
                } else {
                    packed >> 4
                };
                weight_matrix[column * input_size + k] = E2M1_TO_E4M3[nibble as usize];
            }
            for group in 0..scale_columns {
                weight_scale_matrix[group * TILE_N + column] =
                    scales[source_row * scale_columns + group];
            }
        }
        let mut tile_task = task.clone();
        tile_task.task_id = task
            .task_id
            .checked_add(dispatch_count as u64)
            .ok_or_else(|| "deepseek_v4_production_fp4_task_id_overflow".to_string())?;
        let tile_output = runner.run_tile(
            &tile_task,
            activation_matrix.clone(),
            weight_matrix,
            activation_scale_matrix.clone(),
            weight_scale_matrix,
        )?;
        output[output_tile_start..output_tile_start + valid_rows]
            .copy_from_slice(&tile_output[..valid_rows]);
        dispatch_count += 1;
    }
    if output_dtype == DeepseekV4LinearOutputDType::Bf16 {
        output
            .iter_mut()
            .for_each(|value| *value = round_to_bf16(*value));
    }
    Ok(DeepseekV4OfficialFp4Execution {
        tensor_name: None,
        output_size,
        input_size,
        row_start,
        row_count,
        output_dtype: output_dtype.name().to_string(),
        dispatch_count,
        peak_tile_payload_bytes: ARTIFACT_M * input_size
            + input_size * TILE_N
            + ARTIFACT_M * scale_columns
            + scale_columns * TILE_N
            + ARTIFACT_M * TILE_N * std::mem::size_of::<f32>(),
        packed_weight_checksum: checksum(packed_weight),
        weight_scale_checksum: checksum(scales),
        activation_values_checksum: checksum(&activation.values),
        activation_scales_checksum: checksum(&activation.scales),
        output_checksum: checksum_f32(&output),
        output,
    })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_fp4_rows_through_simpler(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    manifest_path: &Path,
    segment_base: u64,
    tensor_name: &str,
    row_start: usize,
    row_count: usize,
    input: &[f32],
    output_dtype: DeepseekV4LinearOutputDType,
) -> Result<DeepseekV4OfficialFp4Execution, String> {
    let tensor = checkpoint.tensor(tensor_name)?;
    if tensor.dtype != DeepseekV4TensorDType::I8
        || tensor.shape.len() != 2
        || tensor.scale_tensor.is_none()
    {
        return Err(format!(
            "deepseek_v4_production_fp4_tensor_contract_invalid:{tensor_name}"
        ));
    }
    let output_size = usize::try_from(tensor.shape[0])
        .map_err(|_| format!("deepseek_v4_production_fp4_output_too_large:{tensor_name}"))?;
    let stored_input = usize::try_from(tensor.shape[1])
        .map_err(|_| format!("deepseek_v4_production_fp4_input_too_large:{tensor_name}"))?;
    let input_size = stored_input.checked_mul(2).ok_or_else(|| {
        format!("deepseek_v4_production_fp4_logical_input_overflow:{tensor_name}")
    })?;
    let weight_offset = row_start
        .checked_mul(stored_input)
        .ok_or_else(|| "deepseek_v4_production_fp4_weight_offset_overflow".to_string())?;
    let weight_bytes = row_count
        .checked_mul(stored_input)
        .ok_or_else(|| "deepseek_v4_production_fp4_weight_bytes_overflow".to_string())?;
    let packed_weight =
        checkpoint.read_expert_slice(tensor_name, weight_offset as u64, weight_bytes as u64)?;
    let scale_name = tensor
        .scale_tensor
        .as_deref()
        .ok_or_else(|| format!("deepseek_v4_production_fp4_scale_missing:{tensor_name}"))?;
    let scale_tensor = checkpoint.tensor(scale_name)?;
    let scale_columns = input_size / MX_SCALE_GROUP;
    if scale_tensor.dtype != DeepseekV4TensorDType::F8E8M0
        || scale_tensor.shape != [output_size as u64, scale_columns as u64]
    {
        return Err(format!(
            "deepseek_v4_production_fp4_scale_contract_invalid:{scale_name}:dtype={}:shape={:?}",
            scale_tensor.dtype.safetensors_name(),
            scale_tensor.shape
        ));
    }
    let scale_offset = row_start
        .checked_mul(scale_columns)
        .ok_or_else(|| "deepseek_v4_production_fp4_scale_offset_overflow".to_string())?;
    let scale_bytes = row_count
        .checked_mul(scale_columns)
        .ok_or_else(|| "deepseek_v4_production_fp4_scale_bytes_overflow".to_string())?;
    let scales =
        checkpoint.read_expert_slice(scale_name, scale_offset as u64, scale_bytes as u64)?;
    let mut execution = execute_fp4_rows_through_simpler(
        topology,
        task,
        manifest_path,
        segment_base,
        &packed_weight,
        &scales,
        output_size,
        input_size,
        row_start,
        row_count,
        input,
        output_dtype,
    )?;
    execution.tensor_name = Some(tensor_name.to_string());
    Ok(execution)
}

fn read_hash_selected_experts(
    checkpoint: &DeepseekV4Checkpoint,
    layer: usize,
    token_id: u64,
) -> Result<Vec<usize>, String> {
    let name = format!("layers.{layer}.ffn.gate.tid2eid");
    let tensor = checkpoint.tensor(&name)?;
    let top_k = usize::try_from(checkpoint.config.num_experts_per_tok)
        .map_err(|_| "deepseek_v4_production_topk_too_large".to_string())?;
    if tensor.dtype != DeepseekV4TensorDType::I64
        || tensor.shape
            != [
                checkpoint.config.vocab_size,
                checkpoint.config.num_experts_per_tok,
            ]
        || token_id >= checkpoint.config.vocab_size
    {
        return Err(format!(
            "deepseek_v4_production_hash_route_contract_invalid:{name}:token={token_id}"
        ));
    }
    let offset = token_id
        .checked_mul(top_k as u64)
        .and_then(|value| value.checked_mul(8))
        .ok_or_else(|| "deepseek_v4_production_hash_route_offset_overflow".to_string())?;
    let bytes = checkpoint.read_tensor_slice(&name, offset, (top_k * 8) as u64)?;
    bytes
        .chunks_exact(8)
        .map(|chunk| {
            let expert = i64::from_le_bytes(chunk.try_into().unwrap());
            usize::try_from(expert)
                .map_err(|_| format!("deepseek_v4_production_hash_expert_invalid:{expert}"))
        })
        .collect()
}

fn read_learned_selection_bias(
    checkpoint: &DeepseekV4Checkpoint,
    layer: usize,
) -> Result<Vec<f32>, String> {
    let name = format!("layers.{layer}.ffn.gate.bias");
    let tensor = checkpoint.tensor(&name)?;
    let experts = usize::try_from(checkpoint.config.n_routed_experts)
        .map_err(|_| "deepseek_v4_production_expert_count_too_large".to_string())?;
    if tensor.dtype != DeepseekV4TensorDType::F32 || tensor.shape != [experts as u64] {
        return Err(format!(
            "deepseek_v4_production_router_bias_contract_invalid:{name}:dtype={}:shape={:?}",
            tensor.dtype.safetensors_name(),
            tensor.shape
        ));
    }
    let payload = checkpoint.read_tensor_slice(&name, 0, (experts * 4) as u64)?;
    let values = payload
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes(chunk.try_into().unwrap()))
        .collect::<Vec<_>>();
    if values.iter().any(|value| !value.is_finite()) {
        return Err(format!(
            "deepseek_v4_production_router_bias_non_finite:{name}"
        ));
    }
    Ok(values)
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_router_through_simpler(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    manifest_path: &Path,
    layer: usize,
    token_id: u64,
    input: &[f32],
) -> Result<DeepseekV4OfficialRouterExecution, String> {
    if layer as u64 >= checkpoint.config.num_hidden_layers {
        return Err(format!(
            "deepseek_v4_production_router_layer_invalid:{layer}"
        ));
    }
    let tensor_name = format!("layers.{layer}.ffn.gate.weight");
    let experts = usize::try_from(checkpoint.config.n_routed_experts)
        .map_err(|_| "deepseek_v4_production_expert_count_too_large".to_string())?;
    let logits = super::execute_deepseek_official_bf16_rows_through_simpler(
        checkpoint,
        topology,
        task,
        manifest_path,
        &tensor_name,
        0,
        experts,
        input,
        DeepseekV4LinearOutputDType::F32,
    )?
    .output;
    let hash_routed = (layer as u64) < checkpoint.config.num_hash_layers;
    let hash_selected = if hash_routed {
        Some(read_hash_selected_experts(checkpoint, layer, token_id)?)
    } else {
        None
    };
    let selection_bias = if hash_routed {
        None
    } else {
        Some(read_learned_selection_bias(checkpoint, layer)?)
    };
    let top_k = usize::try_from(checkpoint.config.num_experts_per_tok)
        .map_err(|_| "deepseek_v4_production_topk_too_large".to_string())?;
    let route = deepseek_v4_flash_router_reference(
        &logits,
        selection_bias.as_deref(),
        hash_selected.as_deref(),
        top_k,
        checkpoint.config.routed_scaling_factor as f32,
    )?;
    Ok(DeepseekV4OfficialRouterExecution {
        layer,
        token_id,
        hash_routed,
        logits_checksum: checksum_f32(&logits),
        probabilities: route.probabilities,
        expert_indices: route.expert_indices,
        expert_weights: route.expert_weights,
    })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_routed_expert_through_simpler(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    layer: usize,
    expert: usize,
    route_weight: f32,
    input: &[f32],
) -> Result<DeepseekV4OfficialRoutedExpertExecution, String> {
    let config = &checkpoint.config;
    if layer as u64 >= config.num_hidden_layers
        || expert as u64 >= config.n_routed_experts
        || !route_weight.is_finite()
        || route_weight <= 0.0
        || input.len() != config.hidden_size as usize
    {
        return Err(format!(
            "deepseek_v4_production_routed_expert_arguments_invalid:layer={layer}:expert={expert}:weight={route_weight}:input={}",
            input.len()
        ));
    }
    let (_, expert_cache_before) = checkpoint.cache_stats()?;
    let prefix = format!("layers.{layer}.ffn.experts.{expert}");
    let gate_manifest = artifact_dir
        .join(format!("k{}", config.hidden_size))
        .join("host_fp4_gemm_manifest.json");
    let down_manifest = artifact_dir
        .join(format!("k{}", config.moe_intermediate_size))
        .join("host_fp4_gemm_manifest.json");
    let intermediate = config.moe_intermediate_size as usize;
    let hidden = config.hidden_size as usize;
    let gate = execute_deepseek_official_fp4_rows_through_simpler(
        checkpoint,
        topology,
        task,
        &gate_manifest,
        segment_base,
        &format!("{prefix}.w1.weight"),
        0,
        intermediate,
        input,
        DeepseekV4LinearOutputDType::Bf16,
    )?;
    let mut up_task = task.clone();
    up_task.task_id = task.task_id.saturating_add(gate.dispatch_count as u64);
    let up = execute_deepseek_official_fp4_rows_through_simpler(
        checkpoint,
        topology,
        &up_task,
        &gate_manifest,
        segment_base.saturating_add(100),
        &format!("{prefix}.w3.weight"),
        0,
        intermediate,
        input,
        DeepseekV4LinearOutputDType::Bf16,
    )?;
    let mut activated =
        deepseek_v4_flash_swiglu_reference(&gate.output, &up.output, config.swiglu_limit as f32)?;
    for value in &mut activated {
        *value = round_to_bf16(*value * route_weight);
    }
    let mut down_task = task.clone();
    down_task.task_id = up_task.task_id.saturating_add(up.dispatch_count as u64);
    let down = execute_deepseek_official_fp4_rows_through_simpler(
        checkpoint,
        topology,
        &down_task,
        &down_manifest,
        segment_base.saturating_add(200),
        &format!("{prefix}.w2.weight"),
        0,
        hidden,
        &activated,
        DeepseekV4LinearOutputDType::Bf16,
    )?;
    let (_, expert_cache_after) = checkpoint.cache_stats()?;
    let expert_disk_read_bytes = expert_cache_after
        .disk_read_bytes
        .checked_sub(expert_cache_before.disk_read_bytes)
        .ok_or_else(|| "deepseek_v4_production_expert_cache_counter_underflow".to_string())?;
    Ok(DeepseekV4OfficialRoutedExpertExecution {
        layer,
        expert,
        route_weight,
        gate_checksum: gate.output_checksum,
        up_checksum: up.output_checksum,
        activated_checksum: checksum_f32(&activated),
        output_checksum: down.output_checksum,
        output: down.output,
        dispatch_count: gate.dispatch_count + up.dispatch_count + down.dispatch_count,
        expert_cache_before,
        expert_cache_after,
        expert_disk_read_bytes,
    })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_routed_experts_through_simpler(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    artifact_dir: &Path,
    segment_base: u64,
    layer: usize,
    selected_experts: &[usize],
    expert_weights: &[f32],
    input: &[f32],
) -> Result<DeepseekV4OfficialRoutedExpertsExecution, String> {
    let expected = checkpoint.config.num_experts_per_tok as usize;
    if selected_experts.len() != expected
        || expert_weights.len() != expected
        || selected_experts.iter().enumerate().any(|(index, expert)| {
            *expert as u64 >= checkpoint.config.n_routed_experts
                || selected_experts[..index].contains(expert)
        })
        || expert_weights
            .iter()
            .any(|weight| !weight.is_finite() || *weight <= 0.0)
    {
        return Err(format!(
            "deepseek_v4_production_routed_selection_invalid:experts={selected_experts:?}:weights={expert_weights:?}"
        ));
    }
    let mut executions = Vec::with_capacity(expected);
    let mut dispatch_count = 0usize;
    for (slot, (&expert, &weight)) in selected_experts.iter().zip(expert_weights).enumerate() {
        let mut expert_task = task.clone();
        expert_task.task_id = task.task_id.saturating_add(dispatch_count as u64);
        let execution = execute_deepseek_official_routed_expert_through_simpler(
            checkpoint,
            topology,
            &expert_task,
            artifact_dir,
            segment_base.saturating_add(slot as u64 * 1_000),
            layer,
            expert,
            weight,
            input,
        )?;
        dispatch_count += execution.dispatch_count;
        executions.push(execution);
    }
    let output = combine_routed_expert_outputs(
        executions
            .iter()
            .map(|execution| execution.output.as_slice()),
        expected,
        checkpoint.config.hidden_size as usize,
    )?;
    Ok(DeepseekV4OfficialRoutedExpertsExecution {
        selected_experts: selected_experts.to_vec(),
        expert_weights: expert_weights.to_vec(),
        output_checksum: checksum_f32(&output),
        output,
        dispatch_count,
        experts: executions,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn e2m1_codes_lower_exactly_to_e4m3_codes() {
        assert_eq!(E2M1_TO_E4M3[0x0], 0x00);
        assert_eq!(E2M1_TO_E4M3[0x1], 0x30);
        assert_eq!(E2M1_TO_E4M3[0x7], 0x4c);
        assert_eq!(E2M1_TO_E4M3[0x8], 0x80);
        assert_eq!(E2M1_TO_E4M3[0xf], 0xcc);
    }

    #[test]
    fn fp4_contract_rejects_shape_scale_and_non_finite_input() {
        let input = vec![0.0; TILE_K];
        let weight = vec![0u8; TILE_K / 2];
        let scales = vec![127u8; TILE_K / MX_SCALE_GROUP];
        assert!(validate_fp4_rows(&weight, &scales, 1, TILE_K, 0, 1, &input).is_ok());
        let mut bad_scale = scales.clone();
        bad_scale[0] = 0xff;
        assert!(validate_fp4_rows(&weight, &bad_scale, 1, TILE_K, 0, 1, &input).is_err());
        assert!(validate_fp4_rows(
            &weight[..weight.len() - 1],
            &scales,
            1,
            TILE_K,
            0,
            1,
            &input
        )
        .is_err());
        assert!(validate_fp4_rows(
            &weight,
            &scales[..scales.len() - 1],
            1,
            TILE_K,
            0,
            1,
            &input
        )
        .is_err());
        let mut non_finite = input;
        non_finite[0] = f32::INFINITY;
        assert!(validate_fp4_rows(&weight, &scales, 1, TILE_K, 0, 1, &non_finite).is_err());
    }

    #[test]
    fn top_six_combine_is_exact_and_fails_closed() {
        let outputs = (1..=6)
            .map(|value| vec![value as f32, -(value as f32)])
            .collect::<Vec<_>>();
        assert_eq!(
            combine_routed_expert_outputs(outputs.iter().map(Vec::as_slice), 6, 2).unwrap(),
            vec![21.0, -21.0]
        );
        assert!(
            combine_routed_expert_outputs(outputs[..5].iter().map(Vec::as_slice), 6, 2).is_err()
        );
        let mut non_finite = outputs;
        non_finite[3][0] = f32::NAN;
        assert!(combine_routed_expert_outputs(non_finite.iter().map(Vec::as_slice), 6, 2).is_err());
    }
}
