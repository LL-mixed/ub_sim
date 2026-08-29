use serde::Serialize;
use std::ffi::OsString;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LingquShmemPtoArgs {
    contract_only: bool,
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
    for arg in args {
        match arg.to_string_lossy().as_ref() {
            "--contract-only" => contract_only = true,
            option => anyhow::bail!("unknown lingqu-shmem-pto-e2e option: {option}"),
        }
    }
    if !contract_only {
        anyhow::bail!(
            "P0 exposes only --contract-only; the executable two-node runner arrives in P3"
        );
    }
    Ok(Some(LingquShmemPtoArgs { contract_only }))
}

pub fn run(args: LingquShmemPtoArgs) -> anyhow::Result<()> {
    debug_assert!(args.contract_only);
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_the_p0_contract_command() {
        let args = args_from(["lingqu-shmem-pto-e2e", "--contract-only"])
            .expect("valid args")
            .expect("recognized command");
        assert!(args.contract_only);
    }

    #[test]
    fn rejects_execution_before_the_runner_exists() {
        let error = args_from(["lingqu-shmem-pto-e2e"]).expect_err("P0 runner must fail closed");
        assert!(error.to_string().contains("--contract-only"));
    }
}
