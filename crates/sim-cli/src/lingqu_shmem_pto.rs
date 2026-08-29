use anyhow::Context;
use serde::Serialize;
use std::ffi::OsString;
use std::path::PathBuf;

use sim_config::ScenarioConfig;
use sim_core::{CompletionStatus, SimError};
use sim_topology::SimTopology;
use sim_workloads::{
    run_host_vector_ub_gm_bridge_dispatch, run_host_vector_ub_gm_dispatch,
    HostVectorUbGmDispatchReport,
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LingquShmemPtoArgs {
    mode: LingquShmemPtoMode,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum LingquShmemPtoMode {
    ContractOnly,
    P2Mock {
        manifest: PathBuf,
        platform: String,
        scenario: PathBuf,
    },
    P2BridgeMock {
        manifest: PathBuf,
        platform: String,
        scenario: PathBuf,
    },
}

#[derive(Debug, Serialize, PartialEq, Eq)]
struct LingquShmemPtoContract {
    command: &'static str,
    implementation_phase: &'static str,
    dispatch_abi_version: u32,
    memref_abi_version: u32,
    ub_gm_address_space: u8,
    max_memrefs: u32,
    max_scalars: u32,
    max_rank: u32,
    sim_npu_default_enabled: bool,
    gva_default_enabled: bool,
    gsva_default_enabled: bool,
}

#[derive(Debug, Serialize)]
struct LingquShmemPtoMockEnvelope<'a> {
    command: &'static str,
    implementation_phase: &'static str,
    manifest: String,
    scenario: String,
    report: &'a HostVectorUbGmDispatchReport,
}

pub fn args() -> anyhow::Result<Option<LingquShmemPtoArgs>> {
    args_from(std::env::args_os().skip(1))
}

fn args_from<I, S>(args: I) -> anyhow::Result<Option<LingquShmemPtoArgs>>
where
    I: IntoIterator<Item = S>,
    S: Into<OsString>,
{
    let mut args = args.into_iter().map(Into::into);
    let Some(command) = args.next() else {
        return Ok(None);
    };
    if command != "lingqu-shmem-pto-e2e" {
        return Ok(None);
    }

    let mut contract_only = false;
    let mut manifest = None;
    let mut bridge_manifest = None;
    let mut platform = "a2a3sim".to_string();
    let mut scenario = PathBuf::from("scenarios/mvp_2host_single_domain.yaml");
    let mut execution_option_seen = false;
    while let Some(arg) = args.next() {
        match arg.to_string_lossy().as_ref() {
            "--contract-only" => contract_only = true,
            "--mock-runtime-manifest" => {
                let value = args
                    .next()
                    .context("--mock-runtime-manifest requires a path")?;
                manifest = Some(PathBuf::from(value));
                execution_option_seen = true;
            }
            "--bridge-runtime-manifest" => {
                let value = args
                    .next()
                    .context("--bridge-runtime-manifest requires a path")?;
                bridge_manifest = Some(PathBuf::from(value));
                execution_option_seen = true;
            }
            "--platform" => {
                let value = args.next().context("--platform requires a value")?;
                platform = value.to_string_lossy().into_owned();
                execution_option_seen = true;
            }
            "--scenario" => {
                let value = args.next().context("--scenario requires a path")?;
                scenario = PathBuf::from(value);
                execution_option_seen = true;
            }
            option => anyhow::bail!("unknown lingqu-shmem-pto-e2e option: {option}"),
        }
    }
    if contract_only {
        if execution_option_seen {
            anyhow::bail!("--contract-only cannot be combined with P2 mock execution options");
        }
        return Ok(Some(LingquShmemPtoArgs {
            mode: LingquShmemPtoMode::ContractOnly,
        }));
    }
    if manifest.is_some() && bridge_manifest.is_some() {
        anyhow::bail!(
            "--mock-runtime-manifest and --bridge-runtime-manifest are mutually exclusive"
        );
    }
    if !matches!(platform.as_str(), "a2a3sim" | "a5sim") {
        anyhow::bail!("--platform must be a2a3sim or a5sim");
    }
    let mode = match (manifest, bridge_manifest) {
        (Some(manifest), None) => LingquShmemPtoMode::P2Mock {
            manifest,
            platform,
            scenario,
        },
        (None, Some(manifest)) => LingquShmemPtoMode::P2BridgeMock {
            manifest,
            platform,
            scenario,
        },
        (None, None) => anyhow::bail!(
            "select --contract-only, --mock-runtime-manifest, or --bridge-runtime-manifest"
        ),
        (Some(_), Some(_)) => unreachable!(),
    };
    Ok(Some(LingquShmemPtoArgs { mode }))
}

pub fn run(args: LingquShmemPtoArgs) -> anyhow::Result<()> {
    match args.mode {
        LingquShmemPtoMode::ContractOnly => run_contract(),
        LingquShmemPtoMode::P2Mock {
            manifest,
            platform,
            scenario,
        } => run_p2_mock(manifest, platform, scenario),
        LingquShmemPtoMode::P2BridgeMock {
            manifest,
            platform,
            scenario,
        } => run_p2_bridge_mock(manifest, platform, scenario),
    }
}

fn run_contract() -> anyhow::Result<()> {
    let contract = LingquShmemPtoContract {
        command: "lingqu-shmem-pto-e2e",
        implementation_phase: "p0_abi",
        dispatch_abi_version: sim_qemu::LINGQU_PTO_DISPATCH_ABI_V2,
        memref_abi_version: sim_qemu::LINGQU_SHMEM_MEMREF_ABI_V1,
        ub_gm_address_space: sim_chipbackend_simpler::AddressSpace::UbGm as u8,
        max_memrefs: sim_qemu::LINGQU_PTO_MAX_MEMREFS,
        max_scalars: sim_qemu::LINGQU_PTO_MAX_SCALARS,
        max_rank: sim_qemu::LINGQU_PTO_MAX_RANK,
        sim_npu_default_enabled: false,
        gva_default_enabled: false,
        gsva_default_enabled: false,
    };
    println!("{}", serde_json::to_string_pretty(&contract)?);
    Ok(())
}

fn run_p2_mock(manifest: PathBuf, platform: String, scenario: PathBuf) -> anyhow::Result<()> {
    let yaml = std::fs::read_to_string(&scenario)
        .with_context(|| format!("failed to read scenario {}", scenario.display()))?;
    let yaml = yaml.replace("chip_backend_mode: stub", "chip_backend_mode: simpler_capi");
    let config = ScenarioConfig::from_yaml_str(&yaml)
        .with_context(|| format!("failed to parse scenario {}", scenario.display()))?;
    let topology = SimTopology::from_config(&config).context("failed to build topology")?;
    let report =
        run_host_vector_ub_gm_dispatch(&config, &topology, &manifest, &platform, 128 * 128)
            .map_err(map_sim_error)
            .context("P2 PTO UB_GM runtime smoke failed")?;
    validate_p2_report(&report)?;
    let envelope = LingquShmemPtoMockEnvelope {
        command: "lingqu-shmem-pto-e2e",
        implementation_phase: "p2_runtime_mock",
        manifest: manifest.display().to_string(),
        scenario: scenario.display().to_string(),
        report: &report,
    };
    println!("{}", serde_json::to_string_pretty(&envelope)?);
    Ok(())
}

fn run_p2_bridge_mock(
    manifest: PathBuf,
    platform: String,
    scenario: PathBuf,
) -> anyhow::Result<()> {
    let report = run_host_vector_ub_gm_bridge_dispatch(&scenario, &manifest, &platform, 128 * 128)
        .map_err(map_sim_error)
        .context("P2 PTO UB_GM authorized bridge smoke failed")?;
    validate_p2_report(&report)?;
    let envelope = LingquShmemPtoMockEnvelope {
        command: "lingqu-shmem-pto-e2e",
        implementation_phase: "p2_authorized_bridge_mock",
        manifest: manifest.display().to_string(),
        scenario: scenario.display().to_string(),
        report: &report,
    };
    println!("{}", serde_json::to_string_pretty(&envelope)?);
    Ok(())
}

fn validate_p2_report(report: &HostVectorUbGmDispatchReport) -> anyhow::Result<()> {
    let expected_bytes = 128 * 128 * std::mem::size_of::<f32>() as u64;
    if report.completion_status != CompletionStatus::Success
        || !report.all_match_expected
        || report.read_calls != 2
        || report.read_bytes != 2 * expected_bytes
        || report.write_calls != 1
        || report.write_bytes != expected_bytes
        || report.fence_calls != 1
        || report.segment_payload_staging_bytes != 0
    {
        anyhow::bail!(
            "P2 PTO UB_GM runtime smoke violated its acceptance contract: {}",
            serde_json::to_string(report)?
        );
    }
    Ok(())
}

fn map_sim_error(error: SimError) -> anyhow::Error {
    anyhow::anyhow!("{error:?}")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_the_p0_contract_command() {
        let args = args_from(["lingqu-shmem-pto-e2e", "--contract-only"])
            .expect("valid args")
            .expect("recognized command");
        assert_eq!(args.mode, LingquShmemPtoMode::ContractOnly);
    }

    #[test]
    fn parses_the_p2_mock_runtime_command() {
        let args = args_from([
            "lingqu-shmem-pto-e2e",
            "--mock-runtime-manifest",
            "/tmp/manifest.json",
            "--platform",
            "a5sim",
            "--scenario",
            "/tmp/scenario.yaml",
        ])
        .expect("valid args")
        .expect("recognized command");
        assert_eq!(
            args.mode,
            LingquShmemPtoMode::P2Mock {
                manifest: PathBuf::from("/tmp/manifest.json"),
                platform: "a5sim".to_string(),
                scenario: PathBuf::from("/tmp/scenario.yaml"),
            }
        );
    }

    #[test]
    fn parses_the_p2_authorized_bridge_runtime_command() {
        let args = args_from([
            "lingqu-shmem-pto-e2e",
            "--bridge-runtime-manifest",
            "/tmp/manifest.json",
            "--platform",
            "a2a3sim",
            "--scenario",
            "/tmp/scenario.yaml",
        ])
        .expect("valid args")
        .expect("recognized command");
        assert_eq!(
            args.mode,
            LingquShmemPtoMode::P2BridgeMock {
                manifest: PathBuf::from("/tmp/manifest.json"),
                platform: "a2a3sim".to_string(),
                scenario: PathBuf::from("/tmp/scenario.yaml"),
            }
        );
    }

    #[test]
    fn rejects_execution_without_a_selected_phase() {
        let error =
            args_from(["lingqu-shmem-pto-e2e"]).expect_err("execution mode must fail closed");
        assert!(error.to_string().contains("--mock-runtime-manifest"));
    }

    #[test]
    fn rejects_contract_and_execution_mode_together() {
        let error = args_from([
            "lingqu-shmem-pto-e2e",
            "--contract-only",
            "--mock-runtime-manifest",
            "/tmp/manifest.json",
        ])
        .expect_err("modes must be exclusive");
        assert!(error.to_string().contains("cannot be combined"));
    }

    #[test]
    fn rejects_direct_and_bridge_runtime_modes_together() {
        let error = args_from([
            "lingqu-shmem-pto-e2e",
            "--mock-runtime-manifest",
            "/tmp/direct.json",
            "--bridge-runtime-manifest",
            "/tmp/bridge.json",
        ])
        .expect_err("runtime modes must be exclusive");
        assert!(error.to_string().contains("mutually exclusive"));
    }
}
