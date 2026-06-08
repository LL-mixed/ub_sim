/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Minimal GVA Manager bootstrap for GSVA aperture agreement.
 *
 * This is the first executable control-plane component for GSVA simulation:
 * every node exports one OBMM manager control region, exchanges descriptors
 * through OBMM bootstrap, imports peer regions, and runs a proposal/accept/
 * commit protocol over OBMM shared-memory MPMC queues.
 */

#define _GNU_SOURCE
#include "obmm_common.h"
#include "obmm_mpmc_queue.h"
#include "obmm_spsc_queue.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "[gva_manager]"
#define MAX_NODES OBMM_POOL_HELPERS_MAX_NODES
#define GVA_MGR_MAGIC 0x4756414dU
#define GVA_MGR_VERSION 1U
#define GVA_MGR_REGION_SIZE (2UL * 1024UL * 1024UL)
#define GVA_MGR_QUEUE_DEPTH OBMM_QUEUE_MIN_DEPTH
#define GVA_MGR_DEFAULT_APERTURE_BASE 0x700000000000ULL
#define GVA_MGR_DEFAULT_APERTURE_SIZE (16UL * 1024UL * 1024UL)
#define GVA_MGR_DEFAULT_SEGMENT_SIZE (4UL * 1024UL * 1024UL)
#define GVA_MGR_DEFAULT_SEGMENT_ALIGNMENT 4096ULL
#define GVA_MGR_TIMEOUT_MS 90000
#define GVA_MGR_PAYLOAD_SLOTS 64U
#define GVA_MGR_BROADCAST_RETRIES 3

enum gva_mgr_segment_state {
    GVA_MGR_SEGMENT_PROPOSED = 1,
    GVA_MGR_SEGMENT_ACTIVE = 2,
    GVA_MGR_SEGMENT_RETIRED = 3,
};

enum gva_mgr_msg_type {
    GVA_MGR_MSG_HELLO = 1,
    GVA_MGR_MSG_APERTURE_PROPOSE = 2,
    GVA_MGR_MSG_APERTURE_ACCEPT = 3,
    GVA_MGR_MSG_APERTURE_COMMIT = 4,
    GVA_MGR_MSG_SEGMENT_ANNOUNCE = 5,
    GVA_MGR_MSG_SEGMENT_ACK = 6,
    GVA_MGR_MSG_SEGMENT_RETIRE = 7,
    GVA_MGR_MSG_SEGMENT_RETIRED_ACK = 8,
    GVA_MGR_MSG_HEARTBEAT = 9,
    GVA_MGR_MSG_ERROR = 10,
};

struct gva_mgr_msg_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint64_t generation;
    uint64_t seq;
    uint32_t src_node;
    uint32_t dst_node;
    uint32_t payload_len;
    uint32_t crc32;
};

struct gva_mgr_aperture_msg {
    struct gva_mgr_msg_hdr hdr;
    uint64_t aperture_base;
    uint64_t aperture_size;
    uint64_t node_stride;
    uint64_t forbidden_hash;
    uint64_t segment_id;
    uint64_t segment_base;
    uint64_t segment_size;
    uint32_t home_node_id;
    uint32_t access_flags;
    uint32_t cache_policy;
    uint32_t segment_state;
    uint32_t status;
    uint32_t reserved;
};

struct gva_mgr_config {
    bool bootstrap;
    bool dump_routes;
    bool allocate_segment;
    bool retire_segment;
    bool reuse_segment;
    bool desc_alloc;
    bool desc_query;
    bool desc_retire;
    int node_id;
    int node_count;
    uint64_t generation;
    uint64_t aperture_base;
    uint64_t aperture_size;
    uint64_t segment_size;
    uint64_t segment_alignment;
    int home_node_id;
    uint32_t access_flags;
    uint32_t cache_policy;
    bool inject_conflict;
    uint64_t requested_home_va;
    uint64_t query_home_va;
    uint64_t segment_id;
    uint64_t epoch;
    uint32_t requested_p_tag;
    uint32_t retire_timeout_ms;
};

struct node_slot {
    int owner_idx;
    bool is_local;
    uint64_t mem_id;
    uint64_t local_pa;
    struct obmm_helpers_region region;
    struct obmm_spsc_queue *ingress_queue[MAX_NODES];
    uint32_t tx_arena_region_id;
    uint8_t *tx_arena;
    uint64_t tx_arena_size;
};

static volatile sig_atomic_t g_alarm_fired;

static void alarm_handler(int signo)
{
    (void)signo;
    g_alarm_fired = 1;
}

static void log_msg(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, TAG " ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static int dump_gva_routes(void)
{
    FILE *fp;
    char line[512];
    unsigned int count = 0;

    fp = fopen("/proc/ub_sim_decoder/gva_routes", "r");
    if (!fp) {
        log_msg("dump-routes failed path=/proc/ub_sim_decoder/gva_routes errno=%d",
                errno);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);

        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        log_msg("route %s", line);
        count++;
    }
    fclose(fp);
    log_msg("result=done action=dump-routes lines=%u", count);
    return 0;
}

static void log_kernel_aperture_proc(void)
{
    FILE *fp;
    char header[160];
    char value[160];
    size_t len;

    fp = fopen("/proc/obmm/gsva_aperture", "r");
    if (!fp) {
        log_msg("kernel aperture proc unavailable errno=%d", errno);
        return;
    }
    if (!fgets(header, sizeof(header), fp) ||
        !fgets(value, sizeof(value), fp)) {
        fclose(fp);
        log_msg("kernel aperture proc read failed errno=%d", errno);
        return;
    }
    fclose(fp);
    len = strlen(value);
    if (len > 0 && value[len - 1] == '\n')
        value[len - 1] = '\0';
    log_msg("kernel aperture proc -> %s", value);
}

static bool parse_u64_str(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    if (!s || s[0] == '\0')
        return false;
    errno = 0;
    value = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0')
        return false;
    *out = (uint64_t)value;
    return true;
}

static bool parse_i32_str(const char *s, int *out)
{
    char *end = NULL;
    long value;

    if (!s || s[0] == '\0')
        return false;
    errno = 0;
    value = strtol(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' ||
        value < INT32_MIN || value > INT32_MAX)
        return false;
    *out = (int)value;
    return true;
}

static bool parse_cache_policy_str(const char *s, uint32_t *out)
{
    if (strcmp(s, "nc") == 0) {
        *out = OBMM_SIM_DEC_CACHE_POLICY_NC;
    } else if (strcmp(s, "wt") == 0 || strcmp(s, "write-through") == 0) {
        *out = OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH;
    } else if (strcmp(s, "rc") == 0 || strcmp(s, "read-cache") == 0) {
        *out = OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE;
    } else if (strcmp(s, "wb") == 0 || strcmp(s, "write-back") == 0) {
        *out = OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK;
    } else if (strcmp(s, "mesi") == 0 || strcmp(s, "directory-mesi") == 0) {
        *out = OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI;
    } else {
        return false;
    }
    return true;
}

static uint64_t default_generation(void)
{
    uint64_t gen = 1469598103934665603ULL;
    const char *session = getenv("GVA_MANAGER_SESSION");

    if (!session || session[0] == '\0')
        session = "default";
    while (*session) {
        gen ^= (unsigned char)*session++;
        gen *= 1099511628211ULL;
    }
    return gen ? gen : 1;
}

static void config_from_env_cmdline(struct gva_mgr_config *cfg)
{
    char value[128];
    uint64_t parsed64;
    int parsed32;

    if (obmm_env_or_cmdline("GVA_MANAGER_NODE_ID", "gva_manager_node_id",
                            value, sizeof(value)) &&
        parse_i32_str(value, &parsed32)) {
        cfg->node_id = parsed32;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_NODE_COUNT", "gva_manager_node_count",
                            value, sizeof(value)) &&
        parse_i32_str(value, &parsed32)) {
        cfg->node_count = parsed32;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_GENERATION", "gva_manager_generation",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64)) {
        cfg->generation = parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_APERTURE_BASE",
                            "gva_manager_aperture_base",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64)) {
        cfg->aperture_base = parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_APERTURE_SIZE",
                            "gva_manager_aperture_size",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64)) {
        cfg->aperture_size = parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_ALLOCATE_SEGMENT",
                            "gva_manager_allocate_segment",
                            value, sizeof(value)) &&
        parse_i32_str(value, &parsed32)) {
        cfg->allocate_segment = parsed32 != 0;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_RETIRE_SEGMENT",
                            "gva_manager_retire_segment",
                            value, sizeof(value)) &&
        parse_i32_str(value, &parsed32)) {
        cfg->retire_segment = parsed32 != 0;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_REUSE_SEGMENT",
                            "gva_manager_reuse_segment",
                            value, sizeof(value)) &&
        parse_i32_str(value, &parsed32)) {
        cfg->reuse_segment = parsed32 != 0;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_SEGMENT_SIZE",
                            "gva_manager_segment_size",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64)) {
        cfg->segment_size = parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_SEGMENT_ALIGNMENT",
                            "gva_manager_segment_alignment",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64)) {
        cfg->segment_alignment = parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_HOME_NODE",
                            "gva_manager_home_node",
                            value, sizeof(value)) &&
        parse_i32_str(value, &parsed32)) {
        cfg->home_node_id = parsed32;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_CACHE_POLICY",
                            "gva_manager_cache_policy",
                            value, sizeof(value)) &&
        !parse_cache_policy_str(value, &cfg->cache_policy)) {
        cfg->cache_policy = UINT32_MAX;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_ACCESS_FLAGS",
                            "gva_manager_access_flags",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64) && parsed64 <= UINT32_MAX) {
        cfg->access_flags = (uint32_t)parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_CONFLICT",
                            "gva_manager_conflict",
                            value, sizeof(value)) &&
        parse_i32_str(value, &parsed32)) {
        cfg->inject_conflict = parsed32 != 0;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_REQUESTED_HOME_VA",
                            "gva_manager_requested_home_va",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64)) {
        cfg->requested_home_va = parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_SEGMENT_ID",
                            "gva_manager_segment_id",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64)) {
        cfg->segment_id = parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_QUERY_HOME_VA",
                            "gva_manager_query_home_va",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64)) {
        cfg->query_home_va = parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_EPOCH",
                            "gva_manager_epoch",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64)) {
        cfg->epoch = parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_REQUESTED_P_TAG",
                            "gva_manager_requested_p_tag",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64) && parsed64 <= UINT32_MAX) {
        cfg->requested_p_tag = (uint32_t)parsed64;
    }
    if (obmm_env_or_cmdline("GVA_MANAGER_RETIRE_TIMEOUT_MS",
                            "gva_manager_retire_timeout_ms",
                            value, sizeof(value)) &&
        parse_u64_str(value, &parsed64) && parsed64 <= UINT32_MAX) {
        cfg->retire_timeout_ms = (uint32_t)parsed64;
    }
}

static int parse_args(int argc, char **argv, struct gva_mgr_config *cfg)
{
    int i;

    memset(cfg, 0, sizeof(*cfg));
    cfg->bootstrap = true;
    cfg->node_id = -1;
    cfg->node_count = 2;
    cfg->generation = default_generation();
    cfg->aperture_base = GVA_MGR_DEFAULT_APERTURE_BASE;
    cfg->aperture_size = GVA_MGR_DEFAULT_APERTURE_SIZE;
    cfg->segment_size = GVA_MGR_DEFAULT_SEGMENT_SIZE;
    cfg->segment_alignment = GVA_MGR_DEFAULT_SEGMENT_ALIGNMENT;
    cfg->home_node_id = 0;
    cfg->cache_policy = OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH;
    cfg->epoch = 1;
    cfg->requested_p_tag = OBMM_GSVA_P_TAG_AUTO;
    cfg->retire_timeout_ms = 5000;

    config_from_env_cmdline(cfg);

    for (i = 1; i < argc; i++) {
        uint64_t parsed64;
        int parsed32;

        if (strcmp(argv[i], "--bootstrap") == 0) {
            cfg->bootstrap = true;
        } else if (strcmp(argv[i], "--alloc") == 0) {
            cfg->bootstrap = false;
            cfg->desc_alloc = true;
        } else if (strcmp(argv[i], "--query") == 0) {
            cfg->bootstrap = false;
            cfg->desc_query = true;
        } else if (strcmp(argv[i], "--retire") == 0) {
            cfg->bootstrap = false;
            cfg->desc_retire = true;
        } else if (strcmp(argv[i], "--allocate-segment") == 0) {
            cfg->bootstrap = true;
            cfg->allocate_segment = true;
        } else if (strcmp(argv[i], "--retire-segment") == 0) {
            cfg->bootstrap = true;
            cfg->allocate_segment = true;
            cfg->retire_segment = true;
        } else if (strcmp(argv[i], "--reuse-segment") == 0) {
            cfg->bootstrap = true;
            cfg->allocate_segment = true;
            cfg->retire_segment = true;
            cfg->reuse_segment = true;
        } else if (strcmp(argv[i], "--dump-routes") == 0) {
            cfg->bootstrap = false;
            cfg->dump_routes = true;
        } else if (strcmp(argv[i], "--node-id") == 0 && i + 1 < argc) {
            if (!parse_i32_str(argv[++i], &parsed32))
                return -EINVAL;
            cfg->node_id = parsed32;
        } else if (strcmp(argv[i], "--node-count") == 0 && i + 1 < argc) {
            if (!parse_i32_str(argv[++i], &parsed32))
                return -EINVAL;
            cfg->node_count = parsed32;
        } else if (strcmp(argv[i], "--generation") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64))
                return -EINVAL;
            cfg->generation = parsed64;
        } else if (strcmp(argv[i], "--aperture-base") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64))
                return -EINVAL;
            cfg->aperture_base = parsed64;
        } else if (strcmp(argv[i], "--aperture-size") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64))
                return -EINVAL;
            cfg->aperture_size = parsed64;
        } else if (strcmp(argv[i], "--segment-size") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64))
                return -EINVAL;
            cfg->segment_size = parsed64;
        } else if (strcmp(argv[i], "--segment-alignment") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64))
                return -EINVAL;
            cfg->segment_alignment = parsed64;
        } else if (strcmp(argv[i], "--home-node") == 0 && i + 1 < argc) {
            if (!parse_i32_str(argv[++i], &parsed32))
                return -EINVAL;
            cfg->home_node_id = parsed32;
        } else if (strcmp(argv[i], "--access-flags") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64) || parsed64 > UINT32_MAX)
                return -EINVAL;
            cfg->access_flags = (uint32_t)parsed64;
        } else if (strcmp(argv[i], "--requested-home-va") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64))
                return -EINVAL;
            cfg->requested_home_va = parsed64;
        } else if (strcmp(argv[i], "--home-va") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64))
                return -EINVAL;
            cfg->query_home_va = parsed64;
        } else if (strcmp(argv[i], "--segment-id") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64))
                return -EINVAL;
            cfg->segment_id = parsed64;
        } else if (strcmp(argv[i], "--epoch") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64))
                return -EINVAL;
            cfg->epoch = parsed64;
        } else if (strcmp(argv[i], "--p-tag") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64) || parsed64 > UINT32_MAX)
                return -EINVAL;
            cfg->requested_p_tag = (uint32_t)parsed64;
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            if (!parse_u64_str(argv[++i], &parsed64) || parsed64 > UINT32_MAX)
                return -EINVAL;
            cfg->retire_timeout_ms = (uint32_t)parsed64;
        } else if (strcmp(argv[i], "--cache-policy") == 0 && i + 1 < argc) {
            if (!parse_cache_policy_str(argv[++i], &cfg->cache_policy))
                return -EINVAL;
        } else if (strcmp(argv[i], "--conflict") == 0) {
            cfg->inject_conflict = true;
        } else {
            fprintf(stderr,
                    "usage: gva_manager --bootstrap --node-id N --node-count C "
                    "[--generation G] [--aperture-base A] "
                    "[--aperture-size S] [--conflict]\n"
                    "       gva_manager --alloc --node-id N --node-count C "
                    "[--aperture-base A] [--aperture-size S] [--requested-home-va VA] "
                    "[--segment-size S] [--segment-alignment A] [--cache-policy POLICY] "
                    "[--access-flags F] [--p-tag P]\n"
                    "       gva_manager --query [--segment-id ID|--home-va VA]\n"
                    "       gva_manager --retire --segment-id ID [--epoch E] [--timeout-ms MS]\n"
                    "       gva_manager --allocate-segment --node-id N "
                    "--node-count C [--home-node N] [--segment-size S] "
                    "[--segment-alignment A] [--cache-policy nc|wt|rc|wb|mesi|directory-mesi] "
                    "[--access-flags F] [--retire-segment] [--reuse-segment]\n"
                    "       gva_manager --dump-routes\n");
            return -EINVAL;
        }
    }

    if (cfg->dump_routes)
        return 0;
    if (cfg->desc_alloc || cfg->desc_query || cfg->desc_retire) {
        int actions = (cfg->desc_alloc ? 1 : 0) + (cfg->desc_query ? 1 : 0) +
                      (cfg->desc_retire ? 1 : 0);

        if (actions != 1)
            return -EINVAL;
        if (cfg->desc_alloc) {
            if (cfg->node_id < 0)
                cfg->node_id = 0;
            if (cfg->node_count < 1 || cfg->node_count > MAX_NODES ||
                cfg->home_node_id < 0 || cfg->home_node_id >= cfg->node_count ||
                cfg->segment_size == 0 || cfg->segment_alignment == 0 ||
                (cfg->segment_size & (4096ULL - 1)) != 0 ||
                (cfg->segment_alignment & (cfg->segment_alignment - 1)) != 0)
                return -EINVAL;
            if (cfg->access_flags == 0)
                cfg->access_flags = OBMM_GSVA_ACCESS_READ |
                                    OBMM_GSVA_ACCESS_WRITE;
            if (!(cfg->access_flags & OBMM_GSVA_ACCESS_READ))
                return -EINVAL;
        }
        if (cfg->desc_query && !cfg->segment_id && !cfg->query_home_va)
            return -EINVAL;
        if (cfg->desc_retire && (!cfg->segment_id || !cfg->epoch))
            return -EINVAL;
        if (cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_NC &&
            cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH &&
            cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE &&
            cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK &&
            cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI)
            return -EINVAL;
        return 0;
    }
    if (!cfg->bootstrap)
        return -EINVAL;
    if (cfg->reuse_segment)
        cfg->retire_segment = true;
    if (cfg->retire_segment || cfg->reuse_segment)
        cfg->allocate_segment = true;
    if (cfg->node_id < 0 || cfg->node_id >= cfg->node_count)
        return -EINVAL;
    if (cfg->node_count < 2 || cfg->node_count > MAX_NODES)
        return -EINVAL;
    if (cfg->aperture_size == 0 ||
        (cfg->aperture_base & (4096ULL - 1)) != 0 ||
        (cfg->aperture_size & (4096ULL - 1)) != 0)
        return -EINVAL;
    if (cfg->allocate_segment &&
        (cfg->home_node_id < 0 || cfg->home_node_id >= cfg->node_count ||
         cfg->segment_size == 0 || cfg->segment_alignment == 0 ||
         (cfg->segment_size & (4096ULL - 1)) != 0 ||
         (cfg->segment_alignment & (cfg->segment_alignment - 1)) != 0))
        return -EINVAL;
    if (cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_NC &&
        cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH &&
        cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE &&
        cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK &&
        cfg->cache_policy != OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI)
        return -EINVAL;
    if (cfg->cache_policy == OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE &&
        !(cfg->access_flags & OBMM_SIM_DEC_ACCESS_READ_ONLY))
        return -EINVAL;
    if (cfg->cache_policy == OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK &&
        !(cfg->access_flags & OBMM_SIM_DEC_ACCESS_EXPLICIT_SYNC))
        return -EINVAL;
    return 0;
}

static uint64_t layout_queue_offset(int peer_idx, int node_count)
{
    uint64_t dir_size = (uint64_t)node_count * sizeof(struct obmm_region_dirent);
    uint64_t queues_base = obmm_align_up_u64(64 + dir_size, 64);

    return queues_base + (uint64_t)peer_idx *
           obmm_queue_region_size(GVA_MGR_QUEUE_DEPTH);
}

static uint64_t layout_tx_arena_offset(int node_count)
{
    uint64_t queues_end = layout_queue_offset(node_count - 1, node_count);

    return obmm_align_up_u64(queues_end, 64);
}

static uint64_t layout_tx_arena_size(int node_count)
{
    uint64_t off = layout_tx_arena_offset(node_count);

    if (off >= GVA_MGR_REGION_SIZE)
        return 0;
    return GVA_MGR_REGION_SIZE - off;
}

static int init_manager_region(void *base, int node_id, int node_count)
{
    struct obmm_pool_header *hdr = (struct obmm_pool_header *)base;
    struct obmm_region_dirent *dir;
    int di = 0;
    int peer;

    memset(base, 0, GVA_MGR_REGION_SIZE);
    hdr->magic = OBMM_POOL_MAGIC;
    hdr->layout_version = OBMM_POOL_LAYOUT_VERSION;
    hdr->node_id = (uint16_t)node_id;
    hdr->node_count = (uint16_t)node_count;
    hdr->region_size = GVA_MGR_REGION_SIZE;
    hdr->directory_offset = 64;
    hdr->directory_count = (uint32_t)node_count;
    hdr->default_queue_depth = GVA_MGR_QUEUE_DEPTH;
    atomic_store_explicit(&hdr->state, OBMM_POOL_STATE_INIT,
                          memory_order_relaxed);
    atomic_store_explicit(&hdr->generation, 0, memory_order_relaxed);

    dir = (struct obmm_region_dirent *)((uint8_t *)base + 64);
    for (peer = 0; peer < node_count; peer++) {
        int peer_slot;

        if (peer == node_id)
            continue;
        peer_slot = (peer < node_id) ? peer : peer - 1;
        dir[di].region_id = (uint32_t)peer_slot;
        dir[di].kind = OBMM_REGION_QUEUE;
        dir[di].peer_node_id = (uint16_t)peer;
        dir[di].offset = layout_queue_offset(peer_slot, node_count);
        dir[di].size = obmm_queue_region_size(GVA_MGR_QUEUE_DEPTH);
        if (obmm_spsc_queue_init((uint8_t *)base + dir[di].offset,
                                 GVA_MGR_QUEUE_DEPTH) != 0)
            return -1;
        di++;
    }

    dir[di].region_id = (uint32_t)(node_count - 1);
    dir[di].kind = OBMM_REGION_TX_ARENA;
    dir[di].peer_node_id = (uint16_t)node_id;
    dir[di].offset = layout_tx_arena_offset(node_count);
    dir[di].size = layout_tx_arena_size(node_count);
    if (dir[di].size < GVA_MGR_PAYLOAD_SLOTS *
        sizeof(struct gva_mgr_aperture_msg)) {
        log_msg("manager region too small for tx arena");
        return -1;
    }
    di++;

    atomic_store_explicit(&hdr->generation, 1, memory_order_release);
    atomic_store_explicit(&hdr->state, OBMM_POOL_STATE_READY,
                          memory_order_release);
    return 0;
}

static int resolve_peer_region(struct node_slot *slot, int local_idx)
{
    const struct obmm_pool_header *hdr =
        (const struct obmm_pool_header *)slot->region.addr;
    const struct obmm_region_dirent *dir;
    bool found_queue = false;
    bool found_tx = false;
    uint32_t i;

    if (hdr->magic != OBMM_POOL_MAGIC ||
        hdr->layout_version != OBMM_POOL_LAYOUT_VERSION ||
        hdr->node_id != (uint16_t)slot->owner_idx ||
        hdr->region_size != GVA_MGR_REGION_SIZE)
        return -EINVAL;
    if (hdr->directory_offset < sizeof(*hdr) ||
        hdr->directory_count == 0 ||
        hdr->directory_count > MAX_NODES)
        return -EINVAL;

    dir = (const struct obmm_region_dirent *)
          ((const uint8_t *)hdr + hdr->directory_offset);
    for (i = 0; i < hdr->directory_count; i++) {
        if (dir[i].offset > hdr->region_size ||
            dir[i].size > hdr->region_size - dir[i].offset)
            return -EINVAL;
        if (dir[i].kind == OBMM_REGION_QUEUE &&
            dir[i].peer_node_id == (uint16_t)local_idx) {
            slot->ingress_queue[local_idx] =
                (struct obmm_spsc_queue *)((uint8_t *)slot->region.addr +
                                           dir[i].offset);
            found_queue = true;
        } else if (dir[i].kind == OBMM_REGION_TX_ARENA &&
                   dir[i].peer_node_id == (uint16_t)slot->owner_idx) {
            slot->tx_arena = (uint8_t *)slot->region.addr + dir[i].offset;
            slot->tx_arena_size = dir[i].size;
            slot->tx_arena_region_id = dir[i].region_id;
            found_tx = true;
        }
    }

    if (slot->owner_idx == local_idx)
        return found_tx ? 0 : -ENOENT;
    return found_queue && found_tx ? 0 : -ENOENT;
}

static int wait_peer_ready(struct node_slot *slot)
{
    const struct obmm_pool_header *hdr =
        (const struct obmm_pool_header *)slot->region.addr;
    long deadline = obmm_now_ms() + GVA_MGR_TIMEOUT_MS;

    while (!g_alarm_fired && obmm_now_ms() < deadline) {
        uint32_t state = atomic_load_explicit(&hdr->state,
                                              memory_order_acquire);

        if (state == OBMM_POOL_STATE_READY)
            return 0;
        if (state == OBMM_POOL_STATE_ERROR)
            return -EIO;
        usleep(100000);
    }
    return -ETIMEDOUT;
}

static int init_bus(struct obmm_mpmc_bus *bus, int node_count, int local_idx,
                    struct node_slot slots[MAX_NODES])
{
    struct obmm_spsc_queue *local_ingress[MAX_NODES] = {0};
    const struct obmm_pool_header *hdr =
        (const struct obmm_pool_header *)slots[local_idx].region.addr;
    const struct obmm_region_dirent *dir =
        (const struct obmm_region_dirent *)
        ((const uint8_t *)hdr + hdr->directory_offset);
    int rc;
    uint32_t i;

    for (i = 0; i < hdr->directory_count; i++) {
        if (dir[i].kind == OBMM_REGION_QUEUE)
            local_ingress[dir[i].peer_node_id] =
                (struct obmm_spsc_queue *)
                ((uint8_t *)slots[local_idx].region.addr + dir[i].offset);
    }

    rc = obmm_mpmc_consumer_init(bus, dir, hdr->directory_count,
                                 (uint32_t)local_idx);
    if (rc != 0)
        return rc;

    for (i = 0; i < bus->rx.lane_count; i++) {
        uint32_t pub = bus->rx.lane[i].publisher_node;

        bus->rx.lane[i].queue = local_ingress[pub];
        if (!bus->rx.lane[i].queue)
            return -ENOENT;
    }

    for (i = 0; i < (uint32_t)node_count; i++) {
        const struct obmm_pool_header *phdr;
        const struct obmm_region_dirent *pdir;

        if ((int)i == local_idx)
            continue;

        phdr = (const struct obmm_pool_header *)slots[i].region.addr;
        pdir = (const struct obmm_region_dirent *)
               ((const uint8_t *)phdr + phdr->directory_offset);
        rc = obmm_mpmc_publisher_init(bus, i, pdir,
                                      phdr->directory_count,
                                      (uint32_t)local_idx);
        if (rc != 0)
            return rc;

        bus->tx[i].queue = slots[i].ingress_queue[local_idx];
        if (!bus->tx[i].queue)
            return -ENOENT;
    }

    return 0;
}

static uint32_t msg_crc(const struct gva_mgr_aperture_msg *msg)
{
    const uint8_t *p = (const uint8_t *)msg;
    size_t crc_off = offsetof(struct gva_mgr_aperture_msg, hdr.crc32);
    uint32_t crc = 2166136261U;
    size_t i;

    for (i = 0; i < sizeof(*msg); i++) {
        if (i >= crc_off && i < crc_off + sizeof(msg->hdr.crc32))
            continue;
        crc ^= p[i];
        crc *= 16777619U;
    }
    return crc ? crc : 1U;
}

static struct gva_mgr_aperture_msg *
payload_slot(struct node_slot *slot, uint64_t seq)
{
    uint64_t idx = seq % GVA_MGR_PAYLOAD_SLOTS;

    if (slot->tx_arena_size < (idx + 1) * sizeof(struct gva_mgr_aperture_msg))
        return NULL;
    return (struct gva_mgr_aperture_msg *)
           (slot->tx_arena + idx * sizeof(struct gva_mgr_aperture_msg));
}

static int send_msg(struct obmm_mpmc_bus *bus, struct node_slot slots[MAX_NODES],
                    int local_idx, int dst, uint16_t type, uint64_t generation,
                    uint64_t seq, uint64_t base, uint64_t size,
                    uint32_t status)
{
    struct gva_mgr_aperture_msg *msg = payload_slot(&slots[local_idx], seq);
    struct obmm_desc desc = {0};
    long deadline;
    int rc = -EAGAIN;

    if (!msg)
        return -ENOSPC;

    memset(msg, 0, sizeof(*msg));
    msg->hdr.magic = GVA_MGR_MAGIC;
    msg->hdr.version = GVA_MGR_VERSION;
    msg->hdr.type = type;
    msg->hdr.generation = generation;
    msg->hdr.seq = seq;
    msg->hdr.src_node = (uint32_t)local_idx;
    msg->hdr.dst_node = (uint32_t)dst;
    msg->hdr.payload_len = sizeof(*msg) - sizeof(msg->hdr);
    msg->aperture_base = base;
    msg->aperture_size = size;
    msg->node_stride = 0;
    msg->status = status;
    msg->hdr.crc32 = msg_crc(msg);
    obmm_publish_payload_for_remote_read(msg, sizeof(*msg));

    desc.seq = seq;
    desc.region_id = slots[local_idx].tx_arena_region_id;
    desc.payload_offset = (uint64_t)((uint8_t *)msg - slots[local_idx].tx_arena);
    desc.payload_len = sizeof(*msg);
    desc.type = type;
    desc.cookie = GVA_MGR_MAGIC;

    deadline = obmm_now_ms() + GVA_MGR_TIMEOUT_MS;
    while (!g_alarm_fired && obmm_now_ms() < deadline) {
        rc = obmm_mpmc_send(bus, (uint32_t)dst, &desc);
        if (rc == 0)
            return 0;
        if (rc != -EAGAIN)
            return rc;
        usleep(100);
    }
    return rc == -EAGAIN ? -ETIMEDOUT : rc;
}

static int send_msg_broadcast_reliable(struct obmm_mpmc_bus *bus,
                                       struct node_slot slots[MAX_NODES],
                                       int local_idx, int dst, uint16_t type,
                                       uint64_t generation, uint64_t seq,
                                       uint64_t base, uint64_t size,
                                       uint32_t status)
{
    int attempt;
    int ret;

    for (attempt = 0; attempt < GVA_MGR_BROADCAST_RETRIES; attempt++) {
        ret = send_msg(bus, slots, local_idx, dst, type, generation, seq,
                       base, size, status);
        if (ret)
            return ret;
        if (attempt + 1 < GVA_MGR_BROADCAST_RETRIES)
            usleep(1000);
    }
    return 0;
}

static int send_segment_msg(struct obmm_mpmc_bus *bus,
                            struct node_slot slots[MAX_NODES],
                            const struct gva_mgr_config *cfg,
                            int dst, uint16_t type, uint64_t seq,
                            uint64_t segment_id, uint64_t segment_base,
                            uint64_t segment_size, uint32_t state,
                            uint32_t status)
{
    struct gva_mgr_aperture_msg *msg = payload_slot(&slots[cfg->node_id], seq);
    struct obmm_desc desc = {0};
    long deadline;
    int rc = -EAGAIN;
    uint64_t node_stride = cfg->aperture_size / (uint64_t)cfg->node_count;

    if (!msg)
        return -ENOSPC;
    node_stride &= ~(4096ULL - 1);

    memset(msg, 0, sizeof(*msg));
    msg->hdr.magic = GVA_MGR_MAGIC;
    msg->hdr.version = GVA_MGR_VERSION;
    msg->hdr.type = type;
    msg->hdr.generation = cfg->generation;
    msg->hdr.seq = seq;
    msg->hdr.src_node = (uint32_t)cfg->node_id;
    msg->hdr.dst_node = (uint32_t)dst;
    msg->hdr.payload_len = sizeof(*msg) - sizeof(msg->hdr);
    msg->aperture_base = cfg->aperture_base;
    msg->aperture_size = cfg->aperture_size;
    msg->node_stride = node_stride;
    msg->segment_id = segment_id;
    msg->segment_base = segment_base;
    msg->segment_size = segment_size;
    msg->home_node_id = (uint32_t)cfg->home_node_id;
    msg->access_flags = cfg->access_flags;
    msg->cache_policy = cfg->cache_policy;
    msg->segment_state = state;
    msg->status = status;
    msg->hdr.crc32 = msg_crc(msg);
    obmm_publish_payload_for_remote_read(msg, sizeof(*msg));

    desc.seq = seq;
    desc.region_id = slots[cfg->node_id].tx_arena_region_id;
    desc.payload_offset = (uint64_t)((uint8_t *)msg - slots[cfg->node_id].tx_arena);
    desc.payload_len = sizeof(*msg);
    desc.type = type;
    desc.cookie = GVA_MGR_MAGIC;

    deadline = obmm_now_ms() + GVA_MGR_TIMEOUT_MS;
    while (!g_alarm_fired && obmm_now_ms() < deadline) {
        rc = obmm_mpmc_send(bus, (uint32_t)dst, &desc);
        if (rc == 0)
            return 0;
        if (rc != -EAGAIN)
            return rc;
        usleep(100);
    }
    return rc == -EAGAIN ? -ETIMEDOUT : rc;
}

static int send_segment_msg_broadcast_reliable(struct obmm_mpmc_bus *bus,
                                               struct node_slot slots[MAX_NODES],
                                               const struct gva_mgr_config *cfg,
                                               int dst, uint16_t type,
                                               uint64_t seq,
                                               uint64_t segment_id,
                                               uint64_t segment_base,
                                               uint64_t segment_size,
                                               uint32_t state,
                                               uint32_t status)
{
    int attempt;
    int ret;

    for (attempt = 0; attempt < GVA_MGR_BROADCAST_RETRIES; attempt++) {
        ret = send_segment_msg(bus, slots, cfg, dst, type, seq, segment_id,
                               segment_base, segment_size, state, status);
        if (ret)
            return ret;
        if (attempt + 1 < GVA_MGR_BROADCAST_RETRIES)
            usleep(1000);
    }
    return 0;
}

static int recv_msg(struct obmm_mpmc_bus *bus, struct node_slot slots[MAX_NODES],
                    struct gva_mgr_aperture_msg *out, uint32_t *src_out)
{
    long deadline = obmm_now_ms() + GVA_MGR_TIMEOUT_MS;

    while (!g_alarm_fired && obmm_now_ms() < deadline) {
        struct obmm_desc desc;
        uint32_t src;
        int rc = obmm_mpmc_recv(bus, &desc, &src);

        if (rc == -EAGAIN) {
            usleep(100);
            continue;
        }
        if (rc != 0)
            return rc;
        if (src >= MAX_NODES || desc.cookie != GVA_MGR_MAGIC ||
            desc.payload_len != sizeof(*out))
            continue;
        if (!slots[src].tx_arena ||
            desc.payload_offset > slots[src].tx_arena_size ||
            desc.payload_len > slots[src].tx_arena_size - desc.payload_offset)
            continue;
        memcpy(out, slots[src].tx_arena + desc.payload_offset, sizeof(*out));
        if (out->hdr.magic != GVA_MGR_MAGIC ||
            out->hdr.version != GVA_MGR_VERSION ||
            out->hdr.src_node != src ||
            out->hdr.crc32 != msg_crc(out))
            continue;
        *src_out = src;
        return 0;
    }
    return -ETIMEDOUT;
}

static void *reserve_aperture(uint64_t base, uint64_t size)
{
    void *addr = (void *)(uintptr_t)base;
    void *mapped;

    mapped = mmap(addr, size, PROT_NONE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mapped == MAP_FAILED)
        return NULL;
    if (mapped != addr) {
        munmap(mapped, size);
        errno = EFAULT;
        return NULL;
    }
    return mapped;
}

static uint64_t make_segment_id(uint64_t generation, uint64_t epoch,
                                int home_node, uint64_t segment_base)
{
    uint64_t id = 1469598103934665603ULL;

    id ^= generation;
    id *= 1099511628211ULL;
    if (epoch != 0) {
        id ^= epoch;
        id *= 1099511628211ULL;
    }
    id ^= (uint64_t)(uint32_t)home_node;
    id *= 1099511628211ULL;
    id ^= segment_base;
    id *= 1099511628211ULL;
    return id ? id : 1;
}

static int compute_owner_sharded_segment_epoch(const struct gva_mgr_config *cfg,
                                               uint64_t epoch,
                                               uint64_t *segment_id,
                                               uint64_t *segment_base,
                                               uint64_t *node_stride)
{
    uint64_t stride = cfg->aperture_size / (uint64_t)cfg->node_count;
    uint64_t slice_base;
    uint64_t slice_end;
    uint64_t base;

    stride &= ~(4096ULL - 1);
    if (stride == 0 || cfg->segment_size > stride)
        return -EINVAL;
    if (UINT64_MAX - cfg->aperture_base <
        (uint64_t)cfg->home_node_id * stride)
        return -EOVERFLOW;
    slice_base = cfg->aperture_base + (uint64_t)cfg->home_node_id * stride;
    if (UINT64_MAX - slice_base < stride)
        return -EOVERFLOW;
    slice_end = slice_base + stride;
    base = obmm_align_up_u64(slice_base, cfg->segment_alignment);
    if (base < slice_base || UINT64_MAX - base < cfg->segment_size ||
        base + cfg->segment_size > slice_end)
        return -ENOSPC;

    *segment_base = base;
    *segment_id = make_segment_id(cfg->generation, epoch, cfg->home_node_id,
                                  base);
    *node_stride = stride;
    return 0;
}

static int compute_owner_sharded_segment(const struct gva_mgr_config *cfg,
                                         uint64_t *segment_id,
                                         uint64_t *segment_base,
                                         uint64_t *node_stride)
{
    return compute_owner_sharded_segment_epoch(cfg, 0, segment_id,
                                               segment_base, node_stride);
}

static bool segment_msg_matches(const struct gva_mgr_config *cfg,
                                const struct gva_mgr_aperture_msg *msg,
                                uint64_t segment_id,
                                uint64_t segment_base,
                                uint64_t segment_size)
{
    return msg->hdr.generation == cfg->generation &&
           msg->segment_id == segment_id &&
           msg->segment_base == segment_base &&
           msg->segment_size == segment_size &&
           msg->home_node_id == (uint32_t)cfg->home_node_id &&
           msg->access_flags == cfg->access_flags &&
           msg->cache_policy == cfg->cache_policy;
}

static int validate_segment_msg(const struct gva_mgr_config *cfg,
                                const struct gva_mgr_aperture_msg *msg,
                                uint64_t segment_id,
                                uint64_t segment_base,
                                uint64_t segment_size,
                                uint64_t node_stride,
                                uint32_t expected_state)
{
    if (msg->aperture_base != cfg->aperture_base ||
        msg->aperture_size != cfg->aperture_size ||
        msg->node_stride != node_stride ||
        msg->segment_size != segment_size ||
        msg->home_node_id != (uint32_t)cfg->home_node_id ||
        msg->segment_state != expected_state)
        return -EINVAL;

    if (!segment_msg_matches(cfg, msg, segment_id, segment_base, segment_size))
        return -EINVAL;
    return 0;
}

static int activate_segment(const struct gva_mgr_config *cfg,
                            struct obmm_mpmc_bus *bus,
                            struct node_slot slots[MAX_NODES],
                            uint64_t *seq, uint64_t segment_id,
                            uint64_t segment_base, uint64_t segment_size,
                            uint64_t node_stride)
{
    int ret;
    int peer;

    if (cfg->node_id == cfg->home_node_id) {
        bool acked[MAX_NODES] = {false};
        int pending = cfg->node_count - 1;

        for (peer = 0; peer < cfg->node_count; peer++) {
            if (peer == cfg->node_id)
                continue;
            ret = send_segment_msg_broadcast_reliable(
                    bus, slots, cfg, peer, GVA_MGR_MSG_SEGMENT_ANNOUNCE,
                    (*seq)++, segment_id, segment_base, segment_size,
                    GVA_MGR_SEGMENT_PROPOSED, 0);
            if (ret) {
                log_msg("send SEGMENT_ANNOUNCE failed peer=%d ret=%d",
                        peer, ret);
                return ret;
            }
        }

        while (pending > 0) {
            struct gva_mgr_aperture_msg msg;
            uint32_t src = 0;

            ret = recv_msg(bus, slots, &msg, &src);
            if (ret) {
                log_msg("timeout waiting SEGMENT_ACK ret=%d", ret);
                return ret;
            }
            if (msg.hdr.type == GVA_MGR_MSG_ERROR) {
                log_msg("peer=%u reported segment error status=%u", src,
                        msg.status);
                return -EIO;
            }
            if (msg.hdr.type == GVA_MGR_MSG_SEGMENT_ACK &&
                !acked[src] &&
                segment_msg_matches(cfg, &msg, segment_id, segment_base,
                                    segment_size)) {
                acked[src] = true;
                pending--;
            }
        }

        for (peer = 0; peer < cfg->node_count; peer++) {
            if (peer == cfg->node_id)
                continue;
            ret = send_segment_msg_broadcast_reliable(
                    bus, slots, cfg, peer, GVA_MGR_MSG_SEGMENT_ANNOUNCE,
                    (*seq)++, segment_id, segment_base, segment_size,
                    GVA_MGR_SEGMENT_ACTIVE, 0);
            if (ret) {
                log_msg("send SEGMENT_ACTIVE failed peer=%d ret=%d",
                        peer, ret);
                return ret;
            }
        }
    } else {
        while (true) {
            struct gva_mgr_aperture_msg msg;
            uint32_t src = 0;

            ret = recv_msg(bus, slots, &msg, &src);
            if (ret) {
                log_msg("timeout waiting SEGMENT_ANNOUNCE ret=%d", ret);
                return ret;
            }
            if (src != (uint32_t)cfg->home_node_id ||
                msg.hdr.type != GVA_MGR_MSG_SEGMENT_ANNOUNCE)
                continue;
            if (msg.segment_state != GVA_MGR_SEGMENT_PROPOSED)
                continue;
            if (!segment_msg_matches(cfg, &msg, segment_id, segment_base,
                                     segment_size))
                continue;
            ret = validate_segment_msg(cfg, &msg, segment_id, segment_base,
                                       segment_size, node_stride,
                                       GVA_MGR_SEGMENT_PROPOSED);
            if (ret) {
                (void)send_segment_msg(bus, slots, cfg, cfg->home_node_id,
                                       GVA_MGR_MSG_ERROR, (*seq)++,
                                       msg.segment_id, msg.segment_base,
                                       msg.segment_size, msg.segment_state,
                                       (uint32_t)-ret);
                return ret;
            }
            ret = send_segment_msg(bus, slots, cfg, cfg->home_node_id,
                                   GVA_MGR_MSG_SEGMENT_ACK, (*seq)++,
                                   segment_id, segment_base, segment_size,
                                   GVA_MGR_SEGMENT_ACTIVE, 0);
            if (ret)
                return ret;
            break;
        }

        while (true) {
            struct gva_mgr_aperture_msg msg;
            uint32_t src = 0;

            ret = recv_msg(bus, slots, &msg, &src);
            if (ret) {
                log_msg("timeout waiting SEGMENT_ACTIVE ret=%d", ret);
                return ret;
            }
            if (src != (uint32_t)cfg->home_node_id ||
                msg.hdr.type != GVA_MGR_MSG_SEGMENT_ANNOUNCE)
                continue;
            if (msg.segment_state != GVA_MGR_SEGMENT_ACTIVE)
                continue;
            if (!segment_msg_matches(cfg, &msg, segment_id, segment_base,
                                     segment_size))
                continue;
            ret = validate_segment_msg(cfg, &msg, segment_id, segment_base,
                                       segment_size, node_stride,
                                       GVA_MGR_SEGMENT_ACTIVE);
            if (ret) {
                log_msg("invalid SEGMENT_ACTIVE ret=%d", ret);
                return ret;
            }
            break;
        }
    }

    return 0;
}

static int retire_segment(const struct gva_mgr_config *cfg,
                          struct obmm_mpmc_bus *bus,
                          struct node_slot slots[MAX_NODES],
                          uint64_t *seq, uint64_t segment_id,
                          uint64_t segment_base, uint64_t segment_size,
                          uint64_t node_stride)
{
    int ret;
    int peer;

    if (cfg->node_id == cfg->home_node_id) {
        bool acked[MAX_NODES] = {false};
        int pending = cfg->node_count - 1;

        for (peer = 0; peer < cfg->node_count; peer++) {
            if (peer == cfg->node_id)
                continue;
            ret = send_segment_msg_broadcast_reliable(
                    bus, slots, cfg, peer, GVA_MGR_MSG_SEGMENT_RETIRE,
                    (*seq)++, segment_id, segment_base, segment_size,
                    GVA_MGR_SEGMENT_RETIRED, 0);
            if (ret) {
                log_msg("send SEGMENT_RETIRE failed peer=%d ret=%d",
                        peer, ret);
                return ret;
            }
        }

        while (pending > 0) {
            struct gva_mgr_aperture_msg msg;
            uint32_t src = 0;

            ret = recv_msg(bus, slots, &msg, &src);
            if (ret) {
                log_msg("timeout waiting SEGMENT_RETIRED_ACK ret=%d", ret);
                return ret;
            }
            if (msg.hdr.type == GVA_MGR_MSG_ERROR) {
                log_msg("peer=%u reported retire error status=%u", src,
                        msg.status);
                return -EIO;
            }
            if (msg.hdr.type == GVA_MGR_MSG_SEGMENT_RETIRED_ACK &&
                !acked[src] &&
                validate_segment_msg(cfg, &msg, segment_id, segment_base,
                                     segment_size, node_stride,
                                     GVA_MGR_SEGMENT_RETIRED) == 0) {
                acked[src] = true;
                pending--;
            }
        }
    } else {
        while (true) {
            struct gva_mgr_aperture_msg msg;
            uint32_t src = 0;

            ret = recv_msg(bus, slots, &msg, &src);
            if (ret) {
                log_msg("timeout waiting SEGMENT_RETIRE ret=%d", ret);
                return ret;
            }
            if (src != (uint32_t)cfg->home_node_id ||
                msg.hdr.type != GVA_MGR_MSG_SEGMENT_RETIRE)
                continue;
            if (msg.segment_state != GVA_MGR_SEGMENT_RETIRED)
                continue;
            if (!segment_msg_matches(cfg, &msg, segment_id, segment_base,
                                     segment_size))
                continue;
            ret = validate_segment_msg(cfg, &msg, segment_id, segment_base,
                                       segment_size, node_stride,
                                       GVA_MGR_SEGMENT_RETIRED);
            if (ret) {
                (void)send_segment_msg(bus, slots, cfg, cfg->home_node_id,
                                       GVA_MGR_MSG_ERROR, (*seq)++,
                                       msg.segment_id, msg.segment_base,
                                       msg.segment_size, msg.segment_state,
                                       (uint32_t)-ret);
                return ret;
            }
            ret = send_segment_msg(bus, slots, cfg, cfg->home_node_id,
                                   GVA_MGR_MSG_SEGMENT_RETIRED_ACK, (*seq)++,
                                   segment_id, segment_base, segment_size,
                                   GVA_MGR_SEGMENT_RETIRED, 0);
            if (ret)
                return ret;
            break;
        }
    }

    return 0;
}

static int run_segment_protocol(const struct gva_mgr_config *cfg,
                                struct obmm_mpmc_bus *bus,
                                struct node_slot slots[MAX_NODES],
                                uint64_t *seq)
{
    uint64_t segment_id = 0;
    uint64_t segment_base = 0;
    uint64_t node_stride = 0;
    int ret;

    if (!cfg->allocate_segment)
        return 0;

    ret = compute_owner_sharded_segment(cfg, &segment_id, &segment_base,
                                        &node_stride);
    if (ret) {
        log_msg("segment allocation failed ret=%d", ret);
        return ret;
    }

    ret = activate_segment(cfg, bus, slots, seq, segment_id, segment_base,
                           cfg->segment_size, node_stride);
    if (ret)
        return ret;

    log_msg("segment active segment_id=%#" PRIx64 " gsva_base=%#" PRIx64
            " size=%#" PRIx64 " node_stride=%#" PRIx64 " home_node=%d"
            " cache_policy=%u access_flags=%u",
            segment_id, segment_base, cfg->segment_size, node_stride,
            cfg->home_node_id, cfg->cache_policy, cfg->access_flags);

    if (cfg->retire_segment) {
        ret = retire_segment(cfg, bus, slots, seq, segment_id, segment_base,
                             cfg->segment_size, node_stride);
        if (ret)
            return ret;
        log_msg("segment retired segment_id=%#" PRIx64 " gsva_base=%#" PRIx64
                " size=%#" PRIx64 " home_node=%d",
                segment_id, segment_base, cfg->segment_size,
                cfg->home_node_id);
    }

    if (cfg->reuse_segment) {
        uint64_t reuse_id = 0;
        uint64_t reuse_base = 0;
        uint64_t reuse_stride = 0;

        ret = compute_owner_sharded_segment_epoch(cfg, 1, &reuse_id,
                                                  &reuse_base, &reuse_stride);
        if (ret)
            return ret;
        if (reuse_base != segment_base || reuse_stride != node_stride ||
            reuse_id == segment_id)
            return -EINVAL;
        ret = activate_segment(cfg, bus, slots, seq, reuse_id, reuse_base,
                               cfg->segment_size, reuse_stride);
        if (ret)
            return ret;
        log_msg("segment reused old_segment_id=%#" PRIx64
                " new_segment_id=%#" PRIx64 " gsva_base=%#" PRIx64
                " size=%#" PRIx64 " home_node=%d",
                segment_id, reuse_id, reuse_base, cfg->segment_size,
                cfg->home_node_id);
        log_msg("segment active segment_id=%#" PRIx64 " gsva_base=%#" PRIx64
                " size=%#" PRIx64 " node_stride=%#" PRIx64 " home_node=%d"
                " cache_policy=%u access_flags=%u",
                reuse_id, reuse_base, cfg->segment_size, reuse_stride,
                cfg->home_node_id, cfg->cache_policy, cfg->access_flags);
    }
    return 0;
}

static int register_kernel_aperture(int obmm_fd, const struct gva_mgr_config *cfg)
{
    struct obmm_cmd_gsva_aperture req = {0};
    struct obmm_cmd_gsva_aperture query = {0};

    req.base = cfg->aperture_base;
    req.size = cfg->aperture_size;
    req.generation = cfg->generation;
    req.node_id = (uint32_t)cfg->node_id;
    req.node_count = (uint32_t)cfg->node_count;
    req.flags = OBMM_GSVA_APERTURE_F_ACTIVE;

    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_REGISTER, &req) != 0) {
        log_msg("kernel aperture register failed errno=%d", errno);
        return -1;
    }
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_QUERY, &query) != 0) {
        log_msg("kernel aperture query failed errno=%d", errno);
        return -1;
    }
    if (!(query.flags & OBMM_GSVA_APERTURE_F_ACTIVE) ||
        query.base != cfg->aperture_base ||
        query.size != cfg->aperture_size ||
        query.generation != cfg->generation) {
        log_msg("kernel aperture query mismatch base=%#" PRIx64
                " size=%#" PRIx64 " generation=%#" PRIx64
                " flags=%#" PRIx64,
                query.base, query.size, query.generation, query.flags);
        return -1;
    }

    log_msg("kernel aperture registry -> ok base=%#" PRIx64
            " size=%#" PRIx64 " generation=%#" PRIx64,
            query.base, query.size, query.generation);
    log_kernel_aperture_proc();
    return 0;
}

static void log_gsva_desc(const char *action,
                          const struct obmm_gsva_segment_desc_v1 *desc)
{
    log_msg("gsva descriptor action=%s version=%u flags=%#x"
            " segment_id=%#" PRIx64 " home_va=%#" PRIx64
            " size=%#" PRIx64 " epoch=%#" PRIx64
            " home_cna=%u owner_node=%u node_count=%u"
            " cache_policy=%u p_tag=%u access_flags=%#x"
            " token_id=%u token_value=%u",
            action, desc->version, desc->flags, desc->segment_id,
            desc->home_va, desc->size, desc->epoch, desc->home_cna,
            desc->owner_node_id, desc->node_count, desc->cache_policy,
            desc->p_tag, desc->access_flags, desc->token_id,
            desc->token_value);
}

static int run_descriptor_cli_action(const struct gva_mgr_config *cfg,
                                     int obmm_fd)
{
    int ret;

    if (cfg->desc_alloc) {
        struct obmm_cmd_gsva_alloc_segment_v1 cmd = {0};

        ret = register_kernel_aperture(obmm_fd, cfg);
        if (ret) {
            log_msg("result=fail action=gsva-segment-alloc stage=aperture errno=%d",
                    errno);
            return ret;
        }

        cmd.version = OBMM_GSVA_ABI_VERSION;
        cmd.flags = OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY |
                    OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED;
        cmd.size = cfg->segment_size;
        cmd.alignment = cfg->segment_alignment;
        cmd.requested_home_va = cfg->requested_home_va;
        cmd.home_node_id = (uint32_t)cfg->home_node_id;
        cmd.cache_policy = cfg->cache_policy;
        cmd.requested_p_tag = cfg->requested_p_tag;
        cmd.access_flags = cfg->access_flags;

        if (ioctl(obmm_fd, OBMM_CMD_GSVA_ALLOC_SEGMENT, &cmd) != 0) {
            log_msg("result=fail action=gsva-segment-alloc stage=alloc errno=%d",
                    errno);
            return -1;
        }

        log_gsva_desc("alloc", &cmd.desc);
        log_msg("result=done action=gsva-segment-alloc"
                " segment_id=%#" PRIx64 " home_va=%#" PRIx64
                " epoch=%#" PRIx64,
                cmd.desc.segment_id, cmd.desc.home_va, cmd.desc.epoch);
        return 0;
    }

    if (cfg->desc_query) {
        struct obmm_cmd_gsva_query_segment_v1 cmd = {0};

        cmd.version = OBMM_GSVA_ABI_VERSION;
        cmd.segment_id = cfg->segment_id;
        cmd.home_va = cfg->query_home_va;

        if (ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_SEGMENT, &cmd) != 0) {
            log_msg("result=fail action=gsva-segment-query errno=%d",
                    errno);
            return -1;
        }

        log_gsva_desc("query", &cmd.desc);
        log_msg("result=done action=gsva-segment-query"
                " segment_id=%#" PRIx64 " home_va=%#" PRIx64
                " epoch=%#" PRIx64,
                cmd.desc.segment_id, cmd.desc.home_va, cmd.desc.epoch);
        return 0;
    }

    if (cfg->desc_retire) {
        struct obmm_cmd_gsva_retire_segment_v1 cmd = {0};

        cmd.version = OBMM_GSVA_ABI_VERSION;
        cmd.segment_id = cfg->segment_id;
        cmd.epoch = cfg->epoch;
        cmd.timeout_ms = cfg->retire_timeout_ms;

        if (ioctl(obmm_fd, OBMM_CMD_GSVA_RETIRE_SEGMENT, &cmd) != 0) {
            log_msg("result=fail action=gsva-segment-retire errno=%d"
                    " status=%u error=%u",
                    errno, cmd.status, cmd.error);
            return -1;
        }

        log_msg("result=done action=gsva-segment-retire"
                " segment_id=%#" PRIx64 " committed_epoch=%#" PRIx64
                " status=%u error=%u",
                cfg->segment_id, cmd.committed_epoch, cmd.status,
                cmd.error);
        return 0;
    }

    return -EINVAL;
}

static int run_protocol(struct gva_mgr_config *cfg,
                        int obmm_fd,
                        struct obmm_mpmc_bus *bus,
                        struct node_slot slots[MAX_NODES])
{
    bool hello[MAX_NODES] = {false};
    bool accepted[MAX_NODES] = {false};
    void *reserved = NULL;
    void *conflict = NULL;
    uint64_t seq = 1;
    int pending;
    int peer;

    if (cfg->inject_conflict) {
        conflict = reserve_aperture(cfg->aperture_base, 4096);
        if (!conflict) {
            log_msg("conflict injection could not pre-map aperture page errno=%d",
                    errno);
            return -1;
        }
        log_msg("conflict injected at base=%#" PRIx64, cfg->aperture_base);
    }

    for (peer = 0; peer < cfg->node_count; peer++) {
        if (peer == cfg->node_id)
            continue;
        if (send_msg_broadcast_reliable(bus, slots, cfg->node_id, peer,
                                        GVA_MGR_MSG_HELLO, cfg->generation,
                                        seq++, cfg->aperture_base,
                                        cfg->aperture_size, 0) != 0) {
            log_msg("send HELLO failed peer=%d", peer);
            goto fail;
        }
    }

    pending = cfg->node_count - 1;
    while (pending > 0) {
        struct gva_mgr_aperture_msg msg;
        uint32_t src = 0;

        if (recv_msg(bus, slots, &msg, &src) != 0) {
            log_msg("timeout waiting HELLO");
            goto fail;
        }
        if (msg.hdr.type == GVA_MGR_MSG_HELLO &&
            msg.hdr.generation == cfg->generation && !hello[src]) {
            hello[src] = true;
            pending--;
        }
    }
    log_msg("bootstrap hello -> ok peers=%d", cfg->node_count - 1);

    if (cfg->node_id == 0) {
        reserved = reserve_aperture(cfg->aperture_base, cfg->aperture_size);
        if (!reserved) {
            log_msg("aperture reserve failed base=%#" PRIx64
                    " size=%#" PRIx64 " errno=%d",
                    cfg->aperture_base, cfg->aperture_size, errno);
            goto fail;
        }
        log_msg("aperture reserved registry=process-local base=%#" PRIx64
                " size=%#" PRIx64,
                cfg->aperture_base, cfg->aperture_size);
        if (register_kernel_aperture(obmm_fd, cfg) != 0)
            goto fail;

        for (peer = 1; peer < cfg->node_count; peer++) {
            if (send_msg_broadcast_reliable(
                    bus, slots, cfg->node_id, peer,
                    GVA_MGR_MSG_APERTURE_PROPOSE, cfg->generation, seq++,
                    cfg->aperture_base, cfg->aperture_size, 0) != 0) {
                log_msg("send PROPOSE failed peer=%d", peer);
                goto fail;
            }
        }

        pending = cfg->node_count - 1;
        while (pending > 0) {
            struct gva_mgr_aperture_msg msg;
            uint32_t src = 0;

            if (recv_msg(bus, slots, &msg, &src) != 0) {
                log_msg("timeout waiting ACCEPT");
                goto fail;
            }
            if (msg.hdr.generation != cfg->generation)
                continue;
            if (msg.hdr.type == GVA_MGR_MSG_ERROR) {
                log_msg("peer=%u reported bootstrap error status=%u", src,
                        msg.status);
                goto fail;
            }
            if (msg.hdr.type == GVA_MGR_MSG_APERTURE_ACCEPT &&
                msg.aperture_base == cfg->aperture_base &&
                msg.aperture_size == cfg->aperture_size && !accepted[src]) {
                accepted[src] = true;
                pending--;
            }
        }

        for (peer = 1; peer < cfg->node_count; peer++) {
            if (send_msg_broadcast_reliable(
                    bus, slots, cfg->node_id, peer,
                    GVA_MGR_MSG_APERTURE_COMMIT, cfg->generation, seq++,
                    cfg->aperture_base, cfg->aperture_size, 0) != 0) {
                log_msg("send COMMIT failed peer=%d", peer);
                goto fail;
            }
        }
    } else {
        while (true) {
            struct gva_mgr_aperture_msg msg;
            uint32_t src = 0;

            if (recv_msg(bus, slots, &msg, &src) != 0) {
                log_msg("timeout waiting PROPOSE");
                goto fail;
            }
            if (src != 0 || msg.hdr.generation != cfg->generation ||
                msg.hdr.type != GVA_MGR_MSG_APERTURE_PROPOSE)
                continue;

            cfg->aperture_base = msg.aperture_base;
            cfg->aperture_size = msg.aperture_size;
            reserved = reserve_aperture(cfg->aperture_base,
                                        cfg->aperture_size);
            if (!reserved) {
                int saved_errno = errno;

                (void)send_msg(bus, slots, cfg->node_id, 0,
                               GVA_MGR_MSG_ERROR, cfg->generation, seq++,
                               cfg->aperture_base, cfg->aperture_size,
                               (uint32_t)saved_errno);
                log_msg("aperture reserve failed base=%#" PRIx64
                        " size=%#" PRIx64 " errno=%d",
                        cfg->aperture_base, cfg->aperture_size, saved_errno);
                goto fail;
            }
            log_msg("aperture reserved registry=process-local base=%#" PRIx64
                    " size=%#" PRIx64,
                    cfg->aperture_base, cfg->aperture_size);
            if (register_kernel_aperture(obmm_fd, cfg) != 0)
                goto fail;

            if (send_msg(bus, slots, cfg->node_id, 0,
                         GVA_MGR_MSG_APERTURE_ACCEPT, cfg->generation,
                         seq++, cfg->aperture_base, cfg->aperture_size,
                         0) != 0) {
                log_msg("send ACCEPT failed");
                goto fail;
            }
            break;
        }

        while (true) {
            struct gva_mgr_aperture_msg msg;
            uint32_t src = 0;

            if (recv_msg(bus, slots, &msg, &src) != 0) {
                log_msg("timeout waiting COMMIT");
                goto fail;
            }
            if (src == 0 &&
                msg.hdr.generation == cfg->generation &&
                msg.hdr.type == GVA_MGR_MSG_APERTURE_COMMIT &&
                msg.aperture_base == cfg->aperture_base &&
                msg.aperture_size == cfg->aperture_size)
                break;
        }
    }

    if (run_segment_protocol(cfg, bus, slots, &seq) != 0)
        goto fail;

    log_msg("result=done generation=%#" PRIx64 " aperture_base=%#" PRIx64
            " aperture_size=%#" PRIx64 " registry=kernel-obmm",
            cfg->generation, cfg->aperture_base, cfg->aperture_size);
    if (reserved)
        munmap(reserved, cfg->aperture_size);
    if (conflict)
        munmap(conflict, 4096);
    return 0;

fail:
    log_msg("result=fail generation=%#" PRIx64, cfg->generation);
    if (reserved)
        munmap(reserved, cfg->aperture_size);
    if (conflict)
        munmap(conflict, 4096);
    return -1;
}

static int manager_cleanup(int obmm_fd, int node_count, int local_idx,
                           struct node_slot slots[MAX_NODES])
{
    int i;

    for (i = 0; i < node_count; i++) {
        if (slots[i].region.addr || slots[i].region.fd >= 0)
            obmm_unmap_region(&slots[i].region);
    }
    for (i = 0; i < node_count; i++) {
        if (!slots[i].mem_id)
            continue;
        if (i == local_idx)
            (void)obmm_do_unexport(obmm_fd, slots[i].mem_id);
        else
            (void)obmm_do_unimport(obmm_fd, slots[i].mem_id);
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct gva_mgr_config cfg;
    struct obmm_helpers_meta metas[MAX_NODES];
    struct obmm_helpers_meta local_meta;
    bool got_meta[MAX_NODES] = {false};
    struct node_slot slots[MAX_NODES];
    struct obmm_mpmc_bus bus;
    int obmm_fd = -1;
    uint64_t local_cna_u64 = 0;
    uint32_t local_cna = 0;
    char value[64];
    int i;
    int rc = 1;

    memset(metas, 0, sizeof(metas));
    memset(&local_meta, 0, sizeof(local_meta));
    memset(slots, 0, sizeof(slots));
    memset(&bus, 0, sizeof(bus));
    for (i = 0; i < MAX_NODES; i++)
        slots[i].region.fd = -1;

    if (parse_args(argc, argv, &cfg) != 0) {
        log_msg("invalid arguments");
        return 2;
    }

    if (cfg.dump_routes)
        return dump_gva_routes() == 0 ? 0 : 1;

    signal(SIGALRM, alarm_handler);
    alarm(GVA_MGR_TIMEOUT_MS / 1000);

    log_msg("start node=%d count=%d generation=%#" PRIx64
            " aperture_base=%#" PRIx64 " aperture_size=%#" PRIx64
            " conflict=%d allocate_segment=%d retire_segment=%d reuse_segment=%d",
            cfg.node_id, cfg.node_count, cfg.generation,
            cfg.aperture_base, cfg.aperture_size, cfg.inject_conflict ? 1 : 0,
            cfg.allocate_segment ? 1 : 0, cfg.retire_segment ? 1 : 0,
            cfg.reuse_segment ? 1 : 0);

    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        log_msg("open /dev/obmm failed errno=%d", errno);
        goto out;
    }
    if (obmm_cmdline_get("linqu_cna", value, sizeof(value))) {
        local_cna_u64 = strtoull(value, NULL, 0);
    } else if (!obmm_parse_hex_u64("/sys/bus/ub/devices/00001/primary_cna",
                                   &local_cna_u64)) {
        log_msg("read primary_cna failed");
        goto out;
    }
    local_cna = (uint32_t)local_cna_u64;
    local_meta.export_cna = local_cna;

    if (cfg.desc_alloc || cfg.desc_query || cfg.desc_retire) {
        if (run_descriptor_cli_action(&cfg, obmm_fd) == 0)
            rc = 0;
        goto out;
    }

    if (obmm_do_export(obmm_fd, &local_meta, GVA_MGR_REGION_SIZE) != 0) {
        log_msg("export manager region failed errno=%d", errno);
        goto out;
    }
    slots[cfg.node_id].owner_idx = cfg.node_id;
    slots[cfg.node_id].is_local = true;
    slots[cfg.node_id].mem_id = local_meta.export_mem_id;

    if (obmm_map_region(local_meta.export_mem_id, GVA_MGR_REGION_SIZE,
                        false, &slots[cfg.node_id].region) != 0) {
        log_msg("map local manager region failed errno=%d", errno);
        goto out;
    }
    if (init_manager_region(slots[cfg.node_id].region.addr,
                            cfg.node_id, cfg.node_count) != 0) {
        log_msg("init manager region failed");
        goto out;
    }
    obmm_publish_payload_for_remote_read(slots[cfg.node_id].region.addr,
                                         GVA_MGR_REGION_SIZE);
    if (resolve_peer_region(&slots[cfg.node_id], cfg.node_id) != 0) {
        log_msg("resolve local manager region failed");
        goto out;
    }

    if (obmm_bootstrap_publish(obmm_fd, cfg.node_id, cfg.node_count,
                               cfg.generation, &local_meta) != 0) {
        log_msg("OBMM bootstrap publish failed errno=%d", errno);
        goto out;
    }
    if (obmm_bootstrap_lookup(obmm_fd, local_cna, cfg.node_count,
                              cfg.generation, metas, got_meta) != 0) {
        log_msg("OBMM bootstrap lookup failed errno=%d", errno);
        goto out;
    }
    log_msg("obmm bootstrap -> ok count=%d", cfg.node_count);

    {
        uint64_t import_pas[MAX_NODES] = {0};
        bool import_osync[MAX_NODES] = {false};
        int import_idx = 0;

        if (!obmm_alloc_import_pas(cfg.node_count - 1, GVA_MGR_REGION_SIZE,
                                   import_pas, import_osync,
                                   obmm_parse_import_cache_mode())) {
            log_msg("alloc import PA failed");
            goto out;
        }

        for (i = 0; i < cfg.node_count; i++) {
            uint64_t mem_id = 0;

            if (i == cfg.node_id)
                continue;
            if (!got_meta[i]) {
                log_msg("missing peer bootstrap record peer=%d", i);
                goto out;
            }
            slots[i].owner_idx = i;
            slots[i].local_pa = import_pas[import_idx];
            if (obmm_do_import(obmm_fd, &metas[i], local_cna,
                               slots[i].local_pa, 0, &mem_id) != 0) {
                log_msg("import peer manager region failed peer=%d errno=%d",
                        i, errno);
                goto out;
            }
            slots[i].mem_id = mem_id;
            if (obmm_map_region(mem_id, GVA_MGR_REGION_SIZE,
                                import_osync[import_idx],
                                &slots[i].region) != 0) {
                log_msg("map peer manager region failed peer=%d errno=%d",
                        i, errno);
                goto out;
            }
            import_idx++;
            if (wait_peer_ready(&slots[i]) != 0 ||
                resolve_peer_region(&slots[i], cfg.node_id) != 0) {
                log_msg("peer manager layout invalid peer=%d", i);
                goto out;
            }
        }
    }
    log_msg("manager queues -> ok");

    if (init_bus(&bus, cfg.node_count, cfg.node_id, slots) != 0) {
        log_msg("MPMC bus init failed");
        goto out;
    }

    if (run_protocol(&cfg, obmm_fd, &bus, slots) == 0)
        rc = 0;

out:
    if (obmm_fd >= 0) {
        manager_cleanup(obmm_fd, cfg.node_count, cfg.node_id, slots);
        close(obmm_fd);
    }
    return rc;
}
