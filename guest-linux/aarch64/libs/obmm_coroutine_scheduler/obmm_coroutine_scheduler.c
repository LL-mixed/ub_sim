/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "obmm_coroutine_scheduler.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OBMM_COROUTINE_SCHEDULER_SCHEDULER_STACK_BYTES (64UL * 1024UL)

enum obmm_coroutine_scheduler_context_state {
    OBMM_COROUTINE_SCHEDULER_CONTEXT_FREE,
    OBMM_COROUTINE_SCHEDULER_CONTEXT_READY,
    OBMM_COROUTINE_SCHEDULER_CONTEXT_READY_REPLAY,
    OBMM_COROUTINE_SCHEDULER_CONTEXT_RUNNING,
    OBMM_COROUTINE_SCHEDULER_CONTEXT_WAIT_REMOTE,
    OBMM_COROUTINE_SCHEDULER_CONTEXT_FAULTED,
    OBMM_COROUTINE_SCHEDULER_CONTEXT_DONE,
};

struct obmm_coroutine_scheduler_context_local {
    enum obmm_coroutine_scheduler_context_state state;
    uint16_t slot;
    obmm_coroutine_scheduler_entry_fn entry;
    void *argument;
    void *mapping;
    size_t mapping_bytes;
    uint64_t waiting_token;
    struct obmm_async_load_context_v2 context __attribute__((aligned(16)));
};

struct obmm_coroutine_scheduler_map_local {
    bool allocated;
    struct obmm_coroutine_scheduler_map map;
};

struct obmm_coroutine_scheduler {
    int fd;
    uint64_t load_timeout_ns;
    obmm_coroutine_scheduler_trace_fn trace;
    void *trace_opaque;
    bool started;
    bool device_reset;
    bool replay_retire;
    bool hot_path_active;
    int first_error;
    uint16_t scheduler_cursor;
    uint16_t logical_contexts;
    uint64_t last_resumed_id;
    struct obmm_async_load_caps_v3 caps;
    struct obmm_coroutine_scheduler_metrics metrics;
    struct obmm_coroutine_scheduler_context_local *current;
    struct obmm_coroutine_scheduler_context_local contexts[OBMM_ASYNC_LOAD_MAX_CONTEXTS];
    struct obmm_coroutine_scheduler_map_local maps[OBMM_ASYNC_LOAD_MAX_PENDING_LOADS];
    void *scheduler_mapping;
    size_t scheduler_mapping_bytes;
    uintptr_t scheduler_stack_top;
    const struct obmm_async_load_event_producer_v3 *event_producer;
    const struct obmm_async_load_event_v3 *event_slots;
    struct obmm_async_load_event_consumer_v3 *event_consumer;
    void *event_ring_mapping;
    size_t event_ring_mapping_bytes;
    void *event_consumer_mapping;
    size_t event_consumer_mapping_bytes;
    sigjmp_buf return_environment;
};

static struct obmm_coroutine_scheduler *active_runtime;

extern void obmm_coroutine_scheduler_upcall_entry(void);
extern void obmm_coroutine_scheduler_context_bootstrap(void);
extern void obmm_coroutine_scheduler_context_finish(void) __attribute__((noreturn));
extern void obmm_coroutine_scheduler_context_resume(
    const struct obmm_async_load_context_v2 *context) __attribute__((noreturn));
extern void obmm_coroutine_scheduler_wait(void);
extern void obmm_coroutine_scheduler_scheduler_enter(void);

_Static_assert(sizeof(struct obmm_async_load_context_v2) ==
               OBMM_ASYNC_LOAD_CONTEXT_STATE_BYTES,
               "OBMM COROUTINE_SCHEDULER context ABI size mismatch");
_Static_assert(offsetof(struct obmm_async_load_context_v2, x) == 16,
               "OBMM COROUTINE_SCHEDULER x-register offset mismatch");
_Static_assert(offsetof(struct obmm_async_load_context_v2, sp) == 264,
               "OBMM COROUTINE_SCHEDULER SP offset mismatch");
_Static_assert(offsetof(struct obmm_async_load_context_v2, pc) == 272,
               "OBMM COROUTINE_SCHEDULER PC offset mismatch");
_Static_assert(offsetof(struct obmm_async_load_context_v2, q) == 288,
               "OBMM COROUTINE_SCHEDULER SIMD offset mismatch");
_Static_assert(offsetof(struct obmm_async_load_context_v2, fpcr) == 800,
               "OBMM COROUTINE_SCHEDULER FPCR offset mismatch");
_Static_assert(offsetof(struct obmm_coroutine_scheduler_context_local, context) % 16 == 0,
               "OBMM COROUTINE_SCHEDULER local context must be 16-byte aligned");
_Static_assert(sizeof(struct obmm_async_load_event_producer_v3) ==
               OBMM_ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES,
               "OBMM COROUTINE_SCHEDULER producer ABI size mismatch");
_Static_assert(sizeof(struct obmm_async_load_event_consumer_v3) ==
               OBMM_ASYNC_LOAD_EVENT_CONSUMER_BYTES,
               "OBMM COROUTINE_SCHEDULER consumer ABI size mismatch");
_Static_assert(sizeof(struct obmm_async_load_event_v3) ==
               OBMM_ASYNC_LOAD_EVENT_SLOT_BYTES,
               "OBMM COROUTINE_SCHEDULER event ABI size mismatch");

static int obmm_coroutine_scheduler_neg_errno(void)
{
    return errno ? -errno : -EIO;
}

static int obmm_coroutine_scheduler_ioctl(
    struct obmm_coroutine_scheduler *runtime, unsigned long request,
    void *argument)
{
    if (runtime->hot_path_active) {
        runtime->metrics.kernel_hotpath_ioctls++;
    }
    return ioctl(runtime->fd, request, argument);
}

static uint64_t obmm_coroutine_scheduler_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static void obmm_coroutine_scheduler_trace(struct obmm_coroutine_scheduler *runtime,
                           enum obmm_coroutine_scheduler_trace_kind kind,
                           const struct obmm_async_load_event_v3 *event,
                           uint64_t previous_context_id,
                           uint64_t context_id)
{
    struct obmm_coroutine_scheduler_trace_event trace_event = {
        .kind = kind,
        .context_id = context_id,
        .previous_context_id = previous_context_id,
    };

    if (!runtime->trace) {
        return;
    }
    if (event) {
        trace_event.status = event->status;
        trace_event.access_bytes = event->access_bytes;
        trace_event.rt = event->rt;
        trace_event.context_id = event->context_id;
        trace_event.sequence = event->sequence;
        trace_event.token = event->plt_token;
        trace_event.pc = event->fault_pc;
        trace_event.value = event->value;
    }
    runtime->trace(runtime->trace_opaque, &trace_event);
}

static uint64_t obmm_coroutine_scheduler_read_tpidr_el0(void)
{
#if defined(__aarch64__)
    uint64_t value;

    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

static uint64_t obmm_coroutine_scheduler_read_nzcv(void)
{
#if defined(__aarch64__)
    uint64_t value;

    __asm__ volatile("mrs %0, nzcv" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

static uint64_t obmm_coroutine_scheduler_read_fpcr(void)
{
#if defined(__aarch64__)
    uint64_t value;

    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

static uint64_t obmm_coroutine_scheduler_read_fpsr(void)
{
#if defined(__aarch64__)
    uint64_t value;

    __asm__ volatile("mrs %0, fpsr" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

static int obmm_coroutine_scheduler_stack_allocate(size_t requested_bytes,
                                   void **mapping,
                                   size_t *mapping_bytes,
                                   uintptr_t *stack_top)
{
    size_t page_bytes;
    size_t usable_bytes;
    size_t total_bytes;
    void *address;
    long page_size = sysconf(_SC_PAGESIZE);

    if (page_size <= 0 || !mapping || !mapping_bytes || !stack_top) {
        return -EINVAL;
    }
    page_bytes = page_size;
    if (requested_bytes > SIZE_MAX - page_bytes + 1) {
        return -EINVAL;
    }
    usable_bytes = (requested_bytes + page_bytes - 1) & ~(page_bytes - 1);
    if (usable_bytes < page_bytes ||
        usable_bytes > SIZE_MAX - page_bytes * 2) {
        return -EINVAL;
    }
    total_bytes = usable_bytes + page_bytes * 2;
    address = mmap(NULL, total_bytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED) {
        return obmm_coroutine_scheduler_neg_errno();
    }
    if (mprotect((char *)address + page_bytes, usable_bytes,
                 PROT_READ | PROT_WRITE) != 0) {
        int error = obmm_coroutine_scheduler_neg_errno();

        munmap(address, total_bytes);
        return error;
    }
    *mapping = address;
    *mapping_bytes = total_bytes;
    *stack_top = ((uintptr_t)address + page_bytes + usable_bytes) & ~15ULL;
    return 0;
}

static int obmm_coroutine_scheduler_event_ring_map(
    struct obmm_coroutine_scheduler *runtime)
{
    const struct obmm_async_load_caps_v3 *caps = &runtime->caps;
    long page_bytes = sysconf(_SC_PAGESIZE);
    uint64_t minimum_ring_bytes =
        OBMM_ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES +
        (uint64_t)caps->event_queue_depth * OBMM_ASYNC_LOAD_EVENT_SLOT_BYTES;

    if (page_bytes <= 0 ||
        caps->event_slot_bytes != OBMM_ASYNC_LOAD_EVENT_SLOT_BYTES ||
        caps->event_producer_header_bytes !=
            OBMM_ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES ||
        caps->event_ring_mmap_bytes < minimum_ring_bytes ||
        !caps->event_consumer_mmap_bytes ||
        caps->event_consumer_mmap_bytes <
            OBMM_ASYNC_LOAD_EVENT_CONSUMER_BYTES ||
        caps->event_ring_mmap_offset % (uint64_t)page_bytes ||
        caps->event_consumer_mmap_offset % (uint64_t)page_bytes) {
        return -EPROTO;
    }
    runtime->event_ring_mapping = mmap(
        NULL, caps->event_ring_mmap_bytes, PROT_READ, MAP_SHARED,
        runtime->fd, caps->event_ring_mmap_offset);
    if (runtime->event_ring_mapping == MAP_FAILED) {
        runtime->event_ring_mapping = NULL;
        return obmm_coroutine_scheduler_neg_errno();
    }
    runtime->event_ring_mapping_bytes = caps->event_ring_mmap_bytes;
    runtime->event_consumer_mapping = mmap(
        NULL, caps->event_consumer_mmap_bytes, PROT_READ | PROT_WRITE,
        MAP_SHARED, runtime->fd, caps->event_consumer_mmap_offset);
    if (runtime->event_consumer_mapping == MAP_FAILED) {
        int error = obmm_coroutine_scheduler_neg_errno();

        runtime->event_consumer_mapping = NULL;
        munmap(runtime->event_ring_mapping,
               runtime->event_ring_mapping_bytes);
        runtime->event_ring_mapping = NULL;
        runtime->event_ring_mapping_bytes = 0;
        return error;
    }
    runtime->event_consumer_mapping_bytes =
        caps->event_consumer_mmap_bytes;
    runtime->event_producer = runtime->event_ring_mapping;
    runtime->event_slots = (const struct obmm_async_load_event_v3 *)
        ((const unsigned char *)runtime->event_ring_mapping +
         OBMM_ASYNC_LOAD_EVENT_PRODUCER_HEADER_BYTES);
    runtime->event_consumer = runtime->event_consumer_mapping;
    return 0;
}

int obmm_coroutine_scheduler_open(struct obmm_coroutine_scheduler **runtime,
                  const struct obmm_coroutine_scheduler_options *options)
{
    const char *device = OBMM_COROUTINE_SCHEDULER_DEFAULT_DEVICE;
    struct obmm_coroutine_scheduler *coroutine_scheduler;
    int ret;

    if (!runtime) {
        return -EINVAL;
    }
    if (options && options->device_path) {
        device = options->device_path;
    }
    coroutine_scheduler = calloc(1, sizeof(*coroutine_scheduler));
    if (!coroutine_scheduler) {
        return -ENOMEM;
    }
    coroutine_scheduler->fd = open(device, O_RDWR | O_CLOEXEC);
    if (coroutine_scheduler->fd < 0) {
        ret = obmm_coroutine_scheduler_neg_errno();
        free(coroutine_scheduler);
        return ret;
    }
    if (options) {
        coroutine_scheduler->load_timeout_ns = options->load_timeout_ns;
        coroutine_scheduler->trace = options->trace;
        coroutine_scheduler->trace_opaque = options->trace_opaque;
        if (options->completion_mode != OBMM_COROUTINE_SCHEDULER_COMPLETION_PATCH &&
            options->completion_mode != OBMM_COROUTINE_SCHEDULER_COMPLETION_REPLAY) {
            close(coroutine_scheduler->fd);
            free(coroutine_scheduler);
            return -EINVAL;
        }
        coroutine_scheduler->replay_retire =
            options->completion_mode == OBMM_COROUTINE_SCHEDULER_COMPLETION_REPLAY;
    }
    if (obmm_coroutine_scheduler_ioctl(
            coroutine_scheduler, OBMM_ASYNC_LOAD_IOCTL_QUERY_CAPS,
            &coroutine_scheduler->caps) != 0) {
        ret = obmm_coroutine_scheduler_neg_errno();
        close(coroutine_scheduler->fd);
        free(coroutine_scheduler);
        return ret;
    }
    if (coroutine_scheduler->caps.abi_version != OBMM_ASYNC_LOAD_ABI_VERSION ||
        !coroutine_scheduler->caps.context_entries ||
        coroutine_scheduler->caps.context_entries > OBMM_ASYNC_LOAD_MAX_CONTEXTS ||
        !coroutine_scheduler->caps.pending_load_entries ||
        coroutine_scheduler->caps.pending_load_entries > OBMM_ASYNC_LOAD_MAX_PENDING_LOADS ||
        !coroutine_scheduler->caps.event_queue_depth ||
        coroutine_scheduler->caps.event_queue_depth > OBMM_ASYNC_LOAD_MAX_EVENTS ||
        coroutine_scheduler->caps.context_state_bytes != OBMM_ASYNC_LOAD_CONTEXT_STATE_BYTES ||
        coroutine_scheduler->caps.resume_hlt_imm != OBMM_ASYNC_LOAD_RESUME_HLT_IMM ||
        coroutine_scheduler->caps.wait_hlt_imm != OBMM_ASYNC_LOAD_WAIT_HLT_IMM ||
        coroutine_scheduler->caps.scheduler_enter_hlt_imm !=
            OBMM_ASYNC_LOAD_SCHEDULER_ENTER_HLT_IMM ||
        (coroutine_scheduler->caps.capabilities &
         (OBMM_ASYNC_LOAD_CAP_DIRECT_EL0_UPCALL | OBMM_ASYNC_LOAD_CAP_EL0_RESUME |
          OBMM_ASYNC_LOAD_CAP_FULL_CONTEXT |
          OBMM_ASYNC_LOAD_CAP_KERNEL_FREE_EVENT_RING |
          OBMM_ASYNC_LOAD_CAP_EL0_WAIT_WAKE |
          OBMM_ASYNC_LOAD_CAP_EL0_SCHEDULER_ENTER)) !=
         (OBMM_ASYNC_LOAD_CAP_DIRECT_EL0_UPCALL | OBMM_ASYNC_LOAD_CAP_EL0_RESUME |
          OBMM_ASYNC_LOAD_CAP_FULL_CONTEXT |
          OBMM_ASYNC_LOAD_CAP_KERNEL_FREE_EVENT_RING |
          OBMM_ASYNC_LOAD_CAP_EL0_WAIT_WAKE |
          OBMM_ASYNC_LOAD_CAP_EL0_SCHEDULER_ENTER) ||
        !coroutine_scheduler->caps.clock_mhz) {
        close(coroutine_scheduler->fd);
        free(coroutine_scheduler);
        return -EPROTO;
    }
    if (coroutine_scheduler->replay_retire &&
        !(coroutine_scheduler->caps.capabilities & OBMM_ASYNC_LOAD_CAP_REPLAY_RETIRE)) {
        close(coroutine_scheduler->fd);
        free(coroutine_scheduler);
        return -EOPNOTSUPP;
    }
    ret = obmm_coroutine_scheduler_event_ring_map(coroutine_scheduler);
    if (ret) {
        close(coroutine_scheduler->fd);
        free(coroutine_scheduler);
        return ret;
    }
    ret = obmm_coroutine_scheduler_stack_allocate(
        OBMM_COROUTINE_SCHEDULER_SCHEDULER_STACK_BYTES, &coroutine_scheduler->scheduler_mapping,
        &coroutine_scheduler->scheduler_mapping_bytes, &coroutine_scheduler->scheduler_stack_top);
    if (ret) {
        munmap(coroutine_scheduler->event_consumer_mapping,
               coroutine_scheduler->event_consumer_mapping_bytes);
        munmap(coroutine_scheduler->event_ring_mapping,
               coroutine_scheduler->event_ring_mapping_bytes);
        close(coroutine_scheduler->fd);
        free(coroutine_scheduler);
        return ret;
    }
    coroutine_scheduler->metrics.clock_mhz = coroutine_scheduler->caps.clock_mhz;
    coroutine_scheduler->scheduler_cursor = coroutine_scheduler->caps.context_entries - 1;
    *runtime = coroutine_scheduler;
    return 0;
}

static void obmm_coroutine_scheduler_release_local_resources(struct obmm_coroutine_scheduler *runtime)
{
    uint32_t index;

    for (index = 0; index < OBMM_ASYNC_LOAD_MAX_CONTEXTS; index++) {
        struct obmm_coroutine_scheduler_context_local *context =
            &runtime->contexts[index];

        if (context->state == OBMM_COROUTINE_SCHEDULER_CONTEXT_FREE) {
            continue;
        }
        munmap(context->mapping, context->mapping_bytes);
        memset(context, 0, sizeof(*context));
    }
    if (runtime->scheduler_mapping) {
        munmap(runtime->scheduler_mapping,
               runtime->scheduler_mapping_bytes);
    }
    if (runtime->event_consumer_mapping) {
        munmap(runtime->event_consumer_mapping,
               runtime->event_consumer_mapping_bytes);
    }
    if (runtime->event_ring_mapping) {
        munmap(runtime->event_ring_mapping,
               runtime->event_ring_mapping_bytes);
    }
    memset(runtime->maps, 0, sizeof(runtime->maps));
}

void obmm_coroutine_scheduler_close(struct obmm_coroutine_scheduler *runtime)
{
    if (!runtime) {
        return;
    }
    if (runtime->started) {
        obmm_coroutine_scheduler_stop(runtime);
    }
    obmm_coroutine_scheduler_release_local_resources(runtime);
    close(runtime->fd);
    if (active_runtime == runtime) {
        active_runtime = NULL;
    }
    free(runtime);
}

int obmm_coroutine_scheduler_get_caps(const struct obmm_coroutine_scheduler *runtime,
                      struct obmm_async_load_caps_v3 *caps)
{
    if (!runtime || !caps) {
        return -EINVAL;
    }
    *caps = runtime->caps;
    return 0;
}

int obmm_coroutine_scheduler_register_map(struct obmm_coroutine_scheduler *runtime, int mapping_fd,
                          uint64_t mem_id, void *gsva_base,
                          size_t length, uint32_t flags,
                          struct obmm_coroutine_scheduler_map *map)
{
    return obmm_coroutine_scheduler_register_map_for_phase(
        runtime, mapping_fd, mem_id, gsva_base, length, flags, 1, map);
}

int obmm_coroutine_scheduler_register_map_for_phase(
    struct obmm_coroutine_scheduler *runtime, int mapping_fd, uint64_t mem_id,
    void *gsva_base, size_t length, uint32_t flags,
    uint64_t model_phase_generation, struct obmm_coroutine_scheduler_map *map)
{
    struct obmm_async_load_map_register_v1 request = {
        .mem_id = mem_id,
        .gsva_base = (uintptr_t)gsva_base,
        .mapped_addr = (uintptr_t)gsva_base,
        .length = length,
        .flags = flags,
        .mapping_fd = mapping_fd,
        .model_phase_generation = model_phase_generation,
    };
    uint32_t index;

    if (!runtime || mapping_fd < 0 || !mem_id || !gsva_base || !length ||
        !map || !model_phase_generation || runtime->started ||
        runtime->device_reset) {
        return -EINVAL;
    }
    if (obmm_coroutine_scheduler_ioctl(
            runtime, OBMM_ASYNC_LOAD_IOCTL_REGISTER_MAP, &request) != 0) {
        return obmm_coroutine_scheduler_neg_errno();
    }
    for (index = 0; index < runtime->caps.pending_load_entries; index++) {
        if (!runtime->maps[index].allocated) {
            break;
        }
    }
    if (index == runtime->caps.pending_load_entries) {
        struct obmm_async_load_map_unregister_v1 rollback = {
            .policy_id = request.policy_id,
            .map_generation = request.map_generation,
        };

        obmm_coroutine_scheduler_ioctl(
            runtime, OBMM_ASYNC_LOAD_IOCTL_UNREGISTER_MAP, &rollback);
        return -ENOSPC;
    }
    *map = (struct obmm_coroutine_scheduler_map) {
        .policy_id = request.policy_id,
        .generation = request.map_generation,
        .length = request.length,
    };
    runtime->maps[index].allocated = true;
    runtime->maps[index].map = *map;
    return 0;
}

int obmm_coroutine_scheduler_unregister_map(struct obmm_coroutine_scheduler *runtime,
                            struct obmm_coroutine_scheduler_map *map)
{
    struct obmm_async_load_map_unregister_v1 request;
    uint32_t index;

    if (!runtime || !map || !map->policy_id || runtime->started) {
        return -EINVAL;
    }
    for (index = 0; index < OBMM_ASYNC_LOAD_MAX_PENDING_LOADS; index++) {
        if (runtime->maps[index].allocated &&
            runtime->maps[index].map.policy_id == map->policy_id &&
            runtime->maps[index].map.generation == map->generation) {
            break;
        }
    }
    if (index == OBMM_ASYNC_LOAD_MAX_PENDING_LOADS) {
        return -ESTALE;
    }
    if (!runtime->device_reset) {
        request = (struct obmm_async_load_map_unregister_v1) {
            .policy_id = map->policy_id,
            .map_generation = map->generation,
        };
        if (obmm_coroutine_scheduler_ioctl(
                runtime, OBMM_ASYNC_LOAD_IOCTL_UNREGISTER_MAP,
                &request) != 0) {
            return obmm_coroutine_scheduler_neg_errno();
        }
    }
    memset(&runtime->maps[index], 0, sizeof(runtime->maps[index]));
    memset(map, 0, sizeof(*map));
    return 0;
}

int obmm_coroutine_scheduler_context_create(struct obmm_coroutine_scheduler *runtime,
                            obmm_coroutine_scheduler_entry_fn entry, void *arg,
                            size_t stack_bytes, uint32_t flags,
                            uint64_t *context_id)
{
    struct obmm_coroutine_scheduler_context_local *local;
    uintptr_t stack_top;
    uint32_t index;
    int home_cpu;
    int ret;

    if (!runtime || !entry || !context_id || runtime->started ||
        runtime->device_reset || flags) {
        return -EINVAL;
    }
    for (index = 0; index < runtime->caps.context_entries; index++) {
        if (runtime->contexts[index].state == OBMM_COROUTINE_SCHEDULER_CONTEXT_FREE) {
            break;
        }
    }
    if (index == runtime->caps.context_entries) {
        return -ENOSPC;
    }
    home_cpu = sched_getcpu();
    if (home_cpu < 0 || home_cpu > UINT16_MAX) {
        return -EINVAL;
    }
    local = &runtime->contexts[index];
    ret = obmm_coroutine_scheduler_stack_allocate(stack_bytes, &local->mapping,
                                  &local->mapping_bytes, &stack_top);
    if (ret) {
        return ret;
    }
    local->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_READY;
    local->slot = index;
    local->entry = entry;
    local->argument = arg;
    local->context.context_id =
        (runtime->caps.owner_generation << 32) |
        ((uint64_t)(uint16_t)home_cpu << 16) | index;
    local->context.x[19] = (uintptr_t)local;
    local->context.sp = stack_top;
    local->context.pc = (uintptr_t)obmm_coroutine_scheduler_context_bootstrap;
    local->context.nzcv = obmm_coroutine_scheduler_read_nzcv();
    local->context.fpcr = obmm_coroutine_scheduler_read_fpcr();
    local->context.fpsr = obmm_coroutine_scheduler_read_fpsr();
    local->context.tpidr_el0 = obmm_coroutine_scheduler_read_tpidr_el0();
    if (runtime->logical_contexts <= index) {
        runtime->logical_contexts = index + 1;
    }
    *context_id = local->context.context_id;
    return 0;
}

int obmm_coroutine_scheduler_context_destroy(struct obmm_coroutine_scheduler *runtime,
                             uint64_t context_id)
{
    uint32_t index;

    if (!runtime || !context_id || runtime->started) {
        return -EINVAL;
    }
    for (index = 0; index < runtime->caps.context_entries; index++) {
        struct obmm_coroutine_scheduler_context_local *local = &runtime->contexts[index];

        if (local->state == OBMM_COROUTINE_SCHEDULER_CONTEXT_FREE ||
            local->context.context_id != context_id) {
            continue;
        }
        munmap(local->mapping, local->mapping_bytes);
        memset(local, 0, sizeof(*local));
        while (runtime->logical_contexts &&
               runtime->contexts[runtime->logical_contexts - 1].state ==
                   OBMM_COROUTINE_SCHEDULER_CONTEXT_FREE) {
            runtime->logical_contexts--;
        }
        return 0;
    }
    return -ESTALE;
}

static struct obmm_coroutine_scheduler_context_local *obmm_coroutine_scheduler_find_context(
    struct obmm_coroutine_scheduler *runtime, uint64_t context_id)
{
    uint16_t slot = context_id & 0xffff;
    struct obmm_coroutine_scheduler_context_local *local;

    if (!context_id || slot >= runtime->logical_contexts) {
        return NULL;
    }
    local = &runtime->contexts[slot];
    return local->state != OBMM_COROUTINE_SCHEDULER_CONTEXT_FREE &&
        local->context.context_id == context_id ? local : NULL;
}

static uint64_t obmm_coroutine_scheduler_ready_count(const struct obmm_coroutine_scheduler *runtime)
{
    uint64_t count = 0;
    uint16_t index;

    for (index = 0; index < runtime->logical_contexts; index++) {
        if (runtime->contexts[index].state == OBMM_COROUTINE_SCHEDULER_CONTEXT_READY ||
            runtime->contexts[index].state ==
                OBMM_COROUTINE_SCHEDULER_CONTEXT_READY_REPLAY) {
            count++;
        }
    }
    return count;
}

static bool obmm_coroutine_scheduler_has_waiting(const struct obmm_coroutine_scheduler *runtime)
{
    uint16_t index;

    for (index = 0; index < runtime->logical_contexts; index++) {
        if (runtime->contexts[index].state ==
            OBMM_COROUTINE_SCHEDULER_CONTEXT_WAIT_REMOTE) {
            return true;
        }
    }
    return false;
}

static struct obmm_coroutine_scheduler_context_local *obmm_coroutine_scheduler_choose_ready(
    struct obmm_coroutine_scheduler *runtime)
{
    uint16_t ordinal;

    for (ordinal = 1; ordinal <= runtime->logical_contexts; ordinal++) {
        uint16_t slot = (runtime->scheduler_cursor + ordinal) %
            runtime->logical_contexts;

        if (runtime->contexts[slot].state == OBMM_COROUTINE_SCHEDULER_CONTEXT_READY ||
            runtime->contexts[slot].state ==
                OBMM_COROUTINE_SCHEDULER_CONTEXT_READY_REPLAY) {
            runtime->scheduler_cursor = slot;
            return &runtime->contexts[slot];
        }
    }
    return NULL;
}

static int obmm_coroutine_scheduler_status_error(uint32_t status)
{
    switch (status) {
    case OBMM_ASYNC_LOAD_STATUS_TIMEOUT:
        return -ETIMEDOUT;
    case OBMM_ASYNC_LOAD_STATUS_PERMISSION:
        return -EACCES;
    case OBMM_ASYNC_LOAD_STATUS_STALE_MAP:
        return -ESTALE;
    case OBMM_ASYNC_LOAD_STATUS_CANCELLED:
        return -ECANCELED;
    case OBMM_ASYNC_LOAD_STATUS_REMOTE_IO:
    case OBMM_ASYNC_LOAD_STATUS_INTERNAL:
    default:
        return -EIO;
    }
}

static int obmm_coroutine_scheduler_protocol_error(
    const struct obmm_coroutine_scheduler *runtime,
    const struct obmm_async_load_event_v3 *event,
    const struct obmm_coroutine_scheduler_context_local *target,
    const char *reason)
{
    fprintf(stderr,
            "OBMM_COROUTINE_SCHEDULER_PROTOCOL_ERROR schema=1 reason=%s kind=%u "
            "event_context=%" PRIu64 " current_context=%" PRIu64 " "
            "target_state=%u event_token=%" PRIu64 " "
            "target_token=%" PRIu64 " event_pc=%" PRIu64 " "
            "target_pc=%" PRIu64 "\n",
            reason, event ? (unsigned int)event->kind : 0,
            event ? (uint64_t)event->context_id : 0,
            runtime && runtime->current ?
                (uint64_t)runtime->current->context.context_id : 0,
            target ? (unsigned int)target->state :
                (unsigned int)OBMM_COROUTINE_SCHEDULER_CONTEXT_FREE,
            event ? (uint64_t)event->plt_token : 0,
            target ? (uint64_t)target->waiting_token : 0,
            event ? (uint64_t)event->fault_pc : 0,
            target ? (uint64_t)target->context.pc : 0);
    return -EPROTO;
}

static int obmm_coroutine_scheduler_process_event(struct obmm_coroutine_scheduler *runtime,
                                  const struct obmm_async_load_event_v3 *event)
{
    struct obmm_coroutine_scheduler_context_local *target =
        obmm_coroutine_scheduler_find_context(runtime, event->context_id);

    if (!target || !event->sequence ||
        event->owner_generation != runtime->caps.owner_generation ||
        !event->plt_token || !event->map_id || !event->map_generation ||
        !event->model_phase_generation ||
        event->flags & ~OBMM_ASYNC_LOAD_EVENT_RETIRE_REPLAY ||
        event->rt > 31 ||
        (event->access_bytes != 1 && event->access_bytes != 2 &&
         event->access_bytes != 4 && event->access_bytes != 8)) {
        return obmm_coroutine_scheduler_protocol_error(
            runtime, event, target, "event-envelope");
    }
    switch (event->kind) {
    case OBMM_ASYNC_LOAD_EVENT_PENDING:
        runtime->metrics.el0_pending_upcalls++;
        if (event->status != OBMM_ASYNC_LOAD_STATUS_SUCCESS ||
            target != runtime->current ||
            target->state != OBMM_COROUTINE_SCHEDULER_CONTEXT_READY ||
            event->fault_pc != target->context.pc) {
            return obmm_coroutine_scheduler_protocol_error(
                runtime, event, target, "pending-state");
        }
        target->waiting_token = event->plt_token;
        target->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_WAIT_REMOTE;
        obmm_coroutine_scheduler_trace(runtime, OBMM_COROUTINE_SCHEDULER_TRACE_UPCALL_PENDING,
                       event, 0, event->context_id);
        return 0;
    case OBMM_ASYNC_LOAD_EVENT_COMPLETE:
        runtime->metrics.el0_complete_upcalls++;
        if (event->status != OBMM_ASYNC_LOAD_STATUS_SUCCESS ||
            target->state != OBMM_COROUTINE_SCHEDULER_CONTEXT_WAIT_REMOTE ||
            target->waiting_token != event->plt_token ||
            event->fault_pc != target->context.pc) {
            return obmm_coroutine_scheduler_protocol_error(
                runtime, event, target, "complete-state");
        }
        if (!!(event->flags & OBMM_ASYNC_LOAD_EVENT_RETIRE_REPLAY) !=
            runtime->replay_retire) {
            return obmm_coroutine_scheduler_protocol_error(
                runtime, event, target, "complete-retire-mode");
        }
        target->waiting_token = 0;
        if (runtime->replay_retire) {
            target->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_READY_REPLAY;
        } else {
            if (event->rt < 31) {
                target->context.x[event->rt] = event->value;
            }
            target->context.pc = event->fault_pc + 4;
            target->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_READY;
        }
        obmm_coroutine_scheduler_trace(runtime, OBMM_COROUTINE_SCHEDULER_TRACE_UPCALL_COMPLETE,
                       event, 0, event->context_id);
        return 0;
    case OBMM_ASYNC_LOAD_EVENT_FAULT:
        runtime->metrics.el0_fault_upcalls++;
        if (event->status == OBMM_ASYNC_LOAD_STATUS_TIMEOUT) {
            runtime->metrics.el0_timeout_faults++;
        }
        if (target->state != OBMM_COROUTINE_SCHEDULER_CONTEXT_WAIT_REMOTE ||
            target->waiting_token != event->plt_token ||
            event->status == OBMM_ASYNC_LOAD_STATUS_SUCCESS) {
            return obmm_coroutine_scheduler_protocol_error(
                runtime, event, target, "fault-state");
        }
        target->waiting_token = 0;
        target->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_FAULTED;
        obmm_coroutine_scheduler_trace(runtime, OBMM_COROUTINE_SCHEDULER_TRACE_UPCALL_FAULT,
                       event, 0, event->context_id);
        return obmm_coroutine_scheduler_status_error(event->status);
    case OBMM_ASYNC_LOAD_EVENT_OWNER_STOP:
        target->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_FAULTED;
        return -ECANCELED;
    default:
        return -EPROTO;
    }
}

static int obmm_coroutine_scheduler_event_ring_validate(
    const struct obmm_coroutine_scheduler *runtime)
{
    const struct obmm_async_load_event_producer_v3 *producer =
        runtime->event_producer;
    const struct obmm_async_load_event_consumer_v3 *consumer =
        runtime->event_consumer;

    if (!producer || !consumer ||
        producer->abi_version != OBMM_ASYNC_LOAD_ABI_VERSION ||
        producer->event_depth != runtime->caps.event_queue_depth ||
        producer->event_slot_bytes != OBMM_ASYNC_LOAD_EVENT_SLOT_BYTES ||
        producer->owner_generation != runtime->caps.owner_generation ||
        consumer->abi_version != OBMM_ASYNC_LOAD_ABI_VERSION ||
        consumer->flags ||
        consumer->owner_generation != runtime->caps.owner_generation ||
        __atomic_load_n(&producer->producer_sequence, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&consumer->consumer_sequence, __ATOMIC_ACQUIRE)) {
        return -EPROTO;
    }
    return 0;
}

static int obmm_coroutine_scheduler_event_ring_next(
    struct obmm_coroutine_scheduler *runtime,
    struct obmm_async_load_event_v3 *event)
{
    uint64_t producer_sequence = __atomic_load_n(
        &runtime->event_producer->producer_sequence, __ATOMIC_ACQUIRE);
    uint64_t consumer_sequence = __atomic_load_n(
        &runtime->event_consumer->consumer_sequence, __ATOMIC_RELAXED);
    uint64_t sequence;
    uint64_t slot;

    if (producer_sequence < consumer_sequence ||
        producer_sequence - consumer_sequence >
            runtime->caps.event_queue_depth) {
        return -EPROTO;
    }
    if (producer_sequence == consumer_sequence) {
        return 0;
    }
    sequence = consumer_sequence + 1;
    slot = (sequence - 1) % runtime->caps.event_queue_depth;
    memcpy(event, &runtime->event_slots[slot], sizeof(*event));
    if (event->sequence != sequence ||
        event->owner_generation != runtime->caps.owner_generation) {
        return -EPROTO;
    }
    return 1;
}

static void obmm_coroutine_scheduler_event_ring_ack(
    struct obmm_coroutine_scheduler *runtime, uint64_t sequence)
{
    __atomic_store_n(&runtime->event_consumer->consumer_sequence,
                     sequence, __ATOMIC_RELEASE);
    runtime->metrics.el0_event_ring_consumed++;
}

static int obmm_coroutine_scheduler_event_ring_handle(
    struct obmm_coroutine_scheduler *runtime,
    const struct obmm_async_load_event_v3 *event)
{
    int ret = obmm_coroutine_scheduler_process_event(runtime, event);

    obmm_coroutine_scheduler_event_ring_ack(runtime, event->sequence);
    return ret;
}

static int obmm_coroutine_scheduler_event_ring_drain(
    struct obmm_coroutine_scheduler *runtime)
{
    for (;;) {
        struct obmm_async_load_event_v3 event;
        int ret = obmm_coroutine_scheduler_event_ring_next(
            runtime, &event);

        if (ret <= 0) {
            return ret;
        }
        if (event.interrupted_pc) {
            return -EPROTO;
        }
        ret = obmm_coroutine_scheduler_event_ring_handle(runtime, &event);
        if (ret) {
            return ret;
        }
    }
}

static void obmm_coroutine_scheduler_record_error(
    struct obmm_coroutine_scheduler *runtime, int error,
    enum obmm_coroutine_scheduler_error_stage stage)
{
    if (error && !runtime->first_error) {
        runtime->first_error = error;
        runtime->metrics.first_error = error;
        runtime->metrics.first_error_stage = stage;
    }
}

static void obmm_coroutine_scheduler_log_context_states(const struct obmm_coroutine_scheduler *runtime)
{
    uint16_t index;

    for (index = 0; index < runtime->logical_contexts; index++) {
        const struct obmm_coroutine_scheduler_context_local *context =
            &runtime->contexts[index];

        fprintf(stderr,
                "OBMM_COROUTINE_SCHEDULER_CONTEXT_STATE schema=1 slot=%u state=%u "
                "context=%" PRIu64 " waiting_token=%" PRIu64 " "
                "pc=%" PRIu64 " first_error=%d stage=%u\n",
                index, (unsigned int)context->state,
                (uint64_t)context->context.context_id,
                (uint64_t)context->waiting_token,
                (uint64_t)context->context.pc,
                runtime->first_error,
                runtime->metrics.first_error_stage);
    }
}

static __attribute__((noreturn)) void obmm_coroutine_scheduler_schedule(
    struct obmm_coroutine_scheduler *runtime, uint64_t scheduler_started_ns)
{
    for (;;) {
        if (runtime->first_error) {
            obmm_coroutine_scheduler_log_context_states(runtime);
            break;
        }
        struct obmm_coroutine_scheduler_context_local *next =
            obmm_coroutine_scheduler_choose_ready(runtime);
        uint64_t ready_count = obmm_coroutine_scheduler_ready_count(runtime);

        if (runtime->metrics.el0_ready_high_water < ready_count) {
            runtime->metrics.el0_ready_high_water = ready_count;
        }
        if (next) {
            uint64_t now_ns = obmm_coroutine_scheduler_now_ns();
            uint64_t previous_context_id = runtime->last_resumed_id;

            if (runtime->last_resumed_id &&
                runtime->last_resumed_id != next->context.context_id) {
                runtime->metrics.el0_context_switches++;
            }
            runtime->metrics.el0_context_restores++;
            runtime->metrics.el0_context_bytes +=
                OBMM_ASYNC_LOAD_CONTEXT_STATE_BYTES;
            if (now_ns >= scheduler_started_ns) {
                runtime->metrics.el0_scheduler_ns +=
                    now_ns - scheduler_started_ns;
            }
            runtime->last_resumed_id = next->context.context_id;
            runtime->current = next;
            next->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_RUNNING;
            obmm_coroutine_scheduler_trace(runtime, OBMM_COROUTINE_SCHEDULER_TRACE_CONTEXT_RESUME,
                           NULL, previous_context_id,
                           next->context.context_id);
            obmm_coroutine_scheduler_context_resume(&next->context);
        }
        if (obmm_coroutine_scheduler_has_waiting(runtime)) {
            int ret;

            runtime->metrics.el0_no_ready_waits++;
            runtime->metrics.el0_wait_assists++;
            __atomic_add_fetch(&runtime->event_consumer->wait_count, 1,
                               __ATOMIC_RELEASE);
            obmm_coroutine_scheduler_wait();
            ret = obmm_coroutine_scheduler_event_ring_drain(runtime);
            obmm_coroutine_scheduler_record_error(
                runtime, ret,
                OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_EVENT_RING_DRAIN);
            continue;
        }
        break;
    }
    {
        uint64_t now_ns = obmm_coroutine_scheduler_now_ns();

        if (now_ns >= scheduler_started_ns) {
            runtime->metrics.el0_scheduler_ns +=
                now_ns - scheduler_started_ns;
        }
    }
    siglongjmp(runtime->return_environment, 1);
    __builtin_unreachable();
}

uintptr_t obmm_coroutine_scheduler_scheduler_stack_top(void)
{
    return active_runtime ? active_runtime->scheduler_stack_top : 0;
}

void obmm_coroutine_scheduler_upcall_dispatch(struct obmm_async_load_context_v2 *frame)
{
    struct obmm_coroutine_scheduler *runtime = active_runtime;
    struct obmm_coroutine_scheduler_context_local *interrupted;
    struct obmm_async_load_event_v3 event = { 0 };
    bool interrupted_was_running;
    uint64_t context_id;
    uint64_t context_flags;
    uint64_t started_ns = obmm_coroutine_scheduler_now_ns();
    int ret;

    if (!runtime || !runtime->started || !frame || !runtime->current ||
        (runtime->current->state != OBMM_COROUTINE_SCHEDULER_CONTEXT_RUNNING &&
         runtime->current->state != OBMM_COROUTINE_SCHEDULER_CONTEXT_DONE)) {
        _exit(127);
    }
    interrupted = runtime->current;
    interrupted_was_running =
        interrupted->state == OBMM_COROUTINE_SCHEDULER_CONTEXT_RUNNING;
    runtime->metrics.el0_context_saves++;
    runtime->metrics.el0_context_bytes += OBMM_ASYNC_LOAD_CONTEXT_STATE_BYTES;
    if (interrupted_was_running) {
        context_id = interrupted->context.context_id;
        context_flags = interrupted->context.flags;
        memcpy(&interrupted->context, frame, sizeof(*frame));
        interrupted->context.context_id = context_id;
        interrupted->context.flags = context_flags;
        interrupted->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_READY;
    }
    ret = obmm_coroutine_scheduler_event_ring_next(runtime, &event);
    if (ret != 1) {
        obmm_coroutine_scheduler_record_error(
            runtime, ret < 0 ? ret : -EAGAIN,
            OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_EVENT_RING_DRAIN);
        interrupted->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_FAULTED;
        obmm_coroutine_scheduler_schedule(runtime, started_ns);
    }
    if (!event.interrupted_pc) {
        obmm_coroutine_scheduler_record_error(
            runtime, -EPROTO, OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_EVENT_VALIDATE);
        interrupted->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_FAULTED;
        obmm_coroutine_scheduler_schedule(runtime, started_ns);
    }
    if (interrupted_was_running) {
        interrupted->context.pc = event.interrupted_pc;
    }
    ret = obmm_coroutine_scheduler_event_ring_handle(runtime, &event);
    obmm_coroutine_scheduler_record_error(
        runtime, ret, OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_EVENT_HANDLE);
    if (ret && interrupted_was_running &&
        interrupted->state == OBMM_COROUTINE_SCHEDULER_CONTEXT_READY) {
        interrupted->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_FAULTED;
    }
    obmm_coroutine_scheduler_schedule(runtime, started_ns);
}

void obmm_coroutine_scheduler_context_entry_c(struct obmm_coroutine_scheduler_context_local *local)
{
    struct obmm_coroutine_scheduler *runtime = active_runtime;

    if (!runtime || runtime->current != local ||
        local->state != OBMM_COROUTINE_SCHEDULER_CONTEXT_RUNNING) {
        _exit(127);
    }
    local->entry(local->argument);
    local->state = OBMM_COROUTINE_SCHEDULER_CONTEXT_DONE;
    obmm_coroutine_scheduler_trace(runtime, OBMM_COROUTINE_SCHEDULER_TRACE_CONTEXT_DONE, NULL, 0,
                   local->context.context_id);
}

void obmm_coroutine_scheduler_schedule_after_exit(void)
{
    struct obmm_coroutine_scheduler *runtime = active_runtime;
    int ret;

    if (!runtime || !runtime->started || !runtime->current ||
        runtime->current->state != OBMM_COROUTINE_SCHEDULER_CONTEXT_DONE) {
        _exit(127);
    }
    runtime->metrics.el0_scheduler_enter_assists++;
    __atomic_add_fetch(&runtime->event_consumer->scheduler_enter_count, 1,
                       __ATOMIC_RELEASE);
    obmm_coroutine_scheduler_scheduler_enter();
    ret = obmm_coroutine_scheduler_event_ring_drain(runtime);
    obmm_coroutine_scheduler_record_error(
        runtime, ret,
        OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_SCHEDULER_ENTER_ASSIST);
    obmm_coroutine_scheduler_schedule(runtime, obmm_coroutine_scheduler_now_ns());
}

static int obmm_coroutine_scheduler_collect_metrics(struct obmm_coroutine_scheduler *runtime)
{
    if (obmm_coroutine_scheduler_ioctl(
            runtime, OBMM_ASYNC_LOAD_IOCTL_GET_STATS,
            &runtime->metrics.device) != 0) {
        return obmm_coroutine_scheduler_neg_errno();
    }
    if (obmm_coroutine_scheduler_ioctl(
            runtime, OBMM_ASYNC_LOAD_IOCTL_GET_OBSERVABILITY,
            &runtime->metrics.observability) != 0) {
        return obmm_coroutine_scheduler_neg_errno();
    }
    if (runtime->caps.capabilities & OBMM_ASYNC_LOAD_CAP_REPLAY_RETIRE &&
        obmm_coroutine_scheduler_ioctl(
            runtime, OBMM_ASYNC_LOAD_IOCTL_GET_REPLAY_STATS,
            &runtime->metrics.replay) != 0) {
        return obmm_coroutine_scheduler_neg_errno();
    }
    return 0;
}

int obmm_coroutine_scheduler_run(struct obmm_coroutine_scheduler *runtime)
{
    struct obmm_async_load_start_v3 request;
    int home_cpu;
    int ret;

    if (!runtime || runtime->started || runtime->device_reset ||
        active_runtime || !runtime->logical_contexts) {
        return -EINVAL;
    }
    home_cpu = sched_getcpu();
    if (home_cpu < 0 || home_cpu > UINT16_MAX) {
        return obmm_coroutine_scheduler_neg_errno();
    }
    request = (struct obmm_async_load_start_v3) {
        .home_cpu = home_cpu,
        .flags = runtime->replay_retire ?
            OBMM_ASYNC_LOAD_START_REPLAY_RETIRE : 0,
        .load_timeout_ns = runtime->load_timeout_ns,
        .upcall_entry = (uintptr_t)obmm_coroutine_scheduler_upcall_entry,
        .logical_contexts = runtime->logical_contexts,
    };
    active_runtime = runtime;
    if (obmm_coroutine_scheduler_ioctl(
            runtime, OBMM_ASYNC_LOAD_IOCTL_START, &request) != 0) {
        ret = obmm_coroutine_scheduler_neg_errno();
        active_runtime = NULL;
        return ret;
    }
    if (request.owner_generation != runtime->caps.owner_generation) {
        obmm_coroutine_scheduler_ioctl(
            runtime, OBMM_ASYNC_LOAD_IOCTL_STOP, NULL);
        active_runtime = NULL;
        return -ESTALE;
    }
    ret = obmm_coroutine_scheduler_event_ring_validate(runtime);
    if (ret) {
        obmm_coroutine_scheduler_ioctl(
            runtime, OBMM_ASYNC_LOAD_IOCTL_STOP, NULL);
        active_runtime = NULL;
        return ret;
    }
    runtime->started = true;
    runtime->first_error = 0;
    runtime->current = NULL;
    runtime->metrics.kernel_hotpath_ioctls = 0;
    runtime->hot_path_active = true;
    if (sigsetjmp(runtime->return_environment, 1) == 0) {
        obmm_coroutine_scheduler_schedule(runtime, obmm_coroutine_scheduler_now_ns());
    }
    runtime->hot_path_active = false;
    runtime->metrics.event_producer_final = __atomic_load_n(
        &runtime->event_producer->producer_sequence, __ATOMIC_ACQUIRE);
    runtime->metrics.event_consumer_final = __atomic_load_n(
        &runtime->event_consumer->consumer_sequence, __ATOMIC_ACQUIRE);
    runtime->metrics.event_wait_wakeups = __atomic_load_n(
        &runtime->event_producer->wait_wakeups, __ATOMIC_ACQUIRE);
    obmm_coroutine_scheduler_record_error(
        runtime,
        runtime->metrics.event_producer_final ==
            runtime->metrics.event_consumer_final ? 0 : -EPROTO,
        OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_EVENT_RING_DRAIN);
    ret = obmm_coroutine_scheduler_collect_metrics(runtime);
    obmm_coroutine_scheduler_record_error(
        runtime, ret, OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_COLLECT_METRICS);
    ret = obmm_coroutine_scheduler_stop(runtime);
    obmm_coroutine_scheduler_record_error(runtime, ret, OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_STOP);
    return runtime->metrics.device.fail_stop && !runtime->first_error ?
        -EIO : runtime->first_error;
}

int obmm_coroutine_scheduler_stop(struct obmm_coroutine_scheduler *runtime)
{
    if (!runtime) {
        return -EINVAL;
    }
    if (!runtime->started) {
        return 0;
    }
    if (obmm_coroutine_scheduler_ioctl(
            runtime, OBMM_ASYNC_LOAD_IOCTL_STOP, NULL) != 0) {
        return obmm_coroutine_scheduler_neg_errno();
    }
    runtime->started = false;
    runtime->device_reset = true;
    runtime->current = NULL;
    if (active_runtime == runtime) {
        active_runtime = NULL;
    }
    return 0;
}

void obmm_coroutine_scheduler_get_metrics(const struct obmm_coroutine_scheduler *runtime,
                          struct obmm_coroutine_scheduler_metrics *metrics)
{
    if (!metrics) {
        return;
    }
    if (!runtime) {
        memset(metrics, 0, sizeof(*metrics));
        return;
    }
    *metrics = runtime->metrics;
}
