# OBMM Shared Memory Pool Lockless Queue Design

## Goal

Build an efficient data sharing and control communication layer on top of the
current OBMM shared memory pool. Each node exports a large OBMM region, imports
peer regions, and can access the mapped regions through regular load/store.

The primary design target is a four-node or eight-node full-mesh topology where
all nodes need low-overhead message exchange and optional payload sharing.

## Design Summary

Use an owner-sharded shared memory layout and build one single-producer,
single-consumer queue per source/destination pair:

```text
queue[dst][src] = messages from src to dst
```

The queue storage lives in the destination node's exported OBMM region. The
source node writes descriptors into that remote region, and the destination node
consumes them from its own local exported memory.

For example, traffic from `nodeA` to `nodeB` uses `nodeB`'s `from_nodeA`
ingress queue:

```text
nodeA writes -> nodeB exported memory -> nodeB reads locally
```

This avoids a global queue, multi-producer contention, and cross-node atomic
compare-and-swap. A four-node full mesh needs 12 queues. An eight-node full mesh
needs 56 queues.

## OBMM Cacheability Model

The current OBMM shared memory pool is asymmetric:

- A node accesses its own exported region through a cacheable mapping.
- A node accesses another node's exported region through an osync, non-cacheable
  mapping.

This cacheability model is a major reason to put `queue[dst][src]` in the
destination node's exported region:

```text
src producer: remote osync stores into dst queue and payload arena
dst consumer: local cacheable loads from its own queue and payload arena
```

That is a good fit for receiver-owned ingress queues. The producer performs
ordered non-cacheable remote stores, while the consumer reads from its local
cacheable memory. The reverse direction, especially consumer updates to `head`,
must be designed carefully because the producer later reads that field through
an osync remote mapping.

The design therefore separates producer-owned and consumer-owned fields onto
different cache lines. This avoids a cacheable local writer and a non-cacheable
remote writer sharing the same line.

## Exported Region Layout

Each node divides its exported OBMM region into control metadata, per-peer
ingress queues, and optional payload arenas:

```text
node N exported region
+------------------------------+
| pool header                  |
+------------------------------+
| ingress queue from node0     |
| ingress queue from node1     |
| ...                          |
+------------------------------+
| payload arena from node0     |
| payload arena from node1     |
| ...                          |
+------------------------------+
| producer-owned data slabs    |
+------------------------------+
```

The important rule is that high-frequency metadata must be single-writer:

- `queue[dst][src].tail` is written only by `src`.
- `queue[dst][src].head` is written only by `dst`.
- payload arenas are partitioned by producer when remote writers are used.

Each node is responsible for initializing the queue, metadata, and arena layout
inside the region that it exports. Peers must treat another node's exported
layout as read/write data, not as something they allocate or initialize. A
typical startup sequence is:

```text
1. node initializes its own exported region header
2. node creates all local ingress queues queue[node][peer]
3. node creates per-peer payload arenas owned by this exported region
4. node announces the finalized metadata in its own exported region
5. peers import the region, read the announcement, and resolve region_id -> mapped base + size
```

This keeps ownership clear: the exporter owns layout construction and versioning;
peers only use the advertised offsets and sizes.

## Metadata Announcement

Metadata exchange is shared-memory based. Each node announces the layout of its
own exported region inside that same exported region. Peers discover the layout
by importing the region and reading the exported metadata header. No peer should
write another node's metadata.

Use a two-phase publish protocol:

```text
1. exporter writes all metadata fields with state = INIT
2. exporter writes queue headers and arena descriptors
3. exporter release-stores generation
4. exporter release-stores state = READY
5. peer acquire-loads state until READY
6. peer reads generation, metadata, and queue/arena directory
```

The metadata should include enough information for peers to avoid hardcoded
layout assumptions. A suggested region header is one 64-byte cache line:

```c
#define OBMM_POOL_MAGIC 0x4f424d51504f4f4cULL /* "OBMQPOOL" */
#define OBMM_POOL_LAYOUT_VERSION 1U

enum obmm_pool_state {
    OBMM_POOL_STATE_INIT = 0,
    OBMM_POOL_STATE_READY = 1,
    OBMM_POOL_STATE_ERROR = 2,
};

struct obmm_pool_header {
    uint64_t magic;
    uint32_t layout_version;
    uint16_t node_id;
    uint16_t node_count;
    _Atomic uint32_t state;
    _Atomic uint32_t generation;
    uint64_t region_size;
    uint64_t directory_offset;
    uint32_t directory_count;
    uint32_t default_queue_depth;
    uint32_t flags;
    uint32_t reserved[3];
};
```

The directory entries describe queues, arenas, and other subregions. Keep each
entry 32 bytes so directory scans are compact and naturally aligned on arm64:

```c
enum obmm_region_kind {
    OBMM_REGION_QUEUE = 1,
    OBMM_REGION_RX_ARENA = 2,
    OBMM_REGION_TX_ARENA = 3,
    OBMM_REGION_DATA_SLAB = 4,
};

struct obmm_region_dirent {
    uint32_t region_id;
    uint16_t kind;
    uint16_t peer_node_id;
    uint64_t offset;
    uint64_t size;
    uint32_t flags;
    uint32_t reserved;
};
```

The meaning of `peer_node_id` depends on `kind`:

- `OBMM_REGION_QUEUE`: ingress queue from `peer_node_id` to this exporter.
- `OBMM_REGION_RX_ARENA`: receiver-owned payload arena writable by
  `peer_node_id`.
- `OBMM_REGION_TX_ARENA`: producer-owned payload arena owned by this exporter.
- `OBMM_REGION_DATA_SLAB`: generic exported data area owned by this exporter.

The implementation should enforce:

```c
static_assert(sizeof(struct obmm_pool_header) == 64);
static_assert(sizeof(struct obmm_region_dirent) == 32);
static_assert(alignof(struct obmm_pool_header) >= 8);
static_assert(alignof(struct obmm_region_dirent) >= 8);
```

## SPSC Queue

Each ingress queue is a fixed-size ring with configurable power-of-two depth.
The default depth is 1024 descriptors. This default must not be hardcoded into
the data structure or protocol; it should be carried in queue metadata and
overridable by configuration.

Head and tail are isolated onto separate cache lines because they are written by
different nodes and observed through different mapping attributes.

```c
#define OBMM_QUEUE_DEFAULT_DEPTH 1024U
#define OBMM_QUEUE_MIN_DEPTH 64U
#define OBMM_QUEUE_MAX_DEPTH 65536U

struct obmm_spsc_queue {
    alignas(64) _Atomic uint32_t head;  /* consumer-owned */
    uint8_t head_pad[60];

    alignas(64) _Atomic uint32_t tail;  /* producer-owned */
    uint8_t tail_pad[60];

    uint32_t size;
    uint32_t mask;
    uint8_t reserved[56];

    alignas(64) struct obmm_desc desc[];
};
```

`size` is the configured queue depth and `mask` is `size - 1`. Initialization
must reject depths that are not powers of two or that fall outside the supported
range.

For the exported queue format, keep the queue header immediately before the
descriptor ring. The queue's directory entry points to the queue header:

```text
queue region
+------------------------------+
| obmm_spsc_queue header       |
+------------------------------+
| obmm_desc[0]                 |
| obmm_desc[1]                 |
| ...                          |
| obmm_desc[size - 1]          |
+------------------------------+
```

The byte size of a queue region is:

```text
align_up(sizeof(struct obmm_spsc_queue), 64) + size * sizeof(struct obmm_desc)
```

Queue initialization is performed by the exporter:

```text
head = 0
tail = 0
size = configured_depth
mask = configured_depth - 1
desc[] = zeroed or left undefined until published by producer
```

## Data Layout and Alignment

The shared data structures should balance compactness and access efficiency,
with arm64 as the primary target:

- Keep hot producer-owned and consumer-owned indices on separate 64-byte cache
  lines.
- Keep descriptor fields naturally aligned: 64-bit fields on 8-byte boundaries,
  32-bit fields on 4-byte boundaries.
- Prefer fixed-size descriptors so queue indexing is cheap and predictable.
- Avoid packed structs for hot-path metadata because unaligned arm64 accesses
  are slower and may complicate atomic access.
- Keep offsets and sizes explicit; do not rely on compiler-specific padding for
  wire/shared-memory compatibility.
- Use compile-time size and alignment checks in the implementation.

Descriptors should stay compact and fixed-size. A 32-byte descriptor is the
first target because it gives two descriptors per 64-byte cache line while still
carrying enough information for region-relative payload addressing:

```c
struct obmm_desc {
    uint64_t seq;
    uint32_t region_id;
    uint32_t payload_len;
    uint64_t payload_offset;
    uint16_t type;
    uint16_t flags;
    uint32_t cookie;
    uint32_t reserved;
};
```

The implementation should enforce:

```c
static_assert(sizeof(struct obmm_desc) == 32);
static_assert(alignof(struct obmm_desc) >= 8);
```

`payload_offset` is a byte offset inside the region or arena identified by
`region_id`. It is not a process-local virtual address and it is not a guest
physical address. The effective address is:

```text
payload_addr = mapped_region_base[region_id] + payload_offset
```

`mapped_region_base[region_id]` is local to each process or guest. For the same
descriptor, the producer and consumer may use different virtual bases, but they
must agree on the same `region_id` and byte offset. This keeps descriptors
stable across guests and mappings.

## Producer Path

The producer is the source node. It appends descriptors to a queue stored in the
destination node's exported memory. These descriptor and `tail` writes are
remote osync, non-cacheable stores.

```c
int obmm_spsc_push(struct obmm_spsc_queue *q, const struct obmm_desc *desc)
{
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
    uint32_t head = atomic_load_explicit(&q->head, memory_order_acquire);

    if (tail - head == q->size)
        return -EAGAIN;

    q->desc[tail & q->mask] = *desc;

    atomic_store_explicit(&q->tail, tail + 1, memory_order_release);
    return 0;
}
```

For a receiver-owned payload, the producer writes the payload first, then
publishes the descriptor with a release-store to `tail`.

## Consumer Path

The consumer is the destination node. It polls its local ingress queues, one per
peer. These descriptor and `tail` reads are local cacheable loads from the
consumer's own exported region. The `head` update is a local cacheable store,
which the producer observes later through its remote osync mapping.

```c
int obmm_spsc_pop(struct obmm_spsc_queue *q, struct obmm_desc *desc)
{
    uint32_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

    if (head == tail)
        return -EAGAIN;

    *desc = q->desc[head & q->mask];

    atomic_store_explicit(&q->head, head + 1, memory_order_release);
    return 0;
}
```

The destination node should poll all ingress queues with bounded batching:

```text
for each peer:
    consume at most N descriptors from queue[local][peer]
```

This prevents one busy peer from starving the others.

## Payload Placement

There are two useful payload modes.

### Receiver-Owned Payload

The producer writes payload bytes directly into a per-producer arena in the
destination node's exported region, then publishes a descriptor to the
destination ingress queue.

```text
src writes payload -> dst.payload_arena_from_src
src writes desc    -> dst.queue_from_src
dst reads payload locally
```

This is best for control messages, medium-size messages, and data that the
receiver will process immediately.

For receiver-owned payloads:

```text
region_id      = dst.arena_from_src
payload_offset = byte offset inside dst.arena_from_src
payload_addr   = dst_arena_from_src_base + payload_offset
```

The source calculates `payload_addr` from its imported osync mapping of the
destination arena. The destination calculates the same logical payload address
from its local cacheable mapping of that arena.

To keep this lockless, each destination node reserves one arena per producer:

```text
nodeB exported region
| queue_from_nodeA   |
| arena_from_nodeA   |
| queue_from_nodeC   |
| arena_from_nodeC   |
```

`nodeA` never allocates from `arena_from_nodeC`, so no cross-producer allocator
lock is needed.

### Producer-Owned Payload

The producer stores payload in its own exported region and publishes only a
descriptor to the destination queue.

```text
src writes payload locally
src writes desc -> dst.queue_from_src
dst remote-reads payload from src exported memory
dst sends completion -> src
src reclaims payload
```

This is best for large payloads and zero-copy sharing. It requires an explicit
completion path so the producer knows when the payload can be reclaimed.

For producer-owned payloads:

```text
region_id      = src.producer_payload_arena
payload_offset = byte offset inside src.producer_payload_arena
payload_addr   = src_payload_arena_base + payload_offset
```

The source writes the payload through its local cacheable mapping. The
destination reads it through its imported osync mapping of the source arena.

## Completion Queues

For producer-owned payloads, use a second queue in the reverse direction:

```text
request/data: src -> dst, queue[dst][src]
completion:   dst -> src, queue[src][dst]
```

The completion descriptor carries the original `seq` or `cookie`. The producer
uses it to release the payload slot.

Receiver-owned payloads can usually be reclaimed locally by the destination
after consumption, so they do not require a completion descriptor unless the
application protocol needs an acknowledgement.

## Memory Ordering

At minimum, queue publication needs release/acquire semantics:

- Producer writes payload and descriptor.
- Producer release-stores `tail`.
- Consumer acquire-loads `tail`.
- Consumer reads descriptor and payload.
- Consumer release-stores `head`.
- Producer acquire-loads `head` before reusing slots.

For kernel code, use `smp_store_release()` and `smp_load_acquire()` for queue
indices. For user-space demos, use C11 atomics with `memory_order_release` and
`memory_order_acquire`.

The OBMM pool's asymmetric mapping model refines this requirement:

- Producer descriptor/payload writes to a peer region are remote osync,
  non-cacheable stores.
- Producer `tail` publication is also a remote osync store and must be ordered
  after the descriptor/payload stores.
- Consumer reads are local cacheable reads from its own exported region.
- Consumer `head` updates are local cacheable stores that must become visible to
  peer osync readers before the peer reuses queue slots.

C11 atomics constrain compiler and local CPU ordering, but they do not by
themselves define the visibility rule between a local cacheable writer and a
remote osync reader. The queue contract must therefore be validated against the
OBMM mapping implementation. If local cacheable updates to queue metadata are
not automatically visible to remote osync readers, `head` publication needs a
flush/cache-maintenance step, or the queue metadata page should use a mapping
mode that makes `head` visibility explicit.

For this reason, the first implementation should include a stress test that
fills and drains queues repeatedly, forcing producer-side `head` reads and slot
reuse. Passing only a never-full queue test is not enough to prove the
cacheability contract.

## Doorbells and Polling

The first implementation should use polling because the queues are in shared
memory and the full-mesh node count is small.

Later, a doorbell can be added:

```text
producer writes descriptor(s)
producer updates tail
producer rings destination doorbell
```

Doorbells should be batched. Ringing once per descriptor defeats most of the
benefit of the shared-memory queue.

## Backpressure

If `tail - head == size`, the producer has three options:

1. Return `-EAGAIN` and let the caller retry.
2. Poll completions and retry.
3. Fall back to a larger producer-owned payload path if the queue only lacks
   descriptor space.

Dropping descriptors should be an explicit application-level policy, not the
default queue behavior.

## Validation Plan

1. Implement `obmm_spsc_queue` as a small reusable helper for user-space demos.
2. Replace the current UDP ACK/COMMIT path in the four-node OBMM pool demo with
   OBMM queue descriptors.
3. Validate four-node full mesh with `QEMU_MEM=8G`, `QEMU_SMP=4`,
   `pmd_mapping=100%`, `obmm.mempool_size=0`, and
   `OBMM_POOL_EXPORT_SIZE_MB=7680`.
4. Add receiver-owned payload arenas and verify remote write plus local read.
5. Add producer-owned payload descriptors and completion queues for large
   payloads.
6. Scale the same queue matrix to eight nodes and validate fairness under mixed
   producer rates.

## Key Constraints

- Use one SPSC queue per source/destination pair.
- Keep each high-frequency cache line single-writer.
- Do not build a global MPSC queue with remote atomic contention.
- Do not require cross-node CAS for the fast path.
- Use `region_id + payload_offset` instead of process-local pointers in
  descriptors.
- Keep descriptors fixed-size and cacheline-friendly.
- Treat the OBMM cacheability asymmetry as part of the queue contract:
  local-owner access is cacheable, peer access is osync/non-cacheable.
- Validate `head` visibility from a local cacheable consumer store to a remote
  osync producer load before relying on slot reuse under pressure.
