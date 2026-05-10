# OBMM SPMC and MPSC Queue Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement SPMC (provider-owned broadcast log) and MPSC (consumer-owned SPSC lane set) queues for OBMM shared-memory inter-node communication.

**Architecture:** SPMC uses a provider-owned ring with per-consumer cursors (64-bit counters). MPSC reuses existing SPSC queues as lanes with round-robin drain. Both are header-only C, no new kernel ABI. Full design spec at `docs/drafts/obmm_spmc_mpsc_queue_design.md`.

**Tech Stack:** C11 (stdatomic.h), header-only inline functions, host-side unit tests with pthread.

**Base path:** `guest-linux/aarch64/apps/obmm_queue_demo/`

**Build and test:**
```bash
cd guest-linux/aarch64/apps/obmm_queue_demo
make test
```

---

### Task 1: Add SPMC wire types to obmm_pool_types.h

**Files:**
- Modify: `obmm_pool_types.h` (append after SPSC queue section)

**Step 1: Add OBMM_REGION_SPMC_STREAM to enum obmm_region_kind**

Add `OBMM_REGION_SPMC_STREAM = 6` after `OBMM_REGION_W4_PAYLOAD = 5` (line 53).

**Step 2: Add SPMC constants and flag defines**

After the SPSC queue section (after line 151 `#endif`), before the closing `#endif`, add SPMC magic, version, max consumers, flags, cursor state enum, and the two wire structs (`obmm_spmc_consumer_cursor`, `obmm_spmc_stream`) with static asserts. Code from design doc sections "First-version wire format" and "Field usage notes".

**Step 3: Run existing tests to verify no regressions**

Run: `cd guest-linux/aarch64/apps/obmm_queue_demo && make test`
Expected: all 8 existing tests PASS.

**Step 4: Commit**

```
git add obmm_pool_types.h
git commit -m "Add SPMC stream wire types to obmm_pool_types.h"
```

---

### Task 2: Add utility macros and visibility helpers to obmm_queue.h

**Files:**
- Modify: `obmm_queue.h` (add before closing `#endif`)

**Step 1: Add OBMM_FOR_EACH_NODE_ID macro**

```c
#define OBMM_FOR_EACH_NODE_ID(nid, mask)                         \
    for (uint64_t _m = (mask);                                   \
         _m != 0 && ((nid) = (uint32_t)(__builtin_ffsll(_m) - 1), 1); \
         _m &= _m - 1)
```

**Step 2: Add visibility helpers as release fences**

```c
static inline void obmm_publish_payload_for_remote_read(const void *addr,
                                                         uint64_t len)
{
    (void)addr; (void)len;
    atomic_thread_fence(memory_order_release);
}

static inline void obmm_publish_desc_for_remote_read(const void *addr,
                                                      uint64_t len)
{
    (void)addr; (void)len;
    atomic_thread_fence(memory_order_release);
}

static inline void obmm_publish_cursor_for_provider_read(const void *addr,
                                                          uint64_t len)
{
    (void)addr; (void)len;
    atomic_thread_fence(memory_order_release);
}
```

**Step 3: Run existing tests**

Run: `make test`
Expected: all 8 existing tests PASS (visibility helpers not yet called).

**Step 4: Commit**

```
git add obmm_queue.h
git commit -m "Add OBMM_FOR_EACH_NODE_ID and visibility helpers to obmm_queue.h"
```

---

### Task 3: Create obmm_spmc_queue.h — layout helpers and init

**Files:**
- Create: `obmm_spmc_queue.h`
- Modify: `obmm_queue_test.c` (add test + include)

**Step 1: Write failing tests for SPMC layout and init**

Add `#include "obmm_spmc_queue.h"` at top of test file. Add test functions:

- `test_spmc_layout`: verify `sizeof(struct obmm_spmc_stream) == 128`, `sizeof(struct obmm_spmc_consumer_cursor) == 64`, `obmm_spmc_region_size()` produces correct desc_offset and total size.
- `test_spmc_init_valid`: init a stream with depth=64, max_consumers=8, consumer_mask=0xFE. Verify tail=0, depth/mask set, cursor_offset/desc_offset correct, active cursors have state=ACTIVE and generation_seen=1.
- `test_spmc_init_invalid`: init with depth=0, depth=63, max_consumers=0, max_consumers=65, consumer_mask bit >= max_consumers. All must return -EINVAL.

Register all in main() test table. Run: `make test` — expected: compile fails (header not found).

**Step 2: Create obmm_spmc_queue.h with layout helpers and init**

Create header with:
- `obmm_spmc_region_size(depth, max_consumers)`
- `obmm_spmc_cursor(stream, node_id)`
- `obmm_spmc_desc_ring(stream)`
- `obmm_spmc_stream_init(base, depth, max_consumers, provider_node, consumer_mask)`

Code from design doc "Layout helpers" and "Stream Initialization" pseudo-code.

**Step 3: Run tests**

Run: `make test`
Expected: all existing + new SPMC layout/init tests PASS.

**Step 4: Commit**

```
git add obmm_spmc_queue.h obmm_queue_test.c
git commit -m "Add SPMC layout helpers and stream init with tests"
```

---

### Task 4: Add SPMC view init and provider payload addr

**Files:**
- Modify: `obmm_spmc_queue.h` (add functions)
- Modify: `obmm_queue_test.c` (add tests)

**Step 1: Write failing tests for view init and payload addr**

- `test_spmc_view_init`: create a pool with header + directory containing one OBMM_REGION_SPMC_STREAM entry + one OBMM_REGION_TX_ARENA entry. Call `obmm_spmc_view_init_from_directory()`. Verify view fields populated.
- `test_spmc_view_init_missing`: no SPMC_STREAM entry → returns -ENOENT.
- `test_spmc_view_init_duplicate`: two SPMC_STREAM entries → returns -EEXIST.
- `test_spmc_view_init_bad_magic`: corrupt stream magic → returns -EINVAL.
- `test_spmc_payload_addr_tx_arena`: descriptor referencing TX arena with valid payload → returns 1 and correct address.
- `test_spmc_payload_addr_no_payload`: descriptor with payload_len=0 → returns 0.
- `test_spmc_payload_addr_oob`: descriptor with payload_offset+payload_len > dirent.size → returns -EINVAL.
- `test_spmc_payload_addr_missing_region`: descriptor with region_id not in directory → returns -EINVAL.

Run: `make test` — expected: compile fails (functions not yet defined).

**Step 2: Add view init and payload addr functions to obmm_spmc_queue.h**

- `obmm_spmc_view_init_from_directory()` — from design doc "View Initialization" pseudo-code. Include `pool_size` field in view struct.
- `obmm_spmc_provider_payload_addr()` — from design doc "Publish Path" section with overflow-safe range checks.

Also add `struct obmm_spmc_stream_view` definition (with `pool_size` field).

**Step 3: Run tests**

Run: `make test`
Expected: all tests PASS.

**Step 4: Commit**

```
git add obmm_spmc_queue.h obmm_queue_test.c
git commit -m "Add SPMC view init and provider payload addr with tests"
```

---

### Task 5: Add SPMC publish, consume, reclaim, reset

**Files:**
- Modify: `obmm_spmc_queue.h` (add functions)
- Modify: `obmm_queue_test.c` (add tests)

**Step 1: Write failing tests for publish/consume**

- `test_spmc_publish_consume`: init stream with 2 consumers. Publish 10 descriptors. Each consumer consumes independently. Verify FIFO order for each consumer.
- `test_spmc_publish_full`: fill stream to depth, verify next publish returns -EAGAIN. Consumer advances head, verify publish succeeds.
- `test_spmc_consume_empty`: consume from empty stream → -EAGAIN.
- `test_spmc_consume_overrun`: advance tail beyond head+depth without consuming, verify consume returns -EOVERFLOW, cursor state becomes PAUSED, drop_count increments.
- `test_spmc_wraparound`: use depth=64, publish+consume interleaved for depth+depth/2 iterations, verify ring index wraps correctly.
- `test_spmc_publish_inactive`: set one consumer state to PAUSED, publish returns -EPIPE.
- `test_spmc_publish_no_active`: init with consumer_mask=0, publish returns -ENODEV.
- `test_spmc_reclaimable_head`: publish 5, consume 3 on one consumer and 2 on another. reclaimable_head returns 2 (minimum).
- `test_spmc_reclaimable_skips_paused`: mark one consumer PAUSED, reclaimable_head skips it.
- `test_spmc_reset`: publish some, reset stream, verify tail=0, cursors reset, generation incremented.
- `test_spmc_reclaim_payloads`: publish 5 descriptors referencing TX arena payload, consume all, reclaim_payloads advances tx_reclaim_offset.

Run: `make test` — expected: compile fails.

**Step 2: Add publish/consume/reclaim/reset to obmm_spmc_queue.h**

- `obmm_spmc_publish()` — from design doc pseudo-code (no consumer_mask param, uses stream's active_consumer_mask)
- `obmm_spmc_consume()` — from design doc pseudo-code
- `obmm_spmc_reclaimable_head()` — from design doc pseudo-code
- `obmm_spmc_reclaim_payloads()` — from design doc pseudo-code
- `obmm_spmc_tx_reclaim_state` struct
- `obmm_spmc_stream_reset()` — from design doc pseudo-code

**Step 3: Run tests**

Run: `make test`
Expected: all tests PASS.

**Step 4: Commit**

```
git add obmm_spmc_queue.h obmm_queue_test.c
git commit -m "Add SPMC publish/consume/reclaim/reset with tests"
```

---

### Task 6: Create obmm_mpsc_queue.h — all MPSC helpers

**Files:**
- Create: `obmm_mpsc_queue.h`
- Modify: `obmm_queue_test.c` (add tests)

**Step 1: Write failing tests for MPSC**

- `test_mpsc_consumer_set_init`: create directory with 3 OBMM_REGION_QUEUE entries for different peers. Init consumer set. Verify lane_count=3, sorted by publisher_node.
- `test_mpsc_consumer_set_init_no_lanes`: empty directory → returns -ENOENT.
- `test_mpsc_consumer_set_init_too_many`: directory with > OBMM_MPSC_MAX_LANES entries → returns -E2BIG.
- `test_mpsc_consumer_set_init_duplicate`: two entries with same peer_node_id → returns -EEXIST.
- `test_mpsc_publisher_lane_init`: directory with one matching entry → returns 0, lane populated.
- `test_mpsc_publisher_lane_missing`: no matching entry → returns -ENOENT.
- `test_mpsc_publisher_lane_duplicate`: two matching entries → returns -EEXIST.
- `test_mpsc_poll_order`: push 3 items to each of 3 lanes, poll all 9, verify per-publisher FIFO order and global rx_seq monotonic.
- `test_mpsc_poll_empty`: poll with all lanes empty → returns -EAGAIN.
- `test_mpsc_poll_fairness`: push 100 to each of 3 lanes, poll all 300, verify max_fairness_gap is bounded (no publisher starved).

Run: `make test` — expected: compile fails.

**Step 2: Create obmm_mpsc_queue.h**

All code from design doc "Lane-Set Helper Contract" and "Consumer Path":
- `obmm_mpsc_lane`, `obmm_mpsc_consumer_set`, `obmm_mpsc_publisher_lane` structs
- `obmm_mpsc_consumer_set_init_from_directory()`
- `obmm_mpsc_publisher_lane_init_from_directory()`
- `obmm_mpsc_push()` — wrapper over `obmm_spsc_push()`
- `obmm_mpsc_poll()` — round-robin with rx_seq assignment

**Step 3: Run tests**

Run: `make test`
Expected: all tests PASS.

**Step 4: Commit**

```
git add obmm_mpsc_queue.h obmm_queue_test.c
git commit -m "Add MPSC lane-set helpers with tests"
```

---

### Task 7: Update Makefile

**Files:**
- Modify: `Makefile`

**Step 1: Add new headers to HEADERS and test dependency**

Update HEADERS to include `obmm_spmc_queue.h obmm_mpsc_queue.h`. Update test target dependency.

**Step 2: Run full test suite**

Run: `make test`
Expected: all tests PASS (no regressions).

**Step 3: Commit**

```
git add Makefile
git commit -m "Update Makefile with SPMC/MPSC headers"
```

---

### Task 8: Demo integration — export layout and SPMC demo mode

**Files:**
- Modify: `obmm_queue_demo.c`

This is the largest task. Break into sub-steps:

**Step 1: Add demo_mode enum and env var parsing**

Add `OBMM_DEMO_MODE` env var parsing. Add `g_demo_mode` global. Parse before Phase 2.

**Step 2: Refactor layout helpers**

Extract `layout_directory_count()`, `layout_queues_base()` from existing `layout_queue_offset()`. Add `layout_spmc_stream_offset()`. Modify `layout_tx_arena_offset()` to skip SPMC stream region. Conditionally enable SPMC based on demo mode.

**Step 3: Modify init_export_layout() for SPMC**

Add `OBMM_REGION_SPMC_STREAM` directory entry when SPMC enabled. Call `obmm_spmc_stream_init()`. Update directory_count.

**Step 4: Modify resolve_peer_layout() for directory scan**

Replace `directory_count == node_count` assumption with directory scan by kind. Handle `OBMM_REGION_SPMC_STREAM`.

**Step 5: Add SPMC demo protocol**

Add `do_spmc_rounds()` function implementing the SPMC demo protocol from design doc. Provider publishes batch + terminal. Consumers consume and ACK.

**Step 6: Add demo mode dispatch in main()**

Route Phase 5 to appropriate function based on `g_demo_mode`.

**Step 7: Build and verify**

Run: `make demo`
Expected: clean build (cross-compile). Run: `make test`
Expected: all tests PASS.

**Step 8: Commit**

```
git add obmm_queue_demo.c
git commit -m "Add SPMC demo mode with export layout changes"
```

---

### Task 9: Demo integration — MPSC demo mode

**Files:**
- Modify: `obmm_queue_demo.c`

**Step 1: Add MPSC demo protocol**

Add `do_mpsc_rounds()` function. Publishers push batch + terminal. Consumer polls with rx_seq tracking.

**Step 2: Add COMBINED demo mode**

SPSC + SPMC + MPSC coexisting in same export region.

**Step 3: Build and verify**

Run: `make demo && make test`
Expected: clean build, all tests PASS.

**Step 4: Commit**

```
git add obmm_queue_demo.c
git commit -m "Add MPSC and combined demo modes"
```

---

## Summary

| Task | Description | New Files | Modified Files |
|------|-------------|-----------|----------------|
| 1 | SPMC wire types | — | obmm_pool_types.h |
| 2 | Utility macros + visibility helpers | — | obmm_queue.h |
| 3 | SPMC layout + init + tests | obmm_spmc_queue.h | obmm_queue_test.c |
| 4 | SPMC view init + payload addr | — | obmm_spmc_queue.h, obmm_queue_test.c |
| 5 | SPMC publish/consume/reclaim/reset | — | obmm_spmc_queue.h, obmm_queue_test.c |
| 6 | MPSC helpers + tests | obmm_mpsc_queue.h | obmm_queue_test.c |
| 7 | Makefile update | — | Makefile |
| 8 | Demo: SPMC mode | — | obmm_queue_demo.c |
| 9 | Demo: MPSC + combined modes | — | obmm_queue_demo.c |
