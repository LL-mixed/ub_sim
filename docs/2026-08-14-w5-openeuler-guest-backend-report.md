# W5 openEuler Guest Backend — Migration Validation Report

**Date:** 2026-08-14
**Scope:** `SIM_W5_GUEST_BACKEND=openeuler` — running the 8-node W5 QEMU cluster
on an openEuler 24.03 rootfs (qcow2 + systemd) instead of the busybox cpio
initramfs. Default backend remains `busybox`; openEuler is opt-in per run.
**Host:** n4-910c (aarch64, openEuler, 2 TB RAM)

## 1. What was built

| Piece | File | Role |
|---|---|---|
| Rootfs builder | `guest-linux/aarch64/scripts/build_openeuler_guest.sh` | Produces `out/openeuler_guest/rootfs.qcow2` (32G virtual) from the `openeuler-2403:v0.0.4` docker image, bakes static tools + `.ko` modules + `linqu-w5-guest.service`, and a small busybox initramfs that insmods virtio/ext4/9p modules then `switch_root`s to systemd on `/dev/vda`. |
| Launcher wiring | `guest-linux/aarch64/scripts/launch_ub_eight_node_headless.sh` | `SIM_W5_GUEST_BACKEND=openeuler` switches each node to `-drive qcow2 overlay + three 9p virtfs shares (w5env/w5tools/w5modules) + -initrd openeuler_guest/initramfs.cpio.gz`. Per-node qcow2 overlays on the read-only base give each QEMU process its own write lock. |
| Per-run guest env | `guest-linux/aarch64/scripts/run_llm_infer_eight_node_guest.sh` (`write_w4_openeuler_env`) | Writes `env.sh` + `runner.sh` into the 9p `w5env` source; the guest's `linqu-w5-guest.service` → `run_app_wrapper` sources `env.sh` and execs `runner.sh` (same role dispatch, ipourma bring-up, serving queue, and `linqu_llm_infer` invocation as the busybox initramfs runner). |
| Container passthrough | `guest-linux/aarch64/scripts/run_w5_in_container.sh` | Forwards `SIM_W5_GUEST_BACKEND` and the `SIM_W5_OPENEULER_*` overrides into the W5 container. |

Usage:

```bash
./guest-linux/aarch64/scripts/build_openeuler_guest.sh --rootfs-source docker
SIM_W5_GUEST_BACKEND=openeuler ./guest-linux/aarch64/scripts/run_w5_in_container.sh -- w5.env
```

## 2. The stall this work was blocked on

Symptom (15/15 deterministic runs on 2026-08-14, all 8 nodes): each guest boots,
the OBMM 2MB pool reaches `pool ready -> ok nodes=8`, `linqu_llm_infer` starts,
prints `stage qwen3_sampler_config`, completes the 7 remote 1GB datastore
imports (`OBMM import mapped map_id=0xe`) — then the whole VM goes silent.
No kernel printk, no RCU stall (timeout was 300 s), no OOM/segfault. The
in-guest periodic dumper survived ~10 s past the last kernel print, then died.
The host harness timed out after ~19 min: `FAIL: w4 guest did not pass on all
nodes rc=2`.

Investigation trail:

- QEMU was exonerated by `gdb -p` on all frozen instances: main loop in
  `ppoll`, vCPU threads in `qemu_wait_io_event` — i.e. the VMs were *idle*
  (WFI), not deadlocked. The guest workload processes were already dead.
- Kernel printk (which cannot be lost to tty contention) showed each node's
  pool maps being freed under the pool PID with **none** of the pool's own
  cleanup prints — the pool was terminated by a signal mid-`do_rounds`,
  precisely when each guest's serial login banner appeared.
- The openEuler image shipped **three** tty consumers racing the W5 service on
  `ttyAMA0`: the builder-baked `serial-getty@ttyAMA0` autologin unit,
  `console-getty` (on `/dev/console` == `ttyAMA0`), and the base image's
  `getty@tty1`. Getty startup performs a terminal hangup/vhangup and session
  takeover; the W5 service processes (runner → `linqu_llm_infer` → pool),
  which hold `ttyAMA0` as their stdout/stderr tty, were killed/stopped as each
  node's getty started. `linqu_llm_infer`'s later output stages never printed
  because the processes were already gone; the VM then sat idle with nothing
  to run. Userspace prints were additionally lost/interleaved from the raw
  tty contention, which is what made the stall look like a kernel/ipourma
  hang. (Root password in the base image is locked — `root:*` — so the login
  prompt was useless even interactively.)

## 3. Fix

`build_openeuler_guest.sh` no longer installs any getty. It masks all three
units at the `/etc/systemd/system` level (symlinks to `/dev/null`, which
outranks `getty.target.wants` and the runtime getty-generator):

```
ln -sf /dev/null /mnt/etc/systemd/system/serial-getty@ttyAMA0.service
ln -sf /dev/null /mnt/etc/systemd/system/console-getty.service
ln -sf /dev/null /mnt/etc/systemd/system/getty@tty1.service
```

plus removal of the base image's `getty.target.wants` entries. The W5 guest
service is now the sole owner of `ttyAMA0`; its stdout/stderr arrive
completely in the per-node serial logs. Note `systemd.mask=` on the kernel
command line was tried first and did **not** stop the gettys on this image —
the image-level mask is the reliable mechanism.

The one-off surgical fix applied to the existing `rootfs.qcow2` (via
`qemu-nbd` in a privileged container) validated the root cause immediately;
the builder change makes it reproducible.

## 4. Validation

| Run | Backend | Result |
|---|---|---|
| 2026-08-14_07-29-16 (`qwen3_0_6b_decode`, 8 nodes, 2 steps) | openEuler (surgical image fix) | **PASS** — `[w4guest8] PASS: W5 inference cluster nodes=8 profile=qwen3_0_6b_decode`; `terminal_tokens: [264, 3644]`, `generated_text_lossy: " a global"`; full pool rounds 1–8 + datastore imports + mem-service bootstrap + decode completed. |
| Rebuild validation (builder-baked rootfs, reverted debug pool, cleaned runner) | openEuler | see §5 |
| busybox regression (default backend) | busybox | see §5 |

The trailing `guest_worker_summary: pass=false ... Error: qwen3 guest decode
worker failed ... pass=false` line is a **known false positive** that equally
affects busybox runs (documented in `W5_8NODE_QWEN14B_16STEP_PP_REPORT.md`
§6): sim-cli greps for `eight-node w5 inference cluster` while the runner
prints `8-node W5 inference cluster`. It is not an openEuler regression.

Contract tests: `guest-linux/aarch64/tests/test_openeuler_guest_backend.py`
(10 cases: builder CLI, getty masking regression guard, no-autologin guard,
launcher backend validation + qemu wiring, runner env/runner generation, no
leftover ipourma dump instrumentation, container env passthrough).

## 5. Post-cleanup validation

Full clean-slate sequence executed after the fix was baked into the builder,
the debug-instrumented pool binary was reverted to committed source and
rebuilt, and the ipourma periodic-dump instrumentation was removed from the
openEuler runner:

| Step | Result |
|---|---|
| `build_initramfs.sh` (guest tools + busybox initramfs, reverted pool) | ok; `out/linqu_ub_obmm_pool` rebuilt from committed source |
| `build_openeuler_guest.sh` full rootfs rebuild | ok; verified via `qemu-nbd` ro-attach: all 3 getty units → `/dev/null`, `getty.target.wants` empty, clean pool binary baked into `/bin` |
| 8-node openEuler run `2026-08-14_08-14-54` | **PASS** — `terminal_tokens: [264, 3644]`, identical to the pre-cleanup surgical-fix run |
| 8-node busybox regression run `2026-08-14_08-24-01` (default backend, `rdinit=/bin/run_app`) | **PASS** — `terminal_tokens: [264, 3644]`, byte-identical to the openEuler backend |
| Contract tests `test_openeuler_guest_backend.py` | 10/10 OK |

Both backends emit the same benign `qwen3 Object Service payload index is
missing` warning (present in every run, artifacts intact) and the same known
sim-cli `pass=false` tail false positive. No leftover QEMU processes after
either run.

## 6. Known limitations / follow-ups

- `systemd.mask=` on the kernel command line is ignored by this image; mask
  gettys in the image instead (the builder does).
- Rootfs build takes ~30–60 min and ~23 GB in `out/openeuler_guest/`; the
  `.build_signature` skip path makes reruns cheap when inputs are unchanged.
- The sim-cli `eight-node` pass-marker false positive (§4) predates this work.
- Single-node debugging entry point: `out/openeuler_guest/run_openeuler_guest.sh`
  (needs the container toolchain env; qemu-img on the host lacks liburing).
