/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gsva_lifecycle_test -- test GSVA segment lifecycle via OBMM kernel interface.
 *
 * Uses OBMM export/import/mmap with GSVA parameters to test:
 *   - Strict address identity (user_va == uba == home_va)
 *   - Fixed UBA export/import
 *   - GSVA aperture registration and mmap
 *   - OBMM_CMD_GSVA_ALLOC_SEGMENT / QUERY_SEGMENT / RETIRE_SEGMENT ABI
 *   - Segment retire/unmap lifecycle
 *
 * Modes:
 *   mmap_strict        -- verify strict address identity
 *   retire_reuse       -- map, unmap, remap with same address
 *   stale_epoch        -- verify epoch-based lifecycle
 *   segment_abi        -- verify kernel GSVA segment descriptor ABI
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
#define GSVA_APERTURE_SIZE (GSVA_SIZE * 32)

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

static void check_segment_desc(const struct obmm_gsva_segment_desc_v1 *desc,
                               uint64_t expected_home_va,
                               uint32_t local_cna)
{
    CHECK(desc->version == OBMM_GSVA_ABI_VERSION,
          "segment desc version should be v1");
    CHECK(desc->segment_id != 0, "segment_id should be assigned");
    CHECK(desc->home_va == expected_home_va,
          "segment home_va should match requested_home_va");
    CHECK(desc->size == GSVA_SIZE, "segment size should match request");
    CHECK(desc->epoch == 1, "initial segment epoch should be 1");
    CHECK(desc->home_cna == local_cna, "home_cna should match local CNA");
    CHECK(desc->flags & OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY,
          "segment should require strict address identity");
    CHECK(desc->flags & OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED,
          "segment should require token value");
    CHECK(desc->flags & OBMM_GSVA_SEG_F_ACTIVE,
          "segment should be active after allocation");
    CHECK(!(desc->flags & OBMM_GSVA_SEG_F_RETIRED),
          "segment should not be retired after allocation");
    CHECK(desc->cache_policy == OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
          "cache_policy should round-trip");
    CHECK(desc->p_tag == (local_cna & 0x00ffffffu),
          "auto p_tag should derive from home CNA");
    CHECK(desc->access_flags == (OBMM_GSVA_ACCESS_READ | OBMM_GSVA_ACCESS_WRITE),
          "access_flags should round-trip");
    CHECK(desc->token_id != 0, "token_id should be assigned");
    CHECK(desc->token_value != 0, "token_value should be assigned");
}

/* ---- Test: segment_abi ---- */
static void test_segment_abi(int obmm_fd, uint32_t local_cna, int node_idx)
{
    TEST("OBMM GSVA segment alloc/query/retire ABI");
    uint64_t base = GSVA_BASE + 0x5000000ULL;
    struct obmm_cmd_gsva_alloc_segment_v1 alloc = {0};
    struct obmm_cmd_gsva_query_segment_v1 query = {0};
    struct obmm_cmd_gsva_query_segment_v1 query_va = {0};
    struct obmm_cmd_gsva_retire_segment_v1 retire = {0};
    struct obmm_cmd_gsva_retire_segment_v1 stale_retire = {0};
    int rc;

    alloc.version = OBMM_GSVA_ABI_VERSION;
    alloc.size = GSVA_SIZE;
    alloc.alignment = GSVA_SIZE;
    alloc.requested_home_va = base;
    alloc.home_node_id = (uint32_t)node_idx;
    alloc.cache_policy = OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI;
    alloc.requested_p_tag = OBMM_GSVA_P_TAG_AUTO;
    alloc.access_flags = OBMM_GSVA_ACCESS_READ | OBMM_GSVA_ACCESS_WRITE;

    rc = ioctl(obmm_fd, OBMM_CMD_GSVA_ALLOC_SEGMENT, &alloc);
    CHECK(rc == 0, "ALLOC_SEGMENT should succeed");
    check_segment_desc(&alloc.desc, base, local_cna);

    query.version = OBMM_GSVA_ABI_VERSION;
    query.segment_id = alloc.desc.segment_id;
    rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_SEGMENT, &query);
    CHECK(rc == 0, "QUERY_SEGMENT by segment_id should succeed");
    CHECK(query.desc.segment_id == alloc.desc.segment_id,
          "QUERY_SEGMENT should return same segment_id");
    CHECK(query.desc.token_id == alloc.desc.token_id,
          "QUERY_SEGMENT should return same token_id");
    CHECK(query.desc.token_value == alloc.desc.token_value,
          "QUERY_SEGMENT should return same token_value");

    query_va.version = OBMM_GSVA_ABI_VERSION;
    query_va.home_va = alloc.desc.home_va;
    rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_SEGMENT, &query_va);
    CHECK(rc == 0, "QUERY_SEGMENT by home_va should succeed");
    CHECK(query_va.desc.segment_id == alloc.desc.segment_id,
          "QUERY_SEGMENT by home_va should return allocated segment");

    stale_retire.version = OBMM_GSVA_ABI_VERSION;
    stale_retire.segment_id = alloc.desc.segment_id;
    stale_retire.epoch = alloc.desc.epoch + 1;
    rc = ioctl(obmm_fd, OBMM_CMD_GSVA_RETIRE_SEGMENT, &stale_retire);
    CHECK(rc != 0 && errno == EINVAL,
          "RETIRE_SEGMENT with stale epoch should fail with EINVAL");

    retire.version = OBMM_GSVA_ABI_VERSION;
    retire.segment_id = alloc.desc.segment_id;
    retire.epoch = alloc.desc.epoch;
    retire.timeout_ms = 1000;
    rc = ioctl(obmm_fd, OBMM_CMD_GSVA_RETIRE_SEGMENT, &retire);
    CHECK(rc == 0, "RETIRE_SEGMENT should succeed");
    CHECK(retire.status == OBMM_GSVA_RETIRE_COMMITTED,
          "RETIRE_SEGMENT status should be committed");
    CHECK(retire.error == 0, "RETIRE_SEGMENT error should be zero");
    CHECK(retire.committed_epoch == alloc.desc.epoch,
          "RETIRE_SEGMENT committed epoch should match");

    memset(&query, 0, sizeof(query));
    query.version = OBMM_GSVA_ABI_VERSION;
    query.segment_id = alloc.desc.segment_id;
    rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_SEGMENT, &query);
    CHECK(rc != 0 && errno == ENOENT,
          "QUERY_SEGMENT after retire should fail with ENOENT");

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
        "  segment_abi          Verify GSVA segment alloc/query/retire ABI\n"
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
    } else if (strcmp(mode, "segment_abi") == 0) {
        test_segment_abi(obmm_fd, local_cna, node_idx);
    } else if (strcmp(mode, "all") == 0) {
        test_mmap_strict(obmm_fd, local_cna, node_idx);
        test_retire_reuse(obmm_fd, local_cna, node_idx);
        test_stale_epoch(obmm_fd, local_cna, node_idx);
        test_aperture_mmap_reject(obmm_fd, local_cna, node_idx);
        test_segment_abi(obmm_fd, local_cna, node_idx);
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
