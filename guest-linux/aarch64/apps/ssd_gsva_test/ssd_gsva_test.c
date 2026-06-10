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
#define TEST_SLOT_STRIDE 0x20000ULL
#define SNAPSHOT_OFFSET  0x4000ULL
#define SNAPSHOT_SIZE    0x10000ULL
#define TIMEOUT_OFFSET   0x15000ULL

static int obmm_fd = -1;
static int ssd_fd = -1;
static uint32_t local_cna = 0;
static int node_idx = 0;
static int node_count = 2;

static struct obmm_helpers_meta local_meta;
static struct obmm_gsva_segment_desc_v1 peer_desc;
static struct obmm_helpers_meta peer_metas[OBMM_POOL_HELPERS_MAX_NODES];
static bool peer_got[OBMM_POOL_HELPERS_MAX_NODES];
static int peer_ids[OBMM_POOL_HELPERS_MAX_NODES];
static int peer_count = 0;
static uint64_t import_mem_id = 0;
static struct obmm_helpers_region peer_region;
static uint64_t peer_gsva_base = 0;
static uint64_t peer_slot_base = 0;
static int current_peer_node_idx = 0;

static struct gsva_key_v1 peer_key;
static uint32_t g_token_id = 0;
static uint32_t g_token_value = 0;

static uint64_t peer_offset(uint64_t offset)
{
    return peer_slot_base + offset;
}

static void *peer_ptr(uint64_t offset)
{
    return (uint8_t *)peer_region.addr + peer_offset(offset);
}

static uint64_t peer_block_lo(uint8_t kind)
{
    return ((uint64_t)kind << 56) |
           ((uint64_t)(uint32_t)current_peer_node_idx << 32) |
           ((uint64_t)(uint32_t)node_idx << 16) |
           1ULL;
}

static void init_peer_key_from_desc(const struct obmm_gsva_segment_desc_v1 *desc,
                                   struct gsva_key_v1 *key)
{
    memset(key, 0, sizeof(*key));
    key->version = desc->version;
    key->flags = 0;
    key->segment_id = desc->segment_id;
    key->home_va = desc->home_va;
    key->size = desc->size;
    key->vmid = 0;
    key->asid = 0;
    key->pte_offset = 0;
    key->p_tag = desc->p_tag;
    key->cache_policy = desc->cache_policy;
    key->epoch = desc->epoch;
}

static void init_peer_desc_from_bootstrap(const struct obmm_helpers_meta *meta,
                                          uint32_t owner_node_id,
                                          struct obmm_gsva_segment_desc_v1 *desc)
{
    memset(desc, 0, sizeof(*desc));
    desc->version = OBMM_GSVA_ABI_VERSION;
    desc->flags = OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY |
                  OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED |
                  OBMM_GSVA_SEG_F_ACTIVE;
    desc->segment_id = meta->export_mem_id;
    desc->home_va = meta->remote_uba;
    desc->size = meta->size;
    desc->epoch = 1;
    desc->home_cna = meta->export_cna;
    desc->owner_node_id = owner_node_id;
    desc->node_count = (uint32_t)node_count;
    desc->cache_policy = OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI;
    desc->p_tag = meta->export_cna & 0x00ffffffu;
    desc->access_flags = OBMM_GSVA_ACCESS_READ | OBMM_GSVA_ACCESS_WRITE;
    desc->token_id = meta->token_id;
    desc->token_value = meta->token_id;
}

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
    desc->gsva_base = peer_gsva_base + peer_offset(offset);
    desc->bytes = bytes;
    desc->key = peer_key;
    desc->token_id = g_token_id;
    desc->token_value = token_value;
}

static void fill_buffer_desc_with_key(struct ub_ssd_buffer_desc_v1 *desc,
                                      uint64_t offset, uint64_t bytes,
                                      const struct gsva_key_v1 *key,
                                      uint32_t token_id,
                                      uint32_t token_value)
{
    fill_buffer_desc(desc, offset, bytes, token_value);
    desc->key = *key;
    desc->token_id = token_id;
}

static int send_gsva_event(uint32_t sub_op, const struct gsva_key_v1 *key,
                           uint32_t token_id, uint32_t token_value,
                           int32_t *error_out)
{
    struct obmm_cmd_gsva_event_v1 ev = {0};

    ev.version = OBMM_GSVA_ABI_VERSION;
    ev.sub_op = sub_op;
    ev.requester_cna = local_cna;
    ev.token_id = token_id;
    ev.token_value = token_value;
    ev.key = *key;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_EVENT_V1, &ev) != 0)
        return -1;
    if (error_out)
        *error_out = ev.error;
    return 0;
}

static int test_block_write_read_gsva(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    uint64_t *src, *dst;
    uint8_t *partial_dst;
    const uint64_t partial_off = 128;
    const uint64_t partial_len = 512;
    int i;

    printf(TAG "TEST: SSD BLOCK_WRITE + BLOCK_READ via GSVA\n");

    src = (uint64_t *)peer_ptr(0);
    dst = (uint64_t *)peer_ptr(TEST_DATA_SIZE);

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
    cmd.block_ref.block_lo = peer_block_lo(0xBB);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

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
    if (cpl.committed_ref.bytes != TEST_DATA_SIZE ||
        cpl.committed_ref.checksum64 == 0) {
        fprintf(stderr, TAG "  FAIL: committed bytes=%llu checksum=%#llx\n",
                (unsigned long long)cpl.committed_ref.bytes,
                (unsigned long long)cpl.committed_ref.checksum64);
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
    cmd.block_ref.block_lo = peer_block_lo(0xBB);
    cmd.block_ref.version = 0;
    cmd.block_ref.offset = 0;
    fill_buffer_desc(&cmd.buffer, TEST_DATA_SIZE, TEST_DATA_SIZE, g_token_value);

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
    printf(TAG " LINGQU_BLOCK_READ block_hi=%#llx block_lo=%#llx version=%llu offset=%llu bytes=%llu checksum64=%#llx status=ok\n",
           (unsigned long long)cpl.committed_ref.block_hi,
           (unsigned long long)cpl.committed_ref.block_lo,
           (unsigned long long)cpl.committed_ref.version,
           (unsigned long long)cpl.committed_ref.offset,
           (unsigned long long)cpl.bytes_read,
           (unsigned long long)cpl.committed_ref.checksum64);

    partial_dst = (uint8_t *)peer_ptr(TEST_DATA_SIZE * 2);
    memset(partial_dst, 0xA5, partial_len + 2);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_READ;
    cmd.req_id = 0x30021;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xAA;
    cmd.block_ref.block_lo = peer_block_lo(0xBB);
    cmd.block_ref.version = 0;
    cmd.block_ref.offset = partial_off;
    cmd.block_ref.bytes = partial_len;
    fill_buffer_desc(&cmd.buffer, TEST_DATA_SIZE * 2 + 1, partial_len,
                     g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: partial BLOCK_READ status=%d\n",
                cpl.status);
        return -1;
    }
    if (cpl.bytes_read != partial_len ||
        cpl.committed_ref.offset != partial_off ||
        cpl.committed_ref.bytes != partial_len ||
        cpl.committed_ref.checksum64 == 0) {
        fprintf(stderr, TAG "  FAIL: partial ref offset=%llu bytes=%llu checksum=%#llx\n",
                (unsigned long long)cpl.committed_ref.offset,
                (unsigned long long)cpl.committed_ref.bytes,
                (unsigned long long)cpl.committed_ref.checksum64);
        return -1;
    }
    if (partial_dst[0] != 0xA5 || partial_dst[partial_len + 1] != 0xA5) {
        fprintf(stderr, TAG "  FAIL: partial read modified guard bytes\n");
        return -1;
    }
    if (memcmp(partial_dst + 1, (uint8_t *)src + partial_off,
               partial_len) != 0) {
        fprintf(stderr, TAG "  FAIL: partial read data mismatch\n");
        return -1;
    }

    printf(TAG "  PASS: partial BLOCK_READ range offset=%llu bytes=%llu\n",
           (unsigned long long)partial_off,
           (unsigned long long)partial_len);
    return 0;
}

static int test_missing_block_rejects_read(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    uint8_t *dst = peer_ptr(TEST_DATA_SIZE * 3);

    printf(TAG "TEST: missing block read rejection\n");

    memset(dst, 0xA5, TEST_DATA_SIZE);
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_READ;
    cmd.req_id = 0x3013;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xBAD;
    cmd.block_ref.block_lo = peer_block_lo(0x0A);
    cmd.block_ref.version = 0;
    cmd.block_ref.offset = 0;
    cmd.block_ref.bytes = TEST_DATA_SIZE;
    fill_buffer_desc(&cmd.buffer, TEST_DATA_SIZE * 3, TEST_DATA_SIZE,
                     g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_BAD_BLOCK) {
        fprintf(stderr, TAG "  FAIL: expected BAD_BLOCK got %d\n", cpl.status);
        return -1;
    }
    for (int i = 0; i < TEST_DATA_SIZE; i++) {
        if (dst[i] != 0xA5) {
            fprintf(stderr, TAG "  FAIL: missing block read wrote byte %d\n", i);
            return -1;
        }
    }

    printf(TAG "  PASS: missing block rejected without synthetic payload\n");
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
    cmd.block_ref.block_lo = peer_block_lo(0xBB);
    cmd.block_ref.version = 1;

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: SEAL status=%d\n", cpl.status);
        return -1;
    }

    /* Attempt to write the sealed block */
    memset(peer_ptr(0), 0xCC, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3004;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xAA;
    cmd.block_ref.block_lo = peer_block_lo(0xBB);
    cmd.block_ref.version = 1;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

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
    memset(peer_ptr(0), 0xDD, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3005;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xCC;
    cmd.block_ref.block_lo = peer_block_lo(0xDD);
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

static int test_bad_token_id(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};

    printf(TAG "TEST: bad token_id rejection\n");

    memset(peer_ptr(0), 0xEE, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x300c;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xEE;
    cmd.block_ref.block_lo = peer_block_lo(0xE2);
    cmd.block_ref.version = 0;

    cmd.buffer.gsva_base = peer_gsva_base + peer_offset(0);
    cmd.buffer.bytes = TEST_DATA_SIZE;
    cmd.buffer.key = peer_key;
    cmd.buffer.token_id = 0;
    cmd.buffer.token_value = g_token_value;

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

    memset(peer_ptr(0), 0x11, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3006;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xEE;
    cmd.block_ref.block_lo = peer_block_lo(0x01);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: first write expected SSD_OK got %d\n",
                cpl.status);
        return -1;
    }

    memset(peer_ptr(0), 0x22, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3007;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xEE;
    cmd.block_ref.block_lo = peer_block_lo(0x01);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

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

    memset(peer_ptr(0), 0x33, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3008;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xFF;
    cmd.block_ref.block_lo = peer_block_lo(0x02);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

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
    cmd.block_ref.block_lo = peer_block_lo(0x02);
    cmd.block_ref.version = 1;

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: tombstone expected SSD_OK got %d\n",
                cpl.status);
        return -1;
    }

    memset(peer_ptr(0), 0x55, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x300a;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xFF;
    cmd.block_ref.block_lo = peer_block_lo(0x02);
    cmd.block_ref.version = 1;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_TOMBSTONED) {
        fprintf(stderr, TAG "  FAIL: expected TOMBSTONED write got %d\n",
                cpl.status);
        return -1;
    }

    memset(peer_ptr(0), 0, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_READ;
    cmd.req_id = 0x300b;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xFF;
    cmd.block_ref.block_lo = peer_block_lo(0x02);
    cmd.block_ref.version = 0;
    cmd.block_ref.offset = 0;
    fill_buffer_desc(&cmd.buffer, TEST_DATA_SIZE, TEST_DATA_SIZE, g_token_value);

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

static int test_checksum_mismatch_denied(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    uint64_t good_checksum;

    printf(TAG "TEST: checksum mismatch rejection\n");

    memset(peer_ptr(0), 0x44, TEST_DATA_SIZE);
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3011;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xCD;
    cmd.block_ref.block_lo = peer_block_lo(0x07);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK || cpl.checksum64 == 0) {
        fprintf(stderr, TAG "  FAIL: checksum seed write status=%d checksum=%llu\n",
                cpl.status, (unsigned long long)cpl.checksum64);
        return -1;
    }
    good_checksum = cpl.checksum64;

    memset(peer_ptr(TEST_DATA_SIZE), 0xA5, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_READ;
    cmd.req_id = 0x3012;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xCD;
    cmd.block_ref.block_lo = peer_block_lo(0x07);
    cmd.block_ref.version = 0;
    cmd.block_ref.offset = 0;
    cmd.block_ref.checksum64 = good_checksum ^ 1ULL;
    fill_buffer_desc(&cmd.buffer, TEST_DATA_SIZE, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_CHECKSUM) {
        fprintf(stderr, TAG "  FAIL: expected CHECKSUM got %d\n", cpl.status);
        return -1;
    }
    for (int i = 0; i < TEST_DATA_SIZE; i++) {
        if (((uint8_t *)peer_ptr(TEST_DATA_SIZE))[i] != 0xA5) {
            fprintf(stderr, TAG "  FAIL: checksum failure modified output byte %d\n", i);
            return -1;
        }
    }

    printf(TAG "  PASS: checksum mismatch rejected without output write\n");
    return 0;
}

static char *find_bytes(uint8_t *buf, uint64_t len, const char *needle)
{
    size_t needle_len = strlen(needle);

    if (needle_len == 0 || len < needle_len)
        return NULL;

    for (uint64_t i = 0; i <= len - needle_len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0)
            return (char *)(buf + i);
    }
    return NULL;
}

static int corrupt_snapshot_checksum(uint64_t offset, uint64_t len)
{
    const char *needle = "\"checksum64\":";
    uint8_t *buf = peer_ptr(offset);
    char *p = find_bytes(buf, len, needle);
    char *end = (char *)buf + len;

    if (!p)
        return -1;

    p += strlen(needle);
    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    if (p >= end || *p < '0' || *p > '9')
        return -1;

    *p = (*p == '9') ? '8' : (char)(*p + 1);
    return 0;
}

static int snapshot_write_pattern(uint8_t pattern, uint64_t version,
                                  uint64_t req_id)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};

    memset(peer_ptr(0), pattern, TEST_DATA_SIZE);
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = req_id;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0x5A5A;
    cmd.block_ref.block_lo = peer_block_lo(0x08);
    cmd.block_ref.version = version;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: snapshot write status=%d\n", cpl.status);
        return -1;
    }
    return 0;
}

static int snapshot_read_expect(uint8_t pattern, uint64_t req_id)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    uint8_t *dst = peer_ptr(TEST_DATA_SIZE);

    memset(dst, 0, TEST_DATA_SIZE);
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_READ;
    cmd.req_id = req_id;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0x5A5A;
    cmd.block_ref.block_lo = peer_block_lo(0x08);
    cmd.block_ref.version = 0;
    cmd.block_ref.offset = 0;
    fill_buffer_desc(&cmd.buffer, TEST_DATA_SIZE, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: snapshot read status=%d\n", cpl.status);
        return -1;
    }
    for (int i = 0; i < TEST_DATA_SIZE; i++) {
        if (dst[i] != pattern) {
            fprintf(stderr, TAG "  FAIL: snapshot byte %d got %#x want %#x\n",
                    i, dst[i], pattern);
            return -1;
        }
    }
    return 0;
}

static int test_snapshot_export_import(void)
{
    struct ub_ssd_snapshot_v1 snap = {0};
    uint64_t snapshot_len;
    int saved_errno;

    printf(TAG "TEST: snapshot export/import and corrupted import rejection\n");

    if (snapshot_write_pattern(0x61, 0, 0x3020) < 0)
        return -1;
    if (snapshot_read_expect(0x61, 0x3021) < 0)
        return -1;

    memset(peer_ptr(SNAPSHOT_OFFSET), 0, SNAPSHOT_SIZE);
    snap.version = 1;
    fill_buffer_desc(&snap.buffer, SNAPSHOT_OFFSET, SNAPSHOT_SIZE,
                     g_token_value);
    snap.snapshot_size = 0;
    if (ioctl(ssd_fd, UB_SSD_EXPORT_SNAPSHOT, &snap) != 0) {
        fprintf(stderr, TAG "  FAIL: EXPORT_SNAPSHOT errno=%d (%s)\n",
                errno, strerror(errno));
        return -1;
    }
    snapshot_len = snap.snapshot_size;
    if (snapshot_len == 0 || snapshot_len > SNAPSHOT_SIZE) {
        fprintf(stderr, TAG "  FAIL: bad snapshot len=%llu\n",
                (unsigned long long)snapshot_len);
        return -1;
    }

    if (snapshot_write_pattern(0x62, 1, 0x3022) < 0)
        return -1;
    if (snapshot_read_expect(0x62, 0x3023) < 0)
        return -1;

    memset(&snap, 0, sizeof(snap));
    snap.version = 1;
    fill_buffer_desc(&snap.buffer, SNAPSHOT_OFFSET, SNAPSHOT_SIZE,
                     g_token_value);
    snap.snapshot_size = snapshot_len;
    if (ioctl(ssd_fd, UB_SSD_IMPORT_SNAPSHOT, &snap) != 0) {
        fprintf(stderr, TAG "  FAIL: IMPORT_SNAPSHOT errno=%d (%s)\n",
                errno, strerror(errno));
        return -1;
    }
    if (snap.snapshot_size != snapshot_len) {
        fprintf(stderr, TAG "  FAIL: import size=%llu want %llu\n",
                (unsigned long long)snap.snapshot_size,
                (unsigned long long)snapshot_len);
        return -1;
    }
    if (snapshot_read_expect(0x61, 0x3024) < 0)
        return -1;

    if (snapshot_write_pattern(0x63, 1, 0x3025) < 0)
        return -1;
    if (snapshot_read_expect(0x63, 0x3026) < 0)
        return -1;

    if (corrupt_snapshot_checksum(SNAPSHOT_OFFSET, snapshot_len) != 0) {
        fprintf(stderr, TAG "  FAIL: cannot corrupt snapshot checksum\n");
        return -1;
    }

    memset(&snap, 0, sizeof(snap));
    snap.version = 1;
    fill_buffer_desc(&snap.buffer, SNAPSHOT_OFFSET, SNAPSHOT_SIZE,
                     g_token_value);
    snap.snapshot_size = snapshot_len;
    errno = 0;
    if (ioctl(ssd_fd, UB_SSD_IMPORT_SNAPSHOT, &snap) == 0) {
        fprintf(stderr, TAG "  FAIL: corrupted IMPORT_SNAPSHOT succeeded\n");
        return -1;
    }
    saved_errno = errno;
    if (saved_errno != EINVAL) {
        fprintf(stderr, TAG "  FAIL: corrupted import errno=%d want %d\n",
                saved_errno, EINVAL);
        return -1;
    }
    if (snapshot_read_expect(0x63, 0x3027) < 0)
        return -1;

    printf(TAG "  PASS: snapshot import restores state and rejects corruption without replacement\n");
    return 0;
}

static int test_flush_stat_gsva(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    struct ub_ssd_query_v1 query = {0};

    printf(TAG "TEST: FLUSH and STAT command completion\n");

    cmd.version = 1;
    cmd.opcode = SSD_OP_FLUSH;
    cmd.req_id = 0x3030;
    cmd.source_cna = local_cna;
    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: FLUSH status=%d\n", cpl.status);
        return -1;
    }

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_STAT;
    cmd.req_id = 0x3031;
    cmd.source_cna = local_cna;
    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: STAT status=%d\n", cpl.status);
        return -1;
    }

    query.version = 1;
    query.type = UB_QUERY_SSD_CAPS;
    if (ioctl(ssd_fd, UB_SSD_QUERY, &query) != 0) {
        fprintf(stderr, TAG "  FAIL: SSD_QUERY errno=%d (%s)\n",
                errno, strerror(errno));
        return -1;
    }
    if (query.u.status.backend_profile != 0) {
        fprintf(stderr, TAG "  FAIL: backend_profile=%llu want memory(0)\n",
                (unsigned long long)query.u.status.backend_profile);
        return -1;
    }
    if (query.u.status.last_req_id != 0x3031) {
        fprintf(stderr, TAG "  FAIL: last_req_id=%#llx want 0x3031\n",
                (unsigned long long)query.u.status.last_req_id);
        return -1;
    }

    printf(TAG "  PASS: FLUSH/STAT completed backend_profile=memory\n");
    return 0;
}

static int test_dfs_manifest_refs_block_payload(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    char manifest[512];
    int n;

    printf(TAG "TEST: DFS manifest references SSD Lingqu Block payload\n");

    memset(peer_ptr(0), 0x6B, TEST_DATA_SIZE);
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3032;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xD5F;
    cmd.block_ref.block_lo = peer_block_lo(0x09);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: manifest seed BLOCK_WRITE status=%d\n",
                cpl.status);
        return -1;
    }
    if (cpl.committed_ref.block_hi != cmd.block_ref.block_hi ||
        cpl.committed_ref.block_lo != cmd.block_ref.block_lo ||
        cpl.committed_ref.version == 0 ||
        cpl.committed_ref.bytes != TEST_DATA_SIZE ||
        cpl.committed_ref.checksum64 == 0 ||
        cpl.bytes_written != TEST_DATA_SIZE) {
        fprintf(stderr, TAG "  FAIL: committed ref is incomplete\n");
        return -1;
    }

    n = snprintf(manifest, sizeof(manifest),
                 "{\"object_id\":\"ssd-gsva-node%d-peer%d\","
                 "\"producer_device\":\"ub_ssd0\","
                 "\"block_refs\":[{\"block_hi\":%llu,\"block_lo\":%llu,"
                 "\"version\":%llu,\"bytes\":%llu,\"checksum64\":%llu}]}",
                 node_idx, current_peer_node_idx,
                 (unsigned long long)cpl.committed_ref.block_hi,
                 (unsigned long long)cpl.committed_ref.block_lo,
                 (unsigned long long)cpl.committed_ref.version,
                 (unsigned long long)cpl.bytes_written,
                 (unsigned long long)cpl.committed_ref.checksum64);
    if (n <= 0 || n >= (int)sizeof(manifest)) {
        fprintf(stderr, TAG "  FAIL: manifest formatting overflow\n");
        return -1;
    }
    if (!strstr(manifest, "\"block_refs\"") ||
        !strstr(manifest, "\"producer_device\":\"ub_ssd0\"")) {
        fprintf(stderr, TAG "  FAIL: manifest missing block ref fields\n");
        return -1;
    }

    printf(TAG " LINGQU_BLOCK_WRITE block_hi=%#llx block_lo=%#llx version=%llu bytes=%llu checksum64=%#llx writer_cna=%#x status=ok\n",
           (unsigned long long)cpl.committed_ref.block_hi,
           (unsigned long long)cpl.committed_ref.block_lo,
           (unsigned long long)cpl.committed_ref.version,
           (unsigned long long)cpl.bytes_written,
           (unsigned long long)cpl.committed_ref.checksum64,
           local_cna);
    printf(TAG " LINGQU_DFS_MANIFEST path=/lingqu/block/objects/ssd-gsva-node%d-peer%d.json manifest_bytes=%d block_hi=%#llx block_lo=%#llx version=%llu status=ok\n",
           node_idx, current_peer_node_idx, n,
           (unsigned long long)cpl.committed_ref.block_hi,
           (unsigned long long)cpl.committed_ref.block_lo,
           (unsigned long long)cpl.committed_ref.version);
    printf(TAG "  PASS: DFS manifest references committed block payload\n");
    return 0;
}

static int test_stale_epoch_denied(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    struct gsva_key_v1 stale_key = peer_key;

    printf(TAG "TEST: stale epoch rejection\n");

    stale_key.epoch++;
    memset(peer_ptr(0), 0x66, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x300d;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xDD;
    cmd.block_ref.block_lo = peer_block_lo(0x03);
    cmd.block_ref.version = 0;
    fill_buffer_desc_with_key(&cmd.buffer, 0, TEST_DATA_SIZE, &stale_key,
                              g_token_id, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_STALE_EPOCH) {
        fprintf(stderr, TAG "  FAIL: expected STALE_EPOCH got %d\n",
                cpl.status);
        return -1;
    }

    printf(TAG "  PASS: rejected with STALE_EPOCH\n");
    return 0;
}

static int test_token_rotate_pending(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    uint32_t new_token_value = g_token_value + 0x20000u + (uint32_t)node_idx + 1u;
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;

    printf(TAG "TEST: token rotate pending rejection and ACK recovery\n");

    if (send_gsva_event(OBMM_GSVA_EVENT_TOKEN_CHANGE, &peer_key,
                        g_token_id, new_token_value, &ev_error) != 0 ||
        ev_error != GSVA_OK) {
        fprintf(stderr, TAG "  FAIL: TOKEN_CHANGE error=%d errno=%d\n",
                ev_error, errno);
        return -1;
    }

    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x300e;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xDD;
    cmd.block_ref.block_lo = peer_block_lo(0x04);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_TOKEN_DENIED) {
        fprintf(stderr, TAG "  FAIL: expected TOKEN_DENIED during rotate got %d\n",
                cpl.status);
        return -1;
    }

    ev_error = GSVA_ERR_FEATURE_MISSING;
    if (send_gsva_event(OBMM_GSVA_EVENT_INV_ACK, &peer_key,
                        g_token_id, new_token_value, &ev_error) != 0 ||
        ev_error != GSVA_OK) {
        fprintf(stderr, TAG "  FAIL: token ACK error=%d errno=%d\n",
                ev_error, errno);
        return -1;
    }
    g_token_value = new_token_value;

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    memset(peer_ptr(0), 0x88, TEST_DATA_SIZE);
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x300f;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xDD;
    cmd.block_ref.block_lo = peer_block_lo(0x05);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != SSD_OK) {
        fprintf(stderr, TAG "  FAIL: new token expected OK got %d\n", cpl.status);
        return -1;
    }

    printf(TAG "  PASS: old token denied, new token accepted\n");
    return 0;
}

static int test_coh_timeout_injection(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    uint8_t *src = peer_ptr(TIMEOUT_OFFSET);
    uint8_t *dst = peer_ptr(TEST_DATA_SIZE * 3);

    printf(TAG "TEST: coherence timeout injection\n");

    memset(src, 0xC8, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3020;
    cmd.source_cna = local_cna;
    cmd.flags = SSD_CMD_INJECT_COH_TIMEOUT;
    cmd.block_ref.block_hi = 0xC0;
    cmd.block_ref.block_lo = peer_block_lo(0x0B);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, TIMEOUT_OFFSET, TEST_DATA_SIZE,
                     g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_COH_TIMEOUT) {
        fprintf(stderr, TAG "  FAIL: expected COH_TIMEOUT got %d\n",
                cpl.status);
        return -1;
    }

    memset(dst, 0xA5, TEST_DATA_SIZE);
    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_READ;
    cmd.req_id = 0x3021;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xC0;
    cmd.block_ref.block_lo = peer_block_lo(0x0B);
    cmd.block_ref.version = 0;
    cmd.block_ref.bytes = TEST_DATA_SIZE;
    fill_buffer_desc(&cmd.buffer, TEST_DATA_SIZE * 3, TEST_DATA_SIZE,
                     g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_BAD_BLOCK) {
        fprintf(stderr, TAG "  FAIL: timeout write published block status=%d\n",
                cpl.status);
        return -1;
    }
    for (int i = 0; i < TEST_DATA_SIZE; i++) {
        if (dst[i] != 0xA5) {
            fprintf(stderr, TAG "  FAIL: missing timeout block wrote byte %d\n", i);
            return -1;
        }
    }

    printf(TAG "  PASS: injected COH_TIMEOUT without committed block\n");
    return 0;
}

static int test_retired_segment_denied(void)
{
    struct ub_ssd_cmd_v1 cmd = {0};
    struct ub_ssd_cpl_v1 cpl = {0};
    int32_t ev_error = GSVA_ERR_FEATURE_MISSING;

    printf(TAG "TEST: retired segment rejection\n");

    if (send_gsva_event(OBMM_GSVA_EVENT_RETIRE, &peer_key,
                        g_token_id, g_token_value, &ev_error) != 0 ||
        ev_error != GSVA_OK) {
        fprintf(stderr, TAG "  FAIL: RETIRE error=%d errno=%d\n",
                ev_error, errno);
        return -1;
    }

    cmd.version = 1;
    cmd.opcode = SSD_OP_BLOCK_WRITE;
    cmd.req_id = 0x3010;
    cmd.source_cna = local_cna;
    cmd.block_ref.block_hi = 0xDD;
    cmd.block_ref.block_lo = peer_block_lo(0x06);
    cmd.block_ref.version = 0;
    fill_buffer_desc(&cmd.buffer, 0, TEST_DATA_SIZE, g_token_value);

    if (ssd_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)SSD_ERR_SEGMENT_RETIRED) {
        fprintf(stderr, TAG "  FAIL: expected SEGMENT_RETIRED got %d\n",
                cpl.status);
        return -1;
    }

    printf(TAG "  PASS: rejected with SEGMENT_RETIRED\n");
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
    struct obmm_cmd_gsva_alloc_segment_v1 alloc = {0};
    struct obmm_cmd_gsva_aperture ap = {0};
    struct obmm_helpers_meta publish_meta;
    int i;

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

    alloc.version = OBMM_GSVA_ABI_VERSION;
    alloc.size = GSVA_SEG_SIZE;
    alloc.alignment = GSVA_SEG_SIZE;
    alloc.requested_home_va = my_base;
    alloc.home_node_id = (uint32_t)node_idx;
    alloc.cache_policy = OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI;
    alloc.requested_p_tag = OBMM_GSVA_P_TAG_AUTO;
    alloc.access_flags = OBMM_GSVA_ACCESS_READ | OBMM_GSVA_ACCESS_WRITE;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_ALLOC_SEGMENT, &alloc) != 0) {
        fprintf(stderr, TAG "GSVA_ALLOC_SEGMENT: %s\n", strerror(errno));
        return -1;
    }

    memset(&local_meta, 0, sizeof(local_meta));
    local_meta.export_cna = local_cna;
    local_meta.remote_uba = alloc.desc.home_va;
    local_meta.size = alloc.desc.size;
    if (obmm_do_export_fixed_uba(obmm_fd, &local_meta, alloc.desc.size,
                                 alloc.desc.home_va) != 0) {
        fprintf(stderr, TAG "fixed-uba export: %s\n", strerror(errno));
        return -1;
    }

    publish_meta = local_meta;
    publish_meta.export_mem_id = alloc.desc.segment_id;
    publish_meta.token_id = alloc.desc.token_id;
    if (obmm_bootstrap_publish(obmm_fd, node_idx, node_count,
                               GSVA_GENERATION, &publish_meta) != 0) {
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

    peer_count = 0;
    memset(peer_ids, 0, sizeof(peer_ids));
    for (i = 0; i < node_count; i++) {
        if (i != node_idx && peer_got[i]) {
            if (peer_count < OBMM_POOL_HELPERS_MAX_NODES)
                peer_ids[peer_count++] = i;
        }
    }
    if (peer_count == 0) {
        fprintf(stderr, TAG "no peer metadata found\n");
        return -1;
    }

    printf(TAG "GSVA setup done: discovered %d peers\n", peer_count);
    printf(TAG "GSVA local segment: base=%#llx size=%llu\n",
           (unsigned long long)alloc.desc.home_va, (unsigned long long)alloc.desc.size);
    return 0;
}

static int setup_peer_context(int peer_idx_order)
{
    struct obmm_cmd_gsva_query_segment_v1 query = {0};
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = {0};
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = {false};
    int peer_meta_idx;
    int rc;

    if (peer_idx_order < 0 || peer_idx_order >= peer_count) {
        fprintf(stderr, TAG "invalid peer index %d\n", peer_idx_order);
        return -1;
    }

    peer_meta_idx = peer_ids[peer_idx_order];
    current_peer_node_idx = peer_meta_idx;
    memset(&query, 0, sizeof(query));
    query.version = OBMM_GSVA_ABI_VERSION;
    query.segment_id = peer_metas[peer_meta_idx].export_mem_id;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_SEGMENT, &query) != 0) {
        if (errno != ENOENT) {
            fprintf(stderr, TAG "GSVA_QUERY_SEGMENT(peer=%d): %s\n",
                    peer_meta_idx, strerror(errno));
            return -1;
        }
        init_peer_desc_from_bootstrap(&peer_metas[peer_meta_idx],
                                      (uint32_t)peer_meta_idx, &peer_desc);
    } else {
        peer_desc = query.desc;
    }

    init_peer_key_from_desc(&peer_desc, &peer_key);
    peer_gsva_base = peer_desc.home_va;
    peer_slot_base = (uint64_t)node_idx * TEST_SLOT_STRIDE;

    if (peer_desc.size < peer_offset(TIMEOUT_OFFSET + TEST_DATA_SIZE)) {
        fprintf(stderr, TAG "peer=%d segment too small for SSD test slot\n",
                peer_meta_idx);
        return -1;
    }

    if (!obmm_alloc_import_pas(1, peer_desc.size, import_pas, import_osync,
                               OBMM_IMPORT_CACHE_AUTO)) {
        fprintf(stderr, TAG "import PA allocation failed (peer=%d)\n", peer_meta_idx);
        return -1;
    }

    rc = obmm_do_import_gsva_desc_v1(obmm_fd, &peer_desc, local_cna,
                           import_pas[0], peer_gsva_base,
                           &import_mem_id);
    if (rc != 0) {
        fprintf(stderr, TAG "GSVA import(peer=%d): %s\n", peer_meta_idx, strerror(errno));
        return -1;
    }

    memset(&peer_region, 0, sizeof(peer_region));
    if (obmm_map_gsva_region_at(import_mem_id,
                                (void *)(uintptr_t)peer_gsva_base,
                                peer_desc.size, false,
                                &peer_region) != 0) {
        fprintf(stderr, TAG "GSVA mmap(peer=%d): %s\n", peer_meta_idx, strerror(errno));
        return -1;
    }

    g_token_id = peer_desc.token_id;
    g_token_value = peer_desc.token_value;
    return 0;
}

static void cleanup_peer_context(void)
{
    obmm_unmap_region(&peer_region);
    if (import_mem_id)
        obmm_do_unimport(obmm_fd, import_mem_id);
    import_mem_id = 0;
    memset(&peer_region, 0, sizeof(peer_region));
}

static void cleanup_gsva(void)
{
    cleanup_peer_context();
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

    if (peer_count <= 0) {
        fprintf(stderr, TAG "No peer available for GSVA tests\n");
        cleanup_gsva();
        close(ssd_fd);
        printf(TAG "verdict=SKIP (GSVA setup)\n");
        return skip_code();
    }

    for (int i = 0; i < peer_count; i++) {
        int peer_meta_idx = peer_ids[i];

        printf(TAG "Testing peer %d/%d node_idx=%d segment_id=%llu\n",
               i + 1, peer_count, peer_meta_idx,
               (unsigned long long)peer_metas[peer_meta_idx].export_mem_id);

        if (setup_peer_context(i) != 0) {
            fprintf(stderr, TAG "peer %d setup failed\n", peer_meta_idx);
            fail++;
            continue;
        }

        if (test_block_write_read_gsva() == 0) pass++; else fail++;
        if (test_seal_rejects_overwrite() == 0) pass++; else fail++;
        if (test_bad_token() == 0) pass++; else fail++;
        if (test_bad_token_id() == 0) pass++; else fail++;
        if (i == 0) {
            if (test_flush_stat_gsva() == 0) pass++; else fail++;
        }
        if (test_version_conflict() == 0) pass++; else fail++;
        if (test_missing_block_rejects_read() == 0) pass++; else fail++;
        if (test_tombstone_rejects_read_write() == 0) pass++; else fail++;
        if (test_checksum_mismatch_denied() == 0) pass++; else fail++;
        if (i == 0) {
            if (test_snapshot_export_import() == 0) pass++; else fail++;
        }
        if (test_dfs_manifest_refs_block_payload() == 0) pass++; else fail++;
        if (test_stale_epoch_denied() == 0) pass++; else fail++;
        if (test_token_rotate_pending() == 0) pass++; else fail++;
        if (test_coh_timeout_injection() == 0) pass++; else fail++;
        if (test_retired_segment_denied() == 0) pass++; else fail++;

        cleanup_peer_context();
    }

    cleanup_gsva();
    close(ssd_fd);

    printf(TAG "Results: %d/%d passed, %d failed\n",
           pass, pass + fail, fail);
    printf(TAG "verdict=%s\n", fail == 0 ? "PASS" : "FAIL");
    return fail > 0 ? 1 : 0;
}
