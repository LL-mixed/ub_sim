# W4 Guest/QEMU Dual-Node System Composition

## 1. Purpose

This document fixes the target runtime composition for `W4` when the real goal is:

1. `W4` runs inside the guest/QEMU dual-node system
2. the system remains dual-node end-to-end
3. `shmem`, `block`, and `KVCache` semantics participate in the guest-visible workload path
4. `simpler` remains the `L0-L2` execution carrier, but is not treated as the owner of `KVCache resolve`

This document is not about the already-closed host-side simulator loop. It defines the system that must exist for the actual guest/QEMU target.

## 2. Non-goals

This composition does not assume:

1. a full production `rust_llm_server` process already exists in guest
2. `simpler` owns `KVCache` lookup, fetch, fill, or promote
3. host-side simulator service objects are the final execution form

## 3. Fixed boundary

The boundary remains:

1. `simulator/QEMU/guest-visible system` owns `L3+`
2. `simpler` owns the `L2` compute step
3. `KVCache resolve` stays outside `simpler`

So:

1. request lifecycle
2. request/control shared state
3. `shmem` service behavior
4. `block` lookup/fetch/fill/promote/evict behavior
5. `KVCache` resolve

must stay on the guest/QEMU system side.

## 4. Required process model

For `W4` in the guest/QEMU dual-node system, the process model is fixed as follows.

### 4.1 Host-side processes

Host side must run:

1. two QEMU instances
2. host-side UB/FM topology and link orchestration
3. the `simpler` execution launcher used by the `ChipBackend` path

Host side must not own the request-driven `W4` business loop.

### 4.2 Guest-side processes

Each guest node must run:

1. guest init / bootstrap
2. guest-visible UB/Linux substrate
3. a `W4` workload process

That `W4` workload process is the owner of:

1. request/control progression
2. `shmem` interactions
3. `block` interactions
4. `KVCache` resolve
5. calling into the `ChipBackend`

## 5. Node-local composition

Each guest node must contain the following logical components.

### 5.1 RequestControl component

Responsibilities:

1. create and advance request lifecycle
2. own request/control shared-view behavior
3. publish request/control state through `shmem`
4. consume result feedback after `simpler` returns

### 5.2 KVCache component

Responsibilities:

1. resolve per-block access
2. decide hit or miss
3. decide fetch/fill/promote/evict/writeback behavior
4. own the block-level cache transition semantics
5. materialize compute-facing shared views
6. consume result feedback after `simpler` returns

This component is a guest/QEMU system component, not a `simpler` component.

### 5.3 Shmem service component

Responsibilities:

1. request/control shared state
2. shared metadata view
3. shared hot view
4. staging buffers
5. compute-facing shared segments

`shmem` is not "local-only". It is the shared-view mechanism.

### 5.4 Block service component

Responsibilities:

1. block identity
2. fetch/fill/promote/evict/writeback
3. cross-node block movement
4. cache movement semantics

`block` is not "remote-only". It is the block-movement mechanism.

### 5.5 ChipBackend dispatch component

Responsibilities:

1. receive the resolved `W4` compute step
2. cross the `host DRAM <-> device GM` boundary
3. invoke the real `simpler` path
4. return completion and result payload back to the guest-side workload

## 6. End-to-end W4 path

The fixed `W4` guest/QEMU path is:

1. guest node receives or creates request work
2. guest RequestControl publishes request/control state via `shmem`
3. guest KVCache resolves the next block access
4. if miss:
   - guest `block` path performs cross-node fetch/fill/promote
5. guest materializes compute-facing shared segments
6. guest issues `ChipBackend` dispatch
7. host-side `simpler` performs the `L2` compute
8. result payload returns to the guest-side workload
9. guest feeds result back into:
   - RequestControl state
   - KVCache state
   - shared-view state
10. subsequent request steps observe the updated service state

`W4` is not closed until step `10` exists in the guest/QEMU system.

## 7. Current guest chain and the missing gap

The current guest boot chain is still demo-oriented.

Current guest entry points visible from initramfs are:

1. `linqu_probe`
2. `linqu_ub_chat`
3. `linqu_ub_rpc`
4. `linqu_ub_tcp_each_server`
5. `linqu_ub_udma_demo`
6. `linqu_ub_obmm_demo`

This comes from:

1. [init.c](/Volumes/repos/pypto_workspace/simulator/guest-linux/aarch64/init.c)
2. [run_demo](/Volumes/repos/pypto_workspace/simulator/guest-linux/aarch64/initramfs/run_demo)
3. [build_initramfs.sh](/Volumes/repos/pypto_workspace/simulator/guest-linux/aarch64/scripts/build_initramfs.sh)
4. [run_ub_dual_node_demo.sh](/Volumes/repos/pypto_workspace/simulator/guest-linux/aarch64/scripts/run_ub_dual_node_demo.sh)

What does not yet exist is:

1. a guest-side `W4` workload binary
2. a guest-side request/control component for `W4`
3. a guest-side `KVCache` component for `W4`
4. a guest-side path that issues real `W4` `ChipBackend` dispatches from inside the guest/QEMU system

That is the real missing gap.

## 8. Fixed implementation target

To say `W4` is closed in the guest/QEMU dual-node system, the implementation must satisfy all of these:

1. `W4` runs as a guest-side workload, not only as a host-side simulator workload
2. request/control is exercised through guest-visible `shmem`
3. `KVCache` resolve happens on the guest/QEMU system side
4. `block` fetch/fill/promote is exercised on the guest/QEMU system side
5. the `ChipBackend` path calls real `simpler`
6. `simpler` result payload returns to the guest-side workload
7. that returned result affects later request/control or `KVCache` behavior in the same guest/QEMU run

## 9. Immediate implication for implementation

The next real implementation step is not to add more host-side `W4` semantics.

The next real implementation step is:

1. create the guest-side `W4` workload binary and entry
2. wire it into the guest dual-node boot/run chain
3. move the request/control and `KVCache` loop that currently exists in host-side `sim-workloads` into the guest/QEMU system path

Only after that can `W4` honestly claim guest/QEMU dual-node closure.

## 10. Current achieved guest/QEMU state

The current guest/QEMU `W4` line has moved beyond a host-only placeholder. The following guest-visible path is now real and verified in the dual-node harness:

1. guest-side `W4` workload entry exists and runs inside both guest nodes
2. `shmem/kvcache` path is carried by `OBMM`
3. `block` path is carried by the real `UBURMA` data path
4. guest dispatch path is carried by `/bin/linqu_ub_udma_demo`
5. the dual-node guest harness now validates:
   - `shmem_kvcache_path=obmm_pool`
   - `block_candidate=uburma_data_path_ready`
   - `dispatch_candidate=uburma_udma_ready`
   - `dispatch_path=ub_udma_demo`

This is the current guest/QEMU `W4` closure floor.

## 11. Current guest/QEMU KVCache db service state

The current guest/QEMU `W4` line now contains a real guest-visible `KVCache` metadata/state `db` service.

This is not:

1. a generic toy key/value demo
2. a raw `guest_uapi` ring path
3. a host-side placeholder

This is:

1. a guest-side `KVCache` metadata/state service
2. layered on top of:
   - `shmem`
   - `URMA/UBURMA`
   - `block`
3. used directly by the guest `W4` workload path

The currently verified service behavior is:

1. request/prefix metadata bootstrap
2. multi-block metadata bootstrap
3. result-fed metadata update
4. stale update rejection
5. multi-node visibility
6. multi-node update propagation
7. multi-node coherence
8. multi-node coverage for:
   - `1` request/prefix metadata record
   - `2` block metadata records
9. cross-node metadata reads by key for:
   - remote `block` metadata
   - remote `request/prefix` metadata

The canonical validator for these capabilities is the multi-node guest harness, not the dual-node smoke harness.

## 12. Current explicit non-closure

`db/dfs` is still not modeled as a standalone guest-visible service in `W4`.

The current rule is explicit:

1. `db/dfs` must not be modeled as a raw `guest_uapi` ring path
2. `db/dfs` is deferred over `shmem/urma`
3. the next `db` target is not a generic toy key/value demo
4. the next `db` target must be a `KVCache`-oriented metadata/state service
5. that `db` service must sit above the already-real lower semantics:
   - `shmem`
   - `urma`
   - `block`
6. that `db` service is expected to carry metadata/state such as:
   - block identity and block-key lookup
   - placement and ownership state
   - hot/shared view identity
   - cache state and transition state
   - result-fed metadata updates
7. current guest/QEMU `W4` may still pass without a standalone `db/dfs` service path, as long as:
   - `shmem/kvcache`
   - `block`
   - `dispatch`
   are all real guest-visible paths

So the current state is:

1. guest/QEMU `W4` mainline closure: present
2. guest-visible `KVCache` metadata/state `db` service: present
3. standalone guest `db/dfs over shmem/urma` service: not yet implemented

## 13. Immediate next implementation target

The next guest/QEMU step is no longer to discover a dispatch path. That part is already real.

The next real target is:

1. continue strengthening the guest-visible `KVCache` metadata/state `db` service as a reusable guest component
2. keep that `db` service built on top of:
   - `shmem`
   - `urma`
   - `block`
3. keep it out of raw `guest_uapi ring` modeling
4. keep that service participating in `W4` without regressing:
   - `OBMM`-backed `shmem/kvcache`
   - `UBURMA` block data path
   - real guest dispatch via `ub_udma_demo`
5. keep using multi-node validation for:
   - cross-node visibility
   - update propagation
   - coherence
   - version/order behavior
   - cross-node metadata reads
6. treat `dfs` as a later service layer over the same lower semantics, after this `KVCache`-supporting `db` service is stronger
