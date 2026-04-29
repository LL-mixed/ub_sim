# `ub_sim.git` Eight-Node Final Validation

Date: 2026-04-15

Repo heads used for this validation:

- `ub_sim.git`: `a7c262b`
- `vendor/qemu_8.2.0_ub`: `a67f287407`
- `guest-linux/kernel_ub`: `9298683e75b0`

## Scope

Final confirmation in the standalone `ub_sim.git` workspace for:

- `8-node smoke`
- `8-node chat`
- `8-node rpc`
- `8-node udma`
- `8-node obmm-pool`

The goal of this pass was not only functional validation, but also confirming
that `ub_sim.git` validation/tooling now rebuilds stale artifacts automatically
and reuses fresh artifacts without unconditional rebuilds.

## Artifact Freshness Model

This validation used the current `ub_sim.git` self-owned freshness rules:

- `scripts/build_qemu_binary.sh`
  - reuses an existing `qemu-system-aarch64` when:
    - QEMU submodule `HEAD` matches
    - target list matches
    - configure args match
- `scripts/build_guest_artifacts.sh`
  - detects whether `out/Image` matches current `guest-linux/kernel_ub` `HEAD`
  - refreshes stale kernel artifacts automatically
- `scripts/build_initramfs.sh`
  - reuses `out/initramfs.cpio.gz` when the input signature matches:
    - guest demo sources
    - guest headers
    - initramfs scripts
    - busybox
    - packaged modules

Observed behavior in this run:

- QEMU binary was reused:
  - `[build_qemu_binary] using existing QEMU binary`
- guest artifacts were reused:
  - `[build_guest_artifacts] using existing local out/ artifacts`
- initramfs was reused:
  - `[build_initramfs] initramfs is up to date`

This confirms that `ub_sim.git` validation is now input-driven instead of
requiring manual rebuild steps after sync.

## Validation Runs

### 1. Eight-Node Smoke

- report:
  - [eight_node_smoke_report.latest.txt](../guest-linux/aarch64/out/eight_node_smoke_report.latest.txt)
- run dir:
  - [2026-04-15_15-58-44_smoke8_3473_headless8](../guest-linux/aarch64/logs/2026-04-15_15-58-44_smoke8_3473_headless8)
- result:
  - `PASS`

Validated:

- `port_num=7`
- eight guests complete bootstrap and enter shell
- eight-node full-mesh identity and route publication remain stable

### 2. Eight-Node Chat Matrix

- report:
  - [eight_node_chat_matrix.latest.txt](../guest-linux/aarch64/out/eight_node_chat_matrix.latest.txt)
- run dir:
  - [2026-04-15_15-59-08_chat8_17708_headless8](../guest-linux/aarch64/logs/2026-04-15_15-59-08_chat8_17708_headless8)
- result:
  - `PASS`

Validated:

- all `28` undirected full-mesh chat pairs passed

### 3. Eight-Node RPC Matrix

- report:
  - [eight_node_rpc_matrix.latest.txt](../guest-linux/aarch64/out/eight_node_rpc_matrix.latest.txt)
- run dir:
  - [2026-04-15_16-01-52_rpc8_16415_headless8](../guest-linux/aarch64/logs/2026-04-15_16-01-52_rpc8_16415_headless8)
- result:
  - `PASS`

Validated:

- all `56` directed RPC calls passed
- each node acted as both server and client

### 4. Eight-Node UDMA Matrix

- report:
  - [eight_node_udma_matrix.latest.txt](../guest-linux/aarch64/out/eight_node_udma_matrix.latest.txt)
- run dir:
  - [2026-04-15_16-03-54_udma8_27148_headless8](../guest-linux/aarch64/logs/2026-04-15_16-03-54_udma8_27148_headless8)
- result:
  - `PASS`

Validated:

- all `56` directed UDMA calls passed
- standalone `ub_sim.git` now reproduces the main-repo eight-node UDMA result

### 5. Eight-Node OBMM Pool

- report:
  - [eight_node_obmm_pool.latest.txt](../guest-linux/aarch64/out/eight_node_obmm_pool.latest.txt)
- run dir:
  - [2026-04-15_16-09-01_obmmpool8_6225_headless8](../guest-linux/aarch64/logs/2026-04-15_16-09-01_obmmpool8_6225_headless8)
- result:
  - `PASS`

Validated:

- each node exported one pool slot
- each node imported all remote pool slots
- eight-node round-based pool synchronization completed successfully
- this run used `ub_sim.git`’s own refreshed/reused artifacts, not the main repo’s `Image`

## Conclusion

Current standalone `ub_sim.git` status:

- `8-node smoke`: `PASS`
- `8-node chat`: `PASS`
- `8-node rpc`: `PASS`
- `8-node udma`: `PASS`
- `8-node obmm-pool`: `PASS`

This is the final confirmation that:

1. the synchronized codepaths from the main repo are present in `ub_sim.git`
2. `ub_sim.git` validation/tooling now correctly picks stale artifacts after sync
3. fresh artifacts are reused instead of being rebuilt blindly
4. the standalone workspace reproduces the main repo’s current eight-node matrix status

## Current Boundary

This report confirms the current validated eight-node matrix set. It does not
claim:

- long-duration soak coverage
- randomized topology perturbation coverage
- acceptance coverage beyond the current scripted smoke/chat/rpc/udma/obmm-pool matrix
