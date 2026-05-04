/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM SPSC queue unit tests.
 *
 * Compile and run on host (no OBMM dependency):
 *   gcc -O2 -Wall -Wextra -I. -o obmm_queue_test obmm_queue_test.c -lpthread
 *   ./obmm_queue_test
 */

#include "obmm_queue.h"

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
