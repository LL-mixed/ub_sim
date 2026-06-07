/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM Directory MESI Coherence Test
 *
 * Tests cross-node cache coherence for OBMM shared memory regions
 * using cache_policy=DIRECTORY_MESI (4).
 *
 * Usage: obmm_coh_test --mode <test> [options]
 *
 * Modes:
 *   write_read     -- Node A writes, Node B reads, verify data
 *   multi_reader   -- Node A exports, all non-home nodes read shared data
 *   writer_inv     -- reader caches a line, writer invalidates and updates it
 *   read_after_wb  -- dirty importer writeback, then exporter backing verify
 *   fence          -- COH_FENCE round-trip validation
 *   dirty_remote_write -- dirty owner is forced out by another remote writer
 *   mixed_rw       -- alias for dirty_remote_write
 *   dma_write_read -- requires a DMA-capable runner (not implemented here)
 *   all            -- Run all tests sequentially
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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

#define COH_DEFAULT_SIZE   (2UL * 1024UL * 1024UL)
#define COH_LINE_SIZE      64

#ifndef OBMM_SHMDEV_SYNC_REMOTE_RANGE
#define OBMM_SHMDEV_SYNC_REMOTE_RANGE _IOW('O', 0x14, struct obmm_cmd_sync_remote_range)
#endif

struct obmm_cmd_sync_remote_range {
    uint64_t offset;
    uint64_t length;
};

enum coh_test_mode {
    MODE_WRITE_READ,
    MODE_MULTI_READER,
    MODE_WRITER_INV,
    MODE_READ_AFTER_WB,
    MODE_FENCE,
    MODE_DIRTY_REMOTE_WRITE,
    MODE_MIXED_RW,
    MODE_DMA_WRITE_READ,
    MODE_ALL,
};

struct coh_config {
    enum coh_test_mode mode;
    uint64_t size;
    uint32_t cache_policy;
    uint32_t iterations;
    bool verbose;
    bool is_exporter;
    int node_id;
    int node_count;
    uint32_t token_value;
    uint64_t generation;
};

static uint64_t parse_uint64(const char *s, const char *name)
{
    char *end = NULL;
    uint64_t val;
    errno = 0;
    val = strtoull(s, &end, 0);
    if (errno || end == s) {
        fprintf(stderr, "invalid %s: %s\n", name, s);
        exit(1);
    }
    return val;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage: obmm_coh_test --mode <test> [options]\n"
        "\n"
        "Modes:\n"
        "  write_read     Node A writes, Node B reads, verify\n"
        "  multi_reader   Node A exports, all non-home nodes read shared data\n"
        "  writer_inv     Reader caches a line, writer invalidates and updates it\n"
        "  read_after_wb  Dirty importer writeback, exporter backing verify\n"
        "  fence          COH_FENCE round-trip validation\n"
        "  dirty_remote_write Dirty owner is forced out by another remote writer\n"
        "  mixed_rw       Alias for dirty_remote_write\n"
        "  dma_write_read Requires a DMA-capable runner\n"
        "  all            Run all tests\n"
        "\n"
        "Options:\n"
        "  --size <bytes>        Region size (default 2M)\n"
        "  --iterations <n>      Iterations (default 1)\n"
        "  --node-id <0|1>       Local node ID\n"
        "  --node-count <n>      Total nodes (default 2)\n"
        "  --is-exporter         This node is the exporter\n"
        "  --token-value <n>     Token value (default 0)\n"
        "  --generation <n>      Bootstrap generation (default 1)\n"
        "  --verbose             Verbose output\n");
}

static int parse_args(int argc, char **argv, struct coh_config *cfg)
{
    int i;
    memset(cfg, 0, sizeof(*cfg));
    cfg->size = COH_DEFAULT_SIZE;
    cfg->cache_policy = OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI;
    cfg->iterations = 1;
    cfg->node_count = 2;
    cfg->generation = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "write_read") == 0)
                cfg->mode = MODE_WRITE_READ;
            else if (strcmp(argv[i], "multi_reader") == 0)
                cfg->mode = MODE_MULTI_READER;
            else if (strcmp(argv[i], "writer_inv") == 0)
                cfg->mode = MODE_WRITER_INV;
            else if (strcmp(argv[i], "read_after_wb") == 0)
                cfg->mode = MODE_READ_AFTER_WB;
            else if (strcmp(argv[i], "fence") == 0)
                cfg->mode = MODE_FENCE;
            else if (strcmp(argv[i], "dirty_remote_write") == 0)
                cfg->mode = MODE_DIRTY_REMOTE_WRITE;
            else if (strcmp(argv[i], "mixed_rw") == 0)
                cfg->mode = MODE_MIXED_RW;
            else if (strcmp(argv[i], "dma_write_read") == 0)
                cfg->mode = MODE_DMA_WRITE_READ;
            else if (strcmp(argv[i], "all") == 0)
                cfg->mode = MODE_ALL;
            else {
                fprintf(stderr, "unknown mode: %s\n", argv[i]);
                return -1;
            }
        } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            cfg->size = parse_uint64(argv[++i], "size");
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            cfg->iterations = (uint32_t)parse_uint64(argv[++i], "iterations");
        } else if (strcmp(argv[i], "--node-id") == 0 && i + 1 < argc) {
            cfg->node_id = (int)parse_uint64(argv[++i], "node-id");
        } else if (strcmp(argv[i], "--node-count") == 0 && i + 1 < argc) {
            cfg->node_count = (int)parse_uint64(argv[++i], "node-count");
        } else if (strcmp(argv[i], "--is-exporter") == 0) {
            cfg->is_exporter = true;
        } else if (strcmp(argv[i], "--token-value") == 0 && i + 1 < argc) {
            cfg->token_value = (uint32_t)parse_uint64(argv[++i], "token-value");
        } else if (strcmp(argv[i], "--generation") == 0 && i + 1 < argc) {
            cfg->generation = parse_uint64(argv[++i], "generation");
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            exit(0);
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (cfg->iterations == 0) {
        fprintf(stderr, "iterations must be greater than 0\n");
        return -1;
    }
    if (cfg->size == 0 || (cfg->size % COH_LINE_SIZE) != 0) {
        fprintf(stderr, "size must be a non-zero multiple of %u bytes\n",
                COH_LINE_SIZE);
        return -1;
    }
    return 0;
}

static int do_sync_remote_range(int shmdev_fd, uint64_t offset, uint64_t len)
{
    struct obmm_cmd_sync_remote_range cmd = {
        .offset = offset,
        .length = len,
    };
    return ioctl(shmdev_fd, OBMM_SHMDEV_SYNC_REMOTE_RANGE, &cmd);
}

static void fill_pattern(uint64_t *buf, size_t count, uint64_t seed)
{
    size_t i;
    for (i = 0; i < count; i++) {
        buf[i] = seed + i;
    }
}

static bool verify_pattern(const uint64_t *buf, size_t count, uint64_t seed)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (buf[i] != seed + i) {
            fprintf(stderr, "VERIFY FAIL at offset %zu: expected %#" PRIx64
                    " got %#" PRIx64 "\n", i * 8, seed + i, buf[i]);
            return false;
        }
    }
    return true;
}

static bool verify_pattern_with_ready(const uint64_t *buf, size_t count,
                                      uint64_t seed, uint64_t ready)
{
    if (count == 0) {
        return false;
    }
    if (!verify_pattern(buf, count - 1, seed)) {
        return false;
    }
    if (buf[count - 1] != ready) {
        fprintf(stderr, "VERIFY FAIL at offset %zu: expected ready %#" PRIx64
                " got %#" PRIx64 "\n", (count - 1) * 8, ready, buf[count - 1]);
        return false;
    }
    return true;
}

static bool wait_word_value(const volatile uint64_t *word, uint64_t expected,
                            long timeout_ms)
{
    long deadline = obmm_now_ms() + timeout_ms;

    while (obmm_now_ms() < deadline) {
        if (*word == expected) {
            return true;
        }
        usleep(100000);
    }
    return *word == expected;
}

static int get_local_cna(void)
{
    static const char *paths[] = {
        "/sys/bus/ub/devices/00001/primary_cna",
        "/sys/bus/ub/devices/00001/port1/cna",
        "/sys/bus/ub/devices/00001/cna",
    };
    char buf[64];

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (obmm_read_file(paths[i], buf, sizeof(buf))) {
            return (int)strtoul(buf, NULL, 0);
        }
    }
    fprintf(stderr, "[obmm-coh-test] cannot read local CNA from UB sysfs\n");
    return 0;
}

static int lookup_exporter_meta(int obmm_fd, uint32_t local_cna,
                                int node_count, uint64_t generation,
                                uint32_t exporter_node,
                                struct obmm_helpers_meta *meta)
{
    long deadline = obmm_now_ms() + OBMM_POOL_HELPERS_WAIT_IFACE_MS;

    while (obmm_now_ms() < deadline) {
        struct obmm_cmd_bootstrap_lookup cmd;
        uint32_t i;

        memset(&cmd, 0, sizeof(cmd));
        cmd.generation = generation;
        cmd.node_count = (uint32_t)node_count;
        cmd.local_cna = local_cna;
        if (ioctl(obmm_fd, OBMM_CMD_BOOTSTRAP_LOOKUP, &cmd) != 0) {
            return -1;
        }

        for (i = 0; i < cmd.count; i++) {
            struct obmm_bootstrap_record *record = &cmd.records[i];

            if (record->node_id != exporter_node) {
                continue;
            }
            meta->export_mem_id = record->export_mem_id;
            meta->remote_uba = record->remote_uba;
            meta->size = record->size;
            meta->token_id = record->token_id;
            meta->export_cna = record->export_cna;
            return 0;
        }
        usleep(100000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int import_region_from_meta(int obmm_fd, const struct coh_config *cfg,
                                   uint32_t local_cna,
                                   const struct obmm_helpers_meta *meta,
                                   uint64_t *import_mem_id,
                                   struct obmm_helpers_region *region)
{
    uint64_t local_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {0};

    if (!obmm_alloc_import_pas(1, meta->size, local_pas, import_osync,
                               OBMM_IMPORT_CACHE_CC)) {
        fprintf(stderr, "cannot allocate cacheable import PA\n");
        return -1;
    }
    if (obmm_do_import_v2(obmm_fd, meta, local_cna, local_pas[0],
                          cfg->token_value,
                          OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM,
                          OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
                          cfg->cache_policy,
                          0, 0, 0, 0, 0, 0, 0, 0, 0,
                          import_mem_id) != 0) {
        fprintf(stderr, "import failed: %s\n", strerror(errno));
        return -1;
    }
    if (obmm_map_region(*import_mem_id, meta->size, false, region) != 0) {
        fprintf(stderr, "mmap import failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int import_exporter_region(int obmm_fd, const struct coh_config *cfg,
                                  uint32_t local_cna,
                                  struct obmm_helpers_meta *meta,
                                  uint64_t *import_mem_id,
                                  struct obmm_helpers_region *region)
{
    if (lookup_exporter_meta(obmm_fd, local_cna, cfg->node_count,
                             cfg->generation, 0, meta) != 0) {
        fprintf(stderr, "bootstrap lookup failed: %s\n", strerror(errno));
        return -1;
    }
    return import_region_from_meta(obmm_fd, cfg, local_cna, meta,
                                   import_mem_id, region);
}

static int run_write_read(struct coh_config *cfg)
{
    int obmm_fd;
    struct obmm_helpers_meta meta = {0};
    struct obmm_helpers_region region = {0};
    uint64_t import_mem_id = 0;
    uint64_t local_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    int local_cna = get_local_cna();
    uint64_t *data;
    size_t word_count;
    uint32_t iter;
    uint64_t final_seed;
    int rc = 0;

    printf("[write_read] starting (exporter=%d node=%d size=%" PRIu64 ")\n",
           cfg->is_exporter, cfg->node_id, cfg->size);
    final_seed = (uint64_t)(cfg->iterations - 1) * 1000;

    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        fprintf(stderr, "open /dev/obmm failed: %s\n", strerror(errno));
        return 1;
    }

    if (cfg->is_exporter) {
        if (obmm_do_export(obmm_fd, &meta, cfg->size) != 0) {
            fprintf(stderr, "export failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }
        meta.export_cna = local_cna;
        printf("[write_read] exported mem_id=%" PRIu64 " uba=%#" PRIx64
               " size=%" PRIu64 "\n", meta.export_mem_id, meta.remote_uba, meta.size);

        if (obmm_map_region(meta.export_mem_id, meta.size, true, &region) != 0) {
            fprintf(stderr, "mmap export failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }

        data = (uint64_t *)region.addr;
        word_count = cfg->size / sizeof(uint64_t);
        for (iter = 0; iter < cfg->iterations; iter++) {
            fill_pattern(data, word_count, iter * 1000);
            if (cfg->verbose)
                printf("[write_read] iter %u: wrote seed=%u*\n", iter, iter * 1000);
        }
        printf("[write_read] exporter final seed=%#" PRIx64 ", waiting for reaper...\n",
               final_seed);
        if (obmm_bootstrap_publish(obmm_fd, cfg->node_id, cfg->node_count,
                                   cfg->generation, &meta) != 0) {
            fprintf(stderr, "[write_read] bootstrap publish failed: %s\n",
                    strerror(errno));
            rc = 1;
            goto out;
        }
        sleep(30);
    } else {
        if (lookup_exporter_meta(obmm_fd, local_cna, cfg->node_count,
                                 cfg->generation, 0, &meta) != 0) {
            fprintf(stderr, "bootstrap lookup failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }

        printf("[write_read] importing mem_id=%" PRIu64 " uba=%#" PRIx64 "\n",
               meta.export_mem_id, meta.remote_uba);

        if (!obmm_alloc_import_pas(1, meta.size, local_pas, import_osync,
                                   OBMM_IMPORT_CACHE_CC)) {
            fprintf(stderr, "cannot allocate cacheable import PA\n");
            rc = 1;
            goto out;
        }

        if (obmm_do_import_v2(obmm_fd, &meta, local_cna, local_pas[0],
                              cfg->token_value,
                              OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM,
                              OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
                              cfg->cache_policy,
                              0, 0, 0, 0, 0, 0, 0, 0, 0,
                              &import_mem_id) != 0) {
            fprintf(stderr, "import failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }

        if (obmm_map_region(import_mem_id, meta.size, false, &region) != 0) {
            fprintf(stderr, "mmap import failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }

        data = (uint64_t *)region.addr;
        word_count = cfg->size / sizeof(uint64_t);
        if (cfg->node_count > 2 && word_count > 4096)
            word_count = 4096;
        if (!verify_pattern(data, word_count, final_seed)) {
            fprintf(stderr, "[write_read] VERIFY FAILED final_seed=%#" PRIx64 "\n",
                    final_seed);
            rc = 1;
            goto out;
        }
        printf("[write_read] importer verify OK final_seed=%#" PRIx64 "\n",
               final_seed);
    }

out:
    obmm_unmap_region(&region);
    if (import_mem_id)
        obmm_do_unimport(obmm_fd, import_mem_id);
    if (cfg->is_exporter && meta.export_mem_id)
        obmm_do_unexport(obmm_fd, meta.export_mem_id);
    close(obmm_fd);
    return rc;
}

static int run_multi_reader(struct coh_config *cfg)
{
    int obmm_fd;
    struct obmm_helpers_meta meta = {0};
    struct obmm_helpers_region region = {0};
    uint64_t import_mem_id = 0;
    int local_cna = get_local_cna();
    int rc = 0;

    printf("[multi_reader] starting (exporter=%d node=%d node_count=%d)\n",
           cfg->is_exporter, cfg->node_id, cfg->node_count);
    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        fprintf(stderr, "open /dev/obmm failed: %s\n", strerror(errno));
        return 1;
    }
    if (cfg->is_exporter) {
        if (obmm_do_export(obmm_fd, &meta, cfg->size) != 0) {
            fprintf(stderr, "export failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }
        meta.export_cna = local_cna;
        if (obmm_bootstrap_publish(obmm_fd, cfg->node_id, cfg->node_count,
                                   cfg->generation, &meta) != 0) {
            fprintf(stderr, "[multi_reader] bootstrap publish failed: %s\n",
                    strerror(errno));
            rc = 1;
            goto out;
        }
        sleep(30);
    } else {
        int shmdev_fd = -1;
        uint64_t *data;
        uint32_t iter;
        size_t word_count;
        const uint64_t ready = 0x5152535455565758ULL;

        if (import_exporter_region(obmm_fd, cfg, local_cna, &meta,
                                   &import_mem_id, &region) != 0) {
            rc = 1;
            goto out;
        }
        data = (uint64_t *)region.addr;
        word_count = cfg->size / sizeof(uint64_t);
        shmdev_fd = obmm_open_shmdev(import_mem_id, false);
        if (shmdev_fd < 0) {
            fprintf(stderr, "[multi_reader] open import shmdev failed: %s\n",
                    strerror(errno));
            rc = 1;
            goto multi_importer_out;
        }
        if (cfg->node_id == 1) {
            fill_pattern(data, word_count, 0x510000);
            data[word_count - 1] = ready;
            if (do_sync_remote_range(shmdev_fd, 0, cfg->size) != 0) {
                fprintf(stderr, "[multi_reader] writer fence failed: %s\n",
                        strerror(errno));
                rc = 1;
                goto multi_importer_out;
            }
            printf("[multi_reader] node=%d initialized coherent data\n",
                   cfg->node_id);
        }
        if (!wait_word_value((volatile uint64_t *)&data[word_count - 1],
                             ready, 30000)) {
            fprintf(stderr, "[multi_reader] node=%d did not observe ready\n",
                    cfg->node_id);
            rc = 1;
            goto multi_importer_out;
        }
        for (iter = 0; iter < cfg->iterations; iter++) {
            if (!verify_pattern_with_ready(data, word_count, 0x510000, ready)) {
                fprintf(stderr, "[multi_reader] verify failed iter=%u\n", iter);
                rc = 1;
                goto multi_importer_out;
            }
        }
        printf("[multi_reader] node=%d verify OK iterations=%u\n",
               cfg->node_id, cfg->iterations);
multi_importer_out:
        if (shmdev_fd >= 0)
            close(shmdev_fd);
    }

out:
    obmm_unmap_region(&region);
    if (import_mem_id)
        obmm_do_unimport(obmm_fd, import_mem_id);
    if (cfg->is_exporter && meta.export_mem_id)
        obmm_do_unexport(obmm_fd, meta.export_mem_id);
    close(obmm_fd);
    return rc;
}

static int run_writer_inv(struct coh_config *cfg)
{
    enum {
        DATA_WORD = 0,
        READER_READY_WORD = COH_LINE_SIZE / sizeof(uint64_t),
        WRITER_DONE_WORD = (COH_LINE_SIZE * 2) / sizeof(uint64_t),
    };
    const uint64_t initial = 0x1111222233334444ULL;
    const uint64_t updated = 0x5555666677778888ULL;
    int obmm_fd;
    int shmdev_fd = -1;
    struct obmm_helpers_meta meta = {0};
    struct obmm_helpers_region region = {0};
    uint64_t import_mem_id = 0;
    int local_cna = get_local_cna();
    uint64_t *data;
    int rc = 0;

    printf("[writer_inv] starting (exporter=%d node=%d)\n",
           cfg->is_exporter, cfg->node_id);
    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        fprintf(stderr, "open /dev/obmm failed: %s\n", strerror(errno));
        return 1;
    }
    if (cfg->is_exporter) {
        if (obmm_do_export(obmm_fd, &meta, cfg->size) != 0) {
            rc = 1;
            goto out;
        }
        meta.export_cna = local_cna;
        if (obmm_map_region(meta.export_mem_id, meta.size, true, &region) != 0) {
            rc = 1;
            goto out;
        }
        data = (uint64_t *)region.addr;
        memset(data, 0, cfg->size);
        data[DATA_WORD] = initial;
        if (obmm_bootstrap_publish(obmm_fd, cfg->node_id, cfg->node_count,
                                   cfg->generation, &meta) != 0) {
            rc = 1;
            goto out;
        }
        if (!wait_word_value((volatile uint64_t *)&data[WRITER_DONE_WORD],
                             updated, 30000)) {
            fprintf(stderr, "[writer_inv] exporter did not observe writer done\n");
            rc = 1;
            goto out;
        }
        if (data[DATA_WORD] != updated) {
            fprintf(stderr, "[writer_inv] persistent data mismatch got %#" PRIx64
                    " expected %#" PRIx64 "\n", data[DATA_WORD], updated);
            rc = 1;
            goto out;
        }
    } else {
        if (import_exporter_region(obmm_fd, cfg, local_cna, &meta,
                                   &import_mem_id, &region) != 0) {
            rc = 1;
            goto out;
        }
        data = (uint64_t *)region.addr;
        shmdev_fd = obmm_open_shmdev(import_mem_id, false);
        if (shmdev_fd < 0) {
            fprintf(stderr, "open shmdev failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }
        if (cfg->node_id == 1) {
            if (data[DATA_WORD] != initial) {
                fprintf(stderr, "[writer_inv] reader initial mismatch\n");
                rc = 1;
                goto out;
            }
            data[READER_READY_WORD] = initial;
            if (do_sync_remote_range(shmdev_fd, COH_LINE_SIZE, COH_LINE_SIZE) != 0) {
                fprintf(stderr, "[writer_inv] reader ready fence failed: %s\n",
                        strerror(errno));
                rc = 1;
                goto out;
            }
            if (!wait_word_value((volatile uint64_t *)&data[WRITER_DONE_WORD],
                                 updated, 30000)) {
                fprintf(stderr, "[writer_inv] reader did not observe writer done\n");
                rc = 1;
                goto out;
            }
            if (data[DATA_WORD] != updated) {
                fprintf(stderr, "[writer_inv] reader stale line got %#" PRIx64
                        " expected %#" PRIx64 "\n", data[DATA_WORD], updated);
                rc = 1;
                goto out;
            }
            printf("[writer_inv] reader observed invalidated update\n");
        } else if (cfg->node_id == 2) {
            if (!wait_word_value((volatile uint64_t *)&data[READER_READY_WORD],
                                 initial, 30000)) {
                fprintf(stderr, "[writer_inv] writer did not observe reader ready\n");
                rc = 1;
                goto out;
            }
            data[DATA_WORD] = updated;
            data[WRITER_DONE_WORD] = updated;
            if (do_sync_remote_range(shmdev_fd, 0, COH_LINE_SIZE * 3) != 0) {
                fprintf(stderr, "[writer_inv] writer fence failed: %s\n",
                        strerror(errno));
                rc = 1;
                goto out;
            }
            printf("[writer_inv] writer published update\n");
        } else {
            if (!wait_word_value((volatile uint64_t *)&data[WRITER_DONE_WORD],
                                 updated, 30000)) {
                fprintf(stderr, "[writer_inv] observer did not observe done\n");
                rc = 1;
                goto out;
            }
        }
    }

out:
    if (shmdev_fd >= 0)
        close(shmdev_fd);
    obmm_unmap_region(&region);
    if (import_mem_id)
        obmm_do_unimport(obmm_fd, import_mem_id);
    if (cfg->is_exporter && meta.export_mem_id)
        obmm_do_unexport(obmm_fd, meta.export_mem_id);
    close(obmm_fd);
    return rc;
}

static int run_dirty_remote_write(struct coh_config *cfg)
{
    enum {
        DATA_WORD = 0,
        OWNER_READY_WORD = COH_LINE_SIZE / sizeof(uint64_t),
        WRITER_DONE_WORD = (COH_LINE_SIZE * 2) / sizeof(uint64_t),
    };
    const uint64_t owner_value = 0xa1a2a3a4a5a6a7a8ULL;
    const uint64_t final_value = 0xb1b2b3b4b5b6b7b8ULL;
    int obmm_fd;
    int shmdev_fd = -1;
    struct obmm_helpers_meta meta = {0};
    struct obmm_helpers_region region = {0};
    uint64_t import_mem_id = 0;
    int local_cna = get_local_cna();
    uint64_t *data;
    int rc = 0;

    printf("[dirty_remote_write] starting (exporter=%d node=%d)\n",
           cfg->is_exporter, cfg->node_id);
    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        fprintf(stderr, "open /dev/obmm failed: %s\n", strerror(errno));
        return 1;
    }
    if (cfg->is_exporter) {
        if (obmm_do_export(obmm_fd, &meta, cfg->size) != 0) {
            rc = 1;
            goto out;
        }
        meta.export_cna = local_cna;
        if (obmm_map_region(meta.export_mem_id, meta.size, true, &region) != 0) {
            rc = 1;
            goto out;
        }
        data = (uint64_t *)region.addr;
        memset(data, 0, cfg->size);
        if (obmm_bootstrap_publish(obmm_fd, cfg->node_id, cfg->node_count,
                                   cfg->generation, &meta) != 0) {
            rc = 1;
            goto out;
        }
        if (!wait_word_value((volatile uint64_t *)&data[WRITER_DONE_WORD],
                             final_value, 30000)) {
            fprintf(stderr, "[dirty_remote_write] exporter did not observe done\n");
            rc = 1;
            goto out;
        }
        if (data[DATA_WORD] != final_value) {
            fprintf(stderr, "[dirty_remote_write] final mismatch got %#" PRIx64
                    " expected %#" PRIx64 "\n", data[DATA_WORD], final_value);
            rc = 1;
            goto out;
        }
    } else {
        if (import_exporter_region(obmm_fd, cfg, local_cna, &meta,
                                   &import_mem_id, &region) != 0) {
            rc = 1;
            goto out;
        }
        data = (uint64_t *)region.addr;
        shmdev_fd = obmm_open_shmdev(import_mem_id, false);
        if (shmdev_fd < 0) {
            fprintf(stderr, "open shmdev failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }
        if (cfg->node_id == 1) {
            data[DATA_WORD] = owner_value;
            data[OWNER_READY_WORD] = owner_value;
            if (do_sync_remote_range(shmdev_fd, COH_LINE_SIZE, COH_LINE_SIZE) != 0) {
                fprintf(stderr, "[dirty_remote_write] owner ready fence failed: %s\n",
                        strerror(errno));
                rc = 1;
                goto out;
            }
            if (!wait_word_value((volatile uint64_t *)&data[WRITER_DONE_WORD],
                                 final_value, 30000)) {
                fprintf(stderr, "[dirty_remote_write] owner did not observe done\n");
                rc = 1;
                goto out;
            }
        } else if (cfg->node_id == 2) {
            if (!wait_word_value((volatile uint64_t *)&data[OWNER_READY_WORD],
                                 owner_value, 30000)) {
                fprintf(stderr, "[dirty_remote_write] writer did not observe owner ready\n");
                rc = 1;
                goto out;
            }
            data[DATA_WORD] = final_value;
            data[WRITER_DONE_WORD] = final_value;
            if (do_sync_remote_range(shmdev_fd, 0, COH_LINE_SIZE * 3) != 0) {
                fprintf(stderr, "[dirty_remote_write] writer fence failed: %s\n",
                        strerror(errno));
                rc = 1;
                goto out;
            }
            printf("[dirty_remote_write] writer forced dirty owner transfer\n");
        } else {
            if (!wait_word_value((volatile uint64_t *)&data[WRITER_DONE_WORD],
                                 final_value, 30000)) {
                fprintf(stderr, "[dirty_remote_write] observer did not observe done\n");
                rc = 1;
                goto out;
            }
        }
    }

out:
    if (shmdev_fd >= 0)
        close(shmdev_fd);
    obmm_unmap_region(&region);
    if (import_mem_id)
        obmm_do_unimport(obmm_fd, import_mem_id);
    if (cfg->is_exporter && meta.export_mem_id)
        obmm_do_unexport(obmm_fd, meta.export_mem_id);
    close(obmm_fd);
    return rc;
}

static int run_fence_test(struct coh_config *cfg)
{
    int obmm_fd;
    struct obmm_helpers_meta meta = {0};
    struct obmm_helpers_region region = {0};
    uint64_t import_mem_id = 0;
    uint64_t local_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    int local_cna = get_local_cna();
    int shmdev_fd = -1;
    uint64_t *data;
    size_t word_count;
    int rc = 0;
    long t0, t1;

    printf("[fence] starting fence round-trip test\n");

    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        fprintf(stderr, "open /dev/obmm failed: %s\n", strerror(errno));
        return 1;
    }

    if (cfg->is_exporter) {
        if (obmm_do_export(obmm_fd, &meta, cfg->size) != 0) {
            fprintf(stderr, "export failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }
        meta.export_cna = local_cna;
        if (obmm_map_region(meta.export_mem_id, meta.size, true, &region) != 0) {
            rc = 1;
            goto out;
        }
        data = (uint64_t *)region.addr;
        word_count = cfg->size / sizeof(uint64_t);
        fill_pattern(data, word_count, 0xDEADBEEF);
        if (obmm_bootstrap_publish(obmm_fd, cfg->node_id, cfg->node_count,
                                   cfg->generation, &meta) != 0) {
            fprintf(stderr, "[fence] bootstrap publish failed: %s\n",
                    strerror(errno));
            rc = 1;
            goto out;
        }
        printf("[fence] exporter wrote pattern, waiting for importer fence\n");
        if (!wait_word_value((volatile uint64_t *)&data[0], 0xCAFEBABE,
                             cfg->node_count > 2 ? 120000 : 30000)) {
            fprintf(stderr, "[fence] persistent-point verify failed: got %#" PRIx64
                    " expected %#" PRIx64 "\n", data[0],
                    (uint64_t)0xCAFEBABE);
            rc = 1;
            goto out;
        }
        printf("[fence] exporter persistent-point verify OK: %#" PRIx64 "\n",
               data[0]);
    } else {
        if (lookup_exporter_meta(obmm_fd, local_cna, cfg->node_count,
                                 cfg->generation, 0, &meta) != 0) {
            rc = 1;
            goto out;
        }

        if (!obmm_alloc_import_pas(1, meta.size, local_pas, import_osync,
                                   OBMM_IMPORT_CACHE_CC)) {
            fprintf(stderr, "cannot allocate cacheable import PA\n");
            rc = 1;
            goto out;
        }

        if (obmm_do_import_v2(obmm_fd, &meta, local_cna, local_pas[0],
                              cfg->token_value,
                              OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM,
                              OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
                              cfg->cache_policy,
                              0, 0, 0, 0, 0, 0, 0, 0, 0,
                              &import_mem_id) != 0) {
            rc = 1;
            goto out;
        }
        if (obmm_map_region(import_mem_id, meta.size, false, &region) != 0) {
            rc = 1;
            goto out;
        }
        data = (uint64_t *)region.addr;

        shmdev_fd = obmm_open_shmdev(import_mem_id, false);
        if (shmdev_fd < 0) {
            fprintf(stderr, "open shmdev for fence failed: %s\n", strerror(errno));
            rc = 1;
            goto out;
        }

        if (cfg->node_count > 2 && cfg->node_id != 1) {
            if (!wait_word_value((volatile uint64_t *)&data[0], 0xCAFEBABE,
                                 120000)) {
                fprintf(stderr, "[fence] observer did not observe dirty data\n");
                rc = 1;
                goto out;
            }
            printf("[fence] observer saw persistent value\n");
        } else {
            /* Read data first to populate local cache */
            word_count = cfg->size / sizeof(uint64_t);
            if (cfg->node_count > 2 && word_count > 4096)
                word_count = 4096;
            if (!verify_pattern(data, word_count, 0xDEADBEEF)) {
                fprintf(stderr, "[fence] initial read failed\n");
                rc = 1;
                goto out;
            }
            printf("[fence] initial read OK\n");

            /* Write dirty data locally */
            data[0] = 0xCAFEBABE;
            printf("[fence] wrote dirty data at offset 0\n");

            /* Sync (fence) to flush dirty data back */
            t0 = obmm_now_ms();
            if (do_sync_remote_range(shmdev_fd, 0, cfg->size) != 0) {
                fprintf(stderr, "[fence] sync_remote_range failed: %s\n", strerror(errno));
                rc = 1;
                goto out;
            }
            t1 = obmm_now_ms();
            printf("[fence] fence completed in %ld ms\n", t1 - t0);
        }
    }

out:
    if (shmdev_fd >= 0)
        close(shmdev_fd);
    obmm_unmap_region(&region);
    if (import_mem_id)
        obmm_do_unimport(obmm_fd, import_mem_id);
    if (cfg->is_exporter && meta.export_mem_id)
        obmm_do_unexport(obmm_fd, meta.export_mem_id);
    close(obmm_fd);
    return rc;
}

static int run_test(enum coh_test_mode mode, struct coh_config *cfg)
{
    switch (mode) {
    case MODE_WRITE_READ:
        return run_write_read(cfg);
    case MODE_FENCE:
        return run_fence_test(cfg);
    case MODE_WRITER_INV:
        return run_writer_inv(cfg);
    case MODE_DIRTY_REMOTE_WRITE:
        return run_dirty_remote_write(cfg);
    case MODE_MIXED_RW:
        return run_dirty_remote_write(cfg);
    case MODE_MULTI_READER:
        return run_multi_reader(cfg);
    case MODE_READ_AFTER_WB:
        return run_fence_test(cfg);
    case MODE_DMA_WRITE_READ:
        fprintf(stderr, "[dma_write_read] not implemented in this dual-node CLI\n");
        return 1;
    case MODE_ALL: {
        int total = 0;
        const struct {
            const char *name;
            enum coh_test_mode m;
        } tests[] = {
            {"write_read", MODE_WRITE_READ},
            {"fence", MODE_FENCE},
            {"read_after_wb", MODE_READ_AFTER_WB},
        };
        size_t t;
        for (t = 0; t < sizeof(tests) / sizeof(tests[0]); t++) {
            struct coh_config sub_cfg = *cfg;
            int ret;

            sub_cfg.generation = cfg->generation + t;
            printf("\n=== running %s ===\n", tests[t].name);
            ret = run_test(tests[t].m, &sub_cfg);
            printf("=== %s: %s ===\n", tests[t].name, ret ? "FAIL" : "PASS");
            total += ret;
        }
        return total;
    }
    }
    return 1;
}

int main(int argc, char **argv)
{
    struct coh_config cfg;
    int rc;

    if (parse_args(argc, argv, &cfg) != 0) {
        usage();
        return 1;
    }

    printf("OBMM Coherence Test\n");
    printf("  mode=%d size=%" PRIu64 " cache_policy=%u iterations=%u\n",
           cfg.mode, cfg.size, cfg.cache_policy, cfg.iterations);
    printf("  node_id=%d node_count=%d is_exporter=%d\n",
           cfg.node_id, cfg.node_count, cfg.is_exporter);

    rc = run_test(cfg.mode, &cfg);

    printf("\nobmm_coh_test: %s\n", rc ? "FAIL" : "PASS");
    return rc;
}
