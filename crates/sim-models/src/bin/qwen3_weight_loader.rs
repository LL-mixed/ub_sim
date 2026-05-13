use std::env;
use std::path::PathBuf;

use sim_config::ScenarioConfig;
use sim_models::qwen3_dense::{
    model_key as qwen3_dense_model_key, profile_from_weights_dir,
    QWEN3_DENSE_DEFAULT_DECODE_TOKENS, QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
    QWEN3_DENSE_DEFAULT_TP_NODES,
};
use sim_models::qwen3_dense_0_6b::{
    load_safetensors_path_metadata, logits_reference_summary, materialize_weight_slice_payload,
    profile_from_dense_profile, qkv_reference_layer_summary, tensor_parallel_plan,
    weight_manifest_from_metadata_for_model, weight_service_load_plan,
};
use sim_topology::SimTopology;

fn weights_config_dir(weights_path: &PathBuf) -> PathBuf {
    if weights_path.is_dir() {
        weights_path.clone()
    } else {
        weights_path
            .parent()
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("."))
    }
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        eprintln!(
            "usage: qwen3_weight_loader <scenario.yaml> <model.safetensors|model.safetensors.index.json|model_dir> [--materialize-check] [--reference-check] [--logits-reference-check]"
        );
        std::process::exit(2);
    }
    let scenario_path = PathBuf::from(&args[1]);
    let weights_path = PathBuf::from(&args[2]);
    let materialize_check = args.iter().any(|arg| arg == "--materialize-check");
    let reference_check = args.iter().any(|arg| arg == "--reference-check");
    let logits_reference_check = args.iter().any(|arg| arg == "--logits-reference-check");

    let config = ScenarioConfig::from_yaml_file(&scenario_path)?;
    let topology = SimTopology::from_config(&config)?;
    let loaded = load_safetensors_path_metadata(&weights_path)?;
    let dense_profile = profile_from_weights_dir(
        &weights_config_dir(&weights_path),
        None,
        QWEN3_DENSE_DEFAULT_TP_NODES,
        QWEN3_DENSE_DEFAULT_PREFILL_TOKENS,
        QWEN3_DENSE_DEFAULT_DECODE_TOKENS,
    )?;
    let model_key = qwen3_dense_model_key(&dense_profile.model_id);
    let manifest_profile = profile_from_dense_profile(&dense_profile);
    let manifest = weight_manifest_from_metadata_for_model(
        &topology,
        &dense_profile.model_id,
        manifest_profile,
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
    let tp_plan = tensor_parallel_plan(&topology, manifest_profile)?;
    let reference_summary = if reference_check {
        Some(qkv_reference_layer_summary(
            &service_plan.manifest,
            &loaded.tensors,
            0,
        )?)
    } else {
        None
    };
    let logits_reference_summary = if logits_reference_check {
        Some(logits_reference_summary(
            &loaded.tensors,
            &[(0, 0), (1, 1), (2, 151_643), (3, 151_935)],
        )?)
    } else {
        None
    };
    let summary = serde_json::json!({
        "model": dense_profile.model_id,
        "model_key": model_key,
        "source": loaded.source,
        "vocab_size": dense_profile.vocab_size,
        "hidden_size": dense_profile.hidden_size,
        "intermediate_size": dense_profile.intermediate_size,
        "num_hidden_layers": dense_profile.num_hidden_layers,
        "num_attention_heads": dense_profile.num_attention_heads,
        "num_key_value_heads": dense_profile.num_key_value_heads,
        "head_dim": dense_profile.head_dim,
        "tp_nodes": dense_profile.tp_nodes,
        "tensors": loaded.tensors.len(),
        "tp_shards": tp_plan.len(),
        "manifest_slices": service_plan.manifest.slices.len(),
        "metadata_db_puts": service_plan.metadata_db_puts.len(),
        "payload_writes": service_plan.payload_writes.len(),
        "materialized_payload_bytes": materialized_payload_bytes,
        "reference_check": reference_summary,
        "logits_reference_check": logits_reference_summary,
    });
    println!("{}", serde_json::to_string_pretty(&summary)?);
    Ok(())
}
