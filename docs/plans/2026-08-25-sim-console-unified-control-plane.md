# Sim Console Unified Control Plane

## 1. Objective

The repository needs one operational surface for its simulator clusters and
demos. A user should be able to select a registered capability, choose a valid
topology and profile, start it, inspect cluster and node state, follow logs, and
stop the run without learning launcher names or environment variables.

The console must cover these capability families:

- 2-node, 4-node, and 8-node simulator topologies;
- OBMM pools and remote-memory data paths;
- URMA and RPC demos;
- standalone Memory Service demos;
- GVA and GSVA identity, lifecycle, and coherence flows;
- UB-SSD and GSVA-backed storage flows;
- direct EL0 upcall and coroutine flows;
- W5 Qwen and DeepSeek V4 Flash pipeline inference.

## 2. Product Boundary

The console is a control plane over existing implementation entrypoints. It
does not move QEMU, Memory Service, model execution, or validation logic into a
web server. Existing scripts initially remain execution adapters, but they are
registered through structured catalog records rather than exposed directly.

The browser never receives or submits a shell command. It selects:

- a reviewed demo ID;
- values for catalog-declared parameters;
- a reviewed execution target ID.

The backend resolves these values into an executable and argument vector.

## 3. Repository Structure

```text
crates/sim-console/
  AGENTS.md
  Cargo.toml
  catalog/
    demos.yaml
  config/
    targets.yaml
  src/
    api.rs
    catalog.rs
    domain.rs
    lib.rs
    main.rs
    runner.rs
  web/
    app.js
    index.html
    styles.css
  tests/
    fixtures/
```

Runtime state is written below:

```text
out/sim-console/runs/<run-id>/
  process.log
  remote-plan.json
  remote-node-logs/
  run.json
```

Guest and QEMU logs remain in `guest-linux/aarch64/logs/` because existing
launchers and reports already treat that directory as authoritative.

## 4. Domain Model

### 4.1 Demo definition

Each catalog record declares:

- stable ID, title, capability family, description, and tags;
- topology kind and node count;
- model and data-plane labels where relevant;
- fixed executable and fixed arguments;
- typed parameters with defaults and allowed values;
- expected duration and required host capabilities;
- lifecycle: `automatic` or `interactive_shell`;
- supported controls and log discovery rules.

Catalog records are reviewed source files. A request cannot add an executable,
argument, environment variable, or filesystem path that the record did not
declare.

### 4.2 Run

A run has one immutable demo ID and resolved launch plan. Its lifecycle is:

```text
queued -> starting -> running -> passed
                             -> failed
                    -> stopped
```

The backend owns the run ID and process group. Stopping a run first sends
`SIGTERM` to the process group so launcher cleanup traps can execute. A bounded
escalation may send `SIGKILL` if the group does not exit.

An `automatic` demo validates its workload, cleans up every guest, and reaches
a terminal run state without operator input. It must not declare node input.
An `interactive_shell` demo validates the workload and then keeps its guests,
serial sockets, and run-scoped serial manifest alive until Stop. It must
declare a node-input adapter. A shell prompt preserved in a log after QEMU has
exited is historical output, not an interactive endpoint.

Phase 1 admits one active run per backend. This protects shared QEMU ports,
sockets, images, and generated artifacts even when a caller bypasses the Web UI
and uses the HTTP API directly.

### 4.3 Node

Nodes are created from topology metadata, not guessed from the process count.
Their observable state is derived from log evidence:

- `unknown`: no evidence yet;
- `booting`: node log exists but no readiness evidence;
- `ready`: readiness or application-start marker observed;
- `passed`: node pass marker observed;
- `failed`: failure, panic, or fatal marker observed;
- `stopped`: run was stopped before a stronger terminal result.

The first phase supports node selection, node-specific log inspection, and
serial input for `interactive_shell` catalog records with a stable adapter.
`automatic` records cannot declare the control. The request selects only a
known run and node; socket paths remain backend-owned. Remote payload bytes
travel through SSH stdin and are not interpolated into a shell command or
written to the process log. Other node actions remain disabled unless their
adapters expose stable control endpoints.

## 5. APIs And CLI

The backend exposes versioned JSON endpoints:

```text
GET    /api/v1/health
GET    /api/v1/catalog
GET    /api/v1/targets
POST   /api/v1/targets/{target_id}/prepare
GET    /api/v1/readiness?target=n4-910c
GET    /api/v1/runs
POST   /api/v1/runs
GET    /api/v1/runs/{run_id}
GET    /api/v1/runs/{run_id}/logs?cursor=N&node=nodeA
POST   /api/v1/runs/{run_id}/nodes/{node_id}/input
POST   /api/v1/runs/{run_id}/stop
```

The same backend domain is available through CLI commands:

```text
sim-console catalog
sim-console targets
sim-console prepare-target <target-id>
sim-console readiness [--target target-id]
sim-console run <demo-id> [--target target-id] [--set name=value]
sim-console runs
sim-console status <run-id>
sim-console logs <run-id> [--node nodeA]
sim-console input <run-id> --node nodeA --text "uname -a"
sim-console stop <run-id>
sim-console serve [--listen 127.0.0.1:9080]
```

CLI parity prevents Web-only lifecycle behavior and provides an automation
surface without returning to per-demo scripts.

## 6. Web Experience

The first screen is an operational workspace:

- left rail: searchable demo catalog grouped by capability;
- main workspace: selected demo, topology, typed configuration, and start;
- run workspace: topology with live node states and selected-node details;
- lower log band: process or node log with cursor-based incremental refresh;
- selected-node serial input for demos with a reviewed node-input adapter;
- run history: recent status, duration, topology, and result.

The page polls lightweight state and log cursors. A later phase may add SSE, but
polling is sufficient for simulator events measured in hundreds of
milliseconds or seconds and avoids coupling lifecycle correctness to a stream.

Launch readiness is separate from run status. Required paths and guest artifact
freshness are checked before the start control is enabled and are rechecked by
the backend before spawning a process. A stale `Image` therefore cannot become
a short-lived failed run merely because an API caller bypassed the Web UI.

## 7. Execution Targets

The local target executes on the host running `sim-console`. SSH targets execute
the complete launcher, including artifact and QEMU builds, in the configured
remote repository. The local backend owns the SSH process, captures process
output, mirrors node logs for observation, and sends a run-scoped stop request
before terminating the local SSH process.

Browser requests select a target ID from `config/targets.yaml`; they never
submit an SSH host, repository path, or command. Adding another testbed is a
configuration review, not a new launcher or Web code change.

Model demos declare a logical model source ID. Each execution target maps that
ID to a path available on the target machine. Readiness validates the mapped
path before launch, and the resolved command receives the target-local path;
an absolute path from the machine hosting sim-console is never reused on an
SSH target. Model weights are provisioned independently and are not transferred
for every run.

Every SSH run transfers the local committed root revision as a Git bundle and
checks it out in a dedicated managed worktree. Top-level submodules are aligned
to the root gitlinks before the launcher starts. The configured source
repository is only an object cache: sim-console does not reset, clean, or run
from that potentially dirty checkout.

Target preparation bootstraps the registered source cache and user-local build
tools, then proves that every top-level gitlink can be checked out without lazy
network access. A reviewed remote mirror is attempted first. If it exposes only
the commit or an incomplete promisor tree, sim-console transfers a local
checkout pack containing the pinned commit plus its current tree and blob
objects. Readiness requires the source mirror to be at that detached revision
with a clean index; commit-object presence by itself is insufficient.
Registered bootstrap files are copied into the target source cache and restored
into every managed worktree, avoiding first-run dependence on external source
sites such as busybox.net.

## 8. Delivery Phases

### Phase 1: local control-plane vertical slice

- catalog and schema validation;
- CLI and HTTP API;
- process-group lifecycle and persistent run metadata;
- process log capture and node-log discovery;
- operational Web UI;
- fixture runner tests that never launch QEMU.

### Phase 2: complete demo onboarding

- validate every registered adapter on its supported host;
- add prerequisite probes for QEMU, kernel, initramfs, model, disk, and device;
- normalize progress and result markers into structured events;
- expose stable node operations where adapters support them.

### Phase 3: remote Linux testbeds

- completed: register `n4-910c` and `n4-910c1` as reviewed targets;
- completed: transfer an exact root revision and prepare a managed worktree;
- completed: execute builds and launchers through the SSH target adapter;
- completed: capture process output and mirror run-scoped node logs;
- completed: stop remote preparation and launcher process groups;
- remaining: add configurable artifact retention and target-wide concurrency
  admission when multiple console backends share one testbed.

### Phase 4: launcher convergence

- move duplicated topology launch logic into shared libraries;
- replace filename-derived behavior with typed launch plans;
- archive redundant scripts only after their catalog entry uses the shared
  implementation and has equivalent tests.

## 9. Acceptance Criteria

Phase 1 is complete when:

1. the catalog lists every requested capability family and 2/4/8-node variants
   where a real repository entrypoint exists;
2. an untrusted API request cannot execute an arbitrary command or path;
3. a fixture run can be started, observed, read by node, stopped, and recovered
   in run history through both CLI and Web APIs;
4. the Web UI renders catalog, topology, node state, logs, and run controls on
   desktop and mobile viewports;
5. all local validation remains lightweight and starts no QEMU process.

The SSH target slice is complete when:

1. Web and CLI can select `n4-910c` or `n4-910c1` by target ID;
2. source preparation, compilation, QEMU launch, and demo execution occur in
   the configured remote managed worktree;
3. the local run log contains remote build and process output;
4. node logs are mirrored under the local run directory;
5. stop terminates both remote run-scoped process groups without touching
   unrelated QEMU processes on the target;
6. an actual registered two-node QEMU demo reaches `passed` on a target.

## 10. Validation Evidence

The SSH target slice was validated on `n4-910c` on 2026-08-25 with the
registered `obmm-pool-2` demo.

- Run: `sim-console-1787654282472-0`
- Target: `n4-910c`
- Frozen validation revision: `9d06536380ace58c10ee2acee1aadd56cf3cbbc2`
- QEMU gitlink: `15951308ea8fa1fbce600d434a5b8cf72e132f14`
- Result: `passed`
- Node A: `passed`, mirrored guest log present
- Node B: `passed`, mirrored guest log present
- Incremental elapsed time: about 56 seconds
- Managed QEMU processes after completion: zero

The validation revision is a detached test-only root revision whose only
delta from repository `HEAD` is the QEMU gitlink required by the model-neutral
bridge ABI. It did not move `master`.

The configured source repository `/home/ll/ub_sim` remained at `67c3ce5f` and
retained its existing dirty worktree. Sim-console prepared and executed from
`/home/ll/sim-console/ub_sim`, confirming that it did not reset or clean the
user's source checkout.
