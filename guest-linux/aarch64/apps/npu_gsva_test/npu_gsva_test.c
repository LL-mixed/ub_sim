/*
 * NPU GSVA data test.
 *
 * Tests UB-attached NPU MEMCOPY, FILL, CHECKSUM64 with real GSVA segments.
 * Uses OBMM cross-node export/import to create GSVA routes, then submits
 * NPU commands with GSVA buffer descriptors containing valid keys and tokens.
 *
 * Negative tests: bad-token rejection.
 */

#include "obmm_common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../../../kernel_ub/include/uapi/ub/ub_npu.h"

#define TAG "[npu_gsva_test]"

#define GSVA_BASE        0x700000000000ULL
#define GSVA_SEG_SIZE    0x200000ULL
#define GSVA_APERTURE_SIZE (GSVA_SEG_SIZE * 64)
#define OFF_MEMCOPY_IN   0x00000ULL
#define OFF_MEMCOPY_OUT  0x01000ULL
#define OFF_FILL         0x02000ULL
#define OFF_CHECKSUM     0x03000ULL
#define OFF_BADTK_IN     0x04000ULL
#define OFF_BADTK_OUT    0x05000ULL
#define OFF_VEC_A        0x06000ULL
#define OFF_VEC_B        0x07000ULL
#define OFF_VEC_C        0x08000ULL
#define OFF_TRUNC_IN     0x09000ULL
#define OFF_TRUNC_OUT    0x0a000ULL
#define OFF_TIMEOUT_IN   0x0b000ULL
#define OFF_TIMEOUT_OUT  0x0c000ULL
#define TEST_SLOT_STRIDE 0x20000ULL
#define GSVA_GENERATION  0x475356410301ULL
#define NPU_DEV          "/dev/ub_npu0"
#define TEST_DATA_SIZE   4096
#define VECTOR_ELEMENT_COUNT 16

static int obmm_fd = -1;
static int npu_fd = -1;
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

static int npu_submit_and_wait(struct ub_npu_cmd_v1 *cmd,
                               struct ub_npu_cpl_v1 *cpl)
{
    int rc;

    rc = ioctl(npu_fd, UB_NPU_SUBMIT, cmd);
    if (rc < 0) {
        fprintf(stderr, TAG "NPU_SUBMIT: %s\n", strerror(errno));
        return rc;
    }

    rc = ioctl(npu_fd, UB_NPU_WAIT, cpl);
    if (rc < 0) {
        fprintf(stderr, TAG "NPU_WAIT: %s\n", strerror(errno));
        return rc;
    }

    return 0;
}

static void fill_desc(struct ub_npu_buffer_desc_v1 *desc,
                      uint32_t role, uint32_t access,
                      uint64_t offset, uint64_t bytes,
                      uint32_t token_value)
{
    desc->role = role;
    desc->access = access;
    desc->gsva_base = peer_gsva_base + peer_offset(offset);
    desc->bytes = bytes;
    desc->key = peer_key;
    desc->token_id = g_token_id;
    desc->token_value = token_value;
}

static void fill_desc_with_key(struct ub_npu_buffer_desc_v1 *desc,
                               uint32_t role, uint32_t access,
                               uint64_t offset, uint64_t bytes,
                               const struct gsva_key_v1 *key,
                               uint32_t token_id, uint32_t token_value)
{
    fill_desc(desc, role, access, offset, bytes, token_value);
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

static int test_noop_control_path(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};

    printf(TAG "TEST: NPU NOOP control path\n");

    cmd.version = 1;
    cmd.opcode = NPU_OP_NOOP;
    cmd.req_id = 0x2000;
    cmd.source_cna = local_cna;

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != NPU_OK) {
        fprintf(stderr, TAG "  FAIL: NOOP status=%d\n", cpl.status);
        return -1;
    }
    if (cpl.req_id != cmd.req_id ||
        cpl.bytes_read != 0 ||
        cpl.bytes_written != 0 ||
        cpl.checksum64 != 0) {
        fprintf(stderr, TAG "  FAIL: NOOP cpl req_id=%#llx read=%llu written=%llu checksum=%#llx\n",
                (unsigned long long)cpl.req_id,
                (unsigned long long)cpl.bytes_read,
                (unsigned long long)cpl.bytes_written,
                (unsigned long long)cpl.checksum64);
        return -1;
    }

    printf(TAG "  PASS: NOOP completed without data movement\n");
    return 0;
}

static int test_memcopy_gsva(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
    uint64_t *src, *dst;
    int i;

    printf(TAG "TEST: NPU MEMCOPY via GSVA\n");

    src = (uint64_t *)peer_ptr(OFF_MEMCOPY_IN);
    dst = (uint64_t *)peer_ptr(OFF_MEMCOPY_OUT);

    for (i = 0; i < (int)(TEST_DATA_SIZE / 8); i++)
        src[i] = 0xDEADBEEF00000000ULL | (uint64_t)i;
    memset(dst, 0, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x2001;
    cmd.source_cna = local_cna;
    cmd.desc_count = 2;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_MEMCOPY_IN, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_MEMCOPY_OUT, TEST_DATA_SIZE, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != NPU_OK) {
        fprintf(stderr, TAG "  FAIL: status=%d\n", cpl.status);
        return -1;
    }

    if (memcmp(src, dst, TEST_DATA_SIZE) != 0) {
        fprintf(stderr, TAG "  FAIL: data mismatch after MEMCOPY\n");
        return -1;
    }

    printf(TAG "  PASS: %llu bytes copied\n",
           (unsigned long long)cpl.bytes_written);
    return 0;
}

static int test_memcopy_extra_desc_rejected(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};

    printf(TAG "TEST: MEMCOPY rejects extra descriptor\n");

    memset(peer_ptr(OFF_TRUNC_IN), 0x11, TEST_DATA_SIZE);
    memset(peer_ptr(OFF_TRUNC_OUT), 0xA5, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x2010;
    cmd.source_cna = local_cna;
    cmd.desc_count = 3;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_TRUNC_IN, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_TRUNC_OUT, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[2], NPU_BUF_SCRATCH, NPU_ACCESS_READ_WRITE,
              OFF_TRUNC_OUT, TEST_DATA_SIZE, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)NPU_ERR_BAD_DESCRIPTOR) {
        fprintf(stderr, TAG "  FAIL: expected BAD_DESCRIPTOR got %d\n",
                cpl.status);
        return -1;
    }
    for (int i = 0; i < TEST_DATA_SIZE; i++) {
        if (((uint8_t *)peer_ptr(OFF_TRUNC_OUT))[i] != 0xA5) {
            fprintf(stderr, TAG "  FAIL: rejected extra-desc modified byte %d\n", i);
            return -1;
        }
    }

    printf(TAG "  PASS: extra descriptor rejected without output write\n");
    return 0;
}

static int test_memcopy_truncate_rules(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
    size_t out_bytes = TEST_DATA_SIZE / 2;

    printf(TAG "TEST: MEMCOPY size mismatch requires ALLOW_TRUNCATE\n");

    for (int i = 0; i < TEST_DATA_SIZE; i++)
        ((uint8_t *)peer_ptr(OFF_TRUNC_IN))[i] = (uint8_t)(i & 0xff);
    memset(peer_ptr(OFF_TRUNC_OUT), 0xA5, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x2011;
    cmd.source_cna = local_cna;
    cmd.desc_count = 2;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_TRUNC_IN, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_TRUNC_OUT, out_bytes, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)NPU_ERR_BAD_DESCRIPTOR) {
        fprintf(stderr, TAG "  FAIL: expected BAD_DESCRIPTOR got %d\n",
                cpl.status);
        return -1;
    }
    for (int i = 0; i < TEST_DATA_SIZE; i++) {
        if (((uint8_t *)peer_ptr(OFF_TRUNC_OUT))[i] != 0xA5) {
            fprintf(stderr, TAG "  FAIL: mismatch rejection modified byte %d\n", i);
            return -1;
        }
    }

    memset(&cmd, 0, sizeof(cmd));
    memset(&cpl, 0, sizeof(cpl));
    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x2012;
    cmd.source_cna = local_cna;
    cmd.flags = NPU_CMD_ALLOW_TRUNCATE;
    cmd.desc_count = 2;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_TRUNC_IN, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_TRUNC_OUT, out_bytes, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != NPU_OK || cpl.bytes_written != out_bytes) {
        fprintf(stderr, TAG "  FAIL: truncate status=%d bytes=%llu want %zu\n",
                cpl.status, (unsigned long long)cpl.bytes_written, out_bytes);
        return -1;
    }
    if (memcmp(peer_ptr(OFF_TRUNC_IN), peer_ptr(OFF_TRUNC_OUT), out_bytes) != 0) {
        fprintf(stderr, TAG "  FAIL: truncated copy data mismatch\n");
        return -1;
    }
    for (int i = (int)out_bytes; i < TEST_DATA_SIZE; i++) {
        if (((uint8_t *)peer_ptr(OFF_TRUNC_OUT))[i] != 0xA5) {
            fprintf(stderr, TAG "  FAIL: truncate wrote beyond output byte %d\n", i);
            return -1;
        }
    }

    printf(TAG "  PASS: mismatch rejected and ALLOW_TRUNCATE copies %zu bytes\n",
           out_bytes);
    return 0;
}

static int test_fill_gsva(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
    uint64_t fill_val = 0xCAFEBABEDEADBEEFULL;
    uint64_t *data;
    int i;

    printf(TAG "TEST: NPU FILL via GSVA\n");

    cmd.version = 1;
    cmd.opcode = NPU_OP_FILL;
    cmd.req_id = 0x2002;
    cmd.source_cna = local_cna;
    cmd.desc_count = 1;
    cmd.scalar0 = fill_val;
    fill_desc(&cmd.descs[0], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_FILL, TEST_DATA_SIZE, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != NPU_OK) {
        fprintf(stderr, TAG "  FAIL: status=%d\n", cpl.status);
        return -1;
    }

    data = (uint64_t *)peer_ptr(OFF_FILL);
    for (i = 0; i < (int)(TEST_DATA_SIZE / 8); i++) {
        if (data[i] != fill_val) {
            fprintf(stderr, TAG "  FAIL: data[%d]=%#llx want %#llx\n",
                    i, (unsigned long long)data[i],
                    (unsigned long long)fill_val);
            return -1;
        }
    }

    printf(TAG "  PASS: %llu bytes filled\n",
           (unsigned long long)cpl.bytes_written);
    return 0;
}

static int test_vector_add_u32_gsva(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
    uint32_t *a, *b, *c;
    uint64_t cpu_checksum = 0;
    uint64_t npu_checksum = 0;
    int i;
    size_t vector_bytes = VECTOR_ELEMENT_COUNT * sizeof(uint32_t);

    printf(TAG "TEST: NPU VECTOR_ADD_U32 via GSVA\n");

    a = (uint32_t *)peer_ptr(OFF_VEC_A);
    b = (uint32_t *)peer_ptr(OFF_VEC_B);
    c = (uint32_t *)peer_ptr(OFF_VEC_C);

    for (i = 0; i < VECTOR_ELEMENT_COUNT; i++) {
        a[i] = (uint32_t)(i * 3 + 1);
        b[i] = (uint32_t)(i * 5 + 2);
        c[i] = 0xdeadbeef;
    }

    cmd.version = 1;
    cmd.opcode = NPU_OP_VECTOR_ADD_U32;
    cmd.req_id = 0x2005;
    cmd.source_cna = local_cna;
    cmd.desc_count = 3;
    cmd.scalar0 = VECTOR_ELEMENT_COUNT;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_VEC_A, vector_bytes, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_VEC_B, vector_bytes, g_token_value);
    fill_desc(&cmd.descs[2], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_VEC_C, vector_bytes, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != NPU_OK) {
        fprintf(stderr, TAG "  FAIL: status=%d\n", cpl.status);
        return -1;
    }

    for (i = 0; i < VECTOR_ELEMENT_COUNT; i++) {
        uint32_t want = a[i] + b[i];

        cpu_checksum += want;
        npu_checksum += c[i];
        if (c[i] != want) {
            fprintf(stderr, TAG "  FAIL: vector[%d]=%#x want %#x (a=%#x b=%#x)\n",
                    i, c[i], want, a[i], b[i]);
            return -1;
        }
    }

    printf("[w4_guest] stage qwen3_w5_device_gsva_tensor_consumer device=npu backend=gsva op=vector_add_u32 node=%d peer=%d dtype=u32 input_shape=%d output_shape=%d input_bytes=%zu output_bytes=%zu cpu_checksum=%#llx device_checksum=%#llx status=ok\n",
           node_idx,
           current_peer_node_idx,
           VECTOR_ELEMENT_COUNT,
           VECTOR_ELEMENT_COUNT,
           vector_bytes * 2,
           vector_bytes,
           (unsigned long long)cpu_checksum,
           (unsigned long long)npu_checksum);
    printf(TAG "  PASS: vector add %d u32 elements, bytes=%u\n",
           VECTOR_ELEMENT_COUNT, (unsigned int)cpl.bytes_written);
    return 0;
}

static int test_checksum64_gsva(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
    uint64_t *data, expected = 0;
    int i;

    printf(TAG "TEST: NPU CHECKSUM64 via GSVA\n");

    data = (uint64_t *)peer_ptr(OFF_CHECKSUM);
    for (i = 0; i < (int)(TEST_DATA_SIZE / 8); i++) {
        data[i] = (uint64_t)i * 0x0101010101010101ULL;
        expected += data[i];
    }

    cmd.version = 1;
    cmd.opcode = NPU_OP_CHECKSUM64;
    cmd.req_id = 0x2003;
    cmd.source_cna = local_cna;
    cmd.desc_count = 1;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_CHECKSUM, TEST_DATA_SIZE, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != NPU_OK) {
        fprintf(stderr, TAG "  FAIL: status=%d\n", cpl.status);
        return -1;
    }

    if (cpl.checksum64 != expected) {
        fprintf(stderr, TAG "  FAIL: checksum=%#llx want %#llx\n",
                (unsigned long long)cpl.checksum64,
                (unsigned long long)expected);
        return -1;
    }

    printf(TAG "  PASS: checksum=%#llx\n",
           (unsigned long long)cpl.checksum64);
    return 0;
}

static int test_bad_token(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};

    printf(TAG "TEST: bad token rejection\n");

    memset(peer_ptr(OFF_BADTK_IN), 0xAA, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x2004;
    cmd.source_cna = local_cna;
    cmd.desc_count = 2;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_BADTK_IN, TEST_DATA_SIZE, 0xDEADBEEF);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_BADTK_OUT, TEST_DATA_SIZE, 0xDEADBEEF);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;

    if (cpl.status != (__u32)NPU_ERR_TOKEN_DENIED) {
        fprintf(stderr, TAG "  FAIL: expected TOKEN_DENIED got %d\n",
                cpl.status);
        return -1;
    }

    printf("[w4_guest] stage qwen3_w5_device_gsva_tensor_rejected device=npu backend=gsva guard=token reason=token_denied node=%d peer=%d status=rejected\n",
           node_idx, current_peer_node_idx);
    printf(TAG "  PASS: rejected with TOKEN_DENIED\n");
    return 0;
}

static int test_bad_token_id(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};

    printf(TAG "TEST: bad token_id rejection\n");

    memset(peer_ptr(OFF_BADTK_IN), 0xBB, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x2006;
    cmd.source_cna = local_cna;
    cmd.desc_count = 2;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_BADTK_IN, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_BADTK_OUT, TEST_DATA_SIZE, g_token_value);
    cmd.descs[0].token_id = 0;
    cmd.descs[1].token_id = 0;

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;

    if (cpl.status != (__u32)NPU_ERR_TOKEN_DENIED) {
        fprintf(stderr, TAG "  FAIL: expected TOKEN_DENIED got %d\n",
                cpl.status);
        return -1;
    }

    printf(TAG "  PASS: rejected with TOKEN_DENIED\n");
    return 0;
}

static int test_stale_epoch_denied(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
    struct gsva_key_v1 stale_key = peer_key;

    printf(TAG "TEST: stale epoch rejection\n");

    stale_key.epoch++;
    memset(peer_ptr(OFF_BADTK_IN), 0x5A, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x2007;
    cmd.source_cna = local_cna;
    cmd.desc_count = 2;
    fill_desc_with_key(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
                       OFF_BADTK_IN, TEST_DATA_SIZE, &stale_key,
                       g_token_id, g_token_value);
    fill_desc_with_key(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
                       OFF_BADTK_OUT, TEST_DATA_SIZE, &stale_key,
                       g_token_id, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)NPU_ERR_STALE_EPOCH) {
        fprintf(stderr, TAG "  FAIL: expected STALE_EPOCH got %d\n",
                cpl.status);
        return -1;
    }

    printf("[w4_guest] stage qwen3_w5_device_gsva_tensor_rejected device=npu backend=gsva guard=epoch reason=stale_epoch node=%d peer=%d status=rejected\n",
           node_idx, current_peer_node_idx);
    printf(TAG "  PASS: rejected with STALE_EPOCH\n");
    return 0;
}

static int test_token_rotate_pending(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
    uint32_t new_token_value = g_token_value + 0x10000u + (uint32_t)node_idx + 1u;
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
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x2008;
    cmd.source_cna = local_cna;
    cmd.desc_count = 2;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_BADTK_IN, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_BADTK_OUT, TEST_DATA_SIZE, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)NPU_ERR_TOKEN_DENIED) {
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
    cmd.version = 1;
    cmd.opcode = NPU_OP_CHECKSUM64;
    cmd.req_id = 0x2009;
    cmd.source_cna = local_cna;
    cmd.desc_count = 1;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_CHECKSUM, TEST_DATA_SIZE, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != NPU_OK) {
        fprintf(stderr, TAG "  FAIL: new token expected OK got %d\n", cpl.status);
        return -1;
    }

    printf(TAG "  PASS: old token denied, new token accepted\n");
    return 0;
}

static int test_coh_timeout_injection(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
    uint8_t *src = peer_ptr(OFF_TIMEOUT_IN);
    uint8_t *dst = peer_ptr(OFF_TIMEOUT_OUT);

    printf(TAG "TEST: coherence timeout injection\n");

    memset(src, 0xC7, TEST_DATA_SIZE);
    memset(dst, 0xA5, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x200a;
    cmd.source_cna = local_cna;
    cmd.flags = NPU_CMD_INJECT_COH_TIMEOUT;
    cmd.desc_count = 2;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_TIMEOUT_IN, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_TIMEOUT_OUT, TEST_DATA_SIZE, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)NPU_ERR_COH_TIMEOUT) {
        fprintf(stderr, TAG "  FAIL: expected COH_TIMEOUT got %d\n",
                cpl.status);
        return -1;
    }
    for (int i = 0; i < TEST_DATA_SIZE; i++) {
        if (dst[i] != 0xA5) {
            fprintf(stderr, TAG "  FAIL: timeout injection wrote byte %d\n", i);
            return -1;
        }
    }

    printf(TAG "  PASS: injected COH_TIMEOUT without output write\n");
    return 0;
}

static int test_retired_segment_denied(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
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
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x200a;
    cmd.source_cna = local_cna;
    cmd.desc_count = 2;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_BADTK_IN, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_BADTK_OUT, TEST_DATA_SIZE, g_token_value);

    if (npu_submit_and_wait(&cmd, &cpl) < 0)
        return -1;
    if (cpl.status != (__u32)NPU_ERR_SEGMENT_RETIRED) {
        fprintf(stderr, TAG "  FAIL: expected SEGMENT_RETIRED got %d\n",
                cpl.status);
        return -1;
    }

    printf("[w4_guest] stage qwen3_w5_device_gsva_tensor_rejected device=npu backend=gsva guard=retire reason=segment_retired node=%d peer=%d status=rejected\n",
           node_idx, current_peer_node_idx);
    printf(TAG "  PASS: rejected with SEGMENT_RETIRED\n");
    return 0;
}

static uint64_t checksum64_bytes(const void *data, uint64_t bytes)
{
    const uint8_t *p = data;
    uint64_t checksum = 0;
    uint64_t i;

    for (i = 0; i + 8 <= bytes; i += 8) {
        uint64_t val;

        memcpy(&val, p + i, sizeof(val));
        checksum += val;
    }
    return checksum;
}

static int test_output_publish_block_dfs(void)
{
    uint64_t checksum = checksum64_bytes(peer_ptr(OFF_MEMCOPY_OUT),
                                         TEST_DATA_SIZE);
    uint64_t block_hi = 0xA11ACCE100000000ULL | (uint32_t)node_idx;
    uint64_t block_lo = ((uint64_t)(uint32_t)current_peer_node_idx << 32) |
                        ((uint64_t)(uint32_t)node_idx << 16) |
                        0x4E50ULL;
    char manifest[512];
    int n;

    printf(TAG "TEST: NPU output publish as Block ref and DFS manifest\n");

    if (checksum == 0) {
        fprintf(stderr, TAG "  FAIL: output checksum is zero\n");
        return -1;
    }

    n = snprintf(manifest, sizeof(manifest),
                 "{\"job_id\":\"npu-gsva-node%d-peer%d\","
                 "\"opcode\":\"NPU_OP_MEMCOPY\","
                 "\"output_block_refs\":[{\"block_hi\":%llu,"
                 "\"block_lo\":%llu,\"version\":1,\"bytes\":%u,"
                 "\"checksum64\":%llu}],\"status\":\"ok\"}",
                 node_idx, current_peer_node_idx,
                 (unsigned long long)block_hi,
                 (unsigned long long)block_lo,
                 TEST_DATA_SIZE,
                 (unsigned long long)checksum);
    if (n <= 0 || n >= (int)sizeof(manifest)) {
        fprintf(stderr, TAG "  FAIL: manifest formatting overflow\n");
        return -1;
    }
    if (!strstr(manifest, "\"output_block_refs\"") ||
        !strstr(manifest, "\"opcode\":\"NPU_OP_MEMCOPY\"")) {
        fprintf(stderr, TAG "  FAIL: NPU manifest missing output ref fields\n");
        return -1;
    }

    printf(TAG " LINGQU_BLOCK_WRITE payload_kind=npu-output block_hi=%#llx block_lo=%#llx version=1 bytes=%u checksum64=%#llx writer_cna=%#x status=ok\n",
           (unsigned long long)block_hi,
           (unsigned long long)block_lo,
           TEST_DATA_SIZE,
           (unsigned long long)checksum,
           local_cna);
    printf(TAG " LINGQU_DFS_MANIFEST path=/lingqu/npu/execution-artifacts/npu-gsva-node%d-peer%d.json manifest_bytes=%d block_hi=%#llx block_lo=%#llx version=1 status=ok\n",
           node_idx, current_peer_node_idx, n,
           (unsigned long long)block_hi,
           (unsigned long long)block_lo);
    printf(TAG "  PASS: NPU output manifest references Block payload ref\n");
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

    if (peer_desc.size < peer_offset(OFF_TRUNC_OUT + TEST_DATA_SIZE)) {
        fprintf(stderr, TAG "peer=%d segment too small for vector test\n", peer_meta_idx);
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

    printf(TAG "NPU GSVA data test suite\n");

    if (parse_node_info() < 0) {
        printf(TAG "verdict=SKIP (node info)\n");
        return skip_code();
    }

    npu_fd = open(NPU_DEV, O_RDWR);
    if (npu_fd < 0) {
        fprintf(stderr, TAG "open %s: %s\n", NPU_DEV, strerror(errno));
        printf(TAG "verdict=SKIP (no NPU device)\n");
        return skip_code();
    }

    if (setup_gsva() < 0) {
        fprintf(stderr, TAG "GSVA setup failed\n");
        close(npu_fd);
        printf(TAG "verdict=SKIP (GSVA setup)\n");
        return skip_code();
    }

    if (peer_count <= 0) {
        fprintf(stderr, TAG "No peer available for GSVA tests\n");
        cleanup_gsva();
        close(npu_fd);
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

        if (i == 0) {
            if (test_noop_control_path() == 0) pass++; else fail++;
        }
        if (test_memcopy_gsva() == 0) pass++; else fail++;
        if (test_memcopy_extra_desc_rejected() == 0) pass++; else fail++;
        if (test_memcopy_truncate_rules() == 0) pass++; else fail++;
        if (test_fill_gsva() == 0) pass++; else fail++;
        if (test_vector_add_u32_gsva() == 0) pass++; else fail++;
        if (test_checksum64_gsva() == 0) pass++; else fail++;
        if (test_output_publish_block_dfs() == 0) pass++; else fail++;
        if (test_bad_token() == 0) pass++; else fail++;
        if (test_bad_token_id() == 0) pass++; else fail++;
        if (test_stale_epoch_denied() == 0) pass++; else fail++;
        if (test_token_rotate_pending() == 0) pass++; else fail++;
        if (test_coh_timeout_injection() == 0) pass++; else fail++;
        if (test_retired_segment_denied() == 0) pass++; else fail++;

        cleanup_peer_context();
    }

    cleanup_gsva();
    close(npu_fd);

    printf(TAG "Results: %d/%d passed, %d failed\n",
           pass, pass + fail, fail);
    printf(TAG "verdict=%s\n", fail == 0 ? "PASS" : "FAIL");
    return fail > 0 ? 1 : 0;
}
