/* SPDX-License-Identifier: MIT */
#ifndef OBMM_ASYNC_UFFD_MODE_H
#define OBMM_ASYNC_UFFD_MODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "obmm_async.h"

enum obmm_uffd_case {
    OBMM_UFFD_CASE_PRESENT_HIT,
    OBMM_UFFD_CASE_MISSING_REMOTE,
};

typedef void (*obmm_uffd_trace_fn)(
    void *opaque, uint64_t ordinal, uint64_t offset,
    int status, uint64_t latency_ns);

struct obmm_uffd_run_config {
    enum obmm_uffd_case test_case;
    struct obmm_async *remote_runtime;
    const struct obmm_async_map *remote_map;
    const struct obmm_async_buffer *staging_buffer;
    const void *source_base;
    size_t source_length;
    uint32_t pages;
    uint32_t worker_threads;
    int handler_cpu;
    uint64_t iterations;
    uint64_t seed;
    uint32_t deadline_us;
    uint32_t trace_sample_ppm;
    bool random_pattern;
    bool verify;
    obmm_uffd_trace_fn trace;
    void *trace_opaque;
};

struct obmm_uffd_metrics {
    uint64_t pages;
    uint64_t faults;
    uint64_t remote_reads;
    uint64_t copy_ok;
    uint64_t duplicates;
    uint64_t failures;
    uint64_t checksum;
    uint64_t fault_ns_p50;
    uint64_t fault_ns_p95;
    uint64_t fault_ns_p99;
    uint64_t fault_ns_max;
    uint64_t remote_ns_p50;
    uint64_t remote_ns_p95;
    uint64_t remote_ns_p99;
    uint64_t remote_ns_max;
    uint64_t copy_ns_p50;
    uint64_t copy_ns_p95;
    uint64_t copy_ns_p99;
    uint64_t copy_ns_max;
    uint64_t wake_ns_p50;
    uint64_t wake_ns_p95;
    uint64_t wake_ns_p99;
    uint64_t wake_ns_max;
    uint64_t handler_cpu_ns;
    uint64_t worker_cpu_ns;
    bool poison_supported;
};

const char *obmm_uffd_case_name(enum obmm_uffd_case test_case);
int obmm_uffd_run(const struct obmm_uffd_run_config *config,
                  struct obmm_uffd_metrics *metrics);

#endif
