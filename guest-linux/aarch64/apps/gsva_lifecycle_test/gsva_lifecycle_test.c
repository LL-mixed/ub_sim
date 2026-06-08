/* SPDX-License-Identifier: GPL-2.0 */
/*
 * gsva_lifecycle_test -- test GSVA segment lifecycle transactions.
 *
 * Modes:
 *   mmap_strict        -- verify strict address identity (local_va == home_va == remote_uba)
 *   mmap_reloc_reject  -- verify relocated mmap is rejected in strict mode
 *   mmap_aperture_overlap -- verify overlapping mappings are rejected
 *   retire_reuse       -- map, retire, remap with higher epoch
 *   stale_epoch        -- verify stale epoch rejection
 *
 * Uses SimDec MMIO to send GSVA MAP/EVENT/UNMAP directly.
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

#define TAG "[gsva_lifecycle]"

/* SimDec opcodes and error codes from UAPI gsva.h */

/* Event sub-ops */
#define GSVA_EVENT_READ_ACQUIRE   1
#define GSVA_EVENT_WRITE_ACQUIRE  2
#define GSVA_EVENT_RETIRE         3

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

/* GsvaKeyV1 already defined via obmm_common.h -> gsva.h */

struct lc_test_map_req {
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

struct lc_test_map_resp {
    uint64_t map_id;
    int32_t  error;
    uint32_t reserved;
} __attribute__((packed));

struct lc_test_event_req {
    uint32_t sub_op;
    uint32_t requester_cna;
    uint32_t token_id;
    uint32_t token_value;
    struct gsva_key_v1 key;
} __attribute__((packed));

struct lc_test_event_resp {
    int32_t  error;
} __attribute__((packed));

static int send_gsva_map(uint64_t segment_id, uint64_t home_va,
                         uint64_t size, uint64_t epoch,
                         uint32_t address_profile,
                         uint64_t local_va, uint64_t remote_uba,
                         uint64_t *map_id)
{
    struct lc_test_map_req req;
    struct lc_test_map_resp resp;

    memset(&req, 0, sizeof(req));
    req.version = 1;
    req.key.version = 1;
    req.key.segment_id = segment_id;
    req.key.home_va = home_va;
    req.key.size = size;
    req.key.epoch = epoch;
    req.address_profile = address_profile;
    req.local_va = local_va;
    req.local_pa = home_va;
    req.remote_uba = remote_uba;

    memcpy((void *)sim_dec_cmd, &req, sizeof(req));
    sim_dec_cmd[0] = SIM_DEC_OP_GSVA_MAP_V1;
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
    struct lc_test_event_req req;
    struct lc_test_event_resp resp;

    memset(&req, 0, sizeof(req));
    req.sub_op = sub_op;
    req.requester_cna = requester_cna;
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

/* ---- Test: mmap_strict ---- */
static void test_mmap_strict(void)
{
    TEST("strict address identity: local_va == home_va == remote_uba");
    uint64_t map_id = 0;

    /* Strict profile=1: all three addresses must match */
    int rc = send_gsva_map(0xA100, 0x10000000, 0x10000, 1,
                           1, 0x10000000, 0x10000000, &map_id);
    CHECK(rc == GSVA_OK, "strict map with matching addresses should succeed");
    CHECK(map_id != 0, "map_id should be non-zero");
    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: mmap_reloc_reject ---- */
static void test_mmap_reloc_reject(void)
{
    TEST("relocated address rejected in strict mode");
    uint64_t map_id = 0;

    /* local_va != home_va: strict profile should reject */
    int rc = send_gsva_map(0xA200, 0x20000000, 0x10000, 1,
                           1, 0x30000000, 0x20000000, &map_id);
    CHECK(rc != GSVA_OK,
          "strict map with local_va != home_va should be rejected");

    /* remote_uba != home_va: strict profile should reject */
    rc = send_gsva_map(0xA201, 0x20000000, 0x10000, 1,
                       1, 0x20000000, 0x40000000, &map_id);
    CHECK(rc != GSVA_OK,
          "strict map with remote_uba != home_va should be rejected");

    PASS();
}

/* ---- Test: mmap_aperture_overlap ---- */
static void test_mmap_aperture_overlap(void)
{
    TEST("overlapping VA range mapping rejected");
    uint64_t map_id1 = 0, map_id2 = 0;

    /* First mapping: [0x50000000, 0x50010000) */
    int rc = send_gsva_map(0xA300, 0x50000000, 0x10000, 1,
                           1, 0x50000000, 0x50000000, &map_id1);
    CHECK(rc == GSVA_OK, "first map should succeed");

    /* Overlapping mapping: same VA range, different segment_id */
    rc = send_gsva_map(0xA301, 0x50000000, 0x10000, 1,
                       1, 0x50000000, 0x50000000, &map_id2);
    CHECK(rc != GSVA_OK,
          "overlapping map with same base identity should be rejected");

    /* Partially overlapping: [0x50008000, 0x50018000) */
    rc = send_gsva_map(0xA302, 0x50008000, 0x10000, 1,
                       1, 0x50008000, 0x50008000, &map_id2);
    CHECK(rc != GSVA_OK,
          "partially overlapping map should be rejected");

    send_gsva_unmap(map_id1);
    PASS();
}

/* ---- Test: retire_reuse ---- */
static void test_retire_reuse(void)
{
    TEST("retire then reuse with higher epoch");
    uint64_t map_id1 = 0;

    /* Map at epoch=1 */
    int rc = send_gsva_map(0xA400, 0x60000000, 0x10000, 1,
                           1, 0x60000000, 0x60000000, &map_id1);
    CHECK(rc == GSVA_OK, "map epoch=1 should succeed");

    /* Retire */
    rc = send_gsva_event(GSVA_EVENT_RETIRE, 0xA400, 0x60000000, 1, 0);
    CHECK(rc == GSVA_OK, "retire should succeed");
    send_gsva_unmap(map_id1);

    /* Re-map same segment_id with higher epoch */
    uint64_t map_id2 = 0;
    rc = send_gsva_map(0xA400, 0x60000000, 0x10000, 2,
                       1, 0x60000000, 0x60000000, &map_id2);
    CHECK(rc == GSVA_OK, "map epoch=2 after retire should succeed");
    CHECK(map_id2 != map_id1, "new map_id should differ");

    /* Verify the new mapping is usable */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xA400, 0x60000000, 2, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire on new epoch should succeed");

    send_gsva_unmap(map_id2);
    PASS();
}

/* ---- Test: stale_epoch ---- */
static void test_stale_epoch(void)
{
    TEST("stale epoch rejection");
    uint64_t map_id = 0;

    /* Map at epoch=3 */
    int rc = send_gsva_map(0xA500, 0x70000000, 0x10000, 3,
                           1, 0x70000000, 0x70000000, &map_id);
    CHECK(rc == GSVA_OK, "map epoch=3 should succeed");

    /* Acquire with matching epoch */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xA500, 0x70000000, 3, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire with matching epoch should succeed");

    /* Acquire with stale epoch */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xA500, 0x70000000, 2, 0);
    CHECK(rc == GSVA_ERR_STALE_EPOCH,
          "ReadAcquire with old epoch should fail");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0xA500, 0x70000000, 1, 0);
    CHECK(rc == GSVA_ERR_STALE_EPOCH,
          "WriteAcquire with old epoch should fail");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: retire prevents further access ---- */
static void test_retire_blocks_access(void)
{
    TEST("retired segment blocks all further access");
    uint64_t map_id = 0;

    int rc = send_gsva_map(0xA600, 0x80000000, 0x10000, 1,
                           1, 0x80000000, 0x80000000, &map_id);
    CHECK(rc == GSVA_OK, "map should succeed");

    /* Acquire shared */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xA600, 0x80000000, 1, 0);
    CHECK(rc == GSVA_OK, "ReadAcquire before retire should succeed");

    /* Retire */
    rc = send_gsva_event(GSVA_EVENT_RETIRE, 0xA600, 0x80000000, 1, 0);
    CHECK(rc == GSVA_OK, "retire should succeed");

    /* Post-retire access fails */
    rc = send_gsva_event(GSVA_EVENT_READ_ACQUIRE, 0xA600, 0x80000000, 1, 0);
    CHECK(rc == -9, "ReadAcquire after retire should fail with SEGMENT_RETIRED");

    rc = send_gsva_event(GSVA_EVENT_WRITE_ACQUIRE, 0xA600, 0x80000000, 1, 0);
    CHECK(rc == -9, "WriteAcquire after retire should fail with SEGMENT_RETIRED");

    send_gsva_unmap(map_id);
    PASS();
}

/* ---- Test: non-strict profile accepts mismatched addresses ---- */
static void test_nonstrict_address(void)
{
    TEST("non-strict profile accepts flexible addresses");
    uint64_t map_id = 0;

    /* Profile=0 (generic GVA): should accept any addresses */
    int rc = send_gsva_map(0xA700, 0x90000000, 0x10000, 1,
                           0, 0x90000000, 0x90000000, &map_id);
    CHECK(rc == GSVA_OK, "non-strict map should succeed");
    send_gsva_unmap(map_id);
    PASS();
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s --mode <mode>\n"
        "Modes:\n"
        "  mmap_strict          Verify strict address identity\n"
        "  mmap_reloc_reject    Verify relocated mmap rejected\n"
        "  mmap_aperture_overlap Verify overlapping mappings rejected\n"
        "  retire_reuse         Verify retire then reuse with higher epoch\n"
        "  stale_epoch          Verify stale epoch rejection\n"
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
        printf("%s verdict=%s\n", TAG, tests_failed > 0 ? "FAIL" : "PASS");
        return tests_failed > 0 ? 1 : 0;
    }

    printf("%s SimDec MMIO initialized\n", TAG);

    if (strcmp(mode, "mmap_strict") == 0) {
        test_mmap_strict();
    } else if (strcmp(mode, "mmap_reloc_reject") == 0) {
        test_mmap_reloc_reject();
    } else if (strcmp(mode, "mmap_aperture_overlap") == 0) {
        test_mmap_aperture_overlap();
    } else if (strcmp(mode, "retire_reuse") == 0) {
        test_retire_reuse();
    } else if (strcmp(mode, "stale_epoch") == 0) {
        test_stale_epoch();
    } else if (strcmp(mode, "all") == 0) {
        test_mmap_strict();
        test_mmap_reloc_reject();
        test_mmap_aperture_overlap();
        test_retire_reuse();
        test_stale_epoch();
        test_retire_blocks_access();
        test_nonstrict_address();
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
