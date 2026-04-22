# UB Simulator Workspace

Standalone repository for the UB/Linqu simulator work.

## Layout

- `crates/`
  Rust workspace crates for simulator control-plane, topology, runtime, UAPI,
  reporting, and CLI work.
- `guest-linux/aarch64/`
  Guest harness source, initramfs assets, demo apps, and launch scripts.
- `scenarios/`
  Scenario YAML inputs consumed by the simulator workspace.
- `vendor/`
  Topology inputs and local notes. The active QEMU tree lives in the
  `vendor/qemu_8.2.0_ub` submodule.
- `docs/`
  Workspace-local design docs, migrated plans, and historical drafts.

## Documentation Entry Points

- [guest-linux/aarch64/README.md](guest-linux/aarch64/README.md)
  Main entry for guest harness usage, initramfs layout, dual-node launchers,
  tmux interactive bring-up, and demo execution order.
- [docs/README.md](docs/README.md)
  Index for workspace-local design notes, migrated plans, and draft material.
- [scenarios/README.md](scenarios/README.md)
  Scenario input overview for simulator runs.
- `vendor/qemu_8.2.0_ub`
  Active QEMU fork submodule; build and runtime usage is referenced from the
  guest harness docs above.
- `guest-linux/kernel_ub`
  Guest kernel submodule; build/sync usage is referenced from the guest harness
  docs above.

## Submodules

- `vendor/qemu_8.2.0_ub`
  Active QEMU fork for dual-node UB simulation.
- `guest-linux/kernel_ub`
  Guest kernel tree used to build the simulator-visible UB modules.

Initialize them after clone:

```bash
git submodule update --init --recursive
```

## Current Scope

- Keep the standalone simulator source and harness in this repo.
- Keep heavyweight third-party trees as submodules.
- Exclude generated guest outputs, logs, toolchains, and archived experiments.

## Notes

- `docs/drafts/` and `docs/plans/` are migrated historical material. Some files
  still use legacy `simulator/...` paths or reference external background docs.
- The authoritative interface sketch for the current Rust workspace is
  `docs/drafts/simulator_rust_interface_sketch_v0.md`.
