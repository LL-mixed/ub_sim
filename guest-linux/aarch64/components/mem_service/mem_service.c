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

#include "kernel_ub/include/uapi/ub/obmm.h"
#include "common/obmm_common.h"
#include "mem_service_qwen3.h"
#include "libs/obmm_queue/obmm_queue_types.h"
#include "libs/obmm_queue/obmm_spsc_queue.h"

#define MEM_SERVICE_CLUSTER_MAX_NODES 8
#define MEM_SERVICE_CLUSTER_MAX_RECORDS 1024
#define MEM_SERVICE_MAYBE_UNUSED __attribute__((unused))
#define MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS 16ULL
#define MEM_SERVICE_DEFAULT_REGION_SIZE_MB 512
#define MEM_SERVICE_CMDLINE_REGION_SIZE "mem_service_region_size_mb"
#define MEM_SERVICE_CLUSTER_QUEUE_DEPTH 512
#define MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH 16
#define MEM_SERVICE_CLUSTER_WAIT_MS 300000L
#define MEM_SERVICE_OBMM_SERVICE_WAIT_MS 300000L
#define MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_MS 600000L
#define MEM_SERVICE_CLUSTER_IMPORT_ALIGN (2ULL * 1024ULL * 1024ULL)
#define MEM_SERVICE_CLUSTER_MAX_WINDOWS 16
#define MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES 8192ULL
#define MEM_SERVICE_OBMM_WEIGHT_OFFSET 0x10000ULL
#define MEM_SERVICE_OBMM_KVCACHE_OFFSET 0x14000ULL
#define MEM_SERVICE_OBMM_HIDDEN_RANGE_INPUT_OFFSET 0x18000ULL
#define MEM_SERVICE_OBMM_HIDDEN_RANGE_OUTPUT_OFFSET 0x58000ULL
#define MEM_SERVICE_OBMM_HIDDEN_RANGE_RUNTIME_OUTPUT_OFFSET 0x98000ULL
#define MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_OFFSET 0xda000ULL
#define MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_SLOTS 1024ULL
#define MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET 0x100000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_OFFSET 0x100000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES 0x200000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES 0x40000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES 0x80000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES 0x100000ULL
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES \
    MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES
#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOTS 32ULL
#define MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_SLOTS 32ULL
#define MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_OFFSET \
    (MEM_SERVICE_OBMM_QWEN3_KV_STATE_OFFSET + \
     (MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES * \
      MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOTS))
#define MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_REGION_BYTES \
    (MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_SLOTS * \
     MEM_SERVICE_OBMM_HIDDEN_RANGE_BYTES)
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_BASE_OFFSET \
    (MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_OFFSET + \
     MEM_SERVICE_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_REGION_BYTES)
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_SLOT_BYTES 0x4000ULL
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_HISTORY_BYTES 0x2000ULL
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES 256ULL
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES 64ULL
#define MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES 128ULL
#define MEM_SERVICE_OBMM_HIDDEN_RANGE_BYTES 262144ULL
#ifndef MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES
#define MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES 64ULL
#endif
#define MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES 64ULL
#define MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_REGION_BYTES \
    (MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_SLOTS * \
     MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES)
#define MEM_SERVICE_OBMM_KIND_WEIGHT_TILE 1U
#define MEM_SERVICE_OBMM_KIND_KVCACHE_BLOCK 2U
#define MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_INPUT 3U
#define MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT 4U
#define MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT 5U
#ifndef MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT
#define MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT 6U
#endif
#define MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE 7U
#define MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY 8U
#define MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES 9U
#define MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED 10U
#define MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE 11U

#ifndef major
#define major(dev) ((unsigned int)(((uint64_t)(dev) >> 24) & 0xffU))
#endif
#ifndef minor
#define minor(dev) ((unsigned int)((uint64_t)(dev) & 0xffffffU))
#endif

struct mem_service_cluster_meta {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
};

struct mem_service_cluster_payload {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
    uint8_t record_pad[48];
    struct mem_service_record records[MEM_SERVICE_CLUSTER_MAX_RECORDS];
};

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

struct mem_service_cluster_payload_header {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
};

struct mem_service_cluster_payload_compact_summary {
    uint16_t record_count;
    uint16_t prefix_count;
    uint16_t block_count;
    uint16_t group_count;
    uint16_t weight_tile_count;
    uint16_t kvcache_object_count;
    uint16_t flags;
    uint16_t hidden_range_count;
    uint64_t block_version_floor;
    uint64_t block_result_floor;
    uint64_t prefix_version_floor;
    uint64_t prefix_result_floor;
};

struct mem_service_qwen3_layer_range_placement {
    uint32_t owner_node;
    uint32_t layer_start;
    uint32_t layer_end;
    uint32_t next_owner_node;
    uint32_t layer_count;
    bool terminal;
};

#define MEM_SERVICE_COMPACT_PREFIX_STATE_READY 0x0001U
#define MEM_SERVICE_COMPACT_PREFIX_VIEW_READY 0x0002U

struct mem_service_mapped_region {
    int fd;
    void *addr;
    size_t len;
    uint64_t mem_id;
};

struct mem_service_cluster_slot {
    int owner_idx;
    bool is_local;
    bool map_osync;
    uint32_t export_cna;
    uint64_t mem_id;
    uint64_t local_pa;
    struct mem_service_mapped_region region;
};

struct mem_service_cluster_runtime {
    bool active;
    bool lazy_remote_activation;
    int node_count;
    int local_idx;
    int obmm_fd;
    uint32_t local_cna;
    uint32_t publish_seq;
    uint16_t observe_epoch;
    uint64_t region_size;
    uint64_t payload_offset;
    uint64_t payload_arena_base;
    uint64_t payload_arena_next;
    uint64_t payload_arena_high_water;
    bool pool_layout_reported;
    struct mem_service_cluster_meta metas[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct mem_service_cluster_slot slots[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_spsc_queue *ingress_queues[MEM_SERVICE_CLUSTER_MAX_NODES];
    void *ingress_queue_base;
    struct obmm_spsc_queue *egress_queues[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_helpers_region egress_import[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_desc pending_descs[MEM_SERVICE_CLUSTER_MAX_NODES][MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH];
    uint8_t pending_desc_count[MEM_SERVICE_CLUSTER_MAX_NODES];
};

#define MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC 0x57344450U
#define MEM_SERVICE_CLUSTER_PAYLOAD_VERSION 1U

static struct mem_service_cluster_runtime g_mem_service_cluster_runtime;

static struct mem_service_record *mem_service_alloc_record(struct mem_service *svc);
static struct mem_service_record *mem_service_find_record(struct mem_service *svc, const char *key);
static struct mem_service_record *mem_service_recycle_qwen3_runtime_record(
    struct mem_service *svc,
    const char *incoming_key);
static int mem_service_activate_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx);

#include "mem_service_cluster_utils.inc"

#include "mem_service_cluster_payload.inc"

#include "mem_service_object_refs.inc"

#include "mem_service_obmm_objects.inc"

#include "mem_service_qwen3_runtime.inc"

#include "mem_service_cluster_read.inc"

#include "mem_service_cluster_runtime.inc"

#include "mem_service_cluster_queue.inc"

#include "mem_service_records.inc"
#include "mem_service_qwen3_records.inc"
#include "mem_service_keys.inc"
#include "mem_service_metadata.inc"

#include "mem_service_cluster_observe.inc"

#include "mem_service_obmm_object_flow.inc"

#include "mem_service_qwen3_runtime_range_wait_flow.inc"

#include "mem_service_qwen3_runtime_range_publish_flow.inc"

#include "mem_service_qwen3_kv_state_flow.inc"

#include "mem_service_qwen3_terminal_token_flow.inc"

#include "mem_service_qwen3_engram_publish_flow.inc"

#include "mem_service_qwen3_engram_wait_flow.inc"

#include "mem_service_qwen3_decode_barrier.inc"
