/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM SPSC queue unit tests.
 *
 * Compile and run on host (no OBMM dependency):
 *   gcc -O2 -Wall -Wextra -I. -o obmm_queue_test obmm_queue_test.c -lpthread
 *   ./obmm_queue_test
 */

#include "obmm_queue.h"
#include "obmm_spmc_queue.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "[obmm_queue_test]"

static int g_fail_count;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, TAG " FAIL: %s: %s (line %d)\n",           \
                    __func__, msg, __LINE__);                           \
            g_fail_count++;                                             \
            return -1;                                                  \
        }                                                               \
    } while (0)

/* ------------------------------------------------------------------ */
/* test_basic_push_pop                                                 */
/* ------------------------------------------------------------------ */

static int test_basic_push_pop(void)
{
    uint32_t depth = 64;
    uint64_t sz = obmm_queue_region_size(depth);
    void *mem = malloc(sz);
    struct obmm_spsc_queue *q;
    struct obmm_desc in, out;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spsc_queue_init(mem, depth);
    CHECK(rc == 0, "init");
    q = (struct obmm_spsc_queue *)mem;

    memset(&in, 0, sizeof(in));
    in.seq = 42;
    in.region_id = 7;
    in.payload_len = 128;
    in.payload_offset = 0x1000;
    in.type = OBMM_DESC_DATA;
    in.cookie = 0xDEADBEEF;

    rc = obmm_spsc_push(q, &in);
    CHECK(rc == 0, "push");

    rc = obmm_spsc_pop(q, &out);
    CHECK(rc == 0, "pop");

    CHECK(out.seq == in.seq, "seq");
    CHECK(out.region_id == in.region_id, "region_id");
    CHECK(out.payload_len == in.payload_len, "payload_len");
    CHECK(out.payload_offset == in.payload_offset, "payload_offset");
    CHECK(out.type == in.type, "type");
    CHECK(out.cookie == in.cookie, "cookie");

    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* test_fifo_order                                                     */
/* ------------------------------------------------------------------ */

static int test_fifo_order(void)
{
    uint32_t depth = 64;
    uint64_t sz = obmm_queue_region_size(depth);
    void *mem = malloc(sz);
    struct obmm_spsc_queue *q;
    struct obmm_desc in, out;
    uint32_t i;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spsc_queue_init(mem, depth);
    CHECK(rc == 0, "init");
    q = (struct obmm_spsc_queue *)mem;

    for (i = 0; i < 10; i++) {
        memset(&in, 0, sizeof(in));
        in.seq = (uint64_t)i;
        in.cookie = i * 100;
        rc = obmm_spsc_push(q, &in);
        CHECK(rc == 0, "push");
    }

    for (i = 0; i < 10; i++) {
        rc = obmm_spsc_pop(q, &out);
        CHECK(rc == 0, "pop");
        CHECK(out.seq == (uint64_t)i, "seq order");
        CHECK(out.cookie == i * 100, "cookie order");
    }

    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* test_full_queue                                                     */
/* ------------------------------------------------------------------ */

static int test_full_queue(void)
{
    uint32_t depth = 64;
    uint64_t sz = obmm_queue_region_size(depth);
    void *mem = malloc(sz);
    struct obmm_spsc_queue *q;
    struct obmm_desc in, out;
    uint32_t i;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spsc_queue_init(mem, depth);
    CHECK(rc == 0, "init");
    q = (struct obmm_spsc_queue *)mem;

    memset(&in, 0, sizeof(in));
    for (i = 0; i < depth; i++) {
        in.seq = (uint64_t)i;
        rc = obmm_spsc_push(q, &in);
        CHECK(rc == 0, "push should succeed");
    }

    rc = obmm_spsc_push(q, &in);
    CHECK(rc == -EAGAIN, "push full should return -EAGAIN");

    rc = obmm_spsc_pop(q, &out);
    CHECK(rc == 0, "pop one");
    CHECK(out.seq == 0, "first popped seq");

    rc = obmm_spsc_push(q, &in);
    CHECK(rc == 0, "push after pop should succeed");

    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* test_empty_queue                                                    */
/* ------------------------------------------------------------------ */

static int test_empty_queue(void)
{
    uint32_t depth = 64;
    uint64_t sz = obmm_queue_region_size(depth);
    void *mem = malloc(sz);
    struct obmm_spsc_queue *q;
    struct obmm_desc out;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spsc_queue_init(mem, depth);
    CHECK(rc == 0, "init");
    q = (struct obmm_spsc_queue *)mem;

    rc = obmm_spsc_pop(q, &out);
    CHECK(rc == -EAGAIN, "pop empty should return -EAGAIN");

    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* test_wrap_around                                                    */
/* ------------------------------------------------------------------ */

static int test_wrap_around(void)
{
    uint32_t depth = 64;
    uint64_t sz = obmm_queue_region_size(depth);
    void *mem = malloc(sz);
    struct obmm_spsc_queue *q;
    struct obmm_desc in, out;
    uint32_t i;
    uint32_t total = depth + depth / 2;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spsc_queue_init(mem, depth);
    CHECK(rc == 0, "init");
    q = (struct obmm_spsc_queue *)mem;

    for (i = 0; i < total; i++) {
        memset(&in, 0, sizeof(in));
        in.seq = (uint64_t)i;

        rc = obmm_spsc_push(q, &in);
        CHECK(rc == 0, "push");

        rc = obmm_spsc_pop(q, &out);
        CHECK(rc == 0, "pop");
        CHECK(out.seq == (uint64_t)i, "seq mismatch");
    }

    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* test_stress                                                         */
/* ------------------------------------------------------------------ */

static int test_stress(void)
{
    uint32_t depth = 1024;
    uint64_t sz = obmm_queue_region_size(depth);
    void *mem = malloc(sz);
    struct obmm_spsc_queue *q;
    struct obmm_desc in, out;
    uint32_t push_count = 0;
    uint32_t pop_count = 0;
    uint32_t total_ops = 100000;
    uint32_t i;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spsc_queue_init(mem, depth);
    CHECK(rc == 0, "init");
    q = (struct obmm_spsc_queue *)mem;

    for (i = 0; i < total_ops; i++) {
        int do_push = (push_count < total_ops / 2) &&
                      ((pop_count >= push_count) || (rand() % 2 == 0));

        if (do_push) {
            memset(&in, 0, sizeof(in));
            in.seq = (uint64_t)push_count;
            in.cookie = push_count * 7;
            rc = obmm_spsc_push(q, &in);
            if (rc == 0)
                push_count++;
        } else {
            rc = obmm_spsc_pop(q, &out);
            if (rc == 0) {
                CHECK(out.seq == (uint64_t)pop_count, "stress seq");
                CHECK(out.cookie == pop_count * 7, "stress cookie");
                pop_count++;
            }
        }
    }

    while (pop_count < push_count) {
        rc = obmm_spsc_pop(q, &out);
        CHECK(rc == 0, "drain pop");
        CHECK(out.seq == (uint64_t)pop_count, "drain seq");
        CHECK(out.cookie == pop_count * 7, "drain cookie");
        pop_count++;
    }

    CHECK(push_count == pop_count, "push/pop balance");

    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* test_concurrent (pthread)                                           */
/* ------------------------------------------------------------------ */

struct concurrent_ctx {
    struct obmm_spsc_queue *q;
    uint32_t count;
    uint32_t *seen;
    uint32_t errors;
};

static void *concurrent_producer(void *arg)
{
    struct concurrent_ctx *ctx = arg;
    struct obmm_desc in;
    uint32_t i;

    memset(&in, 0, sizeof(in));
    for (i = 0; i < ctx->count; i++) {
        in.seq = (uint64_t)i;
        in.cookie = i;
        while (obmm_spsc_push(ctx->q, &in) == -EAGAIN)
            ; /* spin */
    }
    return NULL;
}

static void *concurrent_consumer(void *arg)
{
    struct concurrent_ctx *ctx = arg;
    struct obmm_desc out;
    uint32_t i;

    for (i = 0; i < ctx->count; i++) {
        while (obmm_spsc_pop(ctx->q, &out) == -EAGAIN)
            ; /* spin */
        if (out.seq != (uint64_t)i || out.cookie != i)
            ctx->errors++;
        if (ctx->seen)
            ctx->seen[i] = 1;
    }
    return NULL;
}

static int test_concurrent(void)
{
    uint32_t depth = 1024;
    uint32_t count = 10000;
    uint64_t sz = obmm_queue_region_size(depth);
    void *mem = malloc(sz);
    struct obmm_spsc_queue *q;
    struct concurrent_ctx prod_ctx, cons_ctx;
    pthread_t prod_th, cons_th;
    uint32_t i;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spsc_queue_init(mem, depth);
    CHECK(rc == 0, "init");
    q = (struct obmm_spsc_queue *)mem;

    prod_ctx.q = q;
    prod_ctx.count = count;
    prod_ctx.seen = NULL;
    prod_ctx.errors = 0;

    cons_ctx.q = q;
    cons_ctx.count = count;
    cons_ctx.seen = calloc(count, sizeof(uint32_t));
    cons_ctx.errors = 0;

    CHECK(cons_ctx.seen != NULL, "calloc");

    rc = pthread_create(&prod_th, NULL, concurrent_producer, &prod_ctx);
    CHECK(rc == 0, "pthread_create producer");
    rc = pthread_create(&cons_th, NULL, concurrent_consumer, &cons_ctx);
    CHECK(rc == 0, "pthread_create consumer");

    pthread_join(prod_th, NULL);
    pthread_join(cons_th, NULL);

    CHECK(cons_ctx.errors == 0, "no consumer errors");
    for (i = 0; i < count; i++)
        CHECK(cons_ctx.seen[i] == 1, "all items consumed");

    free(cons_ctx.seen);
    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* test_init_invalid                                                   */
/* ------------------------------------------------------------------ */

static int test_init_invalid(void)
{
    uint32_t depth = 64;
    uint64_t sz = obmm_queue_region_size(depth);
    void *mem = malloc(sz);
    int rc;

    CHECK(mem != NULL, "malloc");

    rc = obmm_spsc_queue_init(mem, 0);
    CHECK(rc == -EINVAL, "depth=0 rejected");

    rc = obmm_spsc_queue_init(mem, 63);
    CHECK(rc == -EINVAL, "non-power-of-two rejected");

    rc = obmm_spsc_queue_init(mem, 32);
    CHECK(rc == -EINVAL, "below min depth rejected");

    rc = obmm_spsc_queue_init(mem, 131072);
    CHECK(rc == -EINVAL, "above max depth rejected");

    rc = obmm_spsc_queue_init(mem, 64);
    CHECK(rc == 0, "valid depth accepted");

    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SPMC layout tests                                                   */
/* ------------------------------------------------------------------ */

static int test_spmc_layout(void)
{
    CHECK(sizeof(struct obmm_spmc_stream) == 128,
          "obmm_spmc_stream must be 128 bytes");
    CHECK(sizeof(struct obmm_spmc_consumer_cursor) == 64,
          "obmm_spmc_consumer_cursor must be 64 bytes");

    uint32_t depth = 64;
    uint32_t max_cons = 8;
    uint64_t sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t cursor_off = obmm_align_up_u64(sizeof(struct obmm_spmc_stream), 64);
    uint64_t desc_off = obmm_align_up_u64(
        cursor_off + (uint64_t)max_cons * sizeof(struct obmm_spmc_consumer_cursor), 64);
    uint64_t expected = desc_off + (uint64_t)depth * sizeof(struct obmm_desc);

    CHECK(sz == expected, "region size matches helper");
    CHECK((desc_off & 63) == 0, "desc_offset 64-byte aligned");

    return 0;
}

/* ------------------------------------------------------------------ */
/* SPMC init tests                                                     */
/* ------------------------------------------------------------------ */

static int test_spmc_init_valid(void)
{
    uint32_t depth = 64;
    uint32_t max_cons = 8;
    uint64_t sz = obmm_spmc_region_size(depth, max_cons);
    void *mem = malloc(sz);
    struct obmm_spmc_stream *s;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spmc_stream_init(mem, depth, max_cons, 0, 0xFE);
    CHECK(rc == 0, "init");

    s = (struct obmm_spmc_stream *)mem;
    CHECK(s->magic == OBMM_SPMC_MAGIC, "magic");
    CHECK(s->version == OBMM_SPMC_VERSION, "version");
    CHECK(s->flags == OBMM_SPMC_F_STRICT, "flags strict");
    CHECK(s->depth == depth, "depth");
    CHECK(s->mask == depth - 1, "mask");
    CHECK(s->max_consumers == max_cons, "max_consumers");
    CHECK(s->provider_node == 0, "provider_node");
    CHECK(s->generation == 1, "generation");
    CHECK(atomic_load_explicit(&s->tail, memory_order_relaxed) == 0, "tail");
    CHECK(atomic_load_explicit(&s->active_consumer_mask, memory_order_relaxed) == 0xFE,
          "active_consumer_mask");

    /* Check active cursors */
    for (uint32_t nid = 1; nid <= 7; ++nid) {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, nid);
        CHECK(c->node_id == nid, "cursor node_id");
        CHECK(atomic_load_explicit(&c->state, memory_order_acquire) ==
              OBMM_SPMC_CONSUMER_ACTIVE, "cursor active");
        CHECK(atomic_load_explicit(&c->generation_seen, memory_order_relaxed) == 1,
              "cursor generation");
    }

    /* Check cursor 0 is detached (not in mask) */
    {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, 0);
        CHECK(atomic_load_explicit(&c->state, memory_order_acquire) ==
              OBMM_SPMC_CONSUMER_DETACHED, "cursor 0 detached");
    }

    free(mem);
    return 0;
}

static int test_spmc_init_invalid(void)
{
    uint32_t depth = 64;
    uint32_t max_cons = 8;
    uint64_t sz = obmm_spmc_region_size(depth, max_cons);
    void *mem = malloc(sz);
    int rc;

    CHECK(mem != NULL, "malloc");

    rc = obmm_spmc_stream_init(mem, 0, max_cons, 0, 0);
    CHECK(rc == -EINVAL, "depth=0");

    rc = obmm_spmc_stream_init(mem, 63, max_cons, 0, 0);
    CHECK(rc == -EINVAL, "non-power-of-two");

    rc = obmm_spmc_stream_init(mem, 32, max_cons, 0, 0);
    CHECK(rc == -EINVAL, "below min");

    rc = obmm_spmc_stream_init(mem, 131072, max_cons, 0, 0);
    CHECK(rc == -EINVAL, "above max");

    rc = obmm_spmc_stream_init(mem, depth, 0, 0, 0);
    CHECK(rc == -EINVAL, "max_consumers=0");

    rc = obmm_spmc_stream_init(mem, depth, 65, 0, 0);
    CHECK(rc == -EINVAL, "max_consumers=65");

    rc = obmm_spmc_stream_init(mem, depth, max_cons, 0, 1ULL << max_cons);
    CHECK(rc == -EINVAL, "consumer_mask bit >= max_consumers");

    rc = obmm_spmc_stream_init(mem, depth, max_cons, 0, 0);
    CHECK(rc == 0, "valid no consumers");

    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SPMC view init tests                                                */
/* ------------------------------------------------------------------ */

/*
 * Build a minimal pool: header + directory with one SPMC_STREAM entry
 * and one TX_ARENA entry.  The stream is initialized at the correct
 * offset within the pool.
 */
static void build_spmc_pool(void *pool, uint64_t pool_sz,
                             uint32_t depth, uint32_t max_cons,
                             uint32_t provider_node,
                             uint64_t consumer_mask,
                             struct obmm_region_dirent *dir_out,
                             uint32_t *dir_count_out)
{
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t dir_off = 64;
    uint32_t dc = 2;

    struct obmm_region_dirent *dir = (struct obmm_region_dirent *)
        ((uint8_t *)pool + dir_off);

    dir[0].region_id = 0;
    dir[0].kind = OBMM_REGION_SPMC_STREAM;
    dir[0].peer_node_id = 0xFFFF;
    dir[0].offset = obmm_align_up_u64(dir_off + dc * sizeof(struct obmm_region_dirent), 64);
    dir[0].size = stream_sz;
    dir[0].flags = 0;
    dir[0].reserved = 0;

    dir[1].region_id = 1;
    dir[1].kind = OBMM_REGION_TX_ARENA;
    dir[1].peer_node_id = (uint16_t)provider_node;
    dir[1].offset = dir[0].offset + stream_sz;
    dir[1].size = pool_sz - dir[1].offset;
    dir[1].flags = 0;
    dir[1].reserved = 0;

    obmm_spmc_stream_init((uint8_t *)pool + dir[0].offset,
                          depth, max_cons, provider_node, consumer_mask);

    memcpy(dir_out, dir, dc * sizeof(struct obmm_region_dirent));
    *dir_count_out = dc;
}

static int test_spmc_view_init(void)
{
    uint32_t depth = 64, max_cons = 8;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x06, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");
    CHECK(v.stream != NULL, "stream set");
    CHECK(v.stream->magic == OBMM_SPMC_MAGIC, "stream magic");

    free(pool);
    return 0;
}

static int test_spmc_view_init_missing(void)
{
    struct obmm_region_dirent dir[1] = {
        { 0, OBMM_REGION_TX_ARENA, 0, 0, 4096, 0, 0 }
    };
    struct obmm_spmc_stream_view v;
    int rc = obmm_spmc_view_init_from_directory(&v, (void *)0x1000, 0x100000,
                                                 dir, 1, 0);
    CHECK(rc == -ENOENT, "no SPMC stream entry");
    return 0;
}

static int test_spmc_view_init_duplicate(void)
{
    struct obmm_region_dirent dir[2] = {
        { 0, OBMM_REGION_SPMC_STREAM, 0xFFFF, 0, 4096, 0, 0 },
        { 1, OBMM_REGION_SPMC_STREAM, 0xFFFF, 4096, 4096, 0, 0 }
    };
    struct obmm_spmc_stream_view v;
    int rc = obmm_spmc_view_init_from_directory(&v, (void *)0x1000, 0x100000,
                                                 dir, 2, 0);
    CHECK(rc == -EEXIST, "duplicate SPMC entries");
    return 0;
}

/* ------------------------------------------------------------------ */
/* SPMC provider payload addr tests                                    */
/* ------------------------------------------------------------------ */

static int test_spmc_payload_addr_tx_arena(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    struct obmm_desc desc;
    const void *addr;
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x02, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    memset(&desc, 0, sizeof(desc));
    desc.region_id = 1; /* TX_ARENA */
    desc.payload_offset = 100;
    desc.payload_len = 200;
    addr = NULL;
    rc = obmm_spmc_provider_payload_addr(&v, &desc, &addr);
    CHECK(rc == 1, "TX arena returns 1");
    CHECK(addr != NULL, "payload addr set");
    CHECK((uint8_t *)addr == v.pool_base + dir[1].offset + 100, "addr correct");

    free(pool);
    return 0;
}

static int test_spmc_payload_addr_no_payload(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    struct obmm_desc desc;
    const void *addr;
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x02, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    memset(&desc, 0, sizeof(desc));
    desc.region_id = 1;
    desc.payload_len = 0;
    rc = obmm_spmc_provider_payload_addr(&v, &desc, &addr);
    CHECK(rc == 0, "no payload returns 0");

    free(pool);
    return 0;
}

static int test_spmc_payload_addr_oob(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    struct obmm_desc desc;
    const void *addr;
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x02, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    memset(&desc, 0, sizeof(desc));
    desc.region_id = 1;
    desc.payload_offset = dir[1].size - 10;
    desc.payload_len = 100; /* overflows dirent */
    rc = obmm_spmc_provider_payload_addr(&v, &desc, &addr);
    CHECK(rc == -EINVAL, "OOB payload");

    free(pool);
    return 0;
}

static int test_spmc_payload_addr_missing(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    struct obmm_desc desc;
    const void *addr;
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x02, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    memset(&desc, 0, sizeof(desc));
    desc.region_id = 99; /* not in directory */
    desc.payload_len = 10;
    rc = obmm_spmc_provider_payload_addr(&v, &desc, &addr);
    CHECK(rc == -EINVAL, "missing region");

    free(pool);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SPMC publish/consume tests                                          */
/* ------------------------------------------------------------------ */

static int test_spmc_publish_consume(void)
{
    uint32_t depth = 64, max_cons = 8;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x06, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    /* Publish 10 descriptors */
    for (uint64_t i = 0; i < 10; i++) {
        struct obmm_desc desc = {0};
        desc.seq = i;
        desc.cookie = (uint32_t)(i * 11);
        rc = obmm_spmc_publish(&v, &desc);
        CHECK(rc == 0, "publish");
    }

    /* Consumer 1 consumes all */
    for (uint64_t i = 0; i < 10; i++) {
        struct obmm_desc out = {0};
        rc = obmm_spmc_consume(&v, 1, &out);
        CHECK(rc == 0, "consume");
        CHECK(out.seq == i, "consumer1 seq");
        CHECK(out.cookie == (uint32_t)(i * 11), "consumer1 cookie");
    }

    /* Consumer 2 consumes all */
    for (uint64_t i = 0; i < 10; i++) {
        struct obmm_desc out = {0};
        rc = obmm_spmc_consume(&v, 2, &out);
        CHECK(rc == 0, "consume");
        CHECK(out.seq == i, "consumer2 seq");
    }

    free(pool);
    return 0;
}

static int test_spmc_publish_full(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    struct obmm_desc desc = {0};
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x02, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    /* Fill to depth */
    for (uint32_t i = 0; i < depth; i++) {
        desc.seq = i;
        rc = obmm_spmc_publish(&v, &desc);
        CHECK(rc == 0, "publish fill");
    }

    rc = obmm_spmc_publish(&v, &desc);
    CHECK(rc == -EAGAIN, "full returns -EAGAIN");

    /* Consumer 1 consumes one */
    {
        struct obmm_desc out;
        rc = obmm_spmc_consume(&v, 1, &out);
        CHECK(rc == 0, "consume one");
        CHECK(out.seq == 0, "first consumed");
    }

    /* Now publish should succeed */
    desc.seq = depth;
    rc = obmm_spmc_publish(&v, &desc);
    CHECK(rc == 0, "publish after consume");

    free(pool);
    return 0;
}

static int test_spmc_consume_empty(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    struct obmm_desc out;
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x02, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    rc = obmm_spmc_consume(&v, 1, &out);
    CHECK(rc == -EAGAIN, "empty returns -EAGAIN");

    free(pool);
    return 0;
}

static int test_spmc_consume_overrun(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    void *mem = malloc(stream_sz);
    struct obmm_spmc_stream *s;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spmc_stream_init(mem, depth, max_cons, 0, 0x02);
    CHECK(rc == 0, "init");
    s = (struct obmm_spmc_stream *)mem;

    /* Simulate overrun by directly advancing tail past head + depth */
    atomic_store_explicit(&s->tail, (uint64_t)depth + 5, memory_order_release);

    /* Consumer should detect overrun */
    {
        struct obmm_spmc_stream_view v = {
            .pool_base = (uint8_t *)mem,
            .pool_size = stream_sz,
            .dir = NULL,
            .dir_count = 0,
            .provider_node = 0,
            .stream = s,
        };
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(s, 1);
        struct obmm_desc out;
        rc = obmm_spmc_consume(&v, 1, &out);
        CHECK(rc == -EOVERFLOW, "overrun returns -EOVERFLOW");
        CHECK(atomic_load_explicit(&c->drop_count, memory_order_relaxed) == 1,
              "drop_count incremented");
        CHECK(atomic_load_explicit(&c->state, memory_order_acquire) ==
              OBMM_SPMC_CONSUMER_PAUSED, "cursor paused");
    }

    free(mem);
    return 0;
}

static int test_spmc_wraparound(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    int rc;
    uint32_t total = depth + depth / 2;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x02, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    for (uint32_t i = 0; i < total; i++) {
        struct obmm_desc desc = {0};
        desc.seq = (uint64_t)i;
        rc = obmm_spmc_publish(&v, &desc);
        CHECK(rc == 0, "publish");

        struct obmm_desc out;
        rc = obmm_spmc_consume(&v, 1, &out);
        CHECK(rc == 0, "consume");
        CHECK(out.seq == (uint64_t)i, "seq mismatch");
    }

    free(pool);
    return 0;
}

static int test_spmc_publish_inactive(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x02, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    /* Pause consumer 1 */
    {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(v.stream, 1);
        atomic_store_explicit(&c->state, OBMM_SPMC_CONSUMER_PAUSED,
                              memory_order_release);
    }

    {
        struct obmm_desc desc = {0};
        rc = obmm_spmc_publish(&v, &desc);
        CHECK(rc == -EPIPE, "publish with paused consumer returns -EPIPE");
    }

    free(pool);
    return 0;
}

static int test_spmc_publish_no_active(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    void *mem = malloc(stream_sz);
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spmc_stream_init(mem, depth, max_cons, 0, 0);
    CHECK(rc == 0, "init with empty mask");

    struct obmm_spmc_stream_view v = {
        .pool_base = (uint8_t *)mem,
        .pool_size = stream_sz,
        .dir = NULL,
        .dir_count = 0,
        .provider_node = 0,
        .stream = (struct obmm_spmc_stream *)mem,
    };
    struct obmm_desc desc = {0};
    rc = obmm_spmc_publish(&v, &desc);
    CHECK(rc == -ENODEV, "publish with no active consumers");

    free(mem);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SPMC reclaim tests                                                  */
/* ------------------------------------------------------------------ */

static int test_spmc_reclaimable_head(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    struct obmm_desc desc = {0};
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x06, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    for (int i = 0; i < 5; i++) {
        desc.seq = (uint64_t)i;
        obmm_spmc_publish(&v, &desc);
    }

    /* Consumer 1 consumes 3, consumer 2 consumes 2 */
    for (int i = 0; i < 3; i++) {
        struct obmm_desc out;
        obmm_spmc_consume(&v, 1, &out);
    }
    for (int i = 0; i < 2; i++) {
        struct obmm_desc out;
        obmm_spmc_consume(&v, 2, &out);
    }

    uint64_t rh = obmm_spmc_reclaimable_head(&v);
    CHECK(rh == 2, "reclaimable head is min(3,2) = 2");

    free(pool);
    return 0;
}

static int test_spmc_reclaimable_skips_paused(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    struct obmm_desc desc = {0};
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x06, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    for (int i = 0; i < 5; i++) {
        desc.seq = (uint64_t)i;
        obmm_spmc_publish(&v, &desc);
    }

    /* Consumer 1 consumes 3 */
    for (int i = 0; i < 3; i++) {
        struct obmm_desc out;
        obmm_spmc_consume(&v, 1, &out);
    }

    /* Pause consumer 2 */
    {
        struct obmm_spmc_consumer_cursor *c = obmm_spmc_cursor(v.stream, 2);
        atomic_store_explicit(&c->state, OBMM_SPMC_CONSUMER_PAUSED,
                              memory_order_release);
    }

    uint64_t rh = obmm_spmc_reclaimable_head(&v);
    CHECK(rh == 3, "reclaimable head skips paused consumer, uses active head=3");

    free(pool);
    return 0;
}

static int test_spmc_reset(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    void *mem = malloc(stream_sz);
    struct obmm_spmc_stream *s;
    int rc;

    CHECK(mem != NULL, "malloc");
    rc = obmm_spmc_stream_init(mem, depth, max_cons, 0, 0x06);
    CHECK(rc == 0, "init");
    s = (struct obmm_spmc_stream *)mem;

    /* Publish some */
    {
        struct obmm_spmc_stream_view v = {
            .pool_base = (uint8_t *)mem,
            .pool_size = stream_sz,
            .dir = NULL,
            .dir_count = 0,
            .provider_node = 0,
            .stream = s,
        };
        struct obmm_desc desc = {0};
        desc.seq = 1;
        obmm_spmc_publish(&v, &desc);
        desc.seq = 2;
        obmm_spmc_publish(&v, &desc);
    }

    rc = obmm_spmc_stream_reset(s, 0x06);
    CHECK(rc == 0, "reset");

    CHECK(atomic_load_explicit(&s->tail, memory_order_relaxed) == 0, "tail reset");
    CHECK(s->generation == 2, "generation incremented");

    /* Verify cursors reset */
    {
        struct obmm_spmc_consumer_cursor *c1 = obmm_spmc_cursor(s, 1);
        struct obmm_spmc_consumer_cursor *c2 = obmm_spmc_cursor(s, 2);
        CHECK(atomic_load_explicit(&c1->head, memory_order_relaxed) == 0, "c1 head");
        CHECK(atomic_load_explicit(&c1->state, memory_order_acquire) ==
              OBMM_SPMC_CONSUMER_ACTIVE, "c1 active");
        CHECK(atomic_load_explicit(&c1->generation_seen, memory_order_relaxed) == 2,
              "c1 generation");
        CHECK(atomic_load_explicit(&c2->head, memory_order_relaxed) == 0, "c2 head");
    }

    free(mem);
    return 0;
}

static int test_spmc_reclaim_payloads(void)
{
    uint32_t depth = 64, max_cons = 4;
    uint64_t stream_sz = obmm_spmc_region_size(depth, max_cons);
    uint64_t pool_sz = 64 + 2 * sizeof(struct obmm_region_dirent) + stream_sz + 4096;
    void *pool = malloc(pool_sz);
    struct obmm_region_dirent dir[2];
    uint32_t dc;
    struct obmm_spmc_stream_view v;
    int rc;

    CHECK(pool != NULL, "malloc");
    memset(pool, 0, pool_sz);
    build_spmc_pool(pool, pool_sz, depth, max_cons, 0, 0x02, dir, &dc);

    rc = obmm_spmc_view_init_from_directory(&v, pool, pool_sz, dir, dc, 0);
    CHECK(rc == 0, "view init");

    /* Publish 3 descriptors with TX arena payloads */
    for (uint64_t i = 0; i < 3; i++) {
        struct obmm_desc desc = {0};
        desc.region_id = 1;
        desc.payload_offset = i * 100;
        desc.payload_len = 80;
        desc.seq = i;
        obmm_spmc_publish(&v, &desc);
    }

    /* Consume all */
    for (uint64_t i = 0; i < 3; i++) {
        struct obmm_desc out;
        obmm_spmc_consume(&v, 1, &out);
    }

    struct obmm_spmc_tx_reclaim_state st = {0, 0};
    rc = obmm_spmc_reclaim_payloads(&v, &st);
    CHECK(rc == 0, "reclaim");
    CHECK(st.desc_reclaimed_to == 3, "reclaimed all 3");
    CHECK(st.tx_reclaim_offset == 200 + 80, "tx offset is max(payload_end)");

    free(pool);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "basic_push_pop",    test_basic_push_pop },
        { "fifo_order",        test_fifo_order },
        { "full_queue",        test_full_queue },
        { "empty_queue",       test_empty_queue },
        { "wrap_around",       test_wrap_around },
        { "stress",            test_stress },
        { "concurrent",        test_concurrent },
        { "init_invalid",      test_init_invalid },
        { "spmc_layout",       test_spmc_layout },
        { "spmc_init_valid",   test_spmc_init_valid },
        { "spmc_init_invalid", test_spmc_init_invalid },
        { "spmc_view_init",    test_spmc_view_init },
        { "spmc_view_missing", test_spmc_view_init_missing },
        { "spmc_view_dup",     test_spmc_view_init_duplicate },
        { "spmc_payload_tx",   test_spmc_payload_addr_tx_arena },
        { "spmc_payload_none", test_spmc_payload_addr_no_payload },
        { "spmc_payload_oob",  test_spmc_payload_addr_oob },
        { "spmc_payload_miss", test_spmc_payload_addr_missing },
        { "spmc_pub_consume",  test_spmc_publish_consume },
        { "spmc_full",         test_spmc_publish_full },
        { "spmc_empty",        test_spmc_consume_empty },
        { "spmc_overrun",      test_spmc_consume_overrun },
        { "spmc_wraparound",   test_spmc_wraparound },
        { "spmc_inactive",     test_spmc_publish_inactive },
        { "spmc_no_active",    test_spmc_publish_no_active },
        { "spmc_reclaim_head", test_spmc_reclaimable_head },
        { "spmc_reclaim_skip", test_spmc_reclaimable_skips_paused },
        { "spmc_reset",        test_spmc_reset },
        { "spmc_reclaim_pay",  test_spmc_reclaim_payloads },
    };
    int pass_count = 0;
    int fail_count = 0;
    int i;

    printf(TAG " running %d tests\n", (int)(sizeof(tests) / sizeof(tests[0])));

    for (i = 0; i < (int)(sizeof(tests) / sizeof(tests[0])); i++) {
        int rc = tests[i].fn();
        if (rc == 0) {
            printf(TAG " PASS: %s\n", tests[i].name);
            pass_count++;
        } else {
            fail_count++;
        }
    }

    if (g_fail_count > 0)
        fail_count += g_fail_count;

    printf(TAG " results: %d passed, %d failed\n", pass_count, fail_count);

    if (fail_count > 0) {
        printf(TAG " FAIL\n");
        return 1;
    }

    printf(TAG " PASS\n");
    return 0;
}
