/* SPDX-License-Identifier: MIT */
/* Production queue logic with local MMIO pages; no guest or model is started. */
#include "../libs/lingqu_shmem_pto/lingqu_shmem_pto_endpoint.c"
#include <assert.h>
#include <pthread.h>

static void encode_u64(uint8_t *bytes, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) bytes[i] = value >> (8 * i);
}

static void *page(void)
{
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    return p;
}

static struct lingqu_shmem_pto_endpoint *create(unsigned cmd_depth, unsigned cq_depth)
{
    struct lingqu_shmem_pto_endpoint *ep = calloc(1, sizeof(*ep));
    assert(ep);
    ep->resource_fd = -1;
    ep->endpoint_mmio = page(); ep->cmdq = page(); ep->cq = page();
    ep->cmdq_depth = cmd_depth; ep->cq_depth = cq_depth;
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CMDQ_BASE, 0x1000);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_BASE, 0x2000);
    return ep;
}

static void discard_fixture(struct lingqu_shmem_pto_endpoint *ep)
{
    /* Fixture pages have no hardware owner. Production cannot discard faults. */
    assert(!munmap(ep->cmdq, 4096) && !munmap(ep->cq, 4096));
    assert(!munmap((void *)ep->endpoint_mmio, 4096));
    free(ep);
}

static int enqueue(struct lingqu_shmem_pto_endpoint *ep, uint64_t id)
{
    LingquPtoDispatchSlotV2 slot = {.descriptor_tag = LINGQU_PTO_DISPATCH_SLOT_TAG_V2};
    encode_u64(slot.op_id_le, id);
    return lingqu_shmem_pto_endpoint_enqueue(ep, &slot);
}

static void completion(struct lingqu_shmem_pto_endpoint *ep, unsigned index, uint64_t id)
{
    uint8_t *slot = ep->cq + index * 64;
    memset(slot, 0, 64);
    encode_u64(slot, id);
    slot[9] = slot[10] = 1;
}

static void check_completion(void)
{
    struct lingqu_shmem_pto_endpoint *ep = create(4, 4);
    struct lingqu_shmem_pto_completion done;
    assert(enqueue(ep, 71) == 0 && enqueue(ep, 72) == 0);
    assert(mmio_read64(ep->endpoint_mmio, LINGQU_REG_DOORBELL) == 2);
    assert(enqueue(ep, 73) == 0 && enqueue(ep, 74) == -ENOSPC);
    assert(lingqu_shmem_pto_endpoint_queued_count(ep) == 3);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 71, &done) == -EAGAIN && !done.op_id);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 99, &done) == -ENOENT && !done.op_id);
    assert(lingqu_shmem_pto_endpoint_close_checked(ep) == -EBUSY);
    assert(mmio_read64(ep->endpoint_mmio, LINGQU_REG_CMDQ_BASE) == 0x1000);
    assert(mmio_read64(ep->endpoint_mmio, LINGQU_REG_CQ_BASE) == 0x2000);
    completion(ep, 0, 73); completion(ep, 1, 71); completion(ep, 2, 72);
    ep->cq[10] = 3; ep->cq[11] = 6; memcpy(ep->cq + 12, "denied", 6);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CMDQ_HEAD, 3);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL, 3);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 72, &done) == 0 && done.op_id == 72);
    assert(lingqu_shmem_pto_endpoint_queued_count(ep) == 2);
    assert(enqueue(ep, 74) == 0); /* Rejected capacity attempt did not consume ID. */
    assert(mmio_read64(ep->endpoint_mmio, LINGQU_REG_CMDQ_TAIL) == 0);
    completion(ep, 3, 74);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CMDQ_HEAD, 0);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL, 0);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 74, &done) == 0 && done.op_id == 74);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 73, &done) == 0 && done.op_id == 73);
    assert(done.status == 3 && !strcmp(done.error_code, "denied"));
    assert(lingqu_shmem_pto_endpoint_poll(ep, 71, &done) == 0 && done.op_id == 71);
    assert(lingqu_shmem_pto_endpoint_queued_count(ep) == 0);
    assert(enqueue(ep, 74) == -ESTALE && enqueue(ep, 70) == -ESTALE);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 71, &done) == -ENOENT);
    assert(lingqu_shmem_pto_endpoint_close_checked(ep) == 0);
}

static void check_capacity(void)
{
    const unsigned depths[] = {2, 4, 64};
    for (unsigned c = 0; c < 3; ++c) for (unsigned q = 0; q < 3; ++q) {
        unsigned limit = (depths[c] < depths[q] ? depths[c] : depths[q]) - 1;
        struct lingqu_shmem_pto_endpoint *ep = create(depths[c], depths[q]);
        for (unsigned i = 1; i <= limit; ++i) assert(enqueue(ep, i) == 0);
        uint64_t tail = mmio_read64(ep->endpoint_mmio, LINGQU_REG_CMDQ_TAIL);
        assert(enqueue(ep, limit + 1) == -ENOSPC);
        assert(ep->queued_count == limit && ep->queued_last_op_id == limit);
        assert(mmio_read64(ep->endpoint_mmio, LINGQU_REG_CMDQ_TAIL) == tail);
        assert(lingqu_shmem_pto_endpoint_close_checked(ep) == -EBUSY);
        discard_fixture(ep);
    }
}

static void check_queue_base_mismatch(void)
{
    struct lingqu_shmem_pto_endpoint *ep = create(4, 4);

    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CMDQ_BASE,
                 UINT64_C(0x1bf856000));
    *(volatile uint32_t *)(ep->endpoint_mmio +
                           LINGQU_REG_CMDQ_BASE_HI) = 1;
    assert(mmio_read_queue_base(ep->endpoint_mmio,
                                LINGQU_REG_CMDQ_BASE,
                                LINGQU_REG_CMDQ_BASE_HI) ==
           UINT64_C(0x1bf856000));
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CMDQ_BASE, 0x1000);
    *(volatile uint32_t *)(ep->endpoint_mmio +
                           LINGQU_REG_CMDQ_BASE_HI) = 0;
    ep->cmdq_phys = 0x3000;
    ep->cq_phys = 0x2000;
    assert(enqueue(ep, 71) == -ESTALE);
    assert(ep->protocol_error);
    assert(ep->queued_count == 0);
    assert(mmio_read64(ep->endpoint_mmio, LINGQU_REG_CMDQ_TAIL) == 0);
    discard_fixture(ep);
}

static void check_protocol(void)
{
    for (unsigned fault = 0; fault < 8; ++fault) {
        struct lingqu_shmem_pto_endpoint *ep = create(4, 4);
        struct lingqu_shmem_pto_completion done;
        if (fault != 7) assert(enqueue(ep, 71) == 0 && enqueue(ep, 72) == 0);
        completion(ep, 0, fault == 0 ? 99 : 71);
        if (fault == 1) ep->cq[9] = 2;
        if (fault == 2) ep->cq[10] = 0;
        if (fault == 3) { ep->cq[10] = 3; ep->cq[11] = 46; }
        if (fault == 4) completion(ep, 1, 71);
        mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL,
                     fault == 5 ? 4 : fault == 4 ? 2 : 1);
        if (fault == 6) mmio_write64(ep->endpoint_mmio, LINGQU_REG_CMDQ_HEAD, 4);
        int rc = fault >= 6 ? enqueue(ep, 73) : lingqu_shmem_pto_endpoint_poll(ep, 71, &done);
        assert(rc == -EPROTO && ep->protocol_error);
        assert(ep->queued_count == (fault == 7 ? 0 : 2));
        assert(mmio_read64(ep->endpoint_mmio, LINGQU_REG_CQ_HEAD) == (fault == 4 ? 1 : 0));
        assert(lingqu_shmem_pto_endpoint_close_checked(ep) == -EBUSY);
        assert(enqueue(ep, 74) == -EPROTO);
        assert(lingqu_shmem_pto_endpoint_poll(ep, 72, &done) == -EPROTO && !done.op_id);
        discard_fixture(ep);
    }
}

static void check_legacy(void)
{
    struct lingqu_shmem_pto_endpoint *ep = create(4, 4);
    struct lingqu_shmem_pto_completion done;
    LingquPtoDispatchSlotV2 slot = {.descriptor_tag = LINGQU_PTO_DISPATCH_SLOT_TAG_V2};
    encode_u64(slot.op_id_le, 72);
    assert(enqueue(ep, 71) == 0);
    assert(lingqu_shmem_pto_endpoint_submit(ep, &slot, 1, &done) == -EBUSY);
    completion(ep, 0, 71);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CMDQ_HEAD, 1);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL, 1);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 71, &done) == 0);
    assert(lingqu_shmem_pto_endpoint_submit(ep, &slot, 1, &done) == -ETIMEDOUT);
    assert(enqueue(ep, 73) == -EBUSY);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 71, &done) == -EBUSY);
    completion(ep, 1, 72);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CMDQ_HEAD, 2);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL, 2);
    assert(lingqu_shmem_pto_endpoint_wait(ep, 72, 1, &done) == 0 && done.op_id == 72);
    assert(enqueue(ep, 72) == -ESTALE);
    assert(lingqu_shmem_pto_endpoint_close_checked(ep) == 0);
}

static void check_replay(void)
{
    struct lingqu_shmem_pto_endpoint *ep = create(4, 4);
    struct lingqu_shmem_pto_completion done;
    assert(enqueue(ep, 71) == 0 && enqueue(ep, 72) == 0);
    completion(ep, 0, 71);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL, 1);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 71, &done) == 0 && done.op_id == 71);
    /* A late response cannot claim the slot of the remaining owned request. */
    completion(ep, 1, 71);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL, 2);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 72, &done) == -EPROTO && !done.op_id);
    assert(ep->queued_count == 1 && queued_request(ep, 72));
    assert(mmio_read64(ep->endpoint_mmio, LINGQU_REG_CQ_HEAD) == 1);
    assert(lingqu_shmem_pto_endpoint_close_checked(ep) == -EBUSY);
    discard_fixture(ep);
}

static void check_arguments(void)
{
    struct lingqu_shmem_pto_endpoint *ep = create(4, 4);
    struct lingqu_shmem_pto_completion done;
    LingquPtoDispatchSlotV2 slot = {0};
    assert(lingqu_shmem_pto_endpoint_enqueue(NULL, &slot) == -EINVAL);
    assert(lingqu_shmem_pto_endpoint_enqueue(ep, NULL) == -EINVAL);
    encode_u64(slot.op_id_le, 1);
    assert(lingqu_shmem_pto_endpoint_enqueue(ep, &slot) == -EINVAL);
    assert(enqueue(ep, 0) == -EINVAL);
    assert(lingqu_shmem_pto_endpoint_poll(NULL, 1, &done) == -EINVAL);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 0, &done) == -EINVAL);
    assert(lingqu_shmem_pto_endpoint_poll(ep, 1, NULL) == -EINVAL);
    assert(!lingqu_shmem_pto_endpoint_queued_count(NULL));
    const unsigned invalid_depths[] = {0, 1, 65, UINT32_MAX};
    for (unsigned i = 0; i < 4; ++i) {
        ep->cmdq_depth = invalid_depths[i];
        assert(enqueue(ep, 1) == -EINVAL);
        ep->cmdq_depth = 4;
        ep->cq_depth = invalid_depths[i];
        assert(enqueue(ep, 1) == -EINVAL);
        ep->cq_depth = 4;
    }
    assert(ep->queued_count == 0 && ep->queued_last_op_id == 0 && !ep->protocol_error);
    assert(enqueue(ep, UINT64_MAX) == 0);
    completion(ep, 0, UINT64_MAX);
    mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL, 1);
    assert(lingqu_shmem_pto_endpoint_poll(ep, UINT64_MAX, &done) == 0);
    assert(done.op_id == UINT64_MAX && enqueue(ep, 1) == -ESTALE);
    assert(lingqu_shmem_pto_endpoint_close_checked(ep) == 0);
}

struct caller_batch {
    struct lingqu_shmem_pto_endpoint *ep;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint64_t first_id;
    unsigned submitted;
    bool allow_poll;
};

struct caller_request {
    struct caller_batch *batch;
    unsigned index;
    struct lingqu_shmem_pto_completion done;
};

static void *queued_caller(void *opaque)
{
    struct caller_request *request = opaque;
    struct caller_batch *batch = request->batch;
    assert(!pthread_mutex_lock(&batch->lock));
    /* Only short endpoint calls are serialized; cond_wait releases the lock. */
    while (batch->submitted != request->index)
        assert(!pthread_cond_wait(&batch->changed, &batch->lock));
    uint64_t id = batch->first_id + request->index;
    assert(enqueue(batch->ep, id) == 0);
    ++batch->submitted;
    assert(!pthread_cond_broadcast(&batch->changed));
    while (!batch->allow_poll)
        assert(!pthread_cond_wait(&batch->changed, &batch->lock));
    assert(lingqu_shmem_pto_endpoint_poll(batch->ep, id, &request->done) == 0);
    assert(request->done.op_id == id && request->done.status == 1);
    assert(!pthread_mutex_unlock(&batch->lock));
    return NULL;
}

static void check_threads(void)
{
    struct lingqu_shmem_pto_endpoint *ep = create(4, 4);
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        struct caller_batch batch = {.ep = ep, .first_id = 1 + cycle * 2};
        struct caller_request requests[2] = {{.batch = &batch}, {.batch = &batch, .index = 1}};
        pthread_t threads[2];
        assert(!pthread_mutex_init(&batch.lock, NULL));
        assert(!pthread_cond_init(&batch.changed, NULL));
        for (unsigned i = 0; i < 2; ++i)
            assert(!pthread_create(&threads[i], NULL, queued_caller, &requests[i]));
        assert(!pthread_mutex_lock(&batch.lock));
        while (batch.submitted != 2)
            assert(!pthread_cond_wait(&batch.changed, &batch.lock));
        assert(lingqu_shmem_pto_endpoint_queued_count(ep) == 2);
        assert(lingqu_shmem_pto_endpoint_close_checked(ep) == -EBUSY);
        assert(mmio_read64(ep->endpoint_mmio, LINGQU_REG_DOORBELL) == 2);
        /* Local device fixture produces the original completions out of order. */
        unsigned tail = mmio_read64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL);
        completion(ep, tail, batch.first_id + 1);
        completion(ep, (tail + 1) % 4, batch.first_id);
        mmio_write64(ep->endpoint_mmio, LINGQU_REG_CMDQ_HEAD,
                     mmio_read64(ep->endpoint_mmio, LINGQU_REG_CMDQ_TAIL));
        mmio_write64(ep->endpoint_mmio, LINGQU_REG_CQ_TAIL, (tail + 2) % 4);
        batch.allow_poll = true;
        assert(!pthread_cond_broadcast(&batch.changed));
        assert(!pthread_mutex_unlock(&batch.lock));
        for (unsigned i = 0; i < 2; ++i) assert(!pthread_join(threads[i], NULL));
        assert(!lingqu_shmem_pto_endpoint_queued_count(ep));
        assert(!pthread_cond_destroy(&batch.changed));
        assert(!pthread_mutex_destroy(&batch.lock));
    }
    assert(lingqu_shmem_pto_endpoint_close_checked(ep) == 0);
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--case")) return 2;
    if (!strcmp(argv[2], "completion")) check_completion();
    else if (!strcmp(argv[2], "capacity")) check_capacity();
    else if (!strcmp(argv[2], "protocol")) check_protocol();
    else if (!strcmp(argv[2], "legacy")) check_legacy();
    else if (!strcmp(argv[2], "replay")) check_replay();
    else if (!strcmp(argv[2], "arguments")) check_arguments();
    else if (!strcmp(argv[2], "threads")) check_threads();
    else if (!strcmp(argv[2], "queue-base-mismatch")) check_queue_base_mismatch();
    else return 2;
    printf("lingqu_shmem_pto_endpoint_queue=pass case=%s\n", argv[2]);
    return 0;
}
