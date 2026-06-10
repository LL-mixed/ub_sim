/*
 * SSD GSVA data test.
 *
 * Tests UB-attached SSD BLOCK_WRITE/BLOCK_READ round-trip via GSVA,
 * BLOCK_SEAL rejection of overwrite, and bad-token rejection.
 */

#include "obmm_common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../../kernel_ub/include/uapi/ub/ub_ssd.h"

#define TAG "[ssd_gsva_test]"

#define GSVA_BASE        0x700000000000ULL
#define GSVA_SEG_SIZE    0x200000ULL
#define GSVA_APERTURE_SIZE (GSVA_SEG_SIZE * 32)
#define GSVA_GENERATION  0x475356410401ULL
#define SSD_DEV          "/dev/ub_ssd0"
#define TEST_DATA_SIZE   4096

static int obmm_fd = -1;
static int ssd_fd = -1;
static uint32_t local_cna = 0;
static int node_idx = 0;
static int node_count = 2;

static struct obmm_helpers_meta local_meta;
static struct obmm_helpers_meta peer_metas[OBMM_POOL_HELPERS_MAX_NODES];
static bool peer_got[OBMM_POOL_HELPERS_MAX_NODES];
static uint64_t import_mem_id = 0;
static struct obmm_helpers_region peer_region;
static uint64_t peer_gsva_base = 0;

static struct gsva_key_v1 peer_key;
static uint32_t g_token_id = 0;

static int ssd_submit_and_wait(struct ub_ssd_cmd_v1 *cmd,
                               struct ub_ssd_cpl_v1 *cpl)
{
    int rc;

    rc = ioctl(ssd_fd, UB_SSD_SUBMIT, cmd);
    if (rc < 0) {
        fprintf(stderr, TAG "SSD_SUBMIT: %s\n", strerror(errno));
        return rc;
    }

    rc = ioctl(ssd_fd, UB_SSD_WAIT, cpl);
    if (rc < 0) {
        fprintf(stderr, TAG "SSD_WAIT: %s\n", strerror(errno));
        return rc;
    }

    return 0;
}

static void fill_buffer_desc(struct ub_ssd_buffer_desc_v1 *desc,
                             uint64_t offset, uint64_t bytes,
                             uint32_t token_value)
{
    desc->gsva_base = peer_gsva_base + offset;
    desc->bytes = bytes;
    desc->key = peer_key;
    desc->token_id = g_token_id;
    desc->token_value = token_value;
}

static int test_block_write_read_gsva(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    uint64_t *src, *dst;
    int i;

    printf(TAG "TEST: SSD BLOCK_WRITE + BLOCK_READ via GSVA\n");

    src = (uint64_t *)peer_region.addr;
    dst = (uint64_t *)((uint8_t *)peer_region.addr + TEST_DATA_SIZE);

    for (i = 0; i < (int)(TEST_DATA_SIZE / 8); i++)
        src[i] = 0x5555DDDD00000000ULL | (uint64_t)i;
    memset(dst, 0, TEST_DATA_SIZE);

    /* BLOCK_WRITE: write src data to SSD block */
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3001;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xAA;
    cmd.block_ref.block_lo = 0xBB00000000000001ULL;
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_id);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: BLOCK_WRITE status=%d\n", cpl.status);
        return -1;
    }
    if (cpl.committed_ref.version != 1) {
        fprintf(stderr, TAG "  FAIL: committed version=%llu want 1\n",
                (unsigned long long)cpl.committed_ref.version);
        return -1;
    }

    printf(TAG "  BLOCK_WRITE: %llu bytes, version=%llu\n",
           (unsigned long long)cpl.bytes_written,
           (unsigned long long)cpl.committed_ref.version);

    /* BLOCK_READ: read back into dst region */
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_READ;
    cmd.req_id = 0x3002;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xAA;
    cmd.block_ref.block_lo = 0xBB00000000000001ULL;
    cmd.block_ref.version = 0;
    cmd.block_ref.offset = 0;
    fill_buffer_desc(&cmd.buffer, TEST_DATA_SIZE, TEST_DATA_SIZE, g_token_id);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: BLOCK_READ status=%d\n", cpl.status);
        return -1;
    }

    if (memcmp(src, dst, TEST_DATA_SIZE) != 0) {
        fprintf(stderr, TAG "  FAIL: read-back data mismatch\n");
        return -1;
    }

    printf(TAG "  BLOCK_READ: %llu bytes verified\n",
           (unsigned long long)cpl.bytes_read);
    return 0;
}

static int test_seal_rejects_overwrite(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};

    printf(TAG "TEST: BLOCK_SEAL rejects subsequent BLOCK_WRITE\n");

    /* SEAL the block */
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_SEAL;
    cmd.req_id = 0x3003;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xAA;
    cmd.block_ref.block_lo = 0xBB00000000000001ULL;
    cmd.block_ref.version = 1;

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: SEAL status=%d\n", cpl.status);
        return -1;
    }

    /* Attempt to write the sealed block */
    memset(peer_region.addr, 0xCC, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3004;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xAA;
    cmd.block_ref.block_lo = 0xBB00000000000001ULL;
    cmd.block_ref.version = 1;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_id);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_SEALED) {
        fprintf(stderr, TAG "  FAIL: expected SEALED got %d\n", cpl.status);
        return -1;
    }

    printf(TAG "  PASS: sealed block rejects write\n");
    return 0;
}

static int test_bad_token(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};

    printf(TAG "TEST: bad token rejection\n");

    /* Write a fresh block with bad token */
    memset(peer_region.addr, 0xDD, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3005;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xCC;
    cmd.block_ref.block_lo = 0xDD00000000000001ULL;
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, 0xDEADBEEF);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;

    if (cpl.status != (__u32)SSD_ERR_TOKEN_DENIED) {
        fprintf(stderr, TAG "  FAIL: expected TOKEN_DENIED got %d\n",
                cpl.status);
        return -1;
    }

    printf(TAG "  PASS: rejected with TOKEN_DENIED\n");
    return 0;
}

static int test_version_conflict(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};

    printf(TAG "TEST: block write version conflict\n");

    memset(peer_region.addr, 0x11, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3006;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xEE;
    cmd.block_ref.block_lo = 0x0100000000000001ULL;
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_id);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: first write expected SSD_OK got %d\n",
                cpl.status);
        return -1;
    }

    memset(peer_region.addr, 0x22, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3007;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xEE;
    cmd.block_ref.block_lo = 0x0100000000000001ULL;
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_id);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_VERSION_CONFLICT) {
        fprintf(stderr, TAG "  FAIL: expected VERSION_CONFLICT got %d\n",
                cpl.status);
        return -1;
    }

    printf(TAG "  PASS: version-conflict write rejected\n");
    return 0;
}

static int test_tombstone_rejects_read_write(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};

    printf(TAG "TEST: tombstoned block rejects write/read\n");

    memset(peer_region.addr, 0x33, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3008;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xFF;
    cmd.block_ref.block_lo = 0x0200000000000001ULL;
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_id);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: initial write expected SSD_OK got %d\n",
                cpl.status);
        return -1;
    }

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_TOMBSTONE;
    cmd.req_id = 0x3009;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xFF;
    cmd.block_ref.block_lo = 0x0200000000000001ULL;
    cmd.block_ref.version = 1;

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: tombstone expected SSD_OK got %d\n",
                cpl.status);
        return -1;
    }

    memset(peer_region.addr, 0x55, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x300a;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xFF;
    cmd.block_ref.block_lo = 0x0200000000000001ULL;
    cmd.block_ref.version = 1;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_id);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_TOMBSTONED) {
        fprintf(stderr, TAG "  FAIL: expected TOMBSTONED write got %d\n",
                cpl.status);
        return -1;
    }

    memset(peer_region.addr, 0, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_READ;
    cmd.req_id = 0x300b;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xFF;
    cmd.block_ref.block_lo = 0x0200000000000001ULL;
    cmd.block_ref.version = 0;
    cmd.block_ref.offset = 0;
    fill_buffer_desc(&cmd.buffer, TEST_DATA_SIZE, TEST_DATA_SIZE, g_token_id);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_TOMBSTONED) {
        fprintf(stderr, TAG "  FAIL: expected TOMBSTONED read got %d\n",
                cpl.status);
        return -1;
    }

    printf(TAG "  PASS: tombstone blocks reject read/write\n");
    return 0;
}

static int parse_node_info(void)
{
    char buf[64];
    uint64_t cna_u64 = 0;

    if (obmm_env_or_cmdline("LINQU_NODE_COUNT", "linqu_node_count",
                            buf, sizeof(buf))) {
        node_count = atoi(buf);
        if (node_count < 2) node_count = 2;
    }
    if (obmm_env_or_cmdline("LINQU_NODE_IDX", "linqu_node_idx",
                            buf, sizeof(buf))) {
        node_idx = atoi(buf);
    }
    if (obmm_env_or_cmdline("LINQU_LOCAL_CNA", "linqu_local_cna",
                            buf, sizeof(buf))) {
        local_cna = (uint32_t)strtoull(buf, NULL, 0);
    } else if (obmm_env_or_cmdline("LINQU_CNA", "linqu_cna",
                                   buf, sizeof(buf))) {
        local_cna = (uint32_t)strtoull(buf, NULL, 0);
    } else if (obmm_parse_hex_u64("/sys/bus/ub/devices/00001/primary_cna",
                                   &cna_u64)) {
        local_cna = (uint32_t)cna_u64;
    } else {
        local_cna = (uint32_t)node_idx;
    }

    printf(TAG "node_idx=%d node_count=%d local_cna=%#x\n",
           node_idx, node_count, local_cna);
    return 0;
}

static int setup_gsva(void)
{
    uint64_t my_base = GSVA_BASE + (uint64_t)node_idx * GSVA_SEG_SIZE;
    struct obmm_cmd_gsva_aperture ap = {0};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    int peer_idx = -1;
    int rc, i;

    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        fprintf(stderr, TAG "open /dev/obmm: %s\n", strerror(errno));
        return -1;
    }

    ap.base = GSVA_BASE;
    ap.size = GSVA_APERTURE_SIZE;
    ap.generation = GSVA_GENERATION;
    ap.flags = OBMM_GSVA_APERTURE_F_ACTIVE;
    ap.node_id = (uint32_t)node_idx;
    ap.node_count = (uint32_t)node_count;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_APERTURE_REGISTER, &ap) != 0) {
        fprintf(stderr, TAG "aperture register: %s\n", strerror(errno));
        return -1;
    }

    memset(&local_meta, 0, sizeof(local_meta));
    local_meta.export_cna = local_cna;
    if (obmm_do_export_fixed_uba(obmm_fd, &local_meta, GSVA_SEG_SIZE,
                                 my_base) != 0) {
        fprintf(stderr, TAG "fixed-uba export: %s\n", strerror(errno));
        return -1;
    }

    if (obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                               GSVA_GENERATION, &local_meta) != 0) {
        fprintf(stderr, TAG "bootstrap publish: %s\n", strerror(errno));
        return -1;
    }

    memset(peer_metas, 0, sizeof(peer_metas));
    memset(peer_got, 0, sizeof(peer_got));
    if (obmm_bootstrap_lookup(obmm_fd, local_cna, node_count,
                              GSVA_GENERATION, peer_metas,
                              peer_got) != 0) {
        fprintf(stderr, TAG "bootstrap lookup: %s\n", strerror(errno));
        return -1;
    }

    for (i = 0; i < node_count; i++) {
        if (i != node_idx && peer_got[i]) {
            peer_idx = i;
            break;
        }
    }
    if (peer_idx < 0) {
        fprintf(stderr, TAG "no peer metadata found\n");
        return -1;
    }

    peer_gsva_base = GSVA_BASE + (uint64_t)peer_idx * GSVA_SEG_SIZE;

    if (!obmm_alloc_import_pas(1, GSVA_SEG_SIZE, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        fprintf(stderr, TAG "import PA allocation failed\n");
        return -1;
    }

    rc = obmm_do_import_v2(obmm_fd, &peer_metas[peer_idx], local_cna,
                           import_pas[0], peer_metas[peer_idx].token_id,
                           OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                           OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                           OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
                           0, 0, 0, 0, 0,
                           peer_metas[peer_idx].export_mem_id,
                           peer_gsva_base, peer_gsva_base, 0,
                           &import_mem_id);
    if (rc != 0) {
        fprintf(stderr, TAG "GSVA import: %s\n", strerror(errno));
        return -1;
    }

    memset(&peer_region, 0, sizeof(peer_region));
    if (obmm_map_gsva_region_at(import_mem_id,
                                (void *)(uintptr_t)peer_gsva_base,
                                GSVA_SEG_SIZE, false,
                                &peer_region) != 0) {
        fprintf(stderr, TAG "GSVA mmap: %s\n", strerror(errno));
        return -1;
    }

    memset(&peer_key, 0, sizeof(peer_key));
    peer_key.version = 1;
    peer_key.segment_id = peer_metas[peer_idx].export_mem_id;
    peer_key.home_va = peer_gsva_base;
    peer_key.size = GSVA_SEG_SIZE;
    peer_key.cache_policy = GSVA_CACHE_POLICY_DIRECTORY_MESI;
    peer_key.epoch = 1;

    g_token_id = peer_metas[peer_idx].token_id;

    printf(TAG "GSVA setup done: peer_base=%#llx token_id=%u\n",
           (unsigned long long)peer_gsva_base, g_token_id);
    return 0;
}

static void cleanup_gsva(void)
{
    obmm_unmap_region(&peer_region);
    if (import_mem_id)
        obmm_do_unimport(obmm_fd, import_mem_id);
    if (local_meta.export_mem_id)
        obmm_do_unexport(obmm_fd, local_meta.export_mem_id);
    if (obmm_fd >= 0)
        close(obmm_fd);
}

static int skip_code(void)
{
    const char *flag = getenv("GSVA_TEST_ALLOW_SKIP");

    return (flag && strcmp(flag, "1") == 0) ? 0 : 1;
}

int main(int argc, char *argv[])
{
    int pass = 0, fail = 0;

    (void)argc;
    (void)argv;

    printf(TAG "SSD GSVA data test suite\n");

    if (parse_node_info() < 0) {
        printf(TAG "verdict=SKIP (node info)\n");
        return skip_code();
    }

    ssd_fd = open(SSD_DEV, O_RDWR);
    if (ssd_fd < 0) {
        fprintf(stderr, TAG "open %s: %s\n", SSD_DEV, strerror(errno));
        printf(TAG "verdict=SKIP (no SSD device)\n");
        return skip_code();
    }

    if (setup_gsva() < 0) {
        fprintf(stderr, TAG "GSVA setup failed\n");
        close(ssd_fd);
        printf(TAG "verdict=SKIP (GSVA setup)\n");
        return skip_code();
    }

    if (test_block_write_read_gsva() == 0) pass++; else fail++;
    if (test_seal_rejects_overwrite() == 0) pass++; else fail++;
    if (test_bad_token() == 0) pass++; else fail++;
    if (test_version_conflict() == 0) pass++; else fail++;
    if (test_tombstone_rejects_read_write() == 0) pass++; else fail++;

    cleanup_gsva();
    close(ssd_fd);

    printf(TAG "Results: %d/%d passed, %d failed\n",
           pass, pass + fail, fail);
    printf(TAG "verdict=%s\n", fail == 0 ? "PASS" : "FAIL");
    return fail > 0 ? 1 : 0;
}
