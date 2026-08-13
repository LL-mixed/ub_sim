/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "obmm_async.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OBMM_COROUTINE_MIN_STACK (16U * 1024U)
#define OBMM_IRQ_WAIT_MS 100
#define OBMM_METRIC_SAMPLE_CAPACITY 4096

enum obmm_coroutine_state {
    OBMM_COROUTINE_FREE,
    OBMM_COROUTINE_READY,
    OBMM_COROUTINE_RUNNING,
    OBMM_COROUTINE_WAIT_REMOTE,
    OBMM_COROUTINE_DONE,
};

struct obmm_context {
    uint64_t x19_x30[12];
    uint64_t sp;
    uint64_t padding;
    unsigned char q8_q15[8][16];
    uint64_t fpcr;
    uint64_t fpsr;
};

struct obmm_future_slot {
    enum obmm_async_future_state state;
    uint32_t generation;
    struct obmm_async_cq_entry_v1 cqe;
    struct obmm_coroutine *waiter;
};

struct obmm_coroutine {
    struct obmm_async *runtime;
    struct obmm_context context;
    enum obmm_coroutine_state state;
    uint32_t generation;
    uint16_t slot;
    obmm_coroutine_entry_fn entry;
    void *arg;
    void *stack_mapping;
    size_t stack_mapping_bytes;
};

struct obmm_metric_samples {
    uint64_t values[OBMM_METRIC_SAMPLE_CAPACITY];
    uint64_t seen;
    uint32_t stored;
};

struct obmm_async {
    int fd;
    enum obmm_async_mode mode;
    uint32_t spin_us;
    struct obmm_async_info_v1 info;
    void *queue_mapping;
    void *buffer_mapping;
    struct obmm_async_sq_entry_v1 *sq;
    struct obmm_async_cq_entry_v1 *cq;
    uint64_t sq_tail;
    uint64_t sq_head;
    uint64_t cq_head;
    uint64_t cq_tail;
    struct obmm_future_slot futures[OBMM_ASYNC_QUEUE_DEPTH];
    struct obmm_context scheduler_context;
    struct obmm_coroutine coroutines[OBMM_ASYNC_MAX_COROUTINES];
    struct obmm_coroutine *current;
    uint16_t next_coroutine;
    struct obmm_async_metrics metrics;
    struct obmm_metric_samples submit_samples;
    struct obmm_metric_samples switch_samples;
    struct obmm_metric_samples cq_drain_samples;
    uint64_t switch_started_ns;
};

extern void obmm_context_switch(struct obmm_context *from,
                                const struct obmm_context *to);
extern void obmm_context_start(void);

#if !defined(__aarch64__)
void obmm_context_switch(struct obmm_context *from,
                         const struct obmm_context *to)
{
    (void)from;
    (void)to;
    abort();
}

void obmm_context_start(void)
{
    abort();
}
#endif

static uint64_t obmm_monotonic_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static uint64_t obmm_metric_mix(uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static void obmm_metric_record(struct obmm_metric_samples *samples,
                               uint64_t value)
{
    uint64_t replacement;

    samples->seen++;
    if (samples->stored < OBMM_METRIC_SAMPLE_CAPACITY) {
        samples->values[samples->stored++] = value;
        return;
    }
    replacement = obmm_metric_mix(samples->seen) % samples->seen;
    if (replacement < OBMM_METRIC_SAMPLE_CAPACITY) {
        samples->values[replacement] = value;
    }
}

static int obmm_metric_compare(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return (a > b) - (a < b);
}

static uint64_t obmm_metric_p50(
    const struct obmm_metric_samples *samples)
{
    uint64_t *copy;
    uint64_t result;

    if (!samples->stored) {
        return 0;
    }
    copy = malloc(samples->stored * sizeof(*copy));
    if (!copy) {
        return 0;
    }
    memcpy(copy, samples->values, samples->stored * sizeof(*copy));
    qsort(copy, samples->stored, sizeof(*copy), obmm_metric_compare);
    result = copy[(samples->stored - 1) / 2];
    free(copy);
    return result;
}

static void obmm_switch_begin(struct obmm_async *runtime)
{
    runtime->switch_started_ns = obmm_monotonic_ns();
}

static void obmm_switch_finish(struct obmm_async *runtime)
{
    uint64_t now_ns;
    uint64_t elapsed_ns;

    if (!runtime->switch_started_ns) {
        return;
    }
    now_ns = obmm_monotonic_ns();
    elapsed_ns = now_ns - runtime->switch_started_ns;
    runtime->switch_started_ns = 0;
    runtime->metrics.switch_ns_total += elapsed_ns;
    obmm_metric_record(&runtime->switch_samples, elapsed_ns);
}

uint64_t obmm_async_token_pack(uint32_t generation, uint16_t queue_id,
                               uint16_t slot)
{
    return ((uint64_t)generation << 32) |
        ((uint64_t)queue_id << 16) | slot;
}

uint32_t obmm_async_token_generation(uint64_t token)
{
    return token >> 32;
}

uint16_t obmm_async_token_queue(uint64_t token)
{
    return token >> 16;
}

uint16_t obmm_async_token_slot(uint64_t token)
{
    return token;
}

int obmm_async_status_to_errno(int32_t status)
{
    switch (status) {
    case OBMM_ASYNC_STATUS_OK:
        return 0;
    case OBMM_ASYNC_STATUS_INVALID:
        return EINVAL;
    case OBMM_ASYNC_STATUS_NO_MAP:
    case OBMM_ASYNC_STATUS_STALE:
    case OBMM_ASYNC_STATUS_RETIRED:
        return ESTALE;
    case OBMM_ASYNC_STATUS_BOUNDS:
        return ERANGE;
    case OBMM_ASYNC_STATUS_PERMISSION:
        return EACCES;
    case OBMM_ASYNC_STATUS_TIMEOUT:
        return ETIMEDOUT;
    case OBMM_ASYNC_STATUS_REMOTE_IO:
        return EIO;
    case OBMM_ASYNC_STATUS_CHECKSUM:
        return EBADMSG;
    case OBMM_ASYNC_STATUS_CANCELLED:
        return ECANCELED;
    case OBMM_ASYNC_STATUS_UNSUPPORTED:
        return EOPNOTSUPP;
    default:
        return EPROTO;
    }
}

static int obmm_kick(struct obmm_async *runtime,
                     struct obmm_async_kick_v1 *kick)
{
    *kick = (struct obmm_async_kick_v1) {
        .sq_tail = runtime->sq_tail,
        .cq_head = runtime->cq_head,
    };
    if (ioctl(runtime->fd, OBMM_ASYNC_IOCTL_KICK, kick) != 0) {
        return -errno;
    }
    runtime->sq_head = kick->sq_head;
    runtime->cq_tail = kick->cq_tail;
    return 0;
}

static bool obmm_runtime_valid(const struct obmm_async *runtime)
{
    return runtime && runtime->fd >= 0 && runtime->queue_mapping &&
        runtime->buffer_mapping;
}

int obmm_async_open(struct obmm_async **runtime_out,
                    const struct obmm_async_options *options)
{
    struct obmm_async_options defaults = {
        .device_path = OBMM_ASYNC_DEFAULT_DEVICE,
        .mode = OBMM_ASYNC_MODE_POLL,
        .spin_us = 10,
    };
    struct obmm_async *runtime;
    const char *device_path;
    int saved_errno;

    if (!runtime_out) {
        return -EINVAL;
    }
    *runtime_out = NULL;
    if (!options) {
        options = &defaults;
    }
    device_path = options->device_path ? options->device_path :
        OBMM_ASYNC_DEFAULT_DEVICE;
    if (options->mode != OBMM_ASYNC_MODE_POLL &&
        options->mode != OBMM_ASYNC_MODE_IRQ) {
        return -EINVAL;
    }
    runtime = calloc(1, sizeof(*runtime));
    if (!runtime) {
        return -ENOMEM;
    }
    runtime->fd = -1;
    runtime->mode = options->mode;
    runtime->spin_us = options->spin_us;
    runtime->fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (runtime->fd < 0) {
        saved_errno = errno;
        free(runtime);
        return -saved_errno;
    }
    if (ioctl(runtime->fd, OBMM_ASYNC_IOCTL_GET_INFO,
              &runtime->info) != 0) {
        saved_errno = errno;
        goto fail;
    }
    if (runtime->info.abi_version != OBMM_ASYNC_ABI_VERSION ||
        runtime->info.queue_depth != OBMM_ASYNC_QUEUE_DEPTH ||
        runtime->info.slot_bytes != OBMM_ASYNC_SLOT_BYTES ||
        runtime->info.queue_mmap_bytes !=
            2 * OBMM_ASYNC_QUEUE_DEPTH * OBMM_ASYNC_SLOT_BYTES ||
        runtime->info.buffer_mmap_bytes == 0) {
        saved_errno = EPROTO;
        goto fail;
    }
    runtime->queue_mapping = mmap(
        NULL, runtime->info.queue_mmap_bytes, PROT_READ | PROT_WRITE,
        MAP_SHARED, runtime->fd, runtime->info.queue_mmap_offset);
    if (runtime->queue_mapping == MAP_FAILED) {
        runtime->queue_mapping = NULL;
        saved_errno = errno;
        goto fail;
    }
    runtime->buffer_mapping = mmap(
        NULL, runtime->info.buffer_mmap_bytes, PROT_READ | PROT_WRITE,
        MAP_SHARED, runtime->fd, runtime->info.buffer_mmap_offset);
    if (runtime->buffer_mapping == MAP_FAILED) {
        runtime->buffer_mapping = NULL;
        saved_errno = errno;
        goto fail;
    }
    runtime->sq = runtime->queue_mapping;
    runtime->cq = (struct obmm_async_cq_entry_v1 *)((unsigned char *)
        runtime->queue_mapping +
        OBMM_ASYNC_QUEUE_DEPTH * OBMM_ASYNC_SLOT_BYTES);
    *runtime_out = runtime;
    return 0;

fail:
    if (runtime->queue_mapping) {
        munmap(runtime->queue_mapping, runtime->info.queue_mmap_bytes);
    }
    close(runtime->fd);
    free(runtime);
    return -saved_errno;
}

static void obmm_coroutine_destroy_all(struct obmm_async *runtime)
{
    uint16_t slot;

    for (slot = 0; slot < OBMM_ASYNC_MAX_COROUTINES; slot++) {
        struct obmm_coroutine *coroutine = &runtime->coroutines[slot];

        if (coroutine->stack_mapping) {
            munmap(coroutine->stack_mapping,
                   coroutine->stack_mapping_bytes);
        }
    }
}

void obmm_async_close(struct obmm_async *runtime)
{
    if (!runtime) {
        return;
    }
    obmm_coroutine_destroy_all(runtime);
    if (runtime->buffer_mapping) {
        munmap(runtime->buffer_mapping,
               runtime->info.buffer_mmap_bytes);
    }
    if (runtime->queue_mapping) {
        munmap(runtime->queue_mapping,
               runtime->info.queue_mmap_bytes);
    }
    if (runtime->fd >= 0) {
        close(runtime->fd);
    }
    free(runtime);
}

int obmm_async_map_register(struct obmm_async *runtime, int obmm_fd,
                            uint64_t mem_id, void *mapped_addr,
                            uint64_t length, struct obmm_async_map *map)
{
    struct obmm_async_map_register_v1 request = {
        .mem_id = mem_id,
        .mapped_addr = (uintptr_t)mapped_addr,
        .length = length,
    };

    if (!obmm_runtime_valid(runtime) || obmm_fd < 0 || !mapped_addr ||
        !length || !map) {
        return -EINVAL;
    }
    if (ioctl(runtime->fd, OBMM_ASYNC_IOCTL_MAP_REGISTER, &request) != 0) {
        return -errno;
    }
    *map = (struct obmm_async_map) {
        .id = request.map_id,
        .generation = request.map_generation,
        .length = length,
    };
    return 0;
}

int obmm_async_map_unregister(struct obmm_async *runtime,
                              struct obmm_async_map *map)
{
    struct obmm_async_map_unregister_v1 request;

    if (!obmm_runtime_valid(runtime) || !map || !map->id) {
        return -EINVAL;
    }
    request = (struct obmm_async_map_unregister_v1) {
        .map_id = map->id,
        .map_generation = map->generation,
    };
    if (ioctl(runtime->fd, OBMM_ASYNC_IOCTL_MAP_UNREGISTER,
              &request) != 0) {
        return -errno;
    }
    memset(map, 0, sizeof(*map));
    return 0;
}

int obmm_async_buffer_alloc(struct obmm_async *runtime, uint64_t length,
                            struct obmm_async_buffer *buffer)
{
    struct obmm_async_buffer_alloc_v1 request = {
        .length = length,
    };

    if (!obmm_runtime_valid(runtime) || !buffer || !length) {
        return -EINVAL;
    }
    if (ioctl(runtime->fd, OBMM_ASYNC_IOCTL_BUFFER_ALLOC, &request) != 0) {
        return -errno;
    }
    if (request.arena_offset > runtime->info.buffer_mmap_bytes ||
        length > runtime->info.buffer_mmap_bytes - request.arena_offset) {
        return -EPROTO;
    }
    *buffer = (struct obmm_async_buffer) {
        .id = request.buffer_id,
        .generation = request.generation,
        .length = length,
        .data = (unsigned char *)runtime->buffer_mapping +
            request.arena_offset,
    };
    return 0;
}

int obmm_async_buffer_free(struct obmm_async *runtime,
                           struct obmm_async_buffer *buffer)
{
    struct obmm_async_buffer_free_v1 request;

    if (!obmm_runtime_valid(runtime) || !buffer || !buffer->id) {
        return -EINVAL;
    }
    request = (struct obmm_async_buffer_free_v1) {
        .buffer_id = buffer->id,
        .generation = buffer->generation,
    };
    if (ioctl(runtime->fd, OBMM_ASYNC_IOCTL_BUFFER_FREE, &request) != 0) {
        return -errno;
    }
    memset(buffer, 0, sizeof(*buffer));
    return 0;
}

static void obmm_copy_result(const struct obmm_async_cq_entry_v1 *cqe,
                             struct obmm_async_result *result)
{
    if (!result) {
        return;
    }
    *result = (struct obmm_async_result) {
        .status = cqe->status,
        .bytes_done = cqe->bytes_done,
        .provider_status = cqe->provider_status,
        .checksum64 = cqe->checksum64,
        .completed_ns = cqe->completed_ns,
        .map_generation = cqe->map_generation,
        .user_data = cqe->user_data,
    };
}

int obmm_async_drain(struct obmm_async *runtime)
{
    struct obmm_async_kick_v1 kick;
    uint64_t started_ns;
    uint64_t available;
    bool had_completions;
    int completed = 0;
    int ret;

    if (!obmm_runtime_valid(runtime)) {
        return -EINVAL;
    }
    started_ns = obmm_monotonic_ns();
    ret = obmm_kick(runtime, &kick);
    if (ret) {
        return ret;
    }
    if (runtime->cq_tail < runtime->cq_head ||
        runtime->cq_tail - runtime->cq_head > OBMM_ASYNC_QUEUE_DEPTH) {
        return -EPROTO;
    }
    atomic_thread_fence(memory_order_acquire);
    available = runtime->cq_tail - runtime->cq_head;
    had_completions = available != 0;
    while (available--) {
        uint64_t cq_slot = runtime->cq_head &
            (OBMM_ASYNC_QUEUE_DEPTH - 1);
        struct obmm_async_cq_entry_v1 cqe = runtime->cq[cq_slot];
        uint16_t slot = obmm_async_token_slot(cqe.token);
        struct obmm_future_slot *future = NULL;

        if (cqe.abi_version == OBMM_ASYNC_ABI_VERSION &&
            cqe.opcode == OBMM_ASYNC_CQ_READ_COMPLETE &&
            obmm_async_token_queue(cqe.token) == runtime->info.queue_id &&
            slot < OBMM_ASYNC_QUEUE_DEPTH) {
            future = &runtime->futures[slot];
        }
        if (!future || future->state != OBMM_ASYNC_FUTURE_SUBMITTED ||
            future->generation !=
                obmm_async_token_generation(cqe.token)) {
            runtime->metrics.stale_completions++;
        } else {
            future->cqe = cqe;
            future->state = OBMM_ASYNC_FUTURE_READY;
            runtime->metrics.completed++;
            if (cqe.status != OBMM_ASYNC_STATUS_OK) {
                runtime->metrics.failed++;
            }
            if (future->waiter &&
                future->waiter->state == OBMM_COROUTINE_WAIT_REMOTE) {
                future->waiter->state = OBMM_COROUTINE_READY;
                future->waiter = NULL;
            }
            completed++;
        }
        runtime->cq_head++;
    }
    if (completed || runtime->cq_head != kick.cq_head) {
        ret = obmm_kick(runtime, &kick);
        if (ret) {
            return ret;
        }
    }
    if (had_completions) {
        uint64_t elapsed_ns = obmm_monotonic_ns() - started_ns;

        runtime->metrics.cq_drain_ns_total += elapsed_ns;
        obmm_metric_record(&runtime->cq_drain_samples, elapsed_ns);
    }
    return completed;
}

static struct obmm_future_slot *obmm_future_reserve(
    struct obmm_async *runtime, uint16_t *slot_out)
{
    uint16_t slot;

    for (slot = 0; slot < OBMM_ASYNC_QUEUE_DEPTH; slot++) {
        struct obmm_future_slot *future = &runtime->futures[slot];

        if (future->state == OBMM_ASYNC_FUTURE_FREE ||
            future->state == OBMM_ASYNC_FUTURE_CONSUMED) {
            future->generation++;
            if (!future->generation) {
                future->generation++;
            }
            future->state = OBMM_ASYNC_FUTURE_SUBMITTED;
            memset(&future->cqe, 0, sizeof(future->cqe));
            future->waiter = NULL;
            *slot_out = slot;
            return future;
        }
    }
    return NULL;
}

int obmm_load_submit(struct obmm_async *runtime,
                     const struct obmm_async_map *map,
                     uint64_t remote_offset,
                     const struct obmm_async_buffer *buffer,
                     uint32_t dst_offset, uint32_t length,
                     uint64_t deadline_ns, uint64_t user_data,
                     struct obmm_async_future *future_out)
{
    struct obmm_async_kick_v1 kick;
    struct obmm_future_slot *future;
    struct obmm_async_sq_entry_v1 *sqe;
    uint16_t slot;
    uint64_t started_ns;
    uint64_t token;
    int ret;

    if (!obmm_runtime_valid(runtime) || !map || !map->id || !buffer ||
        !buffer->id || !future_out || !length ||
        remote_offset > map->length || length > map->length - remote_offset ||
        dst_offset > buffer->length || length > buffer->length - dst_offset) {
        return -EINVAL;
    }
    started_ns = obmm_monotonic_ns();
    ret = obmm_async_drain(runtime);
    if (ret < 0) {
        return ret;
    }
    if (runtime->sq_tail - runtime->sq_head >= OBMM_ASYNC_QUEUE_DEPTH) {
        ret = obmm_kick(runtime, &kick);
        if (ret) {
            return ret;
        }
        if (runtime->sq_tail - runtime->sq_head >=
            OBMM_ASYNC_QUEUE_DEPTH) {
            return -EAGAIN;
        }
    }
    future = obmm_future_reserve(runtime, &slot);
    if (!future) {
        return -EAGAIN;
    }
    token = obmm_async_token_pack(future->generation,
                                  runtime->info.queue_id, slot);
    sqe = &runtime->sq[runtime->sq_tail &
        (OBMM_ASYNC_QUEUE_DEPTH - 1)];
    *sqe = (struct obmm_async_sq_entry_v1) {
        .abi_version = OBMM_ASYNC_ABI_VERSION,
        .opcode = OBMM_ASYNC_SQ_READ,
        .length = length,
        .token = token,
        .map_id = map->id,
        .map_generation = map->generation,
        .remote_offset = remote_offset,
        .dst_buffer_id = buffer->id,
        .dst_offset = dst_offset,
        .deadline_ns = deadline_ns,
        .user_data = user_data,
    };
    atomic_thread_fence(memory_order_release);
    runtime->sq_tail++;
    ret = obmm_kick(runtime, &kick);
    if (ret) {
        future->state = OBMM_ASYNC_FUTURE_FREE;
        return ret;
    }
    if (kick.last_error) {
        future->state = OBMM_ASYNC_FUTURE_FREE;
        return -EIO;
    }
    future_out->token = token;
    runtime->metrics.submitted++;
    {
        uint64_t elapsed_ns = obmm_monotonic_ns() - started_ns;

        runtime->metrics.submit_ns_total += elapsed_ns;
        obmm_metric_record(&runtime->submit_samples, elapsed_ns);
    }
    return 0;
}

static struct obmm_future_slot *obmm_future_lookup(
    struct obmm_async *runtime, const struct obmm_async_future *future)
{
    uint16_t slot;

    if (!runtime || !future ||
        obmm_async_token_queue(future->token) != runtime->info.queue_id) {
        return NULL;
    }
    slot = obmm_async_token_slot(future->token);
    if (slot >= OBMM_ASYNC_QUEUE_DEPTH ||
        runtime->futures[slot].generation !=
            obmm_async_token_generation(future->token)) {
        return NULL;
    }
    return &runtime->futures[slot];
}

int obmm_test(struct obmm_async *runtime,
              const struct obmm_async_future *future_handle,
              struct obmm_async_result *result)
{
    struct obmm_future_slot *future;
    int ret;

    ret = obmm_async_drain(runtime);
    if (ret < 0) {
        return ret;
    }
    future = obmm_future_lookup(runtime, future_handle);
    if (!future || (future->state != OBMM_ASYNC_FUTURE_SUBMITTED &&
                    future->state != OBMM_ASYNC_FUTURE_READY)) {
        return -ESTALE;
    }
    if (future->state != OBMM_ASYNC_FUTURE_READY) {
        return 0;
    }
    obmm_copy_result(&future->cqe, result);
    return 1;
}

static int obmm_wait_for_work(struct obmm_async *runtime)
{
    uint64_t start_ns = obmm_monotonic_ns();
    uint64_t spin_ns = (uint64_t)runtime->spin_us * 1000;

    while (obmm_monotonic_ns() - start_ns < spin_ns) {
        int ret = obmm_async_drain(runtime);

        if (ret != 0) {
            return ret < 0 ? ret : 0;
        }
    }
    runtime->metrics.idle_polls++;
    if (runtime->mode == OBMM_ASYNC_MODE_IRQ) {
        struct pollfd poll_fd = {
            .fd = runtime->fd,
            .events = POLLIN,
        };
        uint64_t irq_snapshot[2];
        int ret = poll(&poll_fd, 1, OBMM_IRQ_WAIT_MS);

        if (ret < 0) {
            return errno == EINTR ? 0 : -errno;
        }
        if (ret > 0 && (poll_fd.revents & POLLIN)) {
            if (read(runtime->fd, irq_snapshot,
                     sizeof(irq_snapshot)) < 0 && errno != EINTR) {
                return -errno;
            }
        }
    } else {
        sched_yield();
    }
    return 0;
}

int obmm_await(struct obmm_async *runtime,
               struct obmm_async_future *future_handle,
               struct obmm_async_result *result)
{
    struct obmm_future_slot *future;
    int ret;

    if (!obmm_runtime_valid(runtime) || !future_handle) {
        return -EINVAL;
    }
    for (;;) {
        ret = obmm_test(runtime, future_handle, result);
        if (ret < 0) {
            return ret;
        }
        if (ret == 1) {
            break;
        }
        future = obmm_future_lookup(runtime, future_handle);
        if (!future) {
            return -ESTALE;
        }
        if (runtime->current) {
            uint64_t wait_started_ns = obmm_monotonic_ns();

            if (future->waiter && future->waiter != runtime->current) {
                return -EBUSY;
            }
            future->waiter = runtime->current;
            runtime->current->state = OBMM_COROUTINE_WAIT_REMOTE;
            runtime->metrics.coroutine_switches++;
            obmm_switch_begin(runtime);
            obmm_context_switch(&runtime->current->context,
                                &runtime->scheduler_context);
            obmm_switch_finish(runtime);
            runtime->metrics.wait_ns +=
                obmm_monotonic_ns() - wait_started_ns;
        } else {
            uint64_t wait_started_ns = obmm_monotonic_ns();

            ret = obmm_wait_for_work(runtime);
            runtime->metrics.wait_ns +=
                obmm_monotonic_ns() - wait_started_ns;
            if (ret) {
                return ret;
            }
        }
    }
    future = obmm_future_lookup(runtime, future_handle);
    if (!future || future->state != OBMM_ASYNC_FUTURE_READY) {
        return -ESTALE;
    }
    ret = -obmm_async_status_to_errno(future->cqe.status);
    future->state = OBMM_ASYNC_FUTURE_CONSUMED;
    future_handle->token = 0;
    return ret;
}

int obmm_cancel(struct obmm_async *runtime,
                const struct obmm_async_future *future)
{
    struct obmm_async_cancel_v1 request;

    if (!obmm_runtime_valid(runtime) ||
        !obmm_future_lookup(runtime, future)) {
        return -ESTALE;
    }
    request.token = future->token;
    if (ioctl(runtime->fd, OBMM_ASYNC_IOCTL_CANCEL, &request) != 0) {
        return -errno;
    }
    return 0;
}

static struct obmm_coroutine *obmm_coroutine_next(
    struct obmm_async *runtime)
{
    uint16_t checked;

    for (checked = 0; checked < OBMM_ASYNC_MAX_COROUTINES; checked++) {
        uint16_t slot = (runtime->next_coroutine + checked) %
            OBMM_ASYNC_MAX_COROUTINES;

        if (runtime->coroutines[slot].state == OBMM_COROUTINE_READY) {
            runtime->next_coroutine = (slot + 1) %
                OBMM_ASYNC_MAX_COROUTINES;
            return &runtime->coroutines[slot];
        }
    }
    return NULL;
}

static bool obmm_coroutine_waiting(const struct obmm_async *runtime)
{
    uint16_t slot;

    for (slot = 0; slot < OBMM_ASYNC_MAX_COROUTINES; slot++) {
        enum obmm_coroutine_state state = runtime->coroutines[slot].state;

        if (state == OBMM_COROUTINE_READY ||
            state == OBMM_COROUTINE_RUNNING ||
            state == OBMM_COROUTINE_WAIT_REMOTE) {
            return true;
        }
    }
    return false;
}

void obmm_context_entry(struct obmm_coroutine *coroutine)
{
    struct obmm_async *runtime = coroutine->runtime;

    obmm_switch_finish(runtime);
    coroutine->entry(coroutine->arg);
    coroutine->state = OBMM_COROUTINE_DONE;
    runtime->current = coroutine;
    runtime->metrics.coroutine_switches++;
    obmm_switch_begin(runtime);
    obmm_context_switch(&coroutine->context,
                        &runtime->scheduler_context);
    abort();
}

struct obmm_context_selftest_state {
    struct obmm_async *runtime;
    uint64_t counter;
};

static void obmm_context_selftest_entry(void *opaque)
{
    struct obmm_context_selftest_state *state = opaque;

    state->counter++;
    obmm_coroutine_yield(state->runtime);
    state->counter++;
}

int obmm_async_context_selftest(void)
{
#if defined(__aarch64__)
    struct obmm_async runtime = { .fd = -1 };
    struct obmm_coroutine *coroutine = &runtime.coroutines[0];
    struct obmm_context_selftest_state state = {
        .runtime = &runtime,
    };
    size_t page_bytes;
    size_t usable_bytes = OBMM_COROUTINE_MIN_STACK;
    size_t mapping_bytes;
    uintptr_t stack_top;
    void *mapping;
    long page_size = sysconf(_SC_PAGESIZE);
    int ret = 0;

    if (page_size <= 0 || (page_size & (page_size - 1)) != 0) {
        return -EINVAL;
    }
    page_bytes = page_size;
    usable_bytes = (usable_bytes + page_bytes - 1) & ~(page_bytes - 1);
    mapping_bytes = usable_bytes + 2 * page_bytes;
    mapping = mmap(NULL, mapping_bytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return -errno;
    }
    if (mprotect((unsigned char *)mapping + page_bytes,
                 usable_bytes, PROT_READ | PROT_WRITE) != 0) {
        ret = -errno;
        goto out;
    }
    *coroutine = (struct obmm_coroutine) {
        .runtime = &runtime,
        .state = OBMM_COROUTINE_RUNNING,
        .generation = 1,
        .entry = obmm_context_selftest_entry,
        .arg = &state,
        .stack_mapping = mapping,
        .stack_mapping_bytes = mapping_bytes,
    };
    stack_top = (uintptr_t)mapping + page_bytes + usable_bytes;
    stack_top &= ~(uintptr_t)0xf;
    coroutine->context.sp = stack_top;
    coroutine->context.x19_x30[0] = (uintptr_t)coroutine;
    coroutine->context.x19_x30[11] = (uintptr_t)obmm_context_start;

    runtime.current = coroutine;
    obmm_context_switch(&runtime.scheduler_context,
                        &coroutine->context);
    if (state.counter != 1 || coroutine->state != OBMM_COROUTINE_READY) {
        ret = -EUCLEAN;
        goto out;
    }
    runtime.current = coroutine;
    coroutine->state = OBMM_COROUTINE_RUNNING;
    obmm_context_switch(&runtime.scheduler_context,
                        &coroutine->context);
    if (state.counter != 2 || coroutine->state != OBMM_COROUTINE_DONE) {
        ret = -EUCLEAN;
    }

out:
    munmap(mapping, mapping_bytes);
    return ret;
#else
    return -EOPNOTSUPP;
#endif
}

int obmm_coroutine_create(struct obmm_async *runtime,
                          obmm_coroutine_entry_fn entry, void *arg,
                          size_t stack_bytes, uint64_t *coroutine_id)
{
    struct obmm_coroutine *coroutine = NULL;
    size_t mapping_bytes;
    size_t page_bytes;
    size_t usable_bytes;
    void *mapping;
    uintptr_t stack_top;
    uint32_t generation;
    long page_size;
    uint16_t slot;

    if (!obmm_runtime_valid(runtime) || !entry || !coroutine_id ||
        stack_bytes < OBMM_COROUTINE_MIN_STACK) {
        return -EINVAL;
    }
    for (slot = 0; slot < OBMM_ASYNC_MAX_COROUTINES; slot++) {
        if (runtime->coroutines[slot].state == OBMM_COROUTINE_FREE ||
            runtime->coroutines[slot].state == OBMM_COROUTINE_DONE) {
            coroutine = &runtime->coroutines[slot];
            break;
        }
    }
    if (!coroutine) {
        return -ENOSPC;
    }
    if (coroutine->stack_mapping) {
        munmap(coroutine->stack_mapping,
               coroutine->stack_mapping_bytes);
    }
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || (page_size & (page_size - 1)) != 0) {
        return -EINVAL;
    }
    page_bytes = page_size;
    usable_bytes = (stack_bytes + page_bytes - 1) & ~(page_bytes - 1);
    if (usable_bytes > SIZE_MAX - 2 * page_bytes) {
        return -EOVERFLOW;
    }
    mapping_bytes = usable_bytes + 2 * page_bytes;
    mapping = mmap(NULL, mapping_bytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        return -errno;
    }
    if (mprotect((unsigned char *)mapping + page_bytes,
                 usable_bytes, PROT_READ | PROT_WRITE) != 0) {
        int saved_errno = errno;

        munmap(mapping, mapping_bytes);
        return -saved_errno;
    }
    generation = coroutine->generation + 1;
    if (!generation) {
        generation++;
    }
    *coroutine = (struct obmm_coroutine) {
        .runtime = runtime,
        .state = OBMM_COROUTINE_READY,
        .generation = generation,
        .slot = slot,
        .entry = entry,
        .arg = arg,
        .stack_mapping = mapping,
        .stack_mapping_bytes = mapping_bytes,
    };
    stack_top = (uintptr_t)mapping + page_bytes + usable_bytes;
    stack_top &= ~(uintptr_t)0xf;
    coroutine->context.sp = stack_top;
    coroutine->context.x19_x30[0] = (uintptr_t)coroutine;
    coroutine->context.x19_x30[11] = (uintptr_t)obmm_context_start;
    *coroutine_id = obmm_async_token_pack(coroutine->generation, 1, slot);
    return 0;
}

int obmm_coroutine_run(struct obmm_async *runtime)
{
    int ret;

    if (!obmm_runtime_valid(runtime) || runtime->current) {
        return -EINVAL;
    }
    while (obmm_coroutine_waiting(runtime)) {
        struct obmm_coroutine *next;

        ret = obmm_async_drain(runtime);
        if (ret < 0) {
            return ret;
        }
        next = obmm_coroutine_next(runtime);
        if (!next) {
            uint64_t idle_started_ns = obmm_monotonic_ns();

            runtime->metrics.no_ready++;
            ret = obmm_wait_for_work(runtime);
            runtime->metrics.idle_ns +=
                obmm_monotonic_ns() - idle_started_ns;
            if (ret) {
                return ret;
            }
            continue;
        }
        runtime->current = next;
        next->state = OBMM_COROUTINE_RUNNING;
        runtime->metrics.coroutine_switches++;
        {
            uint64_t ready_started_ns = obmm_monotonic_ns();

            obmm_switch_begin(runtime);
            obmm_context_switch(&runtime->scheduler_context,
                                &next->context);
            obmm_switch_finish(runtime);
            runtime->metrics.ready_ns +=
                obmm_monotonic_ns() - ready_started_ns;
        }
        runtime->current = NULL;
    }
    return 0;
}

void obmm_coroutine_yield(struct obmm_async *runtime)
{
    struct obmm_coroutine *current;

    if (!runtime || !runtime->current) {
        return;
    }
    current = runtime->current;
    current->state = OBMM_COROUTINE_READY;
    runtime->metrics.coroutine_yields++;
    runtime->metrics.coroutine_switches++;
    obmm_switch_begin(runtime);
    obmm_context_switch(&current->context,
                        &runtime->scheduler_context);
    obmm_switch_finish(runtime);
}

void obmm_async_get_metrics(const struct obmm_async *runtime,
                            struct obmm_async_metrics *metrics)
{
    if (!runtime || !metrics) {
        return;
    }
    *metrics = runtime->metrics;
    metrics->submit_ns_p50 = obmm_metric_p50(&runtime->submit_samples);
    metrics->switch_ns_p50 = obmm_metric_p50(&runtime->switch_samples);
    metrics->cq_drain_ns_p50 =
        obmm_metric_p50(&runtime->cq_drain_samples);
}

int obmm_async_get_observability(
    const struct obmm_async *runtime,
    struct obmm_async_observability_v1 *observability)
{
    if (!obmm_runtime_valid(runtime) || !observability) {
        return -EINVAL;
    }
    memset(observability, 0, sizeof(*observability));
    return ioctl(runtime->fd, OBMM_ASYNC_IOCTL_GET_OBSERVABILITY,
                 observability) == 0 ? 0 : -errno;
}

int obmm_async_reset_observability(struct obmm_async *runtime)
{
    if (!obmm_runtime_valid(runtime)) {
        return -EINVAL;
    }
    if (ioctl(runtime->fd, OBMM_ASYNC_IOCTL_RESET_OBSERVABILITY) != 0) {
        return -errno;
    }
    memset(&runtime->metrics, 0, sizeof(runtime->metrics));
    memset(&runtime->submit_samples, 0,
           sizeof(runtime->submit_samples));
    memset(&runtime->switch_samples, 0,
           sizeof(runtime->switch_samples));
    memset(&runtime->cq_drain_samples, 0,
           sizeof(runtime->cq_drain_samples));
    runtime->switch_started_ns = 0;
    return 0;
}
