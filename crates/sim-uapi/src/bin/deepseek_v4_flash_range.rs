use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;
use std::time::Instant;

use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash::DEEPSEEK_V4_FLASH_PROFILE;
use sim_models::deepseek_v4_flash_gguf::GgufCatalog;
use sim_topology::SimTopology;
use sim_uapi::{
    execute_deepseek_gguf_range_with_progress_through_simpler, DeepseekV4FlashGgufRangeProgress,
    DeepseekV4FlashModelState,
};

#[derive(Debug, PartialEq, Eq)]
struct Args {
    scenario: PathBuf,
    model: PathBuf,
    layer_start: u64,
    layer_end: u64,
    token: usize,
    input: Option<PathBuf>,
    output: PathBuf,
    logits_output: Option<PathBuf>,
    artifact_dir: PathBuf,
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
            "deepseek_range_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_range_topology_failed:{err}"))?;
    let catalog = GgufCatalog::open(&args.model)?;
    catalog.validate_deepseek_v4_flash()?;
    let input = args.input.as_ref().map(read_f32).transpose()?;
    let mut state = DeepseekV4FlashModelState::new()?;
    let started = Instant::now();
    let mut previous = started;
    let execution = execute_deepseek_gguf_range_with_progress_through_simpler(
        &topology,
        &TaskKey {
            logical_system: LogicalSystemId(1),
            coord: HierarchyCoord { levels: [0; 8] },
            scope_depth: 0,
            task_id: 1,
        },
        &args.artifact_dir,
        2_000_000,
        &catalog,
        &mut state,
        args.layer_start,
        args.layer_end,
        args.token,
        0,
        input.as_deref(),
        args.logits_output.is_some(),
        |progress| {
            let now = Instant::now();
            eprintln!(
                "{}",
                progress_json(
                    progress,
                    now.duration_since(previous).as_millis(),
                    now.duration_since(started).as_millis(),
                )
            );
            previous = now;
        },
    )?;
    write_f32(&args.output, &execution.hidden_hc)?;
    let top_token = if let (Some(path), Some(logits)) =
        (args.logits_output.as_ref(), execution.logits.as_ref())
    {
        write_f32(path, logits)?;
        logits
            .iter()
            .copied()
            .enumerate()
            .max_by(|left, right| left.1.total_cmp(&right.1))
            .map(|(token, logit)| serde_json::json!({"id": token, "logit": logit}))
    } else {
        None
    };
    println!(
        "{}",
        serde_json::json!({
            "status": "ok",
            "operation": "deepseek-layer-range",
            "backend": "simpler-c-api",
            "layers": [args.layer_start, args.layer_end],
            "token": args.token,
            "loaded_routed_expert_bytes": execution.loaded_routed_expert_bytes,
            "layer_routes": execution.layer_routes,
            "hidden_values": execution.hidden_hc.len(),
            "output": args.output,
            "logits_output": args.logits_output,
            "top_token": top_token,
        })
    );
    Ok(())
}

fn progress_json(
    progress: &DeepseekV4FlashGgufRangeProgress,
    layer_elapsed_ms: u128,
    total_elapsed_ms: u128,
) -> serde_json::Value {
    serde_json::json!({
        "status": "progress",
        "operation": "deepseek-layer-range",
        "layer": progress.layer_id,
        "compression_ratio": progress.compression_ratio,
        "routed_experts": progress.routed_experts,
        "loaded_routed_expert_bytes": progress.loaded_routed_expert_bytes,
        "layer_elapsed_ms": layer_elapsed_ms,
        "total_elapsed_ms": total_elapsed_ms,
    })
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
        "--layers",
        "--token",
        "--input",
        "--output",
        "--logits-output",
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
    let layers = required("--layers")?;
    let (start, end) = layers
        .split_once(':')
        .ok_or_else(|| format!("invalid_layers:{layers}"))?;
    let layer_start = start
        .parse::<u64>()
        .map_err(|_| format!("invalid_layer_start:{start}"))?;
    let layer_end = end
        .parse::<u64>()
        .map_err(|_| format!("invalid_layer_end:{end}"))?;
    if layer_start >= layer_end || layer_end > DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!("invalid_layers:{layer_start}:{layer_end}"));
    }
    let token_text = required("--token")?;
    let token = token_text
        .parse::<usize>()
        .map_err(|_| format!("invalid_token:{token_text}"))?;
    if token >= DEEPSEEK_V4_FLASH_PROFILE.vocab_size as usize {
        return Err(format!("invalid_token:{token}"));
    }
    let input = options.get("--input").map(PathBuf::from);
    if layer_start == 0 && input.is_some() {
        return Err("deepseek_first_range_rejects_input".to_string());
    }
    if layer_start != 0 && input.is_none() {
        return Err(format!("deepseek_range_input_required:start={layer_start}"));
    }
    let logits_output = options.get("--logits-output").map(PathBuf::from);
    if logits_output.is_some() && layer_end != DEEPSEEK_V4_FLASH_PROFILE.num_hidden_layers {
        return Err(format!(
            "deepseek_logits_require_terminal_range:end={layer_end}"
        ));
    }
    Ok(Args {
        scenario: options
            .get("--scenario")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("scenarios/mvp_2host_single_domain.yaml")),
        model: PathBuf::from(required("--model")?),
        layer_start,
        layer_end,
        token,
        input,
        output: PathBuf::from(required("--output")?),
        logits_output,
        artifact_dir: options
            .get("--artifact-dir")
            .map(PathBuf::from)
            .unwrap_or_else(|| std::env::temp_dir().join("simpler-deepseek-range-artifacts")),
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
    let bytes = values
        .iter()
        .flat_map(|value| value.to_le_bytes())
        .collect::<Vec<_>>();
    fs::write(path, bytes).map_err(|err| format!("write_failed:{}:{err}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn progress_reports_layer_timing_and_route() {
        let value = progress_json(
            &DeepseekV4FlashGgufRangeProgress {
                layer_id: 4,
                compression_ratio: 4,
                routed_experts: vec![170, 64, 182],
                loaded_routed_expert_bytes: 42_467_328,
            },
            125,
            500,
        );
        assert_eq!(value["status"], "progress");
        assert_eq!(value["layer"], 4);
        assert_eq!(value["compression_ratio"], 4);
        assert_eq!(value["routed_experts"], serde_json::json!([170, 64, 182]));
        assert_eq!(value["layer_elapsed_ms"], 125);
        assert_eq!(value["total_elapsed_ms"], 500);
    }

    fn base_args(layers: &str) -> Vec<&str> {
        vec![
            "--model",
            "model.gguf",
            "--layers",
            layers,
            "--token",
            "108149",
            "--output",
            "hidden.f32",
        ]
    }

    #[test]
    fn args_accept_first_and_terminal_ranges() {
        assert_eq!(parse_args(base_args("0:6")).unwrap().input, None);
        let mut terminal = base_args("38:43");
        terminal.extend(["--input", "hidden-in.f32", "--logits-output", "logits.f32"]);
        let args = parse_args(terminal).expect("parse terminal range");
        assert_eq!(args.layer_start, 38);
        assert_eq!(args.layer_end, 43);
        assert_eq!(args.logits_output, Some(PathBuf::from("logits.f32")));
    }

    #[test]
    fn args_fail_closed_on_missing_handoff_and_nonterminal_logits() {
        assert_eq!(
            parse_args(base_args("4:5")).unwrap_err(),
            "deepseek_range_input_required:start=4"
        );
        let mut args = base_args("0:6");
        args.extend(["--logits-output", "logits.f32"]);
        assert_eq!(
            parse_args(args).unwrap_err(),
            "deepseek_logits_require_terminal_range:end=6"
        );
    }
}
