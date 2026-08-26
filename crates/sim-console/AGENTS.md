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
- Remote execution must use a target from the loaded registry. Quote every
  reviewed command argument for the remote shell, keep the remote repository
  root fixed by target configuration, and identify the run with the
  backend-owned run ID.
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
