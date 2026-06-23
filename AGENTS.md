# Repository Guidelines

## Project Structure & Module Organization

This repository is a UB data system simulator meta repo. Rust simulator code lives under `crates/`, with one crate per subsystem such as `sim-cli`, `sim-uapi`, `sim-runtime`, `sim-memory`, and `sim-qemu`. Guest-side Linux, QEMU launchers, apps, drivers, scripts, and Python regression tests live under `guest-linux/aarch64/`. Topology YAML files live in `scenarios/`; QEMU/FM topology and third-party sources live under `vendor/`. Design notes, validation reports, and plans belong in `docs/`. Generated artifacts and logs should stay in `out/`, `logs/`, `target/`, or `build_output/`, not in source directories.

## Build, Test, and Development Commands

- `cargo build --workspace`: build all Rust crates.
- `cargo test --workspace`: run Rust unit tests across the workspace.
- `cargo run --release -p sim-cli -- qwen3-decode-loop --scenario 2host`: run the main CLI decode path; set required model/artifact environment variables first.
- `python3 -m unittest discover guest-linux/aarch64/tests`: run guest harness script and contract tests.
- `cd guest-linux/aarch64 && ./scripts/build_guest_artifacts.sh`: prepare guest kernel, modules, and initramfs inputs.
- `cd guest-linux/aarch64 && ./scripts/build_qemu_binary.sh`: build QEMU through the project wrapper only.

Any command that starts QEMU guests, including `run_ub_*_w4_guest.sh`, `launch_ub_*_headless.sh`, and QMP/socket harnesses, must run outside the Codex sandbox as required by `CLAUDE.md`. Do not request permissions for ad hoc commands assembled by prefixing or concatenating environment-variable assignments. Request approval for a reusable script or command prefix, and describe required environment variables separately.

## Coding Style & Naming Conventions

Rust code uses edition 2021 and standard `rustfmt` style; run `cargo fmt --all` before submitting Rust changes. Keep module and function names `snake_case`, types `PascalCase`, and constants `SCREAMING_SNAKE_CASE`. Python tests use `unittest`, `test_*.py` filenames, and explicit assertions. Shell entrypoints are named by action and topology, for example `run_ub_eight_node_rpc_matrix.sh`.

## Testing Guidelines

Add focused tests with every behavior change. Prefer Rust unit tests beside the affected module and Python contract tests in `guest-linux/aarch64/tests/` for script, artifact, and layout behavior. Automation and CI-style guest validation must use headless scripts, not tmux launchers.

## Commit & Pull Request Guidelines

Git history uses short imperative subjects such as `Add TCP transport benchmark reporting` and `Split mem service Qwen3 runtime helpers`. Keep commits focused, in English, and avoid generated-output churn. Pull requests should describe the user-visible behavior change, list exact validation commands and results, link related docs/issues, and call out QEMU, guest artifact, or environment requirements.

## Security & Configuration Tips

Do not commit model weights, kernel artifacts, SSH targets, secrets, or local absolute paths except documented examples. Use environment variables such as `AARCH64_LINUX_CC`, `BUSYBOX`, and `SIM_QWEN3_0_6B_WEIGHTS_PATH` for machine-specific configuration.
