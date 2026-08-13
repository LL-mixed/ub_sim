# obmm_async_coroutine layout

- `obmm_async_coroutine.c`: the shared P2A/P2B/P4 validation CLI and workload.
- `uffd_mode.[ch]`: the P4 standard userfaultfd MISSING-mode baseline.
- `uffd_state.[ch]`: the portable per-page generation/state machine.
- `test_uffd_state.c`: the host-runnable state-machine unit test.
- `Makefile`: cross-builds the static AArch64 binary into
  `guest-linux/aarch64/out/obmm_async_coroutine/`.

The CLI owns OBMM export/import setup. `async-poll` and `async-irq` route
split-phase reads and cooperative switches through `libs/obmm_async`.
`scheduler-core` registers its contexts through `libs/obmm_scc`, then its data
plane uses only ordinary aligned 1/2/4/8-byte scalar loads. All modes share the
same access generator, payload verification, and checksum definition.
`userfaultfd` uses an anonymous shadow range, a dedicated handler pthread on a
different guest CPU, and only the standard `UFFD_USER_MODE_ONLY`, MISSING,
`UFFDIO_COPY`, and optional `UFFDIO_POISON` contracts. It must not use the
guest kernel's private USWAP or direct-map extensions.
Generated binaries must not be written into this source directory.
