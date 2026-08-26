# Sim Console

`sim-console` is the unified CLI and Web control plane for registered simulator
clusters and demos.

## Start The Web Console

From the repository root:

```bash
cargo run -p sim-console -- serve
```

Open `http://127.0.0.1:9080`.

The default catalog includes OBMM pool, URMA/RPC, standalone Memory Service,
GVA/GSVA, UB-SSD, direct EL0 upcall, and W5 Qwen/DeepSeek V4 Flash entries. The
Web page exposes reviewed demos, targets, and parameters only; it does not
accept commands, SSH options, or raw environment variables.

The execution-target selector defaults to `n4-910c1`. Select `n4-910c`,
`n4-910c1`, or `local` before starting a run. Readiness is evaluated for the
selected target, and each run record retains its target ID.

Before enabling `Start run`, the backend checks required repository paths and
verifies that `out/Image` was built for the current `kernel_ub` source
signature. Missing or stale guest artifacts are reported as launch blockers;
they do not create a run that is already known to fail.

## CLI

```bash
cargo run -p sim-console -- catalog
cargo run -p sim-console -- targets
cargo run -p sim-console -- readiness --target n4-910c
cargo run -p sim-console -- runs
cargo run -p sim-console -- \
  run w5-deepseek-v4-flash-8 --target n4-910c --set steps=2
cargo run -p sim-console -- status <run-id>
cargo run -p sim-console -- logs <run-id> --node nodeA
cargo run -p sim-console -- \
  input <run-id> --node nodeA --text "uname -a"
cargo run -p sim-console -- stop <run-id>
```

`run` stays attached until the registered demo reaches a terminal state. The
Web server admits one active run at a time, can own multiple sequential runs,
and persists their metadata below `out/sim-console/runs/`. The backend enforces
the single-run rule, so API clients cannot bypass the Web control state and
start conflicting QEMU clusters.

## Execution Targets

Reviewed targets live in `config/targets.yaml`. Another registry can be loaded
without changing the catalog:

```bash
cargo run -p sim-console -- --targets /path/to/targets.yaml serve
```

An SSH target declares:

- `ssh_host`: an SSH host alias such as `n4-910c`;
- `connect_timeout_secs`: the bounded SSH connection timeout;
- `repo_root`: the managed worktree used only by sim-console;
- `workspace_source_repo`: an existing repository used as the remote Git
  object cache, not as a worktree to reset or clean;
- `model_sources`: logical model IDs mapped to paths that exist on that target;
- `submodule_mirrors`: reviewed fetch locations for submodule commits that may
  be absent from the remote object cache.

A model demo references a logical ID such as `qwen3-0.6b`, never a path copied
from the machine hosting the Web console. Readiness checks the mapped path on
the selected target, and the runner passes that target-local path to the model
launcher. Sim-console does not copy model weights as part of each run.

Mirror branches seed object discovery, but do not override the repository.
When a branch tip is behind, sim-console fetches the exact root gitlink SHA and
fails closed if that object is unavailable.

For every remote run, sim-console bundles the local committed `HEAD`, transfers
it over SSH, checks out that exact revision in `repo_root`, aligns submodules to
the root gitlinks, and invokes the registered launcher there. Therefore QEMU,
the Rust bridge, guest artifacts, and the demo process are built or reused on
the selected target. The revision is frozen when the run is created and stored
in the run record. Local uncommitted files are deliberately not transferred;
commit a runnable source state before using it remotely.

Process output stays attached to the local run log. Matching remote node logs
are mirrored into the local run directory while the run is active and once
more at completion. Stop requests terminate both the remote preparation group
and the remote launcher group, then terminate the locally owned SSH worker.

The managed `repo_root` must differ from `workspace_source_repo`. Sim-console
never resets or cleans the source repository, which protects an existing dirty
testbed checkout such as `/home/ll/ub_sim`.

## Node Serial Input

W5 demos declare a reviewed `qemu_serial_env` node-input adapter. Select a live
node in the Web console to display its serial log and input line. Enter sends a
UTF-8 line with a trailing newline to that node only. The equivalent CLI is:

```bash
cargo run -p sim-console -- \
  input <run-id> --node nodeA --text "uname -a"
```

Use `--no-newline` when the payload must be written without Enter. Input is
limited to 4096 bytes. The browser cannot provide a socket path, SSH host, or
remote command: the backend resolves the selected node through the run-scoped
serial manifest. Remote payload bytes travel through SSH stdin and are never
interpolated into a shell command or copied into the process log.

## Lightweight Fixture

Use the fixture catalog to test the control plane without QEMU or model data:

```bash
cargo run -p sim-console -- \
  --catalog crates/sim-console/tests/fixtures/catalog.yaml \
  run fixture-cluster --set delay_ms=20
```

## Current Control Boundary

The first implementation provides run start/stop, process logs, node discovery,
node status, node-specific log selection, and serial input for demos with a
reviewed node-input adapter. Existing launchers do not expose one uniform
per-node lifecycle endpoint, so node restart, pause, resume, and QMP commands
remain disabled until adapters publish a stable node-control contract.

Local and reviewed SSH targets use the same catalog, run records, logs, and
controls. Arbitrary browser-provided hosts and commands are not part of the
contract. Add or change a machine in the target registry, review that file,
then select its ID through Web or CLI.
