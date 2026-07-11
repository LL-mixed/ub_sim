use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;

use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash::DEEPSEEK_V4_FLASH_PROFILE;
use sim_models::deepseek_v4_flash_gguf::GgufCatalog;
use sim_topology::SimTopology;
use sim_uapi::{execute_deepseek_gguf_layer_through_simpler, DeepseekV4FlashLayerState};

#[derive(Debug, PartialEq, Eq)]
struct Args {
    scenario: PathBuf,
    model: PathBuf,
    layer: u64,
    token: usize,
    position: u32,
    input: PathBuf,
    output: PathBuf,
    artifact_dir: PathBuf,
    dump_prefix: Option<PathBuf>,
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
            "deepseek_layer_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_layer_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let input = read_f32(&args.input)?;
    let expected =
        (DEEPSEEK_V4_FLASH_PROFILE.hidden_size * DEEPSEEK_V4_FLASH_PROFILE.hc_mult) as usize;
    if input.len() != expected {
        return Err(format!(
            "deepseek_layer_input_shape_invalid:actual={}:expected={expected}",
            input.len()
        ));
    }
    let mut state = DeepseekV4FlashLayerState::new(args.layer)?;
    let execution = execute_deepseek_gguf_layer_through_simpler(
        &topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        },
        &args.artifact_dir,
        1_000_000,
        &catalog,
        &mut state,
        args.layer,
        args.token,
        args.position,
        &input,
    )?;
    if let Some(prefix) = args.dump_prefix.as_ref() {
        write_f32(&dump_path(prefix, "current-kv"), &execution.current_kv)?;
        write_f32(
            &dump_path(prefix, "attention-output"),
            &execution.attention_output,
        )?;
        write_f32(
            &dump_path(prefix, "attention-hc"),
            &execution.attention_output_hc,
        )?;
        write_f32(
            &dump_path(prefix, "ffn-norm"),
            &execution.ffn.normalized_hidden,
        )?;
        write_f32(&dump_path(prefix, "moe-output"), &execution.ffn.moe_output)?;
        write_f32(&dump_path(prefix, "final-hc"), &execution.ffn.output_hc)?;
    }
    write_f32(&args.output, &execution.ffn.output_hc)?;
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "operation": "deepseek-complete-layer",
            "backend": "simpler-c-api",
            "layer": args.layer,
            "token": args.token,
            "position": args.position,
            "expert_indices": execution.ffn.router.expert_indices,
            "expert_weights": execution.ffn.router.expert_weights,
            "loaded_routed_expert_bytes": execution.loaded_routed_expert_bytes,
            "output_values": execution.ffn.output_hc.len(),
            "output": args.output,
        })
    );
    Ok(())
}

fn parse_args<I, S>(args: I) -> Result<Args, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args = args.into_iter().map(Into::into).collect::<Vec<String>>();
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
        "--token",
        "--position",
        "--input",
        "--output",
        "--artifact-dir",
        "--dump-prefix",
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
    if layer >= DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!("invalid_layer:{layer}"));
    }
    let token_text = required("--token")?;
    let token = token_text
        .parse::<usize>()
        .map_err(|_| format!("invalid_token:{token_text}"))?;
    if token >= DEEPSEEK_V4_FLASH_PROFILE.vocab_size as usize {
        return Err(format!("invalid_token:{token}"));
    }
    let position_text = options
        .get("--position")
        .cloned()
        .unwrap_or_else(|| "0".to_string());
    let position = position_text
        .parse::<u32>()
        .map_err(|_| format!("invalid_position:{position_text}"))?;
    if position != 0 {
        return Err(format!(
            "deepseek_layer_fresh_state_requires_position_zero:position={position}"
        ));
    }
    Ok(Args {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        layer,
        token,
        position,
        input: PathBuf::from(required("--input")?),
        output: PathBuf::from(required("--output")?),
        artifact_dir: options
            .get("--artifact-dir")
            .map(PathBuf::from)
            .unwrap_or_else(|| std::env::temp_dir().join("simpler-deepseek-layer-artifacts")),
        dump_prefix: options.get("--dump-prefix").map(PathBuf::from),
    })
}

fn dump_path(prefix: &PathBuf, stage: &str) -> PathBuf {
    PathBuf::from(format!("{}-{stage}.f32", prefix.display()))
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
    let bytes = values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect::<Vec<_>>();
    fs::write(path, bytes).map_err(|err| format!("write_failed:{}:{err}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn required_args() -> Vec<&'static str> {
        vec![
            "--model",
            "model.gguf",
            "--layer",
            "4",
            "--token",
            "108149",
            "--input",
            "input.f32",
            "--output",
            "output.f32",
        ]
    }

    #[test]
    fn args_accept_fresh_single_layer_execution() {
        let args = parse_args(required_args()).expect("parse layer args");
        assert_eq!(args.layer, 4);
        assert_eq!(args.token, 108_149);
        assert_eq!(args.position, 0);
        assert_eq!(args.dump_prefix, None);
    }

    #[test]
    fn args_accept_stage_dump_prefix() {
        let mut args = required_args();
        args.extend(["--dump-prefix", "/tmp/layer4"]);
        let args = parse_args(args).expect("parse dump prefix");
        assert_eq!(args.dump_prefix, Some(PathBuf::from("/tmp/layer4")));
        assert_eq!(
            dump_path(args.dump_prefix.as_ref().unwrap(), "ffn-norm"),
            PathBuf::from("/tmp/layer4-ffn-norm.f32")
        );
    }

    #[test]
    fn args_reject_nonzero_position_without_state() {
        let mut args = required_args();
        args.extend(["--position", "1"]);
        assert_eq!(
            parse_args(args).unwrap_err(),
            "deepseek_layer_fresh_state_requires_position_zero:position=1"
        );
    }
}
