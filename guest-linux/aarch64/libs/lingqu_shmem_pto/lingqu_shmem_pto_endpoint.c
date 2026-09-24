/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "lingqu_shmem_pto_endpoint.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define LINGQU_SHMEM_PTO_PAGE_BYTES 4096u
#define LINGQU_SHMEM_PTO_ENDPOINT_OFFSET 0x1000u
#define LINGQU_SHMEM_PTO_SLOT_BYTES 64u
#define LINGQU_SHMEM_PTO_QUEUED_LIMIT 63u

#define LINGQU_REG_VERSION 0x000u
#define LINGQU_REG_CMDQ_BASE 0x010u
#define LINGQU_REG_CMDQ_SIZE 0x020u
#define LINGQU_REG_CMDQ_HEAD 0x028u
#define LINGQU_REG_CMDQ_TAIL 0x030u
#define LINGQU_REG_CQ_BASE 0x038u
#define LINGQU_REG_CQ_SIZE 0x048u
#define LINGQU_REG_CQ_HEAD 0x050u
#define LINGQU_REG_CQ_TAIL 0x058u
#define LINGQU_REG_DOORBELL 0x068u
#define LINGQU_REG_IRQ_STATUS 0x078u
#define LINGQU_REG_IRQ_ACK 0x080u
#define LINGQU_REG_DEFAULT_SEGMENT 0x088u
#define LINGQU_REG_CANCEL_OP_ID 0x0a0u
#define LINGQU_REG_CANCEL_DOORBELL 0x0a8u

struct lingqu_shmem_pto_queued_request {
    uint64_t op_id;
    bool ready;
    struct lingqu_shmem_pto_completion completion;
};

struct lingqu_shmem_pto_endpoint {
    int resource_fd;
    volatile uint8_t *root_mmio;
    volatile uint8_t *endpoint_mmio;
    uint8_t *cmdq;
    uint8_t *cq;
    uint64_t cmdq_phys;
    uint64_t cq_phys;
    uint32_t cmdq_depth;
    uint32_t cq_depth;
    uint64_t pending_op_id;
    uint64_t cancel_at;
    uint32_t pending_cq_head;
    bool cancel_requested;
    bool protocol_error;
    uint32_t queued_count;
    uint64_t queued_last_op_id;
    struct lingqu_shmem_pto_queued_request queued[LINGQU_SHMEM_PTO_QUEUED_LIMIT];
};

static const char *const lingqu_resource_candidates[] = {
    "/sys/bus/ub/devices/00001/resource0_wc",
    "/sys/bus/ub/devices/00001/resource1_wc",
    "/sys/bus/ub/devices/00001/resource0",
    "/sys/bus/ub/devices/00001/resource1",
    "/sys/bus/ub/devices/00001/resource2",
};

static uint64_t load_u64_le(const uint8_t bytes[8])
{
    uint64_t value = 0;
    size_t index;

    for (index = 0; index < sizeof(value); index++) {
        value |= (uint64_t)bytes[index] << (index * 8);
    }
    return value;
}

static uint64_t mmio_read64(volatile uint8_t *base, uint64_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)(base + offset);
    uint64_t low = reg[0];
    uint64_t high = reg[1];

    return low | (high << 32);
}

static void mmio_write64(volatile uint8_t *base,
                         uint64_t offset,
                         uint64_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)(base + offset);

    reg[0] = (uint32_t)value;
    reg[1] = (uint32_t)(value >> 32);
}

static int monotonic_ms(uint64_t *value)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -EIO;
    }
    *value = (uint64_t)now.tv_sec * 1000u +
             (uint64_t)now.tv_nsec / 1000000u;
    return 0;
}

static int phys_for_virt(void *address, uint64_t *physical_out)
{
    uint64_t virtual_address = (uint64_t)(uintptr_t)address;
    uint64_t page_index = virtual_address / LINGQU_SHMEM_PTO_PAGE_BYTES;
    uint64_t page_offset = virtual_address % LINGQU_SHMEM_PTO_PAGE_BYTES;
    uint64_t entry = 0;
    int fd;
    ssize_t bytes;

    if (!address || !physical_out) {
        return -EINVAL;
    }
    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        return -errno;
    }
    bytes = pread(fd, &entry, sizeof(entry),
                  (off_t)(page_index * sizeof(entry)));
    close(fd);
    if (bytes != (ssize_t)sizeof(entry) ||
        (entry & (UINT64_C(1) << 63)) == 0) {
        return -EFAULT;
    }
    entry &= (UINT64_C(1) << 55) - 1;
    if (entry == 0) {
        return -EFAULT;
    }
    *physical_out = entry * LINGQU_SHMEM_PTO_PAGE_BYTES + page_offset;
    return 0;
}

static int map_resource(struct lingqu_shmem_pto_endpoint *endpoint,
                        const char *path,
                        struct lingqu_shmem_pto_endpoint_info *info)
{
    void *root;
    void *device;
    int fd;

    fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        return -errno;
    }
    root = mmap(NULL, LINGQU_SHMEM_PTO_PAGE_BYTES,
                PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (root == MAP_FAILED) {
        int saved_errno = errno;

        close(fd);
        return -saved_errno;
    }
    device = mmap(NULL, LINGQU_SHMEM_PTO_PAGE_BYTES,
                  PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  LINGQU_SHMEM_PTO_ENDPOINT_OFFSET);
    if (device == MAP_FAILED) {
        int saved_errno = errno;

        munmap(root, LINGQU_SHMEM_PTO_PAGE_BYTES);
        close(fd);
        return -saved_errno;
    }
    info->root_version = mmio_read64(root, LINGQU_REG_VERSION);
    info->default_segment = mmio_read64(
        device, LINGQU_REG_DEFAULT_SEGMENT);
    if (info->root_version == 0 || info->default_segment == 0) {
        munmap(device, LINGQU_SHMEM_PTO_PAGE_BYTES);
        munmap(root, LINGQU_SHMEM_PTO_PAGE_BYTES);
        close(fd);
        return -ENODEV;
    }
    endpoint->resource_fd = fd;
    endpoint->root_mmio = root;
    endpoint->endpoint_mmio = device;
    snprintf(info->resource_path, sizeof(info->resource_path), "%s", path);
    return 0;
}

static int map_queues(struct lingqu_shmem_pto_endpoint *endpoint)
{
    uint64_t cmdq_head;
    uint64_t cmdq_tail;
    uint64_t cq_head;
    uint64_t cq_tail;
    int rc;

    endpoint->cmdq_depth = (uint32_t)mmio_read64(
        endpoint->endpoint_mmio, LINGQU_REG_CMDQ_SIZE);
    endpoint->cq_depth = (uint32_t)mmio_read64(
        endpoint->endpoint_mmio, LINGQU_REG_CQ_SIZE);
    if (endpoint->cmdq_depth < 2 || endpoint->cq_depth < 2 ||
        endpoint->cmdq_depth >
            LINGQU_SHMEM_PTO_PAGE_BYTES / LINGQU_SHMEM_PTO_SLOT_BYTES ||
        endpoint->cq_depth >
            LINGQU_SHMEM_PTO_PAGE_BYTES / LINGQU_SHMEM_PTO_SLOT_BYTES) {
        return -EPROTO;
    }
    cmdq_head = mmio_read64(endpoint->endpoint_mmio,
                            LINGQU_REG_CMDQ_HEAD) % endpoint->cmdq_depth;
    cmdq_tail = mmio_read64(endpoint->endpoint_mmio,
                            LINGQU_REG_CMDQ_TAIL) % endpoint->cmdq_depth;
    cq_head = mmio_read64(endpoint->endpoint_mmio,
                          LINGQU_REG_CQ_HEAD) % endpoint->cq_depth;
    cq_tail = mmio_read64(endpoint->endpoint_mmio,
                          LINGQU_REG_CQ_TAIL) % endpoint->cq_depth;
    if (cmdq_head != cmdq_tail || cq_head != cq_tail) {
        return -EBUSY;
    }
    endpoint->cmdq = mmap(NULL, LINGQU_SHMEM_PTO_PAGE_BYTES,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    endpoint->cq = mmap(NULL, LINGQU_SHMEM_PTO_PAGE_BYTES,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (endpoint->cmdq == MAP_FAILED || endpoint->cq == MAP_FAILED) {
        return -errno;
    }
    memset(endpoint->cmdq, 0, LINGQU_SHMEM_PTO_PAGE_BYTES);
    memset(endpoint->cq, 0, LINGQU_SHMEM_PTO_PAGE_BYTES);
    if (mlock(endpoint->cmdq, LINGQU_SHMEM_PTO_PAGE_BYTES) != 0 ||
        mlock(endpoint->cq, LINGQU_SHMEM_PTO_PAGE_BYTES) != 0) {
        return -errno;
    }
    rc = phys_for_virt(endpoint->cmdq, &endpoint->cmdq_phys);
    if (rc != 0) {
        return rc;
    }
    rc = phys_for_virt(endpoint->cq, &endpoint->cq_phys);
    if (rc != 0) {
        return rc;
    }
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_BASE,
                 endpoint->cmdq_phys);
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_BASE,
                 endpoint->cq_phys);
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_HEAD, cq_tail);
    return 0;
}

int lingqu_shmem_pto_completion_decode(
    const uint8_t slot[LINGQU_SHMEM_PTO_COMPLETION_BYTES],
    struct lingqu_shmem_pto_completion *completion)
{
    size_t finished_offset = 11;
    uint8_t code_length = 0;

    if (!slot || !completion) {
        return -EINVAL;
    }
    memset(completion, 0, sizeof(*completion));
    completion->op_id = load_u64_le(slot);
    completion->task_marker = slot[8];
    completion->source = slot[9];
    completion->status = slot[10];
    if (completion->op_id == 0 || completion->source == 0 ||
        completion->source > 7 || completion->status == 0 ||
        completion->status > 3) {
        return -EPROTO;
    }
    if (completion->status != 1) {
        code_length = slot[11];
        if (code_length == 0 ||
            code_length >= sizeof(completion->error_code) ||
            12u + code_length + sizeof(uint64_t) >
                LINGQU_SHMEM_PTO_COMPLETION_BYTES) {
            return -EPROTO;
        }
        memcpy(completion->error_code, slot + 12, code_length);
        completion->error_code[code_length] = '\0';
        finished_offset = 12u + code_length;
    }
    completion->finished_at = load_u64_le(slot + finished_offset);
    return 0;
}

bool lingqu_shmem_pto_completion_succeeded(
    const struct lingqu_shmem_pto_completion *completion)
{
    return completion && completion->source == 1 &&
           completion->status == 1;
}

int lingqu_shmem_pto_endpoint_open(
    struct lingqu_shmem_pto_endpoint **endpoint_out,
    struct lingqu_shmem_pto_endpoint_info *info_out)
{
    struct lingqu_shmem_pto_endpoint_info info = { 0 };
    struct lingqu_shmem_pto_endpoint *endpoint;
    size_t index;
    int rc = -ENODEV;

    if (!endpoint_out) {
        return -EINVAL;
    }
    *endpoint_out = NULL;
    endpoint = calloc(1, sizeof(*endpoint));
    if (!endpoint) {
        return -ENOMEM;
    }
    endpoint->resource_fd = -1;
    endpoint->cmdq = MAP_FAILED;
    endpoint->cq = MAP_FAILED;
    for (index = 0;
         index < sizeof(lingqu_resource_candidates) /
                     sizeof(lingqu_resource_candidates[0]);
         index++) {
        rc = map_resource(endpoint, lingqu_resource_candidates[index],
                          &info);
        if (rc == 0) {
            break;
        }
    }
    if (rc != 0) {
        free(endpoint);
        return rc;
    }
    rc = map_queues(endpoint);
    if (rc != 0) {
        lingqu_shmem_pto_endpoint_close(endpoint);
        return rc;
    }
    info.cmdq_depth = endpoint->cmdq_depth;
    info.cq_depth = endpoint->cq_depth;
    if (info_out) {
        *info_out = info;
    }
    *endpoint_out = endpoint;
    return 0;
}

static struct lingqu_shmem_pto_queued_request *queued_request(
    struct lingqu_shmem_pto_endpoint *endpoint, uint64_t op_id)
{
    for (unsigned i = 0; i < LINGQU_SHMEM_PTO_QUEUED_LIMIT; ++i)
        if (endpoint->queued[i].op_id == op_id) return &endpoint->queued[i];
    return NULL;
}

static int queue_mapping_valid(struct lingqu_shmem_pto_endpoint *endpoint)
{
    uint64_t cmdq_phys;
    uint64_t cq_phys;
    int rc;

    if (!endpoint->cmdq_phys && !endpoint->cq_phys)
        return 0;
    if (mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_BASE) !=
            endpoint->cmdq_phys ||
        mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CQ_BASE) !=
            endpoint->cq_phys)
        return -ESTALE;
    rc = phys_for_virt(endpoint->cmdq, &cmdq_phys);
    if (rc) return rc;
    rc = phys_for_virt(endpoint->cq, &cq_phys);
    if (rc) return rc;
    return cmdq_phys == endpoint->cmdq_phys &&
                   cq_phys == endpoint->cq_phys ? 0 : -ESTALE;
}

static int queued_collect(struct lingqu_shmem_pto_endpoint *endpoint)
{
    if (endpoint->protocol_error) return -EPROTO;
    uint64_t head = mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CQ_HEAD);
    uint64_t tail = mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CQ_TAIL);
    if (endpoint->cq_depth < 2 || endpoint->cq_depth > 64 ||
        head >= endpoint->cq_depth || tail >= endpoint->cq_depth) goto invalid;
    while (head != tail) {
        uint8_t bytes[LINGQU_SHMEM_PTO_SLOT_BYTES];
        struct lingqu_shmem_pto_completion decoded;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        memcpy(bytes, endpoint->cq + head * LINGQU_SHMEM_PTO_SLOT_BYTES, sizeof(bytes));
        if (lingqu_shmem_pto_completion_decode(bytes, &decoded) || decoded.source != 1)
            goto invalid;
        struct lingqu_shmem_pto_queued_request *request = queued_request(endpoint, decoded.op_id);
        if (!request || request->ready) goto invalid;
        request->completion = decoded;
        request->ready = true;
        head = (head + 1) % endpoint->cq_depth;
        mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_HEAD, head);
    }
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_IRQ_ACK,
                 mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_IRQ_STATUS));
    return 0;
invalid:
    endpoint->protocol_error = true;
    return -EPROTO;
}

int lingqu_shmem_pto_endpoint_enqueue(
    struct lingqu_shmem_pto_endpoint *endpoint, const LingquPtoDispatchSlotV2 *slot)
{
    if (!endpoint || !slot || slot->descriptor_tag != LINGQU_PTO_DISPATCH_SLOT_TAG_V2)
        return -EINVAL;
    uint64_t op_id = load_u64_le(slot->op_id_le);
    if (!op_id) return -EINVAL;
    if (endpoint->pending_op_id) return -EBUSY;
    if (endpoint->protocol_error) return -EPROTO;
    if (op_id <= endpoint->queued_last_op_id) return -ESTALE;
    if (endpoint->cmdq_depth < 2 || endpoint->cmdq_depth > 64 ||
        endpoint->cq_depth < 2 || endpoint->cq_depth > 64) return -EINVAL;
    uint32_t capacity = endpoint->cmdq_depth - 1;
    if (capacity >= endpoint->cq_depth) capacity = endpoint->cq_depth - 1;
    if (capacity > LINGQU_SHMEM_PTO_QUEUED_LIMIT) capacity = LINGQU_SHMEM_PTO_QUEUED_LIMIT;
    if (endpoint->queued_count >= capacity) return -ENOSPC;
    uint64_t head = mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_HEAD);
    uint64_t tail = mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_TAIL);
    if (head >= endpoint->cmdq_depth || tail >= endpoint->cmdq_depth) {
        endpoint->protocol_error = true;
        return -EPROTO;
    }
    uint32_t next = (tail + 1) % endpoint->cmdq_depth;
    if (next == head) return -ENOSPC;
    int rc = queue_mapping_valid(endpoint);
    if (rc) {
        endpoint->protocol_error = true;
        return rc;
    }
    rc = queued_collect(endpoint);
    if (rc) return rc;
    struct lingqu_shmem_pto_queued_request *request = queued_request(endpoint, 0);
    if (!request) return -ENOSPC;
    memcpy(endpoint->cmdq + tail * LINGQU_SHMEM_PTO_SLOT_BYTES, slot, sizeof(*slot));
    rc = queue_mapping_valid(endpoint);
    if (rc) {
        endpoint->protocol_error = true;
        return rc;
    }
    fprintf(stderr, "lingqu-shmem-pto enqueue cmdq=%#" PRIx64
            " slot=%" PRIu64 " tag=%u op_id=%" PRIu64 "\n",
            endpoint->cmdq_phys, tail,
            endpoint->cmdq[tail * LINGQU_SHMEM_PTO_SLOT_BYTES],
            load_u64_le(endpoint->cmdq +
                        tail * LINGQU_SHMEM_PTO_SLOT_BYTES + 1));
    memset(request, 0, sizeof(*request));
    request->op_id = op_id;
    ++endpoint->queued_count;
    endpoint->queued_last_op_id = op_id;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_TAIL, next);
    /* Announce the entire occupied ring: an earlier slot may still await
     * authorization and its resume budget must include this new submission. */
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_DOORBELL,
                 (next + endpoint->cmdq_depth - head) % endpoint->cmdq_depth);
    return 0;
}

int lingqu_shmem_pto_endpoint_poll(
    struct lingqu_shmem_pto_endpoint *endpoint, uint64_t op_id,
    struct lingqu_shmem_pto_completion *completion)
{
    if (!endpoint || !op_id || !completion) return -EINVAL;
    memset(completion, 0, sizeof(*completion));
    if (endpoint->pending_op_id) return -EBUSY;
    if (endpoint->protocol_error) return -EPROTO;
    struct lingqu_shmem_pto_queued_request *request = queued_request(endpoint, op_id);
    if (!request) return -ENOENT;
    int rc = queued_collect(endpoint);
    if (rc) return rc;
    if (!request->ready) return -EAGAIN;
    *completion = request->completion;
    memset(request, 0, sizeof(*request));
    --endpoint->queued_count;
    return 0;
}

uint32_t lingqu_shmem_pto_endpoint_queued_count(
    const struct lingqu_shmem_pto_endpoint *endpoint)
{
    return endpoint ? endpoint->queued_count : 0;
}

static int endpoint_wait_until(
    struct lingqu_shmem_pto_endpoint *endpoint,
    uint64_t deadline,
    struct lingqu_shmem_pto_completion *completion)
{
    uint8_t completion_slot[LINGQU_SHMEM_PTO_SLOT_BYTES];
    uint64_t op_id = endpoint->pending_op_id;

    if (endpoint->protocol_error) return -EPROTO;
    for (;;) {
        uint32_t head = endpoint->pending_cq_head;
        uint32_t observed_tail = (uint32_t)mmio_read64(
            endpoint->endpoint_mmio, LINGQU_REG_CQ_TAIL) % endpoint->cq_depth;
        if (observed_tail != head) {
            struct lingqu_shmem_pto_completion decoded;
            uint32_t next = (head + 1) % endpoint->cq_depth;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            memcpy(completion_slot,
                   endpoint->cq + (size_t)head * LINGQU_SHMEM_PTO_SLOT_BYTES,
                   sizeof(completion_slot));
            if (observed_tail != next ||
                lingqu_shmem_pto_completion_decode(completion_slot, &decoded) ||
                decoded.op_id != op_id || decoded.source != 1) {
                endpoint->protocol_error = true;
                return -EPROTO;
            }
            mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_HEAD, next);
            mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_IRQ_ACK,
                         mmio_read64(endpoint->endpoint_mmio, LINGQU_REG_IRQ_STATUS));
            endpoint->pending_op_id = 0;
            endpoint->cancel_at = 0;
            *completion = decoded;
            return 0;
        }
        uint64_t now;
        int rc = monotonic_ms(&now);
        if (rc) return rc;
        if (!endpoint->cancel_requested && endpoint->cancel_at &&
            now >= endpoint->cancel_at) {
            mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CANCEL_OP_ID, op_id);
            mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CANCEL_DOORBELL, 1);
            endpoint->cancel_requested = true;
        }
        if (now >= deadline) return -ETIMEDOUT;
        const struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&pause, NULL);
    }
}

int lingqu_shmem_pto_endpoint_wait(
    struct lingqu_shmem_pto_endpoint *endpoint,
    uint64_t op_id,
    uint64_t timeout_ms,
    struct lingqu_shmem_pto_completion *completion)
{
    if (!endpoint || !op_id || !timeout_ms || !completion) return -EINVAL;
    memset(completion, 0, sizeof(*completion));
    if (!endpoint->pending_op_id) return -ENOENT;
    if (op_id != endpoint->pending_op_id) return -ESTALE;
    uint64_t started;
    int rc = monotonic_ms(&started);
    if (rc) return rc;
    if (started > UINT64_MAX - timeout_ms) return -EOVERFLOW;
    return endpoint_wait_until(endpoint, started + timeout_ms, completion);
}

uint64_t lingqu_shmem_pto_endpoint_pending_op(
    const struct lingqu_shmem_pto_endpoint *endpoint)
{
    return endpoint ? endpoint->pending_op_id : 0;
}

static int endpoint_submit_internal(
    struct lingqu_shmem_pto_endpoint *endpoint,
    const LingquPtoDispatchSlotV2 *slot,
    uint64_t timeout_ms,
    uint64_t cancel_after_ms,
    struct lingqu_shmem_pto_completion *completion)
{
    uint64_t cancel_at = 0;
    uint64_t deadline;
    uint64_t started_at;
    uint64_t op_id;
    uint32_t cmdq_head;
    uint32_t cmdq_tail;
    uint32_t cmdq_next;
    uint32_t cq_head;
    uint32_t cq_tail;

    if (!endpoint || !slot || !completion || timeout_ms == 0 ||
        (cancel_after_ms != 0 && cancel_after_ms >= timeout_ms) ||
        slot->descriptor_tag != LINGQU_PTO_DISPATCH_SLOT_TAG_V2) {
        return -EINVAL;
    }
    op_id = load_u64_le(slot->op_id_le);
    if (op_id == 0) {
        return -EINVAL;
    }
    memset(completion, 0, sizeof(*completion));
    if (endpoint->pending_op_id || endpoint->queued_count) return -EBUSY;
    if (endpoint->protocol_error) return -EPROTO;
    int rc = monotonic_ms(&started_at);
    if (rc) return rc;
    if (started_at > UINT64_MAX - timeout_ms) return -EOVERFLOW;
    deadline = started_at + timeout_ms;
    if (cancel_after_ms) cancel_at = started_at + cancel_after_ms;
    cmdq_head = (uint32_t)mmio_read64(
        endpoint->endpoint_mmio, LINGQU_REG_CMDQ_HEAD) %
        endpoint->cmdq_depth;
    cmdq_tail = (uint32_t)mmio_read64(
        endpoint->endpoint_mmio, LINGQU_REG_CMDQ_TAIL) %
        endpoint->cmdq_depth;
    cmdq_next = (cmdq_tail + 1) % endpoint->cmdq_depth;
    if (cmdq_next == cmdq_head) {
        return -ENOSPC;
    }
    cq_head = (uint32_t)mmio_read64(
        endpoint->endpoint_mmio, LINGQU_REG_CQ_HEAD) %
        endpoint->cq_depth;
    cq_tail = (uint32_t)mmio_read64(
        endpoint->endpoint_mmio, LINGQU_REG_CQ_TAIL) %
        endpoint->cq_depth;
    if (cq_tail != cq_head) return -EBUSY;
    memcpy(endpoint->cmdq +
               (size_t)cmdq_tail * LINGQU_SHMEM_PTO_SLOT_BYTES,
           slot, sizeof(*slot));
    endpoint->pending_op_id = op_id;
    endpoint->pending_cq_head = cq_head;
    endpoint->cancel_at = cancel_at;
    endpoint->cancel_requested = false;
    if (op_id > endpoint->queued_last_op_id) endpoint->queued_last_op_id = op_id;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_TAIL,
                 cmdq_next);
    mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_DOORBELL, 1);

    return endpoint_wait_until(endpoint, deadline, completion);
}

int lingqu_shmem_pto_endpoint_submit(
    struct lingqu_shmem_pto_endpoint *endpoint,
    const LingquPtoDispatchSlotV2 *slot,
    uint64_t timeout_ms,
    struct lingqu_shmem_pto_completion *completion)
{
    return endpoint_submit_internal(
        endpoint, slot, timeout_ms, 0, completion);
}

int lingqu_shmem_pto_endpoint_submit_cancel_after(
    struct lingqu_shmem_pto_endpoint *endpoint,
    const LingquPtoDispatchSlotV2 *slot,
    uint64_t timeout_ms,
    uint64_t cancel_after_ms,
    struct lingqu_shmem_pto_completion *completion)
{
    if (cancel_after_ms == 0) {
        return -EINVAL;
    }
    return endpoint_submit_internal(
        endpoint, slot, timeout_ms, cancel_after_ms, completion);
}

int lingqu_shmem_pto_endpoint_close_checked(
    struct lingqu_shmem_pto_endpoint *endpoint)
{
    if (!endpoint) {
        return 0;
    }
    if (endpoint->pending_op_id || endpoint->queued_count || endpoint->protocol_error)
        return -EBUSY;
    if (endpoint->endpoint_mmio) {
        mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CMDQ_BASE, 0);
        mmio_write64(endpoint->endpoint_mmio, LINGQU_REG_CQ_BASE, 0);
    }
    if (endpoint->cmdq && endpoint->cmdq != MAP_FAILED) {
        munmap(endpoint->cmdq, LINGQU_SHMEM_PTO_PAGE_BYTES);
    }
    if (endpoint->cq && endpoint->cq != MAP_FAILED) {
        munmap(endpoint->cq, LINGQU_SHMEM_PTO_PAGE_BYTES);
    }
    if (endpoint->endpoint_mmio) {
        munmap((void *)endpoint->endpoint_mmio,
               LINGQU_SHMEM_PTO_PAGE_BYTES);
    }
    if (endpoint->root_mmio) {
        munmap((void *)endpoint->root_mmio,
               LINGQU_SHMEM_PTO_PAGE_BYTES);
    }
    if (endpoint->resource_fd >= 0) {
        close(endpoint->resource_fd);
    }
    free(endpoint);
    return 0;
}

void lingqu_shmem_pto_endpoint_close(
    struct lingqu_shmem_pto_endpoint *endpoint)
{
    (void)lingqu_shmem_pto_endpoint_close_checked(endpoint);
}
