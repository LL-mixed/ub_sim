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

static struct gsva_key_v1 peer_key;
static uint32_t g_token_id = 0;
static uint32_t g_token_value = 0;

static void init_peer_key_from_desc(const struct obmm_gsva_segment_desc_v1 *desc,
                                   struct gsva_key_v1 *key)
{
    memset(key, 0, sizeof(*key));
    key->version = desc->version;
    key->flags = desc->flags;
    key->segment_id = desc->segment_id;
    key->home_va = desc->home_va;
    key->size = desc->size;
    key->vmid = 0;
    key->asid = 0;
    key->pte_offset = desc->home_va;
    key->p_tag = desc->p_tag;
    key->cache_policy = desc->cache_policy;
    key->epoch = desc->epoch;
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
    desc->gsva_base = peer_gsva_base + offset;
    desc->bytes = bytes;
    desc->key = peer_key;
    desc->token_id = g_token_id;
    desc->token_value = token_value;
}

static int test_memcopy_gsva(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};
    uint64_t *src, *dst;
    int i;

    printf(TAG "TEST: NPU MEMCOPY via GSVA\n");

    src = (uint64_t *)((uint8_t *)peer_region.addr + OFF_MEMCOPY_IN);
    dst = (uint64_t *)((uint8_t *)peer_region.addr + OFF_MEMCOPY_OUT);

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

    data = (uint64_t *)((uint8_t *)peer_region.addr + OFF_FILL);
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
    int i;
    size_t vector_bytes = VECTOR_ELEMENT_COUNT * sizeof(uint32_t);

    printf(TAG "TEST: NPU VECTOR_ADD_U32 via GSVA\n");

    a = (uint32_t *)((uint8_t *)peer_region.addr + OFF_VEC_A);
    b = (uint32_t *)((uint8_t *)peer_region.addr + OFF_VEC_B);
    c = (uint32_t *)((uint8_t *)peer_region.addr + OFF_VEC_C);

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

        if (c[i] != want) {
            fprintf(stderr, TAG "  FAIL: vector[%d]=%#x want %#x (a=%#x b=%#x)\n",
                    i, c[i], want, a[i], b[i]);
            return -1;
        }
    }

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

    data = (uint64_t *)((uint8_t *)peer_region.addr + OFF_CHECKSUM);
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

    memset((uint8_t *)peer_region.addr + OFF_BADTK_IN, 0xAA, TEST_DATA_SIZE);

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

    printf(TAG "  PASS: rejected with TOKEN_DENIED\n");
    return 0;
}

static int test_bad_token_id(void)
{
    struct ub_npu_cmd_v1 cmd = {0};
    struct ub_npu_cpl_v1 cpl = {0};

    printf(TAG "TEST: bad token_id rejection\n");

    memset((uint8_t *)peer_region.addr + OFF_BADTK_IN, 0xBB, TEST_DATA_SIZE);

    cmd.version = 1;
    cmd.opcode = NPU_OP_MEMCOPY;
    cmd.req_id = 0x2006;
    cmd.source_cna = local_cna;
    cmd.desc_count = 2;
    fill_desc(&cmd.descs[0], NPU_BUF_INPUT, NPU_ACCESS_READ,
              OFF_BADTK_IN, TEST_DATA_SIZE, g_token_value);
    fill_desc(&cmd.descs[1], NPU_BUF_OUTPUT, NPU_ACCESS_WRITE,
              OFF_BADTK_OUT, TEST_DATA_SIZE, g_token_value);

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
    memset(&query, 0, sizeof(query));
    query.version = OBMM_GSVA_ABI_VERSION;
    query.segment_id = peer_metas[peer_meta_idx].export_mem_id;
    if (ioctl(obmm_fd, OBMM_CMD_GSVA_QUERY_SEGMENT, &query) != 0) {
        fprintf(stderr, TAG "GSVA_QUERY_SEGMENT(peer=%d): %s\n", peer_meta_idx, strerror(errno));
        return -1;
    }

    peer_desc = query.desc;
    init_peer_key_from_desc(&peer_desc, &peer_key);
    peer_gsva_base = query.desc.home_va;

    if (query.desc.size < (OFF_VEC_C + (uint64_t)VECTOR_ELEMENT_COUNT * 4)) {
        fprintf(stderr, TAG "peer=%d segment too small for vector test\n", peer_meta_idx);
        return -1;
    }

    if (!obmm_alloc_import_pas(1, query.desc.size, import_pas, import_osync,
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
                                query.desc.size, false,
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

        if (test_memcopy_gsva() == 0) pass++; else fail++;
        if (test_fill_gsva() == 0) pass++; else fail++;
        if (test_vector_add_u32_gsva() == 0) pass++; else fail++;
        if (test_checksum64_gsva() == 0) pass++; else fail++;
        if (test_bad_token() == 0) pass++; else fail++;
        if (test_bad_token_id() == 0) pass++; else fail++;

        cleanup_peer_context();
    }

    cleanup_gsva();
    close(npu_fd);

    printf(TAG "Results: %d/%d passed, %d failed\n",
           pass, pass + fail, fail);
    printf(TAG "verdict=%s\n", fail == 0 ? "PASS" : "FAIL");
    return fail > 0 ? 1 : 0;
}
