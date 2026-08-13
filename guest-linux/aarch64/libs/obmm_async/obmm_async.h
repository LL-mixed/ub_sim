/* SPDX-License-Identifier: MIT */
#ifndef LIB_OBMM_ASYNC_H
#define LIB_OBMM_ASYNC_H

#include <stddef.h>
#include <stdint.h>

#include <ub/obmm_async.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OBMM_ASYNC_DEFAULT_DEVICE "/dev/linqu-ub0"
#define OBMM_ASYNC_MAX_COROUTINES 64

struct obmm_async;
struct obmm_coroutine;

enum obmm_async_mode {
    OBMM_ASYNC_MODE_POLL,
    OBMM_ASYNC_MODE_IRQ,
};

enum obmm_async_future_state {
    OBMM_ASYNC_FUTURE_FREE,
    OBMM_ASYNC_FUTURE_SUBMITTED,
    OBMM_ASYNC_FUTURE_READY,
    OBMM_ASYNC_FUTURE_CONSUMED,
};

struct obmm_async_options {
    const char *device_path;
    enum obmm_async_mode mode;
    uint32_t spin_us;
};

struct obmm_async_map {
    uint64_t id;
    uint64_t generation;
    uint64_t length;
};

struct obmm_async_buffer {
    uint32_t id;
    uint32_t generation;
    uint64_t length;
    void *data;
};

struct obmm_async_future {
    uint64_t token;
};

struct obmm_async_result {
    int32_t status;
    uint32_t bytes_done;
    uint32_t provider_status;
    uint64_t checksum64;
    uint64_t completed_ns;
    uint64_t map_generation;
    uint64_t user_data;
};

struct obmm_async_metrics {
    uint64_t submitted;
    uint64_t completed;
    uint64_t failed;
    uint64_t stale_completions;
    uint64_t coroutine_switches;
    uint64_t coroutine_yields;
    uint64_t idle_polls;
    uint64_t submit_ns_total;
    uint64_t submit_ns_p50;
    uint64_t switch_ns_total;
    uint64_t switch_ns_p50;
    uint64_t cq_drain_ns_total;
    uint64_t cq_drain_ns_p50;
    uint64_t ready_ns;
    uint64_t wait_ns;
    uint64_t idle_ns;
    uint64_t no_ready;
};

typedef void (*obmm_coroutine_entry_fn)(void *arg);

int obmm_async_open(struct obmm_async **runtime,
                    const struct obmm_async_options *options);
void obmm_async_close(struct obmm_async *runtime);

int obmm_async_map_register(struct obmm_async *runtime, int obmm_fd,
                            uint64_t mem_id, void *mapped_addr,
                            uint64_t length, struct obmm_async_map *map);
int obmm_async_map_unregister(struct obmm_async *runtime,
                              struct obmm_async_map *map);
int obmm_async_buffer_alloc(struct obmm_async *runtime, uint64_t length,
                            struct obmm_async_buffer *buffer);
int obmm_async_buffer_free(struct obmm_async *runtime,
                           struct obmm_async_buffer *buffer);

int obmm_load_submit(struct obmm_async *runtime,
                     const struct obmm_async_map *map,
                     uint64_t remote_offset,
                     const struct obmm_async_buffer *buffer,
                     uint32_t dst_offset, uint32_t length,
                     uint64_t deadline_ns, uint64_t user_data,
                     struct obmm_async_future *future);
int obmm_test(struct obmm_async *runtime,
              const struct obmm_async_future *future,
              struct obmm_async_result *result);
int obmm_await(struct obmm_async *runtime,
               struct obmm_async_future *future,
               struct obmm_async_result *result);
int obmm_cancel(struct obmm_async *runtime,
                const struct obmm_async_future *future);
int obmm_async_drain(struct obmm_async *runtime);

int obmm_coroutine_create(struct obmm_async *runtime,
                          obmm_coroutine_entry_fn entry, void *arg,
                          size_t stack_bytes, uint64_t *coroutine_id);
int obmm_coroutine_run(struct obmm_async *runtime);
void obmm_coroutine_yield(struct obmm_async *runtime);

void obmm_async_get_metrics(const struct obmm_async *runtime,
                            struct obmm_async_metrics *metrics);
int obmm_async_get_observability(
    const struct obmm_async *runtime,
    struct obmm_async_observability_v1 *observability);
int obmm_async_reset_observability(struct obmm_async *runtime);
int obmm_async_status_to_errno(int32_t status);
int obmm_async_context_selftest(void);

uint64_t obmm_async_token_pack(uint32_t generation, uint16_t queue_id,
                               uint16_t slot);
uint32_t obmm_async_token_generation(uint64_t token);
uint16_t obmm_async_token_queue(uint64_t token);
uint16_t obmm_async_token_slot(uint64_t token);

#ifdef __cplusplus
}
#endif

#endif
