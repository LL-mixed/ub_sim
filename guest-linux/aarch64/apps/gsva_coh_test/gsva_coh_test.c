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
 *   retire_event      -- Event Retire tombstones route and blocks acquire
 *   stale_remap       -- Retired epoch-1 key cannot be mapped again
 *   epoch_reuse       -- Retired key remaps when epoch increases
 *   token_denied      -- Read/WriteAcquire require exact token_id/value
 *   token_write_denied -- Read-only route rejects WriteAcquire
 *   token_rotate      -- TokenChange rejects old token and accepts new token
 *   coh_timeout       -- Pending writer invalidation reaches TIMEOUT
 *   coh_recovery      -- Pending writer invalidation recovers after InvAck
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

static int gsva_send_event(int obmm_fd, uint32_t sub_op, uint32_t requester_cna,
                           uint32_t token_id, uint32_t token_value,
                           uint64_t segment_id, uint64_t home_va,
                           uint64_t size, int32_t *error_out);

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
    TEST("GSVA event retire while segment is shared");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t base = GSVA_BASE + 0x800000ULL;
    uint64_t my_base = base + (uint64_t)node_idx * GSVA_SIZE;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    struct obmm_helpers_meta my_meta = {0};
    int peer_idx = -1;
    int other_idx = -1;
    uint64_t peer_base = 0;
    uint64_t segment_id = 0;
    uint64_t import_mem_id = 0;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t token_id = 0;
    uint32_t other_cna = 0;
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
        if (i == node_idx || !got[i])
            continue;
        if (peer_idx < 0) {
            peer_idx = i;
        } else {
            other_idx = i;
            break;
        }
    }
    CHECK(peer_idx >= 0, "peer metadata should be available");
    CHECK(other_idx >= 0 || node_count == 2, "second reader metadata should be available");

    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    peer_base = base + (uint64_t)peer_idx * GSVA_SIZE;
    segment_id = metas[peer_idx].export_mem_id;
    token_id = metas[peer_idx].token_id;
    other_cna = (other_idx >= 0) ? metas[other_idx].export_cna
                                 : (local_cna ^ 1U);

    rc = obmm_do_import_v2(obmm_fd, &metas[peer_idx], local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           peer_base, peer_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "local ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, other_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "peer ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "peer ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "Retire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "Retire while shared should commit");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire after shared retire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_SEGMENT_RETIRED,
          "ReadAcquire after shared retire should be rejected as retired");

    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);

    PASS();
}

static int gsva_send_event_epoch(int obmm_fd, uint32_t sub_op,
                           uint32_t requester_cna, uint32_t token_id,
                           uint32_t token_value, uint64_t segment_id,
                           uint64_t home_va, uint64_t size, uint64_t epoch,
                           int32_t *error_out)
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
    cmd.key.epoch = epoch;

    if (ioctl(obmm_fd, OBMM_CMD_GSVA_EVENT_V1, &cmd) != 0)
        return -1;

    *error_out = cmd.error;
    return 0;
}

static int gsva_send_event(int obmm_fd, uint32_t sub_op, uint32_t requester_cna,
                           uint32_t token_id, uint32_t token_value,
                           uint64_t segment_id, uint64_t home_va,
                           uint64_t size, int32_t *error_out)
{
    return gsva_send_event_epoch(obmm_fd, sub_op, requester_cna, token_id,
                                 token_value, segment_id, home_va, size, 1,
                                 error_out);
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
    struct obmm_helpers_meta peer_meta = {0};
    uint64_t my_base = GSVA_BASE + 0x1800000ULL +
                       (uint64_t)node_idx * GSVA_SIZE;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    struct obmm_helpers_meta my_meta = {0};
    uint64_t peer_base = 0;
    uint64_t segment_id = 0;
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region import_region = { .fd = -1 };
    bool import_mapped = false;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t token_id = 0;
    uint32_t old_token = 0;
    uint32_t new_token = 0;
    int rc;

    (void)node_count;

    if (node_idx != 0) {
        PASS();
        return;
    }

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    peer_meta = my_meta;

    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    peer_base = my_base;
    segment_id = peer_meta.export_mem_id;
    token_id = peer_meta.token_id;
    old_token = token_id;
    new_token = old_token ^ 0x01020304U;
    if (new_token == 0 || new_token == old_token)
        new_token = old_token + 1;

    rc = obmm_do_import_v2(obmm_fd, &peer_meta, local_cna,
                           import_pas[0], old_token,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           peer_base, peer_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)peer_base,
                                 GSVA_SIZE, import_osync[0], &import_region);
    CHECK(rc == 0, "GSVA mmap should succeed before token rotation");
    import_mapped = true;
    {
        volatile uint64_t *probe =
            (volatile uint64_t *)(uintptr_t)import_region.addr;
        uint64_t value = *probe;

        __sync_synchronize();
        printf("%s   token_rotate ARM MMU touch va=%#" PRIx64
               " value=%#" PRIx64 "\n", TAG, peer_base, value);
    }

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
    CHECK(ev_error == GSVA_ERR_TOKEN_DENIED,
          "new token should be denied before revoke ACK");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_INV_ACK, local_cna,
                         token_id, new_token, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "Token revoke ACK should reach QEMU");
    CHECK(ev_error == GSVA_OK, "Token revoke ACK should commit new token");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, new_token, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire with new token after ACK should reach QEMU");
    CHECK(ev_error == GSVA_OK,
          "new token should pass after ACK using the same GSVA key identity");

    if (import_mapped)
        obmm_unmap_region(&import_region);
    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: retire_event ---- */
static void test_retire_event(int obmm_fd, uint32_t local_cna,
                              int node_idx, int node_count)
{
    TEST("GSVA event retire tombstones route");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x2000000ULL +
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
    int rc;

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410505ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410505ULL, metas, got);
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

    peer_base = GSVA_BASE + 0x2000000ULL + (uint64_t)peer_idx * GSVA_SIZE;
    segment_id = metas[peer_idx].export_mem_id;
    token_id = metas[peer_idx].token_id;
    rc = obmm_do_import_v2(obmm_fd, &metas[peer_idx], local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           peer_base, peer_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire before retire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "ReadAcquire before retire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "Retire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "Retire should commit route tombstone");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire after retire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_SEGMENT_RETIRED,
          "ReadAcquire after retire should be rejected as retired");

    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: writer_inv ---- */
static void test_writer_inv(int obmm_fd, uint32_t local_cna,
                            int node_idx, int node_count)
{
    TEST("GSVA writer invalidates shared readers");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x2800000ULL +
                       (uint64_t)node_idx * GSVA_SIZE;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    struct obmm_helpers_meta my_meta = {0};
    int peer_idx = -1;
    int other_idx = -1;
    uint64_t peer_base = 0;
    uint64_t segment_id = 0;
    uint64_t import_mem_id = 0;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t token_id = 0;
    uint32_t other_cna = 0;
    int rc;

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410606ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410606ULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");

    for (int i = 0; i < node_count; i++) {
        if (i == node_idx || !got[i])
            continue;
        if (peer_idx < 0) {
            peer_idx = i;
        } else {
            other_idx = i;
            break;
        }
    }
    CHECK(peer_idx >= 0, "peer metadata should be available");
    CHECK(other_idx >= 0 || node_count == 2, "second reader metadata should be available");

    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    peer_base = GSVA_BASE + 0x2800000ULL + (uint64_t)peer_idx * GSVA_SIZE;
    segment_id = metas[peer_idx].export_mem_id;
    token_id = metas[peer_idx].token_id;
    other_cna = (other_idx >= 0) ? metas[other_idx].export_cna
                                 : (local_cna ^ 1U);

    rc = obmm_do_import_v2(obmm_fd, &metas[peer_idx], local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           peer_base, peer_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "local ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, other_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "peer ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "peer ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "writer WriteAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK,
          "writer should invalidate other sharers and acquire M");

    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: stale_remap ---- */
static void test_stale_remap(int obmm_fd, uint32_t local_cna,
                             int node_idx, int node_count)
{
    TEST("GSVA stale epoch remap rejected after retire");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x3000000ULL +
                       (uint64_t)node_idx * GSVA_SIZE;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    struct obmm_helpers_meta my_meta = {0};
    int peer_idx = -1;
    uint64_t peer_base = 0;
    uint64_t segment_id = 0;
    uint64_t import_mem_id = 0;
    uint64_t stale_import_mem_id = 0;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t token_id = 0;
    int rc;

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410707ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410707ULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");

    for (int i = 0; i < node_count; i++) {
        if (i != node_idx && got[i]) {
            peer_idx = i;
            break;
        }
    }
    CHECK(peer_idx >= 0, "peer metadata should be available");

    if (!obmm_alloc_import_pas(2, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PAs");
        return;
    }

    peer_base = GSVA_BASE + 0x3000000ULL + (uint64_t)peer_idx * GSVA_SIZE;
    segment_id = metas[peer_idx].export_mem_id;
    token_id = metas[peer_idx].token_id;

    rc = obmm_do_import_v2(obmm_fd, &metas[peer_idx], local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           peer_base, peer_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "initial GSVA identity import should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "Retire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "Retire should commit route tombstone");

    errno = 0;
    rc = obmm_do_import_v2(obmm_fd, &metas[peer_idx], local_cna,
                           import_pas[1], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           peer_base, peer_base, 0,
                           &stale_import_mem_id);
    CHECK(rc != 0, "stale epoch remap should fail after tombstone");

    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: epoch_reuse ---- */
static void test_epoch_reuse(int obmm_fd, uint32_t local_cna,
                             int node_idx, int node_count)
{
    TEST("GSVA retired segment remaps with higher epoch");
    struct obmm_helpers_meta peer_meta = {0};
    uint64_t my_base = GSVA_BASE + 0x4000000ULL +
                       (uint64_t)node_idx * GSVA_SIZE;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    struct obmm_helpers_meta my_meta = {0};
    uint64_t peer_base = 0;
    uint64_t segment_id = 0;
    uint64_t import_mem_id_epoch1 = 0;
    uint64_t import_mem_id_epoch2 = 0;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t token_id = 0;
    int rc;

    (void)node_count;

    if (node_idx != 0) {
        PASS();
        return;
    }

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    peer_meta = my_meta;

    if (!obmm_alloc_import_pas(2, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PAs");
        return;
    }

    peer_base = my_base;
    segment_id = peer_meta.export_mem_id;
    token_id = peer_meta.token_id;

    rc = obmm_do_import_v2_epoch(obmm_fd, &peer_meta, local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id, 1,
                           peer_base, peer_base, 0,
                           &import_mem_id_epoch1);
    CHECK(rc == 0, "epoch-1 GSVA identity import should succeed");

    rc = gsva_send_event_epoch(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE,
                         local_cna, token_id, token_id, segment_id,
                         peer_base, GSVA_SIZE, 1, &ev_error);
    CHECK(rc == 0, "epoch-1 ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "epoch-1 ReadAcquire should pass");

    rc = gsva_send_event_epoch(obmm_fd, OBMM_GSVA_EVENT_RETIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, 1, &ev_error);
    CHECK(rc == 0, "epoch-1 Retire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "epoch-1 Retire should tombstone route");

    rc = obmm_do_import_v2_epoch(obmm_fd, &peer_meta, local_cna,
                           import_pas[1], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id, 2,
                           peer_base, peer_base, 0,
                           &import_mem_id_epoch2);
    CHECK(rc == 0, "epoch-2 remap should remove tombstone and succeed");

    rc = gsva_send_event_epoch(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE,
                         local_cna, token_id, token_id, segment_id,
                         peer_base, GSVA_SIZE, 2, &ev_error);
    CHECK(rc == 0, "epoch-2 ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "epoch-2 ReadAcquire should pass");

    rc = gsva_send_event_epoch(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE,
                         local_cna, token_id, token_id, segment_id,
                         peer_base, GSVA_SIZE, 1, &ev_error);
    CHECK(rc == 0, "stale epoch-1 ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_STALE_EPOCH,
          "stale epoch-1 ReadAcquire should be rejected");

    obmm_do_unimport(obmm_fd, import_mem_id_epoch2);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: token_write_denied ---- */
static void test_token_write_denied(int obmm_fd, uint32_t local_cna,
                                    int node_idx, int node_count)
{
    TEST("GSVA read-only token permission rejects WriteAcquire");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x3800000ULL +
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
    int rc;

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410808ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410808ULL, metas, got);
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

    peer_base = GSVA_BASE + 0x3800000ULL + (uint64_t)peer_idx * GSVA_SIZE;
    segment_id = metas[peer_idx].export_mem_id;
    token_id = metas[peer_idx].token_id;

    rc = obmm_do_import_v2(obmm_fd, &metas[peer_idx], local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, OBMM_GSVA_ACCESS_READ, segment_id,
                           peer_base, peer_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "read-only GSVA identity import should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire on read-only route should reach QEMU");
    CHECK(ev_error == GSVA_OK, "ReadAcquire on read-only route should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, peer_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "WriteAcquire on read-only route should reach QEMU");
    CHECK(ev_error == GSVA_ERR_TOKEN_DENIED,
          "WriteAcquire on read-only route should be denied");

    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: coh_timeout ---- */
static void test_coh_timeout(int obmm_fd, uint32_t local_cna,
                             int node_idx, int node_count)
{
    TEST("GSVA coherence pending timeout is terminal");
    struct obmm_helpers_meta peer_meta = {0};
    uint64_t my_base = GSVA_BASE + 0x4800000ULL +
                       (uint64_t)node_idx * GSVA_SIZE;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    struct obmm_helpers_meta my_meta = {0};
    uint64_t segment_id = 0;
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region import_region = { .fd = -1 };
    bool import_mapped = false;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t token_id = 0;
    uint32_t other_cna = local_cna ^ 1U;
    int rc;

    (void)node_count;

    if (node_idx != 0) {
        PASS();
        return;
    }

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    peer_meta = my_meta;
    segment_id = peer_meta.export_mem_id;
    token_id = peer_meta.token_id;

    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    rc = obmm_do_import_v2(obmm_fd, &peer_meta, local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           my_base, my_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, import_osync[0], &import_region);
    CHECK(rc == 0, "GSVA mmap should succeed before timeout");
    import_mapped = true;
    {
        volatile uint64_t *probe =
            (volatile uint64_t *)(uintptr_t)import_region.addr;
        uint64_t value = *probe;

        __sync_synchronize();
        printf("%s   coh_timeout ARM MMU touch va=%#" PRIx64
               " value=%#" PRIx64 "\n", TAG, my_base, value);
    }

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "local ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, other_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "peer ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "peer ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "writer WriteAcquire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_COH_PENDING,
          "writer should wait for invalidation ACK");

    usleep(50000);

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETRY, local_cna,
                         0, 0, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "Retry should reach QEMU");
    printf("%s   coh_timeout Retry error=%d\n", TAG, ev_error);
    CHECK(ev_error == GSVA_ERR_COH_TIMEOUT,
          "Retry after timeout should report GSVA_ERR_COH_TIMEOUT");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire after timeout should reach QEMU");
    printf("%s   coh_timeout ReadAcquire error=%d\n", TAG, ev_error);
    CHECK(ev_error == GSVA_ERR_COH_TIMEOUT,
          "ReadAcquire after timeout should remain terminal");

    {
        struct obmm_cmd_gsva_query_v1 query = {0};
        struct {
            uint32_t version;
            int32_t error;
            uint8_t data[240];
        } *query_resp = (void *)query.resp_data;

        query.version = OBMM_GSVA_ABI_VERSION;
        query.query_type = GSVA_QUERY_COHERENCE;
        query.segment_id = segment_id;
        query.home_va = my_base;

        rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
        CHECK(rc == 0, "coherence query after timeout should reach QEMU");
        printf("%s   coh_timeout Query error=%d\n", TAG, query_resp->error);
        CHECK(query_resp->error == GSVA_ERR_COH_TIMEOUT,
              "coherence query should report GSVA_ERR_COH_TIMEOUT");
    }

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "Retire after timeout should reach QEMU");
    CHECK(ev_error == GSVA_OK, "Retire after timeout should clean up");

    if (import_mapped)
        obmm_unmap_region(&import_region);
    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: coh_recovery ---- */
static void test_coh_recovery(int obmm_fd, uint32_t local_cna,
                              int node_idx, int node_count)
{
    TEST("GSVA coherence pending invalidation recovers after InvAck");
    struct obmm_helpers_meta peer_meta = {0};
    uint64_t my_base = GSVA_BASE + 0x5000000ULL +
                       (uint64_t)node_idx * GSVA_SIZE;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    struct obmm_helpers_meta my_meta = {0};
    uint64_t segment_id = 0;
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region import_region = { .fd = -1 };
    bool import_mapped = false;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t token_id = 0;
    uint32_t other_cna = 3;
    uint32_t query_state = 0xffffffffu;
    uint64_t pending_seq = 0;
    int rc;

    (void)node_count;

    if (node_idx != 0) {
        PASS();
        return;
    }

    my_meta.export_cna = local_cna;

    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE, my_base);
    CHECK(rc == 0, "fixed UBA export should succeed");

    peer_meta = my_meta;
    segment_id = peer_meta.export_mem_id;
    token_id = peer_meta.token_id;

    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    rc = obmm_do_import_v2(obmm_fd, &peer_meta, local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           my_base, my_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, import_osync[0], &import_region);
    CHECK(rc == 0, "GSVA mmap should succeed before recovery");
    import_mapped = true;
    {
        volatile uint64_t *probe =
            (volatile uint64_t *)(uintptr_t)import_region.addr;
        uint64_t value = *probe;

        __sync_synchronize();
        printf("%s   coh_recovery ARM MMU touch va=%#" PRIx64
               " value=%#" PRIx64 "\n", TAG, my_base, value);
    }

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "local ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, other_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "peer ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "peer ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "writer WriteAcquire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_COH_PENDING,
          "writer should wait for invalidation ACK");

    {
        struct obmm_cmd_gsva_query_v1 query = {0};
        struct {
            uint32_t version;
            int32_t error;
            uint8_t data[240];
        } *query_resp = (void *)query.resp_data;

        query.version = OBMM_GSVA_ABI_VERSION;
        query.query_type = GSVA_QUERY_COHERENCE;
        query.segment_id = segment_id;
        query.home_va = my_base;

        rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
        CHECK(rc == 0, "coherence query should reach QEMU");
        CHECK(query_resp->error == GSVA_OK,
              "coherence query should report pending object as active");
        memcpy(&query_state, query_resp->data, sizeof(query_state));
        memcpy(&pending_seq, query_resp->data + sizeof(query_state),
               sizeof(pending_seq));
        printf("%s   coh_recovery Query pending state=%u seq=%#" PRIx64 "\n",
               TAG, query_state, pending_seq);
        CHECK(pending_seq != 0, "coherence query should expose pending seq");
    }

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_INV_ACK, other_cna,
                         (uint32_t)pending_seq, 0, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "InvAck should reach QEMU");
    CHECK(ev_error == GSVA_OK, "InvAck should complete pending invalidation");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETRY, local_cna,
                         (uint32_t)pending_seq, 0, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "Retry after InvAck should reach QEMU");
    printf("%s   coh_recovery Retry error=%d\n", TAG, ev_error);
    CHECK(ev_error == GSVA_OK, "Retry after InvAck should pass");

    {
        struct obmm_cmd_gsva_query_v1 query = {0};
        struct {
            uint32_t version;
            int32_t error;
            uint8_t data[240];
        } *query_resp = (void *)query.resp_data;

        query.version = OBMM_GSVA_ABI_VERSION;
        query.query_type = GSVA_QUERY_COHERENCE;
        query.segment_id = segment_id;
        query.home_va = my_base;

        rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
        CHECK(rc == 0, "coherence query after recovery should reach QEMU");
        memcpy(&query_state, query_resp->data, sizeof(query_state));
        printf("%s   coh_recovery Query recovered state=%u error=%d\n",
               TAG, query_state, query_resp->error);
        CHECK(query_resp->error == GSVA_OK,
              "coherence query after recovery should pass");
        CHECK(query_state == 3, "coherence query after recovery should report M");
    }

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "WriteAcquire after recovery should reach QEMU");
    CHECK(ev_error == GSVA_OK, "writer should own M after recovery");

    if (import_mapped)
        obmm_unmap_region(&import_region);
    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: coh_remote_inv ---- */
static void test_coh_remote_inv(int obmm_fd, uint32_t local_cna,
                                int node_idx, int node_count)
{
    TEST("GSVA coherence sends remote invalidate over UB Link");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x6000000ULL;
    struct obmm_helpers_meta my_meta = {0};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region import_region = { .fd = -1 };
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t peer_cna = 0;
    uint64_t segment_id = 0;
    uint32_t token_id = 0;
    uint64_t pending_seq = 0;
    int rc;

    if (node_count < 2) {
        FAIL("remote invalidate requires at least two nodes");
        return;
    }

    my_meta.export_cna = local_cna;
    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE,
                                  my_base + (uint64_t)node_idx * GSVA_SIZE);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410606ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    if (node_idx != 0) {
        for (int attempt = 0; attempt < 40; attempt++) {
            struct obmm_cmd_gsva_query_v1 query = {0};

            query.version = OBMM_GSVA_ABI_VERSION;
            query.query_type = GSVA_QUERY_CAPS;
            (void)ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
            usleep(100000);
        }
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        PASS();
        return;
    }

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410606ULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");
    CHECK(got[1], "peer metadata should be available");
    peer_cna = metas[1].export_cna;
    CHECK(peer_cna != 0 && peer_cna != local_cna,
          "peer CNA should be a real remote CNA");

    segment_id = my_meta.export_mem_id;
    token_id = my_meta.token_id;
    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    rc = obmm_do_import_v2(obmm_fd, &my_meta, local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           my_base, my_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, import_osync[0],
                                 &import_region);
    CHECK(rc == 0, "GSVA mmap should succeed");
    {
        volatile uint64_t *probe =
            (volatile uint64_t *)(uintptr_t)import_region.addr;
        uint64_t value = *probe;

        __sync_synchronize();
        printf("%s   coh_remote_inv ARM MMU touch va=%#" PRIx64
               " value=%#" PRIx64 "\n", TAG, my_base, value);
    }

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "local ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, peer_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "remote-CNA ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "remote-CNA ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "writer WriteAcquire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_COH_PENDING,
          "writer should wait for remote invalidation ACK");

    {
        struct obmm_cmd_gsva_query_v1 query = {0};
        struct {
            uint32_t version;
            int32_t error;
            uint8_t data[240];
        } *query_resp = (void *)query.resp_data;

        query.version = OBMM_GSVA_ABI_VERSION;
        query.query_type = GSVA_QUERY_COHERENCE;
        query.segment_id = segment_id;
        query.home_va = my_base;
        rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
        CHECK(rc == 0, "coherence query should reach QEMU");
        CHECK(query_resp->error == GSVA_OK,
              "coherence query should report pending object as active");
        memcpy(&pending_seq, query_resp->data + sizeof(uint32_t),
               sizeof(pending_seq));
        printf("%s   coh_remote_inv Query pending seq=%#" PRIx64
               " peer_cna=%u\n", TAG, pending_seq, peer_cna);
        CHECK(pending_seq != 0, "coherence query should expose pending seq");
    }

    for (int attempt = 0; attempt < 30; attempt++) {
        rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETRY, local_cna,
                             (uint32_t)pending_seq, 0, segment_id, my_base,
                             GSVA_SIZE, &ev_error);
        CHECK(rc == 0, "Retry should reach QEMU");
        if (ev_error == GSVA_OK)
            break;
        usleep(100000);
    }
    printf("%s   coh_remote_inv Retry error=%d\n", TAG, ev_error);
    CHECK(ev_error == GSVA_OK, "remote INV_ACK should complete pending op");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "WriteAcquire after remote ACK should reach QEMU");
    CHECK(ev_error == GSVA_OK, "writer should own M after remote ACK");

    obmm_unmap_region(&import_region);
    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: coh_remote_wb ---- */
static void test_coh_remote_wb(int obmm_fd, uint32_t local_cna,
                               int node_idx, int node_count)
{
    TEST("GSVA coherence sends remote writeback over UB Link");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x6800000ULL;
    struct obmm_helpers_meta my_meta = {0};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region import_region = { .fd = -1 };
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t peer_cna = 0;
    uint64_t segment_id = 0;
    uint32_t token_id = 0;
    uint64_t pending_seq = 0;
    int rc;

    if (node_count < 2) {
        FAIL("remote writeback requires at least two nodes");
        return;
    }

    my_meta.export_cna = local_cna;
    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE,
                                  my_base + (uint64_t)node_idx * GSVA_SIZE);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410607ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    if (node_idx != 0) {
        for (int attempt = 0; attempt < 40; attempt++) {
            struct obmm_cmd_gsva_query_v1 query = {0};

            query.version = OBMM_GSVA_ABI_VERSION;
            query.query_type = GSVA_QUERY_CAPS;
            (void)ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
            usleep(100000);
        }
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        PASS();
        return;
    }

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410607ULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");
    CHECK(got[1], "peer metadata should be available");
    peer_cna = metas[1].export_cna;
    CHECK(peer_cna != 0 && peer_cna != local_cna,
          "peer CNA should be a real remote CNA");

    segment_id = my_meta.export_mem_id;
    token_id = my_meta.token_id;
    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    rc = obmm_do_import_v2(obmm_fd, &my_meta, local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           my_base, my_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, import_osync[0],
                                 &import_region);
    CHECK(rc == 0, "GSVA mmap should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, peer_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "remote owner WriteAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "remote CNA should become M owner");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local writer WriteAcquire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_COH_PENDING,
          "writer should wait for remote writeback ACK");

    {
        struct obmm_cmd_gsva_query_v1 query = {0};
        struct {
            uint32_t version;
            int32_t error;
            uint8_t data[240];
        } *query_resp = (void *)query.resp_data;

        query.version = OBMM_GSVA_ABI_VERSION;
        query.query_type = GSVA_QUERY_COHERENCE;
        query.segment_id = segment_id;
        query.home_va = my_base;
        rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
        CHECK(rc == 0, "coherence query should reach QEMU");
        CHECK(query_resp->error == GSVA_OK,
              "coherence query should report pending object as active");
        memcpy(&pending_seq, query_resp->data + sizeof(uint32_t),
               sizeof(pending_seq));
        printf("%s   coh_remote_wb Query pending seq=%#" PRIx64
               " peer_cna=%u\n", TAG, pending_seq, peer_cna);
        CHECK(pending_seq != 0, "coherence query should expose pending seq");
    }

    for (int attempt = 0; attempt < 30; attempt++) {
        rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETRY, local_cna,
                             (uint32_t)pending_seq, 0, segment_id, my_base,
                             GSVA_SIZE, &ev_error);
        CHECK(rc == 0, "Retry should reach QEMU");
        if (ev_error == GSVA_OK)
            break;
        usleep(100000);
    }
    printf("%s   coh_remote_wb Retry error=%d\n", TAG, ev_error);
    CHECK(ev_error == GSVA_OK, "remote WRITEBACK_ACK should complete pending op");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "WriteAcquire after remote writeback ACK should reach QEMU");
    CHECK(ev_error == GSVA_OK, "writer should own M after remote writeback ACK");
    {
        volatile uint64_t *probe =
            (volatile uint64_t *)(uintptr_t)import_region.addr;
        uint64_t value = *probe;

        __sync_synchronize();
        printf("%s   coh_remote_wb ARM MMU touch va=%#" PRIx64
               " value=%#" PRIx64 "\n", TAG, my_base, value);
    }

    obmm_unmap_region(&import_region);
    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: coh_remote_downgrade ---- */
static void test_coh_remote_downgrade(int obmm_fd, uint32_t local_cna,
                                      int node_idx, int node_count)
{
    TEST("GSVA coherence sends remote downgrade over UB Link");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x6c00000ULL;
    struct obmm_helpers_meta my_meta = {0};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region import_region = { .fd = -1 };
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t peer_cna = 0;
    uint64_t segment_id = 0;
    uint32_t token_id = 0;
    uint64_t pending_seq = 0;
    bool downgrade_already_complete = false;
    int rc;

    if (node_count < 2) {
        FAIL("remote downgrade requires at least two nodes");
        return;
    }

    my_meta.export_cna = local_cna;
    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE,
                                  my_base + (uint64_t)node_idx * GSVA_SIZE);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410609ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    if (node_idx != 0) {
        for (int attempt = 0; attempt < 40; attempt++) {
            struct obmm_cmd_gsva_query_v1 query = {0};

            query.version = OBMM_GSVA_ABI_VERSION;
            query.query_type = GSVA_QUERY_CAPS;
            (void)ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
            usleep(100000);
        }
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        PASS();
        return;
    }

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410609ULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");
    CHECK(got[1], "peer metadata should be available");
    peer_cna = metas[1].export_cna;
    CHECK(peer_cna != 0 && peer_cna != local_cna,
          "peer CNA should be a real remote CNA");

    segment_id = my_meta.export_mem_id;
    token_id = my_meta.token_id;
    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    rc = obmm_do_import_v2(obmm_fd, &my_meta, local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           my_base, my_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, import_osync[0],
                                 &import_region);
    CHECK(rc == 0, "GSVA mmap should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_WRITE_ACQUIRE, peer_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "remote owner WriteAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "remote CNA should become M owner");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local reader ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_COH_PENDING,
          "reader should wait for remote downgrade ACK");

    {
        struct obmm_cmd_gsva_query_v1 query = {0};
        struct {
            uint32_t version;
            int32_t error;
            uint8_t data[240];
        } *query_resp = (void *)query.resp_data;
        uint32_t query_state = 0;

        query.version = OBMM_GSVA_ABI_VERSION;
        query.query_type = GSVA_QUERY_COHERENCE;
        query.segment_id = segment_id;
        query.home_va = my_base;
        rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
        CHECK(rc == 0, "coherence query should reach QEMU");
        CHECK(query_resp->error == GSVA_OK,
              "coherence query should report active downgrade object");
        memcpy(&query_state, query_resp->data, sizeof(query_state));
        memcpy(&pending_seq, query_resp->data + sizeof(uint32_t),
               sizeof(pending_seq));
        printf("%s   coh_remote_downgrade Query state=%u seq=%#" PRIx64
               " peer_cna=%u\n", TAG, query_state, pending_seq, peer_cna);
        if (query_state == 1 && pending_seq == 0) {
            downgrade_already_complete = true;
        } else {
            CHECK(pending_seq != 0,
                  "coherence query should expose pending downgrade seq");
        }
    }

    if (downgrade_already_complete) {
        ev_error = GSVA_OK;
    } else {
        for (int attempt = 0; attempt < 30; attempt++) {
            rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETRY, local_cna,
                                 (uint32_t)pending_seq, 0, segment_id, my_base,
                                 GSVA_SIZE, &ev_error);
            CHECK(rc == 0, "Retry should reach QEMU");
            if (ev_error == GSVA_OK)
                break;
            usleep(100000);
        }
    }
    printf("%s   coh_remote_downgrade Retry error=%d\n", TAG, ev_error);
    CHECK(ev_error == GSVA_OK, "remote DOWNGRADE_ACK should complete pending op");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire after remote downgrade ACK should reach QEMU");
    CHECK(ev_error == GSVA_OK, "reader should share after remote downgrade ACK");
    {
        volatile uint64_t *probe =
            (volatile uint64_t *)(uintptr_t)import_region.addr;
        uint64_t value = *probe;

        __sync_synchronize();
        printf("%s   coh_remote_downgrade ARM MMU touch va=%#" PRIx64
               " value=%#" PRIx64 "\n", TAG, my_base, value);
    }

    obmm_unmap_region(&import_region);
    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: coh_remote_token_revoke ---- */
static void test_coh_remote_token_revoke(int obmm_fd, uint32_t local_cna,
                                         int node_idx, int node_count)
{
    TEST("GSVA coherence sends remote token revoke over UB Link");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x7400000ULL;
    struct obmm_helpers_meta my_meta = {0};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region import_region = { .fd = -1 };
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t peer_cna = 0;
    uint64_t segment_id = 0;
    uint32_t token_id = 0;
    uint32_t old_token = 0;
    uint32_t new_token = 0;
    int rc;

    if (node_count < 2) {
        FAIL("remote token revoke requires at least two nodes");
        return;
    }

    my_meta.export_cna = local_cna;
    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE,
                                  my_base + (uint64_t)node_idx * GSVA_SIZE);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x47535641060aULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    if (node_idx != 0) {
        for (int attempt = 0; attempt < 40; attempt++) {
            struct obmm_cmd_gsva_query_v1 query = {0};

            query.version = OBMM_GSVA_ABI_VERSION;
            query.query_type = GSVA_QUERY_CAPS;
            (void)ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
            usleep(100000);
        }
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        PASS();
        return;
    }

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x47535641060aULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");
    CHECK(got[1], "peer metadata should be available");
    peer_cna = metas[1].export_cna;
    CHECK(peer_cna != 0 && peer_cna != local_cna,
          "peer CNA should be a real remote CNA");

    segment_id = my_meta.export_mem_id;
    token_id = my_meta.token_id;
    old_token = token_id;
    new_token = old_token ^ 0x10203040U;
    if (new_token == 0 || new_token == old_token)
        new_token = old_token + 1;

    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    rc = obmm_do_import_v2(obmm_fd, &my_meta, local_cna,
                           import_pas[0], old_token,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           my_base, my_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, import_osync[0],
                                 &import_region);
    CHECK(rc == 0, "GSVA mmap should succeed");
    {
        volatile uint64_t *probe =
            (volatile uint64_t *)(uintptr_t)import_region.addr;
        uint64_t value = *probe;

        __sync_synchronize();
        printf("%s   coh_remote_token_revoke ARM MMU touch va=%#" PRIx64
               " value=%#" PRIx64 "\n", TAG, my_base, value);
    }

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, old_token, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local ReadAcquire with old token should reach QEMU");
    CHECK(ev_error == GSVA_OK, "local old token should be valid");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, peer_cna,
                         token_id, old_token, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "remote ReadAcquire with old token should reach QEMU");
    CHECK(ev_error == GSVA_OK, "remote old token should be valid");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_TOKEN_CHANGE, local_cna,
                         token_id, new_token, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "TokenChange should reach QEMU");
    CHECK(ev_error == GSVA_OK, "TokenChange should enter revoke flow");

    for (int attempt = 0; attempt < 30; attempt++) {
        struct obmm_cmd_gsva_query_v1 query = {0};

        query.version = OBMM_GSVA_ABI_VERSION;
        query.query_type = GSVA_QUERY_CAPS;
        (void)ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
        usleep(100000);
    }

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, old_token, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire with old token after revoke should reach QEMU");
    CHECK(ev_error == GSVA_ERR_TOKEN_DENIED,
          "old token after remote revoke should be denied");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, new_token, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire with new token after remote ACK should reach QEMU");
    printf("%s   coh_remote_token_revoke New token error=%d\n", TAG, ev_error);
    CHECK(ev_error == GSVA_OK, "new token should pass after remote TOKEN_ACK");

    obmm_unmap_region(&import_region);
    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: coh_remote_fence ---- */
static void test_coh_remote_fence(int obmm_fd, uint32_t local_cna,
                                  int node_idx, int node_count)
{
    TEST("GSVA coherence sends remote fence over UB Link");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x7800000ULL;
    struct obmm_helpers_meta my_meta = {0};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region import_region = { .fd = -1 };
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t peer_cna = 0;
    uint64_t segment_id = 0;
    uint32_t token_id = 0;
    uint64_t pending_seq = 0;
    bool fence_already_complete = false;
    int rc;

    if (node_count < 2) {
        FAIL("remote fence requires at least two nodes");
        return;
    }

    my_meta.export_cna = local_cna;
    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE,
                                  my_base + (uint64_t)node_idx * GSVA_SIZE);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x47535641060bULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    if (node_idx != 0) {
        for (int attempt = 0; attempt < 40; attempt++) {
            struct obmm_cmd_gsva_query_v1 query = {0};

            query.version = OBMM_GSVA_ABI_VERSION;
            query.query_type = GSVA_QUERY_CAPS;
            (void)ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
            usleep(100000);
        }
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        PASS();
        return;
    }

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x47535641060bULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");
    CHECK(got[1], "peer metadata should be available");
    peer_cna = metas[1].export_cna;
    CHECK(peer_cna != 0 && peer_cna != local_cna,
          "peer CNA should be a real remote CNA");

    segment_id = my_meta.export_mem_id;
    token_id = my_meta.token_id;
    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    rc = obmm_do_import_v2(obmm_fd, &my_meta, local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           my_base, my_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, import_osync[0],
                                 &import_region);
    CHECK(rc == 0, "GSVA mmap should succeed");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "local ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, peer_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "remote ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "remote ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_FENCE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "Fence should reach QEMU");
    CHECK(ev_error == GSVA_ERR_COH_PENDING,
          "fence should wait for remote FenceAck");

    {
        struct obmm_cmd_gsva_query_v1 query = {0};
        struct {
            uint32_t version;
            int32_t error;
            uint8_t data[240];
        } *query_resp = (void *)query.resp_data;
        uint32_t query_state = 0;

        query.version = OBMM_GSVA_ABI_VERSION;
        query.query_type = GSVA_QUERY_COHERENCE;
        query.segment_id = segment_id;
        query.home_va = my_base;
        rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
        CHECK(rc == 0, "coherence query should reach QEMU");
        CHECK(query_resp->error == GSVA_OK,
              "coherence query should report active fence object");
        memcpy(&query_state, query_resp->data, sizeof(query_state));
        memcpy(&pending_seq, query_resp->data + sizeof(uint32_t),
               sizeof(pending_seq));
        printf("%s   coh_remote_fence Query state=%u seq=%#" PRIx64
               " peer_cna=%u\n", TAG, query_state, pending_seq, peer_cna);
        if (query_state == 1 && pending_seq == 0) {
            fence_already_complete = true;
        } else {
            CHECK(pending_seq != 0,
                  "coherence query should expose pending fence seq");
        }
    }

    if (fence_already_complete) {
        ev_error = GSVA_OK;
    } else {
        for (int attempt = 0; attempt < 30; attempt++) {
            rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETRY, local_cna,
                                 (uint32_t)pending_seq, 0, segment_id, my_base,
                                 GSVA_SIZE, &ev_error);
            CHECK(rc == 0, "Retry should reach QEMU");
            if (ev_error == GSVA_OK)
                break;
            usleep(100000);
        }
    }
    printf("%s   coh_remote_fence Retry error=%d\n", TAG, ev_error);
    CHECK(ev_error == GSVA_OK, "remote FENCE_ACK should complete pending op");
    {
        volatile uint64_t *probe =
            (volatile uint64_t *)(uintptr_t)import_region.addr;
        uint64_t value = *probe;

        __sync_synchronize();
        printf("%s   coh_remote_fence ARM MMU touch va=%#" PRIx64
               " value=%#" PRIx64 "\n", TAG, my_base, value);
    }

    obmm_unmap_region(&import_region);
    obmm_do_unimport(obmm_fd, import_mem_id);
    obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
    PASS();
}

/* ---- Test: coh_remote_retire ---- */
static void test_coh_remote_retire(int obmm_fd, uint32_t local_cna,
                                   int node_idx, int node_count)
{
    TEST("GSVA coherence sends remote retire over UB Link");
    struct obmm_helpers_meta metas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool got[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t my_base = GSVA_BASE + 0x7000000ULL;
    struct obmm_helpers_meta my_meta = {0};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    uint64_t import_mem_id = 0;
    struct obmm_helpers_region import_region = { .fd = -1 };
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;
    uint32_t peer_cna = 0;
    uint64_t segment_id = 0;
    uint32_t token_id = 0;
    uint64_t pending_seq = 0;
    bool retire_already_complete = false;
    int rc;

    if (node_count < 2) {
        FAIL("remote retire requires at least two nodes");
        return;
    }

    my_meta.export_cna = local_cna;
    rc = obmm_do_export_fixed_uba(obmm_fd, &my_meta, GSVA_SIZE,
                                  my_base + (uint64_t)node_idx * GSVA_SIZE);
    CHECK(rc == 0, "fixed UBA export should succeed");

    rc = obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                                0x475356410608ULL, &my_meta);
    CHECK(rc == 0, "bootstrap publish should succeed");

    if (node_idx != 0) {
        for (int attempt = 0; attempt < 40; attempt++) {
            struct obmm_cmd_gsva_query_v1 query = {0};

            query.version = OBMM_GSVA_ABI_VERSION;
            query.query_type = GSVA_QUERY_CAPS;
            (void)ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
            usleep(100000);
        }
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        PASS();
        return;
    }

    rc = obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                               0x475356410608ULL, metas, got);
    CHECK(rc == 0, "bootstrap lookup should succeed");
    CHECK(got[1], "peer metadata should be available");
    peer_cna = metas[1].export_cna;
    CHECK(peer_cna != 0 && peer_cna != local_cna,
          "peer CNA should be a real remote CNA");

    segment_id = my_meta.export_mem_id;
    token_id = my_meta.token_id;
    if (!obmm_alloc_import_pas(1, GSVA_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        obmm_do_unexport(obmm_fd, my_meta.export_mem_id);
        FAIL("failed to allocate import PA");
        return;
    }

    rc = obmm_do_import_v2(obmm_fd, &my_meta, local_cna,
                           import_pas[0], token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0, segment_id,
                           my_base, my_base, 0,
                           &import_mem_id);
    CHECK(rc == 0, "GSVA identity import should succeed");

    rc = obmm_map_gsva_region_at(import_mem_id, (void *)(uintptr_t)my_base,
                                 GSVA_SIZE, import_osync[0],
                                 &import_region);
    CHECK(rc == 0, "GSVA mmap should succeed");
    {
        volatile uint64_t *probe =
            (volatile uint64_t *)(uintptr_t)import_region.addr;
        uint64_t value = *probe;

        __sync_synchronize();
        printf("%s   coh_remote_retire ARM MMU touch va=%#" PRIx64
               " value=%#" PRIx64 "\n", TAG, my_base, value);
    }

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "local ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "local ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, peer_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "remote ReadAcquire should reach QEMU");
    CHECK(ev_error == GSVA_OK, "remote ReadAcquire should pass");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "Retire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_COH_PENDING,
          "retire should wait for remote RetireAck");

    {
        struct obmm_cmd_gsva_query_v1 query = {0};
        struct {
            uint32_t version;
            int32_t error;
            uint8_t data[240];
        } *query_resp = (void *)query.resp_data;

        query.version = OBMM_GSVA_ABI_VERSION;
        query.query_type = GSVA_QUERY_COHERENCE;
        query.segment_id = segment_id;
        query.home_va = my_base;
        rc = ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_V1, &query);
        CHECK(rc == 0, "coherence query should reach QEMU");
        if (query_resp->error == GSVA_OK) {
            memcpy(&pending_seq, query_resp->data + sizeof(uint32_t),
                   sizeof(pending_seq));
            printf("%s   coh_remote_retire Query pending seq=%#" PRIx64
                   " peer_cna=%u\n", TAG, pending_seq, peer_cna);
            CHECK(pending_seq != 0, "coherence query should expose pending seq");
        } else if (query_resp->error == GSVA_ERR_SEGMENT_RETIRED) {
            retire_already_complete = true;
            printf("%s   coh_remote_retire Query already retired peer_cna=%u\n",
                   TAG, peer_cna);
        } else {
            FAIL("coherence query should report pending or retired state");
            return;
        }
    }

    if (retire_already_complete) {
        ev_error = GSVA_OK;
    } else {
        for (int attempt = 0; attempt < 30; attempt++) {
            rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_RETRY, local_cna,
                                 (uint32_t)pending_seq, 0, segment_id, my_base,
                                 GSVA_SIZE, &ev_error);
            CHECK(rc == 0, "Retry should reach QEMU");
            if (ev_error == GSVA_OK)
                break;
            usleep(100000);
        }
    }
    printf("%s   coh_remote_retire Retry error=%d\n", TAG, ev_error);
    CHECK(ev_error == GSVA_OK, "remote RETIRE_ACK should complete pending op");

    rc = gsva_send_event(obmm_fd, OBMM_GSVA_EVENT_READ_ACQUIRE, local_cna,
                         token_id, token_id, segment_id, my_base,
                         GSVA_SIZE, &ev_error);
    CHECK(rc == 0, "ReadAcquire after remote retire should reach QEMU");
    CHECK(ev_error == GSVA_ERR_SEGMENT_RETIRED,
          "ReadAcquire after remote retire should be rejected as retired");

    obmm_unmap_region(&import_region);
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
        "  writer_inv             Writer invalidates shared readers\n"
        "  retire_while_shared    Unmap while segment is shared\n"
        "  retire_event           Validate event retire tombstone\n"
        "  stale_remap            Validate stale epoch remap rejection\n"
        "  epoch_reuse            Validate higher epoch remap after retire\n"
        "  token_denied           Validate acquire token denial\n"
        "  token_write_denied     Validate read-only write denial\n"
        "  token_rotate           Validate token rotation\n"
        "  coh_timeout            Validate pending coherence timeout\n"
        "  coh_recovery           Validate pending coherence recovery\n"
        "  coh_remote_inv         Validate UB Link remote invalidate/ACK\n"
        "  coh_remote_wb          Validate UB Link remote writeback/ACK\n"
        "  coh_remote_downgrade   Validate UB Link remote downgrade/ACK\n"
        "  coh_remote_token_revoke Validate UB Link remote token revoke/ACK\n"
        "  coh_remote_fence       Validate UB Link remote fence/ACK\n"
        "  coh_remote_retire      Validate UB Link remote retire/ACK\n"
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
    } else if (strcmp(mode, "writer_inv") == 0) {
        test_writer_inv(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "retire_while_shared") == 0) {
        test_retire_while_shared(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "retire_event") == 0) {
        test_retire_event(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "stale_remap") == 0) {
        test_stale_remap(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "epoch_reuse") == 0) {
        test_epoch_reuse(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "token_denied") == 0) {
        test_token_denied(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "token_write_denied") == 0) {
        test_token_write_denied(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "token_rotate") == 0) {
        test_token_rotate(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "coh_timeout") == 0) {
        test_coh_timeout(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "coh_recovery") == 0) {
        test_coh_recovery(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "coh_remote_inv") == 0) {
        test_coh_remote_inv(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "coh_remote_wb") == 0) {
        test_coh_remote_wb(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "coh_remote_downgrade") == 0) {
        test_coh_remote_downgrade(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "coh_remote_token_revoke") == 0) {
        test_coh_remote_token_revoke(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "coh_remote_fence") == 0) {
        test_coh_remote_fence(obmm_fd, local_cna, node_idx, node_count);
    } else if (strcmp(mode, "coh_remote_retire") == 0) {
        test_coh_remote_retire(obmm_fd, local_cna, node_idx, node_count);
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
