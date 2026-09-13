/* SPDX-License-Identifier: MIT */
/* Exercise the production endpoint with local queue/MMIO and clock fixtures. */
#define clock_gettime fixture_clock_gettime
#define nanosleep fixture_nanosleep
#include "../libs/lingqu_shmem_pto/lingqu_shmem_pto_endpoint.c"
#undef clock_gettime
#undef nanosleep
#include <assert.h>

static uint64_t fixture_time = 100;
static bool clock_error;

int fixture_clock_gettime(clockid_t clock_id, struct timespec *now)
{
    assert(clock_id == CLOCK_MONOTONIC);
    if (clock_error) { errno = EIO; return -1; }
    now->tv_sec = fixture_time / 1000;
    now->tv_nsec = (fixture_time % 1000) * 1000000;
    return 0;
}

int fixture_nanosleep(const struct timespec *requested, struct timespec *remaining)
{
    (void)remaining;
    assert(requested->tv_nsec == 1000000);
    ++fixture_time;
    return 0;
}

static void *page(void)
{
    void *bytes = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(bytes != MAP_FAILED);
    return bytes;
}

static struct lingqu_shmem_pto_endpoint *fixture_endpoint(void)
{
    struct lingqu_shmem_pto_endpoint *endpoint = calloc(1, sizeof(*endpoint));
    assert(endpoint);
    endpoint->resource_fd = -1;
    endpoint->endpoint_mmio = page();
    endpoint->cmdq = page();
    endpoint->cq = page();
    endpoint->cmdq_depth = endpoint->cq_depth = 4;
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_BASE, 0x1000);
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_BASE, 0x2000);
    return endpoint;
}

static void encode_u64(uint8_t *bytes, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) bytes[i] = value >> (8 * i);
}

static void destroy_fixture(struct lingqu_shmem_pto_endpoint *endpoint)
{
    /* No device exists in this fixture. Production must retain pending pages. */
    assert(munmap(endpoint->cmdq, 4096) == 0);
    assert(munmap(endpoint->cq, 4096) == 0);
    assert(munmap((void *)endpoint->endpoint_mmio, 4096) == 0);
    free(endpoint);
}

static void check_invalid_completions(const LingquPtoDispatchSlotV2 *slot)
{
    for (unsigned fault = 0; fault < 4; ++fault) {
        struct lingqu_shmem_pto_endpoint *endpoint = fixture_endpoint();
        struct lingqu_shmem_pto_completion completion;
        assert(lingqu_shmem_pto_endpoint_submit(endpoint, slot, 1, &completion) == -ETIMEDOUT);
        encode_u64(endpoint->cq, fault == 0 ? 72 : 71);
        endpoint->cq[9] = fault == 1 ? 2 : 1;
        endpoint->cq[10] = fault == 2 ? 0 : 1;
        mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_TAIL, fault == 3 ? 2 : 1);
        assert(lingqu_shmem_pto_endpoint_wait(endpoint, 71, 2, &completion) == -EPROTO);
        assert(!completion.op_id);
        assert(mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CQ_HEAD) == 0);
        assert(lingqu_shmem_pto_endpoint_pending_op(endpoint) == 71);
        assert(lingqu_shmem_pto_endpoint_close_checked(endpoint) == -EBUSY);
        /* A later plausible slot cannot clear a protocol fault. */
        encode_u64(endpoint->cq, 71);
        endpoint->cq[9] = endpoint->cq[10] = 1;
        mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_TAIL, 1);
        assert(lingqu_shmem_pto_endpoint_wait(endpoint, 71, 2, &completion) == -EPROTO);
        destroy_fixture(endpoint);
    }
}

static void check_cancel_and_preflight(const LingquPtoDispatchSlotV2 *slot)
{
    struct lingqu_shmem_pto_endpoint *endpoint = fixture_endpoint();
    struct lingqu_shmem_pto_completion completion;
    assert(lingqu_shmem_pto_endpoint_wait(endpoint, 71, 1, &completion) == -ENOENT);
    assert(lingqu_shmem_pto_endpoint_submit(endpoint, slot, UINT64_MAX, &completion) == -EOVERFLOW);
    assert(!lingqu_shmem_pto_endpoint_pending_op(endpoint));
    assert(!mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_DOORBELL));
    clock_error = true;
    assert(lingqu_shmem_pto_endpoint_submit(endpoint, slot, 1, &completion) == -EIO);
    assert(!lingqu_shmem_pto_endpoint_pending_op(endpoint));
    assert(!mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_DOORBELL));
    clock_error = false;
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_TAIL, 1);
    assert(lingqu_shmem_pto_endpoint_submit(endpoint, slot, 1, &completion) == -EBUSY);
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_TAIL, 0);
    assert(lingqu_shmem_pto_endpoint_submit_cancel_after(endpoint, slot, 3, 1,
                                                       &completion) == -ETIMEDOUT);
    assert(mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CANCEL_OP_ID) == 71);
    assert(mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CANCEL_DOORBELL) == 1);
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CANCEL_DOORBELL, 0);
    assert(lingqu_shmem_pto_endpoint_wait(endpoint, 71, 2, &completion) == -ETIMEDOUT);
    assert(!mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CANCEL_DOORBELL));
    assert(lingqu_shmem_pto_endpoint_close_checked(endpoint) == -EBUSY);
    encode_u64(endpoint->cq, 71);
    endpoint->cq[9] = 1;
    endpoint->cq[10] = 3;
    endpoint->cq[11] = 9;
    memcpy(endpoint->cq + 12, "cancelled", 9);
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_TAIL, 1);
    assert(lingqu_shmem_pto_endpoint_wait(endpoint, 71, 2, &completion) == 0);
    assert(!lingqu_shmem_pto_completion_succeeded(&completion));
    assert(!strcmp(completion.error_code, "cancelled"));
    assert(lingqu_shmem_pto_endpoint_close_checked(endpoint) == 0);
}

int main(void)
{
    struct lingqu_shmem_pto_endpoint *endpoint = fixture_endpoint();
    struct lingqu_shmem_pto_completion completion;
    LingquPtoDispatchSlotV2 slot = {.descriptor_tag = LINGQU_PTO_DISPATCH_SLOT_TAG_V2};
    encode_u64(slot.op_id_le, 71);
    assert(lingqu_shmem_pto_endpoint_submit(endpoint, &slot, 2, &completion) == -ETIMEDOUT);
    assert(lingqu_shmem_pto_endpoint_pending_op(endpoint) == 71);
    assert(lingqu_shmem_pto_endpoint_close_checked(endpoint) == -EBUSY);
    lingqu_shmem_pto_endpoint_close(endpoint);
    assert(mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_BASE) == 0x1000);
    assert(mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CQ_BASE) == 0x2000);
    assert(lingqu_shmem_pto_endpoint_submit(endpoint, &slot, 2, &completion) == -EBUSY);
    assert(lingqu_shmem_pto_endpoint_wait(endpoint, 72, 2, &completion) == -ESTALE);
    clock_error = true;
    assert(lingqu_shmem_pto_endpoint_wait(endpoint, 71, 2, &completion) == -EIO);
    assert(lingqu_shmem_pto_endpoint_pending_op(endpoint) == 71);
    clock_error = false;
    assert(lingqu_shmem_pto_endpoint_wait(endpoint, 71, 2, &completion) == -ETIMEDOUT);
    assert(mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_TAIL) == 1);
    assert(mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CQ_HEAD) == 0);
    encode_u64(endpoint->cq, 71);
    endpoint->cq[9] = endpoint->cq[10] = 1;
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_TAIL, 1);
    assert(lingqu_shmem_pto_endpoint_wait(endpoint, 71, 2, &completion) == 0);
    assert(completion.op_id == 71 && lingqu_shmem_pto_completion_succeeded(&completion));
    assert(lingqu_shmem_pto_endpoint_pending_op(endpoint) == 0);
    assert(mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CQ_HEAD) == 1);
    assert(lingqu_shmem_pto_endpoint_close_checked(endpoint) == 0);
    check_invalid_completions(&slot);
    check_cancel_and_preflight(&slot);
    puts("lingqu_shmem_pto_endpoint_recovery=pass");
    return 0;
}
