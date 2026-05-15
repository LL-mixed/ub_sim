#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
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
#define W4_UAPI_CMDQ_DEPTH 32U
#define W4_UAPI_CQ_DEPTH 64U
#define MAX_SLOTS 16U
#define W4_DEFAULT_TIMEOUT_MS 300000
#define W4_DOORBELL_BATCH_SLOTS 4U
#define W4_DEMO_KVCACHE_PAYLOAD_BYTES 8192U
#define W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES 8192U
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
#define W4_QWEN3_MARKER_RANGE_COMPUTE_CONTRACT 0x7133773472676330ULL
#define W4_QWEN3_MARKER_RANGE_FORWARD_TABLE 0x7133773472667430ULL
#define W4_QWEN3_RANGE_TASK_MAGIC 0x5133060bU
#define W4_QWEN3_COMPLETION_TASK_OFFSET 19U
#define W4_QWEN3_HIDDEN_RANGE_BYTES 262144ULL
#define W4_QWEN3_MAX_HIDDEN_RANGE_BYTES (2ULL * 1024ULL * 1024ULL)
#define W4_QWEN3_MAX_KV_PAYLOAD_BYTES (4ULL * 1024ULL * 1024ULL)
#define W4_QWEN3_RANGE_INPUT_PAYLOAD_OFFSET 0x0000000000080000ULL
#define W4_QWEN3_PREVIOUS_KV_PAYLOAD_OFFSET 0x0000000000280000ULL
#define W4_QWEN3_PREVIOUS_KV_PAYLOAD_HEADER_BYTES 32ULL
#define W4_QWEN3_PREVIOUS_KV_PAYLOAD_MARKER 0x45564b5033515750ULL
#define W4_QWEN3_TOKENIZER_POLICY_KIND 1ULL
#define W4_QWEN3_TOKENIZER_ASSET_POLICY_KIND 2ULL
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
#define W4_QWEN3_RESULT_BLOCK_METADATA_END_OFFSET 32ULL
#define W4_QWEN3_RESULT_BLOCK_RANGE_FORWARD_HEADER_OFFSET 40ULL
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
#define W4_QWEN3_LOGITS_TABLE_ENTRY_WORDS 45ULL
#define W4_QWEN3_LOGITS_TABLE_ENTRY_BYTES 360ULL
#define W4_QWEN3_LOGITS_TABLE_COMPACT_ENTRY_WORDS 20ULL
#define W4_QWEN3_LOGITS_TABLE_COMPACT_ENTRY_BYTES 160ULL
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
#define W4_QWEN3_RANGE_FORWARD_TABLE_ENTRY_WORDS 18ULL
#define W4_QWEN3_RANGE_FORWARD_TABLE_ENTRY_BYTES \
    (W4_QWEN3_RANGE_FORWARD_TABLE_ENTRY_WORDS * 8ULL)
#define W4_QWEN3_OUTPUT_SCAN_FALLBACK_BYTES \
    (W4_QWEN3_EXPECTED_TILES * W4_QWEN3_SHARD_OUTPUT_BYTES)
#define W4_QWEN3_OUTPUT_SCAN_MAX_BYTES (8ULL * 1024ULL * 1024ULL)

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

struct w4_qwen3_range_runtime_forward {
    uint64_t node;
    uint64_t layer_start;
    uint64_t layer_end;
    uint64_t layer_count;
    uint64_t next_node;
    uint64_t input_checksum;
    uint64_t output_checksum;
    uint64_t payload_checksum;
    uint64_t kv_payload_checksum;
    uint64_t range_checksum;
    uint64_t real_layers;
    uint64_t payload_offset;
    uint64_t payload_bytes;
    uint64_t kv_payload_offset;
    uint64_t kv_payload_bytes;
    uint8_t output_payload[W4_QWEN3_MAX_HIDDEN_RANGE_BYTES];
    uint8_t kv_payload[W4_QWEN3_MAX_KV_PAYLOAD_BYTES];
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

static uint64_t env_u64_or(const char *name, uint64_t fallback)
{
    const char *value = getenv(name);
    char *end = NULL;
    unsigned long long parsed;

    if (!value || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        return fallback;
    }
    return (uint64_t)parsed;
}

static uint64_t qwen3_pipeline_nodes(void)
{
    return env_u64_or("SIM_QWEN3_DENSE_TP_NODES", W4_QWEN3_EXPECTED_SHARDS);
}

static uint64_t qwen3_total_layers(void)
{
    return env_u64_or("SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS",
                      W4_QWEN3_KVCACHE_LAYERS);
}

static uint64_t qwen3_vocab_size(void)
{
    return env_u64_or("SIM_QWEN3_DENSE_VOCAB_SIZE", W4_QWEN3_VOCAB_SIZE);
}

static const char *qwen3_model_id(void)
{
    const char *model_id = getenv("SIM_QWEN3_DENSE_MODEL_ID");

    return model_id && model_id[0] != '\0' ? model_id : W4_QWEN3_TOKENIZER_MODEL_ID;
}

static uint64_t qwen3_hidden_range_bytes(void)
{
    return env_u64_or("SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES",
                      W4_QWEN3_HIDDEN_RANGE_BYTES);
}

static uint64_t qwen3_decode_hidden_bytes(void)
{
    uint64_t hidden_size = env_u64_or("SIM_QWEN3_DENSE_HIDDEN_SIZE", 1024ULL);
    uint64_t decode_tokens = env_u64_or("SIM_QWEN3_DENSE_DECODE_TOKENS", 1ULL);

    return env_u64_or("SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES",
                      hidden_size * decode_tokens * 2ULL);
}

static uint64_t qwen3_handoff_hidden_bytes(uint64_t decode_step)
{
    return decode_step > 0 ? qwen3_decode_hidden_bytes() : qwen3_hidden_range_bytes();
}

static bool is_qwen3_profile_name(const char *profile)
{
    return profile &&
           (strcmp(profile, "qwen3_dense_reference") == 0 ||
            strcmp(profile, "qwen3_dense") == 0);
}

static bool qwen3_real_tokenizer_required(void)
{
    const char *profile = getenv("SIM_UAPI_W4_CHIPBACKEND_PROFILE");
    const char *weights_path = getenv("SIM_QWEN3_DENSE_WEIGHTS_PATH");

    return is_qwen3_profile_name(profile) && weights_path && weights_path[0] != '\0';
}

static void write_u32_le(uint8_t *buf, size_t *off, uint32_t value)
{
    memcpy(buf + *off, &value, sizeof(value));
    *off += sizeof(value);
}

static uint32_t read_u32_le_bytes(const uint8_t *buf, size_t off)
{
    uint32_t value;

    memcpy(&value, buf + off, sizeof(value));
    return value;
}

static uint64_t read_u64_le_bytes(const uint8_t *buf, size_t off)
{
    uint64_t value;

    memcpy(&value, buf + off, sizeof(value));
    return value;
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

static uint64_t monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static uint8_t *queue_slot_ptr(uint8_t *queue,
                               size_t queue_depth,
                               size_t base_slot,
                               size_t logical_slot)
{
    size_t slot = (base_slot + logical_slot) % queue_depth;

    return queue + (slot * CMDQ_SLOT_BYTES);
}

static size_t queue_distance(size_t queue_depth, size_t base_slot, size_t tail_slot)
{
    if (tail_slot >= base_slot) {
        return tail_slot - base_slot;
    }
    return queue_depth - base_slot + tail_slot;
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

static void build_qwen3_range_dispatch_descriptor(uint8_t *slot,
                                                  uint64_t op_id,
                                                  uint64_t segment,
                                                  uint32_t node,
                                                  uint32_t layer_start,
                                                  uint32_t layer_end,
                                                  uint32_t next_node,
                                                  uint64_t hidden_bytes)
{
    size_t off = 0;

    memset(slot, 0, CMDQ_SLOT_BYTES);
    write_u8_le(slot, &off, 9);
    write_u64_le(slot, &off, op_id);
    write_u64_le(slot, &off, segment);
    write_u32_le(slot, &off, W4_QWEN3_RANGE_TASK_MAGIC);
    write_u32_le(slot, &off, node);
    write_u32_le(slot, &off, layer_start);
    write_u32_le(slot, &off, layer_end);
    write_u32_le(slot, &off, next_node);
    write_u32_le(slot, &off, (uint32_t)qwen3_pipeline_nodes());
    write_u32_le(slot, &off, (uint32_t)qwen3_total_layers());
    write_u32_le(slot, &off, (uint32_t)hidden_bytes);
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

static void write_segment_u64(volatile uint8_t *ep_mmio, uint64_t offset, uint64_t value)
{
    mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, offset);
    mmio_write64(ep_mmio, REG_SEG_DATA_VALUE, value);
}

static uint64_t read_segment_u64(volatile uint8_t *ep_mmio, uint64_t offset);

static uint64_t qwen3_prompt_header_word(void)
{
    uint64_t word = 0;
    const uint8_t marker[8] = {'Q', '3', 'P', 'R', 'O', 'M', 'P', 'T'};

    memcpy(&word, marker, sizeof(word));
    return word;
}

static void qwen3_prompt_checksum_push_word(uint64_t *acc,
                                            uint64_t *byte_index,
                                            uint64_t word)
{
    for (uint32_t i = 0; i < 8U; ++i) {
        uint8_t byte = (uint8_t)((word >> (i * 8U)) & 0xffU);

        *acc ^= (uint64_t)byte | ((*byte_index) << 8);
        *acc *= 0x00000100000001b3ULL;
        *byte_index += 1U;
    }
}

static uint64_t qwen3_prompt_token_ids_checksum(volatile uint8_t *ep_mmio,
                                                uint64_t token_count)
{
    uint64_t acc = 0xcbf29ce484222325ULL;
    uint64_t byte_index = 0;

    qwen3_prompt_checksum_push_word(&acc, &byte_index, token_count);
    for (uint64_t index = 0; index < token_count; ++index) {
        uint64_t token_offset = 64 + index * sizeof(uint64_t);
        uint64_t token_id = read_segment_u64(ep_mmio, token_offset);

        qwen3_prompt_checksum_push_word(&acc, &byte_index, index);
        qwen3_prompt_checksum_push_word(&acc, &byte_index, token_id);
    }
    return acc;
}

static int seed_qwen3_prompt_tokens_from_env(volatile uint8_t *ep_mmio)
{
    const char *csv = getenv("SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS");
    const char *cursor;
    uint64_t token_count = 0;
    uint64_t token_offset = 64;

    const char *profile = getenv("SIM_UAPI_W4_CHIPBACKEND_PROFILE");

    if (!is_qwen3_profile_name(profile) || !csv || csv[0] == '\0') {
        return 0;
    }

    write_segment_u64(ep_mmio, 0, qwen3_prompt_header_word());
    write_segment_u64(ep_mmio, 8, 0);
    write_segment_u64(ep_mmio, 16, 0);
    write_segment_u64(ep_mmio, 24, 0);
    write_segment_u64(ep_mmio, 32, 0);

    cursor = csv;
    while (*cursor != '\0') {
        char *end = NULL;
        unsigned long long token_id;

        errno = 0;
        token_id = strtoull(cursor, &end, 10);
        if (errno != 0 || end == cursor ||
            token_offset + sizeof(uint64_t) > W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES) {
            fprintf(stderr,
                    "[w4_guest] invalid qwen3 prompt token ids token_count=%" PRIu64
                    " cursor=%s\n",
                    token_count,
                    cursor);
            return -1;
        }
        write_segment_u64(ep_mmio, token_offset, (uint64_t)token_id);
        ++token_count;
        token_offset += sizeof(uint64_t);
        if (*end == ',') {
            cursor = end + 1;
        } else if (*end == '\0') {
            cursor = end;
        } else {
            fprintf(stderr,
                    "[w4_guest] invalid qwen3 prompt token separator token_count=%" PRIu64
                    " char=%c\n",
                    token_count,
                    *end);
            return -1;
        }
    }
    if (token_count == 0) {
        fprintf(stderr, "[w4_guest] qwen3 prompt token ids empty\n");
        return -1;
    }
    write_segment_u64(ep_mmio, 24, token_count);
    printf("[w4_guest] stage qwen3_prompt_tokens_seeded tokens=%" PRIu64
           " source=guest_env target=uapi_segment status=ok\n",
           token_count);
    return 0;
}

static int append_qwen3_terminal_tokens_to_prompt(volatile uint8_t *ep_mmio,
                                                  const uint64_t *terminal_tokens,
                                                  uint64_t terminal_token_count)
{
    uint64_t base_token_count;
    uint64_t next_token_count;

    if (terminal_token_count == 0) {
        return 0;
    }
    if (!terminal_tokens) {
        return -1;
    }

    base_token_count = read_segment_u64(ep_mmio, 24);
    next_token_count = base_token_count;
    for (uint64_t step = 0; step < terminal_token_count; ++step) {
        uint64_t token_offset;

        token_offset = 64 + next_token_count * sizeof(uint64_t);
        if (token_offset + sizeof(uint64_t) > W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES) {
            fprintf(stderr,
                    "[w4_guest] fail qwen3 prompt token append overflow tokens=%" PRIu64 "\n",
                    next_token_count + 1U);
            return -1;
        }
        write_segment_u64(ep_mmio, token_offset, terminal_tokens[step]);
        next_token_count += 1U;
    }

    write_segment_u64(ep_mmio, 24, next_token_count);
    write_segment_u64(ep_mmio,
                      32,
                      qwen3_prompt_token_ids_checksum(ep_mmio, next_token_count));
    printf("[w4_guest] stage qwen3_prompt_tokens_extended base_tokens=%" PRIu64
           " append_tokens=%" PRIu64 " tokens=%" PRIu64
           " source=terminal_result_cache target=uapi_segment status=ok\n",
           base_token_count,
           terminal_token_count,
           next_token_count);
    return 0;
}

static int write_qwen3_prompt_tokens_from_history(volatile uint8_t *ep_mmio,
                                                  const uint64_t *history_tokens,
                                                  uint64_t history_token_count)
{
    uint64_t max_tokens;

    if (!history_tokens || history_token_count == 0) {
        return -1;
    }
    max_tokens = (W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES - 64U) / sizeof(uint64_t);
    if (history_token_count > max_tokens) {
        fprintf(stderr,
                "[w4_guest] fail qwen3 history prompt overflow tokens=%" PRIu64 "\n",
                history_token_count);
        return -1;
    }

    write_segment_u64(ep_mmio, 0, qwen3_prompt_header_word());
    write_segment_u64(ep_mmio, 8, 0);
    write_segment_u64(ep_mmio, 16, 0);
    write_segment_u64(ep_mmio, 24, history_token_count);
    for (uint64_t i = 0; i < history_token_count; ++i) {
        write_segment_u64(ep_mmio, 64ULL + i * sizeof(uint64_t), history_tokens[i]);
    }
    write_segment_u64(ep_mmio,
                      32,
                      qwen3_prompt_token_ids_checksum(ep_mmio, history_token_count));
    printf("[w4_guest] stage qwen3_prompt_tokens_from_history tokens=%" PRIu64
           " source=engram_history_object target=uapi_segment status=ok\n",
           history_token_count);
    return 0;
}

static uint64_t read_qwen3_prompt_tokens(volatile uint8_t *ep_mmio,
                                         uint64_t *tokens,
                                         uint64_t token_capacity)
{
    uint64_t token_count;
    uint64_t max_tokens;

    if (!tokens || token_capacity == 0) {
        return 0;
    }
    token_count = read_segment_u64(ep_mmio, 24);
    max_tokens = (W4_QWEN3_GUEST_INPUT_PAYLOAD_BYTES - 64U) / sizeof(uint64_t);
    if (token_count > max_tokens) {
        token_count = max_tokens;
    }
    if (token_count > token_capacity) {
        token_count = token_capacity;
    }
    for (uint64_t i = 0; i < token_count; ++i) {
        tokens[i] = read_segment_u64(ep_mmio, 64ULL + i * sizeof(uint64_t));
    }
    return token_count;
}

static int seed_kvcache_payload(volatile uint8_t *ep_mmio, uint64_t segment)
{
    uint64_t checksum = 0;
    size_t words = W4_DEMO_KVCACHE_PAYLOAD_BYTES / sizeof(uint64_t);
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
    printf("[w4_guest] stage uapi_kvcache_payload_seeded segment=%" PRIu64 " bytes=%u checksum=0x%016" PRIx64 " role=legacy_demo_payload\n",
           segment, W4_DEMO_KVCACHE_PAYLOAD_BYTES, checksum);
    printf("[w4_guest] stage uapi_kvcache_payload_boundaries segment=%" PRIu64 " offsets=0,248,256,4088,4096,4104 status=ok\n",
           segment);
    if (seed_qwen3_prompt_tokens_from_env(ep_mmio) != 0) {
        return -1;
    }
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
    return is_qwen3_profile_name(profile);
}

static void resolve_role(char *role, size_t role_len);

static uint64_t read_segment_u64(volatile uint8_t *ep_mmio, uint64_t offset)
{
    mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, offset);
    return mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
}

static uint64_t qwen3_output_scan_limit(volatile uint8_t *ep_mmio)
{
    uint64_t explicit_end =
        read_segment_u64(ep_mmio,
                         W4_QWEN3_RESULT_BLOCK_TABLE_HEADER +
                             W4_QWEN3_RESULT_BLOCK_METADATA_END_OFFSET);

    if (explicit_end > W4_QWEN3_OUTPUT_SCAN_FALLBACK_BYTES &&
        explicit_end <= W4_QWEN3_OUTPUT_SCAN_MAX_BYTES) {
        return explicit_end;
    }
    return W4_QWEN3_OUTPUT_SCAN_FALLBACK_BYTES;
}

static void write_segment_bytes(volatile uint8_t *ep_mmio,
                                uint64_t offset,
                                const uint8_t *bytes,
                                uint64_t len)
{
    uint64_t pos;

    for (pos = 0; pos < len; pos += sizeof(uint64_t)) {
        uint64_t word = 0;
        uint64_t chunk = len - pos;

        if (chunk > sizeof(word)) {
            chunk = sizeof(word);
        }
        memcpy(&word, bytes + pos, (size_t)chunk);
        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, offset + pos);
        mmio_write64(ep_mmio, REG_SEG_DATA_VALUE, word);
    }
}

static void read_segment_bytes(volatile uint8_t *ep_mmio,
                               uint64_t offset,
                               uint8_t *bytes,
                               uint64_t len)
{
    uint64_t pos;

    for (pos = 0; pos < len; pos += sizeof(uint64_t)) {
        uint64_t word;
        uint64_t chunk = len - pos;

        if (chunk > sizeof(word)) {
            chunk = sizeof(word);
        }
        word = read_segment_u64(ep_mmio, offset + pos);
        memcpy(bytes + pos, &word, (size_t)chunk);
    }
}

static uint64_t w4_qwen3_hidden_payload_checksum(const uint8_t *bytes, uint64_t len)
{
    uint64_t acc = 0xcbf29ce484222325ULL;
    uint64_t index;

    for (index = 0; index < len; ++index) {
        acc ^= (uint64_t)bytes[index] | (index << 8);
        acc *= 0x00000100000001b3ULL;
    }
    return acc;
}

static bool qwen3_is_trailing_metadata_marker(uint64_t marker)
{
    return marker == W4_QWEN3_MARKER_TEXT_OUTPUT_BYTES_TABLE ||
           marker == W4_QWEN3_MARKER_TOKENIZER_ASSET_TABLE ||
           marker == W4_QWEN3_MARKER_WEIGHT_REFERENCE_TABLE ||
           marker == W4_QWEN3_MARKER_WEIGHT_STAGE_LINK_TABLE ||
           marker == W4_QWEN3_MARKER_MLP_REFERENCE_TABLE ||
           marker == W4_QWEN3_MARKER_LOGITS_REFERENCE_TABLE ||
           marker == W4_QWEN3_MARKER_RANGE_FORWARD_TABLE;
}

static bool qwen3_find_trailing_metadata_table(volatile uint8_t *ep_mmio,
                                               uint64_t target_marker,
                                               uint64_t *table_header)
{
    uint64_t marker = read_segment_u64(ep_mmio, W4_QWEN3_TEXT_OUTPUT_TABLE_HEADER);
    uint64_t cursor = W4_QWEN3_TOKEN_TEXT_TABLE_END;
    uint64_t scan_limit = qwen3_output_scan_limit(ep_mmio);

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
        if (table_marker == target_marker) {
            *table_header = cursor;
            return true;
        }

        table_bytes = read_segment_u64(ep_mmio, cursor + 24);
        next_cursor = cursor + 64ULL + table_bytes;
        if (next_cursor <= cursor || next_cursor > scan_limit) {
            break;
        }
        cursor = next_cursor;
    }

    return false;
}

static bool qwen3_find_metadata_table_by_scan(volatile uint8_t *ep_mmio,
                                              uint64_t target_marker,
                                              uint64_t *table_header)
{
    uint64_t cursor;
    uint64_t scan_limit = qwen3_output_scan_limit(ep_mmio);

    for (cursor = 0; cursor + 32ULL <= scan_limit; cursor += 8ULL) {
        if (read_segment_u64(ep_mmio, cursor) == target_marker) {
            *table_header = cursor;
            return true;
        }
    }

    return false;
}

static bool qwen3_logits_table_candidate_is_valid(volatile uint8_t *ep_mmio,
                                                  uint64_t header,
                                                  bool allow_compact)
{
    uint64_t scan_limit = qwen3_output_scan_limit(ep_mmio);
    uint64_t count;
    uint64_t entry_words;
    uint64_t table_bytes;
    uint64_t token_text_header;

    if (header + 64ULL > scan_limit ||
        read_segment_u64(ep_mmio, header) != W4_QWEN3_MARKER_LOGITS_TABLE) {
        return false;
    }
    count = read_segment_u64(ep_mmio, header + 8);
    entry_words = read_segment_u64(ep_mmio, header + 16);
    table_bytes = read_segment_u64(ep_mmio, header + 24);
    if (count == 0 || count > W4_QWEN3_LOGITS_ENTRIES) {
        return false;
    }
    if (entry_words != W4_QWEN3_LOGITS_TABLE_ENTRY_WORDS &&
        !(allow_compact && entry_words == W4_QWEN3_LOGITS_TABLE_COMPACT_ENTRY_WORDS)) {
        return false;
    }
    if (table_bytes != count * entry_words * 8ULL) {
        return false;
    }
    token_text_header = header + 64ULL + table_bytes;
    if (token_text_header + 64ULL > scan_limit ||
        read_segment_u64(ep_mmio, token_text_header) != W4_QWEN3_MARKER_TOKEN_TEXT_TABLE ||
        read_segment_u64(ep_mmio, token_text_header + 8) != count ||
        read_segment_u64(ep_mmio, token_text_header + 16) !=
            W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_WORDS ||
        read_segment_u64(ep_mmio, token_text_header + 24) !=
            count * W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_BYTES) {
        return false;
    }
    return true;
}

static bool qwen3_find_logits_table_by_scan(volatile uint8_t *ep_mmio,
                                            bool allow_compact,
                                            uint64_t *table_header)
{
    uint64_t cursor;
    uint64_t scan_limit = qwen3_output_scan_limit(ep_mmio);

    for (cursor = 0; cursor + 64ULL <= scan_limit; cursor += 8ULL) {
        if (qwen3_logits_table_candidate_is_valid(ep_mmio, cursor, allow_compact)) {
            *table_header = cursor;
            return true;
        }
    }

    return false;
}

struct w4_qwen3_terminal_token_record {
    uint64_t sampled_token;
    uint64_t runner_up_token;
    uint64_t margin_milli;
    uint64_t logits_checksum;
    uint64_t text_checksum;
    uint64_t top_logit_bits;
    uint64_t runner_up_logit_bits;
    uint64_t piece_word0;
    uint64_t piece_word1;
    uint64_t candidate_count;
    uint64_t candidate_tokens[4];
    uint64_t candidate_logit_bits[4];
    uint64_t candidate_text_checksums[4];
    uint64_t candidate_piece_bytes[4];
    uint64_t candidate_piece_word0[4];
    uint64_t candidate_piece_word1[4];
};

_Static_assert(offsetof(struct w4_qwen3_terminal_token_record, text_checksum) ==
                   4ULL * sizeof(uint64_t),
               "terminal token logits fields must stay tightly packed");
_Static_assert(offsetof(struct w4_qwen3_terminal_token_record, piece_word1) ==
                   8ULL * sizeof(uint64_t),
               "terminal token text fields must stay tightly packed");

struct w4_qwen3_engram_config {
    bool enabled;
    uint32_t owner_node;
    uint64_t no_repeat_ngram_size;
    uint64_t repetition_penalty_milli;
    uint64_t history_window;
    uint64_t blocked_token_ids[16];
    uint64_t blocked_token_count;
};

static int qwen3_read_terminal_token_record(volatile uint8_t *ep_mmio,
                                            struct w4_qwen3_terminal_token_record *record)
{
    uint64_t logits_table_header = W4_QWEN3_LOGITS_TABLE_HEADER;
    uint64_t logits_table_base;
    uint64_t logits_table_bytes;
    uint64_t token_text_table_header;
    uint64_t token_text_table_base;

    if (!record) {
        return -1;
    }
    if (!qwen3_find_logits_table_by_scan(ep_mmio, true, &logits_table_header)) {
        logits_table_header = W4_QWEN3_LOGITS_TABLE_HEADER;
    }
    if (read_segment_u64(ep_mmio, logits_table_header) != W4_QWEN3_MARKER_LOGITS_TABLE) {
        return -1;
    }

    logits_table_base = logits_table_header + 64ULL;
    logits_table_bytes = read_segment_u64(ep_mmio, logits_table_header + 24);
    token_text_table_header = logits_table_base + logits_table_bytes;
    if (read_segment_u64(ep_mmio, token_text_table_header) != W4_QWEN3_MARKER_TOKEN_TEXT_TABLE) {
        token_text_table_header = W4_QWEN3_TOKEN_TEXT_TABLE_HEADER;
    }
    if (read_segment_u64(ep_mmio, token_text_table_header) != W4_QWEN3_MARKER_TOKEN_TEXT_TABLE) {
        return -1;
    }
    token_text_table_base = token_text_table_header + 64ULL;

    read_segment_bytes(ep_mmio,
                       logits_table_base + 32,
                       (uint8_t *)&record->sampled_token,
                       5ULL * sizeof(uint64_t));
    record->top_logit_bits = read_segment_u64(ep_mmio, logits_table_base + 120);
    record->runner_up_logit_bits = read_segment_u64(ep_mmio, logits_table_base + 128);
    read_segment_bytes(ep_mmio,
                       token_text_table_base + 32,
                       (uint8_t *)&record->piece_word0,
                       2ULL * sizeof(uint64_t));
    record->candidate_count = read_segment_u64(ep_mmio, logits_table_base + 160);
    if (record->candidate_count == 0 || record->candidate_count > 4) {
        record->candidate_count = 0;
    }
    for (uint64_t i = 0; i < record->candidate_count; ++i) {
        uint64_t candidate_base = logits_table_base + 168ULL + i * 48ULL;

        record->candidate_tokens[i] = read_segment_u64(ep_mmio, candidate_base);
        record->candidate_logit_bits[i] = read_segment_u64(ep_mmio, candidate_base + 8);
        record->candidate_text_checksums[i] = read_segment_u64(ep_mmio, candidate_base + 16);
        record->candidate_piece_bytes[i] = read_segment_u64(ep_mmio, candidate_base + 24);
        record->candidate_piece_word0[i] = read_segment_u64(ep_mmio, candidate_base + 32);
        record->candidate_piece_word1[i] = read_segment_u64(ep_mmio, candidate_base + 40);
    }
    if (record->candidate_count == 0) {
        record->candidate_count = 1;
        record->candidate_tokens[0] = record->sampled_token;
        record->candidate_logit_bits[0] = record->top_logit_bits;
        record->candidate_text_checksums[0] = record->text_checksum;
        record->candidate_piece_word0[0] = record->piece_word0;
        record->candidate_piece_word1[0] = record->piece_word1;
        if (record->runner_up_token != 0 && record->runner_up_token != record->sampled_token) {
            record->candidate_count = 2;
            record->candidate_tokens[1] = record->runner_up_token;
            record->candidate_logit_bits[1] = record->runner_up_logit_bits;
        }
    }
    return 0;
}

static bool qwen3_guest_engram_is_stop_token(uint64_t token)
{
    return token == 151643ULL || token == 151645ULL;
}

static bool qwen3_guest_engram_history_contains(const uint64_t *history,
                                                uint64_t history_len,
                                                uint64_t token)
{
    for (uint64_t i = 0; i < history_len; ++i) {
        if (history[i] == token) {
            return true;
        }
    }
    return false;
}

static bool qwen3_guest_engram_token_blocked(const struct w4_qwen3_engram_config *config,
                                             uint64_t token)
{
    if (!config) {
        return false;
    }
    for (uint64_t i = 0; i < config->blocked_token_count; ++i) {
        if (config->blocked_token_ids[i] == token) {
            return true;
        }
    }
    return false;
}

static bool qwen3_guest_engram_repeats_ngram(const uint64_t *history,
                                             uint64_t history_len,
                                             uint64_t token,
                                             uint64_t ngram_size)
{
    uint64_t prefix_len;
    uint64_t prefix_start;

    if (!history || ngram_size == 0 || history_len + 1U < ngram_size) {
        return false;
    }
    prefix_len = ngram_size - 1U;
    prefix_start = history_len - prefix_len;
    for (uint64_t i = 0; i + ngram_size <= history_len; ++i) {
        bool prefix_matches = true;

        for (uint64_t j = 0; j < prefix_len; ++j) {
            if (history[i + j] != history[prefix_start + j]) {
                prefix_matches = false;
                break;
            }
        }
        if (prefix_matches && history[i + prefix_len] == token) {
            return true;
        }
    }
    return false;
}

static int64_t qwen3_guest_logit_score_milli(uint64_t logit_bits, int64_t fallback)
{
    union {
        uint32_t bits;
        float value;
    } logit;

    if (logit_bits == 0) {
        return fallback;
    }
    logit.bits = (uint32_t)logit_bits;
    if (!(logit.value == logit.value) || logit.value > 9000000.0f ||
        logit.value < -9000000.0f) {
        return fallback;
    }
    return (int64_t)(logit.value * 1000.0f);
}

static void qwen3_token_piece(uint64_t sampled_token, uint64_t *word0, uint64_t *word1);
static uint64_t qwen3_sample_text_checksum(uint64_t step_index, uint64_t sampled_token);

static uint64_t qwen3_guest_engram_select_token(
    const struct w4_qwen3_engram_config *config,
    const uint64_t *history,
    uint64_t history_len,
    const struct w4_qwen3_terminal_token_record *terminal_token,
    bool *fallback_used,
    uint64_t *blocked_count,
    int64_t *top_score_out,
    int64_t *runner_up_score_out)
{
    uint64_t effective_len = history_len;
    const uint64_t *effective_history = history;
    uint64_t candidate_count;
    uint64_t best_index = UINT64_MAX;
    int64_t best_score = INT64_MIN;
    uint64_t local_blocked_count = 0;

    if (fallback_used) {
        *fallback_used = false;
    }
    if (blocked_count) {
        *blocked_count = 0;
    }
    if (!config || !config->enabled || !terminal_token) {
        return terminal_token ? terminal_token->sampled_token : 0;
    }

    candidate_count = terminal_token->candidate_count;
    if (candidate_count == 0 || candidate_count > 4) {
        candidate_count = 1;
    }
    if (qwen3_guest_engram_is_stop_token(terminal_token->candidate_tokens[0])) {
        return terminal_token->candidate_tokens[0];
    }

    if (config->history_window > 0 && effective_len > config->history_window) {
        effective_history = history + (effective_len - config->history_window);
        effective_len = config->history_window;
    }

    for (uint64_t i = 0; i < candidate_count; ++i) {
        uint64_t token = terminal_token->candidate_tokens[i];
        int64_t fallback_score = i == 0 ? (int64_t)terminal_token->margin_milli : -(int64_t)i;
        int64_t score =
            qwen3_guest_logit_score_milli(terminal_token->candidate_logit_bits[i],
                                          fallback_score);
        bool blocked;

        if (token == 0 && i > 0) {
            continue;
        }
        if (config->repetition_penalty_milli > 1000 &&
            qwen3_guest_engram_history_contains(effective_history, effective_len, token)) {
            score -= (int64_t)(config->repetition_penalty_milli - 1000U);
        }
        blocked = qwen3_guest_engram_token_blocked(config, token) ||
                  qwen3_guest_engram_repeats_ngram(effective_history,
                                                   effective_len,
                                                   token,
                                                   config->no_repeat_ngram_size);
        if (i == 0 && top_score_out) {
            *top_score_out = score;
        } else if (i == 1 && runner_up_score_out) {
            *runner_up_score_out = score;
        }
        if (blocked) {
            local_blocked_count += 1U;
            continue;
        }
        if (best_index == UINT64_MAX || score > best_score) {
            best_index = i;
            best_score = score;
        }
    }
    if (blocked_count) {
        *blocked_count = local_blocked_count;
    }
    if (best_index != UINT64_MAX) {
        return terminal_token->candidate_tokens[best_index];
    }

    if (fallback_used) {
        *fallback_used = true;
    }
    return terminal_token->candidate_tokens[0];
}

static bool qwen3_rewrite_terminal_token_record_for_engram_selection(
    struct w4_qwen3_terminal_token_record *terminal_token,
    uint32_t local_node,
    uint64_t decode_step,
    uint64_t raw_sampled_token,
    uint64_t selected_token)
{
    uint64_t raw_logit_bits = 0;
    uint64_t selected_logit_bits = 0;
    uint64_t selected_text_checksum = 0;
    uint64_t selected_piece_word0 = 0;
    uint64_t selected_piece_word1 = 0;
    uint64_t old_text_checksum;
    uint64_t old_piece_word0;
    uint64_t old_piece_word1;

    if (!terminal_token) {
        return false;
    }
    if (selected_token == raw_sampled_token) {
        terminal_token->sampled_token = selected_token;
        return true;
    }

    old_text_checksum = terminal_token->text_checksum;
    old_piece_word0 = terminal_token->piece_word0;
    old_piece_word1 = terminal_token->piece_word1;
    for (uint64_t i = 0; i < terminal_token->candidate_count && i < 4U; ++i) {
        if (terminal_token->candidate_tokens[i] == raw_sampled_token) {
            raw_logit_bits = terminal_token->candidate_logit_bits[i];
        }
        if (terminal_token->candidate_tokens[i] == selected_token) {
            selected_logit_bits = terminal_token->candidate_logit_bits[i];
            selected_text_checksum = terminal_token->candidate_text_checksums[i];
            selected_piece_word0 = terminal_token->candidate_piece_word0[i];
            selected_piece_word1 = terminal_token->candidate_piece_word1[i];
        }
    }

    if (selected_text_checksum == 0 || (selected_piece_word0 == 0 && selected_piece_word1 == 0)) {
        printf("[w4_guest] fail qwen3 engram terminal rewrite missing selected candidate"
               " text metadata local=node%u step=%" PRIu64
               " raw_token=%" PRIu64 " selected_token=%" PRIu64
               " candidate_count=%" PRIu64 "\n",
               local_node + 1U,
               decode_step,
               raw_sampled_token,
               selected_token,
               terminal_token->candidate_count);
        return false;
    }
    terminal_token->sampled_token = selected_token;
    terminal_token->runner_up_token = raw_sampled_token;
    terminal_token->top_logit_bits = selected_logit_bits;
    terminal_token->runner_up_logit_bits = raw_logit_bits;
    terminal_token->margin_milli = 0;
    terminal_token->text_checksum = selected_text_checksum;
    terminal_token->piece_word0 = selected_piece_word0;
    terminal_token->piece_word1 = selected_piece_word1;
    printf("[w4_guest] stage qwen3_engram_terminal_record_rewrite local=node%u"
           " step=%" PRIu64 " raw_token=%" PRIu64
           " selected_token=%" PRIu64 " runner_up=%" PRIu64
           " old_text_checksum=0x%016" PRIx64
           " new_text_checksum=0x%016" PRIx64
           " old_piece_word0=0x%016" PRIx64
           " old_piece_word1=0x%016" PRIx64
           " new_piece_word0=0x%016" PRIx64
           " new_piece_word1=0x%016" PRIx64
           " text_source=%s status=ok\n",
           local_node + 1U,
           decode_step,
           raw_sampled_token,
           selected_token,
           terminal_token->runner_up_token,
           old_text_checksum,
           terminal_token->text_checksum,
           old_piece_word0,
           old_piece_word1,
           terminal_token->piece_word0,
           terminal_token->piece_word1,
           "selected_candidate_text_metadata");
    return true;
}

static uint64_t qwen3_terminal_token_candidate_text_checksum(
    const struct w4_qwen3_terminal_token_record *terminal_token,
    uint64_t token)
{
    if (!terminal_token) {
        return 0;
    }
    for (uint64_t i = 0; i < terminal_token->candidate_count && i < 4U; ++i) {
        if (terminal_token->candidate_tokens[i] == token) {
            return terminal_token->candidate_text_checksums[i];
        }
    }
    return 0;
}

static int qwen3_engram_select_and_publish_step(
    struct w4_db_service *db_service,
    const struct w4_qwen3_engram_config *config,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    const uint64_t *history_tokens,
    uint64_t history_token_count,
    struct w4_qwen3_terminal_token_record *terminal_token,
    uint64_t *selected_token_out)
{
    uint64_t resolved_candidate_tokens[4] = {0};
    uint64_t resolved_candidate_logit_bits[4] = {0};
    uint64_t resolved_candidate_count = 0;
    uint64_t resolved_candidate_checksum = 0;
    uint64_t raw_sampled_token;
    uint64_t selected_token;
    uint64_t selected_text_checksum;
    uint64_t engram_blocked_count = 0;
    int64_t engram_top_score = 0;
    int64_t engram_runner_up_score = 0;
    bool engram_fallback_used = false;

    if (!db_service || !config || !config->enabled || !terminal_token ||
        !history_tokens || !selected_token_out) {
        return -1;
    }
    if (w4_db_obmm_service_v0_wait_engram_candidates(
            db_service,
            decode_step,
            600000,
            resolved_candidate_tokens,
            resolved_candidate_logit_bits,
            terminal_token->candidate_text_checksums,
            terminal_token->candidate_piece_bytes,
            terminal_token->candidate_piece_word0,
            terminal_token->candidate_piece_word1,
            4U,
            &resolved_candidate_count,
            &resolved_candidate_checksum) != 0 ||
        resolved_candidate_count == 0) {
        fprintf(stderr,
                "[w4_guest] fail qwen3 engram candidates resolve failed local=node%u\n",
                local_node + 1U);
        return -1;
    }

    terminal_token->candidate_count = resolved_candidate_count;
    memcpy(terminal_token->candidate_tokens,
           resolved_candidate_tokens,
           resolved_candidate_count * sizeof(uint64_t));
    memcpy(terminal_token->candidate_logit_bits,
           resolved_candidate_logit_bits,
           resolved_candidate_count * sizeof(uint64_t));
    terminal_token->sampled_token = terminal_token->candidate_tokens[0];
    terminal_token->runner_up_token =
        resolved_candidate_count > 1U ? terminal_token->candidate_tokens[1] : 0U;
    raw_sampled_token = terminal_token->sampled_token;
    selected_token =
        qwen3_guest_engram_select_token(config,
                                        history_tokens,
                                        history_token_count,
                                        terminal_token,
                                        &engram_fallback_used,
                                        &engram_blocked_count,
                                        &engram_top_score,
                                        &engram_runner_up_score);
    selected_text_checksum =
        qwen3_terminal_token_candidate_text_checksum(terminal_token, selected_token);
    if (selected_text_checksum == 0) {
        fprintf(stderr,
                "[w4_guest] fail qwen3 engram selected text metadata missing local=node%u"
                " step=%" PRIu64 " selected_token=%" PRIu64 "\n",
                local_node + 1U,
                decode_step,
                selected_token);
        return -1;
    }
    printf("[w4_guest] stage qwen3_engram_token_select local=node%u step=%" PRIu64
           " history_tokens=%" PRIu64 " raw_token=%" PRIu64
           " runner_up=%" PRIu64 " selected_token=%" PRIu64
           " candidate_count=%" PRIu64
           " candidate2=%" PRIu64 " candidate3=%" PRIu64
           " blocked=%" PRIu64 " fallback=%u top_score_milli=%" PRId64
           " runner_up_score_milli=%" PRId64
           " no_repeat_ngram_size=%" PRIu64
           " repetition_penalty_milli=%" PRIu64
           " history_window=%" PRIu64
           " candidate_checksum=0x%016" PRIx64
           " source=guest_policy status=ok\n",
           local_node + 1U,
           decode_step,
           history_token_count,
           raw_sampled_token,
           terminal_token->runner_up_token,
           selected_token,
           terminal_token->candidate_count,
           terminal_token->candidate_count > 2U ? terminal_token->candidate_tokens[2] : 0U,
           terminal_token->candidate_count > 3U ? terminal_token->candidate_tokens[3] : 0U,
           engram_blocked_count,
           engram_fallback_used ? 1U : 0U,
           engram_top_score,
           engram_runner_up_score,
           config->no_repeat_ngram_size,
           config->repetition_penalty_milli,
           config->history_window,
           resolved_candidate_checksum);
    if (w4_db_obmm_service_v0_publish_engram_step(
            db_service,
            local_node,
            cluster_node_count,
            decode_step,
            history_tokens,
            history_token_count,
            raw_sampled_token,
            terminal_token->runner_up_token,
            selected_token,
            engram_blocked_count,
            engram_fallback_used ? 1U : 0U,
            engram_top_score,
            engram_runner_up_score,
            config->no_repeat_ngram_size,
            config->repetition_penalty_milli,
            config->history_window,
            terminal_token->logits_checksum ? terminal_token->logits_checksum :
                                              resolved_candidate_checksum,
            selected_text_checksum) != 0) {
        fprintf(stderr,
                "[w4_guest] fail qwen3 engram object publish failed local=node%u\n",
                local_node + 1U);
        return -1;
    }
    *selected_token_out = selected_token;
    return 0;
}

static uint64_t qwen3_result_metadata_table_end(volatile uint8_t *ep_mmio)
{
    uint64_t marker = read_segment_u64(ep_mmio, W4_QWEN3_TEXT_OUTPUT_TABLE_HEADER);
    uint64_t cursor = W4_QWEN3_TOKEN_TEXT_TABLE_END;
    uint64_t scan_limit = qwen3_output_scan_limit(ep_mmio);

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
        if (next_cursor <= cursor || next_cursor > scan_limit) {
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
    return (round1_checksum ^ (tile_id * 0x9e3779b97f4a7c15ULL)) % qwen3_vocab_size();
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

    acc = qwen3_fnv1a_bytes(acc, qwen3_model_id(), strlen(qwen3_model_id()));
    acc = qwen3_fnv1a_bytes(acc, &zero, sizeof(zero));
    acc = qwen3_fnv1a_bytes(acc, W4_QWEN3_TOKENIZER_FAMILY,
                            strlen(W4_QWEN3_TOKENIZER_FAMILY));
    acc = qwen3_fnv1a_bytes(acc, &zero, sizeof(zero));
    value = qwen3_vocab_size();
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

static int verify_qwen3_range_completion_contract(const uint8_t *cq,
                                                  size_t slot_count,
                                                  uint32_t dispatch_node,
                                                  uint32_t layer_start,
                                                  uint32_t layer_end,
                                                  uint32_t next_node,
                                                  uint32_t cluster_node_count,
                                                  uint64_t expected_hidden_bytes)
{
    const uint64_t expected_total_layers = qwen3_total_layers();

    for (size_t i = 0; i < slot_count; ++i) {
        const uint8_t *slot = cq + (i * CMDQ_SLOT_BYTES);
        uint64_t op_id = read_u64_le_bytes(slot, 0);
        uint8_t source = slot[9];
        uint8_t status = slot[10];
        uint64_t marker;
        uint32_t task_magic;
        uint32_t range_node;
        uint32_t range_layer_start;
        uint32_t range_layer_end;
        uint32_t range_next_node;
        uint32_t range_pipeline_nodes;
        uint32_t range_total_layers;
        uint32_t range_hidden_bytes;
        size_t off = W4_QWEN3_COMPLETION_TASK_OFFSET;

        if (op_id != 31ULL || source != 1 || status != 1) {
            continue;
        }
        marker = read_u64_le_bytes(slot, off);
        off += sizeof(uint64_t);
        task_magic = read_u32_le_bytes(slot, off);
        off += sizeof(uint32_t);
        range_node = read_u32_le_bytes(slot, off);
        off += sizeof(uint32_t);
        range_layer_start = read_u32_le_bytes(slot, off);
        off += sizeof(uint32_t);
        range_layer_end = read_u32_le_bytes(slot, off);
        off += sizeof(uint32_t);
        range_next_node = read_u32_le_bytes(slot, off);
        off += sizeof(uint32_t);
        range_pipeline_nodes = read_u32_le_bytes(slot, off);
        off += sizeof(uint32_t);
        range_total_layers = read_u32_le_bytes(slot, off);
        off += sizeof(uint32_t);
        range_hidden_bytes = read_u32_le_bytes(slot, off);
        if (marker != W4_QWEN3_MARKER_RANGE_COMPUTE_CONTRACT ||
            task_magic != W4_QWEN3_RANGE_TASK_MAGIC ||
            range_node != dispatch_node ||
            range_layer_start != layer_start ||
            range_layer_end != layer_end ||
            range_next_node != next_node ||
            range_pipeline_nodes != cluster_node_count ||
            range_total_layers != expected_total_layers ||
            range_hidden_bytes != expected_hidden_bytes) {
            fprintf(stderr,
                    "[w4_guest] qwen3 range compute contract mismatch marker=0x%016" PRIx64
                    " magic=0x%08" PRIx32 "/0x%08" PRIx32
                    " node=%" PRIu32 "/%" PRIu32 " layers=[%" PRIu32 ",%" PRIu32 ")/[%" PRIu32 ",%" PRIu32 ")"
                    " next=%" PRIu32 "/%" PRIu32 " nodes=%" PRIu32 "/%" PRIu32
                    " total_layers=%" PRIu32 "/%" PRIu64 " hidden_bytes=%" PRIu32 "/%" PRIu64 "\n",
                    marker,
                    task_magic,
                    (uint32_t)W4_QWEN3_RANGE_TASK_MAGIC,
                    range_node,
                    dispatch_node,
                    range_layer_start,
                    range_layer_end,
                    layer_start,
                    layer_end,
                    range_next_node,
                    next_node,
                    range_pipeline_nodes,
                    cluster_node_count,
                    range_total_layers,
                    expected_total_layers,
                    range_hidden_bytes,
                    expected_hidden_bytes);
            return -1;
        }
        printf("[w4_guest] stage uapi_qwen3_range_compute_contract node=%" PRIu32 " layers=[%" PRIu32 ",%" PRIu32 ") count=%" PRIu32 " next=%" PRIu32 " pipeline_nodes=%" PRIu32 " total_layers=%" PRIu32 " hidden_bytes=%" PRIu32 " source=dispatch_task output=completion status=ok\n",
               range_node,
               range_layer_start,
               range_layer_end,
               range_layer_end - range_layer_start,
               range_next_node,
               range_pipeline_nodes,
               range_total_layers,
               range_hidden_bytes);
        return 0;
    }

    fprintf(stderr,
            "[w4_guest] qwen3 range compute contract completion sideband missing node=%" PRIu32
            " layers=[%" PRIu32 ",%" PRIu32 ") next=%" PRIu32
            " fallback=runtime_forward_metadata\n",
            dispatch_node,
            layer_start,
            layer_end,
            next_node);
    printf("[w4_guest] stage uapi_qwen3_range_compute_contract node=%" PRIu32
           " layers=[%" PRIu32 ",%" PRIu32 ") count=%" PRIu32
           " next=%" PRIu32 " pipeline_nodes=%" PRIu32
           " total_layers=%" PRIu64 " hidden_bytes=%" PRIu64
           " source=runtime_forward output=metadata status=ok\n",
           dispatch_node,
           layer_start,
           layer_end,
           layer_end - layer_start,
           next_node,
           cluster_node_count,
           expected_total_layers,
           expected_hidden_bytes);
    return 0;
}

static int verify_qwen3_range_forward_table(volatile uint8_t *ep_mmio,
                                            uint32_t dispatch_node,
                                            uint32_t layer_start,
                                            uint32_t layer_end,
                                            uint32_t next_node,
                                            uint32_t cluster_node_count,
                                            uint64_t expected_hidden_bytes,
                                            struct w4_qwen3_range_runtime_forward *runtime_out)
{
    uint64_t table_header = 0;
    uint64_t table_marker;
    uint64_t table_count;
    uint64_t entry_words;
    uint64_t table_bytes;
    uint64_t table_checksum;
    uint64_t table_range_checksum;
    uint64_t table_input_checksum;
    uint64_t table_output_checksum;
    uint64_t base;
    uint64_t entry_node;
    uint64_t entry_layer_start;
    uint64_t entry_layer_end;
    uint64_t entry_layer_count;
    uint64_t entry_next_node;
    uint64_t entry_pipeline_nodes;
    uint64_t entry_total_layers;
    uint64_t entry_hidden_bytes;
    uint64_t entry_input_checksum;
    uint64_t entry_output_checksum;
    uint64_t entry_range_checksum;
    uint64_t entry_real_layers;
    uint64_t entry_first_output_checksum;
    uint64_t entry_final_output_checksum;
    uint64_t entry_input_bytes;
    uint64_t entry_output_bytes;
    uint64_t entry_kv_payload_bytes;
    uint64_t entry_kv_payload_checksum;
    uint64_t payload_offset;
    uint64_t payload_bytes;
    uint64_t payload_checksum;
    uint64_t kv_payload_offset;
    uint64_t kv_payload_bytes;
    uint64_t kv_payload_checksum = 0;
    uint64_t scan_limit = qwen3_output_scan_limit(ep_mmio);
    uint64_t expected_total_layers = qwen3_total_layers();
    uint64_t explicit_table_header =
        read_segment_u64(ep_mmio,
                         W4_QWEN3_RESULT_BLOCK_TABLE_HEADER +
                             W4_QWEN3_RESULT_BLOCK_RANGE_FORWARD_HEADER_OFFSET);

    if (explicit_table_header + 64ULL <= scan_limit &&
        read_segment_u64(ep_mmio, explicit_table_header) ==
            W4_QWEN3_MARKER_RANGE_FORWARD_TABLE) {
        table_header = explicit_table_header;
    } else if (!qwen3_find_trailing_metadata_table(ep_mmio,
                                            W4_QWEN3_MARKER_RANGE_FORWARD_TABLE,
                                            &table_header) &&
        !qwen3_find_metadata_table_by_scan(ep_mmio,
                                           W4_QWEN3_MARKER_RANGE_FORWARD_TABLE,
                                           &table_header)) {
        fprintf(stderr,
                "[w4_guest] qwen3 range forward table missing node=%" PRIu32
                " layers=[%" PRIu32 ",%" PRIu32 ")\n",
                dispatch_node,
                layer_start,
                layer_end);
        return -1;
    }
    table_marker = read_segment_u64(ep_mmio, table_header);
    table_count = read_segment_u64(ep_mmio, table_header + 8);
    entry_words = read_segment_u64(ep_mmio, table_header + 16);
    table_bytes = read_segment_u64(ep_mmio, table_header + 24);
    table_checksum = read_segment_u64(ep_mmio, table_header + 32);
    table_range_checksum = read_segment_u64(ep_mmio, table_header + 40);
    table_input_checksum = read_segment_u64(ep_mmio, table_header + 48);
    table_output_checksum = read_segment_u64(ep_mmio, table_header + 56);
    if (table_marker != W4_QWEN3_MARKER_RANGE_FORWARD_TABLE ||
        table_count != 1ULL ||
        entry_words != W4_QWEN3_RANGE_FORWARD_TABLE_ENTRY_WORDS ||
        table_bytes <
            W4_QWEN3_RANGE_FORWARD_TABLE_ENTRY_BYTES + expected_hidden_bytes ||
        table_checksum == 0 ||
        table_range_checksum == 0 ||
        table_input_checksum == 0 ||
        table_output_checksum == 0) {
        fprintf(stderr,
                "[w4_guest] qwen3 range forward table header mismatch marker=0x%016" PRIx64
                " count=%" PRIu64 " entry_words=%" PRIu64 " bytes=%" PRIu64
                " checksum=0x%016" PRIx64 " range=0x%016" PRIx64
                " input=0x%016" PRIx64 " output=0x%016" PRIx64 "\n",
                table_marker,
                table_count,
                entry_words,
                table_bytes,
                table_checksum,
                table_range_checksum,
                table_input_checksum,
                table_output_checksum);
        return -1;
    }

    base = table_header + 64ULL;
    entry_node = read_segment_u64(ep_mmio, base);
    entry_layer_start = read_segment_u64(ep_mmio, base + 8);
    entry_layer_end = read_segment_u64(ep_mmio, base + 16);
    entry_layer_count = read_segment_u64(ep_mmio, base + 24);
    entry_next_node = read_segment_u64(ep_mmio, base + 32);
    entry_pipeline_nodes = read_segment_u64(ep_mmio, base + 40);
    entry_total_layers = read_segment_u64(ep_mmio, base + 48);
    entry_hidden_bytes = read_segment_u64(ep_mmio, base + 56);
    entry_input_checksum = read_segment_u64(ep_mmio, base + 64);
    entry_output_checksum = read_segment_u64(ep_mmio, base + 72);
    entry_range_checksum = read_segment_u64(ep_mmio, base + 80);
    entry_real_layers = read_segment_u64(ep_mmio, base + 88);
    entry_first_output_checksum = read_segment_u64(ep_mmio, base + 96);
    entry_final_output_checksum = read_segment_u64(ep_mmio, base + 104);
    entry_input_bytes = read_segment_u64(ep_mmio, base + 112);
    entry_output_bytes = read_segment_u64(ep_mmio, base + 120);
    entry_kv_payload_bytes = read_segment_u64(ep_mmio, base + 128);
    entry_kv_payload_checksum = read_segment_u64(ep_mmio, base + 136);
    payload_offset = base + W4_QWEN3_RANGE_FORWARD_TABLE_ENTRY_BYTES;
    payload_bytes = entry_output_bytes;
    if (payload_bytes > W4_QWEN3_MAX_HIDDEN_RANGE_BYTES ||
        table_bytes < W4_QWEN3_RANGE_FORWARD_TABLE_ENTRY_BYTES + payload_bytes) {
        fprintf(stderr,
                "[w4_guest] qwen3 range forward payload bounds mismatch bytes=%" PRIu64
                " max=%" PRIu64 " table_bytes=%" PRIu64 "\n",
                payload_bytes,
                (uint64_t)W4_QWEN3_MAX_HIDDEN_RANGE_BYTES,
                table_bytes);
        return -1;
    }
    kv_payload_offset = payload_offset + payload_bytes;
    kv_payload_bytes = table_bytes - W4_QWEN3_RANGE_FORWARD_TABLE_ENTRY_BYTES - payload_bytes;
    if (payload_bytes != expected_hidden_bytes) {
        fprintf(stderr,
                "[w4_guest] qwen3 range forward payload size mismatch bytes=%" PRIu64
                " expected=%" PRIu64 "\n",
                payload_bytes,
                expected_hidden_bytes);
        return -1;
    }
    if (runtime_out) {
        read_segment_bytes(ep_mmio,
                           payload_offset,
                           runtime_out->output_payload,
                           payload_bytes);
        payload_checksum =
            w4_qwen3_hidden_payload_checksum(runtime_out->output_payload,
                                             payload_bytes);
        if (kv_payload_bytes > sizeof(runtime_out->kv_payload)) {
            fprintf(stderr,
                    "[w4_guest] qwen3 range kv payload too large bytes=%" PRIu64
                    " max=%zu\n",
                    kv_payload_bytes,
                    sizeof(runtime_out->kv_payload));
            return -1;
        }
        if (kv_payload_bytes > 0) {
            read_segment_bytes(ep_mmio,
                               kv_payload_offset,
                               runtime_out->kv_payload,
                               kv_payload_bytes);
            kv_payload_checksum =
                w4_qwen3_hidden_payload_checksum(runtime_out->kv_payload,
                                                 kv_payload_bytes);
        }
    } else {
        uint8_t *payload = malloc((size_t)payload_bytes);

        if (!payload) {
            fprintf(stderr,
                    "[w4_guest] qwen3 range forward payload malloc failed bytes=%" PRIu64 "\n",
                    payload_bytes);
            return -1;
        }
        read_segment_bytes(ep_mmio, payload_offset, payload, payload_bytes);
        payload_checksum = w4_qwen3_hidden_payload_checksum(payload, payload_bytes);
        free(payload);
    }
    if (entry_node != dispatch_node ||
        entry_layer_start != layer_start ||
        entry_layer_end != layer_end ||
        entry_layer_count != layer_end - layer_start ||
        entry_next_node != next_node ||
        entry_pipeline_nodes != cluster_node_count ||
        entry_total_layers != expected_total_layers ||
        entry_hidden_bytes != expected_hidden_bytes ||
        entry_real_layers > layer_end - layer_start ||
        entry_input_bytes != expected_hidden_bytes ||
        entry_output_bytes != expected_hidden_bytes ||
        entry_kv_payload_bytes != kv_payload_bytes ||
        entry_kv_payload_checksum != kv_payload_checksum ||
        entry_input_checksum != table_input_checksum ||
        entry_output_checksum != table_output_checksum ||
        entry_range_checksum != table_range_checksum ||
        entry_first_output_checksum == 0 ||
        entry_final_output_checksum == 0) {
        fprintf(stderr,
                "[w4_guest] qwen3 range forward entry mismatch node=%" PRIu64 "/%" PRIu32
                " layers=[%" PRIu64 ",%" PRIu64 ")/[%" PRIu32 ",%" PRIu32 ")"
                " count=%" PRIu64 " next=%" PRIu64 "/%" PRIu32
                " nodes=%" PRIu64 "/%" PRIu32 " total=%" PRIu64
                " hidden=%" PRIu64 " real=%" PRIu64 " input=0x%016" PRIx64
                " output=0x%016" PRIx64 " payload=0x%016" PRIx64
                " kv_bytes=%" PRIu64 "/%" PRIu64 " kv=0x%016" PRIx64
                "/0x%016" PRIx64 " range=0x%016" PRIx64 "\n",
                entry_node,
                dispatch_node,
                entry_layer_start,
                entry_layer_end,
                layer_start,
                layer_end,
                entry_layer_count,
                entry_next_node,
                next_node,
                entry_pipeline_nodes,
                cluster_node_count,
                entry_total_layers,
                entry_hidden_bytes,
                entry_real_layers,
                entry_input_checksum,
                entry_output_checksum,
                payload_checksum,
                entry_kv_payload_bytes,
                kv_payload_bytes,
                entry_kv_payload_checksum,
                kv_payload_checksum,
                entry_range_checksum);
        return -1;
    }
    if (runtime_out) {
        runtime_out->node = entry_node;
        runtime_out->layer_start = entry_layer_start;
        runtime_out->layer_end = entry_layer_end;
        runtime_out->layer_count = entry_layer_count;
        runtime_out->next_node = entry_next_node;
        runtime_out->input_checksum = entry_input_checksum;
        runtime_out->output_checksum = entry_output_checksum;
        runtime_out->payload_checksum = payload_checksum;
        runtime_out->kv_payload_checksum = kv_payload_checksum;
        runtime_out->range_checksum = entry_range_checksum;
        runtime_out->real_layers = entry_real_layers;
        runtime_out->payload_offset = payload_offset;
        runtime_out->payload_bytes = payload_bytes;
        runtime_out->kv_payload_offset = kv_payload_offset;
        runtime_out->kv_payload_bytes = kv_payload_bytes;
    }
    printf("[w4_guest] stage uapi_qwen3_range_runtime_forward node=%" PRIu64
           " layers=[%" PRIu64 ",%" PRIu64 ") count=%" PRIu64
           " next=%" PRIu64 " pipeline_nodes=%" PRIu64
           " total_layers=%" PRIu64 " hidden_bytes=%" PRIu64
           " input_checksum=0x%016" PRIx64 " output_checksum=0x%016" PRIx64
           " range_checksum=0x%016" PRIx64 " real_layers=%" PRIu64
           " payload_offset=0x%016" PRIx64 " payload_bytes=%" PRIu64
           " kv_payload_offset=0x%016" PRIx64 " kv_payload_bytes=%" PRIu64
           " kv_payload_checksum=0x%016" PRIx64
           " source=runtime_forward output=metadata status=ok\n",
           entry_node,
           entry_layer_start,
           entry_layer_end,
           entry_layer_count,
           entry_next_node,
           entry_pipeline_nodes,
           entry_total_layers,
           entry_hidden_bytes,
           entry_input_checksum,
           entry_output_checksum,
           entry_range_checksum,
           entry_real_layers,
           payload_offset,
           payload_bytes,
           kv_payload_offset,
           kv_payload_bytes,
           kv_payload_checksum);
    return 0;
}

static int verify_dispatch_payload(volatile uint8_t *ep_mmio,
                                   const uint8_t *cq,
                                   size_t slot_count,
                                   uint64_t segment,
                                   uint64_t expected_hidden_bytes,
                                   struct w4_qwen3_range_runtime_forward *runtime_out)
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
    printf("[w4_guest] stage uapi_kvcache_payload_dispatch_result segment=%" PRIu64 " word0=0x%016" PRIx64 "\n",
           segment, observed);
    if (qwen3_profile) {
        char role[64];
        uint32_t dispatch_node = 0U;
        uint32_t layer_start = 0U;
        uint32_t layer_end = 0U;
        uint32_t next_node = 0U;
        uint32_t cluster_node_count = w4_cluster_node_count();
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
        uint64_t logits_entry_bytes = W4_QWEN3_LOGITS_TABLE_ENTRY_BYTES;
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
        uint64_t expected_layer_dep_stages;
        uint64_t expected_layer_dep_count;
        bool table_ok = true;
        bool projection_table_ok = true;
        bool layer_dep_table_ok = true;
        bool range_single_phase_forward = false;
        bool terminal_logits_owner = true;
        bool range_only_flow = false;

        resolve_role(role, sizeof(role));
        if (!w4_cluster_role_index(role, cluster_node_count, &dispatch_node) ||
            w4_db_qwen3_layer_range_for_node(dispatch_node,
                                             cluster_node_count,
                                             &layer_start,
                                             &layer_end,
                                             &next_node) != 0) {
            fprintf(stderr,
                    "[w4_guest] qwen3 range compute contract placement unavailable role=%s nodes=%u\n",
                    role,
                    cluster_node_count);
            return -1;
        }
        if (verify_qwen3_range_completion_contract(cq,
                                                   slot_count,
                                                   dispatch_node,
                                                   layer_start,
                                                   layer_end,
                                                   next_node,
                                                   cluster_node_count,
                                                   expected_hidden_bytes) != 0) {
            return -1;
        }
        if (verify_qwen3_range_forward_table(ep_mmio,
                                             dispatch_node,
                                             layer_start,
                                             layer_end,
                                             next_node,
                                             cluster_node_count,
                                             expected_hidden_bytes,
                                             runtime_out) != 0) {
            return -1;
        }
        range_single_phase_forward = cluster_node_count == 8U;
        terminal_logits_owner =
            !range_single_phase_forward || dispatch_node + 1U == cluster_node_count;
        publish_marker = read_segment_u64(ep_mmio, 8);
        resolve_marker = read_segment_u64(ep_mmio, 16);
        compute_marker = read_segment_u64(ep_mmio, 24);
        publish_count = read_segment_u64(ep_mmio, 32);
        resolve_count = read_segment_u64(ep_mmio, 40);
        compute_count = read_segment_u64(ep_mmio, 48);
        range_only_flow = range_single_phase_forward &&
                          publish_count == 0 &&
                          resolve_count == 0 &&
                          compute_count == 0;
        if (publish_marker != W4_QWEN3_MARKER_PUBLISH ||
            resolve_marker != W4_QWEN3_MARKER_RESOLVE ||
            compute_marker != W4_QWEN3_MARKER_COMPUTE ||
            (!range_only_flow &&
             (publish_count != W4_QWEN3_EXPECTED_TILES ||
              resolve_count != (range_single_phase_forward ? 0ULL : W4_QWEN3_EXPECTED_TILES) ||
              compute_count != (range_single_phase_forward ? 0ULL : W4_QWEN3_EXPECTED_TILES)))) {
            fprintf(stderr,
                    "[w4_guest] qwen3 service flow mismatch publish_marker=0x%016" PRIx64
                    " resolve_marker=0x%016" PRIx64
                    " compute_marker=0x%016" PRIx64
                    " publish=%" PRIu64 " resolve=%" PRIu64 " compute=%" PRIu64 "\n",
                    publish_marker, resolve_marker, compute_marker,
                    publish_count, resolve_count, compute_count);
            return -1;
        }
        if (range_only_flow) {
            printf("[w4_guest] stage uapi_qwen3_range_forward_only"
                   " object=range_hidden publish=%" PRIu64
                   " resolve_remote=%" PRIu64 " compute=%" PRIu64
                   " storage=obmm_object metadata=db status=ok\n",
                   publish_count, resolve_count, compute_count);
            goto qwen3_logits_tables;
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
               " mode=%s storage=block metadata=db status=ok\n",
               publish_count, resolve_count, compute_count,
               range_single_phase_forward ? "range_single_phase" : "two_phase");
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
                (!range_single_phase_forward && entry_round0_segment == entry_round1_segment) ||
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
            if (block_sample_nonzero == 0 ||
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
                read_segment_u64(ep_mmio,
                                 W4_QWEN3_RESULT_BLOCK_TABLE_HEADER +
                                     W4_QWEN3_RESULT_BLOCK_METADATA_END_OFFSET);
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
                explicit_metadata_table_end <= qwen3_output_scan_limit(ep_mmio)) {
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
qwen3_logits_tables:
        ;
        uint64_t logits_table_header = W4_QWEN3_LOGITS_TABLE_HEADER;
        uint64_t logits_table_base = W4_QWEN3_LOGITS_TABLE_BASE;
        uint64_t token_text_table_header = W4_QWEN3_TOKEN_TEXT_TABLE_HEADER;
        uint64_t token_text_table_base = W4_QWEN3_TOKEN_TEXT_TABLE_BASE;

        if (!terminal_logits_owner) {
            printf("[w4_guest] stage uapi_qwen3_logits_sampling_table"
                   " node=%u layers=[%u,%u) terminal_owner=0 status=skipped\n",
                   dispatch_node,
                   layer_start,
                   layer_end);
        } else {
            uint64_t expected_logits_entry_words = W4_QWEN3_LOGITS_TABLE_ENTRY_WORDS;

            if (range_only_flow && !qwen3_find_logits_table_by_scan(ep_mmio,
                                                                    true,
                                                                    &logits_table_header)) {
                fprintf(stderr, "[w4_guest] qwen3 logits table missing range_only=1\n");
                return -1;
            }
            logits_table_base = logits_table_header + 64ULL;
            logits_marker = read_segment_u64(ep_mmio, logits_table_header);
            logits_count = read_segment_u64(ep_mmio, logits_table_header + 8);
            logits_entry_words = read_segment_u64(ep_mmio, logits_table_header + 16);
            logits_table_bytes = read_segment_u64(ep_mmio, logits_table_header + 24);
            if (range_only_flow) {
                token_text_table_header = logits_table_base + logits_table_bytes;
                token_text_table_base = token_text_table_header + 64ULL;
                if (logits_entry_words == W4_QWEN3_LOGITS_TABLE_COMPACT_ENTRY_WORDS) {
                    expected_logits_entry_words = W4_QWEN3_LOGITS_TABLE_COMPACT_ENTRY_WORDS;
                }
            }
            logits_entry_bytes = logits_entry_words * 8ULL;
            if (logits_marker != W4_QWEN3_MARKER_LOGITS_TABLE ||
                (range_only_flow ?
                     (logits_count == 0 || logits_count > W4_QWEN3_LOGITS_ENTRIES) :
                     logits_count != W4_QWEN3_LOGITS_ENTRIES) ||
                logits_entry_words != expected_logits_entry_words ||
                logits_table_bytes != logits_count * logits_entry_bytes) {
                fprintf(stderr,
                        "[w4_guest] qwen3 logits table header mismatch marker=0x%016" PRIx64
                        " count=%" PRIu64 " entry_words=%" PRIu64 " bytes=%" PRIu64 "\n",
                        logits_marker, logits_count, logits_entry_words, logits_table_bytes);
                return -1;
            }
            uint64_t sampled_tokens[W4_QWEN3_LOGITS_ENTRIES];
            uint64_t sampled_distinct = 0;
            uint64_t logits_checksum_nonzero = 0;
            uint64_t text_checksum_nonzero = 0;
            uint64_t real_logits_count = 0;

            memset(sampled_tokens, 0, sizeof(sampled_tokens));
            for (uint64_t entry = 0; entry < logits_count; ++entry) {
                uint64_t base = logits_table_base + entry * logits_entry_bytes;
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
                uint64_t full_vocab_checked_token_count = read_segment_u64(ep_mmio, base + 104);
                uint64_t full_vocab_logits_checksum = read_segment_u64(ep_mmio, base + 112);
                uint64_t top_logit_bits = read_segment_u64(ep_mmio, base + 120);
                uint64_t runner_up_logit_bits = read_segment_u64(ep_mmio, base + 128);
                uint64_t runtime_forward_layer_count = read_segment_u64(ep_mmio, base + 136);
                uint64_t runtime_forward_final_hidden_checksum =
                    read_segment_u64(ep_mmio, base + 144);
                uint64_t runtime_forward_checksum = read_segment_u64(ep_mmio, base + 152);
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
                    qwen3_vocab_size();
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
                bool real_logits =
                    full_vocab_checked_token_count == qwen3_vocab_size() &&
                    full_vocab_logits_checksum != 0 &&
                    top_logit_bits != 0 &&
                    runner_up_logit_bits != 0;
                real_logits_count += real_logits ? 1ULL : 0ULL;
                bool seen = false;

                if (entry_shard != expected_shard ||
                    entry_tile != entry ||
                    entry_segment == 0 ||
                    entry_logits_count != qwen3_vocab_size() ||
                    entry_step != entry) {
                    fprintf(stderr,
                            "[w4_guest] qwen3 logits table mismatch entry=%" PRIu64
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
                if (real_logits) {
                    bool runtime_forward_present =
                        runtime_forward_layer_count != 0 ||
                        runtime_forward_final_hidden_checksum != 0 ||
                        runtime_forward_checksum != 0;
                    bool runtime_forward_invalid =
                        runtime_forward_layer_count != qwen3_total_layers() ||
                        runtime_forward_final_hidden_checksum == 0 ||
                        runtime_forward_checksum == 0;
                    if (entry_sampled_token >= qwen3_vocab_size() ||
                        entry_runner_up_token >= qwen3_vocab_size() ||
                        entry_sampled_token == entry_runner_up_token ||
                        entry_margin_milli == 0 ||
                        entry_logits_checksum == 0 ||
                        entry_text_checksum == 0 ||
                        qkv_reference_digest == 0 ||
                        real_path_digest == 0 ||
                        (!range_single_phase_forward && runtime_forward_invalid) ||
                        (range_single_phase_forward && runtime_forward_present &&
                         runtime_forward_invalid)) {
                        fprintf(stderr,
                                "[w4_guest] qwen3 real logits invalid entry=%" PRIu64
                                " token=%" PRIu64 " runner_up=%" PRIu64
                                " margin=%" PRIu64
                                " logits_checksum=0x%016" PRIx64
                                " text_checksum=0x%016" PRIx64
                                " qkv=0x%016" PRIx64
                                " real_path=0x%016" PRIx64
                                " runtime_layers=%" PRIu64
                                " runtime_final=0x%016" PRIx64
                                " runtime=0x%016" PRIx64 "\n",
                                entry, entry_sampled_token, entry_runner_up_token,
                                entry_margin_milli, entry_logits_checksum,
                                entry_text_checksum, qkv_reference_digest,
                                real_path_digest, runtime_forward_layer_count,
                                runtime_forward_final_hidden_checksum,
                                runtime_forward_checksum);
                        return -1;
                    }
                } else if (entry_sampled_token != expected_sampled_token ||
                           entry_runner_up_token != expected_runner_up ||
                           entry_margin_milli != expected_margin ||
                           entry_logits_checksum != expected_logits_checksum ||
                           entry_text_checksum != expected_text_checksum ||
                           full_vocab_checked_token_count != 0 ||
                           full_vocab_logits_checksum != 0 ||
                           top_logit_bits != 0 ||
                           runner_up_logit_bits != 0 ||
                           runtime_forward_layer_count != 0 ||
                           runtime_forward_final_hidden_checksum != 0 ||
                           runtime_forward_checksum != 0) {
                    fprintf(stderr,
                            "[w4_guest] qwen3 fallback logits mismatch entry=%" PRIu64
                            " token=%" PRIu64 "/%" PRIu64
                            " runner_up=%" PRIu64 "/%" PRIu64
                            " margin=%" PRIu64 "/%" PRIu64
                            " logits_checksum=0x%016" PRIx64 "/0x%016" PRIx64
                            " text_checksum=0x%016" PRIx64 "/0x%016" PRIx64
                            " full_vocab=%" PRIu64
                            " full_vocab_checksum=0x%016" PRIx64 "\n",
                            entry, entry_sampled_token, expected_sampled_token,
                            entry_runner_up_token, expected_runner_up,
                            entry_margin_milli, expected_margin,
                            entry_logits_checksum, expected_logits_checksum,
                            entry_text_checksum, expected_text_checksum,
                            full_vocab_checked_token_count,
                            full_vocab_logits_checksum);
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
            if ((real_logits_count == logits_count ? sampled_distinct < 1 : sampled_distinct < 2) ||
                logits_checksum_nonzero != logits_count ||
                text_checksum_nonzero != logits_count) {
                fprintf(stderr,
                        "[w4_guest] qwen3 logits sampling summary mismatch distinct=%" PRIu64
                        " logits_checksum_nonzero=%" PRIu64
                        " text_checksum_nonzero=%" PRIu64
                        " real_logits=%" PRIu64 "\n",
                        sampled_distinct, logits_checksum_nonzero, text_checksum_nonzero,
                        real_logits_count);
                return -1;
            }
            printf("[w4_guest] stage uapi_qwen3_logits_sampling_table entries=%" PRIu64
                   " entry_words=%" PRIu64 " table_bytes=%" PRIu64
                   " vocab=%" PRIu64 " sampled_distinct=%" PRIu64
                   " logits_checksum_nonzero=%" PRIu64
                   " text_checksum_nonzero=%" PRIu64
                   " real_logits=%" PRIu64
                   " status=ok\n",
                   logits_count, logits_entry_words, logits_table_bytes,
                   qwen3_vocab_size(), sampled_distinct,
                   logits_checksum_nonzero, text_checksum_nonzero,
                   real_logits_count);
        }
        if (!terminal_logits_owner) {
            printf("[w4_guest] stage uapi_qwen3_token_text_table"
                   " node=%u layers=[%u,%u) terminal_owner=0 status=skipped\n",
                   dispatch_node,
                   layer_start,
                   layer_end);
        } else {
            token_text_marker = read_segment_u64(ep_mmio, token_text_table_header);
            token_text_count = read_segment_u64(ep_mmio, token_text_table_header + 8);
            token_text_entry_words = read_segment_u64(ep_mmio, token_text_table_header + 16);
            token_text_table_bytes = read_segment_u64(ep_mmio, token_text_table_header + 24);
            token_text_total_bytes = read_segment_u64(ep_mmio, token_text_table_header + 32);
            token_text_policy_hash = read_segment_u64(ep_mmio, token_text_table_header + 40);
            token_text_policy_kind = read_segment_u64(ep_mmio, token_text_table_header + 48);
            const bool real_tokenizer_required = qwen3_real_tokenizer_required();
            if (token_text_marker != W4_QWEN3_MARKER_TOKEN_TEXT_TABLE ||
                token_text_count != (range_only_flow ? logits_count : W4_QWEN3_TOKEN_TEXT_ENTRIES) ||
                token_text_entry_words != W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_WORDS ||
                token_text_table_bytes != token_text_count * W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_BYTES ||
                token_text_total_bytes == 0 ||
                (real_tokenizer_required ?
                  !(token_text_policy_kind == W4_QWEN3_TOKENIZER_ASSET_POLICY_KIND &&
                    token_text_policy_hash != 0) :
                  !(range_only_flow ||
                  (token_text_policy_kind == W4_QWEN3_TOKENIZER_POLICY_KIND &&
                   token_text_total_bytes == token_text_count * W4_QWEN3_TOKEN_TEXT_PIECE_BYTES &&
                   token_text_policy_hash == qwen3_tokenizer_policy_hash()) ||
                  (token_text_policy_kind == W4_QWEN3_TOKENIZER_ASSET_POLICY_KIND &&
                   token_text_policy_hash != 0)))) {
                fprintf(stderr,
                        "[w4_guest] qwen3 token text table header mismatch marker=0x%016" PRIx64
                        " count=%" PRIu64 " entry_words=%" PRIu64
                        " table_bytes=%" PRIu64 " total_bytes=%" PRIu64
                        " policy_hash=0x%016" PRIx64
                        " synthetic_policy_hash=0x%016" PRIx64
                        " policy_kind=%" PRIu64 "\n",
                        token_text_marker, token_text_count, token_text_entry_words,
                        token_text_table_bytes, token_text_total_bytes,
                        token_text_policy_hash, qwen3_tokenizer_policy_hash(),
                        token_text_policy_kind);
                return -1;
            }
            uint64_t boundary_first = 0;
            uint64_t boundary_last = 0;
            uint64_t checksum_matches = 0;
            uint64_t packed_matches = 0;
            uint64_t byte_offset_expected = 0;

            for (uint64_t entry = 0; entry < token_text_count; ++entry) {
                uint64_t base = token_text_table_base +
                                entry * W4_QWEN3_TOKEN_TEXT_TABLE_ENTRY_BYTES;
                uint64_t logits_base = logits_table_base + entry * logits_entry_bytes;
                uint64_t sampled_token = read_segment_u64(ep_mmio, logits_base + 32);
                uint64_t text_checksum = read_segment_u64(ep_mmio, logits_base + 64);
                uint64_t expected_word0 = 0;
                uint64_t expected_word1 = 0;
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

                if (entry_step != entry ||
                    entry_token != sampled_token ||
                    entry_offset != byte_offset_expected ||
                    entry_bytes == 0 ||
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
                if (!range_only_flow &&
                    token_text_policy_kind == W4_QWEN3_TOKENIZER_POLICY_KIND) {
                    qwen3_token_piece(sampled_token, &expected_word0, &expected_word1);
                    if (entry_bytes != W4_QWEN3_TOKEN_TEXT_PIECE_BYTES ||
                        entry_word0 != expected_word0 ||
                        entry_word1 != expected_word1) {
                        fprintf(stderr,
                                "[w4_guest] qwen3 synthetic token text mismatch entry=%" PRIu64
                                " bytes=%" PRIu64 "/%" PRIu64
                                " word0=0x%016" PRIx64 "/0x%016" PRIx64
                                " word1=0x%016" PRIx64 "/0x%016" PRIx64 "\n",
                                entry, entry_bytes,
                                (uint64_t)W4_QWEN3_TOKEN_TEXT_PIECE_BYTES,
                                entry_word0, expected_word0,
                                entry_word1, expected_word1);
                        return -1;
                    }
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
        if (range_only_flow) {
            goto qwen3_done_range_optional_scaffold;
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
        expected_layer_dep_stages = range_single_phase_forward ?
                                        W4_QWEN3_LAYER_DEP_STAGES_PER_TILE - 2ULL :
                                        W4_QWEN3_LAYER_DEP_STAGES_PER_TILE;
        expected_layer_dep_count = W4_QWEN3_EXPECTED_TILES * expected_layer_dep_stages;
        if (layer_dep_marker != W4_QWEN3_MARKER_LAYER_DEP_TABLE ||
            layer_dep_count != expected_layer_dep_count ||
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
            uint64_t expected_stage_count =
                range_single_phase_forward && stage >= 23ULL ? 0ULL : W4_QWEN3_EXPECTED_TILES;

            if (layer_dep_stage_counts[stage] != expected_stage_count) {
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
qwen3_done_range_optional_scaffold:
        (void)range_only_flow;
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

static uint64_t env_u64_or_default(const char *key, uint64_t default_value)
{
    const char *value = getenv(key);
    char *end = NULL;
    unsigned long long parsed;

    if (!value || value[0] == '\0') {
        return default_value;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || (end && *end != '\0')) {
        return default_value;
    }
    return (uint64_t)parsed;
}

static bool env_bool_is_one(const char *key)
{
    const char *value = getenv(key);

    return value && strcmp(value, "1") == 0;
}

static void parse_env_u64_csv_bounded(const char *key,
                                      uint64_t *values,
                                      uint64_t value_capacity,
                                      uint64_t *value_count)
{
    const char *cursor = getenv(key);
    uint64_t count = 0;

    if (value_count) {
        *value_count = 0;
    }
    if (!cursor || !values || value_capacity == 0) {
        return;
    }
    while (*cursor != '\0' && count < value_capacity) {
        char *end = NULL;
        unsigned long long parsed;

        errno = 0;
        parsed = strtoull(cursor, &end, 10);
        if (errno != 0 || end == cursor) {
            break;
        }
        values[count++] = (uint64_t)parsed;
        if (*end == ',') {
            cursor = end + 1;
        } else if (*end == '\0') {
            cursor = end;
        } else {
            break;
        }
    }
    if (value_count) {
        *value_count = count;
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
    struct w4_qwen3_range_runtime_forward runtime_forward;
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
    bool qwen3_runtime_forward_ready = false;
    bool resource_assertions_enabled = false;
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
    uint64_t guest_decode_step = 0;
    uint64_t guest_decode_steps = 1;
    uint64_t guest_decode_step_limit = 1;
    struct w4_qwen3_engram_config qwen3_engram_config;
    uint64_t qwen3_terminal_tokens[256];
    uint64_t qwen3_terminal_token_count = 0;
    uint64_t qwen3_round_input_tokens[1024];
    uint64_t qwen3_round_input_token_count = 0;
    uint64_t uapi_completion_timeout_ms = W4_DEFAULT_TIMEOUT_MS;
    uint64_t decode_round_barrier_timeout_ms = 600000ULL;
    uint64_t round_start_ms = 0;
    uint64_t terminal_gate_ms = 0;
    uint64_t setup_ms = 0;
    uint64_t obmm_stage_ms = 0;
    uint64_t cluster_stage_ms = 0;
    uint64_t map_stage_ms = 0;
    uint64_t seed_payload_ms = 0;
    uint64_t descriptor_ms = 0;
    uint64_t input_wait_ms = 0;
    uint64_t submit_ms = 0;
    uint64_t base_submit_ms = 0;
    uint64_t doorbell_submit_ms = 0;
    uint64_t max_batch_submit_ms = 0;
    uint64_t dispatch_wait_ms = 0;
    uint64_t doorbell_log_ms = 0;
    uint64_t batch_sleep_ms = 0;
    uint64_t post_batch_ms = 0;
    uint64_t compute_window_ms = 0;
    uint64_t completion_decode_ms = 0;
    uint64_t verify_publish_ms = 0;
    uint64_t publish_ms = 0;
    uint64_t round_done_ms = 0;
    uint64_t barrier_ms = 0;
    size_t cmdq_slot_base = 0;
    size_t cq_slot_base = 0;
    size_t cmdq_depth = W4_UAPI_CMDQ_DEPTH;
    size_t cq_depth = W4_UAPI_CQ_DEPTH;
    uint8_t cq_linear[MAX_SLOTS * CMDQ_SLOT_BYTES];
    uint32_t round_dispatch_node = UINT32_MAX;
    uint32_t round_layer_start = 0;
    uint32_t round_layer_end = 0;
    uint32_t round_next_node = 0;
    bool qwen3_round_history_loaded = false;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    memset(&counts, 0, sizeof(counts));
    resolve_role(role, sizeof(role));
    memset(&qwen3_engram_config, 0, sizeof(qwen3_engram_config));
    cluster_observer_mode = env_bool_is_one("LINQU_W4_ALLOW_OBSERVER_ONLY");
    require_uapi_resource = env_bool_is_one("LINQU_W4_REQUIRE_UAPI_RESOURCE");
    enable_db_cluster = env_bool_is_one("LINQU_W4_DB_CLUSTER");
    resource_assertions_enabled = env_bool_is_one("SIM_W4_RESOURCE_ASSERTIONS");
    guest_decode_step = env_u64_or_default("SIM_QWEN3_GUEST_DECODE_STEP", 0);
    guest_decode_steps = env_u64_or_default("SIM_QWEN3_GUEST_DECODE_STEPS", 1);
    qwen3_engram_config.enabled = env_bool_is_one("SIM_QWEN3_GUEST_ENGRAM");
    {
        uint64_t owner_node =
            env_u64_or_default("SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE", 8);

        if (owner_node == 0 || owner_node > 8) {
            owner_node = 8;
        }
        qwen3_engram_config.owner_node = (uint32_t)(owner_node - 1U);
    }
    qwen3_engram_config.no_repeat_ngram_size =
        env_u64_or_default("SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE", 0);
    qwen3_engram_config.repetition_penalty_milli =
        env_u64_or_default("SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI", 1000);
    qwen3_engram_config.history_window =
        env_u64_or_default("SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW", 0);
    parse_env_u64_csv_bounded("SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS",
                              qwen3_engram_config.blocked_token_ids,
                              sizeof(qwen3_engram_config.blocked_token_ids) /
                                  sizeof(qwen3_engram_config.blocked_token_ids[0]),
                              &qwen3_engram_config.blocked_token_count);
    uapi_completion_timeout_ms = env_u64_or_default("SIM_W4_UAPI_COMPLETION_TIMEOUT_MS",
                                                    W4_DEFAULT_TIMEOUT_MS);
    decode_round_barrier_timeout_ms =
        env_u64_or_default("SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS", 600000ULL);
    if (guest_decode_steps == 0) {
        guest_decode_steps = 1;
    }
    guest_decode_step_limit = guest_decode_step + guest_decode_steps;
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
    memset(&runtime_forward, 0, sizeof(runtime_forward));
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

decode_round_start:
    if (guest_decode_step == 0) {
        qwen3_terminal_token_count = 0;
        memset(qwen3_terminal_tokens, 0, sizeof(qwen3_terminal_tokens));
    }
    fd = -1;
    root_map = MAP_FAILED;
    ep_map = MAP_FAILED;
    cmdq = MAP_FAILED;
    cq = MAP_FAILED;
    root_page_base = root_base & ~(PAGE_SIZE_BYTES - 1);
    root_page_off = root_base - root_page_base;
    ep_page_base = ep_base & ~(PAGE_SIZE_BYTES - 1);
    ep_page_off = ep_base - ep_page_base;
    mapped_via_resource = false;
    slot = 0;
    cq_tail = 0;
    terminal_gate_ms = 0;
    setup_ms = 0;
    obmm_stage_ms = 0;
    cluster_stage_ms = 0;
    map_stage_ms = 0;
    seed_payload_ms = 0;
    descriptor_ms = 0;
    input_wait_ms = 0;
    submit_ms = 0;
    base_submit_ms = 0;
    doorbell_submit_ms = 0;
    max_batch_submit_ms = 0;
    dispatch_wait_ms = 0;
    doorbell_log_ms = 0;
    batch_sleep_ms = 0;
    post_batch_ms = 0;
    compute_window_ms = 0;
    completion_decode_ms = 0;
    verify_publish_ms = 0;
    publish_ms = 0;
    round_done_ms = 0;
    barrier_ms = 0;
    round_dispatch_node = UINT32_MAX;
    round_layer_start = 0;
    round_layer_end = 0;
    round_next_node = 0;
    qwen3_round_history_loaded = false;
    memset(&counts, 0, sizeof(counts));
    memset(&runtime_forward, 0, sizeof(runtime_forward));
    qwen3_runtime_forward_ready = false;
    qwen3_round_input_token_count = 0;
    memset(qwen3_round_input_tokens, 0, sizeof(qwen3_round_input_tokens));
    round_start_ms = monotonic_ms();
    printf("[w4_guest] stage qwen3_decode_round_start step=%" PRIu64 " total_steps=%" PRIu64 "\n",
           guest_decode_step,
           guest_decode_steps);
    if (is_qwen3_profile() && enable_db_cluster && cluster_node_count == 8U &&
        guest_decode_step > 0) {
        uint64_t stage_start_ms = monotonic_ms();

        if (!db_service_ready) {
            fprintf(stderr,
                    "[w4_guest] fail qwen3 decode round gate missing step=%" PRIu64 "\n",
                    guest_decode_step);
            goto out;
        }
        if (qwen3_engram_config.enabled) {
            uint32_t local_decode_node = UINT32_MAX;
            bool needs_engram_history = false;
            uint64_t previous_engram_history_checksum = 0;
            uint64_t previous_engram_state_checksum = 0;

            if (w4_cluster_role_index(role, cluster_node_count, &local_decode_node)) {
                needs_engram_history =
                    local_decode_node == 0U ||
                    local_decode_node + 1U == cluster_node_count ||
                    local_decode_node == qwen3_engram_config.owner_node;
            }
            if (!needs_engram_history) {
                printf("[w4_guest] stage qwen3_decode_round_engram_state_skip step=%" PRIu64
                       " local=%s reason=range_worker_stateless status=ok\n",
                       guest_decode_step,
                       role);
            } else {
                if (w4_db_obmm_service_v0_wait_engram_history(
                        &db_service,
                        guest_decode_step - 1,
                        600000,
                        qwen3_round_input_tokens,
                        sizeof(qwen3_round_input_tokens) / sizeof(qwen3_round_input_tokens[0]),
                        &qwen3_round_input_token_count,
                        &previous_engram_history_checksum) != 0) {
                    fprintf(stderr,
                            "[w4_guest] fail qwen3 decode round engram history missing step=%" PRIu64 "\n",
                            guest_decode_step);
                    goto out;
                }
                qwen3_terminal_tokens[guest_decode_step - 1] =
                    qwen3_round_input_tokens[qwen3_round_input_token_count - 1U];
                if (w4_db_obmm_service_v0_wait_engram_state(
                        &db_service,
                        guest_decode_step - 1,
                        600000,
                        qwen3_round_input_token_count,
                        qwen3_terminal_tokens[guest_decode_step - 1],
                        previous_engram_history_checksum,
                        qwen3_engram_config.no_repeat_ngram_size,
                        qwen3_engram_config.repetition_penalty_milli,
                        &previous_engram_state_checksum) != 0) {
                    fprintf(stderr,
                            "[w4_guest] fail qwen3 decode round engram state missing step=%" PRIu64 "\n",
                            guest_decode_step);
                    goto out;
                }
                qwen3_round_history_loaded = true;
                printf("[w4_guest] stage qwen3_decode_round_engram_state_resolved step=%" PRIu64
                       " previous_step=%" PRIu64
                       " selected_token=%" PRIu64
                       " history_tokens=%" PRIu64
                       " history_checksum=0x%016" PRIx64
                       " state_checksum=0x%016" PRIx64
                       " target=next_round_input status=ok\n",
                       guest_decode_step,
                       guest_decode_step - 1,
                       qwen3_terminal_tokens[guest_decode_step - 1],
                       qwen3_round_input_token_count,
                       previous_engram_history_checksum,
                       previous_engram_state_checksum);
            }
        } else if (w4_db_obmm_service_v0_wait_terminal_token_result(
                       &db_service,
                       guest_decode_step - 1,
                       600000,
                       &qwen3_terminal_tokens[guest_decode_step - 1]) != 0) {
            fprintf(stderr,
                    "[w4_guest] fail qwen3 decode round gate missing step=%" PRIu64 "\n",
                    guest_decode_step);
            goto out;
        }
        if (qwen3_terminal_token_count < guest_decode_step) {
            qwen3_terminal_token_count = guest_decode_step;
        }
        terminal_gate_ms = monotonic_ms() - stage_start_ms;
    }

    setup_ms = monotonic_ms();
    if (enable_db_cluster) {
        uint64_t stage_start_ms = monotonic_ms();

        if (guest_decode_step == 0) {
            if (run_obmm_backing_stage() != 0) {
                fprintf(stderr, "[w4_guest] fail obmm backing unavailable\n");
                goto out;
            }
        } else {
            printf("[w4_guest] stage obmm_kvcache_path=reuse decode_step=%" PRIu64 "\n",
                   guest_decode_step);
        }
        obmm_stage_ms = monotonic_ms() - stage_start_ms;
        printf("[w4_guest] stage db_cluster_mode=resource_backed_uapi\n");
        stage_start_ms = monotonic_ms();
        if (resource_assertions_enabled && guest_decode_step == 0) {
            if (w4_resource_backed_db_cluster_assertions(role, cluster_node_count) != 0) {
                fprintf(stderr, "[w4_guest] fail incomplete resource-backed kvcache db cluster assertions\n");
                goto out;
            }
        }
        if (!db_service_ready &&
            w4_db_service_init(&db_service, true, true, true) == 0) {
            db_service_ready = true;
            printf("[w4_guest] stage db_service_cluster=init_ok nodes=%u decode_step=%" PRIu64 "\n",
                   cluster_node_count, guest_decode_step);
        } else if (!db_service_ready) {
            fprintf(stderr, "[w4_guest] fail db_service_cluster init failed decode_step=%" PRIu64 "\n",
                    guest_decode_step);
            goto out;
        } else {
            printf("[w4_guest] stage db_service_cluster=reuse decode_step=%" PRIu64 "\n",
                   guest_decode_step);
        }
        cluster_stage_ms = monotonic_ms() - stage_start_ms;
    }

    {
        uint64_t stage_start_ms = monotonic_ms();

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
        map_stage_ms = monotonic_ms() - stage_start_ms;
    }

    root_mmio = (volatile uint8_t *)root_map + root_page_off;
    ep_mmio = (volatile uint8_t *)ep_map + ep_page_off;
    printf("[w4_guest] step=read_root_version ok version=0x%016" PRIx64 "\n",
           mmio_read64(root_mmio, REG_VERSION));
    default_segment = mmio_read64(ep_mmio, REG_DEFAULT_SEGMENT);
    if (default_segment == 0) {
        fprintf(stderr, "[w4_guest] default segment missing\n");
        goto out;
    }
    cmdq_depth = (size_t)mmio_read64(ep_mmio, REG_CMDQ_SIZE);
    cq_depth = (size_t)mmio_read64(ep_mmio, REG_CQ_SIZE);
    if (cmdq_depth == 0 || cq_depth == 0 ||
        cmdq_depth > PAGE_SIZE_BYTES / CMDQ_SLOT_BYTES ||
        cq_depth > PAGE_SIZE_BYTES / CMDQ_SLOT_BYTES ||
        MAX_SLOTS > cmdq_depth ||
        MAX_SLOTS > cq_depth) {
        fprintf(stderr,
                "[w4_guest] invalid uapi queue depths cmdq=%zu cq=%zu\n",
                cmdq_depth,
                cq_depth);
        goto out;
    }
    printf("[w4_guest] step=read_default_segment ok segment=%" PRIu64 "\n", default_segment);
    setup_ms = monotonic_ms() - setup_ms;
    seed_payload_ms = monotonic_ms();
    if (seed_kvcache_payload(ep_mmio, default_segment) != 0) {
        goto out;
    }
    if (is_qwen3_profile() && enable_db_cluster && cluster_node_count == 8U) {
        if (qwen3_engram_config.enabled && guest_decode_step > 0 &&
            qwen3_round_history_loaded) {
            if (write_qwen3_prompt_tokens_from_history(ep_mmio,
                                                       qwen3_round_input_tokens,
                                                       qwen3_round_input_token_count) != 0) {
                goto out;
            }
        } else if (append_qwen3_terminal_tokens_to_prompt(ep_mmio,
                                                          qwen3_terminal_tokens,
                                                          qwen3_terminal_token_count) != 0) {
            goto out;
        }
    }
    if (is_qwen3_profile() && enable_db_cluster && cluster_node_count == 8U) {
        qwen3_round_input_token_count =
            read_qwen3_prompt_tokens(ep_mmio,
                                     qwen3_round_input_tokens,
                                     sizeof(qwen3_round_input_tokens) /
                                         sizeof(qwen3_round_input_tokens[0]));
    }
    seed_payload_ms = monotonic_ms() - seed_payload_ms;
    descriptor_ms = monotonic_ms();
    cmdq_slot_base = (size_t)(mmio_read64(ep_mmio, REG_CMDQ_HEAD) % cmdq_depth);
    cq_slot_base = (size_t)(mmio_read64(ep_mmio, REG_CQ_TAIL) % cq_depth);
    printf("[w4_guest] stage uapi_queue_round_base step=%" PRIu64 " cmdq_head=%zu cq_tail=%zu cmdq_depth=%zu cq_depth=%zu\n",
           guest_decode_step,
           cmdq_slot_base,
           cq_slot_base,
           cmdq_depth,
           cq_depth);

    printf("[w4_guest] stage uapi_kvcache_shmem_descriptor segment=%" PRIu64 " bytes=128 puts=1 gets=1 role=hot_shared\n",
           default_segment);
    build_shmem_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 3, default_segment, 128);
    build_shmem_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 4, default_segment, 128);
    printf("[w4_guest] stage uapi_kvcache_shmem_descriptor segment=%" PRIu64 " bytes=%u puts=1 gets=1 role=legacy_demo_payload\n",
           default_segment, W4_DEMO_KVCACHE_PAYLOAD_BYTES);
    build_shmem_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 3, default_segment, W4_DEMO_KVCACHE_PAYLOAD_BYTES);
    build_shmem_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 4, default_segment, W4_DEMO_KVCACHE_PAYLOAD_BYTES);
    printf("[w4_guest] stage uapi_kvcache_db_descriptor key=%s bytes=%" PRIu64 "\n",
           key, kvcache_db_bytes);
    build_dbput_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), key, kvcache_db_bytes);
    build_dbget_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), key);
    printf("[w4_guest] stage uapi_kvcache_db_descriptor key=%s bytes=%" PRIu64 " role=aux_block\n",
           key_aux, kvcache_db_bytes);
    build_dbput_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), key_aux, kvcache_db_bytes);
    build_dbget_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), key_aux);
    build_dfs_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 6, path, 256);
    build_dfs_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 5, path, 0);
    printf("[w4_guest] stage uapi_kvcache_block_descriptor block=%s segment=%" PRIu64 " writes=1 reads=1\n",
           block, default_segment);
    build_io_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 100, 2, default_segment, block);
    build_io_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 101, 1, default_segment, block);
    printf("[w4_guest] stage uapi_kvcache_block_descriptor block=%s segment=%" PRIu64 " writes=1 reads=1 role=aux_block_boundary\n",
           block_aux, default_segment);
    build_io_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 102, 2, default_segment, block_aux);
    build_io_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 103, 1, default_segment, block_aux);
    if (is_qwen3_profile() && enable_db_cluster && cluster_node_count == 8U) {
        uint32_t dispatch_node = 0U;
        uint32_t layer_start = 0U;
        uint32_t layer_end = 0U;
        uint32_t next_node = 0U;

        if (!w4_cluster_role_index(role, cluster_node_count, &dispatch_node) ||
            w4_db_qwen3_layer_range_for_node(dispatch_node,
                                             cluster_node_count,
                                             &layer_start,
                                             &layer_end,
                                             &next_node) != 0) {
            fprintf(stderr,
                    "[w4_guest] fail qwen3 range dispatch placement unavailable role=%s nodes=%u\n",
                    role,
                    cluster_node_count);
            goto out;
        }
        round_dispatch_node = dispatch_node;
        round_layer_start = layer_start;
        round_layer_end = layer_end;
        round_next_node = next_node;
        printf("[w4_guest] stage uapi_qwen3_range_dispatch_descriptor node=%u layers=[%u,%u) count=%u next=%u segment=%" PRIu64 " task_id=31 source=db_metadata status=ok\n",
               dispatch_node,
               layer_start,
               layer_end,
               layer_end - layer_start,
               next_node,
               default_segment);
        build_qwen3_range_dispatch_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++),
                                              31,
                                              default_segment,
                                              dispatch_node,
                                              layer_start,
                                              layer_end,
                                              next_node,
                                              qwen3_handoff_hidden_bytes(guest_decode_step));
    } else {
        printf("[w4_guest] stage uapi_chipbackend_dispatch_descriptor block=%s segment=%" PRIu64 " task_id=31\n",
               block, default_segment);
        build_io_descriptor(queue_slot_ptr(cmdq, cmdq_depth, cmdq_slot_base, slot++), 31, 3, default_segment, NULL);
    }

    if (enable_db_cluster && cluster_node_count >= 8U) {
        uint32_t dispatch_node = 0U;

        if (w4_cluster_role_index(role, cluster_node_count, &dispatch_node)) {
            unsigned int delay_ms =
                (unsigned int)(dispatch_node *
                               env_u64_or_default("SIM_QWEN3_GUEST_DISPATCH_STAGGER_MS", 0));

            if (delay_ms > 0U) {
                printf("[w4_guest] stage uapi_dispatch_stagger node=%u delay_ms=%u\n",
                       dispatch_node + 1U,
                       delay_ms);
                usleep(delay_ms * 1000U);
            }
        }
    }
    descriptor_ms = monotonic_ms() - descriptor_ms;
    if (is_qwen3_profile() && enable_db_cluster && cluster_node_count == 8U) {
        uint32_t dispatch_node = 0U;
        uint32_t layer_start = 0U;
        uint32_t layer_end = 0U;
        uint32_t next_node = 0U;

        if (!w4_cluster_role_index(role, cluster_node_count, &dispatch_node) ||
            w4_db_qwen3_layer_range_for_node(dispatch_node,
                                             cluster_node_count,
                                             &layer_start,
                                             &layer_end,
                                             &next_node) != 0) {
            fprintf(stderr,
                    "[w4_guest] fail qwen3 runtime range input placement unavailable role=%s nodes=%u\n",
                    role,
                    cluster_node_count);
            goto out;
        }
        if (layer_start > 0U) {
            uint64_t hidden_range_bytes = qwen3_handoff_hidden_bytes(guest_decode_step);
            uint8_t *range_input_payload = NULL;
            uint64_t range_input_checksum = 0;
            uint64_t stage_start_ms = monotonic_ms();

            if (hidden_range_bytes > W4_QWEN3_MAX_HIDDEN_RANGE_BYTES) {
                fprintf(stderr,
                        "[w4_guest] fail qwen3 runtime range input too large bytes=%" PRIu64
                        " max=%" PRIu64 "\n",
                        hidden_range_bytes,
                        (uint64_t)W4_QWEN3_MAX_HIDDEN_RANGE_BYTES);
                goto out;
            }
            range_input_payload = malloc((size_t)hidden_range_bytes);
            if (!range_input_payload) {
                fprintf(stderr,
                        "[w4_guest] fail qwen3 runtime range input malloc failed bytes=%" PRIu64 "\n",
                        hidden_range_bytes);
                goto out;
            }
            if (w4_db_obmm_service_v0_wait_runtime_range_input(dispatch_node,
                                                               cluster_node_count,
                                                               guest_decode_step,
                                                               range_input_payload,
                                                               hidden_range_bytes,
                                                               &range_input_checksum) != 0) {
                fprintf(stderr,
                        "[w4_guest] fail qwen3 runtime range input resolve failed node=%u layers=[%u,%u)\n",
                        dispatch_node + 1U,
                        layer_start,
                        layer_end);
                free(range_input_payload);
                goto out;
            }
            input_wait_ms = monotonic_ms() - stage_start_ms;
            write_segment_bytes(ep_mmio,
                                W4_QWEN3_RANGE_INPUT_PAYLOAD_OFFSET,
                                range_input_payload,
                                hidden_range_bytes);
            free(range_input_payload);
            printf("[w4_guest] stage qwen3_range_forward_runtime_input_loaded node=%u layers=[%u,%u) input_offset=0x%016" PRIx64 " input_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " source=obmm_object_service target=uapi_segment status=ok\n",
                   dispatch_node + 1U,
                   layer_start,
                   layer_end,
                   (uint64_t)W4_QWEN3_RANGE_INPUT_PAYLOAD_OFFSET,
                   range_input_checksum,
                   hidden_range_bytes);
        }
        if (guest_decode_step > 0) {
            uint8_t *previous_kv_payload = malloc((size_t)W4_QWEN3_MAX_KV_PAYLOAD_BYTES);
            uint64_t previous_kv_payload_bytes = 0;
            uint64_t previous_kv_payload_checksum = 0;

            if (!previous_kv_payload) {
                fprintf(stderr,
                        "[w4_guest] fail qwen3 previous range kv malloc failed bytes=%" PRIu64 "\n",
                        (uint64_t)W4_QWEN3_MAX_KV_PAYLOAD_BYTES);
                goto out;
            }
            if (w4_db_obmm_service_v0_resolve_previous_range_kv_state(
                    &db_service,
                    dispatch_node,
                    cluster_node_count,
                    guest_decode_step,
                    previous_kv_payload,
                    W4_QWEN3_MAX_KV_PAYLOAD_BYTES,
                    &previous_kv_payload_bytes,
                    &previous_kv_payload_checksum) != 0) {
                fprintf(stderr,
                        "[w4_guest] fail qwen3 previous range kv state resolve failed node=%u step=%" PRIu64 "\n",
                        dispatch_node + 1U,
                        guest_decode_step);
                free(previous_kv_payload);
                goto out;
            }
            if (previous_kv_payload_bytes > 0) {
                write_segment_u64(ep_mmio,
                                  W4_QWEN3_PREVIOUS_KV_PAYLOAD_OFFSET,
                                  W4_QWEN3_PREVIOUS_KV_PAYLOAD_MARKER);
                write_segment_u64(ep_mmio,
                                  W4_QWEN3_PREVIOUS_KV_PAYLOAD_OFFSET + 8ULL,
                                  previous_kv_payload_bytes);
                write_segment_u64(ep_mmio,
                                  W4_QWEN3_PREVIOUS_KV_PAYLOAD_OFFSET + 16ULL,
                                  previous_kv_payload_checksum);
                write_segment_u64(ep_mmio,
                                  W4_QWEN3_PREVIOUS_KV_PAYLOAD_OFFSET + 24ULL,
                                  guest_decode_step - 1U);
                write_segment_bytes(
                    ep_mmio,
                    W4_QWEN3_PREVIOUS_KV_PAYLOAD_OFFSET +
                        W4_QWEN3_PREVIOUS_KV_PAYLOAD_HEADER_BYTES,
                    previous_kv_payload,
                    previous_kv_payload_bytes);
                printf("[w4_guest] stage qwen3_range_kv_state_loaded node=%u step=%" PRIu64
                       " previous_step=%" PRIu64
                       " kv_offset=0x%016" PRIx64 " kv_bytes=%" PRIu64
                       " kv_checksum=0x%016" PRIx64
                       " source=object_service target=uapi_segment status=ok\n",
                       dispatch_node + 1U,
                       guest_decode_step,
                       guest_decode_step - 1U,
                       (uint64_t)W4_QWEN3_PREVIOUS_KV_PAYLOAD_OFFSET,
                       previous_kv_payload_bytes,
                       previous_kv_payload_checksum);
            }
            free(previous_kv_payload);
        }
    }

    compute_window_ms = monotonic_ms();
    base_submit_ms = monotonic_ms();
    mmio_write64(ep_mmio, REG_CMDQ_BASE_LO, cmdq_phys);
    mmio_write64(ep_mmio, REG_CQ_BASE_LO, cq_phys);
    mmio_write64(ep_mmio, REG_CQ_HEAD, cq_slot_base);
    base_submit_ms = monotonic_ms() - base_submit_ms;
    submit_ms = base_submit_ms;
    dispatch_wait_ms = 0;
    for (size_t submitted = 0; submitted < slot;) {
        size_t next = submitted + W4_DOORBELL_BATCH_SLOTS;
        size_t expected_cq_distance;
        uint64_t batch_start_ms;
        uint64_t batch_elapsed_ms;
        uint64_t batch_submit_ms;
        uint64_t stage_start_ms;

        if (next > slot) {
            next = slot;
        }
        batch_submit_ms = monotonic_ms();
        mmio_write64(ep_mmio, REG_CMDQ_TAIL, (cmdq_slot_base + next) % cmdq_depth);
        mmio_write64(ep_mmio, REG_DOORBELL, next);
        batch_submit_ms = monotonic_ms() - batch_submit_ms;
        doorbell_submit_ms += batch_submit_ms;
        if (batch_submit_ms > max_batch_submit_ms) {
            max_batch_submit_ms = batch_submit_ms;
        }
        submit_ms += batch_submit_ms;
        batch_start_ms = monotonic_ms();
        for (;;) {
            uint64_t elapsed_ms;

            cq_tail = mmio_read64(ep_mmio, REG_CQ_TAIL);
            expected_cq_distance = queue_distance(cq_depth,
                                                  cq_slot_base,
                                                  (size_t)(cq_tail % cq_depth));
            if (expected_cq_distance >= next) {
                break;
            }

            elapsed_ms = monotonic_ms() - batch_start_ms;
            if (elapsed_ms > uapi_completion_timeout_ms) {
                fprintf(stderr,
                        "[w4_guest] timeout waiting completions cq_tail=%" PRIu64
                        " base=%zu distance=%zu expected=%zu timeout_ms=%" PRIu64 "\n",
                        cq_tail,
                        cq_slot_base,
                        expected_cq_distance,
                        next,
                        uapi_completion_timeout_ms);
                goto out;
            }
            usleep(10000);
        }
        batch_elapsed_ms = monotonic_ms() - batch_start_ms;
        dispatch_wait_ms += batch_elapsed_ms;
        stage_start_ms = monotonic_ms();
        printf("[w4_guest] step=doorbell_batch ok submitted=%zu cq_tail=%" PRIu64
               " submit_ms=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
               next,
               cq_tail,
               batch_submit_ms,
               batch_elapsed_ms);
        doorbell_log_ms += monotonic_ms() - stage_start_ms;
        stage_start_ms = monotonic_ms();
        usleep(1000);
        batch_sleep_ms += monotonic_ms() - stage_start_ms;
        submitted = next;
    }
    post_batch_ms = monotonic_ms();
    printf("[w4_guest] step=doorbell ok slots=%zu\n", slot);

    printf("[w4_guest] step=wait_completions ok cq_tail=%" PRIu64 "\n", cq_tail);
    post_batch_ms = monotonic_ms() - post_batch_ms;

    completion_decode_ms = monotonic_ms();
    memset(cq_linear, 0, sizeof(cq_linear));
    for (size_t i = 0; i < slot; ++i) {
        struct completion_preview preview;
        memcpy(cq_linear + i * CMDQ_SLOT_BYTES,
               queue_slot_ptr(cq, cq_depth, cq_slot_base, i),
               CMDQ_SLOT_BYTES);
        if (decode_completion_preview(cq_linear + i * CMDQ_SLOT_BYTES, &preview) != 0) {
            fprintf(stderr, "[w4_guest] completion decode failed slot=%zu\n", i);
            goto out;
        }
        count_completion(&preview, &counts);
    }
    completion_decode_ms = monotonic_ms() - completion_decode_ms;
    printf("[w4_guest] step=decode_completions ok\n");
    compute_window_ms = monotonic_ms() - compute_window_ms;
    publish_ms = monotonic_ms();
    if (verify_dispatch_payload(ep_mmio,
                                cq_linear,
                                slot,
                                default_segment,
                                qwen3_handoff_hidden_bytes(guest_decode_step),
                                &runtime_forward) != 0) {
        goto out;
    }
    qwen3_runtime_forward_ready =
        is_qwen3_profile() &&
        runtime_forward.payload_bytes == qwen3_handoff_hidden_bytes(guest_decode_step);
    if (qwen3_runtime_forward_ready && enable_db_cluster && cluster_node_count == 8U) {
        uint32_t dispatch_node = 0U;

        if (!db_service_ready &&
            w4_db_service_init(&db_service, true, true, true) == 0) {
            db_service_ready = true;
        }
        if (!w4_cluster_role_index(role, cluster_node_count, &dispatch_node) ||
            !db_service_ready ||
            w4_db_obmm_service_v0_publish_runtime_range_output(
                &db_service,
                dispatch_node,
                cluster_node_count,
                guest_decode_step,
                runtime_forward.output_payload,
                runtime_forward.payload_bytes,
                runtime_forward.payload_checksum,
                runtime_forward.kv_payload,
                runtime_forward.kv_payload_bytes,
                runtime_forward.kv_payload_checksum) != 0) {
            fprintf(stderr,
                    "[w4_guest] fail qwen3 runtime range output publish failed role=%s\n",
                    role);
            goto out;
        }
        if (qwen3_engram_config.enabled &&
            dispatch_node == qwen3_engram_config.owner_node &&
            dispatch_node + 1U != cluster_node_count) {
            struct w4_qwen3_terminal_token_record owner_candidates;
            uint64_t owner_selected_token = 0;

            memset(&owner_candidates, 0, sizeof(owner_candidates));
            if (qwen3_engram_select_and_publish_step(&db_service,
                                                     &qwen3_engram_config,
                                                     dispatch_node,
                                                     cluster_node_count,
                                                     guest_decode_step,
                                                     qwen3_round_input_tokens,
                                                     qwen3_round_input_token_count,
                                                     &owner_candidates,
                                                     &owner_selected_token) != 0) {
                goto out;
            }
        }
        if (dispatch_node + 1U == cluster_node_count) {
            uint64_t decode_step = guest_decode_step;
            struct w4_qwen3_terminal_token_record terminal_token;
            uint64_t raw_sampled_token;
            uint64_t engram_selected_token;

            if (qwen3_read_terminal_token_record(ep_mmio, &terminal_token) != 0) {
                fprintf(stderr,
                        "[w4_guest] fail qwen3 terminal token record read failed role=%s\n",
                        role);
                goto out;
            }
            raw_sampled_token = terminal_token.sampled_token;
            if (qwen3_engram_config.enabled) {
                if (w4_db_obmm_service_v0_publish_engram_candidates(
                        &db_service,
                        dispatch_node,
                        cluster_node_count,
                        decode_step,
                        terminal_token.candidate_tokens,
                        terminal_token.candidate_logit_bits,
                        terminal_token.candidate_text_checksums,
                        terminal_token.candidate_piece_bytes,
                        terminal_token.candidate_piece_word0,
                        terminal_token.candidate_piece_word1,
                        terminal_token.candidate_count) != 0) {
                    fprintf(stderr,
                            "[w4_guest] fail qwen3 engram candidates publish failed role=%s\n",
                            role);
                    goto out;
                }
                if (dispatch_node == qwen3_engram_config.owner_node &&
                    qwen3_engram_select_and_publish_step(&db_service,
                                                         &qwen3_engram_config,
                                                         dispatch_node,
                                                         cluster_node_count,
                                                         decode_step,
                                                         qwen3_round_input_tokens,
                                                         qwen3_round_input_token_count,
                                                         &terminal_token,
                                                         &engram_selected_token) != 0) {
                    goto out;
                }
                if (w4_db_obmm_service_v0_wait_engram_selected_token(
                        &db_service,
                        decode_step,
                        600000,
                        &engram_selected_token) != 0) {
                    fprintf(stderr,
                            "[w4_guest] fail qwen3 engram selected token resolve failed role=%s\n",
                            role);
                    goto out;
                }
                if (!qwen3_rewrite_terminal_token_record_for_engram_selection(
                        &terminal_token,
                        dispatch_node,
                        decode_step,
                        raw_sampled_token,
                        engram_selected_token)) {
                    return 1;
                }
                terminal_token.sampled_token = engram_selected_token;
                printf("[w4_guest] stage qwen3_engram_selected_writeback local=node%u step=%" PRIu64
                       " selected_token=%" PRIu64
                       " source=engram_selected_object target=terminal_token_result status=ok\n",
                       dispatch_node + 1U,
                       decode_step,
                       terminal_token.sampled_token);
            }

            if (w4_db_obmm_service_v0_publish_terminal_token_result(
                    &db_service,
                    dispatch_node,
                    cluster_node_count,
                    decode_step,
                    terminal_token.sampled_token,
                    terminal_token.runner_up_token,
                    terminal_token.margin_milli,
                    terminal_token.logits_checksum,
                    terminal_token.text_checksum,
                    terminal_token.piece_word0,
                    terminal_token.piece_word1) != 0) {
                fprintf(stderr,
                        "[w4_guest] fail qwen3 terminal token result publish failed role=%s\n",
                        role);
                goto out;
            }
        }
    }
    publish_ms = monotonic_ms() - publish_ms;
    verify_publish_ms = publish_ms;

    mmio_write64(ep_mmio, REG_CQ_HEAD, cq_tail % cq_depth);
    mmio_write64(ep_mmio, REG_IRQ_ACK, mmio_read64(ep_mmio, REG_IRQ_STATUS));

    printf("[w4_guest] completion_sources chipbackend=%" PRIu64 " shmem=%" PRIu64
           " dfs=%" PRIu64 " db=%" PRIu64 " block=%" PRIu64 " guest_uapi=%" PRIu64 "\n",
           counts.chipbackend, counts.shmem, counts.dfs, counts.db, counts.block, counts.guest_uapi);
    printf("[w4_guest] completion_status success=%" PRIu64 " retryable=%" PRIu64
           " fatal=%" PRIu64 "\n",
           counts.success, counts.retryable, counts.fatal);
    printf("[w4_guest] stage uapi_kvcache_shmem_completion segment=%" PRIu64 " bytes=128 puts=1 gets=1 source=shmem_service role=hot_shared\n",
           default_segment);
    printf("[w4_guest] stage uapi_kvcache_shmem_completion segment=%" PRIu64 " bytes=%u puts=1 gets=1 source=shmem_service role=legacy_demo_payload\n",
           default_segment, W4_DEMO_KVCACHE_PAYLOAD_BYTES);
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
    if (is_qwen3_profile() && enable_db_cluster && cluster_node_count == 8U) {
        uint32_t dispatch_node = 0U;
        uint64_t stage_start_ms = monotonic_ms();

        if (!db_service_ready &&
            w4_db_service_init(&db_service, true, true, true) == 0) {
            db_service_ready = true;
        }
        if (!w4_cluster_role_index(role, cluster_node_count, &dispatch_node) ||
            !db_service_ready ||
            w4_db_obmm_service_v0_publish_decode_round_done(&db_service,
                                                            dispatch_node,
                                                            cluster_node_count,
                                                            guest_decode_step) != 0) {
            fprintf(stderr,
                    "[w4_guest] fail qwen3 decode round done publish failed role=%s step=%" PRIu64 "\n",
                    role,
                    guest_decode_step);
            goto out;
        }
        round_done_ms = monotonic_ms() - stage_start_ms;
    }
    {
        uint64_t total_ms = monotonic_ms() - round_start_ms;
        uint32_t layer_count =
            round_layer_end > round_layer_start ? round_layer_end - round_layer_start : 0U;
        uint64_t ms_per_layer_milli =
            layer_count > 0U ? (dispatch_wait_ms * 1000ULL) / layer_count : 0ULL;
        uint64_t accounted_ms = terminal_gate_ms + setup_ms + seed_payload_ms +
                                descriptor_ms + input_wait_ms + compute_window_ms +
                                publish_ms + round_done_ms;
        uint64_t compute_accounted_ms = submit_ms + dispatch_wait_ms + batch_sleep_ms +
                                        doorbell_log_ms + post_batch_ms + completion_decode_ms;
        uint64_t compute_unaccounted_ms =
            compute_window_ms > compute_accounted_ms ? compute_window_ms - compute_accounted_ms : 0ULL;
        uint64_t unaccounted_ms = total_ms > accounted_ms ? total_ms - accounted_ms : 0ULL;

        printf("[w4_guest] stage qwen3_worker_timing local=%s step=%" PRIu64
               " node=%u layers=[%u,%u) count=%u next=%u total_ms=%" PRIu64
               " terminal_gate_ms=%" PRIu64 " setup_ms=%" PRIu64
               " obmm_stage_ms=%" PRIu64
               " cluster_ms=%" PRIu64 " map_ms=%" PRIu64
               " seed_payload_ms=%" PRIu64 " descriptor_ms=%" PRIu64
               " input_wait_ms=%" PRIu64 " compute_window_ms=%" PRIu64
               " submit_ms=%" PRIu64 " base_submit_ms=%" PRIu64
               " doorbell_submit_ms=%" PRIu64 " max_batch_submit_ms=%" PRIu64
               " dispatch_ms=%" PRIu64 " doorbell_log_ms=%" PRIu64
               " batch_sleep_ms=%" PRIu64 " post_batch_ms=%" PRIu64
               " completion_decode_ms=%" PRIu64
               " compute_unaccounted_ms=%" PRIu64 " publish_ms=%" PRIu64
               " verify_publish_ms=%" PRIu64 " round_done_ms=%" PRIu64
               " barrier_ms=%" PRIu64 " unaccounted_ms=%" PRIu64
               " dispatch_ms_per_layer_milli=%" PRIu64 "\n",
               role,
               guest_decode_step,
               round_dispatch_node == UINT32_MAX ? 0U : round_dispatch_node + 1U,
               round_layer_start,
               round_layer_end,
               layer_count,
               round_next_node == UINT32_MAX ? 0U : round_next_node + 1U,
               total_ms,
               terminal_gate_ms,
               setup_ms,
               obmm_stage_ms,
               cluster_stage_ms,
               map_stage_ms,
               seed_payload_ms,
               descriptor_ms,
               input_wait_ms,
               compute_window_ms,
               submit_ms,
               base_submit_ms,
               doorbell_submit_ms,
               max_batch_submit_ms,
               dispatch_wait_ms,
               doorbell_log_ms,
               batch_sleep_ms,
               post_batch_ms,
               completion_decode_ms,
               compute_unaccounted_ms,
               publish_ms,
               verify_publish_ms,
               round_done_ms,
               barrier_ms,
               unaccounted_ms,
               ms_per_layer_milli);
    }
    printf("[w4_guest] pass\n");
    rc = 0;
    if (guest_decode_step + 1 < guest_decode_step_limit) {
        uint64_t stage_start_ms = monotonic_ms();

        if (is_qwen3_profile() && enable_db_cluster && cluster_node_count == 8U &&
            w4_db_obmm_service_v0_wait_all_decode_round_done(&db_service,
                                                             cluster_node_count,
                                                             guest_decode_step,
                                                             decode_round_barrier_timeout_ms) != 0) {
            fprintf(stderr,
                    "[w4_guest] fail qwen3 decode round barrier failed step=%" PRIu64 "\n",
                    guest_decode_step);
            goto out;
        }
        barrier_ms = monotonic_ms() - stage_start_ms;
        printf("[w4_guest] stage qwen3_worker_barrier_timing local=%s step=%" PRIu64
               " node=%u barrier_ms=%" PRIu64 " total_with_barrier_ms=%" PRIu64 "\n",
               role,
               guest_decode_step,
               round_dispatch_node == UINT32_MAX ? 0U : round_dispatch_node + 1U,
               barrier_ms,
               monotonic_ms() - round_start_ms);
        if (cq != MAP_FAILED) {
            munmap(cq, PAGE_SIZE_BYTES);
            cq = MAP_FAILED;
        }
        if (cmdq != MAP_FAILED) {
            munmap(cmdq, PAGE_SIZE_BYTES);
            cmdq = MAP_FAILED;
        }
        if (ep_map != MAP_FAILED) {
            munmap(ep_map, PAGE_SIZE_BYTES);
            ep_map = MAP_FAILED;
        }
        if (root_map != MAP_FAILED) {
            munmap(root_map, PAGE_SIZE_BYTES);
            root_map = MAP_FAILED;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        guest_decode_step += 1;
        rc = 1;
        goto decode_round_start;
    }

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
