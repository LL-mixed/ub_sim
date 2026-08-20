/* SPDX-License-Identifier: MIT */
#ifndef LIB_OBMM_COROUTINE_SCHEDULER_H
#define LIB_OBMM_COROUTINE_SCHEDULER_H

#include <stddef.h>
#include <stdint.h>

#include <ub/obmm_async_load.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OBMM_COROUTINE_SCHEDULER_DEFAULT_DEVICE "/dev/linqu-async-load0"

struct obmm_coroutine_scheduler;

enum obmm_coroutine_scheduler_completion_mode {
    OBMM_COROUTINE_SCHEDULER_COMPLETION_PATCH = 0,
    OBMM_COROUTINE_SCHEDULER_COMPLETION_REPLAY = 1,
};

enum obmm_coroutine_scheduler_trace_kind {
    OBMM_COROUTINE_SCHEDULER_TRACE_UPCALL_PENDING = 1,
    OBMM_COROUTINE_SCHEDULER_TRACE_UPCALL_COMPLETE,
    OBMM_COROUTINE_SCHEDULER_TRACE_UPCALL_FAULT,
    OBMM_COROUTINE_SCHEDULER_TRACE_CONTEXT_RESUME,
    OBMM_COROUTINE_SCHEDULER_TRACE_CONTEXT_DONE,
};

enum obmm_coroutine_scheduler_error_stage {
    OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_NONE = 0,
    OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_WAIT_EVENT,
    OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_UPCALL_GET_EVENT,
    OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_EVENT_VALIDATE,
    OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_EVENT_HANDLE,
    OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_COLLECT_METRICS,
    OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_STOP,
    OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_SCHEDULER_ENTER,
};

struct obmm_coroutine_scheduler_trace_event {
    enum obmm_coroutine_scheduler_trace_kind kind;
    uint32_t status;
    uint32_t access_bytes;
    uint32_t rt;
    uint64_t context_id;
    uint64_t previous_context_id;
    uint64_t sequence;
    uint64_t token;
    uint64_t pc;
    uint64_t value;
};

typedef void (*obmm_coroutine_scheduler_trace_fn)(
    void *opaque, const struct obmm_coroutine_scheduler_trace_event *event);

struct obmm_coroutine_scheduler_options {
    const char *device_path;
    uint64_t load_timeout_ns;
    obmm_coroutine_scheduler_trace_fn trace;
    void *trace_opaque;
    enum obmm_coroutine_scheduler_completion_mode completion_mode;
};

struct obmm_coroutine_scheduler_map {
    uint64_t policy_id;
    uint64_t generation;
    uint64_t length;
};

struct obmm_coroutine_scheduler_metrics {
    struct obmm_async_load_stats_v2 device;
    struct obmm_async_load_observability_v2 observability;
    struct obmm_async_load_replay_stats_v1 replay;
    uint64_t el0_pending_upcalls;
    uint64_t el0_complete_upcalls;
    uint64_t el0_fault_upcalls;
    uint64_t el0_timeout_faults;
    uint64_t el0_context_saves;
    uint64_t el0_context_restores;
    uint64_t el0_context_switches;
    uint64_t el0_context_bytes;
    uint64_t el0_no_ready_waits;
    uint64_t el0_scheduler_ns;
    uint64_t el0_ready_high_water;
    uint32_t clock_mhz;
    int32_t first_error;
    uint32_t first_error_stage;
};

typedef void (*obmm_coroutine_scheduler_entry_fn)(void *arg);

int obmm_coroutine_scheduler_open(struct obmm_coroutine_scheduler **runtime,
                  const struct obmm_coroutine_scheduler_options *options);
void obmm_coroutine_scheduler_close(struct obmm_coroutine_scheduler *runtime);
int obmm_coroutine_scheduler_get_caps(const struct obmm_coroutine_scheduler *runtime,
                      struct obmm_async_load_caps_v2 *caps);

int obmm_coroutine_scheduler_register_map(struct obmm_coroutine_scheduler *runtime, int mapping_fd,
                          uint64_t mem_id, void *gsva_base,
                          size_t length, uint32_t flags,
                          struct obmm_coroutine_scheduler_map *map);
int obmm_coroutine_scheduler_register_map_for_phase(
    struct obmm_coroutine_scheduler *runtime, int mapping_fd, uint64_t mem_id,
    void *gsva_base, size_t length, uint32_t flags,
    uint64_t model_phase_generation, struct obmm_coroutine_scheduler_map *map);
int obmm_coroutine_scheduler_unregister_map(struct obmm_coroutine_scheduler *runtime,
                            struct obmm_coroutine_scheduler_map *map);

int obmm_coroutine_scheduler_context_create(struct obmm_coroutine_scheduler *runtime,
                            obmm_coroutine_scheduler_entry_fn entry, void *arg,
                            size_t stack_bytes, uint32_t flags,
                            uint64_t *context_id);
int obmm_coroutine_scheduler_context_destroy(struct obmm_coroutine_scheduler *runtime,
                             uint64_t context_id);

/* Runs the guest EL0 scheduler until all contexts complete or fault. */
int obmm_coroutine_scheduler_run(struct obmm_coroutine_scheduler *runtime);
int obmm_coroutine_scheduler_stop(struct obmm_coroutine_scheduler *runtime);
void obmm_coroutine_scheduler_get_metrics(const struct obmm_coroutine_scheduler *runtime,
                          struct obmm_coroutine_scheduler_metrics *metrics);

#ifdef __cplusplus
}
#endif

#endif
