use std::ffi::c_void;
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

const UB_GM_APERTURE_BASE: u64 = 0x7000_0000_0000;
const UB_GM_APERTURE_SLOT: u64 = 0x0000_0002_0000;
const UB_GM_BACKEND_BASE: u64 = 0x0010_0000;
const UB_GM_BACKEND_SLOT: u64 = 0x0002_0000;
const REQUEST_ID: u64 = 0x5054_4f32;
const PTO_DEVICE_CNA: u32 = 0x0001_0001;
const INPUT_A_BINDING: u64 = 1;
const INPUT_B_BINDING: u64 = 2;
const OUTPUT_BINDING: u64 = 3;

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
        let index = self
            .regions
            .iter()
            .position(|region| region.binding_id == binding_id && region.access == required)?;
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
    Ok(HostVectorUbGmDispatchReport {
        platform: platform.to_string(),
        elems,
        first_values: values.into_iter().take(8).collect(),
        all_match_expected,
        completion_status: completion.status,
        read_calls: backend.counters.read_calls,
        read_bytes: backend.counters.read_bytes,
        write_calls: backend.counters.write_calls,
        write_bytes: backend.counters.write_bytes,
        fence_calls: backend.counters.fence_calls,
        segment_payload_staging_bytes,
    })
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
