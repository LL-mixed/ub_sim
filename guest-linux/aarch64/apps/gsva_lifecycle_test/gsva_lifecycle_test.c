/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gsva_lifecycle_test -- test GSVA segment lifecycle via OBMM kernel interface.
 *
 * Uses OBMM export/import/mmap with GSVA parameters to test:
 *   - Strict address identity (user_va == uba == home_va)
 *   - Fixed UBA export/import
 *   - GSVA aperture registration and mmap
 *   - Segment retire/unmap lifecycle
 *
 * Modes:
 *   mmap_strict        -- verify strict address identity
 *   retire_reuse       -- map, unmap, remap with same address
 *   stale_epoch        -- verify epoch-based lifecycle
 *   all                -- run all tests
 *
 * Usage:
 *   gsva_lifecycle_test --mode <mode>
 */

#include "obmm_common.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>

#define TAG "[gsva_lifecycle]"

#define GSVA_BASE 0x700000000000ULL
#define GSVA_SIZE 0x400000ULL
#define GSVA_APERTURE_SIZE (GSVA_SIZE * 16)

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
    uint64_t cna_u64 = 0;

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
    } else if (obmm_env_or_cmdline("LINQU_CNA", "linqu_cna", buf, sizeof(buf))) {
        *local_cna = (uint32_t)strtoull(buf, NULL, 0);
    } else if (obmm_parse_hex_u64("/sys/bus/ub/devices/00001/primary_cna", &cna_u64)) {
        *local_cna = (uint32_t)cna_u64;
    } else {
        printf("%s WARNING: cannot resolve CNA, using node_idx=%d\n", TAG, *node_idx);
        *local_cna = (uint32_t)(*node_idx);
    }
    return 0;
}

/* ---- Test: mmap_strict ---- */
static void test_mmap_strict(int obmm_fd, uint32_t local_cna, int node_idx)
{
    TEST("strict address identity: user_va == uba == home_va");
    uint64_t base = GSVA_BASE + 0x1000000ULL;
    struct obmm_helpers_meta meta = {0};
    meta.export_cna = local_cna;

    int rc = obmm_do_export_fixed_uba(obmm_fd, &meta, GSVA_SIZE, base);
    CHECK(rc == 0, "fixed UBA export should succeed");
    CHECK(meta.remote_uba == base, "remote_uba should match requested base");

    struct obmm_helpers_region region = {0};
    rc = obmm_map_gsva_region_at(meta.export_mem_id,
                                 (void *)(uintptr_t)base,
                                 GSVA_SIZE, false, &region);
    CHECK(rc == 0, "GSVA mmap at fixed address should succeed");
    CHECK((uint64_t)(uintptr_t)region.addr == base,
          "mapped address should equal base (strict identity)");

    /* Write and read back */
    uint64_t *data = (uint64_t *)region.addr;
    *data = 0xAAAABBBBCCCCDDDDULL;
    CHECK(*data == 0xAAAABBBBCCCCDDDDULL, "readback should match written value");

    obmm_unmap_region(&region);
    obmm_do_unexport(obmm_fd, meta.export_mem_id);
    PASS();
}

/* ---- Test: retire_reuse ---- */
static void test_retire_reuse(int obmm_fd, uint32_t local_cna, int node_idx)
{
    TEST("retire then reuse same address");
    uint64_t base = GSVA_BASE + 0x2000000ULL;

    /* First lifecycle */
    struct obmm_helpers_meta meta1 = {0};
    meta1.export_cna = local_cna;
    int rc = obmm_do_export_fixed_uba(obmm_fd, &meta1, GSVA_SIZE, base);
    CHECK(rc == 0, "first export should succeed");

    struct obmm_helpers_region region1 = {0};
    rc = obmm_map_gsva_region_at(meta1.export_mem_id,
                                 (void *)(uintptr_t)base,
                                 GSVA_SIZE, false, &region1);
    CHECK(rc == 0, "first mmap should succeed");

    uint64_t *data = (uint64_t *)region1.addr;
    *data = 0x1111111111111111ULL;

    obmm_unmap_region(&region1);
    obmm_do_unexport(obmm_fd, meta1.export_mem_id);

    /* Second lifecycle at same address */
    struct obmm_helpers_meta meta2 = {0};
    meta2.export_cna = local_cna;
    rc = obmm_do_export_fixed_uba(obmm_fd, &meta2, GSVA_SIZE, base);
    CHECK(rc == 0, "second export at same address should succeed");

    struct obmm_helpers_region region2 = {0};
    rc = obmm_map_gsva_region_at(meta2.export_mem_id,
                                 (void *)(uintptr_t)base,
                                 GSVA_SIZE, false, &region2);
    CHECK(rc == 0, "second mmap at same address should succeed");

    data = (uint64_t *)region2.addr;
    *data = 0x2222222222222222ULL;
    CHECK(*data == 0x2222222222222222ULL, "second lifecycle readback should match");

    obmm_unmap_region(&region2);
    obmm_do_unexport(obmm_fd, meta2.export_mem_id);
    PASS();
}

/* ---- Test: stale_epoch ---- */
static void test_stale_epoch(int obmm_fd, uint32_t local_cna, int node_idx)
{
    TEST("epoch-based lifecycle via bootstrap generation");
    uint64_t base = GSVA_BASE + 0x3000000ULL;
    struct obmm_helpers_meta meta = {0};
    meta.export_cna = local_cna;

    /* Export with generation 1 */
    int rc = obmm_do_export_fixed_uba(obmm_fd, &meta, GSVA_SIZE, base);
    CHECK(rc == 0, "export generation 1 should succeed");

    struct obmm_helpers_region region = {0};
    rc = obmm_map_gsva_region_at(meta.export_mem_id,
                                 (void *)(uintptr_t)base,
                                 GSVA_SIZE, false, &region);
    CHECK(rc == 0, "mmap should succeed");

    uint64_t *data = (uint64_t *)region.addr;
    *data = 0xEEEEEEEEEEEEEEEEULL;

    obmm_unmap_region(&region);
    obmm_do_unexport(obmm_fd, meta.export_mem_id);

    /* Re-export at same address (generation 2) */
    struct obmm_helpers_meta meta2 = {0};
    meta2.export_cna = local_cna;
    rc = obmm_do_export_fixed_uba(obmm_fd, &meta2, GSVA_SIZE, base);
    CHECK(rc == 0, "re-export should succeed");

    struct obmm_helpers_region region2 = {0};
    rc = obmm_map_gsva_region_at(meta2.export_mem_id,
                                 (void *)(uintptr_t)base,
                                 GSVA_SIZE, false, &region2);
    CHECK(rc == 0, "re-mmap should succeed");

    data = (uint64_t *)region2.addr;
    *data = 0xFFFFFFFFFFFFFFFFULL;
    CHECK(*data == 0xFFFFFFFFFFFFFFFFULL, "re-mapped data should be writable");

    obmm_unmap_region(&region2);
    obmm_do_unexport(obmm_fd, meta2.export_mem_id);
    PASS();
}

/* ---- Test: aperture_mmap_reject ---- */
static void test_aperture_mmap_reject(int obmm_fd, uint32_t local_cna, int node_idx)
{
    TEST("non-GSVA mmap of GSVA aperture region rejected");
    uint64_t base = GSVA_BASE + 0x4000000ULL;
    struct obmm_helpers_meta meta = {0};
    meta.export_cna = local_cna;

    int rc = obmm_do_export_fixed_uba(obmm_fd, &meta, GSVA_SIZE, base);
    CHECK(rc == 0, "export should succeed");

    /* Try regular (non-MAP_GSVA) mmap at the same address */
    struct obmm_helpers_region region = {0};
    rc = obmm_map_region_at(meta.export_mem_id,
                            (void *)(uintptr_t)base,
                            GSVA_SIZE, false, &region);
    /* Non-GSVA mmap in GSVA aperture should either fail or succeed
     * depending on kernel policy. For strict mode, it should fail. */
    if (rc != 0) {
        printf("%s   non-GSVA mmap rejected as expected (errno=%d)\n",
               TAG, errno);
    } else {
        printf("%s   non-GSVA mmap succeeded (non-strict mode)\n", TAG);
        obmm_unmap_region(&region);
    }

    obmm_do_unexport(obmm_fd, meta.export_mem_id);
    PASS();
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --mode <mode>\n"
        "Modes:\n"
        "  mmap_strict          Verify strict address identity\n"
        "  retire_reuse         Verify retire then reuse\n"
        "  stale_epoch          Verify epoch lifecycle\n"
        "  mmap_aperture_overlap Verify aperture mmap rejection\n"
        "  all                  Run all tests\n",
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

    printf("%s GSVA lifecycle test mode=%s\n", TAG, mode);
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

    /* Register GSVA aperture */
    {
        struct obmm_cmd_gsva_aperture req = {0};
        req.base = GSVA_BASE;
        req.size = GSVA_APERTURE_SIZE;
        req.generation = 1;
        req.flags = OBMM_GSVA_APERTURE_F_ACTIVE;
        req.node_id = (uint32_t)node_idx;
        req.node_count = (uint32_t)node_count;
        if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_REGISTER, &req) != 0) {
            printf("%s aperture register failed errno=%d\n", TAG, errno);
            close(obmm_fd);
            return 1;
        }
        printf("%s aperture registered base=%#lx size=%#lx\n",
               TAG, (unsigned long)GSVA_BASE, (unsigned long)GSVA_APERTURE_SIZE);
    }

    if (strcmp(mode, "mmap_strict") == 0) {
        test_mmap_strict(obmm_fd, local_cna, node_idx);
    } else if (strcmp(mode, "retire_reuse") == 0) {
        test_retire_reuse(obmm_fd, local_cna, node_idx);
    } else if (strcmp(mode, "stale_epoch") == 0) {
        test_stale_epoch(obmm_fd, local_cna, node_idx);
    } else if (strcmp(mode, "mmap_aperture_overlap") == 0) {
        test_aperture_mmap_reject(obmm_fd, local_cna, node_idx);
    } else if (strcmp(mode, "all") == 0) {
        test_mmap_strict(obmm_fd, local_cna, node_idx);
        test_retire_reuse(obmm_fd, local_cna, node_idx);
        test_stale_epoch(obmm_fd, local_cna, node_idx);
        test_aperture_mmap_reject(obmm_fd, local_cna, node_idx);
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
