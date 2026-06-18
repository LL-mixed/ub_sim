/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM imported-PA stress tool for SIM_DEC data-path performance validation.
 *
 * Usage: obmm_import_stress [options]
 *   --size <bytes>        Imported region size (default 4 MiB)
 *   --pattern <seq|random|repeat|mixed>
 *   --iterations <n>      Number of iterations (default 1000)
 *   --flush-mode <none|periodic|every>
 *   --period <n>          Flush every N iterations in periodic mode
 *   --verify              Verify data after writes
 *   --read-only           Only read stress
 *   --write-only          Only write stress
 *   --chunk-size <bytes>  Access chunk size (default 8)
 *   --seed <n>            RNG seed (default 42)
 *   --node-count <n>      Participating nodes (default 2)
 *   --peer-index <n>      Peer node to import, 0-based (default next node)
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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "obmm_common.h"

#define STRESS_DEFAULT_SIZE        (4UL * 1024UL * 1024UL)
#define STRESS_DEFAULT_ITERATIONS  1000
#define STRESS_DEFAULT_CHUNK_SIZE  8
#define STRESS_DEFAULT_SEED        42
#define STRESS_DEFAULT_GSVA_BASE   0x700000000000ULL
#define STRESS_DEFAULT_GSVA_GENERATION 0x535456410101ULL

enum stress_pattern {
    PATTERN_SEQ,
    PATTERN_RANDOM,
    PATTERN_REPEAT,
    PATTERN_MIXED,
};

enum flush_mode {
    FLUSH_NONE,
    FLUSH_PERIODIC,
    FLUSH_EVERY,
};

enum stress_gva_mode {
    STRESS_GVA_MODE_LEGACY = 0,
    STRESS_GVA_MODE_GENERIC = 1,
    STRESS_GVA_MODE_GSVA = 2,
};

struct stress_config {
    uint64_t size;
    enum stress_pattern pattern;
    uint64_t iterations;
    enum flush_mode flush;
    uint64_t period;
    bool verify;
    bool read_only;
    bool write_only;
    uint64_t chunk_size;
    uint32_t seed;
    enum stress_gva_mode gva_mode;
    uint32_t map_source;
    uint32_t address_profile;
    uint32_t cache_policy;
    uint32_t vmid;
    uint32_t asid;
    uint32_t tid;
    uint32_t p_tag;
    uint32_t token_value;
    uint32_t access_flags;
    uint64_t gva_id;
    uint64_t local_va;
    uint64_t home_va;
    uint64_t pte_offset;
    uint64_t gsva_base;
    uint64_t gsva_generation;
    int node_count;
    int peer_index;
    bool gva_home_va_set;
    bool gva_local_va_set;
    bool gva_pte_offset_set;
};

struct stress_stats {
    uint64_t reads;
    uint64_t writes;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t flushes;
    uint64_t verify_failures;
    long     duration_ms;
};

static void stress_log(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[obmm_import_stress] ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static long stress_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static uint32_t stress_rng(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static uint64_t stress_rand_offset(struct stress_config *cfg, uint32_t *rng_state)
{
    uint64_t max = cfg->size > cfg->chunk_size ? cfg->size - cfg->chunk_size : 0;
    if (max == 0)
        return 0;
    uint64_t r = ((uint64_t)stress_rng(rng_state) << 32) | stress_rng(rng_state);
    return r % max;
}

static void stress_fill_pattern(uint8_t *buf, uint64_t len, uint64_t addr_seed)
{
    uint64_t i;
    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)((addr_seed + i) * 0x9e3779b9 + 0x85ebca77);
    }
}

static bool stress_verify_pattern(const uint8_t *buf, uint64_t len, uint64_t addr_seed)
{
    uint64_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] != (uint8_t)((addr_seed + i) * 0x9e3779b9 + 0x85ebca77))
            return false;
    }
    return true;
}

static bool stress_do_flush(int shmdev_fd, uint64_t offset, uint64_t len)
{
    obmm_cmd_sync_remote_range cmd = { .offset = offset, .length = len };
    if (ioctl(shmdev_fd, OBMM_SHMDEV_SYNC_REMOTE_RANGE, &cmd) != 0) {
        stress_log("flush failed: offset=%" PRIu64 " len=%" PRIu64 " errno=%d",
                   offset, len, errno);
        return false;
    }
    return true;
}

static bool stress_run(struct stress_config *cfg,
                       uint8_t *imported_va,
                       int shmdev_fd,
                       struct stress_stats *out_stats)
{
    uint64_t iter;
    uint32_t rng_state = cfg->seed;
    uint8_t *tmp_write = malloc(cfg->chunk_size);
    uint8_t *tmp_read = malloc(cfg->chunk_size);
    bool ok = true;

    if (!tmp_write || !tmp_read) {
        stress_log("malloc failed for tmp buffers");
        free(tmp_write);
        free(tmp_read);
        return false;
    }

    memset(out_stats, 0, sizeof(*out_stats));
    long t0 = stress_now_ms();

    for (iter = 0; iter < cfg->iterations && ok; iter++) {
        uint64_t offset;
        switch (cfg->pattern) {
        case PATTERN_SEQ:
            offset = (iter * cfg->chunk_size) % cfg->size;
            break;
        case PATTERN_RANDOM:
            offset = stress_rand_offset(cfg, &rng_state);
            break;
        case PATTERN_REPEAT:
            offset = 0;
            break;
        case PATTERN_MIXED:
            offset = (iter % 3 == 0) ? stress_rand_offset(cfg, &rng_state)
                                     : (iter * cfg->chunk_size) % cfg->size;
            break;
        default:
            offset = 0;
            break;
        }

        if (offset + cfg->chunk_size > cfg->size)
            offset = cfg->size - cfg->chunk_size;

        if (!cfg->read_only) {
            stress_fill_pattern(tmp_write, cfg->chunk_size, offset ^ iter);
            memcpy(imported_va + offset, tmp_write, cfg->chunk_size);
            out_stats->writes++;
            out_stats->write_bytes += cfg->chunk_size;

            if (cfg->verify) {
                memcpy(tmp_read, imported_va + offset, cfg->chunk_size);
                if (!stress_verify_pattern(tmp_read, cfg->chunk_size, offset ^ iter)) {
                    stress_log("verify failure after write at offset=%" PRIu64
                               " iter=%" PRIu64, offset, iter);
                    out_stats->verify_failures++;
                    ok = false;
                    break;
                }
            }
        }

        if (!cfg->write_only) {
            memcpy(tmp_read, imported_va + offset, cfg->chunk_size);
            out_stats->reads++;
            out_stats->read_bytes += cfg->chunk_size;
        }

        if (cfg->flush == FLUSH_EVERY && !cfg->read_only) {
            if (!stress_do_flush(shmdev_fd, offset, cfg->chunk_size)) {
                ok = false;
                break;
            }
            out_stats->flushes++;
        } else if (cfg->flush == FLUSH_PERIODIC && !cfg->read_only) {
            if (cfg->period > 0 && (iter + 1) % cfg->period == 0) {
                if (!stress_do_flush(shmdev_fd, 0, cfg->size)) {
                    ok = false;
                    break;
                }
                out_stats->flushes++;
            }
        }
    }

    if (ok && cfg->flush == FLUSH_NONE && !cfg->read_only &&
        cfg->gva_mode != STRESS_GVA_MODE_GSVA) {
        if (!stress_do_flush(shmdev_fd, 0, cfg->size)) {
            ok = false;
        } else {
            out_stats->flushes++;
        }
    }

    out_stats->duration_ms = stress_now_ms() - t0;
    free(tmp_write);
    free(tmp_read);
    return ok;
}

static void stress_print_stats(const struct stress_config *cfg,
                               const struct stress_stats *stats)
{
    double rmb = stats->read_bytes / (1024.0 * 1024.0);
    double wmb = stats->write_bytes / (1024.0 * 1024.0);
    double dur_s = stats->duration_ms > 0 ? stats->duration_ms / 1000.0 : 0.001;
    double rbw = rmb / dur_s;
    double wbw = wmb / dur_s;

    stress_log("result=done size=%" PRIu64 " iterations=%" PRIu64
               " pattern=%d flush=%d reads=%" PRIu64 " writes=%" PRIu64
               " read_bytes=%" PRIu64 " write_bytes=%" PRIu64
               " flushes=%" PRIu64 " verify_failures=%" PRIu64
               " duration_ms=%ld read_mbps=%.2f write_mbps=%.2f",
               cfg->size, cfg->iterations, cfg->pattern, cfg->flush,
               stats->reads, stats->writes, stats->read_bytes, stats->write_bytes,
               stats->flushes, stats->verify_failures, stats->duration_ms,
               rbw, wbw);

    stress_log("gva_mode=%d map_source=%u address_profile=%u cache_policy=%u vmid=%u asid=%u tid=%u p_tag=%u access_flags=%u gva_id=%" PRIu64
               " local_va=%" PRIx64 " home_va=%" PRIx64 " pte_offset=%" PRIx64 " token_value=%u",
               cfg->gva_mode, cfg->map_source, cfg->address_profile,
               cfg->cache_policy, cfg->vmid, cfg->asid, cfg->tid, cfg->p_tag,
               cfg->access_flags, cfg->gva_id, cfg->local_va, cfg->home_va,
               cfg->pte_offset, cfg->token_value);
}

static bool stress_completion_barrier(int obmm_fd,
                                      int local_idx,
                                      int node_count,
                                      uint32_t local_cna,
                                      uint64_t generation,
                                      const struct obmm_helpers_meta *local_meta)
{
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = { false };

    if (obmm_bootstrap_publish(obmm_fd, local_idx, node_count,
                               generation, local_meta) != 0) {
        stress_log("completion publish failed generation=%" PRIu64 " errno=%d",
                   generation, errno);
        return false;
    }
    stress_log("completion publish ok generation=%" PRIu64, generation);

    if (obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                              generation, metas, got) != 0) {
        stress_log("completion barrier failed generation=%" PRIu64 " errno=%d",
                   generation, errno);
        return false;
    }
    stress_log("completion barrier ok generation=%" PRIu64, generation);
    return true;
}

static bool parse_stress_gva_mode(const char *s, enum stress_gva_mode *mode)
{
    if (strcmp(s, "legacy") == 0) {
        *mode = STRESS_GVA_MODE_LEGACY;
    } else if (strcmp(s, "generic") == 0 || strcmp(s, "gva") == 0) {
        *mode = STRESS_GVA_MODE_GENERIC;
    } else if (strcmp(s, "gsva") == 0) {
        *mode = STRESS_GVA_MODE_GSVA;
    } else {
        return false;
    }
    return true;
}

static bool parse_stress_cache_policy(const char *s, uint32_t *policy)
{
    if (strcmp(s, "nc") == 0) {
        *policy = OBMM_SIM_DEC_CACHE_POLICY_NC;
    } else if (strcmp(s, "wt") == 0 || strcmp(s, "write-through") == 0) {
        *policy = OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH;
    } else if (strcmp(s, "rc") == 0 || strcmp(s, "read-cache") == 0) {
        *policy = OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE;
    } else if (strcmp(s, "wb") == 0 || strcmp(s, "write-back") == 0) {
        *policy = OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK;
    } else if (strcmp(s, "mesi") == 0 || strcmp(s, "directory-mesi") == 0) {
        *policy = OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI;
    } else {
        return false;
    }
    return true;
}

static bool parse_stress_map_source(const char *s, uint32_t *map_source)
{
    if (strcmp(s, "legacy") == 0 || strcmp(s, "legacy-obmm") == 0) {
        *map_source = OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM;
    } else if (strcmp(s, "gva") == 0 || strcmp(s, "gva-manager") == 0) {
        *map_source = OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER;
    } else {
        return false;
    }
    return true;
}

static bool parse_stress_address_profile(const char *s, uint32_t *profile)
{
    if (strcmp(s, "generic") == 0) {
        *profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA;
    } else if (strcmp(s, "gsva") == 0) {
        *profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY;
    } else {
        return false;
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

static bool parse_u32_arg(const char *s, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    *out = (uint32_t)strtoul(s, &end, 0);
    return errno == 0 && end != NULL && *end == '\0';
}

static void stress_init_gva_defaults(struct stress_config *cfg)
{
    cfg->gva_mode = STRESS_GVA_MODE_LEGACY;
    cfg->map_source = OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM;
    cfg->address_profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA;
    cfg->cache_policy = OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH;
    cfg->vmid = 0;
    cfg->asid = 0;
    cfg->tid = 0;
    cfg->p_tag = 0;
    cfg->token_value = 0;
    cfg->access_flags = 0;
    cfg->gva_id = 0;
    cfg->local_va = 0;
    cfg->home_va = 0;
    cfg->pte_offset = 0;
    cfg->gsva_base = STRESS_DEFAULT_GSVA_BASE;
    cfg->gsva_generation = STRESS_DEFAULT_GSVA_GENERATION;
    cfg->gva_local_va_set = false;
    cfg->gva_home_va_set = false;
    cfg->gva_pte_offset_set = false;
}

static bool stress_register_gsva_aperture(int obmm_fd,
                                          const struct stress_config *cfg,
                                          int local_idx, int node_count)
{
    struct obmm_cmd_gsva_aperture req = { 0 };
    struct obmm_cmd_gsva_aperture query = { 0 };
    uint64_t aperture_size = cfg->size * (uint64_t)node_count;

    req.base = cfg->gsva_base;
    req.size = aperture_size;
    req.generation = cfg->gsva_generation;
    req.flags = OBMM_GSVA_APERTURE_F_ACTIVE;
    req.node_id = (uint32_t)local_idx;
    req.node_count = (uint32_t)node_count;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_REGISTER, &req) != 0) {
        stress_log("GSVA aperture register failed base=%" PRIx64
                   " size=%" PRIx64 " generation=%" PRIx64 " errno=%d",
                   req.base, req.size, req.generation, errno);
        return false;
    }
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_QUERY, &query) != 0) {
        stress_log("GSVA aperture query failed errno=%d", errno);
        return false;
    }
    if (query.base != req.base || query.size != req.size ||
        query.generation != req.generation ||
        !(query.flags & OBMM_GSVA_APERTURE_F_ACTIVE)) {
        stress_log("GSVA aperture query mismatch base=%" PRIx64
                   " size=%" PRIx64 " generation=%" PRIx64 " flags=%" PRIx64,
                   query.base, query.size, query.generation, query.flags);
        return false;
    }
    stress_log("GSVA aperture registered base=%" PRIx64 " size=%" PRIx64
               " generation=%" PRIx64 " node=%d/%d",
               req.base, req.size, req.generation, local_idx, node_count);
    return true;
}

static void stress_clear_gsva_aperture(int obmm_fd, uint64_t generation)
{
    struct obmm_cmd_gsva_aperture req = { 0 };

    if (obmm_fd < 0) {
        return;
    }
    req.generation = generation;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_CLEAR, &req) != 0) {
        stress_log("GSVA aperture clear failed generation=%" PRIx64
                   " errno=%d", generation, errno);
    }
}

static int stress_got_count(const bool got[OBMM_POOL_HELPERS_MAX_NODES],
                            int node_count)
{
    int count = 0;
    int i;

    for (i = 0; i < node_count; i++) {
        if (got[i])
            count++;
    }
    return count;
}

static int stress_default_peer_index(int local_idx, int node_count)
{
    return (local_idx + 1) % node_count;
}

static bool stress_parse_local_ipv4_index(int node_count, int *local_idx)
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

static bool stress_finalize_gva_config(struct stress_config *cfg,
                                      uint64_t remote_uba)
{
    if (cfg->gva_mode == STRESS_GVA_MODE_LEGACY) {
        return true;
    }

    if (cfg->gva_local_va_set && cfg->gva_home_va_set &&
        cfg->local_va != cfg->home_va) {
        fprintf(stderr, "local_va/home_va mismatch\n");
        return false;
    }

    if (cfg->gva_mode == STRESS_GVA_MODE_GSVA) {
        cfg->map_source = OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER;
        cfg->address_profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY;
        if (cfg->gva_local_va_set && cfg->local_va != remote_uba) {
            fprintf(stderr, "GSVA requires local_va == remote_uba\n");
            return false;
        }
        if (cfg->gva_home_va_set && cfg->home_va != remote_uba) {
            fprintf(stderr, "GSVA requires home_va == remote_uba\n");
            return false;
        }
        if (cfg->gva_pte_offset_set && cfg->pte_offset != 0) {
            fprintf(stderr, "GSVA requires pte_offset=0\n");
            return false;
        }
        cfg->local_va = remote_uba;
        cfg->home_va = remote_uba;
        cfg->pte_offset = 0;
        cfg->gva_local_va_set = true;
        cfg->gva_home_va_set = true;
        cfg->gva_pte_offset_set = true;
        if (cfg->local_va != remote_uba || cfg->home_va != remote_uba) {
            fprintf(stderr, "GSVA requires remote_uba/local_va/home_va all equal\n");
            return false;
        }
        if (cfg->pte_offset != 0) {
            fprintf(stderr, "GSVA requires pte_offset=0\n");
            return false;
        }
        return true;
    }

    /* generic GVA */
    cfg->map_source = OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM;
    cfg->address_profile = OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA;

    if (!cfg->gva_local_va_set && !cfg->gva_home_va_set && !cfg->gva_pte_offset_set) {
        cfg->local_va = remote_uba;
        cfg->home_va = remote_uba;
        cfg->gva_local_va_set = true;
        cfg->gva_home_va_set = true;
    }

    if (cfg->gva_local_va_set && !cfg->gva_home_va_set) {
        cfg->home_va = cfg->local_va;
        cfg->gva_home_va_set = true;
    }
    if (!cfg->gva_local_va_set && cfg->gva_home_va_set) {
        cfg->local_va = cfg->home_va;
        cfg->gva_local_va_set = true;
    }

    if (!cfg->gva_pte_offset_set && cfg->gva_local_va_set) {
        if (remote_uba < cfg->local_va) {
            fprintf(stderr, "cannot derive pte_offset: remote_uba < local_va\n");
            return false;
        }
        cfg->pte_offset = remote_uba - cfg->local_va;
        cfg->gva_pte_offset_set = true;
    } else if (cfg->gva_pte_offset_set && cfg->gva_local_va_set) {
        if (remote_uba < cfg->pte_offset) {
            fprintf(stderr, "invalid gva config: pte_offset exceeds remote_uba\n");
            return false;
        }
        if (remote_uba != cfg->local_va + cfg->pte_offset) {
            fprintf(stderr, "invalid gva config: local_va + pte_offset != remote_uba\n");
            return false;
        }
    } else if (cfg->gva_pte_offset_set) {
        if (remote_uba < cfg->pte_offset) {
            fprintf(stderr, "invalid gva config: pte_offset exceeds remote_uba\n");
            return false;
        }
        cfg->local_va = remote_uba - cfg->pte_offset;
        cfg->home_va = cfg->local_va;
        cfg->gva_local_va_set = true;
        cfg->gva_home_va_set = true;
    }
    return true;
}

static bool stress_parse_args(int argc, char **argv, struct stress_config *cfg)
{
    int i;
    *cfg = (struct stress_config){
        .size = STRESS_DEFAULT_SIZE,
        .pattern = PATTERN_SEQ,
        .iterations = STRESS_DEFAULT_ITERATIONS,
        .flush = FLUSH_NONE,
        .period = 100,
        .verify = false,
        .read_only = false,
        .write_only = false,
        .chunk_size = STRESS_DEFAULT_CHUNK_SIZE,
        .seed = STRESS_DEFAULT_SEED,
        .node_count = 2,
        .peer_index = -1,
    };
    stress_init_gva_defaults(cfg);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            cfg->size = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--pattern") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            if (strcmp(p, "seq") == 0) cfg->pattern = PATTERN_SEQ;
            else if (strcmp(p, "random") == 0) cfg->pattern = PATTERN_RANDOM;
            else if (strcmp(p, "repeat") == 0) cfg->pattern = PATTERN_REPEAT;
            else if (strcmp(p, "mixed") == 0) cfg->pattern = PATTERN_MIXED;
            else { fprintf(stderr, "unknown pattern %s\n", p); return false; }
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            cfg->iterations = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--flush-mode") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            if (strcmp(p, "none") == 0) cfg->flush = FLUSH_NONE;
            else if (strcmp(p, "periodic") == 0) cfg->flush = FLUSH_PERIODIC;
            else if (strcmp(p, "every") == 0) cfg->flush = FLUSH_EVERY;
            else { fprintf(stderr, "unknown flush-mode %s\n", p); return false; }
        } else if (strcmp(argv[i], "--period") == 0 && i + 1 < argc) {
            cfg->period = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--verify") == 0) {
            cfg->verify = true;
        } else if (strcmp(argv[i], "--read-only") == 0) {
            cfg->read_only = true;
        } else if (strcmp(argv[i], "--write-only") == 0) {
            cfg->write_only = true;
        } else if (strcmp(argv[i], "--chunk-size") == 0 && i + 1 < argc) {
            cfg->chunk_size = strtoull(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            cfg->seed = strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--node-count") == 0 && i + 1 < argc) {
            uint32_t node_count;
            if (!parse_u32_arg(argv[++i], &node_count) ||
                node_count < 2 || node_count > OBMM_POOL_HELPERS_MAX_NODES) {
                fprintf(stderr, "invalid --node-count %s\n", argv[i]);
                return false;
            }
            cfg->node_count = (int)node_count;
        } else if (strcmp(argv[i], "--peer-index") == 0 && i + 1 < argc) {
            uint32_t peer_index;
            if (!parse_u32_arg(argv[++i], &peer_index) ||
                peer_index >= OBMM_POOL_HELPERS_MAX_NODES) {
                fprintf(stderr, "invalid --peer-index %s\n", argv[i]);
                return false;
            }
            cfg->peer_index = (int)peer_index;
        } else if (strcmp(argv[i], "--gva-mode") == 0 && i + 1 < argc) {
            if (!parse_stress_gva_mode(argv[++i], &cfg->gva_mode)) {
                fprintf(stderr, "unknown --gva-mode %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-map-source") == 0 && i + 1 < argc) {
            if (!parse_stress_map_source(argv[++i], &cfg->map_source)) {
                fprintf(stderr, "unknown --gva-map-source %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-address-profile") == 0 && i + 1 < argc) {
            if (!parse_stress_address_profile(argv[++i], &cfg->address_profile)) {
                fprintf(stderr, "unknown --gva-address-profile %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-cache-policy") == 0 && i + 1 < argc) {
            if (!parse_stress_cache_policy(argv[++i], &cfg->cache_policy)) {
                fprintf(stderr, "unknown --gva-cache-policy %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-vmid") == 0 && i + 1 < argc) {
            if (!parse_u32_arg(argv[++i], &cfg->vmid)) {
                fprintf(stderr, "invalid --gva-vmid %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-asid") == 0 && i + 1 < argc) {
            if (!parse_u32_arg(argv[++i], &cfg->asid)) {
                fprintf(stderr, "invalid --gva-asid %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-tid") == 0 && i + 1 < argc) {
            if (!parse_u32_arg(argv[++i], &cfg->tid)) {
                fprintf(stderr, "invalid --gva-tid %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-p-tag") == 0 && i + 1 < argc) {
            if (!parse_u32_arg(argv[++i], &cfg->p_tag)) {
                fprintf(stderr, "invalid --gva-p-tag %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-access-flags") == 0 && i + 1 < argc) {
            if (!parse_u32_arg(argv[++i], &cfg->access_flags)) {
                fprintf(stderr, "invalid --gva-access-flags %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-token-value") == 0 && i + 1 < argc) {
            if (!parse_u32_arg(argv[++i], &cfg->token_value)) {
                fprintf(stderr, "invalid --gva-token-value %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-id") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->gva_id)) {
                fprintf(stderr, "invalid --gva-id %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gva-user-va") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->local_va)) {
                fprintf(stderr, "invalid --gva-user-va %s\n", argv[i]);
                return false;
            }
            cfg->gva_local_va_set = true;
        } else if (strcmp(argv[i], "--gva-home-va") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->home_va)) {
                fprintf(stderr, "invalid --gva-home-va %s\n", argv[i]);
                return false;
            }
            cfg->gva_home_va_set = true;
        } else if (strcmp(argv[i], "--gva-pte-offset") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->pte_offset)) {
                fprintf(stderr, "invalid --gva-pte-offset %s\n", argv[i]);
                return false;
            }
            cfg->gva_pte_offset_set = true;
        } else if (strcmp(argv[i], "--gsva-base") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->gsva_base)) {
                fprintf(stderr, "invalid --gsva-base %s\n", argv[i]);
                return false;
            }
        } else if (strcmp(argv[i], "--gsva-generation") == 0 && i + 1 < argc) {
            if (!parse_u64_arg(argv[++i], &cfg->gsva_generation)) {
                fprintf(stderr, "invalid --gsva-generation %s\n", argv[i]);
                return false;
            }
        } else {
            fprintf(stderr, "unknown option %s\n", argv[i]);
            return false;
        }
    }
    if (cfg->chunk_size == 0 || cfg->chunk_size > cfg->size) {
        fprintf(stderr, "invalid chunk-size\n");
        return false;
    }
    if (cfg->read_only && cfg->write_only) {
        fprintf(stderr, "cannot use both --read-only and --write-only\n");
        return false;
    }
    if (cfg->peer_index >= cfg->node_count) {
        fprintf(stderr, "--peer-index must be less than --node-count\n");
        return false;
    }
    if (cfg->gva_mode == STRESS_GVA_MODE_GSVA && cfg->flush != FLUSH_NONE) {
        fprintf(stderr, "GSVA mode only supports --flush-mode none\n");
        return false;
    }
    if (cfg->gva_mode == STRESS_GVA_MODE_GSVA &&
        cfg->size > UINT64_MAX / OBMM_POOL_HELPERS_MAX_NODES) {
        fprintf(stderr, "GSVA aperture size overflow\n");
        return false;
    }
    return true;
}

static void stress_usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  --size <bytes>        Region size (default %lu)\n"
           "  --pattern <seq|random|repeat|mixed>\n"
           "  --iterations <n>      (default %d)\n"
           "  --flush-mode <none|periodic|every>\n"
           "  --period <n>          Flush period for periodic mode\n"
           "  --verify              Verify writes immediately\n"
           "  --read-only           Read stress only\n"
           "  --write-only          Write stress only\n"
           "  --chunk-size <bytes>  (default %d)\n"
           "  --seed <n>            RNG seed (default %d)\n"
           "  --node-count <n>      Participating nodes, 2..%d (default 2)\n"
           "  --peer-index <n>      Peer node to import, 0-based\n"
           "  --gva-mode <legacy|generic|gsva>\n"
           "  --gva-map-source <legacy|legacy-obmm|gva|gva-manager>\n"
           "  --gva-address-profile <generic|gsva>\n"
           "  --gva-cache-policy <nc|wt|rc|wb|mesi|directory-mesi>\n"
           "  --gva-vmid <n>\n"
           "  --gva-asid <n>\n"
           "  --gva-tid <n>\n"
           "  --gva-p-tag <n>\n"
           "  --gva-access-flags <n>\n"
           "  --gva-token-value <n>\n"
           "  --gva-id <n>\n"
           "  --gva-user-va <addr>\n"
           "  --gva-home-va <addr>\n"
           "  --gva-pte-offset <n>\n"
           "  --gsva-base <addr>\n"
           "  --gsva-generation <n>\n",
           prog, STRESS_DEFAULT_SIZE, STRESS_DEFAULT_ITERATIONS,
           STRESS_DEFAULT_CHUNK_SIZE, STRESS_DEFAULT_SEED,
           OBMM_POOL_HELPERS_MAX_NODES);
}

static bool stress_import_region(int obmm_fd, const struct stress_config *cfg,
                                const struct obmm_helpers_meta *remote_meta,
                                uint32_t local_cna, uint64_t local_pa,
                                uint64_t *import_mem_id)
{
    uint32_t token_value = cfg->token_value ? cfg->token_value : remote_meta->token_id;

    if (cfg->gva_mode == STRESS_GVA_MODE_LEGACY) {
        return obmm_do_import(obmm_fd, remote_meta, local_cna, local_pa,
                             token_value, import_mem_id) == 0;
    }

    return obmm_do_import_v2(obmm_fd, remote_meta, local_cna, local_pa,
                             token_value, cfg->map_source,
                             cfg->address_profile, cfg->cache_policy,
                             cfg->vmid, cfg->asid, cfg->tid, cfg->p_tag,
                             cfg->access_flags, cfg->gva_id,
                             cfg->local_va, cfg->home_va,
                             cfg->pte_offset, import_mem_id) == 0;
}

int main(int argc, char **argv)
{
    struct stress_config cfg;
    struct stress_stats stats;
    int obmm_fd = -1;
    int shmdev_fd = -1;
    uint64_t local_cna = 0;
    int local_idx = -1;
    int node_count = 0;
    int peer_idx = -1;
    struct obmm_helpers_meta local_meta = { 0 };
    struct obmm_helpers_meta remote_metas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    uint64_t local_pas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    uint64_t export_mem_id = 0;
    uint64_t import_mem_id = 0;
    bool gsva_aperture_registered = false;
    void *exported_va = NULL; /* unused; kept for cleanup symmetry */
    void *imported_va = NULL;
    int ret = 1;
    uint64_t generation = 1;
    char cmdline_val[64];
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES];

    if (!stress_parse_args(argc, argv, &cfg)) {
        stress_usage(argv[0]);
        return 1;
    }

    stress_log("starting size=%" PRIu64 " pattern=%d iterations=%" PRIu64
               " flush=%d chunk=%" PRIu64,
               cfg.size, cfg.pattern, cfg.iterations, cfg.flush, cfg.chunk_size);

    obmm_fd = open("/dev/obmm", O_RDWR);
    stress_log("open /dev/obmm fd=%d errno=%d", obmm_fd, errno);
    if (obmm_fd < 0) {
        stress_log("open /dev/obmm failed: %d", errno);
        goto cleanup;
    }

    if (obmm_cmdline_get("linqu_cna", cmdline_val, sizeof(cmdline_val))) {
        local_cna = strtoull(cmdline_val, NULL, 0);
        stress_log("cna from cmdline=%#x", (uint32_t)local_cna);
    } else {
        uint64_t cna_u64 = 0;
        if (!obmm_parse_hex_u64("/sys/bus/ub/devices/00001/primary_cna", &cna_u64)) {
            stress_log("cannot read linqu_cna from cmdline or sysfs");
            goto cleanup;
        }
        local_cna = (uint32_t)cna_u64;
        stress_log("cna from sysfs=%#x", (uint32_t)local_cna);
    }
    node_count = cfg.node_count;
    if (obmm_cmdline_get("linqu_node_idx", cmdline_val, sizeof(cmdline_val))) {
        local_idx = (int)strtol(cmdline_val, NULL, 0);
    } else if (stress_parse_local_ipv4_index(node_count, &local_idx)) {
        stress_log("local_idx from ip=%d", local_idx);
    } else {
        char role[32] = {0};
        if (obmm_cmdline_get("linqu_urma_dp_role", role, sizeof(role))) {
            if (strcmp(role, "nodeA") == 0 || strcmp(role, "exporter") == 0 ||
                strcmp(role, "initiator") == 0 || strcmp(role, "client") == 0) {
                local_idx = 0;
            } else {
                local_idx = 1;
            }
        } else {
            local_idx = 0;
        }
    }
    if (local_idx < 0 || local_idx >= node_count) {
        stress_log("invalid local_idx=%d node_count=%d", local_idx, node_count);
        goto cleanup;
    }
    peer_idx = cfg.peer_index >= 0
        ? cfg.peer_index
        : stress_default_peer_index(local_idx, node_count);
    if (peer_idx < 0 || peer_idx >= node_count || peer_idx == local_idx) {
        stress_log("invalid peer_idx=%d local_idx=%d node_count=%d",
                   peer_idx, local_idx, node_count);
        goto cleanup;
    }
    stress_log("local_idx=%d peer_idx=%d node_count=%d",
               local_idx, peer_idx, node_count);

    if (cfg.gva_mode == STRESS_GVA_MODE_GSVA) {
        if (!stress_register_gsva_aperture(obmm_fd, &cfg, local_idx,
                                           node_count)) {
            goto cleanup;
        }
        gsva_aperture_registered = true;
    }

    local_meta.export_cna = (uint32_t)local_cna;
    if (cfg.gva_mode == STRESS_GVA_MODE_GSVA) {
        uint64_t local_gsva = cfg.gsva_base + (uint64_t)local_idx * cfg.size;

        if (obmm_do_export_fixed_uba(obmm_fd, &local_meta, cfg.size,
                                     local_gsva) != 0) {
            stress_log("fixed GSVA export failed: size=%" PRIu64
                       " uba=%" PRIx64, cfg.size, local_gsva);
            goto cleanup;
        }
    } else if (obmm_do_export(obmm_fd, &local_meta, cfg.size) != 0) {
        stress_log("export failed: size=%" PRIu64, cfg.size);
        goto cleanup;
    }
    export_mem_id = local_meta.export_mem_id;
    stress_log("export ok mem_id=%" PRIx64 " uba=%" PRIx64 " token=%u",
               export_mem_id, local_meta.remote_uba, local_meta.token_id);

    if (obmm_bootstrap_publish(obmm_fd, local_idx, node_count,
                               generation, &local_meta) != 0) {
        stress_log("bootstrap publish failed errno=%d", errno);
        goto cleanup;
    }
    stress_log("bootstrap publish ok");

    if (obmm_bootstrap_lookup(obmm_fd, (uint32_t)local_cna, node_count,
                              generation, remote_metas, got) != 0) {
        stress_log("bootstrap lookup failed errno=%d", errno);
        goto cleanup;
    }
    stress_log("bootstrap lookup ok got_count=%d node_count=%d peer_got=%d",
               stress_got_count(got, node_count), node_count, got[peer_idx]);

    if (!got[peer_idx]) {
        stress_log("remote node not found in bootstrap");
        goto cleanup;
    }
    if (!stress_finalize_gva_config(&cfg, remote_metas[peer_idx].remote_uba)) {
        stress_log("invalid gva config");
        goto cleanup;
    }
    if (cfg.gva_mode == STRESS_GVA_MODE_GSVA && cfg.gva_id == 0) {
        cfg.gva_id = (uint64_t)peer_idx + 1;
    }
    stress_log("using gva config mode=%d map_source=%u address_profile=%u cache_policy=%u vmid=%u asid=%u tid=%u p_tag=%u access_flags=%u token_value=%u "
               "local_va=%" PRIx64 " home_va=%" PRIx64 " pte_offset=%" PRIx64,
               cfg.gva_mode, cfg.map_source, cfg.address_profile,
               cfg.cache_policy, cfg.vmid, cfg.asid, cfg.tid, cfg.p_tag,
               cfg.access_flags, cfg.token_value, cfg.local_va, cfg.home_va,
               cfg.pte_offset);

    /* Wait for OBMM device deferred probe to complete (mem_windows sysfs) */
    stress_log("waiting for OBMM device ready...");
    sleep(2);

    if (!obmm_alloc_import_pas(1, cfg.size, local_pas, import_osync,
                               obmm_parse_import_cache_mode())) {
        stress_log("cannot allocate import PA");
        goto cleanup;
    }
    stress_log("alloc_pas ok pa=%" PRIx64, local_pas[0]);
    if (!stress_import_region(
            obmm_fd, &cfg, &remote_metas[peer_idx], (uint32_t)local_cna,
            local_pas[0], &import_mem_id)) {
        stress_log("import failed errno=%d", errno);
        goto cleanup;
    }
    stress_log("import ok mem_id=%" PRIx64, import_mem_id);

    {
        struct obmm_helpers_region region = {0};
        void *target_va = NULL;

        if (cfg.gva_mode != STRESS_GVA_MODE_LEGACY && cfg.gva_local_va_set) {
            target_va = (void *)(uintptr_t)cfg.local_va;
        }
        int map_ret;

        if (cfg.gva_mode == STRESS_GVA_MODE_GSVA) {
            map_ret = obmm_map_gsva_region_at(import_mem_id, target_va,
                                              cfg.size, import_osync[0],
                                              &region);
        } else {
            map_ret = obmm_map_region_at(import_mem_id, target_va, cfg.size,
                                         import_osync[0], &region);
        }
        if (map_ret != 0) {
            stress_log("map import region failed mem_id=%" PRIx64, import_mem_id);
            goto cleanup;
        }
        if (target_va && region.addr != target_va) {
            stress_log("map mismatch: got=%p expect=%p", region.addr, target_va);
            goto cleanup;
        }
        imported_va = region.addr;
        shmdev_fd = region.fd;
    }

    stress_log("setup complete import_va=%p shmdev_fd=%d", imported_va, shmdev_fd);

    if (stress_run(&cfg, imported_va, shmdev_fd, &stats)) {
        if (!stress_completion_barrier(obmm_fd, local_idx, node_count,
                                       (uint32_t)local_cna, generation + 1,
                                       &local_meta)) {
            stress_log("completion barrier failed");
            goto cleanup;
        }
        stress_print_stats(&cfg, &stats);
        ret = 0;
    } else {
        stress_log("stress_run failed");
    }

cleanup:
    if (imported_va)
        munmap(imported_va, cfg.size);
    if (exported_va)
        munmap(exported_va, cfg.size);
    if (import_mem_id && obmm_fd >= 0)
        obmm_do_unimport(obmm_fd, import_mem_id);
    if (export_mem_id && obmm_fd >= 0)
        obmm_do_unexport(obmm_fd, export_mem_id);
    if (gsva_aperture_registered)
        stress_clear_gsva_aperture(obmm_fd, cfg.gsva_generation);
    if (shmdev_fd >= 0)
        close(shmdev_fd);
    if (obmm_fd >= 0)
        close(obmm_fd);
    return ret;
}
