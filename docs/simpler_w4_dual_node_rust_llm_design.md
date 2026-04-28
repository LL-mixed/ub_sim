# W4 Design: Dual-Node Rust LLM Minimal Profile with `shmem + block + simpler`

## 1. Purpose

`W4` is the first dual-node workload intended to approximate a minimal `rust_llm_server_mvp` request path while preserving the current layering goal:

1. `L0-L2` execution is carried by `simpler`
2. `L3+` orchestration, routing, cache/tier behavior, and cross-node data services are carried by the current simulator

Unlike `W1-W3`, `W4` is not just a data-path smoke workload. It is a minimal request-driven workload that combines:

1. request/control flow
2. KV-cache-like data access
3. cross-node block fetch/fill
4. local shared-memory coordination
5. minimal `simpler` L2 compute

## 1.1 Current boundary and prerequisite status

`W4` is a target design for the first request-driven dual-node workload after the real `L2/L3` execution boundary is closed. It must not be read as if current simulator already has this closure.

Current repository state is:

1. current simulator has already validated `L3-L6 + UB/Linux-visible` behavior
2. `simpler` correctly represents the software-simulated `L0-L2` device or chip side
3. the missing gap is still the real `ChipBackend` execution adapter between simulator `L3+` orchestration and `simpler` `L0-L2` execution

Therefore `W4` has explicit prerequisites:

1. real `ChipBackend` ABI and adapter path
2. real `host DRAM <-> device GM` boundary semantics
3. real dispatch from simulator into `simpler` execution, with completion flowing back across the same boundary

Before these prerequisites are closed, `W4` remains a target workload design rather than a workload that current simulator can honestly claim to execute end-to-end.

## 2. Position of W4 relative to W1-W3

`W1-W3` establish the lower steps of the ladder:

1. `W1`: dual-node `shmem` visibility and mailbox-style coordination with a real `simpler` L2 step
2. `W2`: dual-node remote `block` read plus a real `simpler` L2 step
3. `W3`: dual-node fetch/fill/promote behavior plus a real `simpler` L2 step

`W4` is the first workload that combines these into a minimal request-oriented serving shape after the real `ChipBackend` boundary is in place.

## 3. W4 design goals

`W4` must satisfy all of the following:

1. include a KV-cache-like abstraction
2. define that KV-cache on top of both `shmem` and `block` semantics
3. include bare `shmem` explicitly in workload implementation and validation scope
4. preserve real `simpler` execution in the L2 step once the `ChipBackend` closure exists
5. remain a minimal MVP workload rather than a full serving stack

## 3.1 Terminology correction: do not split `shmem` and `block` by "local vs remote"

`W4` must not be interpreted using the incorrect simplification:

1. `block = remote`
2. `shmem = local`

That simplification is wrong because both `shmem` and `block` may participate in cross-node behavior.

The correct distinction is by service responsibility and access semantics:

1. `block` expresses block-level cache and movement semantics
2. `shmem` expresses shared-view and shared-access semantics

So in `W4`:

1. `block` should be used for:
   - block identity
   - lookup
   - miss
   - fetch
   - fill
   - promote
   - evict
   - writeback
2. `shmem` should be used for:
   - request/control/synchronization
   - shared metadata view
   - shared data windows
   - shared staging buffers
   - compute-facing shared segments

This means both can be cross-node. The distinction is not topology. The distinction is service role.

## 4. Core design statement

`W4` should be modeled as:

1. a dual-node request path
2. with a KV cache built on top of:
   - `shmem`-backed shared hot or staging views
   - `block`-based lookup/fetch/fill/promote/evict semantics
3. where request/control/synchronization also use explicit `shmem`
4. and where the L2 compute step is executed by real `simpler` through the closed `ChipBackend` boundary

This means `W4` is not "block-only" and not "shmem-only".

## 5. Role of `shmem` in W4

In `W4`, `shmem` is not an accidental implementation detail. It is part of the workload definition.

`shmem` should cover three explicit roles.

### 5.1 Request and control plane

Use `shmem` for:

1. request descriptors
2. mailbox or queue slots
3. ready or ack signaling
4. completion or status slots
5. lightweight coordination state

This is the control-facing use of `shmem`.

### 5.2 Shared KV hot tier and shared cache-facing views

Use `shmem` as the backing medium for the shared hot or compute-facing portion of the KV cache.

Examples:

1. KV block metadata exposed in a shared view
2. block table entries
3. page or slot metadata
4. reuse or refcount state
5. hot data segments intended to be shared among participants consuming the current view

This is the cache-facing use of `shmem`.

### 5.3 Staging area between block semantics and compute-facing shared views

Use `shmem` as staging or shared buffer space for:

1. block fetch results before they become part of the current compute-facing view
2. data promoted into the hot shared tier
3. intermediate buffers shared between simulator-side orchestration and the minimal L2 step

This is the bridge between `block` and `simpler`.

## 6. Role of `block` in W4

`block` remains the correct abstraction for block-level cache access and movement behavior.

In `W4`, `block` should cover:

1. KV block identity
2. fetch/fill semantics
3. promotion into local usable state
4. miss handling across views or placements
5. eviction and writeback semantics when required

`block` is the right abstraction for cache movement and block management, but it is not the whole KV-cache abstraction.

## 7. KVCache definition in W4

In `W4`, KVCache should not be reduced to "a raw shared memory region".

Instead:

1. KVCache is a logical abstraction
2. shared hot or compute-facing views may be `shmem`-backed
3. block access, fetch/fill, promotion, and eviction are `block`-based
4. routing, miss handling, and fill decisions remain simulator responsibilities

In short:

1. `shmem` is part of KVCache implementation
2. `block` is part of KVCache block-management semantics
3. KVCache itself is the higher-level serving abstraction above both

## 8. Layering in W4

### 8.1 Simulator responsibilities

The current simulator continues to own:

1. request lifecycle
2. route choice
3. hit or miss decisions
4. block fetch or fill behavior
5. cache tier behavior
6. `shmem` control and shared-state orchestration

These are `L3+` responsibilities.

### 8.2 simpler responsibilities

`simpler` continues to own:

1. the minimal L2 compute step
2. chip-local execution semantics
3. runtime-specific execution path

At this stage, `W4` does not require that full serving semantics move into `simpler`.

But `W4` does require that the L2 step be a real execution path, not a simulator-side placeholder. If the `ChipBackend` boundary is still stubbed, then `W4` is not yet truly in scope.

## 9. W4 phased plan

Before `W4-v0`, there is a prerequisite step.

### 9.0 W4 prerequisite

Goal:

1. close the real `L2/L3` execution adapter
2. make `host DRAM <-> device GM` crossing explicit and testable
3. ensure simulator dispatch enters `simpler` and completion returns from `simpler`

Without this step, the later `W4-v0/v1/v2/v3` phases are only target-shape design, not executable phase definitions.

### 9.1 W4-v0

Goal:

1. run a dual-node request path
2. explicitly include `shmem` request/control behavior
3. include `block` fetch/fill behavior
4. invoke a real `simpler` L2 step through the closed `ChipBackend` boundary

Expected shape:

1. request arrives
2. request/control state is placed or synchronized through `shmem`
3. local check determines hit or miss
4. miss triggers remote `block` fetch
5. fetched data is staged or filled into local shared state
6. minimal L2 step runs in `simpler`
7. completion is reported back through simulator-managed control flow

### 9.2 W4-v1

Goal:

1. make KV-cache behavior explicit rather than implicit
2. cover hit-only, miss-only, and mixed cases
3. show that local hot/shared cache state is `shmem`-backed

### 9.3 W4-v2

Goal:

1. make bare `shmem` validation explicit
2. validate request/control queues, shared metadata, and staging semantics
3. prove that `shmem` is part of workload coverage, not just internal plumbing

### 9.4 W4-v3

Goal:

1. move the L2 step closer to real workload semantics
2. reduce dependence on generic minimal example mapping
3. carry more request, block, or cache context into the `simpler` call

## 10. Validation focus for W4

`W4` validation should explicitly cover both `block` and `shmem`.

Required categories:

1. request/control over `shmem`
2. local shared-state visibility over `shmem`
3. remote miss resolution through `block`
4. local fill or promotion after remote access
5. real `simpler` invocation in the L2 step
6. successful completion of the request path

This prevents `W4` from drifting into a block-only workload while still claiming to model serving behavior.

`W4` validation must not claim proof of `simpler` internal runtime semantics such as:

1. `task_ring`
2. `buffer_ring`
3. `scope.enter/exit`
4. `pl.free`
5. retire ordering

unless the actual `simpler` runtime is in the execution loop through the real adapter boundary.

## 11. Explicit non-goals for initial W4

Initial `W4` should not try to solve all of the following at once:

1. full multi-request serving behavior
2. full fault matrix
3. full KV lifecycle policy
4. full `h2d/d2h` semantic alignment inside `simpler`
5. final production-form LLM kernels

These belong to later refinement stages.

## 12. Summary

The defining statement for `W4` is:

1. `W4` must explicitly include bare `shmem`
2. `W4` must define KV cache on top of `shmem + block`
3. `W4` must keep real `simpler` as the L2 execution carrier after the `ChipBackend` closure is complete
4. `W4` must remain minimal enough to serve as the first request-driven dual-node MVP

This keeps `W4` aligned with both:

1. the current simulator vs `simpler` layering
2. the intended pyPTO or LLM-serving style of workload decomposition
