/* SPDX-License-Identifier: MIT */
#ifndef LIB_OBMM_SCC_H
#define LIB_OBMM_SCC_H

#include <stddef.h>
#include <stdint.h>

#include <ub/obmm_scc.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OBMM_SCC_DEFAULT_DEVICE "/dev/linqu-scc0"

struct obmm_scc;

enum obmm_scc_trace_kind {
    OBMM_SCC_TRACE_UPCALL_PENDING = 1,
    OBMM_SCC_TRACE_UPCALL_COMPLETE,
    OBMM_SCC_TRACE_UPCALL_FAULT,
    OBMM_SCC_TRACE_CONTEXT_RESUME,
    OBMM_SCC_TRACE_CONTEXT_DONE,
};

enum obmm_scc_error_stage {
    OBMM_SCC_ERROR_STAGE_NONE = 0,
    OBMM_SCC_ERROR_STAGE_WAIT_EVENT,
    OBMM_SCC_ERROR_STAGE_UPCALL_GET_EVENT,
    OBMM_SCC_ERROR_STAGE_EVENT_VALIDATE,
    OBMM_SCC_ERROR_STAGE_EVENT_HANDLE,
    OBMM_SCC_ERROR_STAGE_COLLECT_METRICS,
    OBMM_SCC_ERROR_STAGE_STOP,
    OBMM_SCC_ERROR_STAGE_SCHEDULER_ENTER,
};

struct obmm_scc_trace_event {
    enum obmm_scc_trace_kind kind;
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

typedef void (*obmm_scc_trace_fn)(
    void *opaque, const struct obmm_scc_trace_event *event);

struct obmm_scc_options {
    const char *device_path;
    uint64_t load_timeout_ns;
    obmm_scc_trace_fn trace;
    void *trace_opaque;
};

struct obmm_scc_map {
    uint64_t policy_id;
    uint64_t generation;
    uint64_t length;
};

struct obmm_scc_metrics {
    struct obmm_scc_stats_v2 device;
    struct obmm_scc_observability_v2 observability;
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

typedef void (*obmm_scc_entry_fn)(void *arg);

int obmm_scc_open(struct obmm_scc **runtime,
                  const struct obmm_scc_options *options);
void obmm_scc_close(struct obmm_scc *runtime);
int obmm_scc_get_caps(const struct obmm_scc *runtime,
                      struct obmm_scc_caps_v2 *caps);

int obmm_scc_register_map(struct obmm_scc *runtime, int mapping_fd,
                          uint64_t mem_id, void *gsva_base,
                          size_t length, uint32_t flags,
                          struct obmm_scc_map *map);
int obmm_scc_register_map_for_phase(
    struct obmm_scc *runtime, int mapping_fd, uint64_t mem_id,
    void *gsva_base, size_t length, uint32_t flags,
    uint64_t model_phase_generation, struct obmm_scc_map *map);
int obmm_scc_unregister_map(struct obmm_scc *runtime,
                            struct obmm_scc_map *map);

int obmm_scc_context_create(struct obmm_scc *runtime,
                            obmm_scc_entry_fn entry, void *arg,
                            size_t stack_bytes, uint32_t flags,
                            uint64_t *context_id);
int obmm_scc_context_destroy(struct obmm_scc *runtime,
                             uint64_t context_id);

/* Runs the guest EL0 scheduler until all contexts complete or fault. */
int obmm_scc_run(struct obmm_scc *runtime);
int obmm_scc_stop(struct obmm_scc *runtime);
void obmm_scc_get_metrics(const struct obmm_scc *runtime,
                          struct obmm_scc_metrics *metrics);

#ifdef __cplusplus
}
#endif

#endif
