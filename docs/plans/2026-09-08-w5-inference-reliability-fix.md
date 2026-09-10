# W5 inference reliability correction

## Scope and workspace rules

Fix the failures documented in `docs/2026-09-08-w5-inference-n4-910c1-audit.md`.
Keep unrelated in-progress PTO work untouched. Runtime synchronization belongs
in the QEMU UAPI boundary; target preparation and presentation belong in
`crates/sim-console/`. QEMU lock helpers and native unit tests follow the
existing `include/hw/ub/` and `tests/unit/` layouts. Generated test inputs,
packs, build outputs, and logs stay under existing `out/` and build directories.

## Contracts

- Restore the September 10 target baseline first: align the n4-910c1 source
  cache and link the pinned Memory Service managed allocation and provider
  directory implementations into guest binaries. Preserve concurrent GVA
  manager edits. Validate OBMM pool two-node runs on both targets before W5.

- One caller owns each mutable Rust bridge and its UAPI queue transaction.
  An owner may release the BQL for PTO callbacks. A contender must release the
  BQL before waiting for ownership, then reacquire it before accessing device
  state. Nested calls on the owner thread are allowed. Reset and destruction
  must respect ownership too.
- Simpler readiness includes the exact `pto_isa.pin` read from the committed
  Simpler revision. Preparation supplies its complete tree and carries it into
  managed worktrees. Browser input cannot choose dependency paths or revisions.
- W5 per-step PASS remains progress until all requested steps and final run
  validation complete. Host compiler/runtime errors must be available in both
  CLI and WebUI diagnostics, without copying unbounded host logs.
- Final KV publication validation must accept the pinned Memory Service's
  explicit payload mode and copy-byte fields, while retaining checksum,
  nonempty payload, publication sequence, and success requirements.

## Validation and delivery

Use the project QEMU build wrapper and its native-test option. Add lock tests
for concurrent ownership, nested entry, and BQL callback progress; retain real
Qwen and DeepSeek guest reproductions as integration regressions. Add console
tests for pin preparation, incomplete rounds, and host error extraction.
Run full Rust/Python suites and QEMU native tests on n4-910c; execute the
W5 model matrix on n4-910c1. Keep resource-intensive validation remote.
Repeat all six default W5 demos, Qwen eight-node 32 steps, and the affected
PTO callback path. Record exact revisions and test results in the audit report.
Keep commits limited to these corrections, with English messages and a
Co-authored-by trailer; do not push. WebUI remote runs require committed source.
