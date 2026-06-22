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

static long mem_service_wallclock_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void mem_service_cpu_relax_wait(unsigned int *attempt)
{
    struct timespec ts;
    unsigned int step = attempt ? *attempt : 0;
    long usec = 1000L;

    if (step < 32U) {
        usec <<= step / 4U;
    } else {
        usec = 64000L;
    }
    if (usec > 64000L) {
        usec = 64000L;
    }
    ts.tv_sec = usec / 1000000L;
    ts.tv_nsec = (usec % 1000000L) * 1000L;
    (void)nanosleep(&ts, NULL);
    if (attempt && *attempt < 64U) {
        *attempt += 1U;
    }
}

static bool mem_service_parse_ip_list(const char *csv,
                                char ips[MEM_SERVICE_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
                                int *count_out)
{
    char copy[256];
    char *saveptr = NULL;
    char *tok = NULL;
    int count = 0;

    if (!csv || !count_out) {
        return false;
    }
    snprintf(copy, sizeof(copy), "%s", csv);
    tok = strtok_r(copy, ",", &saveptr);
    while (tok && count < MEM_SERVICE_CLUSTER_MAX_NODES) {
        snprintf(ips[count], INET_ADDRSTRLEN, "%s", tok);
        count += 1;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    if (count < 2) {
        return false;
    }
    *count_out = count;
    return true;
}

static bool mem_service_resolve_cluster_nodes(char local_ip[INET_ADDRSTRLEN],
                                        char ips[MEM_SERVICE_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
                                        int *node_count,
                                        int *local_idx)
{
    const char *env_local = getenv("LINQU_UB_LOCAL_IP");
    const char *env_all = getenv("LINQU_UB_ALL_IPS");
    int i;

    if (!env_local || !env_all) {
        return false;
    }
    snprintf(local_ip, INET_ADDRSTRLEN, "%s", env_local);
    if (!mem_service_parse_ip_list(env_all, ips, node_count)) {
        return false;
    }
    for (i = 0; i < *node_count; ++i) {
        if (strcmp(ips[i], local_ip) == 0) {
            *local_idx = i;
            return true;
        }
    }
    return false;
}

static bool mem_service_parse_hex_file_u64(const char *path, uint64_t *value)
{
    char buf[256];
    char *end = NULL;
    unsigned long long v;
    FILE *fp = fopen(path, "r");

    if (!fp) {
        return false;
    }
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    errno = 0;
    v = strtoull(buf, &end, 0);
    if (errno != 0 || end == buf) {
        return false;
    }
    *value = (uint64_t)v;
    return true;
}

static int mem_service_update_region_range_at(const struct mem_service_cluster_slot *slot,
                                        uint64_t offset,
                                        uint64_t length,
                                        bool for_write)
{
    struct obmm_cmd_update_range cmd;
    uintptr_t start;
    uintptr_t end;
    uintptr_t page_size;

    if (!slot || !slot->region.addr || slot->region.fd < 0) {
        return -1;
    }
    if (slot->map_osync) {
        return 0;
    }
    if (length == 0 || offset + length > slot->region.len) {
        return -1;
    }
    start = (uintptr_t)slot->region.addr + (uintptr_t)offset;
    end = start + (uintptr_t)length;
    page_size = (uintptr_t)sysconf(_SC_PAGESIZE);
    if (page_size == 0) {
        page_size = 4096;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.start = start & ~(uintptr_t)(page_size - 1);
    cmd.end = (end + page_size - 1) & ~(uintptr_t)(page_size - 1);
    cmd.mem_state = (slot->map_osync ? OBMM_SHM_MEM_NORMAL_NC : OBMM_SHM_MEM_NORMAL) |
                    OBMM_SHM_MEM_READWRITE;
    cmd.cache_ops = for_write ? OBMM_SHM_CACHE_WB_INVAL : OBMM_SHM_CACHE_INVAL;
    if (ioctl(slot->region.fd, OBMM_SHMDEV_UPDATE_RANGE, &cmd) == 0) {
        return 0;
    }
    fprintf(stderr,
            "[mem_service] update_range_failed owner=%d write=%d fd=%d start=%#llx end=%#llx errno=%d\n",
            slot->owner_idx + 1, for_write ? 1 : 0, slot->region.fd,
            (unsigned long long)cmd.start, (unsigned long long)cmd.end, errno);
    return -1;
}

static int mem_service_update_region_range(const struct mem_service_cluster_slot *slot, bool for_write)
{
    return mem_service_update_region_range_at(slot, 0, sizeof(struct mem_service_cluster_payload), for_write);
}

static int MEM_SERVICE_MAYBE_UNUSED mem_service_sync_remote_range(
    const struct mem_service_cluster_slot *slot,
    uint64_t offset,
    uint64_t length)
{
    obmm_cmd_sync_remote_range cmd;
    struct stat st;
    char fd_path[64];
    char fd_target[256];
    ssize_t n;

    if (!slot || !slot->region.addr || slot->region.fd < 0) {
        return -1;
    }
    if (!slot->map_osync || slot->is_local) {
        return 0;
    }
    if (length == 0) {
        return 0;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.offset = offset;
    cmd.length = length;
    if (ioctl(slot->region.fd, OBMM_SHMDEV_SYNC_REMOTE_RANGE, &cmd) == 0) {
        return 0;
    }
    fd_target[0] = '\0';
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", slot->region.fd);
    n = readlink(fd_path, fd_target, sizeof(fd_target) - 1);
    if (n > 0) {
        fd_target[n] = '\0';
    } else {
        snprintf(fd_target, sizeof(fd_target), "<readlink:%s>", strerror(errno));
    }
    fprintf(stderr,
            "[mem_service] sync_remote_range_failed owner=%d fd=%d target=%s offset=%#" PRIx64
            " len=%#" PRIx64 " errno=%d",
            slot->owner_idx + 1,
            slot->region.fd,
            fd_target,
            offset,
            length,
            errno);
    if (fstat(slot->region.fd, &st) == 0) {
        fprintf(stderr,
                " mode=%#o rdev=%u:%u",
                st.st_mode,
                major(st.st_rdev),
                minor(st.st_rdev));
    }
    fputc('\n', stderr);
    return -1;
}

#include "mem_service_cluster_payload.inc"

#include "mem_service_object_refs.inc"

static void mem_service_fill_obmm_object_payload(uint8_t *dst,
                                           uint64_t len,
                                           uint32_t owner_node,
                                           uint32_t payload_kind)
{
    uint64_t i;

    for (i = 0; i < len; ++i) {
        dst[i] = (uint8_t)((i * 17ULL + (uint64_t)(owner_node + 1U) * 29ULL +
                            (uint64_t)payload_kind * 53ULL) & 0xffU);
    }
    if (len >= 4104U) {
        memcpy(dst + 0, "MSOBMM00", 8);
        memcpy(dst + 248, "MSOBMM248", 9);
        memcpy(dst + 256, "MSOBMM256", 9);
        memcpy(dst + 4088, "MSOBMM4088", 10);
        memcpy(dst + 4096, "MSOBMM4096", 10);
    }
}

static const char *mem_service_object_kind_name(uint32_t payload_kind)
{
    switch (payload_kind) {
    case MEM_SERVICE_OBMM_KIND_WEIGHT_TILE:
        return "weight_tile";
    case MEM_SERVICE_OBMM_KIND_KVCACHE_BLOCK:
        return "kvcache_block";
    case MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_INPUT:
        return "hidden_range_input";
    case MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT:
        return "hidden_range_output";
    case MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT:
        return "hidden_range_runtime_output";
    case MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT:
        return "qwen3_token_result";
    case MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE:
        return "qwen3_kv_state";
    case MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY:
        return "qwen3_engram_history";
    case MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES:
        return "qwen3_engram_candidates";
    case MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED:
        return "qwen3_engram_selected";
    case MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE:
        return "qwen3_engram_state";
    default:
        return "unknown";
    }
}

static int mem_service_payload_arena_alloc(struct mem_service_cluster_runtime *rt,
                                     uint64_t bytes,
                                     uint64_t align,
                                     uint64_t *offset_out)
{
    uint64_t offset;
    uint64_t end;

    if (!rt || !offset_out || rt->local_idx < 0 ||
        rt->local_idx >= rt->node_count || bytes == 0) {
        return -1;
    }
    if (align == 0) {
        align = 64;
    }
    if (rt->payload_arena_base == 0) {
        rt->payload_arena_base =
            obmm_align_up_u64(MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET, align);
        rt->payload_arena_next = rt->payload_arena_base;
        rt->payload_arena_high_water = rt->payload_arena_base;
    }
    offset = obmm_align_up_u64(rt->payload_arena_next, align);
    end = offset + bytes;
    if (end < offset ||
        !rt->slots[rt->local_idx].region.addr ||
        end > rt->slots[rt->local_idx].region.len) {
        printf("[mem_service] gap obmm_pool_allocator=exhausted local=node%d offset=0x%016" PRIx64
               " bytes=%" PRIu64 " payload_bytes=%zu arena_base=0x%016" PRIx64 "\n",
               rt->local_idx + 1,
               offset,
               bytes,
               rt->slots[rt->local_idx].region.len,
               rt->payload_arena_base);
        return -1;
    }
    rt->payload_arena_next = end;
    if (end > rt->payload_arena_high_water) {
        rt->payload_arena_high_water = end;
    }
    *offset_out = offset;
    return 0;
}

#include "mem_service_qwen3_runtime.inc"

static int mem_service_put_obmm_object_record(struct mem_service *svc,
                                        enum mem_service_record_kind record_kind,
                                        const char *key,
                                        uint32_t owner_node,
                                        uint32_t payload_kind,
                                        uint64_t offset,
                                        uint64_t len,
                                        uint64_t checksum,
                                        struct mem_service_record *resolved_out)
{
    struct mem_service_record *rec;
    uint64_t next_version = 1U;

    if (!svc || !key || len == 0) {
        return -1;
    }
    rec = mem_service_find_record(svc, key);
    if (rec && rec->version != UINT64_MAX) {
        next_version = rec->version + 1U;
    }
    if (!rec) {
        rec = mem_service_alloc_record(svc);
    }
    if (!rec) {
        rec = mem_service_recycle_qwen3_runtime_record(svc, key);
    }
    if (!rec) {
        return -1;
    }
    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = record_kind;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    rec->placement_node = owner_node;
    rec->placement_level = 2U;
    rec->hot_segment_id = offset;
    rec->state = MEM_SERVICE_KVCACHE_STATE_HOT;
    rec->version = next_version;
    rec->last_result_segment = offset + len;
    rec->object_owner_node = owner_node;
    rec->object_payload_kind = payload_kind;
    rec->object_backing_offset = offset;
    rec->object_backing_len = len;
    rec->object_payload_checksum = checksum;
    if (resolved_out) {
        memcpy(resolved_out, rec, sizeof(*resolved_out));
    }
    return 0;
}

#include "mem_service_cluster_read.inc"

#include "mem_service_cluster_runtime.inc"

#include "mem_service_cluster_queue.inc"

#include "mem_service_records.inc"
#include "mem_service_qwen3_records.inc"
#include "mem_service_keys.inc"
#include "mem_service_metadata.inc"

#include "mem_service_cluster_observe.inc"

#include "mem_service_obmm_object_flow.inc"

static int mem_service_obmm_service_v0_wait_runtime_range_input_view_internal(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    bool allow_terminal_commit,
    struct mem_service_object_payload_view *view_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_qwen3_layer_range_placement local_placement;
    struct mem_service_qwen3_layer_range_placement source_placement;
    struct mem_service_cluster_slot *source_slot = NULL;
    struct mem_service_record remote_hidden_output;
    struct obmm_desc handoff_desc;
    struct obmm_desc terminal_desc;
    char ingress_key[96];
    char token_result_key[96];
    long deadline;
    long wait_enter_ms;
    long found_local_ms = 0;
    long found_ms = 0;
    long checksum_ms;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    long producer_to_found_ms = 0;
    long producer_to_found_monotonic_ms = 0;
    uint64_t activate_ms = 0;
    uint64_t metadata_ms = 0;
    uint32_t attempts = 0;
    uint16_t expected_epoch;
    unsigned int relax_attempt = 0;
    uint32_t source_node = UINT32_MAX;
    uint32_t terminal_source_node = UINT32_MAX;
    uint64_t hidden_range_bytes = mem_service_qwen3_handoff_hidden_bytes(decode_step);
    const uint8_t *payload_view;
    uint64_t checksum;
    bool terminal_desc_found = false;

    if (!view_out ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    memset(&local_placement, 0, sizeof(local_placement));
    local_placement.owner_node = local_node;
    local_placement.next_owner_node = (local_node + 1U) % cluster_node_count;
    local_placement.terminal = (local_node + 1U == cluster_node_count);
    if (mem_service_qwen3_layer_range_for_node(local_node,
                                         cluster_node_count,
                                         &local_placement.layer_start,
                                         &local_placement.layer_end,
                                         &local_placement.next_owner_node) != 0) {
        return -1;
    }
    local_placement.layer_count =
        local_placement.layer_end - local_placement.layer_start;
    local_placement.terminal =
        local_placement.next_owner_node < local_placement.owner_node;
    if (local_placement.layer_start == 0 && decode_step == 0) {
        return 0;
    }
    if (local_placement.layer_start == 0) {
        struct mem_service_record token_record;
        struct mem_service_cluster_slot *owner_slot;
        char token_result_key[96];
        struct obmm_desc token_desc;
        uint64_t payload_words[8];
        uint64_t checksum;
        bool token_input_found = false;
        bool token_desc_found = false;

        if (mem_service_cluster_runtime_require(rt) != 0) {
            return -1;
        }
        memset(&token_desc, 0, sizeof(token_desc));
        wait_enter_ms = obmm_now_ms();
        found_ms = mem_service_wallclock_ms();
        token_result_key[0] = '\0';
        expected_epoch = (uint16_t)(decode_step & 0xffffU);
        if (expected_epoch == 0) {
            expected_epoch = 1;
        }
        deadline = wait_enter_ms + mem_service_qwen3_runtime_range_wait_ms();
        while (obmm_now_ms() < deadline) {
            int owner_idx;

            attempts++;
            for (owner_idx = 0;
                 !token_desc_found && owner_idx < rt->node_count;
                 ++owner_idx) {
                struct obmm_desc rx;

                if (mem_service_take_pending_qwen3_token_result_desc(rt,
                                                               owner_idx,
                                                               expected_epoch,
                                                               &token_desc)) {
                    source_node = (uint32_t)owner_idx;
                    token_desc_found = true;
                    break;
                }
                if (owner_idx == rt->local_idx ||
                    !rt->ingress_queues[owner_idx]) {
                    continue;
                }
                while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                    if (mem_service_qwen3_token_result_desc_matches(&rx,
                                                              expected_epoch)) {
                        token_desc = rx;
                        source_node = (uint32_t)owner_idx;
                        token_desc_found = true;
                        break;
                    }
                    mem_service_stash_pending_desc(rt, owner_idx, &rx);
                }
            }
            if (!token_desc_found) {
                mem_service_cpu_relax_wait(&relax_attempt);
                continue;
            }
            if (!rt->slots[source_node].region.addr) {
                long activate_start_ms = obmm_now_ms();

                if (mem_service_activate_remote_slot(rt, (int)source_node) != 0) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                activate_ms += (uint64_t)(obmm_now_ms() - activate_start_ms);
            }
            owner_slot = &rt->slots[source_node];
            memset(&token_record, 0, sizeof(token_record));
            {
                long metadata_start_ms = obmm_now_ms();
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;

                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                if (!owner_slot->region.addr ||
                    !mem_service_try_read_stable_compact_summary_region(owner_slot,
                                                                  &compact,
                                                                  &seen) ||
                    !mem_service_slot_find_record_by_obmm_object_backing(
                        owner_slot,
                        MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT,
                        MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                        token_desc.payload_offset,
                        token_desc.payload_len,
                        token_desc.cookie,
                        &token_record)) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                metadata_ms = (uint64_t)(obmm_now_ms() - metadata_start_ms);
            }
            snprintf(token_result_key,
                     sizeof(token_result_key),
                     "%s",
                     token_record.key);
            if (token_record.kind != MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                token_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
                token_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES ||
                token_record.object_backing_offset != token_desc.payload_offset ||
                token_record.object_backing_len != token_desc.payload_len ||
                token_desc.cookie !=
                    (uint32_t)(token_record.object_payload_checksum ^
                               (token_record.object_payload_checksum >> 32)) ||
                token_record.object_backing_offset > owner_slot->region.len ||
                token_record.object_backing_len >
                    owner_slot->region.len - token_record.object_backing_offset) {
                printf("[mem_service] gap qwen3_range_forward=runtime_token_input_invalid local=node%u source=node%u key=%s\n",
                       local_node + 1U,
                       source_node + 1U,
                       token_result_key);
                return -1;
            }
            payload_view =
                (const uint8_t *)owner_slot->region.addr +
                token_record.object_backing_offset;
            memcpy(payload_words,
                   payload_view,
                   MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
            if (payload_words[0] != decode_step - 1U) {
                printf("[mem_service] gap qwen3_range_forward=runtime_token_input_step_mismatch local=node%u source=node%u key=%s got=%" PRIu64 " expected=%" PRIu64 "\n",
                       local_node + 1U,
                       source_node + 1U,
                       token_result_key,
                       payload_words[0],
                       decode_step - 1U);
                return -1;
            }
            checksum = mem_service_qwen3_hidden_payload_checksum(
                payload_view,
                MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
            if (checksum != token_record.object_payload_checksum) {
                mem_service_cpu_relax_wait(&relax_attempt);
                continue;
            }
            token_input_found = true;
            break;
        }
        if (!token_input_found) {
            printf("[mem_service] gap qwen3_range_forward=runtime_token_input_wait_failed local=node%u source=%s step=%" PRIu64 " epoch=%u attempts=%u desc_found=%u\n",
                   local_node + 1U,
                   source_node == UINT32_MAX ? "none" : "descriptor",
                   decode_step - 1U,
                   expected_epoch,
                   attempts,
                   token_desc_found ? 1U : 0U);
            return -1;
        }
        found_local_ms = obmm_now_ms();
        found_ms = mem_service_wallclock_ms();
        producer_publish_ms = (long)token_record.last_result_segment;
        producer_publish_monotonic_ms =
            (long)token_record.object_publish_monotonic_ms;
        producer_clock_offset_ms =
            (long)token_record.object_publish_supernode_offset_ms;
        if (producer_publish_ms > 0 && found_ms > 0) {
            producer_to_found_ms = found_ms - producer_publish_ms;
        }
        if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
            producer_to_found_monotonic_ms =
                found_local_ms - producer_publish_monotonic_ms;
        }
        memset(view_out, 0, sizeof(*view_out));
        view_out->data = payload_view;
        view_out->len = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES;
        view_out->checksum = checksum;
        view_out->owner_node = source_node;
        view_out->payload_kind = token_record.object_payload_kind;
        view_out->backing_offset = token_record.object_backing_offset;
        memcpy(view_out->token_result_words,
               payload_words,
               MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
        if (mem_service_record_to_lingqu_obmm_ref(&token_record,
                                            &view_out->object_ref) != 0) {
            return -1;
        }
        view_out->wait_enter_monotonic_ms =
            wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
        view_out->found_monotonic_ms =
            found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
        view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
        view_out->producer_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        view_out->producer_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        view_out->producer_clock_offset_ms = producer_clock_offset_ms;
        view_out->producer_to_found_supernode_ms = producer_to_found_ms;
        view_out->producer_to_found_monotonic_ms = producer_to_found_monotonic_ms;
        view_out->source_node = source_node;
        view_out->wait_attempts = attempts;
        view_out->activate_ms = activate_ms;
        view_out->metadata_ms = metadata_ms;
        printf("[mem_service] stage qwen3_range_forward_runtime_input_resolve local=node%u source=node%u key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) input_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " token=%" PRIu64 " wait_enter_to_found_ms=%ld producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld producer_to_found_ms=%ld producer_to_found_mono_ms=%ld attempts=%u activate_ms=%" PRIu64 " metadata_ms=%" PRIu64 " copy_ms=0 checksum_ms=0 validation=object_desc_backing queue=obmm_spsc receive=descriptor metadata=lingqu_object_service backing=obmm_shmem source=terminal_token_result target=mapped_view status=ok\n",
               local_node + 1U,
               source_node + 1U,
               token_result_key,
               view_out->object_ref.key_hash,
               view_out->object_ref.object_version,
               local_placement.layer_start,
               local_placement.layer_end,
               checksum,
               view_out->len,
               payload_words[1],
               found_local_ms > 0 ? found_local_ms - wait_enter_ms : -1L,
               producer_publish_ms,
               producer_publish_monotonic_ms,
               producer_clock_offset_ms,
               producer_to_found_ms,
               producer_to_found_monotonic_ms,
               view_out->wait_attempts,
               activate_ms,
               metadata_ms);
        return 0;
    }
    memset(&source_placement, 0, sizeof(source_placement));
    source_placement.owner_node = local_node - 1U;
    source_placement.next_owner_node = local_node;
    source_placement.terminal = false;
    if (mem_service_qwen3_layer_range_for_node(source_placement.owner_node,
                                         cluster_node_count,
                                         &source_placement.layer_start,
                                         &source_placement.layer_end,
                                         &source_placement.next_owner_node) != 0 ||
        source_placement.next_owner_node != local_node) {
        return -1;
    }
    source_placement.layer_count =
        source_placement.layer_end - source_placement.layer_start;
    source_node = source_placement.owner_node;
    if (mem_service_cluster_runtime_require(rt) != 0) {
        return -1;
    }
    if (source_node >= cluster_node_count || !rt->ingress_queues[source_node]) {
        return -1;
    }
    ingress_key[0] = '\0';

    memset(&remote_hidden_output, 0, sizeof(remote_hidden_output));
    memset(&handoff_desc, 0, sizeof(handoff_desc));
    memset(&terminal_desc, 0, sizeof(terminal_desc));
    expected_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (expected_epoch == 0) {
        expected_epoch = 1;
    }
    wait_enter_ms = obmm_now_ms();
    deadline = wait_enter_ms + mem_service_qwen3_runtime_range_wait_ms();
    while (obmm_now_ms() < deadline) {
        struct obmm_desc rx;

        attempts++;
        if (allow_terminal_commit) {
            snprintf(token_result_key,
                     sizeof(token_result_key),
                     "tokens/%s/decode-step%" PRIu64,
                     mem_service_qwen3_model_key(),
                     decode_step);
            if (!terminal_desc_found) {
                for (int owner_idx = 0;
                     !terminal_desc_found && owner_idx < rt->node_count;
                     ++owner_idx) {
                    struct obmm_desc rx;

                    if (mem_service_take_pending_qwen3_token_result_desc(
                            rt,
                            owner_idx,
                            expected_epoch,
                            &terminal_desc)) {
                        terminal_source_node = (uint32_t)owner_idx;
                        terminal_desc_found = true;
                        break;
                    }
                    if (owner_idx == rt->local_idx ||
                        !rt->ingress_queues[owner_idx]) {
                        continue;
                    }
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_qwen3_token_result_desc_matches(
                                &rx,
                                expected_epoch)) {
                            terminal_desc = rx;
                            terminal_source_node = (uint32_t)owner_idx;
                            terminal_desc_found = true;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
            }
            if (terminal_desc_found &&
                terminal_source_node < (uint32_t)rt->node_count) {
                struct mem_service_cluster_slot *token_slot;
                struct mem_service_record token_record;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                uint64_t payload_words[8];
                uint64_t token_checksum;

                if (terminal_source_node != (uint32_t)rt->local_idx &&
                    !rt->slots[terminal_source_node].region.addr) {
                    long activate_start_ms = obmm_now_ms();

                    if (mem_service_activate_remote_slot(
                            rt,
                            (int)terminal_source_node) != 0) {
                        mem_service_cpu_relax_wait(&relax_attempt);
                        continue;
                    }
                    activate_ms +=
                        (uint64_t)(obmm_now_ms() - activate_start_ms);
                }
                token_slot = &rt->slots[terminal_source_node];
                memset(&token_record, 0, sizeof(token_record));
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                {
                    long metadata_start_ms = obmm_now_ms();

                    if (!token_slot->region.addr ||
                        !mem_service_try_read_stable_compact_summary_region(
                            token_slot,
                            &compact,
                            &seen) ||
                        !mem_service_slot_find_record_by_obmm_object_backing(
                            token_slot,
                            MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT,
                            MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                            terminal_desc.payload_offset,
                            terminal_desc.payload_len,
                            terminal_desc.cookie,
                            &token_record)) {
                        mem_service_cpu_relax_wait(&relax_attempt);
                        continue;
                    }
                    metadata_ms +=
                        (uint64_t)(obmm_now_ms() - metadata_start_ms);
                }
                if (token_record.kind != MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                    token_record.object_payload_kind !=
                        MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
                    token_record.object_backing_len !=
                        MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES ||
                    token_record.object_backing_offset !=
                        terminal_desc.payload_offset ||
                    token_record.object_backing_len != terminal_desc.payload_len ||
                    terminal_desc.cookie !=
                        (uint32_t)(token_record.object_payload_checksum ^
                                   (token_record.object_payload_checksum >> 32)) ||
                    token_record.object_backing_offset > token_slot->region.len ||
                    token_record.object_backing_len >
                        token_slot->region.len -
                            token_record.object_backing_offset) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                payload_view =
                    (const uint8_t *)token_slot->region.addr +
                    token_record.object_backing_offset;
                memcpy(payload_words,
                       payload_view,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (payload_words[0] != decode_step) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                token_checksum = mem_service_qwen3_hidden_payload_checksum(
                    payload_view,
                    MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (token_checksum != token_record.object_payload_checksum) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                found_local_ms = obmm_now_ms();
                found_ms = mem_service_wallclock_ms();
                producer_publish_ms = (long)token_record.last_result_segment;
                producer_publish_monotonic_ms =
                    (long)token_record.object_publish_monotonic_ms;
                producer_clock_offset_ms =
                    (long)token_record.object_publish_supernode_offset_ms;
                if (producer_publish_ms > 0 && found_ms > 0) {
                    producer_to_found_ms = found_ms - producer_publish_ms;
                }
                if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
                    producer_to_found_monotonic_ms =
                        found_local_ms - producer_publish_monotonic_ms;
                }
                memset(view_out, 0, sizeof(*view_out));
                view_out->data = payload_view;
                view_out->len = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES;
                view_out->checksum = token_checksum;
                view_out->owner_node = terminal_source_node;
                view_out->payload_kind = token_record.object_payload_kind;
                view_out->backing_offset = token_record.object_backing_offset;
                memcpy(view_out->token_result_words,
                       payload_words,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (mem_service_record_to_lingqu_obmm_ref(&token_record,
                                                    &view_out->object_ref) != 0) {
                    return -1;
                }
                view_out->wait_enter_monotonic_ms =
                    wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
                view_out->found_monotonic_ms =
                    found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
                view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
                view_out->producer_publish_supernode_ms =
                    producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
                view_out->producer_publish_monotonic_ms =
                    producer_publish_monotonic_ms > 0 ?
                        (uint64_t)producer_publish_monotonic_ms :
                        0;
                view_out->producer_clock_offset_ms = producer_clock_offset_ms;
                view_out->producer_to_found_supernode_ms = producer_to_found_ms;
                view_out->producer_to_found_monotonic_ms =
                    producer_to_found_monotonic_ms;
                view_out->source_node = terminal_source_node;
                view_out->wait_attempts = attempts;
                view_out->activate_ms = activate_ms;
                view_out->metadata_ms = metadata_ms;
                printf("[mem_service] stage qwen3_decode_round_terminal_committed"
                       " local=node%u source=node%u step=%" PRIu64
                       " token=%" PRIu64 " object_key=%s"
                       " checksum=0x%016" PRIx64
                       " source=terminal_token_result"
                       " target=decode_round_scheduler receive=descriptor"
                       " status=committed\n",
                       local_node + 1U,
                       terminal_source_node + 1U,
                       decode_step,
                       payload_words[1],
                       token_record.key,
                       token_checksum);
                return 0;
            }
            for (int owner_idx = 0; owner_idx < rt->node_count; ++owner_idx) {
                struct mem_service_cluster_slot *token_slot;
                struct mem_service_record token_record;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                uint64_t payload_words[8];
                uint64_t token_checksum;

                if (owner_idx != rt->local_idx &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    continue;
                }
                token_slot = &rt->slots[owner_idx];
                memset(&token_record, 0, sizeof(token_record));
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                if (!token_slot->region.addr ||
                    !mem_service_try_read_stable_compact_summary_region(token_slot,
                                                                  &compact,
                                                                  &seen) ||
                    !mem_service_slot_find_record(token_slot,
                                            token_result_key,
                                            &token_record) ||
                    token_record.kind != MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                    token_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
                    token_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES ||
                    token_record.object_backing_offset > token_slot->region.len ||
                    token_record.object_backing_len >
                        token_slot->region.len - token_record.object_backing_offset) {
                    continue;
                }
                payload_view =
                    (const uint8_t *)token_slot->region.addr +
                    token_record.object_backing_offset;
                memcpy(payload_words,
                       payload_view,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (payload_words[0] != decode_step) {
                    continue;
                }
                token_checksum = mem_service_qwen3_hidden_payload_checksum(
                    payload_view,
                    MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (token_checksum != token_record.object_payload_checksum) {
                    continue;
                }
                found_local_ms = obmm_now_ms();
                found_ms = mem_service_wallclock_ms();
                producer_publish_ms = (long)token_record.last_result_segment;
                producer_publish_monotonic_ms =
                    (long)token_record.object_publish_monotonic_ms;
                producer_clock_offset_ms =
                    (long)token_record.object_publish_supernode_offset_ms;
                if (producer_publish_ms > 0 && found_ms > 0) {
                    producer_to_found_ms = found_ms - producer_publish_ms;
                }
                if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
                    producer_to_found_monotonic_ms =
                        found_local_ms - producer_publish_monotonic_ms;
                }
                memset(view_out, 0, sizeof(*view_out));
                view_out->data = payload_view;
                view_out->len = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES;
                view_out->checksum = token_checksum;
                view_out->owner_node = (uint32_t)owner_idx;
                view_out->payload_kind = token_record.object_payload_kind;
                view_out->backing_offset = token_record.object_backing_offset;
                memcpy(view_out->token_result_words,
                       payload_words,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (mem_service_record_to_lingqu_obmm_ref(&token_record,
                                                    &view_out->object_ref) != 0) {
                    return -1;
                }
                view_out->wait_enter_monotonic_ms =
                    wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
                view_out->found_monotonic_ms =
                    found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
                view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
                view_out->producer_publish_supernode_ms =
                    producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
                view_out->producer_publish_monotonic_ms =
                    producer_publish_monotonic_ms > 0 ?
                        (uint64_t)producer_publish_monotonic_ms :
                        0;
                view_out->producer_clock_offset_ms = producer_clock_offset_ms;
                view_out->producer_to_found_supernode_ms = producer_to_found_ms;
                view_out->producer_to_found_monotonic_ms =
                    producer_to_found_monotonic_ms;
                view_out->source_node = (uint32_t)owner_idx;
                view_out->wait_attempts = attempts;
                view_out->activate_ms = activate_ms;
                view_out->metadata_ms = metadata_ms;
                printf("[mem_service] stage qwen3_decode_round_terminal_committed"
                       " local=node%u source=node%d step=%" PRIu64
                       " token=%" PRIu64 " object_key=%s"
                       " checksum=0x%016" PRIx64
                       " source=terminal_token_result target=decode_round_scheduler"
                       " status=committed\n",
                       local_node + 1U,
                       owner_idx + 1,
                       decode_step,
                       payload_words[1],
                       token_result_key,
                       token_checksum);
                return 0;
            }
        }
        if (mem_service_take_pending_runtime_range_input_desc(rt,
                                                        (int)source_node,
                                                        expected_epoch,
                                                        &handoff_desc)) {
            break;
        }
        while (obmm_spsc_pop(rt->ingress_queues[source_node], &rx) == 0) {
            if (mem_service_runtime_range_input_desc_matches(&rx,
                                                       expected_epoch)) {
                handoff_desc = rx;
                break;
            }
            mem_service_stash_pending_desc(rt, (int)source_node, &rx);
        }
        if (handoff_desc.type == OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
            break;
        }
        mem_service_cpu_relax_wait(&relax_attempt);
    }
    if (!mem_service_runtime_range_input_desc_matches(&handoff_desc,
                                                expected_epoch)) {
        printf("[mem_service] gap qwen3_range_forward=runtime_ingress_desc_wait_failed local=node%u source=node%u step=%" PRIu64 " attempts=%u\n",
               local_node + 1U,
               source_node + 1U,
               decode_step,
               attempts);
        return -1;
    }
    found_local_ms = obmm_now_ms();
    found_ms = mem_service_wallclock_ms();
    if (!rt->slots[source_node].region.addr) {
        long activate_start_ms = obmm_now_ms();

        if (mem_service_activate_remote_slot(rt, (int)source_node) != 0) {
            return -1;
        }
        activate_ms += (uint64_t)(obmm_now_ms() - activate_start_ms);
    }
    {
        bool metadata_found = false;

        source_slot = &rt->slots[source_node];
        while (obmm_now_ms() < deadline) {
            long metadata_start_ms = obmm_now_ms();
            struct mem_service_cluster_payload_compact_summary compact;
            struct mem_service_cluster_payload_header seen;

            memset(&compact, 0, sizeof(compact));
            memset(&seen, 0, sizeof(seen));
            if (mem_service_try_read_stable_compact_summary_region(source_slot,
                                                             &compact,
                                                             &seen) &&
                mem_service_slot_find_record_by_obmm_object_backing(
                    source_slot,
                    MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT,
                    MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                    handoff_desc.payload_offset,
                    handoff_desc.payload_len,
                    handoff_desc.cookie,
                    &remote_hidden_output)) {
                metadata_ms = (uint64_t)(obmm_now_ms() - metadata_start_ms);
                metadata_found = true;
                break;
            }
            mem_service_cpu_relax_wait(&relax_attempt);
        }
        if (!metadata_found) {
            printf("[mem_service] gap qwen3_range_forward=runtime_ingress_metadata_wait_failed local=node%u source=node%u step=%" PRIu64 " attempts=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 "\n",
                   local_node + 1U,
                   source_node + 1U,
                   decode_step,
                   attempts,
                   handoff_desc.payload_offset,
                   (uint64_t)handoff_desc.payload_len);
            return -1;
        }
    }
    snprintf(ingress_key,
             sizeof(ingress_key),
             "%s",
             remote_hidden_output.key);
    if (!source_slot ||
        remote_hidden_output.object_payload_kind !=
            MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT ||
        remote_hidden_output.object_backing_len != hidden_range_bytes ||
        remote_hidden_output.object_backing_offset != handoff_desc.payload_offset ||
        remote_hidden_output.object_backing_len != handoff_desc.payload_len ||
        handoff_desc.cookie !=
            (uint32_t)(remote_hidden_output.object_payload_checksum ^
                       (remote_hidden_output.object_payload_checksum >> 32)) ||
        !source_slot->region.addr ||
        remote_hidden_output.object_backing_offset + remote_hidden_output.object_backing_len >
            source_slot->region.len) {
        printf("[mem_service] gap qwen3_range_forward=runtime_ingress_wait_failed local=node%u source=node%u step=%" PRIu64 " key=%s\n",
               local_node + 1U,
               source_node + 1U,
               decode_step,
               ingress_key);
        return -1;
    }
    payload_view =
        (const uint8_t *)source_slot->region.addr +
        remote_hidden_output.object_backing_offset;
    checksum = remote_hidden_output.object_payload_checksum;
    checksum_ms = 0;
    producer_publish_ms = (long)remote_hidden_output.last_result_segment;
    producer_publish_monotonic_ms =
        (long)remote_hidden_output.object_publish_monotonic_ms;
    producer_clock_offset_ms =
        (long)remote_hidden_output.object_publish_supernode_offset_ms;
    if (producer_publish_ms > 0 && found_ms > 0) {
        producer_to_found_ms = found_ms - producer_publish_ms;
    }
    if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
        producer_to_found_monotonic_ms =
            found_local_ms - producer_publish_monotonic_ms;
    }
    memset(view_out, 0, sizeof(*view_out));
    view_out->data = payload_view;
    view_out->len = hidden_range_bytes;
    view_out->checksum = checksum;
    view_out->owner_node = source_node;
    view_out->payload_kind = remote_hidden_output.object_payload_kind;
    view_out->backing_offset = remote_hidden_output.object_backing_offset;
    if (mem_service_record_to_lingqu_obmm_ref(&remote_hidden_output,
                                        &view_out->object_ref) != 0) {
        return -1;
    }
    view_out->wait_enter_monotonic_ms =
        wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
    view_out->found_monotonic_ms =
        found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
    view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
    view_out->producer_publish_supernode_ms =
        producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
    view_out->producer_publish_monotonic_ms =
        producer_publish_monotonic_ms > 0 ?
            (uint64_t)producer_publish_monotonic_ms :
            0;
    view_out->producer_clock_offset_ms = producer_clock_offset_ms;
    view_out->producer_to_found_supernode_ms = producer_to_found_ms;
    view_out->producer_to_found_monotonic_ms = producer_to_found_monotonic_ms;
    view_out->source_node = source_node;
    view_out->wait_attempts = attempts;
    view_out->activate_ms = activate_ms;
    view_out->metadata_ms = metadata_ms;
    printf("[mem_service] stage qwen3_range_forward_runtime_input_resolve local=node%u source=node%u key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) input_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " wait_enter_to_found_ms=%ld producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld producer_to_found_ms=%ld producer_to_found_mono_ms=%ld attempts=%u activate_ms=%" PRIu64 " metadata_ms=%" PRIu64 " copy_ms=0 checksum_ms=%ld validation=object_desc_backing queue=obmm_spsc receive=descriptor metadata=lingqu_object_service backing=obmm_shmem target=mapped_view status=ok\n",
           local_node + 1U,
           source_node + 1U,
           ingress_key,
           view_out->object_ref.key_hash,
           view_out->object_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           checksum,
           hidden_range_bytes,
           found_local_ms > 0 ? found_local_ms - wait_enter_ms : -1L,
           producer_publish_ms,
           producer_publish_monotonic_ms,
           producer_clock_offset_ms,
           producer_to_found_ms,
           producer_to_found_monotonic_ms,
           attempts,
           activate_ms,
           metadata_ms,
           checksum_ms);
    return 0;
}

int mem_service_obmm_service_v0_wait_runtime_range_input_view(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_object_payload_view *view_out)
{
    return mem_service_obmm_service_v0_wait_runtime_range_input_view_internal(
        local_node,
        cluster_node_count,
        decode_step,
        false,
        view_out);
}

int mem_service_obmm_service_v0_wait_scheduler_work_item(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_scheduler_work_item *item_out)
{
    struct mem_service_qwen3_layer_range_placement local_placement;
    struct mem_service_object_payload_view view;

    if (!item_out ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    memset(item_out, 0, sizeof(*item_out));
    memset(&local_placement, 0, sizeof(local_placement));
    local_placement.owner_node = local_node;
    local_placement.next_owner_node = (local_node + 1U) % cluster_node_count;
    local_placement.terminal = (local_node + 1U == cluster_node_count);
    if (mem_service_qwen3_layer_range_for_node(local_node,
                                         cluster_node_count,
                                         &local_placement.layer_start,
                                         &local_placement.layer_end,
                                         &local_placement.next_owner_node) != 0) {
        return -1;
    }
    local_placement.layer_count =
        local_placement.layer_end - local_placement.layer_start;
    local_placement.terminal =
        local_placement.next_owner_node < local_placement.owner_node;

    if (local_placement.layer_start == 0 && decode_step == 0) {
        item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD;
        return 0;
    }

    memset(&view, 0, sizeof(view));
    if (mem_service_obmm_service_v0_wait_runtime_range_input_view_internal(
            local_node,
            cluster_node_count,
            decode_step,
            local_placement.layer_start > 0,
            &view) != 0 ||
        !view.data) {
        return -1;
    }
    if (view.payload_kind == MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT &&
        view.len == MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES) {
        uint64_t token_words[8];

        if (local_placement.layer_start == 0) {
            item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD;
            item_out->range_input = view;
            return 0;
        }
        memcpy(token_words,
               view.token_result_words,
               MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
        if (token_words[0] != decode_step) {
            printf("[mem_service] gap qwen3_scheduler_work_item=terminal_step_mismatch local=node%u got=%" PRIu64 " expected=%" PRIu64 "\n",
                   local_node + 1U,
                   token_words[0],
                   decode_step);
            return -1;
        }
        item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_NO_DISPATCH;
        item_out->terminal_step = token_words[0];
        item_out->terminal_token = token_words[1];
        item_out->terminal_owner_node = view.source_node;
        item_out->checksum = view.checksum;
        item_out->wait_enter_monotonic_ms = view.wait_enter_monotonic_ms;
        item_out->found_monotonic_ms = view.found_monotonic_ms;
        item_out->ready_monotonic_ms = view.ready_monotonic_ms;
        item_out->producer_publish_supernode_ms =
            view.producer_publish_supernode_ms;
        item_out->producer_publish_monotonic_ms =
            view.producer_publish_monotonic_ms;
        item_out->producer_clock_offset_ms = view.producer_clock_offset_ms;
        item_out->producer_to_found_supernode_ms =
            view.producer_to_found_supernode_ms;
        item_out->producer_to_found_monotonic_ms =
            view.producer_to_found_monotonic_ms;
        item_out->wait_attempts = view.wait_attempts;
        item_out->activate_ms = view.activate_ms;
        item_out->metadata_ms = view.metadata_ms;
        return 0;
    }
    if (view.payload_kind != MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT) {
        printf("[mem_service] gap qwen3_scheduler_work_item=input_kind_invalid local=node%u kind=%u bytes=%" PRIu64 "\n",
               local_node + 1U,
               view.payload_kind,
               view.len);
        return -1;
    }
    item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD;
    item_out->range_input = view;
    return 0;
}

int mem_service_obmm_service_v0_wait_runtime_range_input(uint32_t local_node,
                                                   uint32_t cluster_node_count,
                                                   uint64_t decode_step,
                                                   uint8_t *payload_out,
                                                   uint64_t payload_len,
                                                   uint64_t *checksum_out)
{
    struct mem_service_object_payload_view view;
    uint64_t hidden_range_bytes = mem_service_qwen3_handoff_hidden_bytes(decode_step);

    if (!payload_out || payload_len != hidden_range_bytes || !checksum_out) {
        return -1;
    }
    if (mem_service_obmm_service_v0_wait_runtime_range_input_view(local_node,
                                                            cluster_node_count,
                                                            decode_step,
                                                            &view) != 0 ||
        !view.data ||
        view.len != payload_len) {
        return -1;
    }
    memcpy(payload_out, view.data, payload_len);
    *checksum_out = view.checksum;
    return 0;
}

int mem_service_obmm_service_v0_publish_runtime_range_output(struct mem_service *svc,
                                                       uint32_t local_node,
                                                       uint32_t cluster_node_count,
                                                       uint64_t decode_step,
                                                       const uint8_t *payload,
                                                       uint64_t payload_len,
                                                       uint64_t expected_checksum,
                                                       const uint8_t *kv_payload,
                                                       uint64_t kv_payload_len,
                                                       uint64_t expected_kv_checksum)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record local_hidden_output;
    struct mem_service_record local_kv_state;
    struct mem_service_qwen3_layer_range_placement local_placement;
    uint32_t target_node;
    bool terminal_range;
    char local_hidden_output_key[96];
    char local_kv_state_key[96];
    uint64_t checksum;
    uint64_t kv_checksum = 0;
    uint64_t kv_state_offset = 0;
    uint64_t kv_state_block_bytes = 0;
    uint64_t kv_state_block_count = 0;
    uint64_t kv_state_reserved_bytes = 0;
    uint64_t runtime_output_offset = 0;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    uint8_t *base;
    uint64_t hidden_range_bytes = mem_service_qwen3_handoff_hidden_bytes(decode_step);
    struct lingqu_obmm_object_ref_wire hidden_ref;
    struct lingqu_obmm_object_ref_wire kv_ref;

    if (!svc || !payload || payload_len != hidden_range_bytes ||
        !kv_payload || kv_payload_len == 0 ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    checksum = mem_service_qwen3_hidden_payload_checksum(payload, payload_len);
    if (checksum != expected_checksum) {
        printf("[mem_service] gap qwen3_range_forward=runtime_output_checksum_mismatch local=node%u checksum=0x%016" PRIx64 " expected=0x%016" PRIx64 "\n",
               local_node + 1U,
               checksum,
               expected_checksum);
        return -1;
    }
    kv_checksum = mem_service_qwen3_hidden_payload_checksum(kv_payload, kv_payload_len);
    if (kv_checksum != expected_kv_checksum) {
        printf("[mem_service] gap qwen3_range_forward=runtime_kv_checksum_mismatch local=node%u checksum=0x%016" PRIx64 " expected=0x%016" PRIx64 " bytes=%" PRIu64 "\n",
               local_node + 1U,
               kv_checksum,
               expected_kv_checksum,
               kv_payload_len);
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !mem_service_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    terminal_range = local_placement.layer_end >= mem_service_qwen3_layer_count();
    target_node = terminal_range ? local_node : local_placement.next_owner_node;
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        mem_service_payload_arena_alloc(rt,
                                  payload_len,
                                  64,
                                  &runtime_output_offset) != 0) {
        return -1;
    }
    if (mem_service_qwen3_kv_state_alloc(rt,
                                   kv_payload_len,
                                   &kv_state_offset,
                                   &kv_state_block_bytes,
                                   &kv_state_block_count,
                                   &kv_state_reserved_bytes) != 0) {
        printf("[mem_service] gap qwen3_range_forward=runtime_kv_block_span_alloc_failed local=node%u step=%" PRIu64 " bytes=%" PRIu64 " block_bytes=%" PRIu64 " blocks=%" PRIu64 " reserved_bytes=%" PRIu64 " region_len=%zu\n",
               local_node + 1U,
               decode_step,
               kv_payload_len,
               kv_state_block_bytes,
               kv_state_block_count,
               kv_state_reserved_bytes,
               local_slot->region.len);
        return -1;
    }
    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + runtime_output_offset, payload, payload_len);
    memcpy(base + kv_state_offset, kv_payload, kv_payload_len);
    if (mem_service_update_region_range_at(local_slot,
                                     runtime_output_offset,
                                     payload_len,
                                     true) != 0 ||
        mem_service_update_region_range_at(local_slot,
                                     kv_state_offset,
                                     kv_payload_len,
                                     true) != 0) {
        return -1;
    }
    (void)msync(base + runtime_output_offset,
                payload_len,
                MS_SYNC);
    (void)msync(base + kv_state_offset, kv_payload_len, MS_SYNC);
    snprintf(local_hidden_output_key,
             sizeof(local_hidden_output_key),
             terminal_range ?
                 "hidden/%s/node%u/range-runtime-output/decode-step%" PRIu64 :
                 "hidden/%s/node%u/range-runtime-input/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             target_node + 1U,
             decode_step);
    snprintf(local_kv_state_key,
             sizeof(local_kv_state_key),
             "kvcache/%s/node%u/layers-%u-%u/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             local_node + 1U,
             local_placement.layer_start,
             local_placement.layer_end,
             decode_step);
    producer_publish_monotonic_ms = obmm_now_ms();
    producer_publish_ms = mem_service_wallclock_ms();
    producer_clock_offset_ms = producer_publish_ms - producer_publish_monotonic_ms;
    if (mem_service_put_obmm_object_record(svc,
                                     terminal_range ?
                                         MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT :
                                         MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT,
                                     local_hidden_output_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                                     runtime_output_offset,
                                     payload_len,
                                     checksum,
                                     &local_hidden_output) != 0 ||
        mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_KVCACHE_OBJECT,
                                     local_kv_state_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE,
                                     kv_state_offset,
                                     kv_payload_len,
                                     kv_checksum,
                                     &local_kv_state) != 0) {
        return -1;
    }
    {
        struct mem_service_record *published_record =
            mem_service_find_record(svc, local_hidden_output_key);

        if (!published_record) {
            return -1;
        }
        published_record->last_result_segment = (uint64_t)producer_publish_ms;
        published_record->object_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        published_record->object_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        published_record->object_publish_supernode_offset_ms =
            producer_clock_offset_ms;
        local_hidden_output.last_result_segment = (uint64_t)producer_publish_ms;
        local_hidden_output.object_publish_monotonic_ms =
            published_record->object_publish_monotonic_ms;
        local_hidden_output.object_publish_supernode_ms =
            published_record->object_publish_supernode_ms;
        local_hidden_output.object_publish_supernode_offset_ms =
            published_record->object_publish_supernode_offset_ms;
    }
    {
        struct mem_service_record *published_record =
            mem_service_find_record(svc, local_kv_state_key);

        if (!published_record) {
            return -1;
        }
        published_record->last_result_segment = (uint64_t)producer_publish_ms;
        published_record->object_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        published_record->object_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        published_record->object_publish_supernode_offset_ms =
            producer_clock_offset_ms;
        local_kv_state.last_result_segment = (uint64_t)producer_publish_ms;
        local_kv_state.object_publish_monotonic_ms =
            published_record->object_publish_monotonic_ms;
        local_kv_state.object_publish_supernode_ms =
            published_record->object_publish_supernode_ms;
        local_kv_state.object_publish_supernode_offset_ms =
            published_record->object_publish_supernode_offset_ms;
    }
    if (mem_service_write_cluster_payload(svc, local_slot) != 0) {
        return -1;
    }
    if (mem_service_record_to_lingqu_obmm_ref(&local_hidden_output, &hidden_ref) != 0 ||
        mem_service_record_to_lingqu_obmm_ref(&local_kv_state, &kv_ref) != 0) {
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    object_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (object_epoch == 0) {
        object_epoch = 1;
    }
    char boundary_observation_id[384];
    const char *service_run_id = mem_service_run_id_from_env();

    boundary_observation_id[0] = '\0';
    if (service_run_id) {
        snprintf(boundary_observation_id,
                 sizeof(boundary_observation_id),
                 "boundary-observation/%s/step%" PRIu64 "/node%u",
                 service_run_id,
                 decode_step,
                 local_node + 1U);
    }
    if (!terminal_range &&
        mem_service_push_obmm_object_desc_to(rt,
                                       target_node,
                                       MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                                       local_hidden_output.object_backing_offset,
                                       local_hidden_output.object_backing_len,
                                       local_hidden_output.object_payload_checksum,
                                       object_epoch) != 0) {
        return -1;
    }
    if (!terminal_range && boundary_observation_id[0] != '\0') {
        printf("[mem_service] stage qwen3_range_forward_runtime_ingress_publish local=node%u target=node%u observation_id=%s step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
               local_node + 1U,
               target_node + 1U,
               boundary_observation_id,
               decode_step,
               local_hidden_output_key,
               hidden_ref.key_hash,
               hidden_ref.object_version,
               local_placement.layer_start,
               local_placement.layer_end,
               local_placement.layer_count,
               checksum,
               payload_len,
               producer_publish_ms,
               producer_publish_monotonic_ms,
               producer_clock_offset_ms,
               object_epoch,
               local_publish_seq);
    } else if (!terminal_range) {
        printf("[mem_service] stage qwen3_range_forward_runtime_ingress_publish local=node%u target=node%u step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
               local_node + 1U,
               target_node + 1U,
               decode_step,
               local_hidden_output_key,
               hidden_ref.key_hash,
               hidden_ref.object_version,
               local_placement.layer_start,
               local_placement.layer_end,
               local_placement.layer_count,
               checksum,
               payload_len,
               producer_publish_ms,
               producer_publish_monotonic_ms,
               producer_clock_offset_ms,
               object_epoch,
               local_publish_seq);
    }
    printf("[mem_service] stage qwen3_range_forward_runtime_output_publish local=node%u step=%" PRIu64 " key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u output_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
           local_node + 1U,
           decode_step,
           hidden_ref.key_hash,
           hidden_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           checksum,
           payload_len,
           producer_publish_ms,
           producer_publish_monotonic_ms,
           producer_clock_offset_ms,
           object_epoch,
           local_publish_seq);
    printf("[mem_service] stage qwen3_range_kv_state_publish local=node%u step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u kv_bytes=%" PRIu64 " kv_checksum=0x%016" PRIx64 " offset=0x%016" PRIx64 " slot_bytes=%" PRIu64 " block_bytes=%" PRIu64 " blocks=%" PRIu64 " reserved_bytes=%" PRIu64 " producer_publish_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service status=ok\n",
           local_node + 1U,
           decode_step,
           local_kv_state_key,
           kv_ref.key_hash,
           kv_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           kv_payload_len,
           kv_checksum,
           kv_state_offset,
           (uint64_t)MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES,
           kv_state_block_bytes,
           kv_state_block_count,
           kv_state_reserved_bytes,
           producer_publish_ms,
           object_epoch,
           local_publish_seq);
    return 0;
}

int mem_service_obmm_service_v0_publish_runtime_range_kv_state(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    const uint8_t *kv_payload,
    uint64_t kv_payload_len,
    uint64_t expected_kv_checksum)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record local_kv_state;
    struct mem_service_qwen3_layer_range_placement local_placement;
    char local_kv_state_key[96];
    uint64_t kv_checksum;
    uint64_t kv_state_offset = 0;
    uint64_t kv_state_block_bytes = 0;
    uint64_t kv_state_block_count = 0;
    uint64_t kv_state_reserved_bytes = 0;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    uint8_t *base;
    struct lingqu_obmm_object_ref_wire kv_ref;

    if (!svc || !kv_payload || kv_payload_len == 0 ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    kv_checksum = mem_service_qwen3_hidden_payload_checksum(kv_payload, kv_payload_len);
    if (kv_checksum != expected_kv_checksum) {
        printf("[mem_service] gap qwen3_range_forward=runtime_kv_checksum_mismatch local=node%u checksum=0x%016" PRIx64 " expected=0x%016" PRIx64 " bytes=%" PRIu64 "\n",
               local_node + 1U,
               kv_checksum,
               expected_kv_checksum,
               kv_payload_len);
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !mem_service_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        mem_service_qwen3_kv_state_alloc(rt,
                                   kv_payload_len,
                                   &kv_state_offset,
                                   &kv_state_block_bytes,
                                   &kv_state_block_count,
                                   &kv_state_reserved_bytes) != 0) {
        return -1;
    }
    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + kv_state_offset, kv_payload, kv_payload_len);
    if (mem_service_update_region_range_at(local_slot,
                                     kv_state_offset,
                                     kv_payload_len,
                                     true) != 0) {
        return -1;
    }
    (void)msync(base + kv_state_offset, kv_payload_len, MS_SYNC);
    snprintf(local_kv_state_key,
             sizeof(local_kv_state_key),
             "kvcache/%s/node%u/layers-%u-%u/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             local_node + 1U,
             local_placement.layer_start,
             local_placement.layer_end,
             decode_step);
    producer_publish_monotonic_ms = obmm_now_ms();
    producer_publish_ms = mem_service_wallclock_ms();
    producer_clock_offset_ms = producer_publish_ms - producer_publish_monotonic_ms;
    if (mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_KVCACHE_OBJECT,
                                     local_kv_state_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE,
                                     kv_state_offset,
                                     kv_payload_len,
                                     kv_checksum,
                                     &local_kv_state) != 0) {
        return -1;
    }
    {
        struct mem_service_record *published_record =
            mem_service_find_record(svc, local_kv_state_key);

        if (!published_record) {
            return -1;
        }
        published_record->last_result_segment = (uint64_t)producer_publish_ms;
        published_record->object_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        published_record->object_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        published_record->object_publish_supernode_offset_ms =
            producer_clock_offset_ms;
        local_kv_state.last_result_segment = (uint64_t)producer_publish_ms;
        local_kv_state.object_publish_monotonic_ms =
            published_record->object_publish_monotonic_ms;
        local_kv_state.object_publish_supernode_ms =
            published_record->object_publish_supernode_ms;
        local_kv_state.object_publish_supernode_offset_ms =
            published_record->object_publish_supernode_offset_ms;
    }
    if (mem_service_write_cluster_payload(svc, local_slot) != 0 ||
        mem_service_record_to_lingqu_obmm_ref(&local_kv_state, &kv_ref) != 0) {
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    object_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (object_epoch == 0) {
        object_epoch = 1;
    }
    printf("[mem_service] stage qwen3_range_kv_state_publish local=node%u step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u kv_bytes=%" PRIu64 " kv_checksum=0x%016" PRIx64 " offset=0x%016" PRIx64 " slot_bytes=%" PRIu64 " block_bytes=%" PRIu64 " blocks=%" PRIu64 " reserved_bytes=%" PRIu64 " producer_publish_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service status=ok\n",
           local_node + 1U,
           decode_step,
           local_kv_state_key,
           kv_ref.key_hash,
           kv_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           kv_payload_len,
           kv_checksum,
           kv_state_offset,
           (uint64_t)MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES,
           kv_state_block_bytes,
           kv_state_block_count,
           kv_state_reserved_bytes,
           producer_publish_ms,
           object_epoch,
           local_publish_seq);
    return 0;
}

int mem_service_obmm_service_v0_try_resolve_range_kv_state_view(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t kv_step,
    struct mem_service_object_payload_view *view_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_qwen3_layer_range_placement local_placement;
    struct mem_service_record kv_state;
    char kv_state_key[96];
    uint64_t checksum;

    if (!view_out) {
        return -1;
    }
    memset(view_out, 0, sizeof(*view_out));
    if (!svc ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !mem_service_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr) {
        return -1;
    }
    snprintf(kv_state_key,
             sizeof(kv_state_key),
             "kvcache/%s/node%u/layers-%u-%u/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             local_node + 1U,
             local_placement.layer_start,
             local_placement.layer_end,
             kv_step);
    memset(&kv_state, 0, sizeof(kv_state));
    {
        struct mem_service_record *local_record = mem_service_find_record(svc, kv_state_key);

        if (local_record) {
            kv_state = *local_record;
        }
    }
    if (kv_state.kind != MEM_SERVICE_RECORD_KVCACHE_OBJECT ||
        kv_state.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE ||
        kv_state.object_backing_len == 0 ||
        kv_state.object_backing_offset + kv_state.object_backing_len >
            local_slot->region.len) {
        printf("[mem_service] stage qwen3_range_kv_state_resolve_missing local=node%u kv_step=%" PRIu64 " key=%s status=miss\n",
               local_node + 1U,
               kv_step,
               kv_state_key);
        return 1;
    }
    checksum = kv_state.object_payload_checksum;
    view_out->data = (const uint8_t *)local_slot->region.addr +
                     kv_state.object_backing_offset;
    view_out->len = kv_state.object_backing_len;
    view_out->checksum = checksum;
    view_out->owner_node = local_node;
    view_out->payload_kind = kv_state.object_payload_kind;
    view_out->backing_offset = kv_state.object_backing_offset;
    if (mem_service_record_to_lingqu_obmm_ref(&kv_state, &view_out->object_ref) != 0) {
        return -1;
    }
    printf("[mem_service] stage qwen3_range_kv_state_resolve local=node%u kv_step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u kv_bytes=%" PRIu64 " kv_checksum=0x%016" PRIx64 " offset=0x%016" PRIx64 " validation=object_ref_metadata source=obmm_object_view backing=obmm_shmem metadata=lingqu_object_service target=mapped_view status=ok\n",
           local_node + 1U,
           kv_step,
           kv_state_key,
           view_out->object_ref.key_hash,
           view_out->object_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           kv_state.object_backing_len,
           checksum,
           kv_state.object_backing_offset);
    return 0;
}

int mem_service_obmm_service_v0_resolve_previous_range_kv_state_view(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_object_payload_view *view_out)
{
    int rc;

    if (!view_out) {
        return -1;
    }
    memset(view_out, 0, sizeof(*view_out));
    if (decode_step == 0) {
        return 0;
    }
    rc = mem_service_obmm_service_v0_try_resolve_range_kv_state_view(
        svc,
        local_node,
        cluster_node_count,
        decode_step - 1U,
        view_out);
    if (rc > 0) {
        printf("[mem_service] gap qwen3_range_kv_state_resolve=missing local=node%u step=%" PRIu64 " previous_step=%" PRIu64 "\n",
               local_node + 1U,
               decode_step,
               decode_step - 1U);
        return -1;
    }
    return rc;
}

int mem_service_obmm_service_v0_resolve_previous_range_kv_state(struct mem_service *svc,
                                                          uint32_t local_node,
                                                          uint32_t cluster_node_count,
                                                          uint64_t decode_step,
                                                          uint8_t *payload_out,
                                                          uint64_t payload_capacity,
                                                          uint64_t *payload_len_out,
                                                          uint64_t *checksum_out)
{
    struct mem_service_object_payload_view view;

    if (!payload_len_out || !checksum_out) {
        return -1;
    }
    *payload_len_out = 0;
    *checksum_out = 0;
    if (!payload_out) {
        return -1;
    }
    if (mem_service_obmm_service_v0_resolve_previous_range_kv_state_view(
            svc,
            local_node,
            cluster_node_count,
            decode_step,
            &view) != 0) {
        return -1;
    }
    if (!view.data || view.len == 0) {
        return 0;
    }
    if (view.len > payload_capacity) {
        printf("[mem_service] gap qwen3_range_kv_state_resolve=payload_too_large local=node%u step=%" PRIu64 " bytes=%" PRIu64 " capacity=%" PRIu64 "\n",
               local_node + 1U,
               decode_step,
               view.len,
               payload_capacity);
        return -1;
    }
    memcpy(payload_out, view.data, (size_t)view.len);
    *payload_len_out = view.len;
    *checksum_out = view.checksum;
    return 0;
}

static int mem_service_obmm_service_v0_publish_terminal_token_result_from_node(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    uint64_t sampled_token,
    uint64_t runner_up_token,
    uint64_t margin_milli,
    uint64_t logits_checksum,
    uint64_t text_checksum,
    uint64_t piece_word0,
    uint64_t piece_word1,
    bool require_terminal_node)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record local_token_result;
    struct mem_service_qwen3_layer_range_placement local_placement;
    char token_result_key[96];
    uint64_t payload_words[8];
    uint64_t token_result_offset;
    uint64_t checksum;
    uint32_t target_node;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    uint8_t *base;

    if (!svc || cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !mem_service_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    if (require_terminal_node && !local_placement.terminal) {
        return 0;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        mem_service_payload_arena_alloc(rt,
                                  MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                                  64,
                                  &token_result_offset) != 0) {
        return -1;
    }

    payload_words[0] = decode_step;
    payload_words[1] = sampled_token;
    payload_words[2] = runner_up_token;
    payload_words[3] = margin_milli;
    payload_words[4] = logits_checksum;
    payload_words[5] = text_checksum;
    payload_words[6] = piece_word0;
    payload_words[7] = piece_word1;
    checksum = mem_service_qwen3_hidden_payload_checksum(
        (const uint8_t *)payload_words,
        MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + token_result_offset, payload_words, MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
    if (mem_service_update_region_range_at(local_slot,
                                     token_result_offset,
                                     MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                                     true) != 0) {
        return -1;
    }
    (void)msync(base + token_result_offset, MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES, MS_SYNC);

    snprintf(token_result_key,
             sizeof(token_result_key),
             "tokens/%s/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             decode_step);
    producer_publish_monotonic_ms = obmm_now_ms();
    producer_publish_ms = mem_service_wallclock_ms();
    producer_clock_offset_ms = producer_publish_ms - producer_publish_monotonic_ms;
    if (mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT,
                                     token_result_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                                     token_result_offset,
                                     MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                                     checksum,
                                     &local_token_result) != 0) {
        return -1;
    }
    {
        struct mem_service_record *published_record =
            mem_service_find_record(svc, token_result_key);

        if (!published_record) {
            return -1;
        }
        published_record->last_result_segment = (uint64_t)producer_publish_ms;
        published_record->object_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        published_record->object_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        published_record->object_publish_supernode_offset_ms =
            producer_clock_offset_ms;
        local_token_result.last_result_segment = (uint64_t)producer_publish_ms;
        local_token_result.object_publish_monotonic_ms =
            published_record->object_publish_monotonic_ms;
        local_token_result.object_publish_supernode_ms =
            published_record->object_publish_supernode_ms;
        local_token_result.object_publish_supernode_offset_ms =
            published_record->object_publish_supernode_offset_ms;
    }
    if (mem_service_write_cluster_payload(svc, local_slot) != 0) {
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    if (mem_service_qwen3_decode_entry_node(cluster_node_count, &target_node) != 0) {
        return -1;
    }
    object_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (object_epoch == 0) {
        object_epoch = 1;
    }
    if (!require_terminal_node) {
        uint32_t node_idx;

        for (node_idx = 0; node_idx < cluster_node_count; ++node_idx) {
            if (node_idx == (uint32_t)rt->local_idx) {
                struct obmm_desc desc;

                if (rt->pending_desc_count[rt->local_idx] >=
                    MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH) {
                    return -1;
                }
                memset(&desc, 0, sizeof(desc));
                desc.type = OBMM_DESC_MEM_SERVICE_OBJECT_PUT;
                desc.flags = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT;
                desc.seq = ((uint64_t)object_epoch << 48) |
                           ((uint64_t)(rt->local_idx + 1) << 32) |
                           (local_token_result.object_backing_offset &
                            0xffffffffULL);
                desc.region_id = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT;
                desc.payload_len =
                    (uint32_t)local_token_result.object_backing_len;
                desc.payload_offset = local_token_result.object_backing_offset;
                desc.cookie =
                    (uint32_t)(local_token_result.object_payload_checksum ^
                               (local_token_result.object_payload_checksum >>
                                32));
                mem_service_stash_pending_desc(rt, rt->local_idx, &desc);
            } else if (mem_service_push_obmm_object_desc_to(
                           rt,
                           node_idx,
                           MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                           local_token_result.object_backing_offset,
                           local_token_result.object_backing_len,
                           local_token_result.object_payload_checksum,
                           object_epoch) != 0) {
                return -1;
            }
        }
    } else if (target_node == (uint32_t)rt->local_idx) {
        struct obmm_desc desc;

        if (rt->pending_desc_count[rt->local_idx] >=
            MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH) {
            return -1;
        }
        memset(&desc, 0, sizeof(desc));
        desc.type = OBMM_DESC_MEM_SERVICE_OBJECT_PUT;
        desc.flags = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT;
        desc.seq = ((uint64_t)object_epoch << 48) |
                   ((uint64_t)(rt->local_idx + 1) << 32) |
                   (local_token_result.object_backing_offset & 0xffffffffULL);
        desc.region_id = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT;
        desc.payload_len = (uint32_t)local_token_result.object_backing_len;
        desc.payload_offset = local_token_result.object_backing_offset;
        desc.cookie =
            (uint32_t)(local_token_result.object_payload_checksum ^
                       (local_token_result.object_payload_checksum >> 32));
        mem_service_stash_pending_desc(rt, rt->local_idx, &desc);
    } else if (mem_service_push_obmm_object_desc_to(
                   rt,
                   target_node,
                   MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                   local_token_result.object_backing_offset,
                   local_token_result.object_backing_len,
                   local_token_result.object_payload_checksum,
                   object_epoch) != 0) {
        return -1;
    }
    printf("[mem_service] stage qwen3_terminal_token_result_publish local=node%u target=node%u step=%" PRIu64 " token=%" PRIu64 " runner_up=%" PRIu64 " margin_milli=%" PRIu64 " logits_checksum=0x%016" PRIx64 " text_checksum=0x%016" PRIx64 " piece_word0=0x%016" PRIx64 " piece_word1=0x%016" PRIx64 " object_key=%s offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " epoch=%u seq=%u backing=obmm_pool metadata=db queue=%s status=ok publisher=%s broadcast_targets=%u\n",
           local_node + 1U,
           target_node + 1U,
           decode_step,
           sampled_token,
           runner_up_token,
           margin_milli,
           logits_checksum,
           text_checksum,
           piece_word0,
           piece_word1,
           token_result_key,
           local_token_result.object_backing_offset,
           local_token_result.object_backing_len,
           local_token_result.object_payload_checksum,
           object_epoch,
           local_publish_seq,
           target_node == (uint32_t)rt->local_idx ? "local_pending" : "obmm_spsc",
           require_terminal_node ? "terminal_node" : "shortpath_boundary",
           require_terminal_node ? 1U : cluster_node_count);
    return 0;
}

int mem_service_obmm_service_v0_publish_terminal_token_result(struct mem_service *svc,
                                                        uint32_t local_node,
                                                        uint32_t cluster_node_count,
                                                        uint64_t decode_step,
                                                        uint64_t sampled_token,
                                                        uint64_t runner_up_token,
                                                        uint64_t margin_milli,
                                                        uint64_t logits_checksum,
                                                        uint64_t text_checksum,
                                                        uint64_t piece_word0,
                                                        uint64_t piece_word1)
{
    return mem_service_obmm_service_v0_publish_terminal_token_result_from_node(
        svc,
        local_node,
        cluster_node_count,
        decode_step,
        sampled_token,
        runner_up_token,
        margin_milli,
        logits_checksum,
        text_checksum,
        piece_word0,
        piece_word1,
        true);
}

int mem_service_obmm_service_v0_publish_shortpath_terminal_token_result(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    uint64_t sampled_token,
    uint64_t runner_up_token,
    uint64_t margin_milli,
    uint64_t logits_checksum,
    uint64_t text_checksum,
    uint64_t piece_word0,
    uint64_t piece_word1)
{
    return mem_service_obmm_service_v0_publish_terminal_token_result_from_node(
        svc,
        local_node,
        cluster_node_count,
        decode_step,
        sampled_token,
        runner_up_token,
        margin_milli,
        logits_checksum,
        text_checksum,
        piece_word0,
        piece_word1,
        false);
}

static uint64_t mem_service_pack_qwen3_engram_candidates(uint64_t decode_step,
                                                   const uint64_t *candidate_tokens,
                                                   const uint64_t *candidate_logit_bits,
                                                   const uint64_t *candidate_text_checksums,
                                                   const uint64_t *candidate_piece_bytes,
                                                   const uint64_t *candidate_piece_word0,
                                                   const uint64_t *candidate_piece_word1,
                                                   uint64_t candidate_count,
                                                   uint64_t *candidate_words,
                                                   size_t candidate_word_count)
{
    uint64_t packed_count;

    if (!candidate_words || candidate_word_count < 32U || !candidate_tokens ||
        candidate_count == 0) {
        return 0;
    }
    packed_count = candidate_count > 4U ? 4U : candidate_count;
    memset(candidate_words, 0, candidate_word_count * sizeof(uint64_t));
    candidate_words[0] = decode_step;
    candidate_words[1] = packed_count;
    for (uint64_t i = 0; i < packed_count; ++i) {
        uint64_t base = 2U + i * 7U;

        candidate_words[base] = i;
        candidate_words[base + 1U] = candidate_tokens[i];
        candidate_words[base + 2U] = candidate_logit_bits ? candidate_logit_bits[i] : 0U;
        candidate_words[base + 3U] =
            candidate_text_checksums ? candidate_text_checksums[i] : 0U;
        candidate_words[base + 4U] = candidate_piece_bytes ? candidate_piece_bytes[i] : 0U;
        candidate_words[base + 5U] = candidate_piece_word0 ? candidate_piece_word0[i] : 0U;
        candidate_words[base + 6U] = candidate_piece_word1 ? candidate_piece_word1[i] : 0U;
    }
    candidate_words[31] = mem_service_checksum_bytes((const uint8_t *)candidate_words,
                                               31U * sizeof(uint64_t));
    return packed_count;
}

int mem_service_obmm_service_v0_publish_engram_candidates(struct mem_service *svc,
                                                    uint32_t local_node,
                                                    uint32_t cluster_node_count,
                                                    uint64_t decode_step,
                                                    const uint64_t *candidate_tokens,
                                                    const uint64_t *candidate_logit_bits,
                                                    const uint64_t *candidate_text_checksums,
                                                    const uint64_t *candidate_piece_bytes,
                                                    const uint64_t *candidate_piece_word0,
                                                    const uint64_t *candidate_piece_word1,
                                                    uint64_t candidate_count)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record candidates_record;
    char candidates_key[96];
    uint64_t candidate_words[32];
    uint64_t candidates_offset;
    uint64_t candidates_bytes = MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES;
    uint64_t candidates_checksum;
    uint64_t packed_count;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    uint8_t *base;

    if (!svc || cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count || !candidate_tokens || candidate_count == 0) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0) {
        return -1;
    }

    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        mem_service_payload_arena_alloc(rt,
                                  candidates_bytes,
                                  64,
                                  &candidates_offset) != 0) {
        return -1;
    }
    packed_count = mem_service_pack_qwen3_engram_candidates(decode_step,
                                                      candidate_tokens,
                                                      candidate_logit_bits,
                                                      candidate_text_checksums,
                                                      candidate_piece_bytes,
                                                      candidate_piece_word0,
                                                      candidate_piece_word1,
                                                      candidate_count,
                                                      candidate_words,
                                                      32U);
    if (packed_count == 0) {
        return -1;
    }
    candidates_checksum =
        mem_service_checksum_bytes((const uint8_t *)candidate_words, candidates_bytes);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + candidates_offset, candidate_words, candidates_bytes);
    if (mem_service_update_region_range_at(local_slot, candidates_offset, candidates_bytes, true) !=
        0) {
        return -1;
    }
    (void)msync(base + candidates_offset, candidates_bytes, MS_SYNC);

    mem_service_qwen3_engram_candidates_key(decode_step,
                                      candidates_key,
                                      sizeof(candidates_key));
    if (mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES,
                                     candidates_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                                     candidates_offset,
                                     candidates_bytes,
                                     candidates_checksum,
                                     &candidates_record) != 0 ||
        mem_service_write_cluster_payload(svc, local_slot) != 0) {
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    object_epoch = rt->observe_epoch;
    if (mem_service_push_obmm_object_descs(rt,
                                     candidates_record.object_payload_kind,
                                     candidates_record.object_backing_offset,
                                     candidates_record.object_backing_len,
                                     candidates_record.object_payload_checksum,
                                     object_epoch) != 0) {
        return -1;
    }

    printf("[mem_service] stage qwen3_engram_candidates_publish local=node%u step=%" PRIu64
           " candidate_count=%" PRIu64
           " candidates_key=%s candidates_version=%" PRIu64
           " candidates_checksum=0x%016" PRIx64
           " epoch=%u seq=%u backing=obmm_pool metadata=db queue=obmm_spsc status=ok\n",
           local_node + 1U,
           decode_step,
           packed_count,
           candidates_key,
           candidates_record.version,
           candidates_checksum,
           object_epoch,
           local_publish_seq);
    return 0;
}

int mem_service_obmm_service_v0_publish_engram_step(struct mem_service *svc,
                                              uint32_t local_node,
                                              uint32_t cluster_node_count,
                                              uint64_t decode_step,
                                              const uint64_t *history_tokens,
                                              uint64_t history_token_count,
                                              uint64_t raw_sampled_token,
                                              uint64_t runner_up_token,
                                              uint64_t selected_token,
                                              uint64_t blocked_count,
                                              uint64_t fallback_used,
                                              int64_t top_score_milli,
                                              int64_t runner_up_score_milli,
                                              uint64_t no_repeat_ngram_size,
                                              uint64_t repetition_penalty_milli,
                                              uint64_t history_window,
                                              uint64_t logits_checksum,
                                              uint64_t text_checksum)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record records[3];
    char history_key[96];
    char selected_key[96];
    char state_key[96];
    uint64_t history_words[1024 + 2];
    uint64_t selected_words[8];
    uint64_t state_words[16];
    uint64_t history_offset;
    uint64_t selected_offset;
    uint64_t state_offset;
    uint64_t published_history_token_count;
    uint64_t history_bytes;
    uint64_t selected_bytes = MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES;
    uint64_t state_bytes = MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES;
    uint64_t history_checksum;
    uint64_t selected_checksum;
    uint64_t state_checksum;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    int owner_idx;
    uint8_t *base;

    if (!svc || cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count || !history_tokens ||
        history_token_count > 1024U) {
        return -1;
    }
    if (history_token_count == UINT64_MAX || history_token_count + 1U > 1024U) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0) {
        return -1;
    }
    owner_idx = mem_service_qwen3_engram_owner_index(cluster_node_count);
    if (owner_idx < 0 || (uint32_t)owner_idx != local_node) {
        return 0;
    }

    local_slot = &rt->slots[rt->local_idx];
    published_history_token_count = history_token_count + 1U;
    history_bytes = (published_history_token_count + 2U) * sizeof(uint64_t);
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        history_bytes > MEM_SERVICE_OBMM_QWEN3_ENGRAM_HISTORY_BYTES ||
        mem_service_payload_arena_alloc(rt, history_bytes, 64, &history_offset) != 0 ||
        mem_service_payload_arena_alloc(rt, selected_bytes, 64, &selected_offset) != 0 ||
        mem_service_payload_arena_alloc(rt, state_bytes, 64, &state_offset) != 0) {
        return -1;
    }

    memset(history_words, 0, sizeof(history_words));
    history_words[0] = decode_step + 1U;
    history_words[1] = published_history_token_count;
    memcpy(&history_words[2], history_tokens, history_token_count * sizeof(uint64_t));
    history_words[2 + history_token_count] = selected_token;

    memset(selected_words, 0, sizeof(selected_words));
    selected_words[0] = decode_step;
    selected_words[1] = selected_token;
    selected_words[2] = raw_sampled_token;
    selected_words[3] = runner_up_token;
    selected_words[4] = fallback_used;
    selected_words[5] = blocked_count;
    selected_words[6] = logits_checksum;

    history_checksum = mem_service_checksum_bytes((const uint8_t *)history_words, history_bytes);
    selected_checksum = mem_service_checksum_bytes((const uint8_t *)selected_words, selected_bytes);

    memset(state_words, 0, sizeof(state_words));
    state_words[0] = decode_step;
    state_words[1] = published_history_token_count;
    state_words[2] = selected_token;
    state_words[3] = history_checksum;
    state_words[4] = no_repeat_ngram_size;
    state_words[5] = repetition_penalty_milli;
    state_words[6] = blocked_count;
    state_words[7] = fallback_used;
    state_words[8] = raw_sampled_token;
    state_words[9] = runner_up_token;
    state_words[10] = (uint64_t)top_score_milli;
    state_words[11] = (uint64_t)runner_up_score_milli;
    state_words[12] = history_window;
    state_words[13] = logits_checksum;
    state_words[14] = text_checksum;
    state_words[15] = mem_service_checksum_bytes((const uint8_t *)state_words,
                                           15U * sizeof(uint64_t));
    state_checksum = mem_service_checksum_bytes((const uint8_t *)state_words, state_bytes);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + history_offset, history_words, history_bytes);
    memcpy(base + selected_offset, selected_words, selected_bytes);
    memcpy(base + state_offset, state_words, state_bytes);
    if (mem_service_update_region_range_at(local_slot, history_offset, history_bytes, true) != 0 ||
        mem_service_update_region_range_at(local_slot, selected_offset, selected_bytes, true) != 0 ||
        mem_service_update_region_range_at(local_slot, state_offset, state_bytes, true) != 0) {
        return -1;
    }
    (void)msync(base + history_offset, history_bytes, MS_SYNC);
    (void)msync(base + selected_offset, selected_bytes, MS_SYNC);
    (void)msync(base + state_offset, state_bytes, MS_SYNC);

    mem_service_qwen3_engram_history_key(history_key, sizeof(history_key));
    mem_service_qwen3_engram_selected_key(decode_step, selected_key, sizeof(selected_key));
    mem_service_qwen3_engram_state_key(decode_step, state_key, sizeof(state_key));

    if (mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY,
                                     history_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                                     history_offset,
                                     history_bytes,
                                     history_checksum,
                                     &records[0]) != 0 ||
        mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED,
                                     selected_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                                     selected_offset,
                                     selected_bytes,
                                     selected_checksum,
                                     &records[1]) != 0 ||
        mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE,
                                     state_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                                     state_offset,
                                     state_bytes,
                                     state_checksum,
                                     &records[2]) != 0 ||
        mem_service_write_cluster_payload(svc, local_slot) != 0) {
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    object_epoch = rt->observe_epoch;
    for (size_t i = 0; i < 3U; ++i) {
        if (mem_service_push_obmm_object_descs(rt,
                                         records[i].object_payload_kind,
                                         records[i].object_backing_offset,
                                         records[i].object_backing_len,
                                         records[i].object_payload_checksum,
                                         object_epoch) != 0) {
            return -1;
        }
    }

    printf("[mem_service] stage qwen3_engram_decision_publish local=node%u step=%" PRIu64
           " objects=3 history_tokens=%" PRIu64 " selected_token=%" PRIu64
           " raw_token=%" PRIu64 " runner_up=%" PRIu64
           " fallback=%" PRIu64 " blocked=%" PRIu64
           " top_score_milli=%" PRId64 " runner_up_score_milli=%" PRId64
           " history_window=%" PRIu64
           " history_key=%s history_version=%" PRIu64
           " selected_key=%s state_key=%s"
           " history_checksum=0x%016" PRIx64
           " selected_checksum=0x%016" PRIx64
           " state_checksum=0x%016" PRIx64
           " logits_checksum=0x%016" PRIx64
           " text_checksum=0x%016" PRIx64
           " epoch=%u seq=%u backing=obmm_pool metadata=db queue=obmm_spsc status=ok\n",
           local_node + 1U,
           decode_step,
           published_history_token_count,
           selected_token,
           raw_sampled_token,
           runner_up_token,
           fallback_used,
           blocked_count,
           top_score_milli,
           runner_up_score_milli,
           history_window,
           history_key,
           records[0].version,
           selected_key,
           state_key,
           history_checksum,
           selected_checksum,
           state_checksum,
           logits_checksum,
           text_checksum,
           object_epoch,
           local_publish_seq);
    return 0;
}

int mem_service_obmm_service_v0_wait_terminal_token_result(struct mem_service *svc,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms,
                                                     uint64_t *sampled_token_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    char token_result_key[96];
    long deadline;
    bool first_scan = true;

    if (!svc) {
        return -1;
    }
    snprintf(token_result_key,
             sizeof(token_result_key),
             "tokens/%s/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             decode_step);
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (first_scan || obmm_now_ms() < deadline) {
        first_scan = false;
        if (mem_service_cluster_runtime_require(rt) == 0) {
            for (int owner_idx = 0; owner_idx < rt->node_count; ++owner_idx) {
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                struct mem_service_record token_record;
                uint64_t payload_words[8];
                uint64_t checksum;
                struct mem_service_cluster_slot *owner_slot;

                if (owner_idx != rt->local_idx &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    continue;
                }
                owner_slot = &rt->slots[owner_idx];
                if (owner_slot->region.addr &&
                    mem_service_try_read_stable_compact_summary_region(owner_slot,
                                                                 &compact,
                                                                 &seen) &&
                    mem_service_slot_find_record(owner_slot,
                                           token_result_key,
                                           &token_record) &&
                    token_record.kind == MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT &&
                    token_record.object_payload_kind ==
                        MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT &&
                    token_record.object_backing_len ==
                        MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES &&
                    token_record.object_backing_offset <= owner_slot->region.len &&
                    token_record.object_backing_len <=
                        owner_slot->region.len - token_record.object_backing_offset) {
                    memcpy(payload_words,
                           (uint8_t *)owner_slot->region.addr +
                               token_record.object_backing_offset,
                           MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                    if (payload_words[0] == decode_step) {
                        checksum = mem_service_qwen3_hidden_payload_checksum(
                            (const uint8_t *)payload_words,
                            MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                        if (checksum != token_record.object_payload_checksum) {
                            usleep(10000);
                            continue;
                        }
                        if (sampled_token_out) {
                            *sampled_token_out = payload_words[1];
                        }
                        printf("[mem_service] stage qwen3_terminal_token_result_wait step=%" PRIu64
                               " object_key=%s owner=node%d offset=0x%016" PRIx64
                               " bytes=%" PRIu64
                               " token=%" PRIu64 " checksum=0x%016" PRIx64
                               " source=obmm_object_record status=ok\n",
                               decode_step,
                               token_result_key,
                               owner_idx + 1,
                               token_record.object_backing_offset,
                               (uint64_t)MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                               payload_words[1],
                               checksum);
                        return 0;
                    }
                }
            }
        }
        if (timeout_ms == 0) {
            break;
        }
        usleep(10000);
    }
    if (timeout_ms != 0) {
        printf("[mem_service] gap qwen3_terminal_token_result_wait=timeout step=%" PRIu64
               " object_key=%s\n",
               decode_step,
               token_result_key);
    }
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_candidates(struct mem_service *svc,
                                                 uint64_t decode_step,
                                                 uint64_t timeout_ms,
                                                 uint64_t *candidate_tokens_out,
                                                 uint64_t *candidate_logit_bits_out,
                                                 uint64_t *candidate_text_checksums_out,
                                                 uint64_t *candidate_piece_bytes_out,
                                                 uint64_t *candidate_piece_word0_out,
                                                 uint64_t *candidate_piece_word1_out,
                                                 uint64_t candidate_capacity,
                                                 uint64_t *candidate_count_out,
                                                 uint64_t *candidate_checksum_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    char candidates_key[96];
    long deadline;
    int candidate_owner_idx = -1;

    if (!svc || !candidate_tokens_out || !candidate_count_out ||
        candidate_capacity == 0) {
        return -1;
    }
    *candidate_count_out = 0;
    if (candidate_checksum_out) {
        *candidate_checksum_out = 0;
    }
    mem_service_qwen3_engram_candidates_key(decode_step,
                                      candidates_key,
                                      sizeof(candidates_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record candidates_record;
        struct obmm_desc candidates_desc;
        uint64_t candidate_words[32];
        uint64_t candidate_count;
        uint64_t inner_checksum;
        uint64_t candidates_checksum;
        bool candidates_record_found = false;

        candidate_owner_idx = -1;
        memset(&candidates_record, 0, sizeof(candidates_record));
        memset(&candidates_desc, 0, sizeof(candidates_desc));
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            rt->node_count > 0) {
            for (int node_idx = 0; node_idx < rt->node_count; ++node_idx) {
                terminal_slot = &rt->slots[node_idx];
                memset(&candidates_record, 0, sizeof(candidates_record));
                if (node_idx == rt->local_idx) {
                    candidates_record_found =
                        mem_service_slot_find_record(terminal_slot,
                                               candidates_key,
                                               &candidates_record);
                } else {
                    struct obmm_desc rx;
                    struct mem_service_cluster_payload_compact_summary compact;
                    struct mem_service_cluster_payload_header seen;

                    if (mem_service_take_pending_qwen3_object_kind_len_desc(
                            rt,
                            node_idx,
                            MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                            MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES,
                            MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES,
                            &candidates_desc)) {
                    } else if (rt->ingress_queues[node_idx]) {
                        while (obmm_spsc_pop(rt->ingress_queues[node_idx], &rx) == 0) {
                            if (mem_service_qwen3_object_desc_kind_len_matches(
                                    &rx,
                                    MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                                    MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES,
                                    MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES)) {
                                candidates_desc = rx;
                                break;
                            }
                            mem_service_stash_pending_desc(rt, node_idx, &rx);
                        }
                    }
                    if (candidates_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                        continue;
                    }
                    if (!terminal_slot->region.addr &&
                        mem_service_activate_remote_slot(rt, node_idx) != 0) {
                        continue;
                    }
                    memset(&compact, 0, sizeof(compact));
                    memset(&seen, 0, sizeof(seen));
                    candidates_record_found =
                        mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                      &compact,
                                                                      &seen) &&
                        mem_service_slot_find_record_by_obmm_object_backing(
                            terminal_slot,
                            MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES,
                            MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                            candidates_desc.payload_offset,
                            candidates_desc.payload_len,
                            candidates_desc.cookie,
                            &candidates_record);
                }
                if (candidates_record_found &&
                    candidates_record.kind == MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES &&
                    candidates_record.version == 1U &&
                    candidates_record.object_payload_kind ==
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES &&
                    candidates_record.object_backing_len ==
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES &&
                    terminal_slot->region.addr &&
                    candidates_record.object_backing_offset +
                            candidates_record.object_backing_len <=
                        terminal_slot->region.len) {
                    candidate_owner_idx = node_idx;
                    break;
                }
                memset(&candidates_desc, 0, sizeof(candidates_desc));
            }
            if (candidate_owner_idx < 0) {
                usleep(10000);
                continue;
            }
            terminal_slot = &rt->slots[candidate_owner_idx];
            if (candidates_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES ||
                candidates_record.version != 1U ||
                candidates_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES ||
                candidates_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES ||
                !terminal_slot->region.addr ||
                candidates_record.object_backing_offset + candidates_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memset(candidate_words, 0, sizeof(candidate_words));
            memcpy(candidate_words,
                   (uint8_t *)terminal_slot->region.addr +
                       candidates_record.object_backing_offset,
                   MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES);
            inner_checksum = mem_service_checksum_bytes((const uint8_t *)candidate_words,
                                                  31U * sizeof(uint64_t));
            candidates_checksum = mem_service_checksum_bytes((const uint8_t *)candidate_words,
                                                       MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES);
            candidate_count = candidate_words[1];
            if (candidate_words[0] == decode_step &&
                candidate_count > 0 &&
                candidate_count <= 4U &&
                candidate_count <= candidate_capacity &&
                candidate_words[31] == inner_checksum &&
                candidates_checksum == candidates_record.object_payload_checksum) {
                for (uint64_t i = 0; i < candidate_count; ++i) {
                    uint64_t base = 2U + i * 7U;

                    candidate_tokens_out[i] = candidate_words[base + 1U];
                    if (candidate_logit_bits_out) {
                        candidate_logit_bits_out[i] = candidate_words[base + 2U];
                    }
                    if (candidate_text_checksums_out) {
                        candidate_text_checksums_out[i] = candidate_words[base + 3U];
                    }
                    if (candidate_piece_bytes_out) {
                        candidate_piece_bytes_out[i] = candidate_words[base + 4U];
                    }
                    if (candidate_piece_word0_out) {
                        candidate_piece_word0_out[i] = candidate_words[base + 5U];
                    }
                    if (candidate_piece_word1_out) {
                        candidate_piece_word1_out[i] = candidate_words[base + 6U];
                    }
                }
                *candidate_count_out = candidate_count;
                if (candidate_checksum_out) {
                    *candidate_checksum_out = candidates_checksum;
                }
                printf("[mem_service] stage qwen3_engram_candidates_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " candidate_count=%" PRIu64 " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       candidates_key,
                       candidate_owner_idx + 1,
                       candidates_record.version,
                       candidate_count,
                       candidates_record.object_backing_len,
                       candidates_checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_candidates_wait=timeout step=%" PRIu64
           " object_key=%s\n",
           decode_step,
           candidates_key);
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_selected_token(struct mem_service *svc,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms,
                                                     uint64_t *selected_token_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    char selected_key[96];
    long deadline;
    int owner_idx = mem_service_qwen3_engram_owner_index(mem_service_qwen3_range_nodes());

    if (!svc) {
        return -1;
    }
    mem_service_qwen3_engram_selected_key(decode_step, selected_key, sizeof(selected_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record selected_record;
        struct obmm_desc selected_desc;
        uint64_t payload_words[8];
        uint64_t checksum;
        uint16_t expected_epoch;
        bool selected_record_found = false;

        memset(&selected_record, 0, sizeof(selected_record));
        memset(&selected_desc, 0, sizeof(selected_desc));
        expected_epoch = (uint16_t)(decode_step + 1U);
        if (expected_epoch == 0) {
            expected_epoch = 1;
        }
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count) {
            terminal_slot = &rt->slots[owner_idx];
            if (owner_idx == rt->local_idx) {
                selected_record_found =
                    mem_service_slot_find_record(terminal_slot,
                                           selected_key,
                                           &selected_record);
            } else {
                struct obmm_desc rx;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;

                if (mem_service_take_pending_qwen3_object_desc(
                        rt,
                        owner_idx,
                        expected_epoch,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES,
                        &selected_desc)) {
                } else if (rt->ingress_queues[owner_idx]) {
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_qwen3_object_desc_matches(
                                &rx,
                                expected_epoch,
                                MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES)) {
                            selected_desc = rx;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
                if (selected_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                    usleep(10000);
                    continue;
                }
                if (!terminal_slot->region.addr &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    usleep(10000);
                    continue;
                }
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                selected_record_found =
                    mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                  &compact,
                                                                  &seen) &&
                    mem_service_slot_find_record_by_obmm_object_backing(
                        terminal_slot,
                        MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                        selected_desc.payload_offset,
                        selected_desc.payload_len,
                        selected_desc.cookie,
                        &selected_record);
            }
            if (!selected_record_found ||
                selected_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED ||
                selected_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED ||
                selected_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES ||
                !terminal_slot->region.addr ||
                selected_record.object_backing_offset + selected_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memcpy(payload_words,
                   (uint8_t *)terminal_slot->region.addr +
                       selected_record.object_backing_offset,
                   MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES);
            checksum = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                            MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES);
            if (payload_words[0] == decode_step &&
                checksum == selected_record.object_payload_checksum) {
                if (selected_token_out) {
                    *selected_token_out = payload_words[1];
                }
                printf("[mem_service] stage qwen3_engram_selected_token_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " bytes=%" PRIu64 " token=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       selected_key,
                       owner_idx + 1,
                       selected_record.version,
                       selected_record.object_backing_len,
                       payload_words[1],
                       checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_selected_token_wait=timeout step=%" PRIu64
           " object_key=%s\n",
           decode_step,
           selected_key);
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_history(struct mem_service *svc,
                                              uint64_t decode_step,
                                              uint64_t timeout_ms,
                                              uint64_t *history_tokens_out,
                                              uint64_t history_token_capacity,
                                              uint64_t *history_token_count_out,
                                              uint64_t *history_checksum_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    char history_key[96];
    long deadline;
    int owner_idx = mem_service_qwen3_engram_owner_index(mem_service_qwen3_range_nodes());
    uint64_t expected_version = decode_step + 1U;

    if (!svc || !history_tokens_out || history_token_capacity == 0 ||
        !history_token_count_out) {
        return -1;
    }
    *history_token_count_out = 0;
    if (history_checksum_out) {
        *history_checksum_out = 0;
    }
    mem_service_qwen3_engram_history_key(history_key, sizeof(history_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record history_record;
        struct obmm_desc history_desc;
        uint64_t payload_words[1024 + 2];
        uint64_t checksum;
        uint64_t history_token_count;
        uint64_t expected_bytes;
        uint16_t expected_epoch;
        bool history_record_found = false;

        memset(&history_record, 0, sizeof(history_record));
        memset(&history_desc, 0, sizeof(history_desc));
        expected_epoch = (uint16_t)expected_version;
        if (expected_epoch == 0) {
            expected_epoch = 1;
        }
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count) {
            terminal_slot = &rt->slots[owner_idx];
            if (owner_idx == rt->local_idx) {
                history_record_found =
                    mem_service_slot_find_record(terminal_slot,
                                           history_key,
                                           &history_record);
            } else {
                struct obmm_desc rx;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;

                if (mem_service_take_pending_qwen3_object_desc(
                        rt,
                        owner_idx,
                        expected_epoch,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                        3U * sizeof(uint64_t),
                        sizeof(payload_words),
                        &history_desc)) {
                } else if (rt->ingress_queues[owner_idx]) {
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_qwen3_object_desc_matches(
                                &rx,
                                expected_epoch,
                                MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                                3U * sizeof(uint64_t),
                                sizeof(payload_words))) {
                            history_desc = rx;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
                if (history_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                    usleep(10000);
                    continue;
                }
                if (!terminal_slot->region.addr &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    usleep(10000);
                    continue;
                }
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                history_record_found =
                    mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                  &compact,
                                                                  &seen) &&
                    mem_service_slot_find_record_by_obmm_object_backing(
                        terminal_slot,
                        MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                        history_desc.payload_offset,
                        history_desc.payload_len,
                        history_desc.cookie,
                        &history_record);
            }
            if (!history_record_found ||
                history_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY ||
                history_record.version != expected_version ||
                history_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY ||
                history_record.object_backing_len < 3U * sizeof(uint64_t) ||
                history_record.object_backing_len > sizeof(payload_words) ||
                !terminal_slot->region.addr ||
                history_record.object_backing_offset + history_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memset(payload_words, 0, sizeof(payload_words));
            memcpy(payload_words,
                   (uint8_t *)terminal_slot->region.addr +
                       history_record.object_backing_offset,
                   history_record.object_backing_len);
            checksum = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                            history_record.object_backing_len);
            history_token_count = payload_words[1];
            expected_bytes = (history_token_count + 2U) * sizeof(uint64_t);
            if (payload_words[0] == expected_version &&
                history_token_count > 0 &&
                history_token_count <= history_token_capacity &&
                expected_bytes == history_record.object_backing_len &&
                checksum == history_record.object_payload_checksum) {
                memcpy(history_tokens_out,
                       &payload_words[2],
                       history_token_count * sizeof(uint64_t));
                *history_token_count_out = history_token_count;
                if (history_checksum_out) {
                    *history_checksum_out = checksum;
                }
                printf("[mem_service] stage qwen3_engram_history_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " history_tokens=%" PRIu64 " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       history_key,
                       owner_idx + 1,
                       history_record.version,
                       history_token_count,
                       history_record.object_backing_len,
                       checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_history_wait=timeout step=%" PRIu64
           " object_key=%s expected_version=%" PRIu64 "\n",
           decode_step,
           history_key,
           expected_version);
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_state(struct mem_service *svc,
                                            uint64_t decode_step,
                                            uint64_t timeout_ms,
                                            uint64_t expected_history_token_count,
                                            uint64_t expected_selected_token,
                                            uint64_t expected_history_checksum,
                                            uint64_t no_repeat_ngram_size,
                                            uint64_t repetition_penalty_milli,
                                            uint64_t *state_checksum_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    char state_key[96];
    long deadline;
    int owner_idx = mem_service_qwen3_engram_owner_index(mem_service_qwen3_range_nodes());

    if (!svc || expected_history_token_count == 0) {
        return -1;
    }
    if (state_checksum_out) {
        *state_checksum_out = 0;
    }
    mem_service_qwen3_engram_state_key(decode_step, state_key, sizeof(state_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record state_record;
        struct obmm_desc state_desc;
        uint64_t state_words[16];
        uint64_t inner_checksum;
        uint64_t state_checksum;
        bool state_record_found = false;

        memset(&state_record, 0, sizeof(state_record));
        memset(&state_desc, 0, sizeof(state_desc));
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count) {
            terminal_slot = &rt->slots[owner_idx];
            if (owner_idx == rt->local_idx) {
                state_record_found =
                    mem_service_slot_find_record(terminal_slot,
                                           state_key,
                                           &state_record);
            } else {
                struct obmm_desc rx;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                uint16_t expected_epoch = (uint16_t)(decode_step + 1U);

                if (expected_epoch == 0) {
                    expected_epoch = 1;
                }
                if (mem_service_take_pending_qwen3_object_desc(
                        rt,
                        owner_idx,
                        expected_epoch,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES,
                        &state_desc)) {
                } else if (rt->ingress_queues[owner_idx]) {
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_qwen3_object_desc_matches(
                                &rx,
                                expected_epoch,
                                MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES)) {
                            state_desc = rx;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
                if (state_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                    usleep(10000);
                    continue;
                }
                if (!terminal_slot->region.addr &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    usleep(10000);
                    continue;
                }
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                state_record_found =
                    mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                  &compact,
                                                                  &seen) &&
                    mem_service_slot_find_record_by_obmm_object_backing(
                        terminal_slot,
                        MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                        state_desc.payload_offset,
                        state_desc.payload_len,
                        state_desc.cookie,
                        &state_record);
            }
            if (!state_record_found ||
                state_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE ||
                state_record.version != 1U ||
                state_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE ||
                state_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES ||
                !terminal_slot->region.addr ||
                state_record.object_backing_offset + state_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memset(state_words, 0, sizeof(state_words));
            memcpy(state_words,
                   (uint8_t *)terminal_slot->region.addr +
                       state_record.object_backing_offset,
                   MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES);
            inner_checksum = mem_service_checksum_bytes((const uint8_t *)state_words,
                                                  15U * sizeof(uint64_t));
            state_checksum = mem_service_checksum_bytes((const uint8_t *)state_words,
                                                  MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES);
            if (state_words[0] == decode_step &&
                state_words[1] == expected_history_token_count &&
                state_words[2] == expected_selected_token &&
                state_words[3] == expected_history_checksum &&
                state_words[4] == no_repeat_ngram_size &&
                state_words[5] == repetition_penalty_milli &&
                state_words[15] == inner_checksum &&
                state_checksum == state_record.object_payload_checksum) {
                if (state_checksum_out) {
                    *state_checksum_out = state_checksum;
                }
                printf("[mem_service] stage qwen3_engram_state_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " history_tokens=%" PRIu64 " selected_token=%" PRIu64
                       " history_checksum=0x%016" PRIx64
                       " blocked=%" PRIu64 " fallback=%" PRIu64
                       " raw_token=%" PRIu64 " runner_up=%" PRIu64
                       " top_score_milli=%" PRId64
                       " runner_up_score_milli=%" PRId64
                       " history_window=%" PRIu64
                       " logits_checksum=0x%016" PRIx64
                       " text_checksum=0x%016" PRIx64
                       " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       state_key,
                       owner_idx + 1,
                       state_record.version,
                       state_words[1],
                       state_words[2],
                       state_words[3],
                       state_words[6],
                       state_words[7],
                       state_words[8],
                       state_words[9],
                       (int64_t)state_words[10],
                       (int64_t)state_words[11],
                       state_words[12],
                       state_words[13],
                       state_words[14],
                       state_record.object_backing_len,
                       state_checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_state_wait=timeout step=%" PRIu64
           " object_key=%s expected_history_tokens=%" PRIu64
           " expected_selected_token=%" PRIu64
           " expected_history_checksum=0x%016" PRIx64 "\n",
           decode_step,
           state_key,
           expected_history_token_count,
           expected_selected_token,
           expected_history_checksum);
    return -1;
}

int mem_service_obmm_service_v0_publish_decode_round_done(struct mem_service *svc,
                                                    uint32_t local_node,
                                                    uint32_t cluster_node_count,
                                                    uint64_t decode_step,
                                                    uint64_t round_scope_hash)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    uint64_t payload_words[8];
    uint64_t checksum;
    uint64_t slot_index;
    uint64_t slot_offset;
    uint8_t *base;

    if (!svc || cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count ||
        mem_service_cluster_runtime_require(rt) != 0) {
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        local_slot->region.len <
            MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_OFFSET +
                MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_REGION_BYTES) {
        return -1;
    }

    slot_index = (decode_step ^
                  (round_scope_hash * 11400714819323198485ULL)) &
                 (MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_SLOTS - 1ULL);
    slot_offset = MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_OFFSET +
                  slot_index * MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES;
    payload_words[0] = 0x71336465636f6465ULL;
    payload_words[1] = decode_step;
    payload_words[2] = local_node;
    payload_words[3] = cluster_node_count;
    payload_words[4] = rt->publish_seq;
    payload_words[5] = rt->observe_epoch;
    payload_words[6] = round_scope_hash;
    payload_words[7] = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                            7U * sizeof(payload_words[0]));
    checksum = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                    MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + slot_offset, payload_words, MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES);
    if (mem_service_update_region_range_at(local_slot, slot_offset,
                                     MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES, true) != 0) {
        return -1;
    }
    (void)msync(base + slot_offset, MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES, MS_SYNC);
    printf("[mem_service] stage qwen3_decode_round_done_publish local=node%u step=%" PRIu64
           " offset=0x%016" PRIx64 " slot=%" PRIu64
           " bytes=%" PRIu64 " scope_hash=0x%016" PRIx64
           " checksum=0x%016" PRIx64
           " backing=obmm_pool status=ok\n",
           local_node + 1U,
           decode_step,
           slot_offset,
           slot_index,
           (uint64_t)MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES,
           round_scope_hash,
           checksum);
    mem_service_report_obmm_pool_usage(rt, local_node, decode_step);
    return 0;
}

int mem_service_obmm_service_v0_wait_all_decode_round_done(struct mem_service *svc,
                                                     uint32_t cluster_node_count,
                                                     uint64_t decode_step,
                                                     uint64_t round_scope_hash,
                                                     uint64_t timeout_ms)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    long deadline;
    uint32_t ready_mask = 0;
    uint32_t expected_mask;
    uint64_t slot_index;
    uint64_t slot_offset;

    if (!svc || cluster_node_count != mem_service_qwen3_range_nodes() ||
        mem_service_cluster_runtime_require(rt) != 0) {
        return -1;
    }
    slot_index = (decode_step ^
                  (round_scope_hash * 11400714819323198485ULL)) &
                 (MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_SLOTS - 1ULL);
    slot_offset = MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_OFFSET +
                  slot_index * MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES;
    expected_mask = (1U << cluster_node_count) - 1U;
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        ready_mask = 0;
        for (uint32_t i = 0; i < cluster_node_count; ++i) {
            struct mem_service_cluster_slot *slot;
            uint64_t payload_words[8];

            if ((int)i != rt->local_idx &&
                mem_service_activate_remote_slot(rt, (int)i) != 0) {
                continue;
            }
            slot = &rt->slots[i];
            if (!slot->region.addr ||
                slot->region.len <
                    slot_offset + MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES) {
                continue;
            }
            memcpy(payload_words,
                   (uint8_t *)slot->region.addr + slot_offset,
                   MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES);
            if (payload_words[0] == 0x71336465636f6465ULL &&
                payload_words[1] == decode_step &&
                payload_words[2] == i &&
                payload_words[3] == cluster_node_count &&
                payload_words[6] == round_scope_hash &&
                payload_words[7] ==
                    mem_service_checksum_bytes((const uint8_t *)payload_words,
                                         7U * sizeof(payload_words[0]))) {
                ready_mask |= 1U << i;
            }
        }
        if (ready_mask == expected_mask) {
            printf("[mem_service] stage qwen3_decode_round_barrier step=%" PRIu64
                   " nodes=%u ready_mask=0x%02x slot=%" PRIu64
                   " scope_hash=0x%016" PRIx64 " status=ok\n",
                   decode_step,
                   cluster_node_count,
                   ready_mask,
                   slot_index,
                   round_scope_hash);
            return 0;
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_decode_round_barrier=timeout step=%" PRIu64
           " nodes=%u ready_mask=0x%02x expected_mask=0x%02x"
           " slot=%" PRIu64 " scope_hash=0x%016" PRIx64 "\n",
           decode_step,
           cluster_node_count,
           ready_mask,
           expected_mask,
           slot_index,
           round_scope_hash);
    return -1;
}
