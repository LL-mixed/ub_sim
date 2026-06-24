use anyhow::Context;
use serde::Serialize;
use std::env;
use std::fs;
use std::hint::black_box;
use std::path::PathBuf;
use std::time::{Duration, Instant};

const DEFAULT_SIZE: usize = 2 * 1024 * 1024;
const DEFAULT_ITERATIONS: u64 = 1_048_576;
const DEFAULT_CHUNK_SIZE: usize = 64;
const DEFAULT_WARMUP: u64 = 4096;
const DEFAULT_LEGACY_MAP_COUNT: usize = 64;
const DEFAULT_LOCAL_PA_BASE: u64 = 0x1000_0000;
const DEFAULT_REMOTE_UBA_BASE: u64 = 0x7000_0000_0000;
const DEFAULT_GVA_BASE: u64 = 0x5000_0000_0000;
const DEFAULT_GSVA_BASE: u64 = 0x7000_0000_0000;
const DEFAULT_TOKEN: u64 = 0x4450_4d42_4753_5641;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DataplaneMicrobenchArgs {
    pub size: usize,
    pub iterations: u64,
    pub chunk_size: usize,
    pub warmup_iterations: u64,
    pub legacy_map_count: usize,
    pub modes: Vec<DataplaneMode>,
    pub verify: bool,
    pub json_output: Option<PathBuf>,
    pub help: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "kebab-case")]
pub enum DataplaneMode {
    #[serde(rename = "legacy-pa-linear")]
    LegacyPaLinear,
    LegacyPaDirect,
    LegacyPaIndexed,
    LegacyPaCached,
    GenericGva,
    Gsva,
}

#[derive(Clone, Debug, Serialize)]
struct DataplaneCaseReport {
    mode: DataplaneMode,
    name: String,
    setup_ns: u64,
    mixed_ns: u64,
    resolve_only_ns: u64,
    copy_only_ns: u64,
    operations: u64,
    iterations: u64,
    read_bytes: u64,
    write_bytes: u64,
    verify_failures: u64,
    checksum: u64,
    mixed_ns_per_op: f64,
    resolve_only_ns_per_op: f64,
    copy_only_ns_per_op: f64,
    speedup_vs_legacy_pa_linear: Option<f64>,
}

#[derive(Clone, Debug, Serialize)]
struct DataplaneMicrobenchReport {
    status: String,
    scope: String,
    size: usize,
    iterations: u64,
    chunk_size: usize,
    warmup_iterations: u64,
    legacy_map_count: usize,
    cases: Vec<DataplaneCaseReport>,
}

#[derive(Clone, Debug)]
struct LegacyMapEntry {
    local_pa_start: u64,
    remote_uba_start: u64,
    bytes: usize,
    token: u64,
}

#[derive(Clone, Debug)]
struct LegacyPaResolver {
    entries: Vec<LegacyMapEntry>,
    segment_bytes: usize,
    cached_entry_index: Option<usize>,
}

#[derive(Clone, Copy, Debug)]
struct DirectWindowResolver {
    access_base: u64,
    remote_uba_base: u64,
    bytes: usize,
    pte_offset: u64,
    token: u64,
}

#[derive(Clone, Copy, Debug)]
struct ResolvedAccess {
    remote_offset: usize,
    remote_uba: u64,
    token: u64,
}

#[derive(Clone, Debug, Default)]
struct WorkloadStats {
    read_bytes: u64,
    write_bytes: u64,
    verify_failures: u64,
    checksum: u64,
}

pub fn args_from_env() -> anyhow::Result<Option<DataplaneMicrobenchArgs>> {
    let args = env::args().skip(1).collect::<Vec<_>>();
    args_from_slice(&args)
}

fn args_from_slice(args: &[String]) -> anyhow::Result<Option<DataplaneMicrobenchArgs>> {
    let Some(first) = args.first() else {
        return Ok(None);
    };
    if first != "dataplane-microbench" {
        return Ok(None);
    }
    parse_args(&args[1..]).map(Some)
}

fn parse_args(args: &[String]) -> anyhow::Result<DataplaneMicrobenchArgs> {
    let mut parsed = DataplaneMicrobenchArgs {
        size: DEFAULT_SIZE,
        iterations: DEFAULT_ITERATIONS,
        chunk_size: DEFAULT_CHUNK_SIZE,
        warmup_iterations: DEFAULT_WARMUP,
        legacy_map_count: DEFAULT_LEGACY_MAP_COUNT,
        modes: vec![
            DataplaneMode::LegacyPaLinear,
            DataplaneMode::LegacyPaDirect,
            DataplaneMode::LegacyPaIndexed,
            DataplaneMode::LegacyPaCached,
            DataplaneMode::GenericGva,
            DataplaneMode::Gsva,
        ],
        verify: false,
        json_output: None,
        help: false,
    };
    let mut index = 0usize;
    while index < args.len() {
        match args[index].as_str() {
            "-h" | "--help" => {
                parsed.help = true;
                index += 1;
            }
            "--size" => {
                parsed.size = parse_usize_arg(args, &mut index, "--size")?;
            }
            "--iterations" | "--iters" => {
                parsed.iterations = parse_u64_arg(args, &mut index, "--iterations")?;
            }
            "--chunk-size" => {
                parsed.chunk_size = parse_usize_arg(args, &mut index, "--chunk-size")?;
            }
            "--warmup" | "--warmup-iterations" => {
                parsed.warmup_iterations = parse_u64_arg(args, &mut index, "--warmup")?;
            }
            "--legacy-map-count" => {
                parsed.legacy_map_count = parse_usize_arg(args, &mut index, "--legacy-map-count")?;
            }
            "--modes" => {
                let value = required_next(args, &mut index, "--modes")?;
                parsed.modes = parse_modes(value)?;
            }
            "--mode" => {
                let value = required_next(args, &mut index, "--mode")?;
                parsed.modes = vec![parse_mode(value)?];
            }
            "--verify" => {
                parsed.verify = true;
                index += 1;
            }
            "--json" => {
                let value = required_next(args, &mut index, "--json")?;
                parsed.json_output = Some(PathBuf::from(value));
            }
            other => anyhow::bail!("unknown dataplane-microbench option `{other}`"),
        }
    }
    if !parsed.help {
        validate_args(&parsed)?;
    }
    Ok(parsed)
}

fn parse_usize_arg(
    args: &[String],
    index: &mut usize,
    name: &'static str,
) -> anyhow::Result<usize> {
    let value = required_next(args, index, name)?;
    parse_usize_text(value).with_context(|| format!("parse {name}"))
}

fn parse_u64_arg(args: &[String], index: &mut usize, name: &'static str) -> anyhow::Result<u64> {
    let value = required_next(args, index, name)?;
    parse_u64_text(value).with_context(|| format!("parse {name}"))
}

fn required_next<'a>(
    args: &'a [String],
    index: &mut usize,
    name: &'static str,
) -> anyhow::Result<&'a str> {
    let value = args
        .get(*index + 1)
        .ok_or_else(|| anyhow::anyhow!("{name} requires a value"))?;
    *index += 2;
    Ok(value)
}

fn parse_usize_text(value: &str) -> anyhow::Result<usize> {
    let parsed = parse_u64_text(value)?;
    usize::try_from(parsed).map_err(|_| anyhow::anyhow!("value exceeds usize: {value}"))
}

fn parse_u64_text(value: &str) -> anyhow::Result<u64> {
    if let Some(hex) = value
        .strip_prefix("0x")
        .or_else(|| value.strip_prefix("0X"))
    {
        u64::from_str_radix(hex, 16).with_context(|| format!("parse hex integer `{value}`"))
    } else {
        value
            .parse::<u64>()
            .with_context(|| format!("parse integer `{value}`"))
    }
}

fn parse_modes(value: &str) -> anyhow::Result<Vec<DataplaneMode>> {
    let mut modes = Vec::new();
    for item in value.split(',') {
        let item = item.trim();
        if item.is_empty() {
            continue;
        }
        let mode = parse_mode(item)?;
        if !modes.contains(&mode) {
            modes.push(mode);
        }
    }
    if modes.is_empty() {
        anyhow::bail!("--modes must select at least one mode");
    }
    Ok(modes)
}

fn parse_mode(value: &str) -> anyhow::Result<DataplaneMode> {
    match value {
        "legacy-pa" | "legacy" | "legacy-pa-linear" | "legacy-linear" => {
            Ok(DataplaneMode::LegacyPaLinear)
        }
        "legacy-pa-direct" | "legacy-direct" => Ok(DataplaneMode::LegacyPaDirect),
        "legacy-pa-indexed" | "legacy-indexed" => Ok(DataplaneMode::LegacyPaIndexed),
        "legacy-pa-cached" | "legacy-cached" => Ok(DataplaneMode::LegacyPaCached),
        "generic-gva" | "generic" | "gva" => Ok(DataplaneMode::GenericGva),
        "gsva" => Ok(DataplaneMode::Gsva),
        other => anyhow::bail!("unsupported dataplane mode `{other}`"),
    }
}

fn validate_args(args: &DataplaneMicrobenchArgs) -> anyhow::Result<()> {
    if args.size == 0 {
        anyhow::bail!("--size must be positive");
    }
    if args.iterations == 0 {
        anyhow::bail!("--iterations must be positive");
    }
    if args.chunk_size == 0 || args.chunk_size > args.size {
        anyhow::bail!("--chunk-size must be positive and no larger than --size");
    }
    if args.size % args.chunk_size != 0 {
        anyhow::bail!("--size must be divisible by --chunk-size");
    }
    if args.legacy_map_count == 0 {
        anyhow::bail!("--legacy-map-count must be positive");
    }
    if args.legacy_map_count > args.size {
        anyhow::bail!("--legacy-map-count must not exceed --size");
    }
    if args.size % args.legacy_map_count != 0 {
        anyhow::bail!("--size must be divisible by --legacy-map-count");
    }
    if (args.size / args.legacy_map_count) % args.chunk_size != 0 {
        anyhow::bail!("legacy map segment size must be divisible by --chunk-size");
    }
    if args.modes.is_empty() {
        anyhow::bail!("at least one dataplane mode must be selected");
    }
    Ok(())
}

pub fn run_cli(args: &DataplaneMicrobenchArgs) -> anyhow::Result<()> {
    if args.help {
        print_usage();
        return Ok(());
    }
    let report = run_report(args)?;
    print_report(&report);
    if let Some(path) = &args.json_output {
        let bytes = serde_json::to_vec_pretty(&report).context("encode dataplane report json")?;
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)
                .with_context(|| format!("create dataplane report dir {}", parent.display()))?;
        }
        fs::write(path, bytes)
            .with_context(|| format!("write dataplane report {}", path.display()))?;
    }
    Ok(())
}

fn print_usage() {
    println!(
        "usage: sim-cli dataplane-microbench [--size BYTES] [--iterations N] [--chunk-size BYTES] [--legacy-map-count N] [--modes legacy-pa-linear,legacy-pa-direct,legacy-pa-indexed,legacy-pa-cached,generic-gva,gsva] [--verify] [--json PATH]"
    );
}

fn run_report(args: &DataplaneMicrobenchArgs) -> anyhow::Result<DataplaneMicrobenchReport> {
    validate_args(args)?;
    let mut cases = Vec::new();
    for mode in &args.modes {
        cases.push(run_case(args, *mode)?);
    }
    let legacy_ns_per_op = cases
        .iter()
        .find(|case| case.mode == DataplaneMode::LegacyPaLinear)
        .map(|case| case.mixed_ns_per_op);
    if let Some(legacy) = legacy_ns_per_op {
        for case in &mut cases {
            if case.mode != DataplaneMode::LegacyPaLinear {
                case.speedup_vs_legacy_pa_linear = Some(legacy / case.mixed_ns_per_op);
            }
        }
    }
    Ok(DataplaneMicrobenchReport {
        status: "pass".to_string(),
        scope: "host-core-data-plane qemu=excluded guest_harness=excluded ioctl=excluded scheduler=excluded".to_string(),
        size: args.size,
        iterations: args.iterations,
        chunk_size: args.chunk_size,
        warmup_iterations: args.warmup_iterations,
        legacy_map_count: args.legacy_map_count,
        cases,
    })
}

fn run_case(
    args: &DataplaneMicrobenchArgs,
    mode: DataplaneMode,
) -> anyhow::Result<DataplaneCaseReport> {
    let setup_start = Instant::now();
    let mut state = BenchState::new(args, mode)?;
    let setup_ns = elapsed_ns(setup_start.elapsed());
    if args.warmup_iterations > 0 {
        let _ = state.run_mixed(args.warmup_iterations, args.verify)?;
    }
    state.reset_payloads();

    let mixed_start = Instant::now();
    let stats = state.run_mixed(args.iterations, args.verify)?;
    let mixed_ns = elapsed_ns(mixed_start.elapsed());

    let resolve_start = Instant::now();
    let resolve_checksum = state.run_resolve_only(args.iterations)?;
    let resolve_only_ns = elapsed_ns(resolve_start.elapsed());

    let copy_start = Instant::now();
    let copy_checksum = state.run_copy_only(args.iterations)?;
    let copy_only_ns = elapsed_ns(copy_start.elapsed());

    let operations = args
        .iterations
        .checked_mul(2)
        .ok_or_else(|| anyhow::anyhow!("operation count overflow"))?;
    let read_bytes = args
        .iterations
        .checked_mul(args.chunk_size as u64)
        .ok_or_else(|| anyhow::anyhow!("read byte count overflow"))?;
    let write_bytes = read_bytes;
    let checksum = stats.checksum ^ resolve_checksum.rotate_left(7) ^ copy_checksum.rotate_left(17);

    Ok(DataplaneCaseReport {
        mode,
        name: mode.name().to_string(),
        setup_ns,
        mixed_ns,
        resolve_only_ns,
        copy_only_ns,
        operations,
        iterations: args.iterations,
        read_bytes,
        write_bytes,
        verify_failures: stats.verify_failures,
        checksum,
        mixed_ns_per_op: ns_per_op(mixed_ns, operations),
        resolve_only_ns_per_op: ns_per_op(resolve_only_ns, operations),
        copy_only_ns_per_op: ns_per_op(copy_only_ns, operations),
        speedup_vs_legacy_pa_linear: None,
    })
}

fn print_report(report: &DataplaneMicrobenchReport) {
    println!(
        "dataplane_microbench: status={} scope=\"{}\" size={} iterations={} chunk_size={} warmup_iterations={} legacy_map_count={}",
        report.status,
        report.scope,
        report.size,
        report.iterations,
        report.chunk_size,
        report.warmup_iterations,
        report.legacy_map_count
    );
    for case in &report.cases {
        println!(
            "dataplane_case: name={} operations={} mixed_ns={} mixed_ns_per_op={:.3} resolve_only_ns={} resolve_ns_per_op={:.3} copy_only_ns={} copy_ns_per_op={:.3} setup_ns={} read_bytes={} write_bytes={} verify_failures={} checksum={:#x}",
            case.name,
            case.operations,
            case.mixed_ns,
            case.mixed_ns_per_op,
            case.resolve_only_ns,
            case.resolve_only_ns_per_op,
            case.copy_only_ns,
            case.copy_only_ns_per_op,
            case.setup_ns,
            case.read_bytes,
            case.write_bytes,
            case.verify_failures,
            case.checksum
        );
    }
    for case in &report.cases {
        if let Some(speedup) = case.speedup_vs_legacy_pa_linear {
            println!(
                "dataplane_delta: case={} baseline=legacy-pa-linear mixed_speedup={:.3}",
                case.name, speedup
            );
        }
    }
}

fn elapsed_ns(duration: Duration) -> u64 {
    duration.as_nanos().min(u128::from(u64::MAX)) as u64
}

fn ns_per_op(ns: u64, operations: u64) -> f64 {
    ns as f64 / operations.max(1) as f64
}

impl DataplaneMode {
    fn name(self) -> &'static str {
        match self {
            DataplaneMode::LegacyPaLinear => "legacy-pa-linear",
            DataplaneMode::LegacyPaDirect => "legacy-pa-direct",
            DataplaneMode::LegacyPaIndexed => "legacy-pa-indexed",
            DataplaneMode::LegacyPaCached => "legacy-pa-cached",
            DataplaneMode::GenericGva => "generic-gva",
            DataplaneMode::Gsva => "gsva",
        }
    }
}

impl LegacyPaResolver {
    fn new(size: usize, map_count: usize) -> anyhow::Result<Self> {
        let segment_bytes = size / map_count;
        let mut entries = Vec::with_capacity(map_count);
        for index in 0..map_count {
            let offset = index
                .checked_mul(segment_bytes)
                .ok_or_else(|| anyhow::anyhow!("legacy segment offset overflow"))?;
            entries.push(LegacyMapEntry {
                local_pa_start: DEFAULT_LOCAL_PA_BASE + offset as u64,
                remote_uba_start: DEFAULT_REMOTE_UBA_BASE + offset as u64,
                bytes: segment_bytes,
                token: DEFAULT_TOKEN ^ (index as u64).wrapping_mul(0x9e37_79b9_7f4a_7c15),
            });
        }
        Ok(Self {
            entries,
            segment_bytes,
            cached_entry_index: None,
        })
    }

    fn resolve_linear(&self, local_pa: u64, len: usize) -> anyhow::Result<ResolvedAccess> {
        let local_pa = black_box(local_pa);
        let len_u64 = len as u64;
        for index in 0..self.entries.len() {
            if !self.entry_contains(index, local_pa, len_u64) {
                continue;
            }
            return self.resolve_entry(index, local_pa, len);
        }
        anyhow::bail!("legacy PA resolve miss pa={local_pa:#x} len={len}")
    }

    fn resolve_direct(&self, local_pa: u64, len: usize) -> anyhow::Result<ResolvedAccess> {
        let local_pa = black_box(local_pa);
        let len_u64 = len as u64;
        let first = self
            .entries
            .first()
            .ok_or_else(|| anyhow::anyhow!("legacy direct resolver has no entries"))?;
        let total_bytes = self.total_bytes() as u64;
        if local_pa < DEFAULT_LOCAL_PA_BASE
            || local_pa + len_u64 > DEFAULT_LOCAL_PA_BASE + total_bytes
        {
            anyhow::bail!("legacy direct PA resolve miss pa={local_pa:#x} len={len}");
        }
        let remote_offset = (local_pa - DEFAULT_LOCAL_PA_BASE) as usize;
        let token = validate_legacy_token(first.token, local_pa, len);
        Ok(ResolvedAccess {
            remote_offset,
            remote_uba: DEFAULT_REMOTE_UBA_BASE + remote_offset as u64,
            token,
        })
    }

    fn resolve_indexed(&self, local_pa: u64, len: usize) -> anyhow::Result<ResolvedAccess> {
        let local_pa = black_box(local_pa);
        let index = self.index_for_pa(local_pa, len)?;
        self.resolve_entry(index, local_pa, len)
    }

    fn resolve_cached(&mut self, local_pa: u64, len: usize) -> anyhow::Result<ResolvedAccess> {
        let local_pa = black_box(local_pa);
        let len_u64 = len as u64;
        if let Some(index) = self.cached_entry_index {
            if self.entry_contains(index, local_pa, len_u64) {
                return self.resolve_entry(index, local_pa, len);
            }
        }
        let index = self.index_for_pa(local_pa, len)?;
        self.cached_entry_index = Some(index);
        self.resolve_entry(index, local_pa, len)
    }

    fn entry_contains(&self, index: usize, local_pa: u64, len_u64: u64) -> bool {
        let Some(entry) = self.entries.get(index) else {
            return false;
        };
        let start = entry.local_pa_start;
        let end = start + entry.bytes as u64;
        local_pa >= start && local_pa + len_u64 <= end
    }

    fn index_for_pa(&self, local_pa: u64, len: usize) -> anyhow::Result<usize> {
        let len_u64 = len as u64;
        let total_bytes = self.total_bytes() as u64;
        if local_pa < DEFAULT_LOCAL_PA_BASE
            || local_pa + len_u64 > DEFAULT_LOCAL_PA_BASE + total_bytes
        {
            anyhow::bail!("legacy indexed PA resolve miss pa={local_pa:#x} len={len}");
        }
        let offset = (local_pa - DEFAULT_LOCAL_PA_BASE) as usize;
        let index = offset / self.segment_bytes;
        if !self.entry_contains(index, local_pa, len_u64) {
            anyhow::bail!("legacy indexed PA resolve crosses segment pa={local_pa:#x} len={len}");
        }
        Ok(index)
    }

    fn resolve_entry(
        &self,
        index: usize,
        local_pa: u64,
        len: usize,
    ) -> anyhow::Result<ResolvedAccess> {
        let entry = self
            .entries
            .get(index)
            .ok_or_else(|| anyhow::anyhow!("legacy PA entry index out of range: {index}"))?;
        let remote_offset = (local_pa - entry.local_pa_start) as usize
            + (entry.remote_uba_start - DEFAULT_REMOTE_UBA_BASE) as usize;
        let token = validate_legacy_token(entry.token, local_pa, len);
        Ok(ResolvedAccess {
            remote_offset,
            remote_uba: entry.remote_uba_start + (local_pa - entry.local_pa_start),
            token,
        })
    }

    fn total_bytes(&self) -> usize {
        self.segment_bytes * self.entries.len()
    }
}

impl DirectWindowResolver {
    fn generic_gva(size: usize) -> Self {
        Self {
            access_base: DEFAULT_GVA_BASE,
            remote_uba_base: DEFAULT_REMOTE_UBA_BASE,
            bytes: size,
            pte_offset: DEFAULT_REMOTE_UBA_BASE - DEFAULT_GVA_BASE,
            token: DEFAULT_TOKEN ^ 0x4756_4120_4449_5245,
        }
    }

    fn gsva(size: usize) -> Self {
        Self {
            access_base: DEFAULT_GSVA_BASE,
            remote_uba_base: DEFAULT_REMOTE_UBA_BASE,
            bytes: size,
            pte_offset: 0,
            token: DEFAULT_TOKEN ^ 0x4753_5641_2049_4445,
        }
    }

    fn resolve_generic(&self, access_va: u64, len: usize) -> anyhow::Result<ResolvedAccess> {
        let access_va = black_box(access_va);
        let remote_uba = access_va
            .checked_add(self.pte_offset)
            .ok_or_else(|| anyhow::anyhow!("generic GVA pte offset overflow"))?;
        self.resolve_remote_uba(remote_uba, len, access_va)
    }

    fn resolve_gsva(&self, access_va: u64, len: usize) -> anyhow::Result<ResolvedAccess> {
        let access_va = black_box(access_va);
        self.resolve_remote_uba(access_va, len, access_va)
    }

    fn resolve_remote_uba(
        &self,
        remote_uba: u64,
        len: usize,
        access_va: u64,
    ) -> anyhow::Result<ResolvedAccess> {
        let access_start = self.access_base;
        let access_end = access_start + self.bytes as u64;
        let len_u64 = len as u64;
        if access_va < access_start || access_va + len_u64 > access_end {
            anyhow::bail!("direct window access outside aperture va={access_va:#x} len={len}");
        }
        let remote_start = self.remote_uba_base;
        let remote_end = remote_start + self.bytes as u64;
        if remote_uba < remote_start || remote_uba + len_u64 > remote_end {
            anyhow::bail!("direct window resolve miss va={access_va:#x} len={len}");
        }
        let remote_offset = (remote_uba - remote_start) as usize;
        let token = self.token ^ access_va.rotate_left(13) ^ len as u64;
        Ok(ResolvedAccess {
            remote_offset,
            remote_uba,
            token,
        })
    }
}

fn validate_legacy_token(token: u64, local_pa: u64, len: usize) -> u64 {
    token ^ local_pa.rotate_left(17) ^ (len as u64).rotate_left(3)
}

struct BenchState {
    mode: DataplaneMode,
    size: usize,
    chunk_size: usize,
    remote: Vec<u8>,
    write_payload: Vec<u8>,
    read_payload: Vec<u8>,
    legacy: LegacyPaResolver,
    generic: DirectWindowResolver,
    gsva: DirectWindowResolver,
}

impl BenchState {
    fn new(args: &DataplaneMicrobenchArgs, mode: DataplaneMode) -> anyhow::Result<Self> {
        let mut state = Self {
            mode,
            size: args.size,
            chunk_size: args.chunk_size,
            remote: vec![0; args.size],
            write_payload: vec![0; args.chunk_size],
            read_payload: vec![0; args.chunk_size],
            legacy: LegacyPaResolver::new(args.size, args.legacy_map_count)?,
            generic: DirectWindowResolver::generic_gva(args.size),
            gsva: DirectWindowResolver::gsva(args.size),
        };
        fill_payload(&mut state.write_payload, 0x42);
        state.reset_payloads();
        Ok(state)
    }

    fn reset_payloads(&mut self) {
        fill_payload(&mut self.remote, 0x11);
        fill_payload(&mut self.read_payload, 0x22);
    }

    fn run_mixed(&mut self, iterations: u64, verify: bool) -> anyhow::Result<WorkloadStats> {
        let mut stats = WorkloadStats::default();
        for iter in 0..iterations {
            let offset = self.offset_for_iter(iter);
            let write_access = self.resolve(offset)?;
            let write_end = write_access
                .remote_offset
                .checked_add(self.chunk_size)
                .ok_or_else(|| anyhow::anyhow!("write range overflow"))?;
            self.remote[write_access.remote_offset..write_end].copy_from_slice(&self.write_payload);
            stats.write_bytes += self.chunk_size as u64;

            let read_access = self.resolve(offset)?;
            let read_end = read_access
                .remote_offset
                .checked_add(self.chunk_size)
                .ok_or_else(|| anyhow::anyhow!("read range overflow"))?;
            self.read_payload
                .copy_from_slice(&self.remote[read_access.remote_offset..read_end]);
            stats.read_bytes += self.chunk_size as u64;
            if verify && self.read_payload != self.write_payload {
                stats.verify_failures += 1;
            }
            stats.checksum = fold_checksum(
                stats.checksum,
                read_access.remote_uba
                    ^ write_access.token
                    ^ self.read_payload[(iter as usize) % self.chunk_size] as u64,
            );
        }
        black_box(stats.checksum);
        Ok(stats)
    }

    fn run_resolve_only(&mut self, iterations: u64) -> anyhow::Result<u64> {
        let mut checksum = 0u64;
        for iter in 0..iterations {
            let offset = self.offset_for_iter(iter);
            let write_access = self.resolve(offset)?;
            let read_access = self.resolve(offset)?;
            checksum = fold_checksum(
                checksum,
                write_access.remote_uba ^ read_access.token ^ write_access.remote_offset as u64,
            );
        }
        Ok(black_box(checksum))
    }

    fn run_copy_only(&mut self, iterations: u64) -> anyhow::Result<u64> {
        let mut checksum = 0u64;
        for iter in 0..iterations {
            let offset = self.offset_for_iter(iter);
            let end = offset
                .checked_add(self.chunk_size)
                .ok_or_else(|| anyhow::anyhow!("copy range overflow"))?;
            self.remote[offset..end].copy_from_slice(&self.write_payload);
            self.read_payload.copy_from_slice(&self.remote[offset..end]);
            checksum = fold_checksum(
                checksum,
                self.read_payload[(iter as usize) % self.chunk_size] as u64 ^ offset as u64,
            );
        }
        Ok(black_box(checksum))
    }

    fn resolve(&mut self, offset: usize) -> anyhow::Result<ResolvedAccess> {
        match self.mode {
            DataplaneMode::LegacyPaLinear => self
                .legacy
                .resolve_linear(DEFAULT_LOCAL_PA_BASE + offset as u64, self.chunk_size),
            DataplaneMode::LegacyPaDirect => self
                .legacy
                .resolve_direct(DEFAULT_LOCAL_PA_BASE + offset as u64, self.chunk_size),
            DataplaneMode::LegacyPaIndexed => self
                .legacy
                .resolve_indexed(DEFAULT_LOCAL_PA_BASE + offset as u64, self.chunk_size),
            DataplaneMode::LegacyPaCached => self
                .legacy
                .resolve_cached(DEFAULT_LOCAL_PA_BASE + offset as u64, self.chunk_size),
            DataplaneMode::GenericGva => self
                .generic
                .resolve_generic(DEFAULT_GVA_BASE + offset as u64, self.chunk_size),
            DataplaneMode::Gsva => self
                .gsva
                .resolve_gsva(DEFAULT_GSVA_BASE + offset as u64, self.chunk_size),
        }
    }

    fn offset_for_iter(&self, iter: u64) -> usize {
        let chunks = self.size / self.chunk_size;
        ((iter as usize) % chunks) * self.chunk_size
    }
}

fn fill_payload(bytes: &mut [u8], seed: u64) {
    let mut state = seed ^ 0xa076_1d64_78bd_642f;
    for byte in bytes {
        state ^= state >> 32;
        state = state.wrapping_mul(0xe703_7ed1_a0b4_28db);
        *byte = (state >> 24) as u8;
    }
}

fn fold_checksum(acc: u64, value: u64) -> u64 {
    acc.rotate_left(7).wrapping_mul(0x9e37_79b1_85eb_ca87) ^ value ^ 0xc2b2_ae3d_27d4_eb4f
}

#[cfg(test)]
mod tests {
    use super::*;

    fn strings(values: &[&str]) -> Vec<String> {
        values.iter().map(|value| value.to_string()).collect()
    }

    #[test]
    fn dataplane_microbench_args_parse_defaults_and_modes() {
        let parsed = args_from_slice(&strings(&[
            "dataplane-microbench",
            "--iterations",
            "128",
            "--size",
            "4096",
            "--chunk-size",
            "64",
            "--legacy-map-count",
            "8",
            "--modes",
            "legacy-pa-linear,legacy-pa-direct,legacy-pa-indexed,legacy-pa-cached,gva,gsva",
        ]))
        .expect("parse")
        .expect("args");

        assert_eq!(parsed.iterations, 128);
        assert_eq!(parsed.size, 4096);
        assert_eq!(parsed.chunk_size, 64);
        assert_eq!(parsed.legacy_map_count, 8);
        assert_eq!(
            parsed.modes,
            vec![
                DataplaneMode::LegacyPaLinear,
                DataplaneMode::LegacyPaDirect,
                DataplaneMode::LegacyPaIndexed,
                DataplaneMode::LegacyPaCached,
                DataplaneMode::GenericGva,
                DataplaneMode::Gsva
            ]
        );
    }

    #[test]
    fn dataplane_microbench_rejects_unaligned_legacy_map_split() {
        let err = parse_args(&strings(&[
            "--size",
            "4097",
            "--chunk-size",
            "64",
            "--legacy-map-count",
            "8",
        ]))
        .expect_err("reject uneven map split");

        assert!(err.to_string().contains("divisible"));
    }

    #[test]
    fn dataplane_microbench_report_covers_all_modes() {
        let args = parse_args(&strings(&[
            "--iterations",
            "256",
            "--warmup",
            "8",
            "--size",
            "4096",
            "--chunk-size",
            "64",
            "--legacy-map-count",
            "8",
            "--verify",
        ]))
        .expect("parse");
        let report = run_report(&args).expect("run report");

        assert_eq!(report.status, "pass");
        assert_eq!(report.cases.len(), 6);
        for case in &report.cases {
            assert_eq!(case.operations, 512);
            assert_eq!(case.read_bytes, 16_384);
            assert_eq!(case.write_bytes, 16_384);
            assert_eq!(case.verify_failures, 0);
            assert!(case.mixed_ns > 0);
            assert!(case.resolve_only_ns > 0);
            assert!(case.copy_only_ns > 0);
        }
        assert!(report
            .cases
            .iter()
            .find(|case| case.mode == DataplaneMode::GenericGva)
            .and_then(|case| case.speedup_vs_legacy_pa_linear)
            .is_some());
        assert!(report
            .cases
            .iter()
            .find(|case| case.mode == DataplaneMode::Gsva)
            .and_then(|case| case.speedup_vs_legacy_pa_linear)
            .is_some());
    }

    #[test]
    fn dataplane_microbench_legacy_aliases_select_linear_mode() {
        let parsed =
            parse_args(&strings(&["--modes", "legacy-pa,legacy"])).expect("parse legacy aliases");

        assert_eq!(parsed.modes, vec![DataplaneMode::LegacyPaLinear]);
    }

    #[test]
    fn dataplane_microbench_report_covers_legacy_baseline_modes() {
        let args = parse_args(&strings(&[
            "--iterations",
            "128",
            "--warmup",
            "8",
            "--size",
            "4096",
            "--chunk-size",
            "64",
            "--legacy-map-count",
            "8",
            "--modes",
            "legacy-pa-linear,legacy-pa-direct,legacy-pa-indexed,legacy-pa-cached",
            "--verify",
        ]))
        .expect("parse");
        let report = run_report(&args).expect("run report");
        let modes = report
            .cases
            .iter()
            .map(|case| case.mode)
            .collect::<Vec<_>>();

        assert_eq!(
            modes,
            vec![
                DataplaneMode::LegacyPaLinear,
                DataplaneMode::LegacyPaDirect,
                DataplaneMode::LegacyPaIndexed,
                DataplaneMode::LegacyPaCached,
            ]
        );
        for case in &report.cases {
            assert_eq!(case.verify_failures, 0);
            assert!(case.mixed_ns > 0);
            assert!(case.resolve_only_ns > 0);
            assert!(case.copy_only_ns > 0);
        }
    }
}
