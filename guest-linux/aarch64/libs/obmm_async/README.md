# libobmm_async layout

- `obmm_async.h`: public EL0 API; it must not expose SIM_DEC or a transport.
- `obmm_async.c`: queue, future, buffer, and coroutine scheduler runtime.
- `obmm_async_aarch64.S`: AAPCS64 context switch; only callee-saved state is
  switched at cooperative suspension points.
- Tests for ABI/layout and runtime behavior live in
  `guest-linux/aarch64/tests/`; generated binaries stay under `out/`.

The UAPI source of truth is `kernel_ub/include/uapi/ub/obmm_async.h`. Public
tokens are `(generation, queue_id, slot)` values, never pointers.

`obmm_async_map_register_strict_pto()` uses V2 with an explicit strict-PTO
flag. It requires a retained mapped view and derives access rights from every
page table entry in the requested range. OBMM clears VMA read/write flags after
installing its page protections, so those flags cannot supply the rights.
On AArch64, OBMM also installs writable PTEs by clearing the hardware AP
read-only bit without setting Linux's software-write/DBM bit. Registration
therefore checks valid user access and the actual hardware read-only bit.
Strict registration checks the OBMM region identity and contiguous physical
pages, and intersects the per-page permissions with the device route rights.
It currently accepts only page-granular O_SYNC mappings, whose permissions
cannot change through OBMM ownership updates. Huge mappings and cacheable
ownership-managed views return unsupported. Registration fails on older
driver/device implementations without retrying
legacy registration. The returned registration only supports PTO; submitting
it to the legacy async read queue returns unsupported. The caller keeps the
original mapping and registration until all PTO work has completed.
