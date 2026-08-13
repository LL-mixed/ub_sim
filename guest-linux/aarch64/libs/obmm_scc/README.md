# `libobmm_scc` directory contract

This directory contains the guest EL0 scheduler runtime for the P2B
direct-upcall experiment.

- `obmm_scc.h` exposes setup, teardown, context, map, run, and metrics
  operations, but never a per-load API.
- `obmm_scc.c` owns the EL0 context store, ready/wait/fault state, round-robin
  policy, completion commit, and `/dev/linqu-scc0` control ioctls.
- `obmm_scc_aarch64.S` saves the interrupted full AArch64 state, switches to
  the dedicated scheduler stack, and invokes the simulated resume primitive.
- QEMU is a mechanism provider only: direct EL0 PC redirection and atomic
  installation of the context selected by this library. QEMU must not own a
  coroutine context store or scheduling policy.
- Normal payload reads remain ordinary AArch64 scalar `LDR` instructions in
  the application. Transport and provider names must not appear here.
- The `Makefile` writes generated objects and archives under
  `guest-linux/aarch64/out/obmm_scc/`; generated files never belong here.
