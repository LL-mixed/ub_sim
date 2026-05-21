#include "w4_kvcache_db_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../kernel_ub/include/uapi/ub/obmm.h"
#include "common/obmm_common.h"
#include "libs/obmm_queue/obmm_queue_types.h"
#include "libs/obmm_queue/obmm_spsc_queue.h"

#define W4_DB_CLUSTER_MAX_NODES 8
#define W4_DB_CLUSTER_MAX_RECORDS 1024
#define W4_DB_MAYBE_UNUSED __attribute__((unused))
#define W4_DB_QWEN3_RECORD_RETAIN_STEPS 16ULL
#define W4_DB_DEFAULT_REGION_SIZE_MB 512
#define W4_DB_CMDLINE_REGION_SIZE "w4_db_region_size_mb"
#define W4_DB_CLUSTER_QUEUE_DEPTH 512
#define W4_DB_CLUSTER_PENDING_DESC_DEPTH 16
#define W4_DB_CLUSTER_WAIT_MS 300000L
#define W4_DB_OBMM_SERVICE_WAIT_MS 300000L
#define W4_DB_QWEN3_RUNTIME_RANGE_WAIT_MS 600000L
#define W4_DB_CLUSTER_IMPORT_ALIGN (2ULL * 1024ULL * 1024ULL)
#define W4_DB_CLUSTER_MAX_WINDOWS 16
#define W4_DB_OBMM_DEMO_OBJECT_BYTES 8192ULL
#define W4_DB_OBMM_WEIGHT_OFFSET 0x10000ULL
#define W4_DB_OBMM_KVCACHE_OFFSET 0x14000ULL
#define W4_DB_OBMM_HIDDEN_RANGE_INPUT_OFFSET 0x18000ULL
#define W4_DB_OBMM_HIDDEN_RANGE_OUTPUT_OFFSET 0x58000ULL
#define W4_DB_OBMM_HIDDEN_RANGE_RUNTIME_OUTPUT_OFFSET 0x98000ULL
#define W4_DB_OBMM_QWEN3_ROUND_DONE_OFFSET 0xda000ULL
#define W4_DB_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET 0x100000ULL
#define W4_DB_OBMM_QWEN3_KV_STATE_OFFSET 0x100000ULL
#define W4_DB_OBMM_QWEN3_KV_STATE_SLOT_BYTES 0x200000ULL
#define W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES 0x40000ULL
#define W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES 0x80000ULL
#define W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES 0x100000ULL
#define W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES \
    W4_DB_OBMM_QWEN3_KV_STATE_SLOT_BYTES
#define W4_DB_OBMM_QWEN3_KV_STATE_SLOTS 32ULL
#define W4_DB_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_SLOTS 32ULL
#define W4_DB_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_OFFSET \
    (W4_DB_OBMM_QWEN3_KV_STATE_OFFSET + \
     (W4_DB_OBMM_QWEN3_KV_STATE_SLOT_BYTES * \
      W4_DB_OBMM_QWEN3_KV_STATE_SLOTS))
#define W4_DB_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_REGION_BYTES \
    (W4_DB_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_SLOTS * \
     W4_DB_OBMM_HIDDEN_RANGE_BYTES)
#define W4_DB_OBMM_QWEN3_ENGRAM_BASE_OFFSET \
    (W4_DB_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_OFFSET + \
     W4_DB_OBMM_QWEN3_RUNTIME_RANGE_OUTPUT_REGION_BYTES)
#define W4_DB_OBMM_QWEN3_ENGRAM_SLOT_BYTES 0x4000ULL
#define W4_DB_OBMM_QWEN3_ENGRAM_HISTORY_BYTES 0x2000ULL
#define W4_DB_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES 256ULL
#define W4_DB_OBMM_QWEN3_ENGRAM_SELECTED_BYTES 64ULL
#define W4_DB_OBMM_QWEN3_ENGRAM_STATE_BYTES 128ULL
#define W4_DB_OBMM_HIDDEN_RANGE_BYTES 262144ULL
#define W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES 64ULL
#define W4_DB_OBMM_QWEN3_ROUND_DONE_BYTES 64ULL
#define W4_DB_OBMM_KIND_WEIGHT_TILE 1U
#define W4_DB_OBMM_KIND_KVCACHE_BLOCK 2U
#define W4_DB_OBMM_KIND_HIDDEN_RANGE_INPUT 3U
#define W4_DB_OBMM_KIND_HIDDEN_RANGE_OUTPUT 4U
#define W4_DB_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT 5U
#define W4_DB_OBMM_KIND_QWEN3_TOKEN_RESULT 6U
#define W4_DB_OBMM_KIND_QWEN3_KV_STATE 7U
#define W4_DB_OBMM_KIND_QWEN3_ENGRAM_HISTORY 8U
#define W4_DB_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES 9U
#define W4_DB_OBMM_KIND_QWEN3_ENGRAM_SELECTED 10U
#define W4_DB_OBMM_KIND_QWEN3_ENGRAM_STATE 11U
#define W4_DB_QWEN3_LAYER_COUNT 28U
#define W4_DB_QWEN3_RANGE_NODES 8U
#define W4_DB_QWEN3_KV_HEADS 8ULL
#define W4_DB_QWEN3_HEAD_DIM 128ULL
#define W4_DB_QWEN3_KV_STREAMS 2ULL
#define W4_DB_QWEN3_KV_ELEM_BYTES 4ULL

struct w4_db_cluster_meta {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
};

struct w4_db_cluster_payload {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
    uint8_t record_pad[48];
    struct w4_db_record records[W4_DB_CLUSTER_MAX_RECORDS];
};

struct w4_db_cluster_payload_header {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
};

struct w4_db_cluster_payload_compact_summary {
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

struct w4_db_qwen3_layer_range_placement {
    uint32_t owner_node;
    uint32_t layer_start;
    uint32_t layer_end;
    uint32_t next_owner_node;
    uint32_t layer_count;
    bool terminal;
};

#define W4_DB_COMPACT_PREFIX_STATE_READY 0x0001U
#define W4_DB_COMPACT_PREFIX_VIEW_READY 0x0002U

struct w4_db_mapped_region {
    int fd;
    void *addr;
    size_t len;
    uint64_t mem_id;
};

struct w4_db_cluster_slot {
    int owner_idx;
    bool is_local;
    bool map_osync;
    uint32_t export_cna;
    uint64_t mem_id;
    uint64_t local_pa;
    struct w4_db_mapped_region region;
};

struct w4_db_cluster_runtime {
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
    struct w4_db_cluster_meta metas[W4_DB_CLUSTER_MAX_NODES];
    struct w4_db_cluster_slot slots[W4_DB_CLUSTER_MAX_NODES];
    struct obmm_spsc_queue *ingress_queues[W4_DB_CLUSTER_MAX_NODES];
    void *ingress_queue_base;
    struct obmm_spsc_queue *egress_queues[W4_DB_CLUSTER_MAX_NODES];
    struct obmm_helpers_region egress_import[W4_DB_CLUSTER_MAX_NODES];
    struct obmm_desc pending_descs[W4_DB_CLUSTER_MAX_NODES][W4_DB_CLUSTER_PENDING_DESC_DEPTH];
    uint8_t pending_desc_count[W4_DB_CLUSTER_MAX_NODES];
};

#define W4_DB_CLUSTER_PAYLOAD_MAGIC 0x57344450U
#define W4_DB_CLUSTER_PAYLOAD_VERSION 1U

static struct w4_db_cluster_runtime g_w4_db_cluster_runtime;

static struct w4_db_record *w4_db_alloc_record(struct w4_db_service *svc);
static struct w4_db_record *w4_db_find_record(struct w4_db_service *svc, const char *key);
static struct w4_db_record *w4_db_recycle_qwen3_runtime_record(
    struct w4_db_service *svc,
    const char *incoming_key);
static int w4_db_activate_remote_slot(struct w4_db_cluster_runtime *rt, int owner_idx);

static long w4_db_wallclock_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void w4_db_cpu_relax_wait(unsigned int *attempt)
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

static bool w4_db_parse_ip_list(const char *csv,
                                char ips[W4_DB_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
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
    while (tok && count < W4_DB_CLUSTER_MAX_NODES) {
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

static bool w4_db_resolve_cluster_nodes(char local_ip[INET_ADDRSTRLEN],
                                        char ips[W4_DB_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
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
    if (!w4_db_parse_ip_list(env_all, ips, node_count)) {
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

static bool w4_db_parse_hex_file_u64(const char *path, uint64_t *value)
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

static int w4_db_update_region_range_at(const struct w4_db_cluster_slot *slot,
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
            "[w4_db] update_range_failed owner=%d write=%d fd=%d start=%#llx end=%#llx errno=%d\n",
            slot->owner_idx + 1, for_write ? 1 : 0, slot->region.fd,
            (unsigned long long)cmd.start, (unsigned long long)cmd.end, errno);
    return -1;
}

static int w4_db_update_region_range(const struct w4_db_cluster_slot *slot, bool for_write)
{
    return w4_db_update_region_range_at(slot, 0, sizeof(struct w4_db_cluster_payload), for_write);
}

static int W4_DB_MAYBE_UNUSED w4_db_sync_remote_range(
    const struct w4_db_cluster_slot *slot,
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
            "[w4_db] sync_remote_range_failed owner=%d fd=%d target=%s offset=%#" PRIx64
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

static uint16_t w4_db_snapshot_metadata_records(struct w4_db_service *svc,
                                                struct w4_db_record *out,
                                                uint16_t max_records)
{
    uint16_t count = 0;
    size_t i;

    if (!svc || !out || max_records == 0) {
        return 0;
    }
    for (i = 0; i < W4_DB_MAX_RECORDS && count < max_records; ++i) {
        if (!svc->records[i].in_use) {
            continue;
        }
        out[count++] = svc->records[i];
    }
    return count;
}

static void w4_db_build_compact_summary(const struct w4_db_record *records,
                                        uint16_t record_count,
                                        struct w4_db_cluster_payload_compact_summary *summary)
{
    uint16_t i;

    memset(summary, 0, sizeof(*summary));
    summary->record_count = record_count;
    summary->flags = W4_DB_COMPACT_PREFIX_STATE_READY | W4_DB_COMPACT_PREFIX_VIEW_READY;
    for (i = 0; i < record_count; ++i) {
        const struct w4_db_record *rec = &records[i];

        if (!rec->in_use) {
            summary->flags &= (uint16_t)~(W4_DB_COMPACT_PREFIX_STATE_READY |
                                          W4_DB_COMPACT_PREFIX_VIEW_READY);
            continue;
        }
        if (rec->kind == W4_DB_RECORD_REQUEST_PREFIX) {
            summary->prefix_count += 1;
            if (summary->prefix_version_floor == 0 ||
                rec->version < summary->prefix_version_floor) {
                summary->prefix_version_floor = rec->version;
            }
            if (summary->prefix_result_floor == 0 ||
                rec->last_result_segment < summary->prefix_result_floor) {
                summary->prefix_result_floor = rec->last_result_segment;
            }
            if (rec->state != W4_KVCACHE_STATE_RELOADED) {
                summary->flags &= (uint16_t)~W4_DB_COMPACT_PREFIX_STATE_READY;
            }
            if (rec->hot_segment_id == 0 || rec->last_result_segment == 0) {
                summary->flags &= (uint16_t)~W4_DB_COMPACT_PREFIX_VIEW_READY;
            }
        } else if (rec->kind == W4_DB_RECORD_PREFIX_GROUP) {
            summary->group_count += 1;
        } else if (rec->kind == W4_DB_RECORD_BLOCK_META) {
            summary->block_count += 1;
            if (summary->block_version_floor == 0 ||
                rec->version < summary->block_version_floor) {
                summary->block_version_floor = rec->version;
            }
            if (summary->block_result_floor == 0 ||
                rec->last_result_segment < summary->block_result_floor) {
                summary->block_result_floor = rec->last_result_segment;
            }
        } else if (rec->kind == W4_DB_RECORD_WEIGHT_TILE) {
            summary->weight_tile_count += 1;
        } else if (rec->kind == W4_DB_RECORD_KVCACHE_OBJECT) {
            summary->kvcache_object_count += 1;
        } else if (rec->kind == W4_DB_RECORD_HIDDEN_RANGE_INPUT ||
                   rec->kind == W4_DB_RECORD_HIDDEN_RANGE_OUTPUT) {
            summary->hidden_range_count += 1;
        } else if (rec->kind == W4_DB_RECORD_LAYER_RANGE_PLACEMENT) {
            summary->hidden_range_count += 1;
        } else {
            summary->flags &= (uint16_t)~(W4_DB_COMPACT_PREFIX_STATE_READY |
                                          W4_DB_COMPACT_PREFIX_VIEW_READY);
        }
    }
}

static int w4_db_write_cluster_payload(struct w4_db_service *svc,
                                       struct w4_db_cluster_slot *slot)
{
    struct w4_db_cluster_payload *payload;
    struct w4_db_cluster_payload_compact_summary compact;
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    uint32_t seq;
    int rc = -1;

    if (!svc || !slot || !slot->region.addr) {
        return -1;
    }
    payload = calloc(1, sizeof(*payload));
    if (!payload) {
        return -1;
    }
    seq = ++rt->publish_seq;
    if (seq == 0) {
        seq = ++rt->publish_seq;
    }
    payload->magic = W4_DB_CLUSTER_PAYLOAD_MAGIC;
    payload->version = W4_DB_CLUSTER_PAYLOAD_VERSION;
    payload->record_count = w4_db_snapshot_metadata_records(svc,
                                                            payload->records,
                                                            W4_DB_CLUSTER_MAX_RECORDS);
    w4_db_build_compact_summary(payload->records, payload->record_count, &compact);
    memcpy(payload->record_pad, &compact, sizeof(compact));
    payload->publish_seq = seq;
    payload->publish_done_seq = 0;
    memset(slot->region.addr, 0, sizeof(*payload));
    memcpy(slot->region.addr, payload, sizeof(*payload));
    __sync_synchronize();
    ((struct w4_db_cluster_payload *)slot->region.addr)->publish_done_seq = seq;
    __sync_synchronize();
    if (w4_db_update_region_range(slot, true) != 0) {
        goto out;
    }
    (void)msync(slot->region.addr, sizeof(*payload), MS_SYNC);
    {
        const uint8_t *bytes = (const uint8_t *)slot->region.addr;
        uint64_t probe_040 = 0;
        uint64_t probe_048 = 0;
        uint64_t probe_050 = 0;

        memcpy(&probe_040, bytes + 0x40, sizeof(probe_040));
        memcpy(&probe_048, bytes + 0x48, sizeof(probe_048));
        memcpy(&probe_050, bytes + 0x50, sizeof(probe_050));
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=write_local_payload probe040=%#" PRIx64 " probe048=%#" PRIx64 " probe050=%#" PRIx64 "\n",
               slot->owner_idx + 1,
               probe_040,
               probe_048,
               probe_050);
    }
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=write_local_done seq=%u done=%u count=%u\n",
           slot->owner_idx + 1,
           ((const struct w4_db_cluster_payload *)slot->region.addr)->publish_seq,
           ((const struct w4_db_cluster_payload *)slot->region.addr)->publish_done_seq,
           ((const struct w4_db_cluster_payload *)slot->region.addr)->record_count);
    rc = 0;

out:
    free(payload);
    return rc;
}

static uint64_t w4_db_checksum_bytes(const uint8_t *bytes, uint64_t len)
{
    uint64_t hash = 1469598103934665603ULL;
    uint64_t i;

    for (i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

int w4_db_record_to_lingqu_obmm_ref(const struct w4_db_record *record,
                                    struct lingqu_obmm_object_ref_wire *ref_out)
{
    if (!record || !record->in_use || !ref_out ||
        record->object_backing_len == 0) {
        return -1;
    }
    memset(ref_out, 0, sizeof(*ref_out));
    ref_out->magic = LINGQU_OBMM_OBJECT_REF_MAGIC;
    ref_out->layout_version = LINGQU_OBMM_OBJECT_REF_LAYOUT_VERSION;
    ref_out->object_kind = (uint16_t)record->object_payload_kind;
    ref_out->state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    ref_out->owner_entity = record->object_owner_node;
    ref_out->producer_entity = record->object_owner_node;
    ref_out->object_version = record->version;
    ref_out->key_hash =
        w4_db_checksum_bytes((const uint8_t *)record->key,
                             (uint64_t)strnlen(record->key,
                                               sizeof(record->key)));
    ref_out->payload_offset = record->object_backing_offset;
    ref_out->payload_bytes = record->object_backing_len;
    ref_out->payload_checksum = record->object_payload_checksum;
    return 0;
}

static uint64_t w4_db_qwen3_hidden_payload_checksum(const uint8_t *bytes,
                                                    uint64_t len)
{
    uint64_t acc = 0xcbf29ce484222325ULL;
    uint64_t index;

    for (index = 0; index < len; ++index) {
        acc ^= (uint64_t)bytes[index] | (index << 8);
        acc *= 0x00000100000001b3ULL;
    }
    return acc;
}

static void w4_db_fill_obmm_object_payload(uint8_t *dst,
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
        memcpy(dst + 0, "W4OBMM00", 8);
        memcpy(dst + 248, "W4OBMM248", 9);
        memcpy(dst + 256, "W4OBMM256", 9);
        memcpy(dst + 4088, "W4OBMM4088", 10);
        memcpy(dst + 4096, "W4OBMM4096", 10);
    }
}

static const char *w4_db_object_kind_name(uint32_t payload_kind)
{
    switch (payload_kind) {
    case W4_DB_OBMM_KIND_WEIGHT_TILE:
        return "weight_tile";
    case W4_DB_OBMM_KIND_KVCACHE_BLOCK:
        return "kvcache_block";
    case W4_DB_OBMM_KIND_HIDDEN_RANGE_INPUT:
        return "hidden_range_input";
    case W4_DB_OBMM_KIND_HIDDEN_RANGE_OUTPUT:
        return "hidden_range_output";
    case W4_DB_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT:
        return "hidden_range_runtime_output";
    case W4_DB_OBMM_KIND_QWEN3_TOKEN_RESULT:
        return "qwen3_token_result";
    case W4_DB_OBMM_KIND_QWEN3_KV_STATE:
        return "qwen3_kv_state";
    case W4_DB_OBMM_KIND_QWEN3_ENGRAM_HISTORY:
        return "qwen3_engram_history";
    case W4_DB_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES:
        return "qwen3_engram_candidates";
    case W4_DB_OBMM_KIND_QWEN3_ENGRAM_SELECTED:
        return "qwen3_engram_selected";
    case W4_DB_OBMM_KIND_QWEN3_ENGRAM_STATE:
        return "qwen3_engram_state";
    default:
        return "unknown";
    }
}

static int w4_db_payload_arena_alloc(struct w4_db_cluster_runtime *rt,
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
            obmm_align_up_u64(W4_DB_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET, align);
        rt->payload_arena_next = rt->payload_arena_base;
        rt->payload_arena_high_water = rt->payload_arena_base;
    }
    offset = obmm_align_up_u64(rt->payload_arena_next, align);
    end = offset + bytes;
    if (end < offset ||
        !rt->slots[rt->local_idx].region.addr ||
        end > rt->slots[rt->local_idx].region.len) {
        printf("[w4_guest] gap obmm_pool_allocator=exhausted local=node%d offset=0x%016" PRIx64
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

static int w4_db_qwen3_kv_state_block_span(uint64_t payload_len,
                                           uint64_t *block_bytes_out,
                                           uint64_t *block_count_out,
                                           uint64_t *reserved_bytes_out)
{
    uint64_t block_bytes;
    uint64_t block_count;
    uint64_t reserved_bytes;

    if (!block_bytes_out || !block_count_out || !reserved_bytes_out ||
        payload_len == 0) {
        return -1;
    }
    if (payload_len <= W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES) {
        block_bytes = W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES;
        block_count = 1U;
    } else if (payload_len <= W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES) {
        block_bytes = W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES;
        block_count = 1U;
    } else if (payload_len <= W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES) {
        block_bytes = W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES;
        block_count = 1U;
    } else if (payload_len <= W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES) {
        block_bytes = W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES;
        block_count = 1U;
    } else {
        block_bytes = W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES;
        block_count =
            (payload_len + block_bytes - 1U) / block_bytes;
    }
    if (block_count == 0 || block_count > UINT64_MAX / block_bytes) {
        return -1;
    }
    reserved_bytes = block_count * block_bytes;
    if (reserved_bytes < payload_len) {
        return -1;
    }
    *block_bytes_out = block_bytes;
    *block_count_out = block_count;
    *reserved_bytes_out = reserved_bytes;
    return 0;
}

static int w4_db_qwen3_kv_state_alloc(struct w4_db_cluster_runtime *rt,
                                      uint64_t payload_len,
                                      uint64_t *offset_out,
                                      uint64_t *block_bytes_out,
                                      uint64_t *block_count_out,
                                      uint64_t *reserved_bytes_out)
{
    uint64_t block_bytes = 0;
    uint64_t block_count = 0;
    uint64_t reserved_bytes = 0;

    if (!offset_out ||
        w4_db_qwen3_kv_state_block_span(payload_len,
                                        &block_bytes,
                                        &block_count,
                                        &reserved_bytes) != 0) {
        return -1;
    }
    if (block_bytes_out) {
        *block_bytes_out = block_bytes;
    }
    if (block_count_out) {
        *block_count_out = block_count;
    }
    if (reserved_bytes_out) {
        *reserved_bytes_out = reserved_bytes;
    }
    return w4_db_payload_arena_alloc(rt,
                                     reserved_bytes,
                                     block_bytes,
                                     offset_out);
}

static void w4_db_report_obmm_pool_layout_once(struct w4_db_cluster_runtime *rt)
{
    struct w4_db_cluster_slot *local_slot;

    if (!rt || rt->pool_layout_reported || rt->local_idx < 0 ||
        rt->local_idx >= rt->node_count) {
        return;
    }
    local_slot = &rt->slots[rt->local_idx];
    if (!local_slot->region.addr) {
        return;
    }
    printf("[w4_guest] stage qwen3_obmm_pool_layout local=node%d nodes=%d per_node_region_bytes=%" PRIu64
           " cluster_region_bytes=%" PRIu64 " payload_offset=%" PRIu64
           " payload_bytes=%zu arena_base=0x%016" PRIx64
           " allocator=linear_payload_arena status=ok\n",
           rt->local_idx + 1,
           rt->node_count,
           rt->region_size,
           rt->region_size * (uint64_t)rt->node_count,
           rt->payload_offset,
           local_slot->region.len,
           (uint64_t)W4_DB_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET);
    rt->pool_layout_reported = true;
}

static void w4_db_report_obmm_pool_usage(struct w4_db_cluster_runtime *rt,
                                         uint32_t local_node,
                                         uint64_t decode_step)
{
    struct w4_db_cluster_slot *local_slot;
    uint64_t arena_used;
    uint64_t payload_used;

    if (!rt || local_node >= (uint32_t)rt->node_count ||
        rt->local_idx < 0 || rt->local_idx >= rt->node_count) {
        return;
    }
    local_slot = &rt->slots[rt->local_idx];
    if (!local_slot->region.addr) {
        return;
    }
    arena_used =
        rt->payload_arena_high_water > rt->payload_arena_base ?
            rt->payload_arena_high_water - rt->payload_arena_base :
            0;
    payload_used = rt->payload_arena_high_water;
    printf("[w4_guest] stage qwen3_obmm_pool_usage local=node%u step=%" PRIu64
           " per_node_region_bytes=%" PRIu64 " cluster_region_bytes=%" PRIu64
           " payload_bytes=%zu payload_high_water_bytes=%" PRIu64
           " payload_used_pct_milli=%" PRIu64 " arena_base=0x%016" PRIx64
           " arena_used_bytes=%" PRIu64 " arena_next=0x%016" PRIx64
           " allocator=linear_payload_arena status=ok\n",
           local_node + 1U,
           decode_step,
           rt->region_size,
           rt->region_size * (uint64_t)rt->node_count,
           local_slot->region.len,
           payload_used,
           local_slot->region.len > 0 ?
               (uint64_t)((payload_used * 100000ULL) /
                          (uint64_t)local_slot->region.len) :
               (uint64_t)0,
           rt->payload_arena_base,
           arena_used,
           rt->payload_arena_next);
}

static int w4_db_qwen3_engram_owner_index(uint32_t cluster_node_count)
{
    const char *env = getenv("SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE");
    char *end = NULL;
    unsigned long parsed;

    if (cluster_node_count == 0) {
        return -1;
    }
    if (!env || !*env) {
        return (int)cluster_node_count - 1;
    }
    errno = 0;
    parsed = strtoul(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0' ||
        parsed == 0 || parsed > cluster_node_count) {
        return (int)cluster_node_count - 1;
    }
    return (int)parsed - 1;
}

static void w4_db_qwen3_engram_session_id(char *out, size_t out_len)
{
    const char *env = getenv("SIM_QWEN3_GUEST_ENGRAM_SESSION_ID");
    size_t len;

    if (!out || out_len == 0) {
        return;
    }
    if (!env || !*env) {
        snprintf(out, out_len, "guest");
        return;
    }
    len = strlen(env);
    if (len == 0 || len >= out_len) {
        snprintf(out, out_len, "guest");
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = env[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F') ||
              c == '_' || c == '-')) {
            snprintf(out, out_len, "guest");
            return;
        }
    }
    snprintf(out, out_len, "%s", env);
}

static void w4_db_qwen3_engram_history_key(char *out, size_t out_len)
{
    char session[32];

    w4_db_qwen3_engram_session_id(session, sizeof(session));
    snprintf(out, out_len, "qwen3/session/%s/tokens/history", session);
}

static void w4_db_qwen3_engram_candidates_key(uint64_t decode_step,
                                              char *out,
                                              size_t out_len)
{
    char session[32];

    w4_db_qwen3_engram_session_id(session, sizeof(session));
    snprintf(out,
             out_len,
             "qwen3/session/%s/step/%" PRIu64 "/candidates/topk",
             session,
             decode_step);
}

static void w4_db_qwen3_engram_selected_key(uint64_t decode_step,
                                            char *out,
                                            size_t out_len)
{
    char session[32];

    w4_db_qwen3_engram_session_id(session, sizeof(session));
    snprintf(out,
             out_len,
             "qwen3/session/%s/step/%" PRIu64 "/tokens/selected",
             session,
             decode_step);
}

static void w4_db_qwen3_engram_state_key(uint64_t decode_step,
                                         char *out,
                                         size_t out_len)
{
    char session[32];

    w4_db_qwen3_engram_session_id(session, sizeof(session));
    snprintf(out,
             out_len,
             "qwen3/session/%s/step/%" PRIu64 "/engram/state",
             session,
             decode_step);
}

static uint64_t w4_db_env_u64_or(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (!value || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return fallback;
    }
    return (uint64_t)parsed;
}

static uint32_t w4_db_qwen3_layer_count(void)
{
    uint64_t value = w4_db_env_u64_or("SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS",
                                      W4_DB_QWEN3_LAYER_COUNT);

    return value > UINT32_MAX ? W4_DB_QWEN3_LAYER_COUNT : (uint32_t)value;
}

static uint64_t w4_db_qwen3_hidden_range_bytes(void)
{
    return w4_db_env_u64_or("SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES",
                            W4_DB_OBMM_HIDDEN_RANGE_BYTES);
}

static uint64_t w4_db_qwen3_decode_hidden_bytes(void)
{
    uint64_t hidden_size = w4_db_env_u64_or("SIM_QWEN3_DENSE_HIDDEN_SIZE", 1024ULL);
    uint64_t decode_tokens = w4_db_env_u64_or("SIM_QWEN3_DENSE_DECODE_TOKENS", 1ULL);

    return w4_db_env_u64_or("SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES",
                            hidden_size * decode_tokens * 2ULL);
}

static uint64_t w4_db_qwen3_handoff_hidden_bytes(uint64_t decode_step)
{
    return decode_step > 0 ? w4_db_qwen3_decode_hidden_bytes() :
                             w4_db_qwen3_hidden_range_bytes();
}

static uint64_t w4_db_qwen3_kv_heads(void)
{
    return w4_db_env_u64_or("SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS",
                            W4_DB_QWEN3_KV_HEADS);
}

static uint64_t w4_db_qwen3_head_dim(void)
{
    return w4_db_env_u64_or("SIM_QWEN3_DENSE_HEAD_DIM",
                            W4_DB_QWEN3_HEAD_DIM);
}

static const char *w4_db_qwen3_model_key(void)
{
    const char *model_key = getenv("SIM_QWEN3_DENSE_MODEL_KEY");

    return model_key && model_key[0] != '\0' ? model_key : "qwen3-0.6b";
}

static uint64_t w4_db_qwen3_range_kv_state_bytes(uint32_t layer_start,
                                                 uint32_t layer_end,
                                                 uint64_t token_count)
{
    uint64_t layer_count;
    uint64_t bytes_per_token_per_layer;

    if (layer_end <= layer_start || layer_end > w4_db_qwen3_layer_count()) {
        return 0;
    }
    layer_count = (uint64_t)(layer_end - layer_start);
    bytes_per_token_per_layer = w4_db_qwen3_kv_heads() *
                                w4_db_qwen3_head_dim() *
                                W4_DB_QWEN3_KV_STREAMS *
                                W4_DB_QWEN3_KV_ELEM_BYTES;
    return layer_count * token_count * bytes_per_token_per_layer;
}

static void w4_db_qwen3_node_range(uint32_t node,
                                   uint32_t node_count,
                                   uint32_t *start_out,
                                   uint32_t *end_out)
{
    uint32_t layer_count = w4_db_qwen3_layer_count();
    uint32_t base = layer_count / node_count;
    uint32_t rem = layer_count % node_count;
    uint32_t start = 0;
    uint32_t i;

    for (i = 0; i < node; ++i) {
        start += base + (i < rem ? 1U : 0U);
    }
    if (start_out) {
        *start_out = start;
    }
    if (end_out) {
        *end_out = start + base + (node < rem ? 1U : 0U);
    }
}

static uint64_t w4_db_qwen3_placement_checksum(uint32_t owner,
                                               uint32_t start,
                                               uint32_t end,
                                               uint32_t next_owner,
                                               bool terminal)
{
    uint64_t hash = 1469598103934665603ULL;

    hash ^= owner + 1U;
    hash *= 1099511628211ULL;
    hash ^= start;
    hash *= 1099511628211ULL;
    hash ^= end;
    hash *= 1099511628211ULL;
    hash ^= next_owner + 1U;
    hash *= 1099511628211ULL;
    hash ^= terminal ? 1U : 0U;
    hash *= 1099511628211ULL;
    return hash;
}

static void w4_db_qwen3_placement_key(uint32_t owner_node,
                                      char *out,
                                      size_t out_len)
{
    snprintf(out,
             out_len,
             "placement/%s/layer-range/node%u",
             w4_db_qwen3_model_key(),
             owner_node + 1U);
}

int w4_db_qwen3_layer_range_for_node(uint32_t local_node,
                                     uint32_t cluster_node_count,
                                     uint32_t *layer_start_out,
                                     uint32_t *layer_end_out,
                                     uint32_t *next_node_out)
{
    if (cluster_node_count != W4_DB_QWEN3_RANGE_NODES ||
        local_node >= cluster_node_count || !layer_start_out ||
        !layer_end_out || !next_node_out) {
        return -1;
    }
    w4_db_qwen3_node_range(local_node,
                           cluster_node_count,
                           layer_start_out,
                           layer_end_out);
    *next_node_out = (local_node + 1U) % cluster_node_count;
    return 0;
}

static int w4_db_put_qwen3_layer_range_placement(
    struct w4_db_service *svc,
    const struct w4_db_qwen3_layer_range_placement *placement)
{
    struct w4_db_record *rec;
    char key[96];

    if (!svc || !placement || placement->layer_start >= placement->layer_end) {
        return -1;
    }
    w4_db_qwen3_placement_key(placement->owner_node, key, sizeof(key));
    rec = w4_db_find_record(svc, key);
    if (!rec) {
        rec = w4_db_alloc_record(svc);
    }
    if (!rec) {
        return -1;
    }
    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = W4_DB_RECORD_LAYER_RANGE_PLACEMENT;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    rec->placement_node = placement->owner_node;
    rec->placement_level = 2U;
    rec->hot_segment_id = placement->layer_start;
    rec->state = W4_KVCACHE_STATE_HOT;
    rec->version = 1U;
    rec->last_result_segment = placement->layer_end;
    rec->object_owner_node = placement->next_owner_node;
    rec->object_backing_len = placement->layer_count;
    rec->object_payload_checksum =
        w4_db_qwen3_placement_checksum(placement->owner_node,
                                       placement->layer_start,
                                       placement->layer_end,
                                       placement->next_owner_node,
                                       placement->terminal);
    return 0;
}

static int w4_db_publish_qwen3_layer_range_placements(
    struct w4_db_service *svc,
    uint32_t node_count)
{
    uint32_t i;

    if (!svc || node_count != W4_DB_QWEN3_RANGE_NODES) {
        return -1;
    }
    for (i = 0; i < node_count; ++i) {
        struct w4_db_qwen3_layer_range_placement placement;

        memset(&placement, 0, sizeof(placement));
        placement.owner_node = i;
        placement.next_owner_node = (i + 1U) % node_count;
        placement.terminal = (i + 1U == node_count);
        w4_db_qwen3_node_range(i,
                               node_count,
                               &placement.layer_start,
                               &placement.layer_end);
        placement.layer_count = placement.layer_end - placement.layer_start;
        if (w4_db_put_qwen3_layer_range_placement(svc, &placement) != 0) {
            return -1;
        }
    }
    return 0;
}

static bool w4_db_read_qwen3_layer_range_placement(
    struct w4_db_service *svc,
    uint32_t owner_node,
    struct w4_db_qwen3_layer_range_placement *placement)
{
    struct w4_db_record rec;
    char key[96];

    if (!svc || !placement) {
        return false;
    }
    w4_db_qwen3_placement_key(owner_node, key, sizeof(key));
    if (w4_db_get_record(svc, key, &rec) != 0 ||
        rec.kind != W4_DB_RECORD_LAYER_RANGE_PLACEMENT ||
        rec.hot_segment_id >= rec.last_result_segment ||
        rec.object_backing_len != rec.last_result_segment - rec.hot_segment_id) {
        return false;
    }
    memset(placement, 0, sizeof(*placement));
    placement->owner_node = rec.placement_node;
    placement->layer_start = (uint32_t)rec.hot_segment_id;
    placement->layer_end = (uint32_t)rec.last_result_segment;
    placement->next_owner_node = rec.object_owner_node;
    placement->layer_count = (uint32_t)rec.object_backing_len;
    placement->terminal = placement->next_owner_node < placement->owner_node;
    return rec.object_payload_checksum ==
           w4_db_qwen3_placement_checksum(placement->owner_node,
                                          placement->layer_start,
                                          placement->layer_end,
                                          placement->next_owner_node,
                                          placement->terminal);
}

static bool w4_db_find_qwen3_layer_range_predecessor(
    struct w4_db_service *svc,
    uint32_t owner_node,
    struct w4_db_qwen3_layer_range_placement *placement)
{
    uint32_t i;

    if (!svc || !placement) {
        return false;
    }
    for (i = 0; i < W4_DB_QWEN3_RANGE_NODES; ++i) {
        struct w4_db_qwen3_layer_range_placement candidate;

        if (!w4_db_read_qwen3_layer_range_placement(svc, i, &candidate)) {
            return false;
        }
        if (!candidate.terminal && candidate.next_owner_node == owner_node) {
            *placement = candidate;
            return true;
        }
    }
    return false;
}

static int w4_db_put_obmm_object_record(struct w4_db_service *svc,
                                        enum w4_db_record_kind record_kind,
                                        const char *key,
                                        uint32_t owner_node,
                                        uint32_t payload_kind,
                                        uint64_t offset,
                                        uint64_t len,
                                        uint64_t checksum,
                                        struct w4_db_record *resolved_out)
{
    struct w4_db_record *rec;
    uint64_t next_version = 1U;

    if (!svc || !key || len == 0) {
        return -1;
    }
    rec = w4_db_find_record(svc, key);
    if (rec && rec->version != UINT64_MAX) {
        next_version = rec->version + 1U;
    }
    if (!rec) {
        rec = w4_db_alloc_record(svc);
    }
    if (!rec) {
        rec = w4_db_recycle_qwen3_runtime_record(svc, key);
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
    rec->state = W4_KVCACHE_STATE_HOT;
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

static bool w4_db_try_read_stable_payload(const struct w4_db_cluster_payload *payload,
                                          struct w4_db_cluster_payload *snapshot)
{
    if (!payload || !snapshot) {
        return false;
    }
    {
        struct w4_db_cluster_payload_header header;
        uint16_t i;

        __sync_synchronize();
        header.magic = payload->magic;
        header.version = payload->version;
        header.record_count = payload->record_count;
        header.publish_seq = payload->publish_seq;
        header.publish_done_seq = payload->publish_done_seq;
        if (header.publish_seq == 0 ||
            header.publish_seq != header.publish_done_seq ||
            header.magic != W4_DB_CLUSTER_PAYLOAD_MAGIC ||
            header.version != W4_DB_CLUSTER_PAYLOAD_VERSION ||
            header.record_count == 0 ||
            header.record_count > W4_DB_CLUSTER_MAX_RECORDS) {
            return false;
        }
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->magic = header.magic;
        snapshot->version = header.version;
        snapshot->record_count = header.record_count;
        snapshot->publish_seq = header.publish_seq;
        snapshot->publish_done_seq = header.publish_done_seq;
        for (i = 0; i < header.record_count; ++i) {
            memcpy(&snapshot->records[i], &payload->records[i], sizeof(snapshot->records[i]));
        }
        __sync_synchronize();
        if (snapshot->publish_seq == snapshot->publish_done_seq &&
            snapshot->publish_seq == header.publish_seq &&
            snapshot->publish_done_seq == header.publish_done_seq &&
            snapshot->magic == W4_DB_CLUSTER_PAYLOAD_MAGIC &&
            snapshot->version == W4_DB_CLUSTER_PAYLOAD_VERSION &&
            snapshot->record_count == header.record_count) {
            return true;
        }
    }
    return false;
}

static bool w4_db_read_stable_payload(const struct w4_db_cluster_payload *payload,
                                      struct w4_db_cluster_payload *snapshot)
{
    int attempts = 8;

    while (attempts-- > 0) {
        if (w4_db_try_read_stable_payload(payload, snapshot)) {
            return true;
        }
        usleep(10000);
    }
    return false;
}

static void w4_db_copy_from_mapped_volatile(void *dst,
                                            const volatile uint8_t *src,
                                            size_t len)
{
    size_t i = 0;
    uint8_t *out = (uint8_t *)dst;

    for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
        uint64_t word = *(const volatile uint64_t *)(src + i);
        memcpy(out + i, &word, sizeof(word));
    }
    for (; i < len; ++i) {
        out[i] = src[i];
    }
}

static bool w4_db_try_read_stable_payload_region(const struct w4_db_cluster_slot *slot,
                                                 struct w4_db_cluster_payload *snapshot,
                                                 struct w4_db_cluster_payload_header *seen_out)
{
    struct w4_db_cluster_payload_header header;
    struct w4_db_cluster_payload_header confirm;
    uint16_t i;
    const volatile uint8_t *mapped_bytes;

    mapped_bytes = slot ? (const volatile uint8_t *)slot->region.addr : NULL;

    if (!slot || !snapshot) {
        return false;
    }
    if (slot->is_local) {
        bool ok = w4_db_try_read_stable_payload((const struct w4_db_cluster_payload *)slot->region.addr,
                                                snapshot);
        if (ok && seen_out) {
            seen_out->magic = snapshot->magic;
            seen_out->version = snapshot->version;
            seen_out->record_count = snapshot->record_count;
            seen_out->publish_seq = snapshot->publish_seq;
            seen_out->publish_done_seq = snapshot->publish_done_seq;
        }
        return ok;
    }
    if (slot->region.fd < 0) {
        return false;
    }
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=read_header_begin mem_id=%" PRIu64 " map_osync=%d addr=%p\n",
           slot->owner_idx + 1,
           g_w4_db_cluster_runtime.local_idx + 1,
           slot->mem_id,
           slot->map_osync ? 1 : 0,
           slot->region.addr);
    fflush(stdout);
    w4_db_copy_from_mapped_volatile(&header, mapped_bytes, sizeof(header));
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=read_header_done seq=%u done=%u count=%u\n",
           slot->owner_idx + 1,
           header.publish_seq,
           header.publish_done_seq,
           header.record_count);
    fflush(stdout);
    if (seen_out) {
        *seen_out = header;
    }
    if (header.publish_seq == 0 ||
        header.publish_seq != header.publish_done_seq ||
        header.magic != W4_DB_CLUSTER_PAYLOAD_MAGIC ||
        header.version != W4_DB_CLUSTER_PAYLOAD_VERSION ||
        header.record_count == 0 ||
        header.record_count > W4_DB_CLUSTER_MAX_RECORDS) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = header.magic;
    snapshot->version = header.version;
    snapshot->record_count = header.record_count;
    snapshot->publish_seq = header.publish_seq;
    snapshot->publish_done_seq = header.publish_done_seq;
    for (i = 0; i < header.record_count; ++i) {
        size_t record_off = offsetof(struct w4_db_cluster_payload, records) +
                            ((size_t)i * sizeof(snapshot->records[0]));
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=record_copy_begin record=%u offset=%zu bytes=%zu\n",
               slot->owner_idx + 1,
               g_w4_db_cluster_runtime.local_idx + 1,
               i,
               record_off,
               sizeof(snapshot->records[i]));
        fflush(stdout);
        w4_db_copy_from_mapped_volatile(&snapshot->records[i],
                                        mapped_bytes + record_off,
                                        sizeof(snapshot->records[i]));
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=record_copy_done record=%u offset=%zu bytes=%zu\n",
               slot->owner_idx + 1,
               g_w4_db_cluster_runtime.local_idx + 1,
               i,
               record_off,
               sizeof(snapshot->records[i]));
        fflush(stdout);
    }
    __sync_synchronize();
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=confirm_header_begin\n",
           slot->owner_idx + 1,
           g_w4_db_cluster_runtime.local_idx + 1);
    fflush(stdout);
    w4_db_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=confirm_header_done seq=%u done=%u count=%u\n",
           slot->owner_idx + 1,
           g_w4_db_cluster_runtime.local_idx + 1,
           confirm.publish_seq,
           confirm.publish_done_seq,
           confirm.record_count);
    fflush(stdout);
    if (confirm.publish_seq != header.publish_seq ||
        confirm.publish_done_seq != header.publish_done_seq ||
        confirm.magic != header.magic ||
        confirm.version != header.version ||
        confirm.record_count != header.record_count) {
        return false;
    }
    return true;
}

static bool w4_db_try_read_stable_compact_summary_region(
    const struct w4_db_cluster_slot *slot,
    struct w4_db_cluster_payload_compact_summary *summary,
    struct w4_db_cluster_payload_header *seen_out)
{
    struct w4_db_cluster_payload_header header;
    struct w4_db_cluster_payload_header confirm;
    const volatile uint8_t *mapped_bytes;
    size_t summary_off = offsetof(struct w4_db_cluster_payload, record_pad);

    if (!slot || !summary || !slot->region.addr) {
        return false;
    }
    if (slot->is_local) {
        const struct w4_db_cluster_payload *payload =
            (const struct w4_db_cluster_payload *)slot->region.addr;

        if (!w4_db_try_read_stable_payload(payload,
                                           &(struct w4_db_cluster_payload){ 0 })) {
            return false;
        }
        memcpy(summary, payload->record_pad, sizeof(*summary));
        if (seen_out) {
            seen_out->magic = payload->magic;
            seen_out->version = payload->version;
            seen_out->record_count = payload->record_count;
            seen_out->publish_seq = payload->publish_seq;
            seen_out->publish_done_seq = payload->publish_done_seq;
        }
        return true;
    }
    mapped_bytes = (const volatile uint8_t *)slot->region.addr;
    if (slot->region.fd < 0) {
        return false;
    }
    w4_db_copy_from_mapped_volatile(&header, mapped_bytes, sizeof(header));
    if (seen_out) {
        *seen_out = header;
    }
    if (header.publish_seq == 0 ||
        header.publish_seq != header.publish_done_seq ||
        header.magic != W4_DB_CLUSTER_PAYLOAD_MAGIC ||
        header.version != W4_DB_CLUSTER_PAYLOAD_VERSION ||
        header.record_count == 0 ||
        header.record_count > W4_DB_CLUSTER_MAX_RECORDS) {
        return false;
    }
    memset(summary, 0, sizeof(*summary));
    w4_db_copy_from_mapped_volatile(summary, mapped_bytes + summary_off, sizeof(*summary));
    __sync_synchronize();
    w4_db_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
    if (confirm.publish_seq != header.publish_seq ||
        confirm.publish_done_seq != header.publish_done_seq ||
        confirm.magic != header.magic ||
        confirm.version != header.version ||
        confirm.record_count != header.record_count ||
        summary->record_count != header.record_count) {
        return false;
    }
    return true;
}

static bool w4_db_wait_compact_summary_region_at_least(
    const struct w4_db_cluster_slot *slot,
    uint32_t min_publish_done_seq,
    long timeout_ms,
    struct w4_db_cluster_payload_compact_summary *summary,
    struct w4_db_cluster_payload_header *seen_out)
{
    long deadline;
    unsigned int relax_attempt = 0;
    struct w4_db_cluster_payload_compact_summary local_summary;
    struct w4_db_cluster_payload_header local_seen;

    if (!slot || !summary) {
        return false;
    }
    deadline = obmm_now_ms() + timeout_ms;
    while (obmm_now_ms() < deadline) {
        memset(&local_summary, 0, sizeof(local_summary));
        memset(&local_seen, 0, sizeof(local_seen));
        if (w4_db_try_read_stable_compact_summary_region(slot, &local_summary, &local_seen)) {
            if (seen_out) {
                *seen_out = local_seen;
            }
            if (local_seen.publish_done_seq >= min_publish_done_seq) {
                *summary = local_summary;
                return true;
            }
        } else if (seen_out) {
            *seen_out = local_seen;
        }
        w4_db_cpu_relax_wait(&relax_attempt);
    }
    return false;
}

static bool w4_db_read_stable_payload_region(const struct w4_db_cluster_slot *slot,
                                             struct w4_db_cluster_payload *snapshot,
                                             struct w4_db_cluster_payload_header *seen_out)
{
    int attempts = 8;
    unsigned int relax_attempt = 0;

    while (attempts-- > 0) {
        if (w4_db_try_read_stable_payload_region(slot, snapshot, seen_out)) {
            return true;
        }
        w4_db_cpu_relax_wait(&relax_attempt);
    }
    return false;
}

static bool W4_DB_MAYBE_UNUSED w4_db_wait_stable_payload_region_at_least(
    const struct w4_db_cluster_slot *slot,
    uint32_t min_publish_done_seq,
    long timeout_ms,
    struct w4_db_cluster_payload *snapshot,
    struct w4_db_cluster_payload_header *seen_out)
{
    long deadline;
    unsigned int relax_attempt = 0;
    struct w4_db_cluster_payload local_snapshot;
    struct w4_db_cluster_payload_header local_seen;

    if (!slot || !snapshot) {
        return false;
    }
    deadline = obmm_now_ms() + timeout_ms;
    while (obmm_now_ms() < deadline) {
        memset(&local_snapshot, 0, sizeof(local_snapshot));
        memset(&local_seen, 0, sizeof(local_seen));
        if (w4_db_try_read_stable_payload_region(slot, &local_snapshot, &local_seen)) {
            if (seen_out) {
                *seen_out = local_seen;
            }
            if (local_snapshot.publish_done_seq >= min_publish_done_seq) {
                *snapshot = local_snapshot;
                return true;
            }
        } else if (seen_out) {
            *seen_out = local_seen;
        }
        w4_db_cpu_relax_wait(&relax_attempt);
    }
    return false;
}

static bool W4_DB_MAYBE_UNUSED w4_db_payload_find_record(
    const struct w4_db_cluster_payload *payload,
    const char *key,
    struct w4_db_record *resolved_out)
{
    struct w4_db_cluster_payload snapshot;
    uint16_t i;

    if (!payload || !key || !resolved_out) {
        return false;
    }
    if (!w4_db_read_stable_payload(payload, &snapshot)) {
        return false;
    }
    for (i = 0; i < snapshot.record_count; ++i) {
        if (!snapshot.records[i].in_use) {
            continue;
        }
        if (strncmp(snapshot.records[i].key, key, sizeof(snapshot.records[i].key)) == 0) {
            *resolved_out = snapshot.records[i];
            return true;
        }
    }
    return false;
}

static bool W4_DB_MAYBE_UNUSED w4_db_payload_snapshot_find_record(
    const struct w4_db_cluster_payload *snapshot,
    const char *key,
    struct w4_db_record *resolved_out)
{
    uint16_t i;

    if (!snapshot || !key || !resolved_out) {
        return false;
    }
    for (i = 0; i < snapshot->record_count; ++i) {
        if (!snapshot->records[i].in_use) {
            continue;
        }
        if (strncmp(snapshot->records[i].key, key, sizeof(snapshot->records[i].key)) == 0) {
            *resolved_out = snapshot->records[i];
            return true;
        }
    }
    return false;
}

static bool w4_db_slot_find_record(const struct w4_db_cluster_slot *slot,
                                   const char *key,
                                   struct w4_db_record *resolved_out)
{
    struct w4_db_cluster_payload_header header;
    struct w4_db_cluster_payload_header confirm;
    const volatile uint8_t *mapped_bytes;
    uint16_t i;

    if (!slot || !key || !resolved_out) {
        return false;
    }
    if (slot->is_local) {
        struct w4_db_cluster_payload snapshot;

        if (!w4_db_read_stable_payload_region(slot, &snapshot, NULL)) {
            return false;
        }
        for (i = 0; i < snapshot.record_count; ++i) {
            if (!snapshot.records[i].in_use) {
                continue;
            }
            if (strncmp(snapshot.records[i].key, key, sizeof(snapshot.records[i].key)) == 0) {
                *resolved_out = snapshot.records[i];
                return true;
            }
        }
        return false;
    }
    if (!slot->region.addr || slot->region.fd < 0) {
        return false;
    }
    mapped_bytes = (const volatile uint8_t *)slot->region.addr;
    w4_db_copy_from_mapped_volatile(&header, mapped_bytes, sizeof(header));
    if (header.publish_seq == 0 ||
        header.publish_seq != header.publish_done_seq ||
        header.magic != W4_DB_CLUSTER_PAYLOAD_MAGIC ||
        header.version != W4_DB_CLUSTER_PAYLOAD_VERSION ||
        header.record_count == 0 ||
        header.record_count > W4_DB_CLUSTER_MAX_RECORDS) {
        return false;
    }
    for (i = 0; i < header.record_count; ++i) {
        bool in_use = false;
        enum w4_db_record_kind kind = 0;
        char record_key[sizeof(resolved_out->key)];
        size_t record_off = offsetof(struct w4_db_cluster_payload, records) +
                            ((size_t)i * sizeof(struct w4_db_record));

        memset(record_key, 0, sizeof(record_key));
        w4_db_copy_from_mapped_volatile(&in_use,
                                        mapped_bytes + record_off +
                                            offsetof(struct w4_db_record, in_use),
                                        sizeof(in_use));
        if (!in_use) {
            continue;
        }
        w4_db_copy_from_mapped_volatile(&kind,
                                        mapped_bytes + record_off +
                                            offsetof(struct w4_db_record, kind),
                                        sizeof(kind));
        if (kind < W4_DB_RECORD_PREFIX_GROUP ||
            kind > W4_DB_RECORD_QWEN3_ENGRAM_STATE) {
            return false;
        }
        w4_db_copy_from_mapped_volatile(record_key,
                                        mapped_bytes + record_off +
                                            offsetof(struct w4_db_record, key),
                                        sizeof(record_key));
        if (strncmp(record_key, key, sizeof(record_key)) == 0) {
            w4_db_copy_from_mapped_volatile(resolved_out,
                                            mapped_bytes + record_off,
                                            sizeof(*resolved_out));
            __sync_synchronize();
            w4_db_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
            if (confirm.publish_seq != header.publish_seq ||
                confirm.publish_done_seq != header.publish_done_seq ||
                confirm.magic != header.magic ||
                confirm.version != header.version ||
                confirm.record_count != header.record_count) {
                return false;
            }
            return true;
        }
    }
    __sync_synchronize();
    w4_db_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
    if (confirm.publish_seq != header.publish_seq ||
        confirm.publish_done_seq != header.publish_done_seq ||
        confirm.magic != header.magic ||
        confirm.version != header.version ||
        confirm.record_count != header.record_count) {
        return false;
    }
    return false;
}

static int w4_db_read_primary_cna(uint32_t *local_cna_out)
{
    uint64_t local_cna_u64 = 0;

    if (!local_cna_out) {
        return -1;
    }
    if (!w4_db_parse_hex_file_u64("/sys/bus/ub/devices/00001/primary_cna", &local_cna_u64)) {
        return -1;
    }
    *local_cna_out = (uint32_t)local_cna_u64;
    return 0;
}

static int w4_db_exchange_cluster_meta(struct w4_db_cluster_runtime *rt,
                                       const struct w4_db_cluster_meta *local_meta)
{
    struct obmm_helpers_meta publish_meta;
    struct obmm_helpers_meta peer_metas[OBMM_POOL_HELPERS_MAX_NODES];
    bool got[OBMM_POOL_HELPERS_MAX_NODES];
    int i;

    memset(&publish_meta, 0, sizeof(publish_meta));
    publish_meta.export_mem_id = local_meta->export_mem_id;
    publish_meta.remote_uba = local_meta->remote_uba;
    publish_meta.size = local_meta->size;
    publish_meta.token_id = local_meta->token_id;
    publish_meta.export_cna = local_meta->export_cna;

    memset(got, 0, sizeof(got));

    if (obmm_bootstrap_publish(rt->obmm_fd, rt->local_idx, rt->node_count,
                               1, &publish_meta) != 0) {
        fprintf(stderr, "[w4_db] FM bootstrap publish failed: %s\n", strerror(errno));
        return -1;
    }

    if (obmm_bootstrap_lookup(rt->obmm_fd, rt->local_cna, rt->node_count,
                              1, peer_metas, got) != 0) {
        fprintf(stderr, "[w4_db] FM bootstrap lookup failed: %s\n", strerror(errno));
        return -1;
    }

    for (i = 0; i < rt->node_count; i++) {
        if (i == rt->local_idx) continue;
        rt->metas[i].export_mem_id = peer_metas[i].export_mem_id;
        rt->metas[i].remote_uba = peer_metas[i].remote_uba;
        rt->metas[i].size = peer_metas[i].size;
        rt->metas[i].token_id = peer_metas[i].token_id;
        rt->metas[i].export_cna = peer_metas[i].export_cna;
    }
    return 0;
}

static int w4_db_init_export_layout(struct w4_db_cluster_runtime *rt, void *base)
{
    int peer_count = rt->node_count - 1;
    uint64_t queue_size = obmm_queue_region_size(W4_DB_CLUSTER_QUEUE_DEPTH);
    uint64_t header_offset = 0;
    uint64_t dir_offset = 64;
    uint64_t dir_count = peer_count + 1;
    uint64_t queue_base = obmm_align_up_u64(dir_offset + dir_count * 32, 64);
    uint64_t payload_offset = obmm_align_up_u64(queue_base + (uint64_t)peer_count * queue_size, 64);
    struct obmm_pool_header *hdr;
    int i, peer_idx;

    hdr = (struct obmm_pool_header *)base;
    memset(hdr, 0, 64);
    hdr->magic = OBMM_POOL_MAGIC;
    hdr->layout_version = OBMM_POOL_LAYOUT_VERSION;
    hdr->node_id = (uint16_t)rt->local_idx;
    hdr->node_count = (uint16_t)rt->node_count;
    atomic_store(&hdr->state, OBMM_POOL_STATE_INIT);
    hdr->region_size = rt->region_size;
    hdr->directory_offset = dir_offset;
    hdr->directory_count = (uint32_t)dir_count;
    hdr->default_queue_depth = W4_DB_CLUSTER_QUEUE_DEPTH;

    peer_idx = 0;
    for (i = 0; i < rt->node_count; i++) {
        struct obmm_region_dirent *de;
        if (i == rt->local_idx) continue;
        de = (struct obmm_region_dirent *)((uint8_t *)base + dir_offset) + peer_idx;
        memset(de, 0, 32);
        de->region_id = (uint32_t)peer_idx;
        de->kind = OBMM_REGION_QUEUE;
        de->peer_node_id = (uint16_t)i;
        de->offset = queue_base + (uint64_t)peer_idx * queue_size;
        de->size = queue_size;

        rt->ingress_queues[i] = (struct obmm_spsc_queue *)((uint8_t *)base + de->offset);
        obmm_spsc_queue_init(rt->ingress_queues[i], W4_DB_CLUSTER_QUEUE_DEPTH);

        peer_idx++;
    }

    {
        struct obmm_region_dirent *de;
        de = (struct obmm_region_dirent *)((uint8_t *)base + dir_offset) + peer_idx;
        memset(de, 0, 32);
        de->region_id = (uint32_t)peer_idx;
        de->kind = OBMM_REGION_W4_PAYLOAD;
        de->peer_node_id = (uint16_t)rt->local_idx;
        de->offset = payload_offset;
        de->size = rt->region_size - payload_offset;
    }

    rt->ingress_queue_base = base;
    atomic_store(&hdr->state, OBMM_POOL_STATE_READY);
    fprintf(stderr, "[w4_db] export layout -> ok queues=%d queue_depth=%d payload_offset=%luKB\n",
            peer_count, W4_DB_CLUSTER_QUEUE_DEPTH, (unsigned long)(payload_offset / 1024));
    (void)header_offset;
    return 0;
}

static bool w4_db_desc_matches_barrier(const struct obmm_desc *desc,
                                       uint16_t desc_type,
                                       uint16_t epoch)
{
    return desc && desc->type == desc_type && (uint16_t)desc->cookie == epoch;
}

static bool w4_db_take_pending_barrier_desc(struct w4_db_cluster_runtime *rt,
                                            int owner_idx,
                                            uint16_t desc_type,
                                            uint16_t epoch)
{
    uint8_t count;
    uint8_t i;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    count = rt->pending_desc_count[owner_idx];
    for (i = 0; i < count; ++i) {
        if (w4_db_desc_matches_barrier(&rt->pending_descs[owner_idx][i],
                                       desc_type,
                                       epoch)) {
            if (i + 1 < count) {
                memmove(&rt->pending_descs[owner_idx][i],
                        &rt->pending_descs[owner_idx][i + 1],
                        (size_t)(count - i - 1) * sizeof(struct obmm_desc));
            }
            rt->pending_desc_count[owner_idx] = (uint8_t)(count - 1);
            return true;
        }
    }
    return false;
}

static bool w4_db_pending_object_desc_matches(const struct obmm_desc *desc,
                                              uint16_t epoch,
                                              const struct w4_db_record *record,
                                              uint32_t kind)
{
    uint32_t checksum_cookie;

    if (!desc || !record || desc->type != OBMM_DESC_W4_OBJECT_PUT ||
        (uint16_t)(desc->seq >> 48) != epoch || desc->flags != kind ||
        desc->payload_offset != record->object_backing_offset ||
        desc->payload_len != record->object_backing_len) {
        return false;
    }
    checksum_cookie = (uint32_t)(record->object_payload_checksum ^
                                 (record->object_payload_checksum >> 32));
    return desc->cookie == checksum_cookie;
}

static bool w4_db_runtime_range_input_desc_matches(const struct obmm_desc *desc,
                                                  uint16_t epoch)
{
    uint64_t decode_step = epoch > 0 ? (uint64_t)epoch - 1ULL : 0ULL;
    uint64_t expected_len = w4_db_qwen3_handoff_hidden_bytes(decode_step);

    if (!desc || desc->type != OBMM_DESC_W4_OBJECT_PUT ||
        desc->flags != W4_DB_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT ||
        desc->payload_len != expected_len) {
        return false;
    }
    return (uint16_t)(desc->seq >> 48) == epoch;
}

static bool w4_db_take_pending_runtime_range_input_desc(
    struct w4_db_cluster_runtime *rt,
    int owner_idx,
    uint16_t epoch,
    struct obmm_desc *desc_out)
{
    uint8_t count;
    uint8_t i;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    count = rt->pending_desc_count[owner_idx];
    for (i = 0; i < count; ++i) {
        if (w4_db_runtime_range_input_desc_matches(&rt->pending_descs[owner_idx][i],
                                                   epoch)) {
            if (desc_out) {
                *desc_out = rt->pending_descs[owner_idx][i];
            }
            if (i + 1 < count) {
                memmove(&rt->pending_descs[owner_idx][i],
                        &rt->pending_descs[owner_idx][i + 1],
                        (size_t)(count - i - 1) * sizeof(struct obmm_desc));
            }
            rt->pending_desc_count[owner_idx] = (uint8_t)(count - 1);
            return true;
        }
    }
    return false;
}

static bool w4_db_take_pending_object_desc(struct w4_db_cluster_runtime *rt,
                                           int owner_idx,
                                           uint16_t epoch,
                                           const struct w4_db_record *record,
                                           uint32_t kind)
{
    uint8_t count;
    uint8_t i;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    count = rt->pending_desc_count[owner_idx];
    for (i = 0; i < count; ++i) {
        if (w4_db_pending_object_desc_matches(&rt->pending_descs[owner_idx][i],
                                              epoch,
                                              record,
                                              kind)) {
            if (i + 1 < count) {
                memmove(&rt->pending_descs[owner_idx][i],
                        &rt->pending_descs[owner_idx][i + 1],
                        (size_t)(count - i - 1) * sizeof(struct obmm_desc));
            }
            rt->pending_desc_count[owner_idx] = (uint8_t)(count - 1);
            return true;
        }
    }
    return false;
}

static void w4_db_stash_pending_desc(struct w4_db_cluster_runtime *rt,
                                     int owner_idx,
                                     const struct obmm_desc *desc)
{
    uint8_t count;

    if (!rt || !desc || owner_idx < 0 || owner_idx >= rt->node_count) {
        return;
    }
    count = rt->pending_desc_count[owner_idx];
    if (count >= W4_DB_CLUSTER_PENDING_DESC_DEPTH) {
        fprintf(stderr,
                "[w4_db] pending desc overflow owner=%d type=%u cookie=0x%x\n",
                owner_idx,
                desc->type,
                desc->cookie);
        return;
    }
    rt->pending_descs[owner_idx][count] = *desc;
    rt->pending_desc_count[owner_idx] = (uint8_t)(count + 1);
}

static int w4_db_queue_barrier(struct w4_db_cluster_runtime *rt,
                               uint16_t desc_type,
                               uint16_t epoch,
                               uint32_t publish_seq)
{
    long deadline = obmm_now_ms() + W4_DB_CLUSTER_WAIT_MS;
    bool got[W4_DB_CLUSTER_MAX_NODES];
    struct obmm_desc desc;
    int i;

    memset(got, 0, sizeof(got));
    got[rt->local_idx] = true;

    memset(&desc, 0, sizeof(desc));
    desc.type = desc_type;
    desc.seq = (uint64_t)epoch | ((uint64_t)publish_seq << 16);
    desc.cookie = (uint32_t)epoch;

    for (i = 0; i < rt->node_count; i++) {
        if (i == rt->local_idx) continue;
        if (rt->egress_queues[i] == NULL) continue;
        while (obmm_spsc_push(rt->egress_queues[i], &desc) != 0) {
            if (obmm_now_ms() > deadline) {
                fprintf(stderr, "[w4_db] queue barrier push timeout type=%d peer=%d\n",
                        desc_type, i);
                return -1;
            }
            usleep(1000);
        }
    }

    while (obmm_now_ms() < deadline) {
        bool all = true;
        for (i = 0; i < rt->node_count; i++) {
            struct obmm_desc rx;
            if (got[i]) continue;
            if (rt->ingress_queues[i] == NULL) continue;
            if (w4_db_take_pending_barrier_desc(rt, i, desc_type, epoch)) {
                got[i] = true;
                continue;
            }
            while (obmm_spsc_pop(rt->ingress_queues[i], &rx) == 0) {
                if (w4_db_desc_matches_barrier(&rx, desc_type, epoch)) {
                    got[i] = true;
                } else {
                    w4_db_stash_pending_desc(rt, i, &rx);
                }
            }
            if (!got[i]) all = false;
        }
        if (all) return 0;
        usleep(1000);
    }

    fprintf(stderr, "[w4_db] queue barrier timeout type=%d missing:", desc_type);
    for (i = 0; i < rt->node_count; i++)
        if (!got[i]) fprintf(stderr, " %d", i);
    fprintf(stderr, "\n");
    return -1;
}

static int w4_db_push_obmm_object_descs(struct w4_db_cluster_runtime *rt,
                                        uint32_t payload_kind,
                                        uint64_t payload_offset,
                                        uint64_t payload_len,
                                        uint64_t checksum,
                                        uint16_t epoch)
{
    long deadline = obmm_now_ms() + W4_DB_CLUSTER_WAIT_MS;
    struct obmm_desc desc;
    int i;

    if (!rt || payload_len > UINT32_MAX || payload_kind > UINT16_MAX) {
        return -1;
    }

    memset(&desc, 0, sizeof(desc));
    desc.type = OBMM_DESC_W4_OBJECT_PUT;
    desc.flags = (uint16_t)payload_kind;
    desc.seq = ((uint64_t)epoch << 48) |
               ((uint64_t)(rt->local_idx + 1) << 32) |
               (payload_offset & 0xffffffffULL);
    desc.region_id = payload_kind;
    desc.payload_len = (uint32_t)payload_len;
    desc.payload_offset = payload_offset;
    desc.cookie = (uint32_t)(checksum ^ (checksum >> 32));

    for (i = 0; i < rt->node_count; i++) {
        if (i == rt->local_idx || rt->egress_queues[i] == NULL) {
            continue;
        }
        while (obmm_spsc_push(rt->egress_queues[i], &desc) != 0) {
            if (obmm_now_ms() > deadline) {
                fprintf(stderr,
                        "[w4_db] object desc push timeout kind=%u peer=%d offset=%#" PRIx64 "\n",
                        payload_kind, i + 1, payload_offset);
                return -1;
            }
            usleep(1000);
        }
    }

    return 0;
}

static int w4_db_push_obmm_object_desc_to(struct w4_db_cluster_runtime *rt,
                                          uint32_t target_node,
                                          uint32_t payload_kind,
                                          uint64_t payload_offset,
                                          uint64_t payload_len,
                                          uint64_t checksum,
                                          uint16_t epoch)
{
    long deadline = obmm_now_ms() + W4_DB_CLUSTER_WAIT_MS;
    struct obmm_desc desc;

    if (!rt || target_node >= (uint32_t)rt->node_count ||
        target_node == (uint32_t)rt->local_idx ||
        payload_len > UINT32_MAX || payload_kind > UINT16_MAX) {
        return -1;
    }
    if (!rt->egress_queues[target_node] &&
        w4_db_activate_remote_slot(rt, (int)target_node) != 0) {
        return -1;
    }
    if (!rt->egress_queues[target_node]) {
        return -1;
    }

    memset(&desc, 0, sizeof(desc));
    desc.type = OBMM_DESC_W4_OBJECT_PUT;
    desc.flags = (uint16_t)payload_kind;
    desc.seq = ((uint64_t)epoch << 48) |
               ((uint64_t)(rt->local_idx + 1) << 32) |
               (payload_offset & 0xffffffffULL);
    desc.region_id = payload_kind;
    desc.payload_len = (uint32_t)payload_len;
    desc.payload_offset = payload_offset;
    desc.cookie = (uint32_t)(checksum ^ (checksum >> 32));

    while (obmm_spsc_push(rt->egress_queues[target_node], &desc) != 0) {
        if (obmm_now_ms() > deadline) {
            fprintf(stderr,
                    "[w4_db] object desc unicast timeout kind=%u target=%u offset=%#" PRIx64 "\n",
                    payload_kind,
                    target_node + 1U,
                    payload_offset);
            return -1;
        }
        usleep(1000);
    }
    return 0;
}

static int w4_db_wait_remote_obmm_object_descs(struct w4_db_cluster_runtime *rt,
                                              uint32_t owner_node,
                                              uint16_t epoch,
                                              const struct w4_db_record *weight,
                                              const struct w4_db_record *kvcache,
                                              const struct w4_db_record *hidden_input,
                                              const struct w4_db_record *hidden_output)
{
    long deadline = obmm_now_ms() + W4_DB_OBMM_SERVICE_WAIT_MS;
    bool saw_weight = false;
    bool saw_kvcache = false;
    bool saw_hidden_input = false;
    bool saw_hidden_output = false;
    struct obmm_spsc_queue *q;

    if (!rt || owner_node >= (uint32_t)rt->node_count || !weight || !kvcache ||
        !hidden_input || !hidden_output) {
        return -1;
    }
    q = rt->ingress_queues[owner_node];
    if (!q) {
        return -1;
    }

    while (obmm_now_ms() < deadline) {
        struct obmm_desc desc;
        bool drained = false;

        if (!saw_weight &&
            w4_db_take_pending_object_desc(rt,
                                           (int)owner_node,
                                           epoch,
                                           weight,
                                           W4_DB_OBMM_KIND_WEIGHT_TILE)) {
            saw_weight = true;
        }
        if (!saw_kvcache &&
            w4_db_take_pending_object_desc(rt,
                                           (int)owner_node,
                                           epoch,
                                           kvcache,
                                           W4_DB_OBMM_KIND_KVCACHE_BLOCK)) {
            saw_kvcache = true;
        }
        if (!saw_hidden_input &&
            w4_db_take_pending_object_desc(rt,
                                           (int)owner_node,
                                           epoch,
                                           hidden_input,
                                           W4_DB_OBMM_KIND_HIDDEN_RANGE_INPUT)) {
            saw_hidden_input = true;
        }
        if (!saw_hidden_output &&
            w4_db_take_pending_object_desc(rt,
                                           (int)owner_node,
                                           epoch,
                                           hidden_output,
                                           W4_DB_OBMM_KIND_HIDDEN_RANGE_OUTPUT)) {
            saw_hidden_output = true;
        }
        while (obmm_spsc_pop(q, &desc) == 0) {
            bool matched = false;
            drained = true;
            if (desc.type != OBMM_DESC_W4_OBJECT_PUT ||
                (uint16_t)(desc.seq >> 48) != epoch) {
                w4_db_stash_pending_desc(rt, (int)owner_node, &desc);
                continue;
            }
            if (desc.flags == W4_DB_OBMM_KIND_WEIGHT_TILE &&
                desc.payload_offset == weight->object_backing_offset &&
                desc.payload_len == weight->object_backing_len &&
                desc.cookie == (uint32_t)(weight->object_payload_checksum ^
                                          (weight->object_payload_checksum >> 32))) {
                saw_weight = true;
                matched = true;
            }
            if (desc.flags == W4_DB_OBMM_KIND_KVCACHE_BLOCK &&
                desc.payload_offset == kvcache->object_backing_offset &&
                desc.payload_len == kvcache->object_backing_len &&
                desc.cookie == (uint32_t)(kvcache->object_payload_checksum ^
                                          (kvcache->object_payload_checksum >> 32))) {
                saw_kvcache = true;
                matched = true;
            }
            if (desc.flags == W4_DB_OBMM_KIND_HIDDEN_RANGE_INPUT &&
                desc.payload_offset == hidden_input->object_backing_offset &&
                desc.payload_len == hidden_input->object_backing_len &&
                desc.cookie == (uint32_t)(hidden_input->object_payload_checksum ^
                                          (hidden_input->object_payload_checksum >> 32))) {
                saw_hidden_input = true;
                matched = true;
            }
            if (desc.flags == W4_DB_OBMM_KIND_HIDDEN_RANGE_OUTPUT &&
                desc.payload_offset == hidden_output->object_backing_offset &&
                desc.payload_len == hidden_output->object_backing_len &&
                desc.cookie == (uint32_t)(hidden_output->object_payload_checksum ^
                                          (hidden_output->object_payload_checksum >> 32))) {
                saw_hidden_output = true;
                matched = true;
            }
            if (!matched) {
                w4_db_stash_pending_desc(rt, (int)owner_node, &desc);
            }
        }
        if (saw_weight && saw_kvcache && saw_hidden_input && saw_hidden_output) {
            return 0;
        }
        if (!drained) {
            usleep(1000);
        }
    }

    printf("[w4_guest] gap obmm_service_v0=object_desc_timeout remote=node%u weight=%u kvcache=%u hidden_input=%u hidden_output=%u epoch=%u\n",
           owner_node + 1U,
           saw_weight ? 1U : 0U,
           saw_kvcache ? 1U : 0U,
           saw_hidden_input ? 1U : 0U,
           saw_hidden_output ? 1U : 0U,
           epoch);
    return -1;
}

static void w4_db_cleanup_cluster_slots(struct w4_db_cluster_runtime *rt)
{
    int i;

    for (i = 0; i < rt->node_count; ++i) {
        /* Undo payload_offset adjustment for local and remote slots */
        if (rt->slots[i].region.addr && rt->payload_offset > 0 &&
            (rt->slots[i].is_local || rt->slots[i].mem_id != 0)) {
            rt->slots[i].region.addr =
                (uint8_t *)rt->slots[i].region.addr - rt->payload_offset;
            rt->slots[i].region.len = rt->region_size;
        }
        if (rt->slots[i].region.addr || rt->slots[i].region.fd >= 0) {
            obmm_unmap_region((struct obmm_helpers_region *)&rt->slots[i].region);
        }
        if (rt->slots[i].mem_id != 0) {
            if (i == rt->local_idx) {
                (void)obmm_do_unexport(rt->obmm_fd, rt->slots[i].mem_id);
            } else {
                (void)obmm_do_unimport(rt->obmm_fd, rt->slots[i].mem_id);
            }
        }
        if (rt->egress_import[i].addr || rt->egress_import[i].fd >= 0) {
            obmm_unmap_region(&rt->egress_import[i]);
        }
    }
}

static int w4_db_activate_remote_slot(struct w4_db_cluster_runtime *rt, int owner_idx)
{
    struct w4_db_cluster_slot *slot;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count || owner_idx == rt->local_idx) {
        return -1;
    }

    slot = &rt->slots[owner_idx];
    if (!slot->map_osync) {
        fprintf(stderr,
                "[w4_guest] invariant violation remote_slot_map_osync_true_expected node=%d map_osync=%d\n",
                owner_idx + 1,
                slot->map_osync ? 1 : 0);
        slot->map_osync = true;
    }
    if (slot->region.addr && slot->mem_id != 0) {
        return 0;
    }
    if (slot->mem_id != 0) {
        (void)obmm_do_unimport(rt->obmm_fd, slot->mem_id);
        slot->mem_id = 0;
    }
    if (slot->region.addr || slot->region.fd >= 0) {
        obmm_unmap_region((struct obmm_helpers_region *)&slot->region);
    }
    {
        struct obmm_helpers_meta import_meta;
        import_meta.export_mem_id = rt->metas[owner_idx].export_mem_id;
        import_meta.remote_uba = rt->metas[owner_idx].remote_uba;
        import_meta.size = rt->metas[owner_idx].size;
        import_meta.token_id = rt->metas[owner_idx].token_id;
        import_meta.export_cna = rt->metas[owner_idx].export_cna;
        if (obmm_do_import(rt->obmm_fd, &import_meta,
                           rt->local_cna, slot->local_pa,
                           import_meta.token_id, &slot->mem_id) != 0) {
            return -1;
        }
    }
    if (obmm_map_region(slot->mem_id,
                        rt->region_size,
                        slot->map_osync,
                        (struct obmm_helpers_region *)&slot->region) != 0) {
        (void)obmm_do_unimport(rt->obmm_fd, slot->mem_id);
        slot->mem_id = 0;
        return -1;
    }

    /* Poll peer's pool state until READY -- ensures cacheable writes by the
     * exporter are visible through our osync import mapping before we read
     * the directory and queue structures. */
    {
        struct obmm_pool_header *phdr =
            (struct obmm_pool_header *)slot->region.addr;
        long poll_deadline = obmm_now_ms() + 90000;
        while (obmm_now_ms() < poll_deadline) {
            uint32_t st = atomic_load_explicit(&phdr->state,
                                               memory_order_acquire);
            if (st == OBMM_POOL_STATE_READY)
                break;
            usleep(1000);
        }
        if (atomic_load_explicit(
                &((struct obmm_pool_header *)slot->region.addr)->state,
                memory_order_acquire) != OBMM_POOL_STATE_READY) {
            fprintf(stderr, "[w4_db] peer node%d pool not READY\n",
                    owner_idx + 1);
            obmm_unmap_region((struct obmm_helpers_region *)&slot->region);
            (void)obmm_do_unimport(rt->obmm_fd, slot->mem_id);
            slot->mem_id = 0;
            return -1;
        }
    }

    /* Resolve egress queue (remote node's ingress queue for us) from directory */
    if (rt->egress_queues[owner_idx] == NULL && slot->region.addr != NULL) {
        struct obmm_pool_header *hdr = (struct obmm_pool_header *)slot->region.addr;
        struct obmm_region_dirent *dir = (struct obmm_region_dirent *)
            ((uint8_t *)slot->region.addr + hdr->directory_offset);
        int d;
        for (d = 0; (uint32_t)d < hdr->directory_count; d++) {
            if (dir[d].kind == OBMM_REGION_QUEUE &&
                dir[d].peer_node_id == (uint16_t)rt->local_idx) {
                rt->egress_queues[owner_idx] = (struct obmm_spsc_queue *)
                    ((uint8_t *)slot->region.addr + dir[d].offset);
                break;
            }
        }
    }

    /* Adjust slot's region.addr to point at the payload sub-region */
    if (rt->payload_offset > 0 && slot->region.addr != NULL) {
        slot->region.addr = (uint8_t *)slot->region.addr + rt->payload_offset;
        slot->region.len = rt->region_size - rt->payload_offset;
    }

    return 0;
}

static void w4_db_cluster_runtime_reset(struct w4_db_cluster_runtime *rt)
{
    if (!rt) {
        return;
    }
    if (rt->obmm_fd >= 0) {
        w4_db_cleanup_cluster_slots(rt);
        close(rt->obmm_fd);
    }
    memset(rt, 0, sizeof(*rt));
    rt->obmm_fd = -1;
    rt->local_idx = -1;
    rt->payload_arena_base = 0;
    rt->payload_arena_next = 0;
    rt->payload_arena_high_water = 0;
    rt->pool_layout_reported = false;
}

static int w4_db_cluster_runtime_init(struct w4_db_cluster_runtime *rt)
{
    char local_ip[INET_ADDRSTRLEN];
    char ips[W4_DB_CLUSTER_MAX_NODES][INET_ADDRSTRLEN];
    struct w4_db_cluster_meta local_meta;
    struct obmm_helpers_meta export_meta;
    uint64_t import_pas[W4_DB_CLUSTER_MAX_NODES];
    bool import_osync[W4_DB_CLUSTER_MAX_NODES];
    int import_count;
    int import_idx;
    char region_size_str[32];
    uint64_t region_size_mb;
    uint64_t payload_offset;
    int i;

    if (!rt) {
        return -1;
    }
    if (rt->active) {
        return 0;
    }
    w4_db_cluster_runtime_reset(rt);
    rt->lazy_remote_activation =
        getenv("SIM_W4_DB_LAZY_REMOTE_ACTIVATION") != NULL &&
        strcmp(getenv("SIM_W4_DB_LAZY_REMOTE_ACTIVATION"), "1") == 0;
    memset(&local_meta, 0, sizeof(local_meta));

    if (!w4_db_resolve_cluster_nodes(local_ip, ips, &rt->node_count, &rt->local_idx)) {
        return -1;
    }

    /* Read region size from /proc/cmdline, default to W4_DB_DEFAULT_REGION_SIZE_MB */
    if (obmm_cmdline_get(W4_DB_CMDLINE_REGION_SIZE, region_size_str, sizeof(region_size_str))) {
        errno = 0;
        region_size_mb = (uint64_t)strtoull(region_size_str, NULL, 0);
        if (errno != 0 || region_size_mb == 0) {
            region_size_mb = W4_DB_DEFAULT_REGION_SIZE_MB;
        }
    } else {
        region_size_mb = W4_DB_DEFAULT_REGION_SIZE_MB;
    }
    rt->region_size = obmm_align_up_u64(region_size_mb * 1024ULL * 1024ULL,
                                         OBMM_POOL_HELPERS_IMPORT_ALIGN);
    fprintf(stderr, "[w4_db] region_size=%luMB (aligned=%luMB)\n",
            (unsigned long)region_size_mb,
            (unsigned long)(rt->region_size / (1024ULL * 1024ULL)));

    rt->obmm_fd = obmm_open_device();
    if (rt->obmm_fd < 0) {
        goto fail;
    }
    if (w4_db_read_primary_cna(&rt->local_cna) != 0) {
        goto fail;
    }
    local_meta.export_cna = rt->local_cna;

    /* Export local region */
    memset(&export_meta, 0, sizeof(export_meta));
    export_meta.export_cna = rt->local_cna;
    if (obmm_do_export(rt->obmm_fd, &export_meta, rt->region_size) != 0) {
        goto fail;
    }
    local_meta.export_mem_id = export_meta.export_mem_id;
    local_meta.remote_uba = export_meta.remote_uba;
    local_meta.size = export_meta.size;
    local_meta.token_id = export_meta.token_id;
    local_meta.export_cna = export_meta.export_cna;

    rt->slots[rt->local_idx].owner_idx = rt->local_idx;
    rt->slots[rt->local_idx].is_local = true;
    rt->slots[rt->local_idx].mem_id = local_meta.export_mem_id;
    rt->slots[rt->local_idx].export_cna = rt->local_cna;
    if (obmm_map_region(local_meta.export_mem_id,
                        rt->region_size,
                        false,
                        (struct obmm_helpers_region *)&rt->slots[rt->local_idx].region) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=map_local_failed mem_id=%" PRIu64 "\n",
               local_meta.export_mem_id);
        goto fail;
    }

    /* Initialize export layout with queues and payload region */
    if (w4_db_init_export_layout(rt, rt->slots[rt->local_idx].region.addr) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=export_layout_failed\n");
        goto fail;
    }

    /* Find payload offset from directory */
    payload_offset = 0;
    {
        struct obmm_pool_header *hdr = (struct obmm_pool_header *)rt->slots[rt->local_idx].region.addr;
        struct obmm_region_dirent *dir = (struct obmm_region_dirent *)
            ((uint8_t *)rt->slots[rt->local_idx].region.addr + hdr->directory_offset);
        for (i = 0; (uint32_t)i < hdr->directory_count; i++) {
            if (dir[i].kind == OBMM_REGION_W4_PAYLOAD) {
                payload_offset = dir[i].offset;
                break;
            }
        }
    }
    if (payload_offset == 0) {
        printf("[w4_guest] gap db_service_cluster_stage=no_payload_entry\n");
        goto fail;
    }

    if (w4_db_update_region_range_at(&rt->slots[rt->local_idx],
                                     0,
                                     payload_offset,
                                     true) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=publish_pool_layout_failed\n");
        goto fail;
    }
    (void)msync(rt->slots[rt->local_idx].region.addr,
                (size_t)payload_offset,
                MS_SYNC);

    /* Adjust local slot's region.addr to point at the payload sub-region */
    rt->payload_offset = payload_offset;
    rt->slots[rt->local_idx].region.addr =
        (uint8_t *)rt->slots[rt->local_idx].region.addr + payload_offset;
    rt->slots[rt->local_idx].region.len = rt->region_size - payload_offset;
    rt->payload_arena_base =
        obmm_align_up_u64(W4_DB_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET, 64);
    rt->payload_arena_next = rt->payload_arena_base;
    rt->payload_arena_high_water = rt->payload_arena_base;
    w4_db_report_obmm_pool_layout_once(rt);

    /* FM bootstrap for peer discovery */
    if (w4_db_exchange_cluster_meta(rt, &local_meta) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=hello_timeout\n");
        goto fail;
    }

    /* Allocate import PAs for peer regions */
    import_count = rt->node_count - 1;
    if (!obmm_alloc_import_pas(import_count, rt->region_size, import_pas, import_osync,
                               obmm_parse_import_cache_mode())) {
        printf("[w4_guest] gap db_service_cluster_stage=import_alloc_failed count=%d\n",
               import_count);
        goto fail;
    }

    import_idx = 0;
    for (i = 0; i < rt->node_count; ++i) {
        if (i == rt->local_idx) {
            continue;
        }
        rt->slots[i].owner_idx = i;
        rt->slots[i].is_local = false;
        rt->slots[i].local_pa = import_pas[import_idx];
        rt->slots[i].map_osync = true;
        fprintf(stderr,
                "[w4_guest] remote_slot_map_osync_forced node=%d map_osync=%d\n",
                i + 1,
                rt->slots[i].map_osync ? 1 : 0);
        rt->slots[i].export_cna = rt->metas[i].export_cna;
        import_idx += 1;
        rt->slots[i].mem_id = 0;
        memset(&rt->slots[i].region, 0, sizeof(rt->slots[i].region));
        rt->slots[i].region.fd = -1;

        if (!rt->lazy_remote_activation) {
            /* Import, map, and resolve egress queue for this peer now so that
             * SPSC queue barriers can push descriptors immediately. */
            if (w4_db_activate_remote_slot(rt, i) != 0) {
                printf("[w4_guest] gap db_service_cluster_stage=activate_remote_failed owner=node%d\n",
                       i + 1);
                goto fail;
            }
        }
    }

    rt->active = true;
    if (rt->lazy_remote_activation) {
        printf("[w4_guest] stage db_service_cluster=local_pool_ready node=%d peers=%d activation=lazy backing=obmm_pool queue=obmm_spsc status=ok\n",
               rt->local_idx + 1,
               rt->node_count - 1);
    }
    return 0;

fail:
    w4_db_cluster_runtime_reset(rt);
    return -1;
}

static struct w4_db_record *w4_db_alloc_record(struct w4_db_service *svc)
{
    size_t i;

    if (!svc) {
        return NULL;
    }
    for (i = 0; i < W4_DB_MAX_RECORDS; ++i) {
        if (!svc->records[i].in_use) {
            svc->records[i].in_use = true;
            svc->record_count += 1;
            return &svc->records[i];
        }
    }
    return NULL;
}

static struct w4_db_record *w4_db_find_record(struct w4_db_service *svc, const char *key)
{
    size_t i;

    if (!svc || !key) {
        return NULL;
    }
    for (i = 0; i < W4_DB_MAX_RECORDS; ++i) {
        if (!svc->records[i].in_use) {
            continue;
        }
        if (strncmp(svc->records[i].key, key, sizeof(svc->records[i].key)) == 0) {
            return &svc->records[i];
        }
    }
    return NULL;
}

static bool w4_db_qwen3_record_kind_recyclable(enum w4_db_record_kind kind)
{
    switch (kind) {
    case W4_DB_RECORD_HIDDEN_RANGE_INPUT:
    case W4_DB_RECORD_HIDDEN_RANGE_OUTPUT:
    case W4_DB_RECORD_KVCACHE_OBJECT:
    case W4_DB_RECORD_QWEN3_TOKEN_RESULT:
    case W4_DB_RECORD_QWEN3_ENGRAM_CANDIDATES:
    case W4_DB_RECORD_QWEN3_ENGRAM_SELECTED:
    case W4_DB_RECORD_QWEN3_ENGRAM_STATE:
        return true;
    default:
        return false;
    }
}

static bool w4_db_key_decode_step(const char *key, uint64_t *step_out)
{
    const char *needle;
    char *end = NULL;
    unsigned long long parsed;

    if (!key || !step_out) {
        return false;
    }
    needle = strstr(key, "decode-step");
    if (needle) {
        needle += strlen("decode-step");
    } else {
        needle = strstr(key, "/step/");
        if (!needle) {
            return false;
        }
        needle += strlen("/step/");
    }
    if (*needle < '0' || *needle > '9') {
        return false;
    }
    errno = 0;
    parsed = strtoull(needle, &end, 10);
    if (errno != 0 || end == needle) {
        return false;
    }
    *step_out = (uint64_t)parsed;
    return true;
}

static struct w4_db_record *w4_db_recycle_qwen3_runtime_record(
    struct w4_db_service *svc,
    const char *incoming_key)
{
    struct w4_db_record *candidate = NULL;
    uint64_t incoming_step;
    uint64_t candidate_step = UINT64_MAX;
    size_t i;

    if (!svc || !w4_db_key_decode_step(incoming_key, &incoming_step) ||
        incoming_step <= W4_DB_QWEN3_RECORD_RETAIN_STEPS) {
        return NULL;
    }
    for (i = 0; i < W4_DB_MAX_RECORDS; ++i) {
        struct w4_db_record *rec = &svc->records[i];
        uint64_t rec_step;

        if (!rec->in_use ||
            !w4_db_qwen3_record_kind_recyclable(rec->kind) ||
            !w4_db_key_decode_step(rec->key, &rec_step) ||
            rec_step + W4_DB_QWEN3_RECORD_RETAIN_STEPS >= incoming_step) {
            continue;
        }
        if (!candidate || rec_step < candidate_step) {
            candidate = rec;
            candidate_step = rec_step;
        }
    }
    if (candidate) {
        printf("[w4_guest] stage db_service_record_recycle key=%s old_step=%" PRIu64
               " incoming_step=%" PRIu64 " retain_steps=%" PRIu64
               " record_count=%zu status=ok\n",
               candidate->key,
               candidate_step,
               incoming_step,
               (uint64_t)W4_DB_QWEN3_RECORD_RETAIN_STEPS,
               svc->record_count);
        memset(candidate, 0, sizeof(*candidate));
    }
    return candidate;
}

static bool w4_db_record_has_member(const struct w4_db_record *rec, const char *block_hash)
{
    uint32_t i;
    if (!rec || !block_hash) {
        return false;
    }
    for (i = 0; i < rec->member_count && i < W4_DB_MAX_GROUP_MEMBERS; ++i) {
        if (strncmp(rec->member_block_hashes[i], block_hash, sizeof(rec->member_block_hashes[i])) == 0) {
            return true;
        }
    }
    return false;
}

bool w4_db_record_has_member_block(const struct w4_db_record *rec, const char *block_hash)
{
    return w4_db_record_has_member(rec, block_hash);
}

static int w4_db_add_member(struct w4_db_record *rec, const char *block_hash)
{
    if (!rec || !block_hash) {
        return -1;
    }
    if (w4_db_record_has_member(rec, block_hash)) {
        return 0;
    }
    if (rec->member_count >= W4_DB_MAX_GROUP_MEMBERS) {
        return -1;
    }
    snprintf(rec->member_block_hashes[rec->member_count], sizeof(rec->member_block_hashes[rec->member_count]), "%s", block_hash);
    rec->member_count += 1;
    return 0;
}

static int w4_db_build_two_part_key(const char *prefix,
                                    const char *first,
                                    const char *middle,
                                    const char *second,
                                    char *out,
                                    size_t out_len)
{
    size_t prefix_len;
    size_t first_len;
    size_t middle_len;
    size_t second_len;
    size_t total_len;
    char *cursor;

    if (!prefix || !first || !middle || !second || !out || out_len == 0) {
        return -1;
    }
    prefix_len = strlen(prefix);
    first_len = strlen(first);
    middle_len = strlen(middle);
    second_len = strlen(second);
    if (prefix_len > SIZE_MAX - first_len ||
        prefix_len + first_len > SIZE_MAX - middle_len ||
        prefix_len + first_len + middle_len > SIZE_MAX - second_len) {
        out[0] = '\0';
        return -1;
    }
    total_len = prefix_len + first_len + middle_len + second_len;
    if (total_len + 1U > out_len) {
        out[0] = '\0';
        return -1;
    }
    cursor = out;
    memcpy(cursor, prefix, prefix_len);
    cursor += prefix_len;
    memcpy(cursor, first, first_len);
    cursor += first_len;
    memcpy(cursor, middle, middle_len);
    cursor += middle_len;
    memcpy(cursor, second, second_len);
    cursor += second_len;
    *cursor = '\0';
    return 0;
}

static int w4_db_build_prefix_key_from_parts_checked(const char *request_id,
                                                     const char *prefix_group,
                                                     char *out,
                                                     size_t out_len)
{
    return w4_db_build_two_part_key("request/",
                                    request_id,
                                    "/prefix/",
                                    prefix_group,
                                    out,
                                    out_len);
}

static int w4_db_build_group_key_from_parts_checked(const char *request_id,
                                                    const char *group_id,
                                                    char *out,
                                                    size_t out_len)
{
    return w4_db_build_two_part_key("request/",
                                    request_id,
                                    "/prefix-group/",
                                    group_id,
                                    out,
                                    out_len);
}

static int w4_db_build_block_key_from_hash_checked(const char *block_hash,
                                                   char *out,
                                                   size_t out_len)
{
    return w4_db_build_two_part_key("block/",
                                    block_hash,
                                    "",
                                    "",
                                    out,
                                    out_len);
}

static int w4_db_build_group_key(const struct w4_db_block_ctx *ctx,
                                 char *out,
                                 size_t out_len)
{
    if (!ctx) {
        return -1;
    }
    return w4_db_build_group_key_from_parts_checked(ctx->request_id,
                                                    ctx->group_id,
                                                    out,
                                                    out_len);
}

void w4_db_build_prefix_key_from_parts(const char *request_id,
                                       const char *prefix_group,
                                       char *out,
                                       size_t out_len)
{
    (void)w4_db_build_prefix_key_from_parts_checked(request_id,
                                                   prefix_group,
                                                   out,
                                                   out_len);
}

void w4_db_build_group_key_from_parts(const char *request_id,
                                      const char *group_id,
                                      char *out,
                                      size_t out_len)
{
    (void)w4_db_build_group_key_from_parts_checked(request_id,
                                                  group_id,
                                                  out,
                                                  out_len);
}

void w4_db_build_block_key_from_hash(const char *block_hash, char *out, size_t out_len)
{
    (void)w4_db_build_block_key_from_hash_checked(block_hash, out, out_len);
}

static int w4_db_put_request_prefix(struct w4_db_service *svc,
                                    const char *key,
                                    const char *request_id,
                                    const char *prefix_group,
                                    const char *group_id,
                                    const char *block_hash)
{
    struct w4_db_record *rec;

    rec = w4_db_find_record(svc, key);
    if (!rec) {
        rec = w4_db_alloc_record(svc);
    }
    if (!rec) {
        return -1;
    }

    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = W4_DB_RECORD_REQUEST_PREFIX;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    snprintf(rec->request_id, sizeof(rec->request_id), "%s", request_id);
    snprintf(rec->prefix_group, sizeof(rec->prefix_group), "%s", prefix_group);
    snprintf(rec->group_id, sizeof(rec->group_id), "%s", group_id);
    snprintf(rec->block_hash, sizeof(rec->block_hash), "%s", block_hash);
    rec->version = 1;
    return 0;
}

static int w4_db_put_prefix_group(struct w4_db_service *svc,
                                  const char *key,
                                  const char *request_id,
                                  const char *group_id,
                                  const char *block_hash,
                                  uint32_t placement_node,
                                  uint32_t placement_level,
                                  uint64_t hot_segment_id,
                                  enum w4_kvcache_state state,
                                  uint64_t last_result_segment)
{
    struct w4_db_record *rec;
    bool is_new = false;
    bool changed = false;

    rec = w4_db_find_record(svc, key);
    if (!rec) {
        rec = w4_db_alloc_record(svc);
        if (!rec) {
            return -1;
        }
        memset(rec, 0, sizeof(*rec));
        rec->in_use = true;
        rec->kind = W4_DB_RECORD_PREFIX_GROUP;
        snprintf(rec->key, sizeof(rec->key), "%s", key);
        snprintf(rec->request_id, sizeof(rec->request_id), "%s", request_id);
        snprintf(rec->group_id, sizeof(rec->group_id), "%s", group_id);
        rec->version = 1;
        is_new = true;
        changed = true;
    }
    if (rec->kind != W4_DB_RECORD_PREFIX_GROUP) {
        return -1;
    }
    if (w4_db_add_member(rec, block_hash) != 0) {
        return -1;
    }
    if (rec->placement_node != placement_node ||
        rec->placement_level != placement_level ||
        rec->hot_segment_id != hot_segment_id ||
        rec->state != state ||
        rec->last_result_segment != last_result_segment) {
        rec->placement_node = placement_node;
        rec->placement_level = placement_level;
        rec->hot_segment_id = hot_segment_id;
        rec->state = state;
        rec->last_result_segment = last_result_segment;
        changed = true;
    }
    if (changed && !is_new && rec->version > 0) {
        rec->version += 1;
    }
    return 0;
}

static int w4_db_put_block_meta(struct w4_db_service *svc,
                                const char *key,
                                const char *request_id,
                                const char *prefix_group,
                                const char *group_id,
                                const char *block_hash,
                                uint32_t placement_node,
                                uint32_t placement_level,
                                uint64_t hot_segment_id,
                                enum w4_kvcache_state state)
{
    struct w4_db_record *rec;

    rec = w4_db_find_record(svc, key);
    if (!rec) {
        rec = w4_db_alloc_record(svc);
    }
    if (!rec) {
        return -1;
    }

    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = W4_DB_RECORD_BLOCK_META;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    snprintf(rec->request_id, sizeof(rec->request_id), "%s", request_id);
    snprintf(rec->prefix_group, sizeof(rec->prefix_group), "%s", prefix_group);
    snprintf(rec->group_id, sizeof(rec->group_id), "%s", group_id);
    snprintf(rec->block_hash, sizeof(rec->block_hash), "%s", block_hash);
    rec->placement_node = placement_node;
    rec->placement_level = placement_level;
    rec->hot_segment_id = hot_segment_id;
    rec->state = state;
    rec->version = 1;
    return 0;
}

static int w4_db_update_block_result(struct w4_db_service *svc,
                                     const char *key,
                                     uint64_t last_result_segment,
                                     enum w4_kvcache_state next_state)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || rec->kind != W4_DB_RECORD_BLOCK_META) {
        return -1;
    }
    if (rec->last_result_segment != 0 && last_result_segment <= rec->last_result_segment) {
        return 1;
    }
    rec->last_result_segment = last_result_segment;
    rec->state = next_state;
    rec->version += 1;
    return 0;
}

static int w4_db_update_prefix_result(struct w4_db_service *svc,
                                      const char *key,
                                      const struct w4_db_block_ctx *ctx,
                                      const struct w4_db_record *block_record)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || rec->kind != W4_DB_RECORD_REQUEST_PREFIX) {
        return -1;
    }
    if (rec->last_result_segment != 0 &&
        strncmp(rec->block_hash, block_record->block_hash, sizeof(rec->block_hash)) == 0 &&
        block_record->last_result_segment < rec->last_result_segment) {
        return 1;
    }
    if (rec->last_result_segment == block_record->last_result_segment &&
        rec->placement_node == ctx->placement_node &&
        rec->placement_level == ctx->placement_level &&
        rec->hot_segment_id == block_record->hot_segment_id &&
        rec->state == block_record->state &&
        strncmp(rec->block_hash, block_record->block_hash, sizeof(rec->block_hash)) == 0) {
        return 1;
    }
    rec->placement_node = block_record->placement_node;
    rec->placement_level = block_record->placement_level;
    rec->hot_segment_id = block_record->hot_segment_id;
    rec->state = block_record->state;
    rec->last_result_segment = block_record->last_result_segment;
    snprintf(rec->block_hash, sizeof(rec->block_hash), "%s", block_record->block_hash);
    rec->version += 1;
    return 0;
}

static int w4_db_update_prefix_group_from_block(struct w4_db_service *svc,
                                                const struct w4_db_block_ctx *ctx,
                                                const struct w4_db_record *block_record)
{
    char group_key[96];

    if (!svc || !ctx || !block_record) {
        return -1;
    }
    if (w4_db_build_group_key(ctx, group_key, sizeof(group_key)) != 0) {
        return -1;
    }
    return w4_db_put_prefix_group(svc,
                                  group_key,
                                  ctx->request_id,
                                  ctx->group_id,
                                  block_record->block_hash,
                                  block_record->placement_node,
                                  block_record->placement_level,
                                  block_record->hot_segment_id,
                                  block_record->state,
                                  block_record->last_result_segment);
}

static int w4_db_update_block_view(struct w4_db_service *svc,
                                   const char *key,
                                   uint64_t hot_segment_id,
                                   uint32_t placement_level)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || rec->kind != W4_DB_RECORD_BLOCK_META) {
        return -1;
    }
    if (rec->hot_segment_id == hot_segment_id && rec->placement_level == placement_level) {
        return 1;
    }
    rec->hot_segment_id = hot_segment_id;
    rec->placement_level = placement_level;
    rec->version += 1;
    return 0;
}

static int w4_db_update_block_owner(struct w4_db_service *svc,
                                    const char *key,
                                    uint32_t placement_node,
                                    uint32_t placement_level,
                                    uint64_t hot_segment_id)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || rec->kind != W4_DB_RECORD_BLOCK_META) {
        return -1;
    }
    if (rec->placement_node == placement_node &&
        rec->placement_level == placement_level &&
        rec->hot_segment_id == hot_segment_id) {
        return 1;
    }
    rec->placement_node = placement_node;
    rec->placement_level = placement_level;
    rec->hot_segment_id = hot_segment_id;
    rec->version += 1;
    return 0;
}

static int w4_db_build_prefix_key(const struct w4_db_block_ctx *ctx,
                                  char *out,
                                  size_t out_len)
{
    if (!ctx) {
        return -1;
    }
    return w4_db_build_prefix_key_from_parts_checked(ctx->request_id,
                                                    ctx->prefix_group,
                                                    out,
                                                    out_len);
}

static int w4_db_build_block_key(const struct w4_db_block_ctx *ctx,
                                 char *out,
                                 size_t out_len)
{
    if (!ctx) {
        return -1;
    }
    return w4_db_build_block_key_from_hash_checked(ctx->block_hash, out, out_len);
}

bool w4_db_prefix_matches_block_meta(const struct w4_db_record *prefix_meta,
                                     const struct w4_db_record *block_meta)
{
    if (!prefix_meta || !block_meta) {
        return false;
    }
    if (prefix_meta->kind != W4_DB_RECORD_REQUEST_PREFIX ||
        block_meta->kind != W4_DB_RECORD_BLOCK_META) {
        return false;
    }
    return prefix_meta->last_result_segment != 0 &&
           block_meta->last_result_segment != 0 &&
           strncmp(prefix_meta->block_hash, block_meta->block_hash,
                   sizeof(prefix_meta->block_hash)) == 0 &&
           prefix_meta->hot_segment_id == block_meta->hot_segment_id &&
           prefix_meta->placement_node == block_meta->placement_node &&
           prefix_meta->placement_level == block_meta->placement_level &&
           prefix_meta->state == block_meta->state;
}

bool w4_db_group_covers_blocks(const struct w4_db_record *group_meta,
                               const struct w4_db_record *primary_block_meta,
                               const struct w4_db_record *aux_block_meta)
{
    if (!group_meta || !primary_block_meta || !aux_block_meta) {
        return false;
    }
    if (group_meta->kind != W4_DB_RECORD_PREFIX_GROUP ||
        primary_block_meta->kind != W4_DB_RECORD_BLOCK_META ||
        aux_block_meta->kind != W4_DB_RECORD_BLOCK_META) {
        return false;
    }
    return group_meta->member_count >= 2 &&
           group_meta->last_result_segment != 0 &&
           w4_db_record_has_member(group_meta, primary_block_meta->block_hash) &&
           w4_db_record_has_member(group_meta, aux_block_meta->block_hash);
}

const char *w4_kvcache_state_name(enum w4_kvcache_state state)
{
    switch (state) {
    case W4_KVCACHE_STATE_MISSING:
        return "missing";
    case W4_KVCACHE_STATE_FILLED:
        return "filled";
    case W4_KVCACHE_STATE_HOT:
        return "hot";
    case W4_KVCACHE_STATE_RELOADED:
        return "reloaded";
    default:
        return "unknown";
    }
}

int w4_db_service_init(struct w4_db_service *svc,
                       bool shmem_ready,
                       bool urma_ready,
                       bool block_ready)
{
    if (!svc) {
        return -1;
    }
    memset(svc, 0, sizeof(*svc));
    svc->shmem_ready = shmem_ready;
    svc->urma_ready = urma_ready;
    svc->block_ready = block_ready;
    if (!svc->shmem_ready || !svc->urma_ready || !svc->block_ready) {
        return -1;
    }
    return 0;
}

int w4_db_get_record(struct w4_db_service *svc, const char *key, struct w4_db_record *out)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || !out) {
        return -1;
    }
    memcpy(out, rec, sizeof(*out));
    return 0;
}

int w4_db_bootstrap_kvcache(struct w4_db_service *svc,
                            const struct w4_db_block_ctx *ctx,
                            struct w4_db_record *resolved_out)
{
    char prefix_key[96];
    char group_key[96];
    char block_key[96];

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    if (w4_db_build_prefix_key(ctx, prefix_key, sizeof(prefix_key)) != 0 ||
        w4_db_build_group_key(ctx, group_key, sizeof(group_key)) != 0 ||
        w4_db_build_block_key(ctx, block_key, sizeof(block_key)) != 0) {
        return -1;
    }

    if (w4_db_put_prefix_group(svc,
                               group_key,
                               ctx->request_id,
                               ctx->group_id,
                               ctx->block_hash,
                               ctx->placement_node,
                               ctx->placement_level,
                               ctx->hot_segment_id,
                               W4_KVCACHE_STATE_FILLED,
                               0) != 0) {
        return -1;
    }
    if (w4_db_put_request_prefix(svc,
                                 prefix_key,
                                 ctx->request_id,
                                 ctx->prefix_group,
                                 ctx->group_id,
                                 ctx->block_hash) != 0) {
        return -1;
    }
    printf("[w4_guest] stage db_service_bootstrap=request_prefix_ok key=%s request=%s prefix=%s block=%s\n",
           prefix_key,
           ctx->request_id,
           ctx->prefix_group,
           ctx->block_hash);

    if (w4_db_put_block_meta(svc,
                             block_key,
                             ctx->request_id,
                             ctx->prefix_group,
                             ctx->group_id,
                             ctx->block_hash,
                             ctx->placement_node,
                             ctx->placement_level,
                             ctx->hot_segment_id,
                             W4_KVCACHE_STATE_FILLED) != 0) {
        return -1;
    }
    printf("[w4_guest] stage db_service_bootstrap=block_meta_ok key=%s placement_node=%u placement_level=%u hot_segment=0x%016" PRIx64 " state=%s\n",
           block_key,
           ctx->placement_node,
           ctx->placement_level,
           ctx->hot_segment_id,
           w4_kvcache_state_name(W4_KVCACHE_STATE_FILLED));

    if (w4_db_update_block_result(svc,
                                  block_key,
                                  ctx->result_segment_id,
                                  W4_KVCACHE_STATE_HOT) != 0) {
        return -1;
    }
    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    printf("[w4_guest] stage db_service_bootstrap=result_update_ok key=%s result_segment=0x%016" PRIx64 " state=%s\n",
           block_key,
           ctx->result_segment_id,
           w4_kvcache_state_name(W4_KVCACHE_STATE_HOT));

    if (w4_db_update_prefix_result(svc,
                                   prefix_key,
                                   ctx,
                                   resolved_out) != 0) {
        return -1;
    }
    printf("[w4_guest] stage db_service_bootstrap=prefix_result_ok key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s result_segment=0x%016" PRIx64 " version=%" PRIu64 "\n",
           prefix_key,
           resolved_out->block_hash,
           resolved_out->hot_segment_id,
           w4_kvcache_state_name(resolved_out->state),
           resolved_out->last_result_segment,
           resolved_out->version);

    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int w4_db_update_prefix_metadata(struct w4_db_service *svc,
                                 const struct w4_db_block_ctx *ctx,
                                 const struct w4_db_record *block_record,
                                 struct w4_db_record *resolved_out)
{
    char prefix_key[96];
    int rc;

    if (!svc || !ctx || !block_record || !resolved_out) {
        return -1;
    }

    if (w4_db_build_prefix_key(ctx, prefix_key, sizeof(prefix_key)) != 0) {
        return -1;
    }
    rc = w4_db_update_prefix_result(svc, prefix_key, ctx, block_record);
    if (rc != 0) {
        return rc;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, block_record) != 0) {
        return -1;
    }
    if (w4_db_get_record(svc, prefix_key, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int w4_db_get_prefix_group_metadata(struct w4_db_service *svc,
                                    const struct w4_db_block_ctx *ctx,
                                    struct w4_db_record *resolved_out)
{
    char group_key[96];

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }
    if (w4_db_build_group_key(ctx, group_key, sizeof(group_key)) != 0) {
        return -1;
    }
    return w4_db_get_record(svc, group_key, resolved_out);
}

int w4_db_apply_block_result(struct w4_db_service *svc,
                             const struct w4_db_block_ctx *ctx,
                             uint64_t result_segment_id,
                             enum w4_kvcache_state next_state,
                             struct w4_db_record *resolved_out)
{
    char block_key[96];
    struct w4_db_record current;
    int rc;

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    if (w4_db_build_block_key(ctx, block_key, sizeof(block_key)) != 0) {
        return -1;
    }
    if (w4_db_get_record(svc, block_key, &current) != 0) {
        return -1;
    }
    if (current.placement_node != ctx->placement_node) {
        return 2;
    }
    rc = w4_db_update_block_result(svc, block_key, result_segment_id, next_state);
    if (rc != 0) {
        return rc;
    }
    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int w4_db_rebind_block_view(struct w4_db_service *svc,
                            const struct w4_db_block_ctx *ctx,
                            uint64_t hot_segment_id,
                            uint32_t placement_level,
                            struct w4_db_record *resolved_out)
{
    char block_key[96];
    struct w4_db_record current;
    int rc;

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    if (w4_db_build_block_key(ctx, block_key, sizeof(block_key)) != 0) {
        return -1;
    }
    if (w4_db_get_record(svc, block_key, &current) != 0) {
        return -1;
    }
    if (current.placement_node != ctx->placement_node) {
        return 2;
    }
    rc = w4_db_update_block_view(svc, block_key, hot_segment_id, placement_level);
    if (rc != 0) {
        return rc;
    }
    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int w4_db_handoff_block_owner(struct w4_db_service *svc,
                              const struct w4_db_block_ctx *ctx,
                              uint32_t placement_node,
                              uint32_t placement_level,
                              uint64_t hot_segment_id,
                              struct w4_db_record *resolved_out)
{
    char block_key[96];
    struct w4_db_record current;
    int rc;

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    if (w4_db_build_block_key(ctx, block_key, sizeof(block_key)) != 0) {
        return -1;
    }
    if (w4_db_get_record(svc, block_key, &current) != 0) {
        return -1;
    }
    if (current.placement_node != ctx->placement_node) {
        return 2;
    }
    rc = w4_db_update_block_owner(svc,
                                  block_key,
                                  placement_node,
                                  placement_level,
                                  hot_segment_id);
    if (rc != 0) {
        return rc;
    }
    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

static void w4_db_reset_remote_slots_for_publish(struct w4_db_cluster_runtime *rt)
{
    (void)rt;
}

int w4_db_cluster_fetch_record(struct w4_db_service *svc,
                               const char *key,
                               struct w4_db_record *resolved_out)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    long deadline;
    int i;
    int rc = -1;

    if (!svc || !key || !resolved_out) {
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0) {
        return -1;
    }
    if (w4_db_write_cluster_payload(svc, &rt->slots[rt->local_idx]) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=write_local_payload_failed\n");
        return -1;
    }
    w4_db_reset_remote_slots_for_publish(rt);

    deadline = obmm_now_ms() + W4_DB_CLUSTER_WAIT_MS;
    while (obmm_now_ms() < deadline) {
        for (i = 0; i < rt->node_count; ++i) {
            if (!rt->slots[i].region.addr) {
                if (i != rt->local_idx && w4_db_activate_remote_slot(rt, i) != 0) {
                    continue;
                }
                if (!rt->slots[i].region.addr) {
                    continue;
                }
            }
            if (w4_db_slot_find_record(&rt->slots[i], key, resolved_out)) {
                rc = 0;
                break;
            }
        }
        if (rc == 0) {
            break;
        }
        usleep(10000);
    }
    if (rc != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=key_not_found key=%s\n", key);
    }
    return rc;
}

int w4_db_publish_observe_cluster(struct w4_db_service *svc,
                                  const struct w4_db_record *local_record,
                                  struct w4_db_cluster_summary *summary)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_cluster_payload *peer_snapshots = NULL;
    struct w4_db_cluster_payload_compact_summary peer_compact[W4_DB_CLUSTER_MAX_NODES];
    struct w4_db_cluster_payload_header seen_header;
    bool peer_ready[W4_DB_CLUSTER_MAX_NODES] = { false };
    uint16_t local_publish_seq;
    uint16_t observed_seq;
    int i;
    int rc = -1;

    if (summary) {
        memset(summary, 0, sizeof(*summary));
    }
    if (!svc || !local_record || !summary) {
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0) {
        goto out;
    }
    peer_snapshots = calloc(W4_DB_CLUSTER_MAX_NODES, sizeof(*peer_snapshots));
    if (!peer_snapshots) {
        goto out;
    }

    summary->active = true;
    summary->placement_coherent = true;
    summary->state_coherent = true;
    summary->prefix_state_ready = true;
    summary->prefix_view_ready = true;
    summary->node_count = (uint32_t)rt->node_count;
    summary->local_version = local_record->version;
    summary->peer_version_floor = local_record->version;
    summary->peer_result_floor = local_record->last_result_segment;
    summary->peer_prefix_version_floor = 0;
    summary->peer_prefix_result_floor = 0;
    summary->peer_record_count_floor = 0;
    summary->peer_prefix_count_floor = 0;
    summary->peer_block_count_floor = 0;
    summary->peer_group_count_floor = 0;

    if (w4_db_write_cluster_payload(svc, &rt->slots[rt->local_idx]) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=write_local_payload_failed\n");
        goto out;
    }
    w4_db_reset_remote_slots_for_publish(rt);
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=remote_slots_reset seq=%u\n",
           rt->local_idx + 1,
           rt->publish_seq);
    if (!w4_db_try_read_stable_payload_region(&rt->slots[rt->local_idx],
                                              &peer_snapshots[rt->local_idx],
                                              NULL)) {
        printf("[w4_guest] gap db_service_cluster_stage=read_local_payload_failed\n");
        goto out;
    }
    w4_db_build_compact_summary(peer_snapshots[rt->local_idx].records,
                                peer_snapshots[rt->local_idx].record_count,
                                &peer_compact[rt->local_idx]);
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=read_local_payload_ok seq=%u\n",
           rt->local_idx + 1,
           rt->publish_seq);
    peer_ready[rt->local_idx] = true;
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    if (w4_db_queue_barrier(rt, OBMM_DESC_W4_READY,
                            rt->observe_epoch,
                            local_publish_seq) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=payload_ready_timeout epoch=%u\n",
               rt->observe_epoch);
        goto out;
    }
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=ready_barrier_ok epoch=%u seq=%u\n",
           rt->local_idx + 1,
           rt->observe_epoch,
           local_publish_seq);

    for (i = 0; i < rt->node_count; ++i) {
        uint16_t owner_publish_seq = local_publish_seq;

        if (i == rt->local_idx) {
            peer_ready[i] = true;
            continue;
        }
        if (w4_db_activate_remote_slot(rt, i) != 0) {
            printf("[w4_guest] gap db_service_cluster_stage=activate_remote_failed owner=node%d reader=node%d\n",
                   i + 1,
                   rt->local_idx + 1);
            goto out;
        }
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=remote_payload_read_wait expect_seq=%u mem_id=%" PRIu64 " map_osync=%d addr=%p\n",
               i + 1,
               rt->local_idx + 1,
               owner_publish_seq,
               rt->slots[i].mem_id,
               rt->slots[i].map_osync ? 1 : 0,
               rt->slots[i].region.addr);
        memset(&seen_header, 0, sizeof(seen_header));
        if (!w4_db_try_read_stable_compact_summary_region(&rt->slots[i],
                                                          &peer_compact[i],
                                                          &seen_header) ||
            seen_header.publish_done_seq < owner_publish_seq) {
            memset(&seen_header, 0, sizeof(seen_header));
            if (w4_db_wait_compact_summary_region_at_least(&rt->slots[i],
                                                           owner_publish_seq,
                                                           W4_DB_CLUSTER_WAIT_MS,
                                                           &peer_compact[i],
                                                           &seen_header)) {
            } else {
                printf("[w4_guest] gap db_service_cluster_stage=payload_not_ready owner=node%d reader=node%d expect_seq=%u seen_seq=%u seen_done=%u magic=0x%08x version=%u count=%u\n",
                       i + 1,
                       rt->local_idx + 1,
                       owner_publish_seq,
                       seen_header.publish_seq,
                       seen_header.publish_done_seq,
                       seen_header.magic,
                       seen_header.version,
                       seen_header.record_count);
                printf("[w4_guest] gap db_service_cluster_stage=payload_not_ready owner=node%d reader=node%d\n",
                       i + 1,
                       rt->local_idx + 1);
                goto out;
            }
        }
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=remote_payload_read_ok seq=%u expect_seq=%u\n",
               i + 1,
               rt->local_idx + 1,
               seen_header.publish_done_seq,
               owner_publish_seq);
        fflush(stdout);
        peer_ready[i] = true;
    }

    observed_seq = (uint16_t)(rt->local_idx + 1);
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=observe_announce_begin epoch=%u seq=%u\n",
           rt->local_idx + 1,
           rt->observe_epoch,
           observed_seq);
    fflush(stdout);
    (void)w4_db_queue_barrier(rt, OBMM_DESC_W4_OBSERVED,
                              rt->observe_epoch, observed_seq);
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=observe_announce_done epoch=%u seq=%u\n",
           rt->local_idx + 1,
           rt->observe_epoch,
           observed_seq);
    fflush(stdout);

    for (i = 0; i < rt->node_count; ++i) {
        uint16_t r;
        struct w4_db_cluster_payload_compact_summary compact = peer_compact[i];

        if (!peer_ready[i]) {
            printf("[w4_guest] gap db_service_cluster_stage=payload_not_ready owner=node%d\n",
                   i + 1);
            goto out;
        }
        if (compact.record_count == 0 || compact.record_count > W4_DB_CLUSTER_MAX_RECORDS) {
            printf("[w4_guest] gap db_service_cluster_stage=compact_summary_invalid owner=node%d count=%u\n",
                   i + 1,
                   compact.record_count);
            goto out;
        }
        if (summary->peer_record_count_floor == 0 ||
            compact.record_count < summary->peer_record_count_floor) {
            summary->peer_record_count_floor = compact.record_count;
        }
        if (i != rt->local_idx) {
            summary->peers_observed += 1;
        }
        if (compact.block_version_floor != 0 &&
            compact.block_version_floor < summary->peer_version_floor) {
            summary->peer_version_floor = compact.block_version_floor;
        }
        if (compact.block_result_floor != 0 &&
            compact.block_result_floor < summary->peer_result_floor) {
            summary->peer_result_floor = compact.block_result_floor;
        }
        if (summary->peer_prefix_version_floor == 0 ||
            (compact.prefix_version_floor != 0 &&
             compact.prefix_version_floor < summary->peer_prefix_version_floor)) {
            summary->peer_prefix_version_floor = compact.prefix_version_floor;
        }
        if (summary->peer_prefix_result_floor == 0 ||
            (compact.prefix_result_floor != 0 &&
             compact.prefix_result_floor < summary->peer_prefix_result_floor)) {
            summary->peer_prefix_result_floor = compact.prefix_result_floor;
        }
        if ((compact.flags & W4_DB_COMPACT_PREFIX_STATE_READY) == 0) {
            summary->prefix_state_ready = false;
        }
        if ((compact.flags & W4_DB_COMPACT_PREFIX_VIEW_READY) == 0) {
            summary->prefix_view_ready = false;
        }
        if (summary->peer_prefix_count_floor == 0 ||
            compact.prefix_count < summary->peer_prefix_count_floor) {
            summary->peer_prefix_count_floor = compact.prefix_count;
        }
        if (summary->peer_block_count_floor == 0 ||
            compact.block_count < summary->peer_block_count_floor) {
            summary->peer_block_count_floor = compact.block_count;
        }
        if (summary->peer_group_count_floor == 0 ||
            compact.group_count < summary->peer_group_count_floor) {
            summary->peer_group_count_floor = compact.group_count;
        }
        printf("[w4_guest] stage db_service_cluster_observe_compact owner=node%d records=%u prefixes=%u blocks=%u groups=%u weight_tiles=%u kvcache_objects=%u block_version_floor=%" PRIu64 " prefix_version_floor=%" PRIu64 "\n",
               i + 1,
               compact.record_count,
               compact.prefix_count,
               compact.block_count,
               compact.group_count,
               compact.weight_tile_count,
               compact.kvcache_object_count,
               compact.block_version_floor,
               compact.prefix_version_floor);
        if (i != rt->local_idx) {
            continue;
        }
        for (r = 0; r < peer_snapshots[i].record_count; ++r) {
            struct w4_db_record *rec = &peer_snapshots[i].records[r];

            if (!rec->in_use) {
                goto out;
            }
            if (rec->kind == W4_DB_RECORD_REQUEST_PREFIX) {
                printf("[w4_guest] stage db_service_cluster_observe owner=node%d kind=request_prefix key=%s version=%" PRIu64 "\n",
                       i + 1,
                       rec->key,
                       rec->version);
            } else if (rec->kind == W4_DB_RECORD_PREFIX_GROUP) {
                printf("[w4_guest] stage db_service_cluster_observe owner=node%d kind=prefix_group key=%s group=%s members=%u state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       i + 1,
                       rec->key,
                       rec->group_id,
                       rec->member_count,
                       w4_kvcache_state_name(rec->state),
                       rec->version,
                       rec->last_result_segment);
            } else if (rec->kind == W4_DB_RECORD_BLOCK_META) {
                if (strncmp(rec->key, local_record->key, sizeof(rec->key)) == 0 &&
                    (rec->placement_node != local_record->placement_node ||
                     rec->placement_level != local_record->placement_level ||
                     rec->hot_segment_id != local_record->hot_segment_id)) {
                    summary->placement_coherent = false;
                }
                if (strncmp(rec->key, local_record->key, sizeof(rec->key)) == 0 &&
                    rec->state != local_record->state) {
                    summary->state_coherent = false;
                }
                printf("[w4_guest] stage db_service_cluster_observe owner=node%d kind=block_meta key=%s state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       i + 1,
                       rec->key,
                       w4_kvcache_state_name(rec->state),
                       rec->version,
                       rec->last_result_segment);
            } else if (rec->kind == W4_DB_RECORD_WEIGHT_TILE ||
                       rec->kind == W4_DB_RECORD_KVCACHE_OBJECT ||
                       rec->kind == W4_DB_RECORD_HIDDEN_RANGE_INPUT ||
                       rec->kind == W4_DB_RECORD_HIDDEN_RANGE_OUTPUT ||
                       rec->kind == W4_DB_RECORD_QWEN3_TOKEN_RESULT ||
                       rec->kind == W4_DB_RECORD_QWEN3_ENGRAM_HISTORY ||
                       rec->kind == W4_DB_RECORD_QWEN3_ENGRAM_CANDIDATES ||
                       rec->kind == W4_DB_RECORD_QWEN3_ENGRAM_SELECTED ||
                       rec->kind == W4_DB_RECORD_QWEN3_ENGRAM_STATE) {
                printf("[w4_guest] stage db_service_cluster_observe owner=node%d kind=%s key=%s offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " version=%" PRIu64 "\n",
                       i + 1,
                       w4_db_object_kind_name(rec->object_payload_kind),
                       rec->key,
                       rec->object_backing_offset,
                       rec->object_backing_len,
                       rec->object_payload_checksum,
                       rec->version);
            } else if (rec->kind == W4_DB_RECORD_LAYER_RANGE_PLACEMENT) {
                printf("[w4_guest] stage db_service_cluster_observe owner=node%d kind=layer_range_placement key=%s owner_node=node%u layers=[%" PRIu64 ",%" PRIu64 ") count=%" PRIu64 " next=node%u checksum=0x%016" PRIx64 " version=%" PRIu64 "\n",
                       i + 1,
                       rec->key,
                       rec->placement_node + 1U,
                       rec->hot_segment_id,
                       rec->last_result_segment,
                       rec->object_backing_len,
                       rec->object_owner_node + 1U,
                       rec->object_payload_checksum,
                       rec->version);
            } else {
                goto out;
            }
        }
    }

    summary->ready = (summary->peers_observed == (uint32_t)(rt->node_count - 1));
    if (summary->ready) {
        printf("[w4_guest] stage db_service_cluster=metadata_visible nodes=%u peers=%u local_version=%" PRIu64 " peer_version_floor=%" PRIu64 " peer_prefix_version_floor=%" PRIu64 " peer_prefix_result_floor=0x%016" PRIx64 " peer_record_count_floor=%u peer_prefix_count_floor=%u peer_block_count_floor=%u peer_group_count_floor=%u prefix_state_ready=%s prefix_view_ready=%s\n",
               summary->node_count,
               summary->peers_observed,
               summary->local_version,
               summary->peer_version_floor,
               summary->peer_prefix_version_floor,
               summary->peer_prefix_result_floor,
               summary->peer_record_count_floor,
               summary->peer_prefix_count_floor,
               summary->peer_block_count_floor,
               summary->peer_group_count_floor,
               summary->prefix_state_ready ? "true" : "false",
               summary->prefix_view_ready ? "true" : "false");
    }
    rc = 0;

out:
    free(peer_snapshots);
    return rc;
}

int w4_db_obmm_service_v0_publish_resolve(struct w4_db_service *svc,
                                          uint32_t local_node,
                                          uint32_t cluster_node_count)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_record local_weight;
    struct w4_db_record local_kvcache;
    struct w4_db_record local_hidden_input;
    struct w4_db_record local_hidden_output;
    struct w4_db_record remote_weight;
    struct w4_db_record remote_kvcache;
    struct w4_db_record remote_hidden_input;
    struct w4_db_record remote_hidden_output;
    struct w4_db_cluster_slot *local_slot;
    struct w4_db_cluster_slot *remote_slot;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long deadline;
    uint8_t *base;
    uint64_t weight_checksum;
    uint64_t kvcache_checksum;
    uint64_t hidden_input_checksum;
    uint64_t hidden_output_checksum;
    uint64_t remote_weight_checksum;
    uint64_t remote_kvcache_checksum;
    uint64_t remote_hidden_input_checksum;
    uint64_t remote_hidden_output_checksum;
    bool remote_payload_checksums_match = false;
    char local_weight_key[96];
    char local_kvcache_key[96];
    char local_hidden_input_key[96];
    char local_hidden_output_key[96];
    char remote_weight_key[96];
    char remote_kvcache_key[96];
    char remote_hidden_input_key[96];
    char remote_hidden_output_key[96];
    uint16_t last_seen_seq = 0;
    uint16_t last_seen_done_seq = 0;
    uint16_t last_seen_record_count = 0;
    bool saw_remote_snapshot = false;
    bool got_remote_weight = false;
    bool got_remote_kvcache = false;
    bool got_remote_hidden_input = false;
    bool got_remote_hidden_output = false;
    unsigned int relax_attempt = 0;
    uint32_t local_range_start = 0;
    uint32_t local_range_end = 0;
    uint32_t remote_range_start = 0;
    uint32_t remote_range_end = 0;
    uint32_t prev_node;
    uint32_t remote_node;
    uint32_t hidden_input_seed_owner;
    uint32_t hidden_input_seed_kind;
    uint32_t total_layers;
    uint32_t min_layers;
    uint32_t max_layers;
    uint64_t hidden_range_bytes;
    uint64_t local_hidden_input_offset;
    uint64_t local_hidden_output_offset;
    struct w4_db_qwen3_layer_range_placement local_placement;
    struct w4_db_qwen3_layer_range_placement remote_placement;
    struct w4_db_qwen3_layer_range_placement predecessor_placement;
    const char *qwen3_model_key;

    memset(&local_placement, 0, sizeof(local_placement));
    memset(&remote_placement, 0, sizeof(remote_placement));
    memset(&predecessor_placement, 0, sizeof(predecessor_placement));
    total_layers = w4_db_qwen3_layer_count();
    min_layers = 0;
    max_layers = 0;
    if (cluster_node_count != 0) {
        min_layers = total_layers / cluster_node_count;
        max_layers = min_layers + (total_layers % cluster_node_count ? 1U : 0U);
    }
    hidden_range_bytes = w4_db_qwen3_hidden_range_bytes();
    local_hidden_input_offset = 0;
    local_hidden_output_offset = 0;
    qwen3_model_key = w4_db_qwen3_model_key();

    if (!svc || cluster_node_count == 0 || local_node >= cluster_node_count) {
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0) {
        return -1;
    }
    if ((uint32_t)rt->local_idx != local_node) {
        printf("[w4_guest] gap obmm_service_v0=local_node_mismatch expected=%u actual=%d\n",
               local_node + 1U,
               rt->local_idx + 1);
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if (!local_slot->region.addr ||
        local_slot->region.len <
            W4_DB_OBMM_HIDDEN_RANGE_RUNTIME_OUTPUT_OFFSET + hidden_range_bytes) {
        printf("[w4_guest] gap obmm_service_v0=local_region_too_small len=%zu\n",
               local_slot->region.len);
        return -1;
    }
    if (cluster_node_count != W4_DB_QWEN3_RANGE_NODES) {
        printf("[w4_guest] gap qwen3_range_forward=node_count_mismatch nodes=%u expected=%u\n",
               cluster_node_count,
               W4_DB_QWEN3_RANGE_NODES);
        return -1;
    }
    if (w4_db_publish_qwen3_layer_range_placements(svc,
                                                   cluster_node_count) != 0 ||
        !w4_db_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        printf("[w4_guest] gap qwen3_range_forward=placement_metadata_missing local=node%u nodes=%u\n",
               local_node + 1U,
               cluster_node_count);
        return -1;
    }
    remote_node = local_placement.next_owner_node;
    if (remote_node >= cluster_node_count || remote_node == local_node ||
        !w4_db_read_qwen3_layer_range_placement(svc,
                                                remote_node,
                                                &remote_placement)) {
        printf("[w4_guest] gap qwen3_range_forward=next_placement_metadata_missing local=node%u next=node%u nodes=%u\n",
               local_node + 1U,
               remote_node + 1U,
               cluster_node_count);
        return -1;
    }
    if (local_placement.layer_start == 0) {
        prev_node = local_node;
        hidden_input_seed_owner = local_node;
        hidden_input_seed_kind = W4_DB_OBMM_KIND_HIDDEN_RANGE_INPUT;
    } else if (w4_db_find_qwen3_layer_range_predecessor(svc,
                                                        local_node,
                                                        &predecessor_placement) &&
               predecessor_placement.layer_end == local_placement.layer_start) {
        prev_node = predecessor_placement.owner_node;
        hidden_input_seed_owner = prev_node;
        hidden_input_seed_kind = W4_DB_OBMM_KIND_HIDDEN_RANGE_OUTPUT;
    } else {
        printf("[w4_guest] gap qwen3_range_forward=predecessor_placement_missing local=node%u layers=[%u,%u)\n",
               local_node + 1U,
               local_placement.layer_start,
               local_placement.layer_end);
        return -1;
    }
    local_range_start = local_placement.layer_start;
    local_range_end = local_placement.layer_end;
    remote_range_start = remote_placement.layer_start;
    remote_range_end = remote_placement.layer_end;

    snprintf(local_weight_key,
             sizeof(local_weight_key),
             "weights/%s/node%u/tile0",
             qwen3_model_key,
             local_node + 1U);
    snprintf(local_kvcache_key,
             sizeof(local_kvcache_key),
             "kvcache/w4/node%u/block0",
             local_node + 1U);
    snprintf(local_hidden_input_key,
             sizeof(local_hidden_input_key),
             "hidden/%s/node%u/range-input",
             qwen3_model_key,
             local_node + 1U);
    snprintf(local_hidden_output_key,
             sizeof(local_hidden_output_key),
             "hidden/%s/node%u/range-output",
             qwen3_model_key,
             local_node + 1U);
    snprintf(remote_weight_key,
             sizeof(remote_weight_key),
             "weights/%s/node%u/tile0",
             qwen3_model_key,
             remote_node + 1U);
    snprintf(remote_kvcache_key,
             sizeof(remote_kvcache_key),
             "kvcache/w4/node%u/block0",
             remote_node + 1U);
    snprintf(remote_hidden_input_key,
             sizeof(remote_hidden_input_key),
             "hidden/%s/node%u/range-input",
             qwen3_model_key,
             remote_node + 1U);
    snprintf(remote_hidden_output_key,
             sizeof(remote_hidden_output_key),
             "hidden/%s/node%u/range-output",
             qwen3_model_key,
             remote_node + 1U);

    base = (uint8_t *)local_slot->region.addr;
    if (w4_db_payload_arena_alloc(rt,
                                  hidden_range_bytes,
                                  64,
                                  &local_hidden_input_offset) != 0 ||
        w4_db_payload_arena_alloc(rt,
                                  hidden_range_bytes,
                                  64,
                                  &local_hidden_output_offset) != 0) {
        printf("[w4_guest] gap obmm_service_v0=hidden_range_arena_alloc_failed local=node%u bytes=%" PRIu64 "\n",
               local_node + 1U,
               hidden_range_bytes);
        return -1;
    }
    w4_db_fill_obmm_object_payload(base + W4_DB_OBMM_WEIGHT_OFFSET,
                                   W4_DB_OBMM_DEMO_OBJECT_BYTES,
                                   local_node,
                                   W4_DB_OBMM_KIND_WEIGHT_TILE);
    w4_db_fill_obmm_object_payload(base + W4_DB_OBMM_KVCACHE_OFFSET,
                                   W4_DB_OBMM_DEMO_OBJECT_BYTES,
                                   local_node,
                                   W4_DB_OBMM_KIND_KVCACHE_BLOCK);
    w4_db_fill_obmm_object_payload(base + local_hidden_input_offset,
                                   hidden_range_bytes,
                                   hidden_input_seed_owner,
                                   hidden_input_seed_kind);
    w4_db_fill_obmm_object_payload(base + local_hidden_output_offset,
                                   hidden_range_bytes,
                                   local_node,
                                   W4_DB_OBMM_KIND_HIDDEN_RANGE_OUTPUT);
    weight_checksum = w4_db_checksum_bytes(base + W4_DB_OBMM_WEIGHT_OFFSET,
                                           W4_DB_OBMM_DEMO_OBJECT_BYTES);
    kvcache_checksum = w4_db_checksum_bytes(base + W4_DB_OBMM_KVCACHE_OFFSET,
                                            W4_DB_OBMM_DEMO_OBJECT_BYTES);
    hidden_input_checksum =
        w4_db_checksum_bytes(base + local_hidden_input_offset,
                             hidden_range_bytes);
    hidden_output_checksum =
        w4_db_checksum_bytes(base + local_hidden_output_offset,
                             hidden_range_bytes);
    if (w4_db_update_region_range_at(local_slot,
                                     W4_DB_OBMM_WEIGHT_OFFSET,
                                     W4_DB_OBMM_DEMO_OBJECT_BYTES,
                                     true) != 0 ||
        w4_db_update_region_range_at(local_slot,
                                     W4_DB_OBMM_KVCACHE_OFFSET,
                                     W4_DB_OBMM_DEMO_OBJECT_BYTES,
                                     true) != 0 ||
        w4_db_update_region_range_at(local_slot,
                                     local_hidden_input_offset,
                                     hidden_range_bytes,
                                     true) != 0 ||
        w4_db_update_region_range_at(local_slot,
                                     local_hidden_output_offset,
                                     hidden_range_bytes,
                                     true) != 0) {
        printf("[w4_guest] gap obmm_service_v0=local_payload_publish_failed\n");
        return -1;
    }
    (void)msync(base + W4_DB_OBMM_WEIGHT_OFFSET, W4_DB_OBMM_DEMO_OBJECT_BYTES, MS_SYNC);
    (void)msync(base + W4_DB_OBMM_KVCACHE_OFFSET, W4_DB_OBMM_DEMO_OBJECT_BYTES, MS_SYNC);
    (void)msync(base + local_hidden_input_offset,
                hidden_range_bytes,
                MS_SYNC);
    (void)msync(base + local_hidden_output_offset,
                hidden_range_bytes,
                MS_SYNC);

    if (w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_WEIGHT_TILE,
                                     local_weight_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_WEIGHT_TILE,
                                     W4_DB_OBMM_WEIGHT_OFFSET,
                                     W4_DB_OBMM_DEMO_OBJECT_BYTES,
                                     weight_checksum,
                                     &local_weight) != 0 ||
        w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_KVCACHE_OBJECT,
                                     local_kvcache_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_KVCACHE_BLOCK,
                                     W4_DB_OBMM_KVCACHE_OFFSET,
                                     W4_DB_OBMM_DEMO_OBJECT_BYTES,
                                     kvcache_checksum,
                                     &local_kvcache) != 0 ||
        w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_HIDDEN_RANGE_INPUT,
                                     local_hidden_input_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_HIDDEN_RANGE_INPUT,
                                     local_hidden_input_offset,
                                     hidden_range_bytes,
                                     hidden_input_checksum,
                                     &local_hidden_input) != 0 ||
        w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_HIDDEN_RANGE_OUTPUT,
                                     local_hidden_output_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_HIDDEN_RANGE_OUTPUT,
                                     local_hidden_output_offset,
                                     hidden_range_bytes,
                                     hidden_output_checksum,
                                     &local_hidden_output) != 0) {
        printf("[w4_guest] gap obmm_service_v0=metadata_put_failed\n");
        return -1;
    }
    printf("[w4_guest] stage obmm_service_v0_publish kind=weight_tile key=%s owner=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_weight.key,
           local_node + 1U,
           local_weight.object_backing_offset,
           local_weight.object_backing_len,
           local_weight.object_payload_checksum);
    printf("[w4_guest] stage obmm_service_v0_publish kind=kvcache_block key=%s owner=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_kvcache.key,
           local_node + 1U,
           local_kvcache.object_backing_offset,
           local_kvcache.object_backing_len,
           local_kvcache.object_payload_checksum);
    printf("[w4_guest] stage qwen3_range_forward_placement local=node%u key=placement/%s/layer-range/node%u layers=[%u,%u) count=%u next=node%u predecessor=node%u terminal=%s source=db_metadata strategy=%s status=ok\n",
           local_node + 1U,
           qwen3_model_key,
           local_node + 1U,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           remote_node + 1U,
           prev_node + 1U,
           local_placement.terminal ? "true" : "false",
           "balanced_layers");
    printf("[w4_guest] stage obmm_service_v0_publish kind=hidden_range_input key=%s owner=node%u layers=[%u,%u) count=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_hidden_input.key,
           local_node + 1U,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           local_hidden_input.object_backing_offset,
           local_hidden_input.object_backing_len,
           local_hidden_input.object_payload_checksum);
    printf("[w4_guest] stage obmm_service_v0_publish kind=hidden_range_output key=%s owner=node%u layers=[%u,%u) count=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_hidden_output.key,
           local_node + 1U,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           local_hidden_output.object_backing_offset,
           local_hidden_output.object_backing_len,
           local_hidden_output.object_payload_checksum);
    printf("[w4_guest] stage qwen3_range_forward_contract local=node%u layers=[%u,%u) count=%u next=node%u pipeline_nodes=%u total_layers=%u min_layers=%u max_layers=%u balanced=true placement_source=db_metadata input_key=%s output_key=%s kv_state_bytes_per_token=%" PRIu64 " backing=obmm_pool metadata=db status=ok\n",
           local_node + 1U,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           remote_node + 1U,
           cluster_node_count,
           total_layers,
           min_layers,
           max_layers,
           local_hidden_input.key,
           local_hidden_output.key,
           w4_db_qwen3_range_kv_state_bytes(local_range_start, local_range_end, 1));

    if (w4_db_write_cluster_payload(svc, local_slot) != 0) {
        printf("[w4_guest] gap obmm_service_v0=metadata_publish_failed\n");
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
    (void)w4_db_queue_barrier(rt, OBMM_DESC_W4_READY,
                              object_epoch, local_publish_seq);
    printf("[w4_guest] stage obmm_service_v0_local_ready_announced local=node%u epoch=%u seq=%u\n",
           local_node + 1U,
           object_epoch,
           local_publish_seq);
    if (w4_db_push_obmm_object_descs(rt,
                                     W4_DB_OBMM_KIND_WEIGHT_TILE,
                                     local_weight.object_backing_offset,
                                     local_weight.object_backing_len,
                                     local_weight.object_payload_checksum,
                                     object_epoch) != 0 ||
        w4_db_push_obmm_object_descs(rt,
                                     W4_DB_OBMM_KIND_KVCACHE_BLOCK,
                                     local_kvcache.object_backing_offset,
                                     local_kvcache.object_backing_len,
                                     local_kvcache.object_payload_checksum,
                                     object_epoch) != 0 ||
        w4_db_push_obmm_object_descs(rt,
                                     W4_DB_OBMM_KIND_HIDDEN_RANGE_INPUT,
                                     local_hidden_input.object_backing_offset,
                                     local_hidden_input.object_backing_len,
                                     local_hidden_input.object_payload_checksum,
                                     object_epoch) != 0 ||
        w4_db_push_obmm_object_descs(rt,
                                     W4_DB_OBMM_KIND_HIDDEN_RANGE_OUTPUT,
                                     local_hidden_output.object_backing_offset,
                                     local_hidden_output.object_backing_len,
                                     local_hidden_output.object_payload_checksum,
                                     object_epoch) != 0) {
        printf("[w4_guest] gap obmm_service_v0=object_desc_publish_failed local=node%u epoch=%u\n",
               local_node + 1U,
               object_epoch);
        return -1;
    }
    printf("[w4_guest] stage obmm_service_v0_object_desc_put local=node%u objects=4 queue=obmm_spsc epoch=%u status=ok\n",
           local_node + 1U,
           object_epoch);
    if (w4_db_activate_remote_slot(rt, (int)remote_node) != 0) {
        printf("[w4_guest] gap obmm_service_v0=remote_slot_import_failed remote=node%u\n",
               remote_node + 1U);
        return -1;
    }
    remote_slot = &rt->slots[remote_node];
    deadline = obmm_now_ms() + W4_DB_OBMM_SERVICE_WAIT_MS;
    while (obmm_now_ms() < deadline) {
        struct w4_db_cluster_payload_header seen;

        memset(&seen, 0, sizeof(seen));
        if (w4_db_try_read_stable_compact_summary_region(remote_slot,
                                                         &(struct w4_db_cluster_payload_compact_summary){ 0 },
                                                         &seen)) {
            last_seen_seq = seen.publish_seq;
            last_seen_done_seq = seen.publish_done_seq;
            last_seen_record_count = seen.record_count;
            saw_remote_snapshot = true;
            got_remote_weight = w4_db_slot_find_record(remote_slot,
                                                       remote_weight_key,
                                                       &remote_weight);
            got_remote_kvcache = w4_db_slot_find_record(remote_slot,
                                                        remote_kvcache_key,
                                                        &remote_kvcache);
            got_remote_hidden_input = w4_db_slot_find_record(remote_slot,
                                                             remote_hidden_input_key,
                                                             &remote_hidden_input);
            got_remote_hidden_output = w4_db_slot_find_record(remote_slot,
                                                              remote_hidden_output_key,
                                                              &remote_hidden_output);
        }
        if (got_remote_weight && got_remote_kvcache &&
            got_remote_hidden_input && got_remote_hidden_output) {
            break;
        }
        w4_db_cpu_relax_wait(&relax_attempt);
    }
    if (remote_weight.kind != W4_DB_RECORD_WEIGHT_TILE ||
        remote_kvcache.kind != W4_DB_RECORD_KVCACHE_OBJECT ||
        remote_hidden_input.kind != W4_DB_RECORD_HIDDEN_RANGE_INPUT ||
        remote_hidden_output.kind != W4_DB_RECORD_HIDDEN_RANGE_OUTPUT) {
        printf("[w4_guest] gap obmm_service_v0=remote_metadata_resolve_failed remote=node%u snapshot=%u seq=%u done=%u count=%u weight=%u kvcache=%u hidden_input=%u hidden_output=%u\n",
               remote_node + 1U,
               saw_remote_snapshot ? 1U : 0U,
               last_seen_seq,
               last_seen_done_seq,
               last_seen_record_count,
               got_remote_weight ? 1U : 0U,
               got_remote_kvcache ? 1U : 0U,
               got_remote_hidden_input ? 1U : 0U,
               got_remote_hidden_output ? 1U : 0U);
        return -1;
    }
    if (remote_weight.kind != W4_DB_RECORD_WEIGHT_TILE ||
        remote_kvcache.kind != W4_DB_RECORD_KVCACHE_OBJECT ||
        remote_hidden_input.kind != W4_DB_RECORD_HIDDEN_RANGE_INPUT ||
        remote_hidden_output.kind != W4_DB_RECORD_HIDDEN_RANGE_OUTPUT ||
        remote_weight.object_backing_len != W4_DB_OBMM_DEMO_OBJECT_BYTES ||
        remote_kvcache.object_backing_len != W4_DB_OBMM_DEMO_OBJECT_BYTES ||
        remote_hidden_input.object_backing_len != hidden_range_bytes ||
        remote_hidden_output.object_backing_len != hidden_range_bytes) {
        printf("[w4_guest] gap obmm_service_v0=remote_metadata_incoherent remote=node%u\n",
               remote_node + 1U);
        return -1;
    }
    if (w4_db_wait_remote_obmm_object_descs(rt,
                                           remote_node,
                                           object_epoch,
                                           &remote_weight,
                                           &remote_kvcache,
                                           &remote_hidden_input,
                                           &remote_hidden_output) != 0) {
        return -1;
    }
    printf("[w4_guest] stage obmm_service_v0_object_desc_get remote=node%u reader=node%u objects=4 queue=obmm_spsc epoch=%u status=ok\n",
           remote_node + 1U,
           local_node + 1U,
           object_epoch);
    if (!remote_slot->region.addr ||
        remote_weight.object_backing_offset + remote_weight.object_backing_len > remote_slot->region.len ||
        remote_kvcache.object_backing_offset + remote_kvcache.object_backing_len > remote_slot->region.len ||
        remote_hidden_input.object_backing_offset + remote_hidden_input.object_backing_len > remote_slot->region.len ||
        remote_hidden_output.object_backing_offset + remote_hidden_output.object_backing_len > remote_slot->region.len) {
        printf("[w4_guest] gap obmm_service_v0=remote_region_too_small remote=node%u\n",
               remote_node + 1U);
        return -1;
    }
    deadline = obmm_now_ms() + W4_DB_OBMM_SERVICE_WAIT_MS;
    do {
        remote_weight_checksum =
            w4_db_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                     remote_weight.object_backing_offset,
                                 remote_weight.object_backing_len);
        remote_kvcache_checksum =
            w4_db_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                     remote_kvcache.object_backing_offset,
                                 remote_kvcache.object_backing_len);
        remote_hidden_input_checksum =
            w4_db_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                     remote_hidden_input.object_backing_offset,
                                 remote_hidden_input.object_backing_len);
        remote_hidden_output_checksum =
            w4_db_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                     remote_hidden_output.object_backing_offset,
                                 remote_hidden_output.object_backing_len);
        remote_payload_checksums_match =
            remote_weight_checksum == remote_weight.object_payload_checksum &&
            remote_kvcache_checksum == remote_kvcache.object_payload_checksum &&
            remote_hidden_input_checksum == remote_hidden_input.object_payload_checksum &&
            remote_hidden_output_checksum == remote_hidden_output.object_payload_checksum;
        if (remote_payload_checksums_match) {
            break;
        }
        w4_db_cpu_relax_wait(&relax_attempt);
    } while (obmm_now_ms() < deadline);
    if (!remote_payload_checksums_match) {
        printf("[w4_guest] gap obmm_service_v0=remote_payload_checksum_mismatch remote=node%u weight=0x%016" PRIx64 "/0x%016" PRIx64 " kvcache=0x%016" PRIx64 "/0x%016" PRIx64 " hidden_input=0x%016" PRIx64 "/0x%016" PRIx64 " hidden_output=0x%016" PRIx64 "/0x%016" PRIx64 "\n",
               remote_node + 1U,
               remote_weight_checksum,
               remote_weight.object_payload_checksum,
               remote_kvcache_checksum,
               remote_kvcache.object_payload_checksum,
               remote_hidden_input_checksum,
               remote_hidden_input.object_payload_checksum,
               remote_hidden_output_checksum,
               remote_hidden_output.object_payload_checksum);
        return -1;
    }
    printf("[w4_guest] stage obmm_service_v0_resolve kind=weight_tile key=%s owner=node%u reader=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_weight.key,
           remote_node + 1U,
           local_node + 1U,
           remote_weight.object_backing_offset,
           remote_weight.object_backing_len,
           remote_weight_checksum);
    printf("[w4_guest] stage obmm_service_v0_resolve kind=kvcache_block key=%s owner=node%u reader=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_kvcache.key,
           remote_node + 1U,
           local_node + 1U,
           remote_kvcache.object_backing_offset,
           remote_kvcache.object_backing_len,
           remote_kvcache_checksum);
    printf("[w4_guest] stage obmm_service_v0_resolve kind=hidden_range_input key=%s owner=node%u reader=node%u layers=[%u,%u) count=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_hidden_input.key,
           remote_node + 1U,
           local_node + 1U,
           remote_range_start,
           remote_range_end,
           remote_range_end - remote_range_start,
           remote_hidden_input.object_backing_offset,
           remote_hidden_input.object_backing_len,
           remote_hidden_input_checksum);
    printf("[w4_guest] stage obmm_service_v0_resolve kind=hidden_range_output key=%s owner=node%u reader=node%u layers=[%u,%u) count=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_hidden_output.key,
           remote_node + 1U,
           local_node + 1U,
           remote_range_start,
           remote_range_end,
           remote_range_end - remote_range_start,
           remote_hidden_output.object_backing_offset,
           remote_hidden_output.object_backing_len,
           remote_hidden_output_checksum);
    if (!local_placement.terminal &&
        remote_hidden_input_checksum != hidden_output_checksum) {
        printf("[w4_guest] gap qwen3_range_forward=next_input_checksum_mismatch local=node%u next=node%u output=0x%016" PRIx64 " next_input=0x%016" PRIx64 "\n",
               local_node + 1U,
               remote_node + 1U,
               hidden_output_checksum,
               remote_hidden_input_checksum);
        return -1;
    }
    printf("[w4_guest] stage qwen3_range_forward_handoff local=node%u next=node%u local_layers=[%u,%u) local_count=%u next_layers=[%u,%u) next_count=%u local_output_checksum=0x%016" PRIx64 " next_input_checksum=0x%016" PRIx64 " terminal=%s placement_source=db_metadata backing=obmm_pool metadata=db queue=obmm_spsc status=ok\n",
           local_node + 1U,
           remote_node + 1U,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           remote_range_start,
           remote_range_end,
           remote_range_end - remote_range_start,
           hidden_output_checksum,
           remote_hidden_input_checksum,
           local_placement.terminal ? "true" : "false");
    printf("[w4_guest] stage qwen3_range_forward_summary local=node%u nodes=%u layers=%u assigned_layers=[%u,%u) assigned_count=%u next=node%u hidden_bytes=%" PRIu64 " objects=2 min_layers=%u max_layers=%u balanced=true placement_source=db_metadata backing=obmm_pool metadata=db status=ok\n",
           local_node + 1U,
           cluster_node_count,
           total_layers,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           remote_node + 1U,
           hidden_range_bytes,
           min_layers,
           max_layers);
    printf("[w4_guest] stage obmm_service_v0=payload_backing_resolved local=node%u remote=node%u objects=4 bytes=%" PRIu64 " hidden_bytes=%" PRIu64 " hidden_input_offset=0x%016" PRIx64 " hidden_output_offset=0x%016" PRIx64 " backing=obmm_pool allocator=linear_payload_arena metadata=db status=ok\n",
           local_node + 1U,
           remote_node + 1U,
           (uint64_t)W4_DB_OBMM_DEMO_OBJECT_BYTES,
           hidden_range_bytes,
           local_hidden_input_offset,
           local_hidden_output_offset);
    return 0;
}

int w4_db_obmm_service_v0_wait_runtime_range_input_view(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct w4_db_object_payload_view *view_out)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_qwen3_layer_range_placement local_placement;
    struct w4_db_qwen3_layer_range_placement source_placement;
    struct w4_db_cluster_slot *source_slot = NULL;
    struct w4_db_record remote_hidden_output;
    struct obmm_desc handoff_desc;
    char ingress_key[96];
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
    uint64_t hidden_range_bytes = w4_db_qwen3_handoff_hidden_bytes(decode_step);
    const uint8_t *payload_view;
    uint64_t checksum;

    if (!view_out ||
        cluster_node_count != W4_DB_QWEN3_RANGE_NODES ||
        local_node >= cluster_node_count) {
        return -1;
    }
    memset(&local_placement, 0, sizeof(local_placement));
    local_placement.owner_node = local_node;
    local_placement.next_owner_node = (local_node + 1U) % cluster_node_count;
    local_placement.terminal = (local_node + 1U == cluster_node_count);
    if (w4_db_qwen3_layer_range_for_node(local_node,
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
    if (local_placement.layer_start == 0) {
        return 0;
    }
    memset(&source_placement, 0, sizeof(source_placement));
    source_placement.owner_node = local_node - 1U;
    source_placement.next_owner_node = local_node;
    source_placement.terminal = false;
    if (w4_db_qwen3_layer_range_for_node(source_placement.owner_node,
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
    if (w4_db_cluster_runtime_init(rt) != 0) {
        return -1;
    }
    if (source_node >= cluster_node_count || !rt->ingress_queues[source_node]) {
        return -1;
    }
    snprintf(ingress_key,
             sizeof(ingress_key),
             "hidden/%s/node%u/range-runtime-input/decode-step%" PRIu64,
             w4_db_qwen3_model_key(),
             local_node + 1U,
             decode_step);

    memset(&remote_hidden_output, 0, sizeof(remote_hidden_output));
    memset(&handoff_desc, 0, sizeof(handoff_desc));
    expected_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (expected_epoch == 0) {
        expected_epoch = 1;
    }
    wait_enter_ms = obmm_now_ms();
    deadline = wait_enter_ms + W4_DB_QWEN3_RUNTIME_RANGE_WAIT_MS;
    while (obmm_now_ms() < deadline) {
        struct obmm_desc rx;

        attempts++;
        if (w4_db_take_pending_runtime_range_input_desc(rt,
                                                        (int)source_node,
                                                        expected_epoch,
                                                        &handoff_desc)) {
            break;
        }
        while (obmm_spsc_pop(rt->ingress_queues[source_node], &rx) == 0) {
            if (w4_db_runtime_range_input_desc_matches(&rx,
                                                       expected_epoch)) {
                handoff_desc = rx;
                break;
            }
            w4_db_stash_pending_desc(rt, (int)source_node, &rx);
        }
        if (handoff_desc.type == OBMM_DESC_W4_OBJECT_PUT) {
            break;
        }
        w4_db_cpu_relax_wait(&relax_attempt);
    }
    if (!w4_db_runtime_range_input_desc_matches(&handoff_desc,
                                                expected_epoch)) {
        printf("[w4_guest] gap qwen3_range_forward=runtime_ingress_desc_wait_failed local=node%u source=node%u key=%s attempts=%u\n",
               local_node + 1U,
               source_node + 1U,
               ingress_key,
               attempts);
        return -1;
    }
    found_local_ms = obmm_now_ms();
    found_ms = w4_db_wallclock_ms();
    if (!rt->slots[source_node].region.addr) {
        long activate_start_ms = obmm_now_ms();

        if (w4_db_activate_remote_slot(rt, (int)source_node) != 0) {
            return -1;
        }
        activate_ms += (uint64_t)(obmm_now_ms() - activate_start_ms);
    }
    {
        long metadata_start_ms = obmm_now_ms();
        struct w4_db_cluster_payload_compact_summary compact;
        struct w4_db_cluster_payload_header seen;

        source_slot = &rt->slots[source_node];
        memset(&compact, 0, sizeof(compact));
        memset(&seen, 0, sizeof(seen));
        if (!w4_db_try_read_stable_compact_summary_region(source_slot,
                                                          &compact,
                                                          &seen) ||
            !w4_db_slot_find_record(source_slot,
                                    ingress_key,
                                    &remote_hidden_output)) {
            return -1;
        }
        metadata_ms = (uint64_t)(obmm_now_ms() - metadata_start_ms);
    }
    if (!source_slot ||
        remote_hidden_output.object_payload_kind !=
            W4_DB_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT ||
        remote_hidden_output.object_backing_len != hidden_range_bytes ||
        remote_hidden_output.object_backing_offset != handoff_desc.payload_offset ||
        remote_hidden_output.object_backing_len != handoff_desc.payload_len ||
        handoff_desc.cookie !=
            (uint32_t)(remote_hidden_output.object_payload_checksum ^
                       (remote_hidden_output.object_payload_checksum >> 32)) ||
        !source_slot->region.addr ||
        remote_hidden_output.object_backing_offset + remote_hidden_output.object_backing_len >
            source_slot->region.len) {
        printf("[w4_guest] gap qwen3_range_forward=runtime_ingress_wait_failed local=node%u key=%s\n",
               local_node + 1U,
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
    if (w4_db_record_to_lingqu_obmm_ref(&remote_hidden_output,
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
    printf("[w4_guest] stage qwen3_range_forward_runtime_input_resolve local=node%u source=node%u key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) input_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " wait_enter_to_found_ms=%ld producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld producer_to_found_ms=%ld producer_to_found_mono_ms=%ld attempts=%u activate_ms=%" PRIu64 " metadata_ms=%" PRIu64 " copy_ms=0 checksum_ms=%ld validation=object_ref_metadata queue=obmm_spsc receive=descriptor metadata=lingqu_object_service backing=obmm_shmem target=mapped_view status=ok\n",
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

int w4_db_obmm_service_v0_wait_runtime_range_input(uint32_t local_node,
                                                   uint32_t cluster_node_count,
                                                   uint64_t decode_step,
                                                   uint8_t *payload_out,
                                                   uint64_t payload_len,
                                                   uint64_t *checksum_out)
{
    struct w4_db_object_payload_view view;
    uint64_t hidden_range_bytes = w4_db_qwen3_handoff_hidden_bytes(decode_step);

    if (!payload_out || payload_len != hidden_range_bytes || !checksum_out) {
        return -1;
    }
    if (w4_db_obmm_service_v0_wait_runtime_range_input_view(local_node,
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

int w4_db_obmm_service_v0_publish_runtime_range_output(struct w4_db_service *svc,
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
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_cluster_slot *local_slot;
    struct w4_db_record local_hidden_output;
    struct w4_db_record local_kv_state;
    struct w4_db_qwen3_layer_range_placement local_placement;
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
    uint64_t hidden_range_bytes = w4_db_qwen3_handoff_hidden_bytes(decode_step);
    struct lingqu_obmm_object_ref_wire hidden_ref;
    struct lingqu_obmm_object_ref_wire kv_ref;

    if (!svc || !payload || payload_len != hidden_range_bytes ||
        !kv_payload || kv_payload_len == 0 ||
        cluster_node_count != W4_DB_QWEN3_RANGE_NODES ||
        local_node >= cluster_node_count) {
        return -1;
    }
    checksum = w4_db_qwen3_hidden_payload_checksum(payload, payload_len);
    if (checksum != expected_checksum) {
        printf("[w4_guest] gap qwen3_range_forward=runtime_output_checksum_mismatch local=node%u checksum=0x%016" PRIx64 " expected=0x%016" PRIx64 "\n",
               local_node + 1U,
               checksum,
               expected_checksum);
        return -1;
    }
    kv_checksum = w4_db_qwen3_hidden_payload_checksum(kv_payload, kv_payload_len);
    if (kv_checksum != expected_kv_checksum) {
        printf("[w4_guest] gap qwen3_range_forward=runtime_kv_checksum_mismatch local=node%u checksum=0x%016" PRIx64 " expected=0x%016" PRIx64 " bytes=%" PRIu64 "\n",
               local_node + 1U,
               kv_checksum,
               expected_kv_checksum,
               kv_payload_len);
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0 ||
        w4_db_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !w4_db_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    terminal_range = local_placement.layer_end >= w4_db_qwen3_layer_count();
    target_node = terminal_range ? local_node : local_placement.next_owner_node;
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        w4_db_payload_arena_alloc(rt,
                                  payload_len,
                                  64,
                                  &runtime_output_offset) != 0) {
        return -1;
    }
    if (w4_db_qwen3_kv_state_alloc(rt,
                                   kv_payload_len,
                                   &kv_state_offset,
                                   &kv_state_block_bytes,
                                   &kv_state_block_count,
                                   &kv_state_reserved_bytes) != 0) {
        printf("[w4_guest] gap qwen3_range_forward=runtime_kv_block_span_alloc_failed local=node%u step=%" PRIu64 " bytes=%" PRIu64 " block_bytes=%" PRIu64 " blocks=%" PRIu64 " reserved_bytes=%" PRIu64 " region_len=%zu\n",
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
    if (w4_db_update_region_range_at(local_slot,
                                     runtime_output_offset,
                                     payload_len,
                                     true) != 0 ||
        w4_db_update_region_range_at(local_slot,
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
             w4_db_qwen3_model_key(),
             target_node + 1U,
             decode_step);
    snprintf(local_kv_state_key,
             sizeof(local_kv_state_key),
             "kvcache/%s/node%u/layers-%u-%u/decode-step%" PRIu64,
             w4_db_qwen3_model_key(),
             local_node + 1U,
             local_placement.layer_start,
             local_placement.layer_end,
             decode_step);
    producer_publish_monotonic_ms = obmm_now_ms();
    producer_publish_ms = w4_db_wallclock_ms();
    producer_clock_offset_ms = producer_publish_ms - producer_publish_monotonic_ms;
    if (w4_db_put_obmm_object_record(svc,
                                     terminal_range ?
                                         W4_DB_RECORD_HIDDEN_RANGE_OUTPUT :
                                         W4_DB_RECORD_HIDDEN_RANGE_INPUT,
                                     local_hidden_output_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                                     runtime_output_offset,
                                     payload_len,
                                     checksum,
                                     &local_hidden_output) != 0 ||
        w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_KVCACHE_OBJECT,
                                     local_kv_state_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_QWEN3_KV_STATE,
                                     kv_state_offset,
                                     kv_payload_len,
                                     kv_checksum,
                                     &local_kv_state) != 0) {
        return -1;
    }
    {
        struct w4_db_record *published_record =
            w4_db_find_record(svc, local_hidden_output_key);

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
        struct w4_db_record *published_record =
            w4_db_find_record(svc, local_kv_state_key);

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
    if (w4_db_write_cluster_payload(svc, local_slot) != 0) {
        return -1;
    }
    if (w4_db_record_to_lingqu_obmm_ref(&local_hidden_output, &hidden_ref) != 0 ||
        w4_db_record_to_lingqu_obmm_ref(&local_kv_state, &kv_ref) != 0) {
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
    const char *w5_run_id = getenv("SIM_W5_RUN_ID");

    boundary_observation_id[0] = '\0';
    if (w5_run_id && w5_run_id[0] != '\0') {
        snprintf(boundary_observation_id,
                 sizeof(boundary_observation_id),
                 "boundary-observation/%s/step%" PRIu64 "/node%u",
                 w5_run_id,
                 decode_step,
                 local_node + 1U);
    }
    if (!terminal_range &&
        w4_db_push_obmm_object_desc_to(rt,
                                       target_node,
                                       W4_DB_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                                       local_hidden_output.object_backing_offset,
                                       local_hidden_output.object_backing_len,
                                       local_hidden_output.object_payload_checksum,
                                       object_epoch) != 0) {
        return -1;
    }
    if (!terminal_range && boundary_observation_id[0] != '\0') {
        printf("[w4_guest] stage qwen3_range_forward_runtime_ingress_publish local=node%u target=node%u observation_id=%s step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
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
        printf("[w4_guest] stage qwen3_range_forward_runtime_ingress_publish local=node%u target=node%u step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
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
    printf("[w4_guest] stage qwen3_range_forward_runtime_output_publish local=node%u step=%" PRIu64 " key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u output_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
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
    printf("[w4_guest] stage qwen3_range_kv_state_publish local=node%u step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u kv_bytes=%" PRIu64 " kv_checksum=0x%016" PRIx64 " offset=0x%016" PRIx64 " slot_bytes=%" PRIu64 " block_bytes=%" PRIu64 " blocks=%" PRIu64 " reserved_bytes=%" PRIu64 " producer_publish_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service status=ok\n",
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
           (uint64_t)W4_DB_OBMM_QWEN3_KV_STATE_SLOT_BYTES,
           kv_state_block_bytes,
           kv_state_block_count,
           kv_state_reserved_bytes,
           producer_publish_ms,
           object_epoch,
           local_publish_seq);
    return 0;
}

int w4_db_obmm_service_v0_resolve_previous_range_kv_state_view(
    struct w4_db_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct w4_db_object_payload_view *view_out)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_cluster_slot *local_slot;
    struct w4_db_qwen3_layer_range_placement local_placement;
    struct w4_db_record kv_state;
    char kv_state_key[96];
    uint64_t checksum;
    uint64_t previous_step;

    if (!view_out) {
        return -1;
    }
    memset(view_out, 0, sizeof(*view_out));
    if (decode_step == 0) {
        return 0;
    }
    if (!svc ||
        cluster_node_count != W4_DB_QWEN3_RANGE_NODES ||
        local_node >= cluster_node_count) {
        return -1;
    }
    previous_step = decode_step - 1U;
    if (w4_db_cluster_runtime_init(rt) != 0 ||
        w4_db_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !w4_db_read_qwen3_layer_range_placement(svc,
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
             w4_db_qwen3_model_key(),
             local_node + 1U,
             local_placement.layer_start,
             local_placement.layer_end,
             previous_step);
    memset(&kv_state, 0, sizeof(kv_state));
    {
        struct w4_db_record *local_record = w4_db_find_record(svc, kv_state_key);

        if (local_record) {
            kv_state = *local_record;
        }
    }
    if (kv_state.kind != W4_DB_RECORD_KVCACHE_OBJECT ||
        kv_state.object_payload_kind != W4_DB_OBMM_KIND_QWEN3_KV_STATE ||
        kv_state.object_backing_len == 0 ||
        kv_state.object_backing_offset + kv_state.object_backing_len >
            local_slot->region.len) {
        printf("[w4_guest] gap qwen3_range_kv_state_resolve=missing local=node%u step=%" PRIu64 " key=%s\n",
               local_node + 1U,
               decode_step,
               kv_state_key);
        return -1;
    }
    checksum = kv_state.object_payload_checksum;
    view_out->data = (const uint8_t *)local_slot->region.addr +
                     kv_state.object_backing_offset;
    view_out->len = kv_state.object_backing_len;
    view_out->checksum = checksum;
    view_out->owner_node = local_node;
    view_out->payload_kind = kv_state.object_payload_kind;
    view_out->backing_offset = kv_state.object_backing_offset;
    if (w4_db_record_to_lingqu_obmm_ref(&kv_state, &view_out->object_ref) != 0) {
        return -1;
    }
    printf("[w4_guest] stage qwen3_range_kv_state_resolve local=node%u step=%" PRIu64 " previous_step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u kv_bytes=%" PRIu64 " kv_checksum=0x%016" PRIx64 " offset=0x%016" PRIx64 " validation=object_ref_metadata source=obmm_object_view backing=obmm_shmem metadata=lingqu_object_service target=mapped_view status=ok\n",
           local_node + 1U,
           decode_step,
           previous_step,
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

int w4_db_obmm_service_v0_resolve_previous_range_kv_state(struct w4_db_service *svc,
                                                          uint32_t local_node,
                                                          uint32_t cluster_node_count,
                                                          uint64_t decode_step,
                                                          uint8_t *payload_out,
                                                          uint64_t payload_capacity,
                                                          uint64_t *payload_len_out,
                                                          uint64_t *checksum_out)
{
    struct w4_db_object_payload_view view;

    if (!payload_len_out || !checksum_out) {
        return -1;
    }
    *payload_len_out = 0;
    *checksum_out = 0;
    if (!payload_out) {
        return -1;
    }
    if (w4_db_obmm_service_v0_resolve_previous_range_kv_state_view(
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
        printf("[w4_guest] gap qwen3_range_kv_state_resolve=payload_too_large local=node%u step=%" PRIu64 " bytes=%" PRIu64 " capacity=%" PRIu64 "\n",
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

static int w4_db_obmm_service_v0_publish_terminal_token_result_from_node(
    struct w4_db_service *svc,
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
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_cluster_slot *local_slot;
    struct w4_db_record local_token_result;
    struct w4_db_qwen3_layer_range_placement local_placement;
    char token_result_key[96];
    uint64_t payload_words[8];
    uint64_t token_result_offset;
    uint64_t checksum;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    uint8_t *base;

    if (!svc || cluster_node_count != W4_DB_QWEN3_RANGE_NODES ||
        local_node >= cluster_node_count) {
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0 ||
        w4_db_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !w4_db_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    if (require_terminal_node && !local_placement.terminal) {
        return 0;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        w4_db_payload_arena_alloc(rt,
                                  W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES,
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
    checksum = w4_db_checksum_bytes((const uint8_t *)payload_words,
                                    W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + token_result_offset, payload_words, W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES);
    if (w4_db_update_region_range_at(local_slot,
                                     token_result_offset,
                                     W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                                     true) != 0) {
        return -1;
    }
    (void)msync(base + token_result_offset, W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES, MS_SYNC);

    snprintf(token_result_key,
             sizeof(token_result_key),
             "tokens/%s/decode-step%" PRIu64,
             w4_db_qwen3_model_key(),
             decode_step);
    if (w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_QWEN3_TOKEN_RESULT,
                                     token_result_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_QWEN3_TOKEN_RESULT,
                                     token_result_offset,
                                     W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                                     checksum,
                                     &local_token_result) != 0 ||
        w4_db_write_cluster_payload(svc, local_slot) != 0) {
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
    if (w4_db_push_obmm_object_descs(rt,
                                     W4_DB_OBMM_KIND_QWEN3_TOKEN_RESULT,
                                     local_token_result.object_backing_offset,
                                     local_token_result.object_backing_len,
                                     local_token_result.object_payload_checksum,
                                     object_epoch) != 0) {
        return -1;
    }
    printf("[w4_guest] stage qwen3_terminal_token_result_publish local=node%u step=%" PRIu64 " token=%" PRIu64 " runner_up=%" PRIu64 " margin_milli=%" PRIu64 " logits_checksum=0x%016" PRIx64 " text_checksum=0x%016" PRIx64 " piece_word0=0x%016" PRIx64 " piece_word1=0x%016" PRIx64 " object_key=%s offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " epoch=%u seq=%u backing=obmm_pool metadata=db queue=obmm_spsc status=ok publisher=%s\n",
           local_node + 1U,
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
           require_terminal_node ? "terminal_node" : "shortpath_boundary");
    return 0;
}

int w4_db_obmm_service_v0_publish_terminal_token_result(struct w4_db_service *svc,
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
    return w4_db_obmm_service_v0_publish_terminal_token_result_from_node(
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

int w4_db_obmm_service_v0_publish_shortpath_terminal_token_result(
    struct w4_db_service *svc,
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
    return w4_db_obmm_service_v0_publish_terminal_token_result_from_node(
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

static uint64_t w4_db_pack_qwen3_engram_candidates(uint64_t decode_step,
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
    candidate_words[31] = w4_db_checksum_bytes((const uint8_t *)candidate_words,
                                               31U * sizeof(uint64_t));
    return packed_count;
}

int w4_db_obmm_service_v0_publish_engram_candidates(struct w4_db_service *svc,
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
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_cluster_slot *local_slot;
    struct w4_db_qwen3_layer_range_placement local_placement;
    struct w4_db_record candidates_record;
    char candidates_key[96];
    uint64_t candidate_words[32];
    uint64_t candidates_offset;
    uint64_t candidates_bytes = W4_DB_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES;
    uint64_t candidates_checksum;
    uint64_t packed_count;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    uint8_t *base;

    if (!svc || cluster_node_count != W4_DB_QWEN3_RANGE_NODES ||
        local_node >= cluster_node_count || !candidate_tokens || candidate_count == 0) {
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0 ||
        w4_db_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !w4_db_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    if (!local_placement.terminal) {
        return 0;
    }

    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        w4_db_payload_arena_alloc(rt,
                                  candidates_bytes,
                                  64,
                                  &candidates_offset) != 0) {
        return -1;
    }
    packed_count = w4_db_pack_qwen3_engram_candidates(decode_step,
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
        w4_db_checksum_bytes((const uint8_t *)candidate_words, candidates_bytes);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + candidates_offset, candidate_words, candidates_bytes);
    if (w4_db_update_region_range_at(local_slot, candidates_offset, candidates_bytes, true) !=
        0) {
        return -1;
    }
    (void)msync(base + candidates_offset, candidates_bytes, MS_SYNC);

    w4_db_qwen3_engram_candidates_key(decode_step,
                                      candidates_key,
                                      sizeof(candidates_key));
    if (w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_QWEN3_ENGRAM_CANDIDATES,
                                     candidates_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                                     candidates_offset,
                                     candidates_bytes,
                                     candidates_checksum,
                                     &candidates_record) != 0 ||
        w4_db_write_cluster_payload(svc, local_slot) != 0) {
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
    if (w4_db_push_obmm_object_descs(rt,
                                     candidates_record.object_payload_kind,
                                     candidates_record.object_backing_offset,
                                     candidates_record.object_backing_len,
                                     candidates_record.object_payload_checksum,
                                     object_epoch) != 0) {
        return -1;
    }

    printf("[w4_guest] stage qwen3_engram_candidates_publish local=node%u step=%" PRIu64
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

int w4_db_obmm_service_v0_publish_engram_step(struct w4_db_service *svc,
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
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_cluster_slot *local_slot;
    struct w4_db_record records[3];
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
    uint64_t selected_bytes = W4_DB_OBMM_QWEN3_ENGRAM_SELECTED_BYTES;
    uint64_t state_bytes = W4_DB_OBMM_QWEN3_ENGRAM_STATE_BYTES;
    uint64_t history_checksum;
    uint64_t selected_checksum;
    uint64_t state_checksum;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    int owner_idx;
    uint8_t *base;

    if (!svc || cluster_node_count != W4_DB_QWEN3_RANGE_NODES ||
        local_node >= cluster_node_count || !history_tokens ||
        history_token_count > 1024U) {
        return -1;
    }
    if (history_token_count == UINT64_MAX || history_token_count + 1U > 1024U) {
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0 ||
        w4_db_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0) {
        return -1;
    }
    owner_idx = w4_db_qwen3_engram_owner_index(cluster_node_count);
    if (owner_idx < 0 || (uint32_t)owner_idx != local_node) {
        return 0;
    }

    local_slot = &rt->slots[rt->local_idx];
    published_history_token_count = history_token_count + 1U;
    history_bytes = (published_history_token_count + 2U) * sizeof(uint64_t);
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        history_bytes > W4_DB_OBMM_QWEN3_ENGRAM_HISTORY_BYTES ||
        w4_db_payload_arena_alloc(rt, history_bytes, 64, &history_offset) != 0 ||
        w4_db_payload_arena_alloc(rt, selected_bytes, 64, &selected_offset) != 0 ||
        w4_db_payload_arena_alloc(rt, state_bytes, 64, &state_offset) != 0) {
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

    history_checksum = w4_db_checksum_bytes((const uint8_t *)history_words, history_bytes);
    selected_checksum = w4_db_checksum_bytes((const uint8_t *)selected_words, selected_bytes);

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
    state_words[15] = w4_db_checksum_bytes((const uint8_t *)state_words,
                                           15U * sizeof(uint64_t));
    state_checksum = w4_db_checksum_bytes((const uint8_t *)state_words, state_bytes);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + history_offset, history_words, history_bytes);
    memcpy(base + selected_offset, selected_words, selected_bytes);
    memcpy(base + state_offset, state_words, state_bytes);
    if (w4_db_update_region_range_at(local_slot, history_offset, history_bytes, true) != 0 ||
        w4_db_update_region_range_at(local_slot, selected_offset, selected_bytes, true) != 0 ||
        w4_db_update_region_range_at(local_slot, state_offset, state_bytes, true) != 0) {
        return -1;
    }
    (void)msync(base + history_offset, history_bytes, MS_SYNC);
    (void)msync(base + selected_offset, selected_bytes, MS_SYNC);
    (void)msync(base + state_offset, state_bytes, MS_SYNC);

    w4_db_qwen3_engram_history_key(history_key, sizeof(history_key));
    w4_db_qwen3_engram_selected_key(decode_step, selected_key, sizeof(selected_key));
    w4_db_qwen3_engram_state_key(decode_step, state_key, sizeof(state_key));

    if (w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_QWEN3_ENGRAM_HISTORY,
                                     history_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                                     history_offset,
                                     history_bytes,
                                     history_checksum,
                                     &records[0]) != 0 ||
        w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_QWEN3_ENGRAM_SELECTED,
                                     selected_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                                     selected_offset,
                                     selected_bytes,
                                     selected_checksum,
                                     &records[1]) != 0 ||
        w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_QWEN3_ENGRAM_STATE,
                                     state_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_QWEN3_ENGRAM_STATE,
                                     state_offset,
                                     state_bytes,
                                     state_checksum,
                                     &records[2]) != 0 ||
        w4_db_write_cluster_payload(svc, local_slot) != 0) {
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
        if (w4_db_push_obmm_object_descs(rt,
                                         records[i].object_payload_kind,
                                         records[i].object_backing_offset,
                                         records[i].object_backing_len,
                                         records[i].object_payload_checksum,
                                         object_epoch) != 0) {
            return -1;
        }
    }

    printf("[w4_guest] stage qwen3_engram_decision_publish local=node%u step=%" PRIu64
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

int w4_db_obmm_service_v0_wait_terminal_token_result(struct w4_db_service *svc,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms,
                                                     uint64_t *sampled_token_out)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    char token_result_key[96];
    long deadline;

    if (!svc) {
        return -1;
    }
    snprintf(token_result_key,
             sizeof(token_result_key),
             "tokens/%s/decode-step%" PRIu64,
             w4_db_qwen3_model_key(),
             decode_step);
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        if (w4_db_cluster_runtime_init(rt) == 0) {
            for (int owner_idx = 0; owner_idx < rt->node_count; ++owner_idx) {
                struct w4_db_cluster_payload_compact_summary compact;
                struct w4_db_cluster_payload_header seen;
                struct w4_db_record token_record;
                uint64_t payload_words[8];
                uint64_t checksum;
                struct w4_db_cluster_slot *owner_slot;

                if (owner_idx != rt->local_idx &&
                    w4_db_activate_remote_slot(rt, owner_idx) != 0) {
                    continue;
                }
                owner_slot = &rt->slots[owner_idx];
                if (owner_slot->region.addr &&
                    w4_db_try_read_stable_compact_summary_region(owner_slot,
                                                                 &compact,
                                                                 &seen) &&
                    w4_db_slot_find_record(owner_slot,
                                           token_result_key,
                                           &token_record) &&
                    token_record.kind == W4_DB_RECORD_QWEN3_TOKEN_RESULT &&
                    token_record.object_payload_kind ==
                        W4_DB_OBMM_KIND_QWEN3_TOKEN_RESULT &&
                    token_record.object_backing_len ==
                        W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES &&
                    token_record.object_backing_offset <= owner_slot->region.len &&
                    token_record.object_backing_len <=
                        owner_slot->region.len - token_record.object_backing_offset) {
                    memcpy(payload_words,
                           (uint8_t *)owner_slot->region.addr +
                               token_record.object_backing_offset,
                           W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                    if (payload_words[0] == decode_step) {
                        checksum = w4_db_checksum_bytes(
                            (const uint8_t *)payload_words,
                            W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                        if (checksum != token_record.object_payload_checksum) {
                            usleep(10000);
                            continue;
                        }
                        if (sampled_token_out) {
                            *sampled_token_out = payload_words[1];
                        }
                        printf("[w4_guest] stage qwen3_terminal_token_result_wait step=%" PRIu64
                               " object_key=%s owner=node%d offset=0x%016" PRIx64
                               " bytes=%" PRIu64
                               " token=%" PRIu64 " checksum=0x%016" PRIx64
                               " source=obmm_object_record status=ok\n",
                               decode_step,
                               token_result_key,
                               owner_idx + 1,
                               token_record.object_backing_offset,
                               (uint64_t)W4_DB_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                               payload_words[1],
                               checksum);
                        return 0;
                    }
                }
            }
        }
        usleep(10000);
    }
    printf("[w4_guest] gap qwen3_terminal_token_result_wait=timeout step=%" PRIu64
           " object_key=%s\n",
           decode_step,
           token_result_key);
    return -1;
}

int w4_db_obmm_service_v0_wait_engram_candidates(struct w4_db_service *svc,
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
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    char candidates_key[96];
    long deadline;
    int terminal_idx = W4_DB_QWEN3_RANGE_NODES - 1;

    if (!svc || !candidate_tokens_out || !candidate_count_out ||
        candidate_capacity == 0) {
        return -1;
    }
    *candidate_count_out = 0;
    if (candidate_checksum_out) {
        *candidate_checksum_out = 0;
    }
    w4_db_qwen3_engram_candidates_key(decode_step,
                                      candidates_key,
                                      sizeof(candidates_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct w4_db_cluster_slot *terminal_slot;
        struct w4_db_record candidates_record;
        uint64_t candidate_words[32];
        uint64_t candidate_count;
        uint64_t inner_checksum;
        uint64_t candidates_checksum;

        memset(&candidates_record, 0, sizeof(candidates_record));
        if (w4_db_cluster_runtime_init(rt) == 0 &&
            terminal_idx >= 0 &&
            terminal_idx < rt->node_count &&
            (terminal_idx == rt->local_idx ||
             w4_db_activate_remote_slot(rt, terminal_idx) == 0)) {
            terminal_slot = &rt->slots[terminal_idx];
            if (!w4_db_slot_find_record(terminal_slot, candidates_key, &candidates_record) ||
                candidates_record.kind != W4_DB_RECORD_QWEN3_ENGRAM_CANDIDATES ||
                candidates_record.version != 1U ||
                candidates_record.object_payload_kind != W4_DB_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES ||
                candidates_record.object_backing_len != W4_DB_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES ||
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
                   W4_DB_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES);
            inner_checksum = w4_db_checksum_bytes((const uint8_t *)candidate_words,
                                                  31U * sizeof(uint64_t));
            candidates_checksum = w4_db_checksum_bytes((const uint8_t *)candidate_words,
                                                       W4_DB_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES);
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
                printf("[w4_guest] stage qwen3_engram_candidates_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " candidate_count=%" PRIu64 " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       candidates_key,
                       terminal_idx + 1,
                       candidates_record.version,
                       candidate_count,
                       candidates_record.object_backing_len,
                       candidates_checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[w4_guest] gap qwen3_engram_candidates_wait=timeout step=%" PRIu64
           " object_key=%s\n",
           decode_step,
           candidates_key);
    return -1;
}

int w4_db_obmm_service_v0_wait_engram_selected_token(struct w4_db_service *svc,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms,
                                                     uint64_t *selected_token_out)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    char selected_key[96];
    long deadline;
    int owner_idx = w4_db_qwen3_engram_owner_index(W4_DB_QWEN3_RANGE_NODES);

    if (!svc) {
        return -1;
    }
    w4_db_qwen3_engram_selected_key(decode_step, selected_key, sizeof(selected_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct w4_db_cluster_slot *terminal_slot;
        struct w4_db_record selected_record;
        uint64_t payload_words[8];
        uint64_t checksum;

        memset(&selected_record, 0, sizeof(selected_record));
        if (w4_db_cluster_runtime_init(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count &&
            (owner_idx == rt->local_idx ||
             w4_db_activate_remote_slot(rt, owner_idx) == 0)) {
            terminal_slot = &rt->slots[owner_idx];
            if (!w4_db_slot_find_record(terminal_slot, selected_key, &selected_record) ||
                selected_record.kind != W4_DB_RECORD_QWEN3_ENGRAM_SELECTED ||
                selected_record.object_payload_kind != W4_DB_OBMM_KIND_QWEN3_ENGRAM_SELECTED ||
                selected_record.object_backing_len != W4_DB_OBMM_QWEN3_ENGRAM_SELECTED_BYTES ||
                !terminal_slot->region.addr ||
                selected_record.object_backing_offset + selected_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memcpy(payload_words,
                   (uint8_t *)terminal_slot->region.addr +
                       selected_record.object_backing_offset,
                   W4_DB_OBMM_QWEN3_ENGRAM_SELECTED_BYTES);
            checksum = w4_db_checksum_bytes((const uint8_t *)payload_words,
                                            W4_DB_OBMM_QWEN3_ENGRAM_SELECTED_BYTES);
            if (payload_words[0] == decode_step &&
                checksum == selected_record.object_payload_checksum) {
                if (selected_token_out) {
                    *selected_token_out = payload_words[1];
                }
                printf("[w4_guest] stage qwen3_engram_selected_token_wait step=%" PRIu64
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
    printf("[w4_guest] gap qwen3_engram_selected_token_wait=timeout step=%" PRIu64
           " object_key=%s\n",
           decode_step,
           selected_key);
    return -1;
}

int w4_db_obmm_service_v0_wait_engram_history(struct w4_db_service *svc,
                                              uint64_t decode_step,
                                              uint64_t timeout_ms,
                                              uint64_t *history_tokens_out,
                                              uint64_t history_token_capacity,
                                              uint64_t *history_token_count_out,
                                              uint64_t *history_checksum_out)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    char history_key[96];
    long deadline;
    int owner_idx = w4_db_qwen3_engram_owner_index(W4_DB_QWEN3_RANGE_NODES);
    uint64_t expected_version = decode_step + 1U;

    if (!svc || !history_tokens_out || history_token_capacity == 0 ||
        !history_token_count_out) {
        return -1;
    }
    *history_token_count_out = 0;
    if (history_checksum_out) {
        *history_checksum_out = 0;
    }
    w4_db_qwen3_engram_history_key(history_key, sizeof(history_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct w4_db_cluster_slot *terminal_slot;
        struct w4_db_record history_record;
        uint64_t payload_words[1024 + 2];
        uint64_t checksum;
        uint64_t history_token_count;
        uint64_t expected_bytes;

        memset(&history_record, 0, sizeof(history_record));
        if (w4_db_cluster_runtime_init(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count &&
            (owner_idx == rt->local_idx ||
             w4_db_activate_remote_slot(rt, owner_idx) == 0)) {
            terminal_slot = &rt->slots[owner_idx];
            if (!w4_db_slot_find_record(terminal_slot, history_key, &history_record) ||
                history_record.kind != W4_DB_RECORD_QWEN3_ENGRAM_HISTORY ||
                history_record.version != expected_version ||
                history_record.object_payload_kind != W4_DB_OBMM_KIND_QWEN3_ENGRAM_HISTORY ||
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
            checksum = w4_db_checksum_bytes((const uint8_t *)payload_words,
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
                printf("[w4_guest] stage qwen3_engram_history_wait step=%" PRIu64
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
    printf("[w4_guest] gap qwen3_engram_history_wait=timeout step=%" PRIu64
           " object_key=%s expected_version=%" PRIu64 "\n",
           decode_step,
           history_key,
           expected_version);
    return -1;
}

int w4_db_obmm_service_v0_wait_engram_state(struct w4_db_service *svc,
                                            uint64_t decode_step,
                                            uint64_t timeout_ms,
                                            uint64_t expected_history_token_count,
                                            uint64_t expected_selected_token,
                                            uint64_t expected_history_checksum,
                                            uint64_t no_repeat_ngram_size,
                                            uint64_t repetition_penalty_milli,
                                            uint64_t *state_checksum_out)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    char state_key[96];
    long deadline;
    int owner_idx = w4_db_qwen3_engram_owner_index(W4_DB_QWEN3_RANGE_NODES);

    if (!svc || expected_history_token_count == 0) {
        return -1;
    }
    if (state_checksum_out) {
        *state_checksum_out = 0;
    }
    w4_db_qwen3_engram_state_key(decode_step, state_key, sizeof(state_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct w4_db_cluster_slot *terminal_slot;
        struct w4_db_record state_record;
        uint64_t state_words[16];
        uint64_t inner_checksum;
        uint64_t state_checksum;

        memset(&state_record, 0, sizeof(state_record));
        if (w4_db_cluster_runtime_init(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count &&
            (owner_idx == rt->local_idx ||
             w4_db_activate_remote_slot(rt, owner_idx) == 0)) {
            terminal_slot = &rt->slots[owner_idx];
            if (!w4_db_slot_find_record(terminal_slot, state_key, &state_record) ||
                state_record.kind != W4_DB_RECORD_QWEN3_ENGRAM_STATE ||
                state_record.version != 1U ||
                state_record.object_payload_kind != W4_DB_OBMM_KIND_QWEN3_ENGRAM_STATE ||
                state_record.object_backing_len != W4_DB_OBMM_QWEN3_ENGRAM_STATE_BYTES ||
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
                   W4_DB_OBMM_QWEN3_ENGRAM_STATE_BYTES);
            inner_checksum = w4_db_checksum_bytes((const uint8_t *)state_words,
                                                  15U * sizeof(uint64_t));
            state_checksum = w4_db_checksum_bytes((const uint8_t *)state_words,
                                                  W4_DB_OBMM_QWEN3_ENGRAM_STATE_BYTES);
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
                printf("[w4_guest] stage qwen3_engram_state_wait step=%" PRIu64
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
    printf("[w4_guest] gap qwen3_engram_state_wait=timeout step=%" PRIu64
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

int w4_db_obmm_service_v0_publish_decode_round_done(struct w4_db_service *svc,
                                                    uint32_t local_node,
                                                    uint32_t cluster_node_count,
                                                    uint64_t decode_step)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_cluster_slot *local_slot;
    uint64_t payload_words[8];
    uint64_t checksum;
    uint8_t *base;

    if (!svc || cluster_node_count != W4_DB_QWEN3_RANGE_NODES ||
        local_node >= cluster_node_count ||
        w4_db_cluster_runtime_init(rt) != 0) {
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        local_slot->region.len <
            W4_DB_OBMM_QWEN3_ROUND_DONE_OFFSET + W4_DB_OBMM_QWEN3_ROUND_DONE_BYTES) {
        return -1;
    }

    payload_words[0] = 0x71336465636f6465ULL;
    payload_words[1] = decode_step;
    payload_words[2] = local_node;
    payload_words[3] = cluster_node_count;
    payload_words[4] = rt->publish_seq;
    payload_words[5] = rt->observe_epoch;
    payload_words[6] = 0;
    payload_words[7] = w4_db_checksum_bytes((const uint8_t *)payload_words,
                                            7U * sizeof(payload_words[0]));
    checksum = w4_db_checksum_bytes((const uint8_t *)payload_words,
                                    W4_DB_OBMM_QWEN3_ROUND_DONE_BYTES);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + W4_DB_OBMM_QWEN3_ROUND_DONE_OFFSET,
           payload_words,
           W4_DB_OBMM_QWEN3_ROUND_DONE_BYTES);
    if (w4_db_update_region_range_at(local_slot,
                                     W4_DB_OBMM_QWEN3_ROUND_DONE_OFFSET,
                                     W4_DB_OBMM_QWEN3_ROUND_DONE_BYTES,
                                     true) != 0) {
        return -1;
    }
    (void)msync(base + W4_DB_OBMM_QWEN3_ROUND_DONE_OFFSET,
                W4_DB_OBMM_QWEN3_ROUND_DONE_BYTES,
                MS_SYNC);
    printf("[w4_guest] stage qwen3_decode_round_done_publish local=node%u step=%" PRIu64
           " offset=0x%016" PRIx64 " bytes=%" PRIu64
           " checksum=0x%016" PRIx64 " backing=obmm_pool status=ok\n",
           local_node + 1U,
           decode_step,
           (uint64_t)W4_DB_OBMM_QWEN3_ROUND_DONE_OFFSET,
           (uint64_t)W4_DB_OBMM_QWEN3_ROUND_DONE_BYTES,
           checksum);
    w4_db_report_obmm_pool_usage(rt, local_node, decode_step);
    return 0;
}

int w4_db_obmm_service_v0_wait_all_decode_round_done(struct w4_db_service *svc,
                                                     uint32_t cluster_node_count,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    long deadline;
    uint32_t ready_mask = 0;
    uint32_t expected_mask;

    if (!svc || cluster_node_count != W4_DB_QWEN3_RANGE_NODES ||
        w4_db_cluster_runtime_init(rt) != 0) {
        return -1;
    }
    expected_mask = (1U << cluster_node_count) - 1U;
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        ready_mask = 0;
        for (uint32_t i = 0; i < cluster_node_count; ++i) {
            struct w4_db_cluster_slot *slot;
            uint64_t payload_words[8];

            if ((int)i != rt->local_idx &&
                w4_db_activate_remote_slot(rt, (int)i) != 0) {
                continue;
            }
            slot = &rt->slots[i];
            if (!slot->region.addr ||
                slot->region.len <
                    W4_DB_OBMM_QWEN3_ROUND_DONE_OFFSET + W4_DB_OBMM_QWEN3_ROUND_DONE_BYTES) {
                continue;
            }
            memcpy(payload_words,
                   (uint8_t *)slot->region.addr + W4_DB_OBMM_QWEN3_ROUND_DONE_OFFSET,
                   W4_DB_OBMM_QWEN3_ROUND_DONE_BYTES);
            if (payload_words[0] == 0x71336465636f6465ULL &&
                payload_words[1] == decode_step &&
                payload_words[2] == i &&
                payload_words[3] == cluster_node_count &&
                payload_words[7] ==
                    w4_db_checksum_bytes((const uint8_t *)payload_words,
                                         7U * sizeof(payload_words[0]))) {
                ready_mask |= 1U << i;
            }
        }
        if (ready_mask == expected_mask) {
            printf("[w4_guest] stage qwen3_decode_round_barrier step=%" PRIu64
                   " nodes=%u ready_mask=0x%02x status=ok\n",
                   decode_step,
                   cluster_node_count,
                   ready_mask);
            return 0;
        }
        usleep(10000);
    }
    printf("[w4_guest] gap qwen3_decode_round_barrier=timeout step=%" PRIu64
           " nodes=%u ready_mask=0x%02x expected_mask=0x%02x\n",
           decode_step,
           cluster_node_count,
           ready_mask,
           expected_mask);
    return -1;
}
