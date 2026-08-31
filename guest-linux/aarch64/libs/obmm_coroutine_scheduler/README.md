# `libobmm_coroutine_scheduler` directory contract

This directory contains the guest EL0 scheduler runtime for the async-load
direct-upcall experiment.

- `obmm_coroutine_scheduler.h` exposes setup, teardown, context, map, run, and metrics
  operations, but never a per-load API.
- `obmm_coroutine_scheduler.c` owns the EL0 context store, ready/wait/fault state,
  round-robin policy, event-ring consumption, completion commit, and
  `/dev/linqu-async-load0` setup/teardown ioctls.
- `obmm_coroutine_scheduler_aarch64.S` saves the interrupted full AArch64 state, switches to
  the dedicated scheduler stack, and invokes the simulated resume, wait, and
  scheduler-enter assists.
- ABI v3 maps a QEMU-produced event ring read-only and an EL0-owned consumer
  page read-write. From `START` through scheduler completion, event delivery,
  acknowledgement, all-blocked wait/wakeup, and context selection execute
  without an ioctl, poll, futex, or signal hot path.
- QEMU provides direct EL0 PC redirection, descriptor-first event publication,
  vCPU wait/wakeup, and atomic installation of the context selected by this
  library. The coroutine context store and scheduling policy remain in EL0.
- Normal payload reads remain ordinary AArch64 scalar `LDR` instructions in
  the application. Transport and provider names must not appear here.
- Patch completion writes the returned value and next PC into the saved EL0
  context. Replay completion marks the context ready to re-execute its `LDR`;
  both modes use the same ABI v3 event ring and assists.
- The `Makefile` writes generated objects and archives under
  `guest-linux/aarch64/out/obmm_coroutine_scheduler/`; generated files never belong here.
