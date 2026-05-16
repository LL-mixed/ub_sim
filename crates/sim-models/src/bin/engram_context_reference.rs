use sim_models::engram_context::deterministic_engram_context_fixture;

#[derive(Clone, Debug, Eq, PartialEq)]
struct CliArgs {
    batch: usize,
    rows: usize,
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args = parse_args(std::env::args().skip(1))?;
    let output = deterministic_engram_context_fixture(args.batch, args.rows)?;
    println!("{}", serde_json::to_string_pretty(&output.report)?);
    Ok(())
}

fn parse_args<I>(args: I) -> Result<CliArgs, String>
where
    I: IntoIterator,
    I::Item: Into<String>,
{
    let mut batch = 1usize;
    let mut rows = 65_536usize;
    let mut pending = args.into_iter().map(Into::into).peekable();

    while let Some(arg) = pending.next() {
        if arg == "--batch" {
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

    Ok(CliArgs { batch, rows })
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
        assert_eq!(args, CliArgs { batch: 4, rows: 32 });
    }

    #[test]
    fn cli_args_reject_unknown_option() {
        let err = parse_args(["--mode=fused"]).expect_err("unknown option should fail");
        assert!(err.contains("unknown engram-context-reference option"));
    }
}
