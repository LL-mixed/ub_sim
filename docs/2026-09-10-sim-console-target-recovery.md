# Sim Console target and W5 recovery

## Confirmed failures and corrections

- The n4-910c1 source cache selected Memory Service `c0f01389e276` while
  root revision `e6c15afc0798` required `c042d664ed2e`. Target preparation
  aligned the cache and restored all 20 environment-readiness checks.
- The n4-910c OBMM pool run `sim-console-1789030427184-0` failed during
  shared initramfs construction, before guest launch. The guest link commands
  omitted `mem_service_allocation.c` and `mem_service_provider_directory.c`.
  Root commit `d247f40` adds both implementations to all four metadata
  consumers, W5 link checking, and initramfs input signatures.
- W5 bridge polling releases the BQL for synchronous PTO callbacks. Other
  vCPU accesses could concurrently mutate the same Rust bridge and completion
  queue. QEMU commit `fd046b39a335` retains per-device recursive ownership
  across the full queue transaction. Contenders release the BQL before
  waiting, allowing callbacks and the current owner to make progress.
  Root commit `27b3678` pins this QEMU change and builds its native tests.
- Root commit `a6c71a0` prepares and verifies the exact committed Simpler
  `pto_isa.pin` separately from the root PTO-ISA gitlink. It transfers the
  pinned checkout into managed workspaces, treats W5 per-step PASS as
  progress until final success, and appends bounded QEMU host diagnostics to
  the process log on failure.
- Root commit `cd231f0` updates the Qwen final KV publication matcher for
  `payload_mode` and `publication_copy_bytes`. The first repaired 32-step
  run completed all 256 range forwards and generated 32 tokens, then failed
  the old matcher despite valid publication records. The regression accepts
  legacy, copy, and in-place records and rejects invalid modes, empty KV,
  and failed publication status.
- Root commit `db0f253` reuses private SSH control sockets and retries only
  explicit pre-authentication banner timeouts. `df64580` kills timed-out
  asynchronous collectors/probes, caps each host log at 8 KiB, and adds SSH
  keepalive detection. Workloads are never replayed after ambiguous disconnects.
  A native child-process regression verifies cancellation actually kills the
  probe. n4-910c reported 18 minutes of uptime at 20:20 on September 10;
  the jump host reboot interrupted an active DeepSeek eight-node run.

## Actual target validation

| Target | Workload | Run | Result |
| --- | --- | --- | --- |
| n4-910c | OBMM pool, 2 nodes | `sim-console-1789030901854-1` | PASS, exit 0, 93.2 s |
| n4-910c1 | OBMM pool, 2 nodes | `sim-console-1789031047806-2` | PASS, exit 0, 147.6 s |
| n4-910c1 | Qwen, 8 nodes, 32 steps | `sim-console-1789031322480-3` | Runtime completed 32 steps; old KV publication matcher failed, exit 1 |
| n4-910c1 | W5 Qwen PP / 2 Nodes, 2 steps | `sim-console-1789032982223-0` | PASS, exit 0, 196.2 s |
| n4-910c1 | W5 Qwen PP / 4 Nodes, 2 steps | `sim-console-1789033197581-1` | PASS, exit 0, 103.0 s |
| n4-910c1 | W5 Qwen PP / 8 Nodes, 2 steps | `sim-console-1789033315941-2` | PASS, exit 0, 139.7 s |
| n4-910c1 | W5 DeepSeek V4 Flash PP / 2 Nodes, 2 steps | `sim-console-1789033465772-3` | PASS, exit 0, 524.8 s |
| n4-910c1 | W5 DeepSeek V4 Flash PP / 4 Nodes, 2 steps | `sim-console-1789034001716-4` | PASS, exit 0, 542.7 s |
| n4-910c1 | W5 DeepSeek V4 Flash PP / 8 Nodes, 2 steps | `sim-console-1789034558762-5` | SSH banner timeout before guest launch |
| n4-910c1 | W5 DeepSeek V4 Flash PP / 8 Nodes, 2 steps | `sim-console-1789040413464-0` | SSH launcher exit 255 during jump-host outage; worker exit 1 |
| n4-910c1 | Qwen, 8 nodes, 32 steps | `sim-console-1789041833309-1` | PASS, exit 0; 256 range forwards and 32 tokens |
| n4-910c1 | W5 DeepSeek V4 Flash PP / 8 Nodes, 2 steps | `sim-console-1789042965461-0` | PASS, exit 0, 455.7 s |

Both OBMM runs used `d247f40`, completed two pool rounds and 20 stress
iterations, and reached final launcher success. Readiness alone is an
environment prerequisite check; these rows record actual workload results.
The final DeepSeek eight-node run used `df64580`; the Qwen 32-step run used
`db0f253` and took 1,053.9 seconds. All six default W5 demos now have complete
successful runs, as does the reported Qwen eight-node 32-step configuration.
Final readiness probes returned 20/20 ready demos for both targets.

## Regression checks

- On n4-910c, the final isolated `df64580` checkout passed
  `cargo test --workspace --target-dir /home/ll/sim-console/ub_sim/target`
  with exit 0; the guest Python unittest discovery passed 411 tests.
  The separate output streams of isolated native Rust tests interleave, so
  the workspace exit status is the authoritative full-suite result.
- QEMU was built on n4-910c using the repository wrapper with
  `--with-obmm-tests`. All five selected native test executables passed
  (31 cases).
  The new lock tests cover 80,000 operations across eight concurrent owners,
  recursive ownership, and progress of a BQL callback while another caller
  waits for bridge ownership.
- Local lightweight console tests at `df64580` passed 43 tests (41 library,
  two CLI). The console build passed. The visual fixture
  completed desktop and mobile catalog, launch, and result views.
- The final pytest collection ran 531 tests: 525 passed and six failed.
  Failures concern existing application inventory, build-matrix inventory,
  a hardcoded Memory Service package checksum, a pre-existing document name,
  the async-map contract, and Python 3.9 lacking `zip(strict=True)`.
  All six also failed in an independent checkout of pre-repair `e6c15af`;
  `out/repair-pytest-baseline.log` records the identical baseline failures.

Logs and run records are under `out/sim-console/`; remote suite logs are
under the managed repository's `out/`. All model, QEMU, integration, and full
suite execution took place on the remote targets. Existing unrelated GVA
manager work was preserved. No Git push was performed.

## Final state

All requested model configurations passed final validation, both targets
returned 20/20 ready demos, and no QEMU guests remained on n4-910c1 after
the last run. The running console includes `df64580`. Rust and unittest
full-suite validation passed; pytest retains exactly the six baseline
failures listed above. No claim is made that the repository's entire pytest
suite is green or that these single successful runs establish indefinite
runtime stability.
