use std::path::PathBuf;

use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash_checkpoint::{DeepseekV4CacheLimits, DeepseekV4Checkpoint};
use sim_topology::SimTopology;
use sim_uapi::{
    execute_deepseek_official_first_token_with_progress_through_simpler,
    validate_deepseek_official_first_token_alignment,
};

#[derive(Debug, PartialEq, Eq)]
struct Args {
    model: PathBuf,
    tokens: Vec<u64>,
    artifact_dir: PathBuf,
    scenario: PathBuf,
    top_k: usize,
    report: Option<PathBuf>,
}

fn usage() -> &'static str {
    "usage: deepseek_v4_flash_official_first_token --model PATH --tokens CSV [--artifact-dir PATH] [--scenario PATH] [--top-k N] [--report PATH]"
}

fn default_scenario() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../scenarios/mvp_2host_single_domain.yaml")
}

fn parse_tokens(value: &str) -> Result<Vec<u64>, String> {
    if value.is_empty() {
        return Err("--tokens must not be empty".to_string());
    }
    value
        .split(',')
        .map(|token| {
            token
                .parse::<u64>()
                .map_err(|_| format!("invalid token id {token}"))
        })
        .collect()
}

fn parse_args<I>(args: I) -> Result<Args, String>
where
    I: IntoIterator<Item = String>,
{
    let mut model = None;
    let mut tokens = None;
    let mut artifact_dir = PathBuf::from("/tmp/deepseek-v4-flash-official-first-token");
    let mut scenario = default_scenario();
    let mut top_k = 5usize;
    let mut report = None;
    let mut args = args.into_iter();
    while let Some(flag) = args.next() {
        let value = args
            .next()
            .ok_or_else(|| format!("missing value for {flag}; {}", usage()))?;
        match flag.as_str() {
            "--model" => model = Some(PathBuf::from(value)),
            "--tokens" => tokens = Some(parse_tokens(&value)?),
            "--artifact-dir" => artifact_dir = PathBuf::from(value),
            "--scenario" => scenario = PathBuf::from(value),
            "--top-k" => {
                top_k = value
                    .parse()
                    .map_err(|_| format!("invalid --top-k {value}"))?
            }
            "--report" => report = Some(PathBuf::from(value)),
            _ => return Err(format!("unknown option {flag}; {}", usage())),
        }
    }
    if top_k == 0 {
        return Err("--top-k must be positive".to_string());
    }
    Ok(Args {
        model: model.ok_or_else(|| format!("--model is required; {}", usage()))?,
        tokens: tokens.ok_or_else(|| format!("--tokens is required; {}", usage()))?,
        artifact_dir,
        scenario,
        top_k,
        report,
    })
}

fn run(args: Args) -> Result<(), String> {
    let cache_limits = DeepseekV4CacheLimits {
        tensor_bytes: 32 * 1024 * 1024,
        expert_bytes: 64 * 1024 * 1024,
    };
    let reference_checkpoint = DeepseekV4Checkpoint::open(&args.model, cache_limits)?;
    let production_checkpoint = DeepseekV4Checkpoint::open(&args.model, cache_limits)?;
    let scenario_yaml = std::fs::read_to_string(&args.scenario)
        .map_err(|err| format!("read scenario {}: {err}", args.scenario.display()))?;
    let config = ScenarioConfig::from_yaml_str(&scenario_yaml)
        .map_err(|err| format!("parse scenario {}: {err}", args.scenario.display()))?;
    let topology = SimTopology::from_config(&config).map_err(|err| err.to_string())?;
    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let execution = execute_deepseek_official_first_token_with_progress_through_simpler(
        &reference_checkpoint,
        &production_checkpoint,
        &topology,
        &task,
        &args.artifact_dir,
        &args.tokens,
        args.top_k,
        |layer| {
            eprintln!(
                "{}",
                serde_json::json!({
                    "event": "layer_complete",
                    "layer_id": layer.layer_id,
                    "compress_ratio": layer.compress_ratio,
                    "selected_experts": layer.selected_experts,
                    "attention_max_abs_diff": layer.attention_max_abs_diff,
                    "raw_kv_max_abs_diff": layer.raw_kv_max_abs_diff,
                    "route_weight_max_abs_diff": layer.route_weight_max_abs_diff,
                    "output_hidden_max_abs_diff": layer.output_hidden_max_abs_diff,
                    "attention_compressor_pending_max_abs_diff": layer.attention_compressor_pending_max_abs_diff,
                    "indexer_compressor_pending_max_abs_diff": layer.indexer_compressor_pending_max_abs_diff,
                    "indexer_query_max_abs_diff": layer.indexer_query_max_abs_diff,
                    "indexer_weights_max_abs_diff": layer.indexer_weights_max_abs_diff,
                })
            );
        },
    )?;
    validate_deepseek_official_first_token_alignment(&execution)?;
    let json = serde_json::to_string_pretty(&execution)
        .map_err(|err| format!("serialize first-token report: {err}"))?;
    if let Some(report) = args.report {
        std::fs::write(&report, format!("{json}\n"))
            .map_err(|err| format!("write report {}: {err}", report.display()))?;
        eprintln!("{}", report.display());
    } else {
        println!("{json}");
    }
    Ok(())
}

fn main() {
    if let Err(error) = parse_args(std::env::args().skip(1)).and_then(run) {
        eprintln!("{error}");
        std::process::exit(2);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_explicit_raw_token_sequence() {
        let args = parse_args([
            "--model".to_string(),
            "/model".to_string(),
            "--tokens".to_string(),
            "1".to_string(),
            "--top-k".to_string(),
            "8".to_string(),
        ])
        .unwrap();
        assert_eq!(args.model, PathBuf::from("/model"));
        assert_eq!(args.tokens, vec![1]);
        assert_eq!(args.top_k, 8);
        assert_eq!(args.report, None);
    }

    #[test]
    fn rejects_missing_or_invalid_tokens() {
        assert!(parse_args(["--model".to_string(), "/model".to_string()]).is_err());
        assert!(parse_args([
            "--model".to_string(),
            "/model".to_string(),
            "--tokens".to_string(),
            "1,nope".to_string(),
        ])
        .is_err());
        assert!(parse_args([
            "--model".to_string(),
            "/model".to_string(),
            "--tokens".to_string(),
            "1".to_string(),
            "--top-k".to_string(),
            "0".to_string(),
        ])
        .is_err());
    }
}
