use std::collections::BTreeMap;
use std::env;
use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{bail, Context, Result};
use sim_console::api;
use sim_console::domain::RunStatus;
use sim_console::{DemoCatalog, RunManager, StartRunRequest, TargetRegistry};

const USAGE: &str = r#"Usage:
  sim-console [--repo-root PATH] [--catalog PATH] [--targets PATH] catalog
  sim-console [--repo-root PATH] [--catalog PATH] [--targets PATH] targets
  sim-console [--repo-root PATH] [--catalog PATH] [--targets PATH] readiness [--target TARGET]
  sim-console [--repo-root PATH] [--catalog PATH] [--targets PATH] runs
  sim-console [--repo-root PATH] [--catalog PATH] [--targets PATH] run DEMO [--target TARGET] [--set NAME=VALUE]
  sim-console [--repo-root PATH] [--catalog PATH] [--targets PATH] status RUN_ID
  sim-console [--repo-root PATH] [--catalog PATH] [--targets PATH] logs RUN_ID [--node NODE]
  sim-console [--repo-root PATH] [--catalog PATH] [--targets PATH] stop RUN_ID
  sim-console [--repo-root PATH] [--catalog PATH] [--targets PATH] serve [--listen ADDRESS]
"#;

#[tokio::main]
async fn main() -> Result<()> {
    let mut args: Vec<String> = env::args().skip(1).collect();
    if args.first().is_some_and(|arg| arg == "__remote-worker") {
        args.remove(0);
        let plan = take_positional(&mut args, "PLAN")?;
        require_empty(&args)?;
        sim_console::runner::run_remote_worker(Path::new(&plan))?;
        return Ok(());
    }
    let repo_root = take_option(&mut args, "--repo-root")?
        .map(PathBuf::from)
        .map(Ok)
        .unwrap_or_else(discover_repo_root)?;
    let catalog_path = take_option(&mut args, "--catalog")?.map(PathBuf::from);
    let targets_path = take_option(&mut args, "--targets")?.map(PathBuf::from);
    let catalog = match catalog_path {
        Some(path) => DemoCatalog::load_path(&absolute_from(&repo_root, &path))?,
        None => DemoCatalog::load_default()?,
    };
    let targets = match targets_path {
        Some(path) => TargetRegistry::load_path(&absolute_from(&repo_root, &path))?,
        None => TargetRegistry::load_default()?,
    };
    let manager = RunManager::with_targets(&repo_root, catalog, targets)?;

    let Some(command) = args.first().cloned() else {
        bail!(USAGE);
    };
    args.remove(0);
    match command.as_str() {
        "catalog" => {
            require_empty(&args)?;
            println!(
                "{}",
                serde_json::to_string_pretty(manager.catalog().as_ref())?
            );
        }
        "targets" => {
            require_empty(&args)?;
            println!(
                "{}",
                serde_json::to_string_pretty(manager.targets().as_ref())?
            );
        }
        "readiness" => {
            let target = take_option(&mut args, "--target")?;
            require_empty(&args)?;
            println!(
                "{}",
                serde_json::to_string_pretty(&manager.readiness(target.as_deref()).await?)?
            );
        }
        "runs" => {
            require_empty(&args)?;
            println!("{}", serde_json::to_string_pretty(&manager.list().await)?);
        }
        "run" => run_command(&manager, args).await?,
        "status" => {
            let run_id = take_positional(&mut args, "RUN_ID")?;
            require_empty(&args)?;
            println!(
                "{}",
                serde_json::to_string_pretty(&manager.get(&run_id).await?)?
            );
        }
        "logs" => {
            let run_id = take_positional(&mut args, "RUN_ID")?;
            let node = take_option(&mut args, "--node")?;
            require_empty(&args)?;
            let logs = manager.logs(&run_id, node.as_deref(), 0).await?;
            for line in logs.lines {
                println!("{line}");
            }
        }
        "stop" => {
            let run_id = take_positional(&mut args, "RUN_ID")?;
            require_empty(&args)?;
            println!(
                "{}",
                serde_json::to_string_pretty(&manager.stop(&run_id).await?)?
            );
        }
        "serve" => {
            let listen = take_option(&mut args, "--listen")?
                .unwrap_or_else(|| "127.0.0.1:9080".to_string())
                .parse::<SocketAddr>()
                .context("invalid --listen address")?;
            require_empty(&args)?;
            let listener = tokio::net::TcpListener::bind(listen).await?;
            let address = listener.local_addr()?;
            println!("sim-console listening on http://{address}");
            axum::serve(listener, api::router(manager)).await?;
        }
        "help" | "-h" | "--help" => print!("{USAGE}"),
        _ => bail!("unknown command: {command}\n\n{USAGE}"),
    }
    Ok(())
}

async fn run_command(manager: &RunManager, mut args: Vec<String>) -> Result<()> {
    let demo_id = take_positional(&mut args, "DEMO")?;
    let target_id = take_option(&mut args, "--target")?;
    let mut parameters = BTreeMap::new();
    while let Some(index) = args.iter().position(|item| item == "--set") {
        if index + 1 >= args.len() {
            bail!("--set requires NAME=VALUE");
        }
        let assignment = args.remove(index + 1);
        args.remove(index);
        let (name, value) = assignment
            .split_once('=')
            .context("--set requires NAME=VALUE")?;
        if name.is_empty() {
            bail!("--set parameter name cannot be empty");
        }
        parameters.insert(name.to_string(), value.to_string());
    }
    require_empty(&args)?;
    let run = manager
        .start(StartRunRequest {
            demo_id,
            target_id,
            parameters,
        })
        .await?;
    println!("started {}", run.id);
    loop {
        let current = manager.get(&run.id).await?;
        if current.status.is_terminal() {
            println!("{}", serde_json::to_string_pretty(&current)?);
            if current.status != RunStatus::Passed {
                bail!(
                    "run {} finished with status {:?}",
                    current.id,
                    current.status
                );
            }
            return Ok(());
        }
        tokio::time::sleep(Duration::from_millis(250)).await;
    }
}

fn take_option(args: &mut Vec<String>, name: &str) -> Result<Option<String>> {
    let Some(index) = args.iter().position(|item| item == name) else {
        return Ok(None);
    };
    if index + 1 >= args.len() {
        bail!("{name} requires a value");
    }
    let value = args.remove(index + 1);
    args.remove(index);
    Ok(Some(value))
}

fn take_positional(args: &mut Vec<String>, name: &str) -> Result<String> {
    if args.is_empty() || args[0].starts_with('-') {
        bail!("missing {name}\n\n{USAGE}");
    }
    Ok(args.remove(0))
}

fn require_empty(args: &[String]) -> Result<()> {
    if !args.is_empty() {
        bail!("unexpected arguments: {}\n\n{USAGE}", args.join(" "));
    }
    Ok(())
}

fn discover_repo_root() -> Result<PathBuf> {
    let current = env::current_dir()?;
    for candidate in current.ancestors() {
        if candidate.join("Cargo.toml").is_file() && candidate.join("guest-linux/aarch64").is_dir()
        {
            return Ok(candidate.to_path_buf());
        }
    }
    bail!("unable to find repository root; pass --repo-root PATH")
}

fn absolute_from(root: &Path, path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        root.join(path)
    }
}
