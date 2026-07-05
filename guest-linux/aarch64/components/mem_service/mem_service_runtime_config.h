#ifndef MEM_SERVICE_RUNTIME_CONFIG_H
#define MEM_SERVICE_RUNTIME_CONFIG_H

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define MEM_SERVICE_CLUSTER_WAIT_MS 300000L
#define MEM_SERVICE_OBMM_SERVICE_WAIT_MS 300000L
#define MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_MS 600000L

static inline long mem_service_env_wait_ms_or_default(const char *name, long fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (!value || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
        parsed > (unsigned long long)LONG_MAX) {
        return fallback;
    }
    return (long)parsed;
}

static inline const char *mem_service_run_id_from_env(void)
{
    const char *run_id = getenv("MEM_SERVICE_RUN_ID");

    if (run_id && run_id[0] != '\0') {
        return run_id;
    }
    run_id = getenv("SIM_W5_RUN_ID");
    return run_id && run_id[0] != '\0' ? run_id : NULL;
}

static inline uint64_t mem_service_run_scope_hash_from_env(void)
{
    const char *run_id = mem_service_run_id_from_env();
    uint64_t hash = 1469598103934665603ULL;

    if (!run_id) {
        return 0;
    }
    while (*run_id) {
        hash ^= (unsigned char)*run_id++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static inline uint64_t mem_service_serving_decode_step_from_env(uint64_t decode_step)
{
    const char *value = getenv("SIM_W5_SERVING_DECODE_STEP_BASE");
    char *end = NULL;
    unsigned long long parsed;

    if (!value || value[0] == '\0') {
        return decode_step;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed > UINT64_MAX - decode_step) {
        return decode_step;
    }
    return (uint64_t)parsed + decode_step;
}

static inline long mem_service_qwen3_runtime_range_wait_ms(void)
{
    long barrier_wait_ms;
    long runtime_wait_ms =
        mem_service_env_wait_ms_or_default("SIM_QWEN3_RUNTIME_RANGE_WAIT_MS", -1);

    if (runtime_wait_ms > 0) {
        return runtime_wait_ms;
    }
    barrier_wait_ms = mem_service_env_wait_ms_or_default(
        "SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS",
        MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_MS);
    return barrier_wait_ms > 0 ? barrier_wait_ms :
        MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_MS;
}

#endif
