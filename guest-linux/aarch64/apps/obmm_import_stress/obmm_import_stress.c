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

    if (ok && cfg->flush == FLUSH_NONE && !cfg->read_only) {
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
    };

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
           "  --seed <n>            RNG seed (default %d)\n",
           prog, STRESS_DEFAULT_SIZE, STRESS_DEFAULT_ITERATIONS,
           STRESS_DEFAULT_CHUNK_SIZE, STRESS_DEFAULT_SEED);
}

int main(int argc, char **argv)
{
    struct stress_config cfg;
    struct stress_stats stats;
    int obmm_fd = -1;
    int shmdev_fd = -1;
    uint64_t local_cna = 0;
    int local_idx = -1;
    int node_count = 2;
    struct obmm_helpers_meta local_meta = { 0 };
    struct obmm_helpers_meta remote_metas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    uint64_t local_pas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    uint64_t export_mem_id = 0;
    uint64_t import_mem_id = 0;
    void *exported_va = NULL; /* unused; kept for cleanup symmetry */
    void *imported_va = NULL;
    int ret = 1;
    uint64_t generation = 1;
    char cmdline_val[64];

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
    if (obmm_cmdline_get("linqu_node_idx", cmdline_val, sizeof(cmdline_val))) {
        local_idx = (int)strtol(cmdline_val, NULL, 0);
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
    stress_log("local_idx=%d node_count=%d", local_idx, node_count);

    local_meta.export_cna = (uint32_t)local_cna;
    if (obmm_do_export(obmm_fd, &local_meta, cfg.size) != 0) {
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
    stress_log("bootstrap lookup ok got[0]=%d got[1]=%d", got[0], got[1]);

    if (!got[local_idx ^ 1]) {
        stress_log("remote node not found in bootstrap");
        goto cleanup;
    }

    /* Wait for OBMM device deferred probe to complete (mem_windows sysfs) */
    stress_log("waiting for OBMM device ready...");
    sleep(2);

    bool import_osync[1];
    if (!obmm_alloc_import_pas(1, cfg.size, local_pas, import_osync,
                               obmm_parse_import_cache_mode())) {
        stress_log("cannot allocate import PA");
        goto cleanup;
    }
    stress_log("alloc_pas ok pa=%" PRIx64, local_pas[0]);
    if (obmm_do_import(obmm_fd, &remote_metas[local_idx ^ 1], (uint32_t)local_cna,
                       local_pas[0], 0, &import_mem_id) != 0) {
        stress_log("import failed errno=%d", errno);
        goto cleanup;
    }
    stress_log("import ok mem_id=%" PRIx64, import_mem_id);

    {
        struct obmm_helpers_region region = {0};
        if (obmm_map_region(import_mem_id, cfg.size, import_osync[0], &region) != 0) {
            stress_log("map import region failed mem_id=%" PRIx64, import_mem_id);
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
    if (shmdev_fd >= 0)
        close(shmdev_fd);
    if (obmm_fd >= 0)
        close(obmm_fd);
    return ret;
}
