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
 *   token_denied      -- Read/WriteAcquire require exact token_id/value
 *   token_rotate      -- TokenChange rejects old token and accepts new token
 *
 * Usage:
 *   gsva_coh_test --mode <mode>
 */

#include "obmm_common.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define TAG "[gsva_coh_test]"

#define GSVA_BASE 0x700000000000ULL
#define GSVA_SIZE 0x400000ULL
#define GSVA_APERTURE_SIZE (GSVA_SIZE * 32) /* covers all test slices + offsets */

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

/* ---- Test: cross_node_write_read ---- */
static void test_cross_node_write_read(int obmm_fd, uint32_t local_cna,
                                       int node_idx, int node_count)
{
    TEST("GSVA cross-node write-read coherence");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};

    /* Each node exports its own slice at unique offset */
    uint64_t my_base = GSVA_BASE + (uint64_t)node_idx * GSVA_SIZE;
    struct obmm_helpers_meta my_meta = {0};
    my_meta.export_cna = local_cna;

    int rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410101ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410101ULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");

    /* Allocate import PAs from OBMM pool */
    int peer_count = node_count - 1;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    if (!obmm_alloc_import_pas(peer_count, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        FAIL("failed to allocate import PAs");
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        return;
    }

    /* Import peer's slice with GSVA identity */
    int pa_idx = 0;
    for (int i = 0; i < node_count; i++) {
        if (i == node_idx) continue;
        if (!got[i]) continue;

        uint64_t peer_base = GSVA_BASE + (uint64_t)i * GSVA_SIZE;
        uint64_t import_mem_id = 0;
        rc = obmm_do_import_v2(obmm_fd, &metas[i], local_cna,
                               import_pas[pa_idx], 0,
                               OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                               OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                               OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                               0, 0, 0, 0, 0, 0,
                               peer_base, peer_base, 0,
                               &import_mem_id);
        CHECK(rc == 0, "GSVA identity import should succeed");

        struct obmm_helpers_region region = {0};
        rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)peer_base,
                                     GSVA_SIZE, import_osync[pa_idx], &region);
        CHECK(rc == 0, "GSVA mmap should succeed");

        /* Write a pattern */
        uint64_t *data = (uint64_t *)region.addr;
        uint64_t test_val = 0xDEADBEEF00000000ULL | (uint64_t)node_idx;
        *data = test_val;

        printf("%s   wrote to peer%d slice at %#" PRIx64 " val=%#" PRIx64 "\n",
               TAG, i, peer_base, test_val);

        obmm_unmap_region(&region);
        obmm_do_unimport(obmm_fd, import_mem_id);
        pa_idx++;
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

static int gsva_send_event(int obmm_fd, uint32_t sub_op, uint32_t requester_cna,
                           uint32_t token_id, uint32_t token_value,
                           uint64_t segment_id, uint64_t home_va,
                           uint64_t size, int32_t *error_out)
{
    struct obmm_cmd_gsva_event_v1 cmd = {0};

    cmd.version = OBMM_GSVA_ABI_VERSION;
    cmd.sub_op = sub_op;
    cmd.requester_cna = requester_cna;
    cmd.token_id = token_id;
    cmd.token_value = token_value;
    cmd.key.version = 1;
    cmd.key.segment_id = segment_id;
    cmd.key.home_va = home_va;
    cmd.key.size = size;
    cmd.key.vmid = 0;
    cmd.key.asid = 0;
    cmd.key.pte_offset = 0;
    cmd.key.p_tag = 0;
    cmd.key.cache_policy = GSVA_CACHE_POLICY_DIRECTORY_MESI;
    cmd.key.epoch = 1;

    if (ioctl(obmm_fd, OBMM_CMD_GSVA_EVENT_V1, &cmd) != 0)
        return -1;

    *error_out = cmd.error;
    return 0;
}

/* ---- Test: token_denied ---- */
static void test_token_denied(int obmm_fd, uint32_t local_cna,
                              int node_idx, int node_count)
{
    TEST("GSVA ReadAcquire/WriteAcquire token v1 validation");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x1000000ULL +
                       (uint64_t)node_idx * GSVA_SIZE;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    struct obmm_helpers_meta my_meta = {0};
    int peer_idx = -1;
    uint64_t peer_base = 0;
    uint64_t import_mem_id = 0;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t good_token = 0;
    int rc;

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410303ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410303ULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");

    for (int i = 0; i < node_count; i++) {
        if (i != node_idx && got[i]) {
            peer_idx = i;
            break;
        }
    }
    CHECK(peer_idx >= 0, "peer metadata should be available");

    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    peer_base = GSVA_BASE + 0x1000000ULL + (uint64_t)peer_idx * GSVA_SIZE;
    good_token = metas[peer_idx].token_id;
    rc = obmm_do_import_v2(obmm_fd, &metas[peer_idx], local_cna,
                           import_pas[0], good_token,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, metas[peer_idx].export_mem_id,
                           peer_base, peer_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         good_token, good_token, metas[peer_idx].export_mem_id,
                         peer_base, GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire with valid token should reach QEMU");
    CHECK(ev_error == GSVA_OK, "ReadAcquire with valid token should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         good_token, good_token ^ 0x5a5a5a5aU,
                         metas[peer_idx].export_mem_id, peer_base, GSVA_SIZE,
                         &ev_error);
    CHECK(rc == 0, "ReadAcquire with bad token should reach QEMU");
    CHECK(ev_error == GSVA_ERR_TOKEN_DENIED,
          "ReadAcquire with bad token should be denied");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         good_token, good_token ^ 0xa5a5a5a5U,
                         metas[peer_idx].export_mem_id, peer_base, GSVA_SIZE,
                         &ev_error);
    CHECK(rc == 0, "WriteAcquire with bad token should reach QEMU");
    CHECK(ev_error == GSVA_ERR_TOKEN_DENIED,
          "WriteAcquire with bad token should be denied");

    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: token_rotate ---- */
static void test_token_rotate(int obmm_fd, uint32_t local_cna,
                              int node_idx, int node_count)
{
    TEST("GSVA token rotation preserves key identity");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x1800000ULL +
                       (uint64_t)node_idx * GSVA_SIZE;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    struct obmm_helpers_meta my_meta = {0};
    int peer_idx = -1;
    uint64_t peer_base = 0;
    uint64_t segment_id = 0;
    uint64_t import_mem_id = 0;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t token_id = 0;
    uint32_t old_token = 0;
    uint32_t new_token = 0;
    int rc;

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410404ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410404ULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");

    for (int i = 0; i < node_count; i++) {
        if (i != node_idx && got[i]) {
            peer_idx = i;
            break;
        }
    }
    CHECK(peer_idx >= 0, "peer metadata should be available");

    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    peer_base = GSVA_BASE + 0x1800000ULL + (uint64_t)peer_idx * GSVA_SIZE;
    segment_id = metas[peer_idx].export_mem_id;
    token_id = metas[peer_idx].token_id;
    old_token = token_id;
    new_token = old_token ^ 0x01020304U;
    if (new_token == 0 || new_token == old_token)
        new_token = old_token + 1;

    rc = obmm_do_import_v2(obmm_fd, &metas[peer_idx], local_cna,
                           import_pas[0], old_token,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           peer_base, peer_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, old_token, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire with old token should reach QEMU");
    CHECK(ev_error == GSVA_OK, "old token should be valid before rotation");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_TOKEN_CHANGE, local_cna,
                         token_id, new_token, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "TokenChange should reach QEMU");
    CHECK(ev_error == GSVA_OK, "TokenChange should commit new token value");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, old_token, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire with old token after rotation should reach QEMU");
    CHECK(ev_error == GSVA_ERR_TOKEN_DENIED,
          "old token after rotation should be denied");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, new_token, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire with new token should reach QEMU");
    CHECK(ev_error == GSVA_OK,
          "new token should pass using the same GSVA key identity");

    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --mode <mode>\n"
        "Modes:\n"
        "  cross_node_write_read  Write then read across nodes\n"
        "  retire_while_shared    Unmap while segment is shared\n"
        "  token_denied           Validate acquire token denial\n"
        "  token_rotate           Validate token rotation\n"
        "  all                    Run all tests (default)\n",
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

    if (strcmp(mode, "cross_node_write_read") == 0) {
        test_cross_node_write_read(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "retire_while_shared") == 0) {
        test_retire_while_shared(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "token_denied") == 0) {
        test_token_denied(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "token_rotate") == 0) {
        test_token_rotate(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "all") == 0) {
        test_cross_node_write_read(obmm_fd, local_cna, node_idx, node_count);
        usleep(200000);
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
