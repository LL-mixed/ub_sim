use std::ffi::{c_void, CString, OsString};
use std::path::Path;
use std::ptr;
use std::sync::Mutex;

use serde::Serialize;
use sim_config::ScenarioConfig;
use sim_core::{
    BufferUsage, CompletionStatus, DispatchBackendProfile, DispatchBackendSpec, DispatchRequest,
    DispatchRuntimeVariant, FunctionLabel, HierarchyCoord, LogicalSystemId, PlLevel,
    PtoUbGmAccessRegistration, SimError, SimplerRuntimeArg, SimplerUbGmAccess, SimplerUbGmBinding,
    SimplerUbGmView, TaskKey,
};
use sim_runtime::{LocalRuntimeEngine, VecEventSink};
use sim_topology::SimTopology;

use crate::load_host_vector_runtime_artifacts;

#[path = "pto_qwen3_range.rs"]
mod qwen3_range;
pub use qwen3_range::{run_qwen3_pto_range_check, Qwen3PtoRangeCheck};

const UB_GM_APERTURE_BASE: u64 = 0x7000_0000_0000;
const UB_GM_APERTURE_SLOT: u64 = 0x0000_0002_0000;
const UB_GM_BACKEND_BASE: u64 = 0x0010_0000;
const UB_GM_BACKEND_SLOT: u64 = 0x0002_0000;
const REQUEST_ID: u64 = 0x5054_4f32;
const PTO_DEVICE_CNA: u32 = 0x0001_0001;
const INPUT_A_BINDING: u64 = 1;
const INPUT_B_BINDING: u64 = 2;
const OUTPUT_BINDING: u64 = 3;
static BRIDGE_MOCK_ENV_LOCK: Mutex<()> = Mutex::new(());

struct BridgeHandle(*mut sim_qemu::LinquUbBridge);

impl Drop for BridgeHandle {
    fn drop(&mut self) {
        sim_qemu::linqu_ub_bridge_free(self.0);
    }
}

struct EnvRestore {
    name: &'static str,
    previous: Option<OsString>,
}

impl EnvRestore {
    fn set(name: &'static str, value: &Path) -> Self {
        let previous = std::env::var_os(name);
        std::env::set_var(name, value);
        Self { name, previous }
    }
}

impl Drop for EnvRestore {
    fn drop(&mut self) {
        match self.previous.take() {
            Some(value) => std::env::set_var(self.name, value),
            None => std::env::remove_var(self.name),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct HostVectorUbGmDispatchReport {
    pub platform: String,
    pub elems: u64,
    pub first_values: Vec<f32>,
    pub all_match_expected: bool,
    pub completion_status: CompletionStatus,
    pub read_calls: u64,
    pub read_bytes: u64,
    pub write_calls: u64,
    pub write_bytes: u64,
    pub fence_calls: u64,
    pub segment_payload_staging_bytes: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum MockAccess {
    Read,
    Write,
    ReadWrite,
}

#[derive(Debug)]
struct MockRegion {
    binding_id: u64,
    ub_gm_base: u64,
    access: MockAccess,
    bytes: Vec<u8>,
}

#[derive(Debug, Default, Clone, Copy)]
struct MockCounters {
    read_calls: u64,
    read_bytes: u64,
    write_calls: u64,
    write_bytes: u64,
    fence_calls: u64,
}

#[derive(Debug)]
struct MockBackend {
    request_id: u64,
    regions: Vec<MockRegion>,
    counters: MockCounters,
}

impl MockBackend {
    fn new(elems: u64) -> Result<Self, SimError> {
        let elems = usize::try_from(elems)
            .map_err(|_| SimError::InvalidInput("pto_ub_gm_mock_element_count_too_large"))?;
        let input_a = f32s_to_bytes(&vec![2.0; elems]);
        let input_b = f32s_to_bytes(&vec![3.0; elems]);
        let output = vec![0u8; elems * std::mem::size_of::<f32>()];
        Ok(Self {
            request_id: REQUEST_ID,
            regions: vec![
                MockRegion {
                    binding_id: INPUT_A_BINDING,
                    ub_gm_base: UB_GM_BACKEND_BASE,
                    access: MockAccess::Read,
                    bytes: input_a,
                },
                MockRegion {
                    binding_id: INPUT_B_BINDING,
                    ub_gm_base: UB_GM_BACKEND_BASE + UB_GM_BACKEND_SLOT,
                    access: MockAccess::Read,
                    bytes: input_b,
                },
                MockRegion {
                    binding_id: OUTPUT_BINDING,
                    ub_gm_base: UB_GM_BACKEND_BASE + 2 * UB_GM_BACKEND_SLOT,
                    access: MockAccess::Write,
                    bytes: output,
                },
            ],
            counters: MockCounters::default(),
        })
    }

    fn locate(
        &self,
        binding_id: u64,
        ub_gm_addr: u64,
        length: u64,
        required: MockAccess,
    ) -> Option<(usize, usize, usize)> {
        if length == 0 {
            return None;
        }
        let index = self.regions.iter().position(|region| {
            region.binding_id == binding_id
                && (region.access == required || region.access == MockAccess::ReadWrite)
        })?;
        let region = &self.regions[index];
        let offset = ub_gm_addr.checked_sub(region.ub_gm_base)?;
        let end = offset.checked_add(length)?;
        if end > region.bytes.len() as u64 {
            return None;
        }
        Some((index, offset as usize, end as usize))
    }

    fn output(&self) -> Option<&[u8]> {
        self.regions
            .iter()
            .find(|region| region.binding_id == OUTPUT_BINDING)
            .map(|region| region.bytes.as_slice())
    }
}

unsafe extern "C" fn mock_read(
    backend_context: *mut c_void,
    request_id: u64,
    binding_id: u64,
    ub_gm_addr: u64,
    dst: *mut c_void,
    length: u64,
) -> i32 {
    if backend_context.is_null() || dst.is_null() {
        return 1;
    }
    let backend = unsafe { &*backend_context.cast::<Mutex<MockBackend>>() };
    let mut backend = match backend.lock() {
        Ok(backend) => backend,
        Err(_) => return 1,
    };
    if request_id != backend.request_id {
        return 1;
    }
    let (index, start, end) = match backend.locate(binding_id, ub_gm_addr, length, MockAccess::Read)
    {
        Some(range) => range,
        None => return 1,
    };
    unsafe {
        ptr::copy_nonoverlapping(
            backend.regions[index].bytes[start..end].as_ptr(),
            dst.cast::<u8>(),
            end - start,
        );
    }
    backend.counters.read_calls += 1;
    backend.counters.read_bytes += length;
    0
}

unsafe extern "C" fn mock_write(
    backend_context: *mut c_void,
    request_id: u64,
    binding_id: u64,
    ub_gm_addr: u64,
    src: *const c_void,
    length: u64,
) -> i32 {
    if backend_context.is_null() || src.is_null() {
        return 1;
    }
    let backend = unsafe { &*backend_context.cast::<Mutex<MockBackend>>() };
    let mut backend = match backend.lock() {
        Ok(backend) => backend,
        Err(_) => return 1,
    };
    if request_id != backend.request_id {
        return 1;
    }
    let (index, start, end) =
        match backend.locate(binding_id, ub_gm_addr, length, MockAccess::Write) {
            Some(range) => range,
            None => return 1,
        };
    unsafe {
        ptr::copy_nonoverlapping(
            src.cast::<u8>(),
            backend.regions[index].bytes[start..end].as_mut_ptr(),
            end - start,
        );
    }
    backend.counters.write_calls += 1;
    backend.counters.write_bytes += length;
    0
}

unsafe extern "C" fn mock_fence(
    backend_context: *mut c_void,
    request_id: u64,
    binding_id: u64,
    ub_gm_addr: u64,
    length: u64,
    flags: u32,
) -> i32 {
    if backend_context.is_null() || flags != 0 {
        return 1;
    }
    let backend = unsafe { &*backend_context.cast::<Mutex<MockBackend>>() };
    let mut backend = match backend.lock() {
        Ok(backend) => backend,
        Err(_) => return 1,
    };
    if request_id != backend.request_id
        || backend
            .locate(binding_id, ub_gm_addr, length, MockAccess::Write)
            .is_none()
    {
        return 1;
    }
    backend.counters.fence_calls += 1;
    0
}

fn f32s_to_bytes(values: &[f32]) -> Vec<u8> {
    values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect()
}

fn bytes_to_f32s(bytes: &[u8]) -> Vec<f32> {
    bytes
        .chunks_exact(std::mem::size_of::<f32>())
        .map(|chunk| {
            let bytes: [u8; 4] = chunk.try_into().expect("four-byte f32 chunk");
            f32::from_le_bytes(bytes)
        })
        .collect()
}

fn ub_gm_arg(
    binding_id: u64,
    slot: u64,
    byte_length: u64,
    elems: u64,
    access: SimplerUbGmAccess,
    usage: BufferUsage,
) -> SimplerRuntimeArg {
    SimplerRuntimeArg::UbGmMemref {
        binding: SimplerUbGmBinding {
            request_id: REQUEST_ID,
            binding_id,
            aperture_base: UB_GM_APERTURE_BASE + slot * UB_GM_APERTURE_SLOT,
            aperture_length: byte_length,
            ub_gm_base: UB_GM_BACKEND_BASE + slot * UB_GM_BACKEND_SLOT,
            mapped_length: byte_length,
            access,
            flags: 0,
            backend_cookie: binding_id,
        },
        view: SimplerUbGmView {
            aperture_offset: 0,
            byte_length,
            shape: vec![elems as u32],
            strides: vec![1],
            // Simpler/PTO DataType::FLOAT32 is frozen as zero in the C ABI.
            dtype: 0,
        },
        usage,
    }
}

pub fn run_host_vector_ub_gm_dispatch(
    config: &ScenarioConfig,
    topology: &SimTopology,
    manifest_path: &Path,
    platform: &str,
    elems: u64,
) -> Result<HostVectorUbGmDispatchReport, SimError> {
    if !matches!(platform, "a2a3sim" | "a5sim") {
        return Err(SimError::InvalidInput(
            "pto_ub_gm_mock_platform_must_be_a2a3sim_or_a5sim",
        ));
    }
    if elems != 128 * 128 {
        return Err(SimError::InvalidInput(
            "pto_ub_gm_host_vector_requires_16384_elements",
        ));
    }
    let ubpu_node = topology
        .ubpus
        .first()
        .map(|ubpu| ubpu.node_id)
        .ok_or(SimError::InvalidInput("missing_ubpu_node"))?;
    let byte_length = elems * std::mem::size_of::<f32>() as u64;
    let args = vec![
        ub_gm_arg(
            INPUT_A_BINDING,
            0,
            byte_length,
            elems,
            SimplerUbGmAccess::Read,
            BufferUsage::Input,
        ),
        ub_gm_arg(
            INPUT_B_BINDING,
            1,
            byte_length,
            elems,
            SimplerUbGmAccess::Read,
            BufferUsage::Input,
        ),
        ub_gm_arg(
            OUTPUT_BINDING,
            2,
            byte_length,
            elems,
            SimplerUbGmAccess::Write,
            BufferUsage::Output,
        ),
    ];
    let runtime_artifacts = load_host_vector_runtime_artifacts(manifest_path, args)?;
    let backend_spec = DispatchBackendSpec {
        profile: DispatchBackendProfile::HostVector,
        platform: platform.to_string(),
        runtime_variant: DispatchRuntimeVariant::HostBuildGraph,
        callable_hint: Some("host_vector_ub_gm_mock".to_string()),
        simpler_runtime: Some(runtime_artifacts),
        context: None,
    };

    let backend = Box::new(Mutex::new(MockBackend::new(elems)?));
    let backend_context = (&*backend as *const Mutex<MockBackend>) as usize;
    let mut runtime = LocalRuntimeEngine::from_config(config);
    runtime
        .register_pto_ub_gm_access(PtoUbGmAccessRegistration {
            read: mock_read,
            write: mock_write,
            fence: mock_fence,
            backend_context,
            pto_device_cna: PTO_DEVICE_CNA,
        })
        .map_err(|_| SimError::InvalidInput("pto_ub_gm_mock_registration_failed"))?;

    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let mut sink = VecEventSink::default();
    let completion = (|| {
        runtime.submit_dispatch(
            DispatchRequest {
                task,
                function: FunctionLabel {
                    name: "host_vector_ub_gm_mock".into(),
                    level: PlLevel::L2,
                },
                backend_spec: Some(backend_spec),
                request: None,
                target_level: PlLevel::L2,
                target_node: ubpu_node,
                input_segments: Vec::new(),
            },
            &mut sink,
        )?;
        let at = config
            .pypto
            .simpler_boundary
            .dispatch_latency_us
            .unwrap_or(15);
        runtime.advance_to(at, &mut sink);
        runtime
            .poll_completions(at, &mut sink)
            .into_iter()
            .next()
            .ok_or(SimError::InvalidInput("missing_dispatch_completion"))
    })();
    runtime.clear_pto_ub_gm_access();
    let completion = completion?;
    let segment_payload_staging_bytes = runtime.host_payload_bytes();

    let backend = backend
        .lock()
        .map_err(|_| SimError::InvalidInput("pto_ub_gm_mock_backend_poisoned"))?;
    let output = backend
        .output()
        .ok_or(SimError::InvalidInput("pto_ub_gm_mock_output_missing"))?;
    let values = bytes_to_f32s(output);
    let all_match_expected = values.iter().all(|value| (*value - 42.0).abs() < 1e-5);
    let counters = backend.counters;
    drop(backend);
    Ok(HostVectorUbGmDispatchReport {
        platform: platform.to_string(),
        elems,
        first_values: values.into_iter().take(8).collect(),
        all_match_expected,
        completion_status: completion.status,
        read_calls: counters.read_calls,
        read_bytes: counters.read_bytes,
        write_calls: counters.write_calls,
        write_bytes: counters.write_bytes,
        fence_calls: counters.fence_calls,
        segment_payload_staging_bytes,
    })
}

fn authorized_memref(
    binding_id: u64,
    slot: u64,
    byte_length: u64,
    elems: u64,
    role: sim_qemu::LingquPtoMemrefRole,
    access: u8,
    arg_index: u32,
) -> Result<sim_qemu::PtoSimUbGmAuthorizedMemrefV1, SimError> {
    let shape_elems = u32::try_from(elems)
        .map_err(|_| SimError::InvalidInput("pto_ub_gm_bridge_element_count_too_large"))?;
    Ok(sim_qemu::PtoSimUbGmAuthorizedMemrefV1 {
        memref: sim_qemu::LingquShmemMemrefV1 {
            abi_version: sim_qemu::LINGQU_SHMEM_MEMREF_ABI_V1,
            struct_bytes: std::mem::size_of::<sim_qemu::LingquShmemMemrefV1>() as u32,
            opaque_mapping_ref: binding_id,
            ub_gm_addr: UB_GM_BACKEND_BASE + slot * UB_GM_BACKEND_SLOT,
            byte_offset: 0,
            byte_length,
            shape_table_iova: 0x1000 + slot * 0x100,
            stride_table_iova: 0x1800 + slot * 0x100,
            arg_index,
            rank: 1,
            dtype: 0,
            role: role as u8,
            access,
            flags: 0,
            reserved0: 0,
            reserved1: 0,
        },
        binding: sim_qemu::PtoSimUbGmBindingV1 {
            request_id: REQUEST_ID,
            binding_id,
            aperture_base: UB_GM_APERTURE_BASE + slot * UB_GM_APERTURE_SLOT,
            aperture_length: byte_length,
            ub_gm_base: UB_GM_BACKEND_BASE + slot * UB_GM_BACKEND_SLOT,
            mapped_length: byte_length,
            access: u32::from(access),
            flags: 0,
            backend_cookie: binding_id,
        },
        shape: [shape_elems, 0, 0, 0, 0],
        strides: [1, 0, 0, 0, 0],
        reserved: 0,
    })
}

pub fn run_host_vector_ub_gm_bridge_dispatch(
    scenario_path: &Path,
    manifest_path: &Path,
    platform: &str,
    elems: u64,
) -> Result<HostVectorUbGmDispatchReport, SimError> {
    if !matches!(platform, "a2a3sim" | "a5sim") {
        return Err(SimError::InvalidInput(
            "pto_ub_gm_mock_platform_must_be_a2a3sim_or_a5sim",
        ));
    }
    if elems != 128 * 128 {
        return Err(SimError::InvalidInput(
            "pto_ub_gm_host_vector_requires_16384_elements",
        ));
    }
    let manifest_text = std::fs::read_to_string(manifest_path)
        .map_err(|_| SimError::NotFound("host_vector_manifest"))?;
    let manifest_json: serde_json::Value = serde_json::from_str(&manifest_text)
        .map_err(|_| SimError::InvalidInput("invalid_host_vector_manifest"))?;
    let manifest_platform = manifest_json["platform"].as_str().unwrap_or("a2a3sim");
    if manifest_platform != platform {
        return Err(SimError::InvalidInput(
            "pto_ub_gm_bridge_manifest_platform_mismatch",
        ));
    }
    let _env_lock = BRIDGE_MOCK_ENV_LOCK
        .lock()
        .map_err(|_| SimError::InvalidInput("pto_ub_gm_bridge_env_lock_poisoned"))?;
    let _manifest_env = EnvRestore::set("SIMPLER_HOST_VECTOR_MANIFEST", manifest_path);
    let _scenario_env = EnvRestore::set("SIM_UAPI_SCENARIO_CONFIG", scenario_path);
    let scenario_c = CString::new(
        scenario_path
            .to_str()
            .ok_or(SimError::InvalidInput("pto_ub_gm_bridge_non_utf8_scenario"))?,
    )
    .map_err(|_| SimError::InvalidInput("pto_ub_gm_bridge_invalid_scenario_path"))?;
    let bridge = BridgeHandle(sim_qemu::linqu_ub_bridge_new_from_yaml(scenario_c.as_ptr()));
    if bridge.0.is_null() {
        return Err(SimError::InvalidInput("pto_ub_gm_bridge_create_failed"));
    }
    if sim_qemu::linqu_ub_bridge_register_endpoint(bridge.0, 1, 0) != 0 {
        return Err(SimError::InvalidInput(
            "pto_ub_gm_bridge_register_endpoint_failed",
        ));
    }

    let backend = Box::new(Mutex::new(MockBackend::new(elems)?));
    let backend_context = (&*backend as *const Mutex<MockBackend>).cast_mut().cast();
    let ops = sim_qemu::PtoSimUbGmAccessOpsV1 {
        abi_version: sim_qemu::PTO_SIM_UB_GM_ACCESS_ABI_V1,
        struct_bytes: std::mem::size_of::<sim_qemu::PtoSimUbGmAccessOpsV1>() as u32,
        read: Some(mock_read),
        write: Some(mock_write),
        fence: Some(mock_fence),
    };
    if sim_qemu::linqu_ub_bridge_register_ub_gm_access_v1(
        bridge.0,
        &ops,
        backend_context,
        PTO_DEVICE_CNA,
    ) != 0
    {
        return Err(SimError::InvalidInput(
            "pto_ub_gm_bridge_register_access_failed",
        ));
    }
    let mut artifact_fingerprint = 0u64;
    if sim_qemu::linqu_ub_bridge_query_ub_gm_callable_v1(
        bridge.0,
        sim_uapi::PTO_UB_GM_HOST_VECTOR_CALLABLE_ID,
        &mut artifact_fingerprint,
    ) != 0
        || artifact_fingerprint == 0
    {
        return Err(SimError::InvalidInput(
            "pto_ub_gm_bridge_query_callable_failed",
        ));
    }

    let byte_length = elems * std::mem::size_of::<f32>() as u64;
    let memrefs = [
        authorized_memref(
            INPUT_A_BINDING,
            0,
            byte_length,
            elems,
            sim_qemu::LingquPtoMemrefRole::Input,
            sim_qemu::LINGQU_PTO_UB_GM_READ,
            0,
        )?,
        authorized_memref(
            INPUT_B_BINDING,
            1,
            byte_length,
            elems,
            sim_qemu::LingquPtoMemrefRole::Input,
            sim_qemu::LINGQU_PTO_UB_GM_READ,
            1,
        )?,
        authorized_memref(
            OUTPUT_BINDING,
            2,
            byte_length,
            elems,
            sim_qemu::LingquPtoMemrefRole::Output,
            sim_qemu::LINGQU_PTO_UB_GM_WRITE,
            2,
        )?,
    ];
    let mut control = sim_qemu::LingquPtoDispatchControlV2 {
        abi_version: sim_qemu::LINGQU_PTO_DISPATCH_ABI_V2,
        struct_bytes: std::mem::size_of::<sim_qemu::LingquPtoDispatchControlV2>() as u32,
        request_id: REQUEST_ID,
        callable_id: sim_uapi::PTO_UB_GM_HOST_VECTOR_CALLABLE_ID,
        memref_count: memrefs.len() as u32,
        scalar_count: 0,
        memref_table_iova: 0x2000,
        scalar_table_iova: 0,
        artifact_fingerprint,
        metadata_crc32: 0,
        requester_cna: PTO_DEVICE_CNA,
    };
    control.metadata_crc32 = sim_qemu::authorized_metadata_crc32(&control, &memrefs, &[])
        .map_err(|_| SimError::InvalidInput("pto_ub_gm_bridge_crc_failed"))?;
    if sim_qemu::linqu_ub_bridge_submit_ub_gm_v2(
        bridge.0,
        1,
        1,
        &control,
        memrefs.as_ptr(),
        memrefs.len() as u32,
        ptr::null(),
        0,
    ) != 0
    {
        return Err(SimError::InvalidInput("pto_ub_gm_bridge_submit_failed"));
    }
    let mut submitted = 0u32;
    let mut pending = 0u32;
    if sim_qemu::linqu_ub_bridge_ring_doorbell(bridge.0, 1, 1, &mut submitted, &mut pending) != 0
        || submitted != 1
        || pending != 0
    {
        return Err(SimError::InvalidInput("pto_ub_gm_bridge_doorbell_failed"));
    }
    let mut completion_slot = [0u8; 64];
    if sim_qemu::linqu_ub_bridge_poll_completion(
        bridge.0,
        1,
        completion_slot.as_mut_ptr(),
        completion_slot.len(),
    ) != 0
    {
        return Err(SimError::InvalidInput(
            "pto_ub_gm_bridge_completion_missing",
        ));
    }
    let completion = sim_qemu::decode_completion(&completion_slot)
        .map_err(|_| SimError::InvalidInput("pto_ub_gm_bridge_bad_completion"))?;
    let backend = backend
        .lock()
        .map_err(|_| SimError::InvalidInput("pto_ub_gm_mock_backend_poisoned"))?;
    let output = backend
        .output()
        .ok_or(SimError::InvalidInput("pto_ub_gm_mock_output_missing"))?;
    let values = bytes_to_f32s(output);
    let all_match_expected = values.iter().all(|value| (*value - 42.0).abs() < 1e-5);
    let counters = backend.counters;
    drop(backend);
    drop(bridge);
    Ok(HostVectorUbGmDispatchReport {
        platform: platform.to_string(),
        elems,
        first_values: values.into_iter().take(8).collect(),
        all_match_expected,
        completion_status: completion.status,
        read_calls: counters.read_calls,
        read_bytes: counters.read_bytes,
        write_calls: counters.write_calls,
        write_bytes: counters.write_bytes,
        fence_calls: counters.fence_calls,
        segment_payload_staging_bytes: 0,
    })
}

#[derive(Debug, Serialize)]
pub struct Qwen3PtoOperatorCheck {
    pub operation: u64,
    pub completion_status: CompletionStatus,
    pub maximum_absolute_error: f32,
    pub read_calls: u64,
    pub write_calls: u64,
    pub read_bytes: u64,
    pub write_bytes: u64,
    pub fence_calls: u64,
}

fn qwen3_operator_oracle(
    op: u64,
    shapes: [(u32, u32); 4],
    parameter: usize,
    data: &[Vec<f32>],
) -> Vec<f32> {
    let (rows, cols) = (shapes[3].0 as usize, shapes[3].1 as usize);
    let a = &data[0];
    let b = &data[1];
    let mut output = vec![0f32; rows * cols];
    for r in 0..rows {
        match op {
            1 => output[r * cols..(r + 1) * cols]
                .copy_from_slice(&a[(r + parameter) * cols..(r + parameter + 1) * cols]),
            2 => {
                let mean = a[r * cols..(r + 1) * cols]
                    .iter()
                    .map(|x| f64::from(*x).powi(2))
                    .sum::<f64>()
                    / cols as f64;
                for c in 0..cols {
                    output[r * cols + c] = (f64::from(a[r * cols + c]) * f64::from(b[c])
                        / (mean + 1e-6).sqrt()) as f32;
                }
            }
            3 => {
                let k = shapes[0].1 as usize;
                for c in 0..cols {
                    output[r * cols + c] = (0..k)
                        .map(|i| f64::from(a[r * k + i]) * f64::from(b[c * k + i]))
                        .sum::<f64>() as f32;
                }
            }
            4 => {
                for c in 0..cols / 2 {
                    let lo = a[r * cols + c];
                    let hi = a[r * cols + cols / 2 + c];
                    output[r * cols + c] = lo * b[c] - hi * b[cols / 2 + c];
                    output[r * cols + cols / 2 + c] = hi * b[c] + lo * b[cols / 2 + c];
                }
            }
            5 | 8 => {
                let count = if op == 5 { parameter + r + 1 } else { cols };
                let mut scores: Vec<f64> = (0..count)
                    .map(|t| {
                        if op == 5 {
                            (0..cols)
                                .map(|c| f64::from(a[r * cols + c]) * f64::from(b[t * cols + c]))
                                .sum::<f64>()
                                * 0.5
                        } else {
                            f64::from(a[r * cols + t]) * 0.5
                        }
                    })
                    .collect();
                let maximum = scores.iter().copied().fold(f64::NEG_INFINITY, f64::max);
                for value in &mut scores {
                    *value = (*value - maximum).exp();
                }
                let sum = scores.iter().sum::<f64>();
                for c in 0..cols {
                    output[r * cols + c] = if op == 5 {
                        (0..count)
                            .map(|t| scores[t] / sum * f64::from(data[2][t * cols + c]))
                            .sum::<f64>() as f32
                    } else {
                        (scores[c] / sum) as f32
                    };
                }
            }
            6 | 7 => {
                for c in 0..cols {
                    let i = r * cols + c;
                    output[i] = if op == 6 {
                        a[i] + b[i]
                    } else {
                        a[i] / (1.0 + (-a[i]).exp()) * b[i]
                    };
                }
            }
            _ => unreachable!("closed test operation set"),
        }
    }
    output
}

pub fn run_qwen3_pto_operator_checks(
    scenario: &Path,
    manifest: &Path,
) -> Result<Vec<Qwen3PtoOperatorCheck>, SimError> {
    let _lock = BRIDGE_MOCK_ENV_LOCK
        .lock()
        .map_err(|_| SimError::InvalidInput("pto_ub_gm_bridge_env_lock_poisoned"))?;
    let _manifest = EnvRestore::set("SIMPLER_QWEN3_PTO_MANIFEST", manifest);
    let _scenario = EnvRestore::set("SIM_UAPI_SCENARIO_CONFIG", scenario);
    let scenario_c = CString::new(scenario.to_string_lossy().as_bytes())
        .map_err(|_| SimError::InvalidInput("invalid_scenario_path"))?;
    let cases: [(u64, [(u32, u32); 4], u64); 8] = [
        (1, [(5, 8), (1, 1), (1, 1), (2, 8)], 2),
        (2, [(2, 8), (1, 8), (1, 1), (2, 8)], 0),
        (3, [(2, 8), (5, 8), (1, 1), (2, 5)], 0),
        (4, [(2, 8), (2, 4), (1, 1), (2, 8)], 0),
        (5, [(2, 8), (3, 8), (3, 8), (2, 8)], 1),
        (6, [(2, 8), (2, 8), (1, 1), (2, 8)], 0),
        (7, [(2, 8), (2, 8), (1, 1), (2, 8)], 0),
        (8, [(2, 8), (1, 1), (1, 1), (2, 8)], 0),
    ];
    let mut reports = Vec::new();
    for (op, shapes, parameter) in cases {
        let data: Vec<Vec<f32>> = shapes
            .iter()
            .enumerate()
            .map(|(index, (r, c))| {
                (0..r * c)
                    .map(|i| {
                        if index == 3 {
                            0.0
                        } else {
                            (((i * 7 + index as u32 * 3) % 29) as i32 - 14) as f32 / 11.0
                        }
                    })
                    .collect()
            })
            .collect();
        let expected = qwen3_operator_oracle(op, shapes, parameter as usize, &data);
        let backend = Box::new(Mutex::new(MockBackend {
            request_id: REQUEST_ID,
            regions: data
                .iter()
                .enumerate()
                .map(|(index, values)| MockRegion {
                    binding_id: index as u64 + 1,
                    ub_gm_base: UB_GM_BACKEND_BASE + index as u64 * UB_GM_BACKEND_SLOT,
                    access: if index == 3 {
                        MockAccess::Write
                    } else {
                        MockAccess::Read
                    },
                    bytes: f32s_to_bytes(values),
                })
                .collect(),
            counters: MockCounters::default(),
        }));
        let bridge = BridgeHandle(sim_qemu::linqu_ub_bridge_new_from_yaml(scenario_c.as_ptr()));
        if bridge.0.is_null() || sim_qemu::linqu_ub_bridge_register_endpoint(bridge.0, 1, 0) != 0 {
            return Err(SimError::InvalidInput("qwen3_pto_bridge_create_failed"));
        }
        let ops = sim_qemu::PtoSimUbGmAccessOpsV1 {
            abi_version: sim_qemu::PTO_SIM_UB_GM_ACCESS_ABI_V1,
            struct_bytes: std::mem::size_of::<sim_qemu::PtoSimUbGmAccessOpsV1>() as u32,
            read: Some(mock_read),
            write: Some(mock_write),
            fence: Some(mock_fence),
        };
        let context = (&*backend as *const Mutex<MockBackend>).cast_mut().cast();
        if sim_qemu::linqu_ub_bridge_register_ub_gm_access_v1(
            bridge.0,
            &ops,
            context,
            PTO_DEVICE_CNA,
        ) != 0
        {
            return Err(SimError::InvalidInput("qwen3_pto_bridge_access_failed"));
        }
        let mut fingerprint = 0;
        if sim_qemu::linqu_ub_bridge_query_ub_gm_callable_v1(bridge.0, 2, &mut fingerprint) != 0 {
            return Err(SimError::InvalidInput("qwen3_pto_bridge_callable_failed"));
        }
        let mut memrefs = Vec::new();
        for (index, &(r, c)) in shapes.iter().enumerate() {
            let output = index == 3;
            let mut memref = authorized_memref(
                index as u64 + 1,
                index as u64,
                u64::from(r * c) * 4,
                u64::from(r * c),
                if output {
                    sim_qemu::LingquPtoMemrefRole::Output
                } else {
                    sim_qemu::LingquPtoMemrefRole::Input
                },
                if output {
                    sim_qemu::LINGQU_PTO_UB_GM_WRITE
                } else {
                    sim_qemu::LINGQU_PTO_UB_GM_READ
                },
                index as u32,
            )?;
            memref.memref.rank = 2;
            memref.shape = [r, c, 0, 0, 0];
            memref.strides = [c, 1, 0, 0, 0];
            memrefs.push(memref);
        }
        let scale = if matches!(op, 5 | 8) {
            u64::from(0.5f32.to_bits())
        } else {
            0
        };
        let scalars: Vec<_> = [op, parameter, scale]
            .into_iter()
            .enumerate()
            .map(|(index, value)| sim_qemu::LingquPtoScalarV1 {
                abi_version: sim_qemu::LINGQU_PTO_SCALAR_ABI_V1,
                struct_bytes: std::mem::size_of::<sim_qemu::LingquPtoScalarV1>() as u32,
                arg_index: 4 + index as u32,
                dtype: if index == 2 { 0 } else { 8 },
                flags: 0,
                value,
            })
            .collect();
        let mut control = sim_qemu::LingquPtoDispatchControlV2 {
            abi_version: sim_qemu::LINGQU_PTO_DISPATCH_ABI_V2,
            struct_bytes: std::mem::size_of::<sim_qemu::LingquPtoDispatchControlV2>() as u32,
            request_id: REQUEST_ID,
            callable_id: 2,
            memref_count: 4,
            scalar_count: 3,
            memref_table_iova: 0x2000,
            scalar_table_iova: 0x3000,
            artifact_fingerprint: fingerprint,
            metadata_crc32: 0,
            requester_cna: PTO_DEVICE_CNA,
        };
        control.metadata_crc32 = sim_qemu::authorized_metadata_crc32(&control, &memrefs, &scalars)
            .map_err(|_| SimError::InvalidInput("qwen3_pto_bridge_crc_failed"))?;
        if sim_qemu::linqu_ub_bridge_submit_ub_gm_v2(
            bridge.0,
            1,
            op,
            &control,
            memrefs.as_ptr(),
            4,
            scalars.as_ptr(),
            3,
        ) != 0
        {
            return Err(SimError::InvalidInput("qwen3_pto_bridge_submit_failed"));
        }
        let (mut submitted, mut pending) = (0, 0);
        if sim_qemu::linqu_ub_bridge_ring_doorbell(bridge.0, 1, 1, &mut submitted, &mut pending)
            != 0
            || submitted != 1
            || pending != 0
        {
            return Err(SimError::InvalidInput("qwen3_pto_bridge_doorbell_failed"));
        }
        let mut slot = [0u8; 64];
        if sim_qemu::linqu_ub_bridge_poll_completion(bridge.0, 1, slot.as_mut_ptr(), slot.len())
            != 0
        {
            return Err(SimError::InvalidInput(
                "qwen3_pto_bridge_completion_missing",
            ));
        }
        let completion = sim_qemu::decode_completion(&slot)
            .map_err(|_| SimError::InvalidInput("qwen3_pto_bridge_bad_completion"))?;
        let state = backend
            .lock()
            .map_err(|_| SimError::InvalidInput("qwen3_pto_backend_poisoned"))?;
        let actual = bytes_to_f32s(&state.regions[3].bytes);
        let error = actual
            .iter()
            .zip(&expected)
            .map(|(a, b)| (a - b).abs())
            .fold(0f32, f32::max);
        if completion.status != CompletionStatus::Success
            || actual.iter().any(|x| !x.is_finite())
            || error > 3e-5
            || state.counters.read_calls == 0
            || state.counters.write_calls == 0
        {
            eprintln!(
                "qwen3_pto_operator_check_failed op={op} status={:?} error={error}",
                completion.status
            );
            return Err(SimError::InvalidInput("qwen3_pto_operator_check_failed"));
        }
        reports.push(Qwen3PtoOperatorCheck {
            operation: op,
            completion_status: completion.status,
            maximum_absolute_error: error,
            read_calls: state.counters.read_calls,
            write_calls: state.counters.write_calls,
            read_bytes: state.counters.read_bytes,
            write_bytes: state.counters.write_bytes,
            fence_calls: state.counters.fence_calls,
        });
    }
    Ok(reports)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mock_callbacks_enforce_request_binding_bounds_and_access() {
        let backend = Box::new(Mutex::new(MockBackend::new(128 * 128).expect("backend")));
        let context = (&*backend as *const Mutex<MockBackend>).cast_mut().cast();
        let mut read = vec![0u8; 16];
        let write = vec![7u8; 16];

        assert_eq!(
            unsafe {
                mock_read(
                    context,
                    REQUEST_ID,
                    INPUT_A_BINDING,
                    UB_GM_BACKEND_BASE,
                    read.as_mut_ptr().cast(),
                    read.len() as u64,
                )
            },
            0
        );
        assert_eq!(
            unsafe {
                mock_write(
                    context,
                    REQUEST_ID,
                    OUTPUT_BINDING,
                    UB_GM_BACKEND_BASE + 2 * UB_GM_BACKEND_SLOT,
                    write.as_ptr().cast(),
                    write.len() as u64,
                )
            },
            0
        );
        assert_ne!(
            unsafe {
                mock_write(
                    context,
                    REQUEST_ID,
                    INPUT_A_BINDING,
                    UB_GM_BACKEND_BASE,
                    write.as_ptr().cast(),
                    write.len() as u64,
                )
            },
            0
        );
        assert_ne!(
            unsafe {
                mock_read(
                    context,
                    REQUEST_ID + 1,
                    INPUT_A_BINDING,
                    UB_GM_BACKEND_BASE,
                    read.as_mut_ptr().cast(),
                    read.len() as u64,
                )
            },
            0
        );
    }
}
