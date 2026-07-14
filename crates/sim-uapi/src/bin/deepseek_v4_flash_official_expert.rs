use std::path::PathBuf;

use serde::Serialize;
use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash_checkpoint::{DeepseekV4CacheLimits, DeepseekV4Checkpoint};
use sim_models::deepseek_v4_flash_checkpoint_reference::deterministic_hidden_fixture;
use sim_topology::SimTopology;
use sim_uapi::{
    execute_deepseek_official_routed_expert_through_simpler,
    execute_deepseek_official_routed_experts_through_simpler,
    execute_deepseek_official_router_through_simpler, DeepseekV4OfficialRoutedExpertExecution,
    DeepseekV4OfficialRouterExecution,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Command {
    Route,
    Expert,
    Selected,
}

#[derive(Debug, PartialEq)]
struct Args {
    command: Command,
    model: PathBuf,
    layer: usize,
    token: u64,
    seed: u64,
    expert: Option<usize>,
    expert_weight: Option<f32>,
    router_artifact: PathBuf,
    expert_artifact_dir: PathBuf,
    scenario: PathBuf,
    tensor_cache_bytes: u64,
    expert_cache_bytes: u64,
}

#[derive(Serialize)]
struct RouterReport<'a> {
    layer: usize,
    token_id: u64,
    hash_routed: bool,
    logits_checksum: &'a str,
    selected_experts: &'a [usize],
    expert_weights: &'a [f32],
}

impl<'a> From<&'a DeepseekV4OfficialRouterExecution> for RouterReport<'a> {
    fn from(route: &'a DeepseekV4OfficialRouterExecution) -> Self {
        Self {
            layer: route.layer,
            token_id: route.token_id,
            hash_routed: route.hash_routed,
            logits_checksum: &route.logits_checksum,
            selected_experts: &route.expert_indices,
            expert_weights: &route.expert_weights,
        }
    }
}

#[derive(Serialize)]
struct ExpertReport<'a> {
    layer: usize,
    expert: usize,
    route_weight: f32,
    gate_checksum: &'a str,
    up_checksum: &'a str,
    activated_checksum: &'a str,
    output_elements: usize,
    output_sample: &'a [f32],
    output_checksum: &'a str,
    dispatch_count: usize,
    expert_disk_read_bytes: u64,
    expert_cache_after: sim_models::deepseek_v4_flash_checkpoint::DeepseekV4CacheStats,
}

impl<'a> From<&'a DeepseekV4OfficialRoutedExpertExecution> for ExpertReport<'a> {
    fn from(execution: &'a DeepseekV4OfficialRoutedExpertExecution) -> Self {
        Self {
            layer: execution.layer,
            expert: execution.expert,
            route_weight: execution.route_weight,
            gate_checksum: &execution.gate_checksum,
            up_checksum: &execution.up_checksum,
            activated_checksum: &execution.activated_checksum,
            output_elements: execution.output.len(),
            output_sample: &execution.output[..execution.output.len().min(8)],
            output_checksum: &execution.output_checksum,
            dispatch_count: execution.dispatch_count,
            expert_disk_read_bytes: execution.expert_disk_read_bytes,
            expert_cache_after: execution.expert_cache_after,
        }
    }
}

#[derive(Serialize)]
struct SelectedReport<'a> {
    route: RouterReport<'a>,
    experts: Vec<ExpertReport<'a>>,
    output_elements: usize,
    output_sample: &'a [f32],
    output_checksum: &'a str,
    dispatch_count: usize,
    expert_disk_read_bytes: u64,
}

fn usage() -> &'static str {
    "usage: deepseek_v4_flash_official_expert route|expert|selected --model PATH --layer N [--token N] [--seed N] [--expert N --expert-weight F] [--router-artifact PATH] [--expert-artifact-dir PATH] [--scenario PATH] [--tensor-cache-bytes N] [--expert-cache-bytes N]"
}

fn default_scenario() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../scenarios/mvp_2host_single_domain.yaml")
}

fn parse_args<I>(args: I) -> Result<Args, String>
where
    I: IntoIterator<Item = String>,
{
    let mut args = args.into_iter();
    let command = match args.next().as_deref() {
        Some("route") => Command::Route,
        Some("expert") => Command::Expert,
        Some("selected") => Command::Selected,
        _ => return Err(usage().to_string()),
    };
    let mut model = None;
    let mut layer = None;
    let mut token = 1u64;
    let mut seed = 7u64;
    let mut expert = None;
    let mut expert_weight: Option<f32> = None;
    let mut router_artifact =
        PathBuf::from("/tmp/simpler-host-bf16-gemm-artifacts/host_gemm_manifest.json");
    let mut expert_artifact_dir = PathBuf::from("/tmp/simpler-host-fp4-expert-artifacts");
    let mut scenario = default_scenario();
    let mut tensor_cache_bytes = 64 * 1024 * 1024u64;
    let mut expert_cache_bytes = 256 * 1024 * 1024u64;
    while let Some(flag) = args.next() {
        let value = args
            .next()
            .ok_or_else(|| format!("missing value for {flag}; {}", usage()))?;
        match flag.as_str() {
            "--model" => model = Some(PathBuf::from(value)),
            "--layer" => {
                layer = Some(
                    value
                        .parse()
                        .map_err(|_| format!("invalid --layer {value}"))?,
                )
            }
            "--token" => {
                token = value
                    .parse()
                    .map_err(|_| format!("invalid --token {value}"))?
            }
            "--seed" => {
                seed = value
                    .parse()
                    .map_err(|_| format!("invalid --seed {value}"))?
            }
            "--expert" => {
                expert = Some(
                    value
                        .parse()
                        .map_err(|_| format!("invalid --expert {value}"))?,
                )
            }
            "--expert-weight" => {
                expert_weight = Some(
                    value
                        .parse()
                        .map_err(|_| format!("invalid --expert-weight {value}"))?,
                )
            }
            "--router-artifact" => router_artifact = PathBuf::from(value),
            "--expert-artifact-dir" => expert_artifact_dir = PathBuf::from(value),
            "--scenario" => scenario = PathBuf::from(value),
            "--tensor-cache-bytes" => {
                tensor_cache_bytes = value
                    .parse()
                    .map_err(|_| format!("invalid --tensor-cache-bytes {value}"))?
            }
            "--expert-cache-bytes" => {
                expert_cache_bytes = value
                    .parse()
                    .map_err(|_| format!("invalid --expert-cache-bytes {value}"))?
            }
            _ => return Err(format!("unknown option {flag}; {}", usage())),
        }
    }
    if command == Command::Expert {
        if expert.is_none() || expert_weight.is_none() {
            return Err("expert requires --expert and --expert-weight".to_string());
        }
    } else if expert.is_some() || expert_weight.is_some() {
        return Err("--expert and --expert-weight are only valid for expert".to_string());
    }
    if expert_weight.is_some_and(|weight| !weight.is_finite() || weight <= 0.0)
        || tensor_cache_bytes == 0
        || expert_cache_bytes == 0
    {
        return Err("cache sizes and expert weight must be positive and finite".to_string());
    }
    Ok(Args {
        command,
        model: model.ok_or_else(|| format!("--model is required; {}", usage()))?,
        layer: layer.ok_or_else(|| format!("--layer is required; {}", usage()))?,
        token,
        seed,
        expert,
        expert_weight,
        router_artifact,
        expert_artifact_dir,
        scenario,
        tensor_cache_bytes,
        expert_cache_bytes,
    })
}

fn topology(path: &PathBuf) -> Result<SimTopology, String> {
    let yaml = std::fs::read_to_string(path)
        .map_err(|err| format!("read scenario {}: {err}", path.display()))?;
    let config = ScenarioConfig::from_yaml_str(&yaml)
        .map_err(|err| format!("parse scenario {}: {err}", path.display()))?;
    SimTopology::from_config(&config).map_err(|err| err.to_string())
}

fn run(args: Args) -> Result<(), String> {
    let checkpoint = DeepseekV4Checkpoint::open(
        &args.model,
        DeepseekV4CacheLimits {
            tensor_bytes: args.tensor_cache_bytes,
            expert_bytes: args.expert_cache_bytes,
        },
    )?;
    let topology = topology(&args.scenario)?;
    let input = deterministic_hidden_fixture(args.seed, checkpoint.config.hidden_size as usize);
    let task = TaskKey {
        logical_system: LogicalSystemId(1),
        coord: HierarchyCoord { levels: [0; 8] },
        scope_depth: 0,
        task_id: 1,
    };
    let json = match args.command {
        Command::Route => {
            let route = execute_deepseek_official_router_through_simpler(
                &checkpoint,
                &topology,
                &task,
                &args.router_artifact,
                args.layer,
                args.token,
                &input,
            )?;
            serde_json::to_string_pretty(&RouterReport::from(&route))
        }
        Command::Expert => {
            let execution = execute_deepseek_official_routed_expert_through_simpler(
                &checkpoint,
                &topology,
                &task,
                &args.expert_artifact_dir,
                900_000,
                args.layer,
                args.expert.unwrap(),
                args.expert_weight.unwrap(),
                &input,
            )?;
            serde_json::to_string_pretty(&ExpertReport::from(&execution))
        }
        Command::Selected => {
            let route = execute_deepseek_official_router_through_simpler(
                &checkpoint,
                &topology,
                &task,
                &args.router_artifact,
                args.layer,
                args.token,
                &input,
            )?;
            let mut routed_task = task.clone();
            routed_task.task_id = 100;
            let routed = execute_deepseek_official_routed_experts_through_simpler(
                &checkpoint,
                &topology,
                &routed_task,
                &args.expert_artifact_dir,
                1_000_000,
                args.layer,
                &route.expert_indices,
                &route.expert_weights,
                &input,
            )?;
            let expert_disk_read_bytes = routed
                .experts
                .iter()
                .map(|execution| execution.expert_disk_read_bytes)
                .sum();
            let report = SelectedReport {
                route: RouterReport::from(&route),
                experts: routed.experts.iter().map(ExpertReport::from).collect(),
                output_elements: routed.output.len(),
                output_sample: &routed.output[..routed.output.len().min(8)],
                output_checksum: &routed.output_checksum,
                dispatch_count: routed.dispatch_count,
                expert_disk_read_bytes,
            };
            serde_json::to_string_pretty(&report)
        }
    }
    .map_err(|err| format!("serialize official expert execution: {err}"))?;
    println!("{json}");
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
    fn route_and_expert_contracts_parse_strictly() {
        let route = parse_args(
            ["route", "--model", "/models/ds4", "--layer", "2"]
                .into_iter()
                .map(str::to_string),
        )
        .unwrap();
        assert_eq!(route.command, Command::Route);
        assert_eq!(route.token, 1);

        let expert = parse_args(
            [
                "expert",
                "--model",
                "/models/ds4",
                "--layer",
                "3",
                "--expert",
                "29",
                "--expert-weight",
                "0.25",
            ]
            .into_iter()
            .map(str::to_string),
        )
        .unwrap();
        assert_eq!(expert.expert, Some(29));
        assert_eq!(expert.expert_weight, Some(0.25));
    }

    #[test]
    fn args_reject_ambiguous_missing_and_non_finite_values() {
        assert!(parse_args(Vec::<String>::new()).is_err());
        assert!(parse_args(
            ["expert", "--model", "m", "--layer", "1"]
                .into_iter()
                .map(str::to_string)
        )
        .is_err());
        assert!(parse_args(
            ["route", "--model", "m", "--layer", "1", "--expert", "2",]
                .into_iter()
                .map(str::to_string)
        )
        .is_err());
        assert!(parse_args(
            [
                "expert",
                "--model",
                "m",
                "--layer",
                "1",
                "--expert",
                "2",
                "--expert-weight",
                "NaN",
            ]
            .into_iter()
            .map(str::to_string)
        )
        .is_err());
    }
}
