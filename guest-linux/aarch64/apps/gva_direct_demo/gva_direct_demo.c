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

#define TAG "[gva_direct_demo]"
#define GVA_DIRECT_DEFAULT_SIZE (4UL * 1024UL * 1024UL)
#define GVA_DIRECT_LOCAL_VA 0x710000000000ULL
#define GVA_DIRECT_HOME_VA  0x720000000000ULL
#define GVA_DIRECT_GENERATION 0x475641440101ULL
#define GVA_DIRECT_MAGIC 0x47564144454d4fULL
#define GVA_DIRECT_A 0x13579bdf2468ace0ULL
#define GVA_DIRECT_B 0xfdb97531eca86420ULL
#define GVA_DIRECT_TIMEOUT_MS 90000
#define GVA_DIRECT_ACCESS_FAULT_UPI_MISMATCH (1U << 31)

enum gva_direct_mode {
    GVA_DIRECT_WRITE_READ,
    GVA_DIRECT_SYNC,
    GVA_DIRECT_UNMAP_FAULT,
    GVA_DIRECT_DUMP,
    GVA_DIRECT_INVALID_CACHE,
    GVA_DIRECT_OVERLAP,
    GVA_DIRECT_INVALID_PTAG,
    GVA_DIRECT_TOKEN_MISMATCH,
    GVA_DIRECT_INVALID_UPI,
};

struct gva_direct_config {
    enum gva_direct_mode mode;
    uint64_t size;
    uint64_t local_va;
    uint64_t home_va;
};

struct gva_direct_payload {
    volatile uint64_t magic;
    volatile uint64_t phase;
    volatile uint64_t value;
    volatile uint64_t home_ptr;
    volatile uint64_t peer_ptr;
    volatile uint64_t sync_done;
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

static bool parse_args(int argc, char **argv, struct gva_direct_config *cfg)
{
    int i;

    cfg->mode = GVA_DIRECT_WRITE_READ;
    cfg->size = GVA_DIRECT_DEFAULT_SIZE;
    cfg->local_va = GVA_DIRECT_LOCAL_VA;
    cfg->home_va = GVA_DIRECT_HOME_VA;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "write-read") == 0) {
                cfg->mode = GVA_DIRECT_WRITE_READ;
            } else if (strcmp(mode, "sync") == 0) {
                cfg->mode = GVA_DIRECT_SYNC;
            } else if (strcmp(mode, "unmap-fault") == 0) {
                cfg->mode = GVA_DIRECT_UNMAP_FAULT;
            } else if (strcmp(mode, "dump") == 0) {
                cfg->mode = GVA_DIRECT_DUMP;
            } else if (strcmp(mode, "invalid-cache") == 0) {
                cfg->mode = GVA_DIRECT_INVALID_CACHE;
            } else if (strcmp(mode, "overlap") == 0) {
                cfg->mode = GVA_DIRECT_OVERLAP;
            } else if (strcmp(mode, "invalid-ptag") == 0) {
                cfg->mode = GVA_DIRECT_INVALID_PTAG;
            } else if (strcmp(mode, "token-mismatch") == 0) {
                cfg->mode = GVA_DIRECT_TOKEN_MISMATCH;
            } else if (strcmp(mode, "invalid-upi") == 0) {
                cfg->mode = GVA_DIRECT_INVALID_UPI;
            } else {
                return false;
            }
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &cfg->size))
                return false;
        } else if (strcmp(argv[i], "--local-va") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &cfg->local_va))
                return false;
        } else if (strcmp(argv[i], "--home-va") == 0 && i + 1 < argc) {
            if (!parse_u64(argv[++i], &cfg->home_va))
                return false;
        } else {
            return false;
        }
    }

    return cfg->size >= sizeof(struct gva_direct_payload) &&
           (cfg->size & 4095ULL) == 0 &&
           (cfg->local_va & 4095ULL) == 0 &&
           (cfg->home_va & 4095ULL) == 0 &&
           cfg->local_va != 0 && cfg->home_va != 0 &&
           cfg->local_va != cfg->home_va;
}

static const char *mode_name(enum gva_direct_mode mode)
{
    switch (mode) {
    case GVA_DIRECT_WRITE_READ:
        return "write-read";
    case GVA_DIRECT_SYNC:
        return "sync";
    case GVA_DIRECT_UNMAP_FAULT:
        return "unmap-fault";
    case GVA_DIRECT_DUMP:
        return "dump";
    case GVA_DIRECT_INVALID_CACHE:
        return "invalid-cache";
    case GVA_DIRECT_OVERLAP:
        return "overlap";
    case GVA_DIRECT_INVALID_PTAG:
        return "invalid-ptag";
    case GVA_DIRECT_TOKEN_MISMATCH:
        return "token-mismatch";
    case GVA_DIRECT_INVALID_UPI:
        return "invalid-upi";
    }
    return "unknown";
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

static int wait_phase(volatile uint64_t *phase, uint64_t expect)
{
    long deadline = obmm_now_ms() + GVA_DIRECT_TIMEOUT_MS;

    while (obmm_now_ms() < deadline) {
        if (*phase == expect)
            return 0;
        usleep(1000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int sync_import_range(int fd, uint64_t offset, uint64_t length)
{
    struct obmm_cmd_sync_import_range cmd = {0};

    cmd.offset = offset;
    cmd.length = length;
    return ioctl(fd, OBMM_SHMDEV_SYNC_IMPORT_RANGE, &cmd);
}

static int dump_proc_gva_routes(uint64_t local_va, uint64_t home_va,
                                uint64_t pte_offset)
{
    FILE *fp;
    char line[512];
    char local_hex[32];
    char home_hex[32];
    char offset_hex[32];
    bool matched = false;

    snprintf(local_hex, sizeof(local_hex), "%" PRIx64, local_va);
    snprintf(home_hex, sizeof(home_hex), "%" PRIx64, home_va);
    snprintf(offset_hex, sizeof(offset_hex), "%" PRIx64, pte_offset);

    fp = fopen("/proc/ub_sim_decoder/gva_routes", "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);

        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        log_msg("guest_proc_route_dump %s", line);
        if (strstr(line, local_hex) && strstr(line, home_hex) &&
            strstr(line, offset_hex))
            matched = true;
    }

    fclose(fp);
    if (!matched) {
        errno = ENOENT;
        return -1;
    }
    return 0;
}

static int run_home(int obmm_fd, uint32_t local_cna,
                    const struct gva_direct_config *cfg,
                    struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_region region = {0};
    struct gva_direct_payload *payload;
    int ret = -1;

    local_meta->export_cna = local_cna;
    if (obmm_do_export(obmm_fd, local_meta, cfg->size) != 0)
        return -1;
    if (obmm_bootstrap_publish(obmm_fd, 0, 2, GVA_DIRECT_GENERATION,
                               local_meta) != 0)
        goto out_unexport;

    if (cfg->mode == GVA_DIRECT_INVALID_CACHE ||
        cfg->mode == GVA_DIRECT_OVERLAP ||
        cfg->mode == GVA_DIRECT_INVALID_PTAG ||
        cfg->mode == GVA_DIRECT_TOKEN_MISMATCH ||
        cfg->mode == GVA_DIRECT_INVALID_UPI) {
        usleep(3000000);
        log_msg("result=done mode=%s role=home uba=%#" PRIx64,
                mode_name(cfg->mode), local_meta->remote_uba);
        ret = 0;
        goto out_unexport;
    }

    if (obmm_map_region_at(local_meta->export_mem_id,
                           (void *)(uintptr_t)cfg->home_va,
                           cfg->size, false, &region) != 0)
        goto out_unexport;
    if ((uint64_t)(uintptr_t)region.addr != cfg->home_va) {
        errno = EINVAL;
        goto out_unmap;
    }

    payload = (struct gva_direct_payload *)region.addr;
    memset(payload, 0, sizeof(*payload));
    payload->magic = GVA_DIRECT_MAGIC;
    payload->value = GVA_DIRECT_A;
    payload->home_ptr = cfg->home_va;
    __sync_synchronize();
    payload->phase = 1;
    log_msg("home wrote value=%#" PRIx64 " home_va=%#" PRIx64
            " uba=%#" PRIx64, (uint64_t)GVA_DIRECT_A, cfg->home_va,
            local_meta->remote_uba);

    if (wait_phase(&payload->phase, 2) != 0)
        goto out_unmap;
    if (payload->value != GVA_DIRECT_B || payload->peer_ptr != cfg->local_va) {
        log_msg("home verify failed value=%#" PRIx64 " peer_ptr=%#" PRIx64,
                (uint64_t)payload->value, (uint64_t)payload->peer_ptr);
        errno = EIO;
        goto out_unmap;
    }

    log_msg("result=done mode=%s role=home local_va=%#" PRIx64
            " home_va=%#" PRIx64 " uba=%#" PRIx64 " pte_offset=remote-local"
            " value=%#" PRIx64 " sync_done=%" PRIu64,
            mode_name(cfg->mode), cfg->local_va, cfg->home_va,
            local_meta->remote_uba, (uint64_t)payload->value,
            (uint64_t)payload->sync_done);
    ret = 0;

out_unmap:
    obmm_unmap_region(&region);
out_unexport:
    if (local_meta->export_mem_id)
        (void)obmm_do_unexport(obmm_fd, local_meta->export_mem_id);
    return ret;
}

static int publish_peer_dummy(int obmm_fd, uint32_t local_cna,
                              const struct gva_direct_config *cfg,
                              struct obmm_helpers_meta *local_meta)
{
    local_meta->export_cna = local_cna;
    if (obmm_do_export(obmm_fd, local_meta, cfg->size) != 0)
        return -1;
    return obmm_bootstrap_publish(obmm_fd, 1, 2, GVA_DIRECT_GENERATION,
                                  local_meta);
}

static int run_peer_unmap_fault(int obmm_fd, uint64_t import_mem_id,
                                struct obmm_helpers_region *region)
{
    struct obmm_helpers_region should_fail = {0};

    obmm_unmap_region(region);
    if (obmm_do_unimport(obmm_fd, import_mem_id) != 0)
        return -1;
    if (obmm_map_region(import_mem_id, 4096, false, &should_fail) == 0) {
        obmm_unmap_region(&should_fail);
        errno = EINVAL;
        return -1;
    }
    log_msg("unmap fault -> ok mem_id=%#" PRIx64 " errno=%d",
            import_mem_id, errno);
    return 0;
}

static int run_peer(int obmm_fd, uint32_t local_cna,
                    const struct gva_direct_config *cfg,
                    struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    uint64_t second_import_mem_id = 0;
    struct obmm_helpers_region region = {0};
    struct gva_direct_payload *payload;
    uint64_t pte_offset;
    uint32_t cache_policy = OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH;
    uint32_t token_value = 0;
    uint32_t p_tag = 0;
    uint32_t access_flags = 0;
    int ret = -1;

    if (publish_peer_dummy(obmm_fd, local_cna, cfg, local_meta) != 0)
        return -1;
    if (obmm_bootstrap_lookup(obmm_fd, local_cna, 2, GVA_DIRECT_GENERATION,
                              metas, got) != 0)
        goto out_unexport;
    if (!got[0] || metas[0].size < sizeof(*payload)) {
        errno = EINVAL;
        goto out_unexport;
    }
    if (!obmm_alloc_import_pas(1, cfg->size, import_pas, import_osync,
                               obmm_parse_import_cache_mode()))
        goto out_unexport;

    pte_offset = metas[0].remote_uba - cfg->local_va;
    if (cfg->mode == GVA_DIRECT_INVALID_CACHE)
        cache_policy = 0xffffffffU;
    if (cfg->mode == GVA_DIRECT_INVALID_PTAG)
        p_tag = 0xffffffffU;
    if (cfg->mode == GVA_DIRECT_TOKEN_MISMATCH)
        token_value = 0xffffffffU;
    if (cfg->mode == GVA_DIRECT_INVALID_UPI)
        access_flags = GVA_DIRECT_ACCESS_FAULT_UPI_MISMATCH;
    if (obmm_do_import_v2(obmm_fd, &metas[0], local_cna, import_pas[0],
                          token_value,
                          OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                          OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
                          cache_policy,
                          0, 0, 0, p_tag, access_flags, 1, cfg->local_va,
                          cfg->home_va, pte_offset, &import_mem_id) != 0) {
        if (cfg->mode == GVA_DIRECT_INVALID_CACHE) {
            log_msg("result=done mode=invalid-cache role=peer bad_cache_policy=%#x errno=%d",
                    cache_policy, errno);
            ret = 0;
        }
        goto out_unexport;
    }
    if (cfg->mode == GVA_DIRECT_INVALID_CACHE) {
        (void)obmm_do_unimport(obmm_fd, import_mem_id);
        import_mem_id = 0;
        log_msg("result=fail mode=invalid-cache role=peer reason=import-accepted");
        errno = EINVAL;
        goto out_unexport;
    }
    log_msg("guest_route_dump map_source=%u address_profile=%u cache_policy=%u "
            "gva_id=%u local_va=%#" PRIx64 " home_va=%#" PRIx64
            " pte_offset=%#" PRIx64 " uba=%#" PRIx64 " import_pa=%#"
            PRIx64,
            OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
            OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
            OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH, 1, cfg->local_va,
            cfg->home_va, pte_offset, metas[0].remote_uba, import_pas[0]);

    if (cfg->mode == GVA_DIRECT_DUMP &&
        dump_proc_gva_routes(cfg->local_va, cfg->home_va, pte_offset) != 0) {
        log_msg("guest_proc_route_dump failed errno=%d", errno);
        goto out_unimport;
    }

    if (cfg->mode == GVA_DIRECT_OVERLAP) {
        if (obmm_do_import_v2(obmm_fd, &metas[0], local_cna, import_pas[0], 0,
                              OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                              OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
                              OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH,
                              0, 0, 0, 0, 0, 2, cfg->local_va,
                              cfg->home_va, pte_offset,
                              &second_import_mem_id) == 0) {
            (void)obmm_do_unimport(obmm_fd, second_import_mem_id);
            log_msg("result=fail mode=overlap role=peer reason=second-import-accepted");
            errno = EBUSY;
            goto out_unimport;
        }
        log_msg("result=done mode=overlap role=peer import_pa=%#" PRIx64
                " errno=%d", import_pas[0], errno);
        ret = 0;
        goto out_unimport;
    }

    if (obmm_map_region_at(import_mem_id, (void *)(uintptr_t)cfg->local_va,
                           cfg->size, import_osync[0], &region) != 0)
        goto out_unimport;
    if ((uint64_t)(uintptr_t)region.addr != cfg->local_va) {
        errno = EINVAL;
        goto out_unmap;
    }

    payload = (struct gva_direct_payload *)region.addr;
    if (cfg->mode == GVA_DIRECT_INVALID_PTAG ||
        cfg->mode == GVA_DIRECT_TOKEN_MISMATCH ||
        cfg->mode == GVA_DIRECT_INVALID_UPI) {
        uint64_t observed = payload->phase;

        payload->value = GVA_DIRECT_B;
        __sync_synchronize();
        log_msg("result=done mode=%s role=peer fault_injected=1 observed_phase=%#"
                PRIx64 " p_tag=%#x token_value=%#x access_flags=%#x",
                mode_name(cfg->mode), observed, p_tag, token_value,
                access_flags);
        ret = 0;
        goto out_unmap;
    }

    if (wait_phase(&payload->phase, 1) != 0)
        goto out_unmap;
    if (payload->magic != GVA_DIRECT_MAGIC || payload->value != GVA_DIRECT_A ||
        payload->home_ptr != cfg->home_va) {
        log_msg("peer verify A failed magic=%#" PRIx64 " value=%#" PRIx64
                " home_ptr=%#" PRIx64,
                (uint64_t)payload->magic, (uint64_t)payload->value,
                (uint64_t)payload->home_ptr);
        errno = EIO;
        goto out_unmap;
    }

    payload->value = GVA_DIRECT_B;
    payload->peer_ptr = cfg->local_va;
    if (cfg->mode == GVA_DIRECT_SYNC) {
        if (sync_import_range(region.fd, 0, sizeof(*payload)) != 0) {
            log_msg("sync failed errno=%d", errno);
            goto out_unmap;
        }
        payload->sync_done = 1;
        log_msg("sync -> ok mem_id=%#" PRIx64 " len=%zu",
                import_mem_id, sizeof(*payload));
    }
    __sync_synchronize();
    payload->phase = 2;

    if (cfg->mode == GVA_DIRECT_UNMAP_FAULT) {
        if (run_peer_unmap_fault(obmm_fd, import_mem_id, &region) != 0)
            goto out_unexport;
        import_mem_id = 0;
    }

    log_msg("result=done mode=%s role=peer local_va=%#" PRIx64
            " home_va=%#" PRIx64 " uba=%#" PRIx64 " pte_offset=%#"
            PRIx64 " value=%#" PRIx64,
            mode_name(cfg->mode), cfg->local_va, cfg->home_va,
            metas[0].remote_uba, pte_offset, (uint64_t)GVA_DIRECT_B);
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

int main(int argc, char **argv)
{
    struct gva_direct_config cfg;
    struct obmm_helpers_meta local_meta = {0};
    uint32_t local_cna = 0;
    int local_idx = -1;
    int obmm_fd = -1;
    int ret = 1;

    if (!parse_args(argc, argv, &cfg)) {
        fprintf(stderr, "usage: gva_direct_demo --mode write-read|sync|unmap-fault|dump|invalid-cache|overlap|invalid-ptag|token-mismatch|invalid-upi "
                "[--size S] [--local-va A] [--home-va A]\n");
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

    log_msg("start mode=%s node=%d cna=%#x local_va=%#" PRIx64
            " home_va=%#" PRIx64 " size=%#" PRIx64,
            mode_name(cfg.mode), local_idx, local_cna, cfg.local_va,
            cfg.home_va, cfg.size);

    if (local_idx == 0)
        ret = run_home(obmm_fd, local_cna, &cfg, &local_meta) == 0 ? 0 : 1;
    else
        ret = run_peer(obmm_fd, local_cna, &cfg, &local_meta) == 0 ? 0 : 1;

out:
    close(obmm_fd);
    return ret;
}
