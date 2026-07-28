# Repository Guidelines

## Project Structure & Module Organization

This repository is a UB data system simulator meta repo. Rust simulator code lives under `crates/`, one crate per subsystem. Current crates: `sim-core`, `sim-config`, `sim-report`, `sim-topology`, `sim-runtime`, `sim-services`, `sim-memory`, `sim-models`, `sim-qemu`, `sim-uapi`, `sim-workloads`, `sim-cli`, and `sim-chipbackend-simpler` (see `Cargo.toml` for the authoritative list). Guest-side Linux, QEMU launchers, apps, drivers, scripts, and Python regression tests live under `guest-linux/aarch64/`; shared guest code also lives under `guest-linux/aarch64/components/` (currently `llm_infer`, `mem_service`) and `guest-linux/aarch64/apps/` (per-feature C programs). Topology YAML files live in `scenarios/` and are named `mvp_<N>host_*.yaml`. QEMU/FM topology and third-party sources live under `vendor/`; note `vendor/qemu_8.2.0_ub` and `guest-linux/kernel_ub` are git submodules (see `.gitmodules`). Design notes, validation reports, and plans belong in `docs/`. Generated artifacts and logs should stay in `out/`, `logs/`, `target/`, or `build_output/`, not in source directories.

`CLAUDE.md` holds additional local-environment and QEMU-build/run gotchas; read it before touching QEMU build or guest-launch scripts.

## Build, Test, and Development Commands

- `cargo build --workspace`: build all Rust crates.
- `cargo test --workspace`: run Rust unit tests across the workspace.
- `cargo fmt --all`: format Rust code; run before submitting changes.
- `cargo run --release -p sim-cli -- qwen3-decode-loop --scenario 2host`: run the main CLI decode path; set required model/artifact environment variables first.
- `python3 -m unittest discover guest-linux/aarch64/tests`: run guest harness script and contract tests (`test_*.py`).
- `python3 -m unittest guest-linux/aarch64/tests/<module>`: run a single contract test module.
- `cd guest-linux/aarch64 && ./scripts/build_guest_artifacts.sh`: prepare guest kernel, modules, and initramfs inputs.
- `cd guest-linux/aarch64 && ./scripts/build_qemu_binary.sh`: build QEMU through the project wrapper only. Do not run `vendor/qemu_8.2.0_ub` configure/ninja directly; on macOS the wrapper appends `--disable-zstd` to avoid a libzstd header mismatch. Override with `QEMU_CONFIGURE_ARGS=...` only if zstd headers/link paths are complete.

Any command that starts QEMU guests, including `run_ub_*_w4_guest.sh`, `launch_ub_*_headless.sh`, `qwen3-guest-decode-loop`, and QMP/socket harnesses, must run outside the Codex sandbox as required by `CLAUDE.md`. The harness creates QMP/serial/monitor UNIX sockets and spawns multiple guest processes, which the sandbox blocks with `Failed to bind socket ... Operation not permitted`. Do not request permissions for ad hoc commands assembled by prefixing or concatenating environment-variable assignments. Request approval for a reusable script or command prefix, and describe required environment variables separately. For the Qwen3-0.6B 8-node W4 guest 2-step path, use the fixed entry point `guest-linux/aarch64/scripts/run_ub_eight_node_w4_guest_qwen3_0_6b_2step.sh`. After guest runs, confirm no leftover QEMU processes with `pgrep -fl qemu-system-aarch64`.

## Coding Style & Naming Conventions

Rust code uses edition 2021 and standard `rustfmt` style; run `cargo fmt --all` before submitting Rust changes. Keep module and function names `snake_case`, types `PascalCase`, and constants `SCREAMING_SNAKE_CASE`. Python tests use `unittest`, `test_*.py` filenames, and explicit assertions. Shell entrypoints are named by action and topology, for example `run_ub_eight_node_rpc_matrix.sh`.

## Testing Guidelines

Add focused tests with every behavior change. Prefer Rust unit tests beside the affected module and Python contract tests in `guest-linux/aarch64/tests/` for script, artifact, and layout behavior. Automation and CI-style guest validation must use headless scripts, not tmux launchers.

## Commit & Pull Request Guidelines

Git history uses short imperative subjects such as `Add TCP transport benchmark reporting` and `Split mem service Qwen3 runtime helpers`. Keep commits focused, in English, and avoid generated-output churn. Pull requests should describe the user-visible behavior change, list exact validation commands and results, link related docs/issues, and call out QEMU, guest artifact, or environment requirements.

## Security & Configuration Tips

Do not commit model weights, kernel artifacts, SSH targets, secrets, or local absolute paths except documented examples. Use environment variables such as `AARCH64_LINUX_CC`, `BUSYBOX`, and `SIM_QWEN3_0_6B_WEIGHTS_PATH` for machine-specific configuration. Compiled guest apps and submodules are gitignored (e.g. `guest-linux/aarch64/apps/mem_service/linqu_mem_service*`, `guest-linux/aarch64/apps/obmm_*/obmm_*`, `apps/llm_infer/linqu_llm_infer`); build them rather than expecting them in-tree. `vendor/qemu_8.2.0_ub` and `guest-linux/kernel_ub` are git submodules — initialize with `git submodule update --init` on a fresh clone. `CLAUDE.md` is also gitignored; it is local environment notes only.

## Memory Service Architecture Law

`mem_service` is the repository's complete replacement for Mooncake. No
deployment or serving integration may depend on Mooncake as a sidecar,
fallback, transfer engine, store, or scheduler.

The service core must remain transport-neutral:

- Core headers, records, readiness, wire contracts, and placement policy must
  not name RoCE, RDMA verbs, URMA, UB shared memory, TCP, CUDA, or a device CNA.
- Transport-specific code must implement the provider contract outside the
  core. The QEMU eight-node environment selects UB/URMA and shared-memory
  providers; DGX deployments select a RoCE full-mesh provider.
- Serving engines call one Memory Service SDK. DS4 and W5 must not select,
  configure, or call a transport directly.
- The control plane resolves object identity, placement, version, lease, and
  lifecycle. Payloads move directly between provider endpoints; the control
  plane must not proxy the payload.
- Service readiness and provider readiness are separate. A control-plane
  process may be ready with zero data providers, but it must report
  `data_plane_ready=0`.
- Every provider must pass the same registration, bounds, transfer,
  completion, checksum, and fail-closed conformance suite.

Provider-neutral contracts live in
`guest-linux/aarch64/components/mem_service/`. Concrete provider
implementations live under
`guest-linux/aarch64/components/mem_service/providers/` and follow that
directory's `README.md`.
