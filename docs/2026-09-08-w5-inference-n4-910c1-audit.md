# W5 inference verification on n4-910c1

This is the September 8 pre-repair audit. Subsequent fixes and retests are
recorded in [the September 10 recovery report](2026-09-10-sim-console-target-recovery.md).

## Outcome

Ten new remote runs covered all six catalog demos, three dependency-repair
retests, and the reported Qwen eight-node 32-step configuration. All used
revision `8ebd7c96624df90af23a6a5c1ec0113d3d8ad68d`.

- Qwen two-node and four-node default two-step runs passed.
- Qwen eight-node failed both the fresh two-step and fresh 32-step runs.
- All three DeepSeek demos initially failed on an unavailable pinned PTO-ISA
  dependency. After repairing only its generated cache, two-node and four-node
  runs passed. Eight-node failed with a QEMU double free in completion handling.
- Four demo configurations have a complete default two-step pass in this audit.
  Both eight-node demos remain broken in the tested configuration. Shared
  runtime code also means a single two/four-node pass is not a stability proof.
- The WebUI overstates per-node completion and hides the detailed host-side
  compiler failure behind a shortened guest error.

## Scope and evidence rules

- Verify all six W5 Inference catalog entries using the running WebUI backend
  API and its registered remote target, starting with the default two steps.
- Recheck the reported Qwen eight-node 32-step failure separately.
- Execute model workloads only on the remote target, one run at a time.
- Keep existing working-tree changes untouched; record the committed revision
  selected by sim-console for each run.
- Store generated evidence in the existing `out/sim-console/runs/<run-id>/`
  layout. This document contains findings and links, not copied bulk logs.
- Require terminal exit status, complete decode counts, and guest evidence for
  a pass. A per-step PASS marker does not prove whole-run success.
- Distinguish reproduced failures, historical evidence, and hypotheses.

## Baseline

The current committed revision is
`8ebd7c96624df90af23a6a5c1ec0113d3d8ad68d`.

The reported run `sim-console-1788871219146-2` requested Qwen eight-node
32-step inference. It completed one token (264) and then Node A timed out
waiting for four completions, with only three available:
`cq_tail=18 base=15 distance=3 expected=4 timeout_ms=900000`.
The init process subsequently exited and triggered the displayed kernel panic.
The immediately preceding eight-step run `sim-console-1788871077120-1`
completed 8/8 steps and 8/8 nodes on the same revision and target.

## Original-environment results

All following runs used the existing WebUI API, target `n4-910c1`, the current
revision above, and `steps=2`.

| Demo | Run ID | Result | Evidence |
| --- | --- | --- | --- |
| Qwen / 2 nodes | `sim-console-1788872502162-3` | PASS, 37.1 s | 2/2 steps, 2/2 nodes, tokens 264 and 3644 |
| Qwen / 4 nodes | `sim-console-1788872567243-4` | PASS, 41.0 s | 2/2 steps, 4/4 nodes, tokens 264 and 3644 |
| Qwen / 8 nodes | `sim-console-1788872613260-5` | FAIL, 43.0 s | One token; Node A QEMU exited during second round |
| DeepSeek / 2 nodes | `sim-console-1788872659389-6` | FAIL, 96.8 s | First matmul artifact producer failed |
| DeepSeek / 4 nodes | `sim-console-1788872760515-7` | FAIL, 75.6 s | Same pinned PTO-ISA acquisition failure |
| DeepSeek / 8 nodes | `sim-console-1788872841787-8` | FAIL, 161.9 s | Same pinned PTO-ISA acquisition failure |

## Faults identified

### Qwen completion processing

The reported 32-step failure and the fresh default two-step failure both reach
the second decode round. The reported run consumed four commands but delivered
only three completions. The fresh run submitted four commands, then logged
completions with op IDs `5, 7, 6, 5, 6, 8`: two duplicate completions before the
QEMU process exited. The two-step run disproves a failure confined to 32 steps.

Source inspection finds an unsafe concurrency boundary:

- `hw/ub/ub_ubc.c:linqu_uapi_poll_completion` releases the QEMU global lock
  before entering `linqu_ub_bridge_poll_completion`.
- Both doorbell processing and guest CQ-tail/status reads call
  `linqu_uapi_flush_cq`, allowing overlapping calls while the lock is released.
- The Rust bridge reconstructs an exclusive mutable reference from the same
  raw pointer, then mutates runtime and completion queues without a bridge
  synchronization guard.

The observed overlapping flush calls and duplicate/missing completions are
consistent with this race. A controlled correction and rerun are still needed
to establish that it accounts for every observed QEMU exit. No core dump or
host signal evidence was obtained for the fresh QEMU exit.

The reported run also has one SIM_DEC remote-read timeout. It subsequently
resolves the terminal token and reads KV state successfully, so that timeout
alone does not explain the missing completion.

The existing
`test_qemu_releases_bql_while_waiting_for_pto_worker_callbacks` in
`guest-linux/aarch64/tests/test_lingqu_shmem_pto_abi.py` checks source-call
ordering. It does not exercise simultaneous bridge entry or enforce exactly
one completion per submitted command. A correction needs a runtime regression
covering overlapping doorbell/poll operations and both missing and duplicate
completions, while preserving worker callbacks that must acquire the QEMU lock.

The fresh 32-step run `sim-console-1788874263591-12` failed after three tokens
`[264, 3644, 7653]`, during step 3 (the fourth step). Node A reported
`model range compute contract completion sideband missing`. Its first batch
of four commands advanced CQ tail from 47 to 54, indicating seven completions
where four were expected. Later the guest reached its total completion count
without the actual model-dispatch sideband, and init exited with code 1.

The repaired-dependency DeepSeek eight-node run
`sim-console-1788874177273-11` exposed the same unsafe area on Node B. During
completion handling, its host log contains repeated op ID 2, corrupted error
text, and `free(): double free detected in tcache 2`, followed by QEMU exit.
This is direct evidence of a host runtime memory-management failure, beyond
the initial dependency issue and the secondary guest kernel panic.

### Earlier Qwen output-publication failure

`sim-console-1788870621538-0` ran revision `ec01ba1`, before the GSVA route
opt-in added by `8ebd7c9`. Its first hidden-output UB-SSD write returned device
status `-4` (`SSD_ERR_BAD_DESCRIPTOR`), surfaced as `backend_attach=internal`.
The current-revision two-node rerun passed the complete default workload.

### DeepSeek pinned build dependency

The first DeepSeek two-node host log shows that Simpler requires PTO-ISA commit
`594817f22e7240d769d6febad162679ad4816133` in
`vendor/simpler/build/pto-isa`. GitHub clone attempts and the GitCode fallback
could not check out that commit. The artifact producer raises
`OSError: PTO-ISA not available`, and the guest receives
`simpler_host_matmul_artifact_producer_failed`.

The repository's top-level PTO-ISA gitlink is a different revision. Checking
that top-level checkout and GCC 15 does not establish readiness of Simpler's
managed pinned dependency. The exact required commit and complete tree exist
locally. After all six original-environment runs finished, a complete checkout
pack was imported into the absent remote managed cache. The cache is clean at
the exact required commit, tree `47c1d24181531d8a836a98f76bb1d764f8cc54bd`.
Both copies of the pack have SHA-256
`14734132c96cf79d535ebfad79fc908d7c261932f6bb3aa580bf5ec65b60003a`.
Only generated build-cache contents changed; root and submodule source
revisions were retained. DeepSeek retests use the same WebUI API and parameters.

This repairs the current machine's cache. A durable readiness/preparation fix
must also provision the exact Simpler pin from a fetchable source or verified
checkout pack; a fresh target cannot depend on this manual cache recovery.

### WebUI node status

`crates/sim-console/src/runner.rs:classify_node_log` treats any
`[w4_guest] pass` in its log tail as node PASS. W5 emits that marker per decode
step. Thus B through H were displayed as passed after only step zero, while
the cluster still needed 31 more steps. Run-level failure was correctly
reported, but the per-node completion presentation overstated progress.

DeepSeek compiler errors are logged in the remote QEMU log. The WebUI mirrors
guest logs, which contain only the shortened artifact-producer failure code.
First-run operator compilation similarly appears as 0% guest progress, with
the actual build activity visible in the host log. Surface a bounded host
failure excerpt and distinguish build activity from decode progress.

## Evidence files

- Original matrix: `out/sim-console/w5-audit-20260908.json`.
- Retest matrix: `out/sim-console/w5-audit-20260908-retest.json` (written as runs
  finish).
- Each run retains `run.json`, `process.log`, and mirrored guest logs in
  `out/sim-console/runs/<run-id>/`.
- Additional host-side excerpts are in
  `out/sim-console/<run-id>-nodeA-qemu-evidence.txt`; the recovered DeepSeek
  eight-node crash uses `<run-id>-nodeB-qemu-evidence.txt`. Each records the
  original remote file path and one-based source line numbers.

## Results after repairing the generated dependency cache

| Demo | Steps | Run ID | Result |
| --- | --- | --- | --- |
| DeepSeek / 2 nodes | 2 | `sim-console-1788873075344-9` | PASS, 692.9 s; 2/2 steps and 2/2 nodes; token IDs `[65, 20]` |
| DeepSeek / 4 nodes | 2 | `sim-console-1788873769464-10` | PASS, 404.2 s; 2/2 steps and 4/4 nodes; token IDs `[65, 20]` |
| DeepSeek / 8 nodes | 2 | `sim-console-1788874177273-11` | FAIL, 82.0 s; Node B QEMU double free in completion handling |
| Qwen / 8 nodes | 32 | `sim-console-1788874263591-12` | FAIL, 52.4 s; only 3/32 tokens; model-dispatch completion sideband missing |

The first recovered two-node run includes initial operator compilation. Its
second decode round took 208.7 s. Thus the longer wall time does not establish
a persistent slowdown of subsequent runs.

## Reproduction and next corrections

Runs were started through the existing WebUI backend at
`POST http://127.0.0.1:9080/api/v1/runs`, with this request shape:

```json
{
  "demo_id": "w5-qwen-8",
  "target_id": "n4-910c1",
  "parameters": { "steps": "32" }
}
```

The catalog's CLI equivalent is:

```bash
target/debug/sim-console run w5-qwen-8 --target n4-910c1 --set steps=32
```

Run configurations remain the catalog defaults apart from the explicitly
recorded step count. The audit did not switch off GSVA, reduce guest vCPU
count, change model weights, or modify runtime source to obtain a pass.

Prioritized corrections:

1. Serialize ownership of the mutable Rust bridge across all QEMU entrypoints,
   while allowing worker callbacks to acquire the QEMU lock. Add an actual
   concurrent doorbell/CQ regression verifying no loss, duplication, malformed
   completions, or double frees, then repeat both eight-node workloads.
2. Extend target preparation/readiness to provision and verify Simpler's own
   exact `pto_isa.pin` checkout and executable operator artifacts. Make that
   revision remotely obtainable or transfer a verified exact checkout pack.
3. Make W5 node PASS depend on all requested rounds and final validation.
   Include relevant remote QEMU failure excerpts and operator-build progress
   in the WebUI.

All ten runs reached terminal states through launcher cleanup. The final
`ssh n4-910c1 pgrep -fl qemu-system-aarch64` returned exit 1 with no output,
confirming no remaining QEMU process. Product source was not changed by this
audit, and no commit or push was performed. Existing unrelated workspace
edits were retained. Tests establish the stated demo outcomes; numerical
equivalence to an independent model oracle and broad stability are outside
this audit's scope.
