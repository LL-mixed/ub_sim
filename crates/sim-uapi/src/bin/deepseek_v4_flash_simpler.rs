use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash::{DEEPSEEK_V4_FLASH_PROFILE, DEEPSEEK_V4_FLASH_SWIGLU_CLAMP};
use sim_models::deepseek_v4_flash_gguf::GgufCatalog;
use sim_topology::SimTopology;
use sim_uapi::{
    execute_deepseek_grouped_q8_projection_through_simpler,
    execute_deepseek_q8_projection_through_simpler, execute_deepseek_shared_expert_through_simpler,
};
use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

#[derive(Debug, PartialEq, Eq)]
struct ProjectQ8Args {
    scenario: PathBuf,
    model: PathBuf,
    tensor: String,
    input: PathBuf,
    output: PathBuf,
    manifest: PathBuf,
    groups: usize,
}

#[derive(Debug, PartialEq, Eq)]
struct SharedExpertArgs {
    scenario: PathBuf,
    model: PathBuf,
    layer: u64,
    input: PathBuf,
    output: PathBuf,
    artifact_dir: PathBuf,
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = if args.first().map(String::as_str) == Some("shared-expert") {
        run_shared_expert(args)
    } else {
        run(args)
    };
    if let Err(err) = result {
        eprintln!("{err}");
        std::process::exit(1);
    }
}

fn run_shared_expert<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_shared_expert_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_simpler_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_simpler_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let tensor_name = |kind: &str| format!("blk.{}.ffn_{}.weight", args.layer, kind);
    let gate_name = tensor_name("gate_shexp");
    let up_name = tensor_name("up_shexp");
    let down_name = tensor_name("down_shexp");
    let gate = catalog.tensor(&gate_name)?;
    let up = catalog.tensor(&up_name)?;
    let down = catalog.tensor(&down_name)?;
    for tensor in [gate, up, down] {
        if tensor.tensor_type.name != "q8_0" {
            return Err(format!(
                "deepseek_shared_expert_q8_tensor_required:{}:{}",
                tensor.name, tensor.tensor_type.name
            ));
        }
    }
    let input = read_f32(&args.input)?;
    let output = execute_deepseek_shared_expert_through_simpler(
        &topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        },
        &args.artifact_dir,
        200_000,
        &catalog.read_tensor(&gate_name)?,
        &gate.dimensions,
        &catalog.read_tensor(&up_name)?,
        &up.dimensions,
        &catalog.read_tensor(&down_name)?,
        &down.dimensions,
        &input,
        DEEPSEEK_V4_FLASH_SWIGLU_CLAMP,
    )?;
    write_f32(&args.output, &output)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "model": args.model,
            "layer": args.layer,
            "input_values": input.len(),
            "output_values": output.len(),
            "output": args.output,
            "backend": "simpler-c-api",
            "operation": "deepseek-shared-expert",
        })
    );
    Ok(())
}

fn run<I, S>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = parse_args(args)?;
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_simpler_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_simpler_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let tensor = catalog.tensor(&args.tensor)?;
    if tensor.tensor_type.name != "q8_0" {
        return Err(format!(
            "deepseek_simpler_q8_tensor_required:{}:{}",
            args.tensor, tensor.tensor_type.name
        ));
    }
    let input = read_f32(&args.input)?;
    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let weight = catalog.read_tensor(&args.tensor)?;
    let output = if args.groups == 1 {
        execute_deepseek_q8_projection_through_simpler(
            &topology,
            &task,
            &args.manifest,
            100_000,
            &weight,
            &tensor.dimensions,
            &input,
        )?
    } else {
        execute_deepseek_grouped_q8_projection_through_simpler(
            &topology,
            &task,
            &args.manifest,
            100_000,
            &weight,
            &tensor.dimensions,
            &input,
            args.groups,
        )?
    };
    write_f32(&args.output, &output)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "model": args.model,
            "tensor": args.tensor,
            "input_values": input.len(),
            "output_values": output.len(),
            "groups": args.groups,
            "output": args.output,
            "backend": "simpler-c-api",
        })
    );
    Ok(())
}

fn parse_shared_expert_args<I, S>(args: I) -> Result<SharedExpertArgs, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    if args.first().map(String::as_str) != Some("shared-expert") {
        return Err(
            "usage: deepseek-v4-flash-simpler shared-expert --model FILE --layer N --input FILE --output FILE [--scenario FILE] [--artifact-dir DIR]"
                .to_string(),
        );
    }
    let mut options = BTreeMap::new();
    let mut index = 1;
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
    if let Some(option) = options.keys().find(|name| {
        !matches!(
            name.as_str(),
            "--scenario" | "--model" | "--layer" | "--input" | "--output" | "--artifact-dir"
        )
    }) {
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
    if layer >= DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!("invalid_layer:{layer}"));
    }
    Ok(SharedExpertArgs {
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
                std::env::temp_dir().join("simpler-deepseek-shared-expert-artifacts")
            }),
    })
}

fn write_f32(path: &PathBuf, values: &[f32]) -> Result<(), String> {
    let bytes: Vec<u8> = values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect();
    fs::write(path, bytes).map_err(|err| {
        format!(
            "deepseek_simpler_output_write_failed:{}:{err}",
            path.display()
        )
    })
}

fn parse_args<I, S>(args: I) -> Result<ProjectQ8Args, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    if args.first().map(String::as_str) != Some("project-q8") {
        return Err(
            "usage: deepseek-v4-flash-simpler project-q8 --model FILE --tensor NAME --input FILE --output FILE [--groups N] [--scenario FILE] [--manifest FILE]"
                .to_string(),
        );
    }
    let mut options = BTreeMap::new();
    let mut index = 1;
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
    let required = |name: &str| {
        options
            .get(name)
            .cloned()
            .ok_or_else(|| format!("required_option_missing:{name}"))
    };
    let unknown = options.keys().find(|name| {
        !matches!(
            name.as_str(),
            "--scenario"
                | "--model"
                | "--tensor"
                | "--input"
                | "--output"
                | "--manifest"
                | "--groups"
        )
    });
    if let Some(option) = unknown {
        return Err(format!("unknown_option:{option}"));
    }
    let groups = options
        .get("--groups")
        .map(|value| {
            value
                .parse::<usize>()
                .map_err(|_| format!("invalid_groups:{value}"))
        })
        .transpose()?
        .unwrap_or(1);
    if groups == 0 {
        return Err("invalid_groups:0".to_string());
    }
    Ok(ProjectQ8Args {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        tensor: required("--tensor")?,
        input: PathBuf::from(required("--input")?),
        output: PathBuf::from(required("--output")?),
        manifest: options
            .get("--manifest")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                std::env::temp_dir()
                    .join("simpler-deepseek-q8-project-artifacts")
                    .join("host_q8_block_dot_manifest.json")
            }),
        groups,
    })
}

fn read_f32(path: &PathBuf) -> Result<Vec<f32>, String> {
    let bytes = fs::read(path).map_err(|err| {
        format!(
            "deepseek_simpler_input_read_failed:{}:{err}",
            path.display()
        )
    })?;
    if bytes.len() % 4 != 0 {
        return Err(format!(
            "deepseek_simpler_input_unaligned:{}:{}",
            path.display(),
            bytes.len()
        ));
    }
    Ok(bytes
        .chunks_exact(4)
        .map(|chunk| f32::from_le_bytes(chunk.try_into().expect("four-byte chunk")))
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn shared_expert_args_accept_real_layer() {
        let args = parse_shared_expert_args([
            "shared-expert",
            "--model",
            "model.gguf",
            "--layer",
            "42",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect("parse shared expert command");
        assert_eq!(args.layer, 42);
        assert!(args
            .artifact_dir
            .ends_with("simpler-deepseek-shared-expert-artifacts"));
    }

    #[test]
    fn shared_expert_args_reject_layer_outside_model() {
        let err = parse_shared_expert_args([
            "shared-expert",
            "--model",
            "model.gguf",
            "--layer",
            "43",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect_err("reject layer outside model");
        assert_eq!(err, "invalid_layer:43");
    }

    #[test]
    fn project_q8_args_have_reusable_defaults() {
        let args = parse_args([
            "project-q8",
            "--model",
            "model.gguf",
            "--tensor",
            "blk.0.attn_q_a.weight",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ])
        .expect("parse project-q8 command");
        assert_eq!(
            args.scenario,
            PathBuf::from("scenarios/mvp_2host_single_domain.yaml")
        );
        assert!(args.manifest.ends_with("host_q8_block_dot_manifest.json"));
        assert_eq!(args.groups, 1);
    }

    #[test]
    fn project_q8_args_accept_grouped_projection() {
        let args = parse_args([
            "project-q8",
            "--model",
            "model.gguf",
            "--tensor",
            "blk.0.attn_output_a.weight",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
            "--groups",
            "8",
        ])
        .expect("parse grouped project-q8 command");
        assert_eq!(args.groups, 8);
    }

    #[test]
    fn project_q8_args_reject_zero_groups() {
        let err = parse_args([
            "project-q8",
            "--model",
            "model.gguf",
            "--tensor",
            "tensor",
            "--input",
            "input",
            "--output",
            "output",
            "--groups",
            "0",
        ])
        .expect_err("reject zero groups");
        assert_eq!(err, "invalid_groups:0");
    }

    #[test]
    fn project_q8_args_reject_unknown_options() {
        let err = parse_args([
            "project-q8",
            "--model",
            "model.gguf",
            "--tensor",
            "tensor",
            "--input",
            "input",
            "--output",
            "output",
            "--bogus",
            "value",
        ])
        .expect_err("reject unknown option");
        assert_eq!(err, "unknown_option:--bogus");
    }
}
