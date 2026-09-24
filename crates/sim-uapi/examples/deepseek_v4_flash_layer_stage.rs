use std::ffi::OsString;
use std::fs;
use std::path::PathBuf;

use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash_gguf::GgufCatalog;
use sim_topology::SimTopology;
use sim_uapi::{
    deepseek_v4_flash_embedding_hc, execute_deepseek_gguf_layer_through_simpler,
    DeepseekV4FlashModelState,
};

struct Args {
    scenario: PathBuf,
    model: PathBuf,
    token: usize,
    artifact_dir: PathBuf,
    attention_output: PathBuf,
    final_output: PathBuf,
}

fn parse_args(args: impl IntoIterator<Item = OsString>) -> Result<Args, String> {
    let args = args.into_iter().collect::<Vec<_>>();
    if args.len() != 6 {
        return Err(
            "usage: deepseek_v4_flash_layer_stage SCENARIO MODEL_GGUF TOKEN ".to_string()
                + "ARTIFACT_DIR ATTENTION_OUT FINAL_OUT",
        );
    }
    let token = args[2]
        .to_str()
        .ok_or("token is not valid UTF-8")?
        .parse::<usize>()
        .map_err(|_| "invalid token".to_string())?;
    Ok(Args {
        scenario: PathBuf::from(&args[0]),
        model: PathBuf::from(&args[1]),
        token,
        artifact_dir: PathBuf::from(&args[3]),
        attention_output: PathBuf::from(&args[4]),
        final_output: PathBuf::from(&args[5]),
    })
}

fn write_f32(path: &PathBuf, values: &[f32]) -> Result<(), String> {
    let payload = values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect::<Vec<_>>();
    fs::write(path, payload).map_err(|error| format!("write_failed:{}:{error}", path.display()))
}

fn run(args: Args) -> Result<(), String> {
    let config = ScenarioConfig::from_yaml_file(&args.scenario)
        .map_err(|error| format!("scenario_load_failed:{error}"))?;
    let topology =
        SimTopology::from_config(&config).map_err(|error| format!("topology_failed:{error}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let embedded = deepseek_v4_flash_embedding_hc(&catalog, args.token)?;
    let mut state = DeepseekV4FlashModelState::new()?;
    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let execution = execute_deepseek_gguf_layer_through_simpler(
        &topology,
        &task,
        &args.artifact_dir,
        2_000_000,
        &catalog,
        state.layer_mut(0)?,
        0,
        args.token,
        0,
        &embedded,
    )?;
    write_f32(&args.attention_output, &execution.attention_output_hc)?;
    let mut raw_attention_path = args.attention_output.as_os_str().to_os_string();
    raw_attention_path.push(".raw.f32");
    write_f32(
        &PathBuf::from(raw_attention_path),
        &execution.attention_output,
    )?;
    write_f32(&args.final_output, &execution.ffn.output_hc)?;
    println!(
        "layer_stage=pass layer=0 token={} attention_values={} final_values={} routes={:?}",
        args.token,
        execution.attention_output_hc.len(),
        execution.ffn.output_hc.len(),
        execution.ffn.router.expert_indices
    );
    Ok(())
}

fn main() {
    let result = parse_args(std::env::args_os().skip(1)).and_then(run);
    if let Err(error) = result {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn requires_exactly_six_arguments() {
        assert!(parse_args([]).is_err());
        assert!(parse_args([OsString::from("scenario")]).is_err());
        let args = parse_args(
            [
                "scenario",
                "model.gguf",
                "42",
                "artifacts",
                "attention.f32",
                "final.f32",
            ]
            .map(OsString::from),
        )
        .unwrap();
        assert_eq!(args.token, 42);
        assert_eq!(args.model, PathBuf::from("model.gguf"));
    }
}
