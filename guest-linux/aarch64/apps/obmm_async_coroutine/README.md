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
EL1 through the implementation-defined remote-load data-abort reason. The
completion-driven mode sleeps on the driver waitqueue and resumes at the
unchanged faulting PC after a CQ event and IRQ. When either void-response policy
is enabled, the handler keeps the task runnable, calls `schedule()`, and returns
through `ERET`; the repeated load issues a fresh UB transaction. The source UBC
drops and retires the old transaction's late real response without CQ/IRQ.
Normal NC and Normal Cacheable both use this tokenless replay contract. A
successful Cacheable replay fills the ordinary 64-byte cache line. Linux-task
mode does not enter the EL0 coroutine scheduler.

Producer/consumer performance runs may assign multiple remote loads to every
coroutine or pthread. `--iterations` must be a multiple of `--coroutines` or
`--threads`. Use `--async-load-event-log off` for timed runs. In direct-EL0
mode this installs no coroutine trace callback, so the timed hot path performs
no causal event lookup, formatting, buffering, or output. In Linux-task mode it
also sets the driver's `remote_load_event_log` module parameter to zero.
Aggregate mechanism counters, per-load latency samples, checksum, replay
exact-once checks, and cleanup gates remain active.

Run the paired comparison through:

```text
guest-linux/aarch64/scripts/run_ub_async_load_scheduler_compare.py \
  --scenario-config scenarios/mvp_2host_async_load_remote_10ms.yaml \
  --base-model-manifest out/.../remote_memory_model_manifest_v1.json \
  --output-dir out/obmm-remote-load/scheduler-compare-<run-id>
```

The comparison fixes both paths to 8-byte ordinary loads, replay retirement,
the same model manifest, the same seed, four contexts by default, and zero
per-event logging. It emits raw runner logs plus paired CSV/JSON summaries and
fails if checksums or artifact fingerprints differ within a pair.

`userfaultfd` uses an anonymous shadow range, a dedicated handler pthread on a
different guest CPU, and only the standard `UFFD_USER_MODE_ONLY`, MISSING,
`UFFDIO_COPY`, and optional `UFFDIO_POISON` contracts. It must not use the
guest kernel's private USWAP or direct-map extensions.
Generated binaries must not be written into this source directory.
