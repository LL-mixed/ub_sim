/* SPDX-License-Identifier: GPL-2.0 */

#define _GNU_SOURCE
#include "obmm_common.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAG "[obmm_gsva_demo]"
#define GSVA_DEMO_DEFAULT_SIZE (4UL * 1024UL * 1024UL)
#define GSVA_DEMO_DEFAULT_BASE 0x700000000000ULL
#define GSVA_DEMO_GENERATION 0x475356410101ULL
#define GSVA_DEMO_MAGIC 0x4753564144454d4fULL
#define GSVA_DEMO_A 0x1111222233334444ULL
#define GSVA_DEMO_B 0xaaaabbbbccccddddULL
#define GSVA_DEMO_MATRIX_VALUE_BASE 0x4753564d00000000ULL
#define GSVA_DEMO_TIMEOUT_MS 90000

enum gsva_demo_mode {
    GSVA_DEMO_IDENTITY,
    GSVA_DEMO_CONFLICT,
    GSVA_DEMO_STALE_GENERATION,
    GSVA_DEMO_INVALID_OFFSET,
    GSVA_DEMO_MATRIX,
    GSVA_DEMO_MMAP_MODE,
    GSVA_DEMO_OUTSIDE_APERTURE,
    GSVA_DEMO_OUTSIDE_IMPORT,
};

struct gsva_demo_config {
    enum gsva_demo_mode mode;
    uint64_t base;
    uint64_t size;
    int node_count;
};

struct gsva_demo_payload {
    volatile uint64_t magic;
    volatile uint64_t phase;
    volatile uint64_t value;
    volatile uint64_t home_ptr;
    volatile uint64_t peer_ptr;
};

struct gsva_matrix_payload {
    volatile uint64_t magic;
    volatile uint64_t phase;
    volatile uint64_t owner_node;
    volatile uint64_t node_count;
    volatile uint64_t ptr;
    volatile uint64_t values[OBMM_POOL_HELPERS_MAX_NODES];
};

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

static bool parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0')
        return false;
    *out = (uint64_t)value;
    return true;
}

static bool parse_args(int argc, char **argv, struct gsva_demo_config *cfg)
{
    int i;

    cfg->mode = GSVA_DEMO_IDENTITY;
    cfg->base = GSVA_DEMO_DEFAULT_BASE;
    cfg->size = GSVA_DEMO_DEFAULT_SIZE;
    cfg->node_count = 2;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "identity") == 0) {
                cfg->mode = GSVA_DEMO_IDENTITY;
            } else if (strcmp(mode, "conflict") == 0) {
                cfg->mode = GSVA_DEMO_CONFLICT;
            } else if (strcmp(mode, "stale-generation") == 0) {
                cfg->mode = GSVA_DEMO_STALE_GENERATION;
            } else if (strcmp(mode, "invalid-offset") == 0) {
                cfg->mode = GSVA_DEMO_INVALID_OFFSET;
            } else if (strcmp(mode, "matrix") == 0) {
                cfg->mode = GSVA_DEMO_MATRIX;
            } else if (strcmp(mode, "mmap-mode") == 0) {
                cfg->mode = GSVA_DEMO_MMAP_MODE;
            } else if (strcmp(mode, "outside-aperture") == 0) {
                cfg->mode = GSVA_DEMO_OUTSIDE_APERTURE;
            } else if (strcmp(mode, "outside-import") == 0) {
                cfg->mode = GSVA_DEMO_OUTSIDE_IMPORT;
            } else {
                return false;
            }
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &cfg->base))
                return false;
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &cfg->size))
                return false;
        } else if (strcmp(argv[i], "--node-count") == 0 && i + 1 < argc) {
            uint64_t value;

            if (!parse_u64(argv[++i], &value) ||
                value < 2 || value > OBMM_POOL_HELPERS_MAX_NODES)
                return false;
            cfg->node_count = (int)value;
        } else {
            return false;
        }
    }

    if (cfg->mode != GSVA_DEMO_MATRIX)
        cfg->node_count = 2;

    return cfg->base != 0 &&
           cfg->size >= sizeof(struct gsva_matrix_payload) &&
           cfg->size <= UINT64_MAX / (uint64_t)cfg->node_count &&
           (cfg->base & 4095ULL) == 0 && (cfg->size & 4095ULL) == 0;
}

static int get_local_identity(uint32_t *local_cna, int *local_idx)
{
    char value[64];
    uint64_t cna_u64 = 0;
    char role[32] = {0};

    if (obmm_cmdline_get("linqu_cna", value, sizeof(value))) {
        *local_cna = (uint32_t)strtoull(value, NULL, 0);
    } else if (obmm_parse_hex_u64("/sys/bus/ub/devices/00001/primary_cna",
                                  &cna_u64)) {
        *local_cna = (uint32_t)cna_u64;
    } else {
        return -1;
    }

    if (obmm_cmdline_get("linqu_node_idx", value, sizeof(value))) {
        *local_idx = (int)strtol(value, NULL, 0);
    } else if (obmm_cmdline_get("linqu_urma_dp_role", role, sizeof(role))) {
        *local_idx = (strcmp(role, "nodeA") == 0 ||
                      strcmp(role, "exporter") == 0 ||
                      strcmp(role, "initiator") == 0 ||
                      strcmp(role, "client") == 0) ? 0 : 1;
    } else {
        *local_idx = 0;
    }
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

static int register_aperture(int obmm_fd, const struct gsva_demo_config *cfg,
                             int local_idx)
{
    struct obmm_cmd_gsva_aperture req = {0};
    struct obmm_cmd_gsva_aperture query = {0};
    uint64_t aperture_size = cfg->size;

    if (cfg->mode == GSVA_DEMO_MATRIX)
        aperture_size *= (uint64_t)cfg->node_count;

    req.base = cfg->base;
    req.size = aperture_size;
    req.generation = GSVA_DEMO_GENERATION;
    req.flags = OBMM_GSVA_APERTURE_F_ACTIVE;
    req.node_id = (uint32_t)local_idx;
    req.node_count = (uint32_t)cfg->node_count;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_REGISTER, &req) != 0)
        return -1;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_QUERY, &query) != 0)
        return -1;
    if (query.base != cfg->base || query.size != aperture_size ||
        query.generation != GSVA_DEMO_GENERATION ||
        !(query.flags & OBMM_GSVA_APERTURE_F_ACTIVE)) {
        errno = EINVAL;
        return -1;
    }
    log_kernel_aperture_proc();
    return 0;
}

static int clear_aperture(int obmm_fd, uint64_t generation)
{
    struct obmm_cmd_gsva_aperture req = {0};

    req.generation = generation;
    return ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_CLEAR, &req);
}

static int wait_phase(volatile uint64_t *phase, uint64_t expect)
{
    long deadline = obmm_now_ms() + GSVA_DEMO_TIMEOUT_MS;

    while (obmm_now_ms() < deadline) {
        if (*phase == expect)
            return 0;
        usleep(1000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static uint64_t matrix_slot_base(const struct gsva_demo_config *cfg, int node_idx)
{
    return cfg->base + (uint64_t)node_idx * cfg->size;
}

static uint64_t matrix_value(int writer_idx, int owner_idx)
{
    return GSVA_DEMO_MATRIX_VALUE_BASE |
           ((uint64_t)(uint32_t)writer_idx << 8) |
           (uint64_t)(uint32_t)owner_idx;
}

static int wait_matrix_ready(struct gsva_matrix_payload *payload, int owner,
                             int node_count, uint64_t ptr)
{
    long deadline = obmm_now_ms() + GSVA_DEMO_TIMEOUT_MS;

    while (obmm_now_ms() < deadline) {
        if (payload->phase >= 1 &&
            payload->magic == GSVA_DEMO_MAGIC &&
            payload->owner_node == (uint64_t)owner &&
            payload->node_count == (uint64_t)node_count &&
            payload->ptr == ptr)
            return 0;
        usleep(1000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int wait_matrix_done(struct gsva_matrix_payload *payload)
{
    long deadline = obmm_now_ms() + GSVA_DEMO_TIMEOUT_MS;

    while (obmm_now_ms() < deadline) {
        if (payload->phase >= 2)
            return 0;
        usleep(1000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int run_matrix(int obmm_fd, uint32_t local_cna,
                      const struct gsva_demo_config *cfg, int local_idx,
                      struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    struct obmm_helpers_region regions[OBMM_POOL_HELPERS_MAX_NODES];
    struct gsva_matrix_payload *payloads[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    uint64_t local_base = matrix_slot_base(cfg, local_idx);
    int import_count = cfg->node_count - 1;
    int import_idx = 0;
    int owner;
    int writer;
    int ret = -1;

    memset(regions, 0, sizeof(regions));
    for (owner = 0; owner < OBMM_POOL_HELPERS_MAX_NODES; owner++)
        regions[owner].fd = -1;

    local_meta->export_cna = local_cna;
    if (obmm_do_export_fixed_uba(obmm_fd, local_meta, cfg->size,
                                 local_base) != 0)
        return -1;
    if (local_meta->remote_uba != local_base) {
        log_msg("matrix fixed export returned wrong uba=%#" PRIx64
                " expected=%#" PRIx64, local_meta->remote_uba, local_base);
        errno = EINVAL;
        goto out_unexport;
    }
    if (obmm_bootstrap_publish(obmm_fd, local_idx, cfg->node_count,
                               GSVA_DEMO_GENERATION, local_meta) != 0)
        goto out_unexport;
    if (obmm_map_gsva_region_at(local_meta->export_mem_id,
                                (void *)(uintptr_t)local_base, cfg->size,
                                false, &regions[local_idx]) != 0)
        goto out_unexport;
    payloads[local_idx] =
        (struct gsva_matrix_payload *)regions[local_idx].addr;
    memset(payloads[local_idx], 0, sizeof(*payloads[local_idx]));
    payloads[local_idx]->magic = GSVA_DEMO_MAGIC;
    payloads[local_idx]->owner_node = (uint64_t)local_idx;
    payloads[local_idx]->node_count = (uint64_t)cfg->node_count;
    payloads[local_idx]->ptr = local_base;
    __sync_synchronize();
    payloads[local_idx]->phase = 1;

    if (obmm_bootstrap_lookup(obmm_fd, local_cna, cfg->node_count,
                              GSVA_DEMO_GENERATION, metas, got) != 0)
        goto out_cleanup;

    if (!obmm_alloc_import_pas(import_count, cfg->size, import_pas,
                               import_osync, obmm_parse_import_cache_mode()))
        goto out_cleanup;

    for (owner = 0; owner < cfg->node_count; owner++) {
        uint64_t slot_base = matrix_slot_base(cfg, owner);

        if (owner == local_idx)
            continue;
        if (!got[owner] || metas[owner].remote_uba != slot_base ||
            metas[owner].size != cfg->size) {
            errno = EINVAL;
            goto out_cleanup;
        }
        if (obmm_do_import_v2(obmm_fd, &metas[owner], local_cna,
                              import_pas[import_idx], 0,
                              OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                              OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                              OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH,
                              0, 0, 0, 0, 0, (uint64_t)owner + 1,
                              slot_base, slot_base, 0,
                              &import_mem_id[owner]) != 0)
            goto out_cleanup;
        if (obmm_map_gsva_region_at(import_mem_id[owner],
                                    (void *)(uintptr_t)slot_base, cfg->size,
                                    import_osync[import_idx],
                                    &regions[owner]) != 0)
            goto out_cleanup;
        payloads[owner] = (struct gsva_matrix_payload *)regions[owner].addr;
        import_idx++;
    }

    for (owner = 0; owner < cfg->node_count; owner++) {
        uint64_t slot_base = matrix_slot_base(cfg, owner);

        if (!payloads[owner] ||
            wait_matrix_ready(payloads[owner], owner, cfg->node_count,
                              slot_base) != 0)
            goto out_cleanup;
    }

    for (owner = 0; owner < cfg->node_count; owner++)
        payloads[owner]->values[local_idx] = matrix_value(local_idx, owner);
    __sync_synchronize();

    for (owner = 0; owner < cfg->node_count; owner++) {
        for (writer = 0; writer < cfg->node_count; writer++) {
            long deadline = obmm_now_ms() + GSVA_DEMO_TIMEOUT_MS;
            uint64_t expect = matrix_value(writer, owner);

            while (obmm_now_ms() < deadline &&
                   payloads[owner]->values[writer] != expect) {
                usleep(1000);
            }
            if (payloads[owner]->values[writer] != expect) {
                log_msg("matrix verify failed owner=%d writer=%d value=%#"
                        PRIx64 " expect=%#" PRIx64,
                        owner, writer,
                        (uint64_t)payloads[owner]->values[writer], expect);
                errno = ETIMEDOUT;
                goto out_cleanup;
            }
        }
    }

    payloads[local_idx]->phase = 2;
    __sync_synchronize();
    for (owner = 0; owner < cfg->node_count; owner++) {
        if (wait_matrix_done(payloads[owner]) != 0)
            goto out_cleanup;
    }

    log_msg("result=done mode=matrix node=%d node_count=%d slice_base=%#"
            PRIx64 " ptr=%#" PRIx64 " value_from_node0=%#" PRIx64
            " value_from_last=%#" PRIx64,
            local_idx, cfg->node_count, local_base, local_base,
            (uint64_t)payloads[local_idx]->values[0],
            (uint64_t)payloads[local_idx]->values[cfg->node_count - 1]);
    ret = 0;

out_cleanup:
    for (owner = 0; owner < cfg->node_count; owner++)
        obmm_unmap_region(&regions[owner]);
    for (owner = 0; owner < cfg->node_count; owner++) {
        if (import_mem_id[owner])
            (void)obmm_do_unimport(obmm_fd, import_mem_id[owner]);
    }
out_unexport:
    if (local_meta->export_mem_id)
        (void)obmm_do_unexport(obmm_fd, local_meta->export_mem_id);
    return ret;
}

static int run_identity_home(int obmm_fd, uint32_t local_cna,
                             const struct gsva_demo_config *cfg,
                             struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_region region = {0};
    struct gsva_demo_payload *payload;
    int ret = -1;

    local_meta->export_cna = local_cna;
    if (obmm_do_export_fixed_uba(obmm_fd, local_meta, cfg->size,
                                 cfg->base) != 0)
        return -1;
    if (local_meta->remote_uba != cfg->base) {
        log_msg("fixed export returned wrong uba=%#" PRIx64 " expected=%#"
                PRIx64, local_meta->remote_uba, cfg->base);
        errno = EINVAL;
        goto out_unexport;
    }
    log_msg("fixed export -> ok mem_id=%#" PRIx64 " uba=%#" PRIx64
            " token=%u", local_meta->export_mem_id, local_meta->remote_uba,
            local_meta->token_id);

    if (obmm_bootstrap_publish(obmm_fd, 0, 2, GSVA_DEMO_GENERATION,
                               local_meta) != 0)
        goto out_unexport;

    if (obmm_map_gsva_region_at(local_meta->export_mem_id,
                                (void *)(uintptr_t)cfg->base, cfg->size,
                                false, &region) != 0)
        goto out_unexport;
    if ((uint64_t)(uintptr_t)region.addr != cfg->base) {
        errno = EINVAL;
        goto out_unmap;
    }

    payload = (struct gsva_demo_payload *)region.addr;
    memset(payload, 0, sizeof(*payload));
    payload->magic = GSVA_DEMO_MAGIC;
    payload->value = GSVA_DEMO_A;
    payload->home_ptr = cfg->base;
    __sync_synchronize();
    payload->phase = 1;
    log_msg("home wrote value=%#" PRIx64 " ptr=%#" PRIx64,
            (uint64_t)GSVA_DEMO_A, cfg->base);

    if (wait_phase(&payload->phase, 2) != 0)
        goto out_unmap;
    if (payload->value != GSVA_DEMO_B || payload->peer_ptr != cfg->base) {
        log_msg("home verify failed value=%#" PRIx64 " peer_ptr=%#" PRIx64,
                (uint64_t)payload->value, (uint64_t)payload->peer_ptr);
        errno = EIO;
        goto out_unmap;
    }

    log_msg("result=done mode=identity role=home ptr=%#" PRIx64
            " home_va=%#" PRIx64 " uba=%#" PRIx64 " value=%#" PRIx64,
            cfg->base, cfg->base, local_meta->remote_uba,
            (uint64_t)payload->value);
    ret = 0;

out_unmap:
    obmm_unmap_region(&region);
out_unexport:
    if (local_meta->export_mem_id)
        (void)obmm_do_unexport(obmm_fd, local_meta->export_mem_id);
    return ret;
}

static int run_identity_peer(int obmm_fd, uint32_t local_cna,
                             const struct gsva_demo_config *cfg,
                             struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region region = {0};
    struct gsva_demo_payload *payload;
    int ret = -1;

    local_meta->export_cna = local_cna;
    if (obmm_do_export(obmm_fd, local_meta, cfg->size) != 0)
        return -1;
    if (obmm_bootstrap_publish(obmm_fd, 1, 2, GSVA_DEMO_GENERATION,
                               local_meta) != 0)
        goto out_unexport;
    if (obmm_bootstrap_lookup(obmm_fd, local_cna, 2, GSVA_DEMO_GENERATION,
                              metas, got) != 0)
        goto out_unexport;
    if (!got[0] || metas[0].remote_uba != cfg->base) {
        errno = EINVAL;
        goto out_unexport;
    }
    if (!obmm_alloc_import_pas(1, cfg->size, import_pas, import_osync,
                               obmm_parse_import_cache_mode()))
        goto out_unexport;

    if (obmm_do_import_v2(obmm_fd, &metas[0], local_cna, import_pas[0], 0,
                          OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                          OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                          OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH,
                          0, 0, 0, 0, 0, 1, cfg->base, cfg->base, 0,
                          &import_mem_id) != 0)
        goto out_unexport;
    if (obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)cfg->base,
                                cfg->size, import_osync[0], &region) != 0)
        goto out_unimport;
    if ((uint64_t)(uintptr_t)region.addr != cfg->base) {
        errno = EINVAL;
        goto out_unmap;
    }

    payload = (struct gsva_demo_payload *)region.addr;
    if (wait_phase(&payload->phase, 1) != 0)
        goto out_unmap;
    if (payload->magic != GSVA_DEMO_MAGIC || payload->value != GSVA_DEMO_A ||
        payload->home_ptr != cfg->base) {
        log_msg("peer verify A failed magic=%#" PRIx64 " value=%#" PRIx64
                " home_ptr=%#" PRIx64,
                (uint64_t)payload->magic, (uint64_t)payload->value,
                (uint64_t)payload->home_ptr);
        errno = EIO;
        goto out_unmap;
    }

    payload->value = GSVA_DEMO_B;
    payload->peer_ptr = cfg->base;
    __sync_synchronize();
    payload->phase = 2;
    log_msg("result=done mode=identity role=peer ptr=%#" PRIx64
            " user_va=%#" PRIx64 " uba=%#" PRIx64 " value=%#" PRIx64,
            cfg->base, cfg->base, metas[0].remote_uba,
            (uint64_t)GSVA_DEMO_B);
    ret = 0;

out_unmap:
    obmm_unmap_region(&region);
out_unimport:
    if (import_mem_id)
        (void)obmm_do_unimport(obmm_fd, import_mem_id);
out_unexport:
    if (local_meta->export_mem_id)
        (void)obmm_do_unexport(obmm_fd, local_meta->export_mem_id);
    return ret;
}

static int run_conflict(int obmm_fd, const struct gsva_demo_config *cfg,
                        int local_idx)
{
    struct obmm_helpers_meta meta = {0};
    struct obmm_helpers_region region = {0};
    int saved_errno;

    if (register_aperture(obmm_fd, cfg, local_idx) != 0)
        return -1;

    if (obmm_do_export(obmm_fd, &meta, cfg->size) != 0)
        return -1;

    if (obmm_map_region_at(meta.export_mem_id, (void *)(uintptr_t)cfg->base,
                           cfg->size, false, &region) == 0) {
        obmm_unmap_region(&region);
        (void)obmm_do_unexport(obmm_fd, meta.export_mem_id);
        (void)clear_aperture(obmm_fd, GSVA_DEMO_GENERATION);
        log_msg("result=fail mode=conflict reason=normal-obmm-mmap-entered-gsva-aperture");
        errno = EINVAL;
        return -1;
    }

    saved_errno = errno;
    (void)obmm_do_unexport(obmm_fd, meta.export_mem_id);
    (void)clear_aperture(obmm_fd, GSVA_DEMO_GENERATION);
    log_msg("result=done mode=conflict role=%s reason=normal-obmm-mmap-rejected errno=%d",
            local_idx == 0 ? "home" : "peer", saved_errno);
    return 0;
}

static int run_mmap_mode(int obmm_fd, const struct gsva_demo_config *cfg,
                         int local_idx)
{
    struct obmm_helpers_meta gsva_meta = {0};
    struct obmm_helpers_meta normal_meta = {0};
    struct obmm_helpers_region region = {0};
    int saved_errno;
    bool gsva_mapped = false;
    int ret = -1;

    if (obmm_do_export_fixed_uba(obmm_fd, &gsva_meta, cfg->size,
                                 cfg->base) != 0)
        return -1;
    if (gsva_meta.remote_uba != cfg->base) {
        errno = EINVAL;
        goto out_gsva;
    }

    if (obmm_map_gsva_region_at(gsva_meta.export_mem_id,
                                (void *)(uintptr_t)cfg->base, cfg->size,
                                false, &region) != 0)
        goto out_gsva;
    if ((uint64_t)(uintptr_t)region.addr != cfg->base) {
        errno = EINVAL;
        goto out_unmap_gsva;
    }
    gsva_mapped = true;
    log_msg("mmap-mode gsva segment -> ok ptr=%#" PRIx64 " uba=%#"
            PRIx64, cfg->base, gsva_meta.remote_uba);

out_unmap_gsva:
    obmm_unmap_region(&region);
    if (!gsva_mapped)
        goto out_gsva;

    if (obmm_do_export(obmm_fd, &normal_meta, cfg->size) != 0)
        goto out_gsva;
    if (obmm_map_region_at(normal_meta.export_mem_id,
                           (void *)(uintptr_t)cfg->base, cfg->size, false,
                           &region) == 0) {
        obmm_unmap_region(&region);
        log_msg("result=fail mode=mmap-mode role=%s reason=normal-obmm-mmap-entered-gsva-aperture",
                local_idx == 0 ? "home" : "peer");
        errno = EINVAL;
        goto out_normal;
    }
    saved_errno = errno;
    if (saved_errno != EINVAL && saved_errno != EEXIST) {
        log_msg("mmap-mode normal mmap rejected with unexpected errno=%d",
                saved_errno);
        errno = saved_errno;
        goto out_normal;
    }

    log_msg("result=done mode=mmap-mode role=%s gsva_ptr=%#" PRIx64
            " normal_reject_errno=%d",
            local_idx == 0 ? "home" : "peer", cfg->base, saved_errno);
    ret = 0;

out_normal:
    if (normal_meta.export_mem_id)
        (void)obmm_do_unexport(obmm_fd, normal_meta.export_mem_id);
out_gsva:
    if (gsva_meta.export_mem_id)
        (void)obmm_do_unexport(obmm_fd, gsva_meta.export_mem_id);
    return ret;
}

static int run_outside_aperture(int obmm_fd, const struct gsva_demo_config *cfg,
                                int local_idx)
{
    struct obmm_helpers_meta meta = {0};
    uint64_t outside_base;
    int saved_errno;

    if (register_aperture(obmm_fd, cfg, local_idx) != 0)
        return -1;
    if (UINT64_MAX - cfg->base < cfg->size) {
        (void)clear_aperture(obmm_fd, GSVA_DEMO_GENERATION);
        errno = EOVERFLOW;
        return -1;
    }
    outside_base = cfg->base + cfg->size;

    if (obmm_do_export_fixed_uba(obmm_fd, &meta, cfg->size,
                                 outside_base) == 0) {
        (void)obmm_do_unexport(obmm_fd, meta.export_mem_id);
        (void)clear_aperture(obmm_fd, GSVA_DEMO_GENERATION);
        log_msg("result=fail mode=outside-aperture role=%s reason=fixed-export-accepted uba=%#"
                PRIx64,
                local_idx == 0 ? "home" : "peer", outside_base);
        errno = EINVAL;
        return -1;
    }

    saved_errno = errno;
    (void)clear_aperture(obmm_fd, GSVA_DEMO_GENERATION);
    log_msg("result=done mode=outside-aperture role=%s rejected_uba=%#"
            PRIx64 " errno=%d",
            local_idx == 0 ? "home" : "peer", outside_base, saved_errno);
    return 0;
}

static int run_stale_generation(int obmm_fd, const struct gsva_demo_config *cfg,
                                int local_idx)
{
    struct obmm_cmd_gsva_aperture query = {0};
    uint64_t stale_generation = GSVA_DEMO_GENERATION + 1;
    int saved_errno;

    if (register_aperture(obmm_fd, cfg, local_idx) != 0)
        return -1;

    if (clear_aperture(obmm_fd, stale_generation) == 0) {
        log_msg("result=fail mode=stale-generation reason=stale-clear-accepted");
        errno = EINVAL;
        return -1;
    }
    saved_errno = errno;

    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_QUERY, &query) != 0)
        return -1;
    if (query.base != cfg->base || query.size != cfg->size ||
        query.generation != GSVA_DEMO_GENERATION ||
        !(query.flags & OBMM_GSVA_APERTURE_F_ACTIVE)) {
        log_msg("result=fail mode=stale-generation reason=aperture-mutated");
        errno = EINVAL;
        return -1;
    }

    (void)clear_aperture(obmm_fd, GSVA_DEMO_GENERATION);
    log_msg("result=done mode=stale-generation role=%s stale_generation=%#"
            PRIx64 " errno=%d",
            local_idx == 0 ? "home" : "peer", stale_generation, saved_errno);
    return 0;
}

static int run_invalid_offset_home(int obmm_fd, uint32_t local_cna,
                                   const struct gsva_demo_config *cfg,
                                   struct obmm_helpers_meta *local_meta)
{
    int ret = -1;

    local_meta->export_cna = local_cna;
    if (obmm_do_export_fixed_uba(obmm_fd, local_meta, cfg->size,
                                 cfg->base) != 0)
        return -1;
    if (local_meta->remote_uba != cfg->base) {
        errno = EINVAL;
        goto out_unexport;
    }
    if (obmm_bootstrap_publish(obmm_fd, 0, 2, GSVA_DEMO_GENERATION,
                               local_meta) != 0)
        goto out_unexport;

    usleep(3000000);
    log_msg("result=done mode=invalid-offset role=home uba=%#" PRIx64,
            local_meta->remote_uba);
    ret = 0;

out_unexport:
    if (local_meta->export_mem_id)
        (void)obmm_do_unexport(obmm_fd, local_meta->export_mem_id);
    return ret;
}

static int run_invalid_offset_peer(int obmm_fd, uint32_t local_cna,
                                   const struct gsva_demo_config *cfg,
                                   struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    uint64_t bad_pte_offset = 0x1000;
    int saved_errno;
    int ret = -1;

    local_meta->export_cna = local_cna;
    if (obmm_do_export(obmm_fd, local_meta, cfg->size) != 0)
        return -1;
    if (obmm_bootstrap_publish(obmm_fd, 1, 2, GSVA_DEMO_GENERATION,
                               local_meta) != 0)
        goto out_unexport;
    if (obmm_bootstrap_lookup(obmm_fd, local_cna, 2, GSVA_DEMO_GENERATION,
                              metas, got) != 0)
        goto out_unexport;
    if (!got[0] || metas[0].remote_uba != cfg->base) {
        errno = EINVAL;
        goto out_unexport;
    }
    if (!obmm_alloc_import_pas(1, cfg->size, import_pas, import_osync,
                               obmm_parse_import_cache_mode()))
        goto out_unexport;

    if (obmm_do_import_v2(obmm_fd, &metas[0], local_cna, import_pas[0], 0,
                          OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                          OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                          OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH,
                          0, 0, 0, 0, 0, 1, cfg->base, cfg->base,
                          bad_pte_offset, &import_mem_id) == 0) {
        (void)obmm_do_unimport(obmm_fd, import_mem_id);
        log_msg("result=fail mode=invalid-offset role=peer reason=import-accepted");
        errno = EINVAL;
        goto out_unexport;
    }

    saved_errno = errno;
    log_msg("result=done mode=invalid-offset role=peer bad_pte_offset=%#"
            PRIx64 " errno=%d",
            bad_pte_offset, saved_errno);
    ret = 0;

out_unexport:
    if (local_meta->export_mem_id)
        (void)obmm_do_unexport(obmm_fd, local_meta->export_mem_id);
    return ret;
}

static int run_outside_import_home(int obmm_fd, uint32_t local_cna,
                                   const struct gsva_demo_config *cfg,
                                   struct obmm_helpers_meta *local_meta)
{
    int ret = -1;

    local_meta->export_cna = local_cna;
    if (obmm_do_export(obmm_fd, local_meta, cfg->size) != 0)
        return -1;
    if (obmm_bootstrap_publish(obmm_fd, 0, 2, GSVA_DEMO_GENERATION,
                               local_meta) != 0)
        goto out_unexport;

    usleep(3000000);
    log_msg("result=done mode=outside-import role=home published_uba=%#"
            PRIx64, local_meta->remote_uba);
    ret = 0;

out_unexport:
    if (local_meta->export_mem_id)
        (void)obmm_do_unexport(obmm_fd, local_meta->export_mem_id);
    return ret;
}

static int run_outside_import_peer(int obmm_fd, uint32_t local_cna,
                                   const struct gsva_demo_config *cfg,
                                   struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    struct obmm_helpers_meta import_meta;
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    uint64_t outside_base;
    int saved_errno;
    int ret = -1;

    if (UINT64_MAX - cfg->base < cfg->size) {
        errno = EOVERFLOW;
        return -1;
    }
    outside_base = cfg->base + cfg->size;

    local_meta->export_cna = local_cna;
    if (obmm_do_export(obmm_fd, local_meta, cfg->size) != 0)
        return -1;
    if (obmm_bootstrap_publish(obmm_fd, 1, 2, GSVA_DEMO_GENERATION,
                               local_meta) != 0)
        goto out_unexport;
    if (obmm_bootstrap_lookup(obmm_fd, local_cna, 2, GSVA_DEMO_GENERATION,
                              metas, got) != 0)
        goto out_unexport;
    if (!got[0]) {
        errno = EINVAL;
        goto out_unexport;
    }
    if (!obmm_alloc_import_pas(1, cfg->size, import_pas, import_osync,
                               obmm_parse_import_cache_mode()))
        goto out_unexport;

    import_meta = metas[0];
    import_meta.remote_uba = outside_base;
    if (obmm_do_import_v2(obmm_fd, &import_meta, local_cna, import_pas[0], 0,
                          OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                          OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                          OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH,
                          0, 0, 0, 0, 0, 1, outside_base, outside_base,
                          0, &import_mem_id) == 0) {
        (void)obmm_do_unimport(obmm_fd, import_mem_id);
        log_msg("result=fail mode=outside-import role=peer reason=import-accepted uba=%#"
                PRIx64, outside_base);
        errno = EINVAL;
        goto out_unexport;
    }

    saved_errno = errno;
    log_msg("result=done mode=outside-import role=peer rejected_uba=%#"
            PRIx64 " errno=%d",
            outside_base, saved_errno);
    ret = 0;

out_unexport:
    if (local_meta->export_mem_id)
        (void)obmm_do_unexport(obmm_fd, local_meta->export_mem_id);
    return ret;
}

int main(int argc, char **argv)
{
    struct gsva_demo_config cfg;
    struct obmm_helpers_meta local_meta = {0};
    uint32_t local_cna = 0;
    int local_idx = -1;
    int obmm_fd = -1;
    int ret = 1;

    if (!parse_args(argc, argv, &cfg)) {
        fprintf(stderr, "usage: obmm_gsva_demo --mode identity|conflict|stale-generation|invalid-offset|matrix|mmap-mode|outside-aperture|outside-import "
                "[--base A] [--size S] [--node-count N]\n");
        return 2;
    }
    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        log_msg("open /dev/obmm failed errno=%d", errno);
        return 1;
    }
    if (get_local_identity(&local_cna, &local_idx) != 0) {
        log_msg("resolve local identity failed");
        goto out;
    }
    log_msg("start mode=%d node=%d count=%d cna=%#x base=%#" PRIx64
            " size=%#" PRIx64, cfg.mode, local_idx, cfg.node_count,
            local_cna, cfg.base, cfg.size);

    if (cfg.mode == GSVA_DEMO_CONFLICT) {
        ret = run_conflict(obmm_fd, &cfg, local_idx) == 0 ? 0 : 1;
        goto out;
    }
    if (cfg.mode == GSVA_DEMO_MMAP_MODE) {
        if (register_aperture(obmm_fd, &cfg, local_idx) != 0) {
            log_msg("aperture register failed errno=%d", errno);
            goto out;
        }
        log_msg("kernel aperture registry -> ok base=%#" PRIx64
                " size=%#" PRIx64, cfg.base, cfg.size);
        ret = run_mmap_mode(obmm_fd, &cfg, local_idx) == 0 ? 0 : 1;
        goto out;
    }
    if (cfg.mode == GSVA_DEMO_OUTSIDE_APERTURE) {
        ret = run_outside_aperture(obmm_fd, &cfg, local_idx) == 0 ? 0 : 1;
        goto out;
    }
    if (cfg.mode == GSVA_DEMO_STALE_GENERATION) {
        ret = run_stale_generation(obmm_fd, &cfg, local_idx) == 0 ? 0 : 1;
        goto out;
    }
    if (cfg.mode == GSVA_DEMO_INVALID_OFFSET) {
        if (register_aperture(obmm_fd, &cfg, local_idx) != 0) {
            log_msg("aperture register failed errno=%d", errno);
            goto out;
        }
        log_msg("kernel aperture registry -> ok base=%#" PRIx64
                " size=%#" PRIx64, cfg.base, cfg.size);
        ret = local_idx == 0 ?
            run_invalid_offset_home(obmm_fd, local_cna, &cfg, &local_meta) :
            run_invalid_offset_peer(obmm_fd, local_cna, &cfg, &local_meta);
        ret = ret == 0 ? 0 : 1;
        goto out;
    }
    if (cfg.mode == GSVA_DEMO_OUTSIDE_IMPORT) {
        if (register_aperture(obmm_fd, &cfg, local_idx) != 0) {
            log_msg("aperture register failed errno=%d", errno);
            goto out;
        }
        log_msg("kernel aperture registry -> ok base=%#" PRIx64
                " size=%#" PRIx64, cfg.base, cfg.size);
        ret = local_idx == 0 ?
            run_outside_import_home(obmm_fd, local_cna, &cfg, &local_meta) :
            run_outside_import_peer(obmm_fd, local_cna, &cfg, &local_meta);
        ret = ret == 0 ? 0 : 1;
        goto out;
    }

    if (register_aperture(obmm_fd, &cfg, local_idx) != 0) {
        log_msg("aperture register failed errno=%d", errno);
        goto out;
    }
    log_msg("kernel aperture registry -> ok base=%#" PRIx64 " size=%#"
            PRIx64, cfg.base,
            cfg.mode == GSVA_DEMO_MATRIX ?
            cfg.size * (uint64_t)cfg.node_count : cfg.size);

    if (cfg.mode == GSVA_DEMO_MATRIX) {
        ret = run_matrix(obmm_fd, local_cna, &cfg, local_idx,
                         &local_meta) == 0 ? 0 : 1;
        goto out;
    }

    if (local_idx == 0)
        ret = run_identity_home(obmm_fd, local_cna, &cfg, &local_meta) == 0 ? 0 : 1;
    else
        ret = run_identity_peer(obmm_fd, local_cna, &cfg, &local_meta) == 0 ? 0 : 1;

out:
    close(obmm_fd);
    return ret;
}
