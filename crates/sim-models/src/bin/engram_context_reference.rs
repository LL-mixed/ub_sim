use sim_models::{
    engram_context::deterministic_engram_context_fixture,
    engram_simt_adapter::{
        artifact_config_from_env, discover_engram_simt_artifact, EngramSimtArtifactConfig,
    },
};
use std::path::PathBuf;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum CliMode {
    CpuReference,
    FusedSimt,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct CliArgs {
    mode: CliMode,
    batch: usize,
    rows: usize,
    artifact_dir: Option<PathBuf>,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = parse_args(std::env::args().skip(1))?;
    match args.mode {
        CliMode::CpuReference => {
            let output = deterministic_engram_context_fixture(args.batch, args.rows)?;
            println!("{}", serde_json::to_string_pretty(&output.report)?);
        }
        CliMode::FusedSimt => {
            let config = if let Some(artifact_dir) = args.artifact_dir {
                EngramSimtArtifactConfig::new(artifact_dir, args.batch, args.rows)
            } else {
                artifact_config_from_env(args.batch, args.rows)?
            };
            let spec = discover_engram_simt_artifact(&config)?;
            println!("{}", serde_json::to_string_pretty(&spec)?);
        }
    }
    Ok(())
}

fn parse_args<I>(args: I) -> Result<CliArgs, String>
where
    I: IntoIterator,
    I::Item: Into<String>,
{
    let mut mode = CliMode::CpuReference;
    let mut batch = 1usize;
    let mut rows = 65_536usize;
    let mut artifact_dir = None;
    let mut pending = args.into_iter().map(Into::into).peekable();

    while let Some(arg) = pending.next() {
        if arg == "--mode" {
            let value = pending
                .next()
                .ok_or_else(|| "--mode requires a value".to_string())?;
            mode = parse_mode(&value)?;
        } else if let Some(value) = arg.strip_prefix("--mode=") {
            mode = parse_mode(value)?;
        } else if arg == "--artifact-dir" {
            let value = pending
                .next()
                .ok_or_else(|| "--artifact-dir requires a value".to_string())?;
            artifact_dir = Some(PathBuf::from(value));
        } else if let Some(value) = arg.strip_prefix("--artifact-dir=") {
            artifact_dir = Some(PathBuf::from(value));
        } else if arg == "--batch" {
            let value = pending
                .next()
                .ok_or_else(|| "--batch requires a value".to_string())?;
            batch = parse_positive_usize("--batch", &value)?;
        } else if let Some(value) = arg.strip_prefix("--batch=") {
            batch = parse_positive_usize("--batch", value)?;
        } else if arg == "--rows" || arg == "--table-rows" {
            let value = pending
                .next()
                .ok_or_else(|| format!("{arg} requires a value"))?;
            rows = parse_positive_usize(&arg, &value)?;
        } else if let Some(value) = arg.strip_prefix("--rows=") {
            rows = parse_positive_usize("--rows", value)?;
        } else if let Some(value) = arg.strip_prefix("--table-rows=") {
            rows = parse_positive_usize("--table-rows", value)?;
        } else {
            return Err(format!("unknown engram-context-reference option: {arg}"));
        }
    }

    Ok(CliArgs {
        mode,
        batch,
        rows,
        artifact_dir,
    })
}

fn parse_mode(value: &str) -> Result<CliMode, String> {
    match value {
        "cpu" | "cpu-reference" => Ok(CliMode::CpuReference),
        "fused-simt" => Ok(CliMode::FusedSimt),
        _ => Err(format!("unsupported --mode: {value}")),
    }
}

fn parse_positive_usize(name: &str, value: &str) -> Result<usize, String> {
    let parsed = value
        .parse::<usize>()
        .map_err(|err| format!("{name} must be a positive integer: {err}"))?;
    if parsed == 0 {
        return Err(format!("{name} must be > 0"));
    }
    Ok(parsed)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cli_args_accept_batch_and_rows() {
        let args = parse_args(["--batch=4", "--rows", "32"]).expect("parse args");
        assert_eq!(
            args,
            CliArgs {
                mode: CliMode::CpuReference,
                batch: 4,
                rows: 32,
                artifact_dir: None
            }
        );
    }

    #[test]
    fn cli_args_accept_fused_simt_artifact_dir() {
        let args = parse_args([
            "--mode=fused-simt",
            "--batch=16",
            "--table-rows=65536",
            "--artifact-dir=/tmp/engram-simt-build",
        ])
        .expect("parse args");
        assert_eq!(
            args,
            CliArgs {
                mode: CliMode::FusedSimt,
                batch: 16,
                rows: 65_536,
                artifact_dir: Some(PathBuf::from("/tmp/engram-simt-build"))
            }
        );
    }

    #[test]
    fn cli_args_reject_unknown_option() {
        let err = parse_args(["--mode=fused"]).expect_err("unsupported mode should fail");
        assert!(err.contains("unsupported --mode"));
    }
}
