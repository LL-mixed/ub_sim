use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash::{
    deepseek_v4_flash_layer_compress_ratio, deepseek_v4_flash_rope_coefficients,
    DEEPSEEK_V4_FLASH_PROFILE, DEEPSEEK_V4_FLASH_RMS_EPS,
};
use sim_models::deepseek_v4_flash_gguf::{decode_f16_tensor, decode_f32_tensor, GgufCatalog};
use sim_topology::SimTopology;
use sim_uapi::{
    execute_deepseek_ratio4_attention_through_simpler, DeepseekV4FlashAttentionWeights,
    DeepseekV4FlashCompressorWeights, DeepseekV4FlashIndexerWeights,
    DeepseekV4FlashProjectionFormat, DeepseekV4FlashRatio4AttentionState,
    DeepseekV4FlashRatio4AttentionWeights,
};

#[derive(Debug, PartialEq, Eq)]
struct Args {
    scenario: PathBuf,
    model: PathBuf,
    layer: u64,
    input: PathBuf,
    output: PathBuf,
    artifact_dir: PathBuf,
}

struct TensorPayload {
    payload: Vec<u8>,
    dimensions: Vec<u64>,
    tensor_type: String,
}

fn main() {
    if let Err(err) = run(std::env::args().skip(1)) {
        eprintln!("{err}");
        std::process::exit(1);
    }
}

fn run<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_ratio4_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_ratio4_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let name = |suffix: &str| format!("blk.{}.{}.weight", args.layer, suffix);
    let tensor = |suffix: &str| -> Result<TensorPayload, String> {
        let name = name(suffix);
        let metadata = catalog.tensor(&name)?;
        Ok(TensorPayload {
            payload: catalog.read_tensor(&name)?,
            dimensions: metadata.dimensions.clone(),
            tensor_type: metadata.tensor_type.name.to_string(),
        })
    };
    let f32_tensor = |suffix: &str| -> Result<Vec<f32>, String> {
        let tensor = tensor(suffix)?;
        if tensor.tensor_type != "f32" {
            return Err(format!(
                "deepseek_ratio4_f32_tensor_required:{suffix}:{}",
                tensor.tensor_type
            ));
        }
        decode_f32_tensor(&tensor.payload, &tensor.dimensions)
    };
    let f16_values = |suffix: &str| -> Result<Vec<f32>, String> {
        let tensor = tensor(suffix)?;
        if tensor.tensor_type != "f16" {
            return Err(format!(
                "deepseek_ratio4_f16_tensor_required:{suffix}:{}",
                tensor.tensor_type
            ));
        }
        decode_f16_tensor(&tensor.payload, &tensor.dimensions)
    };

    let hc_function = tensor("hc_attn_fn")?;
    let q_a = tensor("attn_q_a")?;
    let q_b = tensor("attn_q_b")?;
    let kv = tensor("attn_kv")?;
    let output_a = tensor("attn_output_a")?;
    let output_b = tensor("attn_output_b")?;
    let attention_compressor_kv = tensor("attn_compressor_kv")?;
    let attention_compressor_gate = tensor("attn_compressor_gate")?;
    let indexer_compressor_kv = tensor("indexer_compressor_kv")?;
    let indexer_compressor_gate = tensor("indexer_compressor_gate")?;
    let indexer_query = tensor("indexer.attn_q_b")?;
    let indexer_head_weights = tensor("indexer.proj")?;
    for (label, value, expected) in [
        ("hc", &hc_function, "f16"),
        ("q-a", &q_a, "q8_0"),
        ("q-b", &q_b, "q8_0"),
        ("kv", &kv, "q8_0"),
        ("output-a", &output_a, "q8_0"),
        ("output-b", &output_b, "q8_0"),
        ("attention-compressor-kv", &attention_compressor_kv, "f16"),
        (
            "attention-compressor-gate",
            &attention_compressor_gate,
            "f16",
        ),
        ("indexer-compressor-kv", &indexer_compressor_kv, "f16"),
        ("indexer-compressor-gate", &indexer_compressor_gate, "f16"),
        ("indexer-head-weights", &indexer_head_weights, "f16"),
    ] {
        if value.tensor_type != expected {
            return Err(format!(
                "deepseek_ratio4_tensor_type_invalid:{label}:actual={}:expected={expected}",
                value.tensor_type
            ));
        }
    }
    let indexer_query_format = match indexer_query.tensor_type.as_str() {
        "f16" => DeepseekV4FlashProjectionFormat::F16,
        "q8_0" => DeepseekV4FlashProjectionFormat::Q8_0,
        other => {
            return Err(format!(
                "deepseek_ratio4_indexer_query_type_invalid:{other}"
            ))
        }
    };

    let hidden_size = DEEPSEEK_V4_FLASH_PROFILE.hidden_size as usize;
    let hc_mult = DEEPSEEK_V4_FLASH_PROFILE.hc_mult as usize;
    let head_dim = DEEPSEEK_V4_FLASH_PROFILE.head_dim as usize;
    let residuals = read_f32(&args.input)?;
    let row_values = hidden_size * hc_mult;
    if residuals.len() != row_values * 4 {
        return Err(format!(
            "deepseek_ratio4_input_shape_invalid:actual={}:expected={}",
            residuals.len(),
            row_values * 4
        ));
    }
    let weights = DeepseekV4FlashRatio4AttentionWeights {
        attention: DeepseekV4FlashAttentionWeights {
            hc_function: &hc_function.payload,
            hc_function_dimensions: &hc_function.dimensions,
            hc_scale: &f32_tensor("hc_attn_scale")?,
            hc_base: &f32_tensor("hc_attn_base")?,
            attention_norm: &f32_tensor("attn_norm")?,
            q_a: &q_a.payload,
            q_a_dimensions: &q_a.dimensions,
            q_a_norm: &f32_tensor("attn_q_a_norm")?,
            q_b: &q_b.payload,
            q_b_dimensions: &q_b.dimensions,
            kv: &kv.payload,
            kv_dimensions: &kv.dimensions,
            kv_norm: &f32_tensor("attn_kv_a_norm")?,
            sinks: &f32_tensor("attn_sinks")?,
            output_a: &output_a.payload,
            output_a_dimensions: &output_a.dimensions,
            output_b: &output_b.payload,
            output_b_dimensions: &output_b.dimensions,
        },
        attention_compressor: DeepseekV4FlashCompressorWeights {
            kv: &attention_compressor_kv.payload,
            kv_dimensions: &attention_compressor_kv.dimensions,
            gate: &attention_compressor_gate.payload,
            gate_dimensions: &attention_compressor_gate.dimensions,
            ape: &f16_values("attn_compressor_ape")?,
            norm: &f32_tensor("attn_compressor_norm")?,
        },
        indexer_compressor: DeepseekV4FlashCompressorWeights {
            kv: &indexer_compressor_kv.payload,
            kv_dimensions: &indexer_compressor_kv.dimensions,
            gate: &indexer_compressor_gate.payload,
            gate_dimensions: &indexer_compressor_gate.dimensions,
            ape: &f16_values("indexer_compressor_ape")?,
            norm: &f32_tensor("indexer_compressor_norm")?,
        },
        indexer: DeepseekV4FlashIndexerWeights {
            query: &indexer_query.payload,
            query_dimensions: &indexer_query.dimensions,
            query_format: indexer_query_format,
            head_weights: &indexer_head_weights.payload,
            head_weight_dimensions: &indexer_head_weights.dimensions,
        },
    };
    let mut state = DeepseekV4FlashRatio4AttentionState::new(
        head_dim,
        DEEPSEEK_V4_FLASH_PROFILE.indexer_head_dim as usize,
        DEEPSEEK_V4_FLASH_PROFILE.sliding_window as usize,
        DEEPSEEK_V4_FLASH_PROFILE.indexer_top_k as usize,
    )?;
    let mut outputs = Vec::with_capacity(residuals.len());
    let mut selected = Vec::new();
    for position in 0..4usize {
        let rope = deepseek_v4_flash_rope_coefficients(args.layer, position as u32)?;
        let compressed_position = (position + 1).saturating_sub(4) as u32;
        let compressed_rope = deepseek_v4_flash_rope_coefficients(args.layer, compressed_position)?;
        let execution = execute_deepseek_ratio4_attention_through_simpler(
            &topology,
            &TaskKey {
                logical_system: LogicalSystemId(1),
                coord: HierarchyCoord { levels: [0; 8] },
                scope_depth: 0,
                task_id: 1 + (position as u64) * 100,
            },
            &args.artifact_dir,
            900_000 + (position as u64) * 10_000,
            &mut state,
            &weights,
            &residuals[position * row_values..(position + 1) * row_values],
            position as u32,
            &rope.cos,
            &rope.sin,
            &compressed_rope.cos,
            &compressed_rope.sin,
            hc_mult,
            DEEPSEEK_V4_FLASH_PROFILE.hc_sinkhorn_iters as usize,
            DEEPSEEK_V4_FLASH_PROFILE.num_attention_heads as usize,
            head_dim,
            DEEPSEEK_V4_FLASH_PROFILE.qk_rope_head_dim as usize,
            DEEPSEEK_V4_FLASH_PROFILE.output_groups as usize,
            DEEPSEEK_V4_FLASH_PROFILE.indexer_heads as usize,
            DEEPSEEK_V4_FLASH_PROFILE.indexer_head_dim as usize,
            DEEPSEEK_V4_FLASH_PROFILE.indexer_top_k as usize,
            DEEPSEEK_V4_FLASH_RMS_EPS,
        )?;
        outputs.extend_from_slice(&execution.output_hc);
        selected = execution.selected_compressed_rows;
    }
    write_f32(&args.output, &outputs)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "layer": args.layer,
            "tokens": 4,
            "raw_rows": state.raw_rows(),
            "compressed_rows": state.compressed_rows(),
            "selected_compressed_rows": selected,
            "output": args.output,
            "backend": "simpler-c-api",
        })
    );
    Ok(())
}

fn parse_args<I, S>(args: I) -> Result<Args, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    let mut options = BTreeMap::new();
    let mut index = 0;
    while index < args.len() {
        let option = &args[index];
        if !option.starts_with("--") || index + 1 >= args.len() {
            return Err(format!("invalid_option:{option}"));
        }
        if options
            .insert(option.clone(), args[index + 1].clone())
            .is_some()
        {
            return Err(format!("duplicate_option:{option}"));
        }
        index += 2;
    }
    let allowed = [
        "--scenario",
        "--model",
        "--layer",
        "--input",
        "--output",
        "--artifact-dir",
    ];
    if let Some(option) = options
        .keys()
        .find(|name| !allowed.contains(&name.as_str()))
    {
        return Err(format!("unknown_option:{option}"));
    }
    let required = |name: &str| {
        options
            .get(name)
            .cloned()
            .ok_or_else(|| format!("required_option_missing:{name}"))
    };
    let layer_text = required("--layer")?;
    let layer = layer_text
        .parse::<u64>()
        .map_err(|_| format!("invalid_layer:{layer_text}"))?;
    if deepseek_v4_flash_layer_compress_ratio(layer) != Some(4) {
        return Err(format!(
            "ratio4_attention_requires_ratio4_layer:layer={layer}"
        ));
    }
    Ok(Args {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        layer,
        input: PathBuf::from(required("--input")?),
        output: PathBuf::from(required("--output")?),
        artifact_dir: options
            .get("--artifact-dir")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                std::env::temp_dir().join("simpler-deepseek-ratio4-attention-artifacts")
            }),
    })
}

fn read_f32(path: &PathBuf) -> Result<Vec<f32>, String> {
    let bytes = fs::read(path).map_err(|err| format!("read_failed:{}:{err}", path.display()))?;
    if bytes.len() % 4 != 0 {
        return Err(format!("unaligned_f32_file:{}", path.display()));
    }
    Ok(bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes(chunk.try_into().expect("four bytes")))
        .collect())
}

fn write_f32(path: &PathBuf, values: &[f32]) -> Result<(), String> {
    let bytes: Vec<u8> = values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect();
    fs::write(path, bytes).map_err(|err| format!("write_failed:{}:{err}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn required_args(layer: &str) -> Vec<&str> {
        vec![
            "--model",
            "model.gguf",
            "--layer",
            layer,
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ]
    }

    #[test]
    fn args_accept_ratio4_layer() {
        let args = parse_args(required_args("2")).expect("parse ratio-4 args");
        assert_eq!(args.layer, 2);
    }

    #[test]
    fn args_reject_other_layer_kinds() {
        assert_eq!(
            parse_args(required_args("3")).unwrap_err(),
            "ratio4_attention_requires_ratio4_layer:layer=3"
        );
    }
}
