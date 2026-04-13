# QEMU UB Alignment Gap Analysis v0

## Goal

Align the current `simulator/qemu-device/linqu-ub` path with the existing UB device model already implemented in `/Volumes/repos/ub/qemu` on branch `qemu-8.2.0`, instead of continuing to grow an independent device contract.

The immediate target is objective `#2`: support real Linux `drivers/ub` bring-up on top of a guest-visible device/runtime model that matches the existing UB QEMU implementation more closely.

## What Exists In `/Volumes/repos/ub/qemu`

The `qemu-8.2.0` branch already contains a substantial UB platform model:

- `hw/ub/ub.c`
- `hw/ub/ub_common.c`
- `hw/ub/ub_enum.c`
- `hw/ub/ub_ummu.c`
- `hw/ub/ub_ummu_internal.h`
- `hw/ub/hisi/ubc_msgq.c`
- `hw/arm/virt.c`
- `hw/core/sysbus-fdt.c`

Important properties of that implementation:

- `virt.c` exposes a dedicated `VIRT_UBIOS_INFO_TABLE` memory slot.
- `virt.c` populates `/chosen/linux,ubios-information-table`.
- `virt.c` instantiates `TYPE_UB_UMMU` directly.
- `virt.c` maps UBC msgq windows and aliases controller MMIO into a high UB MMIO region.
- `hisi/ubc_msgq.c` implements a real SQ/RQ/CQ queue protocol over guest DMA memory.
- `ub_ummu.c` provides a much richer UMMU implementation than the current fake register block in `linqu-ub`.

## What We Currently Have

Our current path is centered around:

- `simulator/qemu-device/hw/misc/linqu_ub.c`
- `simulator/qemu-device/include/hw/misc/linqu_ub_regs.h`
- `simulator/vendor/qemu/hw/core/sysbus-fdt.c`
- `simulator/crates/sim-qemu/src/ffi.rs`

Current strengths:

- Real ARM64 guest-visible MMIO path exists.
- Guest Linux probe and a minimal guest driver already work.
- Real `ubfi`, `ubus`, `ummu`, and `hisi_ubus` bring-up has progressed much further than before.
- We already added minimal UBIOS, UBC, and UMMU surfaces sufficient to get real kernel bring-up into later stages.

Current weakness:

- The contract is still custom and only partially overlaps the existing UB QEMU model.
- We are duplicating behavior that already exists upstream in `/Volumes/repos/ub/qemu`.

## Main Gaps

### 1. UBIOS Placement And `virt` Integration

`/Volumes/repos/ub/qemu`:

- Places UBIOS in a dedicated `virt` memmap slot (`VIRT_UBIOS_INFO_TABLE`).
- Calls `ub_init_ubios_info_table(...)` from `virt.c`.

Current `linqu-ub`:

- Exposes UBIOS as a second MMIO region on the platform device.
- Populates `/chosen/linux,ubios-information-table` from `sysbus-fdt.c`.

Implication:

- Our current UBIOS exposure works for bring-up, but it does not match the existing UB QEMU design.
- We should move toward a `virt`-owned UBIOS placement model.

### 2. UBC Message Queue Protocol

`/Volumes/repos/ub/qemu/hw/ub/hisi/ubc_msgq.c`:

- Implements host processing of SQ entries from guest DMA memory.
- Initializes SQ/RQ/CQ using `*_ADDR`, `*_DEPTH`, producer and consumer pointers.
- Advances `SQ_CI`, `RQ_PI`, `CQ_PI`.
- Uses real queue depth validation and DMA reads/writes.

Current `linqu-ub`:

- Has a custom msgq implementation with compatible-looking registers.
- Handles only a minimal subset of:
  - cfg read/write
  - enum topo query
  - enum NA cfg/query
  - a minimal HiSilicon private EU config response

Implication:

- Our msgq path is the most obvious place to align next.
- We should either port or mirror the queue state model and handler split from `ubc_msgq.c`.

### 3. UMMU Model

`/Volumes/repos/ub/qemu/hw/ub/ub_ummu.c`:

- Provides a real sysbus UMMU device (`TYPE_UB_UMMU`).
- Hooks into UB bus and IOMMU ops.
- Has a richer register model and lifecycle.

Current `linqu-ub`:

- Embeds a fake UMMU register block inside one MMIO region.
- It is only as rich as needed for current bring-up.

Implication:

- Our current fake UMMU block is useful as a temporary shim.
- Long term, aligning to the existing `TYPE_UB_UMMU` implementation is cleaner than extending the fake block indefinitely.

### 4. Device Decomposition

`/Volumes/repos/ub/qemu`:

- Splits responsibilities across UB controller, UMMU, msgq, common helpers, and `virt` integration.

Current `linqu-ub`:

- Packs UBIOS, msgq, endpoint MMIO, UMMU, IRQ, and Rust bridge logic into one device implementation.

Implication:

- The monolithic `linqu-ub` device was good for initial bring-up.
- It is now the wrong architectural direction if the target is real UB driver compatibility.

## Recommended Migration Order

### Stage 1: Treat `/Volumes/repos/ub/qemu` As The Reference Contract

Do not invent new guest-visible behavior unless required.

Immediate rule:

- If `/Volumes/repos/ub/qemu` already defines a UB-facing register layout, queue lifecycle, `virt` hookup, or UBIOS placement, that becomes the preferred reference.

### Stage 2: Align Message Queue Semantics First

Focus on `hw/ub/hisi/ubc_msgq.c` first, because it is the path currently blocking real `hisi_ubus` progress.

Concrete steps:

1. Compare our current msgq register semantics against `ubc_msgq.c`.
2. Match queue init and queue progress rules:
   - SQ init
   - RQ init
   - CQ init
   - producer and consumer movement
3. Match the minimal handler set needed by current real-kernel bring-up:
   - cfg read path
   - guid read path
   - enum path
   - minimal HiSilicon private path

### Stage 3: Move UBIOS Ownership Toward `virt.c`

Once msgq alignment is underway, move away from making the platform device own UBIOS placement.

Target shape:

- `virt.c` owns UBIOS placement.
- `/chosen/linux,ubios-information-table` is populated from `virt.c`.
- the device model consumes shared UB platform state instead of owning the table outright.

### Stage 4: Replace The Fake Embedded UMMU With A Real UB UMMU Device Model

Do not keep expanding the current fake UMMU block if the real `TYPE_UB_UMMU` already exists in the reference QEMU tree.

Target shape:

- dedicated UMMU device object
- UB bus linkage
- real IOMMU-facing lifecycle

## Immediate Next Step

The next concrete implementation step should be:

1. diff our current msgq register and queue behavior against `/Volumes/repos/ub/qemu/hw/ub/hisi/ubc_msgq.c`
2. modify `linqu-ub` msgq handling to follow that state model more closely
3. use real Linux `hisi_ubus` logs as the acceptance loop

This is the shortest path to keep advancing real UB bring-up without continuing to grow a private device contract.
