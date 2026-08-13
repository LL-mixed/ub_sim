/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "obmm_scc.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OBMM_SCC_SCHEDULER_STACK_BYTES (64UL * 1024UL)

enum obmm_scc_context_state {
    OBMM_SCC_CONTEXT_FREE,
    OBMM_SCC_CONTEXT_READY,
    OBMM_SCC_CONTEXT_RUNNING,
    OBMM_SCC_CONTEXT_WAIT_REMOTE,
    OBMM_SCC_CONTEXT_FAULTED,
    OBMM_SCC_CONTEXT_DONE,
};

struct obmm_scc_context_local {
    enum obmm_scc_context_state state;
    uint16_t slot;
    obmm_scc_entry_fn entry;
    void *argument;
    void *mapping;
    size_t mapping_bytes;
    uint64_t waiting_token;
    struct obmm_scc_context_v2 context __attribute__((aligned(16)));
};

struct obmm_scc_map_local {
    bool allocated;
    struct obmm_scc_map map;
};

struct obmm_scc {
    int fd;
    uint64_t load_timeout_ns;
    obmm_scc_trace_fn trace;
    void *trace_opaque;
    bool started;
    bool device_reset;
    int first_error;
    uint16_t scheduler_cursor;
    uint16_t logical_contexts;
    uint64_t last_resumed_id;
    struct obmm_scc_caps_v2 caps;
    struct obmm_scc_metrics metrics;
    struct obmm_scc_context_local *current;
    struct obmm_scc_context_local contexts[OBMM_SCC_MAX_CONTEXTS];
    struct obmm_scc_map_local maps[OBMM_SCC_MAX_PENDING_LOADS];
    void *scheduler_mapping;
    size_t scheduler_mapping_bytes;
    uintptr_t scheduler_stack_top;
    sigjmp_buf return_environment;
};

static struct obmm_scc *active_runtime;

extern void obmm_scc_upcall_entry(void);
extern void obmm_scc_context_bootstrap(void);
extern void obmm_scc_context_finish(void) __attribute__((noreturn));
extern void obmm_scc_context_resume(
    const struct obmm_scc_context_v2 *context) __attribute__((noreturn));

_Static_assert(sizeof(struct obmm_scc_context_v2) ==
               OBMM_SCC_CONTEXT_STATE_BYTES,
               "OBMM SCC context ABI size mismatch");
_Static_assert(offsetof(struct obmm_scc_context_v2, x) == 16,
               "OBMM SCC x-register offset mismatch");
_Static_assert(offsetof(struct obmm_scc_context_v2, sp) == 264,
               "OBMM SCC SP offset mismatch");
_Static_assert(offsetof(struct obmm_scc_context_v2, pc) == 272,
               "OBMM SCC PC offset mismatch");
_Static_assert(offsetof(struct obmm_scc_context_v2, q) == 288,
               "OBMM SCC SIMD offset mismatch");
_Static_assert(offsetof(struct obmm_scc_context_v2, fpcr) == 800,
               "OBMM SCC FPCR offset mismatch");
_Static_assert(offsetof(struct obmm_scc_context_local, context) % 16 == 0,
               "OBMM SCC local context must be 16-byte aligned");

static int obmm_scc_neg_errno(void)
{
    return errno ? -errno : -EIO;
}

static uint64_t obmm_scc_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static void obmm_scc_trace(struct obmm_scc *runtime,
                           enum obmm_scc_trace_kind kind,
                           const struct obmm_scc_event_v2 *event,
                           uint64_t previous_context_id,
                           uint64_t context_id)
{
    struct obmm_scc_trace_event trace_event = {
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

static uint64_t obmm_scc_read_tpidr_el0(void)
{
#if defined(__aarch64__)
    uint64_t value;

    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

static uint64_t obmm_scc_read_nzcv(void)
{
#if defined(__aarch64__)
    uint64_t value;

    __asm__ volatile("mrs %0, nzcv" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

static uint64_t obmm_scc_read_fpcr(void)
{
#if defined(__aarch64__)
    uint64_t value;

    __asm__ volatile("mrs %0, fpcr" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

static uint64_t obmm_scc_read_fpsr(void)
{
#if defined(__aarch64__)
    uint64_t value;

    __asm__ volatile("mrs %0, fpsr" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

static int obmm_scc_stack_allocate(size_t requested_bytes,
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
        return obmm_scc_neg_errno();
    }
    if (mprotect((char *)address + page_bytes, usable_bytes,
                 PROT_READ | PROT_WRITE) != 0) {
        int error = obmm_scc_neg_errno();

        munmap(address, total_bytes);
        return error;
    }
    *mapping = address;
    *mapping_bytes = total_bytes;
    *stack_top = ((uintptr_t)address + page_bytes + usable_bytes) & ~15ULL;
    return 0;
}

int obmm_scc_open(struct obmm_scc **runtime,
                  const struct obmm_scc_options *options)
{
    const char *device = OBMM_SCC_DEFAULT_DEVICE;
    struct obmm_scc *scc;
    int ret;

    if (!runtime) {
        return -EINVAL;
    }
    if (options && options->device_path) {
        device = options->device_path;
    }
    scc = calloc(1, sizeof(*scc));
    if (!scc) {
        return -ENOMEM;
    }
    scc->fd = open(device, O_RDWR | O_CLOEXEC);
    if (scc->fd < 0) {
        ret = obmm_scc_neg_errno();
        free(scc);
        return ret;
    }
    if (options) {
        scc->load_timeout_ns = options->load_timeout_ns;
        scc->trace = options->trace;
        scc->trace_opaque = options->trace_opaque;
    }
    if (ioctl(scc->fd, OBMM_SCC_IOCTL_QUERY_CAPS, &scc->caps) != 0 ||
        scc->caps.abi_version != OBMM_SCC_ABI_VERSION ||
        !scc->caps.context_entries ||
        scc->caps.context_entries > OBMM_SCC_MAX_CONTEXTS ||
        !scc->caps.pending_load_entries ||
        scc->caps.pending_load_entries > OBMM_SCC_MAX_PENDING_LOADS ||
        !scc->caps.event_queue_depth ||
        scc->caps.event_queue_depth > OBMM_SCC_MAX_EVENTS ||
        scc->caps.context_state_bytes != OBMM_SCC_CONTEXT_STATE_BYTES ||
        scc->caps.resume_hlt_imm != OBMM_SCC_RESUME_HLT_IMM ||
        (scc->caps.capabilities &
         (OBMM_SCC_CAP_DIRECT_EL0_UPCALL | OBMM_SCC_CAP_EL0_RESUME |
          OBMM_SCC_CAP_FULL_CONTEXT)) !=
         (OBMM_SCC_CAP_DIRECT_EL0_UPCALL | OBMM_SCC_CAP_EL0_RESUME |
          OBMM_SCC_CAP_FULL_CONTEXT) ||
        !scc->caps.clock_mhz) {
        ret = errno ? -errno : -EPROTO;
        close(scc->fd);
        free(scc);
        return ret;
    }
    ret = obmm_scc_stack_allocate(
        OBMM_SCC_SCHEDULER_STACK_BYTES, &scc->scheduler_mapping,
        &scc->scheduler_mapping_bytes, &scc->scheduler_stack_top);
    if (ret) {
        close(scc->fd);
        free(scc);
        return ret;
    }
    scc->metrics.clock_mhz = scc->caps.clock_mhz;
    scc->scheduler_cursor = scc->caps.context_entries - 1;
    *runtime = scc;
    return 0;
}

static void obmm_scc_release_local_resources(struct obmm_scc *runtime)
{
    uint32_t index;

    for (index = 0; index < OBMM_SCC_MAX_CONTEXTS; index++) {
        struct obmm_scc_context_local *context =
            &runtime->contexts[index];

        if (context->state == OBMM_SCC_CONTEXT_FREE) {
            continue;
        }
        munmap(context->mapping, context->mapping_bytes);
        memset(context, 0, sizeof(*context));
    }
    if (runtime->scheduler_mapping) {
        munmap(runtime->scheduler_mapping,
               runtime->scheduler_mapping_bytes);
    }
    memset(runtime->maps, 0, sizeof(runtime->maps));
}

void obmm_scc_close(struct obmm_scc *runtime)
{
    if (!runtime) {
        return;
    }
    if (runtime->started) {
        obmm_scc_stop(runtime);
    }
    obmm_scc_release_local_resources(runtime);
    close(runtime->fd);
    if (active_runtime == runtime) {
        active_runtime = NULL;
    }
    free(runtime);
}

int obmm_scc_get_caps(const struct obmm_scc *runtime,
                      struct obmm_scc_caps_v2 *caps)
{
    if (!runtime || !caps) {
        return -EINVAL;
    }
    *caps = runtime->caps;
    return 0;
}

int obmm_scc_register_map(struct obmm_scc *runtime, int mapping_fd,
                          uint64_t mem_id, void *gsva_base,
                          size_t length, uint32_t flags,
                          struct obmm_scc_map *map)
{
    return obmm_scc_register_map_for_phase(
        runtime, mapping_fd, mem_id, gsva_base, length, flags, 1, map);
}

int obmm_scc_register_map_for_phase(
    struct obmm_scc *runtime, int mapping_fd, uint64_t mem_id,
    void *gsva_base, size_t length, uint32_t flags,
    uint64_t model_phase_generation, struct obmm_scc_map *map)
{
    struct obmm_scc_map_register_v1 request = {
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
    if (ioctl(runtime->fd, OBMM_SCC_IOCTL_REGISTER_MAP, &request) != 0) {
        return obmm_scc_neg_errno();
    }
    for (index = 0; index < runtime->caps.pending_load_entries; index++) {
        if (!runtime->maps[index].allocated) {
            break;
        }
    }
    if (index == runtime->caps.pending_load_entries) {
        struct obmm_scc_map_unregister_v1 rollback = {
            .policy_id = request.policy_id,
            .map_generation = request.map_generation,
        };

        ioctl(runtime->fd, OBMM_SCC_IOCTL_UNREGISTER_MAP, &rollback);
        return -ENOSPC;
    }
    *map = (struct obmm_scc_map) {
        .policy_id = request.policy_id,
        .generation = request.map_generation,
        .length = request.length,
    };
    runtime->maps[index].allocated = true;
    runtime->maps[index].map = *map;
    return 0;
}

int obmm_scc_unregister_map(struct obmm_scc *runtime,
                            struct obmm_scc_map *map)
{
    struct obmm_scc_map_unregister_v1 request;
    uint32_t index;

    if (!runtime || !map || !map->policy_id || runtime->started) {
        return -EINVAL;
    }
    for (index = 0; index < OBMM_SCC_MAX_PENDING_LOADS; index++) {
        if (runtime->maps[index].allocated &&
            runtime->maps[index].map.policy_id == map->policy_id &&
            runtime->maps[index].map.generation == map->generation) {
            break;
        }
    }
    if (index == OBMM_SCC_MAX_PENDING_LOADS) {
        return -ESTALE;
    }
    if (!runtime->device_reset) {
        request = (struct obmm_scc_map_unregister_v1) {
            .policy_id = map->policy_id,
            .map_generation = map->generation,
        };
        if (ioctl(runtime->fd, OBMM_SCC_IOCTL_UNREGISTER_MAP,
                  &request) != 0) {
            return obmm_scc_neg_errno();
        }
    }
    memset(&runtime->maps[index], 0, sizeof(runtime->maps[index]));
    memset(map, 0, sizeof(*map));
    return 0;
}

int obmm_scc_context_create(struct obmm_scc *runtime,
                            obmm_scc_entry_fn entry, void *arg,
                            size_t stack_bytes, uint32_t flags,
                            uint64_t *context_id)
{
    struct obmm_scc_context_local *local;
    uintptr_t stack_top;
    uint32_t index;
    int home_cpu;
    int ret;

    if (!runtime || !entry || !context_id || runtime->started ||
        runtime->device_reset || flags) {
        return -EINVAL;
    }
    for (index = 0; index < runtime->caps.context_entries; index++) {
        if (runtime->contexts[index].state == OBMM_SCC_CONTEXT_FREE) {
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
    ret = obmm_scc_stack_allocate(stack_bytes, &local->mapping,
                                  &local->mapping_bytes, &stack_top);
    if (ret) {
        return ret;
    }
    local->state = OBMM_SCC_CONTEXT_READY;
    local->slot = index;
    local->entry = entry;
    local->argument = arg;
    local->context.context_id =
        (runtime->caps.owner_generation << 32) |
        ((uint64_t)(uint16_t)home_cpu << 16) | index;
    local->context.x[19] = (uintptr_t)local;
    local->context.sp = stack_top;
    local->context.pc = (uintptr_t)obmm_scc_context_bootstrap;
    local->context.nzcv = obmm_scc_read_nzcv();
    local->context.fpcr = obmm_scc_read_fpcr();
    local->context.fpsr = obmm_scc_read_fpsr();
    local->context.tpidr_el0 = obmm_scc_read_tpidr_el0();
    if (runtime->logical_contexts <= index) {
        runtime->logical_contexts = index + 1;
    }
    *context_id = local->context.context_id;
    return 0;
}

int obmm_scc_context_destroy(struct obmm_scc *runtime,
                             uint64_t context_id)
{
    uint32_t index;

    if (!runtime || !context_id || runtime->started) {
        return -EINVAL;
    }
    for (index = 0; index < runtime->caps.context_entries; index++) {
        struct obmm_scc_context_local *local = &runtime->contexts[index];

        if (local->state == OBMM_SCC_CONTEXT_FREE ||
            local->context.context_id != context_id) {
            continue;
        }
        munmap(local->mapping, local->mapping_bytes);
        memset(local, 0, sizeof(*local));
        while (runtime->logical_contexts &&
               runtime->contexts[runtime->logical_contexts - 1].state ==
                   OBMM_SCC_CONTEXT_FREE) {
            runtime->logical_contexts--;
        }
        return 0;
    }
    return -ESTALE;
}

static struct obmm_scc_context_local *obmm_scc_find_context(
    struct obmm_scc *runtime, uint64_t context_id)
{
    uint16_t slot = context_id & 0xffff;
    struct obmm_scc_context_local *local;

    if (!context_id || slot >= runtime->logical_contexts) {
        return NULL;
    }
    local = &runtime->contexts[slot];
    return local->state != OBMM_SCC_CONTEXT_FREE &&
        local->context.context_id == context_id ? local : NULL;
}

static uint64_t obmm_scc_ready_count(const struct obmm_scc *runtime)
{
    uint64_t count = 0;
    uint16_t index;

    for (index = 0; index < runtime->logical_contexts; index++) {
        if (runtime->contexts[index].state == OBMM_SCC_CONTEXT_READY) {
            count++;
        }
    }
    return count;
}

static bool obmm_scc_has_waiting(const struct obmm_scc *runtime)
{
    uint16_t index;

    for (index = 0; index < runtime->logical_contexts; index++) {
        if (runtime->contexts[index].state ==
            OBMM_SCC_CONTEXT_WAIT_REMOTE) {
            return true;
        }
    }
    return false;
}

static struct obmm_scc_context_local *obmm_scc_choose_ready(
    struct obmm_scc *runtime)
{
    uint16_t ordinal;

    for (ordinal = 1; ordinal <= runtime->logical_contexts; ordinal++) {
        uint16_t slot = (runtime->scheduler_cursor + ordinal) %
            runtime->logical_contexts;

        if (runtime->contexts[slot].state == OBMM_SCC_CONTEXT_READY) {
            runtime->scheduler_cursor = slot;
            return &runtime->contexts[slot];
        }
    }
    return NULL;
}

static int obmm_scc_status_error(uint32_t status)
{
    switch (status) {
    case OBMM_SCC_STATUS_TIMEOUT:
        return -ETIMEDOUT;
    case OBMM_SCC_STATUS_PERMISSION:
        return -EACCES;
    case OBMM_SCC_STATUS_STALE_MAP:
        return -ESTALE;
    case OBMM_SCC_STATUS_CANCELLED:
        return -ECANCELED;
    case OBMM_SCC_STATUS_REMOTE_IO:
    case OBMM_SCC_STATUS_INTERNAL:
    default:
        return -EIO;
    }
}

static int obmm_scc_process_event(struct obmm_scc *runtime,
                                  const struct obmm_scc_event_v2 *event)
{
    struct obmm_scc_context_local *target =
        obmm_scc_find_context(runtime, event->context_id);

    if (!target || !event->sequence || !event->plt_token ||
        event->rt > 31 ||
        (event->access_bytes != 1 && event->access_bytes != 2 &&
         event->access_bytes != 4 && event->access_bytes != 8)) {
        return -EPROTO;
    }
    switch (event->kind) {
    case OBMM_SCC_EVENT_PENDING:
        runtime->metrics.el0_pending_upcalls++;
        if (event->status != OBMM_SCC_STATUS_SUCCESS ||
            target != runtime->current ||
            target->state != OBMM_SCC_CONTEXT_READY ||
            event->fault_pc != target->context.pc) {
            return -EPROTO;
        }
        target->waiting_token = event->plt_token;
        target->state = OBMM_SCC_CONTEXT_WAIT_REMOTE;
        obmm_scc_trace(runtime, OBMM_SCC_TRACE_UPCALL_PENDING,
                       event, 0, event->context_id);
        return 0;
    case OBMM_SCC_EVENT_COMPLETE:
        runtime->metrics.el0_complete_upcalls++;
        if (event->status != OBMM_SCC_STATUS_SUCCESS ||
            target->state != OBMM_SCC_CONTEXT_WAIT_REMOTE ||
            target->waiting_token != event->plt_token ||
            event->fault_pc != target->context.pc) {
            return -EPROTO;
        }
        if (event->rt < 31) {
            target->context.x[event->rt] = event->value;
        }
        target->context.pc = event->fault_pc + 4;
        target->waiting_token = 0;
        target->state = OBMM_SCC_CONTEXT_READY;
        obmm_scc_trace(runtime, OBMM_SCC_TRACE_UPCALL_COMPLETE,
                       event, 0, event->context_id);
        return 0;
    case OBMM_SCC_EVENT_FAULT:
        runtime->metrics.el0_fault_upcalls++;
        if (event->status == OBMM_SCC_STATUS_TIMEOUT) {
            runtime->metrics.el0_timeout_faults++;
        }
        if (target->state != OBMM_SCC_CONTEXT_WAIT_REMOTE ||
            target->waiting_token != event->plt_token ||
            event->status == OBMM_SCC_STATUS_SUCCESS) {
            return -EPROTO;
        }
        target->waiting_token = 0;
        target->state = OBMM_SCC_CONTEXT_FAULTED;
        obmm_scc_trace(runtime, OBMM_SCC_TRACE_UPCALL_FAULT,
                       event, 0, event->context_id);
        return obmm_scc_status_error(event->status);
    case OBMM_SCC_EVENT_OWNER_STOP:
        target->state = OBMM_SCC_CONTEXT_FAULTED;
        return -ECANCELED;
    default:
        return -EPROTO;
    }
}

static void obmm_scc_record_error(
    struct obmm_scc *runtime, int error,
    enum obmm_scc_error_stage stage)
{
    if (error && !runtime->first_error) {
        runtime->first_error = error;
        runtime->metrics.first_error = error;
        runtime->metrics.first_error_stage = stage;
    }
}

static __attribute__((noreturn)) void obmm_scc_schedule(
    struct obmm_scc *runtime, uint64_t scheduler_started_ns)
{
    for (;;) {
        struct obmm_scc_context_local *next =
            obmm_scc_choose_ready(runtime);
        uint64_t ready_count = obmm_scc_ready_count(runtime);

        if (runtime->metrics.el0_ready_high_water < ready_count) {
            runtime->metrics.el0_ready_high_water = ready_count;
        }
        if (next) {
            uint64_t now_ns = obmm_scc_now_ns();
            uint64_t previous_context_id = runtime->last_resumed_id;

            if (runtime->last_resumed_id &&
                runtime->last_resumed_id != next->context.context_id) {
                runtime->metrics.el0_context_switches++;
            }
            runtime->metrics.el0_context_restores++;
            runtime->metrics.el0_context_bytes +=
                OBMM_SCC_CONTEXT_STATE_BYTES;
            if (now_ns >= scheduler_started_ns) {
                runtime->metrics.el0_scheduler_ns +=
                    now_ns - scheduler_started_ns;
            }
            runtime->last_resumed_id = next->context.context_id;
            runtime->current = next;
            next->state = OBMM_SCC_CONTEXT_RUNNING;
            obmm_scc_trace(runtime, OBMM_SCC_TRACE_CONTEXT_RESUME,
                           NULL, previous_context_id,
                           next->context.context_id);
            obmm_scc_context_resume(&next->context);
        }
        if (obmm_scc_has_waiting(runtime)) {
            struct obmm_scc_event_v2 event = {
                .flags = OBMM_SCC_EVENT_GET_WAIT,
            };
            int ret;

            runtime->metrics.el0_no_ready_waits++;
            ret = ioctl(runtime->fd, OBMM_SCC_IOCTL_GET_EVENT, &event);
            if (ret != 0) {
                obmm_scc_record_error(
                    runtime, obmm_scc_neg_errno(),
                    OBMM_SCC_ERROR_STAGE_WAIT_EVENT);
                break;
            }
            ret = obmm_scc_process_event(runtime, &event);
            obmm_scc_record_error(
                runtime, ret, OBMM_SCC_ERROR_STAGE_EVENT_HANDLE);
            continue;
        }
        break;
    }
    {
        uint64_t now_ns = obmm_scc_now_ns();

        if (now_ns >= scheduler_started_ns) {
            runtime->metrics.el0_scheduler_ns +=
                now_ns - scheduler_started_ns;
        }
    }
    siglongjmp(runtime->return_environment, 1);
    __builtin_unreachable();
}

uintptr_t obmm_scc_scheduler_stack_top(void)
{
    return active_runtime ? active_runtime->scheduler_stack_top : 0;
}

void obmm_scc_upcall_dispatch(struct obmm_scc_context_v2 *frame)
{
    struct obmm_scc *runtime = active_runtime;
    struct obmm_scc_context_local *interrupted;
    struct obmm_scc_event_v2 event = { 0 };
    bool interrupted_was_running;
    uint64_t context_id;
    uint64_t context_flags;
    uint64_t started_ns = obmm_scc_now_ns();
    int ret;

    if (!runtime || !runtime->started || !frame || !runtime->current ||
        (runtime->current->state != OBMM_SCC_CONTEXT_RUNNING &&
         runtime->current->state != OBMM_SCC_CONTEXT_DONE)) {
        _exit(127);
    }
    interrupted = runtime->current;
    interrupted_was_running =
        interrupted->state == OBMM_SCC_CONTEXT_RUNNING;
    runtime->metrics.el0_context_saves++;
    runtime->metrics.el0_context_bytes += OBMM_SCC_CONTEXT_STATE_BYTES;
    if (interrupted_was_running) {
        context_id = interrupted->context.context_id;
        context_flags = interrupted->context.flags;
        memcpy(&interrupted->context, frame, sizeof(*frame));
        interrupted->context.context_id = context_id;
        interrupted->context.flags = context_flags;
        interrupted->state = OBMM_SCC_CONTEXT_READY;
    }
    ret = ioctl(runtime->fd, OBMM_SCC_IOCTL_GET_EVENT, &event);
    if (ret != 0) {
        obmm_scc_record_error(
            runtime, obmm_scc_neg_errno(),
            OBMM_SCC_ERROR_STAGE_UPCALL_GET_EVENT);
        interrupted->state = OBMM_SCC_CONTEXT_FAULTED;
        obmm_scc_schedule(runtime, started_ns);
    }
    if (!event.interrupted_pc) {
        obmm_scc_record_error(
            runtime, -EPROTO, OBMM_SCC_ERROR_STAGE_EVENT_VALIDATE);
        interrupted->state = OBMM_SCC_CONTEXT_FAULTED;
        obmm_scc_schedule(runtime, started_ns);
    }
    if (interrupted_was_running) {
        interrupted->context.pc = event.interrupted_pc;
    }
    ret = obmm_scc_process_event(runtime, &event);
    obmm_scc_record_error(
        runtime, ret, OBMM_SCC_ERROR_STAGE_EVENT_HANDLE);
    if (ret && interrupted_was_running &&
        interrupted->state == OBMM_SCC_CONTEXT_READY) {
        interrupted->state = OBMM_SCC_CONTEXT_FAULTED;
    }
    obmm_scc_schedule(runtime, started_ns);
}

void obmm_scc_context_entry_c(struct obmm_scc_context_local *local)
{
    struct obmm_scc *runtime = active_runtime;

    if (!runtime || runtime->current != local ||
        local->state != OBMM_SCC_CONTEXT_RUNNING) {
        _exit(127);
    }
    local->entry(local->argument);
    local->state = OBMM_SCC_CONTEXT_DONE;
    obmm_scc_trace(runtime, OBMM_SCC_TRACE_CONTEXT_DONE, NULL, 0,
                   local->context.context_id);
}

void obmm_scc_schedule_after_exit(void)
{
    struct obmm_scc *runtime = active_runtime;

    if (!runtime || !runtime->started || !runtime->current ||
        runtime->current->state != OBMM_SCC_CONTEXT_DONE) {
        _exit(127);
    }
    obmm_scc_schedule(runtime, obmm_scc_now_ns());
}

static int obmm_scc_collect_metrics(struct obmm_scc *runtime)
{
    if (ioctl(runtime->fd, OBMM_SCC_IOCTL_GET_STATS,
              &runtime->metrics.device) != 0) {
        return obmm_scc_neg_errno();
    }
    if (ioctl(runtime->fd, OBMM_SCC_IOCTL_GET_OBSERVABILITY,
              &runtime->metrics.observability) != 0) {
        return obmm_scc_neg_errno();
    }
    return 0;
}

int obmm_scc_run(struct obmm_scc *runtime)
{
    struct obmm_scc_start_v2 request;
    int home_cpu;
    int ret;

    if (!runtime || runtime->started || runtime->device_reset ||
        active_runtime || !runtime->logical_contexts) {
        return -EINVAL;
    }
    home_cpu = sched_getcpu();
    if (home_cpu < 0 || home_cpu > UINT16_MAX) {
        return obmm_scc_neg_errno();
    }
    request = (struct obmm_scc_start_v2) {
        .home_cpu = home_cpu,
        .load_timeout_ns = runtime->load_timeout_ns,
        .upcall_entry = (uintptr_t)obmm_scc_upcall_entry,
        .logical_contexts = runtime->logical_contexts,
    };
    active_runtime = runtime;
    if (ioctl(runtime->fd, OBMM_SCC_IOCTL_START, &request) != 0) {
        ret = obmm_scc_neg_errno();
        active_runtime = NULL;
        return ret;
    }
    if (request.owner_generation != runtime->caps.owner_generation) {
        ioctl(runtime->fd, OBMM_SCC_IOCTL_STOP);
        active_runtime = NULL;
        return -ESTALE;
    }
    runtime->started = true;
    runtime->first_error = 0;
    runtime->current = NULL;
    if (sigsetjmp(runtime->return_environment, 1) == 0) {
        obmm_scc_schedule(runtime, obmm_scc_now_ns());
    }
    ret = obmm_scc_collect_metrics(runtime);
    obmm_scc_record_error(
        runtime, ret, OBMM_SCC_ERROR_STAGE_COLLECT_METRICS);
    ret = obmm_scc_stop(runtime);
    obmm_scc_record_error(runtime, ret, OBMM_SCC_ERROR_STAGE_STOP);
    return runtime->metrics.device.fail_stop && !runtime->first_error ?
        -EIO : runtime->first_error;
}

int obmm_scc_stop(struct obmm_scc *runtime)
{
    if (!runtime) {
        return -EINVAL;
    }
    if (!runtime->started) {
        return 0;
    }
    if (ioctl(runtime->fd, OBMM_SCC_IOCTL_STOP) != 0) {
        return obmm_scc_neg_errno();
    }
    runtime->started = false;
    runtime->device_reset = true;
    runtime->current = NULL;
    if (active_runtime == runtime) {
        active_runtime = NULL;
    }
    return 0;
}

void obmm_scc_get_metrics(const struct obmm_scc *runtime,
                          struct obmm_scc_metrics *metrics)
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
