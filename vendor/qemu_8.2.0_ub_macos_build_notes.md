# qemu_8.2.0_ub on macOS: build notes and host-compatibility tricks

This note records the key changes and build-time tricks that were needed to get:

- `simulator/vendor/qemu_8.2.0_ub`

building natively on macOS as `qemu-system-aarch64`, while keeping the UB-related implementation available for ARM64 guest bring-up.

This is not a generic upstream-quality portability note. It is a practical record of:

- what was changed
- why it was changed
- which parts are true platform-support work
- which parts are temporary host-compatibility shims

---

## Scope

These notes are specifically about:

- building on macOS host
- targeting `aarch64-softmmu`
- keeping UB-related code enabled

They are not a statement that every QEMU feature in this tree is fully supported on macOS.

---

## High-level approach

The macOS build path was made to work by combining four strategies:

1. remove an unnecessary build-system gate that disabled `ub`
2. compile only the narrow target we actually need
3. disable or route around host-only features that are irrelevant to our goal
4. add Darwin-side compatibility shims for Linux-specific headers, structs, and symbols that UB/SMMU paths currently assume

---

## Build-system changes

## 1. Allow `ub` on macOS

File:
- `simulator/vendor/qemu_8.2.0_ub/meson.build`

Important change:
- `have_ub` no longer requires `targetos == 'linux'`

Current shape:
- `have_ub = get_option('ub').allowed()`

Related changes:
- `ham_migration` no longer requires Linux
- `urma_migration` still remains constrained by `aarch64`
- `ubmem_vmmu` still depends on `ub` and `aarch64`

Why:
- the old gate disabled the whole UB stack before we even got to real host-compatibility work
- it was too coarse

---

## 2. Narrow build target and disable irrelevant host features

The working build path is intentionally narrow:

- target: `aarch64-softmmu`
- result: `qemu-system-aarch64`

Recommended configure direction:
- disable GUI/audio/network/export/test paths that are not needed for UB bring-up
- avoid pulling in optional subsystems that create unrelated macOS blockers

In practice, the build was driven with a minimized configure setup along the lines of:

```sh
./configure \
  --target-list=aarch64-softmmu \
  --disable-vmnet \
  --disable-coreaudio \
  --disable-cocoa \
  --disable-sdl \
  --disable-gtk \
  --disable-opengl \
  --disable-vnc \
  --disable-tools \
  --disable-slirp \
  --disable-linux-user \
  --disable-bsd-user \
  --disable-docs
```

Notes:
- exact flags can change over time
- the point is to keep the macOS build focused on the one binary we actually need

Working out-of-tree build example:

```sh
rm -rf /tmp/ub-qemu-build-verify
mkdir -p /tmp/ub-qemu-build-verify
cd /tmp/ub-qemu-build-verify
/Volumes/repos/pypto_workspace/simulator/vendor/qemu_8.2.0_ub/configure \
  --target-list=aarch64-softmmu \
  --disable-vmnet \
  --disable-coreaudio \
  --disable-cocoa \
  --disable-sdl \
  --disable-gtk \
  --disable-opengl \
  --disable-vnc \
  --disable-tools \
  --disable-slirp \
  --disable-linux-user \
  --disable-bsd-user \
  --disable-docs
ninja -j8 qemu-system-aarch64
```

---

## libfdt / FDT handling

## 3. Use system `libfdt`

The tree was configured to work with system `libfdt` instead of relying on a more fragile local subproject path during this bring-up.

Why:
- it reduces one layer of build friction on macOS
- it avoids wasting time on FDT build plumbing when the actual goal is UB guest bring-up

Related area:
- `hw/core/sysbus-fdt.c`
- device-tree generation is critical for:
  - `/chosen/linux,ubios-information-table`
  - `ub,ubc`
  - `ub,ummu`

This is not a UB-specific trick, but it was a practical part of getting this tree to build and run cleanly on macOS.

---

## Darwin compatibility shims

## 4. Darwin stub file for missing Linux-only runtime pieces

File:
- `simulator/vendor/qemu_8.2.0_ub/stubs/darwin-link-shims.c`

Purpose:
- provide stub implementations for Linux-specific or otherwise unavailable symbols on macOS so the narrowed build can link

This file currently includes stubs for:
- `gdbstub` entry points
- `iommufd` allocation/query helpers
- `kvm_arm_reset_vcpu`
- coroutine info hooks used by this tree

Why:
- some UB/SMMU paths pull in functions that are valid on Linux but not available on Darwin
- for the current macOS target, these functions are not the core of what we need to validate
- we need the binary to link and run far enough to exercise UB guest bring-up

Important caution:
- these are link-time compatibility shims
- they are not full feature implementations

---

## 5. Darwin-side mock definitions for `iommufd` / `io_uring` / UMMU / SMMU host ABI

File:
- `simulator/vendor/qemu_8.2.0_ub/include/sysemu/iommufd.h`

What was done:
- on non-Linux hosts, this file now provides minimal substitute definitions for:
  - `enum iommu_hw_info_type`
  - `struct iommu_hwpt_pgfault`
  - `struct iommu_hwpt_page_response`
  - `struct iommu_hw_info_arm_smmuv3`
  - `struct iommu_hw_info_ummu`
  - `struct iommu_hwpt_arm_smmuv3`
  - `struct iommu_hwpt_ummu`
  - minimal `io_uring` types
  - related constants like:
    - `IOMMU_HWPT_DATA_UMMU`
    - `IOMMU_VIOMMU_TYPE_UMMU`
    - `IOMMU_HW_INFO_TYPE_UMMU`

Why:
- the UB `UMMU` and ARM SMMU code paths in this tree assume Linux host ABI types exist
- on macOS, these types do not exist
- without these definitions, the code does not compile even in narrowed configurations

Important caution:
- these are compile-time compatibility definitions
- they do not magically make Darwin provide real Linux `iommufd` behavior

---

## 6. Non-Linux fallback in `kvm.h`

File:
- `simulator/vendor/qemu_8.2.0_ub/include/sysemu/kvm.h`

What was done:
- when not on Linux, avoid directly including Linux KVM headers
- provide minimal fallback definitions used by this tree, such as:
  - `MAX_NODES`
  - `MAX_NUMA_NODE`
  - `struct kvm_numa_node`
  - `struct kvm_numa_info`

Why:
- parts of the tree still include `sysemu/kvm.h` from generic code
- macOS host build does not have the Linux KVM userspace ABI headers expected there

---

## 7. Darwin-safe mmap/fs probing path

File:
- `simulator/vendor/qemu_8.2.0_ub/util/mmap-alloc.c`

What was done:
- Linux-specific `fstatfs`/`statfs` probing is now guarded under `CONFIG_LINUX`
- non-Linux paths fall back to neutral behavior instead of trying to use Linux fs magic and APIs

Why:
- host file-system probing code was assuming Linux-specific interfaces
- this was a direct macOS compile blocker

---

## Additional host-compatibility edits

The main macOS bring-up commit also touched a few generic files to reduce Linux/KVM coupling in code that is still pulled into the narrowed build:

- `hw/core/machine-qmp-cmds.c`
- `hw/core/numa.c`
- `include/sysemu/hw_accel.h`

---

## Topology configuration entry point

Static UB topology is now injectable through:

- environment variable: `UB_FM_TOPOLOGY_FILE`
- environment variable: `UB_FM_NODE_ID`

Current behavior:
- if `UB_FM_TOPOLOGY_FILE` is set, `virt` loads that topology snapshot through `ub_fm`
- if it is not set, `virt` falls back to its built-in single-node snapshot
- if `UB_FM_NODE_ID` is set, names like `nodeA.ubcdev0` are mapped to local
  device IDs only when the prefix matches the local node id
- links that do not involve the local node are ignored in the local process

Current file shape:
- INI-style file
- one `[link "..."]` section per point-to-point link
- required keys:
  - `a_device_id`
  - `a_port_idx`
  - `b_device_id`
  - `b_port_idx`
  - `link_up`

Current examples:
- `simulator/vendor/ub_topology_single_node.ini`
- `simulator/vendor/ub_topology_two_node_v0.ini`

Important note:
- endpoint IDs that do not resolve in the current process are treated as `pending`
- this is intentional
- it lets a single-node process carry future-facing remote link declarations without breaking boot
- `stubs/cpu-synchronize-state.c`
- `migration/ham.c`
- `migration/ram-compress.h`
- `target/arm/cpu.c`
- `hw/arm/boot.c`
- `hw/arm/vexpress.c`
- `system/device_tree.c`

These are best understood as support edits to make the narrowed macOS build link and boot, not as the conceptual center of the UB work.

---

## UB-specific bring-up changes that are adjacent to macOS build enablement

These are not just "build tricks", but they were part of the same effort and are worth noting here because they matter to whether the resulting macOS-built QEMU is actually useful.

Key files:
- `hw/arm/virt.c`
- `hw/core/sysbus-fdt.c`
- `hw/ub/ub_ubc.c`
- `hw/ub/ub_ummu.c`
- `hw/ub/ub_ummu_internal.h`
- `hw/vfio/ub.h`
- `include/hw/ub/ub.h`
- `include/hw/ub/ub_ummu.h`
- `include/hw/ub/hisi/ubc.h`

Examples of what was enabled in this area:
- `UBIOS` exposure in DT
- `ub,ubc` and `ub,ummu` nodes
- UBC/UMMU ordering fixes
- minimal HiSilicon/UMMU contract shaping for guest bring-up

These are what turned the macOS-built binary from "builds" into "useful for real ARM64 guest UB bring-up".

---

## Header / ABI mock summary

The most important mock-or-shim areas are:

### Linux-only host ABI substitutes
- `include/sysemu/iommufd.h`
- `include/sysemu/kvm.h`

### Darwin stub symbols
- `stubs/darwin-link-shims.c`

### Linux-only fs probing guards
- `util/mmap-alloc.c`

### AArch64/Linux header additions used by this tree
- `linux-headers/asm-arm64/ptrace.h`

These should be treated as host-portability scaffolding around the UB implementation, not as part of the final UB platform model itself.

---

## Practical build recipe

## Prerequisites

Typical host prerequisites that were useful in practice:
- Homebrew Python
- `distlib` installed for the Python environment QEMU build expected
- system `libfdt`
- Xcode / clang toolchain

## Build flow

1. configure a narrow `aarch64-softmmu` build
2. disable unneeded frontends/features
3. build:

```sh
ninja -j8 qemu-system-aarch64
```

The working output binary used in validation was:
- `qemu-system-aarch64`

Validation was then done with:
- ARM64 Linux kernel
- initramfs
- real Linux `drivers/ub`

---

## What is temporary vs. what is strategic

## Strategic

- removing the coarse `ub` gate in `meson.build`
- keeping the build narrowed to the exact target we need
- preserving the UB/FDT/UMMU/UBIOS changes that make the native macOS-built binary useful for guest bring-up

## Likely temporary or at least pragmatic

- Darwin link stubs in `stubs/darwin-link-shims.c`
- non-Linux mock `iommufd` / `io_uring` ABI definitions in `include/sysemu/iommufd.h`
- various small generic edits that only exist to reduce Linux host coupling during this bring-up

These are acceptable for current progress, but they should not be confused with a complete upstream-quality Darwin port.

---

## Recommended future cleanup

If this tree keeps being used on macOS, the next cleanup steps should be:

1. isolate host-portability shims more cleanly
- keep Linux ABI mock definitions tightly scoped
- avoid spreading Darwin-specific conditionals deeper into UB logic

2. document exact configure flags alongside the repo
- if build flags drift, reproducibility will suffer

3. separate "host portability" from "UB platform behavior"
- build support should remain clearly distinct from platform-model changes

4. keep validating with real guest `drivers/ub`
- build success alone is not enough

---

## Related commits

The macOS-native UB bring-up work started from:

- `2de133842b` `ub: enable macos build and arm64 guest bring-up`
- `0de4bafabd` `ub: realize ummu before ubc device`

Later commits then continued the guest-visible UB topology work on top of that base.
