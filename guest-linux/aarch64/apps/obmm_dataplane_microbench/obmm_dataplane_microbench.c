/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM data-plane microbenchmark.
 *
 * This tool intentionally keeps a narrow surface: one fixed mixed read/write
 * workload across legacy PA, generic GVA, and strict GSVA identity mappings.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "obmm_common.h"

#define DP_DEFAULT_SIZE        (2UL * 1024UL * 1024UL)
#define DP_DEFAULT_ITERATIONS  32768ULL
#define DP_DEFAULT_CHUNK_SIZE  64ULL
#define DP_DEFAULT_GSVA_BASE   0x700000000000ULL
#define DP_DEFAULT_GSVA_GENERATION 0x44504d424701ULL
#define DP_BOOTSTRAP_GENERATION 0x44504d424101ULL

enum dp_mode {
    DP_MODE_LEGACY_PA = 0,
    DP_MODE_GENERIC_GVA = 1,
    DP_MODE_GSVA = 2,
};

struct dp_config {
    enum dp_mode mode;
    uint64_t size;
    uint64_t iterations;
    uint64_t chunk_size;
    uint64_t generic_pte_offset;
    uint64_t gsva_base;
    uint64_t gsva_generation;
    int node_count;
    int peer_index;
    bool verify;
};

struct dp_stats {
    uint64_t reads;
    uint64_t writes;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t verify_failures;
    long duration_ms;
};

static void dp_log(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "[obmm_dataplane_microbench] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static long dp_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void dp_fill_pattern(uint8_t *buf, uint64_t len, uint64_t seed)
{
    uint64_t i;

    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)((seed + i) * 0x9e3779b9 + 0x85ebca77);
    }
}

static bool dp_verify_pattern(const uint8_t *buf, uint64_t len, uint64_t seed)
{
    uint64_t i;

    for (i = 0; i < len; i++) {
        if (buf[i] != (uint8_t)((seed + i) * 0x9e3779b9 + 0x85ebca77)) {
            return false;
        }
    }
    return true;
}

static bool parse_u64_arg(const char *s, uint64_t *out)
{
    char *end = NULL;

    errno = 0;
    *out = strtoull(s, &end, 0);
    return errno == 0 && end != NULL && *end == '\0';
}

static bool parse_mode(const char *s, enum dp_mode *mode)
{
    if (strcmp(s, "legacy-pa") == 0 || strcmp(s, "legacy") == 0) {
        *mode = DP_MODE_LEGACY_PA;
    } else if (strcmp(s, "generic-gva") == 0 ||
               strcmp(s, "generic") == 0 ||
               strcmp(s, "gva") == 0) {
        *mode = DP_MODE_GENERIC_GVA;
    } else if (strcmp(s, "gsva") == 0) {
        *mode = DP_MODE_GSVA;
    } else {
        return false;
    }
    return true;
}

static const char *mode_name(enum dp_mode mode)
{
    switch (mode) {
    case DP_MODE_LEGACY_PA:
        return "legacy-pa";
    case DP_MODE_GENERIC_GVA:
        return "generic-gva";
    case DP_MODE_GSVA:
        return "gsva";
    default:
        return "unknown";
    }
}

static void init_config(struct dp_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = DP_MODE_LEGACY_PA;
    cfg->size = DP_DEFAULT_SIZE;
    cfg->iterations = DP_DEFAULT_ITERATIONS;
    cfg->chunk_size = DP_DEFAULT_CHUNK_SIZE;
    cfg->generic_pte_offset = 0x1000;
    cfg->gsva_base = DP_DEFAULT_GSVA_BASE;
    cfg->gsva_generation = DP_DEFAULT_GSVA_GENERATION;
    cfg->node_count = 2;
    cfg->peer_index = -1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --mode <legacy-pa|generic-gva|gsva>\n"
            "  --size <bytes>\n"
            "  --iterations <n>\n"
            "  --chunk-size <bytes>\n"
            "  --verify\n"
            "  --node-count <n>\n"
            "  --peer-index <n>\n"
            "  --generic-pte-offset <n>\n"
            "  --gsva-base <addr>\n"
            "  --gsva-generation <n>\n",
            prog);
}

static bool parse_args(int argc, char **argv, struct dp_config *cfg)
{
    int i;

    init_config(cfg);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (!parse_mode(argv[++i], &cfg->mode)) {
                fprintf(stderr, "invalid --mode %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->size)) {
                fprintf(stderr, "invalid --size %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->iterations)) {
                fprintf(stderr, "invalid --iterations %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--chunk-size") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->chunk_size)) {
                fprintf(stderr, "invalid --chunk-size %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--verify") == 0) {
            cfg->verify = true;
        } else if (strcmp(argv[i], "--node-count") == 0 && i + 1 < argc) {
            uint64_t node_count;

            if (!parse_u64_arg(argv[++i], &node_count) ||
                node_count < 2 || node_count > OBMM_POOL_HELPERS_MAX_NODES) {
                fprintf(stderr, "invalid --node-count %s\n", argv[i]);
                return false;
            }
            cfg->node_count = (int)node_count;
        } else if (strcmp(argv[i], "--peer-index") == 0 && i + 1 < argc) {
            uint64_t peer_index;

            if (!parse_u64_arg(argv[++i], &peer_index) ||
                peer_index >= OBMM_POOL_HELPERS_MAX_NODES) {
                fprintf(stderr, "invalid --peer-index %s\n", argv[i]);
                return false;
            }
            cfg->peer_index = (int)peer_index;
        } else if (strcmp(argv[i], "--generic-pte-offset") == 0 &&
                   i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->generic_pte_offset)) {
                fprintf(stderr, "invalid --generic-pte-offset %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gsva-base") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->gsva_base)) {
                fprintf(stderr, "invalid --gsva-base %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gsva-generation") == 0 &&
                   i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->gsva_generation)) {
                fprintf(stderr, "invalid --gsva-generation %s\n", argv[i]);
                return false;
            }
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return false;
        }
    }
    if (cfg->size == 0 || cfg->chunk_size == 0 ||
        cfg->chunk_size > cfg->size) {
        fprintf(stderr, "invalid size/chunk-size\n");
        return false;
    }
    if ((cfg->size % OBMM_POOL_HELPERS_IMPORT_ALIGN) != 0) {
        fprintf(stderr, "size must be aligned to %" PRIu64 "\n",
                (uint64_t)OBMM_POOL_HELPERS_IMPORT_ALIGN);
        return false;
    }
    if (cfg->mode == DP_MODE_GENERIC_GVA &&
        cfg->generic_pte_offset == 0) {
        fprintf(stderr, "generic-gva requires nonzero --generic-pte-offset\n");
        return false;
    }
    if (cfg->peer_index >= cfg->node_count) {
        fprintf(stderr, "--peer-index must be less than --node-count\n");
        return false;
    }
    if (cfg->mode == DP_MODE_GSVA &&
        cfg->size > UINT64_MAX / (uint64_t)cfg->node_count) {
        fprintf(stderr, "GSVA aperture size overflow\n");
        return false;
    }
    return true;
}

static int default_peer_index(int local_idx, int node_count)
{
    return (local_idx + 1) % node_count;
}

static int got_count(const bool got[OBMM_POOL_HELPERS_MAX_NODES],
                     int node_count)
{
    int count = 0;
    int i;

    for (i = 0; i < node_count; i++) {
        if (got[i]) {
            count++;
        }
    }
    return count;
}

static bool parse_local_ipv4_index(int node_count, int *local_idx)
{
    char local_ip[INET_ADDRSTRLEN];
    char *end = NULL;
    long octet;

    if (!obmm_env_or_cmdline("LINQU_UB_LOCAL_IP", "linqu_ipourma_ipv4",
                             local_ip, sizeof(local_ip))) {
        return false;
    }
    end = strrchr(local_ip, '.');
    if (!end || end[1] == '\0') {
        return false;
    }
    errno = 0;
    octet = strtol(end + 1, &end, 10);
    if (errno != 0 || !end || *end != '\0' || octet < 1 ||
        octet > node_count) {
        return false;
    }
    *local_idx = (int)octet - 1;
    return true;
}

static bool resolve_local_identity(uint64_t *local_cna, int *local_idx,
                                   int node_count)
{
    char value[64];

    if (obmm_cmdline_get("linqu_cna", value, sizeof(value))) {
        *local_cna = strtoull(value, NULL, 0);
        dp_log("cna from cmdline=%#x", (uint32_t)*local_cna);
    } else {
        uint64_t cna_u64 = 0;

        if (!obmm_parse_hex_u64("/sys/bus/ub/devices/00001/primary_cna",
                                &cna_u64)) {
            dp_log("cannot read local CNA");
            return false;
        }
        *local_cna = (uint32_t)cna_u64;
        dp_log("cna from sysfs=%#x", (uint32_t)*local_cna);
    }

    if (obmm_cmdline_get("linqu_node_idx", value, sizeof(value))) {
        *local_idx = (int)strtol(value, NULL, 0);
    } else if (parse_local_ipv4_index(node_count, local_idx)) {
        dp_log("local_idx from ip=%d", *local_idx);
    } else {
        char role[32] = { 0 };

        if (obmm_cmdline_get("linqu_urma_dp_role", role, sizeof(role))) {
            *local_idx = (strcmp(role, "nodeA") == 0 ||
                          strcmp(role, "exporter") == 0 ||
                          strcmp(role, "initiator") == 0 ||
                          strcmp(role, "client") == 0) ? 0 : 1;
        } else {
            *local_idx = 0;
        }
    }
    if (*local_idx < 0 || *local_idx >= node_count) {
        dp_log("invalid local_idx=%d", *local_idx);
        return false;
    }
    return true;
}

static bool register_gsva_aperture(int obmm_fd, const struct dp_config *cfg,
                                   int local_idx)
{
    struct obmm_cmd_gsva_aperture req = { 0 };
    struct obmm_cmd_gsva_aperture query = { 0 };

    req.base = cfg->gsva_base;
    req.size = cfg->size * (uint64_t)cfg->node_count;
    req.generation = cfg->gsva_generation;
    req.flags = OBMM_GSVA_APERTURE_F_ACTIVE;
    req.node_id = (uint32_t)local_idx;
    req.node_count = (uint32_t)cfg->node_count;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_REGISTER, &req) != 0) {
        dp_log("GSVA aperture register failed base=%" PRIx64
               " size=%" PRIx64 " errno=%d", req.base, req.size, errno);
        return false;
    }
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_QUERY, &query) != 0) {
        dp_log("GSVA aperture query failed errno=%d", errno);
        return false;
    }
    if (query.base != req.base || query.size != req.size ||
        query.generation != req.generation ||
        !(query.flags & OBMM_GSVA_APERTURE_F_ACTIVE)) {
        dp_log("GSVA aperture query mismatch base=%" PRIx64
               " size=%" PRIx64 " generation=%" PRIx64 " flags=%" PRIx64,
               query.base, query.size, query.generation, query.flags);
        return false;
    }
    dp_log("GSVA aperture registered base=%" PRIx64 " size=%" PRIx64
           " generation=%" PRIx64 " node=%d/%d",
           req.base, req.size, req.generation, local_idx, cfg->node_count);
    return true;
}

static void clear_gsva_aperture(int obmm_fd, uint64_t generation)
{
    struct obmm_cmd_gsva_aperture req = { 0 };

    if (obmm_fd < 0) {
        return;
    }
    req.generation = generation;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_CLEAR, &req) != 0) {
        dp_log("GSVA aperture clear failed generation=%" PRIx64
               " errno=%d", generation, errno);
    }
}

static bool run_bench(const struct dp_config *cfg, uint8_t *imported_va,
                      struct dp_stats *stats)
{
    uint8_t *tmp_write = malloc(cfg->chunk_size);
    uint8_t *tmp_read = malloc(cfg->chunk_size);
    uint64_t iter;
    bool ok = true;
    long t0;

    if (!tmp_write || !tmp_read) {
        dp_log("malloc failed");
        free(tmp_write);
        free(tmp_read);
        return false;
    }
    memset(stats, 0, sizeof(*stats));
    t0 = dp_now_ms();
    for (iter = 0; iter < cfg->iterations; iter++) {
        uint64_t offset = (iter * cfg->chunk_size) % cfg->size;

        dp_fill_pattern(tmp_write, cfg->chunk_size, offset ^ iter);
        memcpy(imported_va + offset, tmp_write, cfg->chunk_size);
        stats->writes++;
        stats->write_bytes += cfg->chunk_size;

        if (cfg->verify) {
            memcpy(tmp_read, imported_va + offset, cfg->chunk_size);
            if (!dp_verify_pattern(tmp_read, cfg->chunk_size, offset ^ iter)) {
                dp_log("verify failure offset=%" PRIu64 " iter=%" PRIu64,
                       offset, iter);
                stats->verify_failures++;
                ok = false;
                break;
            }
        }

        memcpy(tmp_read, imported_va + offset, cfg->chunk_size);
        stats->reads++;
        stats->read_bytes += cfg->chunk_size;
    }
    stats->duration_ms = dp_now_ms() - t0;
    free(tmp_write);
    free(tmp_read);
    return ok;
}

static bool completion_barrier(int obmm_fd, int local_idx, uint32_t local_cna,
                               int node_count,
                               const struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    uint64_t generation = DP_BOOTSTRAP_GENERATION + 1;

    if (obmm_bootstrap_publish(obmm_fd, local_idx, node_count, generation,
                               local_meta) != 0) {
        dp_log("completion publish failed generation=%" PRIu64
               " errno=%d", generation, errno);
        return false;
    }
    if (obmm_bootstrap_lookup(obmm_fd, local_cna, node_count, generation,
                              metas, got) != 0) {
        dp_log("completion barrier failed generation=%" PRIu64
               " errno=%d", generation, errno);
        return false;
    }
    dp_log("completion barrier ok generation=%" PRIu64 " got_count=%d node_count=%d",
           generation, got_count(got, node_count), node_count);
    return true;
}

static void print_result(const struct dp_config *cfg, const struct dp_stats *stats,
                         uint64_t import_mem_id, uint64_t local_va,
                         uint64_t home_va, uint64_t pte_offset,
                         uint64_t gva_id)
{
    double rmb = stats->read_bytes / (1024.0 * 1024.0);
    double wmb = stats->write_bytes / (1024.0 * 1024.0);
    double dur_s = stats->duration_ms > 0 ? stats->duration_ms / 1000.0 : 0.001;

    dp_log("result=done mode=%s size=%" PRIu64 " iterations=%" PRIu64
           " chunk_size=%" PRIu64 " reads=%" PRIu64 " writes=%" PRIu64
           " read_bytes=%" PRIu64 " write_bytes=%" PRIu64
           " verify_failures=%" PRIu64 " duration_ms=%ld"
           " read_mbps=%.2f write_mbps=%.2f",
           mode_name(cfg->mode), cfg->size, cfg->iterations,
           cfg->chunk_size, stats->reads, stats->writes,
           stats->read_bytes, stats->write_bytes, stats->verify_failures,
           stats->duration_ms, rmb / dur_s, wmb / dur_s);
    dp_log("mapping mode=%s import_mem_id=%" PRIx64 " gva_id=%" PRIu64
           " local_va=%" PRIx64 " home_va=%" PRIx64
           " pte_offset=%" PRIx64,
           mode_name(cfg->mode), import_mem_id, gva_id,
           local_va, home_va, pte_offset);
}

int main(int argc, char **argv)
{
    struct dp_config cfg;
    struct dp_stats stats;
    int obmm_fd = -1;
    int shmdev_fd = -1;
    uint64_t local_cna = 0;
    int local_idx = -1;
    int node_count = 0;
    int peer_idx = -1;
    struct obmm_helpers_meta local_meta = { 0 };
    struct obmm_helpers_meta remote_metas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    uint64_t local_pas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    uint64_t export_mem_id = 0;
    uint64_t import_mem_id = 0;
    uint64_t local_va = 0;
    uint64_t home_va = 0;
    uint64_t pte_offset = 0;
    uint64_t gva_id = 0;
    bool gsva_aperture_registered = false;
    struct obmm_helpers_region region = { 0 };
    void *target_va = NULL;
    int ret = 1;

    if (!parse_args(argc, argv, &cfg)) {
        usage(argv[0]);
        return 1;
    }

    dp_log("start mode=%s size=%" PRIu64 " iterations=%" PRIu64
           " chunk_size=%" PRIu64 " verify=%d",
           mode_name(cfg.mode), cfg.size, cfg.iterations,
           cfg.chunk_size, cfg.verify ? 1 : 0);

    obmm_fd = open("/dev/obmm", O_RDWR);
    if (obmm_fd < 0) {
        dp_log("open /dev/obmm failed errno=%d", errno);
        goto cleanup;
    }
    node_count = cfg.node_count;
    if (!resolve_local_identity(&local_cna, &local_idx, node_count)) {
        goto cleanup;
    }
    peer_idx = cfg.peer_index >= 0
        ? cfg.peer_index
        : default_peer_index(local_idx, node_count);
    if (peer_idx < 0 || peer_idx >= node_count || peer_idx == local_idx) {
        dp_log("invalid peer_idx=%d local_idx=%d node_count=%d",
               peer_idx, local_idx, node_count);
        goto cleanup;
    }
    dp_log("local_idx=%d peer_idx=%d node_count=%d",
           local_idx, peer_idx, node_count);

    if (cfg.mode == DP_MODE_GSVA) {
        if (!register_gsva_aperture(obmm_fd, &cfg, local_idx)) {
            goto cleanup;
        }
        gsva_aperture_registered = true;
    }

    local_meta.export_cna = (uint32_t)local_cna;
    if (cfg.mode == DP_MODE_GSVA) {
        uint64_t local_gsva = cfg.gsva_base + (uint64_t)local_idx * cfg.size;

        if (obmm_do_export_fixed_uba(obmm_fd, &local_meta, cfg.size,
                                     local_gsva) != 0) {
            dp_log("fixed GSVA export failed uba=%" PRIx64 " errno=%d",
                   local_gsva, errno);
            goto cleanup;
        }
    } else if (obmm_do_export(obmm_fd, &local_meta, cfg.size) != 0) {
        dp_log("export failed size=%" PRIu64 " errno=%d", cfg.size, errno);
        goto cleanup;
    }
    export_mem_id = local_meta.export_mem_id;
    dp_log("export ok mem_id=%" PRIx64 " uba=%" PRIx64 " token=%u",
           export_mem_id, local_meta.remote_uba, local_meta.token_id);

    if (obmm_bootstrap_publish(obmm_fd, local_idx, node_count,
                               DP_BOOTSTRAP_GENERATION, &local_meta) != 0) {
        dp_log("bootstrap publish failed errno=%d", errno);
        goto cleanup;
    }
    if (obmm_bootstrap_lookup(obmm_fd, (uint32_t)local_cna, node_count,
                              DP_BOOTSTRAP_GENERATION, remote_metas,
                              got) != 0) {
        dp_log("bootstrap lookup failed errno=%d", errno);
        goto cleanup;
    }
    dp_log("bootstrap lookup ok got_count=%d node_count=%d peer_got=%d",
           got_count(got, node_count), node_count, got[peer_idx]);
    if (!got[peer_idx]) {
        dp_log("remote peer not found");
        goto cleanup;
    }

    if (!obmm_alloc_import_pas(1, cfg.size, local_pas, import_osync,
                               obmm_parse_import_cache_mode())) {
        dp_log("cannot allocate import PA");
        goto cleanup;
    }

    if (cfg.mode == DP_MODE_LEGACY_PA) {
        if (obmm_do_import(obmm_fd, &remote_metas[peer_idx],
                           (uint32_t)local_cna, local_pas[0],
                           remote_metas[peer_idx].token_id,
                           &import_mem_id) != 0) {
            dp_log("legacy import failed errno=%d", errno);
            goto cleanup;
        }
    } else if (cfg.mode == DP_MODE_GENERIC_GVA) {
        pte_offset = cfg.generic_pte_offset;
        if (remote_metas[peer_idx].remote_uba < pte_offset) {
            dp_log("generic GVA local_va underflow remote_uba=%" PRIx64
                   " pte_offset=%" PRIx64,
                   remote_metas[peer_idx].remote_uba, pte_offset);
            goto cleanup;
        }
        local_va = remote_metas[peer_idx].remote_uba - pte_offset;
        home_va = local_va;
        if (obmm_do_import_v2(obmm_fd, &remote_metas[peer_idx],
                              (uint32_t)local_cna, local_pas[0],
                              remote_metas[peer_idx].token_id,
                              OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM,
                              OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
                              OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH,
                              0, 0, 0, 0, 0, 0,
                              local_va, home_va, pte_offset,
                              &import_mem_id) != 0) {
            dp_log("generic GVA import failed errno=%d", errno);
            goto cleanup;
        }
        target_va = (void *)(uintptr_t)local_va;
    } else {
        local_va = remote_metas[peer_idx].remote_uba;
        home_va = local_va;
        gva_id = (uint64_t)peer_idx + 1;
        if (obmm_do_import_v2(obmm_fd, &remote_metas[peer_idx],
                              (uint32_t)local_cna, local_pas[0],
                              remote_metas[peer_idx].token_id,
                              OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                              OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                              OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH,
                              0, 0, 0, 0, 0, gva_id,
                              local_va, home_va, 0,
                              &import_mem_id) != 0) {
            dp_log("GSVA import failed errno=%d", errno);
            goto cleanup;
        }
        target_va = (void *)(uintptr_t)local_va;
    }
    dp_log("import ok mem_id=%" PRIx64, import_mem_id);

    if (cfg.mode == DP_MODE_GSVA) {
        if (obmm_map_gsva_region_at(import_mem_id, target_va, cfg.size,
                                    import_osync[0], &region) != 0) {
            dp_log("MAP_GSVA failed mem_id=%" PRIx64 " errno=%d",
                   import_mem_id, errno);
            goto cleanup;
        }
    } else if (obmm_map_region_at(import_mem_id, target_va, cfg.size,
                                  import_osync[0], &region) != 0) {
        dp_log("mmap failed mem_id=%" PRIx64 " errno=%d", import_mem_id, errno);
        goto cleanup;
    }
    if (target_va && region.addr != target_va) {
        dp_log("map mismatch got=%p expect=%p", region.addr, target_va);
        goto cleanup;
    }
    shmdev_fd = region.fd;
    dp_log("setup complete import_va=%p shmdev_fd=%d", region.addr, shmdev_fd);

    if (!run_bench(&cfg, region.addr, &stats)) {
        dp_log("bench failed");
        goto cleanup;
    }
    if (!completion_barrier(obmm_fd, local_idx, (uint32_t)local_cna,
                            node_count, &local_meta)) {
        goto cleanup;
    }
    print_result(&cfg, &stats, import_mem_id, local_va, home_va, pte_offset,
                 gva_id);
    ret = 0;

cleanup:
    if (region.addr) {
        munmap(region.addr, cfg.size);
    }
    if (shmdev_fd >= 0) {
        close(shmdev_fd);
        shmdev_fd = -1;
        region.fd = -1;
    }
    if (import_mem_id && obmm_fd >= 0) {
        obmm_do_unimport(obmm_fd, import_mem_id);
    }
    if (export_mem_id && obmm_fd >= 0) {
        obmm_do_unexport(obmm_fd, export_mem_id);
    }
    if (gsva_aperture_registered) {
        clear_gsva_aperture(obmm_fd, cfg.gsva_generation);
    }
    if (obmm_fd >= 0) {
        close(obmm_fd);
    }
    return ret;
}
