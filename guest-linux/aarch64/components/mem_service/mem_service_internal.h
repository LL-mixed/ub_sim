#ifndef MEM_SERVICE_INTERNAL_H
#define MEM_SERVICE_INTERNAL_H

#include "mem_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/sysmacros.h>
#endif
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "mem_service_cluster_payload_contract.h"
#include "mem_service_guest_runtime.h"
#include "mem_service_object_contract.h"
#include "mem_service_qwen3.h"

#define MEM_SERVICE_MAYBE_UNUSED __attribute__((unused))
#define MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS 16ULL
#define MEM_SERVICE_CLUSTER_WAIT_MS 300000L
#define MEM_SERVICE_OBMM_SERVICE_WAIT_MS 300000L
#define MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_MS 600000L

#ifndef major
#define major(dev) ((unsigned int)(((uint64_t)(dev) >> 24) & 0xffU))
#endif
#ifndef minor
#define minor(dev) ((unsigned int)((uint64_t)(dev) & 0xffffffU))
#endif

static long mem_service_env_wait_ms_or_default(const char *name, long fallback)
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

static const char *mem_service_run_id_from_env(void)
{
    const char *run_id = getenv("MEM_SERVICE_RUN_ID");

    if (run_id && run_id[0] != '\0') {
        return run_id;
    }
    run_id = getenv("SIM_W5_RUN_ID");
    return run_id && run_id[0] != '\0' ? run_id : NULL;
}

static long mem_service_qwen3_runtime_range_wait_ms(void)
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

struct mem_service_qwen3_layer_range_placement {
    uint32_t owner_node;
    uint32_t layer_start;
    uint32_t layer_end;
    uint32_t next_owner_node;
    uint32_t layer_count;
    bool terminal;
};

#endif
