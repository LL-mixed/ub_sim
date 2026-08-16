use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;
use std::time::Instant;

use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash::DEEPSEEK_V4_FLASH_PROFILE;
use sim_models::deepseek_v4_flash_checkpoint::{DeepseekV4CacheLimits, DeepseekV4Checkpoint};
use sim_models::deepseek_v4_flash_gguf::GgufCatalog;
use sim_topology::SimTopology;
use sim_uapi::{
    execute_deepseek_gguf_range_with_progress_through_simpler,
    execute_deepseek_gguf_sequence_range_through_simpler,
    execute_deepseek_official_range_with_progress_through_simpler,
    execute_deepseek_official_sequence_range_through_simpler, set_simpler_dispatch_log_enabled,
    DeepseekV4FlashGgufRangeProgress, DeepseekV4FlashModelState, DeepseekV4OfficialRangeProgress,
};

#[derive(Debug, PartialEq, Eq)]
struct Args {
    scenario: PathBuf,
    model: PathBuf,
    layer_start: u64,
    layer_end: u64,
    tokens: Vec<usize>,
    position: u32,
    input: Option<PathBuf>,
    state_input: Option<PathBuf>,
    state_output: Option<PathBuf>,
    output: PathBuf,
    logits_output: Option<PathBuf>,
    artifact_dir: PathBuf,
    dispatch_log: bool,
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
    set_simpler_dispatch_log_enabled(args.dispatch_log);
    let config = ScenarioConfig::from_yaml_file(&args.scenario).map_err(|err| {
        format!(
            "deepseek_range_scenario_load_failed:{}:{err}",
            args.scenario.display()
        )
    })?;
    let topology = SimTopology::from_config(&config)
        .map_err(|err| format!("deepseek_range_topology_failed:{err}"))?;
    let input = args.input.as_ref().map(read_f32).transpose()?;
    let mut state = DeepseekV4FlashModelState::new()?;
    if let Some(path) = &args.state_input {
        let payload =
            fs::read(path).map_err(|err| format!("state_read_failed:{}:{err}", path.display()))?;
        state.restore_range_state(args.layer_start, args.layer_end, &payload)?;
    }
    let started = Instant::now();
    let mut previous = started;
    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let official = is_official_model_path(&args.model);
    let (hidden_hc, logits, loaded_routed_expert_bytes, layer_routes, model_format) = if official {
        let checkpoint = DeepseekV4Checkpoint::open(&args.model, DeepseekV4CacheLimits::default())?;
        if args.tokens.len() == 1 {
            let execution = execute_deepseek_official_range_with_progress_through_simpler(
                &checkpoint,
                &topology,
                &task,
                &args.artifact_dir,
                2_000_000,
                &mut state,
                args.layer_start,
                args.layer_end,
                args.tokens[0],
                args.position,
                input.as_deref(),
                args.logits_output.is_some(),
                |progress| {
                    let now = Instant::now();
                    eprintln!(
                        "{}",
                        official_progress_json(
                            progress,
                            now.duration_since(previous).as_millis(),
                            now.duration_since(started).as_millis(),
                        )
                    );
                    previous = now;
                },
            )?;
            (
                execution.hidden_hc,
                execution.logits,
                execution.routed_expert_bytes,
                serde_json::json!(execution.layer_routes),
                "official-safetensors",
            )
        } else {
            let execution = execute_deepseek_official_sequence_range_through_simpler(
                &checkpoint,
                &topology,
                &task,
                &args.artifact_dir,
                2_000_000,
                &mut state,
                args.layer_start,
                args.layer_end,
                &args.tokens,
                args.position,
                input.as_deref(),
                args.logits_output.is_some(),
            )?;
            (
                execution.hidden_hc,
                execution.logits,
                execution.routed_expert_bytes,
                serde_json::json!(execution.token_layer_routes),
                "official-safetensors",
            )
        }
    } else {
        let catalog = GgufCatalog::open(&args.model)?;
        catalog.validate_deepseek_v4_flash()?;
        if args.tokens.len() == 1 {
            let execution = execute_deepseek_gguf_range_with_progress_through_simpler(
                &topology,
                &task,
                &args.artifact_dir,
                2_000_000,
                &catalog,
                &mut state,
                args.layer_start,
                args.layer_end,
                args.tokens[0],
                args.position,
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
            (
                execution.hidden_hc,
                execution.logits,
                execution.loaded_routed_expert_bytes as u64,
                serde_json::json!(execution.layer_routes),
                "gguf",
            )
        } else {
            let execution = execute_deepseek_gguf_sequence_range_through_simpler(
                &topology,
                &task,
                &args.artifact_dir,
                2_000_000,
                &catalog,
                &mut state,
                args.layer_start,
                args.layer_end,
                &args.tokens,
                args.position,
                input.as_deref(),
                args.logits_output.is_some(),
            )?;
            (
                execution.hidden_hc,
                execution.logits,
                execution.loaded_routed_expert_bytes as u64,
                serde_json::json!(execution.token_layer_routes),
                "gguf",
            )
        }
    };
    write_f32(&args.output, &hidden_hc)?;
    if let Some(path) = &args.state_output {
        let payload = state.encode_range_state(args.layer_start, args.layer_end)?;
        fs::write(path, payload)
            .map_err(|err| format!("state_write_failed:{}:{err}", path.display()))?;
    }
    let top_token =
        if let (Some(path), Some(logits)) = (args.logits_output.as_ref(), logits.as_ref()) {
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
            "model_format": model_format,
            "layers": [args.layer_start, args.layer_end],
            "tokens": args.tokens,
            "position": args.position,
            "loaded_routed_expert_bytes": loaded_routed_expert_bytes,
            "layer_routes": layer_routes,
            "hidden_values": hidden_hc.len(),
            "output": args.output,
            "state_output": args.state_output,
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

fn official_progress_json(
    progress: &DeepseekV4OfficialRangeProgress,
    layer_elapsed_ms: u128,
    total_elapsed_ms: u128,
) -> serde_json::Value {
    serde_json::json!({
        "status": "progress",
        "operation": "deepseek-layer-range",
        "model_format": "official-safetensors",
        "layer": progress.layer_id,
        "compression_ratio": progress.compression_ratio,
        "routed_experts": progress.routed_experts,
        "routed_expert_bytes": progress.routed_expert_bytes,
        "raw_rows": progress.raw_rows,
        "compressed_rows": progress.compressed_rows,
        "layer_elapsed_ms": layer_elapsed_ms,
        "total_elapsed_ms": total_elapsed_ms,
    })
}

fn is_official_model_path(path: &std::path::Path) -> bool {
    if path.is_dir() {
        return true;
    }
    path.file_name()
        .and_then(|name| name.to_str())
        .is_some_and(|name| {
            name.ends_with(".safetensors") || name.ends_with(".safetensors.index.json")
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
        "--tokens",
        "--position",
        "--input",
        "--state-input",
        "--state-output",
        "--output",
        "--logits-output",
        "--artifact-dir",
        "--dispatch-log",
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
    let tokens = match (options.get("--token"), options.get("--tokens")) {
        (Some(token), None) => vec![parse_token(token)?],
        (None, Some(tokens)) => {
            if tokens.is_empty() {
                return Err("invalid_tokens:empty".to_string());
            }
            tokens
                .split(',')
                .map(parse_token)
                .collect::<Result<Vec<_>, _>>()?
        }
        (Some(_), Some(_)) => return Err("token_source_conflict".to_string()),
        (None, None) => return Err("required_token_source_missing".to_string()),
    };
    if tokens.is_empty() {
        return Err("invalid_tokens:empty".to_string());
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
        tokens,
        position: options
            .get("--position")
            .map(|value| {
                value
                    .parse::<u32>()
                    .map_err(|_| format!("invalid_position:{value}"))
            })
            .transpose()?
            .unwrap_or(0),
        input,
        state_input: options.get("--state-input").map(PathBuf::from),
        state_output: options.get("--state-output").map(PathBuf::from),
        output: PathBuf::from(required("--output")?),
        logits_output,
        artifact_dir: options
            .get("--artifact-dir")
            .map(PathBuf::from)
            .unwrap_or_else(|| std::env::temp_dir().join("simpler-deepseek-range-artifacts")),
        dispatch_log: options
            .get("--dispatch-log")
            .map(|value| parse_bool("dispatch_log", value))
            .transpose()?
            .unwrap_or(false),
    })
}

fn parse_token(value: &str) -> Result<usize, String> {
    let token = value
        .parse::<usize>()
        .map_err(|_| format!("invalid_token:{value}"))?;
    if token >= DEEPSEEK_V4_FLASH_PROFILE.vocab_size as usize {
        return Err(format!("invalid_token:{token}"));
    }
    Ok(token)
}

fn parse_bool(name: &str, value: &str) -> Result<bool, String> {
    match value {
        "true" => Ok(true),
        "false" => Ok(false),
        _ => Err(format!("invalid_{name}:{value}")),
    }
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
        let first = parse_args(base_args("0:6")).unwrap();
        assert_eq!(first.input, None);
        assert_eq!(first.tokens, vec![108_149]);
        assert_eq!(first.position, 0);
        assert!(!first.dispatch_log);
        let mut terminal = base_args("38:43");
        terminal.extend([
            "--input",
            "hidden-in.f32",
            "--logits-output",
            "logits.f32",
            "--dispatch-log",
            "true",
        ]);
        let args = parse_args(terminal).expect("parse terminal range");
        assert_eq!(args.layer_start, 38);
        assert_eq!(args.layer_end, 43);
        assert_eq!(args.logits_output, Some(PathBuf::from("logits.f32")));
        assert!(args.dispatch_log);
    }

    #[test]
    fn args_accept_multi_token_state_round_trip_options() {
        let args = parse_args([
            "--model",
            "model.gguf",
            "--layers",
            "6:12",
            "--tokens",
            "1,2,3",
            "--position",
            "7",
            "--input",
            "hidden-in.f32",
            "--state-input",
            "state-in.bin",
            "--state-output",
            "state-out.bin",
            "--output",
            "hidden-out.f32",
        ])
        .expect("parse multi-token range");
        assert_eq!(args.tokens, vec![1, 2, 3]);
        assert_eq!(args.position, 7);
        assert_eq!(args.state_input, Some(PathBuf::from("state-in.bin")));
        assert_eq!(args.state_output, Some(PathBuf::from("state-out.bin")));
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
        let mut args = base_args("0:6");
        args.extend(["--dispatch-log", "yes"]);
        assert_eq!(parse_args(args).unwrap_err(), "invalid_dispatch_log:yes");
        let mut args = base_args("0:6");
        args.extend(["--tokens", "1,2"]);
        assert_eq!(parse_args(args).unwrap_err(), "token_source_conflict");
    }

    #[test]
    fn model_format_detection_distinguishes_official_and_gguf_sources() {
        assert!(is_official_model_path(std::path::Path::new(
            "checkpoint/model.safetensors.index.json"
        )));
        assert!(is_official_model_path(std::path::Path::new(
            "checkpoint/model-00001-of-00061.safetensors"
        )));
        assert!(!is_official_model_path(std::path::Path::new("model.gguf")));
    }
}
