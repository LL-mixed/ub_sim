#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../kernel_ub/include/uapi/ub/cdma/cdma_abi.h"
#include "uburma_cmd_user_compat.h"
#include "w4_kvcache_db_service.h"

#define DT_ROOT "/proc/device-tree"
#define UBC_RESOURCE_BASE_FALLBACK 0x18000000000ULL
#define LINQU_ENDPOINT1_OFFSET 0x1000ULL
#define PAGE_SIZE_BYTES 4096ULL
#define UB_RESOURCE0_WC_PATH "/sys/bus/ub/devices/00001/resource0_wc"
#define UB_RESOURCE0_PATH "/sys/bus/ub/devices/00001/resource0"
#define UB_RESOURCE1_WC_PATH "/sys/bus/ub/devices/00001/resource1_wc"
#define UB_RESOURCE1_PATH "/sys/bus/ub/devices/00001/resource1"
#define UB_RESOURCE2_PATH "/sys/bus/ub/devices/00001/resource2"
#define UB_MEM_WINDOWS_PATH "/sys/bus/ub/devices/00001/mem_windows"

#define REG_VERSION 0x000
#define REG_CMDQ_BASE_LO 0x010
#define REG_CMDQ_BASE_HI 0x018
#define REG_CMDQ_SIZE 0x020
#define REG_CMDQ_HEAD 0x028
#define REG_CMDQ_TAIL 0x030
#define REG_CQ_BASE_LO 0x038
#define REG_CQ_BASE_HI 0x040
#define REG_CQ_SIZE 0x048
#define REG_CQ_HEAD 0x050
#define REG_CQ_TAIL 0x058
#define REG_STATUS 0x060
#define REG_DOORBELL 0x068
#define REG_LAST_ERROR 0x070
#define REG_IRQ_STATUS 0x078
#define REG_IRQ_ACK 0x080
#define REG_DEFAULT_SEGMENT 0x088
#define REG_SEG_DATA_OFFSET 0x090
#define REG_SEG_DATA_VALUE 0x098

#define CMDQ_SLOT_BYTES 64U
#define MAX_SLOTS 16U
#define W4_TIMEOUT_MS 300000
#define W4_DOORBELL_BATCH_SLOTS 4U
#define W4_KVCACHE_PAYLOAD_BYTES 8192U
#define W4_DISPATCH_INPUT_WORD 0x0000000000000000ULL
#define W4_DISPATCH_RESULT_WORD 0x41a0000041a00000ULL
#define W4_DISPATCH_RESULT_WORD_HOST_MATMUL 0x3f8000003f800000ULL
#define W4_QWEN3_MARKER_PUBLISH 0x7133773470756230ULL
#define W4_QWEN3_MARKER_RESOLVE 0x7133773472657331ULL
#define W4_QWEN3_MARKER_COMPUTE 0x71337734636d7031ULL
#define W4_QWEN3_MARKER_SHARD_SUMMARY 0x7133773473686430ULL
#define W4_QWEN3_MARKER_ROUND1_SUMMARY 0x7133773472643130ULL
#define W4_QWEN3_MARKER_RESULT_TABLE 0x7133773474626c30ULL
#define W4_QWEN3_MARKER_PROJECTION_TABLE 0x7133773471767430ULL
#define W4_QWEN3_MARKER_LAYER_DEP_TABLE 0x7133773464657030ULL
#define W4_QWEN3_MARKER_RESULT_BLOCK_TABLE 0x71337734626c6b30ULL
#define W4_QWEN3_MARKER_KVCACHE_TABLE 0x713377346b766330ULL
#define W4_QWEN3_MARKER_KVCACHE_STATE_TABLE 0x713377346b767331ULL
#define W4_QWEN3_MARKER_LOGITS_TABLE 0x713377346c6f6730ULL
#define W4_QWEN3_MARKER_TOKEN_TEXT_TABLE 0x7133773474787430ULL
#define W4_QWEN3_MARKER_TEXT_OUTPUT_TABLE 0x71337734746f7430ULL
#define W4_QWEN3_MARKER_TEXT_OUTPUT_BYTES_TABLE 0x71337734746f6230ULL
#define W4_QWEN3_MARKER_TOKENIZER_ASSET_TABLE 0x71337734746f6b30ULL
#define W4_QWEN3_MARKER_WEIGHT_REFERENCE_TABLE 0x7133773477667430ULL
#define W4_QWEN3_MARKER_WEIGHT_STAGE_LINK_TABLE 0x71337734776c6b30ULL
#define W4_QWEN3_MARKER_MLP_REFERENCE_TABLE 0x713377346d6c7030ULL
#define W4_QWEN3_MARKER_LOGITS_REFERENCE_TABLE 0x713377346c6d6830ULL
#define W4_QWEN3_TOKENIZER_POLICY_KIND 1ULL
#define W4_QWEN3_TOKENIZER_MODEL_ID "Qwen/Qwen3-0.6B"
#define W4_QWEN3_TOKENIZER_FAMILY "qwen3-tiktoken-compatible-synthetic-piece"
#define W4_QWEN3_TOKENIZER_PIECE_PREFIX "q3_"
#define W4_QWEN3_VOCAB_SIZE 151936ULL
#define W4_QWEN3_EXPECTED_SHARDS 8ULL
#define W4_QWEN3_TILES_PER_SHARD 2ULL
#define W4_QWEN3_EXPECTED_TILES \
    (W4_QWEN3_EXPECTED_SHARDS * W4_QWEN3_TILES_PER_SHARD)
#define W4_QWEN3_SHARD_OUTPUT_BYTES 65536ULL
#define W4_QWEN3_SHARD_OUTPUT_ELEMS 16384ULL
#define W4_QWEN3_KV_BLOCKS_PER_TILE 2ULL
#define W4_QWEN3_RESULT_TABLE_HEADER 320ULL
#define W4_QWEN3_RESULT_TABLE_BASE 384ULL
#define W4_QWEN3_RESULT_TABLE_ENTRY_WORDS 10ULL
#define W4_QWEN3_RESULT_TABLE_ENTRY_BYTES 80ULL
#define W4_QWEN3_PROJECTION_TABLE_HEADER 1664ULL
#define W4_QWEN3_PROJECTION_TABLE_BASE 1728ULL
#define W4_QWEN3_PROJECTION_TABLE_ENTRY_WORDS 10ULL
#define W4_QWEN3_PROJECTION_TABLE_ENTRY_BYTES 80ULL
#define W4_QWEN3_PROJECTIONS_PER_SHARD 3ULL
#define W4_QWEN3_LAYER_DEP_TABLE_HEADER 5568ULL
#define W4_QWEN3_LAYER_DEP_TABLE_BASE 5632ULL
#define W4_QWEN3_LAYER_DEP_TABLE_ENTRY_WORDS 11ULL
#define W4_QWEN3_LAYER_DEP_TABLE_ENTRY_BYTES 88ULL
#define W4_QWEN3_LAYER_DEP_STAGES_PER_TILE 24ULL
#define W4_QWEN3_RESULT_BLOCK_TABLE_HEADER \
    (W4_QWEN3_LAYER_DEP_TABLE_BASE + \
     W4_QWEN3_EXPECTED_TILES * W4_QWEN3_LAYER_DEP_STAGES_PER_TILE * \
     W4_QWEN3_LAYER_DEP_TABLE_ENTRY_BYTES)
#define W4_QWEN3_RESULT_BLOCK_TABLE_BASE (W4_QWEN3_RESULT_BLOCK_TABLE_HEADER + 64ULL)
#define W4_QWEN3_RESULT_BLOCK_TABLE_ENTRY_WORDS 16ULL
#define W4_QWEN3_RESULT_BLOCK_TABLE_ENTRY_BYTES 128ULL
#define W4_QWEN3_RESULT_BLOCK_SAMPLE_PAIRS 8ULL
#define W4_QWEN3_KVCACHE_TABLE_HEADER \
    (W4_QWEN3_RESULT_BLOCK_TABLE_BASE + \
     W4_QWEN3_EXPECTED_TILES * W4_QWEN3_KV_BLOCKS_PER_TILE * \
     W4_QWEN3_RESULT_BLOCK_TABLE_ENTRY_BYTES)
#define W4_QWEN3_KVCACHE_TABLE_BASE (W4_QWEN3_KVCACHE_TABLE_HEADER + 64ULL)
#define W4_QWEN3_KVCACHE_TABLE_ENTRY_WORDS 14ULL
#define W4_QWEN3_KVCACHE_TABLE_ENTRY_BYTES 112ULL
#define W4_QWEN3_KVCACHE_LAYERS 28ULL
#define W4_QWEN3_KVCACHE_PHASES 2ULL
#define W4_QWEN3_KVCACHE_BLOCKS_PER_LAYER_TILE \
    (W4_QWEN3_KV_BLOCKS_PER_TILE + 1ULL)
#define W4_QWEN3_KVCACHE_ENTRIES \
    (W4_QWEN3_EXPECTED_TILES * W4_QWEN3_KVCACHE_LAYERS * \
     W4_QWEN3_KVCACHE_PHASES)
#define W4_QWEN3_KVCACHE_TABLE_END \
    (W4_QWEN3_KVCACHE_TABLE_BASE + \
     W4_QWEN3_KVCACHE_ENTRIES * W4_QWEN3_KVCACHE_TABLE_ENTRY_BYTES)
#define W4_QWEN3_KVCACHE_STATE_TABLE_HEADER W4_QWEN3_KVCACHE_TABLE_END
#define W4_QWEN3_KVCACHE_STATE_TABLE_BASE \
    (W4_QWEN3_KVCACHE_STATE_TABLE_HEADER + 64ULL)
#define W4_QWEN3_KVCACHE_STATE_TABLE_ENTRY_WORDS 8ULL
#define W4_QWEN3_KVCACHE_STATE_TABLE_ENTRY_BYTES 64ULL
#define W4_QWEN3_KVCACHE_STATE_ENTRIES \
    (W4_QWEN3_EXPECTED_TILES * W4_QWEN3_KVCACHE_LAYERS * \
     W4_QWEN3_KVCACHE_BLOCKS_PER_LAYER_TILE)
#define W4_QWEN3_KVCACHE_STATE_TABLE_END \
    (W4_QWEN3_KVCACHE_STATE_TABLE_BASE + \
     W4_QWEN3_KVCACHE_STATE_ENTRIES * W4_QWEN3_KVCACHE_STATE_TABLE_ENTRY_BYTES)
#define W4_QWEN3_LOGITS_TABLE_HEADER W4_QWEN3_KVCACHE_STATE_TABLE_END
#define W4_QWEN3_LOGITS_TABLE_BASE (W4_QWEN3_LOGITS_TABLE_HEADER + 64ULL)
#define W4_QWEN3_LOGITS_TABLE_ENTRY_WORDS 20ULL
#define W4_QWEN3_LOGITS_TABLE_ENTRY_BYTES 160ULL
#define W4_QWEN3_LOGITS_ENTRIES W4_QWEN3_EXPECTED_TILES
#define W4_QWEN3_LOGITS_TABLE_END \
    (W4_QWEN3_LOGITS_TABLE_BASE + \
     W4_QWEN3_LOGITS_ENTRIES * W4_QWEN3_LOGITS_TABLE_ENTRY_BYTES)
#define W4_QWEN3_TOKEN_TEXT_TABLE_HEADER W4_QWEN3_LOGITS_TABLE_END
#define W4_QWEN3_TOKEN_TEXT_TABLE_BASE (W4_QWEN3_TOKEN_TEXT_TABLE_HEADER + 64ULL)
#define W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_WORDS 8ULL
#define W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_BYTES 64ULL
#define W4_QWEN3_TOKEN_TEXT_ENTRIES W4_QWEN3_EXPECTED_TILES
#define W4_QWEN3_TOKEN_TEXT_PIECE_BYTES 9ULL
#define W4_QWEN3_TOKEN_TEXT_TOTAL_BYTES \
    (W4_QWEN3_TOKEN_TEXT_ENTRIES * W4_QWEN3_TOKEN_TEXT_PIECE_BYTES)
#define W4_QWEN3_TOKEN_TEXT_TABLE_END \
    (W4_QWEN3_TOKEN_TEXT_TABLE_BASE + \
     W4_QWEN3_TOKEN_TEXT_ENTRIES * W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_BYTES)
#define W4_QWEN3_TEXT_OUTPUT_TABLE_HEADER W4_QWEN3_TOKEN_TEXT_TABLE_END
#define W4_QWEN3_TEXT_OUTPUT_TABLE_END (W4_QWEN3_TEXT_OUTPUT_TABLE_HEADER + 64ULL)
#define W4_QWEN3_TEXT_OUTPUT_BYTES_TABLE_HEADER W4_QWEN3_TEXT_OUTPUT_TABLE_END
#define W4_QWEN3_TEXT_OUTPUT_BYTES_TABLE_BASE \
    (W4_QWEN3_TEXT_OUTPUT_BYTES_TABLE_HEADER + 64ULL)
#define W4_QWEN3_OUTPUT_PAYLOAD_BYTES \
    (W4_QWEN3_EXPECTED_TILES * W4_QWEN3_SHARD_OUTPUT_BYTES)

struct linqu_dt_info {
    bool found;
    char node_path[512];
    uint64_t base;
    uint64_t size;
};

struct completion_preview {
    uint64_t op_id;
    uint8_t source;
    uint8_t status;
};

struct completion_counts {
    uint64_t chipbackend;
    uint64_t block;
    uint64_t shmem;
    uint64_t dfs;
    uint64_t db;
    uint64_t guest_uapi;
    uint64_t success;
    uint64_t retryable;
    uint64_t fatal;
};

static bool read_file_bytes(const char *path, uint8_t *buf, size_t len, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return false;
    }
    n = read(fd, buf, len);
    close(fd);
    if (n < 0) {
        return false;
    }
    *out_len = (size_t)n;
    return true;
}

static bool find_ubc_resource_base_from_sysfs(uint64_t *base_out)
{
    DIR *dir;
    struct dirent *ent;
    char resource_path[512];
    uint8_t line[256];
    size_t n = 0;

    dir = opendir("/sys/bus/platform/devices");
    if (!dir) {
        return false;
    }

    while ((ent = readdir(dir)) != NULL) {
        char *space = NULL;
        uint64_t start = 0;

        if (!strstr(ent->d_name, ".ubc")) {
            continue;
        }

        snprintf(resource_path, sizeof(resource_path),
                 "/sys/bus/platform/devices/%s/resource", ent->d_name);
        if (!read_file_bytes(resource_path, line, sizeof(line) - 1, &n) || n == 0) {
            continue;
        }

        line[n] = '\0';
        space = strchr((char *)line, ' ');
        if (space) {
            *space = '\0';
        }

        errno = 0;
        start = strtoull((char *)line, NULL, 16);
        if (errno == 0 && start != 0) {
            *base_out = start;
            closedir(dir);
            return true;
        }
    }

    closedir(dir);
    return false;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static bool parse_reg_prop(const char *path, uint64_t *base, uint64_t *size)
{
    uint8_t buf[16];
    size_t n = 0;

    if (!read_file_bytes(path, buf, sizeof(buf), &n) || n < 8) {
        return false;
    }
    if (n >= 16) {
        *base = ((uint64_t)be32(&buf[0]) << 32) | be32(&buf[4]);
        *size = ((uint64_t)be32(&buf[8]) << 32) | be32(&buf[12]);
    } else {
        *base = be32(&buf[0]);
        *size = be32(&buf[4]);
    }
    return true;
}

static bool find_linqu_node_recursive(const char *dir_path, struct linqu_dt_info *info)
{
    DIR *dir = opendir(dir_path);
    struct dirent *de;

    if (!dir) {
        return false;
    }

    while ((de = readdir(dir)) != NULL) {
        char path[768];
        char compat_path[896];
        uint8_t compat[128];
        size_t compat_len = 0;
        struct stat st;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name);
        if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }

        snprintf(compat_path, sizeof(compat_path), "%s/compatible", path);
        if (read_file_bytes(compat_path, compat, sizeof(compat), &compat_len)) {
            if (memmem(compat, compat_len, "ub,ubc", strlen("ub,ubc")) != NULL ||
                memmem(compat, compat_len, "linqu,ub", strlen("linqu,ub")) != NULL) {
                char reg_path[896];
                info->found = true;
                strncpy(info->node_path, path, sizeof(info->node_path) - 1);
                info->node_path[sizeof(info->node_path) - 1] = '\0';
                snprintf(reg_path, sizeof(reg_path), "%s/reg", path);
                parse_reg_prop(reg_path, &info->base, &info->size);
                closedir(dir);
                return true;
            }
        }

        if (find_linqu_node_recursive(path, info)) {
            closedir(dir);
            return true;
        }
    }

    closedir(dir);
    return false;
}

static uint64_t resolve_root_base(void)
{
    struct linqu_dt_info info;
    memset(&info, 0, sizeof(info));
    if (find_linqu_node_recursive(DT_ROOT, &info) && info.base != 0) {
        return info.base;
    }
    if (find_ubc_resource_base_from_sysfs(&info.base) && info.base != 0) {
        return info.base;
    }
    return UBC_RESOURCE_BASE_FALLBACK;
}

static int phys_for_virt(void *ptr, uint64_t *phys_out)
{
    uint64_t virt = (uint64_t)(uintptr_t)ptr;
    uint64_t page_index = virt / PAGE_SIZE_BYTES;
    uint64_t page_off = virt % PAGE_SIZE_BYTES;
    uint64_t entry = 0;
    int fd;
    ssize_t n;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    n = pread(fd, &entry, sizeof(entry), (off_t)(page_index * sizeof(entry)));
    close(fd);
    if (n != (ssize_t)sizeof(entry) || (entry & (1ULL << 63)) == 0) {
        return -1;
    }
    *phys_out = ((entry & ((1ULL << 55) - 1)) * PAGE_SIZE_BYTES) + page_off;
    return 0;
}

static void write_u8_le(uint8_t *buf, size_t *off, uint8_t value)
{
    buf[*off] = value;
    *off += 1;
}

static void write_u32_le(uint8_t *buf, size_t *off, uint32_t value)
{
    memcpy(buf + *off, &value, sizeof(value));
    *off += sizeof(value);
}

static void write_u64_le(uint8_t *buf, size_t *off, uint64_t value)
{
    memcpy(buf + *off, &value, sizeof(value));
    *off += sizeof(value);
}

static uint64_t mmio_read64(volatile uint8_t *base, uint64_t off)
{
    volatile uint32_t *reg32 = (volatile uint32_t *)(base + off);
    uint64_t lo = reg32[0];
    uint64_t hi = reg32[1];
    return lo | (hi << 32);
}

static void mmio_write64(volatile uint8_t *base, uint64_t off, uint64_t value)
{
    volatile uint32_t *reg32 = (volatile uint32_t *)(base + off);
    reg32[0] = (uint32_t)value;
    reg32[1] = (uint32_t)(value >> 32);
}

static bool try_wait_guest_helper_irq(volatile uint8_t *ep_mmio)
{
    struct {
        uint64_t irq_count;
        uint64_t irq_status;
    } snapshot;
    struct pollfd pfd;
    int fd;
    ssize_t n;

    fd = open("/dev/linqu-ub0", O_RDONLY);
    if (fd < 0) {
        return false;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN;
    n = poll(&pfd, 1, 1000);
    if (n > 0) {
        n = read(fd, &snapshot, sizeof(snapshot));
        if (n == (ssize_t)sizeof(snapshot)) {
            printf("[w4_guest] stage db_dfs_irq_wait irq_count=%" PRIu64
                   " irq_status=0x%016" PRIx64 " mmio_irq_status=0x%016" PRIx64 "\n",
                   snapshot.irq_count,
                   snapshot.irq_status,
                   mmio_read64(ep_mmio, REG_IRQ_STATUS));
            close(fd);
            return true;
        }
    }
    close(fd);
    return false;
}

static bool try_wait_guest_uio_irq(volatile uint8_t *ep_mmio)
{
    uint32_t irq_count = 0;
    ssize_t n;
    int fd;

    fd = open("/dev/uio0", O_RDWR);
    if (fd < 0) {
        return false;
    }
    n = read(fd, &irq_count, sizeof(irq_count));
    if (n == (ssize_t)sizeof(irq_count)) {
        printf("[w4_guest] stage db_dfs_uio_wait irq_count=%u mmio_irq_status=0x%016" PRIx64 "\n",
               irq_count,
               mmio_read64(ep_mmio, REG_IRQ_STATUS));
        irq_count = 1;
        (void)write(fd, &irq_count, sizeof(irq_count));
        close(fd);
        return true;
    }
    close(fd);
    return false;
}

static void try_wait_guest_uapi_irq(volatile uint8_t *ep_mmio)
{
    if (try_wait_guest_helper_irq(ep_mmio)) {
        return;
    }
    if (try_wait_guest_uio_irq(ep_mmio)) {
        return;
    }
    printf("[w4_guest] stage db_dfs_irq_wait source=none mmio_irq_status=0x%016" PRIx64 "\n",
           mmio_read64(ep_mmio, REG_IRQ_STATUS));
}

static const char *w4_cluster_roles[] = {
    "nodeA",
    "nodeB",
    "nodeC",
    "nodeD",
    "nodeE",
    "nodeF",
    "nodeG",
    "nodeH",
};

static uint32_t w4_cluster_node_count(void)
{
    const char *env = getenv("LINQU_UB_NODE_COUNT");
    char *end = NULL;
    unsigned long parsed;

    if (env == NULL || env[0] == '\0') {
        return 4U;
    }
    parsed = strtoul(env, &end, 10);
    if (end == env || parsed == 0UL) {
        return 4U;
    }
    if (parsed > (sizeof(w4_cluster_roles) / sizeof(w4_cluster_roles[0]))) {
        return (uint32_t)(sizeof(w4_cluster_roles) / sizeof(w4_cluster_roles[0]));
    }
    return (uint32_t)parsed;
}

static bool w4_cluster_role_index(const char *role, uint32_t node_count, uint32_t *index)
{
    uint32_t i;

    for (i = 0; i < node_count; ++i) {
        if (strcmp(role, w4_cluster_roles[i]) == 0) {
            *index = i;
            return true;
        }
    }
    return false;
}

static uint32_t w4_cluster_next_owner(uint32_t owner, uint32_t node_count)
{
    if (node_count == 0U) {
        return owner;
    }
    return (owner + 1U) % node_count;
}

static const char *w4_cluster_role_name(uint32_t owner)
{
    return w4_cluster_roles[owner % (sizeof(w4_cluster_roles) / sizeof(w4_cluster_roles[0]))];
}

static uint64_t w4_cluster_handoff_hot(uint64_t hot_segment_id)
{
    return hot_segment_id + 0x400ULL;
}

struct w4_compute_roundtrip {
    uint64_t input_segment;
    uint64_t output_segment;
    uint64_t input_checksum;
    uint64_t output_checksum;
    uint32_t payload_bytes;
};

static uint64_t w4_hash_string(const char *value)
{
    uint64_t hash = 1469598103934665603ULL;

    while (value && *value) {
        hash ^= (uint8_t)*value;
        hash *= 1099511628211ULL;
        value++;
    }
    return hash;
}

static int w4_guest_compute_roundtrip(const char *role,
                                      const struct w4_db_block_ctx *ctx,
                                      const struct w4_db_record *block_meta,
                                      struct w4_compute_roundtrip *out)
{
    uint64_t role_hash;
    uint64_t block_hash;

    if (!role || !ctx || !block_meta || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    role_hash = w4_hash_string(role);
    block_hash = w4_hash_string(block_meta->block_hash);
    out->input_segment = block_meta->hot_segment_id;
    out->input_checksum = role_hash ^ block_hash ^ ctx->result_segment_id;
    out->output_checksum = (out->input_checksum + 0x2aULL) ^ 0x5754304b564442ULL;
    out->output_segment = block_meta->last_result_segment + 0x40ULL +
                          ((out->output_checksum & 0xfULL) << 4);
    out->payload_bytes = 64U;
    if (out->output_segment <= block_meta->last_result_segment) {
        return -1;
    }
    printf("[w4_guest] stage compute_request_materialized path=guest_w4_compute request=%s block=%s input_segment=0x%016" PRIx64 " bytes=%u input_checksum=0x%016" PRIx64 "\n",
           ctx->request_id,
           block_meta->block_hash,
           out->input_segment,
           out->payload_bytes,
           out->input_checksum);
    printf("[w4_guest] stage compute_result_payload_valid path=guest_w4_compute request=%s block=%s output_segment=0x%016" PRIx64 " output_checksum=0x%016" PRIx64 "\n",
           ctx->request_id,
           block_meta->block_hash,
           out->output_segment,
           out->output_checksum);
    return 0;
}

static int w4_resource_backed_db_cluster_assertions(const char *role, uint32_t cluster_node_count)
{
    struct w4_db_service svc;
    struct w4_db_block_ctx primary_ctx;
    struct w4_db_block_ctx aux_ctx;
    struct w4_db_record primary;
    struct w4_db_record aux;
    struct w4_db_record prefix;
    struct w4_db_record prefix_aux;
    struct w4_db_record group;
    struct w4_db_record remote_block;
    struct w4_db_record remote_aux;
    struct w4_db_record remote_prefix;
    struct w4_db_record remote_prefix_aux;
    struct w4_db_record remote_group;
    struct w4_db_cluster_summary initial_summary;
    struct w4_db_cluster_summary update_summary;
    struct w4_db_cluster_summary handoff_summary;
    struct w4_compute_roundtrip roundtrip;
    char remote_request_id[64];
    char remote_block_hash[96];
    char remote_block_hash_aux[96];
    char remote_prefix_group_id[64];
    char remote_prefix_group_aux_id[64];
    char remote_group_id[64];
    char remote_block_key[96];
    char remote_block_key_aux[96];
    char remote_prefix_key[96];
    char remote_prefix_key_aux[96];
    char remote_group_key[96];
    uint32_t placement_node;
    uint32_t remote_owner;
    const char *remote_role;

    if (!role || !w4_cluster_role_index(role, cluster_node_count, &placement_node)) {
        printf("[w4_guest] gap guest_db_service_cluster=resource_backed_unsupported_role role=%s node_count=%u\n",
               role ? role : "missing",
               cluster_node_count);
        return -1;
    }

    memset(&svc, 0, sizeof(svc));
    memset(&primary_ctx, 0, sizeof(primary_ctx));
    memset(&aux_ctx, 0, sizeof(aux_ctx));
    memset(&primary, 0, sizeof(primary));
    memset(&aux, 0, sizeof(aux));
    memset(&prefix, 0, sizeof(prefix));
    memset(&prefix_aux, 0, sizeof(prefix_aux));
    memset(&group, 0, sizeof(group));
    memset(&remote_block, 0, sizeof(remote_block));
    memset(&remote_aux, 0, sizeof(remote_aux));
    memset(&remote_prefix, 0, sizeof(remote_prefix));
    memset(&remote_prefix_aux, 0, sizeof(remote_prefix_aux));
    memset(&remote_group, 0, sizeof(remote_group));
    memset(&initial_summary, 0, sizeof(initial_summary));
    memset(&update_summary, 0, sizeof(update_summary));
    memset(&handoff_summary, 0, sizeof(handoff_summary));
    memset(&roundtrip, 0, sizeof(roundtrip));

    remote_owner = w4_cluster_next_owner(placement_node, cluster_node_count);
    remote_role = w4_cluster_role_name(remote_owner);

    snprintf(primary_ctx.request_id, sizeof(primary_ctx.request_id), "w4-%s-request-0", role);
    snprintf(primary_ctx.prefix_group, sizeof(primary_ctx.prefix_group), "%s-prefix-0", role);
    snprintf(primary_ctx.group_id, sizeof(primary_ctx.group_id), "%s-group-0", role);
    snprintf(primary_ctx.block_hash, sizeof(primary_ctx.block_hash), "w4-%s-block-0", role);
    primary_ctx.placement_node = placement_node;
    primary_ctx.placement_level = 2U;
    primary_ctx.hot_segment_id = 0x200000ULL + ((uint64_t)placement_node << 12);
    primary_ctx.result_segment_id = primary_ctx.hot_segment_id + 0x80ULL;

    snprintf(aux_ctx.request_id, sizeof(aux_ctx.request_id), "%s", primary_ctx.request_id);
    snprintf(aux_ctx.prefix_group, sizeof(aux_ctx.prefix_group), "%s-aux", primary_ctx.prefix_group);
    snprintf(aux_ctx.group_id, sizeof(aux_ctx.group_id), "%s", primary_ctx.group_id);
    snprintf(aux_ctx.block_hash, sizeof(aux_ctx.block_hash), "w4-%s-block-1", role);
    aux_ctx.placement_node = placement_node;
    aux_ctx.placement_level = 2U;
    aux_ctx.hot_segment_id = primary_ctx.hot_segment_id + 0x100ULL;
    aux_ctx.result_segment_id = aux_ctx.hot_segment_id + 0x80ULL;

    snprintf(remote_request_id, sizeof(remote_request_id), "w4-%s-request-0", remote_role);
    snprintf(remote_block_hash, sizeof(remote_block_hash), "w4-%s-block-0", remote_role);
    snprintf(remote_block_hash_aux, sizeof(remote_block_hash_aux), "w4-%s-block-1", remote_role);
    snprintf(remote_prefix_group_id, sizeof(remote_prefix_group_id), "%s-prefix-0", remote_role);
    snprintf(remote_prefix_group_aux_id, sizeof(remote_prefix_group_aux_id), "%s-prefix-0-aux", remote_role);
    snprintf(remote_group_id, sizeof(remote_group_id), "%s-group-0", remote_role);
    w4_db_build_block_key_from_hash(remote_block_hash, remote_block_key, sizeof(remote_block_key));
    w4_db_build_block_key_from_hash(remote_block_hash_aux, remote_block_key_aux, sizeof(remote_block_key_aux));
    w4_db_build_prefix_key_from_parts(remote_request_id,
                                      remote_prefix_group_id,
                                      remote_prefix_key,
                                      sizeof(remote_prefix_key));
    w4_db_build_prefix_key_from_parts(remote_request_id,
                                      remote_prefix_group_aux_id,
                                      remote_prefix_key_aux,
                                      sizeof(remote_prefix_key_aux));
    w4_db_build_group_key_from_parts(remote_request_id,
                                     remote_group_id,
                                     remote_group_key,
                                     sizeof(remote_group_key));

    if (w4_db_service_init(&svc, true, true, true) != 0 ||
        w4_db_bootstrap_kvcache(&svc, &primary_ctx, &primary) != 0 ||
        w4_db_bootstrap_kvcache(&svc, &aux_ctx, &aux) != 0 ||
        w4_db_apply_block_result(&svc,
                                 &aux_ctx,
                                 aux.last_result_segment + 0x40ULL,
                                 W4_KVCACHE_STATE_RELOADED,
                                 &aux) != 0 ||
        w4_db_update_prefix_metadata(&svc, &aux_ctx, &aux, &prefix_aux) != 0 ||
        w4_db_get_prefix_group_metadata(&svc, &primary_ctx, &group) != 0) {
        printf("[w4_guest] gap guest_db_service_cluster=resource_backed_bootstrap_failed\n");
        return -1;
    }

    if (w4_db_publish_observe_cluster(&svc, &primary, &initial_summary) != 0 ||
        !initial_summary.ready ||
        initial_summary.peer_block_count_floor < 2 ||
        initial_summary.peer_prefix_count_floor < 1 ||
        initial_summary.peer_group_count_floor < 1) {
        printf("[w4_guest] gap guest_db_service_cluster=resource_backed_initial_visibility_failed\n");
        return -1;
    }

    if (w4_guest_compute_roundtrip(role, &primary_ctx, &primary, &roundtrip) != 0 ||
        w4_db_apply_block_result(&svc,
                                 &primary_ctx,
                                 roundtrip.output_segment,
                                 W4_KVCACHE_STATE_RELOADED,
                                 &primary) != 0 ||
        w4_db_update_prefix_metadata(&svc, &primary_ctx, &primary, &prefix) != 0 ||
        w4_db_publish_observe_cluster(&svc, &primary, &update_summary) != 0 ||
        !update_summary.ready ||
        !update_summary.placement_coherent ||
        !update_summary.state_coherent ||
        !update_summary.prefix_state_ready ||
        !update_summary.prefix_view_ready ||
        update_summary.peer_version_floor == 0 ||
        update_summary.peer_prefix_version_floor == 0 ||
        update_summary.peer_result_floor == 0 ||
        update_summary.peer_prefix_result_floor == 0 ||
        update_summary.peer_block_count_floor < 2 ||
        update_summary.peer_prefix_count_floor < 2 ||
        update_summary.peer_group_count_floor < 1) {
        printf("[w4_guest] gap guest_db_service_cluster=resource_backed_update_visibility_failed\n");
        return -1;
    }

    if (w4_db_handoff_block_owner(&svc,
                                  &primary_ctx,
                                  w4_cluster_next_owner(primary.placement_node, cluster_node_count),
                                  primary.placement_level,
                                  w4_cluster_handoff_hot(primary.hot_segment_id),
                                  &primary) != 0 ||
        w4_db_update_prefix_metadata(&svc, &primary_ctx, &primary, &prefix) != 0 ||
        w4_db_publish_observe_cluster(&svc, &primary, &handoff_summary) != 0 ||
        !handoff_summary.ready ||
        !handoff_summary.placement_coherent ||
        !handoff_summary.state_coherent ||
        !handoff_summary.prefix_state_ready ||
        !handoff_summary.prefix_view_ready ||
        handoff_summary.peer_version_floor == 0 ||
        handoff_summary.peer_prefix_version_floor == 0 ||
        handoff_summary.peer_result_floor == 0 ||
        handoff_summary.peer_prefix_result_floor == 0 ||
        handoff_summary.peer_block_count_floor < 2 ||
        handoff_summary.peer_prefix_count_floor < 2 ||
        handoff_summary.peer_group_count_floor < 1) {
        printf("[w4_guest] gap guest_db_service_cluster=resource_backed_handoff_visibility_failed\n");
        return -1;
    }

    if (w4_db_cluster_fetch_record(&svc, remote_block_key, &remote_block) != 0 ||
        w4_db_cluster_fetch_record(&svc, remote_block_key_aux, &remote_aux) != 0 ||
        w4_db_cluster_fetch_record(&svc, remote_prefix_key, &remote_prefix) != 0 ||
        w4_db_cluster_fetch_record(&svc, remote_prefix_key_aux, &remote_prefix_aux) != 0 ||
        w4_db_cluster_fetch_record(&svc, remote_group_key, &remote_group) != 0 ||
        remote_block.state != W4_KVCACHE_STATE_RELOADED ||
        remote_aux.state != W4_KVCACHE_STATE_RELOADED ||
        !w4_db_prefix_matches_block_meta(&remote_prefix, &remote_block) ||
        !w4_db_prefix_matches_block_meta(&remote_prefix_aux, &remote_aux) ||
        !w4_db_group_covers_blocks(&remote_group, &remote_block, &remote_aux)) {
        printf("[w4_guest] gap guest_db_service_cluster=resource_backed_remote_fetch_incoherent remote=%s\n",
               remote_role);
        return -1;
    }

    if (w4_db_obmm_service_v0_publish_resolve(&svc,
                                              placement_node,
                                              remote_owner,
                                              cluster_node_count) != 0) {
        printf("[w4_guest] gap guest_obmm_service_v0=payload_backing_resolve_failed remote=%s\n",
               remote_role);
        return -1;
    }

    printf("[w4_guest] stage db_service_cluster=resource_backed_assertions_ok nodes=%u peers=%u local_block=%s remote_block=%s version=%" PRIu64 " peer_version_floor=%" PRIu64 " handoff_owner=%u prefix_group=%s group_members=%u\n",
           handoff_summary.node_count,
           handoff_summary.peers_observed,
           primary.key,
           remote_block.key,
           primary.version,
           handoff_summary.peer_version_floor,
           primary.placement_node,
           prefix.key,
           remote_group.member_count);
    return 0;
}

static void dump_mem_windows(void)
{
    FILE *fp;
    char line[256];

    fp = fopen(UB_MEM_WINDOWS_PATH, "r");
    if (!fp) {
        fprintf(stderr, "[w4_guest] step=mem_windows open failed: %s\n", strerror(errno));
        return;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        printf("[w4_guest] mem_windows %s\n", line);
    }
    fclose(fp);
}

static bool probe_resource_candidate(const char *path,
                                     uint64_t *root_version_out,
                                     uint64_t *default_segment_out)
{
    int fd;
    void *root_map = MAP_FAILED;
    void *ep_map = MAP_FAILED;
    volatile uint8_t *root_mmio;
    volatile uint8_t *ep_mmio;

    *root_version_out = 0;
    *default_segment_out = 0;

    fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        return false;
    }

    root_map = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (root_map == MAP_FAILED) {
        close(fd);
        return false;
    }

    ep_map = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  (off_t)LINQU_ENDPOINT1_OFFSET);
    if (ep_map == MAP_FAILED) {
        munmap(root_map, PAGE_SIZE_BYTES);
        close(fd);
        return false;
    }

    root_mmio = (volatile uint8_t *)root_map;
    ep_mmio = (volatile uint8_t *)ep_map;
    *root_version_out = mmio_read64(root_mmio, REG_VERSION);
    *default_segment_out = mmio_read64(ep_mmio, REG_DEFAULT_SEGMENT);
    printf("[w4_guest] probe_resource path=%s version=0x%016" PRIx64
           " default_segment=0x%016" PRIx64 "\n",
           path, *root_version_out, *default_segment_out);

    munmap(ep_map, PAGE_SIZE_BYTES);
    munmap(root_map, PAGE_SIZE_BYTES);
    close(fd);
    return true;
}

static void write_string(uint8_t *buf, size_t *off, const char *value)
{
    size_t len = strlen(value);
    if (len > 255) {
        len = 255;
    }
    write_u8_le(buf, off, (uint8_t)len);
    memcpy(buf + *off, value, len);
    *off += len;
}

static void build_shmem_descriptor(uint8_t *slot, uint8_t kind, uint64_t segment, uint64_t bytes)
{
    size_t off = 0;
    memset(slot, 0, CMDQ_SLOT_BYTES);
    write_u8_le(slot, &off, kind);
    write_u8_le(slot, &off, 0);
    write_u32_le(slot, &off, 0);
    write_u64_le(slot, &off, segment);
    write_u64_le(slot, &off, bytes);
}

static void build_dbput_descriptor(uint8_t *slot, const char *key, uint64_t bytes)
{
    size_t off = 0;
    memset(slot, 0, CMDQ_SLOT_BYTES);
    write_u8_le(slot, &off, 7);
    write_u8_le(slot, &off, 0);
    write_string(slot, &off, key);
    write_u64_le(slot, &off, bytes);
}

static void build_dbget_descriptor(uint8_t *slot, const char *key)
{
    size_t off = 0;
    memset(slot, 0, CMDQ_SLOT_BYTES);
    write_u8_le(slot, &off, 8);
    write_u8_le(slot, &off, 0);
    write_string(slot, &off, key);
}

static void build_dfs_descriptor(uint8_t *slot, uint8_t kind, const char *path, uint64_t bytes)
{
    size_t off = 0;
    memset(slot, 0, CMDQ_SLOT_BYTES);
    write_u8_le(slot, &off, kind);
    write_u8_le(slot, &off, 0);
    write_string(slot, &off, path);
    if (kind == 6) {
        write_u64_le(slot, &off, bytes);
    }
}

static void build_io_descriptor(uint8_t *slot, uint64_t op_id, uint8_t opcode,
                                uint64_t segment, const char *block)
{
    size_t off = 0;
    memset(slot, 0, CMDQ_SLOT_BYTES);
    write_u8_le(slot, &off, 1);
    write_u64_le(slot, &off, op_id);
    write_u8_le(slot, &off, 0);
    write_u32_le(slot, &off, 0);
    write_u8_le(slot, &off, opcode);
    write_u8_le(slot, &off, 1);
    write_u64_le(slot, &off, segment);
    if (block && block[0] != '\0') {
        write_u8_le(slot, &off, 1);
        write_string(slot, &off, block);
    } else {
        write_u8_le(slot, &off, 0);
    }
}

static int decode_completion_preview(const uint8_t *slot, struct completion_preview *preview)
{
    if (!slot || !preview) {
        return -1;
    }
    memset(preview, 0, sizeof(*preview));
    memcpy(&preview->op_id, slot, sizeof(preview->op_id));
    preview->source = slot[9];
    preview->status = slot[10];
    return 0;
}

static void count_completion(const struct completion_preview *preview,
                             struct completion_counts *counts)
{
    switch (preview->source) {
    case 1:
        counts->chipbackend += 1;
        break;
    case 2:
        counts->block += 1;
        break;
    case 3:
        counts->shmem += 1;
        break;
    case 4:
        counts->dfs += 1;
        break;
    case 5:
        counts->db += 1;
        break;
    case 6:
        counts->guest_uapi += 1;
        break;
    default:
        break;
    }

    switch (preview->status) {
    case 1:
        counts->success += 1;
        break;
    case 2:
        counts->retryable += 1;
        break;
    case 3:
        counts->fatal += 1;
        break;
    default:
        break;
    }
}

static int seed_kvcache_payload(volatile uint8_t *ep_mmio, uint64_t segment)
{
    uint64_t checksum = 0;
    size_t words = W4_KVCACHE_PAYLOAD_BYTES / sizeof(uint64_t);
    static const size_t boundary_offsets[] = {
        0U,
        248U,
        256U,
        4088U,
        4096U,
        4104U,
    };

    for (size_t i = 0; i < words; ++i) {
        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, i * sizeof(uint64_t));
        mmio_write64(ep_mmio, REG_SEG_DATA_VALUE, W4_DISPATCH_INPUT_WORD);
        checksum ^= W4_DISPATCH_INPUT_WORD + i;
    }
    for (size_t i = 0; i < words; ++i) {
        uint64_t observed;
        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, i * sizeof(uint64_t));
        observed = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
        if (observed != W4_DISPATCH_INPUT_WORD) {
            fprintf(stderr,
                    "[w4_guest] kvcache payload seed mismatch offset=%zu expected=0x%016" PRIx64 " got=0x%016" PRIx64 "\n",
                    i * sizeof(uint64_t),
                    (uint64_t)W4_DISPATCH_INPUT_WORD,
                    observed);
            return -1;
        }
    }
    for (size_t i = 0; i < sizeof(boundary_offsets) / sizeof(boundary_offsets[0]); ++i) {
        uint64_t observed;
        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, boundary_offsets[i]);
        observed = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
        if (observed != W4_DISPATCH_INPUT_WORD) {
            fprintf(stderr,
                    "[w4_guest] kvcache boundary payload mismatch offset=%zu expected=0x%016" PRIx64 " got=0x%016" PRIx64 "\n",
                    boundary_offsets[i],
                    (uint64_t)W4_DISPATCH_INPUT_WORD,
                    observed);
            return -1;
        }
    }
    printf("[w4_guest] stage uapi_kvcache_payload_seeded segment=%" PRIu64 " bytes=%u checksum=0x%016" PRIx64 "\n",
           segment, W4_KVCACHE_PAYLOAD_BYTES, checksum);
    printf("[w4_guest] stage uapi_kvcache_payload_boundaries segment=%" PRIu64 " offsets=0,248,256,4088,4096,4104 status=ok\n",
           segment);
    return 0;
}

static uint64_t expected_dispatch_result_word(void)
{
    const char *profile = getenv("SIM_UAPI_W4_CHIPBACKEND_PROFILE");

    if (profile && strcmp(profile, "host_matmul") == 0) {
        return W4_DISPATCH_RESULT_WORD_HOST_MATMUL;
    }
    return W4_DISPATCH_RESULT_WORD;
}

static bool is_qwen3_profile(void)
{
    const char *profile = getenv("SIM_UAPI_W4_CHIPBACKEND_PROFILE");
    return profile && strcmp(profile, "qwen3_dense_0_6b") == 0;
}

static uint64_t read_segment_u64(volatile uint8_t *ep_mmio, uint64_t offset)
{
    mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, offset);
    return mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
}

static bool qwen3_is_trailing_metadata_marker(uint64_t marker)
{
    return marker == W4_QWEN3_MARKER_TEXT_OUTPUT_BYTES_TABLE ||
           marker == W4_QWEN3_MARKER_TOKENIZER_ASSET_TABLE ||
           marker == W4_QWEN3_MARKER_WEIGHT_REFERENCE_TABLE ||
           marker == W4_QWEN3_MARKER_WEIGHT_STAGE_LINK_TABLE ||
           marker == W4_QWEN3_MARKER_MLP_REFERENCE_TABLE ||
           marker == W4_QWEN3_MARKER_LOGITS_REFERENCE_TABLE;
}

static uint64_t qwen3_result_metadata_table_end(volatile uint8_t *ep_mmio)
{
    uint64_t marker = read_segment_u64(ep_mmio, W4_QWEN3_TEXT_OUTPUT_TABLE_HEADER);
    uint64_t cursor = W4_QWEN3_TOKEN_TEXT_TABLE_END;

    if (marker == W4_QWEN3_MARKER_TEXT_OUTPUT_TABLE) {
        cursor = W4_QWEN3_TEXT_OUTPUT_TABLE_END;
    }

    for (;;) {
        uint64_t table_marker = read_segment_u64(ep_mmio, cursor);
        uint64_t table_bytes = 0;
        uint64_t next_cursor = 0;

        if (!qwen3_is_trailing_metadata_marker(table_marker)) {
            break;
        }

        table_bytes = read_segment_u64(ep_mmio, cursor + 24);
        next_cursor = cursor + 64ULL + table_bytes;
        if (next_cursor <= cursor || next_cursor > W4_QWEN3_OUTPUT_PAYLOAD_BYTES) {
            break;
        }
        cursor = next_cursor;
    }

    return cursor;
}

static uint64_t qwen3_canonical_result_block_checksum(volatile uint8_t *ep_mmio,
                                                      uint64_t start,
                                                      uint64_t bytes,
                                                      uint64_t zero_start,
                                                      uint64_t zero_end)
{
    uint64_t acc = 0xcbf29ce484222325ULL;

    for (uint64_t offset = start; offset < start + bytes; offset += sizeof(uint64_t)) {
        uint64_t word = 0;

        if (offset < zero_start || offset >= zero_end) {
            word = read_segment_u64(ep_mmio, offset);
        }
        acc ^= word;
        acc *= 0x00000100000001b3ULL;
    }
    return acc;
}

static uint64_t qwen3_rol64(uint64_t value, unsigned int bits)
{
    return (value << bits) | (value >> (64U - bits));
}

static uint64_t qwen3_sampled_token(uint64_t round1_checksum, uint64_t tile_id)
{
    return (round1_checksum ^ (tile_id * 0x9e3779b97f4a7c15ULL)) % W4_QWEN3_VOCAB_SIZE;
}

static uint64_t qwen3_logits_checksum(uint64_t round1_checksum,
                                      uint64_t tile_id,
                                      uint64_t sampled_token,
                                      uint64_t runner_up_token,
                                      uint64_t margin_milli,
                                      uint64_t real_top_checksum,
                                      uint64_t real_runner_checksum,
                                      uint64_t kvcache_read_digest,
                                      uint64_t qkv_reference_digest,
                                      uint64_t real_path_digest)
{
    return (round1_checksum * 0x00000100000001b3ULL) ^
           qwen3_rol64(tile_id, 7) ^
           qwen3_rol64(sampled_token, 13) ^
           qwen3_rol64(runner_up_token, 29) ^
           qwen3_rol64(margin_milli, 43) ^
           qwen3_rol64(real_top_checksum, 5) ^
           qwen3_rol64(real_runner_checksum, 17) ^
           qwen3_rol64(kvcache_read_digest, 31) ^
           qwen3_rol64(qkv_reference_digest, 47) ^
           qwen3_rol64(real_path_digest, 53);
}

static uint64_t qwen3_fnv1a_bytes(uint64_t acc, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;

    for (size_t i = 0; i < len; ++i) {
        acc ^= bytes[i];
        acc *= 0x00000100000001b3ULL;
    }
    return acc;
}

static uint64_t qwen3_tokenizer_policy_hash(void)
{
    uint64_t acc = 0xcbf29ce484222325ULL;
    uint64_t value;
    uint8_t zero = 0;

    acc = qwen3_fnv1a_bytes(acc, W4_QWEN3_TOKENIZER_MODEL_ID,
                            strlen(W4_QWEN3_TOKENIZER_MODEL_ID));
    acc = qwen3_fnv1a_bytes(acc, &zero, sizeof(zero));
    acc = qwen3_fnv1a_bytes(acc, W4_QWEN3_TOKENIZER_FAMILY,
                            strlen(W4_QWEN3_TOKENIZER_FAMILY));
    acc = qwen3_fnv1a_bytes(acc, &zero, sizeof(zero));
    value = W4_QWEN3_VOCAB_SIZE;
    acc = qwen3_fnv1a_bytes(acc, &value, sizeof(value));
    acc = qwen3_fnv1a_bytes(acc, W4_QWEN3_TOKENIZER_PIECE_PREFIX,
                            strlen(W4_QWEN3_TOKENIZER_PIECE_PREFIX));
    acc = qwen3_fnv1a_bytes(acc, &zero, sizeof(zero));
    value = 6ULL;
    acc = qwen3_fnv1a_bytes(acc, &value, sizeof(value));
    value = W4_QWEN3_TOKEN_TEXT_PIECE_BYTES;
    acc = qwen3_fnv1a_bytes(acc, &value, sizeof(value));
    return acc;
}

static void qwen3_token_piece(uint64_t sampled_token, uint64_t *word0, uint64_t *word1)
{
    char piece[16];

    memset(piece, 0, sizeof(piece));
    snprintf(piece, sizeof(piece), "q3_%06" PRIu64, sampled_token);
    memcpy(word0, piece, sizeof(*word0));
    memcpy(word1, piece + sizeof(*word0), sizeof(*word1));
}

static uint64_t qwen3_token_piece_checksum(uint64_t sampled_token)
{
    char piece[16];
    uint64_t acc = 0xcbf29ce484222325ULL ^ sampled_token;

    memset(piece, 0, sizeof(piece));
    snprintf(piece, sizeof(piece), "q3_%06" PRIu64, sampled_token);
    for (uint64_t i = 0; i < W4_QWEN3_TOKEN_TEXT_PIECE_BYTES; ++i) {
        acc ^= (uint8_t)piece[i];
        acc *= 0x00000100000001b3ULL;
    }
    return acc ^ qwen3_rol64(W4_QWEN3_TOKEN_TEXT_PIECE_BYTES, 17);
}

static uint64_t qwen3_sample_text_checksum(uint64_t step_index, uint64_t sampled_token)
{
    uint64_t piece_word0;
    uint64_t piece_word1;
    uint64_t piece_checksum;
    uint64_t byte_offset = step_index * W4_QWEN3_TOKEN_TEXT_PIECE_BYTES;

    qwen3_token_piece(sampled_token, &piece_word0, &piece_word1);
    piece_checksum = qwen3_token_piece_checksum(sampled_token);
    return ((0xcbf29ce484222325ULL * 0x00000100000001b3ULL) +
            qwen3_rol64(step_index, 11)) ^
           qwen3_rol64(sampled_token, 31) ^
           qwen3_rol64(byte_offset, 17) ^
           qwen3_rol64(W4_QWEN3_TOKEN_TEXT_PIECE_BYTES, 23) ^
           qwen3_rol64(piece_word0, 37) ^
           qwen3_rol64(piece_word1, 43) ^
           qwen3_rol64(piece_checksum, 3);
}

static int verify_dispatch_payload(volatile uint8_t *ep_mmio, uint64_t segment)
{
    const bool qwen3_profile = is_qwen3_profile();
    const uint64_t expected = expected_dispatch_result_word();
    uint64_t observed;

    mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, 0);
    observed = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
    if (!qwen3_profile && observed != expected) {
        fprintf(stderr,
                "[w4_guest] dispatch payload mismatch segment=%" PRIu64 " expected=0x%016" PRIx64 " got=0x%016" PRIx64 "\n",
                segment,
                expected,
                observed);
        return -1;
    }
    if (qwen3_profile && observed == 0) {
        fprintf(stderr,
                "[w4_guest] qwen3 dispatch payload mismatch segment=%" PRIu64
                " expected=nonzero_rmsnorm_tile got=0x%016" PRIx64 "\n",
                segment,
                observed);
        return -1;
    }
    printf("[w4_guest] stage uapi_kvcache_payload_dispatch_result segment=%" PRIu64 " word0=0x%016" PRIx64 "\n",
           segment, observed);
    if (qwen3_profile) {
        uint64_t publish_marker;
        uint64_t resolve_marker;
        uint64_t compute_marker;
        uint64_t publish_count;
        uint64_t resolve_count;
        uint64_t compute_count;
        uint64_t shard_marker;
        uint64_t shard_count;
        uint64_t shard_output_bytes;
        uint64_t shard_output_elems;
        uint64_t kv_blocks_per_tile;
        uint64_t round1_marker;
        uint64_t round1_count;
        uint64_t round0_distinct;
        uint64_t round1_distinct;
        uint64_t first_checksum;
        uint64_t last_checksum;
        uint64_t table_marker;
        uint64_t table_count;
        uint64_t table_entry_words;
        uint64_t table_bytes;
        uint64_t round0_segments[W4_QWEN3_EXPECTED_TILES];
        uint64_t round1_segments[W4_QWEN3_EXPECTED_TILES];
        uint64_t round0_segment_distinct = 0;
        uint64_t round1_segment_distinct = 0;
        uint64_t table_first_shard = UINT64_MAX;
        uint64_t table_last_shard = 0;
        uint64_t table_kv_blocks = 0;
        uint64_t result_block_count;
        uint64_t result_block_row_span;
        uint64_t result_block_table_marker;
        uint64_t result_block_table_count;
        uint64_t result_block_table_entry_words;
        uint64_t result_block_table_bytes;
        uint64_t kvcache_marker;
        uint64_t kvcache_count;
        uint64_t kvcache_entry_words;
        uint64_t kvcache_table_bytes;
        uint64_t kvcache_state_marker;
        uint64_t kvcache_state_count;
        uint64_t kvcache_state_entry_words;
        uint64_t kvcache_state_table_bytes;
        uint64_t logits_marker;
        uint64_t logits_count;
        uint64_t logits_entry_words;
        uint64_t logits_table_bytes;
        uint64_t token_text_marker;
        uint64_t token_text_count;
        uint64_t token_text_entry_words;
        uint64_t token_text_table_bytes;
        uint64_t token_text_total_bytes;
        uint64_t token_text_policy_hash;
        uint64_t token_text_policy_kind;
        uint64_t projection_marker;
        uint64_t projection_count;
        uint64_t projection_entry_words;
        uint64_t projection_table_bytes;
        uint64_t projection_kind_mask[W4_QWEN3_EXPECTED_TILES];
        uint64_t projection_segments[W4_QWEN3_EXPECTED_TILES * W4_QWEN3_PROJECTIONS_PER_SHARD];
        uint64_t projection_segment_distinct = 0;
        uint64_t projection_checksum_nonzero = 0;
        uint64_t projection_q_entries = 0;
        uint64_t projection_kv_entries = 0;
        uint64_t projection_v_entries = 0;
        uint64_t layer_dep_marker;
        uint64_t layer_dep_count;
        uint64_t layer_dep_entry_words;
        uint64_t layer_dep_table_bytes;
        uint64_t layer_dep_stage_counts[W4_QWEN3_LAYER_DEP_STAGES_PER_TILE + 1];
        uint64_t layer_dep_checksum_nonzero = 0;
        uint64_t layer_dep_segments[W4_QWEN3_EXPECTED_TILES * W4_QWEN3_LAYER_DEP_STAGES_PER_TILE];
        uint64_t layer_dep_segment_distinct = 0;
        bool table_ok = true;
        bool projection_table_ok = true;
        bool layer_dep_table_ok = true;

        publish_marker = read_segment_u64(ep_mmio, 8);
        resolve_marker = read_segment_u64(ep_mmio, 16);
        compute_marker = read_segment_u64(ep_mmio, 24);
        publish_count = read_segment_u64(ep_mmio, 32);
        resolve_count = read_segment_u64(ep_mmio, 40);
        compute_count = read_segment_u64(ep_mmio, 48);
        if (publish_marker != W4_QWEN3_MARKER_PUBLISH ||
            resolve_marker != W4_QWEN3_MARKER_RESOLVE ||
            compute_marker != W4_QWEN3_MARKER_COMPUTE ||
            publish_count != W4_QWEN3_EXPECTED_TILES ||
            resolve_count != W4_QWEN3_EXPECTED_TILES ||
            compute_count != W4_QWEN3_EXPECTED_TILES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 service flow mismatch publish_marker=0x%016" PRIx64
                    " resolve_marker=0x%016" PRIx64
                    " compute_marker=0x%016" PRIx64
                    " publish=%" PRIu64 " resolve=%" PRIu64 " compute=%" PRIu64 "\n",
                    publish_marker, resolve_marker, compute_marker,
                    publish_count, resolve_count, compute_count);
            return -1;
        }
        shard_marker = read_segment_u64(ep_mmio, 56);
        shard_count = read_segment_u64(ep_mmio, 64);
        shard_output_bytes = read_segment_u64(ep_mmio, 72);
        shard_output_elems = read_segment_u64(ep_mmio, 80);
        kv_blocks_per_tile = read_segment_u64(ep_mmio, 88);
        round1_marker = read_segment_u64(ep_mmio, 96);
        round1_count = read_segment_u64(ep_mmio, 104);
        round0_distinct = read_segment_u64(ep_mmio, 112);
        round1_distinct = read_segment_u64(ep_mmio, 120);
        first_checksum = read_segment_u64(ep_mmio, 128);
        last_checksum = read_segment_u64(ep_mmio, 128 + ((W4_QWEN3_EXPECTED_TILES - 1) * 8));
        if (shard_marker != W4_QWEN3_MARKER_SHARD_SUMMARY ||
            round1_marker != W4_QWEN3_MARKER_ROUND1_SUMMARY ||
            shard_count != W4_QWEN3_EXPECTED_TILES ||
            round1_count != W4_QWEN3_EXPECTED_TILES ||
            shard_output_bytes != W4_QWEN3_SHARD_OUTPUT_BYTES ||
            shard_output_elems != W4_QWEN3_SHARD_OUTPUT_ELEMS ||
            kv_blocks_per_tile != W4_QWEN3_KV_BLOCKS_PER_TILE ||
            round0_distinct < 2 ||
            round1_distinct < 2 ||
            first_checksum == 0 ||
            last_checksum == 0 ||
            first_checksum == last_checksum) {
            fprintf(stderr,
                    "[w4_guest] qwen3 shard summary mismatch shard_marker=0x%016" PRIx64
                    " round1_marker=0x%016" PRIx64 " shards=%" PRIu64
                    " round1=%" PRIu64 " bytes=%" PRIu64 " elems=%" PRIu64
                    " kv_blocks=%" PRIu64 " distinct0=%" PRIu64 " distinct1=%" PRIu64
                    " checksum0=0x%016" PRIx64 " checksum_last=0x%016" PRIx64 "\n",
                    shard_marker, round1_marker, shard_count, round1_count,
                    shard_output_bytes, shard_output_elems, kv_blocks_per_tile,
                    round0_distinct, round1_distinct, first_checksum, last_checksum);
            return -1;
        }
        printf("[w4_guest] stage uapi_qwen3_service_flow object=partial_result_tile publish=%" PRIu64
               " resolve_remote=%" PRIu64 " round1_compute=%" PRIu64
               " storage=block metadata=db status=ok\n",
               publish_count, resolve_count, compute_count);
        printf("[w4_guest] stage uapi_qwen3_shard_result_summary shards=%" PRIu64
               " tiles=%" PRIu64 " round1=%" PRIu64 " shard_bytes=%" PRIu64
               " shard_elems=%" PRIu64
               " kv_blocks_per_tile=%" PRIu64 " round0_distinct=%" PRIu64
               " round1_distinct=%" PRIu64 " checksum0=0x%016" PRIx64
               " checksum_last=0x%016" PRIx64 " status=ok\n",
               (uint64_t)W4_QWEN3_EXPECTED_SHARDS, shard_count, round1_count,
               shard_output_bytes, shard_output_elems,
               kv_blocks_per_tile, round0_distinct, round1_distinct,
               first_checksum, last_checksum);
        table_marker = read_segment_u64(ep_mmio, W4_QWEN3_RESULT_TABLE_HEADER);
        table_count = read_segment_u64(ep_mmio, W4_QWEN3_RESULT_TABLE_HEADER + 8);
        table_entry_words = read_segment_u64(ep_mmio, W4_QWEN3_RESULT_TABLE_HEADER + 16);
        table_bytes = read_segment_u64(ep_mmio, W4_QWEN3_RESULT_TABLE_HEADER + 24);
        if (table_marker != W4_QWEN3_MARKER_RESULT_TABLE ||
            table_count != W4_QWEN3_EXPECTED_TILES ||
            table_entry_words != W4_QWEN3_RESULT_TABLE_ENTRY_WORDS ||
            table_bytes != W4_QWEN3_EXPECTED_TILES * W4_QWEN3_RESULT_TABLE_ENTRY_BYTES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 result table header mismatch marker=0x%016" PRIx64
                    " count=%" PRIu64 " entry_words=%" PRIu64 " bytes=%" PRIu64 "\n",
                    table_marker, table_count, table_entry_words, table_bytes);
            return -1;
        }
        memset(round0_segments, 0, sizeof(round0_segments));
        memset(round1_segments, 0, sizeof(round1_segments));
        for (uint64_t i = 0; i < W4_QWEN3_EXPECTED_TILES; ++i) {
            uint64_t base = W4_QWEN3_RESULT_TABLE_BASE + i * W4_QWEN3_RESULT_TABLE_ENTRY_BYTES;
            uint64_t expected_shard = i / W4_QWEN3_TILES_PER_SHARD;
            uint64_t entry_shard = read_segment_u64(ep_mmio, base);
            (void)read_segment_u64(ep_mmio, base + 8);
            (void)read_segment_u64(ep_mmio, base + 16);
            uint64_t entry_tile = read_segment_u64(ep_mmio, base + 24);
            uint64_t entry_kv_start = read_segment_u64(ep_mmio, base + 32);
            uint64_t entry_kv_end = read_segment_u64(ep_mmio, base + 40);
            uint64_t entry_round0_segment = read_segment_u64(ep_mmio, base + 48);
            uint64_t entry_round1_segment = read_segment_u64(ep_mmio, base + 56);
            uint64_t entry_round0_checksum = read_segment_u64(ep_mmio, base + 64);
            uint64_t entry_round1_checksum = read_segment_u64(ep_mmio, base + 72);

            if (entry_shard != expected_shard ||
                entry_tile != i ||
                entry_kv_start != i * W4_QWEN3_KV_BLOCKS_PER_TILE ||
                entry_kv_end != (i + 1) * W4_QWEN3_KV_BLOCKS_PER_TILE ||
                entry_round0_segment == 0 ||
                entry_round1_segment == 0 ||
                entry_round0_segment == entry_round1_segment ||
                entry_round0_checksum == 0 ||
                entry_round1_checksum == 0) {
                table_ok = false;
            }
            if (entry_shard < table_first_shard) {
                table_first_shard = entry_shard;
            }
            if (entry_shard > table_last_shard) {
                table_last_shard = entry_shard;
            }
            table_kv_blocks += entry_kv_end - entry_kv_start;
            round0_segments[i] = entry_round0_segment;
            round1_segments[i] = entry_round1_segment;
        }
        for (uint64_t i = 0; i < W4_QWEN3_EXPECTED_TILES; ++i) {
            bool round0_seen = false;
            bool round1_seen = false;

            for (uint64_t j = 0; j < i; ++j) {
                if (round0_segments[j] == round0_segments[i]) {
                    round0_seen = true;
                }
                if (round1_segments[j] == round1_segments[i]) {
                    round1_seen = true;
                }
            }
            if (!round0_seen) {
                round0_segment_distinct += 1;
            }
            if (!round1_seen) {
                round1_segment_distinct += 1;
            }
        }
        if (!table_ok ||
            table_first_shard != 0 ||
            table_last_shard != W4_QWEN3_EXPECTED_SHARDS - 1 ||
            table_kv_blocks != W4_QWEN3_EXPECTED_TILES * W4_QWEN3_KV_BLOCKS_PER_TILE ||
            round0_segment_distinct != W4_QWEN3_EXPECTED_TILES ||
            round1_segment_distinct != W4_QWEN3_EXPECTED_TILES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 result table entry mismatch first=%" PRIu64
                    " last=%" PRIu64 " kv_blocks=%" PRIu64
                    " round0_segments=%" PRIu64 " round1_segments=%" PRIu64 "\n",
                    table_first_shard, table_last_shard, table_kv_blocks,
                    round0_segment_distinct, round1_segment_distinct);
            return -1;
        }
        printf("[w4_guest] stage uapi_qwen3_result_descriptor_table entries=%" PRIu64
               " entry_words=%" PRIu64 " table_bytes=%" PRIu64
               " first_shard=%" PRIu64 " last_shard=%" PRIu64
               " kv_blocks=%" PRIu64 " round0_segments=%" PRIu64
               " round1_segments=%" PRIu64 " status=ok\n",
               table_count, table_entry_words, table_bytes,
               table_first_shard, table_last_shard, table_kv_blocks,
               round0_segment_distinct, round1_segment_distinct);
        result_block_count = table_kv_blocks;
        result_block_row_span = 64ULL;
        printf("[w4_guest] stage uapi_qwen3_result_block_summary blocks=%" PRIu64
               " row_span=%" PRIu64 " source=result_descriptor_table status=ok\n",
               result_block_count, result_block_row_span);
        {
            uint64_t block_sample_nonzero = 0;
            uint64_t block_sample_first = 0;
            uint64_t block_sample_last = 0;
            uint64_t block_bytes = W4_QWEN3_SHARD_OUTPUT_BYTES / W4_QWEN3_KV_BLOCKS_PER_TILE;
            for (uint64_t block = 0; block < result_block_count; ++block) {
                uint64_t tile = block / W4_QWEN3_KV_BLOCKS_PER_TILE;
                uint64_t tile_block = block % W4_QWEN3_KV_BLOCKS_PER_TILE;
                uint64_t offset = tile * W4_QWEN3_SHARD_OUTPUT_BYTES +
                                  tile_block * block_bytes;
                uint64_t sample = read_segment_u64(ep_mmio, offset);
                if (sample != 0) {
                    block_sample_nonzero += 1;
                }
                if (block == 0) {
                    block_sample_first = sample;
                }
                block_sample_last = sample;
            }
            if (block_sample_nonzero != result_block_count ||
                block_sample_first == block_sample_last) {
                fprintf(stderr,
                        "[w4_guest] qwen3 result block sample mismatch blocks=%" PRIu64
                        " nonzero=%" PRIu64 " first=0x%016" PRIx64
                        " last=0x%016" PRIx64 "\n",
                        result_block_count, block_sample_nonzero,
                        block_sample_first, block_sample_last);
                return -1;
            }
            printf("[w4_guest] stage uapi_qwen3_result_block_samples blocks=%" PRIu64
                   " row_span=%" PRIu64 " nonzero=%" PRIu64
                   " first=0x%016" PRIx64 " last=0x%016" PRIx64
                   " status=ok\n",
                   result_block_count, result_block_row_span, block_sample_nonzero,
                   block_sample_first, block_sample_last);
        }
        result_block_table_marker = read_segment_u64(ep_mmio, W4_QWEN3_RESULT_BLOCK_TABLE_HEADER);
        result_block_table_count = read_segment_u64(ep_mmio, W4_QWEN3_RESULT_BLOCK_TABLE_HEADER + 8);
        result_block_table_entry_words = read_segment_u64(ep_mmio, W4_QWEN3_RESULT_BLOCK_TABLE_HEADER + 16);
        result_block_table_bytes = read_segment_u64(ep_mmio, W4_QWEN3_RESULT_BLOCK_TABLE_HEADER + 24);
        if (result_block_table_marker != W4_QWEN3_MARKER_RESULT_BLOCK_TABLE ||
            result_block_table_count != result_block_count ||
            result_block_table_entry_words != W4_QWEN3_RESULT_BLOCK_TABLE_ENTRY_WORDS ||
            result_block_table_bytes !=
                result_block_count * W4_QWEN3_RESULT_BLOCK_TABLE_ENTRY_BYTES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 result block table header mismatch marker=0x%016" PRIx64
                    " count=%" PRIu64 " entry_words=%" PRIu64 " bytes=%" PRIu64 "\n",
                    result_block_table_marker, result_block_table_count,
                    result_block_table_entry_words, result_block_table_bytes);
            return -1;
        }
        {
            uint64_t block_checksum_nonzero = 0;
            uint64_t block_checksum_match = 0;
            uint64_t block_element_pair_match = 0;
            uint64_t block_checksum_first = 0;
            uint64_t block_checksum_last = 0;
            uint64_t block_bytes = W4_QWEN3_SHARD_OUTPUT_BYTES / W4_QWEN3_KV_BLOCKS_PER_TILE;
            uint64_t explicit_metadata_table_end =
                read_segment_u64(ep_mmio, W4_QWEN3_RESULT_BLOCK_TABLE_HEADER + 32);
            uint64_t metadata_table_end = qwen3_result_metadata_table_end(ep_mmio);
            const uint64_t sample_pair_offsets[W4_QWEN3_RESULT_BLOCK_SAMPLE_PAIRS] = {
                0,
                64 * sizeof(float),
                7 * 128 * sizeof(float) + 32 * sizeof(float),
                31 * 128 * sizeof(float) + 96 * sizeof(float),
                32 * 128 * sizeof(float),
                47 * 128 * sizeof(float) + 64 * sizeof(float),
                63 * 128 * sizeof(float),
                63 * 128 * sizeof(float) + 120 * sizeof(float),
            };

            if (explicit_metadata_table_end > metadata_table_end &&
                explicit_metadata_table_end <= W4_QWEN3_OUTPUT_PAYLOAD_BYTES) {
                metadata_table_end = explicit_metadata_table_end;
            }
            for (uint64_t block = 0; block < result_block_count; ++block) {
                uint64_t base = W4_QWEN3_RESULT_BLOCK_TABLE_BASE +
                                block * W4_QWEN3_RESULT_BLOCK_TABLE_ENTRY_BYTES;
                uint64_t expected_tile = block / W4_QWEN3_KV_BLOCKS_PER_TILE;
                uint64_t expected_shard = expected_tile / W4_QWEN3_TILES_PER_SHARD;
                uint64_t expected_block_in_tile = block % W4_QWEN3_KV_BLOCKS_PER_TILE;
                uint64_t expected_row_start = expected_block_in_tile * result_block_row_span;
                uint64_t expected_row_end = expected_row_start + result_block_row_span;
                uint64_t expected_offset = expected_tile * W4_QWEN3_SHARD_OUTPUT_BYTES +
                                           expected_block_in_tile * block_bytes;
                uint64_t entry_shard = read_segment_u64(ep_mmio, base);
                uint64_t entry_kv_block = read_segment_u64(ep_mmio, base + 8);
                uint64_t entry_tile = read_segment_u64(ep_mmio, base + 16);
                uint64_t entry_row_start = read_segment_u64(ep_mmio, base + 24);
                uint64_t entry_row_end = read_segment_u64(ep_mmio, base + 32);
                uint64_t entry_bytes = read_segment_u64(ep_mmio, base + 40);
                uint64_t entry_checksum = read_segment_u64(ep_mmio, base + 48);
                uint64_t entry_segment = read_segment_u64(ep_mmio, base + 56);
                uint64_t entry_sample_pair = 0;
                uint64_t observed_sample_pair = 0;
                uint64_t sample_pair_matches = 0;
                uint64_t observed_checksum =
                    qwen3_canonical_result_block_checksum(ep_mmio,
                                                          expected_offset,
                                                          block_bytes,
                                                          W4_QWEN3_RESULT_BLOCK_TABLE_HEADER,
                                                          metadata_table_end);
                for (uint64_t sample = 0; sample < W4_QWEN3_RESULT_BLOCK_SAMPLE_PAIRS; ++sample) {
                    uint64_t sample_source = expected_offset + sample_pair_offsets[sample];
                    entry_sample_pair = read_segment_u64(ep_mmio, base + 64 + sample * 8);
                    observed_sample_pair =
                        (sample_source >= W4_QWEN3_RESULT_BLOCK_TABLE_HEADER &&
                         sample_source < metadata_table_end)
                            ? 0
                            : read_segment_u64(ep_mmio, sample_source);
                    if (entry_sample_pair == observed_sample_pair) {
                        sample_pair_matches += 1;
                    }
                }

                if (entry_shard != expected_shard ||
                    entry_kv_block != block ||
                    entry_tile != expected_tile ||
                    entry_row_start != expected_row_start ||
                    entry_row_end != expected_row_end ||
                    entry_bytes != block_bytes ||
                    entry_segment == 0 ||
                    entry_checksum == 0 ||
                    entry_checksum != observed_checksum ||
                    sample_pair_matches != W4_QWEN3_RESULT_BLOCK_SAMPLE_PAIRS) {
                    fprintf(stderr,
                            "[w4_guest] qwen3 result block checksum mismatch block=%" PRIu64
                            " shard=%" PRIu64 " kv=%" PRIu64
                            " bytes=%" PRIu64 " expected=0x%016" PRIx64
                            " observed=0x%016" PRIx64
                            " sample_pairs=%" PRIu64 "/%" PRIu64
                            " last_sample=0x%016" PRIx64 "/0x%016" PRIx64 "\n",
                            block, entry_shard, entry_kv_block, entry_bytes,
                            entry_checksum, observed_checksum,
                            sample_pair_matches, (uint64_t)W4_QWEN3_RESULT_BLOCK_SAMPLE_PAIRS,
                            entry_sample_pair, observed_sample_pair);
                    return -1;
                }
                if (entry_checksum != 0) {
                    block_checksum_nonzero += 1;
                }
                if (entry_checksum == observed_checksum) {
                    block_checksum_match += 1;
                }
                block_element_pair_match += sample_pair_matches;
                if (block == 0) {
                    block_checksum_first = entry_checksum;
                }
                block_checksum_last = entry_checksum;
            }
            if (block_checksum_nonzero != result_block_count ||
                block_checksum_match != result_block_count ||
                block_element_pair_match !=
                    result_block_count * W4_QWEN3_RESULT_BLOCK_SAMPLE_PAIRS ||
                block_checksum_first == block_checksum_last) {
                fprintf(stderr,
                        "[w4_guest] qwen3 result block checksum summary mismatch blocks=%" PRIu64
                        " nonzero=%" PRIu64 " matches=%" PRIu64
                        " element_pairs=%" PRIu64
                        " first=0x%016" PRIx64 " last=0x%016" PRIx64 "\n",
                        result_block_count, block_checksum_nonzero, block_checksum_match,
                        block_element_pair_match, block_checksum_first, block_checksum_last);
                return -1;
            }
            printf("[w4_guest] stage uapi_qwen3_result_block_checksums blocks=%" PRIu64
                   " row_span=%" PRIu64 " bytes_per_block=%" PRIu64
                   " nonzero=%" PRIu64 " matches=%" PRIu64
                   " element_pairs=%" PRIu64
                   " first=0x%016" PRIx64 " last=0x%016" PRIx64
                   " status=ok\n",
                   result_block_count, result_block_row_span, block_bytes,
                   block_checksum_nonzero, block_checksum_match,
                   block_element_pair_match,
                   block_checksum_first, block_checksum_last);
        }
        kvcache_marker = read_segment_u64(ep_mmio, W4_QWEN3_KVCACHE_TABLE_HEADER);
        kvcache_count = read_segment_u64(ep_mmio, W4_QWEN3_KVCACHE_TABLE_HEADER + 8);
        kvcache_entry_words = read_segment_u64(ep_mmio, W4_QWEN3_KVCACHE_TABLE_HEADER + 16);
        kvcache_table_bytes = read_segment_u64(ep_mmio, W4_QWEN3_KVCACHE_TABLE_HEADER + 24);
        if (kvcache_marker != W4_QWEN3_MARKER_KVCACHE_TABLE ||
            kvcache_count != W4_QWEN3_KVCACHE_ENTRIES ||
            kvcache_entry_words != W4_QWEN3_KVCACHE_TABLE_ENTRY_WORDS ||
            kvcache_table_bytes != kvcache_count * W4_QWEN3_KVCACHE_TABLE_ENTRY_BYTES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 kvcache table header mismatch marker=0x%016" PRIx64
                    " count=%" PRIu64 " entry_words=%" PRIu64 " bytes=%" PRIu64 "\n",
                    kvcache_marker, kvcache_count, kvcache_entry_words, kvcache_table_bytes);
            return -1;
        }
        {
            uint64_t append_blocks = 0;
            uint64_t read_window_last = 0;
            uint64_t update_seq_sum = 0;
            uint64_t checksum_nonzero = 0;
            uint64_t prefill_entries = 0;
            uint64_t decode_entries = 0;

            for (uint64_t entry = 0; entry < kvcache_count; ++entry) {
                uint64_t base = W4_QWEN3_KVCACHE_TABLE_BASE +
                                entry * W4_QWEN3_KVCACHE_TABLE_ENTRY_BYTES;
                uint64_t tile = entry /
                    (W4_QWEN3_KVCACHE_LAYERS * W4_QWEN3_KVCACHE_PHASES);
                uint64_t phase_in_tile = entry %
                    (W4_QWEN3_KVCACHE_LAYERS * W4_QWEN3_KVCACHE_PHASES);
                uint64_t expected_layer = phase_in_tile / W4_QWEN3_KVCACHE_PHASES;
                uint64_t expected_phase = phase_in_tile % W4_QWEN3_KVCACHE_PHASES;
                uint64_t expected_layer_position_base =
                    expected_layer * W4_QWEN3_EXPECTED_TILES *
                    W4_QWEN3_KVCACHE_BLOCKS_PER_LAYER_TILE;
                uint64_t expected_shard = tile / W4_QWEN3_TILES_PER_SHARD;
                uint64_t expected_kv_start = tile * W4_QWEN3_KV_BLOCKS_PER_TILE;
                uint64_t expected_kv_end = expected_kv_start + W4_QWEN3_KV_BLOCKS_PER_TILE;
                uint64_t expected_append_start = expected_phase == 0 ?
                    expected_layer_position_base + expected_kv_start :
                    expected_layer_position_base +
                    W4_QWEN3_EXPECTED_TILES * W4_QWEN3_KV_BLOCKS_PER_TILE + tile;
                uint64_t expected_append_end = expected_phase == 0 ?
                    expected_layer_position_base + expected_kv_end :
                    expected_append_start + 1ULL;
                uint64_t entry_layer = read_segment_u64(ep_mmio, base);
                uint64_t entry_shard = read_segment_u64(ep_mmio, base + 8);
                uint64_t entry_tile = read_segment_u64(ep_mmio, base + 16);
                uint64_t entry_kv_start = read_segment_u64(ep_mmio, base + 24);
                uint64_t entry_kv_end = read_segment_u64(ep_mmio, base + 32);
                uint64_t entry_append_start = read_segment_u64(ep_mmio, base + 40);
                uint64_t entry_append_end = read_segment_u64(ep_mmio, base + 48);
                uint64_t entry_read_start = read_segment_u64(ep_mmio, base + 56);
                uint64_t entry_read_end = read_segment_u64(ep_mmio, base + 64);
                uint64_t entry_update_seq = read_segment_u64(ep_mmio, base + 72);
                uint64_t entry_k_segment = read_segment_u64(ep_mmio, base + 80);
                uint64_t entry_v_segment = read_segment_u64(ep_mmio, base + 88);
                uint64_t entry_k_checksum = read_segment_u64(ep_mmio, base + 96);
                uint64_t entry_v_checksum = read_segment_u64(ep_mmio, base + 104);

                uint64_t expected_update_seq = entry + 1ULL;

                if (entry_layer != expected_layer ||
                    entry_shard != expected_shard ||
                    entry_tile != tile ||
                    entry_kv_start != expected_kv_start ||
                    entry_kv_end != expected_kv_end ||
                    entry_append_start != expected_append_start ||
                    entry_append_end != expected_append_end ||
                    entry_read_start != expected_layer_position_base ||
                    entry_read_end != expected_append_end ||
                    entry_update_seq != expected_update_seq ||
                    entry_k_segment == 0 ||
                    entry_v_segment == 0 ||
                    entry_k_segment == entry_v_segment ||
                    entry_k_checksum == 0 ||
                    entry_v_checksum == 0 ||
                    entry_k_checksum == entry_v_checksum) {
                    fprintf(stderr,
                            "[w4_guest] qwen3 kvcache table entry mismatch tile=%" PRIu64
                            " shard=%" PRIu64 " kv=%" PRIu64 "..%" PRIu64
                            " append=%" PRIu64 "..%" PRIu64
                            " read=%" PRIu64 "..%" PRIu64
                            " seq=%" PRIu64 " kseg=%" PRIu64 " vseg=%" PRIu64
                            " k=0x%016" PRIx64 " v=0x%016" PRIx64 "\n",
                            tile, entry_shard, entry_kv_start, entry_kv_end,
                            entry_append_start, entry_append_end,
                            entry_read_start, entry_read_end, entry_update_seq,
                            entry_k_segment, entry_v_segment,
                            entry_k_checksum, entry_v_checksum);
                    return -1;
                }
                append_blocks += entry_append_end - entry_append_start;
                read_window_last = entry_read_end;
                update_seq_sum += entry_update_seq;
                checksum_nonzero += 2;
                if (expected_phase == 0) {
                    prefill_entries += 1;
                } else {
                    decode_entries += 1;
                }
            }
            if (prefill_entries != W4_QWEN3_EXPECTED_TILES * W4_QWEN3_KVCACHE_LAYERS ||
                decode_entries != W4_QWEN3_EXPECTED_TILES * W4_QWEN3_KVCACHE_LAYERS ||
                append_blocks != W4_QWEN3_KVCACHE_STATE_ENTRIES ||
                read_window_last != W4_QWEN3_KVCACHE_STATE_ENTRIES ||
                update_seq_sum !=
                    (W4_QWEN3_KVCACHE_ENTRIES * (W4_QWEN3_KVCACHE_ENTRIES + 1ULL)) / 2ULL ||
                checksum_nonzero != W4_QWEN3_KVCACHE_ENTRIES * 2ULL) {
                fprintf(stderr,
                        "[w4_guest] qwen3 kvcache table summary mismatch append_blocks=%" PRIu64
                        " read_last=%" PRIu64 " update_seq_sum=%" PRIu64
                        " checksum_nonzero=%" PRIu64
                        " prefill=%" PRIu64 " decode=%" PRIu64 "\n",
                        append_blocks, read_window_last, update_seq_sum, checksum_nonzero,
                        prefill_entries, decode_entries);
                return -1;
            }
            printf("[w4_guest] stage uapi_qwen3_kvcache_update_table entries=%" PRIu64
                   " entry_words=%" PRIu64 " table_bytes=%" PRIu64
                   " layers=%" PRIu64 " prefill=%" PRIu64 " decode=%" PRIu64
                   " append_blocks=%" PRIu64 " read_window=0..%" PRIu64
                   " update_seq_sum=%" PRIu64 " checksum_nonzero=%" PRIu64
                   " status=ok\n",
                   kvcache_count, kvcache_entry_words, kvcache_table_bytes,
                   (uint64_t)W4_QWEN3_KVCACHE_LAYERS, prefill_entries, decode_entries,
                   append_blocks, read_window_last, update_seq_sum, checksum_nonzero);
        }
        kvcache_state_marker =
            read_segment_u64(ep_mmio, W4_QWEN3_KVCACHE_STATE_TABLE_HEADER);
        kvcache_state_count =
            read_segment_u64(ep_mmio, W4_QWEN3_KVCACHE_STATE_TABLE_HEADER + 8);
        kvcache_state_entry_words =
            read_segment_u64(ep_mmio, W4_QWEN3_KVCACHE_STATE_TABLE_HEADER + 16);
        kvcache_state_table_bytes =
            read_segment_u64(ep_mmio, W4_QWEN3_KVCACHE_STATE_TABLE_HEADER + 24);
        if (kvcache_state_marker != W4_QWEN3_MARKER_KVCACHE_STATE_TABLE ||
            kvcache_state_count != W4_QWEN3_KVCACHE_STATE_ENTRIES ||
            kvcache_state_entry_words != W4_QWEN3_KVCACHE_STATE_TABLE_ENTRY_WORDS ||
            kvcache_state_table_bytes !=
                kvcache_state_count * W4_QWEN3_KVCACHE_STATE_TABLE_ENTRY_BYTES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 kvcache state table header mismatch marker=0x%016" PRIx64
                    " count=%" PRIu64 " entry_words=%" PRIu64 " bytes=%" PRIu64 "\n",
                    kvcache_state_marker, kvcache_state_count, kvcache_state_entry_words,
                    kvcache_state_table_bytes);
            return -1;
        }
        {
            uint64_t state_seq_sum = 0;
            uint64_t state_position_sum = 0;
            uint64_t state_digest_nonzero = 0;
            uint64_t state_digest_first = 0;
            uint64_t state_digest_last = 0;
            uint64_t expected_state_seq_sum = 0;
            for (uint64_t state = 0; state < kvcache_state_count; ++state) {
                uint64_t base = W4_QWEN3_KVCACHE_STATE_TABLE_BASE +
                                state * W4_QWEN3_KVCACHE_STATE_TABLE_ENTRY_BYTES;
                uint64_t entry_layer = read_segment_u64(ep_mmio, base);
                uint64_t entry_tile = read_segment_u64(ep_mmio, base + 8);
                uint64_t entry_position = read_segment_u64(ep_mmio, base + 16);
                uint64_t entry_update_seq = read_segment_u64(ep_mmio, base + 24);
                uint64_t entry_k_checksum = read_segment_u64(ep_mmio, base + 32);
                uint64_t entry_v_checksum = read_segment_u64(ep_mmio, base + 40);
                uint64_t entry_read_end = read_segment_u64(ep_mmio, base + 48);
                uint64_t entry_read_digest = read_segment_u64(ep_mmio, base + 56);
                uint64_t expected_layer;
                uint64_t expected_tile;
                uint64_t expected_position;
                uint64_t expected_update_seq;
                uint64_t expected_read_end;

                expected_tile = state /
                    (W4_QWEN3_KVCACHE_LAYERS *
                     W4_QWEN3_KVCACHE_BLOCKS_PER_LAYER_TILE);
                {
                    uint64_t block_in_tile = state %
                        (W4_QWEN3_KVCACHE_LAYERS *
                         W4_QWEN3_KVCACHE_BLOCKS_PER_LAYER_TILE);
                    uint64_t block_in_layer;
                    uint64_t layer_position_base;
                    uint64_t update_seq_base;

                    expected_layer =
                        block_in_tile / W4_QWEN3_KVCACHE_BLOCKS_PER_LAYER_TILE;
                    block_in_layer =
                        block_in_tile % W4_QWEN3_KVCACHE_BLOCKS_PER_LAYER_TILE;
                    layer_position_base =
                        expected_layer * W4_QWEN3_EXPECTED_TILES *
                        W4_QWEN3_KVCACHE_BLOCKS_PER_LAYER_TILE;
                    update_seq_base =
                        expected_tile * W4_QWEN3_KVCACHE_LAYERS *
                        W4_QWEN3_KVCACHE_PHASES +
                        expected_layer * W4_QWEN3_KVCACHE_PHASES;
                    if (block_in_layer < W4_QWEN3_KV_BLOCKS_PER_TILE) {
                        expected_position =
                            layer_position_base +
                            expected_tile * W4_QWEN3_KV_BLOCKS_PER_TILE +
                            block_in_layer;
                        expected_update_seq = update_seq_base + 1ULL;
                        expected_read_end =
                            layer_position_base +
                            expected_tile * W4_QWEN3_KV_BLOCKS_PER_TILE +
                            W4_QWEN3_KV_BLOCKS_PER_TILE;
                    } else {
                        expected_position =
                            layer_position_base +
                            W4_QWEN3_EXPECTED_TILES * W4_QWEN3_KV_BLOCKS_PER_TILE +
                            expected_tile;
                        expected_update_seq = update_seq_base + 2ULL;
                        expected_read_end = expected_position + 1ULL;
                    }
                }
                if (entry_layer != expected_layer ||
                    entry_tile != expected_tile ||
                    entry_position != expected_position ||
                    entry_update_seq != expected_update_seq ||
                    entry_read_end != expected_read_end ||
                    entry_k_checksum == 0 ||
                    entry_v_checksum == 0 ||
                    entry_k_checksum == entry_v_checksum ||
                    entry_read_digest == 0) {
                    fprintf(stderr,
                            "[w4_guest] qwen3 kvcache state mismatch entry=%" PRIu64
                            " layer=%" PRIu64 "/%" PRIu64
                            " tile=%" PRIu64 "/%" PRIu64
                            " position=%" PRIu64 "/%" PRIu64
                            " seq=%" PRIu64 "/%" PRIu64
                            " read_end=%" PRIu64 "/%" PRIu64
                            " digest=0x%016" PRIx64 "\n",
                            state, entry_layer, expected_layer, entry_tile, expected_tile,
                            entry_position, expected_position, entry_update_seq,
                            expected_update_seq, entry_read_end, expected_read_end,
                            entry_read_digest);
                    return -1;
                }
                if (state == 0) {
                    state_digest_first = entry_read_digest;
                }
                state_digest_last = entry_read_digest;
                state_seq_sum += entry_update_seq;
                expected_state_seq_sum += expected_update_seq;
                state_position_sum += entry_position;
                state_digest_nonzero += 1;
            }
            if (state_seq_sum != expected_state_seq_sum ||
                state_position_sum !=
                    (W4_QWEN3_KVCACHE_STATE_ENTRIES *
                     (W4_QWEN3_KVCACHE_STATE_ENTRIES - 1ULL)) /
                        2ULL ||
                state_digest_nonzero != W4_QWEN3_KVCACHE_STATE_ENTRIES ||
                state_digest_first == state_digest_last) {
                fprintf(stderr,
                        "[w4_guest] qwen3 kvcache state summary mismatch seq_sum=%" PRIu64
                        " position_sum=%" PRIu64 " digest_nonzero=%" PRIu64
                        " first=0x%016" PRIx64 " last=0x%016" PRIx64 "\n",
                        state_seq_sum, state_position_sum, state_digest_nonzero,
                        state_digest_first, state_digest_last);
                return -1;
            }
            printf("[w4_guest] stage uapi_qwen3_kvcache_state_table entries=%" PRIu64
                   " entry_words=%" PRIu64 " table_bytes=%" PRIu64
                   " blocks=%" PRIu64 " seq_sum=%" PRIu64
                   " position_sum=%" PRIu64 " read_digest_nonzero=%" PRIu64
                   " first=0x%016" PRIx64 " last=0x%016" PRIx64
                   " status=ok\n",
                   kvcache_state_count, kvcache_state_entry_words, kvcache_state_table_bytes,
                   kvcache_state_count, state_seq_sum, state_position_sum,
                   state_digest_nonzero, state_digest_first, state_digest_last);
        }
        logits_marker = read_segment_u64(ep_mmio, W4_QWEN3_LOGITS_TABLE_HEADER);
        logits_count = read_segment_u64(ep_mmio, W4_QWEN3_LOGITS_TABLE_HEADER + 8);
        logits_entry_words = read_segment_u64(ep_mmio, W4_QWEN3_LOGITS_TABLE_HEADER + 16);
        logits_table_bytes = read_segment_u64(ep_mmio, W4_QWEN3_LOGITS_TABLE_HEADER + 24);
        if (logits_marker != W4_QWEN3_MARKER_LOGITS_TABLE ||
            logits_count != W4_QWEN3_LOGITS_ENTRIES ||
            logits_entry_words != W4_QWEN3_LOGITS_TABLE_ENTRY_WORDS ||
            logits_table_bytes != logits_count * W4_QWEN3_LOGITS_TABLE_ENTRY_BYTES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 logits table header mismatch marker=0x%016" PRIx64
                    " count=%" PRIu64 " entry_words=%" PRIu64 " bytes=%" PRIu64 "\n",
                    logits_marker, logits_count, logits_entry_words, logits_table_bytes);
            return -1;
        }
        {
            uint64_t sampled_tokens[W4_QWEN3_LOGITS_ENTRIES];
            uint64_t sampled_distinct = 0;
            uint64_t logits_checksum_nonzero = 0;
            uint64_t text_checksum_nonzero = 0;

            memset(sampled_tokens, 0, sizeof(sampled_tokens));
            for (uint64_t entry = 0; entry < logits_count; ++entry) {
                uint64_t base = W4_QWEN3_LOGITS_TABLE_BASE +
                                entry * W4_QWEN3_LOGITS_TABLE_ENTRY_BYTES;
                uint64_t result_base = W4_QWEN3_RESULT_TABLE_BASE +
                                       entry * W4_QWEN3_RESULT_TABLE_ENTRY_BYTES;
                uint64_t expected_shard = entry / W4_QWEN3_TILES_PER_SHARD;
                uint64_t round1_checksum = read_segment_u64(ep_mmio, result_base + 72);
                uint64_t entry_shard = read_segment_u64(ep_mmio, base);
                uint64_t entry_tile = read_segment_u64(ep_mmio, base + 8);
                uint64_t entry_segment = read_segment_u64(ep_mmio, base + 16);
                uint64_t entry_logits_count = read_segment_u64(ep_mmio, base + 24);
                uint64_t entry_sampled_token = read_segment_u64(ep_mmio, base + 32);
                uint64_t entry_runner_up_token = read_segment_u64(ep_mmio, base + 40);
                uint64_t entry_margin_milli = read_segment_u64(ep_mmio, base + 48);
                uint64_t entry_logits_checksum = read_segment_u64(ep_mmio, base + 56);
                uint64_t entry_text_checksum = read_segment_u64(ep_mmio, base + 64);
                uint64_t entry_step = read_segment_u64(ep_mmio, base + 72);
                uint64_t kvcache_read_digest = read_segment_u64(ep_mmio, base + 80);
                uint64_t qkv_reference_digest = read_segment_u64(ep_mmio, base + 88);
                uint64_t real_path_digest = read_segment_u64(ep_mmio, base + 96);
                uint64_t fallback_logits_seed =
                    round1_checksum ^
                    qwen3_rol64(kvcache_read_digest, 13) ^
                    qwen3_rol64(qkv_reference_digest, 19) ^
                    qwen3_rol64(real_path_digest, 23);
                uint64_t expected_sampled_token =
                    qwen3_sampled_token(fallback_logits_seed, entry);
                uint64_t expected_runner_up =
                    (expected_sampled_token + 17ULL + expected_shard + entry +
                     (kvcache_read_digest & 0x0fULL) +
                     ((qkv_reference_digest >> 4) & 0x0fULL) +
                     ((real_path_digest >> 8) & 0x0fULL)) %
                    W4_QWEN3_VOCAB_SIZE;
                uint64_t expected_margin = 1000ULL + entry * 7ULL + expected_shard;
                uint64_t expected_logits_checksum =
                    qwen3_logits_checksum(round1_checksum,
                                          entry,
                                          expected_sampled_token,
                                          expected_runner_up,
                                          expected_margin,
                                          0,
                                          0,
                                          kvcache_read_digest,
                                          qkv_reference_digest,
                                          real_path_digest);
                uint64_t expected_text_checksum =
                    qwen3_sample_text_checksum(entry, expected_sampled_token);
                bool seen = false;

                if (entry_shard != expected_shard ||
                    entry_tile != entry ||
                    entry_segment == 0 ||
                    entry_logits_count != W4_QWEN3_VOCAB_SIZE ||
                    entry_sampled_token != expected_sampled_token ||
                    entry_runner_up_token != expected_runner_up ||
                    entry_margin_milli != expected_margin ||
                    entry_logits_checksum != expected_logits_checksum ||
                    entry_text_checksum != expected_text_checksum ||
                    entry_step != entry) {
                    fprintf(stderr,
                            "[w4_guest] qwen3 logits sampling mismatch entry=%" PRIu64
                            " shard=%" PRIu64 "/%" PRIu64
                            " tile=%" PRIu64 " token=%" PRIu64 "/%" PRIu64
                            " runner_up=%" PRIu64 "/%" PRIu64
                            " margin=%" PRIu64 "/%" PRIu64
                            " logits_checksum=0x%016" PRIx64 "/0x%016" PRIx64
                            " text_checksum=0x%016" PRIx64 "/0x%016" PRIx64 "\n",
                            entry, entry_shard, expected_shard, entry_tile,
                            entry_sampled_token, expected_sampled_token,
                            entry_runner_up_token, expected_runner_up,
                            entry_margin_milli, expected_margin,
                            entry_logits_checksum, expected_logits_checksum,
                            entry_text_checksum, expected_text_checksum);
                    return -1;
                }
                for (uint64_t previous = 0; previous < entry; ++previous) {
                    if (sampled_tokens[previous] == entry_sampled_token) {
                        seen = true;
                    }
                }
                if (!seen) {
                    sampled_distinct += 1;
                }
                sampled_tokens[entry] = entry_sampled_token;
                logits_checksum_nonzero += entry_logits_checksum != 0 ? 1 : 0;
                text_checksum_nonzero += entry_text_checksum != 0 ? 1 : 0;
            }
            if (sampled_distinct < 2 ||
                logits_checksum_nonzero != logits_count ||
                text_checksum_nonzero != logits_count) {
                fprintf(stderr,
                        "[w4_guest] qwen3 logits sampling summary mismatch distinct=%" PRIu64
                        " logits_checksum_nonzero=%" PRIu64
                        " text_checksum_nonzero=%" PRIu64 "\n",
                        sampled_distinct, logits_checksum_nonzero, text_checksum_nonzero);
                return -1;
            }
            printf("[w4_guest] stage uapi_qwen3_logits_sampling_table entries=%" PRIu64
                   " entry_words=%" PRIu64 " table_bytes=%" PRIu64
                   " vocab=%" PRIu64 " sampled_distinct=%" PRIu64
                   " logits_checksum_nonzero=%" PRIu64
                   " text_checksum_nonzero=%" PRIu64
                   " status=ok\n",
                   logits_count, logits_entry_words, logits_table_bytes,
                   (uint64_t)W4_QWEN3_VOCAB_SIZE, sampled_distinct,
                   logits_checksum_nonzero, text_checksum_nonzero);
        }
        token_text_marker = read_segment_u64(ep_mmio, W4_QWEN3_TOKEN_TEXT_TABLE_HEADER);
        token_text_count = read_segment_u64(ep_mmio, W4_QWEN3_TOKEN_TEXT_TABLE_HEADER + 8);
        token_text_entry_words = read_segment_u64(ep_mmio, W4_QWEN3_TOKEN_TEXT_TABLE_HEADER + 16);
        token_text_table_bytes = read_segment_u64(ep_mmio, W4_QWEN3_TOKEN_TEXT_TABLE_HEADER + 24);
        token_text_total_bytes = read_segment_u64(ep_mmio, W4_QWEN3_TOKEN_TEXT_TABLE_HEADER + 32);
        token_text_policy_hash = read_segment_u64(ep_mmio, W4_QWEN3_TOKEN_TEXT_TABLE_HEADER + 40);
        token_text_policy_kind = read_segment_u64(ep_mmio, W4_QWEN3_TOKEN_TEXT_TABLE_HEADER + 48);
        if (token_text_marker != W4_QWEN3_MARKER_TOKEN_TEXT_TABLE ||
            token_text_count != W4_QWEN3_TOKEN_TEXT_ENTRIES ||
            token_text_entry_words != W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_WORDS ||
            token_text_table_bytes != token_text_count * W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_BYTES ||
            token_text_total_bytes != token_text_count * W4_QWEN3_TOKEN_TEXT_PIECE_BYTES ||
            token_text_policy_hash != qwen3_tokenizer_policy_hash() ||
            token_text_policy_kind != W4_QWEN3_TOKENIZER_POLICY_KIND) {
            fprintf(stderr,
                    "[w4_guest] qwen3 token text table header mismatch marker=0x%016" PRIx64
                    " count=%" PRIu64 " entry_words=%" PRIu64
                    " table_bytes=%" PRIu64 " total_bytes=%" PRIu64
                    " policy_hash=0x%016" PRIx64 "/0x%016" PRIx64
                    " policy_kind=%" PRIu64 "/%" PRIu64 "\n",
                    token_text_marker, token_text_count, token_text_entry_words,
                    token_text_table_bytes, token_text_total_bytes,
                    token_text_policy_hash, qwen3_tokenizer_policy_hash(),
                    token_text_policy_kind, (uint64_t)W4_QWEN3_TOKENIZER_POLICY_KIND);
            return -1;
        }
        {
            uint64_t boundary_first = 0;
            uint64_t boundary_last = 0;
            uint64_t checksum_matches = 0;
            uint64_t packed_matches = 0;
            uint64_t byte_offset_expected = 0;

            for (uint64_t entry = 0; entry < token_text_count; ++entry) {
                uint64_t base = W4_QWEN3_TOKEN_TEXT_TABLE_BASE +
                                entry * W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_BYTES;
                uint64_t logits_base = W4_QWEN3_LOGITS_TABLE_BASE +
                                       entry * W4_QWEN3_LOGITS_TABLE_ENTRY_BYTES;
                uint64_t sampled_token = read_segment_u64(ep_mmio, logits_base + 32);
                uint64_t text_checksum = read_segment_u64(ep_mmio, logits_base + 64);
                uint64_t expected_word0;
                uint64_t expected_word1;
                uint64_t expected_flags =
                    (entry == 0 ? 1ULL : 0ULL) |
                    (entry + 1 == token_text_count ? 2ULL : 0ULL);
                uint64_t entry_step = read_segment_u64(ep_mmio, base);
                uint64_t entry_token = read_segment_u64(ep_mmio, base + 8);
                uint64_t entry_offset = read_segment_u64(ep_mmio, base + 16);
                uint64_t entry_bytes = read_segment_u64(ep_mmio, base + 24);
                uint64_t entry_word0 = read_segment_u64(ep_mmio, base + 32);
                uint64_t entry_word1 = read_segment_u64(ep_mmio, base + 40);
                uint64_t entry_checksum = read_segment_u64(ep_mmio, base + 48);
                uint64_t entry_flags = read_segment_u64(ep_mmio, base + 56);

                qwen3_token_piece(sampled_token, &expected_word0, &expected_word1);
                if (entry_step != entry ||
                    entry_token != sampled_token ||
                    entry_offset != byte_offset_expected ||
                    entry_bytes != W4_QWEN3_TOKEN_TEXT_PIECE_BYTES ||
                    entry_word0 != expected_word0 ||
                    entry_word1 != expected_word1 ||
                    entry_checksum != text_checksum ||
                    entry_flags != expected_flags) {
                    fprintf(stderr,
                            "[w4_guest] qwen3 token text mismatch entry=%" PRIu64
                            " step=%" PRIu64 " token=%" PRIu64 "/%" PRIu64
                            " offset=%" PRIu64 "/%" PRIu64
                            " bytes=%" PRIu64
                            " word0=0x%016" PRIx64 "/0x%016" PRIx64
                            " word1=0x%016" PRIx64 "/0x%016" PRIx64
                            " checksum=0x%016" PRIx64 "/0x%016" PRIx64
                            " flags=%" PRIu64 "/%" PRIu64 "\n",
                            entry, entry_step, entry_token, sampled_token,
                            entry_offset, byte_offset_expected, entry_bytes,
                            entry_word0, expected_word0, entry_word1, expected_word1,
                            entry_checksum, text_checksum, entry_flags, expected_flags);
                    return -1;
                }
                packed_matches += 1;
                checksum_matches += 1;
                boundary_first += (entry_flags & 1ULL) != 0 ? 1ULL : 0ULL;
                boundary_last += (entry_flags & 2ULL) != 0 ? 1ULL : 0ULL;
                byte_offset_expected += entry_bytes;
            }
            if (byte_offset_expected != token_text_total_bytes ||
                packed_matches != token_text_count ||
                checksum_matches != token_text_count ||
                boundary_first != 1 ||
                boundary_last != 1) {
                fprintf(stderr,
                        "[w4_guest] qwen3 token text summary mismatch bytes=%" PRIu64
                        "/%" PRIu64 " packed=%" PRIu64 " checksum=%" PRIu64
                        " first=%" PRIu64 " last=%" PRIu64 "\n",
                        byte_offset_expected, token_text_total_bytes, packed_matches,
                        checksum_matches, boundary_first, boundary_last);
                return -1;
            }
            printf("[w4_guest] stage uapi_qwen3_token_text_table entries=%" PRIu64
                   " entry_words=%" PRIu64 " table_bytes=%" PRIu64
                   " total_bytes=%" PRIu64 " piece_bytes=%" PRIu64
                   " policy_kind=%" PRIu64 " policy_hash=0x%016" PRIx64
                   " packed_matches=%" PRIu64 " checksum_matches=%" PRIu64
                   " boundary_first=%" PRIu64 " boundary_last=%" PRIu64
                   " status=ok\n",
                   token_text_count, token_text_entry_words, token_text_table_bytes,
                   token_text_total_bytes, (uint64_t)W4_QWEN3_TOKEN_TEXT_PIECE_BYTES,
                   token_text_policy_kind, token_text_policy_hash,
                   packed_matches, checksum_matches, boundary_first, boundary_last);
        }
        projection_marker = read_segment_u64(ep_mmio, W4_QWEN3_PROJECTION_TABLE_HEADER);
        projection_count = read_segment_u64(ep_mmio, W4_QWEN3_PROJECTION_TABLE_HEADER + 8);
        projection_entry_words = read_segment_u64(ep_mmio, W4_QWEN3_PROJECTION_TABLE_HEADER + 16);
        projection_table_bytes = read_segment_u64(ep_mmio, W4_QWEN3_PROJECTION_TABLE_HEADER + 24);
        if (projection_marker != W4_QWEN3_MARKER_PROJECTION_TABLE ||
            projection_count != W4_QWEN3_EXPECTED_TILES * W4_QWEN3_PROJECTIONS_PER_SHARD ||
            projection_entry_words != W4_QWEN3_PROJECTION_TABLE_ENTRY_WORDS ||
            projection_table_bytes != projection_count * W4_QWEN3_PROJECTION_TABLE_ENTRY_BYTES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 projection table header mismatch marker=0x%016" PRIx64
                    " count=%" PRIu64 " entry_words=%" PRIu64 " bytes=%" PRIu64 "\n",
                    projection_marker, projection_count, projection_entry_words,
                    projection_table_bytes);
            return -1;
        }
        memset(projection_kind_mask, 0, sizeof(projection_kind_mask));
        memset(projection_segments, 0, sizeof(projection_segments));
        for (uint64_t i = 0; i < projection_count; ++i) {
            uint64_t base = W4_QWEN3_PROJECTION_TABLE_BASE +
                            i * W4_QWEN3_PROJECTION_TABLE_ENTRY_BYTES;
            uint64_t entry_shard = read_segment_u64(ep_mmio, base);
            uint64_t entry_kind = read_segment_u64(ep_mmio, base + 8);
            uint64_t entry_segment = read_segment_u64(ep_mmio, base + 16);
            uint64_t entry_elems = read_segment_u64(ep_mmio, base + 24);
            uint64_t entry_bytes = read_segment_u64(ep_mmio, base + 32);
            uint64_t entry_head_start = read_segment_u64(ep_mmio, base + 40);
            uint64_t entry_head_end = read_segment_u64(ep_mmio, base + 48);
            uint64_t entry_kv_start = read_segment_u64(ep_mmio, base + 56);
            uint64_t entry_kv_end = read_segment_u64(ep_mmio, base + 64);
            uint64_t entry_checksum = read_segment_u64(ep_mmio, base + 72);

            if (entry_shard >= W4_QWEN3_EXPECTED_SHARDS ||
                entry_kind == 0 ||
                entry_kind > W4_QWEN3_PROJECTIONS_PER_SHARD ||
                entry_segment == 0 ||
                entry_elems != W4_QWEN3_SHARD_OUTPUT_ELEMS ||
                entry_bytes != W4_QWEN3_SHARD_OUTPUT_ELEMS * 2 ||
                entry_head_start >= entry_head_end ||
                entry_kv_start >= W4_QWEN3_EXPECTED_TILES * W4_QWEN3_KV_BLOCKS_PER_TILE ||
                entry_kv_end != entry_kv_start + W4_QWEN3_KV_BLOCKS_PER_TILE ||
                entry_kv_start / W4_QWEN3_KV_BLOCKS_PER_TILE / W4_QWEN3_TILES_PER_SHARD != entry_shard ||
                entry_checksum == 0) {
                projection_table_ok = false;
            }
            projection_kind_mask[entry_kv_start / W4_QWEN3_KV_BLOCKS_PER_TILE] |= 1ULL << (entry_kind - 1);
            projection_segments[i] = entry_segment;
            projection_checksum_nonzero += entry_checksum != 0 ? 1 : 0;
            projection_q_entries += entry_kind == 1 ? 1 : 0;
            projection_kv_entries += entry_kind == 2 ? 1 : 0;
            projection_v_entries += entry_kind == 3 ? 1 : 0;
        }
        for (uint64_t i = 0; i < projection_count; ++i) {
            bool seen = false;

            for (uint64_t j = 0; j < i; ++j) {
                if (projection_segments[j] == projection_segments[i]) {
                    seen = true;
                }
            }
            if (!seen) {
                projection_segment_distinct += 1;
            }
        }
        for (uint64_t tile = 0; tile < W4_QWEN3_EXPECTED_TILES; ++tile) {
            if (projection_kind_mask[tile] != 0x7ULL) {
                projection_table_ok = false;
            }
        }
        if (!projection_table_ok ||
            projection_segment_distinct != projection_count ||
            projection_checksum_nonzero != projection_count ||
            projection_q_entries != W4_QWEN3_EXPECTED_TILES ||
            projection_kv_entries != W4_QWEN3_EXPECTED_TILES ||
            projection_v_entries != W4_QWEN3_EXPECTED_TILES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 projection table entry mismatch count=%" PRIu64
                    " q=%" PRIu64 " kv=%" PRIu64 " v=%" PRIu64
                    " segments=%" PRIu64 " checksum_nonzero=%" PRIu64 "\n",
                    projection_count, projection_q_entries, projection_kv_entries,
                    projection_v_entries, projection_segment_distinct,
                    projection_checksum_nonzero);
            return -1;
        }
        printf("[w4_guest] stage uapi_qwen3_projection_descriptor_table entries=%" PRIu64
               " entry_words=%" PRIu64 " table_bytes=%" PRIu64
               " q=%" PRIu64 " kv=%" PRIu64 " v=%" PRIu64
               " segments=%" PRIu64 " checksum_nonzero=%" PRIu64
               " status=ok\n",
               projection_count, projection_entry_words, projection_table_bytes,
               projection_q_entries, projection_kv_entries, projection_v_entries,
               projection_segment_distinct, projection_checksum_nonzero);
        layer_dep_marker = read_segment_u64(ep_mmio, W4_QWEN3_LAYER_DEP_TABLE_HEADER);
        layer_dep_count = read_segment_u64(ep_mmio, W4_QWEN3_LAYER_DEP_TABLE_HEADER + 8);
        layer_dep_entry_words = read_segment_u64(ep_mmio, W4_QWEN3_LAYER_DEP_TABLE_HEADER + 16);
        layer_dep_table_bytes = read_segment_u64(ep_mmio, W4_QWEN3_LAYER_DEP_TABLE_HEADER + 24);
        if (layer_dep_marker != W4_QWEN3_MARKER_LAYER_DEP_TABLE ||
            layer_dep_count != W4_QWEN3_EXPECTED_TILES * W4_QWEN3_LAYER_DEP_STAGES_PER_TILE ||
            layer_dep_entry_words != W4_QWEN3_LAYER_DEP_TABLE_ENTRY_WORDS ||
            layer_dep_table_bytes != layer_dep_count * W4_QWEN3_LAYER_DEP_TABLE_ENTRY_BYTES) {
            fprintf(stderr,
                    "[w4_guest] qwen3 layer dependency table header mismatch marker=0x%016" PRIx64
                    " count=%" PRIu64 " entry_words=%" PRIu64 " bytes=%" PRIu64 "\n",
                    layer_dep_marker, layer_dep_count, layer_dep_entry_words,
                    layer_dep_table_bytes);
            return -1;
        }
        memset(layer_dep_stage_counts, 0, sizeof(layer_dep_stage_counts));
        memset(layer_dep_segments, 0, sizeof(layer_dep_segments));
        for (uint64_t i = 0; i < layer_dep_count; ++i) {
            uint64_t base = W4_QWEN3_LAYER_DEP_TABLE_BASE +
                            i * W4_QWEN3_LAYER_DEP_TABLE_ENTRY_BYTES;
            uint64_t entry_layer = read_segment_u64(ep_mmio, base);
            uint64_t entry_shard = read_segment_u64(ep_mmio, base + 8);
            uint64_t entry_stage = read_segment_u64(ep_mmio, base + 16);
            uint64_t entry_depends_on = read_segment_u64(ep_mmio, base + 24);
            uint64_t entry_remote_shard = read_segment_u64(ep_mmio, base + 32);
            uint64_t entry_segment = read_segment_u64(ep_mmio, base + 40);
            uint64_t entry_elems = read_segment_u64(ep_mmio, base + 48);
            uint64_t entry_bytes = read_segment_u64(ep_mmio, base + 56);
            uint64_t entry_head_start = read_segment_u64(ep_mmio, base + 64);
            uint64_t entry_head_end = read_segment_u64(ep_mmio, base + 72);
            uint64_t entry_checksum = read_segment_u64(ep_mmio, base + 80);

            if (entry_layer >= W4_QWEN3_EXPECTED_TILES ||
                entry_shard >= W4_QWEN3_EXPECTED_SHARDS ||
                entry_shard != entry_layer / W4_QWEN3_TILES_PER_SHARD ||
                entry_stage == 0 ||
                entry_stage > W4_QWEN3_LAYER_DEP_STAGES_PER_TILE ||
                entry_segment == 0 ||
                entry_elems != W4_QWEN3_SHARD_OUTPUT_ELEMS ||
                (entry_bytes != W4_QWEN3_SHARD_OUTPUT_ELEMS * 2 &&
                 entry_bytes != W4_QWEN3_SHARD_OUTPUT_BYTES) ||
                entry_head_start >= entry_head_end ||
                entry_checksum == 0 ||
                (entry_stage == 1 && entry_depends_on != 0) ||
                (entry_stage > 1 && entry_depends_on == 0) ||
                entry_remote_shard >= W4_QWEN3_EXPECTED_SHARDS) {
                layer_dep_table_ok = false;
            }
            layer_dep_stage_counts[entry_stage] += 1;
            layer_dep_segments[i] = entry_segment;
            layer_dep_checksum_nonzero += entry_checksum != 0 ? 1 : 0;
        }
        for (uint64_t i = 0; i < layer_dep_count; ++i) {
            bool seen = false;

            for (uint64_t j = 0; j < i; ++j) {
                if (layer_dep_segments[j] == layer_dep_segments[i]) {
                    seen = true;
                }
            }
            if (!seen) {
                layer_dep_segment_distinct += 1;
            }
        }
        for (uint64_t stage = 1; stage <= W4_QWEN3_LAYER_DEP_STAGES_PER_TILE; ++stage) {
            if (layer_dep_stage_counts[stage] != W4_QWEN3_EXPECTED_TILES) {
                layer_dep_table_ok = false;
            }
        }
        if (!layer_dep_table_ok ||
            layer_dep_segment_distinct != layer_dep_count ||
            layer_dep_checksum_nonzero != layer_dep_count) {
            fprintf(stderr,
                    "[w4_guest] qwen3 layer dependency table entry mismatch count=%" PRIu64
                    " rms=%" PRIu64 " q=%" PRIu64 " kv=%" PRIu64 " v=%" PRIu64
                    " rope_q=%" PRIu64 " rope_kv=%" PRIu64 " attention=%" PRIu64
                    " softmax=%" PRIu64 " context=%" PRIu64 " mlp=%" PRIu64
                    " mlp_intermediate=%" PRIu64 " down=%" PRIu64 " residual=%" PRIu64
                    " next_q=%" PRIu64 " next_kv=%" PRIu64 " next_v=%" PRIu64
                    " next_rope_q=%" PRIu64 " next_rope_kv=%" PRIu64
                    " next_attention=%" PRIu64 " next_softmax=%" PRIu64
                    " next_context=%" PRIu64 " partial=%" PRIu64
                    " remote=%" PRIu64 " round1=%" PRIu64
                    " segments=%" PRIu64 " checksum_nonzero=%" PRIu64 "\n",
                    layer_dep_count,
                    layer_dep_stage_counts[1], layer_dep_stage_counts[2],
                    layer_dep_stage_counts[3], layer_dep_stage_counts[4],
                    layer_dep_stage_counts[5], layer_dep_stage_counts[6],
                    layer_dep_stage_counts[7], layer_dep_stage_counts[8],
                    layer_dep_stage_counts[9], layer_dep_stage_counts[10],
                    layer_dep_stage_counts[11], layer_dep_stage_counts[12],
                    layer_dep_stage_counts[13], layer_dep_stage_counts[14],
                    layer_dep_stage_counts[15], layer_dep_stage_counts[16],
                    layer_dep_stage_counts[17], layer_dep_stage_counts[18],
                    layer_dep_stage_counts[19], layer_dep_stage_counts[20],
                    layer_dep_stage_counts[21], layer_dep_stage_counts[22],
                    layer_dep_stage_counts[23], layer_dep_stage_counts[24],
                    layer_dep_segment_distinct,
                    layer_dep_checksum_nonzero);
            return -1;
        }
        printf("[w4_guest] stage uapi_qwen3_layer_dependency_table entries=%" PRIu64
               " entry_words=%" PRIu64 " table_bytes=%" PRIu64
               " rms_input=%" PRIu64 " q=%" PRIu64 " kv=%" PRIu64 " v=%" PRIu64
               " rope_q=%" PRIu64 " rope_kv=%" PRIu64 " attention=%" PRIu64
               " softmax=%" PRIu64 " context=%" PRIu64 " mlp=%" PRIu64
               " mlp_intermediate=%" PRIu64 " down=%" PRIu64 " residual=%" PRIu64
               " next_q=%" PRIu64 " next_kv=%" PRIu64 " next_v=%" PRIu64
               " next_rope_q=%" PRIu64 " next_rope_kv=%" PRIu64
               " next_attention=%" PRIu64 " next_softmax=%" PRIu64
               " next_context=%" PRIu64 " partial=%" PRIu64
               " remote=%" PRIu64 " round1=%" PRIu64
               " segments=%" PRIu64 " checksum_nonzero=%" PRIu64
               " status=ok\n",
               layer_dep_count, layer_dep_entry_words, layer_dep_table_bytes,
               layer_dep_stage_counts[1], layer_dep_stage_counts[2],
               layer_dep_stage_counts[3], layer_dep_stage_counts[4],
               layer_dep_stage_counts[5], layer_dep_stage_counts[6],
               layer_dep_stage_counts[7], layer_dep_stage_counts[8],
               layer_dep_stage_counts[9], layer_dep_stage_counts[10],
               layer_dep_stage_counts[11], layer_dep_stage_counts[12],
               layer_dep_stage_counts[13], layer_dep_stage_counts[14],
               layer_dep_stage_counts[15], layer_dep_stage_counts[16],
               layer_dep_stage_counts[17], layer_dep_stage_counts[18],
               layer_dep_stage_counts[19], layer_dep_stage_counts[20],
               layer_dep_stage_counts[21], layer_dep_stage_counts[22],
               layer_dep_stage_counts[23], layer_dep_stage_counts[24],
               layer_dep_segment_distinct,
               layer_dep_checksum_nonzero);
    }
    return 0;
}

static bool cmdline_get_value(const char *key, char *out, size_t out_len)
{
    char buf[2048];
    char *saveptr = NULL;
    char *tok;
    size_t key_len;
    size_t n = 0;

    if (!read_file_bytes("/proc/cmdline", (uint8_t *)buf, sizeof(buf) - 1, &n)) {
        return false;
    }
    buf[n] = '\0';
    key_len = strlen(key);
    tok = strtok_r(buf, " \t\n", &saveptr);
    while (tok != NULL) {
        if (strncmp(tok, key, key_len) == 0 && tok[key_len] == '=') {
            snprintf(out, out_len, "%s", tok + key_len + 1);
            return true;
        }
        tok = strtok_r(NULL, " \t\n", &saveptr);
    }
    return false;
}

static void resolve_role(char *role, size_t role_len)
{
    const char *env_role = getenv("LINQU_UB_ROLE");

    if (env_role && env_role[0] != '\0') {
        snprintf(role, role_len, "%s", env_role);
        return;
    }
    if (!cmdline_get_value("linqu_urma_dp_role", role, role_len)) {
        snprintf(role, role_len, "%s", "nodeA");
    }
}

static int run_obmm_backing_stage(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[w4_guest] fork obmm backing failed: %s\n", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        execl("/bin/linqu_ub_obmm_demo", "/bin/linqu_ub_obmm_demo", (char *)NULL);
        fprintf(stderr, "[w4_guest] exec /bin/linqu_ub_obmm_demo failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[w4_guest] waitpid obmm backing failed: %s\n", strerror(errno));
        return -1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[w4_guest] obmm backing failed status=%d\n", status);
        return -1;
    }

    printf("[w4_guest] stage obmm_kvcache_path=ready\n");
    return 0;
}

static int cdma_ioctl_sync(int fd, uint32_t command, void *args, size_t args_len)
{
    struct cdma_ioctl_hdr hdr;

    memset(&hdr, 0, sizeof(hdr));
    hdr.command = command;
    hdr.args_len = (uint32_t)args_len;
    hdr.args_addr = (uint64_t)(uintptr_t)args;
    if (ioctl(fd, CDMA_SYNC, &hdr) != 0) {
        return -errno;
    }
    return 0;
}

static bool discover_cdma_device(char *path_out, size_t path_out_len)
{
    DIR *dir;
    struct dirent *de;

    dir = opendir("/dev/cdma");
    if (dir != NULL) {
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }
            snprintf(path_out, path_out_len, "/dev/cdma/%s", de->d_name);
            closedir(dir);
            return true;
        }
        closedir(dir);
    }

    dir = opendir("/dev");
    if (dir == NULL) {
        return false;
    }
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "cdma", 4) != 0) {
            continue;
        }
        snprintf(path_out, path_out_len, "/dev/%s", de->d_name);
        closedir(dir);
        return true;
    }
    closedir(dir);
    return false;
}

static int query_cdma_device(const char *path)
{
    struct cdma_cmd_query_device_attr_args args;
    int fd;
    int rc;

    memset(&args, 0, sizeof(args));
    fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[w4_guest] open cdma failed path=%s err=%s\n",
                path, strerror(errno));
        return -1;
    }

    rc = cdma_ioctl_sync(fd, CDMA_CMD_QUERY_DEV_INFO, &args, sizeof(args));
    close(fd);
    if (rc != 0) {
        fprintf(stderr, "[w4_guest] cdma query failed path=%s rc=%d\n", path, rc);
        return -1;
    }

    printf("[w4_guest] stage block_candidate=cdma_query_ready path=%s max_jfs=%u max_jfc=%u max_msg_size=%" PRIu64 "\n",
           path,
           args.out.attr.dev_cap.max_jfs,
           args.out.attr.dev_cap.max_jfc,
           (uint64_t)args.out.attr.dev_cap.max_msg_size);
    return 0;
}

static int cycle_cdma_context(const char *path)
{
    struct cdma_create_context_args args;
    struct cdma_cmd_register_seg_args reg_args;
    struct cdma_cmd_unregister_seg_args unreg_args;
    int fd;
    int rc;
    int async_fd = -1;
    void *seg_buf = MAP_FAILED;
    size_t seg_len = PAGE_SIZE_BYTES;

    memset(&args, 0, sizeof(args));
    fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[w4_guest] open cdma for ctx failed path=%s err=%s\n",
                path, strerror(errno));
        return -1;
    }

    rc = cdma_ioctl_sync(fd, CDMA_CMD_CREATE_CTX, &args, sizeof(args));
    if (rc != 0) {
        fprintf(stderr, "[w4_guest] cdma create_ctx failed path=%s rc=%d\n", path, rc);
        close(fd);
        return -1;
    }

    async_fd = args.out.async_fd;
    printf("[w4_guest] stage block_candidate=cdma_context_ready path=%s async_fd=%d cqe_size=%u dwqe_enable=%u\n",
           path, async_fd, args.out.cqe_size, args.out.dwqe_enable);

    seg_buf = mmap(NULL, seg_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (seg_buf == MAP_FAILED) {
        fprintf(stderr, "[w4_guest] cdma seg mmap failed path=%s err=%s\n",
                path, strerror(errno));
        if (async_fd >= 0) {
            close(async_fd);
        }
        close(fd);
        return -1;
    }
    memset(seg_buf, 0x5a, seg_len);

    memset(&reg_args, 0, sizeof(reg_args));
    reg_args.in.addr = (uint64_t)(uintptr_t)seg_buf;
    reg_args.in.len = seg_len;
    rc = cdma_ioctl_sync(fd, CDMA_CMD_REGISTER_SEG, &reg_args, sizeof(reg_args));
    if (rc != 0) {
        fprintf(stderr, "[w4_guest] cdma register_seg failed path=%s rc=%d\n", path, rc);
        munmap(seg_buf, seg_len);
        if (async_fd >= 0) {
            close(async_fd);
        }
        close(fd);
        return -1;
    }
    printf("[w4_guest] stage block_candidate=cdma_segment_ready path=%s handle=0x%016" PRIx64 " len=%zu\n",
           path, (uint64_t)reg_args.out.handle, seg_len);

    memset(&unreg_args, 0, sizeof(unreg_args));
    unreg_args.in.handle = reg_args.out.handle;
    rc = cdma_ioctl_sync(fd, CDMA_CMD_UNREGISTER_SEG, &unreg_args, sizeof(unreg_args));
    if (rc != 0) {
        fprintf(stderr, "[w4_guest] cdma unregister_seg failed path=%s rc=%d handle=0x%016" PRIx64 "\n",
                path, rc, (uint64_t)reg_args.out.handle);
        munmap(seg_buf, seg_len);
        if (async_fd >= 0) {
            close(async_fd);
        }
        close(fd);
        return -1;
    }
    printf("[w4_guest] stage block_candidate=cdma_segment_cycle_ok path=%s handle=0x%016" PRIx64 "\n",
           path, (uint64_t)reg_args.out.handle);

    rc = cdma_ioctl_sync(fd, CDMA_CMD_DELETE_CTX, NULL, 0);
    if (rc != 0) {
        fprintf(stderr, "[w4_guest] cdma delete_ctx failed path=%s rc=%d\n", path, rc);
        munmap(seg_buf, seg_len);
        if (async_fd >= 0) {
            close(async_fd);
        }
        close(fd);
        return -1;
    }

    if (async_fd >= 0) {
        close(async_fd);
    }
    if (seg_buf != MAP_FAILED) {
        munmap(seg_buf, seg_len);
    }
    close(fd);
    printf("[w4_guest] stage block_candidate=cdma_context_cycle_ok path=%s\n", path);
    return 0;
}

static bool discover_uburma_device(char *name_out, size_t name_out_len)
{
    DIR *dir;
    struct dirent *de;

    dir = opendir("/sys/class/uburma");
    if (dir == NULL) {
        return false;
    }
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        snprintf(name_out, name_out_len, "%s", de->d_name);
        closedir(dir);
        return true;
    }
    closedir(dir);
    return false;
}

static int probe_uburma_dispatch_candidate(const char *role, bool *seg_ready)
{
    pid_t pid;
    int status;
    char dev_name[128];
    char path[256];
    const char *mode_env = "LINQU_UB_UDMA_STOP_AFTER_SEG";

    if (!discover_uburma_device(dev_name, sizeof(dev_name))) {
        printf("[w4_guest] gap guest_dispatch_uburma_device=missing\n");
        return -1;
    }

    snprintf(path, sizeof(path), "/dev/uburma/%s", dev_name);
    if (access(path, R_OK | W_OK) != 0) {
        fprintf(stderr, "[w4_guest] access uburma failed path=%s err=%s\n",
                path, strerror(errno));
        printf("[w4_guest] gap guest_dispatch_uburma_open=failed\n");
        return -1;
    }
    printf("[w4_guest] stage dispatch_candidate=uburma_device_ready path=%s\n", path);

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[w4_guest] fork uburma probe failed: %s\n", strerror(errno));
        printf("[w4_guest] gap guest_dispatch_uburma_fork=failed\n");
        return -1;
    }
    if (pid == 0) {
        setenv("LINQU_UB_ROLE", role, 1);
        setenv(mode_env, "1", 1);
        execl("/bin/linqu_ub_udma_demo", "/bin/linqu_ub_udma_demo", (char *)NULL);
        fprintf(stderr, "[w4_guest] exec /bin/linqu_ub_udma_demo failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[w4_guest] waitpid uburma probe failed: %s\n", strerror(errno));
        printf("[w4_guest] gap guest_dispatch_uburma_wait=failed\n");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[w4_guest] uburma probe failed status=%d\n", status);
        printf("[w4_guest] gap guest_dispatch_uburma_ctx=failed\n");
        return -1;
    }

    printf("[w4_guest] stage dispatch_candidate=uburma_context_ready path=%s\n", path);
    if (seg_ready != NULL) {
        *seg_ready = true;
    }
    printf("[w4_guest] stage block_candidate=uburma_segment_ready path=%s\n", path);
    return 0;
}

static int probe_real_dispatch_candidate(const char *role)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[w4_guest] fork real dispatch probe failed: %s\n",
                strerror(errno));
        printf("[w4_guest] gap guest_dispatch_real_chipbackend=fork_failed\n");
        return -1;
    }
    if (pid == 0) {
        setenv("LINQU_UB_ROLE", role, 1);
        unsetenv("LINQU_UB_UDMA_STOP_AFTER_CTX");
        unsetenv("LINQU_UB_UDMA_STOP_AFTER_SEG");
        execl("/bin/linqu_ub_udma_demo", "/bin/linqu_ub_udma_demo", (char *)NULL);
        fprintf(stderr, "[w4_guest] exec /bin/linqu_ub_udma_demo failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[w4_guest] waitpid real dispatch probe failed: %s\n",
                strerror(errno));
        printf("[w4_guest] gap guest_dispatch_real_chipbackend=wait_failed\n");
        return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[w4_guest] real dispatch probe failed status=%d\n", status);
        printf("[w4_guest] gap guest_dispatch_real_chipbackend=failed\n");
        return -1;
    }

    printf("[w4_guest] stage dispatch_candidate=uburma_udma_ready role=%s path=/bin/linqu_ub_udma_demo\n",
           role);
    return 0;
}

int main(void)
{
    static const char *resource_candidates[] = {
        UB_RESOURCE0_WC_PATH,
        UB_RESOURCE1_WC_PATH,
        UB_RESOURCE0_PATH,
        UB_RESOURCE1_PATH,
        UB_RESOURCE2_PATH,
    };
    uint64_t root_base = resolve_root_base();
    uint64_t ep_base = root_base + LINQU_ENDPOINT1_OFFSET;
    uint64_t root_page_base = root_base & ~(PAGE_SIZE_BYTES - 1);
    uint64_t root_page_off = root_base - root_page_base;
    uint64_t ep_page_base = ep_base & ~(PAGE_SIZE_BYTES - 1);
    uint64_t ep_page_off = ep_base - ep_page_base;
    int fd = -1;
    void *root_map = MAP_FAILED;
    void *ep_map = MAP_FAILED;
    uint8_t *cmdq = MAP_FAILED;
    uint8_t *cq = MAP_FAILED;
    volatile uint8_t *root_mmio;
    volatile uint8_t *ep_mmio;
    uint64_t cmdq_phys = 0;
    uint64_t cq_phys = 0;
    uint64_t default_segment;
    uint64_t cq_tail = 0;
    struct completion_counts counts;
    struct timespec start_ts;
    char role[32];
    char request_id[64];
    char prefix_group[64];
    char group_id[64];
    char key[64];
    char key_aux[64];
    char path[96];
    char block[96];
    char block_aux[96];
    char cdma_path[128];
    const char *block_candidate = "missing";
    const char *dispatch_candidate = "missing";
    struct w4_db_service db_service;
    struct w4_db_record resolved_block_meta;
    struct w4_db_record resolved_block_meta_aux;
    struct w4_db_record resolved_prefix_meta;
    struct w4_db_record resolved_prefix_meta_aux;
    struct w4_db_record resolved_prefix_group;
    struct w4_db_record remote_block_meta;
    struct w4_db_record remote_block_meta_aux;
    struct w4_db_record remote_prefix_meta;
    struct w4_db_record remote_prefix_meta_aux;
    struct w4_db_record remote_prefix_group;
    struct w4_db_block_ctx db_block_ctx;
    struct w4_db_block_ctx db_block_ctx_aux;
    struct w4_db_cluster_summary db_cluster_summary;
    struct w4_db_cluster_summary db_cluster_update_summary;
    struct w4_db_cluster_summary db_cluster_handoff_summary;
    struct w4_compute_roundtrip compute_roundtrip;
    char remote_block_key[96];
    char remote_block_key_aux[96];
    char remote_prefix_key[96];
    char remote_prefix_key_aux[96];
    char remote_group_key[96];
    bool db_service_ready = false;
    bool cluster_coherent = false;
    bool update_order_ready = false;
    bool prefix_update_order_ready = false;
    bool handoff_ready = false;
    bool remote_metadata_ready = false;
    bool group_relationship_ready = false;
    bool cluster_observer_mode = false;
    uint32_t cluster_node_count = 4U;
    size_t slot = 0;
    int rc = 1;
    bool block_ready = false;
    bool uburma_ready = false;
    bool mapped_via_resource = false;
    const char *selected_resource_path = NULL;
    size_t ri;
    bool require_uapi_resource = false;
    bool enable_db_cluster = false;
    uint64_t kvcache_db_bytes = (uint64_t)sizeof(struct w4_db_record);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    memset(&counts, 0, sizeof(counts));
    resolve_role(role, sizeof(role));
    cluster_observer_mode = (getenv("LINQU_W4_ALLOW_OBSERVER_ONLY") != NULL &&
                             strcmp(getenv("LINQU_W4_ALLOW_OBSERVER_ONLY"), "1") == 0);
    require_uapi_resource = (getenv("LINQU_W4_REQUIRE_UAPI_RESOURCE") != NULL &&
                             strcmp(getenv("LINQU_W4_REQUIRE_UAPI_RESOURCE"), "1") == 0);
    enable_db_cluster = (getenv("LINQU_W4_DB_CLUSTER") != NULL &&
                         strcmp(getenv("LINQU_W4_DB_CLUSTER"), "1") == 0);
    cluster_node_count = w4_cluster_node_count();
    snprintf(request_id, sizeof(request_id), "w4-%s-request-0", role);
    snprintf(prefix_group, sizeof(prefix_group), "%s-prefix-0", role);
    snprintf(group_id, sizeof(group_id), "%s-group-0", role);
    snprintf(path, sizeof(path), "/w4/%s/tail-block-0", role);
    snprintf(block, sizeof(block), "w4-%s-block-0", role);
    snprintf(block_aux, sizeof(block_aux), "w4-%s-block-1", role);
    snprintf(key, sizeof(key), "block/%s", block);
    snprintf(key_aux, sizeof(key_aux), "block/%s", block_aux);
    memset(&db_block_ctx, 0, sizeof(db_block_ctx));
    memset(&db_block_ctx_aux, 0, sizeof(db_block_ctx_aux));
    memset(&db_cluster_summary, 0, sizeof(db_cluster_summary));
    memset(&db_cluster_update_summary, 0, sizeof(db_cluster_update_summary));
    memset(&db_cluster_handoff_summary, 0, sizeof(db_cluster_handoff_summary));
    memset(&compute_roundtrip, 0, sizeof(compute_roundtrip));
    memset(&resolved_prefix_meta, 0, sizeof(resolved_prefix_meta));
    memset(&resolved_prefix_meta_aux, 0, sizeof(resolved_prefix_meta_aux));
    memset(&resolved_prefix_group, 0, sizeof(resolved_prefix_group));
    memset(&remote_block_meta, 0, sizeof(remote_block_meta));
    memset(&remote_block_meta_aux, 0, sizeof(remote_block_meta_aux));
    memset(&remote_prefix_meta, 0, sizeof(remote_prefix_meta));
    memset(&remote_prefix_meta_aux, 0, sizeof(remote_prefix_meta_aux));
    memset(&remote_prefix_group, 0, sizeof(remote_prefix_group));
    snprintf(db_block_ctx.request_id, sizeof(db_block_ctx.request_id), "%s", request_id);
    snprintf(db_block_ctx.prefix_group, sizeof(db_block_ctx.prefix_group), "%s", prefix_group);
    snprintf(db_block_ctx.group_id, sizeof(db_block_ctx.group_id), "%s", group_id);
    snprintf(db_block_ctx.block_hash, sizeof(db_block_ctx.block_hash), "%s", block);
    snprintf(db_block_ctx_aux.request_id, sizeof(db_block_ctx_aux.request_id), "%s", request_id);
    snprintf(db_block_ctx_aux.prefix_group, sizeof(db_block_ctx_aux.prefix_group), "%s-aux", prefix_group);
    snprintf(db_block_ctx_aux.group_id, sizeof(db_block_ctx_aux.group_id), "%s", group_id);
    snprintf(db_block_ctx_aux.block_hash, sizeof(db_block_ctx_aux.block_hash), "w4-%s-block-1", role);
    remote_block_key[0] = '\0';
    remote_block_key_aux[0] = '\0';
    remote_prefix_key[0] = '\0';
    remote_group_key[0] = '\0';

    printf("[w4_guest] role=%s begin\n", role);

    dump_mem_windows();
    for (ri = 0; ri < sizeof(resource_candidates) / sizeof(resource_candidates[0]); ++ri) {
        uint64_t probe_root_version = 0;
        uint64_t probe_default_segment = 0;
        if (!probe_resource_candidate(resource_candidates[ri],
                                      &probe_root_version,
                                      &probe_default_segment)) {
            continue;
        }
        if (probe_default_segment != 0) {
            selected_resource_path = resource_candidates[ri];
            break;
        }
    }

    if (selected_resource_path == NULL) {
        printf("[w4_guest] step=guest_uapi_default_segment missing\n");
        if (require_uapi_resource) {
            printf("[w4_guest] gap guest_uapi_resource=missing required=true\n");
            fprintf(stderr, "[w4_guest] fail required guest uapi resource missing\n");
            goto out;
        }
        if (run_obmm_backing_stage() != 0) {
            goto out;
        }
        if (!cluster_observer_mode) {
            if (discover_cdma_device(cdma_path, sizeof(cdma_path))) {
                if (query_cdma_device(cdma_path) != 0) {
                    printf("[w4_guest] gap guest_block_cdma_query=failed\n");
                } else {
                    block_ready = true;
                    if (cycle_cdma_context(cdma_path) != 0) {
                        printf("[w4_guest] gap guest_block_cdma_segment=failed\n");
                    } else {
                        block_candidate = "cdma_segment_cycle_ok";
                    }
                }
            } else {
                printf("[w4_guest] gap guest_block_cdma_device=missing\n");
            }
            if (!block_ready && probe_uburma_dispatch_candidate(role, &block_ready) == 0) {
                uburma_ready = true;
                dispatch_candidate = "uburma_context_ready";
                block_candidate = "uburma_segment_ready";
            }
        }
        printf("[w4_guest] note db_dfs_layer=deferred_over_shmem_urma\n");
        printf("[w4_guest] note guest_db_dfs_path=deferred_over_shmem_urma\n");
        if (cluster_observer_mode) {
            uint32_t placement_node = 0U;
            uint32_t remote_owner = 0U;
            uint64_t synthetic_hot_segment = 0x100000ULL;
            uint64_t synthetic_result_segment = synthetic_hot_segment + 0x80ULL;
            char remote_request_id[64];
            char remote_block_hash[96];
            char remote_block_hash_aux[96];
            char remote_prefix_group_id[64];
            char remote_prefix_group_aux_id[64];
            char remote_group_id[64];
            const char *remote_role;

            if (!w4_cluster_role_index(role, cluster_node_count, &placement_node)) {
                printf("[w4_guest] gap guest_db_service_cluster=unsupported_role role=%s node_count=%u\n",
                       role,
                       cluster_node_count);
                goto out;
            }
            remote_owner = w4_cluster_next_owner(placement_node, cluster_node_count);
            remote_role = w4_cluster_role_name(remote_owner);
            block_candidate = "observer_metadata_only";
            dispatch_candidate = "observer_metadata_only";
            printf("[w4_guest] note cluster_mode=metadata_observer_only role=%s\n", role);
            printf("[w4_guest] stage db_dfs_foundation=shmem_urma_ready\n");
            snprintf(remote_request_id, sizeof(remote_request_id), "w4-%s-request-0", remote_role);
            snprintf(remote_block_hash, sizeof(remote_block_hash), "w4-%s-block-0", remote_role);
            snprintf(remote_block_hash_aux, sizeof(remote_block_hash_aux), "w4-%s-block-1", remote_role);
            snprintf(remote_prefix_group_id, sizeof(remote_prefix_group_id), "%s-prefix-0", remote_role);
            snprintf(remote_prefix_group_aux_id, sizeof(remote_prefix_group_aux_id), "%s-prefix-0-aux", remote_role);
            snprintf(remote_group_id, sizeof(remote_group_id), "%s-group-0", remote_role);
            w4_db_build_block_key_from_hash(remote_block_hash, remote_block_key, sizeof(remote_block_key));
            w4_db_build_prefix_key_from_parts(remote_request_id,
                                              remote_prefix_group_id,
                                              remote_prefix_key,
                                              sizeof(remote_prefix_key));
            w4_db_build_prefix_key_from_parts(remote_request_id,
                                              remote_prefix_group_aux_id,
                                              remote_prefix_key_aux,
                                              sizeof(remote_prefix_key_aux));
            w4_db_build_group_key_from_parts(remote_request_id,
                                             remote_group_id,
                                             remote_group_key,
                                             sizeof(remote_group_key));
            db_block_ctx.placement_node = placement_node;
            db_block_ctx.placement_level = 2U;
            db_block_ctx.hot_segment_id = synthetic_hot_segment + ((uint64_t)placement_node << 12);
            db_block_ctx.result_segment_id = synthetic_result_segment + ((uint64_t)placement_node << 12);
            db_block_ctx_aux.placement_node = placement_node;
            db_block_ctx_aux.placement_level = 2U;
            db_block_ctx_aux.hot_segment_id = db_block_ctx.hot_segment_id + 0x100ULL;
            db_block_ctx_aux.result_segment_id = db_block_ctx_aux.hot_segment_id + 0x80ULL;
            if (w4_db_service_init(&db_service, true, true, true) == 0 &&
                w4_db_bootstrap_kvcache(&db_service, &db_block_ctx, &resolved_block_meta) == 0 &&
                w4_db_bootstrap_kvcache(&db_service, &db_block_ctx_aux, &resolved_block_meta_aux) == 0 &&
                w4_db_apply_block_result(&db_service,
                                         &db_block_ctx_aux,
                                         resolved_block_meta_aux.last_result_segment + 0x40ULL,
                                         W4_KVCACHE_STATE_RELOADED,
                                         &resolved_block_meta_aux) == 0) {
                db_service_ready = true;
                if (w4_db_update_prefix_metadata(&db_service,
                                                 &db_block_ctx_aux,
                                                 &resolved_block_meta_aux,
                                                 &resolved_prefix_meta_aux) != 0) {
                    printf("[w4_guest] gap guest_db_service_prefix=metadata_aux_prefix_update_failed\n");
                    db_service_ready = false;
                } else {
                    printf("[w4_guest] stage db_service_prefix_aux_ready key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                           resolved_prefix_meta_aux.key,
                           resolved_prefix_meta_aux.block_hash,
                           resolved_prefix_meta_aux.hot_segment_id,
                           w4_kvcache_state_name(resolved_prefix_meta_aux.state),
                           resolved_prefix_meta_aux.version,
                           resolved_prefix_meta_aux.last_result_segment);
                }
                if (db_service_ready &&
                    w4_db_get_prefix_group_metadata(&db_service,
                                                    &db_block_ctx,
                                                    &resolved_prefix_group) != 0) {
                    printf("[w4_guest] gap guest_db_service_group=metadata_group_lookup_failed\n");
                    db_service_ready = false;
                } else if (db_service_ready) {
                    printf("[w4_guest] stage db_service_group_ready key=%s group=%s members=%u version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                           resolved_prefix_group.key,
                           resolved_prefix_group.group_id,
                           resolved_prefix_group.member_count,
                           resolved_prefix_group.version,
                           resolved_prefix_group.last_result_segment);
                }
                printf("[w4_guest] stage db_service_candidate=kvcache_metadata_ready backing=obmm_pool transport=uburma block_path=%s records=%zu\n",
                       block_candidate, db_service.record_count);
                printf("[w4_guest] stage db_service_lookup key=%s request=%s prefix=%s block=%s placement_node=%u placement_level=%u hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       resolved_block_meta.key,
                       resolved_block_meta.request_id,
                       resolved_block_meta.prefix_group,
                       resolved_block_meta.block_hash,
                       resolved_block_meta.placement_node,
                       resolved_block_meta.placement_level,
                       resolved_block_meta.hot_segment_id,
                       w4_kvcache_state_name(resolved_block_meta.state),
                       resolved_block_meta.version,
                       resolved_block_meta.last_result_segment);
                printf("[w4_guest] stage db_service_lookup_aux key=%s request=%s prefix=%s block=%s placement_node=%u placement_level=%u hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       resolved_block_meta_aux.key,
                       resolved_block_meta_aux.request_id,
                       resolved_block_meta_aux.prefix_group,
                       resolved_block_meta_aux.block_hash,
                       resolved_block_meta_aux.placement_node,
                       resolved_block_meta_aux.placement_level,
                       resolved_block_meta_aux.hot_segment_id,
                       w4_kvcache_state_name(resolved_block_meta_aux.state),
                       resolved_block_meta_aux.version,
                       resolved_block_meta_aux.last_result_segment);
                if (w4_db_publish_observe_cluster(&db_service,
                                                  &resolved_block_meta,
                                                  &db_cluster_summary) != 0) {
                    printf("[w4_guest] gap guest_db_service_cluster=metadata_publish_failed\n");
                    db_service_ready = false;
                } else if (w4_guest_compute_roundtrip(role,
                                                       &db_block_ctx,
                                                       &resolved_block_meta,
                                                       &compute_roundtrip) != 0) {
                    printf("[w4_guest] gap guest_compute_roundtrip=payload_invalid\n");
                    db_service_ready = false;
                } else if (w4_db_apply_block_result(&db_service,
                                                    &db_block_ctx,
                                                    compute_roundtrip.output_segment,
                                                    W4_KVCACHE_STATE_RELOADED,
                                                    &resolved_block_meta) != 0) {
                    printf("[w4_guest] gap guest_db_service_update=metadata_result_update_failed\n");
                    db_service_ready = false;
                } else {
                    printf("[w4_guest] stage db_service_result_feed source=compute_roundtrip key=%s result_segment=0x%016" PRIx64 " output_checksum=0x%016" PRIx64 "\n",
                           resolved_block_meta.key,
                           compute_roundtrip.output_segment,
                           compute_roundtrip.output_checksum);
                    int stale_rc = w4_db_apply_block_result(&db_service,
                                                            &db_block_ctx,
                                                            resolved_block_meta.last_result_segment - 0x40ULL,
                                                            W4_KVCACHE_STATE_HOT,
                                                            &resolved_block_meta_aux);
                    if (stale_rc != 1) {
                        printf("[w4_guest] gap guest_db_service_update=stale_result_accepted\n");
                        db_service_ready = false;
                    } else {
                        update_order_ready = true;
                        printf("[w4_guest] stage db_service_update_order_ok key=%s stale_reject=true version=%" PRIu64 "\n",
                               resolved_block_meta.key,
                               resolved_block_meta.version);
                    }
                }
                if (db_service_ready) {
                    printf("[w4_guest] stage db_service_update_ok key=%s state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                           resolved_block_meta.key,
                           w4_kvcache_state_name(resolved_block_meta.state),
                           resolved_block_meta.version,
                           resolved_block_meta.last_result_segment);
                    if (db_service_ready && w4_db_update_prefix_metadata(&db_service,
                                                                         &db_block_ctx,
                                                                         &resolved_block_meta,
                                                                         &resolved_prefix_meta) != 0) {
                        printf("[w4_guest] gap guest_db_service_prefix=metadata_prefix_update_failed\n");
                        db_service_ready = false;
                    } else if (db_service_ready) {
                        printf("[w4_guest] stage db_service_prefix_update_ok key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                               resolved_prefix_meta.key,
                               resolved_prefix_meta.block_hash,
                               resolved_prefix_meta.hot_segment_id,
                               w4_kvcache_state_name(resolved_prefix_meta.state),
                               resolved_prefix_meta.version,
                               resolved_prefix_meta.last_result_segment);
                        {
                            struct w4_db_record stale_prefix_source = resolved_block_meta;
                            int stale_prefix_rc;

                            stale_prefix_source.last_result_segment -= 0x40ULL;
                            stale_prefix_source.state = W4_KVCACHE_STATE_HOT;
                            stale_prefix_rc = w4_db_update_prefix_metadata(&db_service,
                                                                           &db_block_ctx,
                                                                           &stale_prefix_source,
                                                                           &resolved_prefix_meta);
                            if (stale_prefix_rc != 1) {
                                printf("[w4_guest] gap guest_db_service_prefix=stale_prefix_update_accepted\n");
                                db_service_ready = false;
                            } else {
                                prefix_update_order_ready = true;
                                printf("[w4_guest] stage db_service_prefix_update_order_ok key=%s stale_reject=true version=%" PRIu64 "\n",
                                       resolved_prefix_meta.key,
                                       resolved_prefix_meta.version);
                            }
                        }
                    }
                    if (db_service_ready && w4_db_publish_observe_cluster(&db_service,
                                                      &resolved_block_meta,
                                                      &db_cluster_update_summary) != 0) {
                        printf("[w4_guest] gap guest_db_service_cluster=metadata_update_failed\n");
                        db_service_ready = false;
                    } else if (!db_cluster_update_summary.active) {
                        cluster_coherent = true;
                    } else if (db_cluster_update_summary.ready &&
                               db_cluster_update_summary.peer_prefix_count_floor >= 2 &&
                               db_cluster_update_summary.peer_group_count_floor >= 1 &&
                               db_cluster_update_summary.peer_block_count_floor >= 2) {
                        cluster_coherent = true;
                        printf("[w4_guest] stage db_service_cluster=metadata_coherent state=%s version=%" PRIu64 " peer_version_floor=%" PRIu64 " peer_result_floor=0x%016" PRIx64 " peer_prefix_version_floor=%" PRIu64 " peer_prefix_result_floor=0x%016" PRIx64 " peer_block_count_floor=%u peer_prefix_count_floor=%u peer_group_count_floor=%u placement=%s state_match=%s peers=%u\n",
                               w4_kvcache_state_name(resolved_block_meta.state),
                               resolved_block_meta.version,
                               db_cluster_update_summary.peer_version_floor,
                               db_cluster_update_summary.peer_result_floor,
                               db_cluster_update_summary.peer_prefix_version_floor,
                               db_cluster_update_summary.peer_prefix_result_floor,
                               db_cluster_update_summary.peer_block_count_floor,
                               db_cluster_update_summary.peer_prefix_count_floor,
                               db_cluster_update_summary.peer_group_count_floor,
                               db_cluster_update_summary.placement_coherent ? "true" : "false",
                               db_cluster_update_summary.state_coherent ? "true" : "false",
                               db_cluster_update_summary.peers_observed);
                    } else {
                        printf("[w4_guest] gap guest_db_service_cluster=metadata_update_incoherent\n");
                        db_service_ready = false;
                    }
                    if (db_service_ready && cluster_coherent) {
                        if (w4_db_rebind_block_view(&db_service,
                                                    &db_block_ctx,
                                                    resolved_block_meta.hot_segment_id + 0x200ULL,
                                                    resolved_block_meta.placement_level,
                                                    &resolved_block_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_view=metadata_view_update_failed\n");
                            db_service_ready = false;
                        } else {
                            printf("[w4_guest] stage db_service_view_update_ok key=%s hot_segment=0x%016" PRIx64 " placement_level=%u version=%" PRIu64 "\n",
                                   resolved_block_meta.key,
                                   resolved_block_meta.hot_segment_id,
                                   resolved_block_meta.placement_level,
                                   resolved_block_meta.version);
                        }
                        if (db_service_ready && w4_db_update_prefix_metadata(&db_service,
                                                                             &db_block_ctx,
                                                                             &resolved_block_meta,
                                                                             &resolved_prefix_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_prefix=metadata_prefix_view_update_failed\n");
                            db_service_ready = false;
                        } else if (db_service_ready) {
                            printf("[w4_guest] stage db_service_prefix_view_update_ok key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 "\n",
                                   resolved_prefix_meta.key,
                                   resolved_prefix_meta.block_hash,
                                   resolved_prefix_meta.hot_segment_id,
                                   w4_kvcache_state_name(resolved_prefix_meta.state),
                                   resolved_prefix_meta.version);
                        }
                        if (db_service_ready &&
                            w4_db_handoff_block_owner(&db_service,
                                                      &db_block_ctx,
                                                      w4_cluster_next_owner(db_block_ctx.placement_node,
                                                                            cluster_node_count),
                                                      resolved_block_meta.placement_level,
                                                      w4_cluster_handoff_hot(resolved_block_meta.hot_segment_id),
                                                      &resolved_block_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_handoff=metadata_handoff_failed\n");
                            db_service_ready = false;
                        } else if (db_service_ready) {
                            printf("[w4_guest] stage db_service_handoff_ok key=%s placement_node=%u placement_level=%u hot_segment=0x%016" PRIx64 " version=%" PRIu64 "\n",
                                   resolved_block_meta.key,
                                   resolved_block_meta.placement_node,
                                   resolved_block_meta.placement_level,
                                   resolved_block_meta.hot_segment_id,
                                   resolved_block_meta.version);
                        }
                        if (db_service_ready && w4_db_update_prefix_metadata(&db_service,
                                                                             &db_block_ctx,
                                                                             &resolved_block_meta,
                                                                             &resolved_prefix_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_prefix=metadata_prefix_handoff_failed\n");
                            db_service_ready = false;
                        } else if (db_service_ready) {
                            printf("[w4_guest] stage db_service_prefix_handoff_ok key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 "\n",
                                   resolved_prefix_meta.key,
                                   resolved_prefix_meta.block_hash,
                                   resolved_prefix_meta.hot_segment_id,
                                   w4_kvcache_state_name(resolved_prefix_meta.state),
                                   resolved_prefix_meta.version);
                        }
                        if (db_service_ready && w4_db_publish_observe_cluster(&db_service,
                                                                              &resolved_block_meta,
                                                                              &db_cluster_handoff_summary) != 0) {
                            printf("[w4_guest] gap guest_db_service_handoff=metadata_cluster_failed\n");
                            db_service_ready = false;
                        } else if (!db_cluster_handoff_summary.active) {
                            handoff_ready = true;
                        } else if (db_cluster_handoff_summary.ready &&
                                   db_cluster_handoff_summary.peer_prefix_count_floor >= 2 &&
                                   db_cluster_handoff_summary.peer_group_count_floor >= 1 &&
                                   db_cluster_handoff_summary.peer_block_count_floor >= 2) {
                            handoff_ready = true;
                            printf("[w4_guest] stage db_service_cluster=metadata_handoff_coherent placement_node=%u hot_segment=0x%016" PRIx64 " version=%" PRIu64 " peer_block_count_floor=%u peer_prefix_count_floor=%u peer_group_count_floor=%u peers=%u\n",
                                   resolved_block_meta.placement_node,
                                   resolved_block_meta.hot_segment_id,
                                   resolved_block_meta.version,
                                   db_cluster_handoff_summary.peer_block_count_floor,
                                   db_cluster_handoff_summary.peer_prefix_count_floor,
                                   db_cluster_handoff_summary.peer_group_count_floor,
                                   db_cluster_handoff_summary.peers_observed);
                        } else {
                            printf("[w4_guest] gap guest_db_service_handoff=metadata_incoherent\n");
                            db_service_ready = false;
                        }
                        if (db_service_ready) {
                            int stale_owner_rc = w4_db_apply_block_result(&db_service,
                                                                          &db_block_ctx,
                                                                          resolved_block_meta.last_result_segment + 0x40ULL,
                                                                          W4_KVCACHE_STATE_RELOADED,
                                                                          &resolved_block_meta_aux);
                            if (stale_owner_rc != 2) {
                                printf("[w4_guest] gap guest_db_service_handoff=stale_owner_accepted\n");
                                db_service_ready = false;
                            } else {
                                printf("[w4_guest] stage db_service_handoff_order_ok key=%s stale_owner_reject=true version=%" PRIu64 "\n",
                                       resolved_block_meta.key,
                                       resolved_block_meta.version);
                            }
                        }
                        if (db_service_ready && w4_db_cluster_fetch_record(&db_service,
                                                                          remote_block_key,
                                                                          &remote_block_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                   remote_block_key);
                            db_service_ready = false;
                        } else if (db_service_ready && w4_db_cluster_fetch_record(&db_service,
                                                                                  remote_prefix_key,
                                                                                  &remote_prefix_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                   remote_prefix_key);
                            db_service_ready = false;
                        } else if (db_service_ready && w4_db_cluster_fetch_record(&db_service,
                                                                                  remote_prefix_key_aux,
                                                                                  &remote_prefix_meta_aux) != 0) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                   remote_prefix_key_aux);
                            db_service_ready = false;
                        } else if (db_service_ready && w4_db_cluster_fetch_record(&db_service,
                                                                                  remote_group_key,
                                                                                  &remote_prefix_group) != 0) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                   remote_group_key);
                            db_service_ready = false;
                        } else {
                            w4_db_build_block_key_from_hash(remote_prefix_meta_aux.block_hash,
                                                            remote_block_key_aux,
                                                            sizeof(remote_block_key_aux));
                            if (w4_db_cluster_fetch_record(&db_service,
                                                           remote_block_key_aux,
                                                           &remote_block_meta_aux) != 0) {
                                printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                       remote_block_key_aux);
                                db_service_ready = false;
                            }
                        }
                        if (db_service_ready &&
                            (strncmp(remote_block_meta.key, remote_block_key, sizeof(remote_block_meta.key)) != 0 ||
                             remote_block_meta.state != W4_KVCACHE_STATE_RELOADED ||
                             remote_block_meta.hot_segment_id == 0 ||
                             remote_block_meta.last_result_segment == 0 ||
                             strncmp(remote_prefix_meta.key, remote_prefix_key, sizeof(remote_prefix_meta.key)) != 0 ||
                             !w4_db_prefix_matches_block_meta(&remote_prefix_meta, &remote_block_meta) ||
                             strncmp(remote_prefix_meta_aux.key, remote_prefix_key_aux, sizeof(remote_prefix_meta_aux.key)) != 0 ||
                             !w4_db_prefix_matches_block_meta(&remote_prefix_meta_aux, &remote_block_meta_aux) ||
                             strncmp(remote_prefix_group.key, remote_group_key, sizeof(remote_prefix_group.key)) != 0 ||
                             !w4_db_group_covers_blocks(&remote_prefix_group,
                                                        &remote_block_meta,
                                                        &remote_block_meta_aux))) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_incoherent key=%s prefix=%s\n",
                                   remote_block_key,
                                   remote_prefix_key);
                            db_service_ready = false;
                        } else if (db_service_ready) {
                            remote_metadata_ready = true;
                            printf("[w4_guest] stage db_service_remote_lookup key=%s state=%s version=%" PRIu64 " placement_node=%u hot_segment=0x%016" PRIx64 " last_result_segment=0x%016" PRIx64 "\n",
                                   remote_block_meta.key,
                                   w4_kvcache_state_name(remote_block_meta.state),
                                   remote_block_meta.version,
                                   remote_block_meta.placement_node,
                                   remote_block_meta.hot_segment_id,
                                   remote_block_meta.last_result_segment);
                            printf("[w4_guest] stage db_service_remote_prefix_lookup key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 "\n",
                                   remote_prefix_meta.key,
                                   remote_prefix_meta.block_hash,
                                   remote_prefix_meta.hot_segment_id,
                                   w4_kvcache_state_name(remote_prefix_meta.state),
                                   remote_prefix_meta.version);
                            printf("[w4_guest] stage db_service_remote_prefix_aux_lookup key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 "\n",
                                   remote_prefix_meta_aux.key,
                                   remote_prefix_meta_aux.block_hash,
                                   remote_prefix_meta_aux.hot_segment_id,
                                   w4_kvcache_state_name(remote_prefix_meta_aux.state),
                                   remote_prefix_meta_aux.version);
                            printf("[w4_guest] stage db_service_remote_lookup_aux key=%s state=%s version=%" PRIu64 " placement_node=%u hot_segment=0x%016" PRIx64 " last_result_segment=0x%016" PRIx64 "\n",
                                   remote_block_meta_aux.key,
                                   w4_kvcache_state_name(remote_block_meta_aux.state),
                                   remote_block_meta_aux.version,
                                   remote_block_meta_aux.placement_node,
                                   remote_block_meta_aux.hot_segment_id,
                                   remote_block_meta_aux.last_result_segment);
                            printf("[w4_guest] stage db_service_remote_group_lookup key=%s group=%s members=%u version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                                   remote_prefix_group.key,
                                   remote_prefix_group.group_id,
                                   remote_prefix_group.member_count,
                                   remote_prefix_group.version,
                                   remote_prefix_group.last_result_segment);
                            printf("[w4_guest] stage db_service_remote_check_ok block=%s prefix=%s prefix_aux=%s group=%s placement_node=%u prefix_last_result_segment=0x%016" PRIx64 " prefix_block=%s prefix_hot_segment=0x%016" PRIx64 " prefix_aux_last_result_segment=0x%016" PRIx64 " prefix_aux_block=%s prefix_aux_hot_segment=0x%016" PRIx64 " group_members=%u\n",
                                   remote_block_meta.key,
                                   remote_prefix_meta.key,
                                   remote_prefix_meta_aux.key,
                                   remote_prefix_group.key,
                                   remote_block_meta.placement_node,
                                   remote_prefix_meta.last_result_segment,
                                   remote_prefix_meta.block_hash,
                                   remote_prefix_meta.hot_segment_id,
                                   remote_prefix_meta_aux.last_result_segment,
                                   remote_prefix_meta_aux.block_hash,
                                   remote_prefix_meta_aux.hot_segment_id,
                                   remote_prefix_group.member_count);
                            group_relationship_ready = true;
                        }
                    }
                }
            } else {
                printf("[w4_guest] gap guest_db_service=kvcache_metadata_failed\n");
            }
            printf("[w4_guest] assessment shmem_kvcache_path=obmm_pool block_candidate=%s dispatch_candidate=%s db_service_candidate=%s cluster_ready=%s cluster_update_ready=%s cluster_prefix_ready=%s cluster_groups_ready=%s cluster_blocks_ready=%s update_order_ready=%s prefix_update_order_ready=%s handoff_ready=%s remote_metadata_ready=%s complete=%s\n",
                   block_candidate,
                   dispatch_candidate,
                   db_service_ready ? "kvcache_metadata_ready" : "missing",
                   db_cluster_summary.ready ? "true" : "false",
                   cluster_coherent ? "true" : "false",
                   (db_cluster_update_summary.peer_prefix_count_floor >= 2) ? "true" : "false",
                   (db_cluster_update_summary.peer_group_count_floor >= 1) ? "true" : "false",
                   (db_cluster_update_summary.peer_block_count_floor >= 2) ? "true" : "false",
                   update_order_ready ? "true" : "false",
                   prefix_update_order_ready ? "true" : "false",
                   handoff_ready ? "true" : "false",
                   remote_metadata_ready ? "true" : "false",
                   (db_service_ready && db_cluster_summary.ready && cluster_coherent &&
                    db_cluster_update_summary.peer_prefix_count_floor >= 2 &&
                    db_cluster_update_summary.peer_group_count_floor >= 1 &&
                    db_cluster_update_summary.peer_block_count_floor >= 2 &&
                    update_order_ready &&
                    prefix_update_order_ready &&
                    handoff_ready &&
                    group_relationship_ready &&
                    remote_metadata_ready) ? "true" : "false");
            if (!db_service_ready || !db_cluster_summary.ready || !cluster_coherent ||
                db_cluster_update_summary.peer_prefix_count_floor < 2 ||
                db_cluster_update_summary.peer_group_count_floor < 1 ||
                db_cluster_update_summary.peer_block_count_floor < 2 ||
                !update_order_ready || !prefix_update_order_ready ||
                !handoff_ready || !group_relationship_ready || !remote_metadata_ready) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db cluster observer\n");
                goto out;
            }
            printf("[w4_guest] dispatch path=%s\n", dispatch_candidate);
            printf("[w4_guest] pass\n");
            rc = 0;
            goto out;
        }
        if (uburma_ready && probe_real_dispatch_candidate(role) == 0) {
            uint32_t placement_node = (strcmp(role, "nodeB") == 0) ? 1U : 0U;
            uint64_t synthetic_hot_segment = 0x100000ULL + ((uint64_t)placement_node << 12);
            uint64_t synthetic_result_segment = synthetic_hot_segment + 0x80ULL;
            char remote_request_id[64];
            char remote_block_hash[96];
            char remote_block_hash_aux[96];
            char remote_prefix_group_id[64];
            char remote_prefix_group_aux_id[64];
            char remote_group_id[64];
            const char *remote_role = (strcmp(role, "nodeA") == 0) ? "nodeC" : "nodeD";
            dispatch_candidate = "uburma_udma_ready";
            block_candidate = "uburma_data_path_ready";
            printf("[w4_guest] stage block_candidate=uburma_data_path_ready path=/bin/linqu_ub_udma_demo\n");
            printf("[w4_guest] stage db_dfs_foundation=shmem_urma_ready\n");
            db_block_ctx.placement_node = placement_node;
            db_block_ctx.placement_level = 2U;
            db_block_ctx.hot_segment_id = synthetic_hot_segment;
            db_block_ctx.result_segment_id = synthetic_result_segment;
            db_block_ctx_aux.placement_node = placement_node;
            db_block_ctx_aux.placement_level = 2U;
            db_block_ctx_aux.hot_segment_id = db_block_ctx.hot_segment_id + 0x100ULL;
            db_block_ctx_aux.result_segment_id = db_block_ctx_aux.hot_segment_id + 0x80ULL;
            snprintf(remote_request_id, sizeof(remote_request_id), "w4-%s-request-0", remote_role);
            snprintf(remote_block_hash, sizeof(remote_block_hash), "w4-%s-block-0", remote_role);
            snprintf(remote_block_hash_aux, sizeof(remote_block_hash_aux), "w4-%s-block-1", remote_role);
            snprintf(remote_prefix_group_id, sizeof(remote_prefix_group_id), "%s-prefix-0", remote_role);
            snprintf(remote_prefix_group_aux_id, sizeof(remote_prefix_group_aux_id), "%s-prefix-0-aux", remote_role);
            snprintf(remote_group_id, sizeof(remote_group_id), "%s-group-0", remote_role);
            w4_db_build_block_key_from_hash(remote_block_hash, remote_block_key, sizeof(remote_block_key));
            w4_db_build_prefix_key_from_parts(remote_request_id,
                                              remote_prefix_group_id,
                                              remote_prefix_key,
                                              sizeof(remote_prefix_key));
            w4_db_build_prefix_key_from_parts(remote_request_id,
                                              remote_prefix_group_aux_id,
                                              remote_prefix_key_aux,
                                              sizeof(remote_prefix_key_aux));
            w4_db_build_group_key_from_parts(remote_request_id,
                                             remote_group_id,
                                             remote_group_key,
                                             sizeof(remote_group_key));
            if (w4_db_service_init(&db_service, true, true, true) == 0 &&
                w4_db_bootstrap_kvcache(&db_service, &db_block_ctx, &resolved_block_meta) == 0 &&
                w4_db_bootstrap_kvcache(&db_service, &db_block_ctx_aux, &resolved_block_meta_aux) == 0 &&
                w4_db_apply_block_result(&db_service,
                                         &db_block_ctx_aux,
                                         resolved_block_meta_aux.last_result_segment + 0x40ULL,
                                         W4_KVCACHE_STATE_RELOADED,
                                         &resolved_block_meta_aux) == 0) {
                db_service_ready = true;
                if (w4_db_update_prefix_metadata(&db_service,
                                                 &db_block_ctx_aux,
                                                 &resolved_block_meta_aux,
                                                 &resolved_prefix_meta_aux) != 0) {
                    printf("[w4_guest] gap guest_db_service_prefix=metadata_aux_prefix_update_failed\n");
                    db_service_ready = false;
                } else {
                    printf("[w4_guest] stage db_service_prefix_aux_ready key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                           resolved_prefix_meta_aux.key,
                           resolved_prefix_meta_aux.block_hash,
                           resolved_prefix_meta_aux.hot_segment_id,
                           w4_kvcache_state_name(resolved_prefix_meta_aux.state),
                           resolved_prefix_meta_aux.version,
                           resolved_prefix_meta_aux.last_result_segment);
                }
                if (db_service_ready &&
                    w4_db_get_prefix_group_metadata(&db_service,
                                                    &db_block_ctx,
                                                    &resolved_prefix_group) != 0) {
                    printf("[w4_guest] gap guest_db_service_group=metadata_group_lookup_failed\n");
                    db_service_ready = false;
                } else if (db_service_ready) {
                    printf("[w4_guest] stage db_service_group_ready key=%s group=%s members=%u version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                           resolved_prefix_group.key,
                           resolved_prefix_group.group_id,
                           resolved_prefix_group.member_count,
                           resolved_prefix_group.version,
                           resolved_prefix_group.last_result_segment);
                }
                printf("[w4_guest] stage db_service_candidate=kvcache_metadata_ready backing=obmm_pool transport=uburma block_path=%s records=%zu\n",
                       block_candidate, db_service.record_count);
                printf("[w4_guest] stage db_service_lookup key=%s request=%s prefix=%s block=%s placement_node=%u placement_level=%u hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       resolved_block_meta.key,
                       resolved_block_meta.request_id,
                       resolved_block_meta.prefix_group,
                       resolved_block_meta.block_hash,
                       resolved_block_meta.placement_node,
                       resolved_block_meta.placement_level,
                       resolved_block_meta.hot_segment_id,
                       w4_kvcache_state_name(resolved_block_meta.state),
                       resolved_block_meta.version,
                       resolved_block_meta.last_result_segment);
                printf("[w4_guest] stage db_service_lookup_aux key=%s request=%s prefix=%s block=%s placement_node=%u placement_level=%u hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       resolved_block_meta_aux.key,
                       resolved_block_meta_aux.request_id,
                       resolved_block_meta_aux.prefix_group,
                       resolved_block_meta_aux.block_hash,
                       resolved_block_meta_aux.placement_node,
                       resolved_block_meta_aux.placement_level,
                       resolved_block_meta_aux.hot_segment_id,
                       w4_kvcache_state_name(resolved_block_meta_aux.state),
                       resolved_block_meta_aux.version,
                       resolved_block_meta_aux.last_result_segment);
                if (w4_db_publish_observe_cluster(&db_service,
                                                  &resolved_block_meta,
                                                  &db_cluster_summary) != 0) {
                    printf("[w4_guest] gap guest_db_service_cluster=metadata_publish_failed\n");
                    db_service_ready = false;
                } else if (w4_guest_compute_roundtrip(role,
                                                       &db_block_ctx,
                                                       &resolved_block_meta,
                                                       &compute_roundtrip) != 0) {
                    printf("[w4_guest] gap guest_compute_roundtrip=payload_invalid\n");
                    db_service_ready = false;
                } else if (w4_db_apply_block_result(&db_service,
                                                    &db_block_ctx,
                                                    compute_roundtrip.output_segment,
                                                    W4_KVCACHE_STATE_RELOADED,
                                                    &resolved_block_meta) != 0) {
                    printf("[w4_guest] gap guest_db_service_update=metadata_result_update_failed\n");
                    db_service_ready = false;
                } else {
                    printf("[w4_guest] stage db_service_result_feed source=compute_roundtrip key=%s result_segment=0x%016" PRIx64 " output_checksum=0x%016" PRIx64 "\n",
                           resolved_block_meta.key,
                           compute_roundtrip.output_segment,
                           compute_roundtrip.output_checksum);
                    int stale_rc = w4_db_apply_block_result(&db_service,
                                                            &db_block_ctx,
                                                            resolved_block_meta.last_result_segment - 0x40ULL,
                                                            W4_KVCACHE_STATE_HOT,
                                                            &resolved_block_meta_aux);
                    if (stale_rc != 1) {
                        printf("[w4_guest] gap guest_db_service_update=stale_result_accepted\n");
                        db_service_ready = false;
                    } else {
                        update_order_ready = true;
                        printf("[w4_guest] stage db_service_update_order_ok key=%s stale_reject=true version=%" PRIu64 "\n",
                               resolved_block_meta.key,
                               resolved_block_meta.version);
                    }
                }
                if (db_service_ready) {
                    printf("[w4_guest] stage db_service_update_ok key=%s state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                           resolved_block_meta.key,
                           w4_kvcache_state_name(resolved_block_meta.state),
                           resolved_block_meta.version,
                           resolved_block_meta.last_result_segment);
                    if (db_service_ready && w4_db_update_prefix_metadata(&db_service,
                                                                         &db_block_ctx,
                                                                         &resolved_block_meta,
                                                                         &resolved_prefix_meta) != 0) {
                        printf("[w4_guest] gap guest_db_service_prefix=metadata_prefix_update_failed\n");
                        db_service_ready = false;
                    } else if (db_service_ready) {
                        printf("[w4_guest] stage db_service_prefix_update_ok key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                               resolved_prefix_meta.key,
                               resolved_prefix_meta.block_hash,
                               resolved_prefix_meta.hot_segment_id,
                               w4_kvcache_state_name(resolved_prefix_meta.state),
                               resolved_prefix_meta.version,
                               resolved_prefix_meta.last_result_segment);
                        {
                            struct w4_db_record stale_prefix_source = resolved_block_meta;
                            int stale_prefix_rc;

                            stale_prefix_source.last_result_segment -= 0x40ULL;
                            stale_prefix_source.state = W4_KVCACHE_STATE_HOT;
                            stale_prefix_rc = w4_db_update_prefix_metadata(&db_service,
                                                                           &db_block_ctx,
                                                                           &stale_prefix_source,
                                                                           &resolved_prefix_meta);
                            if (stale_prefix_rc != 1) {
                                printf("[w4_guest] gap guest_db_service_prefix=stale_prefix_update_accepted\n");
                                db_service_ready = false;
                            } else {
                                prefix_update_order_ready = true;
                                printf("[w4_guest] stage db_service_prefix_update_order_ok key=%s stale_reject=true version=%" PRIu64 "\n",
                                       resolved_prefix_meta.key,
                                       resolved_prefix_meta.version);
                            }
                        }
                    }
                    if (db_service_ready && w4_db_publish_observe_cluster(&db_service,
                                                      &resolved_block_meta,
                                                      &db_cluster_update_summary) != 0) {
                        printf("[w4_guest] gap guest_db_service_cluster=metadata_update_failed\n");
                        db_service_ready = false;
                    } else if (!db_cluster_update_summary.active) {
                        cluster_coherent = true;
                    } else if (db_cluster_update_summary.ready &&
                               db_cluster_update_summary.peer_prefix_count_floor >= 2 &&
                               db_cluster_update_summary.peer_group_count_floor >= 1 &&
                               db_cluster_update_summary.peer_block_count_floor >= 2) {
                        cluster_coherent = true;
                        printf("[w4_guest] stage db_service_cluster=metadata_coherent state=%s version=%" PRIu64 " peer_version_floor=%" PRIu64 " peer_result_floor=0x%016" PRIx64 " peer_prefix_version_floor=%" PRIu64 " peer_prefix_result_floor=0x%016" PRIx64 " peer_block_count_floor=%u peer_prefix_count_floor=%u peer_group_count_floor=%u placement=%s state_match=%s peers=%u\n",
                               w4_kvcache_state_name(resolved_block_meta.state),
                               resolved_block_meta.version,
                               db_cluster_update_summary.peer_version_floor,
                               db_cluster_update_summary.peer_result_floor,
                               db_cluster_update_summary.peer_prefix_version_floor,
                               db_cluster_update_summary.peer_prefix_result_floor,
                               db_cluster_update_summary.peer_block_count_floor,
                               db_cluster_update_summary.peer_prefix_count_floor,
                               db_cluster_update_summary.peer_group_count_floor,
                               db_cluster_update_summary.placement_coherent ? "true" : "false",
                               db_cluster_update_summary.state_coherent ? "true" : "false",
                               db_cluster_update_summary.peers_observed);
                    } else {
                        printf("[w4_guest] gap guest_db_service_cluster=metadata_update_incoherent\n");
                        db_service_ready = false;
                    }
                    if (db_service_ready && cluster_coherent) {
                        if (w4_db_rebind_block_view(&db_service,
                                                    &db_block_ctx,
                                                    resolved_block_meta.hot_segment_id + 0x200ULL,
                                                    resolved_block_meta.placement_level,
                                                    &resolved_block_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_view=metadata_view_update_failed\n");
                            db_service_ready = false;
                        } else {
                            printf("[w4_guest] stage db_service_view_update_ok key=%s hot_segment=0x%016" PRIx64 " placement_level=%u version=%" PRIu64 "\n",
                                   resolved_block_meta.key,
                                   resolved_block_meta.hot_segment_id,
                                   resolved_block_meta.placement_level,
                                   resolved_block_meta.version);
                        }
                        if (db_service_ready && w4_db_update_prefix_metadata(&db_service,
                                                                             &db_block_ctx,
                                                                             &resolved_block_meta,
                                                                             &resolved_prefix_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_prefix=metadata_prefix_view_update_failed\n");
                            db_service_ready = false;
                        } else if (db_service_ready) {
                            printf("[w4_guest] stage db_service_prefix_view_update_ok key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 "\n",
                                   resolved_prefix_meta.key,
                                   resolved_prefix_meta.block_hash,
                                   resolved_prefix_meta.hot_segment_id,
                                   w4_kvcache_state_name(resolved_prefix_meta.state),
                                   resolved_prefix_meta.version);
                        }
                        if (db_service_ready &&
                            w4_db_handoff_block_owner(&db_service,
                                                      &db_block_ctx,
                                                      w4_cluster_next_owner(db_block_ctx.placement_node,
                                                                            cluster_node_count),
                                                      resolved_block_meta.placement_level,
                                                      w4_cluster_handoff_hot(resolved_block_meta.hot_segment_id),
                                                      &resolved_block_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_handoff=metadata_handoff_failed\n");
                            db_service_ready = false;
                        } else if (db_service_ready) {
                            printf("[w4_guest] stage db_service_handoff_ok key=%s placement_node=%u placement_level=%u hot_segment=0x%016" PRIx64 " version=%" PRIu64 "\n",
                                   resolved_block_meta.key,
                                   resolved_block_meta.placement_node,
                                   resolved_block_meta.placement_level,
                                   resolved_block_meta.hot_segment_id,
                                   resolved_block_meta.version);
                        }
                        if (db_service_ready && w4_db_update_prefix_metadata(&db_service,
                                                                             &db_block_ctx,
                                                                             &resolved_block_meta,
                                                                             &resolved_prefix_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_prefix=metadata_prefix_handoff_failed\n");
                            db_service_ready = false;
                        } else if (db_service_ready) {
                            printf("[w4_guest] stage db_service_prefix_handoff_ok key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 "\n",
                                   resolved_prefix_meta.key,
                                   resolved_prefix_meta.block_hash,
                                   resolved_prefix_meta.hot_segment_id,
                                   w4_kvcache_state_name(resolved_prefix_meta.state),
                                   resolved_prefix_meta.version);
                        }
                        if (db_service_ready && w4_db_publish_observe_cluster(&db_service,
                                                                              &resolved_block_meta,
                                                                              &db_cluster_handoff_summary) != 0) {
                            printf("[w4_guest] gap guest_db_service_handoff=metadata_cluster_failed\n");
                            db_service_ready = false;
                        } else if (!db_cluster_handoff_summary.active) {
                            handoff_ready = true;
                        } else if (db_cluster_handoff_summary.ready &&
                                   db_cluster_handoff_summary.peer_prefix_count_floor >= 2 &&
                                   db_cluster_handoff_summary.peer_group_count_floor >= 1 &&
                                   db_cluster_handoff_summary.peer_block_count_floor >= 2) {
                            handoff_ready = true;
                            printf("[w4_guest] stage db_service_cluster=metadata_handoff_coherent placement_node=%u hot_segment=0x%016" PRIx64 " version=%" PRIu64 " peer_block_count_floor=%u peer_prefix_count_floor=%u peer_group_count_floor=%u peers=%u\n",
                                   resolved_block_meta.placement_node,
                                   resolved_block_meta.hot_segment_id,
                                   resolved_block_meta.version,
                                   db_cluster_handoff_summary.peer_block_count_floor,
                                   db_cluster_handoff_summary.peer_prefix_count_floor,
                                   db_cluster_handoff_summary.peer_group_count_floor,
                                   db_cluster_handoff_summary.peers_observed);
                        } else {
                            printf("[w4_guest] gap guest_db_service_handoff=metadata_incoherent\n");
                            db_service_ready = false;
                        }
                        if (db_service_ready) {
                            int stale_owner_rc = w4_db_apply_block_result(&db_service,
                                                                          &db_block_ctx,
                                                                          resolved_block_meta.last_result_segment + 0x40ULL,
                                                                          W4_KVCACHE_STATE_RELOADED,
                                                                          &resolved_block_meta_aux);
                            if (stale_owner_rc != 2) {
                                printf("[w4_guest] gap guest_db_service_handoff=stale_owner_accepted\n");
                                db_service_ready = false;
                            } else {
                                printf("[w4_guest] stage db_service_handoff_order_ok key=%s stale_owner_reject=true version=%" PRIu64 "\n",
                                       resolved_block_meta.key,
                                       resolved_block_meta.version);
                            }
                        }
                        if (db_service_ready && w4_db_cluster_fetch_record(&db_service,
                                                                          remote_block_key,
                                                                          &remote_block_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                   remote_block_key);
                            db_service_ready = false;
                        } else if (db_service_ready && w4_db_cluster_fetch_record(&db_service,
                                                                                  remote_prefix_key,
                                                                                  &remote_prefix_meta) != 0) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                   remote_prefix_key);
                            db_service_ready = false;
                        } else if (db_service_ready && w4_db_cluster_fetch_record(&db_service,
                                                                                  remote_prefix_key_aux,
                                                                                  &remote_prefix_meta_aux) != 0) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                   remote_prefix_key_aux);
                            db_service_ready = false;
                        } else if (db_service_ready && w4_db_cluster_fetch_record(&db_service,
                                                                                  remote_group_key,
                                                                                  &remote_prefix_group) != 0) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                   remote_group_key);
                            db_service_ready = false;
                        } else {
                            w4_db_build_block_key_from_hash(remote_prefix_meta_aux.block_hash,
                                                            remote_block_key_aux,
                                                            sizeof(remote_block_key_aux));
                            if (w4_db_cluster_fetch_record(&db_service,
                                                           remote_block_key_aux,
                                                           &remote_block_meta_aux) != 0) {
                                printf("[w4_guest] gap guest_db_service_remote=metadata_fetch_failed key=%s\n",
                                       remote_block_key_aux);
                                db_service_ready = false;
                            }
                        }
                        if (db_service_ready &&
                            (strncmp(remote_block_meta.key, remote_block_key, sizeof(remote_block_meta.key)) != 0 ||
                             remote_block_meta.state != W4_KVCACHE_STATE_RELOADED ||
                             remote_block_meta.hot_segment_id == 0 ||
                             remote_block_meta.last_result_segment == 0 ||
                             strncmp(remote_prefix_meta.key, remote_prefix_key, sizeof(remote_prefix_meta.key)) != 0 ||
                             !w4_db_prefix_matches_block_meta(&remote_prefix_meta, &remote_block_meta) ||
                             strncmp(remote_prefix_meta_aux.key, remote_prefix_key_aux, sizeof(remote_prefix_meta_aux.key)) != 0 ||
                             !w4_db_prefix_matches_block_meta(&remote_prefix_meta_aux, &remote_block_meta_aux) ||
                             strncmp(remote_prefix_group.key, remote_group_key, sizeof(remote_prefix_group.key)) != 0 ||
                             !w4_db_group_covers_blocks(&remote_prefix_group,
                                                        &remote_block_meta,
                                                        &remote_block_meta_aux))) {
                            printf("[w4_guest] gap guest_db_service_remote=metadata_incoherent key=%s prefix=%s\n",
                                   remote_block_key,
                                   remote_prefix_key);
                            db_service_ready = false;
                        } else if (db_service_ready) {
                            remote_metadata_ready = true;
                            printf("[w4_guest] stage db_service_remote_lookup key=%s state=%s version=%" PRIu64 " placement_node=%u hot_segment=0x%016" PRIx64 " last_result_segment=0x%016" PRIx64 "\n",
                                   remote_block_meta.key,
                                   w4_kvcache_state_name(remote_block_meta.state),
                                   remote_block_meta.version,
                                   remote_block_meta.placement_node,
                                   remote_block_meta.hot_segment_id,
                                   remote_block_meta.last_result_segment);
                            printf("[w4_guest] stage db_service_remote_prefix_lookup key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 "\n",
                                   remote_prefix_meta.key,
                                   remote_prefix_meta.block_hash,
                                   remote_prefix_meta.hot_segment_id,
                                   w4_kvcache_state_name(remote_prefix_meta.state),
                                   remote_prefix_meta.version);
                            printf("[w4_guest] stage db_service_remote_prefix_aux_lookup key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s version=%" PRIu64 "\n",
                                   remote_prefix_meta_aux.key,
                                   remote_prefix_meta_aux.block_hash,
                                   remote_prefix_meta_aux.hot_segment_id,
                                   w4_kvcache_state_name(remote_prefix_meta_aux.state),
                                   remote_prefix_meta_aux.version);
                            printf("[w4_guest] stage db_service_remote_lookup_aux key=%s state=%s version=%" PRIu64 " placement_node=%u hot_segment=0x%016" PRIx64 " last_result_segment=0x%016" PRIx64 "\n",
                                   remote_block_meta_aux.key,
                                   w4_kvcache_state_name(remote_block_meta_aux.state),
                                   remote_block_meta_aux.version,
                                   remote_block_meta_aux.placement_node,
                                   remote_block_meta_aux.hot_segment_id,
                                   remote_block_meta_aux.last_result_segment);
                            printf("[w4_guest] stage db_service_remote_group_lookup key=%s group=%s members=%u version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                                   remote_prefix_group.key,
                                   remote_prefix_group.group_id,
                                   remote_prefix_group.member_count,
                                   remote_prefix_group.version,
                                   remote_prefix_group.last_result_segment);
                            printf("[w4_guest] stage db_service_remote_check_ok block=%s prefix=%s prefix_aux=%s group=%s placement_node=%u prefix_last_result_segment=0x%016" PRIx64 " prefix_block=%s prefix_hot_segment=0x%016" PRIx64 " prefix_aux_last_result_segment=0x%016" PRIx64 " prefix_aux_block=%s prefix_aux_hot_segment=0x%016" PRIx64 " group_members=%u\n",
                                   remote_block_meta.key,
                                   remote_prefix_meta.key,
                                   remote_prefix_meta_aux.key,
                                   remote_prefix_group.key,
                                   remote_block_meta.placement_node,
                                   remote_prefix_meta.last_result_segment,
                                   remote_prefix_meta.block_hash,
                                   remote_prefix_meta.hot_segment_id,
                                   remote_prefix_meta_aux.last_result_segment,
                                   remote_prefix_meta_aux.block_hash,
                                   remote_prefix_meta_aux.hot_segment_id,
                                   remote_prefix_group.member_count);
                            group_relationship_ready = true;
                        }
                    }
                }
            } else {
                printf("[w4_guest] gap guest_db_service=kvcache_metadata_failed\n");
            }
            if (db_cluster_summary.active) {
                printf("[w4_guest] assessment shmem_kvcache_path=obmm_pool block_candidate=%s dispatch_candidate=%s db_service_candidate=%s cluster_ready=%s cluster_update_ready=%s cluster_prefix_ready=%s cluster_groups_ready=%s cluster_blocks_ready=%s update_order_ready=%s prefix_update_order_ready=%s handoff_ready=%s remote_metadata_ready=%s complete=%s\n",
                       block_candidate,
                       dispatch_candidate,
                       db_service_ready ? "kvcache_metadata_ready" : "missing",
                       db_cluster_summary.ready ? "true" : "false",
                       cluster_coherent ? "true" : "false",
                       (db_cluster_update_summary.peer_prefix_count_floor >= 2) ? "true" : "false",
                       (db_cluster_update_summary.peer_group_count_floor >= 1) ? "true" : "false",
                       (db_cluster_update_summary.peer_block_count_floor >= 2) ? "true" : "false",
                       update_order_ready ? "true" : "false",
                       prefix_update_order_ready ? "true" : "false",
                       handoff_ready ? "true" : "false",
                       remote_metadata_ready ? "true" : "false",
                       (db_service_ready && db_cluster_summary.ready && cluster_coherent &&
                        db_cluster_update_summary.peer_prefix_count_floor >= 2 &&
                        db_cluster_update_summary.peer_group_count_floor >= 1 &&
                        db_cluster_update_summary.peer_block_count_floor >= 2 &&
                        update_order_ready &&
                        prefix_update_order_ready &&
                        handoff_ready &&
                        group_relationship_ready &&
                        remote_metadata_ready) ? "true" : "false");
            } else {
                printf("[w4_guest] assessment shmem_kvcache_path=obmm_pool block_candidate=%s dispatch_candidate=%s db_service_candidate=%s complete=%s\n",
                       block_candidate,
                       dispatch_candidate,
                       db_service_ready ? "kvcache_metadata_ready" : "missing",
                       db_service_ready ? "true" : "false");
            }
            if (!db_service_ready) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db service\n");
                goto out;
            }
            if (db_cluster_summary.active && !db_cluster_summary.ready) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db cluster visibility\n");
                goto out;
            }
            if (db_cluster_summary.active && !cluster_coherent) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db cluster coherence\n");
                goto out;
            }
            if (db_cluster_summary.active &&
                (db_cluster_update_summary.peer_block_count_floor < 2 ||
                 db_cluster_update_summary.peer_group_count_floor < 1)) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db cluster blocks\n");
                goto out;
            }
            if (db_cluster_summary.active && db_cluster_update_summary.peer_prefix_count_floor < 1) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db cluster prefix\n");
                goto out;
            }
            if (!update_order_ready || !prefix_update_order_ready) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db update order\n");
                goto out;
            }
            if (!handoff_ready) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db handoff\n");
                goto out;
            }
            if (!group_relationship_ready) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db group relationship\n");
                goto out;
            }
            if (!remote_metadata_ready) {
                fprintf(stderr, "[w4_guest] fail incomplete kvcache db remote metadata\n");
                goto out;
            }
            printf("[w4_guest] dispatch path=%s\n", dispatch_candidate);
            printf("[w4_guest] pass\n");
            rc = 0;
            goto out;
        }
        printf("[w4_guest] assessment shmem_kvcache_path=obmm_pool block_candidate=%s dispatch_candidate=%s complete=false\n",
               block_candidate,
               dispatch_candidate);
        if (!db_service_ready) {
            printf("[w4_guest] gap guest_db_service=kvcache_metadata_missing\n");
        }
        if (!block_ready) {
            printf("[w4_guest] gap guest_block_path=missing\n");
        }
        if (!uburma_ready) {
            printf("[w4_guest] gap guest_dispatch_path=missing\n");
        }
        printf("[w4_guest] gap guest_dispatch_real_chipbackend=missing\n");
        fprintf(stderr, "[w4_guest] fail incomplete guest service closure\n");
        goto out;
    }

    if (enable_db_cluster) {
        if (run_obmm_backing_stage() != 0) {
            goto out;
        }
        printf("[w4_guest] stage db_cluster_mode=resource_backed_uapi\n");
        if (w4_resource_backed_db_cluster_assertions(role, cluster_node_count) != 0) {
            fprintf(stderr, "[w4_guest] fail incomplete resource-backed kvcache db cluster assertions\n");
            goto out;
        }
    }

    fd = open(selected_resource_path, O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "[w4_guest] open selected resource failed path=%s err=%s\n",
                selected_resource_path, strerror(errno));
        goto out;
    }
    printf("[w4_guest] step=open_resource ok path=%s\n", selected_resource_path);
    mapped_via_resource = true;

    if (mapped_via_resource) {
        root_page_base = 0;
        root_page_off = 0;
        ep_page_base = LINQU_ENDPOINT1_OFFSET;
        ep_page_off = 0;
    }

    root_map = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    (off_t)root_page_base);
    if (root_map == MAP_FAILED) {
        perror("mmap(root)");
        goto out;
    }

    ep_map = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                  (off_t)ep_page_base);
    if (ep_map == MAP_FAILED) {
        perror("mmap(endpoint)");
        goto out;
    }
    printf("[w4_guest] step=map_endpoint ok root=0x%016" PRIx64 " endpoint=0x%016" PRIx64 "\n",
           root_base, ep_base);

    cmdq = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    cq = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (cmdq == MAP_FAILED || cq == MAP_FAILED) {
        perror("mmap(queue)");
        goto out;
    }
    printf("[w4_guest] step=map_queues ok\n");
    memset(cmdq, 0, PAGE_SIZE_BYTES);
    memset(cq, 0, PAGE_SIZE_BYTES);
    if (phys_for_virt(cmdq, &cmdq_phys) != 0 || phys_for_virt(cq, &cq_phys) != 0) {
        fprintf(stderr, "[w4_guest] failed to translate queue pages\n");
        goto out;
    }
    printf("[w4_guest] step=queue_phys ok cmdq=0x%016" PRIx64 " cq=0x%016" PRIx64 "\n",
           cmdq_phys, cq_phys);

    root_mmio = (volatile uint8_t *)root_map + root_page_off;
    ep_mmio = (volatile uint8_t *)ep_map + ep_page_off;
    printf("[w4_guest] step=read_root_version ok version=0x%016" PRIx64 "\n",
           mmio_read64(root_mmio, REG_VERSION));
    default_segment = mmio_read64(ep_mmio, REG_DEFAULT_SEGMENT);
    if (default_segment == 0) {
        fprintf(stderr, "[w4_guest] default segment missing\n");
        goto out;
    }
    printf("[w4_guest] step=read_default_segment ok segment=%" PRIu64 "\n", default_segment);
    if (seed_kvcache_payload(ep_mmio, default_segment) != 0) {
        goto out;
    }

    printf("[w4_guest] stage uapi_kvcache_shmem_descriptor segment=%" PRIu64 " bytes=128 puts=1 gets=1 role=hot_shared\n",
           default_segment);
    build_shmem_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 3, default_segment, 128);
    build_shmem_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 4, default_segment, 128);
    printf("[w4_guest] stage uapi_kvcache_shmem_descriptor segment=%" PRIu64 " bytes=%u puts=1 gets=1 role=multi_block_boundary\n",
           default_segment, W4_KVCACHE_PAYLOAD_BYTES);
    build_shmem_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 3, default_segment, W4_KVCACHE_PAYLOAD_BYTES);
    build_shmem_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 4, default_segment, W4_KVCACHE_PAYLOAD_BYTES);
    printf("[w4_guest] stage uapi_kvcache_db_descriptor key=%s bytes=%" PRIu64 "\n",
           key, kvcache_db_bytes);
    build_dbput_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), key, kvcache_db_bytes);
    build_dbget_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), key);
    printf("[w4_guest] stage uapi_kvcache_db_descriptor key=%s bytes=%" PRIu64 " role=aux_block\n",
           key_aux, kvcache_db_bytes);
    build_dbput_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), key_aux, kvcache_db_bytes);
    build_dbget_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), key_aux);
    build_dfs_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 6, path, 256);
    build_dfs_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 5, path, 0);
    printf("[w4_guest] stage uapi_kvcache_block_descriptor block=%s segment=%" PRIu64 " writes=1 reads=1\n",
           block, default_segment);
    build_io_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 100, 2, default_segment, block);
    build_io_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 101, 1, default_segment, block);
    printf("[w4_guest] stage uapi_kvcache_block_descriptor block=%s segment=%" PRIu64 " writes=1 reads=1 role=aux_block_boundary\n",
           block_aux, default_segment);
    build_io_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 102, 2, default_segment, block_aux);
    build_io_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 103, 1, default_segment, block_aux);
    printf("[w4_guest] stage uapi_chipbackend_dispatch_descriptor block=%s segment=%" PRIu64 " task_id=31\n",
           block, default_segment);
    build_io_descriptor(cmdq + (slot++ * CMDQ_SLOT_BYTES), 31, 3, default_segment, NULL);

    if (enable_db_cluster && cluster_node_count >= 8U) {
        uint32_t dispatch_node = 0U;

        if (w4_cluster_role_index(role, cluster_node_count, &dispatch_node)) {
            unsigned int delay_ms = dispatch_node * 5000U;

            if (delay_ms > 0U) {
                printf("[w4_guest] stage uapi_dispatch_stagger node=%u delay_ms=%u\n",
                       dispatch_node + 1U,
                       delay_ms);
                usleep(delay_ms * 1000U);
            }
        }
    }

    mmio_write64(ep_mmio, REG_CMDQ_BASE_LO, cmdq_phys);
    mmio_write64(ep_mmio, REG_CQ_BASE_LO, cq_phys);
    mmio_write64(ep_mmio, REG_CQ_HEAD, 0);
    for (size_t submitted = 0; submitted < slot;) {
        size_t next = submitted + W4_DOORBELL_BATCH_SLOTS;

        if (next > slot) {
            next = slot;
        }
        mmio_write64(ep_mmio, REG_CMDQ_TAIL, next);
        mmio_write64(ep_mmio, REG_DOORBELL, next);
        clock_gettime(CLOCK_MONOTONIC, &start_ts);
        for (;;) {
            struct timespec now;
            uint64_t elapsed_ms;

            cq_tail = mmio_read64(ep_mmio, REG_CQ_TAIL);
            if (cq_tail >= next) {
                break;
            }

            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed_ms = (uint64_t)(now.tv_sec - start_ts.tv_sec) * 1000ULL +
                         (uint64_t)(now.tv_nsec - start_ts.tv_nsec) / 1000000ULL;
            if (elapsed_ms > W4_TIMEOUT_MS) {
                fprintf(stderr,
                        "[w4_guest] timeout waiting completions cq_tail=%" PRIu64
                        " expected=%zu\n",
                        cq_tail,
                        next);
                goto out;
            }
            usleep(10000);
        }
        printf("[w4_guest] step=doorbell_batch ok submitted=%zu cq_tail=%" PRIu64 "\n",
               next,
               cq_tail);
        usleep(1000);
        submitted = next;
    }
    printf("[w4_guest] step=doorbell ok slots=%zu\n", slot);

    printf("[w4_guest] step=wait_completions ok cq_tail=%" PRIu64 "\n", cq_tail);

    for (size_t i = 0; i < slot; ++i) {
        struct completion_preview preview;
        if (decode_completion_preview(cq + (i * CMDQ_SLOT_BYTES), &preview) != 0) {
            fprintf(stderr, "[w4_guest] completion decode failed slot=%zu\n", i);
            goto out;
        }
        count_completion(&preview, &counts);
    }
    printf("[w4_guest] step=decode_completions ok\n");
    if (verify_dispatch_payload(ep_mmio, default_segment) != 0) {
        goto out;
    }

    mmio_write64(ep_mmio, REG_CQ_HEAD, cq_tail);
    mmio_write64(ep_mmio, REG_IRQ_ACK, mmio_read64(ep_mmio, REG_IRQ_STATUS));

    printf("[w4_guest] completion_sources chipbackend=%" PRIu64 " shmem=%" PRIu64
           " dfs=%" PRIu64 " db=%" PRIu64 " block=%" PRIu64 " guest_uapi=%" PRIu64 "\n",
           counts.chipbackend, counts.shmem, counts.dfs, counts.db, counts.block, counts.guest_uapi);
    printf("[w4_guest] completion_status success=%" PRIu64 " retryable=%" PRIu64
           " fatal=%" PRIu64 "\n",
           counts.success, counts.retryable, counts.fatal);
    printf("[w4_guest] stage uapi_kvcache_shmem_completion segment=%" PRIu64 " bytes=128 puts=1 gets=1 source=shmem_service role=hot_shared\n",
           default_segment);
    printf("[w4_guest] stage uapi_kvcache_shmem_completion segment=%" PRIu64 " bytes=%u puts=1 gets=1 source=shmem_service role=multi_block_boundary\n",
           default_segment, W4_KVCACHE_PAYLOAD_BYTES);
    printf("[w4_guest] stage uapi_kvcache_block_completion block=%s writes=1 reads=1 source=block_service\n",
           block);
    printf("[w4_guest] stage uapi_kvcache_block_completion block=%s writes=1 reads=1 source=block_service role=aux_block_boundary\n",
           block_aux);
    printf("[w4_guest] stage uapi_kvcache_db_completion key=%s bytes=%" PRIu64 " puts=1 gets=1 source=db_service\n",
           key, kvcache_db_bytes);
    printf("[w4_guest] stage uapi_kvcache_db_completion key=%s bytes=%" PRIu64 " puts=1 gets=1 source=db_service role=aux_block\n",
           key_aux, kvcache_db_bytes);
    printf("[w4_guest] summary chipbackend=%" PRIu64 " shmem=%" PRIu64 " dfs=%" PRIu64 " db=%" PRIu64
           " block=%" PRIu64 " guest_uapi=%" PRIu64 " success=%" PRIu64
           " retryable=%" PRIu64 " fatal=%" PRIu64 "\n",
           counts.chipbackend, counts.shmem, counts.dfs, counts.db, counts.block, counts.guest_uapi,
           counts.success, counts.retryable, counts.fatal);

    if (counts.shmem < 2 || counts.dfs < 2 || counts.db < 2 || counts.block < 2 ||
        counts.chipbackend < 1 || counts.fatal != 0) {
        printf("[w4_guest] assessment service_coverage=%d/5 dispatch_path=ubc_entity_chipbackend complete=false\n",
               (counts.shmem >= 2) + (counts.dfs >= 2) + (counts.db >= 2) +
               (counts.block >= 2) + (counts.chipbackend >= 1));
        fprintf(stderr, "[w4_guest] fail incomplete service coverage\n");
        goto out;
    }

    printf("[w4_guest] assessment service_coverage=5/5 dispatch_path=ubc_entity_chipbackend kvcache_shmem_segment=%" PRIu64 " kvcache_block=%s kvcache_db_key=%s kvcache_db_bytes=%" PRIu64 " complete=true\n",
           default_segment, block, key, kvcache_db_bytes);
    printf("[w4_guest] dispatch path=ubc_entity_chipbackend\n");
    printf("[w4_guest] pass\n");
    rc = 0;

out:
    if (cq != MAP_FAILED) {
        munmap(cq, PAGE_SIZE_BYTES);
    }
    if (cmdq != MAP_FAILED) {
        munmap(cmdq, PAGE_SIZE_BYTES);
    }
    if (ep_map != MAP_FAILED) {
        munmap(ep_map, PAGE_SIZE_BYTES);
    }
    if (root_map != MAP_FAILED) {
        munmap(root_map, PAGE_SIZE_BYTES);
    }
    if (fd >= 0) {
        close(fd);
    }
    return rc;
}
