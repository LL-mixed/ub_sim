use std::env;
use std::path::PathBuf;

use sim_config::ScenarioConfig;
use sim_models::qwen3_dense_0_6b::{
    load_safetensors_path_metadata, materialize_weight_slice_payload, tensor_parallel_plan,
    weight_manifest_from_metadata, weight_service_load_plan, QWEN3_DENSE_0_6B_PROFILE,
};
use sim_topology::SimTopology;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        eprintln!(
            "usage: qwen3_weight_loader <scenario.yaml> <model.safetensors|model.safetensors.index.json|model_dir> [--materialize-check]"
        );
        std::process::exit(2);
    }
    let scenario_path = PathBuf::from(&args[1]);
    let weights_path = PathBuf::from(&args[2]);
    let materialize_check = args.iter().any(|arg| arg == "--materialize-check");

    let config = ScenarioConfig::from_yaml_file(&scenario_path)?;
    let topology = SimTopology::from_config(&config)?;
    let loaded = load_safetensors_path_metadata(&weights_path)?;
    let manifest = weight_manifest_from_metadata(
        &topology,
        QWEN3_DENSE_0_6B_PROFILE,
        loaded.source.clone(),
        &loaded.tensors,
    )?;
    let materialized_payload_bytes = if materialize_check {
        manifest
            .slices
            .iter()
            .map(|slice| {
                materialize_weight_slice_payload(slice, &loaded.tensors).map(|bytes| bytes.len())
            })
            .sum::<Result<usize, String>>()?
    } else {
        0
    };
    let service_plan = weight_service_load_plan(manifest)?;
    let tp_plan = tensor_parallel_plan(&topology, QWEN3_DENSE_0_6B_PROFILE)?;
    let summary = serde_json::json!({
        "model": "Qwen/Qwen3-0.6B",
        "source": loaded.source,
        "tensors": loaded.tensors.len(),
        "tp_shards": tp_plan.len(),
        "manifest_slices": service_plan.manifest.slices.len(),
        "metadata_db_puts": service_plan.metadata_db_puts.len(),
        "payload_writes": service_plan.payload_writes.len(),
        "materialized_payload_bytes": materialized_payload_bytes,
    });
    println!("{}", serde_json::to_string_pretty(&summary)?);
    Ok(())
}
