/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "obmm_async.h"
#include "obmm_common.h"
#include "obmm_scc.h"
#include "uffd_mode.h"
#include "logical_op.h"

#define ASYNC_EXPORT_BYTES (2UL * 1024UL * 1024UL)
#define ASYNC_BOOTSTRAP_GENERATION 0x4153594e4301ULL
#define ASYNC_COROUTINE_STACK_BYTES (64UL * 1024UL)
#define ASYNC_TRACE_SAMPLE_LIMIT 65536
#define ASYNC_P2B_VALUE_OFFSET_BASE 0x1000UL
#define ASYNC_P2B_VALUE_OFFSET_STRIDE 0x1000UL
#define ASYNC_P2B_TRACE_LINES 128
#define ASYNC_P2B_TRACE_LINE_BYTES 384

enum async_pattern {
    ASYNC_PATTERN_SEQUENTIAL,
    ASYNC_PATTERN_RANDOM,
    ASYNC_PATTERN_DEPENDENT,
    ASYNC_PATTERN_MIXED,
};

enum async_app_mode {
    ASYNC_APP_MODE_SYNC,
    ASYNC_APP_MODE_POLL,
    ASYNC_APP_MODE_IRQ,
    ASYNC_APP_MODE_SCHEDULER_CORE,
    ASYNC_APP_MODE_USERFAULTFD,
};

enum async_baseline_case {
    ASYNC_BASELINE_LOCAL_DRAM,
    ASYNC_BASELINE_OBMM_LOCAL_HIT,
    ASYNC_BASELINE_SYNC_REMOTE_ZERO,
    ASYNC_BASELINE_SYNC_REMOTE_MODELED,
};

enum async_expected_outcome {
    ASYNC_OUTCOME_SUCCESS,
    ASYNC_OUTCOME_ERROR,
    ASYNC_OUTCOME_DROP_TIMEOUT,
    ASYNC_OUTCOME_DUPLICATE_LATE,
};

enum async_option_flag {
    ASYNC_OPTION_COROUTINES = 1U << 0,
    ASYNC_OPTION_INFLIGHT = 1U << 1,
    ASYNC_OPTION_LOOKAHEAD = 1U << 2,
    ASYNC_OPTION_COMPUTE_US = 1U << 3,
    ASYNC_OPTION_UFFD_CASE = 1U << 4,
    ASYNC_OPTION_WORKER_THREADS = 1U << 5,
    ASYNC_OPTION_HANDLER_CPU = 1U << 6,
    ASYNC_OPTION_PAGES = 1U << 7,
    ASYNC_OPTION_BASELINE_CASE = 1U << 8,
};

struct async_config {
    enum async_app_mode mode;
    enum async_pattern pattern;
    enum obmm_uffd_case uffd_case;
    enum async_baseline_case baseline_case;
    enum async_expected_outcome expected_outcome;
    uint32_t coroutines;
    uint32_t inflight;
    uint32_t lookahead;
    uint32_t access_bytes;
    uint32_t compute_us;
    uint32_t deadline_us;
    uint32_t worker_threads;
    uint32_t pages;
    uint32_t option_flags;
    uint64_t iterations;
    uint64_t warmup;
    uint64_t seed;
    int node_count;
    int peer_index;
    int producer_index;
    int handler_cpu;
    uint32_t min_duration_ms;
    uint32_t trace_sample_ppm;
    const char *eval_band;
    const char *eval_case;
    bool verify;
    bool self_test;
    bool p2b_producer_consumer;
};

struct async_request {
    struct obmm_async_future future;
    uint32_t buffer_slot;
    uint64_t ordinal;
    uint64_t offset;
    uint64_t submit_ns;
};

struct async_app;

struct async_p2b_trace_line {
    char text[ASYNC_P2B_TRACE_LINE_BYTES];
};

struct async_operation_trace {
    uint64_t ordinal;
    uint64_t offset;
    uint64_t latency_ns;
    uint32_t length;
    int status;
    bool remote;
};

struct async_worker {
    struct async_app *app;
    uint32_t worker_id;
    uint64_t begin;
    uint64_t end;
    uint64_t stride;
    uint64_t dependent_offset;
    uint64_t progress;
    uint64_t last_monotonic_ns;
    uint64_t context_id;
    uint64_t p2b_expected;
    uint64_t p2b_actual;
    uint64_t p2b_pending_upcalls;
    uint64_t p2b_complete_upcalls;
    uint64_t p2b_resumes_after_complete;
};

struct async_app {
    struct async_config config;
    struct obmm_async *runtime;
    struct obmm_async_map remote_map;
    struct obmm_scc *scc;
    struct obmm_scc_map scc_map;
    struct obmm_scc_metrics scc_metrics;
    struct obmm_async_observability_v1 observability;
    void *remote_address;
    void *local_address;
    struct obmm_async_buffer buffers[OBMM_ASYNC_QUEUE_DEPTH];
    bool buffer_busy[OBMM_ASYNC_QUEUE_DEPTH];
    struct async_worker workers[OBMM_ASYNC_MAX_COROUTINES];
    uint64_t *latencies_ns;
    uint64_t latency_count;
    uint64_t completed;
    uint64_t failures;
    uint64_t timeouts;
    uint64_t verify_failures;
    bool verify_failure_recorded;
    uint64_t checksum;
    uint64_t pending;
    uint64_t compute_steps;
    uint64_t compute_while_pending;
    uint64_t measurement_start_ns;
    uint64_t measurement_end_ns;
    uint64_t measurement_cpu_start_ns;
    uint64_t measurement_cpu_end_ns;
    atomic_uint_fast64_t operation_trace_next;
    struct async_operation_trace *operation_trace;
    uint64_t trace_sampled;
    uint64_t trace_dropped;
    uint32_t p2b_trace_count;
    uint32_t p2b_trace_dropped;
    struct async_p2b_trace_line p2b_trace[ASYNC_P2B_TRACE_LINES];
    struct obmm_uffd_metrics uffd_metrics;
};

static _Thread_local uint64_t async_last_monotonic_ns;
static _Thread_local uint64_t async_last_raw_ns;
static atomic_uint_fast64_t async_clock_regressions;
static sigjmp_buf async_sync_fault_environment;
static volatile sig_atomic_t async_sync_fault_armed;

static void async_sync_fault_handler(int signal_number)
{
    if (async_sync_fault_armed) {
        async_sync_fault_armed = 0;
        siglongjmp(async_sync_fault_environment, signal_number);
    }
    _exit(128 + signal_number);
}

static void async_compute(struct async_worker *worker);
static uint64_t async_request_offset(struct async_worker *worker,
                                     uint64_t iteration,
                                     uint64_t *random_state);

static uint64_t async_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    {
        uint64_t value = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;

        if (async_last_monotonic_ns && value < async_last_monotonic_ns) {
            atomic_fetch_add_explicit(&async_clock_regressions, 1,
                                      memory_order_relaxed);
        }
        async_last_monotonic_ns = value;
        return value;
    }
}

static uint64_t async_raw_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
        return 0;
    }
    {
        uint64_t value = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;

        if (async_last_raw_ns && value < async_last_raw_ns) {
            atomic_fetch_add_explicit(&async_clock_regressions, 1,
                                      memory_order_relaxed);
        }
        async_last_raw_ns = value;
        return value;
    }
}

static uint64_t async_worker_now_ns(struct async_worker *worker)
{
    struct timespec now;
    uint64_t value;

    if (!worker || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    value = (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
    if (worker->last_monotonic_ns &&
        value < worker->last_monotonic_ns) {
        atomic_fetch_add_explicit(&async_clock_regressions, 1,
                                  memory_order_relaxed);
    }
    worker->last_monotonic_ns = value;
    return value;
}

static int async_drain_observability(struct async_app *app)
{
    uint64_t timeout_ns = (uint64_t)app->config.deadline_us * 1000;
    uint64_t start_ns = async_raw_now_ns();
    struct timespec pause = {
        .tv_nsec = 100000,
    };

    if (!app->runtime) {
        return -EINVAL;
    }
    for (;;) {
        struct obmm_async_observability_v1 observability;
        int ret = obmm_async_get_observability(
            app->runtime, &observability);

        if (ret) {
            return ret;
        }
        if (!observability.model_pending &&
            !observability.backend_pending) {
            return 0;
        }
        ret = obmm_async_drain(app->runtime);
        if (ret < 0) {
            return ret;
        }
        if (!timeout_ns || async_raw_now_ns() - start_ns >= timeout_ns) {
            return -ETIMEDOUT;
        }
        nanosleep(&pause, NULL);
    }
}

static uint64_t async_process_now_ns(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static void async_measurement_begin(struct async_app *app)
{
    app->measurement_start_ns = async_now_ns();
    app->measurement_cpu_start_ns = async_process_now_ns();
}

static void async_measurement_end(struct async_app *app)
{
    app->measurement_cpu_end_ns = async_process_now_ns();
    app->measurement_end_ns = async_now_ns();
}

static uint64_t async_elapsed_ns(uint64_t start, uint64_t end)
{
    return end >= start ? end - start : 0;
}

static void async_trace_operation(
    struct async_app *app, uint64_t ordinal, bool remote,
    uint64_t offset, uint32_t length, int status, uint64_t latency_ns)
{
    struct async_operation_trace *trace;
    uint64_t draw;
    uint64_t slot;

    if (!app->config.trace_sample_ppm) {
        return;
    }
    draw = obmm_logical_splitmix64(
        app->config.seed ^ ordinal ^ 0x74726163655f7631ULL) % 1000000;
    if (draw >= app->config.trace_sample_ppm) {
        return;
    }
    slot = atomic_fetch_add_explicit(
        &app->operation_trace_next, 1, memory_order_relaxed);
    if (slot >= ASYNC_TRACE_SAMPLE_LIMIT || !app->operation_trace) {
        return;
    }
    trace = &app->operation_trace[slot];
    trace->ordinal = ordinal;
    trace->remote = remote;
    trace->offset = offset;
    trace->length = length;
    trace->status = status;
    trace->latency_ns = latency_ns;
}

static int async_prepare_operation_trace(struct async_app *app)
{
    if (!app->config.trace_sample_ppm) {
        return 0;
    }
    app->operation_trace = calloc(
        ASYNC_TRACE_SAMPLE_LIMIT, sizeof(*app->operation_trace));
    return app->operation_trace ? 0 : -ENOMEM;
}

static void async_flush_operation_trace(struct async_app *app)
{
    uint64_t reserved = atomic_load_explicit(
        &app->operation_trace_next, memory_order_acquire);
    uint64_t index;

    app->trace_sampled = reserved > ASYNC_TRACE_SAMPLE_LIMIT ?
        ASYNC_TRACE_SAMPLE_LIMIT : reserved;
    app->trace_dropped = reserved - app->trace_sampled;
    for (index = 0; index < app->trace_sampled; index++) {
        const struct async_operation_trace *trace =
            &app->operation_trace[index];

        printf("OBMM_OPERATION_TRACE schema=1 ordinal=%llu remote=%u "
               "offset=%llu length=%u status=%d latency_ns=%llu\n",
               (unsigned long long)trace->ordinal,
               trace->remote ? 1U : 0U,
               (unsigned long long)trace->offset, trace->length,
               trace->status,
               (unsigned long long)trace->latency_ns);
    }
    fflush(stdout);
    free(app->operation_trace);
    app->operation_trace = NULL;
}

static void async_uffd_trace(void *opaque, uint64_t ordinal,
                             uint64_t offset, int status,
                             uint64_t latency_ns)
{
    struct async_app *app = opaque;

    async_trace_operation(app, ordinal, true, offset, 4096,
                          status, latency_ns);
}

static void async_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s --mode sync-mmio|async-poll|async-irq|scheduler-core|"
            "userfaultfd "
            "[--coroutines N] [--inflight N] [--lookahead N] "
            "[--access-bytes 1|2|4|8|64|256|4096|65536] "
            "[--pattern sequential|random|dependent|mixed] "
            "[--uffd-case present-hit|missing-remote] "
            "[--worker-threads 1|2|4|8] [--handler-cpu N] "
            "[--pages N] "
            "[--case local-dram|obmm-local-hit|sync-remote-zero|"
            "sync-remote-modeled] [--warmup N] [--min-duration-ms N] "
            "[--trace-sample-ppm 0..1000000] "
            "[--expected-outcome success|error|drop-timeout|duplicate-late] "
            "[--compute-us N] [--iterations N] [--deadline-us N] "
            "[--seed N] [--node-count N] [--peer-index N] "
            "[--p2b-producer-consumer] [--producer-index N] [--verify]\n",
            program);
}

static bool async_parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;

    errno = 0;
    *value = strtoull(text, &end, 0);
    return errno == 0 && end && *end == '\0';
}

static bool async_parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed;

    if (!async_parse_u64(text, &parsed) || parsed > UINT32_MAX) {
        return false;
    }
    *value = parsed;
    return true;
}

static bool async_parse_args(int argc, char **argv,
                             struct async_config *config)
{
    int index;

    *config = (struct async_config) {
        .mode = ASYNC_APP_MODE_POLL,
        .pattern = ASYNC_PATTERN_SEQUENTIAL,
        .uffd_case = OBMM_UFFD_CASE_MISSING_REMOTE,
        .baseline_case = ASYNC_BASELINE_SYNC_REMOTE_MODELED,
        .expected_outcome = ASYNC_OUTCOME_SUCCESS,
        .coroutines = 8,
        .inflight = 32,
        .lookahead = 16,
        .access_bytes = 64,
        .compute_us = 10,
        .deadline_us = 1000000,
        .worker_threads = 1,
        .pages = 64,
        .iterations = 1024,
        .seed = 1,
        .node_count = 2,
        .peer_index = -1,
        .producer_index = 0,
        .handler_cpu = 0,
    };
    for (index = 1; index < argc; index++) {
        const char *option = argv[index];
        const char *value;

        if (strcmp(option, "--verify") == 0) {
            config->verify = true;
            continue;
        }
        if (strcmp(option, "--self-test") == 0) {
            config->self_test = true;
            continue;
        }
        if (strcmp(option, "--p2b-producer-consumer") == 0) {
            config->p2b_producer_consumer = true;
            continue;
        }
        if (index + 1 >= argc) {
            return false;
        }
        value = argv[++index];
        if (strcmp(option, "--mode") == 0) {
            if (strcmp(value, "sync-mmio") == 0) {
                config->mode = ASYNC_APP_MODE_SYNC;
            } else if (strcmp(value, "async-poll") == 0) {
                config->mode = ASYNC_APP_MODE_POLL;
            } else if (strcmp(value, "async-irq") == 0) {
                config->mode = ASYNC_APP_MODE_IRQ;
            } else if (strcmp(value, "scheduler-core") == 0) {
                config->mode = ASYNC_APP_MODE_SCHEDULER_CORE;
            } else if (strcmp(value, "userfaultfd") == 0) {
                config->mode = ASYNC_APP_MODE_USERFAULTFD;
            } else {
                return false;
            }
        } else if (strcmp(option, "--pattern") == 0) {
            if (strcmp(value, "sequential") == 0) {
                config->pattern = ASYNC_PATTERN_SEQUENTIAL;
            } else if (strcmp(value, "random") == 0) {
                config->pattern = ASYNC_PATTERN_RANDOM;
            } else if (strcmp(value, "dependent") == 0) {
                config->pattern = ASYNC_PATTERN_DEPENDENT;
            } else if (strcmp(value, "mixed") == 0) {
                config->pattern = ASYNC_PATTERN_MIXED;
            } else {
                return false;
            }
        } else if (strcmp(option, "--case") == 0) {
            if (strcmp(value, "local-dram") == 0) {
                config->baseline_case = ASYNC_BASELINE_LOCAL_DRAM;
            } else if (strcmp(value, "obmm-local-hit") == 0) {
                config->baseline_case = ASYNC_BASELINE_OBMM_LOCAL_HIT;
            } else if (strcmp(value, "sync-remote-zero") == 0) {
                config->baseline_case = ASYNC_BASELINE_SYNC_REMOTE_ZERO;
            } else if (strcmp(value, "sync-remote-modeled") == 0) {
                config->baseline_case = ASYNC_BASELINE_SYNC_REMOTE_MODELED;
            } else {
                return false;
            }
            config->option_flags |= ASYNC_OPTION_BASELINE_CASE;
        } else if (strcmp(option, "--expected-outcome") == 0) {
            if (strcmp(value, "success") == 0) {
                config->expected_outcome = ASYNC_OUTCOME_SUCCESS;
            } else if (strcmp(value, "error") == 0) {
                config->expected_outcome = ASYNC_OUTCOME_ERROR;
            } else if (strcmp(value, "drop-timeout") == 0) {
                config->expected_outcome = ASYNC_OUTCOME_DROP_TIMEOUT;
            } else if (strcmp(value, "duplicate-late") == 0) {
                config->expected_outcome = ASYNC_OUTCOME_DUPLICATE_LATE;
            } else {
                return false;
            }
        } else if (strcmp(option, "--coroutines") == 0) {
            if (!async_parse_u32(value, &config->coroutines)) {
                return false;
            }
            config->option_flags |= ASYNC_OPTION_COROUTINES;
        } else if (strcmp(option, "--inflight") == 0) {
            if (!async_parse_u32(value, &config->inflight)) {
                return false;
            }
            config->option_flags |= ASYNC_OPTION_INFLIGHT;
        } else if (strcmp(option, "--lookahead") == 0) {
            if (!async_parse_u32(value, &config->lookahead)) {
                return false;
            }
            config->option_flags |= ASYNC_OPTION_LOOKAHEAD;
        } else if (strcmp(option, "--access-bytes") == 0) {
            if (!async_parse_u32(value, &config->access_bytes)) {
                return false;
            }
        } else if (strcmp(option, "--compute-us") == 0) {
            if (!async_parse_u32(value, &config->compute_us)) {
                return false;
            }
            config->option_flags |= ASYNC_OPTION_COMPUTE_US;
        } else if (strcmp(option, "--uffd-case") == 0) {
            if (strcmp(value, "present-hit") == 0) {
                config->uffd_case = OBMM_UFFD_CASE_PRESENT_HIT;
            } else if (strcmp(value, "missing-remote") == 0) {
                config->uffd_case = OBMM_UFFD_CASE_MISSING_REMOTE;
            } else {
                return false;
            }
            config->option_flags |= ASYNC_OPTION_UFFD_CASE;
        } else if (strcmp(option, "--worker-threads") == 0) {
            if (!async_parse_u32(value, &config->worker_threads)) {
                return false;
            }
            config->option_flags |= ASYNC_OPTION_WORKER_THREADS;
        } else if (strcmp(option, "--handler-cpu") == 0) {
            uint32_t handler_cpu;

            if (!async_parse_u32(value, &handler_cpu) ||
                handler_cpu > INT32_MAX) {
                return false;
            }
            config->handler_cpu = handler_cpu;
            config->option_flags |= ASYNC_OPTION_HANDLER_CPU;
        } else if (strcmp(option, "--pages") == 0) {
            if (!async_parse_u32(value, &config->pages)) {
                return false;
            }
            config->option_flags |= ASYNC_OPTION_PAGES;
        } else if (strcmp(option, "--deadline-us") == 0) {
            if (!async_parse_u32(value, &config->deadline_us)) {
                return false;
            }
        } else if (strcmp(option, "--iterations") == 0) {
            if (!async_parse_u64(value, &config->iterations)) {
                return false;
            }
        } else if (strcmp(option, "--warmup") == 0) {
            if (!async_parse_u64(value, &config->warmup)) {
                return false;
            }
        } else if (strcmp(option, "--min-duration-ms") == 0) {
            if (!async_parse_u32(value, &config->min_duration_ms)) {
                return false;
            }
        } else if (strcmp(option, "--trace-sample-ppm") == 0) {
            if (!async_parse_u32(value, &config->trace_sample_ppm) ||
                config->trace_sample_ppm > 1000000) {
                return false;
            }
        } else if (strcmp(option, "--eval-band") == 0) {
            if (strcmp(value, "scalar") != 0 &&
                strcmp(value, "range") != 0) {
                return false;
            }
            config->eval_band = value;
        } else if (strcmp(option, "--eval-case") == 0) {
            if (!value[0]) {
                return false;
            }
            config->eval_case = value;
        } else if (strcmp(option, "--seed") == 0) {
            if (!async_parse_u64(value, &config->seed)) {
                return false;
            }
        } else if (strcmp(option, "--node-count") == 0) {
            uint32_t node_count;

            if (!async_parse_u32(value, &node_count) ||
                node_count > OBMM_POOL_HELPERS_MAX_NODES) {
                return false;
            }
            config->node_count = node_count;
        } else if (strcmp(option, "--peer-index") == 0) {
            uint32_t peer_index;

            if (!async_parse_u32(value, &peer_index) ||
                peer_index > INT32_MAX) {
                return false;
            }
            config->peer_index = peer_index;
        } else if (strcmp(option, "--producer-index") == 0) {
            uint32_t producer_index;

            if (!async_parse_u32(value, &producer_index) ||
                producer_index > INT32_MAX) {
                return false;
            }
            config->producer_index = producer_index;
        } else {
            return false;
        }
    }
    if (config->self_test) {
        return true;
    }
    if (!config->coroutines ||
        config->coroutines > OBMM_ASYNC_MAX_COROUTINES ||
        !config->inflight || config->inflight > OBMM_ASYNC_QUEUE_DEPTH ||
        config->lookahead > config->inflight ||
        !config->iterations || config->iterations > 100000000 ||
        config->node_count < 2 ||
        config->peer_index >= config->node_count ||
        config->producer_index >= config->node_count) {
        return false;
    }
    if (config->p2b_producer_consumer &&
        (config->mode != ASYNC_APP_MODE_SCHEDULER_CORE ||
         config->node_count != 2 || config->coroutines < 2 ||
         config->iterations != config->coroutines ||
         config->warmup != 0 || config->access_bytes != 8 ||
         config->pattern != ASYNC_PATTERN_SEQUENTIAL || !config->verify)) {
        return false;
    }
    if (config->access_bytes != 1 && config->access_bytes != 2 &&
        config->access_bytes != 4 && config->access_bytes != 8 &&
        config->access_bytes != 64 &&
        config->access_bytes != 256 && config->access_bytes != 4096 &&
        config->access_bytes != 65536) {
        return false;
    }
    if (config->mode == ASYNC_APP_MODE_SCHEDULER_CORE &&
        config->access_bytes > 8) {
        return false;
    }
    if (config->access_bytes == 4096 &&
        config->mode != ASYNC_APP_MODE_USERFAULTFD &&
        (ASYNC_EXPORT_BYTES / 4096) % config->coroutines) {
        return false;
    }
    if (config->mode == ASYNC_APP_MODE_USERFAULTFD) {
        uint32_t p2_only = ASYNC_OPTION_COROUTINES |
            ASYNC_OPTION_INFLIGHT | ASYNC_OPTION_LOOKAHEAD |
            ASYNC_OPTION_COMPUTE_US;

        if (config->option_flags & p2_only ||
            config->access_bytes != 4096 ||
            (config->pattern != ASYNC_PATTERN_SEQUENTIAL &&
             config->pattern != ASYNC_PATTERN_RANDOM) ||
            (config->worker_threads != 1 &&
             config->worker_threads != 2 &&
             config->worker_threads != 4 &&
             config->worker_threads != 8) ||
            !config->pages ||
            config->pages > ASYNC_EXPORT_BYTES / 4096) {
            return false;
        }
    } else if (config->option_flags &
               (ASYNC_OPTION_UFFD_CASE | ASYNC_OPTION_WORKER_THREADS |
                ASYNC_OPTION_HANDLER_CPU | ASYNC_OPTION_PAGES)) {
        return false;
    }
    if ((config->eval_band == NULL) != (config->eval_case == NULL)) {
        return false;
    }
    if (config->mode != ASYNC_APP_MODE_SYNC &&
        config->option_flags & ASYNC_OPTION_BASELINE_CASE) {
        return false;
    }
    if (config->mode == ASYNC_APP_MODE_SYNC &&
        config->option_flags &
            (ASYNC_OPTION_INFLIGHT | ASYNC_OPTION_LOOKAHEAD |
             ASYNC_OPTION_UFFD_CASE |
             ASYNC_OPTION_WORKER_THREADS | ASYNC_OPTION_HANDLER_CPU |
             ASYNC_OPTION_PAGES)) {
        return false;
    }
    if (config->mode == ASYNC_APP_MODE_SYNC &&
        config->option_flags & ASYNC_OPTION_COROUTINES &&
        !config->eval_case) {
        return false;
    }
    if (config->mode == ASYNC_APP_MODE_SYNC &&
        !(config->option_flags & ASYNC_OPTION_COROUTINES)) {
        config->coroutines = 1;
    }
    return config->access_bytes <= ASYNC_EXPORT_BYTES;
}

static const char *async_mode_name(enum async_app_mode mode)
{
    if (mode == ASYNC_APP_MODE_SYNC) {
        return "sync-mmio";
    }
    if (mode == ASYNC_APP_MODE_USERFAULTFD) {
        return "userfaultfd";
    }
    if (mode == ASYNC_APP_MODE_SCHEDULER_CORE) {
        return "scheduler-core";
    }
    return mode == ASYNC_APP_MODE_IRQ ? "async-irq" : "async-poll";
}

static const char *async_baseline_case_name(enum async_baseline_case test_case)
{
    switch (test_case) {
    case ASYNC_BASELINE_LOCAL_DRAM:
        return "local-dram";
    case ASYNC_BASELINE_OBMM_LOCAL_HIT:
        return "obmm-local-hit";
    case ASYNC_BASELINE_SYNC_REMOTE_ZERO:
        return "sync-remote-zero";
    case ASYNC_BASELINE_SYNC_REMOTE_MODELED:
        return "sync-remote-modeled";
    }
    return "invalid";
}

static enum obmm_async_mode async_split_phase_mode(enum async_app_mode mode)
{
    return mode == ASYNC_APP_MODE_IRQ ? OBMM_ASYNC_MODE_IRQ :
        OBMM_ASYNC_MODE_POLL;
}

static uint8_t async_pattern_byte(uint64_t seed, uint64_t offset)
{
    return (uint8_t)((seed + offset) * 0x9e3779b9U + 0x85ebca77U);
}

static void async_fill_export(void *address, uint64_t length, uint64_t seed)
{
    uint8_t *bytes = address;
    uint64_t offset;

    for (offset = 0; offset < length; offset++) {
        bytes[offset] = async_pattern_byte(seed, offset);
    }
}

static uint64_t async_p2b_offset(uint32_t coroutine_id)
{
    return ASYNC_P2B_VALUE_OFFSET_BASE +
        (uint64_t)coroutine_id * ASYNC_P2B_VALUE_OFFSET_STRIDE;
}

static uint64_t async_p2b_value(uint64_t seed, uint32_t coroutine_id)
{
    return 0xa11c000000000000ULL ^
        (seed * 0x9e3779b97f4a7c15ULL) ^ coroutine_id;
}

static int async_bootstrap_lookup_node(
    int obmm_fd, uint32_t local_cna, int node_count, uint64_t generation,
    uint32_t node_index, struct obmm_helpers_meta *meta)
{
    long deadline = obmm_now_ms() + OBMM_POOL_HELPERS_WAIT_IFACE_MS;

    while (obmm_now_ms() < deadline) {
        struct obmm_cmd_bootstrap_lookup command = {
            .generation = generation,
            .node_count = (uint32_t)node_count,
            .local_cna = local_cna,
        };
        uint32_t index;

        if (ioctl(obmm_fd, OBMM_CMD_BOOTSTRAP_LOOKUP, &command) != 0) {
            return -1;
        }
        for (index = 0; index < command.count; index++) {
            const struct obmm_bootstrap_record *record =
                &command.records[index];

            if (record->node_id != node_index) {
                continue;
            }
            *meta = (struct obmm_helpers_meta) {
                .export_mem_id = record->export_mem_id,
                .remote_uba = record->remote_uba,
                .size = record->size,
                .token_id = record->token_id,
                .export_cna = record->export_cna,
            };
            return 0;
        }
        usleep(100000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static struct async_worker *async_p2b_worker_by_context(
    struct async_app *app, uint64_t context_id)
{
    uint32_t index;

    for (index = 0; index < app->config.coroutines; index++) {
        if (app->workers[index].context_id == context_id) {
            return &app->workers[index];
        }
    }
    return NULL;
}

static void async_p2b_record(struct async_app *app, const char *format, ...)
{
    struct async_p2b_trace_line *line;
    va_list arguments;

    if (app->p2b_trace_count >= ASYNC_P2B_TRACE_LINES) {
        app->p2b_trace_dropped++;
        return;
    }
    line = &app->p2b_trace[app->p2b_trace_count++];
    va_start(arguments, format);
    if (vsnprintf(line->text, sizeof(line->text), format, arguments) < 0) {
        line->text[0] = '\0';
        app->p2b_trace_dropped++;
    }
    va_end(arguments);
}

static void async_p2b_flush_trace(const struct async_app *app)
{
    uint32_t index;

    for (index = 0; index < app->p2b_trace_count; index++) {
        puts(app->p2b_trace[index].text);
    }
    fflush(stdout);
}

static void async_p2b_scc_trace(
    void *opaque, const struct obmm_scc_trace_event *event)
{
    struct async_app *app = opaque;
    struct async_worker *worker = async_p2b_worker_by_context(
        app, event->context_id);
    uint32_t coroutine_id = worker ? worker->worker_id : UINT32_MAX;

    switch (event->kind) {
    case OBMM_SCC_TRACE_UPCALL_PENDING:
        if (worker) {
            worker->p2b_pending_upcalls++;
        }
        async_p2b_record(
            app,
            "OBMM_P2B_UPCALL schema=1 event=pending coroutine=%u "
            "context_id=%016llx sequence=%llu token=%016llx "
            "pc=%016llx bytes=%u rt=%u status=%u",
            coroutine_id, (unsigned long long)event->context_id,
            (unsigned long long)event->sequence,
            (unsigned long long)event->token,
            (unsigned long long)event->pc, event->access_bytes,
            event->rt, event->status);
        break;
    case OBMM_SCC_TRACE_UPCALL_COMPLETE:
        if (worker) {
            worker->p2b_complete_upcalls++;
        }
        async_p2b_record(
            app,
            "OBMM_P2B_UPCALL schema=1 event=complete coroutine=%u "
            "context_id=%016llx sequence=%llu token=%016llx "
            "pc=%016llx bytes=%u rt=%u value=%016llx status=%u",
            coroutine_id, (unsigned long long)event->context_id,
            (unsigned long long)event->sequence,
            (unsigned long long)event->token,
            (unsigned long long)event->pc, event->access_bytes,
            event->rt, (unsigned long long)event->value,
            event->status);
        break;
    case OBMM_SCC_TRACE_UPCALL_FAULT:
        async_p2b_record(
            app,
            "OBMM_P2B_UPCALL schema=1 event=fault coroutine=%u "
            "context_id=%016llx sequence=%llu token=%016llx "
            "pc=%016llx status=%u",
            coroutine_id, (unsigned long long)event->context_id,
            (unsigned long long)event->sequence,
            (unsigned long long)event->token,
            (unsigned long long)event->pc, event->status);
        break;
    case OBMM_SCC_TRACE_CONTEXT_RESUME:
        if (worker && worker->p2b_complete_upcalls) {
            worker->p2b_resumes_after_complete++;
        }
        async_p2b_record(
            app,
            "OBMM_P2B_SCHEDULE schema=1 event=resume "
            "from_context_id=%016llx to_context_id=%016llx "
            "to_coroutine=%u after_complete=%u",
            (unsigned long long)event->previous_context_id,
            (unsigned long long)event->context_id, coroutine_id,
            worker && worker->p2b_complete_upcalls ? 1U : 0U);
        break;
    case OBMM_SCC_TRACE_CONTEXT_DONE:
        async_p2b_record(
            app,
            "OBMM_P2B_SCHEDULE schema=1 event=done "
            "context_id=%016llx coroutine=%u",
            (unsigned long long)event->context_id, coroutine_id);
        break;
    }
}

static bool async_verify_payload(struct async_app *app, const void *address,
                                 uint32_t length, uint64_t offset,
                                 uint64_t ordinal)
{
    const uint8_t *bytes = address;
    uint32_t index;

    for (index = 0; index < length; index++) {
        uint8_t expected = async_pattern_byte(
            app->config.seed, offset + index);

        if (bytes[index] != expected) {
            if (!app->verify_failure_recorded) {
                app->verify_failure_recorded = true;
                fprintf(stderr,
                        "OBMM_VERIFY_FAILURE schema=1 mode=%s ordinal=%" PRIu64
                        " offset=%" PRIu64 " byte_index=%u expected=%02x"
                        " actual=%02x\n",
                        async_mode_name(app->config.mode), ordinal, offset,
                        index, expected, bytes[index]);
            }
            return false;
        }
    }
    return true;
}

static uint64_t async_payload_checksum(const void *address, uint32_t length)
{
    const uint8_t *bytes = address;
    uint64_t hash = 14695981039346656037ULL;
    uint32_t index;

    for (index = 0; index < length; index++) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static bool async_operation_remote(const struct async_worker *worker,
                                   uint64_t ordinal)
{
    return worker->app->config.pattern != ASYNC_PATTERN_MIXED ||
        (((ordinal - worker->begin) / worker->stride) & 1);
}

static __attribute__((noinline)) uint64_t async_scc_scalar_load(
    const volatile void *address, uint32_t access_bytes);

static int async_sync_read_one(struct async_app *app,
                               const struct obmm_async_map *remote_map,
                               const void *local_source, uint64_t offset,
                               uint64_t ordinal,
                               uint64_t operation_ordinal,
                               bool measurement)
{
    struct obmm_async_result result = { 0 };
    uint64_t before = async_raw_now_ns();
    uint64_t deadline_ns = 0;
    uint64_t checksum;
    int ret = 0;

    if (app->config.deadline_us) {
        deadline_ns = async_now_ns() +
            (uint64_t)app->config.deadline_us * 1000;
    }
    if (remote_map && measurement && app->config.access_bytes <= 8) {
        uint64_t value = 0;
        int fault_signal = 0;

        if (app->config.expected_outcome == ASYNC_OUTCOME_ERROR ||
            app->config.expected_outcome == ASYNC_OUTCOME_DROP_TIMEOUT) {
            fault_signal = sigsetjmp(async_sync_fault_environment, 1);
            if (!fault_signal) {
                async_sync_fault_armed = 1;
                value = async_scc_scalar_load(
                    (const volatile uint8_t *)app->remote_address + offset,
                    app->config.access_bytes);
                async_sync_fault_armed = 0;
            }
        } else {
            value = async_scc_scalar_load(
                (const volatile uint8_t *)app->remote_address + offset,
                app->config.access_bytes);
        }
        if (fault_signal) {
            uint64_t latency = async_raw_now_ns() - before;
            int status = app->config.expected_outcome ==
                ASYNC_OUTCOME_DROP_TIMEOUT ? -ETIMEDOUT : -EIO;

            app->failures++;
            if (status == -ETIMEDOUT) {
                app->timeouts++;
            }
            async_trace_operation(
                app, ordinal, true, offset, app->config.access_bytes,
                status, latency);
            return status;
        }

        /*
         * A modeled error must fail closed even when the simulated scalar
         * load retires with a poison/zero value instead of raising SIGBUS.
         * Record the architected outcome explicitly so the summary cannot
         * report status=fail with a zero failure count.
         */
        if (app->config.expected_outcome == ASYNC_OUTCOME_ERROR ||
            app->config.expected_outcome == ASYNC_OUTCOME_DROP_TIMEOUT) {
            uint64_t latency = async_raw_now_ns() - before;
            int status = app->config.expected_outcome ==
                ASYNC_OUTCOME_DROP_TIMEOUT ? -ETIMEDOUT : -EIO;

            app->failures++;
            if (status == -ETIMEDOUT) {
                app->timeouts++;
            }
            async_trace_operation(
                app, ordinal, true, offset, app->config.access_bytes,
                status, latency);
            return status;
        }

        memcpy(app->buffers[0].data, &value,
               app->config.access_bytes);
        checksum = async_payload_checksum(
            app->buffers[0].data, app->config.access_bytes);
    } else if (remote_map) {
        struct obmm_async_future future;

        ret = obmm_load_submit(
            app->runtime, remote_map, offset, &app->buffers[0], 0,
            app->config.access_bytes, deadline_ns, operation_ordinal,
            &future);
        if (!ret) {
            ret = obmm_await(app->runtime, &future, &result);
        }
        if (ret) {
            if (measurement) {
                app->failures++;
                if (result.status == OBMM_ASYNC_STATUS_TIMEOUT) {
                    app->timeouts++;
                }
            }
            return ret;
        }
        checksum = result.checksum64;
    } else {
        memcpy(app->buffers[0].data,
               (const uint8_t *)local_source + offset,
               app->config.access_bytes);
        checksum = async_payload_checksum(app->buffers[0].data,
                                          app->config.access_bytes);
    }
    if (measurement) {
        uint64_t latency = async_raw_now_ns() - before;

        if (app->latency_count < app->config.iterations) {
            app->latencies_ns[app->latency_count++] = latency;
        }
        app->completed++;
        app->checksum ^= checksum + ordinal;
        if (app->config.verify && !async_verify_payload(
                app, app->buffers[0].data, app->config.access_bytes,
                offset, ordinal)) {
            app->verify_failures++;
        }
        async_trace_operation(
            app, ordinal, remote_map != NULL, offset,
            app->config.access_bytes, 0, latency);
        async_compute(&app->workers[0]);
    }
    return 0;
}

static int async_run_sync_phase(struct async_app *app,
                                const struct obmm_async_map *remote_map,
                                const void *local_source, uint64_t operations,
                                bool measurement)
{
    uint64_t random_states[OBMM_ASYNC_MAX_COROUTINES];
    uint64_t ordinal;
    uint32_t index;

    for (index = 0; index < app->config.coroutines; index++) {
        struct async_worker *worker = &app->workers[index];

        *worker = (struct async_worker) {
            .app = app,
            .worker_id = index,
            .begin = index,
            .end = operations,
            .stride = app->config.coroutines,
            .dependent_offset = index,
        };
        random_states[index] = app->config.seed ^
            ((uint64_t)index << 32) ^ 1;
    }
    for (ordinal = 0; ordinal < operations; ordinal++) {
        struct async_worker *worker;
        const struct obmm_async_map *operation_map;
        uint64_t local_ordinal;
        uint64_t offset;
        int ret;

        index = ordinal % app->config.coroutines;
        worker = &app->workers[index];
        local_ordinal = ordinal / app->config.coroutines;
        offset = async_request_offset(
            worker, ordinal, &random_states[index]);
        operation_map = async_operation_remote(worker, ordinal) ?
            remote_map : NULL;
        ret = async_sync_read_one(
            app, operation_map, local_source, offset, ordinal,
            obmm_logical_remote_ordinal(
                index, local_ordinal, app->config.coroutines,
                app->config.pattern == ASYNC_PATTERN_MIXED),
            measurement);
        if (ret) {
            return ret;
        }
        if (app->config.pattern == ASYNC_PATTERN_DEPENDENT) {
            uint64_t value = 0;

            memcpy(&value, app->buffers[0].data,
                   app->config.access_bytes < sizeof(value) ?
                       app->config.access_bytes : sizeof(value));
            worker->dependent_offset = value;
        }
    }
    return 0;
}

static int async_run_sync_workload(struct async_app *app, int obmm_fd,
                                   uint64_t import_mem_id,
                                   void *export_address)
{
    struct obmm_async_map warm_map = { 0 };
    const struct obmm_async_map *measurement_map = NULL;
    const void *local_source = NULL;
    void *local_dram = MAP_FAILED;
    struct sigaction fault_action = { 0 };
    struct sigaction previous_bus = { 0 };
    struct sigaction previous_segv = { 0 };
    bool fault_handlers_installed = false;
    int ret;

    if (app->config.expected_outcome == ASYNC_OUTCOME_ERROR ||
        app->config.expected_outcome == ASYNC_OUTCOME_DROP_TIMEOUT) {
        fault_action.sa_handler = async_sync_fault_handler;
        sigemptyset(&fault_action.sa_mask);
        if (sigaction(SIGBUS, &fault_action, &previous_bus) != 0) {
            return -errno;
        }
        if (sigaction(SIGSEGV, &fault_action, &previous_segv) != 0) {
            int saved_errno = errno;

            sigaction(SIGBUS, &previous_bus, NULL);
            return -saved_errno;
        }
        fault_handlers_installed = true;
    }

    app->latencies_ns = calloc(app->config.iterations,
                               sizeof(*app->latencies_ns));
    if (!app->latencies_ns) {
        return -ENOMEM;
    }
    ret = obmm_async_buffer_alloc(app->runtime,
                                  app->config.access_bytes,
                                  &app->buffers[0]);
    if (ret) {
        return ret;
    }
    if (app->config.baseline_case == ASYNC_BASELINE_LOCAL_DRAM) {
        local_dram = mmap(NULL, ASYNC_EXPORT_BYTES,
                          PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (local_dram == MAP_FAILED) {
            return -errno;
        }
        async_fill_export(local_dram, ASYNC_EXPORT_BYTES,
                          app->config.seed);
        local_source = local_dram;
    } else if (app->config.baseline_case ==
               ASYNC_BASELINE_OBMM_LOCAL_HIT) {
        local_source = export_address;
    } else {
        measurement_map = &app->remote_map;
        local_source = export_address;
        if (app->config.warmup && obmm_async_map_register(
                app->runtime, obmm_fd, import_mem_id,
                app->remote_address, ASYNC_EXPORT_BYTES,
                &warm_map) != 0) {
            ret = -EIO;
            goto cleanup;
        }
    }

    ret = async_run_sync_phase(
        app, warm_map.id ? &warm_map : measurement_map,
        local_source, app->config.warmup, false);
    if (ret) {
        goto cleanup;
    }
    if (warm_map.id) {
        ret = obmm_async_map_unregister(app->runtime, &warm_map);
        if (ret) {
            goto cleanup;
        }
    }
    ret = async_drain_observability(app);
    if (ret) {
        goto cleanup;
    }
    ret = obmm_async_reset_observability(app->runtime);
    if (ret) {
        goto cleanup;
    }
    async_measurement_begin(app);
    ret = async_run_sync_phase(
        app, measurement_map, local_source, app->config.iterations, true);
    async_measurement_end(app);

cleanup:
    async_sync_fault_armed = 0;
    if (fault_handlers_installed) {
        sigaction(SIGBUS, &previous_bus, NULL);
        sigaction(SIGSEGV, &previous_segv, NULL);
    }
    if (warm_map.id) {
        obmm_async_map_unregister(app->runtime, &warm_map);
    }
    if (local_dram != MAP_FAILED) {
        munmap(local_dram, ASYNC_EXPORT_BYTES);
    }
    return ret;
}

static uint64_t async_xorshift64(uint64_t *state)
{
    uint64_t value = *state;

    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static int async_default_peer(int local_index, int node_count)
{
    return (local_index + 1) % node_count;
}

static bool async_parse_local_ipv4_index(int node_count, int *local_index)
{
    char local_ip[INET_ADDRSTRLEN];
    char *end;
    long octet;

    if (!obmm_env_or_cmdline("LINQU_UB_LOCAL_IP", "linqu_ipourma_ipv4",
                             local_ip, sizeof(local_ip))) {
        return false;
    }
    end = strrchr(local_ip, '.');
    if (!end || !end[1]) {
        return false;
    }
    errno = 0;
    octet = strtol(end + 1, &end, 10);
    if (errno || *end || octet < 1 || octet > node_count) {
        return false;
    }
    *local_index = octet - 1;
    return true;
}

static bool async_resolve_identity(uint64_t *local_cna, int *local_index,
                                   int node_count)
{
    char value[64];

    if (obmm_cmdline_get("linqu_cna", value, sizeof(value))) {
        *local_cna = strtoull(value, NULL, 0);
    } else if (!obmm_parse_hex_u64(
                   "/sys/bus/ub/devices/00001/primary_cna", local_cna)) {
        return false;
    }
    if (obmm_cmdline_get("linqu_node_idx", value, sizeof(value))) {
        *local_index = strtol(value, NULL, 0);
    } else if (!async_parse_local_ipv4_index(node_count, local_index)) {
        *local_index = 0;
    }
    return *local_index >= 0 && *local_index < node_count;
}

static int async_buffer_acquire(struct async_app *app)
{
    uint32_t slot;

    for (slot = 0; slot < app->config.inflight; slot++) {
        if (!app->buffer_busy[slot]) {
            app->buffer_busy[slot] = true;
            return slot;
        }
    }
    return -1;
}

static void async_compute(struct async_worker *worker)
{
    struct async_app *app = worker->app;
    uint64_t deadline = async_worker_now_ns(worker) +
        (uint64_t)app->config.compute_us * 1000;
    volatile uint64_t value = worker->progress + 1;

    do {
        value = value * 6364136223846793005ULL + 1;
    } while (async_worker_now_ns(worker) < deadline);
    worker->progress ^= value;
    worker->progress++;
    app->compute_steps++;
    if (app->pending) {
        app->compute_while_pending++;
    }
}

static uint64_t async_request_offset(struct async_worker *worker,
                                     uint64_t iteration,
                                     uint64_t *random_state)
{
    const struct async_config *config = &worker->app->config;
    uint64_t slots = ASYNC_EXPORT_BYTES / config->access_bytes;
    uint64_t slot;

    if (config->pattern == ASYNC_PATTERN_RANDOM) {
        slot = config->access_bytes == 4096 ?
            obmm_logical_worker_page(
                config->seed, worker->worker_id,
                (iteration - worker->begin) / worker->stride,
                config->coroutines, slots, true) :
            async_xorshift64(random_state) % slots;
    } else if (config->pattern == ASYNC_PATTERN_DEPENDENT) {
        slot = worker->dependent_offset % slots;
    } else if (config->access_bytes == 4096) {
        slot = obmm_logical_worker_page(
            config->seed, worker->worker_id,
            (iteration - worker->begin) / worker->stride,
            config->coroutines, slots, false);
    } else {
        slot = iteration % slots;
    }
    return slot * config->access_bytes;
}

static void async_split_phase_worker_entry(void *opaque)
{
    struct async_worker *worker = opaque;
    struct async_app *app = worker->app;
    struct async_request requests[OBMM_ASYNC_QUEUE_DEPTH];
    uint64_t random_state = app->config.seed ^
        ((uint64_t)worker->worker_id << 32) ^ 1;
    uint64_t next = worker->begin;
    uint32_t active = 0;
    uint32_t head = 0;
    uint32_t pipeline = app->config.pattern == ASYNC_PATTERN_DEPENDENT ||
            app->config.lookahead == 0 ?
        1 : app->config.lookahead;

    worker->dependent_offset = worker->begin;
    while (next < worker->end || active) {
        while (next < worker->end && active < pipeline) {
            struct async_request *request =
                &requests[(head + active) % OBMM_ASYNC_QUEUE_DEPTH];
            uint64_t deadline_ns = 0;
            int buffer_slot = async_buffer_acquire(app);
            int ret;

            if (buffer_slot < 0) {
                break;
            }
            request->buffer_slot = buffer_slot;
            request->ordinal = next;
            request->offset = async_request_offset(
                worker, next, &random_state);
            request->submit_ns = async_now_ns();
            if (!async_operation_remote(worker, next)) {
                uint64_t checksum;

                memcpy(app->buffers[buffer_slot].data,
                       (const uint8_t *)app->local_address +
                           request->offset,
                       app->config.access_bytes);
                checksum = async_payload_checksum(
                    app->buffers[buffer_slot].data,
                    app->config.access_bytes);
                app->latencies_ns[app->latency_count++] =
                    async_now_ns() - request->submit_ns;
                app->completed++;
                app->checksum ^= checksum + next;
                if (app->config.verify && !async_verify_payload(
                        app, app->buffers[buffer_slot].data,
                        app->config.access_bytes, request->offset, next)) {
                    app->verify_failures++;
                }
                async_trace_operation(
                    app, next, false, request->offset,
                    app->config.access_bytes, 0,
                    async_now_ns() - request->submit_ns);
                app->buffer_busy[buffer_slot] = false;
                async_compute(worker);
                next += worker->stride;
                continue;
            }
            if (app->config.deadline_us) {
                deadline_ns = request->submit_ns +
                    (uint64_t)app->config.deadline_us * 1000;
            }
            ret = obmm_load_submit(
                app->runtime, &app->remote_map, request->offset,
                &app->buffers[buffer_slot], 0,
                app->config.access_bytes, deadline_ns,
                obmm_logical_remote_ordinal(
                    worker->worker_id,
                    (next - worker->begin) / worker->stride,
                    app->config.coroutines,
                    app->config.pattern == ASYNC_PATTERN_MIXED),
                &request->future);
            if (ret) {
                app->buffer_busy[buffer_slot] = false;
                if (ret == -EAGAIN) {
                    break;
                }
                app->failures++;
                async_compute(worker);
                next += worker->stride;
                continue;
            }
            app->pending++;
            active++;
            async_compute(worker);
            next += worker->stride;
        }
        if (!active) {
            obmm_coroutine_yield(app->runtime);
            continue;
        }
        {
            struct async_request *request = &requests[head];
            struct obmm_async_result result = { 0 };
            int ret = obmm_await(app->runtime, &request->future, &result);
            uint64_t latency_ns = async_now_ns() - request->submit_ns;

            app->pending--;
            app->buffer_busy[request->buffer_slot] = false;
            if (app->latency_count < app->config.iterations) {
                app->latencies_ns[app->latency_count++] = latency_ns;
            }
            if (ret) {
                app->failures++;
                if (result.status == OBMM_ASYNC_STATUS_TIMEOUT) {
                    app->timeouts++;
                }
            } else {
                app->completed++;
                app->checksum ^= result.checksum64 + result.user_data;
                if (app->config.verify &&
                    !async_verify_payload(
                        app, app->buffers[request->buffer_slot].data,
                        app->config.access_bytes, request->offset,
                        request->ordinal)) {
                    app->verify_failures++;
                }
                if (app->config.pattern == ASYNC_PATTERN_DEPENDENT) {
                    uint64_t value = 0;

                    memcpy(&value,
                           app->buffers[request->buffer_slot].data,
                           app->config.access_bytes < sizeof(value) ?
                               app->config.access_bytes : sizeof(value));
                    worker->dependent_offset = value;
                }
            }
            async_trace_operation(
                app, request->ordinal, true, request->offset,
                app->config.access_bytes, ret, latency_ns);
            head = (head + 1) % OBMM_ASYNC_QUEUE_DEPTH;
            active--;
        }
    }
}

static __attribute__((noinline)) uint64_t async_scc_scalar_load(
    const volatile void *address, uint32_t access_bytes)
{
    switch (access_bytes) {
    case 1:
        return *(const volatile uint8_t *)address;
    case 2:
        return *(const volatile uint16_t *)address;
    case 4:
        return *(const volatile uint32_t *)address;
    case 8:
        return *(const volatile uint64_t *)address;
    default:
        return 0;
    }
}

static void async_scc_worker_entry(void *opaque)
{
    struct async_worker *worker = opaque;
    struct async_app *app = worker->app;
    uint64_t random_state = app->config.seed ^
        ((uint64_t)worker->worker_id << 32) ^ 1;
    uint64_t iteration;

    if (app->config.p2b_producer_consumer) {
        uint64_t offset = async_p2b_offset(worker->worker_id);
        const volatile void *address =
            (const volatile uint8_t *)app->remote_address + offset;
        uint64_t submit_ns = async_worker_now_ns(worker);
        uint64_t latency_ns;

        worker->p2b_expected = async_p2b_value(
            app->config.seed, worker->worker_id);
        async_p2b_record(
            app,
            "OBMM_P2B_LDR schema=1 event=issue coroutine=%u "
            "context_id=%016llx offset=%llu expected=%016llx",
            worker->worker_id,
            (unsigned long long)worker->context_id,
            (unsigned long long)offset,
            (unsigned long long)worker->p2b_expected);
        app->pending++;
        worker->p2b_actual = async_scc_scalar_load(address, 8);
        app->pending--;
        latency_ns = async_worker_now_ns(worker) - submit_ns;
        if (app->latency_count < app->config.iterations) {
            app->latencies_ns[app->latency_count++] = latency_ns;
        }
        app->completed++;
        app->checksum ^= async_payload_checksum(
            &worker->p2b_actual, sizeof(worker->p2b_actual)) +
            worker->worker_id;
        if (worker->p2b_actual != worker->p2b_expected) {
            app->verify_failures++;
        }
        async_p2b_record(
            app,
            "OBMM_P2B_LDR schema=1 event=retire coroutine=%u "
            "context_id=%016llx offset=%llu expected=%016llx "
            "actual=%016llx latency_ns=%llu status=%s",
            worker->worker_id,
            (unsigned long long)worker->context_id,
            (unsigned long long)offset,
            (unsigned long long)worker->p2b_expected,
            (unsigned long long)worker->p2b_actual,
            (unsigned long long)latency_ns,
            worker->p2b_actual == worker->p2b_expected ?
                "pass" : "fail");
        return;
    }

    worker->dependent_offset = worker->begin;
    for (iteration = worker->begin; iteration < worker->end;
         iteration += worker->stride) {
        uint64_t offset = async_request_offset(
            worker, iteration, &random_state);
        const volatile void *address =
            (const volatile uint8_t *)(
                async_operation_remote(worker, iteration) ?
                    app->remote_address : app->local_address) + offset;
        uint64_t submit_ns = async_worker_now_ns(worker);
        uint64_t value;
        uint8_t payload[sizeof(value)] = { 0 };
        uint64_t latency_ns;

        app->pending++;
        value = async_scc_scalar_load(address, app->config.access_bytes);
        app->pending--;
        latency_ns = async_worker_now_ns(worker) - submit_ns;
        memcpy(payload, &value, app->config.access_bytes);
        if (app->latency_count < app->config.iterations) {
            app->latencies_ns[app->latency_count++] = latency_ns;
        }
        app->completed++;
        app->checksum ^=
            async_payload_checksum(payload, app->config.access_bytes) +
            iteration;
        if (app->config.verify &&
            !async_verify_payload(app, payload, app->config.access_bytes,
                                  offset, iteration)) {
            app->verify_failures++;
        }
        async_trace_operation(
            app, iteration,
            async_operation_remote(worker, iteration), offset,
            app->config.access_bytes, 0, latency_ns);
        if (app->config.pattern == ASYNC_PATTERN_DEPENDENT) {
            worker->dependent_offset = value;
        }
        async_compute(worker);
    }
}

static int async_compare_u64(const void *left, const void *right)
{
    uint64_t a = *(const uint64_t *)left;
    uint64_t b = *(const uint64_t *)right;

    return (a > b) - (a < b);
}

static uint64_t async_percentile_us(uint64_t *values, uint64_t count,
                                    uint32_t percentile)
{
    uint64_t index;

    if (!count) {
        return 0;
    }
    qsort(values, count, sizeof(*values), async_compare_u64);
    index = ((count - 1) * percentile) / 100;
    return values[index] / 1000;
}

static uint64_t async_percentile_ns(uint64_t *values, uint64_t count,
                                    uint32_t percentile)
{
    uint64_t index;

    if (!count) {
        return 0;
    }
    qsort(values, count, sizeof(*values), async_compare_u64);
    index = ((count - 1) * percentile) / 100;
    return values[index];
}

static void async_print_eval_summary(const struct async_app *app,
                                     const struct obmm_async_metrics *metrics,
                                     const char *run_status,
                                     uint64_t makespan_ns,
                                     uint64_t process_cpu_ns,
                                     uint64_t latency_p50_ns,
                                     uint64_t latency_p95_ns,
                                     uint64_t latency_p99_ns,
                                     uint64_t latency_max_ns)
{
    uint64_t minimum_ns = (uint64_t)app->config.min_duration_ms *
        1000000;
    const char *status = run_status;
    uint64_t helper_cpu_ns = app->uffd_metrics.handler_cpu_ns;
    uint64_t backend_pending = app->config.mode ==
        ASYNC_APP_MODE_SCHEDULER_CORE ?
        app->scc_metrics.observability.backend_pending_current :
        app->observability.backend_pending;
    uint64_t backend_pending_high = app->config.mode ==
        ASYNC_APP_MODE_SCHEDULER_CORE ?
        app->scc_metrics.observability.backend_pending_high_water :
        app->observability.backend_pending_high_water;
    uint64_t backend_capacity = app->config.mode ==
        ASYNC_APP_MODE_SCHEDULER_CORE ?
        app->scc_metrics.observability.backend_capacity :
        app->observability.backend_capacity;
    uint64_t sink_copy_bytes = app->config.mode ==
        ASYNC_APP_MODE_SCHEDULER_CORE ?
        app->scc_metrics.observability.backend_sink_copy_bytes :
        app->observability.backend_sink_copy_bytes;
    uint64_t sink_copy_ns = app->config.mode ==
        ASYNC_APP_MODE_SCHEDULER_CORE ?
        app->scc_metrics.observability.backend_sink_copy_ns :
        app->observability.backend_sink_copy_ns;
    uint64_t backend_late = app->config.mode ==
        ASYNC_APP_MODE_SCHEDULER_CORE ?
        app->scc_metrics.observability.backend_late :
        app->observability.backend_late;
    uint64_t backend_duplicate = app->config.mode ==
        ASYNC_APP_MODE_SCHEDULER_CORE ?
        app->scc_metrics.observability.backend_duplicate :
        app->observability.backend_duplicate;
    uint64_t scc_pending =
        app->scc_metrics.observability.scc_pending_current;
    uint64_t counter_overflow =
        app->completed == UINT64_MAX || app->failures == UINT64_MAX ||
        app->timeouts == UINT64_MAX || backend_pending_high == UINT64_MAX ||
        sink_copy_bytes == UINT64_MAX || sink_copy_ns == UINT64_MAX;

    if (!app->config.eval_case) {
        return;
    }
    if (app->config.mode == ASYNC_APP_MODE_SCHEDULER_CORE &&
        app->scc_metrics.clock_mhz) {
        helper_cpu_ns = app->scc_metrics.el0_scheduler_ns;
    }
    if (strcmp(status, "pass") == 0 && minimum_ns &&
        makespan_ns < minimum_ns) {
        status = "invalid";
    }
    printf("OBMM_EVAL_SUMMARY schema=1 band=%s mode=%s case=%s seed=%llu "
           "operations=%llu checksum=%016llx failures=%llu timeouts=%llu "
           "guest_ns_p50=%llu guest_ns_p95=%llu guest_ns_p99=%llu "
           "guest_ns_max=%llu makespan_ns=%llu model_wait_ns=%llu "
           "useful_work_ns=%llu application_cpu_ns=%llu "
           "helper_cpu_ns=%llu extra_vcpus=%u trace_sample_ppm=%u "
           "trace_sampled=%llu trace_dropped=%llu "
           "ready_ns=%llu wait_ns=%llu idle_ns=%llu no_ready=%llu "
           "submit_ns_p50=%llu submit_ns_total=%llu "
           "switch_ns_p50=%llu switch_ns_total=%llu "
           "cq_drain_ns_p50=%llu cq_drain_ns_total=%llu "
           "configured_lookahead=%u backend_pending_high=%llu "
           "backend_capacity=%llu sink_copy_bytes=%llu sink_copy_ns=%llu "
           "backend_late=%llu backend_duplicate=%llu "
           "scc_save_cycles=%llu scc_schedule_cycles=%llu "
           "scc_restore_cycles=%llu scc_commit_cycles=%llu "
           "el0_upcalls_pending=%llu el0_upcalls_complete=%llu "
           "el0_upcalls_fault=%llu el0_context_saves=%llu "
           "el0_context_restores=%llu el0_context_switches=%llu "
           "el0_context_bytes=%llu el0_scheduler_ns=%llu "
           "el0_no_ready_waits=%llu direct_el0_upcalls=%llu "
           "qemu_context_saves=%llu qemu_context_restores=%llu "
           "qemu_context_switches=%llu qemu_context_bytes=%llu "
           "uffd_fault_ns_p50=%llu uffd_fault_ns_p95=%llu "
           "uffd_fault_ns_p99=%llu uffd_fault_ns_max=%llu "
           "uffd_remote_ns_p50=%llu uffd_remote_ns_p95=%llu "
           "uffd_remote_ns_p99=%llu uffd_remote_ns_max=%llu "
           "uffd_copy_ns_p50=%llu uffd_copy_ns_p95=%llu "
           "uffd_copy_ns_p99=%llu uffd_copy_ns_max=%llu "
           "uffd_wake_ns_p50=%llu uffd_wake_ns_p95=%llu "
           "uffd_wake_ns_p99=%llu uffd_wake_ns_max=%llu "
           "uffd_handler_cpu_ns=%llu uffd_worker_cpu_ns=%llu "
           "model_pending_final=%llu backend_pending_final=%llu "
           "scc_pending_final=%llu counter_overflow=%llu "
           "clock_regressions=%llu fail_closed_process_exit=0 status=%s\n",
           app->config.eval_band, async_mode_name(app->config.mode),
           app->config.eval_case,
           (unsigned long long)app->config.seed,
           (unsigned long long)app->completed,
           (unsigned long long)app->checksum,
           (unsigned long long)app->failures,
           (unsigned long long)app->timeouts,
           (unsigned long long)latency_p50_ns,
           (unsigned long long)latency_p95_ns,
           (unsigned long long)latency_p99_ns,
           (unsigned long long)latency_max_ns,
           (unsigned long long)makespan_ns,
           (unsigned long long)app->observability.model_service_ns,
           (unsigned long long)(app->compute_steps *
                                app->config.compute_us * 1000ULL),
           (unsigned long long)process_cpu_ns,
           (unsigned long long)helper_cpu_ns,
           (unsigned int)(
               app->config.mode == ASYNC_APP_MODE_USERFAULTFD),
           app->config.trace_sample_ppm,
           (unsigned long long)app->trace_sampled,
           (unsigned long long)app->trace_dropped,
           (unsigned long long)metrics->ready_ns,
           (unsigned long long)metrics->wait_ns,
           (unsigned long long)metrics->idle_ns,
           (unsigned long long)metrics->no_ready,
           (unsigned long long)metrics->submit_ns_p50,
           (unsigned long long)metrics->submit_ns_total,
           (unsigned long long)metrics->switch_ns_p50,
           (unsigned long long)metrics->switch_ns_total,
           (unsigned long long)metrics->cq_drain_ns_p50,
           (unsigned long long)metrics->cq_drain_ns_total,
           app->config.mode == ASYNC_APP_MODE_POLL ||
               app->config.mode == ASYNC_APP_MODE_IRQ ?
               app->config.lookahead : 0,
           (unsigned long long)backend_pending_high,
           (unsigned long long)backend_capacity,
           (unsigned long long)sink_copy_bytes,
           (unsigned long long)sink_copy_ns,
           (unsigned long long)backend_late,
           (unsigned long long)backend_duplicate,
           (unsigned long long)app->scc_metrics.observability.save_cycles,
           (unsigned long long)app->scc_metrics.observability.schedule_cycles,
           (unsigned long long)app->scc_metrics.observability.restore_cycles,
           (unsigned long long)app->scc_metrics.observability.commit_cycles,
           (unsigned long long)app->scc_metrics.el0_pending_upcalls,
           (unsigned long long)app->scc_metrics.el0_complete_upcalls,
           (unsigned long long)app->scc_metrics.el0_fault_upcalls,
           (unsigned long long)app->scc_metrics.el0_context_saves,
           (unsigned long long)app->scc_metrics.el0_context_restores,
           (unsigned long long)app->scc_metrics.el0_context_switches,
           (unsigned long long)app->scc_metrics.el0_context_bytes,
           (unsigned long long)app->scc_metrics.el0_scheduler_ns,
           (unsigned long long)app->scc_metrics.el0_no_ready_waits,
           (unsigned long long)app->scc_metrics.device.direct_upcalls,
           (unsigned long long)app->scc_metrics.device.context_saves,
           (unsigned long long)app->scc_metrics.device.context_restores,
           (unsigned long long)app->scc_metrics.device.context_switches,
           (unsigned long long)app->scc_metrics.device.context_bytes_moved,
           (unsigned long long)app->uffd_metrics.fault_ns_p50,
           (unsigned long long)app->uffd_metrics.fault_ns_p95,
           (unsigned long long)app->uffd_metrics.fault_ns_p99,
           (unsigned long long)app->uffd_metrics.fault_ns_max,
           (unsigned long long)app->uffd_metrics.remote_ns_p50,
           (unsigned long long)app->uffd_metrics.remote_ns_p95,
           (unsigned long long)app->uffd_metrics.remote_ns_p99,
           (unsigned long long)app->uffd_metrics.remote_ns_max,
           (unsigned long long)app->uffd_metrics.copy_ns_p50,
           (unsigned long long)app->uffd_metrics.copy_ns_p95,
           (unsigned long long)app->uffd_metrics.copy_ns_p99,
           (unsigned long long)app->uffd_metrics.copy_ns_max,
           (unsigned long long)app->uffd_metrics.wake_ns_p50,
           (unsigned long long)app->uffd_metrics.wake_ns_p95,
           (unsigned long long)app->uffd_metrics.wake_ns_p99,
           (unsigned long long)app->uffd_metrics.wake_ns_max,
           (unsigned long long)app->uffd_metrics.handler_cpu_ns,
           (unsigned long long)app->uffd_metrics.worker_cpu_ns,
           (unsigned long long)app->observability.model_pending,
           (unsigned long long)backend_pending,
           (unsigned long long)scc_pending,
           (unsigned long long)counter_overflow,
           (unsigned long long)atomic_load_explicit(
               &async_clock_regressions, memory_order_relaxed),
           status);
}

static bool async_completion_barrier(
    int obmm_fd, int local_index, uint32_t local_cna, int node_count,
    const struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = { false };

    return obmm_bootstrap_publish(
               obmm_fd, local_index, node_count,
               ASYNC_BOOTSTRAP_GENERATION + 1, local_meta) == 0 &&
        obmm_bootstrap_lookup(
               obmm_fd, local_cna, node_count,
               ASYNC_BOOTSTRAP_GENERATION + 1, metas, got) == 0;
}

static int async_prepare_workers(struct async_app *app)
{
    uint32_t index;

    app->latencies_ns = calloc(app->config.iterations,
                               sizeof(*app->latencies_ns));
    if (!app->latencies_ns) {
        return -ENOMEM;
    }
    for (index = 0; index < app->config.coroutines; index++) {
        struct async_worker *worker = &app->workers[index];

        *worker = (struct async_worker) {
            .app = app,
            .worker_id = index,
            .begin = index,
            .end = app->config.iterations,
            .stride = app->config.coroutines,
        };
    }
    return 0;
}

static int async_run_split_phase_workload(struct async_app *app)
{
    uint32_t index;
    int ret;

    ret = async_prepare_workers(app);
    if (ret) {
        return ret;
    }
    for (index = 0; index < app->config.inflight; index++) {
        ret = obmm_async_buffer_alloc(app->runtime,
                                      app->config.access_bytes,
                                      &app->buffers[index]);
        if (ret) {
            return ret;
        }
    }
    for (index = 0; index < app->config.coroutines; index++) {
        uint64_t coroutine_id;

        ret = obmm_coroutine_create(
            app->runtime, async_split_phase_worker_entry,
            &app->workers[index],
            ASYNC_COROUTINE_STACK_BYTES, &coroutine_id);
        if (ret) {
            return ret;
        }
    }
    return obmm_coroutine_run(app->runtime);
}

static int async_run_uffd_once(struct async_app *app,
                               const struct obmm_async_map *remote_map,
                               uint64_t iterations,
                               struct obmm_uffd_metrics *uffd_metrics)
{
    struct obmm_uffd_run_config config = {
        .test_case = app->config.uffd_case,
        .remote_runtime = app->runtime,
        .remote_map = remote_map,
        .source_base = app->remote_address,
        .source_length = ASYNC_EXPORT_BYTES,
        .pages = app->config.pages,
        .worker_threads = app->config.worker_threads,
        .handler_cpu = app->config.handler_cpu,
        .iterations = iterations,
        .seed = app->config.seed,
        .deadline_us = app->config.deadline_us,
        .trace_sample_ppm = app->config.trace_sample_ppm,
        .random_pattern = app->config.pattern == ASYNC_PATTERN_RANDOM,
        .verify = app->config.verify,
        .trace = app->config.trace_sample_ppm ? async_uffd_trace : NULL,
        .trace_opaque = app,
    };
    int ret;

    ret = obmm_async_buffer_alloc(app->runtime, 4096,
                                  &app->buffers[0]);
    if (ret) {
        return ret;
    }
    config.staging_buffer = &app->buffers[0];
    ret = obmm_uffd_run(&config, uffd_metrics);
    if (ret == -EOPNOTSUPP) {
        uffd_metrics->pages = app->config.pages;
    }
    return ret;
}

static int async_pin_current_cpu(void)
{
    cpu_set_t affinity;
    int cpu = sched_getcpu();

    if (cpu < 0) {
        return -errno;
    }
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    return sched_setaffinity(0, sizeof(affinity), &affinity) == 0 ?
        0 : -errno;
}

static int async_run_scc_workload(struct async_app *app)
{
    struct obmm_scc_caps_v2 caps;
    uint32_t index;
    int ret;

    ret = async_pin_current_cpu();
    if (ret) {
        return ret;
    }
    ret = obmm_scc_get_caps(app->scc, &caps);
    if (ret || app->config.coroutines > caps.context_entries) {
        return ret ? ret : -ENOSPC;
    }
    ret = async_prepare_workers(app);
    if (ret) {
        return ret;
    }
    for (index = 0; index < app->config.coroutines; index++) {
        uint64_t context_id;

        ret = obmm_scc_context_create(
            app->scc, async_scc_worker_entry, &app->workers[index],
            ASYNC_COROUTINE_STACK_BYTES, 0, &context_id);
        if (ret) {
            return ret;
        }
        app->workers[index].context_id = context_id;
        if (app->config.p2b_producer_consumer) {
            async_p2b_record(
                app,
                "OBMM_P2B_CONTEXT schema=1 coroutine=%u "
                "context_id=%016llx state=ready",
                index, (unsigned long long)context_id);
        }
    }
    ret = obmm_scc_run(app->scc);
    obmm_scc_get_metrics(app->scc, &app->scc_metrics);
    app->failures += app->scc_metrics.el0_fault_upcalls;
    app->timeouts += app->scc_metrics.el0_timeout_faults;
    return ret;
}

static void async_free_buffers(struct async_app *app)
{
    uint32_t index;

    if (!app->runtime) {
        return;
    }
    for (index = 0; index < app->config.inflight; index++) {
        if (app->buffers[index].id) {
            obmm_async_buffer_free(app->runtime, &app->buffers[index]);
        }
    }
}

static void async_reset_workload_state(struct async_app *app)
{
    async_free_buffers(app);
    free(app->latencies_ns);
    app->latencies_ns = NULL;
    app->latency_count = 0;
    app->completed = 0;
    app->failures = 0;
    app->timeouts = 0;
    app->verify_failures = 0;
    app->verify_failure_recorded = false;
    app->checksum = 0;
    app->pending = 0;
    app->compute_steps = 0;
    app->compute_while_pending = 0;
    atomic_store_explicit(
        &app->operation_trace_next, 0, memory_order_relaxed);
    app->trace_sampled = 0;
    app->trace_dropped = 0;
    memset(app->buffer_busy, 0, sizeof(app->buffer_busy));
    memset(app->workers, 0, sizeof(app->workers));
    memset(&app->uffd_metrics, 0, sizeof(app->uffd_metrics));
}

static int async_run_split_phase_with_warmup(
    struct async_app *app, int obmm_fd, uint64_t import_mem_id)
{
    struct obmm_async_map measurement_map = app->remote_map;
    struct obmm_async_map warm_map = { 0 };
    uint64_t iterations = app->config.iterations;
    uint32_t trace_sample_ppm = app->config.trace_sample_ppm;
    int ret;

    if (app->config.warmup) {
        ret = obmm_async_map_register(
            app->runtime, obmm_fd, import_mem_id, app->remote_address,
            ASYNC_EXPORT_BYTES, &warm_map);
        if (ret) {
            return ret;
        }
        app->remote_map = warm_map;
        app->config.iterations = app->config.warmup;
        app->config.trace_sample_ppm = 0;
        ret = async_run_split_phase_workload(app);
        app->config.trace_sample_ppm = trace_sample_ppm;
        async_reset_workload_state(app);
        if (obmm_async_map_unregister(app->runtime, &warm_map) != 0 &&
            !ret) {
            ret = -EIO;
        }
        app->remote_map = measurement_map;
        app->config.iterations = iterations;
        if (ret) {
            return ret;
        }
    }
    ret = async_drain_observability(app);
    if (ret) {
        return ret;
    }
    ret = obmm_async_reset_observability(app->runtime);
    if (ret) {
        return ret;
    }
    async_measurement_begin(app);
    ret = async_run_split_phase_workload(app);
    async_measurement_end(app);
    return ret;
}

static int async_run_uffd_with_warmup(
    struct async_app *app, int obmm_fd, uint64_t import_mem_id)
{
    struct obmm_async_map warm_map = { 0 };
    struct obmm_uffd_metrics warm_metrics = { 0 };
    uint32_t trace_sample_ppm = app->config.trace_sample_ppm;
    int ret;

    if (app->config.warmup) {
        ret = obmm_async_map_register(
            app->runtime, obmm_fd, import_mem_id, app->remote_address,
            ASYNC_EXPORT_BYTES, &warm_map);
        if (ret) {
            return ret;
        }
        app->config.trace_sample_ppm = 0;
        ret = async_run_uffd_once(
            app, &warm_map, app->config.warmup, &warm_metrics);
        app->config.trace_sample_ppm = trace_sample_ppm;
        async_reset_workload_state(app);
        if (obmm_async_map_unregister(app->runtime, &warm_map) != 0 &&
            !ret) {
            ret = -EIO;
        }
        if (ret) {
            return ret;
        }
    }
    ret = async_drain_observability(app);
    if (ret) {
        return ret;
    }
    ret = obmm_async_reset_observability(app->runtime);
    if (ret) {
        return ret;
    }
    async_measurement_begin(app);
    ret = async_run_uffd_once(
        app, &app->remote_map, app->config.iterations,
        &app->uffd_metrics);
    async_measurement_end(app);
    app->failures = app->uffd_metrics.failures;
    app->checksum = app->uffd_metrics.checksum;
    if (!ret) {
        app->completed = app->config.iterations;
    }
    return ret;
}

static int async_run_scc_once(struct async_app *app,
                              const struct obmm_scc_options *options,
                              int mapping_fd, uint64_t import_mem_id,
                              uint64_t iterations,
                              uint64_t model_phase_generation)
{
    uint64_t saved_iterations = app->config.iterations;
    int ret;

    app->config.iterations = iterations;
    ret = obmm_scc_open(&app->scc, options);
    if (!ret) {
        ret = obmm_scc_register_map_for_phase(
            app->scc, mapping_fd, import_mem_id, app->remote_address,
            ASYNC_EXPORT_BYTES,
            app->config.pattern == ASYNC_PATTERN_MIXED ?
                OBMM_SCC_MAP_LOGICAL_MIXED : 0,
            model_phase_generation,
            &app->scc_map);
    }
    if (!ret) {
        ret = async_run_scc_workload(app);
    }
    if (app->scc && app->scc_map.policy_id) {
        int unregister_ret = obmm_scc_unregister_map(
            app->scc, &app->scc_map);

        if (!ret && unregister_ret) {
            ret = unregister_ret;
        }
    }
    obmm_scc_close(app->scc);
    app->scc = NULL;
    app->config.iterations = saved_iterations;
    return ret;
}

static int async_run_scc_with_warmup(
    struct async_app *app, const struct obmm_scc_options *options,
    int mapping_fd, uint64_t import_mem_id)
{
    uint32_t trace_sample_ppm = app->config.trace_sample_ppm;
    int ret;

    if (app->config.warmup) {
        app->config.trace_sample_ppm = 0;
        ret = async_run_scc_once(
            app, options, mapping_fd, import_mem_id,
            app->config.warmup, 2);
        app->config.trace_sample_ppm = trace_sample_ppm;
        async_reset_workload_state(app);
        memset(&app->scc_metrics, 0, sizeof(app->scc_metrics));
        if (ret) {
            return ret;
        }
    }
    ret = async_drain_observability(app);
    if (ret) {
        return ret;
    }
    ret = obmm_async_reset_observability(app->runtime);
    if (ret) {
        return ret;
    }
    async_measurement_begin(app);
    ret = async_run_scc_once(
        app, options, mapping_fd, import_mem_id,
        app->config.iterations, 1);
    async_measurement_end(app);
    return ret;
}

static int async_run_p2b_producer(struct async_app *app, int obmm_fd,
                                  uint32_t local_cna, int local_index)
{
    struct obmm_helpers_meta meta = {
        .export_cna = local_cna,
    };
    struct obmm_helpers_region region = {
        .fd = -1,
    };
    uint32_t index;

    if (obmm_do_export(obmm_fd, &meta, ASYNC_EXPORT_BYTES) != 0) {
        return -errno;
    }
    if (obmm_map_region(meta.export_mem_id, ASYNC_EXPORT_BYTES, false,
                        &region) != 0) {
        int error = -errno;

        obmm_do_unexport(obmm_fd, meta.export_mem_id);
        return error;
    }
    memset(region.addr, 0, ASYNC_EXPORT_BYTES);
    for (index = 0; index < app->config.coroutines; index++) {
        uint64_t offset = async_p2b_offset(index);
        uint64_t value = async_p2b_value(app->config.seed, index);

        *(volatile uint64_t *)((uint8_t *)region.addr + offset) = value;
        printf("OBMM_P2B_WRITE schema=1 producer_node=%d coroutine=%u "
               "export_mem_id=%llu offset=%llu value=%016llx\n",
               local_index, index,
               (unsigned long long)meta.export_mem_id,
               (unsigned long long)offset, (unsigned long long)value);
    }
    __sync_synchronize();
    if (obmm_bootstrap_publish(
            obmm_fd, local_index, app->config.node_count,
            ASYNC_BOOTSTRAP_GENERATION, &meta) != 0) {
        int error = -errno;

        obmm_unmap_region(&region);
        obmm_do_unexport(obmm_fd, meta.export_mem_id);
        return error;
    }
    printf("OBMM_P2B_EXPORT schema=1 role=producer node=%d "
           "export_mem_id=%llu remote_uba=%016llx bytes=%llu writes=%u "
           "status=ready\n",
           local_index, (unsigned long long)meta.export_mem_id,
           (unsigned long long)meta.remote_uba,
           (unsigned long long)meta.size, app->config.coroutines);
    fflush(stdout);

    for (;;) {
        pause();
    }
}

static int async_run_p2b_consumer(struct async_app *app, int obmm_fd,
                                  uint32_t local_cna, int local_index)
{
    struct obmm_helpers_meta producer_meta = { 0 };
    struct obmm_helpers_region import_region = {
        .fd = -1,
    };
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    uint64_t local_pas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    struct obmm_async_options async_options = {
        .device_path = OBMM_ASYNC_DEFAULT_DEVICE,
        .mode = OBMM_ASYNC_MODE_POLL,
        .spin_us = 10,
    };
    struct obmm_scc_options scc_options = {
        .device_path = OBMM_SCC_DEFAULT_DEVICE,
        .load_timeout_ns = (uint64_t)app->config.deadline_us * 1000,
        .trace = async_p2b_scc_trace,
        .trace_opaque = app,
    };
    uint64_t import_mem_id = 0;
    uint32_t verified = 0;
    uint32_t index;
    int ret;
    bool pass;

    if (async_bootstrap_lookup_node(
            obmm_fd, local_cna, app->config.node_count,
            ASYNC_BOOTSTRAP_GENERATION, app->config.producer_index,
            &producer_meta) != 0) {
        ret = -errno;
        goto cleanup;
    }
    if (producer_meta.size < async_p2b_offset(
            app->config.coroutines - 1) + sizeof(uint64_t)) {
        ret = -ERANGE;
        goto cleanup;
    }
    if (!obmm_alloc_import_pas(1, producer_meta.size, local_pas,
                               import_osync,
                               obmm_parse_import_cache_mode())) {
        ret = -ENOMEM;
        goto cleanup;
    }
    if (obmm_do_import(
            obmm_fd, &producer_meta, local_cna, local_pas[0],
            producer_meta.token_id, &import_mem_id) != 0) {
        ret = -errno;
        goto cleanup;
    }
    if (obmm_map_region(import_mem_id, producer_meta.size,
                        import_osync[0], &import_region) != 0) {
        ret = -errno;
        goto cleanup;
    }
    printf("OBMM_P2B_IMPORT schema=1 role=consumer node=%d "
           "producer_node=%d source_export_mem_id=%llu "
           "import_mem_id=%llu bytes=%llu status=mapped\n",
           local_index, app->config.producer_index,
           (unsigned long long)producer_meta.export_mem_id,
           (unsigned long long)import_mem_id,
           (unsigned long long)producer_meta.size);
    fflush(stdout);

    app->remote_address = import_region.addr;
    ret = obmm_async_open(&app->runtime, &async_options);
    if (ret) {
        goto cleanup;
    }
    ret = async_run_scc_with_warmup(
        app, &scc_options, import_region.fd, import_mem_id);
    async_p2b_flush_trace(app);
    if (!ret) {
        ret = async_drain_observability(app);
    }
    if (!ret && obmm_async_get_observability(
            app->runtime, &app->observability) != 0) {
        ret = -EIO;
    }
    for (index = 0; index < app->config.coroutines; index++) {
        struct async_worker *worker = &app->workers[index];
        bool worker_pass =
            worker->p2b_actual == worker->p2b_expected &&
            worker->p2b_pending_upcalls == 1 &&
            worker->p2b_complete_upcalls == 1 &&
            worker->p2b_resumes_after_complete >= 1;

        if (worker_pass) {
            verified++;
        }
        printf("OBMM_P2B_COROUTINE_SUMMARY schema=1 coroutine=%u "
               "context_id=%016llx expected=%016llx actual=%016llx "
               "pending=%llu complete=%llu resumes_after_complete=%llu "
               "status=%s\n",
               index, (unsigned long long)worker->context_id,
               (unsigned long long)worker->p2b_expected,
               (unsigned long long)worker->p2b_actual,
               (unsigned long long)worker->p2b_pending_upcalls,
               (unsigned long long)worker->p2b_complete_upcalls,
               (unsigned long long)worker->p2b_resumes_after_complete,
               worker_pass ? "pass" : "fail");
    }
    pass = ret == 0 && app->completed == app->config.coroutines &&
        app->verify_failures == 0 && verified == app->config.coroutines &&
        app->scc_metrics.el0_pending_upcalls == app->config.coroutines &&
        app->scc_metrics.el0_complete_upcalls == app->config.coroutines &&
        app->scc_metrics.el0_fault_upcalls == 0 &&
        app->scc_metrics.el0_context_switches > 0 &&
        app->p2b_trace_dropped == 0 &&
        app->scc_metrics.el0_context_saves ==
            app->scc_metrics.device.direct_upcalls &&
        app->scc_metrics.device.context_saves == 0 &&
        app->scc_metrics.device.context_restores == 0 &&
        app->scc_metrics.device.context_switches == 0 &&
        app->scc_metrics.device.context_bytes_moved == 0 &&
        app->scc_metrics.observability.scc_pending_current == 0 &&
        app->scc_metrics.observability.backend_pending_current == 0;
    printf("OBMM_P2B_SUMMARY schema=1 role=consumer "
           "producer_node=%d consumer_node=%d "
           "source_export_mem_id=%llu import_mem_id=%llu "
           "coroutines=%u completed=%llu values_verified=%u "
           "el0_upcalls_pending=%llu el0_upcalls_complete=%llu "
           "el0_upcalls_fault=%llu el0_context_saves=%llu "
           "el0_context_restores=%llu el0_context_switches=%llu "
           "direct_el0_upcalls=%llu qemu_context_saves=%llu "
           "qemu_context_restores=%llu qemu_context_switches=%llu "
           "qemu_context_bytes=%llu scc_pending_final=%llu "
           "backend_pending_final=%llu trace_dropped=%u status=%s\n",
           app->config.producer_index, local_index,
           (unsigned long long)producer_meta.export_mem_id,
           (unsigned long long)import_mem_id, app->config.coroutines,
           (unsigned long long)app->completed, verified,
           (unsigned long long)app->scc_metrics.el0_pending_upcalls,
           (unsigned long long)app->scc_metrics.el0_complete_upcalls,
           (unsigned long long)app->scc_metrics.el0_fault_upcalls,
           (unsigned long long)app->scc_metrics.el0_context_saves,
           (unsigned long long)app->scc_metrics.el0_context_restores,
           (unsigned long long)app->scc_metrics.el0_context_switches,
           (unsigned long long)app->scc_metrics.device.direct_upcalls,
           (unsigned long long)app->scc_metrics.device.context_saves,
           (unsigned long long)app->scc_metrics.device.context_restores,
           (unsigned long long)app->scc_metrics.device.context_switches,
           (unsigned long long)app->scc_metrics.device.context_bytes_moved,
           (unsigned long long)
               app->scc_metrics.observability.scc_pending_current,
           (unsigned long long)
               app->scc_metrics.observability.backend_pending_current,
           app->p2b_trace_dropped, pass ? "pass" : "fail");
    if (ret) {
        printf("OBMM_P2B_SCC_ERROR schema=1 rc=%d stage=%u\n",
               app->scc_metrics.first_error,
               app->scc_metrics.first_error_stage);
    }
    fflush(stdout);
    if (!pass && !ret) {
        ret = -EIO;
    }

cleanup:
    if (ret) {
        fprintf(stderr,
                "OBMM_P2B_ERROR schema=1 role=consumer node=%d rc=%d "
                "errno=%d\n",
                local_index, ret, errno);
    }
    async_free_buffers(app);
    obmm_async_close(app->runtime);
    app->runtime = NULL;
    obmm_unmap_region(&import_region);
    if (import_mem_id) {
        obmm_do_unimport(obmm_fd, import_mem_id);
    }
    free(app->latencies_ns);
    app->latencies_ns = NULL;
    return ret;
}

int main(int argc, char **argv)
{
    struct async_app app = { 0 };
    struct obmm_async_options options;
    struct obmm_scc_options scc_options = {
        .device_path = OBMM_SCC_DEFAULT_DEVICE,
    };
    struct obmm_helpers_meta local_meta = { 0 };
    struct obmm_helpers_meta remote_metas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    struct obmm_helpers_region export_region = { .fd = -1 };
    struct obmm_helpers_region import_region = { .fd = -1 };
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    uint64_t local_pas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    struct obmm_async_metrics metrics = { 0 };
    uint64_t export_mem_id = 0;
    uint64_t import_mem_id = 0;
    uint64_t local_cna = 0;
    uint64_t latency_p50_us = 0;
    uint64_t latency_p99_us = 0;
    uint64_t latency_p50_ns = 0;
    uint64_t latency_p95_ns = 0;
    uint64_t latency_p99_ns = 0;
    uint64_t latency_max_ns = 0;
    uint64_t overlap_milli = 0;
    uint64_t scc_util_milli = 0;
    uint64_t workload_start_ns = 0;
    uint64_t workload_end_ns = 0;
    uint64_t workload_cpu_start_ns = 0;
    uint64_t workload_cpu_end_ns = 0;
    uint64_t stale_completions = 0;
    uint64_t switches = 0;
    int local_index = -1;
    int peer_index = -1;
    int obmm_fd = -1;
    int run_status = -1;
    bool barrier_ok = false;
    const char *status = "fail";
    const char *failure_stage = "startup";

    if (!async_parse_args(argc, argv, &app.config)) {
        async_usage(argv[0]);
        return 2;
    }
    if (app.config.self_test) {
        int selftest_status = obmm_async_context_selftest();

        printf("OBMM_ASYNC_SELFTEST abi=%u status=%s rc=%d\n",
               OBMM_ASYNC_ABI_VERSION,
               selftest_status == 0 ? "pass" : "fail",
               selftest_status);
        return selftest_status == 0 ? 0 : 1;
    }
    atomic_init(&app.operation_trace_next, 0);
    failure_stage = "allocate-operation-trace";
    run_status = async_prepare_operation_trace(&app);
    if (run_status != 0) {
        goto cleanup;
    }
    failure_stage = "open-obmm";
    obmm_fd = open("/dev/obmm", O_RDWR | O_CLOEXEC);
    if (obmm_fd < 0) {
        run_status = -errno;
        goto cleanup;
    }
    failure_stage = "resolve-identity";
    if (!async_resolve_identity(&local_cna, &local_index,
                                app.config.node_count)) {
        run_status = -ENOENT;
        goto cleanup;
    }
    if (app.config.p2b_producer_consumer) {
        if (local_index == app.config.producer_index) {
            run_status = async_run_p2b_producer(
                &app, obmm_fd, local_cna, local_index);
        } else {
            run_status = async_run_p2b_consumer(
                &app, obmm_fd, local_cna, local_index);
        }
        async_flush_operation_trace(&app);
        close(obmm_fd);
        return run_status == 0 ? 0 : 1;
    }
    peer_index = app.config.peer_index >= 0 ? app.config.peer_index :
        async_default_peer(local_index, app.config.node_count);
    local_meta.export_cna = local_cna;
    failure_stage = "export";
    if (obmm_do_export(obmm_fd, &local_meta, ASYNC_EXPORT_BYTES) != 0) {
        run_status = -errno;
        goto cleanup;
    }
    export_mem_id = local_meta.export_mem_id;
    failure_stage = "map-export";
    if (obmm_map_region(export_mem_id, ASYNC_EXPORT_BYTES, false,
                        &export_region) != 0) {
        run_status = -errno;
        goto cleanup;
    }
    async_fill_export(export_region.addr, ASYNC_EXPORT_BYTES,
                      app.config.seed);
    app.local_address = export_region.addr;
    failure_stage = "bootstrap-publish";
    if (obmm_bootstrap_publish(
            obmm_fd, local_index, app.config.node_count,
            ASYNC_BOOTSTRAP_GENERATION, &local_meta) != 0) {
        run_status = -errno;
        goto cleanup;
    }
    failure_stage = "bootstrap-lookup";
    if (obmm_bootstrap_lookup(
            obmm_fd, local_cna, app.config.node_count,
            ASYNC_BOOTSTRAP_GENERATION, remote_metas, got) != 0 ||
        !got[peer_index]) {
        run_status = -ENOENT;
        goto cleanup;
    }
    failure_stage = "allocate-import-pa";
    if (!obmm_alloc_import_pas(1, ASYNC_EXPORT_BYTES, local_pas,
                               import_osync,
                               obmm_parse_import_cache_mode())) {
        run_status = -ENOMEM;
        goto cleanup;
    }
    failure_stage = "import";
    if (obmm_do_import(obmm_fd, &remote_metas[peer_index], local_cna,
                       local_pas[0], remote_metas[peer_index].token_id,
                       &import_mem_id) != 0) {
        run_status = -errno;
        goto cleanup;
    }
    failure_stage = "map-import";
    if (obmm_map_region(import_mem_id, ASYNC_EXPORT_BYTES,
                        import_osync[0], &import_region) != 0) {
        run_status = -errno;
        goto cleanup;
    }
    app.remote_address = import_region.addr;
    options = (struct obmm_async_options) {
        .device_path = OBMM_ASYNC_DEFAULT_DEVICE,
        .mode = async_split_phase_mode(app.config.mode),
        .spin_us = 10,
    };
    failure_stage = "async-open";
    run_status = obmm_async_open(&app.runtime, &options);
    if (run_status != 0) {
        goto cleanup;
    }
    if (app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE) {
        scc_options.load_timeout_ns =
            (uint64_t)app.config.deadline_us * 1000;
        failure_stage = "scc-workload";
        workload_start_ns = async_now_ns();
        workload_cpu_start_ns = async_process_now_ns();
        run_status = async_run_scc_with_warmup(
            &app, &scc_options, import_region.fd, import_mem_id);
        workload_cpu_end_ns = async_process_now_ns();
        workload_end_ns = async_now_ns();
        stale_completions =
            app.scc_metrics.device.stale_completions;
        switches = app.scc_metrics.el0_context_switches;
    } else {
        failure_stage = "async-map-register";
        run_status = obmm_async_map_register(
            app.runtime, obmm_fd, import_mem_id, import_region.addr,
            ASYNC_EXPORT_BYTES, &app.remote_map);
        if (run_status != 0) {
            goto cleanup;
        }
        failure_stage = "workload";
        workload_start_ns = async_now_ns();
        workload_cpu_start_ns = async_process_now_ns();
        if (app.config.mode == ASYNC_APP_MODE_USERFAULTFD) {
            run_status = async_run_uffd_with_warmup(
                &app, obmm_fd, import_mem_id);
        } else if (app.config.mode == ASYNC_APP_MODE_SYNC) {
            run_status = async_run_sync_workload(
                &app, obmm_fd, import_mem_id, export_region.addr);
        } else {
            run_status = async_run_split_phase_with_warmup(
                &app, obmm_fd, import_mem_id);
        }
        workload_cpu_end_ns = async_process_now_ns();
        workload_end_ns = async_now_ns();
        obmm_async_get_metrics(app.runtime, &metrics);
        stale_completions = metrics.stale_completions;
        switches = metrics.coroutine_switches;
    }
    {
        int drain_status = async_drain_observability(&app);

        if (drain_status && !run_status) {
            run_status = drain_status;
        }
    }
    if (obmm_async_get_observability(
            app.runtime, &app.observability) != 0) {
        run_status = run_status ? run_status : -EIO;
    }
    if (app.measurement_end_ns > app.measurement_start_ns) {
        workload_start_ns = app.measurement_start_ns;
        workload_end_ns = app.measurement_end_ns;
        workload_cpu_start_ns = app.measurement_cpu_start_ns;
        workload_cpu_end_ns = app.measurement_cpu_end_ns;
    }
    barrier_ok = async_completion_barrier(
        obmm_fd, local_index, local_cna, app.config.node_count,
        &local_meta);
    if (app.config.mode != ASYNC_APP_MODE_USERFAULTFD &&
        app.latency_count) {
        uint64_t *copy = malloc(app.latency_count * sizeof(*copy));

        if (copy) {
            memcpy(copy, app.latencies_ns,
                   app.latency_count * sizeof(*copy));
            latency_p50_us = async_percentile_us(
                copy, app.latency_count, 50);
            memcpy(copy, app.latencies_ns,
                   app.latency_count * sizeof(*copy));
            latency_p99_us = async_percentile_us(
                copy, app.latency_count, 99);
            memcpy(copy, app.latencies_ns,
                   app.latency_count * sizeof(*copy));
            latency_p50_ns = async_percentile_ns(
                copy, app.latency_count, 50);
            memcpy(copy, app.latencies_ns,
                   app.latency_count * sizeof(*copy));
            latency_p95_ns = async_percentile_ns(
                copy, app.latency_count, 95);
            memcpy(copy, app.latencies_ns,
                   app.latency_count * sizeof(*copy));
            latency_p99_ns = async_percentile_ns(
                copy, app.latency_count, 99);
            memcpy(copy, app.latencies_ns,
                   app.latency_count * sizeof(*copy));
            latency_max_ns = async_percentile_ns(
                copy, app.latency_count, 100);
            free(copy);
        }
    }
    if (app.compute_steps) {
        overlap_milli = app.compute_while_pending * 1000 /
            app.compute_steps;
    }
    if (app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE &&
        workload_end_ns > workload_start_ns &&
        app.scc_metrics.clock_mhz) {
        __uint128_t available_cycles =
            (__uint128_t)(workload_end_ns - workload_start_ns) *
            app.scc_metrics.clock_mhz / 1000;
        __uint128_t utilization =
            (__uint128_t)app.scc_metrics.device.modeled_cycles * 1000 /
            available_cycles;

        scc_util_milli = utilization > 1000 ? 1000 : utilization;
    }
    if (run_status == -EOPNOTSUPP &&
        app.config.mode == ASYNC_APP_MODE_USERFAULTFD) {
        status = "unsupported";
    } else if (run_status == 0 && barrier_ok && app.failures == 0 &&
        app.verify_failures == 0 &&
        app.completed == app.config.iterations) {
        status = "pass";
    }

cleanup:
    async_flush_operation_trace(&app);
    if (strcmp(status, "pass") != 0) {
        fprintf(stderr,
                "OBMM_APP_ERROR schema=1 stage=%s rc=%d errno=%d status=%s\n",
                failure_stage, run_status, errno, status);
    }
    if (app.scc && app.scc_map.policy_id) {
        obmm_scc_unregister_map(app.scc, &app.scc_map);
    }
    obmm_scc_close(app.scc);
    if (app.runtime && app.remote_map.id) {
        obmm_async_map_unregister(app.runtime, &app.remote_map);
    }
    async_free_buffers(&app);
    obmm_async_close(app.runtime);
    obmm_unmap_region(&import_region);
    obmm_unmap_region(&export_region);
    if (import_mem_id && obmm_fd >= 0) {
        obmm_do_unimport(obmm_fd, import_mem_id);
    }
    if (export_mem_id && obmm_fd >= 0) {
        obmm_do_unexport(obmm_fd, export_mem_id);
    }
    if (obmm_fd >= 0) {
        close(obmm_fd);
    }
    free(app.latencies_ns);
    if (app.config.mode == ASYNC_APP_MODE_SYNC) {
        printf("OBMM_BASELINE_SUMMARY schema=1 case=%s status=%s "
               "iterations=%llu bytes=%u checksum=%016llx failures=%llu "
               "timeouts=%llu guest_ns_p50=%llu guest_ns_p99=%llu "
               "model_service_ns=%llu model_accept_publish_ns=%llu "
               "model_duplicated=%llu model_duplicate_published=%llu "
               "model_pending=%llu backend_pending=%llu\n",
               async_baseline_case_name(app.config.baseline_case), status,
               (unsigned long long)app.completed,
               app.config.access_bytes,
               (unsigned long long)app.checksum,
               (unsigned long long)app.failures,
               (unsigned long long)app.timeouts,
               (unsigned long long)latency_p50_ns,
               (unsigned long long)latency_p99_ns,
               (unsigned long long)app.observability.model_service_ns,
               (unsigned long long)
                   app.observability.model_accept_publish_ns,
               (unsigned long long)app.observability.model_duplicated,
               (unsigned long long)
                   app.observability.model_duplicate_published,
               (unsigned long long)app.observability.model_pending,
               (unsigned long long)app.observability.backend_pending);
        async_print_eval_summary(
            &app, &metrics, status,
            async_elapsed_ns(workload_start_ns, workload_end_ns),
            async_elapsed_ns(workload_cpu_start_ns,
                             workload_cpu_end_ns),
            latency_p50_ns, latency_p95_ns,
            latency_p99_ns, latency_max_ns);
        return strcmp(status, "pass") == 0 ? 0 : 1;
    }
    if (app.config.mode == ASYNC_APP_MODE_USERFAULTFD) {
        if (strcmp(status, "unsupported") == 0) {
            printf("OBMM_UFFD_UNSUPPORTED reason=required-capability\n");
        }
        printf("OBMM_UFFD_SUMMARY schema=1 case=%s pages=%llu "
               "faults=%llu remote_reads=%llu copy_ok=%llu "
               "duplicates=%llu checksum=%016llx fault_ns_p50=%llu "
               "fault_ns_p95=%llu fault_ns_p99=%llu fault_ns_max=%llu "
               "remote_ns_p50=%llu remote_ns_p95=%llu "
               "remote_ns_p99=%llu remote_ns_max=%llu "
               "copy_ns_p50=%llu copy_ns_p95=%llu copy_ns_p99=%llu "
               "copy_ns_max=%llu wake_ns_p50=%llu wake_ns_p95=%llu "
               "wake_ns_p99=%llu wake_ns_max=%llu "
               "handler_cpu_ns=%llu worker_cpu_ns=%llu "
               "model_service_ns=%llu model_accept_publish_ns=%llu "
               "model_duplicated=%llu model_duplicate_published=%llu "
               "model_pending=%llu backend_pending=%llu "
               "poison_supported=%u failures=%llu status=%s\n",
               obmm_uffd_case_name(app.config.uffd_case),
               (unsigned long long)app.uffd_metrics.pages,
               (unsigned long long)app.uffd_metrics.faults,
               (unsigned long long)app.uffd_metrics.remote_reads,
               (unsigned long long)app.uffd_metrics.copy_ok,
               (unsigned long long)app.uffd_metrics.duplicates,
               (unsigned long long)app.uffd_metrics.checksum,
               (unsigned long long)app.uffd_metrics.fault_ns_p50,
               (unsigned long long)app.uffd_metrics.fault_ns_p95,
               (unsigned long long)app.uffd_metrics.fault_ns_p99,
               (unsigned long long)app.uffd_metrics.fault_ns_max,
               (unsigned long long)app.uffd_metrics.remote_ns_p50,
               (unsigned long long)app.uffd_metrics.remote_ns_p95,
               (unsigned long long)app.uffd_metrics.remote_ns_p99,
               (unsigned long long)app.uffd_metrics.remote_ns_max,
               (unsigned long long)app.uffd_metrics.copy_ns_p50,
               (unsigned long long)app.uffd_metrics.copy_ns_p95,
               (unsigned long long)app.uffd_metrics.copy_ns_p99,
               (unsigned long long)app.uffd_metrics.copy_ns_max,
               (unsigned long long)app.uffd_metrics.wake_ns_p50,
               (unsigned long long)app.uffd_metrics.wake_ns_p95,
               (unsigned long long)app.uffd_metrics.wake_ns_p99,
               (unsigned long long)app.uffd_metrics.wake_ns_max,
               (unsigned long long)app.uffd_metrics.handler_cpu_ns,
               (unsigned long long)app.uffd_metrics.worker_cpu_ns,
               (unsigned long long)app.observability.model_service_ns,
               (unsigned long long)
                   app.observability.model_accept_publish_ns,
               (unsigned long long)app.observability.model_duplicated,
               (unsigned long long)
                   app.observability.model_duplicate_published,
               (unsigned long long)app.observability.model_pending,
               (unsigned long long)app.observability.backend_pending,
               app.uffd_metrics.poison_supported,
               (unsigned long long)app.uffd_metrics.failures, status);
        async_print_eval_summary(
            &app, &metrics, status,
            async_elapsed_ns(workload_start_ns, workload_end_ns),
            async_elapsed_ns(workload_cpu_start_ns,
                             workload_cpu_end_ns),
            app.uffd_metrics.fault_ns_p50,
            app.uffd_metrics.fault_ns_p95,
            app.uffd_metrics.fault_ns_p99,
            app.uffd_metrics.fault_ns_max);
        return strcmp(status, "pass") == 0 ? 0 :
            strcmp(status, "unsupported") == 0 ? 77 : 1;
    }
    printf("OBMM_ASYNC_SUMMARY abi=%u mode=%s status=%s "
           "coroutines=%u inflight=%u lookahead=%u completed=%" PRIu64
           " failures=%" PRIu64 " timeouts=%" PRIu64
           " stale=%" PRIu64 " checksum=%016" PRIx64
           " latency_us_p50=%" PRIu64 " latency_us_p99=%" PRIu64
           " submit_ns_p50=%" PRIu64 " submit_ns_total=%" PRIu64
           " switch_ns_p50=%" PRIu64 " switch_ns_total=%" PRIu64
           " cq_drain_ns_p50=%" PRIu64 " cq_drain_ns_total=%" PRIu64
           " ready_ns=%" PRIu64 " wait_ns=%" PRIu64
           " idle_ns=%" PRIu64 " no_ready=%" PRIu64
           " overlap_milli=%" PRIu64
           " pending_high=%" PRIu64 " ready_high=%" PRIu64
           " switches=%" PRIu64 " capacity_stalls=%" PRIu64
           " scc_util_milli=%" PRIu64 " scc_cycles=%" PRIu64
           " context_bytes=%" PRIu64
           " model_service_ns=%" PRIu64
           " model_accept_publish_ns=%" PRIu64
           " model_duplicated=%" PRIu64
           " model_duplicate_published=%" PRIu64
           " model_pending=%" PRIu64 " scc_pending=%" PRIu64
           " backend_pending=%" PRIu64
           " backend_pending_high=%" PRIu64
           " backend_late=%" PRIu64 " backend_duplicate=%" PRIu64
           " backend_capacity=%" PRIu64
           " sink_copy_bytes=%" PRIu64 " sink_copy_ns=%" PRIu64
           " scc_save_cycles=%" PRIu64
           " scc_schedule_cycles=%" PRIu64
           " scc_restore_cycles=%" PRIu64
           " scc_commit_cycles=%" PRIu64
           " scc_logical_contexts=%" PRIu64
           " el0_upcalls_pending=%" PRIu64
           " el0_upcalls_complete=%" PRIu64
           " el0_upcalls_fault=%" PRIu64
           " el0_context_saves=%" PRIu64
           " el0_context_restores=%" PRIu64
           " el0_context_switches=%" PRIu64
           " el0_context_bytes=%" PRIu64
           " el0_scheduler_ns=%" PRIu64
           " el0_no_ready_waits=%" PRIu64
           " direct_el0_upcalls=%" PRIu64
           " qemu_context_saves=%" PRIu64
           " qemu_context_restores=%" PRIu64
           " qemu_context_switches=%" PRIu64
           " qemu_context_bytes=%" PRIu64
           "\n",
           OBMM_ASYNC_ABI_VERSION, async_mode_name(app.config.mode), status,
           app.config.coroutines,
           app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ? 1 :
               app.config.inflight,
           app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ? 0 :
               app.config.lookahead,
           app.completed, app.failures, app.timeouts,
           stale_completions, app.checksum,
           latency_p50_us, latency_p99_us,
           metrics.submit_ns_p50, metrics.submit_ns_total,
           metrics.switch_ns_p50, metrics.switch_ns_total,
           metrics.cq_drain_ns_p50, metrics.cq_drain_ns_total,
           metrics.ready_ns, metrics.wait_ns,
           metrics.idle_ns,
           app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ?
               app.scc_metrics.el0_no_ready_waits : metrics.no_ready,
           overlap_milli,
           (uint64_t)app.scc_metrics.device.pending_high_water,
           (uint64_t)app.scc_metrics.el0_ready_high_water, switches,
           (uint64_t)app.scc_metrics.device.capacity_stalls,
           scc_util_milli,
           (uint64_t)app.scc_metrics.device.modeled_cycles,
           (uint64_t)app.scc_metrics.el0_context_bytes,
           (uint64_t)app.observability.model_service_ns,
           (uint64_t)app.observability.model_accept_publish_ns,
           (uint64_t)app.observability.model_duplicated,
           (uint64_t)app.observability.model_duplicate_published,
           (uint64_t)app.observability.model_pending,
           (uint64_t)app.scc_metrics.observability.scc_pending_current,
           (uint64_t)(app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ?
               app.scc_metrics.observability.backend_pending_current :
               app.observability.backend_pending),
           (uint64_t)(app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ?
               app.scc_metrics.observability.backend_pending_high_water :
               app.observability.backend_pending_high_water),
           (uint64_t)(app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ?
               app.scc_metrics.observability.backend_late :
               app.observability.backend_late),
           (uint64_t)(app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ?
               app.scc_metrics.observability.backend_duplicate :
               app.observability.backend_duplicate),
           (uint64_t)(app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ?
               app.scc_metrics.observability.backend_capacity :
               app.observability.backend_capacity),
           (uint64_t)(app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ?
               app.scc_metrics.observability.backend_sink_copy_bytes :
               app.observability.backend_sink_copy_bytes),
           (uint64_t)(app.config.mode == ASYNC_APP_MODE_SCHEDULER_CORE ?
               app.scc_metrics.observability.backend_sink_copy_ns :
               app.observability.backend_sink_copy_ns),
           (uint64_t)app.scc_metrics.observability.save_cycles,
           (uint64_t)app.scc_metrics.observability.schedule_cycles,
           (uint64_t)app.scc_metrics.observability.restore_cycles,
           (uint64_t)app.scc_metrics.observability.commit_cycles,
           (uint64_t)app.scc_metrics.observability.logical_contexts,
           (uint64_t)app.scc_metrics.el0_pending_upcalls,
           (uint64_t)app.scc_metrics.el0_complete_upcalls,
           (uint64_t)app.scc_metrics.el0_fault_upcalls,
           (uint64_t)app.scc_metrics.el0_context_saves,
           (uint64_t)app.scc_metrics.el0_context_restores,
           (uint64_t)app.scc_metrics.el0_context_switches,
           (uint64_t)app.scc_metrics.el0_context_bytes,
           (uint64_t)app.scc_metrics.el0_scheduler_ns,
           (uint64_t)app.scc_metrics.el0_no_ready_waits,
           (uint64_t)app.scc_metrics.device.direct_upcalls,
           (uint64_t)app.scc_metrics.device.context_saves,
           (uint64_t)app.scc_metrics.device.context_restores,
           (uint64_t)app.scc_metrics.device.context_switches,
           (uint64_t)app.scc_metrics.device.context_bytes_moved);
    async_print_eval_summary(
        &app, &metrics, status,
        async_elapsed_ns(workload_start_ns, workload_end_ns),
        async_elapsed_ns(workload_cpu_start_ns,
                         workload_cpu_end_ns),
        latency_p50_ns, latency_p95_ns,
        latency_p99_ns, latency_max_ns);
    return strcmp(status, "pass") == 0 ? 0 : 1;
}
