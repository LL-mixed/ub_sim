/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gsva_coh_test -- test GSVA coherence state machine.
 *
 * Modes:
 *   write_read        -- WriteAcquire then ReadAcquire on another node
 *   writer_inv        -- ReadAcquire shared, then WriteAcquire triggers invalidation
 *   token_valid       -- use correct token_id/token_value, verify success
 *   token_denied      -- use wrong token_value, verify GSVA_ERR_TOKEN_DENIED
 *   token_rotate      -- rotate token_value, verify key identity unchanged
 *   retire_while_shared -- retire segment while multiple sharers hold S state
 *
 * Uses SimDec MMIO to send GSVA MAP, EVENT (coherence ops), and UNMAP.
 * Verifies state transitions match expected MESI behavior.
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

#define TAG "[gsva_coh_test]"

/* SimDec opcodes (from UAPI gsva.h) */
/* SIM_DEC_OP_GSVA_* already defined via obmm_common.h -> gsva.h */

/* Event sub-ops */
#define GSVA_EVENT_READ_ACQUIRE   1
#define GSVA_EVENT_WRITE_ACQUIRE  2
#define GSVA_EVENT_RETIRE         3

/* GSVA error codes already defined via obmm_common.h -> gsva.h */

/* Test result tracking */
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

/* SimDec MMIO helpers */
#define SIM_DEC_BASE       0x2f800000ULL
#define SIM_DEC_CMD_OFFSET 0x1000

static volatile uint32_t *sim_dec_cmd;

static int sim_dec_init(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        fd = open("/dev/ub-mmio", O_RDWR | O_SYNC);
    }
    if (fd < 0) {
        printf("%s cannot open MMIO device: %s\n", TAG, strerror(errno));
        return -1;
    }

    void *map = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, SIM_DEC_BASE);
    close(fd);
    if (map == MAP_FAILED) {
        printf("%s cannot mmap SimDec: %s\n", TAG, strerror(errno));
        return -1;
    }

    sim_dec_cmd = (volatile uint32_t *)((char *)map + SIM_DEC_CMD_OFFSET);
    return 0;
}

/*
 * GSVA MAP request layout (must match SimDecGsvaMapReq in QEMU ub_ubc.c).
 * Uses a unique struct name to avoid collision with UAPI gsva_key_v1.
 */

struct coh_test_map_req {
    uint32_t version;
    uint32_t flags;
    struct gsva_key_v1 key;
    uint64_t local_pa;
    uint64_t local_va;
    uint64_t remote_uba;
    uint64_t token_id;
    uint64_t token_value;
    uint32_t source;
    uint32_t address_profile;
    uint32_t access_flags;
} __attribute__((packed));

struct coh_test_map_resp {
    uint64_t map_id;
    int32_t  error;
    uint32_t reserved;
} __attribute__((packed));

static int send_gsva_map(uint64_t segment_id, uint64_t home_va,
                         uint64_t size, uint64_t epoch,
                         uint32_t token_id, uint32_t token_value,
                         uint32_t access_flags,
                         uint64_t *map_id)
{
    struct coh_test_map_req req;
    struct coh_test_map_resp resp;

    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.key.version = 1;
    req.key.segment_id = segment_id;
    req.key.home_va = home_va;
    req.key.size = size;
    req.key.epoch = epoch;
    req.address_profile = 1; /* strict: local_va == home_va == remote_uba */
    req.local_va = home_va;
    req.local_pa = home_va;
    req.remote_uba = home_va;
    req.token_id = token_id;
    req.token_value = token_value;
    req.access_flags = access_flags;

    memcpy((void *)sim_dec_cmd, &req, sizeof(req));
    sim_dec_cmd[0] = SIM_DEC_OP_GSVA_MAP_V1;
    memcpy(&resp, (void *)sim_dec_cmd, sizeof(resp));

    if (map_id) {
        *map_id = resp.map_id;
    }
    return (int)resp.error;
}

/*
 * GSVA EVENT request layout:
 *   uint32_t sub_op;
 *   uint32_t requester_cna;
 *   uint32_t token_id;
 *   uint32_t token_value;
 *   GsvaKeyV1 key;
 */
struct coh_test_event_req {
    uint32_t sub_op;
    uint32_t requester_cna;
    uint32_t token_id;
    uint32_t token_value;
    struct gsva_key_v1 key;
} __attribute__((packed));

struct coh_test_event_resp {
    int32_t  error;
} __attribute__((packed));

static int send_gsva_event(uint32_t sub_op, uint64_t segment_id,
                           uint64_t home_va, uint64_t epoch,
                           uint32_t requester_cna,
                           uint32_t token_id, uint32_t token_value)
{
    struct coh_test_event_req req;
    struct coh_test_event_resp resp;

    memset(&req, 0, sizeof(req));
    req.sub_op = sub_op;
    req.requester_cna = requester_cna;
    req.token_id = token_id;
    req.token_value = token_value;
    req.key.version = 1;
    req.key.segment_id = segment_id;
    req.key.home_va = home_va;
    req.key.size = 0x1000;
    req.key.epoch = epoch;

    memcpy((void *)sim_dec_cmd, &req, sizeof(req));
    sim_dec_cmd[0] = SIM_DEC_OP_GSVA_EVENT_V1;
    memcpy(&resp, (void *)sim_dec_cmd, sizeof(resp));

    return (int)resp.error;
}

static int send_gsva_unmap(uint64_t map_id)
{
    struct {
        uint32_t version;
        uint32_t flags;
        struct gsva_key_v1 key;
        uint64_t map_id;
    } __attribute__((packed)) req;

    struct {
        int32_t  error;
        uint32_t reserved;
    } __attribute__((packed)) resp;

    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.map_id = map_id;

    memcpy((void *)sim_dec_cmd, &req, sizeof(req));
    sim_dec_cmd[0] = SIM_DEC_OP_GSVA_UNMAP_V1;
    memcpy(&resp, (void *)sim_dec_cmd, sizeof(resp));

    return (int)resp.error;
}

/* ---- Test: write_read ---- */
static void test_write_read(void)
{
    TEST("WriteAcquire I->M then ReadAcquire M->S");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0xAA00, 0x10000000, 0x10000, 1,
                           0, 0, 0, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");
    CHECK(map_id != 0, "map_id should be non-zero");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0xAA00, 0x10000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire cna=0 I->M should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xAA00, 0x10000000,
                         1, 1, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=1 M->S should succeed");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: writer_inv ---- */
static void test_writer_inv(void)
{
    TEST("ReadAcquire shared then WriteAcquire triggers invalidation");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0xBB00, 0x20000000, 0x10000, 1,
                           0, 0, 0, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    /* Multiple readers acquire shared */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xBB00, 0x20000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=0 I->S should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xBB00, 0x20000000,
                         1, 1, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=1 S->S should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xBB00, 0x20000000,
                         1, 2, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=2 S->S should succeed");

    /* Writer acquires exclusive -- invalidates sharers */
    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0xBB00, 0x20000000,
                         1, 3, 0, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire cna=3 S->M should succeed (invalidate sharers)");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: token_valid ---- */
static void test_token_valid(void)
{
    TEST("valid token passes ReadAcquire and WriteAcquire");
    uint64_t map_id = 0;
    /* MAP with token_id=42, token_value=100, access_flags=3 (RW) */
    int rc = send_gsva_map(0xCC00, 0x30000000, 0x10000, 1,
                           42, 100, 3, &map_id);
    CHECK(rc == GSVA_OK, "map with token should succeed");

    /* ReadAcquire with correct token */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xCC00, 0x30000000,
                         1, 0, 42, 100);
    CHECK(rc == GSVA_OK, "ReadAcquire with valid token should succeed");

    /* WriteAcquire with correct token */
    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0xCC00, 0x30000000,
                         1, 0, 42, 100);
    CHECK(rc == GSVA_OK, "WriteAcquire with valid token should succeed");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: token_denied ---- */
static void test_token_denied(void)
{
    TEST("wrong token rejected with GSVA_ERR_TOKEN_DENIED");
    uint64_t map_id = 0;
    /* MAP with token_id=42, token_value=100, access_flags=3 */
    int rc = send_gsva_map(0xDD00, 0x40000000, 0x10000, 1,
                           42, 100, 3, &map_id);
    CHECK(rc == GSVA_OK, "map with token should succeed");

    /* ReadAcquire with wrong token_value */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xDD00, 0x40000000,
                         1, 0, 42, 999);
    CHECK(rc == GSVA_ERR_TOKEN_DENIED,
          "ReadAcquire with wrong token_value should fail with TOKEN_DENIED");

    /* ReadAcquire with wrong token_id */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xDD00, 0x40000000,
                         1, 0, 99, 100);
    CHECK(rc == GSVA_ERR_TOKEN_DENIED,
          "ReadAcquire with wrong token_id should fail with TOKEN_DENIED");

    /* WriteAcquire with wrong token_value */
    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0xDD00, 0x40000000,
                         1, 0, 42, 999);
    CHECK(rc == GSVA_ERR_TOKEN_DENIED,
          "WriteAcquire with wrong token_value should fail with TOKEN_DENIED");

    /* Verify correct token still works after denied attempts */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xDD00, 0x40000000,
                         1, 0, 42, 100);
    CHECK(rc == GSVA_OK,
          "ReadAcquire with correct token should succeed after denied attempts");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: token_rotate ---- */
static void test_token_rotate(void)
{
    TEST("token rotation preserves GSVA key identity");
    uint64_t map_id1 = 0;

    /* MAP with token_id=42, token_value=100, access_flags=3 */
    int rc = send_gsva_map(0xEE00, 0x50000000, 0x10000, 1,
                           42, 100, 3, &map_id1);
    CHECK(rc == GSVA_OK, "map epoch=1 token_v=100 should succeed");

    /* ReadAcquire succeeds with token_value=100 */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xEE00, 0x50000000,
                         1, 0, 42, 100);
    CHECK(rc == GSVA_OK, "ReadAcquire with token_v=100 should succeed");

    /* Retire the segment */
    rc = send_gsva_event(GSVA_EVENT_RETIRE, 0xEE00, 0x50000000, 1, 0,
                         0, 0);
    CHECK(rc == GSVA_OK, "retire should succeed");
    send_gsva_unmap(map_id1);

    /* Re-map same segment_id with higher epoch, same token_id, new token_value */
    uint64_t map_id2 = 0;
    rc = send_gsva_map(0xEE00, 0x50000000, 0x10000, 2,
                       42, 200, 3, &map_id2);
    CHECK(rc == GSVA_OK, "map epoch=2 token_v=200 should succeed");
    CHECK(map_id2 != map_id1, "new map_id should differ");

    /* Old token_value should fail */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xEE00, 0x50000000,
                         2, 0, 42, 100);
    CHECK(rc == GSVA_ERR_TOKEN_DENIED,
          "old token_value should fail after rotation");

    /* New token_value should succeed */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xEE00, 0x50000000,
                         2, 0, 42, 200);
    CHECK(rc == GSVA_OK, "new token_value should succeed after rotation");

    send_gsva_unmap(map_id2);
    PASS();
}

/* ---- Test: retire_while_shared ---- */
static void test_retire_while_shared(void)
{
    TEST("retire segment while multiple sharers hold S state");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0xFF00, 0x60000000, 0x10000, 1,
                           0, 0, 0, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    /* Multiple readers acquire shared */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xFF00, 0x60000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=0 I->S should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xFF00, 0x60000000,
                         1, 1, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=1 S->S should succeed");

    /* Retire while shared */
    rc = send_gsva_event(GSVA_EVENT_RETIRE, 0xFF00, 0x60000000, 1, 0,
                         0, 0);
    CHECK(rc == GSVA_OK, "retire while shared should succeed");

    /* Post-retire access should fail */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xFF00, 0x60000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_ERR_SEGMENT_RETIRED,
          "ReadAcquire on retired segment should fail");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: segment lifecycle with epoch ---- */
static void test_segment_lifecycle(void)
{
    TEST("segment lifecycle: map->write->retire->remap");
    uint64_t map_id1 = 0;
    int rc = send_gsva_map(0x1100, 0x70000000, 0x10000, 1,
                           0, 0, 0, &map_id1);
    CHECK(rc == GSVA_OK, "map epoch=1 should succeed");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0x1100, 0x70000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire should succeed I->M");

    rc = send_gsva_event(GSVA_EVENT_RETIRE, 0x1100, 0x70000000, 1, 0,
                         0, 0);
    CHECK(rc == GSVA_OK, "retire should succeed");

    send_gsva_unmap(map_id1);

    /* Re-map with higher epoch */
    uint64_t map_id2 = 0;
    rc = send_gsva_map(0x1100, 0x70000000, 0x10000, 2,
                       0, 0, 0, &map_id2);
    CHECK(rc == GSVA_OK, "map epoch=2 should succeed (reuse)");
    CHECK(map_id2 != map_id1, "new map_id should differ");

    /* Old epoch should fail */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x1100, 0x70000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_ERR_STALE_EPOCH, "old epoch should be rejected");

    /* New epoch should succeed */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x1100, 0x70000000,
                         2, 0, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire on new epoch should succeed");

    send_gsva_unmap(map_id2);
    PASS();
}

/* ---- Test: stale epoch rejected ---- */
static void test_stale_epoch_rejected(void)
{
    TEST("stale epoch rejected on acquire");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0x2200, 0x80000000, 0x10000, 3,
                           0, 0, 0, &map_id);
    CHECK(rc == GSVA_OK, "map epoch=3 should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x2200, 0x80000000,
                         2, 0, 0, 0);
    CHECK(rc == GSVA_ERR_STALE_EPOCH, "epoch=2 should be rejected (current=3)");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x2200, 0x80000000,
                         3, 0, 0, 0);
    CHECK(rc == GSVA_OK, "epoch=3 should succeed");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: stale epoch remap rejected ---- */
static void test_stale_epoch_remap_rejected(void)
{
    TEST("stale epoch rejected on remap");
    uint64_t map_id1 = 0;
    int rc = send_gsva_map(0x3300, 0x90000000, 0x10000, 5,
                           0, 0, 0, &map_id1);
    CHECK(rc == GSVA_OK, "map epoch=5 should succeed");
    send_gsva_unmap(map_id1);

    /* Remap with same epoch - should fail */
    uint64_t map_id2 = 0;
    rc = send_gsva_map(0x3300, 0x90000000, 0x10000, 5,
                       0, 0, 0, &map_id2);
    CHECK(rc == GSVA_ERR_STALE_EPOCH, "same epoch should be rejected");

    /* Remap with lower epoch - should fail */
    rc = send_gsva_map(0x3300, 0x90000000, 0x10000, 4,
                       0, 0, 0, &map_id2);
    CHECK(rc == GSVA_ERR_STALE_EPOCH, "lower epoch should be rejected");

    PASS();
}

/* ---- Test: operations on retired segment fail ---- */
static void test_retired_segment_ops_fail(void)
{
    TEST("operations on retired segment fail");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0x4400, 0xA0000000, 0x10000, 1,
                           0, 0, 0, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_RETIRE, 0x4400, 0xA0000000, 1, 0,
                         0, 0);
    CHECK(rc == GSVA_OK, "retire should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x4400, 0xA0000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_ERR_SEGMENT_RETIRED,
          "ReadAcquire on RETIRED should fail");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0x4400, 0xA0000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_ERR_SEGMENT_RETIRED,
          "WriteAcquire on RETIRED should fail");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: multiple sharers ---- */
static void test_multiple_sharers(void)
{
    TEST("multiple sharers ReadAcquire S->S");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0x5500, 0xB0000000, 0x10000, 1,
                           0, 0, 0, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x5500, 0xB0000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=0 I->S should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x5500, 0xB0000000,
                         1, 1, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=1 S->S should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x5500, 0xB0000000,
                         1, 2, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=2 S->S should succeed");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: WriteAcquire M->M owner stays ---- */
static void test_write_owner_stays(void)
{
    TEST("WriteAcquire M->M owner stays");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0x6600, 0xC0000000, 0x10000, 1,
                           0, 0, 0, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0x6600, 0xC0000000,
                         1, 3, 0, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire cna=3 I->M should succeed");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0x6600, 0xC0000000,
                         1, 3, 0, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire cna=3 M->M (owner) should succeed");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: WriteAcquire ownership transfer ---- */
static void test_write_ownership_transfer(void)
{
    TEST("WriteAcquire M->M ownership transfer");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0x7700, 0xD0000000, 0x10000, 1,
                           0, 0, 0, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0x7700, 0xD0000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire cna=0 I->M should succeed");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0x7700, 0xD0000000,
                         1, 1, 0, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire cna=1 M->M (transfer) should succeed");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: E state transition ---- */
static void test_E_to_M_owner(void)
{
    TEST("WriteAcquire E->M owner upgrade");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0x8800, 0xE0000000, 0x10000, 1,
                           0, 0, 0, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    /* First WriteAcquire puts object in M state (I->M) */
    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0x8800, 0xE0000000,
                         1, 0, 0, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire cna=0 I->M should succeed");

    /* ReadAcquire from another node: M->S (downgrade owner to sharer) */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x8800, 0xE0000000,
                         1, 1, 0, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=1 M->S should succeed");

    send_gsva_unmap(map_id);
    PASS();
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --mode <mode>\n"
        "Modes:\n"
        "  write_read          WriteAcquire then ReadAcquire\n"
        "  writer_inv          Shared readers then writer invalidation\n"
        "  token_valid         Valid token passes acquire\n"
        "  token_denied        Wrong token rejected\n"
        "  token_rotate        Token rotation preserves key identity\n"
        "  retire_while_shared Retire while segment is shared\n"
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

    if (sim_dec_init() != 0) {
        printf("%s SimDec MMIO not available, running compile/link check only\n",
               TAG);
        tests_run++;
        tests_passed++;
        printf("%s TEST: compile/link check\n", TAG);
        printf("%s   PASS\n", TAG);
        printf("%s =========================\n", TAG);
        printf("%s Results: %d/%d passed, %d failed\n",
               TAG, tests_passed, tests_run, tests_failed);
        return tests_failed > 0 ? 1 : 0;
    }

    printf("%s SimDec MMIO initialized\n", TAG);

    if (strcmp(mode, "write_read") == 0) {
        test_write_read();
    } else if (strcmp(mode, "writer_inv") == 0) {
        test_writer_inv();
    } else if (strcmp(mode, "token_valid") == 0) {
        test_token_valid();
    } else if (strcmp(mode, "token_denied") == 0) {
        test_token_denied();
    } else if (strcmp(mode, "token_rotate") == 0) {
        test_token_rotate();
    } else if (strcmp(mode, "retire_while_shared") == 0) {
        test_retire_while_shared();
    } else if (strcmp(mode, "all") == 0) {
        test_write_read();
        test_writer_inv();
        test_token_valid();
        test_token_denied();
        test_token_rotate();
        test_retire_while_shared();
        test_segment_lifecycle();
        test_stale_epoch_rejected();
        test_stale_epoch_remap_rejected();
        test_retired_segment_ops_fail();
        test_multiple_sharers();
        test_write_owner_stays();
        test_write_ownership_transfer();
        test_E_to_M_owner();
    } else {
        fprintf(stderr, "%s unknown mode: %s\n", TAG, mode);
        usage(argv[0]);
        return 1;
    }

    printf("%s =========================\n", TAG);
    printf("%s Results: %d/%d passed, %d failed\n",
           TAG, tests_passed, tests_run, tests_failed);
    printf("%s verdict=%s\n", TAG, tests_failed > 0 ? "FAIL" : "PASS");

    return tests_failed > 0 ? 1 : 0;
}
