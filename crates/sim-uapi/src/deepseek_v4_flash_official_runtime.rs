//! Production MXFP8 dispatch for the official DeepSeek V4 Flash checkpoint.

use std::path::{Path, PathBuf};

use serde::Serialize;
use sim_core::{
    BackendDispatchOperation, BufferUsage, CompletionStatus, DispatchBackendProfile,
    DispatchBackendSpec, DispatchRuntimeVariant, FunctionLabel, MemoryEndpoint, PlLevel,
    SegmentHandle, SimplerRuntimeArg, SimplerRuntimeArtifacts, TaskKey,
};
use sim_models::deepseek_v4_flash_checkpoint::{
    checksum64, DeepseekV4Checkpoint, DeepseekV4TensorDType,
};
use sim_runtime::{LocalRuntimeEngine, VecEventSink};
use sim_topology::SimTopology;

use super::{
    bytes_to_f32s, ensure_simpler_host_fp32_gemm_manifest, ensure_simpler_host_gemm_manifest,
    f32s_to_bytes, host_vector_dispatch_lock_guard, kvcache_host_matmul_request, opaque_binding,
    run_host_gemm, scenario_config_for_chipbackend, simpler_host_artifact_producer_path,
    with_suppressed_stdio, HostVectorDispatchLock, SimplerRuntimeManifestEnvelope,
};

const TILE: usize = 128;
const MX_SCALE_GROUP: usize = 32;
const TILE_ELEMENTS: usize = TILE * TILE;
const SCALE_TILE_ELEMENTS: usize = TILE * (TILE / MX_SCALE_GROUP);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DeepseekV4LinearOutputDType {
    Bf16,
    F32,
}

impl DeepseekV4LinearOutputDType {
    pub fn name(self) -> &'static str {
        match self {
            Self::Bf16 => "bf16",
            Self::F32 => "fp32",
        }
    }
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialFp8Execution {
    pub tensor_name: Option<String>,
    pub output_size: usize,
    pub input_size: usize,
    pub row_start: usize,
    pub row_count: usize,
    pub output_dtype: String,
    pub dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
    pub activation_values_checksum: String,
    pub activation_scales_checksum: String,
    pub output: Vec<f32>,
    pub output_checksum: String,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialBf16Execution {
    pub tensor_name: String,
    pub output_size: usize,
    pub input_size: usize,
    pub row_start: usize,
    pub row_count: usize,
    pub output_dtype: String,
    pub dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
    pub input_checksum: String,
    pub output: Vec<f32>,
    pub output_checksum: String,
}

#[derive(Clone, Debug, Serialize, PartialEq)]
pub struct DeepseekV4OfficialF32Execution {
    pub tensor_name: String,
    pub output_size: usize,
    pub input_size: usize,
    pub row_start: usize,
    pub row_count: usize,
    pub output_dtype: String,
    pub dispatch_count: usize,
    pub peak_tile_payload_bytes: usize,
    pub input_checksum: String,
    pub output: Vec<f32>,
    pub output_checksum: String,
}

#[derive(Debug, PartialEq, Eq)]
pub(super) struct DynamicFp8Activation {
    pub(super) values: Vec<u8>,
    pub(super) scales: Vec<u8>,
}

struct HostFp8GemmRunner {
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
    now_us: u64,
}

pub fn ensure_simpler_host_fp8_gemm_manifest(manifest_path: &Path) -> Result<(), String> {
    if let Ok(text) = std::fs::read_to_string(manifest_path) {
        if let (Ok(manifest), Ok(value)) = (
            serde_json::from_str::<SimplerRuntimeManifestEnvelope>(&text),
            serde_json::from_str::<serde_json::Value>(&text),
        ) {
            if super::simpler_manifest_has_current_capi_abi(manifest_path)
                && value["host_fp8_gemm_manifest_version"].as_u64() == Some(1)
                && manifest.platform.as_deref() == Some("a5sim")
                && manifest.host_fp8_gemm.as_ref().is_some_and(|geometry| {
                    geometry.m == TILE as u64
                        && geometry.k == TILE as u64
                        && geometry.n == TILE as u64
                        && geometry.input_dtype == "fp8_e4m3_ue8m0"
                        && geometry.output_dtype == "fp32"
                        && geometry.tile == TILE as u64
                })
            {
                return Ok(());
            }
        }
    }

    let output_dir = manifest_path.parent().ok_or_else(|| {
        format!(
            "host_fp8_gemm_manifest_has_no_parent:{}",
            manifest_path.display()
        )
    })?;
    let script = simpler_host_artifact_producer_path();
    let status = std::process::Command::new("python3")
        .arg(&script)
        .arg("--profile")
        .arg("host_fp8_gemm")
        .arg("--output-dir")
        .arg(output_dir)
        .arg("--platform")
        .arg("a5sim")
        .arg("--gemm-m")
        .arg(TILE.to_string())
        .arg("--gemm-k")
        .arg(TILE.to_string())
        .arg("--gemm-n")
        .arg(TILE.to_string())
        .status()
        .map_err(|err| format!("run_simpler_host_fp8_gemm_artifact_producer_failed:{err}"))?;
    if !status.success() || !manifest_path.exists() {
        return Err(format!(
            "simpler_host_fp8_gemm_artifact_producer_failed:{}:status={status}",
            script.display()
        ));
    }
    Ok(())
}

fn fp8_gemm_backend_spec_from_manifest(
    manifest_path: &Path,
    endpoints: &[MemoryEndpoint; 5],
) -> Result<DispatchBackendSpec, String> {
    let text = std::fs::read_to_string(manifest_path).map_err(|err| {
        format!(
            "read_simpler_fp8_gemm_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let manifest: SimplerRuntimeManifestEnvelope = serde_json::from_str(&text).map_err(|err| {
        format!(
            "parse_simpler_fp8_gemm_manifest_failed:{}:{err}",
            manifest_path.display()
        )
    })?;
    let geometry = manifest
        .host_fp8_gemm
        .ok_or_else(|| "simpler_fp8_gemm_manifest_missing_geometry".to_string())?;
    if manifest.platform.as_deref() != Some("a5sim")
        || geometry.m != TILE as u64
        || geometry.k != TILE as u64
        || geometry.n != TILE as u64
        || geometry.input_dtype != "fp8_e4m3_ue8m0"
        || geometry.output_dtype != "fp32"
        || geometry.tile != TILE as u64
    {
        return Err(format!(
            "simpler_fp8_gemm_manifest_contract_mismatch:platform={:?}:geometry={}x{}x{}:{}/{}:tile{}",
            manifest.platform,
            geometry.m,
            geometry.k,
            geometry.n,
            geometry.input_dtype,
            geometry.output_dtype,
            geometry.tile
        ));
    }
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
                bytes: TILE_ELEMENTS as u64,
            },
            SimplerRuntimeArg::InputSegment {
                endpoint: endpoints[1].clone(),
                bytes: TILE_ELEMENTS as u64,
            },
            SimplerRuntimeArg::InputSegment {
                endpoint: endpoints[2].clone(),
                bytes: SCALE_TILE_ELEMENTS as u64,
            },
            SimplerRuntimeArg::InputSegment {
                endpoint: endpoints[3].clone(),
                bytes: SCALE_TILE_ELEMENTS as u64,
            },
            SimplerRuntimeArg::OutputSegment {
                endpoint: endpoints[4].clone(),
                bytes: (TILE_ELEMENTS * std::mem::size_of::<f32>()) as u64,
            },
            SimplerRuntimeArg::ScalarU64(TILE as u64),
            SimplerRuntimeArg::ScalarU64(TILE as u64),
            SimplerRuntimeArg::ScalarU64(TILE as u64),
        ],
    };
    Ok(DispatchBackendSpec {
        profile: DispatchBackendProfile::HostFp8Gemm,
        platform: "a5sim".to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_fp8_gemm".to_string()),
        simpler_runtime: Some(runtime),
        context: None,
    })
}

impl HostFp8GemmRunner {
    fn new(
        topology: &SimTopology,
        manifest_path: &Path,
        segment_base: u64,
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
        if activation.len() != TILE_ELEMENTS
            || weight.len() != TILE_ELEMENTS
            || activation_scale.len() != SCALE_TILE_ELEMENTS
            || weight_scale.len() != SCALE_TILE_ELEMENTS
        {
            return Err(format!(
                "host_fp8_gemm_tile_size_mismatch:a={}:b={}:as={}:bs={}",
                activation.len(),
                weight.len(),
                activation_scale.len(),
                weight_scale.len()
            ));
        }
        let output_bytes = TILE_ELEMENTS * std::mem::size_of::<f32>();
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
        let backend_spec = fp8_gemm_backend_spec_from_manifest(&self.manifest_path, &endpoints)?;
        let bindings = vec![
            opaque_binding(
                "fp8_activation_e4m3",
                BufferUsage::Input,
                endpoints[0].clone(),
                TILE_ELEMENTS as u64,
            ),
            opaque_binding(
                "fp8_weight_e4m3",
                BufferUsage::Input,
                endpoints[1].clone(),
                TILE_ELEMENTS as u64,
            ),
            opaque_binding(
                "fp8_activation_scale_ue8m0",
                BufferUsage::Input,
                endpoints[2].clone(),
                SCALE_TILE_ELEMENTS as u64,
            ),
            opaque_binding(
                "fp8_weight_scale_ue8m0",
                BufferUsage::Input,
                endpoints[3].clone(),
                SCALE_TILE_ELEMENTS as u64,
            ),
            opaque_binding(
                "fp8_output_fp32",
                BufferUsage::Output,
                endpoints[4].clone(),
                output_bytes as u64,
            ),
        ];
        let dispatch = BackendDispatchOperation {
            task: task.clone(),
            function: FunctionLabel {
                name: "host_fp8_gemm".into(),
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
            .ok_or_else(|| "simpler_capi_fp8_gemm_time_overflow".to_string())?;
        let completion = with_suppressed_stdio(|| {
            self.runtime
                .submit_backend_dispatch(dispatch, &mut self.sink)
                .map_err(|err| err.to_string())?;
            self.runtime.advance_to(self.now_us, &mut self.sink);
            self.runtime
                .poll_completions(self.now_us, &mut self.sink)
                .into_iter()
                .next()
                .ok_or_else(|| "simpler_capi_fp8_gemm_dispatch_did_not_complete".to_string())
        })?;
        if completion.status != CompletionStatus::Success {
            return Err(format!(
                "simpler_capi_fp8_gemm_dispatch_failed:{:?}",
                completion.status
            ));
        }
        let payload = self
            .runtime
            .host_segment_payload(self.host_node, self.output)
            .ok_or_else(|| "missing_host_fp8_gemm_output_payload".to_string())?;
        let output = bytes_to_f32s(payload);
        if output.len() != TILE_ELEMENTS || output.iter().any(|value| !value.is_finite()) {
            return Err(format!(
                "simpler_capi_fp8_gemm_output_invalid:len={}:first={:?}",
                output.len(),
                output.first()
            ));
        }
        Ok(output)
    }
}

fn encode_fp8_e4m3(value: f32) -> Result<u8, String> {
    if !value.is_finite() {
        return Err("deepseek_v4_production_fp8_encode_non_finite".to_string());
    }
    let mut bits = value.to_bits();
    let sign = bits & 0x8000_0000;
    bits ^= sign;
    let encoded = if bits >= (543u32 << 21) {
        0x7e
    } else if bits < (121u32 << 23) {
        let denorm_mask = 141u32 << 23;
        let shifted = (f32::from_bits(bits) + f32::from_bits(denorm_mask)).to_bits();
        shifted.wrapping_sub(denorm_mask) as u8
    } else {
        let mantissa_odd = (bits >> 20) & 1;
        bits = bits.wrapping_add(((7i32 - 127) as u32) << 23);
        bits = bits.wrapping_add(0x7f_fff + mantissa_odd);
        (bits >> 20) as u8
    };
    Ok(encoded | (sign >> 24) as u8)
}

fn encode_ue8m0_ceil(value: f32) -> Result<u8, String> {
    if !value.is_finite() || value <= 0.0 {
        return Err(format!(
            "deepseek_v4_production_ue8m0_encode_invalid:{value}"
        ));
    }
    let exponent = value.log2().ceil() as i32;
    Ok((exponent.clamp(-127, 127) + 127) as u8)
}

fn decode_ue8m0(bits: u8) -> Result<f32, String> {
    if bits == 0xff {
        return Err("deepseek_v4_production_ue8m0_nan".to_string());
    }
    Ok(2.0f32.powi(i32::from(bits) - 127))
}

pub(super) fn quantize_dynamic_fp8(input: &[f32]) -> Result<DynamicFp8Activation, String> {
    if input.is_empty() || !input.len().is_multiple_of(TILE) {
        return Err(format!(
            "deepseek_v4_production_fp8_activation_shape_invalid:{}",
            input.len()
        ));
    }
    if input.iter().any(|value| !value.is_finite()) {
        return Err("deepseek_v4_production_fp8_activation_non_finite".to_string());
    }
    let mut values = Vec::with_capacity(input.len());
    let mut scales = Vec::with_capacity(input.len() / TILE);
    for block in input.chunks_exact(TILE) {
        let absolute_max = block
            .iter()
            .map(|value| value.abs())
            .fold(1.0e-4f32, f32::max);
        let scale_bits = encode_ue8m0_ceil(absolute_max / 448.0)?;
        let scale = decode_ue8m0(scale_bits)?;
        scales.push(scale_bits);
        for value in block {
            values.push(encode_fp8_e4m3((value / scale).clamp(-448.0, 448.0))?);
        }
    }
    Ok(DynamicFp8Activation { values, scales })
}

pub(super) fn round_to_bf16(value: f32) -> f32 {
    let bits = value.to_bits();
    if !value.is_finite() {
        return value;
    }
    let rounding_bias = 0x7fffu32 + ((bits >> 16) & 1);
    f32::from_bits(bits.wrapping_add(rounding_bias) & 0xffff_0000)
}

fn encode_bf16_rne(value: f32) -> Result<u16, String> {
    if !value.is_finite() {
        return Err("deepseek_v4_production_bf16_input_non_finite".to_string());
    }
    let bits = value.to_bits();
    let rounding_bias = 0x7fffu32 + ((bits >> 16) & 1);
    Ok((bits.wrapping_add(rounding_bias) >> 16) as u16)
}

pub(super) fn checksum(values: &[u8]) -> String {
    format!("fnv1a64:{:016x}", checksum64(values))
}

pub(super) fn checksum_f32(values: &[f32]) -> String {
    let mut bytes = Vec::with_capacity(values.len() * std::mem::size_of::<f32>());
    for value in values {
        bytes.extend_from_slice(&value.to_le_bytes());
    }
    checksum(&bytes)
}

#[allow(clippy::too_many_arguments)]
pub(super) fn validate_fp8_rows(
    weight: &[u8],
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
        || !input_size.is_multiple_of(TILE)
        || row_start
            .checked_add(row_count)
            .is_none_or(|end| end > output_size)
        || weight.len() != row_count.checked_mul(input_size).unwrap_or(usize::MAX)
    {
        return Err(format!(
            "deepseek_v4_production_fp8_shape_invalid:out={output_size}:in={input_size}:row={row_start}+{row_count}:weight={}:input={}",
            weight.len(),
            input.len()
        ));
    }
    if input.iter().any(|value| !value.is_finite()) {
        return Err("deepseek_v4_production_fp8_activation_non_finite".to_string());
    }
    if weight.iter().any(|value| value & 0x7f == 0x7f) {
        return Err("deepseek_v4_production_fp8_weight_nan".to_string());
    }
    let scale_columns = input_size.div_ceil(TILE);
    let expected_scales = output_size.div_ceil(TILE) * scale_columns;
    if scales.len() != expected_scales || scales.contains(&0xff) {
        return Err(format!(
            "deepseek_v4_production_fp8_scale_invalid:actual={}:expected={expected_scales}",
            scales.len()
        ));
    }
    Ok(scale_columns)
}

#[allow(clippy::too_many_arguments)]
pub fn execute_fp8_rows_through_simpler(
    topology: &SimTopology,
    task: &TaskKey,
    manifest_path: &Path,
    segment_base: u64,
    weight: &[u8],
    scales: &[u8],
    output_size: usize,
    input_size: usize,
    row_start: usize,
    row_count: usize,
    input: &[f32],
    output_dtype: DeepseekV4LinearOutputDType,
) -> Result<DeepseekV4OfficialFp8Execution, String> {
    let scale_columns = validate_fp8_rows(
        weight,
        scales,
        output_size,
        input_size,
        row_start,
        row_count,
        input,
    )?;

    ensure_simpler_host_fp8_gemm_manifest(manifest_path)?;
    let activation = quantize_dynamic_fp8(input)?;
    let mut runner = HostFp8GemmRunner::new(topology, manifest_path, segment_base)?;
    let mut output = vec![0.0f32; row_count];
    let mut dispatch_count = 0usize;
    for output_tile_start in (0..row_count).step_by(TILE) {
        let valid_rows = (row_count - output_tile_start).min(TILE);
        for k_block in 0..scale_columns {
            let mut activation_tile = vec![0u8; TILE_ELEMENTS];
            activation_tile[..TILE]
                .copy_from_slice(&activation.values[k_block * TILE..(k_block + 1) * TILE]);
            let mut weight_tile = vec![0u8; TILE_ELEMENTS];
            for column in 0..valid_rows {
                let source_row = output_tile_start + column;
                let source = &weight[source_row * input_size + k_block * TILE
                    ..source_row * input_size + (k_block + 1) * TILE];
                for (k, value) in source.iter().copied().enumerate() {
                    weight_tile[column * TILE + k] = value;
                }
            }
            let mut activation_scale_tile = vec![127u8; SCALE_TILE_ELEMENTS];
            for lane in 0..TILE / MX_SCALE_GROUP {
                activation_scale_tile[lane] = activation.scales[k_block];
            }
            let mut weight_scale_tile = vec![127u8; SCALE_TILE_ELEMENTS];
            for column in 0..valid_rows {
                let global_row = row_start + output_tile_start + column;
                let scale = scales[(global_row / TILE) * scale_columns + k_block];
                for lane in 0..TILE / MX_SCALE_GROUP {
                    weight_scale_tile[lane * TILE + column] = scale;
                }
            }
            let mut tile_task = task.clone();
            tile_task.task_id = task
                .task_id
                .checked_add(dispatch_count as u64)
                .ok_or_else(|| "deepseek_v4_production_fp8_task_id_overflow".to_string())?;
            let tile_output = runner.run_tile(
                &tile_task,
                activation_tile,
                weight_tile,
                activation_scale_tile,
                weight_scale_tile,
            )?;
            for column in 0..valid_rows {
                output[output_tile_start + column] += tile_output[column];
            }
            dispatch_count += 1;
        }
    }
    if output.iter().any(|value| !value.is_finite()) {
        return Err("deepseek_v4_production_fp8_accumulator_non_finite".to_string());
    }
    if output_dtype == DeepseekV4LinearOutputDType::Bf16 {
        output
            .iter_mut()
            .for_each(|value| *value = round_to_bf16(*value));
    }
    Ok(DeepseekV4OfficialFp8Execution {
        tensor_name: None,
        output_size,
        input_size,
        row_start,
        row_count,
        output_dtype: output_dtype.name().to_string(),
        dispatch_count,
        peak_tile_payload_bytes: TILE_ELEMENTS * 2
            + SCALE_TILE_ELEMENTS * 2
            + TILE_ELEMENTS * std::mem::size_of::<f32>(),
        activation_values_checksum: checksum(&activation.values),
        activation_scales_checksum: checksum(&activation.scales),
        output_checksum: checksum_f32(&output),
        output,
    })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_fp8_rows_through_simpler(
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
) -> Result<DeepseekV4OfficialFp8Execution, String> {
    let tensor = checkpoint.tensor(tensor_name)?;
    if tensor.dtype != DeepseekV4TensorDType::F8E4M3
        || tensor.shape.len() != 2
        || tensor.scale_tensor.is_none()
    {
        return Err(format!(
            "deepseek_v4_production_fp8_tensor_contract_invalid:{tensor_name}"
        ));
    }
    let output_size = usize::try_from(tensor.shape[0])
        .map_err(|_| format!("deepseek_v4_production_fp8_output_too_large:{tensor_name}"))?;
    let input_size = usize::try_from(tensor.shape[1])
        .map_err(|_| format!("deepseek_v4_production_fp8_input_too_large:{tensor_name}"))?;
    let byte_offset = row_start
        .checked_mul(input_size)
        .ok_or_else(|| "deepseek_v4_production_fp8_weight_offset_overflow".to_string())?;
    let bytes = row_count
        .checked_mul(input_size)
        .ok_or_else(|| "deepseek_v4_production_fp8_weight_bytes_overflow".to_string())?;
    let weight = checkpoint.read_tensor_slice(tensor_name, byte_offset as u64, bytes as u64)?;
    let scale_name = tensor
        .scale_tensor
        .as_deref()
        .ok_or_else(|| format!("deepseek_v4_production_fp8_scale_missing:{tensor_name}"))?;
    let scale_tensor = checkpoint.tensor(scale_name)?;
    if scale_tensor.dtype != DeepseekV4TensorDType::F8E8M0 {
        return Err(format!(
            "deepseek_v4_production_fp8_scale_dtype_invalid:{scale_name}"
        ));
    }
    let scales = checkpoint.read_tensor_slice(scale_name, 0, scale_tensor.payload_bytes())?;
    let mut execution = execute_fp8_rows_through_simpler(
        topology,
        task,
        manifest_path,
        segment_base,
        &weight,
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

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_bf16_rows_through_simpler(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    manifest_path: &Path,
    tensor_name: &str,
    row_start: usize,
    row_count: usize,
    input: &[f32],
    output_dtype: DeepseekV4LinearOutputDType,
) -> Result<DeepseekV4OfficialBf16Execution, String> {
    let tensor = checkpoint.tensor(tensor_name)?;
    if tensor.dtype != DeepseekV4TensorDType::Bf16
        || tensor.shape.len() != 2
        || tensor.scale_tensor.is_some()
    {
        return Err(format!(
            "deepseek_v4_production_bf16_tensor_contract_invalid:{tensor_name}"
        ));
    }
    let output_size = usize::try_from(tensor.shape[0])
        .map_err(|_| format!("deepseek_v4_production_bf16_output_too_large:{tensor_name}"))?;
    let input_size = usize::try_from(tensor.shape[1])
        .map_err(|_| format!("deepseek_v4_production_bf16_input_too_large:{tensor_name}"))?;
    if row_count == 0
        || input.len() != input_size
        || !input_size.is_multiple_of(TILE)
        || row_start
            .checked_add(row_count)
            .is_none_or(|end| end > output_size)
    {
        return Err(format!(
            "deepseek_v4_production_bf16_shape_invalid:out={output_size}:in={input_size}:row={row_start}+{row_count}:input={}",
            input.len()
        ));
    }

    let mut input_bf16 = Vec::with_capacity(input_size * std::mem::size_of::<u16>());
    for value in input {
        input_bf16.extend_from_slice(&encode_bf16_rne(*value)?.to_le_bytes());
    }
    ensure_simpler_host_gemm_manifest(manifest_path, TILE as u64, input_size as u64, TILE as u64)?;

    let row_bytes = input_size
        .checked_mul(std::mem::size_of::<u16>())
        .ok_or_else(|| "deepseek_v4_production_bf16_row_bytes_overflow".to_string())?;
    let mut output = vec![0.0f32; row_count];
    let mut dispatch_count = 0usize;
    for output_tile_start in (0..row_count).step_by(TILE) {
        let valid_rows = (row_count - output_tile_start).min(TILE);
        let global_row = row_start + output_tile_start;
        let byte_offset = global_row
            .checked_mul(row_bytes)
            .ok_or_else(|| "deepseek_v4_production_bf16_weight_offset_overflow".to_string())?;
        let byte_count = valid_rows
            .checked_mul(row_bytes)
            .ok_or_else(|| "deepseek_v4_production_bf16_weight_bytes_overflow".to_string())?;
        let weight_rows =
            checkpoint.read_tensor_slice(tensor_name, byte_offset as u64, byte_count as u64)?;

        let mut activation_matrix = vec![0u8; TILE * input_size * 2];
        activation_matrix[..input_bf16.len()].copy_from_slice(&input_bf16);
        let mut weight_matrix = vec![0u8; input_size * TILE * 2];
        for column in 0..valid_rows {
            for k in 0..input_size {
                let source = (column * input_size + k) * 2;
                let destination = (k * TILE + column) * 2;
                weight_matrix[destination..destination + 2]
                    .copy_from_slice(&weight_rows[source..source + 2]);
            }
        }
        let mut tile_task = task.clone();
        tile_task.task_id = task
            .task_id
            .checked_add(dispatch_count as u64)
            .ok_or_else(|| "deepseek_v4_production_bf16_task_id_overflow".to_string())?;
        let tile_output = run_host_gemm(
            topology,
            &tile_task,
            manifest_path,
            activation_matrix,
            weight_matrix,
            TILE,
            input_size,
            TILE,
            "bf16",
            2,
            "host_gemm",
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
    Ok(DeepseekV4OfficialBf16Execution {
        tensor_name: tensor_name.to_string(),
        output_size,
        input_size,
        row_start,
        row_count,
        output_dtype: output_dtype.name().to_string(),
        dispatch_count,
        peak_tile_payload_bytes: TILE * input_size * 4 + TILE * TILE * std::mem::size_of::<f32>(),
        input_checksum: checksum(&input_bf16),
        output_checksum: checksum_f32(&output),
        output,
    })
}

#[allow(clippy::too_many_arguments)]
pub fn execute_deepseek_official_f32_rows_through_simpler(
    checkpoint: &DeepseekV4Checkpoint,
    topology: &SimTopology,
    task: &TaskKey,
    manifest_path: &Path,
    tensor_name: &str,
    row_start: usize,
    row_count: usize,
    input: &[f32],
    output_dtype: DeepseekV4LinearOutputDType,
) -> Result<DeepseekV4OfficialF32Execution, String> {
    let tensor = checkpoint.tensor(tensor_name)?;
    if tensor.dtype != DeepseekV4TensorDType::F32
        || tensor.shape.len() != 2
        || tensor.scale_tensor.is_some()
    {
        return Err(format!(
            "deepseek_v4_production_f32_tensor_contract_invalid:{tensor_name}"
        ));
    }
    let output_size = usize::try_from(tensor.shape[0])
        .map_err(|_| format!("deepseek_v4_production_f32_output_too_large:{tensor_name}"))?;
    let input_size = usize::try_from(tensor.shape[1])
        .map_err(|_| format!("deepseek_v4_production_f32_input_too_large:{tensor_name}"))?;
    if row_count == 0
        || input.len() != input_size
        || !input_size.is_multiple_of(TILE)
        || input.iter().any(|value| !value.is_finite())
        || row_start
            .checked_add(row_count)
            .is_none_or(|end| end > output_size)
    {
        return Err(format!(
            "deepseek_v4_production_f32_shape_invalid:out={output_size}:in={input_size}:row={row_start}+{row_count}:input={}",
            input.len()
        ));
    }
    ensure_simpler_host_fp32_gemm_manifest(
        manifest_path,
        TILE as u64,
        input_size as u64,
        TILE as u64,
    )?;
    let row_bytes = input_size
        .checked_mul(std::mem::size_of::<f32>())
        .ok_or_else(|| "deepseek_v4_production_f32_row_bytes_overflow".to_string())?;
    let mut activation_matrix = vec![0.0f32; TILE * input_size];
    activation_matrix[..input_size].copy_from_slice(input);
    let activation_payload = f32s_to_bytes(&activation_matrix);
    let mut output = vec![0.0f32; row_count];
    let mut dispatch_count = 0usize;
    for output_tile_start in (0..row_count).step_by(TILE) {
        let valid_rows = (row_count - output_tile_start).min(TILE);
        let global_row = row_start + output_tile_start;
        let byte_offset = global_row
            .checked_mul(row_bytes)
            .ok_or_else(|| "deepseek_v4_production_f32_weight_offset_overflow".to_string())?;
        let byte_count = valid_rows
            .checked_mul(row_bytes)
            .ok_or_else(|| "deepseek_v4_production_f32_weight_bytes_overflow".to_string())?;
        let weight_rows =
            checkpoint.read_tensor_slice(tensor_name, byte_offset as u64, byte_count as u64)?;
        let mut weight_matrix = vec![0.0f32; input_size * TILE];
        for column in 0..valid_rows {
            for k in 0..input_size {
                let source = (column * input_size + k) * 4;
                let value = f32::from_le_bytes(
                    weight_rows[source..source + 4]
                        .try_into()
                        .expect("four-byte F32 weight"),
                );
                if !value.is_finite() {
                    return Err(format!(
                        "deepseek_v4_production_f32_weight_non_finite:{tensor_name}:row={}:column={k}",
                        global_row + column
                    ));
                }
                weight_matrix[k * TILE + column] = value;
            }
        }
        let mut tile_task = task.clone();
        tile_task.task_id = task
            .task_id
            .checked_add(dispatch_count as u64)
            .ok_or_else(|| "deepseek_v4_production_f32_task_id_overflow".to_string())?;
        let tile_output = run_host_gemm(
            topology,
            &tile_task,
            manifest_path,
            activation_payload.clone(),
            f32s_to_bytes(&weight_matrix),
            TILE,
            input_size,
            TILE,
            "fp32",
            4,
            "host_fp32_gemm",
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
    let peak_tile_payload_bytes = TILE
        .checked_mul(input_size)
        .and_then(|elements| elements.checked_mul(8))
        .and_then(|bytes| bytes.checked_add(TILE * TILE * 4))
        .ok_or_else(|| "deepseek_v4_production_f32_peak_payload_overflow".to_string())?;
    Ok(DeepseekV4OfficialF32Execution {
        tensor_name: tensor_name.to_string(),
        output_size,
        input_size,
        row_start,
        row_count,
        output_dtype: output_dtype.name().to_string(),
        dispatch_count,
        peak_tile_payload_bytes,
        input_checksum: checksum_f32(input),
        output_checksum: checksum_f32(&output),
        output,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn production_quantizer_covers_zero_extreme_and_ties() {
        let mut input = vec![0.0; TILE];
        input[0] = 1.0;
        input[1] = -1.0;
        input[2] = 1.0625;
        let quantized = quantize_dynamic_fp8(&input).expect("quantize FP8 activation");
        assert_eq!(quantized.scales.len(), 1);
        assert_eq!(quantized.values.len(), TILE);
        assert_eq!(quantized.values[1], quantized.values[0] | 0x80);
        assert!(quantize_dynamic_fp8(&[0.0; TILE - 1]).is_err());
        let mut non_finite = vec![0.0; TILE];
        non_finite[0] = f32::NAN;
        assert!(quantize_dynamic_fp8(&non_finite).is_err());
        let extreme = quantize_dynamic_fp8(&[f32::MAX; TILE]).unwrap();
        assert!(extreme.values.iter().all(|value| value & 0x7f != 0x7f));

        let fixture =
            sim_models::deepseek_v4_flash_checkpoint_reference::deterministic_hidden_fixture(
                7, 4096,
            );
        let production = quantize_dynamic_fp8(&fixture).unwrap();
        let reference =
            sim_models::deepseek_v4_flash_checkpoint_reference::quantize_dynamic_fp8_reference(
                &fixture, TILE,
            )
            .unwrap();
        assert_eq!(production.values, reference.values);
        assert_eq!(production.scales, reference.scales);
    }

    #[test]
    fn fp8_contract_rejects_shape_nan_and_scale_mismatches() {
        let input = vec![0.0; TILE];
        let weight = vec![0x38; TILE];
        assert!(validate_fp8_rows(&weight, &[127], 1, TILE, 0, 1, &input).is_ok());
        assert!(
            validate_fp8_rows(&weight[..TILE - 1], &[127], 1, TILE, 0, 1, &input)
                .unwrap_err()
                .contains("shape_invalid")
        );
        let mut nan_weight = weight.clone();
        nan_weight[0] = 0x7f;
        assert!(
            validate_fp8_rows(&nan_weight, &[127], 1, TILE, 0, 1, &input)
                .unwrap_err()
                .contains("weight_nan")
        );
        assert!(validate_fp8_rows(&weight, &[0xff], 1, TILE, 0, 1, &input)
            .unwrap_err()
            .contains("scale_invalid"));
        let mut non_finite = input;
        non_finite[0] = f32::INFINITY;
        assert!(
            validate_fp8_rows(&weight, &[127], 1, TILE, 0, 1, &non_finite)
                .unwrap_err()
                .contains("non_finite")
        );
    }

    #[test]
    fn output_rounding_supports_bf16_and_f32_boundaries() {
        assert_eq!(round_to_bf16(1.0), 1.0);
        assert_eq!(
            round_to_bf16(f32::from_bits(0x3f80_8000)).to_bits(),
            0x3f80_0000
        );
        assert_eq!(
            round_to_bf16(f32::from_bits(0x3f81_8000)).to_bits(),
            0x3f82_0000
        );
        assert_eq!(DeepseekV4LinearOutputDType::Bf16.name(), "bf16");
        assert_eq!(DeepseekV4LinearOutputDType::F32.name(), "fp32");
    }

    #[test]
    fn malformed_fp8_manifest_fails_closed() {
        let root = std::env::temp_dir().join(format!(
            "deepseek-v4-fp8-manifest-contract-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&root).unwrap();
        let manifest = root.join("host_fp8_gemm_manifest.json");
        std::fs::write(
            &manifest,
            r#"{"platform":"a2a3sim","host_fp8_gemm":{"m":128,"k":128,"n":128,"input_dtype":"fp8_e4m3_ue8m0","output_dtype":"fp32","tile":128},"simpler_runtime":{}}"#,
        )
        .unwrap();
        let error = fp8_gemm_backend_spec_from_manifest(
            &manifest,
            &std::array::from_fn(|_| MemoryEndpoint {
                node: 0,
                segment: SegmentHandle(1),
                offset: 0,
            }),
        )
        .unwrap_err();
        assert!(error.contains("parse_simpler_fp8_gemm_manifest_failed"));
        let _ = std::fs::remove_dir_all(root);
    }

    #[test]
    fn bf16_encoding_rejects_non_finite_and_rounds_ties_to_even() {
        assert_eq!(
            encode_bf16_rne(f32::from_bits(0x3f80_8000)).unwrap(),
            0x3f80
        );
        assert_eq!(
            encode_bf16_rne(f32::from_bits(0x3f81_8000)).unwrap(),
            0x3f82
        );
        assert!(encode_bf16_rne(f32::NAN).is_err());
        assert!(encode_bf16_rne(f32::INFINITY).is_err());
    }
}
