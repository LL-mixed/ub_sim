# Sim Console Rules

## Purpose

`sim-console` is the repository control plane for discovering, launching, and
observing registered simulator demos. It is an operational interface, not a
second implementation of QEMU, Memory Service, or model runtime behavior.

## Directory Layout

- `src/`: backend domain, catalog, process supervision, HTTP API, and CLI.
- `catalog/`: reviewed demo definitions. Browser requests may reference demo
  and parameter IDs only; they must never supply executable paths or raw
  environment variables.
- `config/`: reviewed execution-target definitions. Browser and CLI requests
  may select a target ID, but they must never override its SSH host, repository
  path, transport, or remote command construction.
- `web/`: dependency-free static frontend served by the backend.
- `tests/fixtures/`: lightweight commands and catalogs that never launch QEMU,
  load model weights, or require accelerator hardware.

Generated run state belongs in the repository `out/sim-console/` directory.
Simulator and guest logs remain under their existing `logs/` directories.

## Execution Contract

- Execute programs with an argument vector. Never invoke `sh -c`, `eval`, or a
  command string assembled from a request.
- Resolve every executable and config path relative to the repository root and
  reject paths that escape it.
- Parameters are declared in the catalog with an explicit type, allowed values,
  and argument position. Reject unknown parameters and invalid values.
- The backend owns run IDs, process groups, state transitions, log capture, and
  stop escalation. A browser cannot address an arbitrary host PID.
- Existing launcher scripts are adapters. New lifecycle behavior belongs in the
  control plane or a shared launcher library, not in another per-demo wrapper.
- Node input must use a catalog-declared adapter and a run-owned endpoint. A
  request may select a known node and provide payload bytes, but it must never
  provide a socket path or remote command. Do not interpolate or log the input
  payload; carry it over process stdin to the selected serial transport.
- Demo lifecycle is explicit. An `automatic` demo validates its workload,
  cleans up its guests, and exits; it must not declare `node_input`. An
  `interactive_shell` demo remains live after workload validation until Stop;
  it must declare `node_input` and publish a run-owned serial endpoint. A shell
  prompt in a completed log is not sufficient evidence that a demo is
  interactive.
- Remote execution must use a target from the loaded registry. Quote every
  reviewed command argument for the remote shell, keep the remote repository
  root fixed by target configuration, and identify the run with the
  backend-owned run ID.
- Target preparation may select only a registered target ID. Source and mirror
  URLs, repository paths, tool installers, and commands remain backend-owned;
  the browser must not supply them. Preparation is idempotent, must not replace
  a non-Git path, and is serialized with run admission.
- Prefer user-local build tools. A target may install only fixed, reviewed
  system runtime and native-build packages required by repository entrypoints.
  The openEuler set must stay aligned with
  `scripts/prepare_w5_container_deps.sh` and uses passwordless `dnf`. Package
  names and install commands must never come from the browser. Seed exact
  committed submodule checkouts from reviewed mirrors. When a mirror lacks the
  complete pinned tree, transfer a local checkout pack containing the commit,
  trees, and blobs required for an offline checkout. Commit presence alone is
  not readiness evidence.
- Simpler-backed demos require CMake and a real GCC major 15 simulation-kernel
  compiler. Readiness must verify the compiler version, not only the executable
  name. When the target distribution does not package GCC 15, preparation may
  install the fixed GCC 15 Conda toolchain below the target user's home and
  expose versioned user-local compiler links. Never alias an older compiler as
  `gcc-15` or `g++-15`.
- Target bootstrap files must be repository-relative, reviewed in the target
  registry, and copied from the console repository into the remote source
  cache. Remote runs copy them into the managed worktree after checkout. The
  browser must never select or upload an arbitrary bootstrap file.
- Stop must terminate the remote run, not only the local SSH client. A remote
  run is not terminal until the SSH adapter exits and launcher cleanup has had
  a chance to run.

## API And State

- JSON API types are versioned under `/api/v1` and shared with the CLI domain.
- Run states are `queued`, `starting`, `running`, `passed`, `failed`, or
  `stopped`. Terminal states never transition back to a live state.
- Node state is derived from declared topology and observed logs. Unknown data
  must remain `unknown`; do not manufacture health.
- Persist metadata atomically so a backend restart can display prior runs.

## Frontend

- The first screen is the operational console: catalog, topology, run state,
  node selection, logs, and controls. Do not add a marketing landing page.
- Keep configuration progressive. Show the common path first and reveal
  advanced details only for the selected demo.
- Controls must reflect backend capabilities. Do not show an enabled node action
  that the selected adapter cannot perform.
- The UI must remain usable at desktop and mobile widths without overlapping
  controls, clipped labels, or layout shifts.

## Testing

- Every catalog, lifecycle, API, and CLI behavior needs a focused Rust test.
- Browser behavior must be checked against the fixture runner, not a QEMU demo.
- Local tests must not launch QEMU, build kernels, or load model weights.
