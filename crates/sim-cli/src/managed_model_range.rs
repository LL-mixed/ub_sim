//! Replay a provider-read input bundle through the actual model command queue.
//! Offline files confer no service access and cannot validate physical leases.
use sim_config::ScenarioConfig;
use sim_core::CompletionStatus;
use sim_qemu::{GuestDescriptor, QemuBackendAdapter};
use sim_services::managed_object_ref::ManagedObjectRef;
use sim_topology::SimTopology;
use sim_uapi::{
    managed_model_input::{ManagedModelInput, ManagedServingObjectKey},
    ModelRangeDispatchReq,
};
use std::{ffi::OsStr, fs, io::Write, path::PathBuf};

const USAGE: &str = "managed-model-range-run --scenario <yaml> --descriptor <64-byte file> \
    --segment <input file> --output <new file> [--provider-callbacks] \
    [--input <V2 reference file> <payload file>]...";

pub fn run_if_requested() -> anyhow::Result<Option<()>> {
    let mut args = std::env::args_os().skip(1);
    let command = args.next();
    if command.as_deref() == Some(OsStr::new("managed-model-object-key")) {
        let parse = |value: Option<std::ffi::OsString>| -> anyhow::Result<u64> {
            Ok(value
                .ok_or_else(|| {
                    anyhow::anyhow!("managed-model-object-key <run> <request> <producer> <step>")
                })?
                .to_str()
                .ok_or_else(|| anyhow::anyhow!("integer must be UTF-8"))?
                .parse()?)
        };
        let key = ManagedServingObjectKey {
            run: parse(args.next())?,
            request: parse(args.next())?,
            producer: u32::try_from(parse(args.next())?)?,
            step: parse(args.next())?,
        };
        anyhow::ensure!(
            args.next().is_none(),
            "unexpected managed object key argument"
        );
        println!("{}", key.encode().map_err(anyhow::Error::msg)?);
        return Ok(Some(()));
    }
    if command.as_deref() == Some(OsStr::new("managed-model-provider-fingerprint")) {
        anyhow::ensure!(args.next().is_none(), "fingerprint accepts no arguments");
        println!(
            "{}",
            sim_uapi::managed_model_dispatch::fingerprint().map_err(anyhow::Error::msg)?
        );
        return Ok(Some(()));
    }
    if command.as_deref() != Some(OsStr::new("managed-model-range-run")) {
        return Ok(None);
    }
    let mut scenario = None;
    let mut descriptor = None;
    let mut segment = None;
    let mut output = None;
    let mut input_paths = Vec::new();
    let mut provider_callbacks = false;
    while let Some(flag) = args.next() {
        if flag == "--provider-callbacks" {
            anyhow::ensure!(!provider_callbacks, "duplicate option");
            provider_callbacks = true;
            continue;
        }
        let value = PathBuf::from(args.next().ok_or_else(|| anyhow::anyhow!(USAGE))?);
        let destination = match flag.to_str() {
            Some("--scenario") => &mut scenario,
            Some("--descriptor") => &mut descriptor,
            Some("--segment") => &mut segment,
            Some("--output") => &mut output,
            Some("--input") => {
                anyhow::ensure!(input_paths.len() < 2, "at most two plain W5 inputs");
                let payload = PathBuf::from(args.next().ok_or_else(|| anyhow::anyhow!(USAGE))?);
                input_paths.push((value, payload));
                continue;
            }
            _ => anyhow::bail!(USAGE),
        };
        anyhow::ensure!(destination.replace(value).is_none(), "duplicate option");
    }
    let required = |value: Option<PathBuf>| value.ok_or_else(|| anyhow::anyhow!(USAGE));
    let scenario = required(scenario)?;
    let descriptor = required(descriptor)?;
    let segment = required(segment)?;
    let output = required(output)?;
    anyhow::ensure!(!output.exists(), "output already exists");
    let slot = fs::read(descriptor)?;
    anyhow::ensure!(slot.len() == 64, "descriptor must contain exactly 64 bytes");
    let GuestDescriptor::ModelRangeDispatch(req) =
        GuestDescriptor::decode(&slot).map_err(anyhow::Error::msg)?
    else {
        anyhow::bail!("expected model-range descriptor");
    };
    anyhow::ensure!(
        req.object_ref_count as usize == input_paths.len(),
        "input count mismatch"
    );
    let inputs = input_paths
        .into_iter()
        .map(|(reference, payload)| {
            let reference = ManagedObjectRef::from_le_bytes(&fs::read(reference)?)
                .map_err(anyhow::Error::msg)?;
            ManagedModelInput::from_provider_bytes(reference, fs::read(payload)?)
                .map_err(anyhow::Error::msg)
        })
        .collect::<anyhow::Result<Vec<_>>>()?;
    let bytes = fs::read(segment)?;
    let config = ScenarioConfig::from_yaml_file(scenario)?;
    let topology = SimTopology::from_config(&config)?;
    if provider_callbacks {
        return replay_callbacks(topology, req, bytes, inputs, output).map(Some);
    }
    let mut adapter = QemuBackendAdapter::new(topology);
    let session = adapter.register_endpoint(req.task.coord.levels[1])?;
    // A replay owns a fresh segment; the file's process-local handle is replaced.
    let segment = adapter.create_segment(&session, bytes.len() as u64)?;
    adapter.write_segment_payload(segment, 0, &bytes)?;
    let op_id = req.op_id;
    adapter.enqueue_managed_model_range(
        &session,
        ModelRangeDispatchReq {
            op_id,
            segment,
            task: req.task,
            object_ref_table_offset: req.object_ref_table_offset,
            object_ref_count: req.object_ref_count,
        },
        inputs,
    )?;
    adapter.ring_doorbell(&session, None)?;
    let events = adapter.drain_cq(&session)?;
    anyhow::ensure!(
        events.len() == 1 && events[0].op_id == op_id,
        "original completion missing"
    );
    anyhow::ensure!(
        matches!(events[0].status, CompletionStatus::Success),
        "managed model dispatch failed: {:?}",
        events[0].status
    );
    let mut result = vec![0; bytes.len()];
    adapter.read_segment_payload(segment, 0, &mut result)?;
    fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(output)?
        .write_all(&result)?;
    println!(
        "{}",
        serde_json::json!({
            "op_id": op_id, "completion": "success", "managed_inputs": true,
            "input_source": "offline_provider_read_bundle", "service_access_granted": false,
            "physical_lease_verified": false,
        })
    );
    Ok(Some(()))
}

struct ReplayMemory {
    buffers: Vec<Vec<u8>>,
    reads: usize,
    writes: usize,
    fences: usize,
    request: u64,
    written_bytes: u64,
}

impl ReplayMemory {
    fn registration(&mut self) -> sim_core::PtoUbGmAccessRegistration {
        // This offline fixture has one provider. Its nonzero transport identity
        // is independent of the model's zero-based logical node coordinate.
        sim_core::PtoUbGmAccessRegistration {
            read: replay_read,
            write: replay_write,
            fence: replay_fence,
            backend_context: self as *mut Self as usize,
            pto_device_cna: 0xf001,
        }
    }
}

fn replay_view(
    memory: &mut ReplayMemory,
    request: u64,
    binding: u64,
    address: u64,
    len: u64,
    writing: bool,
) -> Option<&mut [u8]> {
    let index = usize::try_from(binding.checked_sub(1)?).ok()?;
    if request != memory.request || writing != (index + 1 == memory.buffers.len()) {
        return None;
    }
    let offset = address.checked_sub(binding.checked_mul(0x1000_0000)?)?;
    let end = offset.checked_add(len)?;
    memory
        .buffers
        .get_mut(index)?
        .get_mut(usize::try_from(offset).ok()?..usize::try_from(end).ok()?)
}

unsafe extern "C" fn replay_read(
    ctx: *mut std::ffi::c_void,
    request: u64,
    binding: u64,
    address: u64,
    dst: *mut std::ffi::c_void,
    len: u64,
) -> i32 {
    let memory = unsafe { &mut *ctx.cast::<ReplayMemory>() };
    let Some(bytes) = replay_view(memory, request, binding, address, len, false) else {
        return -1;
    };
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), dst.cast(), bytes.len());
    }
    memory.reads += 1;
    0
}

unsafe extern "C" fn replay_write(
    ctx: *mut std::ffi::c_void,
    request: u64,
    binding: u64,
    address: u64,
    src: *const std::ffi::c_void,
    len: u64,
) -> i32 {
    let memory = unsafe { &mut *ctx.cast::<ReplayMemory>() };
    let Some(bytes) = replay_view(memory, request, binding, address, len, true) else {
        return -1;
    };
    bytes.copy_from_slice(unsafe { std::slice::from_raw_parts(src.cast(), len as usize) });
    memory.writes += 1;
    memory.written_bytes = len;
    0
}

unsafe extern "C" fn replay_fence(
    ctx: *mut std::ffi::c_void,
    request: u64,
    binding: u64,
    address: u64,
    len: u64,
    flags: u32,
) -> i32 {
    let memory = unsafe { &mut *ctx.cast::<ReplayMemory>() };
    if flags != 0
        || memory.writes != 1
        || replay_view(memory, request, binding, address, len, true).is_none()
    {
        return -1;
    }
    memory.fences += 1;
    0
}

fn replay_callbacks(
    topology: SimTopology,
    req: sim_qemu::GuestModelRangeDispatchDescriptor,
    bytes: Vec<u8>,
    inputs: Vec<ManagedModelInput>,
    output: PathBuf,
) -> anyhow::Result<()> {
    use sim_core::{
        BufferUsage, SimplerRuntimeArg, SimplerUbGmAccess, SimplerUbGmBinding, SimplerUbGmView,
    };
    use sim_uapi::managed_model_dispatch::{
        encode_control, fingerprint, CALLABLE_ID, MAX_VIEW_BYTES, PREFIX_BYTES,
    };
    anyhow::ensure!(
        bytes.len() >= PREFIX_BYTES && bytes.len() as u64 <= MAX_VIEW_BYTES,
        "invalid segment capacity"
    );
    // Match each file reference against the original descriptor table before
    // repacking control. Never silently replace a stale table with supplied refs.
    let start = req.object_ref_table_offset as usize;
    let table = bytes
        .get(
            start
                ..start
                    .checked_add(inputs.len() * 256)
                    .ok_or_else(|| anyhow::anyhow!("reference table overflow"))?,
        )
        .ok_or_else(|| anyhow::anyhow!("reference table out of bounds"))?;
    let refs = table
        .chunks_exact(256)
        .map(ManagedObjectRef::from_le_bytes)
        .collect::<Result<Vec<_>, _>>()
        .map_err(anyhow::Error::msg)?;
    let control = encode_control(
        req.op_id,
        req.op_id,
        req.task.coord.levels,
        &bytes[..PREFIX_BYTES],
        &refs,
    )
    .map_err(anyhow::Error::msg)?;
    let mut buffers = vec![control];
    for reference in &refs {
        let input = inputs
            .iter()
            .find(|input| input.reference() == reference)
            .ok_or_else(|| anyhow::anyhow!("managed_model_input_identity_mismatch"))?;
        buffers.push(input.bytes().to_vec());
    }
    // Separate output allocation, initially zero; inline input never aliases it.
    buffers.push(vec![0; bytes.len()]);
    let mut memory = Box::new(ReplayMemory {
        buffers,
        reads: 0,
        writes: 0,
        fences: 0,
        request: req.op_id,
        written_bytes: 0,
    });
    let args = memory
        .buffers
        .iter()
        .enumerate()
        .map(|(i, buffer)| {
            let output = i + 1 == memory.buffers.len();
            let base = (i as u64 + 1) * 0x1000_0000;
            SimplerRuntimeArg::UbGmMemref {
                binding: SimplerUbGmBinding {
                    request_id: req.op_id,
                    binding_id: i as u64 + 1,
                    aperture_base: base,
                    aperture_length: buffer.len() as u64,
                    ub_gm_base: base,
                    mapped_length: buffer.len() as u64,
                    flags: 0,
                    backend_cookie: i as u64 + 1,
                    access: if output {
                        SimplerUbGmAccess::Write
                    } else {
                        SimplerUbGmAccess::Read
                    },
                },
                view: SimplerUbGmView {
                    aperture_offset: 0,
                    byte_length: buffer.len() as u64,
                    shape: vec![buffer.len() as u32],
                    strides: vec![1],
                    dtype: 4,
                },
                usage: if output {
                    BufferUsage::Output
                } else {
                    BufferUsage::Input
                },
            }
        })
        .collect();
    let mut adapter = QemuBackendAdapter::new(topology);
    let session = adapter.register_endpoint(req.task.coord.levels[1])?;
    let registration = memory.registration();
    let requester = registration.pto_device_cna;
    adapter.register_pto_ub_gm_access(registration)?;
    adapter.enqueue_pto_ub_gm_dispatch(
        &session,
        sim_uapi::PtoUbGmDispatchV2Req {
            op_id: req.op_id,
            request_id: req.op_id,
            callable_id: CALLABLE_ID,
            artifact_fingerprint: fingerprint().map_err(anyhow::Error::msg)?,
            requester_cna: requester,
            args,
        },
    )?;
    adapter.ring_doorbell(&session, None)?;
    let events = adapter.drain_cq(&session)?;
    anyhow::ensure!(
        events.len() == 1 && events[0].op_id == req.op_id,
        "original completion missing"
    );
    anyhow::ensure!(
        matches!(events[0].status, CompletionStatus::Success),
        "managed provider dispatch failed: {:?}",
        events[0].status
    );
    anyhow::ensure!(
        memory.reads == inputs.len() + 1 && memory.writes == 1 && memory.fences == 1,
        "provider callback count mismatch"
    );
    fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(output)?
        .write_all(memory.buffers.last().unwrap())?;
    println!(
        "{}",
        serde_json::json!({"op_id": req.op_id, "completion": "success",
        "input_source": "offline_provider_callback_fixture", "service_access_granted": false,
        "physical_lease_verified": false, "reads": memory.reads, "writes": memory.writes,
        "fences": memory.fences, "output_bytes": memory.written_bytes})
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn replay_provider_identity_is_independent_of_zero_based_model_node() {
        let config = ScenarioConfig::from_yaml_file(
            PathBuf::from(env!("CARGO_MANIFEST_DIR"))
                .join("../../scenarios/mvp_2host_single_domain.yaml"),
        )
        .unwrap();
        for node in [0, 1] {
            let mut memory = ReplayMemory {
                buffers: vec![vec![0; 64]],
                reads: 0,
                writes: 0,
                fences: 0,
                request: 17,
                written_bytes: 0,
            };
            let registration = memory.registration();
            assert_eq!(registration.pto_device_cna, 0xf001);
            let mut adapter = QemuBackendAdapter::new(SimTopology::from_config(&config).unwrap());
            let session = adapter.register_endpoint(node).unwrap();
            assert_eq!(session.entity, node);
            adapter.register_pto_ub_gm_access(registration).unwrap();
            let data = [1u8, 2, 3, 4];
            assert_eq!(
                unsafe {
                    (registration.write)(
                        registration.backend_context as *mut _,
                        17,
                        1,
                        0x1000_0000,
                        data.as_ptr().cast(),
                        data.len() as u64,
                    )
                },
                0
            );
            assert_eq!(memory.written_bytes, 4);
            assert_eq!(&memory.buffers[0][..4], &data);
            assert_eq!(&memory.buffers[0][4..], &[0; 60]);
        }
    }
}
