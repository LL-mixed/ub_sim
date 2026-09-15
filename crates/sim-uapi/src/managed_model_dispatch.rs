//! W5 execution on the existing request-bound provider access callbacks.
//! The guest retains SDK mapping owners until the original CQ completion.
//! Unlike the PTO numerical kernels, the existing Rust model runtime owns
//! temporary input/output bytes; no payload is sent through the service core.

use super::*;
use sim_core::{SimplerUbGmAccess, SimplerUbGmBinding, SimplerUbGmView};
use sim_services::managed_object_ref::ManagedObjectRef;

pub const CALLABLE_ID: u64 = 4;
pub const CONTROL_MAGIC: u64 = 0x314d_474d_3557_514c;
pub const PREFIX_BYTES: usize = 8192;
pub const CONTROL_BYTES: usize = 64 + PREFIX_BYTES + 2 * ManagedObjectRef::BYTE_LEN;
pub const MAX_VIEW_BYTES: u64 = 64 * 1024 * 1024;

fn read_u32_le_at(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().unwrap())
}

/// Fingerprints the compiled adapter and model dispatch source, not the weights.
pub fn fingerprint() -> Result<u64, String> {
    let profile = std::env::var("SIM_UAPI_W4_CHIPBACKEND_PROFILE").unwrap_or_default();
    if !matches!(
        profile.as_str(),
        "qwen3_dense" | "deepseek-v4-flash-simpler" | "deepseek_v4_flash_simpler"
    ) {
        return Err("managed_model_backend_unsupported".into());
    }
    Ok(include_bytes!("managed_model_dispatch.rs")
        .iter()
        .chain(include_bytes!("managed_model_input.rs"))
        .chain(include_bytes!("lib.rs"))
        .chain(profile.as_bytes())
        .fold(0xcbf2_9ce4_8422_2325u64, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(0x100_0000_01b3)
        }))
}

/// Fixed little-endian control view. Reserved bytes and unused refs are zero.
pub fn encode_control(
    op_id: u64,
    request_id: u64,
    levels: [u32; 8],
    prefix: &[u8],
    references: &[ManagedObjectRef],
) -> Result<Vec<u8>, String> {
    if op_id == 0 || request_id == 0 || prefix.len() > PREFIX_BYTES || references.len() > 2 {
        return Err("managed_model_bad_control".into());
    }
    let mut bytes = vec![0; CONTROL_BYTES];
    bytes[0..8].copy_from_slice(&CONTROL_MAGIC.to_le_bytes());
    bytes[8..12].copy_from_slice(&1u32.to_le_bytes());
    bytes[12..16].copy_from_slice(&(references.len() as u32).to_le_bytes());
    bytes[16..24].copy_from_slice(&op_id.to_le_bytes());
    bytes[24..32].copy_from_slice(&request_id.to_le_bytes());
    for (i, level) in levels.iter().enumerate() {
        bytes[32 + i * 4..36 + i * 4].copy_from_slice(&level.to_le_bytes());
    }
    bytes[64..64 + prefix.len()].copy_from_slice(prefix);
    for (i, reference) in references.iter().enumerate() {
        let start = 64 + PREFIX_BYTES + i * ManagedObjectRef::BYTE_LEN;
        bytes[start..start + ManagedObjectRef::BYTE_LEN].copy_from_slice(reference.as_le_bytes());
    }
    Ok(bytes)
}

struct View<'a> {
    binding: &'a SimplerUbGmBinding,
    view: &'a SimplerUbGmView,
    address: u64,
}

fn views(req: &PtoUbGmDispatchV2Req) -> Result<Vec<View<'_>>, String> {
    if req.op_id == 0 || req.request_id == 0 || !(2..=4).contains(&req.args.len()) {
        return Err("managed_model_bad_control".into());
    }
    let mut result = Vec::new();
    for (i, arg) in req.args.iter().enumerate() {
        let SimplerRuntimeArg::UbGmMemref {
            binding,
            view,
            usage,
        } = arg
        else {
            return Err("managed_model_bad_memref".into());
        };
        let output = i + 1 == req.args.len();
        let permission = if output {
            SimplerUbGmAccess::Write
        } else {
            SimplerUbGmAccess::Read
        };
        let expected_usage = if output {
            BufferUsage::Output
        } else {
            BufferUsage::Input
        };
        let address = binding
            .aperture_base
            .checked_add(view.aperture_offset)
            .ok_or("managed_model_view_overflow")?;
        if binding.request_id != req.request_id
            || binding.binding_id != i as u64 + 1
            || binding.backend_cookie == 0
            || binding.flags != 0
            || binding.access != permission
            || *usage != expected_usage
            || view.dtype != 4
            || view.shape != [view.byte_length as u32]
            || view.strides != [1]
            || view.byte_length == 0
            || view.byte_length > MAX_VIEW_BYTES
            || view.aperture_offset > binding.aperture_length
            || view.byte_length > binding.aperture_length - view.aperture_offset
            || address < binding.ub_gm_base
            || address - binding.ub_gm_base > binding.mapped_length
            || view.byte_length > binding.mapped_length - (address - binding.ub_gm_base)
            || address.checked_add(view.byte_length).is_none()
        {
            return Err("managed_model_bad_memref".into());
        }
        result.push(View {
            binding,
            view,
            address,
        });
    }
    if result[0].view.byte_length != CONTROL_BYTES as u64 {
        return Err("managed_model_control_length".into());
    }
    // An output must not overwrite any live control/input view.
    let output = result.last().unwrap();
    for input in &result[..result.len() - 1] {
        if output.address < input.address + input.view.byte_length
            && input.address < output.address + output.view.byte_length
        {
            return Err("managed_model_output_alias".into());
        }
    }
    Ok(result)
}

fn read(access: PtoUbGmAccessRegistration, view: &View<'_>) -> Result<Vec<u8>, String> {
    let mut bytes = vec![0; view.view.byte_length as usize];
    let rc = unsafe {
        (access.read)(
            access.backend_context as *mut std::ffi::c_void,
            view.binding.request_id,
            view.binding.binding_id,
            view.address,
            bytes.as_mut_ptr().cast(),
            bytes.len() as u64,
        )
    };
    if rc != 0 {
        return Err(format!("managed_model_provider_read:{rc}"));
    }
    Ok(bytes)
}

pub(super) fn run(
    topology: &SimTopology,
    req: &PtoUbGmDispatchV2Req,
    access: PtoUbGmAccessRegistration,
) -> Result<(), String> {
    execute(req, access, |task, segment, operands| {
        run_model_range_chipbackend(topology, task, segment, Some(operands))
    })
}

fn execute(
    req: &PtoUbGmDispatchV2Req,
    access: PtoUbGmAccessRegistration,
    compute: impl FnOnce(&TaskKey, &[u8], &ModelRangeDispatchOperands) -> Result<Vec<u8>, String>,
) -> Result<(), String> {
    if req.callable_id != CALLABLE_ID
        || req.artifact_fingerprint != fingerprint()?
        || req.requester_cna != access.pto_device_cna
    {
        return Err("managed_model_dispatch_identity".into());
    }
    let views = views(req)?;
    let control = read(access, &views[0])?;
    let count = read_u32_le_at(&control, 12) as usize;
    if read_u64_le_at(&control, 0) != CONTROL_MAGIC
        || read_u32_le_at(&control, 8) != 1
        || count != views.len() - 2
        || read_u64_le_at(&control, 16) != req.op_id
        || read_u64_le_at(&control, 24) != req.request_id
        || control[64 + PREFIX_BYTES + count * ManagedObjectRef::BYTE_LEN..]
            .iter()
            .any(|byte| *byte != 0)
    {
        return Err("managed_model_bad_control".into());
    }
    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        scope_depth: 8,
        task_id: req.op_id,
        coord: HierarchyCoord {
            levels: std::array::from_fn(|i| read_u32_le_at(&control, 32 + i * 4)),
        },
    };
    // Preserve prompt control only. The model cannot fall back to inline tensors.
    let mut segment =
        vec![0; QWEN3_DENSE_PROFILE_OBJECT_REF_TABLE_OFFSET + count * ManagedObjectRef::BYTE_LEN];
    segment[..PREFIX_BYTES].copy_from_slice(&control[64..64 + PREFIX_BYTES]);
    segment[QWEN3_DENSE_PROFILE_OBJECT_REF_TABLE_OFFSET..].copy_from_slice(
        &control[64 + PREFIX_BYTES..64 + PREFIX_BYTES + count * ManagedObjectRef::BYTE_LEN],
    );
    let mut inputs = Vec::new();
    for (i, view) in views[1..views.len() - 1].iter().enumerate() {
        let start = 64 + PREFIX_BYTES + i * ManagedObjectRef::BYTE_LEN;
        let reference =
            ManagedObjectRef::from_le_bytes(&control[start..start + ManagedObjectRef::BYTE_LEN])?;
        if reference.payload_bytes() != view.view.byte_length {
            return Err("managed_model_input_length_mismatch".into());
        }
        inputs.push(ManagedModelInput::from_provider_bytes(
            reference,
            read(access, view)?,
        )?);
    }
    let model_req = ModelRangeDispatchReq {
        op_id: req.op_id,
        task: task.clone(),
        segment: SegmentHandle(0),
        object_ref_count: count as u32,
        object_ref_table_offset: QWEN3_DENSE_PROFILE_OBJECT_REF_TABLE_OFFSET as u32,
    };
    let operands = resolve_managed_operands(&model_req, &segment, inputs)?;
    let bytes = compute(&task, &segment, &operands)?;
    let output = views.last().unwrap();
    if bytes.is_empty() || bytes.len() as u64 > output.view.byte_length {
        return Err("managed_model_output_length".into());
    }
    let context = access.backend_context as *mut std::ffi::c_void;
    let rc = unsafe {
        (access.write)(
            context,
            req.request_id,
            output.binding.binding_id,
            output.address,
            bytes.as_ptr().cast(),
            bytes.len() as u64,
        )
    };
    if rc != 0 {
        return Err(format!("managed_model_provider_write:{rc}"));
    }
    let rc = unsafe {
        (access.fence)(
            context,
            req.request_id,
            output.binding.binding_id,
            output.address,
            bytes.len() as u64,
            0,
        )
    };
    if rc != 0 {
        return Err(format!("managed_model_provider_fence:{rc}"));
    }
    eprintln!("managed-model-provider: op_id={} request_id={} inputs={} output_bytes={} completion=success",
        req.op_id, req.request_id, count, bytes.len());
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::c_void;

    struct Memory {
        control: Vec<u8>,
        output: Vec<u8>,
        calls: Vec<&'static str>,
        fault: &'static str,
    }
    unsafe extern "C" fn read_cb(
        ctx: *mut c_void,
        request: u64,
        binding: u64,
        address: u64,
        dst: *mut c_void,
        length: u64,
    ) -> i32 {
        let memory = unsafe { &mut *ctx.cast::<Memory>() };
        memory.calls.push("read");
        if memory.fault == "read"
            || request != 19
            || binding != 1
            || address != 0x10000
            || length != CONTROL_BYTES as u64
        {
            return -5;
        }
        unsafe {
            std::ptr::copy_nonoverlapping(memory.control.as_ptr(), dst.cast(), length as usize);
        }
        0
    }
    unsafe extern "C" fn write_cb(
        ctx: *mut c_void,
        request: u64,
        binding: u64,
        address: u64,
        src: *const c_void,
        length: u64,
    ) -> i32 {
        let memory = unsafe { &mut *ctx.cast::<Memory>() };
        memory.calls.push("write");
        if memory.fault == "write"
            || request != 19
            || binding != 2
            || address != 0x20000
            || length > memory.output.len() as u64
        {
            return -5;
        }
        memory.output[..length as usize]
            .copy_from_slice(unsafe { std::slice::from_raw_parts(src.cast(), length as usize) });
        0
    }
    unsafe extern "C" fn fence_cb(
        ctx: *mut c_void,
        request: u64,
        binding: u64,
        address: u64,
        length: u64,
        flags: u32,
    ) -> i32 {
        let memory = unsafe { &mut *ctx.cast::<Memory>() };
        memory.calls.push("fence");
        if memory.fault == "fence"
            || request != 19
            || binding != 2
            || address != 0x20000
            || length != 4
            || flags != 0
        {
            return -5;
        }
        0
    }

    fn setup() -> (Memory, PtoUbGmDispatchV2Req) {
        let levels = [
            0x5133_060b,
            0,
            0,
            1,
            2,
            qwen3_dense_runtime_tp_nodes() as u32,
            qwen3_dense_runtime_total_layers() as u32,
            qwen3_dense_runtime_hidden_range_bytes() as u32,
        ];
        let memory = Memory {
            control: encode_control(17, 19, levels, &[], &[]).unwrap(),
            output: vec![0xa5; 32],
            calls: vec![],
            fault: "",
        };
        let req = PtoUbGmDispatchV2Req {
            op_id: 17,
            request_id: 19,
            callable_id: CALLABLE_ID,
            requester_cna: 7,
            artifact_fingerprint: fingerprint().unwrap(),
            args: [(0x10000, CONTROL_BYTES as u64), (0x20000, 32)]
                .iter()
                .enumerate()
                .map(|(i, &(address, len))| SimplerRuntimeArg::UbGmMemref {
                    binding: SimplerUbGmBinding {
                        request_id: 19,
                        binding_id: i as u64 + 1,
                        aperture_base: address,
                        aperture_length: len,
                        ub_gm_base: address,
                        mapped_length: len,
                        access: if i == 0 {
                            SimplerUbGmAccess::Read
                        } else {
                            SimplerUbGmAccess::Write
                        },
                        flags: 0,
                        backend_cookie: 17,
                    },
                    view: SimplerUbGmView {
                        aperture_offset: 0,
                        byte_length: len,
                        shape: vec![len as u32],
                        strides: vec![1],
                        dtype: 4,
                    },
                    usage: if i == 0 {
                        BufferUsage::Input
                    } else {
                        BufferUsage::Output
                    },
                })
                .collect(),
        };
        (memory, req)
    }
    fn access(memory: &mut Memory) -> PtoUbGmAccessRegistration {
        PtoUbGmAccessRegistration {
            read: read_cb,
            write: write_cb,
            fence: fence_cb,
            backend_context: memory as *mut Memory as usize,
            pto_device_cna: 7,
        }
    }

    #[test]
    fn registered_io_rejects_invalid_views_and_propagates_failures() {
        const CHILD: &str = "MANAGED_MODEL_PROVIDER_TEST_CHILD";
        if std::env::var_os(CHILD).is_none() {
            let mut child = std::process::Command::new(std::env::current_exe().unwrap());
            for (key, _) in std::env::vars_os() {
                if key.to_string_lossy().starts_with("SIM_") {
                    child.env_remove(key);
                }
            }
            assert!(child.args(["--exact", "managed_model_dispatch::tests::registered_io_rejects_invalid_views_and_propagates_failures", "--nocapture"])
                .env(CHILD, "1").env("SIM_UAPI_W4_CHIPBACKEND_PROFILE", "qwen3_dense")
                .status().unwrap().success());
            return;
        }
        let (mut memory, req) = setup();
        execute(&req, access(&mut memory), |task, segment, operands| {
            assert_eq!(task.task_id, 17);
            assert!(operands.managed);
            assert!(segment[8192..].iter().all(|byte| *byte == 0));
            Ok(vec![1, 2, 3, 4])
        })
        .unwrap();
        assert_eq!(memory.calls, ["read", "write", "fence"]);
        assert_eq!(&memory.output[..4], &[1, 2, 3, 4]);
        assert!(memory.output[4..].iter().all(|byte| *byte == 0xa5));
        for (fault, expected) in [
            ("read", vec!["read"]),
            ("write", vec!["read", "write"]),
            ("fence", vec!["read", "write", "fence"]),
        ] {
            let (mut memory, req) = setup();
            memory.fault = fault;
            assert!(
                execute(&req, access(&mut memory), |_, _, _| Ok(vec![1, 2, 3, 4]))
                    .unwrap_err()
                    .contains(fault)
            );
            assert_eq!(memory.calls, expected);
        }
        for fault in 0..12 {
            let (mut memory, mut req) = setup();
            if fault == 0 {
                req.artifact_fingerprint ^= 1;
            }
            if fault == 1 {
                req.requester_cna += 1;
            }
            if fault == 2 {
                req.request_id += 1;
            }
            if let SimplerRuntimeArg::UbGmMemref {
                binding,
                view,
                usage,
            } = &mut req.args[1]
            {
                match fault {
                    3 => binding.access = SimplerUbGmAccess::ReadWrite,
                    4 => binding.flags = 1,
                    5 => view.byte_length += 1,
                    6 => view.aperture_offset = u64::MAX,
                    7 => binding.binding_id = 1,
                    8 => binding.backend_cookie = 0,
                    9 => *usage = BufferUsage::Input,
                    10 => view.dtype = 0,
                    11 => {
                        binding.aperture_base = 0x10000;
                        binding.ub_gm_base = 0x10000;
                    }
                    _ => (),
                }
            }
            assert!(
                execute(&req, access(&mut memory), |_, _, _| panic!(
                    "invalid dispatch computed"
                ))
                .is_err(),
                "fault {fault}"
            );
            assert!(memory.calls.is_empty(), "fault {fault}");
        }
        for offset in [0, 8, 12, 16, 24, CONTROL_BYTES - 1] {
            let (mut memory, req) = setup();
            memory.control[offset] ^= 1;
            assert!(execute(&req, access(&mut memory), |_, _, _| panic!(
                "bad control computed"
            ))
            .is_err());
            assert_eq!(memory.calls, ["read"]);
        }
        for size in [0, 33] {
            let (mut memory, req) = setup();
            assert_eq!(
                execute(&req, access(&mut memory), |_, _, _| Ok(vec![0; size])).unwrap_err(),
                "managed_model_output_length"
            );
            assert_eq!(memory.calls, ["read"]);
        }
        let (mut memory, req) = setup();
        assert_eq!(
            execute(&req, access(&mut memory), |_, _, _| Err(
                "model_failure".into()
            ))
            .unwrap_err(),
            "model_failure"
        );
        assert_eq!(memory.calls, ["read"]);
    }
}
