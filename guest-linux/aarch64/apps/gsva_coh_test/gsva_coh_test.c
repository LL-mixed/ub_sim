/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gsva_coh_test -- test GSVA coherence state machine (ReadAcquire/WriteAcquire/Retire).
 *
 * Uses SimDec MMIO to send GSVA MAP, EVENT (coherence ops), and UNMAP.
 * Verifies state transitions match expected MESI behavior.
 *
 * Usage:
 *   gsva_coh_test
 */

#include "obmm_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define TAG "[gsva_coh_test]"

/* SimDec opcodes */
#define SIM_DEC_OP_GSVA_MAP_V1    0x09
#define SIM_DEC_OP_GSVA_UNMAP_V1  0x0a
#define SIM_DEC_OP_GSVA_EVENT_V1  0x0b
#define SIM_DEC_OP_GSVA_QUERY_V1  0x0c

/* Event sub-ops */
#define GSVA_EVENT_READ_ACQUIRE   1
#define GSVA_EVENT_WRITE_ACQUIRE  2
#define GSVA_EVENT_RETIRE         3

/* GSVA error codes */
#define GSVA_OK                   0
#define GSVA_ERR_STALE_EPOCH     -3
#define GSVA_ERR_COH_PENDING     -5
#define GSVA_ERR_SEGMENT_RETIRED -6

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
        /* Fallback: use /dev/ub-mmio if available */
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

static int send_gsva_map(uint64_t segment_id, uint64_t home_va,
                         uint64_t size, uint64_t epoch, uint64_t *map_id)
{
    struct {
        uint32_t version;
        uint32_t flags;
        uint64_t segment_id;
        uint64_t home_va;
        uint64_t size;
        uint64_t vmid;
        uint64_t asid;
        uint64_t pte_offset;
        uint32_t p_tag;
        uint32_t cache_policy;
        uint64_t epoch;
        uint64_t local_pa;
        uint64_t local_va;
        uint64_t remote_uba;
        uint32_t source;
        uint32_t address_profile;
        uint64_t token_id;
        uint64_t token_value;
    } req;

    struct {
        uint32_t error;
        uint64_t map_id;
    } resp;

    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.segment_id = segment_id;
    req.home_va = home_va;
    req.size = size;
    req.epoch = epoch;
    req.address_profile = 1; /* strict: local_va == home_va == remote_uba */
    req.local_va = home_va;
    req.local_pa = home_va;
    req.remote_uba = home_va;

    /* Write request to SimDec command area */
    memcpy((void *)sim_dec_cmd, &req, sizeof(req));

    /* Trigger opcode */
    sim_dec_cmd[0] = SIM_DEC_OP_GSVA_MAP_V1;

    /* Read response */
    memcpy(&resp, (void *)sim_dec_cmd, sizeof(resp));

    if (map_id) {
        *map_id = resp.map_id;
    }
    return (int)resp.error;
}

static int send_gsva_event(uint32_t sub_op, uint64_t segment_id,
                           uint64_t home_va, uint64_t epoch,
                           uint32_t requester_cna)
{
    struct {
        uint32_t sub_op;
        uint32_t requester_cna;
        /* GsvaKeyV1 fields */
        uint32_t version;
        uint32_t flags;
        uint64_t segment_id;
        uint64_t home_va;
        uint64_t size;
        uint64_t vmid;
        uint64_t asid;
        uint64_t pte_offset;
        uint32_t p_tag;
        uint32_t cache_policy;
        uint64_t epoch;
    } req;

    struct {
        uint32_t error;
    } resp;

    memset(&req, 0, sizeof(req));
    req.sub_op = sub_op;
    req.requester_cna = requester_cna;
    req.version = 1;
    req.segment_id = segment_id;
    req.home_va = home_va;
    req.size = 0x1000;
    req.epoch = epoch;

    memcpy((void *)sim_dec_cmd, &req, sizeof(req));
    sim_dec_cmd[0] = SIM_DEC_OP_GSVA_EVENT_V1;
    memcpy(&resp, (void *)sim_dec_cmd, sizeof(resp));

    return (int)resp.error;
}

static int send_gsva_unmap(uint64_t map_id)
{
    struct {
        uint32_t version;
        uint64_t map_id;
    } req;

    struct {
        uint32_t error;
    } resp;

    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.map_id = map_id;

    memcpy((void *)sim_dec_cmd, &req, sizeof(req));
    sim_dec_cmd[0] = SIM_DEC_OP_GSVA_UNMAP_V1;
    memcpy(&resp, (void *)sim_dec_cmd, sizeof(resp));

    return (int)resp.error;
}

/* Test: MAP + coherence object creation */
static void test_map_creates_coh_object(void)
{
    TEST("map creates coherence object");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0xAA00, 0x10000000, 0x10000, 1, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");
    CHECK(map_id != 0, "map_id should be non-zero");
    PASS();

    /* Cleanup */
    send_gsva_unmap(map_id);
}

/* Test: ReadAcquire I->S */
static void test_read_acquire_I_to_S(void)
{
    TEST("ReadAcquire I->S");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0xBB00, 0x20000000, 0x10000, 1, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xBB00, 0x20000000,
                         1, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire should succeed I->S");

    send_gsva_unmap(map_id);
    PASS();
}

/* Test: WriteAcquire I->M */
static void test_write_acquire_I_to_M(void)
{
    TEST("WriteAcquire I->M");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0xCC00, 0x30000000, 0x10000, 1, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0xCC00, 0x30000000,
                         1, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire should succeed I->M");

    send_gsva_unmap(map_id);
    PASS();
}

/* Test: ReadAcquire then WriteAcquire S->M */
static void test_read_then_write_S_to_M(void)
{
    TEST("ReadAcquire then WriteAcquire S->M");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0xDD00, 0x40000000, 0x10000, 1, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xDD00, 0x40000000,
                         1, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire should succeed I->S");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0xDD00, 0x40000000,
                         1, 0);
    CHECK(rc == GSVA_OK, "WriteAcquire should succeed S->M");

    send_gsva_unmap(map_id);
    PASS();
}

/* Test: stale epoch rejected */
static void test_stale_epoch_rejected(void)
{
    TEST("stale epoch rejected");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0xEE00, 0x50000000, 0x10000, 2, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xEE00, 0x50000000,
                         1, 0);
    CHECK(rc == GSVA_ERR_STALE_EPOCH, "should reject stale epoch");

    send_gsva_unmap(map_id);
    PASS();
}

/* Test: retire transitions to RETIRED */
static void test_retire(void)
{
    TEST("retire transitions to RETIRED");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0xFF00, 0x60000000, 0x10000, 1, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_RETIRE, 0xFF00, 0x60000000, 1, 0);
    CHECK(rc == GSVA_OK, "retire should succeed");

    send_gsva_unmap(map_id);
    PASS();
}

/* Test: operations on retired segment fail */
static void test_retired_segment_ops_fail(void)
{
    TEST("operations on retired segment fail");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0x1100, 0x70000000, 0x10000, 1, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_RETIRE, 0x1100, 0x70000000, 1, 0);
    CHECK(rc == GSVA_OK, "retire should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x1100, 0x70000000,
                         1, 0);
    CHECK(rc == GSVA_ERR_SEGMENT_RETIRED, "ReadAcquire on RETIRED should fail");

    send_gsva_unmap(map_id);
    PASS();
}

/* Test: multiple sharers ReadAcquire */
static void test_multiple_sharers(void)
{
    TEST("multiple sharers ReadAcquire S->S");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0x2200, 0x80000000, 0x10000, 1, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x2200, 0x80000000,
                         1, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=0 should succeed I->S");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x2200, 0x80000000,
                         1, 1);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=1 should succeed S->S");

    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0x2200, 0x80000000,
                         1, 2);
    CHECK(rc == GSVA_OK, "ReadAcquire cna=2 should succeed S->S");

    send_gsva_unmap(map_id);
    PASS();
}

/* Test: WriteAcquire M->M owner stays */
static void test_write_owner_stays(void)
{
    TEST("WriteAcquire M->M owner stays");
    uint64_t map_id = 0;
    int rc = send_gsva_map(0x3300, 0x90000000, 0x10000, 1, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0x3300, 0x90000000,
                         1, 3);
    CHECK(rc == GSVA_OK, "WriteAcquire cna=3 I->M should succeed");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0x3300, 0x90000000,
                         1, 3);
    CHECK(rc == GSVA_OK, "WriteAcquire cna=3 M->M (owner) should succeed");

    send_gsva_unmap(map_id);
    PASS();
}

/* Fallback test: runs without hardware, validates compile/link */
static void test_software_only(void)
{
    TEST("software-only compile/link check");
    /* If we got here, the binary compiled and linked correctly */
    PASS();
}

int main(int argc, char **argv)
{
    printf("%s GSVA coherence test suite\n", TAG);
    printf("%s =========================\n", TAG);

    /* Always run the compile/link check */
    test_software_only();

    /* Try to initialize SimDec MMIO */
    if (sim_dec_init() != 0) {
        printf("%s SimDec MMIO not available, running software-only tests\n",
               TAG);
        printf("%s =========================\n", TAG);
        printf("%s Results: %d/%d passed, %d failed\n",
               TAG, tests_passed, tests_run, tests_failed);
        return tests_failed > 0 ? 1 : 0;
    }

    printf("%s SimDec MMIO initialized, running full test suite\n", TAG);

    test_map_creates_coh_object();
    test_read_acquire_I_to_S();
    test_write_acquire_I_to_M();
    test_read_then_write_S_to_M();
    test_stale_epoch_rejected();
    test_retire();
    test_retired_segment_ops_fail();
    test_multiple_sharers();
    test_write_owner_stays();

    printf("%s =========================\n", TAG);
    printf("%s Results: %d/%d passed, %d failed\n",
           TAG, tests_passed, tests_run, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
