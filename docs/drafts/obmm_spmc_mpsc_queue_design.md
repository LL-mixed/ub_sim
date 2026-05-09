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
2. SPMC is implemented as a strict or masked provider-owned broadcast stream.
   It uses a new shared-memory ABI and 64-bit monotonic head/tail counters.
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

This keeps existing SPSC queue offsets stable relative to the current
`layout_queue_offset(peer_slot, node_count)` logic. Only `directory_count` and
`layout_tx_arena_offset(node_count)` need to change when SPMC is enabled.

For the first demo, allocate at most one SPMC stream per exported pool:

```text
spmc_stream_count = OBMM_SPMC_ENABLED ? 1 : 0
directory_count = peer_count + spmc_stream_count + 1 /* TX arena */

queue_base = align_up(directory_offset +
                      directory_count * sizeof(struct obmm_region_dirent),
                      64)
spmc_stream_offset = queue_base +
                     peer_count * obmm_queue_region_size(queue_depth)
tx_arena_offset = spmc_stream_offset +
                  spmc_stream_count *
                  obmm_spmc_region_size(spmc_depth, max_consumers)
```

`init_export_layout()` must initialize the stream before publishing
`OBMM_POOL_STATE_READY`, just like it initializes each ingress SPSC queue today.
Peer layout validation must stop requiring `directory_count == node_count`;
SPMC-capable validation should accept at least the existing SPSC/TX entries and
scan any additional entries by `kind`.

First-version wire format:

```c
#define OBMM_SPMC_MAGIC 0x4f424d53504d4301ULL /* "OBMSPMC" */
#define OBMM_SPMC_VERSION 1
#define OBMM_SPMC_MAX_CONSUMERS 64

#define OBMM_SPMC_F_STRICT          (1u << 0)
#define OBMM_SPMC_F_MASKED          (1u << 1)
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

Each high-frequency cache line has one writer:

- provider writes `tail` and descriptor slots;
- consumer `i` writes only cursor `i` and its own state counters;
- all other accesses are reads.

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
- a first-version request with lossy-mode flags.

The implementation should use a local runtime view for helpers that need both
the stream and its provider directory. This is not shared-memory wire format:

```c
struct obmm_spmc_stream_view {
    uint8_t *pool_base;
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
10. optionally doorbell selected consumers
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
                      const struct obmm_desc *desc,
                      uint64_t consumer_mask)
{
    struct obmm_spmc_stream *s = v->stream;
    struct obmm_desc *ring = obmm_spmc_desc_ring(s);
    const void *payload_addr = NULL;
    int payload_rc;
    uint64_t active = atomic_load_explicit(&s->active_consumer_mask,
                                           memory_order_acquire);
    uint64_t wait_mask = consumer_mask;
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_relaxed);
    uint64_t min_head = tail;

    if ((consumer_mask & ~active) != 0)
        return -ENODEV;

    if (wait_mask == 0)
        return -EINVAL;

    for_each_node_id(i, wait_mask) {
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

`consumer_mask` is a per-publish mask. It must be a subset of
`active_consumer_mask` for strict broadcast. Masked broadcast may pass a smaller
mask, but only the selected consumers participate in backpressure and
reclamation for that descriptor class.

`obmm_spmc_provider_payload_addr()` is a directory lookup through
`struct obmm_spmc_stream_view`, not a type-specific guess. The first
implementation should resolve the descriptor's `region_id` in the provider's
exported pool directory and report a provider-owned payload only when:

```text
dirent.region_id == desc->region_id
dirent.kind == OBMM_REGION_TX_ARENA
dirent.peer_node_id == provider_node
desc->payload_len > 0
desc->payload_offset + desc->payload_len <= dirent.size
```

If the descriptor references any other region kind, SPMC publish must skip the
provider-payload visibility helper and rely on that region's own publication
contract. If `payload_offset + payload_len` is outside the region, publish must
return `-EINVAL` instead of publishing a bad descriptor.

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
-EINVAL: descriptor references a missing region or an out-of-bounds payload
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

    if (tail - head > s->depth)
        return -EOVERFLOW;

    *desc = ring[head & s->mask];
    atomic_store_explicit(&c->head, head + 1, memory_order_release);
    obmm_publish_cursor_for_provider_read(&c->head, sizeof(c->head));
    return 0;
}
```

For provider-owned payloads, the consumer must update `head` only after it is
done reading the payload. Provider reclamation depends on all relevant consumer
heads advancing past a descriptor.

Because the first SPMC ABI does not include per-slot sequence numbers,
`-EOVERFLOW` is fatal for strict streams. The consumer must report the error to
the control plane instead of attempting to guess a safe resync point.

### Backpressure Modes

SPMC needs an explicit policy because the slowest consumer can hold the whole
ring:

1. Strict broadcast: provider blocks or returns `-EAGAIN` when any attached
   consumer is slow.
2. Masked broadcast: provider only waits for consumers in `consumer_mask`.
3. Loss-tolerant stream: provider may overwrite old slots and mark slow
   consumers overrun. This requires the separate lossy slot format below.

For W4/Qwen correctness paths, use strict or masked broadcast. Loss-tolerant
mode is only for telemetry or profiling streams and is not part of the first
ABI.

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

### Reclamation

Provider-owned payload reclamation uses the same min-head rule:

```text
reclaimable up to min(consumer[i].head for i in active consumer_mask)
```

Pseudo-code:

```c
uint64_t obmm_spmc_reclaimable_head(struct obmm_spmc_stream_view *v,
                                    uint64_t consumer_mask)
{
    struct obmm_spmc_stream *s = v->stream;
    uint64_t active = atomic_load_explicit(&s->active_consumer_mask,
                                           memory_order_acquire);
    uint64_t wait_mask = active & consumer_mask;
    uint64_t tail = atomic_load_explicit(&s->tail, memory_order_acquire);
    uint64_t min_head = tail;

    if (wait_mask == 0)
        return tail;

    for_each_node_id(i, wait_mask) {
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

For strict broadcast, callers pass the stream's full active mask. For masked
broadcast, callers pass the descriptor class mask that was used for
publication. If ACK-lane mode is enabled, this helper should read the
provider-local ACK state that is maintained from reverse SPSC ACK descriptors
instead of reading the cursor lines directly.

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

Payload slabs can use a descriptor-indexed ring:

```text
payload_slot = desc_seq & payload_slot_mask
```

This works when payload lifetime is tied to descriptor lifetime. For variable
payload sizes, the provider can use an arena cursor plus per-descriptor
`payload_offset` and reclaim by walking completed descriptors.

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
10. build `struct obmm_spmc_stream_view` from imported base, directory,
    directory count, provider node, and stream pointer.
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

Consumer-side initialization scans the local exported pool directory for queues
whose destination is the local consumer. The pool owner is implicit because this
directory belongs to the consumer's exported pool:

```text
include dirent where:
    kind == OBMM_REGION_QUEUE
    peer_node_id == publisher_node
```

The resulting `queue` pointers are local cacheable mappings because the
consumer owns the exported region. Lanes are sorted by `publisher_node` for
stable logs and deterministic tests.

Consumer-side initialization must return `-ENOENT` if no lanes match, and
`-E2BIG` if more than `OBMM_MPSC_MAX_LANES` lanes match.

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
- `flags` should carry strict/masked mode and whether payloads are
  producer-owned. The first implementation must reject lossy-mode flags.
- the stream header carries the active `consumer_mask`.

Suggested directory flags:

```c
#define OBMM_REGION_F_SPMC_STRICT           (1u << 0)
#define OBMM_REGION_F_SPMC_MASKED           (1u << 1)
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
  in `consumer_mask`.
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
- Masked stream: only consumers in `consumer_mask` participate in capacity and
  reclamation.
- Detached consumer: provider clears its bit in `active_consumer_mask` after
  timeout or management action.
- Overrun in first-version strict/masked streams: consumer detects `tail - head
  > depth`, increments `drop_count`, returns `-EOVERFLOW`, and leaves recovery
  to the control plane.
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

int obmm_spmc_view_init_from_directory(struct obmm_spmc_stream_view *v,
                                       void *pool_base,
                                       const struct obmm_region_dirent *dir,
                                       uint32_t dir_count,
                                       uint32_t provider_node);

int obmm_spmc_provider_payload_addr(
    const struct obmm_spmc_stream_view *v,
    const struct obmm_desc *desc,
    const void **payload_addr_out);

int obmm_spmc_publish(struct obmm_spmc_stream_view *v,
                      const struct obmm_desc *desc,
                      uint64_t consumer_mask);

int obmm_spmc_consume(struct obmm_spmc_stream_view *v,
                      uint32_t consumer_idx,
                      struct obmm_desc *desc);

uint64_t obmm_spmc_reclaimable_head(struct obmm_spmc_stream_view *v,
                                    uint64_t consumer_mask);
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
     consumers, and lossy flags.
   - SPMC view initialization resolves `OBMM_REGION_SPMC_STREAM` from a local
     or imported pool directory and rejects bad offset/size/magic/version.
   - `obmm_spmc_provider_payload_addr()` returns 1 for provider TX arena
     descriptors, 0 for valid non-provider-payload descriptors, and `-EINVAL`
     for missing or out-of-bounds payload references.
   - SPMC strict publish/consume preserves descriptor order across ring-index
     wraparound.
   - SPMC full condition returns `-EAGAIN` when `tail - min_head >= depth`.
   - SPMC overrun returns `-EOVERFLOW` and increments `drop_count`.
   - MPSC set initialization groups only matching SPSC lanes.
   - MPSC poll preserves per-publisher order and assigns monotonic consumer
     `rx_seq`.
2. Four-node SPMC guest demo:
   - node0 publishes one stream;
   - node1/node2/node3 consume at different rates;
   - strict mode must backpressure node0 when any active consumer is slow;
   - masked mode must keep publishing when the slow consumer is not in the
     publish mask.
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

Useful knobs:

```text
OBMM_SPMC_DEPTH=64|1024
OBMM_SPMC_MODE=strict|masked
OBMM_SPMC_PROVIDER=0
OBMM_SPMC_CONSUMER_MASK=0xfe
OBMM_SPMC_SLOW_CONSUMER=3
OBMM_SPMC_ACK_MODE=cursor|spsc_ack

OBMM_MPSC_DEPTH=64|1024
OBMM_MPSC_CONSUMER=7
OBMM_MPSC_PUBLISHER_MASK=0x7f
OBMM_MPSC_POLL_BUDGET=1
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
