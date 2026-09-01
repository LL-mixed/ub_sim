# OBMM remote-memory evaluation CLI

- `obmm_async_coroutine.c`: the shared submit/await, async-load, Linux-task
  replay, and baseline validation CLI.
- `uffd_mode.[ch]`: the standard userfaultfd MISSING-mode baseline.
- `uffd_state.[ch]`: the portable per-page generation/state machine.
- `test_uffd_state.c`: the host-runnable state-machine unit test.
- `Makefile`: cross-builds the static AArch64 binary into
  `guest-linux/aarch64/out/obmm_async_coroutine/`.

The CLI owns OBMM export/import setup. `async-poll` and `async-irq` route
split-phase reads and cooperative switches through `libs/obmm_async`.
`async-load` registers its contexts through `libs/obmm_coroutine_scheduler`, then its data
plane uses only ordinary aligned 1/2/4/8-byte scalar loads. All modes share the
same access generator, payload verification, and checksum definition.
`async-load --kernel-task-replay --threads N` selects the Linux process/thread
PoC. Every pthread issues an ordinary scalar load. A remote pending load enters
EL1 through the implementation-defined remote-load data-abort reason, sleeps on
the driver waitqueue, and resumes at the unchanged faulting PC after a CQ event
and IRQ. The repeated load retires through the device PLT replay entry. This
mode does not enter the EL0 coroutine scheduler.
`userfaultfd` uses an anonymous shadow range, a dedicated handler pthread on a
different guest CPU, and only the standard `UFFD_USER_MODE_ONLY`, MISSING,
`UFFDIO_COPY`, and optional `UFFDIO_POISON` contracts. It must not use the
guest kernel's private USWAP or direct-map extensions.
Generated binaries must not be written into this source directory.
