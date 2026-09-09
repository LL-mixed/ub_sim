use anyhow::Context;
use serde::Serialize;
use std::ffi::OsString;
use std::path::{Path, PathBuf};
use std::process::Command;

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
    P3Fingerprint {
        manifest: PathBuf,
    },
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
    P3E2e(LingquShmemPtoE2eArgs),
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct LingquShmemPtoE2eArgs {
    manifest: PathBuf,
    scenario: PathBuf,
    nodes: u32,
    layout: String,
    elements: u32,
    evidence_dir: Option<PathBuf>,
    kernel_image: Option<PathBuf>,
    initramfs_image: Option<PathBuf>,
    run_id: Option<String>,
    runner: Option<PathBuf>,
    cna_base: u32,
    max_runtime: Option<u64>,
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

#[derive(Debug, Serialize, PartialEq, Eq)]
struct LingquShmemPtoFingerprintEnvelope {
    command: &'static str,
    implementation_phase: &'static str,
    manifest: String,
    callable_id: u64,
    artifact_fingerprint: u64,
    artifact_fingerprint_hex: String,
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
    let remaining = args.collect::<Vec<_>>();
    if remaining.iter().any(|arg| arg == "--manifest") {
        return parse_e2e_args(remaining).map(Some);
    }
    let mut args = remaining.into_iter();

    let mut contract_only = false;
    let mut fingerprint_manifest = None;
    let mut manifest = None;
    let mut bridge_manifest = None;
    let mut platform = "a2a3sim".to_string();
    let mut scenario = PathBuf::from("scenarios/mvp_2host_single_domain.yaml");
    let mut execution_option_seen = false;
    let mut p2_modifier_seen = false;
    while let Some(arg) = args.next() {
        match arg.to_string_lossy().as_ref() {
            "--contract-only" => contract_only = true,
            "--fingerprint-manifest" => {
                let value = args
                    .next()
                    .context("--fingerprint-manifest requires a path")?;
                fingerprint_manifest = Some(PathBuf::from(value));
                execution_option_seen = true;
            }
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
                p2_modifier_seen = true;
            }
            "--scenario" => {
                let value = args.next().context("--scenario requires a path")?;
                scenario = PathBuf::from(value);
                execution_option_seen = true;
                p2_modifier_seen = true;
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
    let selected_execution_modes = usize::from(fingerprint_manifest.is_some())
        + usize::from(manifest.is_some())
        + usize::from(bridge_manifest.is_some());
    if selected_execution_modes > 1 {
        anyhow::bail!(
            "--fingerprint-manifest, --mock-runtime-manifest, and --bridge-runtime-manifest are mutually exclusive"
        );
    }
    if fingerprint_manifest.is_some() && p2_modifier_seen {
        anyhow::bail!("--fingerprint-manifest cannot be combined with P2 execution options");
    }
    if !matches!(platform.as_str(), "a2a3sim" | "a5sim") {
        anyhow::bail!("--platform must be a2a3sim or a5sim");
    }
    let mode = match (fingerprint_manifest, manifest, bridge_manifest) {
        (Some(manifest), None, None) => LingquShmemPtoMode::P3Fingerprint { manifest },
        (None, Some(manifest), None) => LingquShmemPtoMode::P2Mock {
            manifest,
            platform,
            scenario,
        },
        (None, None, Some(manifest)) => LingquShmemPtoMode::P2BridgeMock {
            manifest,
            platform,
            scenario,
        },
        (None, None, None) => anyhow::bail!(
            "select --contract-only, --fingerprint-manifest, --mock-runtime-manifest, or --bridge-runtime-manifest"
        ),
        _ => unreachable!(),
    };
    Ok(Some(LingquShmemPtoArgs { mode }))
}

fn parse_e2e_args(args: Vec<OsString>) -> anyhow::Result<LingquShmemPtoArgs> {
    let mut args = args.into_iter();
    let mut manifest = None;
    let mut scenario = None;
    let mut nodes = 2u32;
    let mut layout = "nd".to_string();
    let mut elements = None;
    let mut evidence_dir = None;
    let mut kernel_image = None;
    let mut initramfs_image = None;
    let mut run_id = None;
    let mut runner = None;
    let mut cna_base = 0xf001u32;
    let mut max_runtime = None;

    while let Some(arg) = args.next() {
        match arg.to_string_lossy().as_ref() {
            "--manifest" => {
                manifest = Some(PathBuf::from(
                    args.next().context("--manifest requires a path")?,
                ));
            }
            "--scenario" => {
                scenario = Some(PathBuf::from(
                    args.next().context("--scenario requires a path")?,
                ));
            }
            "--kernel" => {
                let kernel = args.next().context("--kernel requires a value")?;
                if kernel != "vector-add" {
                    anyhow::bail!("--kernel currently supports vector-add");
                }
            }
            "--nodes" => {
                nodes = args
                    .next()
                    .context("--nodes requires a value")?
                    .to_string_lossy()
                    .parse()
                    .context("--nodes must be 2 or 8")?;
            }
            "--layout" => {
                layout = args
                    .next()
                    .context("--layout requires a value")?
                    .to_string_lossy()
                    .into_owned();
            }
            "--elements" => {
                elements = Some(
                    args.next()
                        .context("--elements requires a value")?
                        .to_string_lossy()
                        .parse()
                        .context("--elements must be a positive integer")?,
                );
            }
            "--verify" => {}
            "--evidence-dir" => {
                evidence_dir = Some(PathBuf::from(
                    args.next().context("--evidence-dir requires a path")?,
                ));
            }
            "--kernel-image" => {
                kernel_image = Some(PathBuf::from(
                    args.next().context("--kernel-image requires a path")?,
                ));
            }
            "--initramfs-image" => {
                initramfs_image = Some(PathBuf::from(
                    args.next().context("--initramfs-image requires a path")?,
                ));
            }
            "--run-id" => {
                run_id = Some(
                    args.next()
                        .context("--run-id requires a value")?
                        .to_string_lossy()
                        .into_owned(),
                );
            }
            "--runner" => {
                runner = Some(PathBuf::from(
                    args.next().context("--runner requires a path")?,
                ));
            }
            "--cna-base" => {
                let value = args
                    .next()
                    .context("--cna-base requires a value")?
                    .to_string_lossy()
                    .into_owned();
                cna_base = parse_u32(&value).context("--cna-base must be a 24-bit CNA")?;
            }
            "--max-runtime" => {
                max_runtime = Some(
                    args.next()
                        .context("--max-runtime requires a value")?
                        .to_string_lossy()
                        .parse()
                        .context("--max-runtime must be a positive integer")?,
                );
            }
            option => anyhow::bail!("unknown lingqu-shmem-pto-e2e option: {option}"),
        }
    }
    let manifest = manifest.context("--manifest is required")?;
    if !matches!(nodes, 2 | 8) {
        anyhow::bail!("--nodes must be 2 or 8");
    }
    if nodes == 8 && layout != "nd" {
        anyhow::bail!("the eight-node functional demo currently requires --layout nd");
    }
    let expected_elements = match layout.as_str() {
        "nd" => 16_384,
        "tail" => 16_256,
        "cross-page" | "unaligned" => 64,
        "nd-strided" | "dn" => 15,
        "nz" => 128,
        _ => anyhow::bail!("unsupported PTO UB_GM layout: {layout}"),
    };
    let elements = elements.unwrap_or(expected_elements);
    if elements != expected_elements {
        anyhow::bail!("layout {layout} requires {expected_elements} elements, received {elements}");
    }
    if cna_base == 0 || cna_base.saturating_add(nodes - 1) > 0x00ff_ffff {
        anyhow::bail!("--cna-base must leave one valid 24-bit CNA per node");
    }
    if max_runtime == Some(0) {
        anyhow::bail!("--max-runtime must be a positive integer");
    }
    let scenario = scenario.unwrap_or_else(|| {
        PathBuf::from(if nodes == 8 {
            "scenarios/mvp_8host_single_domain.yaml"
        } else {
            "scenarios/mvp_2host_single_domain.yaml"
        })
    });
    Ok(LingquShmemPtoArgs {
        mode: LingquShmemPtoMode::P3E2e(LingquShmemPtoE2eArgs {
            manifest,
            scenario,
            nodes,
            layout,
            elements,
            evidence_dir,
            kernel_image,
            initramfs_image,
            run_id,
            runner,
            cna_base,
            max_runtime,
        }),
    })
}

fn parse_u32(value: &str) -> anyhow::Result<u32> {
    if let Some(hex) = value
        .strip_prefix("0x")
        .or_else(|| value.strip_prefix("0X"))
    {
        Ok(u32::from_str_radix(hex, 16)?)
    } else {
        Ok(value.parse()?)
    }
}

pub fn run(args: LingquShmemPtoArgs) -> anyhow::Result<()> {
    match args.mode {
        LingquShmemPtoMode::ContractOnly => run_contract(),
        LingquShmemPtoMode::P3Fingerprint { manifest } => run_p3_fingerprint(manifest),
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
        LingquShmemPtoMode::P3E2e(args) => run_p3_e2e(args),
    }
}

fn run_p3_e2e(args: LingquShmemPtoE2eArgs) -> anyhow::Result<()> {
    let runner = args
        .runner
        .unwrap_or_else(|| default_e2e_runner(args.nodes));
    let mut command = Command::new(&runner);

    command
        .arg("--manifest")
        .arg(&args.manifest)
        .arg("--scenario")
        .arg(&args.scenario)
        .arg("--layout")
        .arg(&args.layout)
        .arg("--elements")
        .arg(args.elements.to_string());
    if let Ok(sim_cli_bin) = std::env::current_exe() {
        command.arg("--sim-cli-bin").arg(sim_cli_bin);
    }
    if args.nodes == 8 {
        command
            .arg("--cna-base")
            .arg(format!("0x{:x}", args.cna_base));
    } else {
        command
            .arg("--nodea-cna")
            .arg(format!("0x{:x}", args.cna_base))
            .arg("--nodeb-cna")
            .arg(format!("0x{:x}", args.cna_base + 1));
    }
    if let Some(path) = args.evidence_dir {
        command.arg("--evidence-dir").arg(path);
    }
    if let Some(path) = args.kernel_image {
        command.arg("--kernel-image").arg(path);
    }
    if let Some(path) = args.initramfs_image {
        command.arg("--initramfs-image").arg(path);
    }
    if let Some(run_id) = args.run_id {
        command.arg("--run-id").arg(run_id);
    }
    if let Some(max_runtime) = args.max_runtime {
        command.arg("--max-runtime").arg(max_runtime.to_string());
    }
    let status = command
        .status()
        .with_context(|| format!("failed to run PTO UB_GM E2E runner {}", runner.display()))?;
    if !status.success() {
        anyhow::bail!("PTO UB_GM E2E runner failed with {status}");
    }
    Ok(())
}

fn default_e2e_runner(nodes: u32) -> PathBuf {
    let workspace = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(Path::parent)
        .expect("sim-cli must live below the workspace root");
    let script = if nodes == 8 {
        "run_ub_eight_node_lingqu_shmem_pto_direct.sh"
    } else {
        "run_ub_dual_node_lingqu_shmem_pto_direct.sh"
    };
    workspace.join("guest-linux/aarch64/scripts").join(script)
}

fn run_p3_fingerprint(manifest: PathBuf) -> anyhow::Result<()> {
    let manifest = std::fs::canonicalize(&manifest)
        .with_context(|| format!("failed to resolve manifest {}", manifest.display()))?;
    let (callable_id, artifact_fingerprint) = sim_uapi::pto_ub_gm_callable_from_manifest(&manifest)
        .map_err(anyhow::Error::msg)
        .context("failed to fingerprint PTO UB_GM callable artifacts")?;
    let envelope = LingquShmemPtoFingerprintEnvelope {
        command: "lingqu-shmem-pto-e2e",
        implementation_phase: "p3_guest_runtime",
        manifest: manifest.display().to_string(),
        callable_id,
        artifact_fingerprint,
        artifact_fingerprint_hex: format!("0x{artifact_fingerprint:016x}"),
    };
    println!("{}", serde_json::to_string_pretty(&envelope)?);
    Ok(())
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
    fn parses_the_p3_fingerprint_command() {
        let args = args_from([
            "lingqu-shmem-pto-e2e",
            "--fingerprint-manifest",
            "/tmp/manifest.json",
        ])
        .expect("valid args")
        .expect("recognized command");
        assert_eq!(
            args.mode,
            LingquShmemPtoMode::P3Fingerprint {
                manifest: PathBuf::from("/tmp/manifest.json"),
            }
        );
    }

    #[test]
    fn parses_the_eight_node_e2e_command() {
        let args = args_from([
            "lingqu-shmem-pto-e2e",
            "--manifest",
            "/tmp/manifest.json",
            "--nodes",
            "8",
            "--kernel",
            "vector-add",
            "--layout",
            "nd",
            "--elements",
            "16384",
            "--verify",
            "--evidence-dir",
            "/tmp/evidence",
            "--cna-base",
            "0xf001",
        ])
        .expect("valid args")
        .expect("recognized command");
        assert_eq!(
            args.mode,
            LingquShmemPtoMode::P3E2e(LingquShmemPtoE2eArgs {
                manifest: PathBuf::from("/tmp/manifest.json"),
                scenario: PathBuf::from("scenarios/mvp_8host_single_domain.yaml"),
                nodes: 8,
                layout: "nd".to_string(),
                elements: 16_384,
                evidence_dir: Some(PathBuf::from("/tmp/evidence")),
                kernel_image: None,
                initramfs_image: None,
                run_id: None,
                runner: None,
                cna_base: 0xf001,
                max_runtime: None,
            })
        );
    }

    #[test]
    fn rejects_non_nd_eight_node_e2e_layout() {
        let error = args_from([
            "lingqu-shmem-pto-e2e",
            "--manifest",
            "/tmp/manifest.json",
            "--nodes",
            "8",
            "--layout",
            "tail",
        ])
        .expect_err("eight-node layout must fail closed");
        assert!(error.to_string().contains("requires --layout nd"));
    }

    #[test]
    fn executes_the_selected_e2e_runner() {
        let args = args_from([
            "lingqu-shmem-pto-e2e",
            "--manifest",
            "/tmp/manifest.json",
            "--nodes",
            "8",
            "--runner",
            "/usr/bin/true",
        ])
        .expect("valid args")
        .expect("recognized command");

        run(args).expect("runner succeeds");
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

    #[test]
    fn rejects_fingerprint_and_p2_runtime_modes_together() {
        let error = args_from([
            "lingqu-shmem-pto-e2e",
            "--fingerprint-manifest",
            "/tmp/fingerprint.json",
            "--bridge-runtime-manifest",
            "/tmp/bridge.json",
        ])
        .expect_err("runtime modes must be exclusive");
        assert!(error.to_string().contains("mutually exclusive"));
    }

    #[test]
    fn rejects_fingerprint_and_p2_modifiers_together() {
        let error = args_from([
            "lingqu-shmem-pto-e2e",
            "--fingerprint-manifest",
            "/tmp/fingerprint.json",
            "--platform",
            "a5sim",
        ])
        .expect_err("fingerprint query must not accept P2 modifiers");
        assert!(error.to_string().contains("cannot be combined"));
    }
}
