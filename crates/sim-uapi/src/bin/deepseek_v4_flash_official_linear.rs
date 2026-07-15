use std::path::PathBuf;

use sim_config::ScenarioConfig;
use sim_core::{HierarchyCoord, LogicalSystemId, TaskKey};
use sim_models::deepseek_v4_flash_checkpoint::{
    DeepseekV4CacheLimits, DeepseekV4Checkpoint, DeepseekV4TensorDType,
};
use sim_models::deepseek_v4_flash_checkpoint_reference::deterministic_hidden_fixture;
use sim_topology::SimTopology;
use sim_uapi::{
    execute_deepseek_official_bf16_rows_through_simpler,
    execute_deepseek_official_f32_rows_through_simpler,
    execute_deepseek_official_fp4_rows_through_simpler,
    execute_deepseek_official_fp8_rows_through_simpler, DeepseekV4LinearOutputDType,
};

#[derive(Debug, PartialEq, Eq)]
struct Args {
    model: PathBuf,
    tensor: String,
    artifact: Option<PathBuf>,
    scenario: PathBuf,
    row_start: usize,
    row_count: usize,
    seed: u64,
    output_dtype: DeepseekV4LinearOutputDType,
}

fn usage() -> &'static str {
    "usage: deepseek_v4_flash_official_linear --model PATH --tensor NAME [--artifact PATH] [--scenario PATH] [--row-start N] [--row-count N] [--seed N] [--output-dtype bf16|fp32]"
}

fn default_scenario() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../scenarios/mvp_2host_single_domain.yaml")
}

fn parse_args<I>(args: I) -> Result<Args, String>
where
    I: IntoIterator<Item = String>,
{
    let mut model = None;
    let mut tensor = None;
    let mut artifact = None;
    let mut scenario = default_scenario();
    let mut row_start = 0usize;
    let mut row_count = 8usize;
    let mut seed = 7u64;
    let mut output_dtype = DeepseekV4LinearOutputDType::Bf16;
    let mut args = args.into_iter();
    while let Some(flag) = args.next() {
        let value = args
            .next()
            .ok_or_else(|| format!("missing value for {flag}; {}", usage()))?;
        match flag.as_str() {
            "--model" => model = Some(PathBuf::from(value)),
            "--tensor" => tensor = Some(value),
            "--artifact" => artifact = Some(PathBuf::from(value)),
            "--scenario" => scenario = PathBuf::from(value),
            "--row-start" => {
                row_start = value
                    .parse()
                    .map_err(|_| format!("invalid --row-start {value}"))?
            }
            "--row-count" => {
                row_count = value
                    .parse()
                    .map_err(|_| format!("invalid --row-count {value}"))?
            }
            "--seed" => {
                seed = value
                    .parse()
                    .map_err(|_| format!("invalid --seed {value}"))?
            }
            "--output-dtype" => {
                output_dtype = match value.as_str() {
                    "bf16" => DeepseekV4LinearOutputDType::Bf16,
                    "fp32" => DeepseekV4LinearOutputDType::F32,
                    _ => {
                        return Err(format!(
                            "invalid --output-dtype {value}; expected bf16|fp32"
                        ))
                    }
                }
            }
            _ => return Err(format!("unknown option {flag}; {}", usage())),
        }
    }
    if row_count == 0 {
        return Err("--row-count must be positive".to_string());
    }
    Ok(Args {
        model: model.ok_or_else(|| format!("--model is required; {}", usage()))?,
        tensor: tensor.ok_or_else(|| format!("--tensor is required; {}", usage()))?,
        artifact,
        scenario,
        row_start,
        row_count,
        seed,
        output_dtype,
    })
}

fn run(args: Args) -> Result<(), String> {
    let checkpoint = DeepseekV4Checkpoint::open(&args.model, DeepseekV4CacheLimits::default())?;
    let tensor = checkpoint.tensor(&args.tensor)?;
    let input_size = tensor
        .shape
        .get(1)
        .copied()
        .ok_or_else(|| format!("tensor is not a matrix: {}", args.tensor))?;
    let stored_input_size = usize::try_from(input_size)
        .map_err(|_| format!("tensor input is too large: {}", args.tensor))?;
    let input_size = if tensor.dtype == DeepseekV4TensorDType::I8 {
        stored_input_size
            .checked_mul(2)
            .ok_or_else(|| format!("tensor logical input is too large: {}", args.tensor))?
    } else {
        stored_input_size
    };
    let input = deterministic_hidden_fixture(args.seed, input_size);
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
    let json = match tensor.dtype {
        DeepseekV4TensorDType::F32 => {
            let artifact = args.artifact.unwrap_or_else(|| {
                PathBuf::from(format!(
                    "/tmp/simpler-host-fp32-gemm-{input_size}-artifacts/host_fp32_gemm_manifest.json"
                ))
            });
            let execution = execute_deepseek_official_f32_rows_through_simpler(
                &checkpoint,
                &topology,
                &task,
                &artifact,
                &args.tensor,
                args.row_start,
                args.row_count,
                &input,
                args.output_dtype,
            )?;
            serde_json::to_string_pretty(&execution)
        }
        DeepseekV4TensorDType::F8E4M3 => {
            let artifact = args.artifact.unwrap_or_else(|| {
                PathBuf::from("/tmp/simpler-host-fp8-gemm-artifacts/host_fp8_gemm_manifest.json")
            });
            let execution = execute_deepseek_official_fp8_rows_through_simpler(
                &checkpoint,
                &topology,
                &task,
                &artifact,
                700_000,
                &args.tensor,
                args.row_start,
                args.row_count,
                &input,
                args.output_dtype,
            )?;
            serde_json::to_string_pretty(&execution)
        }
        DeepseekV4TensorDType::Bf16 => {
            let artifact = args.artifact.unwrap_or_else(|| {
                PathBuf::from("/tmp/simpler-host-bf16-gemm-artifacts/host_gemm_manifest.json")
            });
            let execution = execute_deepseek_official_bf16_rows_through_simpler(
                &checkpoint,
                &topology,
                &task,
                &artifact,
                &args.tensor,
                args.row_start,
                args.row_count,
                &input,
                args.output_dtype,
            )?;
            serde_json::to_string_pretty(&execution)
        }
        DeepseekV4TensorDType::I8 => {
            let artifact = args.artifact.unwrap_or_else(|| {
                PathBuf::from(format!(
                    "/tmp/simpler-host-fp4-gemm-{input_size}-artifacts/host_fp4_gemm_manifest.json"
                ))
            });
            let execution = execute_deepseek_official_fp4_rows_through_simpler(
                &checkpoint,
                &topology,
                &task,
                &artifact,
                800_000,
                &args.tensor,
                args.row_start,
                args.row_count,
                &input,
                args.output_dtype,
            )?;
            serde_json::to_string_pretty(&execution)
        }
        other => {
            return Err(format!(
                "unsupported official linear dtype for {}: {}",
                args.tensor,
                other.safetensors_name()
            ))
        }
    }
    .map_err(|err| format!("serialize official linear execution: {err}"))?;
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
    fn args_accept_explicit_production_operator_contract() {
        let args = parse_args(
            [
                "--model",
                "/models/ds4",
                "--tensor",
                "layers.0.attn.wkv.weight",
                "--row-start",
                "127",
                "--row-count",
                "2",
                "--seed",
                "9",
                "--output-dtype",
                "fp32",
            ]
            .into_iter()
            .map(str::to_string),
        )
        .unwrap();
        assert_eq!(args.row_start, 127);
        assert_eq!(args.row_count, 2);
        assert_eq!(args.seed, 9);
        assert_eq!(args.output_dtype, DeepseekV4LinearOutputDType::F32);
    }

    #[test]
    fn args_fail_closed_on_missing_unknown_and_zero_rows() {
        assert!(parse_args(Vec::<String>::new()).is_err());
        assert!(parse_args(["--wat", "x"].into_iter().map(str::to_string)).is_err());
        assert!(parse_args(
            ["--model", "m", "--tensor", "t", "--row-count", "0"]
                .into_iter()
                .map(str::to_string)
        )
        .is_err());
    }
}
