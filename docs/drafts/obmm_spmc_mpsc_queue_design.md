# OBMM SPMC and MPSC Queue Design

## Goal

This design extends the existing OBMM shared-memory pool SPSC queue model to
two higher-level communication patterns:

- SPMC: one provider node publishes data once and multiple peer nodes consume
  the same stream.
- MPSC: multiple publisher nodes send independent descriptors to one consumer
  node.

The design assumes the current OBMM cacheability contract:

- A node accesses the region it exports through a local cacheable mapping.
- A node accesses a peer's exported region through an imported non-cacheable
  osync mapping.

The main objective is to keep the fast path single-writer per cache line and to
avoid remote atomic compare-and-swap or remote multi-writer contention.

## Existing Baseline

The current SPSC design uses one queue per source and destination pair:

```text
queue[dst][src] = descriptors from src to dst
```

The queue storage lives in the destination node's exported region:

```text
src producer writes dst.queue_from_src through remote NC/osync mapping
dst consumer reads dst.queue_from_src through local cacheable mapping
```

This is the right primitive for point-to-point traffic. SPMC and MPSC should be
implemented as queue sets specialized for their traffic shape, not as a single
global MPMC ring.

## Design Summary

Use different physical layouts for SPMC and MPSC:

```text
SPMC:
  provider-owned broadcast log in provider exported memory
  provider writes descriptors/payload locally once
  each peer reads through imported NC mapping and advances its own cursor

MPSC:
  per-publisher SPSC lanes in consumer exported memory
  each publisher writes only its own lane
  consumer drains all lanes locally with a fair scheduler
```

This matches the cacheability model:

- SPMC optimizes fanout by turning N remote descriptor writes into one local
  descriptor write plus N remote reads.
- MPSC optimizes fan-in by avoiding a shared remote tail and keeping each
  producer's writes isolated to its own lane.

## Implementation Contract

This document is intended to drive the first implementation, so the first
implementation must be deliberately narrow:

1. MPSC is implemented first as a helper over the existing SPSC queue wire
   format. It adds no new shared-memory ABI.
2. SPMC is implemented as a provider-owned broadcast stream with one fixed
   delivery set per stream. It uses a new shared-memory ABI and 64-bit
   monotonic head/tail counters.
3. Loss-tolerant SPMC is not part of the first implementation. It requires
   per-slot sequence validation and should use a later ABI version.
4. Dynamic hot membership is not part of the fast path. The first
   implementation supports initialization-time membership plus explicit
   management detach.
5. The SPMC cursor visibility path must be validated before SPMC is used for
   correctness-critical W4/Qwen traffic. If that path is not reliable, keep the
   provider-owned broadcast log for publication and use reverse SPSC ACK lanes
   for reclaim/backpressure.

The expected first code change set is:

- `guest-linux/aarch64/apps/obmm_queue_demo/obmm_pool_types.h`
  - add `OBMM_REGION_SPMC_STREAM`;
  - add SPMC wire structs and static assertions;
  - keep `struct obmm_desc` and `struct obmm_spsc_queue` ABI-compatible.
- `guest-linux/aarch64/apps/obmm_queue_demo/obmm_queue.h`
  - keep existing SPSC helpers unchanged;
  - add shared alignment/region-size helpers if needed.
  - add OBMM visibility helper boundaries used by SPSC, SPMC, and MPSC:
    `obmm_publish_payload_for_remote_read()`,
    `obmm_publish_desc_for_remote_read()`, and
    `obmm_publish_cursor_for_provider_read()`.
- `guest-linux/aarch64/apps/obmm_queue_demo/obmm_spmc_queue.h`
  - add SPMC init/publish/consume/reclaim helpers.
- `guest-linux/aarch64/apps/obmm_queue_demo/obmm_mpsc_queue.h`
  - add MPSC lane-set helpers built on `obmm_spsc_push()` and
    `obmm_spsc_pop()`.
- `guest-linux/aarch64/apps/obmm_queue_demo/obmm_queue_test.c`
  - add host-side unit tests for layout, wraparound, backpressure, lane
    fairness, and ordering.
- `guest-linux/aarch64/apps/obmm_queue_demo/obmm_queue_demo.c`
  - add guest demo modes for SPMC and MPSC validation.

## Concurrency Model

The first implementation assumes single-threaded access per role:

- One provider thread calls `obmm_spmc_publish()` on a given stream.
- One consumer thread calls `obmm_spmc_consume()` on a given consumer
  cursor.
- One publisher thread calls `obmm_mpsc_push()` on a given lane.
- One consumer thread calls `obmm_mpsc_poll()` on a given consumer set.

A node may simultaneously be a SPMC provider, a SPMC consumer on a
different stream, and a MPSC publisher — these are independent roles and
may use independent threads. The first implementation does not provide
internal locking. Callers that need multi-threaded access to the same
role must coordinate externally.

## Utility Macros

The pseudo-code below uses a bitmask iteration macro that must be defined
once in a shared header:

```c
/*
 * Iterate over each node ID set in a uint64_t bitmask.
 * __builtin_ffsll returns the 1-based index of the lowest set bit,
 * or 0 if the mask is zero.  The loop clears each processed bit.
 */
#define OBMM_FOR_EACH_NODE_ID(nid, mask)                        \
    for (uint64_t _m = (mask);                                  \
         _m != 0 && ((nid) = (uint32_t)(__builtin_ffsll(_m) - 1), 1); \
         _m &= _m - 1)
```

`nid` is declared as `uint32_t` in the surrounding scope. The macro
clears the lowest set bit on each iteration so the loop body may not
modify `mask` through `nid`. The `OBMM_` prefix is intentional; this helper
belongs in an OBMM queue header and should not use a generic name such as
`for_each_node_id`.

## Non-Goals

- Do not build a true global MPSC ring whose tail is updated by many remote
  producers through atomic fetch-add or CAS.
- Do not require remote atomic operations in the fast path.
- Do not share one writable cache line between multiple nodes.
- Do not encode process-local virtual addresses in descriptors.
- Do not implement lossy SPMC on top of the strict SPMC slot layout.

## Shared Descriptor Model

Reuse `struct obmm_desc` as the descriptor payload:

```c
struct obmm_desc {
    uint64_t seq;
    uint32_t region_id;
    uint32_t payload_len;
    uint64_t payload_offset;
    uint16_t type;
    uint16_t flags;
    uint32_t cookie;
};
```

`region_id + payload_offset` remains the addressing contract. A descriptor is
valid across all nodes because each node resolves `region_id` through its local
view of the imported/exported OBMM directory.

For producer-owned payloads:

```text
payload region = provider TX arena
provider writes payload locally cacheable
peers read payload remotely through NC/osync mapping
```

For receiver-owned payloads:

```text
payload region = consumer RX arena partitioned by publisher
publisher writes payload remotely through NC/osync mapping
consumer reads payload locally cacheable
```

SPMC should prefer producer-owned payloads. MPSC can use either producer-owned
payloads or receiver-owned per-publisher RX arenas, depending on payload size.

## SPMC: Provider-Owned Broadcast Log

### Use Case

SPMC is for one provider publishing the same logical item to multiple peers:

- one Qwen range owner publishes a hidden/runtime tensor to several downstream
  consumers;
- one node publishes a token result, barrier event, or object metadata to a
  peer set;
- one owner exposes immutable data that multiple peers read independently.

### Physical Layout

The SPMC queue lives in the provider's exported region:

```text
provider exported region
+--------------------------------------------------+
| pool header / directory                           |
+--------------------------------------------------+
| spmc stream header                                |
|   tail cache line          provider writes        |
|   consumer cursor[0]       consumer 0 writes      |
|   consumer cursor[1]       consumer 1 writes      |
|   ...                                            |
+--------------------------------------------------+
| descriptor ring                                  |
+--------------------------------------------------+
| provider TX payload arena                        |
+--------------------------------------------------+
```

In the existing `obmm_queue_demo` export layout, SPMC is an additional region.
It does not replace any ingress SPSC queue. The first SPMC-capable demo layout
should be:

```text
offset 0:              struct obmm_pool_header
directory_offset:      region directory entries
after directory:       ingress SPSC queues, one per peer
after ingress queues:  optional SPMC stream(s) owned by this export
after SPMC streams:    TX arena
```

This preserves the existing peer-slot order, not the absolute queue offsets.
The current `layout_queue_offset(peer_slot, node_count)` logic derives
`queues_base` from `node_count * sizeof(struct obmm_region_dirent)`. Once SPMC
adds another directory entry, that helper must be refactored to use the actual
`directory_count` or a precomputed `queues_base`. Otherwise the additional
`OBMM_REGION_SPMC_STREAM` dirent can overlap the first ingress queue.

Fullmesh mode must keep the current layout exactly. SPMC-enabled modes use a
larger directory and therefore may shift the absolute offsets of the ingress
queues, SPMC stream, and TX arena. This is acceptable because same-version
nodes discover peer layouts by scanning the exported directory rather than by
recomputing offsets from mode-local assumptions.

For the first demo, allocate at most one SPMC stream per exported pool:

```text
spmc_stream_count = OBMM_SPMC_ENABLED ? 1 : 0
directory_count = peer_count + spmc_stream_count + 1 /* TX arena */

queues_base = align_up(directory_offset +
                       directory_count * sizeof(struct obmm_region_dirent),
                       64)
queue_offset(peer_slot) = queues_base +
                          peer_slot * obmm_queue_region_size(queue_depth)
spmc_stream_offset = align_up(queues_base +
                              peer_count * obmm_queue_region_size(queue_depth),
                              64)
tx_arena_offset = align_up(spmc_stream_offset +
                           spmc_stream_count *
                           obmm_spmc_region_size(spmc_depth, max_consumers),
                           64)
```

`init_export_layout()` must initialize the stream before publishing
`OBMM_POOL_STATE_READY`, just like it initializes each ingress SPSC queue today.
Peer layout validation must stop requiring `directory_count == node_count`;
SPMC-capable validation should accept at least the existing SPSC/TX entries and
scan any additional entries by `kind`.

### Export Layout Modification Checklist

When SPMC is enabled, the following existing functions must change:

| Function | Change |
|----------|--------|
| `layout_directory_count()` | New. Returns `peer_count + 1` for fullmesh and `peer_count + spmc_stream_count + 1` when SPMC is enabled. |
| `layout_queues_base()` | New. Computes `align_up(directory_offset + directory_count * sizeof(struct obmm_region_dirent), 64)`. |
| `layout_queue_offset()` | Must take `queues_base` or `directory_count`; it must not infer directory size from `node_count`. It preserves peer-slot order but may return different absolute offsets in SPMC-enabled modes. |
| `layout_spmc_stream_offset()` | New. `align_up(queues_base + peer_count * obmm_queue_region_size(queue_depth), 64)`. |
| `layout_tx_arena_offset()` | Must skip the SPMC stream region: `obmm_align_up(spmc_stream_offset + spmc_stream_count * obmm_spmc_region_size(spmc_depth, max_consumers), 64)`. |
| `layout_tx_arena_size()` | Adjusts automatically since it subtracts the new arena offset from `g_export_size`. |
| `validate_export_layout()` | Add space check for the SPMC stream between the last ingress queue and the TX arena. The minimum export size grows by `obmm_spmc_region_size(spmc_depth, max_consumers)` when SPMC is enabled. |
| `init_export_layout()` | Write an additional `OBMM_REGION_SPMC_STREAM` directory entry after the existing queue and TX arena entries. Keep the TX arena `region_id` stable at `peer_count`; use `peer_count + 1` for the optional SPMC stream. Call `obmm_spmc_stream_init()` at the computed offset. `directory_count` becomes `peer_count + spmc_stream_count + 1`. |
| `resolve_peer_layout()` | Replace the assumption that `directory_count == node_count` with a directory scan by `kind`. For each dirent, dispatch by kind: `OBMM_REGION_QUEUE` → resolve ingress queue, `OBMM_REGION_TX_ARENA` → resolve TX arena, `OBMM_REGION_SPMC_STREAM` → build view via `obmm_spmc_view_init_from_directory()`. Unknown kinds must be ignored (forward compatibility). |
| `g_export_size` | The minimum export size increases. The demo must compute the required size from the enabled features and either validate the configured size or adjust upward. The first implementation should compute the minimum required size at startup and fail with a clear message if `OBMM_POOL_EXPORT_SIZE_MB` is too small. |

### Directory Count Compatibility

Enabling SPMC increases `directory_count` from `node_count` to at least
`node_count + 1`. This has compatibility implications:

- **Same-version nodes**: All nodes run the updated demo binary. No issue;
  `resolve_peer_layout()` scans by kind and uses each dirent's `offset` and
  `size`. Nodes must not recompute peer queue offsets from local mode flags.
- **Mixed-version nodes**: An old binary that checks
  `directory_count == node_count` will reject the new layout. The first
  implementation does not need to support mixed versions. If mixed-version
  compatibility becomes a requirement, the layout version must bump to 2
  and old binaries must be updated to accept the new version.
- **Layout version**: The first SPMC implementation keeps
  `OBMM_POOL_LAYOUT_VERSION` at 1. The pool header's `directory_count`
  already allows the consumer to iterate the directory without knowing the
  count in advance. The version should only bump when the header struct
  layout or the directory entry format changes, not when new region kinds
  are added.

First-version wire format:

```c
#define OBMM_SPMC_MAGIC 0x4f424d53504d4301ULL /* "OBMSPMC" */
#define OBMM_SPMC_VERSION 1
#define OBMM_SPMC_MAX_CONSUMERS 64

#define OBMM_SPMC_F_STRICT          (1u << 0)
#define OBMM_SPMC_F_FIXED_MASK      (1u << 1)
#define OBMM_SPMC_F_PRODUCER_PAYLOAD (1u << 2)

enum obmm_spmc_cursor_state {
    OBMM_SPMC_CONSUMER_DETACHED = 0,
    OBMM_SPMC_CONSUMER_ATTACHING = 1,
    OBMM_SPMC_CONSUMER_ACTIVE = 2,
    OBMM_SPMC_CONSUMER_PAUSED = 3,
    OBMM_SPMC_CONSUMER_DEAD = 4,
};

struct obmm_spmc_consumer_cursor {
    alignas(64) _Atomic uint64_t head;       /* written by this consumer */
    _Atomic uint64_t observed_seq;           /* last accepted seq */
    _Atomic uint64_t drop_count;             /* local overrun accounting */
    _Atomic uint32_t state;                  /* enum obmm_spmc_cursor_state */
    _Atomic uint32_t generation_seen;
    uint32_t node_id;                        /* cursor index owner */
    uint32_t reserved0;
    uint8_t reserved[24];
};

struct obmm_spmc_stream {
    alignas(64) uint64_t magic;
    _Atomic uint64_t active_consumer_mask;   /* bit index is node id */
    uint64_t generation;
    uint32_t version;
    uint32_t flags;
    uint32_t header_bytes;                   /* sizeof(struct obmm_spmc_stream) */
    uint32_t cursor_offset;
    uint32_t desc_offset;
    uint32_t depth;                          /* power of two */
    uint32_t mask;                           /* depth - 1 */
    uint32_t max_consumers;                  /* <= OBMM_SPMC_MAX_CONSUMERS */
    uint32_t provider_node;
    uint8_t header_reserved[4];

    alignas(64) _Atomic uint64_t tail;       /* provider-owned */
    uint8_t tail_pad[56];

    /*
     * Cursors start at cursor_offset:
     *   struct obmm_spmc_consumer_cursor cursor[max_consumers]
     *
     * The descriptor ring starts at desc_offset:
     *   struct obmm_desc desc[depth]
     */
};

static_assert(sizeof(struct obmm_spmc_consumer_cursor) == 64,
              "SPMC cursor must occupy one cache line");
static_assert(sizeof(struct obmm_spmc_stream) == 128,
              "SPMC stream header must occupy two cache lines");
```

Field usage notes for `struct obmm_spmc_consumer_cursor`:

- `observed_seq`: In strict mode the consumer reads every slot in order,
  so `observed_seq` should match the last descriptor sequence accepted by
  that consumer and provides little additional information when providers
  set `desc.seq` from the publish counter. It is reserved for future
  lossy-mode consumers that may skip slots and need to record the last
  sequence they actually accepted. The first implementation must still
  store `desc.seq` into it on every successful consume for forward
  compatibility, but no caller reads it in strict mode.
- `drop_count`: Incremented by the consumer when it detects overrun
  (`tail - head > depth`). The provider may periodically acquire-load
  this field across all cursors for monitoring and logging. It is not
  used in the fast-path reclaim or backpressure calculation.
- `generation_seen`: Set to the stream's current `generation` during
  cursor initialization or attach. The provider may compare this against
  the current stream generation to detect consumers that have not yet
  completed a generation transition. The first implementation initializes
  it but does not otherwise gate on it.

Each high-frequency cache line has one writer:

- provider writes `tail` and descriptor slots;
- consumer `i` writes only cursor `i` and its own state counters;
- all other accesses are reads.

SPMC uses 64-bit monotonic head/tail counters instead of the 32-bit
counters used by the existing SPSC queue. Rationale:

- The existing SPSC `uint32_t` counters wrap at 4G. This is acceptable
  because SPSC is point-to-point and the ring-index derivation
  `counter & mask` is correct regardless of wrap. However, SPMC has
  multiple independent consumers advancing at different rates. A slow
  consumer that pauses for a long time could see the provider's tail
  wrap past it, making the `tail - head > depth` overrun check
  unreliable with 32-bit arithmetic. 64-bit counters eliminate this
  concern entirely.
- SPSC stays at 32-bit. Changing SPSC counters would break the existing
  wire format. If a future SPSC v2 needs 64-bit counters, it should
  use a separate ABI version.

Depth must be a power of two in the same accepted range as SPSC queue depths.
The first implementation should reuse `OBMM_QUEUE_MIN_DEPTH`,
`OBMM_QUEUE_MAX_DEPTH`, and `OBMM_QUEUE_DEFAULT_DEPTH`.

The first implementation uses node-id-indexed cursors:

```text
consumer_idx == node_id
active_consumer_mask bit N means cursor[N] is active
provider_node may appear in the mask only if the provider also consumes
```

This wastes at most 64 cursor cache lines and avoids a second mapping table in
the first ABI. A dense stream-local index can be added later with an explicit
`node_id -> cursor_idx` directory field.

The first ABI does not store a consumer mask per descriptor. Therefore one SPMC
stream has one delivery set at a time: the current `active_consumer_mask`.
Fixed-mask fanout is represented by creating a stream whose initial active mask
is the desired subset, not by varying the mask on each publish. If the
application needs two different fanout groups concurrently, it should allocate
two SPMC streams. A future ABI can add per-slot or sideband masks for true
per-descriptor masked broadcast.

Layout helpers must be part of the implementation, rather than open-coded:

```c
uint64_t obmm_spmc_region_size(uint32_t depth, uint32_t max_consumers);
struct obmm_spmc_consumer_cursor *
obmm_spmc_cursor(struct obmm_spmc_stream *s, uint32_t node_id);
struct obmm_desc *obmm_spmc_desc_ring(struct obmm_spmc_stream *s);
```

The region size is:

```text
cursor_offset = align_up(sizeof(struct obmm_spmc_stream), 64)
desc_offset = align_up(cursor_offset +
                       max_consumers * sizeof(struct obmm_spmc_consumer_cursor),
                       64)
region_size = desc_offset + depth * sizeof(struct obmm_desc)
```

`obmm_spmc_stream_init()` must reject:

- non-power-of-two depth;
- depth outside the existing SPSC depth range;
- `max_consumers > OBMM_SPMC_MAX_CONSUMERS`;
- a `consumer_mask` bit outside `max_consumers`;
- any first-version caller-visible option that requests lossy mode. The simple
  first init helper may avoid a flags parameter entirely and always initialize
  `OBMM_SPMC_F_STRICT`.

The implementation should use a local runtime view for helpers that need both
the stream and its provider directory. This is not shared-memory wire format:

```c
struct obmm_spmc_stream_view {
    uint8_t *pool_base;
    uint64_t pool_size;
    const struct obmm_region_dirent *dir;
    uint32_t dir_count;
    uint32_t provider_node;
    struct obmm_spmc_stream *stream;
};
```

The view is built after local export initialization or after importing a peer
export. It lets publish/consume helpers validate descriptor region references
without adding process-local pointers to `struct obmm_spmc_stream`.

### Membership

SPMC membership is a control-plane action. It is not an implicit side effect of
polling the stream.

Initialization:

```text
provider initializes tail = 0
for each consumer bit in active_consumer_mask:
    cursor[node].head = 0
    cursor[node].observed_seq = 0
    cursor[node].drop_count = 0
    cursor[node].generation_seen = generation
    cursor[node].node_id = node
    cursor[node].state = ACTIVE
```

Attach after initialization is allowed only through a management path:

```text
1. management pauses publication or chooses a new generation
2. provider sets cursor[node].head = current tail
3. provider stores cursor[node].generation_seen = generation
4. provider stores cursor[node].state = ACTIVE
5. provider release-updates active_consumer_mask
```

Detach:

```text
1. consumer stores state = PAUSED or DETACHED after it stops reading
2. provider acquire-loads state
3. provider clears the consumer bit from active_consumer_mask
4. provider no longer includes that head in capacity or reclaim
```

Failure handling may mark a stuck consumer `DEAD` and clear its active bit, but
that is a policy decision outside the fast path. Correctness-critical streams
should prefer explicit management detach over timeout-based detach.

### Stream Initialization

`obmm_spmc_stream_init()` writes the stream header and all active cursors:

```c
int obmm_spmc_stream_init(void *base, uint32_t depth,
                           uint32_t max_consumers,
                           uint32_t provider_node,
                           uint64_t consumer_mask)
{
    struct obmm_spmc_stream *s = (struct obmm_spmc_stream *)base;
    uint64_t cursor_off, desc_off;

    if (depth < OBMM_QUEUE_MIN_DEPTH || depth > OBMM_QUEUE_MAX_DEPTH)
        return -EINVAL;
    if ((depth & (depth - 1)) != 0)
        return -EINVAL;
    if (max_consumers == 0 || max_consumers > OBMM_SPMC_MAX_CONSUMERS)
        return -EINVAL;
    if (consumer_mask != 0 &&
        (63 - __builtin_clzll(consumer_mask)) >= max_consumers)
        return -EINVAL;

    cursor_off = obmm_align_up_u64(sizeof(struct obmm_spmc_stream), 64);
    desc_off = obmm_align_up_u64(cursor_off +
                    (uint64_t)max_consumers * sizeof(struct obmm_spmc_consumer_cursor),
                    64);

    memset(base, 0, desc_off + (uint64_t)depth * sizeof(struct obmm_desc));

    s->magic = OBMM_SPMC_MAGIC;
    s->version = OBMM_SPMC_VERSION;
    s->flags = OBMM_SPMC_F_STRICT;
    s->generation = 1;
    s->header_bytes = sizeof(struct obmm_spmc_stream);
    s->cursor_offset = (uint32_t)cursor_off;
    s->desc_offset = (uint32_t)desc_off;
    s->depth = depth;
    s->mask = depth - 1;
    s->max_consumers = max_consumers;
    s->provider_node = provider_node;

    atomic_store_explicit(&s->active_consumer_mask, consumer_mask,
                          memory_order_relaxed);
    atomic_store_explicit(&s->tail, 0, memory_order_relaxed);

    uint32_t nid;
    OBMM_FOR_EACH_NODE_ID(nid, consumer_mask) {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, nid);
        c->node_id = nid;
        atomic_store_explicit(&c->generation_seen, s->generation,
                              memory_order_relaxed);
        atomic_store_explicit(&c->state, OBMM_SPMC_CONSUMER_ACTIVE,
                              memory_order_release);
    }

    return 0;
}
```

The `memset` zeroes inactive cursors so their state is
`OBMM_SPMC_CONSUMER_DETACHED` (enum value 0). The release-store on each
active cursor's state pairs with the acquire-load in `obmm_spmc_consume()`.

### Generation Lifecycle

`generation` in the stream header is a control-plane counter:

- Initialized to 1 during `obmm_spmc_stream_init()`.
- Incremented by `obmm_spmc_stream_reset()` when the provider reinitializes
  the stream after a fatal error (e.g. an overrun that invalidated the strict
  ring). This is a management operation that pauses publication, resets tail
  to 0, and reinitializes active cursors to head 0.
- Not incremented on attach or detach. These are mask/state transitions,
  not stream resets.
- `uint64_t` avoids wraparound in practice. No overflow handling needed.

Providers compare each cursor's `generation_seen` against the stream
generation to detect consumers that have not completed a transition.
In the first implementation this comparison is for monitoring only; the
provider does not gate publish or reclaim on it.

`obmm_spmc_stream_reset()` is distinct from `obmm_spmc_stream_init()`: init
constructs a new stream in zeroed storage, while reset preserves the stream
header placement and directory entry but starts a new generation. First-version
strict reset always discards in-flight descriptors. It must not attempt to
resume from `min_head` because the strict slot format has no per-slot sequence
number and cannot prove that a slow consumer's next slot is still valid.

```c
int obmm_spmc_stream_reset(struct obmm_spmc_stream *s,
                           uint64_t consumer_mask)
{
    uint64_t new_generation = s->generation + 1;
    uint32_t nid;

    if (consumer_mask != 0 &&
        (63 - __builtin_clzll(consumer_mask)) >= s->max_consumers)
        return -EINVAL;

    atomic_store_explicit(&s->active_consumer_mask, 0, memory_order_release);
    memset((uint8_t *)s + s->cursor_offset, 0,
           s->desc_offset - s->cursor_offset +
           (uint64_t)s->depth * sizeof(struct obmm_desc));

    s->generation = new_generation;
    atomic_store_explicit(&s->tail, 0, memory_order_relaxed);

    OBMM_FOR_EACH_NODE_ID(nid, consumer_mask) {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, nid);
        c->node_id = nid;
        atomic_store_explicit(&c->head, 0, memory_order_relaxed);
        atomic_store_explicit(&c->observed_seq, 0, memory_order_relaxed);
        atomic_store_explicit(&c->generation_seen, new_generation,
                              memory_order_relaxed);
        atomic_store_explicit(&c->state, OBMM_SPMC_CONSUMER_ACTIVE,
                              memory_order_release);
    }

    atomic_store_explicit(&s->active_consumer_mask, consumer_mask,
                          memory_order_release);
    return 0;
}
```

### Publish Path

The provider publishes one descriptor for all consumers:

```text
1. load tail locally
2. load each attached consumer head through local cacheable mapping
3. compute min_head across attached consumers
4. if tail - min_head >= depth, apply backpressure policy
5. write payload locally into provider TX arena
6. make payload visible to remote NC readers
7. write desc[tail & mask] locally
8. make descriptor visible to remote NC readers
9. release-store tail = tail + 1
10. optionally doorbell active consumers
```

The critical points are steps 6 and 8. C atomics order the local CPU, but they
do not by themselves prove that local cacheable writes become visible to peer
NC readers. The implementation must wrap the real visibility primitive behind
OBMM helpers:

```c
void obmm_publish_payload_for_remote_read(const void *addr, uint64_t len);
void obmm_publish_desc_for_remote_read(const void *addr, uint64_t len);
void obmm_publish_cursor_for_provider_read(const void *addr, uint64_t len);
```

These helpers should live in `obmm_queue.h`, next to the existing header-only
SPSC helpers, because they describe the common OBMM queue visibility boundary
rather than SPMC-specific logic. For the first user-space demo they may
initially be `static inline` wrappers around release fences while the validation
test proves the current mapping is already coherent enough. If validation
fails, these helpers must become the place where cache maintenance, an ioctl,
or an architecture-specific publish operation is added. The code must not claim
that a C11 release fence alone is the OBMM visibility contract.

Pseudo-code:

```c
int obmm_spmc_publish(struct obmm_spmc_stream_view *v,
                      const struct obmm_desc *desc)
{
    struct obmm_spmc_stream *s = v->stream;
    struct obmm_desc *ring = obmm_spmc_desc_ring(s);
    const void *payload_addr = NULL;
    int payload_rc;
    uint64_t active = atomic_load_explicit(&s->active_consumer_mask,
                                           memory_order_acquire);
    uint64_t wait_mask = active;
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
    uint64_t min_head = tail;
    uint32_t i;

    if (wait_mask == 0)
        return -ENODEV;

    OBMM_FOR_EACH_NODE_ID(i, wait_mask) {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, i);
        uint32_t state = atomic_load_explicit(&c->state,
                                              memory_order_acquire);
        if (state != OBMM_SPMC_CONSUMER_ACTIVE)
            return -EPIPE;

        uint64_t head = atomic_load_explicit(&c->head, memory_order_acquire);
        min_head = min(min_head, head);
    }

    if (tail - min_head >= s->depth)
        return -EAGAIN;

    payload_rc = obmm_spmc_provider_payload_addr(v, desc, &payload_addr);
    if (payload_rc < 0)
        return payload_rc;
    if (payload_rc > 0)
        obmm_publish_payload_for_remote_read(payload_addr, desc->payload_len);
    ring[tail & s->mask] = *desc;
    obmm_publish_desc_for_remote_read(&ring[tail & s->mask],
                                      sizeof(ring[tail & s->mask]));
    atomic_store_explicit(&s->tail, tail + 1, memory_order_release);
    return 0;
}
```

`obmm_spmc_publish()` uses the stream's current `active_consumer_mask`.
The first ABI does not allow the caller to provide a different mask for a
single descriptor because the ring slot does not record that mask. This keeps
capacity and reclamation derivable from cursor heads alone.

The publish path has an inherent time-of-check-to-time-of-use (TOCTOU)
window: between loading `active_consumer_mask` and storing `tail`, a consumer
may detach or its state may change. This is intentional and benign:

- If a consumer detaches after the active-mask load but before the tail store,
  the descriptor is written to the ring and the detached consumer will not read
  it. The descriptor occupies one slot that will be reclaimed when all
  remaining active consumers advance past it.
- If a consumer's state changes from ACTIVE to PAUSED/DEAD after the state
  check, the provider writes the descriptor anyway. The slot is not leaked;
  it is reclaimed once the provider observes the state change and clears that
  consumer from `active_consumer_mask`.
- The provider does not need to hold a lock across publish. Membership
  changes (attach/detach) are control-plane actions that are serialized
  outside the fast path.

`obmm_spmc_provider_payload_addr()` is a directory lookup through
`struct obmm_spmc_stream_view`, not a type-specific guess. The first
implementation must resolve the descriptor's `region_id` in the provider's
exported pool directory and validate the payload range against the matched
dirent before publication:

```text
dirent.region_id == desc->region_id
dirent.offset <= v->pool_size
dirent.size <= v->pool_size - dirent.offset
desc->payload_len == 0
    OR desc->payload_offset <= dirent.size
       AND desc->payload_len <= dirent.size - desc->payload_offset
```

The helper reports a provider-owned payload only when the matched dirent also
satisfies:

```text
dirent.kind == OBMM_REGION_TX_ARENA
dirent.peer_node_id == provider_node
desc->payload_len > 0
```

If the descriptor references any other valid region kind, SPMC publish must
skip the provider-payload visibility helper and rely on that region's own
publication contract. If the region is missing or the payload range is outside
the matched region, publish must return `-EINVAL` instead of publishing a bad
descriptor. Range validation must use subtraction as shown above rather than
`payload_offset + payload_len`, so overflow cannot make an invalid range look
valid.

Suggested helper signature:

```c
int obmm_spmc_provider_payload_addr(
    const struct obmm_spmc_stream_view *v,
    const struct obmm_desc *desc,
    const void **payload_addr_out);
```

Return values:

```text
 1: descriptor references provider TX arena; payload_addr_out is valid
 0: descriptor references another valid region or has no payload
-EINVAL: descriptor references a missing region, an out-of-pool dirent, or an
         out-of-bounds payload
```

When it returns 1, `*payload_addr_out` is:

```text
v->pool_base + tx_arena_dirent.offset + desc->payload_offset
```

### Consume Path

Each peer consumes independently:

```text
1. read this consumer cursor head from provider region through remote NC
2. acquire-load provider tail through remote NC
3. if head == tail, stream is empty for this consumer
4. if tail - head > depth, report overrun and do not read the slot
5. read descriptor from provider ring through remote NC
6. read payload through remote NC, if descriptor references provider TX arena
7. process payload
8. release-store this consumer's head = head + 1 through remote NC
```

Pseudo-code:

```c
int obmm_spmc_consume(struct obmm_spmc_stream_view *v,
                      uint32_t consumer_idx,
                      struct obmm_desc *desc)
{
    struct obmm_spmc_stream *s = v->stream;
    struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, consumer_idx);
    struct obmm_desc *ring = obmm_spmc_desc_ring(s);
    uint32_t state = atomic_load_explicit(&c->state, memory_order_acquire);
    uint64_t head = atomic_load_explicit(&c->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);

    if (state != OBMM_SPMC_CONSUMER_ACTIVE)
        return -ENODEV;

    if (head == tail)
        return -EAGAIN;

    if (tail - head > s->depth) {
        atomic_fetch_add_explicit(&c->drop_count, 1, memory_order_relaxed);
        atomic_store_explicit(&c->state, OBMM_SPMC_CONSUMER_PAUSED,
                              memory_order_release);
        obmm_publish_cursor_for_provider_read(c, sizeof(*c));
        return -EOVERFLOW;
    }

    *desc = ring[head & s->mask];
    atomic_store_explicit(&c->observed_seq, desc->seq, memory_order_relaxed);
    atomic_store_explicit(&c->head, head + 1, memory_order_release);
    obmm_publish_cursor_for_provider_read(c, sizeof(*c));
    return 0;
}
```

For provider-owned payloads, the consumer must update `head` only after it is
done reading the payload. Provider reclamation depends on all relevant consumer
heads advancing past a descriptor.

Because the first SPMC ABI does not include per-slot sequence numbers,
`-EOVERFLOW` is fatal for strict streams. The consumer must report the error to
the control plane instead of attempting to guess a safe resync point.

Overrun recovery is a control-plane decision, not a fast-path action:

1. The consumer increments `drop_count`, stores
   `state = OBMM_SPMC_CONSUMER_PAUSED`, publishes the cursor cache line for
   provider visibility, and returns `-EOVERFLOW` to its caller.
2. The control plane (application-level coordinator) decides:
   - **Stream reset**: provider pauses publication, calls
     `obmm_spmc_stream_reset()` with a new generation, resets `tail` to 0,
     and reinitializes active cursors to head 0. This discards all in-flight
     descriptors in the strict stream.
   - **Consumer detach**: provider clears the failed consumer's bit from
     `active_consumer_mask` and continues with the remaining consumers.
   - **Full teardown**: destroy the stream and recreate it.
3. The provider detects the PAUSED state via its next
   `obmm_spmc_reclaimable_head()` call or an explicit notification from the
   control plane. It does not auto-recover.

The first implementation must implement consumer PAUSED state storage and
provider state observation. Full stream reset as a management operation is
recommended but its mechanism (ioctl, shared memory flag, out-of-band signal)
is outside the queue layer.

### Backpressure Modes

SPMC needs an explicit policy because the slowest consumer can hold the whole
ring:

1. Strict broadcast: provider blocks or returns `-EAGAIN` when any attached
   consumer is slow.
2. Fixed-mask broadcast: provider initializes the stream with a subset of
   consumers and waits only for currently active consumers in that stream.
3. Loss-tolerant stream: provider may overwrite old slots and mark slow
   consumers overrun. This requires the separate lossy slot format below.

For W4/Qwen correctness paths, use strict or fixed-mask broadcast. Do not mix
different fanout masks in one first-version stream. Loss-tolerant mode is only
for telemetry or profiling streams and is not part of the first ABI.

Strict-mode publish must use this invariant:

```text
tail - min_head < depth
```

`tail` and `head` are 64-bit monotonic counters. Ring index is derived only at
the point of slot access:

```text
slot = counter & mask
```

With 64-bit counters, wraparound is not a practical test concern. Unit tests
should still force ring-index wraparound by using a small depth.

Loss-tolerant mode needs a different slot format:

```c
struct obmm_spmc_lossy_slot {
    _Atomic uint64_t seq;
    struct obmm_desc desc;
};
```

The producer writes `desc`, publishes descriptor visibility, and then
release-stores `seq = tail`. The consumer validates that the slot sequence
matches the expected `head` before accepting the descriptor. Without this
per-slot sequence, a slow consumer cannot distinguish a valid old slot from an
overwritten new slot.

This struct must not appear in the first implementation's header files. When
lossy mode is implemented in a later version, it should be added to
`obmm_pool_types.h` alongside a new `OBMM_SPMC_F_LOSSY` flag and a stream
header version bump. Including it under `#if 0` would create a maintenance
burden and give the false impression that the layout is frozen.

### Reclamation

Descriptor-slot reclamation uses the same min-head rule:

```text
reclaimable up to min(consumer[i].head for i in active_consumer_mask)
```

Pseudo-code:

```c
uint64_t obmm_spmc_reclaimable_head(struct obmm_spmc_stream_view *v)
{
    struct obmm_spmc_stream *s = v->stream;
    uint64_t wait_mask = atomic_load_explicit(&s->active_consumer_mask,
                                              memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    uint64_t min_head = tail;
    uint32_t i;

    if (wait_mask == 0)
        return tail;

    OBMM_FOR_EACH_NODE_ID(i, wait_mask) {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, i);
        uint32_t state = atomic_load_explicit(&c->state,
                                              memory_order_acquire);

        if (state != OBMM_SPMC_CONSUMER_ACTIVE)
            continue;

        min_head = min(min_head,
                       atomic_load_explicit(&c->head, memory_order_acquire));
    }

    return min_head;
}
```

The first ABI has no per-slot consumer mask, so reclaim is always computed
against the stream's active delivery set. If the application needs independent
reclaim for different fanout groups, it must use separate SPMC streams. If
ACK-lane mode is enabled, this helper should read the provider-local ACK state
that is maintained from reverse SPSC ACK descriptors instead of reading the
cursor lines directly.

Provider payload reclamation is caller-owned and must be separate from the
shared-memory stream ABI. The queue layer can provide a helper state for the
simple first implementation:

```c
struct obmm_spmc_tx_reclaim_state {
    uint64_t desc_reclaimed_to;
    uint64_t tx_reclaim_offset;
};

int obmm_spmc_reclaim_payloads(struct obmm_spmc_stream_view *v,
                               struct obmm_spmc_tx_reclaim_state *st)
{
    struct obmm_spmc_stream *s = v->stream;
    struct obmm_desc *ring = obmm_spmc_desc_ring(s);
    uint64_t reclaim_head = obmm_spmc_reclaimable_head(v);

    while (st->desc_reclaimed_to < reclaim_head) {
        struct obmm_desc *d = &ring[st->desc_reclaimed_to & s->mask];
        const void *payload_addr;
        int rc = obmm_spmc_provider_payload_addr(v, d, &payload_addr);

        if (rc < 0)
            return rc;
        if (rc > 0) {
            uint64_t end = d->payload_offset + d->payload_len;
            if (end > st->tx_reclaim_offset)
                st->tx_reclaim_offset = end;
        }
        st->desc_reclaimed_to++;
    }

    return 0;
}
```

This helper is valid only for a provider TX arena allocated monotonically by
offset without wraparound. It advances a high-water reclaim offset; it is not a
general free-list allocator. Descriptor-indexed fixed payload slots are simpler:
the provider may reuse `payload_slot = desc_seq & payload_slot_mask` once
`desc_seq < obmm_spmc_reclaimable_head(v)`. Variable-size wraparound reclaim
requires a provider-local allocation log or allocator metadata and is outside
the first queue helper.

The first implementation has two legal reclaim/backpressure modes:

1. Cursor mode: provider reads remote-written cursor heads from its local
   cacheable mapping. This mode is enabled only after the cursor visibility
   validation passes.
2. ACK-lane mode: each consumer sends consumed sequence ACKs back to the
   provider through an existing reverse SPSC lane. Provider publication still
   uses the SPMC broadcast log, but reclaim/backpressure uses ACK descriptors
   instead of directly polling remote-written cursor lines.

Cursor mode is simpler and lower overhead. ACK-lane mode is the correctness
fallback when remote NC writes into provider-owned memory are not reliably
visible to provider cacheable reads under stress.

For fixed-size payload slabs, prefer a descriptor-indexed ring:

```text
payload_slot = desc_seq & payload_slot_mask
```

This works when payload lifetime is tied to descriptor lifetime. For variable
payload sizes, the first helper only supports monotonic high-water reclaim.
Wraparound reuse requires provider-local allocator metadata and should be added
after the basic descriptor/cursor path is validated.

### Why Not Per-Consumer SPSC For SPMC

Per-consumer SPSC is still valid:

```text
provider writes queue[consumer0][provider]
provider writes queue[consumer1][provider]
...
```

But it performs N remote descriptor writes and N remote tail updates for one
logical broadcast. The provider-owned SPMC log performs one local descriptor
write and lets consumers remote-read. Under the current cacheability model this
is the better default for fanout.

Per-consumer SPSC remains useful when each consumer needs a different message,
when consumers require local-cacheable queue reads, or when fanout is tiny and
latency matters more than provider write amplification.

### Consumer Import Path

Consumers access a provider's SPMC stream through the same Phase 4 import path
used by the existing queue demo. A consumer imports the provider's whole
exported region; it does not import the SPMC stream as a separate object.

Consumer-side setup:

```text
1. receive or bootstrap the provider export metadata;
2. allocate an import PA/osync slot;
3. call obmm_do_import(obmm_fd, &provider_meta, local_cna, import_pa, osync);
4. mmap the imported region;
5. poll provider pool header until state == OBMM_POOL_STATE_READY;
6. validate pool header magic, layout version, node id, node count, and size;
7. scan provider directory for OBMM_REGION_SPMC_STREAM;
8. validate dirent offset/size and stream magic/version/depth/provider_node;
9. compute stream pointer as imported_base + dirent.offset;
10. build `struct obmm_spmc_stream_view` from imported base, mapped pool size,
    directory, directory count, provider node, and stream pointer.
```

Provider-local setup uses the same directory entry but resolves the stream
through the provider's own cacheable export mapping:

```text
provider_stream = local_export_base + spmc_dirent.offset
consumer_stream = imported_provider_base + spmc_dirent.offset
```

The consumer cursor lives inside the provider's stream, so a consumer's
`obmm_spmc_consume()` writes its cursor through the imported NC/osync mapping.
That write is exactly the visibility path that must be validated before cursor
mode is used for W4/Qwen correctness traffic.

### View Initialization

`obmm_spmc_view_init_from_directory()` resolves the SPMC stream from a pool
directory and builds the runtime view. It works for both provider-local and
consumer-imported mappings:

```c
int obmm_spmc_view_init_from_directory(struct obmm_spmc_stream_view *v,
                                        void *pool_base,
                                        uint64_t pool_size,
                                        const struct obmm_region_dirent *dir,
                                        uint32_t dir_count,
                                        uint32_t provider_node)
{
    const struct obmm_region_dirent *spmc_ent = NULL;

    for (uint32_t i = 0; i < dir_count; ++i) {
        if (dir[i].kind == OBMM_REGION_SPMC_STREAM) {
            if (spmc_ent != NULL)
                return -EEXIST;  /* multiple SPMC streams not supported */
            spmc_ent = &dir[i];
        }
    }
    if (spmc_ent == NULL)
        return -ENOENT;

    if (spmc_ent->offset > pool_size ||
        spmc_ent->size > pool_size - spmc_ent->offset)
        return -EINVAL;
    if (spmc_ent->size < sizeof(struct obmm_spmc_stream))
        return -EINVAL;

    struct obmm_spmc_stream *s =
        (struct obmm_spmc_stream *)((uint8_t *)pool_base + spmc_ent->offset);

    if (s->magic != OBMM_SPMC_MAGIC)
        return -EINVAL;
    if (s->version != OBMM_SPMC_VERSION)
        return -EINVAL;
    if (s->provider_node != provider_node)
        return -EINVAL;
    if (s->max_consumers == 0 || s->max_consumers > OBMM_SPMC_MAX_CONSUMERS)
        return -EINVAL;
    if (s->depth < OBMM_QUEUE_MIN_DEPTH || s->depth > OBMM_QUEUE_MAX_DEPTH)
        return -EINVAL;
    if ((s->depth & (s->depth - 1)) != 0)
        return -EINVAL;
    if (s->cursor_offset < sizeof(*s) || (s->cursor_offset & 63) != 0)
        return -EINVAL;
    if (s->desc_offset < s->cursor_offset ||
        (s->desc_offset & 63) != 0)
        return -EINVAL;
    if (s->desc_offset - s->cursor_offset <
        (uint64_t)s->max_consumers * sizeof(struct obmm_spmc_consumer_cursor))
        return -EINVAL;
    if (s->desc_offset > spmc_ent->size ||
        (uint64_t)s->depth * sizeof(struct obmm_desc) >
            spmc_ent->size - s->desc_offset)
        return -EINVAL;

    v->pool_base = (uint8_t *)pool_base;
    v->pool_size = pool_size;
    v->dir = dir;
    v->dir_count = dir_count;
    v->provider_node = provider_node;
    v->stream = s;
    return 0;
}
```

`pool_size` must be the mapped exported-region size from the validated pool
header or import metadata. The view initializer first proves that the SPMC
dirent is wholly inside that mapping, then validates all stream-internal
offsets relative to `spmc_ent->size`.

The view does not carry a flag distinguishing provider-local from
consumer-imported mappings. The difference matters only at the visibility
boundary, where the existing OBMM publish helpers are called by role
(provider calls `obmm_publish_desc_for_remote_read()`, consumer calls
`obmm_publish_cursor_for_provider_read()`), not by mapping type.

## MPSC: Consumer-Owned Lane Set

### Use Case

MPSC is for many publishers sending independent messages to one consumer:

- many range workers publish completion descriptors to one coordinator;
- many nodes publish object updates to one owner;
- many producers submit requests to one service node.

### Physical Layout

The recommended MPSC implementation is a logical queue built from SPSC lanes:

```text
consumer exported region
+--------------------------------------------------+
| pool header / directory                           |
+--------------------------------------------------+
| lane from publisher 0: obmm_spsc_queue            |
| lane from publisher 1: obmm_spsc_queue            |
| ...                                               |
+--------------------------------------------------+
| optional RX arena partitioned per publisher       |
+--------------------------------------------------+
```

This is already compatible with the existing SPSC layout:

```text
queue[consumer][publisher]
```

Each publisher has exactly one lane and writes only that lane's tail and
descriptor slots. The consumer drains all lanes locally.

### Lane-Set Helper Contract

MPSC does not add a new wire format. It adds local helper structs that group
existing `OBMM_REGION_QUEUE` entries:

```c
#define OBMM_MPSC_MAX_LANES OBMM_SPMC_MAX_CONSUMERS
#define OBMM_MPSC_DEFAULT_BUDGET 1

struct obmm_mpsc_lane {
    uint32_t publisher_node;
    uint32_t weight;
    uint32_t credit;
    struct obmm_spsc_queue *queue;
};

struct obmm_mpsc_consumer_set {
    uint32_t consumer_node;
    uint32_t lane_count;
    uint32_t next_lane;
    uint32_t budget;
    uint64_t rx_seq;
    struct obmm_mpsc_lane lane[OBMM_MPSC_MAX_LANES];
};

struct obmm_mpsc_publisher_lane {
    uint32_t publisher_node;
    uint32_t consumer_node;
    struct obmm_spsc_queue *queue;
};
```

Consumer-side initialization scans the local exported pool directory for all
SPSC queue dirents. The destination is the local consumer by construction
because this directory belongs to the consumer's exported pool; each queue
dirent's `peer_node_id` names the publisher:

```text
include dirent where:
    kind == OBMM_REGION_QUEUE
publisher_node = dirent.peer_node_id
```

The resulting `queue` pointers are local cacheable mappings because the
consumer owns the exported region. Lanes are sorted by `publisher_node` for
stable logs and deterministic tests.

Consumer-side initialization must return `-ENOENT` if no lanes match, and
`-E2BIG` if more than `OBMM_MPSC_MAX_LANES` lanes match. It must also reject
duplicate queue entries for the same `publisher_node`, because duplicate lanes
would make per-publisher ordering ambiguous.

Publisher-side initialization resolves exactly one remote queue by scanning the
target consumer's imported directory:

```text
include dirent where:
    kind == OBMM_REGION_QUEUE
    peer_node_id == local_publisher_node
```

The resulting `queue` pointer is an imported NC/osync mapping into the
consumer's exported region. `obmm_mpsc_push()` is only a typed wrapper over
`obmm_spsc_push()` on this lane.

Publisher-side initialization must return `-ENOENT` if the lane is missing and
`-EEXIST` if the directory contains more than one matching lane.

### Producer Path

The producer path is the existing SPSC push:

```text
publisher writes payload
publisher writes descriptor into consumer lane through remote NC
publisher release-stores lane tail through remote NC
```

There is no shared producer-side atomic state. Fullness and backpressure are
per-publisher:

```text
if lane[publisher] is full:
    return -EAGAIN or retry according to application policy
```

### Consumer Path

The consumer polls lanes with bounded fairness:

```text
for each poll round:
    for each publisher lane in round-robin order:
        pop at most one descriptor
        assign consumer-local receive sequence
```

Pseudo-code:

```c
int obmm_mpsc_poll(struct obmm_mpsc_consumer_set *set,
                   struct obmm_desc *out,
                   uint32_t *publisher_out,
                   uint64_t *rx_seq_out)
{
    for (uint32_t n = 0; n < set->lane_count; ++n) {
        uint32_t i = (set->next_lane + n) % set->lane_count;
        struct obmm_mpsc_lane *lane = &set->lane[i];

        if (obmm_spsc_pop(lane->queue, out) == 0) {
            if (rx_seq_out != NULL)
                *rx_seq_out = set->rx_seq++;
            set->next_lane = (i + 1) % set->lane_count;
            *publisher_out = lane->publisher_node;
            return 0;
        }
    }
    return -EAGAIN;
}
```

The single-descriptor `obmm_mpsc_poll()` rotates after every successful pop for
simple fairness. A later `obmm_mpsc_drain()` helper may drain up to
`set->budget` descriptors from a selected lane before rotating.

For higher throughput, use deficit round-robin:

```text
lane credit += lane weight
while credit > 0 and lane not empty:
    pop descriptor
    credit--
```

This prevents one hot publisher from starving cold publishers while still
allowing weighted priority.

### Ordering

MPSC has two different ordering concepts:

- Per-publisher order: guaranteed by each SPSC lane.
- Global consumer order: assigned by the consumer when it dequeues descriptors.

Do not try to force global order at the producers through a shared remote
counter. `desc.seq` remains the publisher-owned per-lane sequence. If the
application needs a total receive order, the consumer should return a separate
local `rx_seq` after dequeue. If the application needs causal order across
publishers, carry application-level epochs in the payload or a higher-level
descriptor field.

### Why Not A True Shared MPSC Ring

A true shared MPSC ring in the consumer's exported region would require all
publishers to update one shared `tail`:

```text
publisher0 remote atomic fetch_add tail
publisher1 remote atomic fetch_add tail
publisher2 remote atomic fetch_add tail
...
```

This is a bad default under the OBMM mapping model:

- remote atomics over NC/osync mappings may not be supported or may be slow;
- all producers contend on one cache line;
- failure or timeout while filling a reserved slot creates a hole the consumer
  must handle;
- correctness depends on remote atomic ordering that the SPSC design avoids.

A ticket-based true MPSC ring can be kept as a future slow path only if OBMM
explicitly exposes reliable remote atomic fetch-add and a per-slot ready flag
protocol:

```text
producer ticket = remote_fetch_add(tail)
producer writes slot[ticket & mask]
producer release-stores slot.ready = ticket_generation
consumer drains slots in ticket order
```

This should not be the first implementation.

## Region Directory Extensions

The existing directory can represent SPSC queues. For SPMC, add an explicit
region kind so peers do not confuse a broadcast log with an SPSC queue:

```c
enum obmm_region_kind {
    OBMM_REGION_QUEUE = 1,
    OBMM_REGION_RX_ARENA = 2,
    OBMM_REGION_TX_ARENA = 3,
    OBMM_REGION_DATA_SLAB = 4,
    OBMM_REGION_W4_PAYLOAD = 5,
    OBMM_REGION_SPMC_STREAM = 6,
};
```

For `OBMM_REGION_SPMC_STREAM`:

- `peer_node_id` should be `UINT16_MAX` or `0xffff` to indicate broadcast.
- `offset` points to `struct obmm_spmc_stream` in the provider's exported pool.
- `size` is the value returned by `obmm_spmc_region_size()`.
- `flags` should carry strict/fixed-mask mode and whether payloads are
  producer-owned. The first implementation must reject lossy-mode flags.
- the stream header carries the active delivery mask in `active_consumer_mask`.

Suggested directory flags:

```c
#define OBMM_REGION_F_SPMC_STRICT           (1u << 0)
#define OBMM_REGION_F_SPMC_FIXED_MASK       (1u << 1)
#define OBMM_REGION_F_SPMC_PRODUCER_PAYLOAD (1u << 2)
```

The directory is discovery metadata. The stream header is the source of truth
for depth, offsets, generation, and active consumers after the mapping is
opened.

MPSC does not need a new region kind if it is implemented as a lane set over
existing `OBMM_REGION_QUEUE` entries. A helper can group all
`queue[consumer][publisher]` entries into an `obmm_mpsc_consumer_set` at
runtime.

## Doorbells

Polling is still the first implementation because node counts are small.
Doorbells can be added later:

- SPMC: provider batches descriptors, updates `tail`, then rings all consumers
  in the stream's current `active_consumer_mask`.
- MPSC: each publisher batches descriptors, updates its lane `tail`, then rings
  the consumer.

Doorbells should never be one per descriptor by default. Use thresholds:

```text
ring when batch_count >= K
ring when elapsed_since_last_ring >= T
ring when queue transitions empty -> non-empty
```

## Memory Ordering Contract

SPMC publish:

```text
provider local cacheable payload writes
provider publish barrier for remote NC visibility
provider local descriptor write
provider release-store tail
consumer acquire-load tail through remote NC
consumer remote NC descriptor read
consumer remote NC payload read
consumer release-store own head through remote NC
provider acquire-load consumer heads before reuse
```

SPMC first implementation must expose these helper boundaries:

```c
void obmm_publish_payload_for_remote_read(const void *addr, uint64_t len);
void obmm_publish_desc_for_remote_read(const void *addr, uint64_t len);
void obmm_publish_cursor_for_provider_read(const void *addr, uint64_t len);
```

These helpers belong in `obmm_queue.h` as shared queue visibility helpers. The
first two helpers are called by the provider before publishing `tail`. The third
helper is called by a consumer after writing its cursor and before it expects
the provider to observe that cursor for reclaim/backpressure.

Initial user-space implementations may be release fences if the visibility
stress tests pass on the current QEMU/kernel model. The helpers exist so the
implementation has one place to add explicit cache maintenance or a driver
operation later. Do not spread raw `atomic_thread_fence()` calls through queue
logic as if they were the whole OBMM visibility contract.

MPSC publish:

```text
publisher payload write, local or remote depending on payload mode
publisher descriptor write into its lane
publisher release-store lane tail
consumer acquire-load lane tail locally
consumer local descriptor read
consumer release-store lane head locally
publisher acquire-load lane head through remote NC before slot reuse
```

The two visibility risks that must be validated are the same as SPSC:

- local cacheable payload writes must become visible to peer NC readers before
  descriptor publication;
- local cacheable head updates must become visible to peer NC readers before
  producers reuse queue slots.

SPMC adds a third risk:

- peer remote NC head updates into provider-owned cursor lines must become
  visible to the provider's local cacheable reads.

If this third path is not coherent enough for reliable polling, SPMC cursor
lines should either be mapped with explicit visibility semantics or updated via
a reverse SPSC ACK lane.

## Backpressure And Failure Handling

### SPMC

- Strict stream: slowest active consumer controls capacity.
- Fixed-mask stream: only consumers in the stream's active delivery set
  participate in capacity and reclamation.
- Detached consumer: provider clears its bit in `active_consumer_mask` after
  timeout or management action.
- Overrun in first-version strict/fixed-mask streams: consumer detects `tail - head
  > depth` (the slot at `head & mask` has been overwritten), increments
  `drop_count`, returns `-EOVERFLOW`, and leaves recovery to the control plane.
- Overrun in future lossy streams: consumer validates per-slot sequence,
  resyncs to a known-good slot, and increments `drop_count`.

### MPSC

- Each publisher has independent lane capacity.
- A slow consumer backpressures all publishers eventually, but hot publishers
  cannot overwrite cold publishers because lanes are independent.
- A failed publisher only affects its lane.
- A failed consumer is visible as all lanes becoming full.

## Sizing

For 8 nodes with depth 1024 and 32-byte descriptors:

SPMC stream descriptor ring:

```text
1024 * 32 = 32 KB
consumer cursors: 8 * 64 = 512 bytes
header and padding: less than 1 KB
```

MPSC lane set for one consumer with seven publishers:

```text
7 * (SPSC header + 1024 * 32) ~= 7 * 32 KB = 224 KB
```

Both are small relative to the current W4 DB OBMM region default of 64 MB per
node. Payload arenas, not descriptor queues, dominate capacity planning.

## API Sketch

SPMC:

```c
uint64_t obmm_spmc_region_size(uint32_t depth, uint32_t max_consumers);

int obmm_spmc_stream_init(void *base, uint32_t depth,
                          uint32_t max_consumers,
                          uint32_t provider_node,
                          uint64_t consumer_mask);

int obmm_spmc_stream_reset(struct obmm_spmc_stream *s,
                           uint64_t consumer_mask);

int obmm_spmc_view_init_from_directory(struct obmm_spmc_stream_view *v,
                                       void *pool_base,
                                       uint64_t pool_size,
                                       const struct obmm_region_dirent *dir,
                                       uint32_t dir_count,
                                       uint32_t provider_node);

int obmm_spmc_provider_payload_addr(
    const struct obmm_spmc_stream_view *v,
    const struct obmm_desc *desc,
    const void **payload_addr_out);

int obmm_spmc_publish(struct obmm_spmc_stream_view *v,
                      const struct obmm_desc *desc);

int obmm_spmc_consume(struct obmm_spmc_stream_view *v,
                      uint32_t consumer_idx,
                      struct obmm_desc *desc);

uint64_t obmm_spmc_reclaimable_head(struct obmm_spmc_stream_view *v);

int obmm_spmc_reclaim_payloads(struct obmm_spmc_stream_view *v,
                               struct obmm_spmc_tx_reclaim_state *st);
```

MPSC:

```c
int obmm_mpsc_consumer_set_init_from_directory(
    struct obmm_mpsc_consumer_set *set,
    const struct obmm_region_dirent *dir,
    uint32_t dir_count,
    uint32_t local_consumer_node);

int obmm_mpsc_publisher_lane_init_from_directory(
    struct obmm_mpsc_publisher_lane *lane,
    const struct obmm_region_dirent *consumer_dir,
    uint32_t dir_count,
    uint32_t local_publisher_node,
    uint32_t target_consumer_node);

int obmm_mpsc_push(struct obmm_mpsc_publisher_lane *lane,
                   const struct obmm_desc *desc);

int obmm_mpsc_poll(struct obmm_mpsc_consumer_set *set,
                   struct obmm_desc *desc,
                   uint32_t *publisher_node,
                   uint64_t *rx_seq);
```

`obmm_mpsc_push()` is a convenience wrapper over `obmm_spsc_push()` for the
publisher's resolved lane. The consumer-side `obmm_mpsc_poll()` owns fairness and
optional receive sequence assignment without rewriting `desc->seq`.

## W4/Qwen Usage

SPMC fits data that is produced once and consumed by multiple nodes:

- terminal token result broadcast;
- shared runtime metadata for a decode step;
- immutable weight or policy descriptor broadcast;
- one range's hidden output if multiple downstream consumers need the same
  tensor.

MPSC fits fan-in to a coordinator:

- per-node worker timing summaries;
- per-range completion events;
- object-service publish notifications;
- barrier readiness reports.

For the current range-forward pipeline, the predecessor-to-successor hidden
handoff remains SPSC because each range has one producer and one consumer. SPMC
becomes useful when the same range output must feed multiple peers. MPSC becomes
useful when a single coordinator needs to collect events from all workers.

## Demo Integration

### Demo Modes

The demo program must support multiple modes selected at startup:

```c
enum demo_mode {
    DEMO_MODE_FULLMESH = 0,   /* existing: all-to-all SPSC rounds */
    DEMO_MODE_SPMC     = 1,   /* one provider broadcasts to consumers */
    DEMO_MODE_COMBINED = 2,   /* SPSC + SPMC + MPSC combined validation */
};
```

Selection via `OBMM_DEMO_MODE` environment variable (default `fullmesh`).

Each mode reuses the existing Phase 1-4 bootstrap (identity, export, exchange,
import). Mode differences appear only in Phase 5 (rounds):

- **FULLMESH**: Existing `do_rounds()` + `do_queue_stress()`. No SPMC stream
  allocated. `directory_count == node_count` as before.
- **SPMC**: Provider node allocates one SPMC stream. Consumer nodes resolve
  the stream view from the provider's imported directory. The phase runs the
  SPMC broadcast protocol described below.
- **COMBINED**: Same as FULLMESH for SPSC lanes, plus one SPMC stream and one
  MPSC consumer set active simultaneously. Tests that the three queue types
  coexist in the same export region.

The SPMC-enabled layout is active whenever `OBMM_DEMO_MODE` is `spmc` or
`combined`. When the mode is `fullmesh`, no SPMC stream is allocated and the
layout is identical to the current format.

### SPMC Demo Protocol

```text
provider (node 0):
  1. allocate and init SPMC stream in export region
  2. for batch = 0..N-1:
       write payload to TX arena
       build descriptor with seq=batch, payload_offset, payload_len
       obmm_spmc_publish()
       if -EAGAIN: spin-wait and retry
  3. publish TERMINAL descriptor (type = OBMM_DESC_COMMIT, seq = N)
  4. wait for all consumer ACK descriptors via existing SPSC lanes

consumer (node i, i != 0):
  1. import provider region, resolve SPMC stream view
  2. loop:
       obmm_spmc_consume(view, my_node_id, &desc)
       if -EAGAIN: spin-wait and retry
       if descriptor type is COMMIT with seq == N: break
       verify payload checksum matches provider's published data
       increment local consumed count
  3. push ACK descriptor to provider's SPSC lane with consumed count
```

Completion: provider waits for ACK from every consumer, then prints summary.

### MPSC Demo Protocol

```text
publishers (nodes 0..N-2):
  1. resolve MPSC publisher lane to consumer (node N-1)
  2. for seq = 0..M-1:
       build descriptor with seq, type = OBMM_DESC_DATA
       obmm_mpsc_push() with retry on -EAGAIN
  3. push TERMINAL descriptor (type = OBMM_DESC_COMMIT)

consumer (node N-1):
  1. resolve MPSC consumer set from local export directory
  2. loop:
       obmm_mpsc_poll(&set, &desc, &pub_node, &rx_seq)
       if -EAGAIN: spin-wait and retry
       track per-publisher count and rx_seq monotonicity
       break when TERMINAL received from every publisher
  3. print per-publisher stats and global ordering summary
```

### Environment Variables

Full specification of demo knobs:

| Variable | Default | Values | Description |
|----------|---------|--------|-------------|
| `OBMM_DEMO_MODE` | `fullmesh` | `fullmesh`, `spmc`, `combined` | Demo phase 5 mode |
| `OBMM_POOL_EXPORT_SIZE_MB` | `2` | any positive integer | Export region size in MB |
| `OBMM_QUEUE_DEPTH` | `1024` | power-of-2 in [64, 65536] | SPSC queue depth |
| `OBMM_BOOTSTRAP` | `fm` | `fm`, `udp` | Bootstrap mode |
| `OBMM_BOOTSTRAP_SESSION` | `default` | any string | Session identifier |
| `OBMM_SPMC_DEPTH` | `1024` | power-of-2 in [64, 65536] | SPMC stream ring depth |
| `OBMM_SPMC_PROVIDER` | `0` | valid node ID | SPMC provider node |
| `OBMM_SPMC_MAX_CONSUMERS` | `64` | 1..64 | Max consumer slots in stream |
| `OBMM_SPMC_BATCH_COUNT` | `1000` | any positive integer | Descriptors to publish per demo |
| `OBMM_SPMC_SLOW_CONSUMER` | (none) | valid node ID | Consumer that sleeps between consumes |
| `OBMM_MPSC_CONSUMER` | `node_count - 1` | valid node ID | MPSC consumer node |
| `OBMM_MPSC_BATCH_COUNT` | `1000` | any positive integer | Descriptors per publisher |

Relationships:
- `OBMM_QUEUE_DEPTH` is shared by SPSC queues and MPSC lanes (MPSC lanes are
  SPSC queues). `OBMM_SPMC_DEPTH` is independent.
- When `OBMM_DEMO_MODE=spmc`, only `OBMM_SPMC_*` variables are relevant.
  Existing SPSC lanes are still initialized for the ACK path.
- `OBMM_POOL_EXPORT_SIZE_MB` must be large enough for the combined layout.
  The demo must compute the minimum required size and exit with an error
  message showing the required MB if the configured value is too small.

## Validation Plan

1. Host unit tests in `obmm_queue_test.c`:
   - SPMC layout:
     `sizeof(struct obmm_spmc_stream) == 128`,
     `sizeof(struct obmm_spmc_consumer_cursor) == 64`,
     `desc_offset` is 64-byte aligned, and region size matches the helper.
   - SPMC export layout places ingress SPSC queues before the SPMC stream and
     places the TX arena after the SPMC stream without changing per-peer queue
     slot ordering.
   - SPMC init rejects invalid depth, invalid consumer mask, too many
     consumers, and any future lossy-mode option.
   - SPMC init with valid parameters produces correct cursor_offset and
     desc_offset, zeroes inactive cursors, and sets active cursor state to
     ACTIVE with the expected generation.
   - SPMC view initialization resolves `OBMM_REGION_SPMC_STREAM` from a local
     or imported pool directory and rejects bad offset/size/magic/version.
   - SPMC view initialization rejects multiple SPMC stream entries (-EEXIST)
     and missing entries (-ENOENT).
   - `obmm_spmc_provider_payload_addr()` returns 1 for provider TX arena
     descriptors, 0 for valid non-provider-payload descriptors, and `-EINVAL`
     for missing regions, out-of-pool dirents, or out-of-bounds payload
     references.
   - SPMC strict publish/consume preserves descriptor order across ring-index
     wraparound.
   - SPMC full condition returns `-EAGAIN` when `tail - min_head >= depth`.
   - SPMC publish returns `-ENODEV` when `active_consumer_mask` is empty.
   - SPMC publish returns `-EPIPE` when a target consumer is not ACTIVE.
   - SPMC overrun returns `-EOVERFLOW` and increments `drop_count`.
   - SPMC consumer sets state to PAUSED after detecting overrun.
   - SPMC reclaimable_head returns the minimum across active consumer heads
     and skips PAUSED/DEAD consumers.
   - SPMC payload reclaim advances `desc_reclaimed_to` and
     `tx_reclaim_offset` for monotonically allocated provider TX payloads.
   - MPSC consumer set initialization groups local exported SPSC queue dirents,
     rejects duplicate publisher nodes, returns `-ENOENT` when no lanes match,
     and returns `-E2BIG` when too many match.
   - MPSC publisher lane initialization returns `-ENOENT` when the lane is
     missing and `-EEXIST` when multiple matching lanes exist.
   - MPSC poll preserves per-publisher order and assigns monotonic consumer
     `rx_seq`.
   - MPSC poll rotates fairly: no publisher starved when all lanes have
     traffic.
   - `OBMM_FOR_EACH_NODE_ID()` iterates all set bits in ascending order.
   - Export layout with SPMC enabled produces directory_count =
     node_count + 1 and valid offsets for all regions.
   - Export layout minimum size validation fails with a clear message when
     OBMM_POOL_EXPORT_SIZE_MB is too small for the enabled features.
2. Four-node SPMC guest demo:
   - node0 publishes one stream;
   - node1/node2/node3 consume at different rates;
   - strict mode must backpressure node0 when any active consumer is slow;
   - fixed-mask mode must keep publishing when the slow consumer is not a
     member of that stream's active delivery set.
3. Four-node MPSC guest demo:
   - node0/node1/node2 publish to node3;
   - node3 drains with round-robin polling;
   - validate per-publisher descriptor order and global receive sequence.
4. Eight-node scale demos:
   - SPMC: one provider and seven consumers;
   - MPSC: seven publishers and one consumer;
   - run with depth 64 and 1024 to cover wraparound and steady state.
5. Visibility stress tests:
   - provider local cacheable payload writes become visible to peer NC readers
     before `tail` publication;
   - provider local descriptor writes become visible to peer NC readers before
     `tail` publication;
   - consumer remote NC cursor writes become visible to provider local
     cacheable reads before reclaim;
   - publisher remote NC SPSC writes remain compatible with existing SPSC
     visibility tests.
6. W4/Qwen integration gate:
   - do not use SPMC for correctness-critical W4/Qwen traffic until the
     standalone SPMC visibility test passes;
   - if cursor visibility fails, switch SPMC reclaim/backpressure to reverse
     SPSC ACK-lane mode before W4/Qwen integration;
   - MPSC may be used for fan-in telemetry/control events once the lane-set
     demo passes.

Minimum guest-demo pass logs should include:

```text
obmm-spmc: mode=strict provider=0 consumers=0x0e depth=64 published=N
obmm-spmc: consumer=1 consumed=N drops=0 checksum=...
obmm-spmc: consumer=2 consumed=N drops=0 checksum=...
obmm-spmc: consumer=3 consumed=N drops=0 checksum=...
obmm-spmc: backpressure_seen=1 overflow_seen=0
obmm-spmc: PASS

obmm-mpsc: consumer=3 publishers=0x07 depth=64 received=N
obmm-mpsc: publisher=0 sent=A received=A reordered=0
obmm-mpsc: publisher=1 sent=B received=B reordered=0
obmm-mpsc: publisher=2 sent=C received=C reordered=0
obmm-mpsc: rx_seq_monotonic=1 max_fairness_gap=...
obmm-mpsc: PASS
```

Useful knobs (see Demo Integration section for full specification):

```text
OBMM_DEMO_MODE=fullmesh|spmc|combined
OBMM_SPMC_DEPTH=64|1024
OBMM_SPMC_PROVIDER=0
OBMM_SPMC_MAX_CONSUMERS=64
OBMM_SPMC_BATCH_COUNT=1000
OBMM_SPMC_SLOW_CONSUMER=<node_id>
OBMM_MPSC_CONSUMER=<node_id>
OBMM_MPSC_BATCH_COUNT=1000
OBMM_QUEUE_DEPTH=64|1024
OBMM_POOL_EXPORT_SIZE_MB=2|...
```

Suggested future scripts:

```text
guest-linux/aarch64/scripts/run_ub_four_node_obmm_spmc_demo.sh
guest-linux/aarch64/scripts/run_ub_eight_node_obmm_spmc_demo.sh
guest-linux/aarch64/scripts/run_ub_four_node_obmm_mpsc_demo.sh
guest-linux/aarch64/scripts/run_ub_eight_node_obmm_mpsc_demo.sh
```

## Recommendation

Implement SPMC as a provider-owned broadcast log and MPSC as a consumer-owned
set of SPSC lanes.

This keeps the fast path aligned with the OBMM cacheability model:

- SPMC favors one local provider write and multiple remote consumer reads.
- MPSC favors isolated remote publisher writes and one local consumer drain.
- Neither design needs cross-node CAS or a multi-writer remote tail.

The only part that needs careful hardware/model validation is cursor
visibility, especially SPMC consumer heads written remotely into provider-owned
memory and later read locally by the provider. If that path is not reliable
under pressure, use reverse SPSC ACK lanes for SPMC reclamation while keeping
the provider-owned broadcast log for descriptor publication.
