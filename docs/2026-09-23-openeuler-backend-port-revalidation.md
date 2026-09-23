# openEuler Guest Backend Port — n4-910c Re-validation

**Date:** 2026-09-23
**Scope:** Re-validation of the openEuler guest backend after porting
`43d03177` onto current master (`c1ee0d44`), following the rebase that
temporarily dropped it. Machine: n4-910c (branch `port-openeuler-validation`).

## Results

| Validation | Result | Evidence |
|---|---|---|
| 8-node interactive shell cluster (`boot_ub_eight_node_shell.sh up`) | **PASS** | 8/8 nodes logged `[shell-mode] node up: hostname=ub-node-N ub_ip=10.0.0.N`; host-side serial injection executed commands in nodeA/nodeH; `/dev/ub_npu0` `/dev/ub_ssd0` `/dev/ubagg` present in guest; `/run/w5/modules` 9p share mounted |
| W5 `qwen3_0_6b_decode`, 8 nodes × 2 steps, `SIM_W5_GUEST_BACKEND=openeuler`, `--legacy-serving` | **PASS** | `guest_worker_summary: pass=true range_forwards=16 runtime_inputs=15 runtime_outputs=16`, `terminal_tokens: [264, 3644]` (run `2026-09-22_13-48-59`) |
| Same config, busybox backend (control) | **PASS** | `terminal_tokens: [264, 3644]` — byte-identical to the openEuler backend and to the 2026-08-14 reference (run `2026-09-22_12-54-08`) |

## Port fixes required by the current n4 environment

Committed on top of the port:

- `afa68bd6` — the prebuilt QEMU on n4 was rebuilt with GTK enabled; the
  shell container must install libepoxy/gtk3/cairo/gdk-pixbuf2/libX11 in
  addition to pixman/liburing.
- `7683f0dd` — the prebuilt QEMU links the host's `liburing.so.1`, but the
  openEuler 24.03 image only ships liburing 2.4 (`liburing.so.2`); the boot
  script now copies the host's `.so.1` into the container when missing.
- `526599e7` — the launcher now defaults to a per-launch unique shared dir
  (invisible to the host); the boot script pins `UB_FM_SHARED_DIR` to the
  bind-mounted path so `status`/`attach` work.

## Environment staleness found on n4 (not port bugs)

Each produced a misleading error far from its cause:

1. Stale `target/release/sim-cli` reused blindly (existence-only check) —
   broke `managed-model-provider-fingerprint` with a "scenario config" error.
2. `make Image` does not rebuild modules; the Sep 7 kernel ran with Aug 20
   `.ko`s → modversions CRC mismatch → 9p modules rejected → `/run/w5/env`
   empty. Fixed by `make modules` + full `build_openeuler_guest.sh` rebuild.
3. Stale `out/kernel_uapi` (installed UAPI headers) broke guest tool builds
   (`obmm_cmd_gsva_export_segment_v1` missing). Fixed by `headers_install`.
4. Stale `out/guest_tools/` — nothing refreshes it; the rootfs baked an
   Aug 20 `linqu_llm_infer` that lacks markers the current validator expects.
5. Stale `out/initramfs.cpio.gz` — old mem_service binary rejected the new
   `listen` config field. Fixed by rebuilding with `build_initramfs.sh`.

## Known upstream issue (out of scope)

Managed serving (`SIM_W5_MANAGED_SERVING=1`, the current default) fails on
n4 at the `canary-exchange` startup stage on **both** backends. Both runs
above used `--legacy-serving`. Needs separate investigation.
