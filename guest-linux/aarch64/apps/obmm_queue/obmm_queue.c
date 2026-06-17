/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM lockless queue app -- multi-node full mesh.
 *
 * Builds on top of the OBMM shared memory pool.  Each node exports one
 * region, imports peer regions, and communicates through SPSC ingress
 * queues stored in the destination node's exported memory.
 *
 * Phase 1: Network setup
 * Phase 2: Export + layout init (pool header, directory, queues, arenas)
 * Phase 3: FM/QEMU bootstrap exchange (UDP fallback for legacy runs)
 * Phase 4: Import peer regions + poll READY state
 * Phase 5: Queue-based rounds (DATA / ACK / COMMIT descriptors)
 * Phase 6: Report + cleanup
 */

#define _GNU_SOURCE
#include "obmm_queue_types.h"
#include "obmm_spsc_queue.h"
#include "obmm_spmc_queue.h"
#include "obmm_mpsc_queue.h"
#include "obmm_mpmc_queue.h"
#include "obmm_pool_helpers.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "[obmm_queue]"
#define MAX_NODES  OBMM_POOL_HELPERS_MAX_NODES
#define RUN_TIMEOUT_S 120
#define ARENA_PAYLOAD_SIZE 256
#define STRESS_PASSES 2
#define PUSH_TIMEOUT_MS 30000
#define SPMC_BATCH_COUNT_DEFAULT 1000
#define MPSC_BATCH_COUNT_DEFAULT 1000
#define MPMC_BATCH_COUNT_DEFAULT 500

enum queue_mode {
    QUEUE_MODE_FULLMESH = 0,
    QUEUE_MODE_SPMC     = 1,
    QUEUE_MODE_COMBINED = 2,
    QUEUE_MODE_MPMC     = 3,
};

/* ------------------------------------------------------------------ */
/* Per-node state                                                      */
/* ------------------------------------------------------------------ */

struct node_slot {
    int owner_idx;
    bool is_local;
    bool map_osync;
    uint64_t mem_id;
    uint64_t local_pa;
    uint32_t export_cna;
    struct obmm_helpers_region region;
    struct obmm_pool_header *header;
    /* resolved queue pointers (into mapped memory) */
    struct obmm_spsc_queue *ingress_queue[MAX_NODES]; /* queue[local][peer] */
    uint32_t ingress_queue_region_id[MAX_NODES];
    uint8_t *tx_arena;                                /* producer-owned slab */
    uint64_t tx_arena_size;
    uint32_t tx_arena_region_id;
};

static volatile sig_atomic_t g_alarm_fired;
static uint64_t g_export_size;
static uint32_t g_queue_depth;
static enum queue_mode g_queue_mode;
static uint32_t g_spmc_depth;
static uint32_t g_spmc_provider;
static uint32_t g_spmc_batch_count;
static uint32_t g_mpsc_consumer;
static uint32_t g_mpsc_batch_count;
static uint32_t g_mpmc_batch_count;

static void alarm_handler(int signo)
{
    (void)signo;
    g_alarm_fired = 1;
}

static uint32_t parse_queue_depth(void)
{
    const char *env = getenv("OBMM_QUEUE_DEPTH");
    char *end = NULL;
    unsigned long value;

    if (env == NULL || env[0] == '\0')
        return OBMM_QUEUE_DEFAULT_DEPTH;

    errno = 0;
    value = strtoul(env, &end, 0);
    if (errno != 0 || end == env || *end != '\0' ||
        value < OBMM_QUEUE_MIN_DEPTH ||
        value > OBMM_QUEUE_MAX_DEPTH ||
        (value & (value - 1)) != 0) {
        fprintf(stderr, TAG " warn: invalid OBMM_QUEUE_DEPTH=%s, using %u\n",
                env, OBMM_QUEUE_DEFAULT_DEPTH);
        return OBMM_QUEUE_DEFAULT_DEPTH;
    }

    return (uint32_t)value;
}

static enum queue_mode parse_queue_mode(void)
{
    const char *env = getenv("OBMM_QUEUE_MODE");
    const char *env_name = "OBMM_QUEUE_MODE";

    if (!env || env[0] == '\0') {
        env = getenv("OBMM_DEMO_MODE");
        env_name = "OBMM_DEMO_MODE";
    }
    if (!env || env[0] == '\0' || strcmp(env, "fullmesh") == 0)
        return QUEUE_MODE_FULLMESH;
    if (strcmp(env, "spmc") == 0)
        return QUEUE_MODE_SPMC;
    if (strcmp(env, "combined") == 0)
        return QUEUE_MODE_COMBINED;
    if (strcmp(env, "mpmc") == 0)
        return QUEUE_MODE_MPMC;
    fprintf(stderr, TAG " warn: unknown %s=%s, using fullmesh\n", env_name, env);
    return QUEUE_MODE_FULLMESH;
}

static uint32_t parse_spmc_depth(void)
{
    const char *env = getenv("OBMM_SPMC_DEPTH");
    char *end = NULL;
    unsigned long value;
    if (!env || env[0] == '\0')
        return OBMM_QUEUE_DEFAULT_DEPTH;
    errno = 0;
    value = strtoul(env, &end, 0);
    if (errno || end == env || *end != '\0' ||
        value < OBMM_QUEUE_MIN_DEPTH || value > OBMM_QUEUE_MAX_DEPTH ||
        (value & (value - 1)) != 0) {
        fprintf(stderr, TAG " warn: invalid OBMM_SPMC_DEPTH=%s, using %u\n",
                env, OBMM_QUEUE_DEFAULT_DEPTH);
        return OBMM_QUEUE_DEFAULT_DEPTH;
    }
    return (uint32_t)value;
}

static uint32_t parse_env_u32(const char *name, uint32_t default_val)
{
    const char *env = getenv(name);
    char *end = NULL;
    unsigned long value;
    if (!env || env[0] == '\0')
        return default_val;
    errno = 0;
    value = strtoul(env, &end, 0);
    if (errno || end == env || *end != '\0' || value > UINT32_MAX)
        return default_val;
    return (uint32_t)value;
}

static bool g_spmc_enabled(void)
{
    return g_queue_mode == QUEUE_MODE_SPMC || g_queue_mode == QUEUE_MODE_COMBINED;
}

static bool checked_add_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (UINT64_MAX - a < b)
        return false;
    *out = a + b;
    return true;
}

static bool checked_mul_u64(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a != 0 && b > UINT64_MAX / a)
        return false;
    *out = a * b;
    return true;
}

/* ------------------------------------------------------------------ */
/* Export region layout                                                */
/* ------------------------------------------------------------------ */

/*
 * Layout within each node's exported region:
 *
 *   offset 0:              pool_header (64 bytes)
 *   offset 64:             directory entries
 *   after directory:       ingress queues (one per peer)
 *   after queues:          optional SPMC stream (if enabled)
 *   after SPMC stream:     TX arena owned by this exporter
 */

static uint32_t layout_directory_count(int node_count)
{
    int peer_count = node_count - 1;
    int spmc_entries = g_spmc_enabled() ? 1 : 0;
    return (uint32_t)(peer_count + spmc_entries + 1); /* queues + spmc + tx */
}

static uint64_t layout_queues_base(int node_count)
{
    uint64_t dir_size = (uint64_t)layout_directory_count(node_count) *
                        sizeof(struct obmm_region_dirent);
    return obmm_align_up_u64(64 + dir_size, 64);
}

static uint64_t layout_queue_offset(int peer_idx, int node_count)
{
    uint64_t queues_base = layout_queues_base(node_count);
    uint64_t queue_sz = obmm_queue_region_size(g_queue_depth);
    return queues_base + (uint64_t)peer_idx * queue_sz;
}

static uint64_t layout_spmc_stream_offset(int node_count)
{
    int peer_count = node_count - 1;
    uint64_t queues_base = layout_queues_base(node_count);
    uint64_t queues_end = queues_base +
                          (uint64_t)peer_count * obmm_queue_region_size(g_queue_depth);
    return obmm_align_up_u64(queues_end, 64);
}

static uint64_t layout_tx_arena_offset(int node_count)
{
    uint64_t after_queues = layout_spmc_stream_offset(node_count);
    if (g_spmc_enabled()) {
        uint64_t spmc_sz = obmm_spmc_region_size(g_spmc_depth, node_count);
        return obmm_align_up_u64(after_queues + spmc_sz, 64);
    }
    return after_queues;
}

static uint64_t layout_tx_arena_size(int node_count)
{
    uint64_t arena_base = layout_tx_arena_offset(node_count);
    return g_export_size - arena_base;
}

static int validate_export_layout(int node_count)
{
    uint64_t peer_count = (uint64_t)(node_count - 1);
    uint64_t dir_entries, dir_size, queues_base, queue_sz, queues_size;
    uint64_t queues_end, spmc_sz, spmc_end, arena_base, arena_size;

    if (node_count < 2 || node_count > MAX_NODES)
        return -1;
    dir_entries = layout_directory_count(node_count);
    if (!checked_mul_u64(dir_entries, sizeof(struct obmm_region_dirent),
                         &dir_size) ||
        !checked_add_u64(64, dir_size, &queues_base))
        return -1;
    queues_base = obmm_align_up_u64(queues_base, 64);

    queue_sz = obmm_queue_region_size(g_queue_depth);
    if (!checked_mul_u64(peer_count, queue_sz, &queues_size) ||
        !checked_add_u64(queues_base, queues_size, &queues_end))
        return -1;
    queues_end = obmm_align_up_u64(queues_end, 64);

    if (g_spmc_enabled()) {
        spmc_sz = obmm_spmc_region_size(g_spmc_depth, node_count);
        if (!checked_add_u64(queues_end, spmc_sz, &spmc_end))
            return -1;
        arena_base = obmm_align_up_u64(spmc_end, 64);
    } else {
        arena_base = queues_end;
    }

    if (g_export_size < arena_base) {
        fprintf(stderr,
                TAG " fail: export layout too small export=%" PRIu64
                " required_before_tx_arena=%" PRIu64 "\n",
                g_export_size, arena_base);
        return -1;
    }

    arena_size = g_export_size - arena_base;
    if (arena_size < ARENA_PAYLOAD_SIZE) {
        fprintf(stderr,
                TAG " fail: export layout leaves tx_arena=%" PRIu64
                " bytes, need at least %u\n",
                arena_size, ARENA_PAYLOAD_SIZE);
        return -1;
    }

    return 0;
}

static int init_export_layout(void *base, int node_id, int node_count)
{
    struct obmm_pool_header *hdr = (struct obmm_pool_header *)base;
    struct obmm_region_dirent *dir;
    int peer_count = node_count - 1;
    int di = 0;
    int peer;
    int rc;

    /* phase 1: write header with state=INIT */
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = OBMM_POOL_MAGIC;
    hdr->layout_version = OBMM_POOL_LAYOUT_VERSION;
    hdr->node_id = (uint16_t)node_id;
    hdr->node_count = (uint16_t)node_count;
    atomic_store_explicit(&hdr->state, OBMM_POOL_STATE_INIT,
                          memory_order_relaxed);
    atomic_store_explicit(&hdr->generation, 0, memory_order_relaxed);
    hdr->region_size = g_export_size;
    hdr->directory_offset = 64;
    hdr->directory_count = layout_directory_count(node_count);
    hdr->default_queue_depth = g_queue_depth;

    /* phase 2: write directory entries */
    dir = (struct obmm_region_dirent *)((uint8_t *)base + 64);
    for (peer = 0; peer < node_count; peer++) {
        int peer_slot;
        if (peer == node_id)
            continue;
        /* map peer index to slot 0..peer_count-1 */
        peer_slot = (peer < node_id) ? peer : peer - 1;
        /* QUEUE entry */
        dir[di].region_id = (uint32_t)peer_slot;
        dir[di].kind = OBMM_REGION_QUEUE;
        dir[di].peer_node_id = (uint16_t)peer;
        dir[di].offset = layout_queue_offset(peer_slot, node_count);
        dir[di].size = obmm_queue_region_size(g_queue_depth);
        dir[di].flags = 0;
        dir[di].reserved = 0;
        di++;
    }
    /* TX_ARENA entry: producer-owned payload slab in this export. */
    dir[di].region_id = (uint32_t)peer_count;
    dir[di].kind = OBMM_REGION_TX_ARENA;
    dir[di].peer_node_id = (uint16_t)node_id;
    dir[di].offset = layout_tx_arena_offset(node_count);
    dir[di].size = layout_tx_arena_size(node_count);
    dir[di].flags = 0;
    dir[di].reserved = 0;
    di++;

    /* optional SPMC_STREAM entry (between queues and TX arena) */
    if (g_spmc_enabled()) {
        uint64_t spmc_off = layout_spmc_stream_offset(node_count);
        uint64_t spmc_sz = obmm_spmc_region_size(g_spmc_depth, node_count);
        uint64_t consumer_mask = 0;
        int p;
        for (p = 0; p < node_count; p++) {
            if (p == node_id)
                continue;
            consumer_mask |= (1ULL << p);
        }
        dir[di].region_id = (uint32_t)(peer_count + 1);
        dir[di].kind = OBMM_REGION_SPMC_STREAM;
        dir[di].peer_node_id = 0xFFFF;
        dir[di].offset = spmc_off;
        dir[di].size = spmc_sz;
        dir[di].flags = OBMM_SPMC_F_STRICT | OBMM_SPMC_F_PRODUCER_PAYLOAD;
        dir[di].reserved = 0;
        di++;

        rc = obmm_spmc_stream_init((uint8_t *)base + spmc_off,
                                    g_spmc_depth, node_count,
                                    (uint32_t)node_id, consumer_mask);
        if (rc != 0) {
            fprintf(stderr, TAG " SPMC stream init failed rc=%d\n", rc);
            return -1;
        }
    }

    /* phase 3: initialize each ingress queue */
    for (peer = 0; peer < node_count; peer++) {
        int peer_slot;
        uint64_t qoff;
        if (peer == node_id)
            continue;
        peer_slot = (peer < node_id) ? peer : peer - 1;
        qoff = layout_queue_offset(peer_slot, node_count);
        rc = obmm_spsc_queue_init((uint8_t *)base + qoff, g_queue_depth);
        if (rc != 0) {
            fprintf(stderr, TAG " queue init failed peer=%d\n", peer);
            return -1;
        }
    }

    /* phase 4: publish -- release-store generation, then state=READY */
    atomic_store_explicit(&hdr->generation, 1, memory_order_release);
    atomic_store_explicit(&hdr->state, OBMM_POOL_STATE_READY,
                          memory_order_release);

    fprintf(stderr, TAG " export layout -> ok dir_entries=%d queues=%d "
            "queue_depth=%u tx_arena=%" PRIu64 "KB\n",
            di, peer_count, g_queue_depth, layout_tx_arena_size(node_count) >> 10);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Resolve queues from imported peer region                            */
/* ------------------------------------------------------------------ */

static int resolve_peer_layout(struct node_slot *slot, int local_idx)
{
    const struct obmm_pool_header *hdr = slot->header;
    const struct obmm_region_dirent *dir;
    bool found_queue = false;
    bool found_tx_arena = false;
    uint32_t i;

    if (hdr->magic != OBMM_POOL_MAGIC) {
        fprintf(stderr, TAG " bad magic in peer region\n");
        return -1;
    }
    if (hdr->layout_version != OBMM_POOL_LAYOUT_VERSION) {
        fprintf(stderr, TAG " layout version mismatch\n");
        return -1;
    }
    if (hdr->node_id != (uint16_t)slot->owner_idx ||
        hdr->node_count < 2 || hdr->node_count > MAX_NODES ||
        hdr->region_size != g_export_size ||
        hdr->default_queue_depth != g_queue_depth) {
        fprintf(stderr, TAG " peer metadata mismatch owner=%d\n",
                slot->owner_idx + 1);
        return -1;
    }
    if (hdr->directory_offset < sizeof(*hdr) ||
        hdr->directory_offset > hdr->region_size ||
        hdr->directory_count < (uint32_t)hdr->node_count) {
        fprintf(stderr, TAG " peer directory metadata invalid owner=%d dir_count=%u node_count=%u\n",
                slot->owner_idx + 1, hdr->directory_count, hdr->node_count);
        return -1;
    }
    if ((uint64_t)hdr->directory_count >
        (hdr->region_size - hdr->directory_offset) /
        sizeof(struct obmm_region_dirent)) {
        fprintf(stderr, TAG " peer directory outside region owner=%d\n",
                slot->owner_idx + 1);
        return -1;
    }

    dir = (const struct obmm_region_dirent *)
          ((const uint8_t *)hdr + hdr->directory_offset);

    for (i = 0; i < hdr->directory_count; i++) {
        if (dir[i].offset > hdr->region_size ||
            dir[i].size > hdr->region_size - dir[i].offset) {
            fprintf(stderr, TAG " peer dirent outside region owner=%d idx=%u\n",
                    slot->owner_idx + 1, i);
            return -1;
        }
        if (dir[i].kind == OBMM_REGION_QUEUE) {
            struct obmm_spsc_queue *q;
            if (dir[i].peer_node_id != (uint16_t)local_idx)
                continue;
            if (found_queue || dir[i].size != obmm_queue_region_size(g_queue_depth)) {
                fprintf(stderr, TAG " peer queue dirent invalid owner=%d\n",
                        slot->owner_idx + 1);
                return -1;
            }
            q = (struct obmm_spsc_queue *)
                ((uint8_t *)slot->region.addr + dir[i].offset);
            if (q->size != g_queue_depth || q->mask != g_queue_depth - 1) {
                fprintf(stderr, TAG " peer queue header invalid owner=%d\n",
                        slot->owner_idx + 1);
                return -1;
            }
            slot->ingress_queue[local_idx] =
                q;
            slot->ingress_queue_region_id[local_idx] = dir[i].region_id;
            found_queue = true;
        } else if (dir[i].kind == OBMM_REGION_TX_ARENA) {
            if (dir[i].peer_node_id != (uint16_t)slot->owner_idx)
                continue;
            if (found_tx_arena || dir[i].size < ARENA_PAYLOAD_SIZE) {
                fprintf(stderr, TAG " peer tx arena dirent invalid owner=%d\n",
                        slot->owner_idx + 1);
                return -1;
            }
            slot->tx_arena = (uint8_t *)slot->region.addr + dir[i].offset;
            slot->tx_arena_size = dir[i].size;
            slot->tx_arena_region_id = dir[i].region_id;
            found_tx_arena = true;
        }
    }
    if (!found_queue || !found_tx_arena) {
        fprintf(stderr, TAG " peer layout missing queue/tx_arena owner=%d\n",
                slot->owner_idx + 1);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* UDP bootstrap exchange                                              */
/* ------------------------------------------------------------------ */

static int exchange_hello(int sockfd,
                          struct sockaddr_in peers[MAX_NODES],
                          int node_count, int local_idx,
                          const struct obmm_helpers_meta *local_meta,
                          struct obmm_helpers_meta metas[MAX_NODES],
                          bool got[MAX_NODES])
{
    struct obmm_helpers_pool_msg msg;
    long deadline = obmm_now_ms() + RUN_TIMEOUT_S * 1000L;
    int i;

    metas[local_idx] = *local_meta;
    got[local_idx] = true;

    while (!g_alarm_fired && obmm_now_ms() < deadline) {
        struct sockaddr_in from;
        struct obmm_helpers_pool_msg rx;
        bool all = true;
        for (i = 0; i < node_count; i++) {
            if (!got[i]) { all = false; break; }
        }
        if (all)
            return 0;
        for (i = 0; i < node_count; i++) {
            if (i == local_idx || got[i])
                continue;
            obmm_init_pool_msg(&msg, OBMM_MSG_HELLO, local_idx, i);
            msg.meta = *local_meta;
            (void)obmm_send_udp(sockfd, &peers[i], &msg, sizeof(msg));
        }
        while (obmm_recv_udp(sockfd, &rx, sizeof(rx), &from) ==
               (ssize_t)sizeof(rx)) {
            if (rx.magic != OBMM_POOL_HELPERS_MAGIC ||
                rx.version != OBMM_POOL_HELPERS_VERSION)
                continue;
            if (rx.type == OBMM_MSG_HELLO && rx.src_idx < node_count) {
                metas[rx.src_idx] = rx.meta;
                got[rx.src_idx] = true;
            }
        }
        usleep(100000);
    }
    fprintf(stderr, TAG " timeout waiting for HELLO\n");
    return -1;
}

static int exchange_ready(int sockfd,
                          struct sockaddr_in peers[MAX_NODES],
                          int node_count, int local_idx)
{
    bool ready[MAX_NODES] = { false };
    struct obmm_helpers_pool_msg msg;
    long deadline = obmm_now_ms() + RUN_TIMEOUT_S * 1000L;
    int i;

    ready[local_idx] = true;
    while (!g_alarm_fired && obmm_now_ms() < deadline) {
        struct sockaddr_in from;
        struct obmm_helpers_pool_msg rx;
        bool all = true;
        for (i = 0; i < node_count; i++) {
            if (!ready[i]) { all = false; break; }
        }
        if (all)
            return 0;
        for (i = 0; i < node_count; i++) {
            if (i == local_idx || ready[i])
                continue;
            obmm_init_pool_msg(&msg, OBMM_MSG_READY, local_idx, i);
            (void)obmm_send_udp(sockfd, &peers[i], &msg, sizeof(msg));
        }
        while (obmm_recv_udp(sockfd, &rx, sizeof(rx), &from) ==
               (ssize_t)sizeof(rx)) {
            if (rx.magic != OBMM_POOL_HELPERS_MAGIC ||
                rx.version != OBMM_POOL_HELPERS_VERSION)
                continue;
            if (rx.type == OBMM_MSG_READY && rx.src_idx < node_count)
                ready[rx.src_idx] = true;
        }
        usleep(100000);
    }
    fprintf(stderr, TAG " timeout waiting for READY\n");
    return -1;
}

/* ------------------------------------------------------------------ */
/* Queue-based round                                                   */
/* ------------------------------------------------------------------ */

struct round_payload {
    uint32_t magic;
    uint16_t owner_idx;
    uint16_t round_idx;
    uint64_t cookie;
};

#define ROUND_PAYLOAD_MAGIC 0x514d4d4fU /* "OMMQ" */

static uint64_t round_cookie(int owner, int round)
{
    return ((uint64_t)(owner + 1) << 32) | (uint32_t)(round + 1);
}

static void write_arena_payload(uint8_t *arena, uint64_t arena_size,
                                uint64_t payload_offset,
                                int owner_idx, int round_idx)
{
    struct round_payload p;
    uint8_t *payload;
    uint64_t fill_len;

    if (payload_offset > arena_size || arena_size - payload_offset < sizeof(p))
        return;

    payload = arena + payload_offset;
    fill_len = arena_size - payload_offset;
    if (fill_len > ARENA_PAYLOAD_SIZE)
        fill_len = ARENA_PAYLOAD_SIZE;

    p.magic = ROUND_PAYLOAD_MAGIC;
    p.owner_idx = (uint16_t)owner_idx;
    p.round_idx = (uint16_t)round_idx;
    p.cookie = round_cookie(owner_idx, round_idx);
    memcpy(payload, &p, sizeof(p));
    if (fill_len > sizeof(p))
        memset(payload + sizeof(p), 0xAB, fill_len - sizeof(p));
}

static bool verify_arena_payload(const uint8_t *arena, uint64_t arena_size,
                                 uint64_t payload_offset, uint32_t payload_len,
                                 int owner_idx, int round_idx)
{
    const struct round_payload *p;

    if (payload_len < sizeof(*p))
        return false;
    if (payload_offset > arena_size || arena_size - payload_offset < payload_len)
        return false;

    p = (const struct round_payload *)(arena + payload_offset);
    return p->magic == ROUND_PAYLOAD_MAGIC &&
           p->owner_idx == (uint16_t)owner_idx &&
           p->round_idx == (uint16_t)round_idx &&
           p->cookie == round_cookie(owner_idx, round_idx);
}

static int push_desc_wait(struct obmm_spsc_queue *q,
                          const struct obmm_desc *desc,
                          const char *what, int peer_idx)
{
    long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
    int rc;

    do {
        rc = obmm_spsc_push(q, desc);
        if (rc == 0)
            return 0;
        if (rc != -EAGAIN)
            break;
        usleep(50);
    } while (!g_alarm_fired && obmm_now_ms() < deadline);

    fprintf(stderr, TAG " push %s timeout peer=%d rc=%d\n",
            what, peer_idx + 1, rc);
    return -1;
}

static int do_rounds(int node_count, int local_idx,
                     struct node_slot slots[MAX_NODES])
{
    /* local_ingress[peer] = queue[local][peer] in local export */
    struct obmm_spsc_queue *local_ingress[MAX_NODES];
    /* remote_ingress[peer] = queue[peer][local] in peer export (to push) */
    struct obmm_spsc_queue *remote_ingress[MAX_NODES];
    uint8_t *local_tx_arena = NULL;
    uint64_t local_tx_arena_size = 0;
    uint32_t local_tx_arena_region_id = 0;
    int round_idx, i;

    memset(local_ingress, 0, sizeof(local_ingress));
    memset(remote_ingress, 0, sizeof(remote_ingress));

    /* resolve local ingress queues from our own export */
    {
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[local_idx].region.addr;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);
        uint32_t di;
        for (di = 0; di < hdr->directory_count; di++) {
            if (dir[di].kind == OBMM_REGION_QUEUE) {
                int peer = dir[di].peer_node_id;
                local_ingress[peer] =
                    (struct obmm_spsc_queue *)
                    ((uint8_t *)slots[local_idx].region.addr + dir[di].offset);
            } else if (dir[di].kind == OBMM_REGION_TX_ARENA &&
                       dir[di].peer_node_id == (uint16_t)local_idx) {
                local_tx_arena =
                    (uint8_t *)slots[local_idx].region.addr + dir[di].offset;
                local_tx_arena_size = dir[di].size;
                local_tx_arena_region_id = dir[di].region_id;
            }
        }
    }
    slots[local_idx].tx_arena = local_tx_arena;
    slots[local_idx].tx_arena_size = local_tx_arena_size;
    slots[local_idx].tx_arena_region_id = local_tx_arena_region_id;

    /* resolve remote queues from peer exports */
    for (i = 0; i < node_count; i++) {
        if (i == local_idx)
            continue;
        remote_ingress[i] = slots[i].ingress_queue[local_idx];
        if (!local_ingress[i] || !remote_ingress[i] ||
            !slots[i].tx_arena || slots[i].tx_arena_size < ARENA_PAYLOAD_SIZE) {
            fprintf(stderr, TAG " unresolved queue/tx_arena peer=%d\n", i + 1);
            return -1;
        }
    }
    if (!local_tx_arena || local_tx_arena_size < ARENA_PAYLOAD_SIZE) {
        fprintf(stderr, TAG " local tx_arena unresolved\n");
        return -1;
    }

    for (round_idx = 0; round_idx < node_count; round_idx++) {
        if (g_alarm_fired)
            return -1;

        if (local_idx == round_idx) {
            /* ---- OWNER ---- */
            write_arena_payload(local_tx_arena, local_tx_arena_size,
                                0, round_idx, round_idx);
            for (i = 0; i < node_count; i++) {
                struct obmm_desc desc;
                int rc;
                if (i == local_idx)
                    continue;
                if (!remote_ingress[i]) {
                    fprintf(stderr, TAG " peer=%d queue unresolved\n", i);
                    return -1;
                }
                /* push DATA descriptor to peer's ingress queue */
                memset(&desc, 0, sizeof(desc));
                desc.seq = (uint64_t)round_idx;
                desc.region_id = local_tx_arena_region_id;
                desc.payload_offset = 0;
                desc.type = OBMM_DESC_DATA;
                desc.cookie = (uint32_t)round_cookie(round_idx, round_idx);
                desc.payload_len = ARENA_PAYLOAD_SIZE;
                rc = push_desc_wait(remote_ingress[i], &desc, "DATA", i);
                if (rc != 0) {
                    fprintf(stderr, TAG " push DATA failed peer=%d\n", i);
                    return -1;
                }
            }
            fprintf(stderr, TAG " round=%d owner=%d DATA sent\n",
                    round_idx + 1, local_idx + 1);

            /* wait for ACK from all peers */
            {
                bool acked[MAX_NODES] = { false };
                int pending = node_count - 1;
                long deadline = obmm_now_ms() + 30000;
                while (pending > 0 && !g_alarm_fired && obmm_now_ms() < deadline) {
                    for (i = 0; i < node_count; i++) {
                        struct obmm_desc desc;
                        if (i == local_idx || acked[i])
                            continue;
                        if (obmm_spsc_pop(local_ingress[i], &desc) == 0 &&
                            desc.type == OBMM_DESC_ACK &&
                            desc.seq == (uint64_t)round_idx) {
                            acked[i] = true;
                            pending--;
                            fprintf(stderr,
                                    TAG " round=%d owner=%d ACK from node=%d\n",
                                    round_idx + 1, local_idx + 1, i + 1);
                        }
                    }
                    if (pending > 0)
                        usleep(100);
                }
                if (pending > 0) {
                    fprintf(stderr,
                            TAG " round=%d timeout waiting ACKs pending=%d\n",
                            round_idx + 1, pending);
                    return -1;
                }
            }

            /* broadcast COMMIT */
            for (i = 0; i < node_count; i++) {
                struct obmm_desc desc;
                int rc;
                if (i == local_idx)
                    continue;
                memset(&desc, 0, sizeof(desc));
                desc.seq = (uint64_t)round_idx;
                desc.type = OBMM_DESC_COMMIT;
                desc.cookie = (uint32_t)round_cookie(round_idx, round_idx);
                rc = push_desc_wait(remote_ingress[i], &desc, "COMMIT", i);
                if (rc != 0) {
                    fprintf(stderr, TAG " push COMMIT failed peer=%d\n", i);
                    return -1;
                }
            }
            fprintf(stderr, TAG " round=%d owner=%d commit -> ok\n",
                    round_idx + 1, local_idx + 1);
        } else {
            /* ---- NON-OWNER ---- */
            int owner = round_idx;
            /* wait for DATA descriptor */
            {
                long deadline = obmm_now_ms() + 30000;
                bool got_data = false;
                struct obmm_desc data_desc;
                memset(&data_desc, 0, sizeof(data_desc));
                while (!got_data && !g_alarm_fired && obmm_now_ms() < deadline) {
                    struct obmm_desc desc;
                    if (obmm_spsc_pop(local_ingress[owner], &desc) == 0 &&
                        desc.type == OBMM_DESC_DATA &&
                        desc.seq == (uint64_t)round_idx) {
                        data_desc = desc;
                        got_data = true;
                    } else {
                        usleep(100);
                    }
                }
                if (!got_data) {
                    fprintf(stderr,
                            TAG " round=%d timeout waiting DATA from owner=%d\n",
                            round_idx + 1, owner + 1);
                    return -1;
                }

                if (data_desc.region_id != slots[owner].tx_arena_region_id) {
                    fprintf(stderr,
                            TAG " round=%d region mismatch owner=%d got=%u expect=%u\n",
                            round_idx + 1, owner + 1, data_desc.region_id,
                            slots[owner].tx_arena_region_id);
                    return -1;
                }

                /* verify payload through region_id + payload_offset */
                if (!verify_arena_payload(slots[owner].tx_arena,
                                          slots[owner].tx_arena_size,
                                          data_desc.payload_offset,
                                          data_desc.payload_len,
                                          owner, round_idx)) {
                    fprintf(stderr,
                            TAG " round=%d payload verify fail owner=%d\n",
                            round_idx + 1, owner + 1);
                    return -1;
                }
            }
            fprintf(stderr, TAG " round=%d node=%d DATA verified owner=%d\n",
                    round_idx + 1, local_idx + 1, owner + 1);

            /* send ACK */
            {
                struct obmm_desc desc;
                int rc;
                memset(&desc, 0, sizeof(desc));
                desc.seq = (uint64_t)round_idx;
                desc.type = OBMM_DESC_ACK;
                desc.cookie = (uint32_t)round_cookie(round_idx, round_idx);
                rc = push_desc_wait(remote_ingress[owner], &desc, "ACK", owner);
                if (rc != 0) {
                    fprintf(stderr, TAG " push ACK failed owner=%d\n", owner);
                    return -1;
                }
            }
            fprintf(stderr, TAG " round=%d node=%d ACK -> owner=%d\n",
                    round_idx + 1, local_idx + 1, owner + 1);

            /* wait for COMMIT */
            {
                long deadline = obmm_now_ms() + 30000;
                bool got_commit = false;
                while (!got_commit && !g_alarm_fired && obmm_now_ms() < deadline) {
                    struct obmm_desc desc;
                    if (obmm_spsc_pop(local_ingress[owner], &desc) == 0 &&
                        desc.type == OBMM_DESC_COMMIT &&
                        desc.seq == (uint64_t)round_idx) {
                        got_commit = true;
                    } else {
                        usleep(100);
                    }
                }
                if (!got_commit) {
                    fprintf(stderr,
                            TAG " round=%d timeout waiting COMMIT owner=%d\n",
                            round_idx + 1, owner + 1);
                    return -1;
                }
            }
            fprintf(stderr, TAG " round=%d node=%d saw COMMIT from owner=%d\n",
                    round_idx + 1, local_idx + 1, owner + 1);
        }
    }

    fprintf(stderr, TAG " rounds -> ok count=%d\n", node_count);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Queue fill/drain stress                                             */
/* ------------------------------------------------------------------ */

static int wait_and_drain_stress_batch(struct obmm_spsc_queue *q,
                                       uint32_t depth, int owner,
                                       int local_idx, int pass)
{
    uint32_t consumed = 0;
    long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;

    while (consumed < depth && !g_alarm_fired && obmm_now_ms() < deadline) {
        struct obmm_desc desc;
        if (obmm_spsc_pop(q, &desc) == 0) {
            uint64_t expect_seq =
                ((uint64_t)pass << 48) |
                ((uint64_t)owner << 32) |
                (uint64_t)consumed;
            if (desc.type != OBMM_DESC_STRESS ||
                desc.seq != expect_seq ||
                desc.cookie != (uint32_t)(expect_seq ^ 0xa5a50000U)) {
                fprintf(stderr,
                        TAG " stress desc mismatch owner=%d local=%d pass=%d idx=%u\n",
                        owner + 1, local_idx + 1, pass + 1, consumed);
                return -1;
            }
            consumed++;
        } else {
            usleep(50);
        }
    }

    if (consumed != depth) {
        fprintf(stderr,
                TAG " stress drain timeout owner=%d local=%d pass=%d consumed=%u/%u\n",
                owner + 1, local_idx + 1, pass + 1, consumed, depth);
        return -1;
    }

    return 0;
}

static int do_queue_stress(int node_count, int local_idx,
                           struct node_slot slots[MAX_NODES])
{
    struct obmm_spsc_queue *local_ingress[MAX_NODES];
    struct obmm_spsc_queue *remote_ingress[MAX_NODES];
    int owner, peer, pass;
    uint32_t depth = g_queue_depth;

    memset(local_ingress, 0, sizeof(local_ingress));
    memset(remote_ingress, 0, sizeof(remote_ingress));

    {
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[local_idx].region.addr;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);
        uint32_t di;
        for (di = 0; di < hdr->directory_count; di++) {
            if (dir[di].kind == OBMM_REGION_QUEUE) {
                int p = dir[di].peer_node_id;
                local_ingress[p] =
                    (struct obmm_spsc_queue *)
                    ((uint8_t *)slots[local_idx].region.addr + dir[di].offset);
            }
        }
    }

    for (peer = 0; peer < node_count; peer++) {
        if (peer == local_idx)
            continue;
        remote_ingress[peer] = slots[peer].ingress_queue[local_idx];
        if (!local_ingress[peer] || !remote_ingress[peer]) {
            fprintf(stderr, TAG " stress unresolved queue peer=%d\n", peer + 1);
            return -1;
        }
    }

    for (owner = 0; owner < node_count; owner++) {
        if (g_alarm_fired)
            return -1;

        if (local_idx == owner) {
            for (pass = 0; pass < STRESS_PASSES; pass++) {
                bool acked[MAX_NODES] = { false };
                int pending = node_count - 1;
                long deadline;

                for (peer = 0; peer < node_count; peer++) {
                    uint32_t n;
                    if (peer == local_idx)
                        continue;
                    for (n = 0; n < depth; n++) {
                        struct obmm_desc desc;
                        uint64_t seq =
                            ((uint64_t)pass << 48) |
                            ((uint64_t)owner << 32) |
                            (uint64_t)n;
                        memset(&desc, 0, sizeof(desc));
                        desc.seq = seq;
                        desc.type = OBMM_DESC_STRESS;
                        desc.cookie = (uint32_t)(seq ^ 0xa5a50000U);
                        if (push_desc_wait(remote_ingress[peer], &desc,
                                           "STRESS", peer) != 0) {
                            fprintf(stderr,
                                    TAG " stress fill failed owner=%d peer=%d pass=%d idx=%u\n",
                                    owner + 1, peer + 1, pass + 1, n);
                            return -1;
                        }
                    }
                }

                deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
                while (pending > 0 && !g_alarm_fired &&
                       obmm_now_ms() < deadline) {
                    for (peer = 0; peer < node_count; peer++) {
                        struct obmm_desc desc;
                        if (peer == local_idx || acked[peer])
                            continue;
                        if (obmm_spsc_pop(local_ingress[peer], &desc) == 0 &&
                            desc.type == OBMM_DESC_STRESS_ACK &&
                            desc.seq == (uint64_t)pass) {
                            acked[peer] = true;
                            pending--;
                        }
                    }
                    if (pending > 0)
                        usleep(100);
                }
                if (pending > 0) {
                    fprintf(stderr,
                            TAG " stress ACK timeout owner=%d pass=%d pending=%d\n",
                            owner + 1, pass + 1, pending);
                    return -1;
                }
                fprintf(stderr,
                        TAG " stress owner=%d pass=%d fill/drain -> ok depth=%u\n",
                        owner + 1, pass + 1, depth);
            }
        } else {
            for (pass = 0; pass < STRESS_PASSES; pass++) {
                struct obmm_desc ack;
                if (wait_and_drain_stress_batch(local_ingress[owner], depth,
                                                owner, local_idx, pass) != 0)
                    return -1;

                memset(&ack, 0, sizeof(ack));
                ack.seq = (uint64_t)pass;
                ack.type = OBMM_DESC_STRESS_ACK;
                ack.cookie = (uint32_t)round_cookie(owner, pass);
                if (push_desc_wait(remote_ingress[owner], &ack,
                                   "STRESS_ACK", owner) != 0)
                    return -1;
            }
            fprintf(stderr,
                    TAG " stress node=%d drained owner=%d passes=%d depth=%u\n",
                    local_idx + 1, owner + 1, STRESS_PASSES, depth);
        }
    }

    fprintf(stderr, TAG " queue stress -> ok passes=%d depth=%u\n",
            STRESS_PASSES, depth);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SPMC queue protocol                                                 */
/* ------------------------------------------------------------------ */

static int do_spmc_queue(int node_count, int local_idx,
                        struct node_slot slots[MAX_NODES])
{
    uint32_t provider = g_spmc_provider;
    uint32_t batch_count = g_spmc_batch_count;
    struct obmm_spmc_stream_view view;
    struct obmm_spsc_queue *ack_queue[MAX_NODES];
    struct obmm_spsc_queue *remote_ingress[MAX_NODES];
    uint32_t node_count_u = (uint32_t)node_count;
    uint32_t local_idx_u = (uint32_t)local_idx;
    uint32_t i;

    memset(ack_queue, 0, sizeof(ack_queue));
    memset(remote_ingress, 0, sizeof(remote_ingress));

    /* resolve remote ingress queues for ACK path */
    for (i = 0; i < node_count_u; i++) {
        if (i == local_idx_u)
            continue;
        remote_ingress[i] = slots[i].ingress_queue[local_idx];
    }

    /* resolve local ingress queues for ACK reception */
    {
        struct obmm_spsc_queue *local_ingress[MAX_NODES];
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[local_idx].region.addr;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);
        memset(local_ingress, 0, sizeof(local_ingress));
        for (uint32_t di = 0; di < hdr->directory_count; di++) {
            if (dir[di].kind == OBMM_REGION_QUEUE)
                local_ingress[dir[di].peer_node_id] =
                    (struct obmm_spsc_queue *)
                    ((uint8_t *)slots[local_idx].region.addr + dir[di].offset);
        }
        for (i = 0; i < node_count_u; i++)
            ack_queue[i] = local_ingress[i];
    }

    if (local_idx == (int)provider) {
        /* ---- PROVIDER ---- */
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[local_idx].region.addr;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);
        int rc;

        rc = obmm_spmc_view_init_from_directory(&view,
                slots[local_idx].region.addr, hdr->region_size,
                dir, hdr->directory_count, provider);
        if (rc != 0) {
            fprintf(stderr, TAG " spmc provider view init failed rc=%d\n", rc);
            return -1;
        }

        for (uint32_t batch = 0; batch < batch_count; batch++) {
            struct obmm_desc desc = {0};
            desc.seq = (uint64_t)batch;
            desc.region_id = slots[local_idx].tx_arena_region_id;
            desc.payload_offset = 0;
            desc.payload_len = ARENA_PAYLOAD_SIZE;
            desc.type = OBMM_DESC_DATA;
            desc.cookie = (uint32_t)batch;

            /* write payload to TX arena */
            write_arena_payload(slots[local_idx].tx_arena,
                                slots[local_idx].tx_arena_size, 0,
                                (int)provider, (int)batch);

            /* spin until publish succeeds */
            {
                long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
                while (!g_alarm_fired && obmm_now_ms() < deadline) {
                    rc = obmm_spmc_publish(&view, &desc);
                    if (rc == 0)
                        break;
                    if (rc != -EAGAIN) {
                        fprintf(stderr, TAG " spmc publish error rc=%d\n", rc);
                        return -1;
                    }
                    usleep(50);
                }
                if (rc != 0) {
                    fprintf(stderr, TAG " spmc publish timeout batch=%u\n", batch);
                    return -1;
                }
            }
        }

        /* publish TERMINAL */
        {
            struct obmm_desc desc = {0};
            desc.seq = (uint64_t)batch_count;
            desc.type = OBMM_DESC_COMMIT;
            long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
            while (!g_alarm_fired && obmm_now_ms() < deadline) {
                rc = obmm_spmc_publish(&view, &desc);
                if (rc == 0)
                    break;
                if (rc != -EAGAIN)
                    return -1;
                usleep(50);
            }
            if (rc != 0)
                return -1;
        }
        fprintf(stderr, TAG " spmc provider=%d published=%u\n",
                provider + 1, batch_count);

        /* wait for ACK from all consumers */
        {
            bool acked[MAX_NODES] = { false };
            int pending = 0;
            for (i = 0; i < node_count_u; i++)
                if (i != local_idx_u)
                    pending++;
            long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
            while (pending > 0 && !g_alarm_fired && obmm_now_ms() < deadline) {
                for (i = 0; i < node_count_u; i++) {
                    struct obmm_desc desc;
                    if (i == local_idx_u || acked[i])
                        continue;
                    if (ack_queue[i] &&
                        obmm_spsc_pop(ack_queue[i], &desc) == 0 &&
                        desc.type == OBMM_DESC_ACK) {
                        acked[i] = true;
                        pending--;
                    }
                }
                if (pending > 0)
                    usleep(100);
            }
            if (pending > 0) {
                fprintf(stderr, TAG " spmc ACK timeout pending=%d\n", pending);
                return -1;
            }
        }
        fprintf(stderr, TAG " spmc provider -> ok\n");
    } else {
        /* ---- CONSUMER ---- */
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[provider].header;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);
        int rc;
        uint32_t consumed = 0;

        rc = obmm_spmc_view_init_from_directory(&view,
                slots[provider].region.addr, hdr->region_size,
                dir, hdr->directory_count, provider);
        if (rc != 0) {
            fprintf(stderr, TAG " spmc consumer view init failed rc=%d\n", rc);
            return -1;
        }

        {
            long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
            while (!g_alarm_fired && obmm_now_ms() < deadline) {
                struct obmm_desc desc;
                rc = obmm_spmc_consume(&view, (uint32_t)local_idx, &desc);
                if (rc == 0) {
                    if (desc.type == OBMM_DESC_COMMIT &&
                        desc.seq == (uint64_t)batch_count)
                        break;
                    consumed++;
                } else if (rc == -EAGAIN) {
                    usleep(100);
                } else {
                    fprintf(stderr, TAG " spmc consume error rc=%d\n", rc);
                    return -1;
                }
            }
        }

        fprintf(stderr, TAG " spmc consumer=%d consumed=%u -> ok\n",
                local_idx + 1, consumed);

        /* send ACK via SPSC to provider */
        if (remote_ingress[provider]) {
            struct obmm_desc desc = {0};
            desc.type = OBMM_DESC_ACK;
            desc.seq = (uint64_t)consumed;
            push_desc_wait(remote_ingress[provider], &desc, "SPMC_ACK",
                           (int)provider);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* MPSC queue protocol                                                  */
/* ------------------------------------------------------------------ */

static int do_mpsc_queue(int node_count, int local_idx,
                        struct node_slot slots[MAX_NODES])
{
    uint32_t consumer = g_mpsc_consumer;
    uint32_t batch_count = g_mpsc_batch_count;
    struct obmm_mpsc_consumer_set cset;
    struct obmm_mpsc_publisher_lane plane;
    struct obmm_spsc_queue *local_ingress[MAX_NODES];
    struct obmm_spsc_queue *remote_ingress[MAX_NODES];
    uint32_t node_count_u = (uint32_t)node_count;
    uint32_t local_idx_u = (uint32_t)local_idx;
    uint32_t i;

    memset(local_ingress, 0, sizeof(local_ingress));
    memset(remote_ingress, 0, sizeof(remote_ingress));

    /* resolve local ingress queues */
    {
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[local_idx].region.addr;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);
        for (uint32_t di = 0; di < hdr->directory_count; di++) {
            if (dir[di].kind == OBMM_REGION_QUEUE)
                local_ingress[dir[di].peer_node_id] =
                    (struct obmm_spsc_queue *)
                    ((uint8_t *)slots[local_idx].region.addr + dir[di].offset);
        }
    }

    /* resolve remote ingress queues */
    for (i = 0; i < node_count_u; i++) {
        if (i == local_idx_u)
            continue;
        remote_ingress[i] = slots[i].ingress_queue[local_idx];
    }

    if (local_idx == (int)consumer) {
        /* ---- CONSUMER ---- */
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[local_idx].region.addr;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);
        int rc;
        uint32_t consumed = 0;
        uint32_t expected_total = 0;
        bool acked[MAX_NODES] = { false };
        int pending = 0;

        rc = obmm_mpsc_consumer_set_init_from_directory(&cset,
                dir, hdr->directory_count, (uint32_t)local_idx);
        if (rc != 0) {
            fprintf(stderr, TAG " mpsc consumer set init failed rc=%d\n", rc);
            return -1;
        }

        /* fill queue pointers */
        for (i = 0; i < cset.lane_count; i++) {
            uint32_t pub = cset.lane[i].publisher_node;
            cset.lane[i].queue = local_ingress[pub];
            if (!cset.lane[i].queue) {
                fprintf(stderr, TAG " mpsc lane %u queue unresolved\n", pub);
                return -1;
            }
        }

        expected_total = batch_count * (uint32_t)(node_count - 1);
        pending = node_count - 1;

        {
            long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
            while (consumed < expected_total && !g_alarm_fired &&
                   obmm_now_ms() < deadline) {
                struct obmm_desc desc;
                uint32_t publisher;
                uint64_t rx_seq;
                rc = obmm_mpsc_poll(&cset, &desc, &publisher, &rx_seq);
                if (rc == 0) {
                    consumed++;
                } else if (rc == -EAGAIN) {
                    /* check for TERMINAL from each publisher */
                    for (uint32_t p = 0; p < node_count_u; p++) {
                        struct obmm_desc tdesc;
                        if (p == local_idx_u || acked[p])
                            continue;
                        if (obmm_spsc_pop(local_ingress[p], &tdesc) == 0 &&
                            tdesc.type == OBMM_DESC_COMMIT) {
                            acked[p] = true;
                            pending--;
                        }
                    }
                    usleep(100);
                } else {
                    fprintf(stderr, TAG " mpsc poll error rc=%d\n", rc);
                    return -1;
                }
            }
        }

        /* drain remaining terminals */
        {
            long deadline = obmm_now_ms() + 5000;
            while (pending > 0 && !g_alarm_fired && obmm_now_ms() < deadline) {
                for (uint32_t p = 0; p < node_count_u; p++) {
                    struct obmm_desc tdesc;
                    if (p == local_idx_u || acked[p])
                        continue;
                    if (obmm_spsc_pop(local_ingress[p], &tdesc) == 0 &&
                        tdesc.type == OBMM_DESC_COMMIT) {
                        acked[p] = true;
                        pending--;
                    }
                }
                if (pending > 0)
                    usleep(100);
            }
        }

        fprintf(stderr, TAG " mpsc consumer=%d consumed=%u/%u pending=%d\n",
                local_idx + 1, consumed, expected_total, pending);

        /* send ACK to each publisher */
        for (i = 0; i < node_count_u; i++) {
            if (i == local_idx_u)
                continue;
            if (remote_ingress[i]) {
                struct obmm_desc desc = {0};
                desc.type = OBMM_DESC_ACK;
                desc.seq = (uint64_t)consumed;
                push_desc_wait(remote_ingress[i], &desc, "MPSC_ACK", (int)i);
            }
        }
        fprintf(stderr, TAG " mpsc consumer -> ok\n");
    } else {
        /* ---- PUBLISHER ---- */
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[consumer].region.addr;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);
        int rc;

        rc = obmm_mpsc_publisher_lane_init_from_directory(&plane,
                dir, hdr->directory_count,
                (uint32_t)local_idx, consumer);
        if (rc != 0) {
            fprintf(stderr, TAG " mpsc publisher lane init failed rc=%d\n", rc);
            return -1;
        }

        /* publisher writes to remote ingress of consumer */
        plane.queue = remote_ingress[consumer];
        if (!plane.queue) {
            fprintf(stderr, TAG " mpsc publisher queue unresolved consumer=%u\n",
                    consumer);
            return -1;
        }

        for (uint32_t batch = 0; batch < batch_count; batch++) {
            struct obmm_desc desc = {0};
            desc.seq = ((uint64_t)local_idx << 32) | (uint64_t)batch;
            desc.type = OBMM_DESC_DATA;
            desc.cookie = (uint32_t)batch;

            long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
            while (!g_alarm_fired && obmm_now_ms() < deadline) {
                rc = obmm_mpsc_push(&plane, &desc);
                if (rc == 0)
                    break;
                if (rc != -EAGAIN) {
                    fprintf(stderr, TAG " mpsc push error rc=%d\n", rc);
                    return -1;
                }
                usleep(50);
            }
            if (rc != 0) {
                fprintf(stderr, TAG " mpsc push timeout batch=%u\n", batch);
                return -1;
            }
        }

        /* send TERMINAL */
        {
            struct obmm_desc desc = {0};
            desc.type = OBMM_DESC_COMMIT;
            desc.seq = (uint64_t)batch_count;
            long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
            while (!g_alarm_fired && obmm_now_ms() < deadline) {
                rc = obmm_mpsc_push(&plane, &desc);
                if (rc == 0)
                    break;
                if (rc != -EAGAIN)
                    return -1;
                usleep(50);
            }
            if (rc != 0) {
                fprintf(stderr, TAG " mpsc terminal push timeout\n");
                return -1;
            }
        }

        fprintf(stderr, TAG " mpsc publisher=%d published=%u -> consumer=%u\n",
                local_idx + 1, batch_count, consumer + 1);

        /* wait for ACK from consumer */
        {
            long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
            bool got_ack = false;
            while (!got_ack && !g_alarm_fired && obmm_now_ms() < deadline) {
                struct obmm_desc desc;
                if (local_ingress[consumer] &&
                    obmm_spsc_pop(local_ingress[consumer], &desc) == 0 &&
                    desc.type == OBMM_DESC_ACK) {
                    got_ack = true;
                } else {
                    usleep(100);
                }
            }
            if (!got_ack) {
                fprintf(stderr, TAG " mpsc publisher=%d ACK timeout\n",
                        local_idx + 1);
                return -1;
            }
        }
        fprintf(stderr, TAG " mpsc publisher=%d -> ok\n", local_idx + 1);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* MPMC queue protocol                                                  */
/* ------------------------------------------------------------------ */

static int do_mpmc_queue(int node_count, int local_idx,
                        struct node_slot slots[MAX_NODES])
{
    uint32_t batch_count = g_mpmc_batch_count;
    struct obmm_mpmc_bus bus;
    struct obmm_spsc_queue *local_ingress[MAX_NODES];
    struct obmm_spsc_queue *remote_ingress[MAX_NODES];
    uint32_t node_count_u = (uint32_t)node_count;
    uint32_t local_idx_u = (uint32_t)local_idx;
    uint32_t i;
    int rc;

    memset(local_ingress, 0, sizeof(local_ingress));
    memset(remote_ingress, 0, sizeof(remote_ingress));

    /* resolve local ingress queues */
    {
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[local_idx].region.addr;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);
        for (uint32_t di = 0; di < hdr->directory_count; di++) {
            if (dir[di].kind == OBMM_REGION_QUEUE)
                local_ingress[dir[di].peer_node_id] =
                    (struct obmm_spsc_queue *)
                    ((uint8_t *)slots[local_idx].region.addr + dir[di].offset);
        }
    }

    /* resolve remote ingress queues */
    for (i = 0; i < node_count_u; i++) {
        if (i == local_idx_u)
            continue;
        remote_ingress[i] = slots[i].ingress_queue[local_idx];
    }

    /* ---- Consumer init ---- */
    {
        const struct obmm_pool_header *hdr =
            (const struct obmm_pool_header *)slots[local_idx].region.addr;
        const struct obmm_region_dirent *dir =
            (const struct obmm_region_dirent *)
            ((const uint8_t *)hdr + hdr->directory_offset);

        rc = obmm_mpmc_consumer_init(&bus, dir, hdr->directory_count,
                                     (uint32_t)local_idx);
        if (rc != 0) {
            fprintf(stderr, TAG " mpmc consumer init failed rc=%d\n", rc);
            return -1;
        }

        /* fill consumer lane queue pointers */
        for (i = 0; i < bus.rx.lane_count; i++) {
            uint32_t pub = bus.rx.lane[i].publisher_node;
            bus.rx.lane[i].queue = local_ingress[pub];
            if (!bus.rx.lane[i].queue) {
                fprintf(stderr, TAG " mpmc lane %u queue unresolved\n", pub);
                return -1;
            }
        }
    }

    /* ---- Publisher init for every peer ---- */
    for (i = 0; i < node_count_u; i++) {
        const struct obmm_pool_header *phdr;
        const struct obmm_region_dirent *pdir;

        if (i == local_idx_u)
            continue;

        phdr = (const struct obmm_pool_header *)slots[i].region.addr;
        pdir = (const struct obmm_region_dirent *)
               ((const uint8_t *)phdr + phdr->directory_offset);

        rc = obmm_mpmc_publisher_init(&bus, i, pdir,
                                      phdr->directory_count,
                                      (uint32_t)local_idx);
        if (rc != 0) {
            fprintf(stderr, TAG " mpmc publisher init target=%u rc=%d\n", i, rc);
            return -1;
        }

        /* publisher lane writes to remote ingress of target */
        bus.tx[i].queue = remote_ingress[i];
        if (!bus.tx[i].queue) {
            fprintf(stderr, TAG " mpmc publisher queue unresolved target=%u\n", i);
            return -1;
        }
    }

    /* ---- Publish phase: send batch_count descriptors to each peer ---- */
    for (i = 0; i < node_count_u; i++) {
        if (i == local_idx_u)
            continue;
        for (uint32_t batch = 0; batch < batch_count; batch++) {
            struct obmm_desc desc = {0};
            desc.seq = ((uint64_t)local_idx << 32) | (uint64_t)batch;
            desc.type = OBMM_DESC_DATA;
            desc.cookie = (uint32_t)batch;

            long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
            while (!g_alarm_fired && obmm_now_ms() < deadline) {
                rc = obmm_mpmc_send(&bus, i, &desc);
                if (rc == 0)
                    break;
                if (rc != -EAGAIN) {
                    fprintf(stderr, TAG " mpmc send error target=%u rc=%d\n", i, rc);
                    return -1;
                }
                usleep(50);
            }
            if (rc != 0) {
                fprintf(stderr, TAG " mpmc send timeout target=%u batch=%u\n",
                        i, batch);
                return -1;
            }
        }

        /* send COMMIT to mark end of stream */
        {
            struct obmm_desc desc = {0};
            desc.type = OBMM_DESC_COMMIT;
            desc.seq = (uint64_t)batch_count;
            long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;
            while (!g_alarm_fired && obmm_now_ms() < deadline) {
                rc = obmm_mpmc_send(&bus, i, &desc);
                if (rc == 0)
                    break;
                if (rc != -EAGAIN)
                    return -1;
                usleep(50);
            }
            if (rc != 0) {
                fprintf(stderr, TAG " mpmc commit timeout target=%u\n", i);
                return -1;
            }
        }
    }

    fprintf(stderr, TAG " mpmc publisher=%d published=%u -> ok\n",
            local_idx + 1, batch_count);

    /* ---- Consume phase: receive from all publishers ---- */
    {
        uint32_t per_pub[MAX_NODES] = {0};
        uint32_t total_expected = batch_count * (uint32_t)(node_count - 1);
        uint32_t consumed = 0;
        bool got_commit[MAX_NODES] = { false };
        int pending = node_count - 1;
        long deadline = obmm_now_ms() + PUSH_TIMEOUT_MS;

        while (consumed < total_expected && !g_alarm_fired &&
               obmm_now_ms() < deadline) {
            struct obmm_desc desc;
            uint32_t src;

            rc = obmm_mpmc_recv(&bus, &desc, &src);
            if (rc == 0) {
                if (desc.type == OBMM_DESC_COMMIT) {
                    if (!got_commit[src]) {
                        got_commit[src] = true;
                        pending--;
                    }
                } else {
                    per_pub[src]++;
                    consumed++;
                }
            } else if (rc == -EAGAIN) {
                usleep(100);
            } else {
                fprintf(stderr, TAG " mpmc recv error rc=%d\n", rc);
                return -1;
            }
        }

        /* drain remaining commits */
        {
            long drain_deadline = obmm_now_ms() + 5000;
            while (pending > 0 && !g_alarm_fired &&
                   obmm_now_ms() < drain_deadline) {
                struct obmm_desc desc;
                uint32_t src;
                rc = obmm_mpmc_recv(&bus, &desc, &src);
                if (rc == 0 && desc.type == OBMM_DESC_COMMIT &&
                    !got_commit[src]) {
                    got_commit[src] = true;
                    pending--;
                }
                if (pending > 0)
                    usleep(100);
            }
        }

        fprintf(stderr, TAG " mpmc consumer=%d received=%u/%u",
                local_idx + 1, consumed, total_expected);
        for (i = 0; i < node_count_u; i++) {
            if (i == local_idx_u)
                continue;
            fprintf(stderr, " from=%u:%u", i + 1, per_pub[i]);
        }
        fprintf(stderr, " -> ok\n");

        if (consumed != total_expected) {
            fprintf(stderr, TAG " mpmc consumer=%d count mismatch "
                    "%u/%u\n", local_idx + 1, consumed, total_expected);
            return -1;
        }

        /* verify per-publisher counts */
        for (i = 0; i < node_count_u; i++) {
            if (i == local_idx_u)
                continue;
            if (per_pub[i] != batch_count) {
                fprintf(stderr, TAG " mpmc consumer=%d from=%u "
                        "got=%u expected=%u\n",
                        local_idx + 1, i + 1, per_pub[i], batch_count);
                return -1;
            }
        }
    }

    fprintf(stderr, TAG " mpmc -> ok\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

static void cleanup(int obmm_fd, int node_count, int local_idx,
                    struct node_slot slots[MAX_NODES])
{
    int i;
    for (i = 0; i < node_count; i++) {
        if (slots[i].region.addr || slots[i].region.fd >= 0)
            obmm_unmap_region(&slots[i].region);
    }
    for (i = 0; i < node_count; i++) {
        if (i == local_idx) {
            if (slots[i].mem_id)
                (void)obmm_do_unexport(obmm_fd, slots[i].mem_id);
        } else {
            if (slots[i].mem_id)
                (void)obmm_do_unimport(obmm_fd, slots[i].mem_id);
        }
    }
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

enum bootstrap_mode {
    BOOTSTRAP_FM,
    BOOTSTRAP_UDP,
};

static enum bootstrap_mode parse_bootstrap_mode(void)
{
    const char *env = getenv("OBMM_BOOTSTRAP");

    if (!env || env[0] == '\0' || strcmp(env, "fm") == 0)
        return BOOTSTRAP_FM;
    if (strcmp(env, "udp") == 0)
        return BOOTSTRAP_UDP;
    fprintf(stderr, TAG " unknown OBMM_BOOTSTRAP=%s, using fm\n", env);
    return BOOTSTRAP_FM;
}

static uint64_t parse_bootstrap_generation(void)
{
    const char *session = getenv("OBMM_BOOTSTRAP_SESSION");
    uint64_t hash = 1469598103934665603ULL;

    if (!session || session[0] == '\0')
        session = "default";
    while (*session) {
        hash ^= (unsigned char)*session++;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

int main(void)
{
    char ifname[IFNAMSIZ];
    unsigned int ifindex = 0;
    char local_ip[INET_ADDRSTRLEN];
    char ips[MAX_NODES][INET_ADDRSTRLEN];
    int node_count = 0;
    int local_idx = -1;
    struct sockaddr_in peers[MAX_NODES];
    struct obmm_helpers_meta metas[MAX_NODES];
    bool got_meta[MAX_NODES] = { false };
    struct node_slot slots[MAX_NODES];
    struct obmm_helpers_meta local_meta;
    struct in_addr local_addr;
    int sockfd = -1;
    int obmm_fd = -1;
    uint32_t local_cna = 0;
    uint64_t local_cna_u64 = 0;
    uint64_t bootstrap_generation = 0;
    enum bootstrap_mode bootstrap_mode;
    int i;
    int rc = 1;

    memset(slots, 0, sizeof(slots));
    memset(&local_meta, 0, sizeof(local_meta));

    signal(SIGALRM, alarm_handler);
    alarm(RUN_TIMEOUT_S);
    g_export_size = obmm_parse_export_size();
    g_queue_depth = parse_queue_depth();
    g_queue_mode = parse_queue_mode();
    g_spmc_depth = parse_spmc_depth();
    g_spmc_provider = parse_env_u32("OBMM_SPMC_PROVIDER", 0);
    g_spmc_batch_count = parse_env_u32("OBMM_SPMC_BATCH_COUNT", SPMC_BATCH_COUNT_DEFAULT);
    g_mpsc_consumer = parse_env_u32("OBMM_MPSC_CONSUMER", 0);
    g_mpsc_batch_count = parse_env_u32("OBMM_MPSC_BATCH_COUNT", MPSC_BATCH_COUNT_DEFAULT);
    g_mpmc_batch_count = parse_env_u32("OBMM_MPMC_BATCH_COUNT", MPMC_BATCH_COUNT_DEFAULT);
    bootstrap_mode = parse_bootstrap_mode();
    bootstrap_generation = parse_bootstrap_generation();

    fprintf(stderr, TAG " start export_size=%" PRIu64
            "MB queue_depth=%u mode=%s bootstrap=%s session=%" PRIx64 "\n",
            g_export_size >> 20, g_queue_depth,
            g_queue_mode == QUEUE_MODE_SPMC ? "spmc" :
            g_queue_mode == QUEUE_MODE_COMBINED ? "combined" :
            g_queue_mode == QUEUE_MODE_MPMC ? "mpmc" : "fullmesh",
            bootstrap_mode == BOOTSTRAP_FM ? "fm" : "udp",
            bootstrap_generation);

    /* ---- Phase 1: Node identity and optional UDP setup ---- */
    if (!obmm_resolve_nodes(local_ip, ips, &node_count, &local_idx)) {
        fprintf(stderr, TAG " resolve nodes failed\n");
        return 1;
    }
    fprintf(stderr, TAG " node=%d ip=%s count=%d\n",
            local_idx + 1, local_ip, node_count);
    if (validate_export_layout(node_count) != 0) {
        fprintf(stderr, TAG " export layout validation failed\n");
        return 1;
    }

    if (bootstrap_mode == BOOTSTRAP_UDP) {
        if (!obmm_wait_iface(ifname, sizeof(ifname), &ifindex)) {
            fprintf(stderr, TAG " ipourma iface not ready\n");
            return 1;
        }
        if (!obmm_get_local_ipv4(ifname, &local_addr) ||
            strcmp(inet_ntoa(local_addr), local_ip) != 0) {
            if (!obmm_set_ipv4(ifname, local_ip)) {
                fprintf(stderr, TAG " set ip failed\n");
                return 1;
            }
        }
        for (i = 0; i < node_count; i++) {
            struct in_addr peer_addr;
            if (i == local_idx)
                continue;
            memset(&peers[i], 0, sizeof(peers[i]));
            peers[i].sin_family = AF_INET;
            peers[i].sin_port = htons(OBMM_POOL_HELPERS_PORT);
            inet_pton(AF_INET, ips[i], &peers[i].sin_addr);
            peer_addr = peers[i].sin_addr;
            obmm_install_arp(ifname, &peer_addr);
        }
        sockfd = obmm_create_udp(ifname);
        if (sockfd < 0) {
            fprintf(stderr, TAG " create socket failed\n");
            return 1;
        }
    }

    /* ---- Phase 2: Export + layout init ---- */
    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        fprintf(stderr, TAG " open /dev/obmm failed: %s\n", strerror(errno));
        goto out;
    }
    if (!obmm_parse_hex_u64("/sys/bus/ub/devices/00001/primary_cna",
                            &local_cna_u64)) {
        fprintf(stderr, TAG " read primary_cna failed\n");
        goto out;
    }
    local_cna = (uint32_t)local_cna_u64;
    local_meta.export_cna = local_cna;
    if (obmm_do_export(obmm_fd, &local_meta, g_export_size) != 0) {
        fprintf(stderr, TAG " export failed\n");
        goto out;
    }
    fprintf(stderr, TAG " export -> ok mem_id=%" PRIu64 " size=%" PRIu64 "MB\n",
            local_meta.export_mem_id, g_export_size >> 20);

    slots[local_idx].owner_idx = local_idx;
    slots[local_idx].is_local = true;
    slots[local_idx].mem_id = local_meta.export_mem_id;
    slots[local_idx].export_cna = local_cna;
    if (obmm_map_region(local_meta.export_mem_id, g_export_size, false,
                        &slots[local_idx].region) != 0) {
        fprintf(stderr, TAG " map local export failed\n");
        goto out;
    }
    slots[local_idx].header =
        (struct obmm_pool_header *)slots[local_idx].region.addr;

    if (init_export_layout(slots[local_idx].region.addr,
                           local_idx, node_count) != 0) {
        fprintf(stderr, TAG " layout init failed\n");
        goto out;
    }

    /* ---- Phase 3: Bootstrap exchange for export/import tokens ---- */
    if (bootstrap_mode == BOOTSTRAP_FM) {
        if (obmm_bootstrap_publish(obmm_fd, local_idx, node_count,
                                   bootstrap_generation, &local_meta) != 0) {
            fprintf(stderr, TAG " FM bootstrap publish failed: %s\n",
                    strerror(errno));
            goto out;
        }
        if (obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                                  bootstrap_generation,
                                  metas, got_meta) != 0) {
            fprintf(stderr, TAG " FM bootstrap lookup failed: %s\n",
                    strerror(errno));
            goto out;
        }
        fprintf(stderr, TAG " bootstrap fm -> ok count=%d\n", node_count);
    } else {
        if (exchange_hello(sockfd, peers, node_count, local_idx,
                           &local_meta, metas, got_meta) != 0) {
            fprintf(stderr, TAG " HELLO exchange failed\n");
            goto out;
        }
        fprintf(stderr, TAG " bootstrap udp -> ok count=%d\n", node_count);
    }

    /* ---- Phase 4: Import peer regions ---- */
    {
        uint64_t import_pas[MAX_NODES];
        bool import_osync[MAX_NODES];
        int import_count = node_count - 1;
        int import_idx = 0;
        if (!obmm_alloc_import_pas(import_count, g_export_size,
                                   import_pas, import_osync,
                                   obmm_parse_import_cache_mode())) {
            fprintf(stderr, TAG " alloc import PA failed\n");
            goto out;
        }
        for (i = 0; i < node_count; i++) {
            uint64_t mem_id;
            if (i == local_idx)
                continue;
            slots[i].owner_idx = i;
            slots[i].is_local = false;
            slots[i].local_pa = import_pas[import_idx];
            slots[i].map_osync = import_osync[import_idx];
            slots[i].export_cna = metas[i].export_cna;
            if (obmm_do_import(obmm_fd, &metas[i], local_cna,
                               slots[i].local_pa, &mem_id) != 0) {
                fprintf(stderr, TAG " import failed peer=%d\n", i);
                goto out;
            }
            slots[i].mem_id = mem_id;
            if (obmm_map_region(mem_id, g_export_size, slots[i].map_osync,
                                &slots[i].region) != 0) {
                fprintf(stderr, TAG " map peer region failed peer=%d\n", i);
                goto out;
            }
            import_idx++;
            fprintf(stderr, TAG " import -> ok peer=%d\n", i + 1);
        }
    }

    /* poll peer READY state */
    for (i = 0; i < node_count; i++) {
        if (i == local_idx)
            continue;
        slots[i].header = (struct obmm_pool_header *)slots[i].region.addr;
        {
            long deadline = obmm_now_ms() + 30000;
            while (!g_alarm_fired && obmm_now_ms() < deadline) {
                uint32_t state = atomic_load_explicit(&slots[i].header->state,
                                                      memory_order_acquire);
                uint32_t generation =
                    atomic_load_explicit(&slots[i].header->generation,
                                         memory_order_acquire);
                if (state == OBMM_POOL_STATE_READY && generation != 0)
                    break;
                usleep(1000);
            }
            if (atomic_load_explicit(&slots[i].header->state,
                                     memory_order_acquire)
                != OBMM_POOL_STATE_READY) {
                fprintf(stderr, TAG " peer %d not READY\n", i + 1);
                goto out;
            }
            if (atomic_load_explicit(&slots[i].header->generation,
                                     memory_order_acquire) == 0) {
                fprintf(stderr, TAG " peer %d generation not published\n", i + 1);
                goto out;
            }
        }
        if (resolve_peer_layout(&slots[i], local_idx) != 0) {
            fprintf(stderr, TAG " resolve peer %d layout failed\n", i + 1);
            goto out;
        }
    }

    if (bootstrap_mode == BOOTSTRAP_UDP) {
        if (exchange_ready(sockfd, peers, node_count, local_idx) != 0) {
            fprintf(stderr, TAG " READY exchange failed\n");
            goto out;
        }
    }
    fprintf(stderr, TAG " pool ready -> ok nodes=%d\n", node_count);
    usleep(500000);

    /* ---- Phase 5: Queue-based rounds ---- */
    switch (g_queue_mode) {
    case QUEUE_MODE_FULLMESH:
        if (do_rounds(node_count, local_idx, slots) != 0)
            goto out;
        if (do_queue_stress(node_count, local_idx, slots) != 0)
            goto out;
        break;
    case QUEUE_MODE_SPMC:
        if (do_spmc_queue(node_count, local_idx, slots) != 0)
            goto out;
        break;
    case QUEUE_MODE_COMBINED:
        if (do_rounds(node_count, local_idx, slots) != 0)
            goto out;
        if (do_queue_stress(node_count, local_idx, slots) != 0)
            goto out;
        if (do_spmc_queue(node_count, local_idx, slots) != 0)
            goto out;
        if (do_mpsc_queue(node_count, local_idx, slots) != 0)
            goto out;
        if (do_mpmc_queue(node_count, local_idx, slots) != 0)
            goto out;
        break;
    case QUEUE_MODE_MPMC:
        if (do_mpmc_queue(node_count, local_idx, slots) != 0)
            goto out;
        break;
    }

    fprintf(stderr, TAG " pass\n");
    rc = 0;

out:
    cleanup(obmm_fd, node_count, local_idx, slots);
    if (obmm_fd >= 0)
        close(obmm_fd);
    if (sockfd >= 0)
        close(sockfd);
    return rc;
}
