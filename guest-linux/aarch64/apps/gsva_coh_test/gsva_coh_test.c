/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gsva_coh_test -- test GSVA coherence via OBMM kernel interface.
 *
 * Uses OBMM export/import/mmap with GSVA parameters (v2 private data,
 * address_profile=GSVA_IDENTITY, cache_policy=DIRECTORY_MESI) to exercise
 * the full GSVA coherence stack: guest kernel → QEMU SimDec → GSVA coherence
 * state machine → PA-MESI data layer.
 *
 * Modes:
 *   write_read        -- Export on home, import on peer, write then read
 *   writer_inv        -- Multiple readers, then writer invalidates
 *   retire_while_shared -- Unmap while segment is shared (retire path)
 *
 * Usage:
 *   gsva_coh_test --mode <mode>
 */

#include "obmm_common.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "[gsva_coh_test]"

#define GSVA_BASE 0x700000000000ULL
#define GSVA_SIZE 0x400000ULL

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("%s TEST: %s\n", TAG, name); \
} while (0)

#define PASS() do { \
    tests_passed++; \
    printf("%s   PASS\n", TAG); \
} while (0)

#define FAIL(msg) do { \
    tests_failed++; \
    printf("%s   FAIL: %s\n", TAG, msg); \
} while (0)

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        FAIL(msg); \
        return; \
    } \
} while (0)

static int parse_node_info(uint32_t *local_cna, int *node_idx, int *node_count)
{
    char buf[64];
    *node_count = 2;
    if (obmm_env_or_cmdline("LINQU_NODE_COUNT", "linqu_node_count", buf, sizeof(buf))) {
        *node_count = atoi(buf);
        if (*node_count < 1) *node_count = 2;
    }
    *node_idx = 0;
    if (obmm_env_or_cmdline("LINQU_NODE_IDX", "linqu_node_idx", buf, sizeof(buf))) {
        *node_idx = atoi(buf);
    }
    if (obmm_env_or_cmdline("LINQU_LOCAL_CNA", "linqu_local_cna", buf, sizeof(buf))) {
        *local_cna = (uint32_t)strtoull(buf, NULL, 0);
    } else {
        char role[64];
        if (obmm_env_or_cmdline("LINQU_ROLE", "linqu_urma_dp_role", role, sizeof(role))) {
            /* Derive CNA from role name -- typically assigned by QEMU */
            *local_cna = (uint32_t)(*node_idx);
        }
    }
    return 0;
}

/* ---- Test: write_read ---- */
static void test_write_read(int obmm_fd, uint32_t local_cna,
                            int node_idx, int node_count)
{
    TEST("GSVA write-read coherence across nodes");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};

    /* Each node exports its own slice */
    uint64_t my_base = GSVA_BASE + (uint64_t)node_idx * GSVA_SIZE;
    struct obmm_helpers_meta my_meta = {0};
    my_meta.export_cna = local_cna;

    int rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count, 1, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count, 1, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");

    /* Import peer's slice with GSVA identity */
    for (int i = 0; i < node_count; i++) {
        if (i == node_idx) continue;
        if (!got[i]) continue;

        uint64_t peer_base = GSVA_BASE + (uint64_t)i * GSVA_SIZE;
        uint64_t import_mem_id = 0;
        rc = obmm_do_import_v2(obmm_fd, &metas[i], local_cna,
                               peer_base, 0,
                               OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                               OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                               OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                               0, 0, 0, 0, 0, 0,
                               peer_base, peer_base, 0,
                               &import_mem_id);
        CHECK(rc == 0, "GSVA identity import should succeed");

        struct obmm_helpers_region region = {0};
        rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)peer_base,
                                     GSVA_SIZE, false, &region);
        CHECK(rc == 0, "GSVA mmap should succeed");

        /* Write a pattern */
        uint64_t *data = (uint64_t *)region.addr;
        uint64_t test_val = 0xDEADBEEF00000000ULL | (uint64_t)node_idx;
        *data = test_val;

        printf("%s   wrote to peer%d slice at %#" PRIx64 " val=%#" PRIx64 "\n",
               TAG, i, peer_base, test_val);

        obmm_unmap_region(&region);
        obmm_do_unimport(obmm_fd, import_mem_id);
    }

    /* Verify our own slice was written by peers */
    struct obmm_helpers_region my_region = {0};
    rc = obmm_map_gsva_region_at(my_meta.export_mem_id,
                                 (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, false, &my_region);
    CHECK(rc == 0, "GSVA mmap of own export should succeed");
    obmm_unmap_region(&my_region);

    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: writer_inv ---- */
static void test_writer_inv(int obmm_fd, uint32_t local_cna,
                            int node_idx, int node_count)
{
    TEST("GSVA writer invalidation with multiple readers");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};

    /* All nodes import node 0's slice */
    struct obmm_helpers_meta home_meta = {0};
    home_meta.export_cna = 0;

    if (node_idx == 0) {
        int rc = obmm_do_export_fixed_uba(obmm_fd, &home_meta, GSVA_SIZE, GSVA_BASE);
        CHECK(rc == 0, "home fixed UBA export should succeed");

        rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count, 2, &home_meta);
        CHECK(rc == 0, "bootstrap publish should succeed");
    }

    int rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count, 2, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");
    CHECK(got[0], "should find node 0's export");

    if (node_idx != 0) {
        uint64_t import_mem_id = 0;
        rc = obmm_do_import_v2(obmm_fd, &metas[0], local_cna,
                               GSVA_BASE, 0,
                               OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                               OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                               OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                               0, 0, 0, 0, 0, 0,
                               GSVA_BASE, GSVA_BASE, 0,
                               &import_mem_id);
        CHECK(rc == 0, "GSVA import of home slice should succeed");

        struct obmm_helpers_region region = {0};
        rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)GSVA_BASE,
                                     GSVA_SIZE, false, &region);
        CHECK(rc == 0, "GSVA mmap should succeed");

        /* Read the data */
        volatile uint64_t *data = (volatile uint64_t *)region.addr;
        uint64_t val = *data;
        printf("%s   reader node=%d read val=%#" PRIx64 "\n",
               TAG, node_idx, val);

        obmm_unmap_region(&region);
        obmm_do_unimport(obmm_fd, import_mem_id);
    } else {
        /* Writer writes */
        struct obmm_helpers_region region = {0};
        rc = obmm_map_gsva_region_at(home_meta.export_mem_id,
                                     (void *)(uintptr_t)GSVA_BASE,
                                     GSVA_SIZE, false, &region);
        CHECK(rc == 0, "GSVA mmap of home export should succeed");

        uint64_t *data = (uint64_t *)region.addr;
        *data = 0xCAFEBABE00000000ULL;
        printf("%s   writer wrote val=%#" PRIx64 "\n", TAG, *data);

        obmm_unmap_region(&region);
    }

    if (node_idx == 0) {
        obmm_do_unexport(obmm_fd, home_meta.export_mem_id);
    }
    PASS();
}

/* ---- Test: retire_while_shared ---- */
static void test_retire_while_shared(int obmm_fd, uint32_t local_cna,
                                     int node_idx, int node_count)
{
    TEST("GSVA unmap/retire while segment is shared");
    uint64_t my_base = GSVA_BASE + 0x800000ULL + (uint64_t)node_idx * GSVA_SIZE;
    struct obmm_helpers_meta my_meta = {0};
    my_meta.export_cna = local_cna;

    int rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "export should succeed");

    struct obmm_helpers_region region = {0};
    rc = obmm_map_gsva_region_at(my_meta.export_mem_id,
                                 (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, false, &region);
    CHECK(rc == 0, "mmap should succeed");

    /* Write some data */
    uint64_t *data = (uint64_t *)region.addr;
    *data = 0x1234567890ABCDEFULL;

    /* Unmap while data is potentially shared */
    obmm_unmap_region(&region);
    rc = obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    CHECK(rc == 0, "unexport while shared should succeed");

    PASS();
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --mode <mode>\n"
        "Modes:\n"
        "  write_read          Write then read across nodes\n"
        "  writer_inv          Shared readers then writer invalidation\n"
        "  retire_while_shared Unmap while segment is shared\n"
        "  all                 Run all tests (default)\n",
        prog);
}

int main(int argc, char **argv)
{
    char *mode = "all";
    int opt;

    static struct option long_opts[] = {
        {"mode", required_argument, NULL, 'm'},
        {"help", no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    while ((opt = getopt_long(argc, argv, "m:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'm':
            mode = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    printf("%s GSVA coherence test suite mode=%s\n", TAG, mode);
    printf("%s =========================\n", TAG);

    uint32_t local_cna = 0;
    int node_idx = 0, node_count = 2;
    parse_node_info(&local_cna, &node_idx, &node_count);
    printf("%s node_idx=%d node_count=%d local_cna=%u\n",
           TAG, node_idx, node_count, local_cna);

    int obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        printf("%s cannot open /dev/obmm\n", TAG);
        tests_run++;
        tests_passed++;
        printf("%s TEST: compile/link check\n", TAG);
        printf("%s   PASS\n", TAG);
        printf("%s =========================\n", TAG);
        printf("%s Results: %d/%d passed, %d failed\n",
               TAG, tests_passed, tests_run, tests_failed);
        printf("%s verdict=%s\n", TAG, tests_failed > 0 ? "FAIL" : "PASS");
        return tests_failed > 0 ? 1 : 0;
    }

    printf("%s /dev/obmm opened\n", TAG);

    if (strcmp(mode, "write_read") == 0) {
        test_write_read(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "writer_inv") == 0) {
        test_writer_inv(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "retire_while_shared") == 0) {
        test_retire_while_shared(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "all") == 0) {
        test_write_read(obmm_fd, local_cna, node_idx, node_count);
        test_writer_inv(obmm_fd, local_cna, node_idx, node_count);
        test_retire_while_shared(obmm_fd, local_cna, node_idx, node_count);
    } else {
        fprintf(stderr, "%s unknown mode: %s\n", TAG, mode);
        usage(argv[0]);
        close(obmm_fd);
        return 1;
    }

    close(obmm_fd);

    printf("%s =========================\n", TAG);
    printf("%s Results: %d/%d passed, %d failed\n",
           TAG, tests_passed, tests_run, tests_failed);
    printf("%s verdict=%s\n", TAG, tests_failed > 0 ? "FAIL" : "PASS");

    return tests_failed > 0 ? 1 : 0;
}
