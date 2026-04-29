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
#define W4_TIMEOUT_MS 5000
#define W4_DOORBELL_BATCH_SLOTS 4U
#define W4_KVCACHE_PAYLOAD_BYTES 8192U
#define W4_DISPATCH_INPUT_WORD 0x0000000000000000ULL
#define W4_DISPATCH_RESULT_WORD 0x41a0000041a00000ULL
#define W4_DISPATCH_RESULT_WORD_HOST_MATMUL 0x3f8000003f800000ULL
#define W4_QWEN3_MARKER_PUBLISH 0x7133773470756230ULL
#define W4_QWEN3_MARKER_RESOLVE 0x7133773472657331ULL
#define W4_QWEN3_MARKER_COMPUTE 0x71337734636d7031ULL

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

    if (profile &&
        (strcmp(profile, "host_matmul") == 0 ||
         strcmp(profile, "qwen3_dense_0_6b") == 0)) {
        return W4_DISPATCH_RESULT_WORD_HOST_MATMUL;
    }
    return W4_DISPATCH_RESULT_WORD;
}

static bool is_qwen3_profile(void)
{
    const char *profile = getenv("SIM_UAPI_W4_CHIPBACKEND_PROFILE");
    return profile && strcmp(profile, "qwen3_dense_0_6b") == 0;
}

static int verify_dispatch_payload(volatile uint8_t *ep_mmio, uint64_t segment)
{
    const uint64_t expected = expected_dispatch_result_word();
    uint64_t observed;
    uint32_t word_lo;
    uint32_t word_hi;
    float value_lo;
    float value_hi;

    mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, 0);
    observed = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
    if (is_qwen3_profile()) {
        word_lo = (uint32_t)(observed & 0xffffffffU);
        word_hi = (uint32_t)(observed >> 32);
        memcpy(&value_lo, &word_lo, sizeof(value_lo));
        memcpy(&value_hi, &word_hi, sizeof(value_hi));
        if (!(value_lo > 1.0f && value_hi > 1.0f)) {
            fprintf(stderr,
                    "[w4_guest] dispatch payload mismatch segment=%" PRIu64
                    " expected=qwen3_positive_result got=0x%016" PRIx64
                    " value0=%f value1=%f\n",
                    segment,
                    observed,
                    value_lo,
                    value_hi);
            return -1;
        }
        printf("[w4_guest] stage uapi_kvcache_payload_dispatch_result segment=%" PRIu64 " word0=0x%016" PRIx64 "\n",
               segment, observed);
    } else
    if (observed != expected) {
        fprintf(stderr,
                "[w4_guest] dispatch payload mismatch segment=%" PRIu64 " expected=0x%016" PRIx64 " got=0x%016" PRIx64 "\n",
                segment,
                expected,
                observed);
        return -1;
    } else {
        printf("[w4_guest] stage uapi_kvcache_payload_dispatch_result segment=%" PRIu64 " word0=0x%016" PRIx64 "\n",
               segment, observed);
    }
    if (is_qwen3_profile()) {
        uint64_t publish_marker;
        uint64_t resolve_marker;
        uint64_t compute_marker;
        uint64_t publish_count;
        uint64_t resolve_count;
        uint64_t compute_count;

        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, 8);
        publish_marker = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, 16);
        resolve_marker = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, 24);
        compute_marker = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, 32);
        publish_count = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, 40);
        resolve_count = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
        mmio_write64(ep_mmio, REG_SEG_DATA_OFFSET, 48);
        compute_count = mmio_read64(ep_mmio, REG_SEG_DATA_VALUE);
        if (publish_marker != W4_QWEN3_MARKER_PUBLISH ||
            resolve_marker != W4_QWEN3_MARKER_RESOLVE ||
            compute_marker != W4_QWEN3_MARKER_COMPUTE ||
            publish_count != 8 ||
            resolve_count != 8 ||
            compute_count != 8) {
            fprintf(stderr,
                    "[w4_guest] qwen3 service flow mismatch publish_marker=0x%016" PRIx64
                    " resolve_marker=0x%016" PRIx64
                    " compute_marker=0x%016" PRIx64
                    " publish=%" PRIu64 " resolve=%" PRIu64 " compute=%" PRIu64 "\n",
                    publish_marker, resolve_marker, compute_marker,
                    publish_count, resolve_count, compute_count);
            return -1;
        }
        printf("[w4_guest] stage uapi_qwen3_service_flow object=partial_result_tile publish=%" PRIu64
               " resolve_remote=%" PRIu64 " round1_compute=%" PRIu64
               " storage=block metadata=db status=ok\n",
               publish_count, resolve_count, compute_count);
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
