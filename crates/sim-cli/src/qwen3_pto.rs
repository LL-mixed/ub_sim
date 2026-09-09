use anyhow::Context;
use std::path::PathBuf;

#[path = "qwen3_pto_trace.rs"]
mod trace;

fn parse_args(
    mut args: impl Iterator<Item = std::ffi::OsString>,
) -> anyhow::Result<Option<(PathBuf, PathBuf)>> {
    if args.next().as_deref() != Some(std::ffi::OsStr::new("qwen3-pto-operator-check")) {
        return Ok(None);
    }
    let mut manifest = None;
    let mut scenario = None;
    while let Some(flag) = args.next() {
        match flag.to_str() {
            Some("--manifest") if manifest.is_none() => {
                manifest = Some(PathBuf::from(
                    args.next().context("--manifest needs a path")?,
                ));
            }
            Some("--scenario") if scenario.is_none() => {
                scenario = Some(PathBuf::from(
                    args.next().context("--scenario needs a path")?,
                ));
            }
            _ => anyhow::bail!("unknown or repeated option: {}", flag.to_string_lossy()),
        }
    }
    let manifest = manifest.context("provide --manifest with a Qwen3 PTO operator manifest")?;
    let scenario = scenario.context("provide --scenario with a simpler_capi topology YAML")?;
    Ok(Some((manifest, scenario)))
}

pub fn run_if_requested() -> anyhow::Result<Option<()>> {
    if trace::run_if_requested()? {
        return Ok(Some(()));
    }
    if let Some((manifest, scenario, weights, end, tokens)) =
        parse_range_args(std::env::args_os().skip(1))?
    {
        let report =
            sim_workloads::run_qwen3_pto_range_check(&scenario, &manifest, &weights, end, &tokens)
                .map_err(anyhow::Error::msg)?;
        println!(
            "{}",
            serde_json::to_string_pretty(&serde_json::json!({
                "status": "pass", "scope": "real-weight range oracle; mock backing; no QEMU guest",
                "result": report,
            }))?
        );
        return Ok(Some(()));
    }
    let Some((manifest, scenario)) = parse_args(std::env::args_os().skip(1))? else {
        return Ok(None);
    };
    let reports = sim_workloads::run_qwen3_pto_operator_checks(&scenario, &manifest)?;
    println!(
        "{}",
        serde_json::to_string_pretty(&serde_json::json!({
            "status": "pass", "scope": "operator-only; bridge mock backing; no QEMU guest",
            "backend": "sim-qemu bridge / Simpler / PTO UB_GM", "results": reports,
        }))?
    );
    Ok(Some(()))
}

type RangeArgs = (PathBuf, PathBuf, PathBuf, u32, Vec<u32>);

fn parse_range_args(
    mut args: impl Iterator<Item = std::ffi::OsString>,
) -> anyhow::Result<Option<RangeArgs>> {
    if args.next().as_deref() != Some(std::ffi::OsStr::new("qwen3-pto-range-check")) {
        return Ok(None);
    }
    let mut values = std::collections::BTreeMap::new();
    while let Some(flag) = args.next() {
        let flag = flag.to_str().context("option must be UTF-8")?.to_string();
        anyhow::ensure!(
            [
                "--manifest",
                "--scenario",
                "--weights",
                "--layer-end",
                "--tokens"
            ]
            .contains(&flag.as_str()),
            "unknown option: {flag}"
        );
        let value = args
            .next()
            .with_context(|| format!("{flag} needs a value"))?;
        anyhow::ensure!(values.insert(flag, value).is_none(), "repeated option");
    }
    let get = |name: &str| values.get(name).with_context(|| format!("provide {name}"));
    let end: u32 = get("--layer-end")?
        .to_str()
        .context("invalid layer end")?
        .parse()?;
    anyhow::ensure!(end > 0, "layer end must be positive");
    let tokens = get("--tokens")?
        .to_str()
        .context("invalid token list")?
        .split(',')
        .map(str::parse)
        .collect::<Result<Vec<u32>, _>>()?;
    anyhow::ensure!(
        !tokens.is_empty() && tokens.len() <= 4096,
        "token count out of range"
    );
    Ok(Some((
        get("--manifest")?.into(),
        get("--scenario")?.into(),
        get("--weights")?.into(),
        end,
        tokens,
    )))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn range_cli_requires_explicit_model_and_range() {
        let parse = |args: &[&str]| parse_range_args(args.iter().map(std::ffi::OsString::from));
        assert!(parse(&["qwen3-pto-range-check"]).is_err());
        assert!(parse(&["qwen3-pto-operator-check"]).unwrap().is_none());
        let args = [
            "qwen3-pto-range-check",
            "--manifest",
            "range.json",
            "--scenario",
            "two.yaml",
            "--weights",
            "weights",
            "--layer-end",
            "14",
            "--tokens",
            "1,42",
        ];
        let result = parse(&args).unwrap().unwrap();
        assert_eq!(result.3, 14);
        assert_eq!(result.4, vec![1, 42]);
    }

    fn parse(args: &[&str]) -> anyhow::Result<Option<(PathBuf, PathBuf)>> {
        parse_args(args.iter().map(std::ffi::OsString::from))
    }

    #[test]
    fn operator_cli_requires_explicit_artifacts_and_scenario() {
        assert!(parse(&["other-command"]).unwrap().is_none());
        assert_eq!(
            parse(&[
                "qwen3-pto-operator-check",
                "--manifest",
                "model.json",
                "--scenario",
                "two.yaml"
            ])
            .unwrap(),
            Some((PathBuf::from("model.json"), PathBuf::from("two.yaml")))
        );
        for args in [
            vec!["qwen3-pto-operator-check"],
            vec!["qwen3-pto-operator-check", "--manifest"],
            vec![
                "qwen3-pto-operator-check",
                "--manifest",
                "a",
                "--manifest",
                "b",
            ],
            vec!["qwen3-pto-operator-check", "--fallback"],
        ] {
            assert!(parse(&args).is_err());
        }
    }
}
