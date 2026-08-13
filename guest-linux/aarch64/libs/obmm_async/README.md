# libobmm_async layout

- `obmm_async.h`: public EL0 API; it must not expose SIM_DEC or a transport.
- `obmm_async.c`: queue, future, buffer, and coroutine scheduler runtime.
- `obmm_async_aarch64.S`: AAPCS64 context switch; only callee-saved state is
  switched at cooperative suspension points.
- Tests for ABI/layout and runtime behavior live in
  `guest-linux/aarch64/tests/`; generated binaries stay under `out/`.

The UAPI source of truth is `kernel_ub/include/uapi/ub/obmm_async.h`. Public
tokens are `(generation, queue_id, slot)` values, never pointers.
