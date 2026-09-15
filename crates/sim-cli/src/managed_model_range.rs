//! Replay a provider-read input bundle through the actual model command queue.
//! Offline files confer no service access and cannot validate physical leases.
use sim_config::ScenarioConfig;
use sim_core::CompletionStatus;
use sim_qemu::{GuestDescriptor, QemuBackendAdapter};
use sim_services::managed_object_ref::ManagedObjectRef;
use sim_topology::SimTopology;
use sim_uapi::{managed_model_input::ManagedModelInput, ModelRangeDispatchReq};
use std::{ffi::OsStr, fs, io::Write, path::PathBuf};

const USAGE: &str = "managed-model-range-run --scenario <yaml> --descriptor <64-byte file> \
    --segment <input file> --output <new file> [--input <V2 reference file> <payload file>]...";

pub fn run_if_requested() -> anyhow::Result<Option<()>> {
    let mut args = std::env::args_os().skip(1);
    if args.next().as_deref() != Some(OsStr::new("managed-model-range-run")) {
        return Ok(None);
    }
    let mut scenario = None;
    let mut descriptor = None;
    let mut segment = None;
    let mut output = None;
    let mut input_paths = Vec::new();
    while let Some(flag) = args.next() {
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
