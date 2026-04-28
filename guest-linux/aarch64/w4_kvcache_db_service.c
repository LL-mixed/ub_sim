#include "w4_kvcache_db_service.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../kernel_ub/include/uapi/ub/obmm.h"

#define W4_DB_CLUSTER_PORT 18561
#define W4_DB_CLUSTER_MAX_NODES 8
#define W4_DB_CLUSTER_MAX_RECORDS 12
#define W4_DB_CLUSTER_REGION_SIZE (2ULL * 1024ULL * 1024ULL)
#define W4_DB_CLUSTER_WAIT_MS 20000L
#define W4_DB_OBMM_SERVICE_WAIT_MS 120000L
#define W4_DB_CLUSTER_IMPORT_ALIGN (2ULL * 1024ULL * 1024ULL)
#define W4_DB_CLUSTER_MAX_WINDOWS 16
#define W4_DB_OBMM_OBJECT_BYTES 8192ULL
#define W4_DB_OBMM_WEIGHT_OFFSET 0x10000ULL
#define W4_DB_OBMM_KVCACHE_OFFSET 0x14000ULL
#define W4_DB_OBMM_KIND_WEIGHT_TILE 1U
#define W4_DB_OBMM_KIND_KVCACHE_BLOCK 2U

struct w4_db_cluster_meta {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
};

struct w4_db_cluster_msg {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint16_t src_idx;
    uint16_t dst_idx;
    uint16_t reserved0;
    uint16_t reserved1;
    struct w4_db_cluster_meta meta;
};

struct w4_db_cluster_payload {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
    uint8_t record_pad[48];
    struct w4_db_record records[W4_DB_CLUSTER_MAX_RECORDS];
};

struct w4_db_cluster_payload_header {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint32_t publish_seq;
    uint32_t publish_done_seq;
};

struct w4_db_mapped_region {
    int fd;
    void *addr;
    size_t len;
    uint64_t mem_id;
};

struct w4_db_mem_window {
    uint64_t base_pa;
    uint64_t size_bytes;
    bool is_cacheable;
};

struct w4_db_cluster_slot {
    int owner_idx;
    bool is_local;
    bool map_osync;
    uint32_t export_cna;
    uint64_t mem_id;
    uint64_t local_pa;
    struct w4_db_mapped_region region;
};

struct w4_db_cluster_runtime {
    bool active;
    int node_count;
    int local_idx;
    int sockfd;
    int obmm_fd;
    uint32_t local_cna;
    uint32_t publish_seq;
    uint16_t observe_epoch;
    struct sockaddr_in peers[W4_DB_CLUSTER_MAX_NODES];
    struct w4_db_cluster_meta metas[W4_DB_CLUSTER_MAX_NODES];
    struct w4_db_cluster_slot slots[W4_DB_CLUSTER_MAX_NODES];
};

#define W4_DB_CLUSTER_MAGIC 0x57344442U
#define W4_DB_CLUSTER_VERSION 1U
#define W4_DB_CLUSTER_MSG_HELLO 1U
#define W4_DB_CLUSTER_MSG_READY 2U
#define W4_DB_CLUSTER_MSG_OBSERVED 3U
#define W4_DB_CLUSTER_PAYLOAD_MAGIC 0x57344450U

static struct w4_db_cluster_runtime g_w4_db_cluster_runtime;

static struct w4_db_record *w4_db_alloc_record(struct w4_db_service *svc);
static struct w4_db_record *w4_db_find_record(struct w4_db_service *svc, const char *key);

static long w4_db_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void w4_db_cpu_relax_wait(unsigned int *attempt)
{
    struct timespec ts;
    unsigned int step = attempt ? *attempt : 0;
    long usec = 1000L;

    if (step < 32U) {
        usec <<= step / 4U;
    } else {
        usec = 64000L;
    }
    if (usec > 64000L) {
        usec = 64000L;
    }
    ts.tv_sec = usec / 1000000L;
    ts.tv_nsec = (usec % 1000000L) * 1000L;
    (void)nanosleep(&ts, NULL);
    if (attempt && *attempt < 64U) {
        *attempt += 1U;
    }
}

static bool w4_db_parse_ip_list(const char *csv,
                                char ips[W4_DB_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
                                int *count_out)
{
    char copy[256];
    char *saveptr = NULL;
    char *tok = NULL;
    int count = 0;

    if (!csv || !count_out) {
        return false;
    }
    snprintf(copy, sizeof(copy), "%s", csv);
    tok = strtok_r(copy, ",", &saveptr);
    while (tok && count < W4_DB_CLUSTER_MAX_NODES) {
        snprintf(ips[count], INET_ADDRSTRLEN, "%s", tok);
        count += 1;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    if (count < 2) {
        return false;
    }
    *count_out = count;
    return true;
}

static bool w4_db_resolve_cluster_nodes(char local_ip[INET_ADDRSTRLEN],
                                        char ips[W4_DB_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
                                        int *node_count,
                                        int *local_idx)
{
    const char *env_local = getenv("LINQU_UB_LOCAL_IP");
    const char *env_all = getenv("LINQU_UB_ALL_IPS");
    int i;

    if (!env_local || !env_all) {
        return false;
    }
    snprintf(local_ip, INET_ADDRSTRLEN, "%s", env_local);
    if (!w4_db_parse_ip_list(env_all, ips, node_count)) {
        return false;
    }
    for (i = 0; i < *node_count; ++i) {
        if (strcmp(ips[i], local_ip) == 0) {
            *local_idx = i;
            return true;
        }
    }
    return false;
}

static bool w4_db_find_ipourma_iface(char *name, size_t name_len)
{
    FILE *fp;
    char line[512];

    fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        return false;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *colon = strchr(line, ':');
        char *left;
        size_t n;

        if (!colon) {
            continue;
        }
        *colon = '\0';
        left = line;
        while (*left == ' ' || *left == '\t') {
            left++;
        }
        if (strncmp(left, "ipourma", strlen("ipourma")) != 0) {
            continue;
        }
        n = strcspn(left, " \t\r\n");
        if (n >= name_len) {
            n = name_len - 1;
        }
        memcpy(name, left, n);
        name[n] = '\0';
        fclose(fp);
        return true;
    }
    fclose(fp);
    return false;
}

static void w4_db_install_static_arp(const char *ifname, const struct in_addr *peer_addr)
{
    struct arpreq req;
    struct sockaddr_in *pa;
    uint32_t peer = ntohl(peer_addr->s_addr);
    unsigned char mac[6] = {
        0x02, 0x00, 0x00, 0x00,
        (unsigned char)((peer >> 8) & 0xff),
        (unsigned char)(peer & 0xff),
    };
    int fd;

    memset(&req, 0, sizeof(req));
    pa = (struct sockaddr_in *)&req.arp_pa;
    pa->sin_family = AF_INET;
    pa->sin_addr = *peer_addr;
    req.arp_ha.sa_family = ARPHRD_ETHER;
    memcpy(req.arp_ha.sa_data, mac, sizeof(mac));
    req.arp_flags = ATF_PERM | ATF_COM;
    snprintf(req.arp_dev, sizeof(req.arp_dev), "%s", ifname);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return;
    }
    (void)ioctl(fd, SIOCSARP, &req);
    close(fd);
}

static int w4_db_create_udp_socket(const char *ifname)
{
    int sockfd;
    int one = 1;
    int flags;
    struct sockaddr_in bind_addr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        return -1;
    }
    (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    (void)setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(W4_DB_CLUSTER_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        close(sockfd);
        return -1;
    }
    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }
    return sockfd;
}

static int w4_db_send_msg(int sockfd, const struct sockaddr_in *peer, const void *buf, size_t len)
{
    ssize_t n = sendto(sockfd, buf, len, MSG_DONTWAIT,
                       (const struct sockaddr *)peer, sizeof(*peer));
    return (n == (ssize_t)len) ? 0 : -1;
}

static ssize_t w4_db_recv_msg(int sockfd, void *buf, size_t len, struct sockaddr_in *from)
{
    socklen_t fromlen = sizeof(*from);
    return recvfrom(sockfd, buf, len, MSG_DONTWAIT, (struct sockaddr *)from, &fromlen);
}

static bool w4_db_parse_hex_file_u64(const char *path, uint64_t *value)
{
    char buf[256];
    char *end = NULL;
    unsigned long long v;
    FILE *fp = fopen(path, "r");

    if (!fp) {
        return false;
    }
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    errno = 0;
    v = strtoull(buf, &end, 0);
    if (errno != 0 || end == buf) {
        return false;
    }
    *value = (uint64_t)v;
    return true;
}

static bool w4_db_parse_windows(struct w4_db_mem_window windows[W4_DB_CLUSTER_MAX_WINDOWS],
                                int *count_out)
{
    FILE *fp;
    char line[256];
    int count = 0;

    fp = fopen("/sys/bus/ub/devices/00001/mem_windows", "r");
    if (!fp) {
        return false;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long mar = 0;
        unsigned long long decode = 0;
        unsigned long long cc_base_mb = 0;
        unsigned long long cc_size_mb = 0;
        unsigned long long nc_base_mb = 0;
        unsigned long long nc_size_mb = 0;

        if (sscanf(line,
                   "mar%llu decode=%llx cc_base_mb=%llx cc_size_mb=%llx nc_base_mb=%llx nc_size_mb=%llx",
                   &mar, &decode, &cc_base_mb, &cc_size_mb, &nc_base_mb, &nc_size_mb) != 6) {
            continue;
        }
        if (nc_size_mb != 0 && count < W4_DB_CLUSTER_MAX_WINDOWS) {
            windows[count].base_pa = ((uint64_t)nc_base_mb) << 20;
            windows[count].size_bytes = ((uint64_t)nc_size_mb) << 20;
            windows[count].is_cacheable = false;
            count += 1;
        } else if (cc_size_mb != 0 && count < W4_DB_CLUSTER_MAX_WINDOWS) {
            windows[count].base_pa = ((uint64_t)cc_base_mb) << 20;
            windows[count].size_bytes = ((uint64_t)cc_size_mb) << 20;
            windows[count].is_cacheable = true;
            count += 1;
        }
    }
    fclose(fp);
    *count_out = count;
    return count > 0;
}

static uint64_t w4_db_align_up_u64(uint64_t value, uint64_t align)
{
    return (value + align - 1U) & ~(align - 1U);
}

static bool w4_db_allocate_import_pas(int import_count,
                                      uint64_t size_per_import,
                                      uint64_t pas[W4_DB_CLUSTER_MAX_NODES],
                                      bool map_osync[W4_DB_CLUSTER_MAX_NODES])
{
    struct w4_db_mem_window windows[W4_DB_CLUSTER_MAX_WINDOWS];
    int window_count = 0;
    int import_idx = 0;
    int wi;

    if (import_count <= 0) {
        return true;
    }
    if (!w4_db_parse_windows(windows, &window_count)) {
        printf("[w4_guest] gap db_service_cluster_stage=parse_windows_failed\n");
        return false;
    }
    for (wi = 0; wi < window_count && import_idx < import_count; ++wi) {
        uint64_t cur = w4_db_align_up_u64(windows[wi].base_pa, W4_DB_CLUSTER_IMPORT_ALIGN);
        uint64_t end = windows[wi].base_pa + windows[wi].size_bytes;

        while (import_idx < import_count && cur + size_per_import <= end) {
            pas[import_idx] = cur;
            map_osync[import_idx] = !windows[wi].is_cacheable;
            import_idx += 1;
            cur = w4_db_align_up_u64(cur + size_per_import, W4_DB_CLUSTER_IMPORT_ALIGN);
        }
    }
    return import_idx == import_count;
}

static int w4_db_open_obmm(void)
{
    return open("/dev/obmm", O_RDWR);
}

static int w4_db_open_region_dev(uint64_t mem_id, bool map_osync)
{
    char path[128];

    snprintf(path, sizeof(path), "/dev/obmm_shmdev%" PRIu64, mem_id);
    return open(path, O_RDWR | (map_osync ? O_SYNC : 0));
}

static int w4_db_map_region_device(uint64_t mem_id,
                                   size_t len,
                                   bool map_osync,
                                   struct w4_db_mapped_region *region)
{
    memset(region, 0, sizeof(*region));
    region->fd = -1;
    region->mem_id = mem_id;
    region->len = len;

    region->fd = w4_db_open_region_dev(mem_id, map_osync);
    if (region->fd < 0) {
        return -1;
    }
    region->addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, region->fd, 0);
    if (region->addr == MAP_FAILED) {
        close(region->fd);
        region->fd = -1;
        region->addr = NULL;
        return -1;
    }
    return 0;
}

static int w4_db_update_region_range_at(const struct w4_db_cluster_slot *slot,
                                        uint64_t offset,
                                        uint64_t length,
                                        bool for_write)
{
    struct obmm_cmd_update_range cmd;
    uintptr_t start;
    uintptr_t end;
    uintptr_t page_size;

    if (!slot || !slot->region.addr || slot->region.fd < 0) {
        return -1;
    }
    if (slot->map_osync) {
        return 0;
    }
    if (length == 0 || offset + length > slot->region.len) {
        return -1;
    }
    start = (uintptr_t)slot->region.addr + (uintptr_t)offset;
    end = start + (uintptr_t)length;
    page_size = (uintptr_t)sysconf(_SC_PAGESIZE);
    if (page_size == 0) {
        page_size = 4096;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.start = start & ~(uintptr_t)(page_size - 1);
    cmd.end = (end + page_size - 1) & ~(uintptr_t)(page_size - 1);
    cmd.mem_state = (slot->map_osync ? OBMM_SHM_MEM_NORMAL_NC : OBMM_SHM_MEM_NORMAL) |
                    OBMM_SHM_MEM_READWRITE;
    cmd.cache_ops = for_write ? OBMM_SHM_CACHE_WB_INVAL : OBMM_SHM_CACHE_INVAL;
    if (ioctl(slot->region.fd, OBMM_SHMDEV_UPDATE_RANGE, &cmd) == 0) {
        return 0;
    }
    fprintf(stderr,
            "[w4_db] update_range_failed owner=%d write=%d fd=%d start=%#llx end=%#llx errno=%d\n",
            slot->owner_idx + 1, for_write ? 1 : 0, slot->region.fd,
            (unsigned long long)cmd.start, (unsigned long long)cmd.end, errno);
    return -1;
}

static int w4_db_update_region_range(const struct w4_db_cluster_slot *slot, bool for_write)
{
    return w4_db_update_region_range_at(slot, 0, sizeof(struct w4_db_cluster_payload), for_write);
}

static int w4_db_sync_remote_range(const struct w4_db_cluster_slot *slot,
                                  uint64_t offset,
                                  uint64_t length)
{
    obmm_cmd_sync_remote_range cmd;
    struct stat st;
    char fd_path[64];
    char fd_target[256];
    ssize_t n;

    if (!slot || !slot->region.addr || slot->region.fd < 0) {
        return -1;
    }
    if (!slot->map_osync || slot->is_local) {
        return 0;
    }
    if (length == 0) {
        return 0;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.offset = offset;
    cmd.length = length;
    if (ioctl(slot->region.fd, OBMM_SHMDEV_SYNC_REMOTE_RANGE, &cmd) == 0) {
        return 0;
    }
    fd_target[0] = '\0';
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", slot->region.fd);
    n = readlink(fd_path, fd_target, sizeof(fd_target) - 1);
    if (n > 0) {
        fd_target[n] = '\0';
    } else {
        snprintf(fd_target, sizeof(fd_target), "<readlink:%s>", strerror(errno));
    }
    fprintf(stderr,
            "[w4_db] sync_remote_range_failed owner=%d fd=%d target=%s offset=%#" PRIx64
            " len=%#" PRIx64 " errno=%d",
            slot->owner_idx + 1,
            slot->region.fd,
            fd_target,
            offset,
            length,
            errno);
    if (fstat(slot->region.fd, &st) == 0) {
        fprintf(stderr,
                " mode=%#o rdev=%u:%u",
                st.st_mode,
                major(st.st_rdev),
                minor(st.st_rdev));
    }
    fputc('\n', stderr);
    return -1;
}

static void w4_db_unmap_region_device(struct w4_db_mapped_region *region)
{
    if (region->addr && region->addr != MAP_FAILED) {
        munmap(region->addr, region->len);
        region->addr = NULL;
    }
    if (region->fd >= 0) {
        close(region->fd);
        region->fd = -1;
    }
}

static int w4_db_do_export_region(int obmm_fd, struct w4_db_cluster_meta *meta)
{
    struct obmm_cmd_export cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.length = 1;
    cmd.size[0] = W4_DB_CLUSTER_REGION_SIZE;
    cmd.flags = OBMM_EXPORT_FLAG_ALLOW_MMAP;
    cmd.pxm_numa = 0;
    if (ioctl(obmm_fd, OBMM_CMD_EXPORT, &cmd) != 0) {
        return -1;
    }
    meta->export_mem_id = cmd.mem_id;
    meta->remote_uba = cmd.uba;
    meta->size = W4_DB_CLUSTER_REGION_SIZE;
    meta->token_id = cmd.tokenid;
    return 0;
}

static int w4_db_do_unexport_region(int obmm_fd, uint64_t mem_id)
{
    struct obmm_cmd_unexport cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.mem_id = mem_id;
    return ioctl(obmm_fd, OBMM_CMD_UNEXPORT, &cmd);
}

static int w4_db_do_import_region(int obmm_fd,
                                  const struct w4_db_cluster_meta *meta,
                                  uint32_t local_cna,
                                  uint64_t local_pa,
                                  uint64_t *import_mem_id)
{
    struct obmm_sim_dec_import_priv_v1_user {
        uint32_t magic;
        uint16_t version;
        uint16_t len;
        uint64_t remote_uba;
        uint32_t token_value;
        uint32_t flags;
    } priv;
    struct obmm_cmd_import cmd;

    memset(&priv, 0, sizeof(priv));
    priv.magic = 0x53444950U;
    priv.version = 1;
    priv.len = sizeof(priv);
    priv.remote_uba = meta->remote_uba;
    priv.token_value = meta->token_id;

    memset(&cmd, 0, sizeof(cmd));
    cmd.flags = OBMM_IMPORT_FLAG_ALLOW_MMAP;
    cmd.addr = local_pa;
    cmd.length = meta->size;
    cmd.tokenid = meta->token_id;
    cmd.scna = local_cna;
    cmd.dcna = meta->export_cna;
    cmd.priv_len = sizeof(priv);
    cmd.priv = &priv;
    if (ioctl(obmm_fd, OBMM_CMD_IMPORT, &cmd) != 0) {
        return -1;
    }
    *import_mem_id = cmd.mem_id;
    return 0;
}

static int w4_db_do_unimport_region(int obmm_fd, uint64_t mem_id)
{
    struct obmm_cmd_unimport cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.mem_id = mem_id;
    return ioctl(obmm_fd, OBMM_CMD_UNIMPORT, &cmd);
}

static void w4_db_init_cluster_msg(struct w4_db_cluster_msg *msg,
                                   uint16_t type,
                                   int src_idx,
                                   int dst_idx)
{
    memset(msg, 0, sizeof(*msg));
    msg->magic = W4_DB_CLUSTER_MAGIC;
    msg->version = W4_DB_CLUSTER_VERSION;
    msg->type = type;
    msg->src_idx = (uint16_t)src_idx;
    msg->dst_idx = (uint16_t)dst_idx;
}

static uint16_t w4_db_snapshot_metadata_records(struct w4_db_service *svc,
                                                struct w4_db_record *out,
                                                uint16_t max_records)
{
    uint16_t count = 0;
    size_t i;

    if (!svc || !out || max_records == 0) {
        return 0;
    }
    for (i = 0; i < W4_DB_MAX_RECORDS && count < max_records; ++i) {
        if (!svc->records[i].in_use) {
            continue;
        }
        out[count++] = svc->records[i];
    }
    return count;
}

static int w4_db_write_cluster_payload(struct w4_db_service *svc,
                                       struct w4_db_cluster_slot *slot)
{
    struct w4_db_cluster_payload payload;
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    uint32_t seq;

    if (!svc || !slot || !slot->region.addr) {
        return -1;
    }
    seq = ++rt->publish_seq;
    if (seq == 0) {
        seq = ++rt->publish_seq;
    }
    memset(&payload, 0, sizeof(payload));
    payload.magic = W4_DB_CLUSTER_PAYLOAD_MAGIC;
    payload.version = W4_DB_CLUSTER_VERSION;
    payload.record_count = w4_db_snapshot_metadata_records(svc,
                                                           payload.records,
                                                           W4_DB_CLUSTER_MAX_RECORDS);
    payload.publish_seq = seq;
    payload.publish_done_seq = 0;
    memset(slot->region.addr, 0, sizeof(payload));
    memcpy(slot->region.addr, &payload, sizeof(payload));
    __sync_synchronize();
    ((struct w4_db_cluster_payload *)slot->region.addr)->publish_done_seq = seq;
    __sync_synchronize();
    if (w4_db_update_region_range(slot, true) != 0) {
        return -1;
    }
    (void)msync(slot->region.addr, sizeof(payload), MS_SYNC);
    {
        const uint8_t *bytes = (const uint8_t *)slot->region.addr;
        uint64_t probe_040 = 0;
        uint64_t probe_048 = 0;
        uint64_t probe_050 = 0;

        memcpy(&probe_040, bytes + 0x40, sizeof(probe_040));
        memcpy(&probe_048, bytes + 0x48, sizeof(probe_048));
        memcpy(&probe_050, bytes + 0x50, sizeof(probe_050));
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=write_local_payload probe040=%#" PRIx64 " probe048=%#" PRIx64 " probe050=%#" PRIx64 "\n",
               slot->owner_idx + 1,
               probe_040,
               probe_048,
               probe_050);
    }
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=write_local_done seq=%u done=%u count=%u\n",
           slot->owner_idx + 1,
           ((const struct w4_db_cluster_payload *)slot->region.addr)->publish_seq,
           ((const struct w4_db_cluster_payload *)slot->region.addr)->publish_done_seq,
           ((const struct w4_db_cluster_payload *)slot->region.addr)->record_count);
    return 0;
}

static uint64_t w4_db_checksum_bytes(const uint8_t *bytes, uint64_t len)
{
    uint64_t hash = 1469598103934665603ULL;
    uint64_t i;

    for (i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static void w4_db_fill_obmm_object_payload(uint8_t *dst,
                                           uint64_t len,
                                           uint32_t owner_node,
                                           uint32_t payload_kind)
{
    uint64_t i;

    for (i = 0; i < len; ++i) {
        dst[i] = (uint8_t)((i * 17ULL + (uint64_t)(owner_node + 1U) * 29ULL +
                            (uint64_t)payload_kind * 53ULL) & 0xffU);
    }
    if (len >= 4104U) {
        memcpy(dst + 0, "W4OBMM00", 8);
        memcpy(dst + 248, "W4OBMM248", 9);
        memcpy(dst + 256, "W4OBMM256", 9);
        memcpy(dst + 4088, "W4OBMM4088", 10);
        memcpy(dst + 4096, "W4OBMM4096", 10);
    }
}

static const char *w4_db_object_kind_name(uint32_t payload_kind)
{
    switch (payload_kind) {
    case W4_DB_OBMM_KIND_WEIGHT_TILE:
        return "weight_tile";
    case W4_DB_OBMM_KIND_KVCACHE_BLOCK:
        return "kvcache_block";
    default:
        return "unknown";
    }
}

static int w4_db_put_obmm_object_record(struct w4_db_service *svc,
                                        enum w4_db_record_kind record_kind,
                                        const char *key,
                                        uint32_t owner_node,
                                        uint32_t payload_kind,
                                        uint64_t offset,
                                        uint64_t len,
                                        uint64_t checksum,
                                        struct w4_db_record *resolved_out)
{
    struct w4_db_record *rec;

    if (!svc || !key || len == 0) {
        return -1;
    }
    rec = w4_db_find_record(svc, key);
    if (!rec) {
        rec = w4_db_alloc_record(svc);
    }
    if (!rec) {
        return -1;
    }
    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = record_kind;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    rec->placement_node = owner_node;
    rec->placement_level = 2U;
    rec->hot_segment_id = offset;
    rec->state = W4_KVCACHE_STATE_HOT;
    rec->version = 1U;
    rec->last_result_segment = offset + len;
    rec->object_owner_node = owner_node;
    rec->object_payload_kind = payload_kind;
    rec->object_backing_offset = offset;
    rec->object_backing_len = len;
    rec->object_payload_checksum = checksum;
    if (resolved_out) {
        memcpy(resolved_out, rec, sizeof(*resolved_out));
    }
    return 0;
}

static bool w4_db_try_read_stable_payload(const struct w4_db_cluster_payload *payload,
                                          struct w4_db_cluster_payload *snapshot)
{
    if (!payload || !snapshot) {
        return false;
    }
    {
        struct w4_db_cluster_payload_header header;
        uint16_t i;

        __sync_synchronize();
        header.magic = payload->magic;
        header.version = payload->version;
        header.record_count = payload->record_count;
        header.publish_seq = payload->publish_seq;
        header.publish_done_seq = payload->publish_done_seq;
        if (header.publish_seq == 0 ||
            header.publish_seq != header.publish_done_seq ||
            header.magic != W4_DB_CLUSTER_PAYLOAD_MAGIC ||
            header.version != W4_DB_CLUSTER_VERSION ||
            header.record_count == 0 ||
            header.record_count > W4_DB_CLUSTER_MAX_RECORDS) {
            return false;
        }
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->magic = header.magic;
        snapshot->version = header.version;
        snapshot->record_count = header.record_count;
        snapshot->publish_seq = header.publish_seq;
        snapshot->publish_done_seq = header.publish_done_seq;
        for (i = 0; i < header.record_count; ++i) {
            memcpy(&snapshot->records[i], &payload->records[i], sizeof(snapshot->records[i]));
        }
        __sync_synchronize();
        if (snapshot->publish_seq == snapshot->publish_done_seq &&
            snapshot->publish_seq == header.publish_seq &&
            snapshot->publish_done_seq == header.publish_done_seq &&
            snapshot->magic == W4_DB_CLUSTER_PAYLOAD_MAGIC &&
            snapshot->version == W4_DB_CLUSTER_VERSION &&
            snapshot->record_count == header.record_count) {
            return true;
        }
    }
    return false;
}

static bool w4_db_read_stable_payload(const struct w4_db_cluster_payload *payload,
                                      struct w4_db_cluster_payload *snapshot)
{
    int attempts = 8;

    while (attempts-- > 0) {
        if (w4_db_try_read_stable_payload(payload, snapshot)) {
            return true;
        }
        usleep(10000);
    }
    return false;
}

static void w4_db_copy_from_mapped_volatile(void *dst,
                                            const volatile uint8_t *src,
                                            size_t len)
{
    size_t i = 0;
    uint8_t *out = (uint8_t *)dst;

    for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
        uint64_t word = *(const volatile uint64_t *)(src + i);
        memcpy(out + i, &word, sizeof(word));
    }
    for (; i < len; ++i) {
        out[i] = src[i];
    }
}

static bool w4_db_try_read_stable_payload_region(const struct w4_db_cluster_slot *slot,
                                                 struct w4_db_cluster_payload *snapshot,
                                                 struct w4_db_cluster_payload_header *seen_out)
{
    struct w4_db_cluster_payload_header header;
    struct w4_db_cluster_payload_header confirm;
    uint16_t i;
    const volatile uint8_t *mapped_bytes;

    mapped_bytes = slot ? (const volatile uint8_t *)slot->region.addr : NULL;

    if (!slot || !snapshot) {
        return false;
    }
    if (slot->is_local) {
        bool ok = w4_db_try_read_stable_payload((const struct w4_db_cluster_payload *)slot->region.addr,
                                                snapshot);
        if (ok && seen_out) {
            seen_out->magic = snapshot->magic;
            seen_out->version = snapshot->version;
            seen_out->record_count = snapshot->record_count;
            seen_out->publish_seq = snapshot->publish_seq;
            seen_out->publish_done_seq = snapshot->publish_done_seq;
        }
        return ok;
    }
    if (slot->region.fd < 0) {
        return false;
    }
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=read_header_begin mem_id=%" PRIu64 " map_osync=%d addr=%p\n",
           slot->owner_idx + 1,
           g_w4_db_cluster_runtime.local_idx + 1,
           slot->mem_id,
           slot->map_osync ? 1 : 0,
           slot->region.addr);
    fflush(stdout);
    w4_db_copy_from_mapped_volatile(&header, mapped_bytes, sizeof(header));
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=read_header_done seq=%u done=%u count=%u\n",
           slot->owner_idx + 1,
           header.publish_seq,
           header.publish_done_seq,
           header.record_count);
    fflush(stdout);
    if (seen_out) {
        *seen_out = header;
    }
    if (header.publish_seq == 0 ||
        header.publish_seq != header.publish_done_seq ||
        header.magic != W4_DB_CLUSTER_PAYLOAD_MAGIC ||
        header.version != W4_DB_CLUSTER_VERSION ||
        header.record_count == 0 ||
        header.record_count > W4_DB_CLUSTER_MAX_RECORDS) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->magic = header.magic;
    snapshot->version = header.version;
    snapshot->record_count = header.record_count;
    snapshot->publish_seq = header.publish_seq;
    snapshot->publish_done_seq = header.publish_done_seq;
    for (i = 0; i < header.record_count; ++i) {
        size_t record_off = offsetof(struct w4_db_cluster_payload, records) +
                            ((size_t)i * sizeof(snapshot->records[0]));
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=record_copy_begin record=%u offset=%zu bytes=%zu\n",
               slot->owner_idx + 1,
               g_w4_db_cluster_runtime.local_idx + 1,
               i,
               record_off,
               sizeof(snapshot->records[i]));
        fflush(stdout);
        w4_db_copy_from_mapped_volatile(&snapshot->records[i],
                                        mapped_bytes + record_off,
                                        sizeof(snapshot->records[i]));
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=record_copy_done record=%u offset=%zu bytes=%zu\n",
               slot->owner_idx + 1,
               g_w4_db_cluster_runtime.local_idx + 1,
               i,
               record_off,
               sizeof(snapshot->records[i]));
        fflush(stdout);
    }
    __sync_synchronize();
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=confirm_header_begin\n",
           slot->owner_idx + 1,
           g_w4_db_cluster_runtime.local_idx + 1);
    fflush(stdout);
    w4_db_copy_from_mapped_volatile(&confirm, mapped_bytes, sizeof(confirm));
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=confirm_header_done seq=%u done=%u count=%u\n",
           slot->owner_idx + 1,
           g_w4_db_cluster_runtime.local_idx + 1,
           confirm.publish_seq,
           confirm.publish_done_seq,
           confirm.record_count);
    fflush(stdout);
    if (confirm.publish_seq != header.publish_seq ||
        confirm.publish_done_seq != header.publish_done_seq ||
        confirm.magic != header.magic ||
        confirm.version != header.version ||
        confirm.record_count != header.record_count) {
        return false;
    }
    return true;
}

static bool w4_db_read_stable_payload_region(const struct w4_db_cluster_slot *slot,
                                             struct w4_db_cluster_payload *snapshot,
                                             struct w4_db_cluster_payload_header *seen_out)
{
    int attempts = 8;
    unsigned int relax_attempt = 0;

    while (attempts-- > 0) {
        if (w4_db_try_read_stable_payload_region(slot, snapshot, seen_out)) {
            return true;
        }
        w4_db_cpu_relax_wait(&relax_attempt);
    }
    return false;
}

static bool w4_db_wait_stable_payload_region_at_least(
    const struct w4_db_cluster_slot *slot,
    uint32_t min_publish_done_seq,
    long timeout_ms,
    struct w4_db_cluster_payload *snapshot,
    struct w4_db_cluster_payload_header *seen_out)
{
    long deadline;
    unsigned int relax_attempt = 0;
    struct w4_db_cluster_payload local_snapshot;
    struct w4_db_cluster_payload_header local_seen;

    if (!slot || !snapshot) {
        return false;
    }
    deadline = w4_db_now_ms() + timeout_ms;
    while (w4_db_now_ms() < deadline) {
        memset(&local_snapshot, 0, sizeof(local_snapshot));
        memset(&local_seen, 0, sizeof(local_seen));
        if (w4_db_try_read_stable_payload_region(slot, &local_snapshot, &local_seen)) {
            if (seen_out) {
                *seen_out = local_seen;
            }
            if (local_snapshot.publish_done_seq >= min_publish_done_seq) {
                *snapshot = local_snapshot;
                return true;
            }
        } else if (seen_out) {
            *seen_out = local_seen;
        }
        w4_db_cpu_relax_wait(&relax_attempt);
    }
    return false;
}

static bool w4_db_payload_find_record(const struct w4_db_cluster_payload *payload,
                                      const char *key,
                                      struct w4_db_record *resolved_out)
{
    struct w4_db_cluster_payload snapshot;
    uint16_t i;

    if (!payload || !key || !resolved_out) {
        return false;
    }
    if (!w4_db_read_stable_payload(payload, &snapshot)) {
        return false;
    }
    for (i = 0; i < snapshot.record_count; ++i) {
        if (!snapshot.records[i].in_use) {
            continue;
        }
        if (strncmp(snapshot.records[i].key, key, sizeof(snapshot.records[i].key)) == 0) {
            *resolved_out = snapshot.records[i];
            return true;
        }
    }
    return false;
}

static bool w4_db_payload_snapshot_find_record(const struct w4_db_cluster_payload *snapshot,
                                               const char *key,
                                               struct w4_db_record *resolved_out)
{
    uint16_t i;

    if (!snapshot || !key || !resolved_out) {
        return false;
    }
    for (i = 0; i < snapshot->record_count; ++i) {
        if (!snapshot->records[i].in_use) {
            continue;
        }
        if (strncmp(snapshot->records[i].key, key, sizeof(snapshot->records[i].key)) == 0) {
            *resolved_out = snapshot->records[i];
            return true;
        }
    }
    return false;
}

static bool w4_db_slot_find_record(const struct w4_db_cluster_slot *slot,
                                   const char *key,
                                   struct w4_db_record *resolved_out)
{
    struct w4_db_cluster_payload snapshot;
    uint16_t i;

    if (!slot || !key || !resolved_out) {
        return false;
    }
    if (!w4_db_read_stable_payload_region(slot, &snapshot, NULL)) {
        return false;
    }
    for (i = 0; i < snapshot.record_count; ++i) {
        if (!snapshot.records[i].in_use) {
            continue;
        }
        if (strncmp(snapshot.records[i].key, key, sizeof(snapshot.records[i].key)) == 0) {
            *resolved_out = snapshot.records[i];
            return true;
        }
    }
    return false;
}

static int w4_db_read_primary_cna(uint32_t *local_cna_out)
{
    uint64_t local_cna_u64 = 0;

    if (!local_cna_out) {
        return -1;
    }
    if (!w4_db_parse_hex_file_u64("/sys/bus/ub/devices/00001/primary_cna", &local_cna_u64)) {
        return -1;
    }
    *local_cna_out = (uint32_t)local_cna_u64;
    return 0;
}

static int w4_db_exchange_cluster_meta(int sockfd,
                                       struct sockaddr_in peers[W4_DB_CLUSTER_MAX_NODES],
                                       int node_count,
                                       int local_idx,
                                       const struct w4_db_cluster_meta *local_meta,
                                       struct w4_db_cluster_meta metas[W4_DB_CLUSTER_MAX_NODES],
                                       bool got_meta[W4_DB_CLUSTER_MAX_NODES])
{
    struct w4_db_cluster_msg msg;
    long deadline = w4_db_now_ms() + W4_DB_CLUSTER_WAIT_MS;

    metas[local_idx] = *local_meta;
    got_meta[local_idx] = true;
    while (w4_db_now_ms() < deadline) {
        bool all = true;
        struct sockaddr_in from;
        struct w4_db_cluster_msg rx;
        int i;

        for (i = 0; i < node_count; ++i) {
            if (!got_meta[i]) {
                all = false;
                break;
            }
        }
        if (all) {
            return 0;
        }
        for (i = 0; i < node_count; ++i) {
            if (i == local_idx) {
                continue;
            }
            w4_db_init_cluster_msg(&msg, W4_DB_CLUSTER_MSG_HELLO, local_idx, i);
            msg.meta = *local_meta;
            (void)w4_db_send_msg(sockfd, &peers[i], &msg, sizeof(msg));
        }
        while (w4_db_recv_msg(sockfd, &rx, sizeof(rx), &from) == (ssize_t)sizeof(rx)) {
            if (rx.magic != W4_DB_CLUSTER_MAGIC || rx.version != W4_DB_CLUSTER_VERSION) {
                continue;
            }
            if ((rx.type == W4_DB_CLUSTER_MSG_HELLO ||
                 rx.type == W4_DB_CLUSTER_MSG_READY) &&
                rx.src_idx < (uint16_t)node_count) {
                metas[rx.src_idx] = rx.meta;
                got_meta[rx.src_idx] = true;
            }
        }
        usleep(100000);
    }
    return -1;
}

static int w4_db_import_cluster_peers(int obmm_fd,
                                      uint32_t local_cna,
                                      int node_count,
                                      int local_idx,
                                      const struct w4_db_cluster_meta metas[W4_DB_CLUSTER_MAX_NODES],
                                      struct w4_db_cluster_slot slots[W4_DB_CLUSTER_MAX_NODES])
{
    uint64_t import_pas[W4_DB_CLUSTER_MAX_NODES];
    bool import_osync[W4_DB_CLUSTER_MAX_NODES];
    int import_count = node_count - 1;
    int import_idx = 0;
    int i;

    if (!w4_db_allocate_import_pas(import_count,
                                   W4_DB_CLUSTER_REGION_SIZE,
                                   import_pas,
                                   import_osync)) {
        printf("[w4_guest] gap db_service_cluster_stage=import_alloc_failed count=%d size=0x%016" PRIx64 "\n",
               import_count,
               (uint64_t)W4_DB_CLUSTER_REGION_SIZE);
        return -1;
    }
    for (i = 0; i < node_count; ++i) {
        if (i == local_idx) {
            continue;
        }
        slots[i].owner_idx = i;
        slots[i].is_local = false;
        slots[i].local_pa = import_pas[import_idx];
        slots[i].map_osync = true;
        fprintf(stderr,
                "[w4_guest] remote_slot_map_osync_forced node=%d map_osync=%d\n",
                i + 1,
                slots[i].map_osync ? 1 : 0);
        slots[i].export_cna = metas[i].export_cna;
        import_idx += 1;
        slots[i].mem_id = 0;
        memset(&slots[i].region, 0, sizeof(slots[i].region));
        slots[i].region.fd = -1;
    }
    return 0;
}

static int w4_db_wait_until_cluster_barrier(int sockfd,
                                            struct sockaddr_in peers[W4_DB_CLUSTER_MAX_NODES],
                                            int node_count,
                                            int local_idx,
                                            uint16_t msg_type,
                                            uint16_t epoch,
                                            uint16_t local_publish_seq,
                                            const struct w4_db_cluster_meta metas[W4_DB_CLUSTER_MAX_NODES],
                                            uint16_t ready_seq[W4_DB_CLUSTER_MAX_NODES],
                                            const char *gap_stage)
{
    bool got_ready[W4_DB_CLUSTER_MAX_NODES] = { false };
    struct w4_db_cluster_msg msg;
    long deadline = w4_db_now_ms() + W4_DB_CLUSTER_WAIT_MS;

    got_ready[local_idx] = true;
    ready_seq[local_idx] = local_publish_seq;
    while (w4_db_now_ms() < deadline) {
        bool all = true;
        struct sockaddr_in from;
        struct w4_db_cluster_msg rx;
        int i;

        for (i = 0; i < node_count; ++i) {
            if (!got_ready[i]) {
                all = false;
                break;
            }
        }
        if (all) {
            for (i = 0; i < 3; ++i) {
                int j;

                for (j = 0; j < node_count; ++j) {
                    if (j == local_idx) {
                        continue;
                    }
                    w4_db_init_cluster_msg(&msg, msg_type, local_idx, j);
                    msg.reserved0 = epoch;
                    msg.reserved1 = local_publish_seq;
                    msg.meta = metas[local_idx];
                    (void)w4_db_send_msg(sockfd, &peers[j], &msg, sizeof(msg));
                }
                usleep(20000);
            }
            return 0;
        }
        for (i = 0; i < node_count; ++i) {
            if (i == local_idx) {
                continue;
            }
            w4_db_init_cluster_msg(&msg, msg_type, local_idx, i);
            msg.reserved0 = epoch;
            msg.reserved1 = local_publish_seq;
            msg.meta = metas[local_idx];
            (void)w4_db_send_msg(sockfd, &peers[i], &msg, sizeof(msg));
        }
        {
            int recv_budget = 256;

            while (recv_budget-- > 0 &&
                   w4_db_recv_msg(sockfd, &rx, sizeof(rx), &from) == (ssize_t)sizeof(rx)) {
            if (rx.magic != W4_DB_CLUSTER_MAGIC || rx.version != W4_DB_CLUSTER_VERSION) {
                continue;
            }
            if (rx.type == msg_type &&
                rx.reserved0 == epoch &&
                rx.reserved1 != 0 &&
                rx.src_idx < (uint16_t)node_count &&
                rx.meta.export_mem_id == metas[rx.src_idx].export_mem_id &&
                rx.meta.export_cna == metas[rx.src_idx].export_cna &&
                rx.meta.remote_uba == metas[rx.src_idx].remote_uba &&
                rx.meta.token_id == metas[rx.src_idx].token_id &&
                rx.src_idx < (uint16_t)node_count) {
                got_ready[rx.src_idx] = true;
                ready_seq[rx.src_idx] = rx.reserved1;
            }
            }
        }
        usleep(100000);
    }
    {
        int i;

        for (i = 0; i < node_count; ++i) {
            if (i == local_idx) {
                continue;
            }
            if (!got_ready[i]) {
                printf("[w4_guest] gap db_service_cluster_stage=%s owner=node%d epoch=%u expected_seq=%u\n",
                       gap_stage,
                       i + 1,
                       epoch,
                       ready_seq[i]);
            }
        }
    }
    return -1;
}

static void w4_db_broadcast_cluster_msg(int sockfd,
                                        struct sockaddr_in peers[W4_DB_CLUSTER_MAX_NODES],
                                        int node_count,
                                        int local_idx,
                                        uint16_t msg_type,
                                        uint16_t epoch,
                                        uint16_t seq,
                                        const struct w4_db_cluster_meta metas[W4_DB_CLUSTER_MAX_NODES])
{
    struct w4_db_cluster_msg msg;
    int i;

    for (i = 0; i < node_count; ++i) {
        if (i == local_idx) {
            continue;
        }
        w4_db_init_cluster_msg(&msg, msg_type, local_idx, i);
        msg.reserved0 = epoch;
        msg.reserved1 = seq;
        msg.meta = metas[local_idx];
        (void)w4_db_send_msg(sockfd, &peers[i], &msg, sizeof(msg));
    }
}

static void w4_db_announce_cluster_msg(int sockfd,
                                       struct sockaddr_in peers[W4_DB_CLUSTER_MAX_NODES],
                                       int node_count,
                                       int local_idx,
                                       uint16_t msg_type,
                                       uint16_t epoch,
                                       uint16_t seq,
                                       const struct w4_db_cluster_meta metas[W4_DB_CLUSTER_MAX_NODES])
{
    int i;

    for (i = 0; i < 5; ++i) {
        w4_db_broadcast_cluster_msg(sockfd, peers, node_count, local_idx,
                                    msg_type, epoch, seq, metas);
        usleep(20000);
    }
}

static int w4_db_wait_for_target_ready(int sockfd,
                                       struct sockaddr_in peers[W4_DB_CLUSTER_MAX_NODES],
                                       int node_count,
                                       int local_idx,
                                       int target_idx,
                                       uint16_t epoch,
                                       uint16_t local_publish_seq,
                                       uint16_t expected_target_seq,
                                       const struct w4_db_cluster_meta metas[W4_DB_CLUSTER_MAX_NODES],
                                       uint16_t *target_seq_out)
{
    long deadline = w4_db_now_ms() + W4_DB_CLUSTER_WAIT_MS;
    uint16_t last_target_seq = 0;

    if (target_idx == local_idx) {
        if (target_seq_out) {
            *target_seq_out = local_publish_seq;
        }
        return 0;
    }
    while (w4_db_now_ms() < deadline) {
        struct sockaddr_in from;
        struct w4_db_cluster_msg rx;

        while (w4_db_recv_msg(sockfd, &rx, sizeof(rx), &from) == (ssize_t)sizeof(rx)) {
            if (rx.magic != W4_DB_CLUSTER_MAGIC || rx.version != W4_DB_CLUSTER_VERSION) {
                continue;
            }
            if (rx.type == W4_DB_CLUSTER_MSG_READY &&
                rx.reserved0 == epoch &&
                rx.reserved1 >= expected_target_seq &&
                rx.src_idx == (uint16_t)target_idx &&
                rx.src_idx < (uint16_t)node_count &&
                rx.meta.export_mem_id == metas[rx.src_idx].export_mem_id &&
                rx.meta.export_cna == metas[rx.src_idx].export_cna &&
                rx.meta.remote_uba == metas[rx.src_idx].remote_uba &&
                rx.meta.token_id == metas[rx.src_idx].token_id) {
                if (target_seq_out) {
                    *target_seq_out = rx.reserved1;
                }
                return 0;
            }
            if (rx.type == W4_DB_CLUSTER_MSG_READY &&
                rx.src_idx == (uint16_t)target_idx &&
                rx.reserved1 > last_target_seq) {
                last_target_seq = rx.reserved1;
            }
        }
        usleep(100000);
    }
    printf("[w4_guest] gap db_service_cluster_stage=target_ready_timeout target=node%d epoch=%u expected_seq=%u last_seq=%u\n",
           target_idx + 1,
           epoch,
           expected_target_seq,
           last_target_seq);
    return -1;
}

static int w4_db_wait_for_reader_done(int sockfd,
                                      struct sockaddr_in peers[W4_DB_CLUSTER_MAX_NODES],
                                      int node_count,
                                      int local_idx,
                                      int reader_idx,
                                      uint16_t epoch,
                                      uint16_t seq,
                                      const struct w4_db_cluster_meta metas[W4_DB_CLUSTER_MAX_NODES])
{
    long deadline = w4_db_now_ms() + W4_DB_CLUSTER_WAIT_MS;

    if (reader_idx == local_idx) {
        return 0;
    }
    while (w4_db_now_ms() < deadline) {
        struct sockaddr_in from;
        struct w4_db_cluster_msg msg;
        struct w4_db_cluster_msg rx;

        if (reader_idx >= 0 && reader_idx < node_count) {
            w4_db_init_cluster_msg(&msg, W4_DB_CLUSTER_MSG_READY, local_idx, reader_idx);
            msg.reserved0 = epoch;
            msg.reserved1 = seq;
            msg.meta = metas[local_idx];
            (void)w4_db_send_msg(sockfd, &peers[reader_idx], &msg, sizeof(msg));
        }

        while (w4_db_recv_msg(sockfd, &rx, sizeof(rx), &from) == (ssize_t)sizeof(rx)) {
            if (rx.magic != W4_DB_CLUSTER_MAGIC || rx.version != W4_DB_CLUSTER_VERSION) {
                continue;
            }
            if (rx.type == W4_DB_CLUSTER_MSG_OBSERVED &&
                rx.reserved0 == epoch &&
                rx.reserved1 == seq &&
                rx.src_idx == (uint16_t)reader_idx &&
                rx.src_idx < (uint16_t)node_count &&
                rx.meta.export_mem_id == metas[rx.src_idx].export_mem_id &&
                rx.meta.export_cna == metas[rx.src_idx].export_cna &&
                rx.meta.remote_uba == metas[rx.src_idx].remote_uba &&
                rx.meta.token_id == metas[rx.src_idx].token_id) {
                return 0;
            }
        }
        usleep(100000);
    }
    printf("[w4_guest] gap db_service_cluster_stage=reader_done_timeout reader=node%d epoch=%u\n",
           reader_idx + 1,
           epoch);
    return -1;
}

static void w4_db_cleanup_cluster_slots(int obmm_fd,
                                        int node_count,
                                        int local_idx,
                                        struct w4_db_cluster_slot slots[W4_DB_CLUSTER_MAX_NODES])
{
    int i;

    for (i = 0; i < node_count; ++i) {
        if (slots[i].region.addr || slots[i].region.fd >= 0) {
            w4_db_unmap_region_device(&slots[i].region);
        }
        if (slots[i].mem_id != 0) {
            if (i == local_idx) {
                (void)w4_db_do_unexport_region(obmm_fd, slots[i].mem_id);
            } else {
                (void)w4_db_do_unimport_region(obmm_fd, slots[i].mem_id);
            }
        }
    }
}

static int w4_db_activate_remote_slot(struct w4_db_cluster_runtime *rt, int owner_idx)
{
    struct w4_db_cluster_slot *slot;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count || owner_idx == rt->local_idx) {
        return -1;
    }

    slot = &rt->slots[owner_idx];
    if (!slot->map_osync) {
        fprintf(stderr,
                "[w4_guest] invariant violation remote_slot_map_osync_true_expected node=%d map_osync=%d\n",
                owner_idx + 1,
                slot->map_osync ? 1 : 0);
        slot->map_osync = true;
    }
    if (slot->region.addr && slot->mem_id != 0) {
        return 0;
    }
    if (slot->mem_id != 0) {
        (void)w4_db_do_unimport_region(rt->obmm_fd, slot->mem_id);
        slot->mem_id = 0;
    }
    if (slot->region.addr || slot->region.fd >= 0) {
        w4_db_unmap_region_device(&slot->region);
    }
    if (w4_db_do_import_region(rt->obmm_fd,
                               &rt->metas[owner_idx],
                               rt->local_cna,
                               slot->local_pa,
                               &slot->mem_id) != 0) {
        return -1;
    }
    if (w4_db_map_region_device(slot->mem_id,
                                W4_DB_CLUSTER_REGION_SIZE,
                                slot->map_osync,
                                &slot->region) != 0) {
        (void)w4_db_do_unimport_region(rt->obmm_fd, slot->mem_id);
        slot->mem_id = 0;
        return -1;
    }
    return 0;
}

static void w4_db_cluster_runtime_reset(struct w4_db_cluster_runtime *rt)
{
    if (!rt) {
        return;
    }
    if (rt->obmm_fd >= 0) {
        w4_db_cleanup_cluster_slots(rt->obmm_fd, rt->node_count, rt->local_idx, rt->slots);
        close(rt->obmm_fd);
    }
    if (rt->sockfd >= 0) {
        close(rt->sockfd);
    }
    memset(rt, 0, sizeof(*rt));
    rt->sockfd = -1;
    rt->obmm_fd = -1;
    rt->local_idx = -1;
}

static int w4_db_cluster_runtime_init(struct w4_db_cluster_runtime *rt)
{
    char local_ip[INET_ADDRSTRLEN];
    char ips[W4_DB_CLUSTER_MAX_NODES][INET_ADDRSTRLEN];
    char ifname[IFNAMSIZ];
    struct w4_db_cluster_meta local_meta;
    struct in_addr peer_addr;
    bool got_meta[W4_DB_CLUSTER_MAX_NODES] = { false };
    int i;

    if (!rt) {
        return -1;
    }
    if (rt->active) {
        return 0;
    }
    w4_db_cluster_runtime_reset(rt);
    memset(&local_meta, 0, sizeof(local_meta));

    if (!w4_db_resolve_cluster_nodes(local_ip, ips, &rt->node_count, &rt->local_idx)) {
        return -1;
    }
    if (!w4_db_find_ipourma_iface(ifname, sizeof(ifname))) {
        return -1;
    }
    for (i = 0; i < rt->node_count; ++i) {
        if (i == rt->local_idx) {
            continue;
        }
        memset(&rt->peers[i], 0, sizeof(rt->peers[i]));
        rt->peers[i].sin_family = AF_INET;
        rt->peers[i].sin_port = htons(W4_DB_CLUSTER_PORT);
        inet_pton(AF_INET, ips[i], &rt->peers[i].sin_addr);
        peer_addr = rt->peers[i].sin_addr;
        w4_db_install_static_arp(ifname, &peer_addr);
    }
    rt->sockfd = w4_db_create_udp_socket(ifname);
    if (rt->sockfd < 0) {
        goto fail;
    }
    rt->obmm_fd = w4_db_open_obmm();
    if (rt->obmm_fd < 0) {
        goto fail;
    }
    if (w4_db_read_primary_cna(&rt->local_cna) != 0) {
        goto fail;
    }
    local_meta.export_cna = rt->local_cna;
    if (w4_db_do_export_region(rt->obmm_fd, &local_meta) != 0) {
        goto fail;
    }
    rt->slots[rt->local_idx].owner_idx = rt->local_idx;
    rt->slots[rt->local_idx].is_local = true;
    rt->slots[rt->local_idx].mem_id = local_meta.export_mem_id;
    rt->slots[rt->local_idx].export_cna = rt->local_cna;
    if (w4_db_map_region_device(local_meta.export_mem_id,
                                W4_DB_CLUSTER_REGION_SIZE,
                                false,
                                &rt->slots[rt->local_idx].region) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=map_local_failed mem_id=%" PRIu64 "\n",
               local_meta.export_mem_id);
        goto fail;
    }
    if (w4_db_exchange_cluster_meta(rt->sockfd,
                                    rt->peers,
                                    rt->node_count,
                                    rt->local_idx,
                                    &local_meta,
                                    rt->metas,
                                    got_meta) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=hello_timeout\n");
        goto fail;
    }
    if (w4_db_import_cluster_peers(rt->obmm_fd,
                                   rt->local_cna,
                                   rt->node_count,
                                   rt->local_idx,
                                   rt->metas,
                                   rt->slots) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=import_failed\n");
        goto fail;
    }
    rt->active = true;
    return 0;

fail:
    w4_db_cluster_runtime_reset(rt);
    return -1;
}

static struct w4_db_record *w4_db_alloc_record(struct w4_db_service *svc)
{
    size_t i;

    if (!svc) {
        return NULL;
    }
    for (i = 0; i < W4_DB_MAX_RECORDS; ++i) {
        if (!svc->records[i].in_use) {
            svc->records[i].in_use = true;
            svc->record_count += 1;
            return &svc->records[i];
        }
    }
    return NULL;
}

static struct w4_db_record *w4_db_find_record(struct w4_db_service *svc, const char *key)
{
    size_t i;

    if (!svc || !key) {
        return NULL;
    }
    for (i = 0; i < W4_DB_MAX_RECORDS; ++i) {
        if (!svc->records[i].in_use) {
            continue;
        }
        if (strncmp(svc->records[i].key, key, sizeof(svc->records[i].key)) == 0) {
            return &svc->records[i];
        }
    }
    return NULL;
}

static bool w4_db_record_has_member(const struct w4_db_record *rec, const char *block_hash)
{
    uint32_t i;
    if (!rec || !block_hash) {
        return false;
    }
    for (i = 0; i < rec->member_count && i < W4_DB_MAX_GROUP_MEMBERS; ++i) {
        if (strncmp(rec->member_block_hashes[i], block_hash, sizeof(rec->member_block_hashes[i])) == 0) {
            return true;
        }
    }
    return false;
}

bool w4_db_record_has_member_block(const struct w4_db_record *rec, const char *block_hash)
{
    return w4_db_record_has_member(rec, block_hash);
}

static int w4_db_add_member(struct w4_db_record *rec, const char *block_hash)
{
    if (!rec || !block_hash) {
        return -1;
    }
    if (w4_db_record_has_member(rec, block_hash)) {
        return 0;
    }
    if (rec->member_count >= W4_DB_MAX_GROUP_MEMBERS) {
        return -1;
    }
    snprintf(rec->member_block_hashes[rec->member_count], sizeof(rec->member_block_hashes[rec->member_count]), "%s", block_hash);
    rec->member_count += 1;
    return 0;
}

static void w4_db_build_group_key(const struct w4_db_block_ctx *ctx,
                                  char *out,
                                  size_t out_len)
{
    snprintf(out, out_len, "request/%s/prefix-group/%s", ctx->request_id, ctx->group_id);
}

void w4_db_build_prefix_key_from_parts(const char *request_id,
                                       const char *prefix_group,
                                       char *out,
                                       size_t out_len)
{
    snprintf(out, out_len, "request/%s/prefix/%s", request_id, prefix_group);
}

void w4_db_build_group_key_from_parts(const char *request_id,
                                      const char *group_id,
                                      char *out,
                                      size_t out_len)
{
    snprintf(out, out_len, "request/%s/prefix-group/%s", request_id, group_id);
}

void w4_db_build_block_key_from_hash(const char *block_hash, char *out, size_t out_len)
{
    snprintf(out, out_len, "block/%s", block_hash);
}

static int w4_db_put_request_prefix(struct w4_db_service *svc,
                                    const char *key,
                                    const char *request_id,
                                    const char *prefix_group,
                                    const char *group_id,
                                    const char *block_hash)
{
    struct w4_db_record *rec;

    rec = w4_db_find_record(svc, key);
    if (!rec) {
        rec = w4_db_alloc_record(svc);
    }
    if (!rec) {
        return -1;
    }

    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = W4_DB_RECORD_REQUEST_PREFIX;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    snprintf(rec->request_id, sizeof(rec->request_id), "%s", request_id);
    snprintf(rec->prefix_group, sizeof(rec->prefix_group), "%s", prefix_group);
    snprintf(rec->group_id, sizeof(rec->group_id), "%s", group_id);
    snprintf(rec->block_hash, sizeof(rec->block_hash), "%s", block_hash);
    rec->version = 1;
    return 0;
}

static int w4_db_put_prefix_group(struct w4_db_service *svc,
                                  const char *key,
                                  const char *request_id,
                                  const char *group_id,
                                  const char *block_hash,
                                  uint32_t placement_node,
                                  uint32_t placement_level,
                                  uint64_t hot_segment_id,
                                  enum w4_kvcache_state state,
                                  uint64_t last_result_segment)
{
    struct w4_db_record *rec;
    bool is_new = false;
    bool changed = false;

    rec = w4_db_find_record(svc, key);
    if (!rec) {
        rec = w4_db_alloc_record(svc);
        if (!rec) {
            return -1;
        }
        memset(rec, 0, sizeof(*rec));
        rec->in_use = true;
        rec->kind = W4_DB_RECORD_PREFIX_GROUP;
        snprintf(rec->key, sizeof(rec->key), "%s", key);
        snprintf(rec->request_id, sizeof(rec->request_id), "%s", request_id);
        snprintf(rec->group_id, sizeof(rec->group_id), "%s", group_id);
        rec->version = 1;
        is_new = true;
        changed = true;
    }
    if (rec->kind != W4_DB_RECORD_PREFIX_GROUP) {
        return -1;
    }
    if (w4_db_add_member(rec, block_hash) != 0) {
        return -1;
    }
    if (rec->placement_node != placement_node ||
        rec->placement_level != placement_level ||
        rec->hot_segment_id != hot_segment_id ||
        rec->state != state ||
        rec->last_result_segment != last_result_segment) {
        rec->placement_node = placement_node;
        rec->placement_level = placement_level;
        rec->hot_segment_id = hot_segment_id;
        rec->state = state;
        rec->last_result_segment = last_result_segment;
        changed = true;
    }
    if (changed && !is_new && rec->version > 0) {
        rec->version += 1;
    }
    return 0;
}

static int w4_db_put_block_meta(struct w4_db_service *svc,
                                const char *key,
                                const char *request_id,
                                const char *prefix_group,
                                const char *group_id,
                                const char *block_hash,
                                uint32_t placement_node,
                                uint32_t placement_level,
                                uint64_t hot_segment_id,
                                enum w4_kvcache_state state)
{
    struct w4_db_record *rec;

    rec = w4_db_find_record(svc, key);
    if (!rec) {
        rec = w4_db_alloc_record(svc);
    }
    if (!rec) {
        return -1;
    }

    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = W4_DB_RECORD_BLOCK_META;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    snprintf(rec->request_id, sizeof(rec->request_id), "%s", request_id);
    snprintf(rec->prefix_group, sizeof(rec->prefix_group), "%s", prefix_group);
    snprintf(rec->group_id, sizeof(rec->group_id), "%s", group_id);
    snprintf(rec->block_hash, sizeof(rec->block_hash), "%s", block_hash);
    rec->placement_node = placement_node;
    rec->placement_level = placement_level;
    rec->hot_segment_id = hot_segment_id;
    rec->state = state;
    rec->version = 1;
    return 0;
}

static int w4_db_update_block_result(struct w4_db_service *svc,
                                     const char *key,
                                     uint64_t last_result_segment,
                                     enum w4_kvcache_state next_state)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || rec->kind != W4_DB_RECORD_BLOCK_META) {
        return -1;
    }
    if (rec->last_result_segment != 0 && last_result_segment <= rec->last_result_segment) {
        return 1;
    }
    rec->last_result_segment = last_result_segment;
    rec->state = next_state;
    rec->version += 1;
    return 0;
}

static int w4_db_update_prefix_result(struct w4_db_service *svc,
                                      const char *key,
                                      const struct w4_db_block_ctx *ctx,
                                      const struct w4_db_record *block_record)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || rec->kind != W4_DB_RECORD_REQUEST_PREFIX) {
        return -1;
    }
    if (rec->last_result_segment != 0 &&
        strncmp(rec->block_hash, block_record->block_hash, sizeof(rec->block_hash)) == 0 &&
        block_record->last_result_segment < rec->last_result_segment) {
        return 1;
    }
    if (rec->last_result_segment == block_record->last_result_segment &&
        rec->placement_node == ctx->placement_node &&
        rec->placement_level == ctx->placement_level &&
        rec->hot_segment_id == block_record->hot_segment_id &&
        rec->state == block_record->state &&
        strncmp(rec->block_hash, block_record->block_hash, sizeof(rec->block_hash)) == 0) {
        return 1;
    }
    rec->placement_node = block_record->placement_node;
    rec->placement_level = block_record->placement_level;
    rec->hot_segment_id = block_record->hot_segment_id;
    rec->state = block_record->state;
    rec->last_result_segment = block_record->last_result_segment;
    snprintf(rec->block_hash, sizeof(rec->block_hash), "%s", block_record->block_hash);
    rec->version += 1;
    return 0;
}

static int w4_db_update_prefix_group_from_block(struct w4_db_service *svc,
                                                const struct w4_db_block_ctx *ctx,
                                                const struct w4_db_record *block_record)
{
    char group_key[96];

    if (!svc || !ctx || !block_record) {
        return -1;
    }
    w4_db_build_group_key(ctx, group_key, sizeof(group_key));
    return w4_db_put_prefix_group(svc,
                                  group_key,
                                  ctx->request_id,
                                  ctx->group_id,
                                  block_record->block_hash,
                                  block_record->placement_node,
                                  block_record->placement_level,
                                  block_record->hot_segment_id,
                                  block_record->state,
                                  block_record->last_result_segment);
}

static int w4_db_update_block_view(struct w4_db_service *svc,
                                   const char *key,
                                   uint64_t hot_segment_id,
                                   uint32_t placement_level)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || rec->kind != W4_DB_RECORD_BLOCK_META) {
        return -1;
    }
    if (rec->hot_segment_id == hot_segment_id && rec->placement_level == placement_level) {
        return 1;
    }
    rec->hot_segment_id = hot_segment_id;
    rec->placement_level = placement_level;
    rec->version += 1;
    return 0;
}

static int w4_db_update_block_owner(struct w4_db_service *svc,
                                    const char *key,
                                    uint32_t placement_node,
                                    uint32_t placement_level,
                                    uint64_t hot_segment_id)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || rec->kind != W4_DB_RECORD_BLOCK_META) {
        return -1;
    }
    if (rec->placement_node == placement_node &&
        rec->placement_level == placement_level &&
        rec->hot_segment_id == hot_segment_id) {
        return 1;
    }
    rec->placement_node = placement_node;
    rec->placement_level = placement_level;
    rec->hot_segment_id = hot_segment_id;
    rec->version += 1;
    return 0;
}

static void w4_db_build_prefix_key(const struct w4_db_block_ctx *ctx,
                                   char *out,
                                   size_t out_len)
{
    w4_db_build_prefix_key_from_parts(ctx->request_id, ctx->prefix_group, out, out_len);
}

static void w4_db_build_block_key(const struct w4_db_block_ctx *ctx,
                                  char *out,
                                  size_t out_len)
{
    w4_db_build_block_key_from_hash(ctx->block_hash, out, out_len);
}

bool w4_db_prefix_matches_block_meta(const struct w4_db_record *prefix_meta,
                                     const struct w4_db_record *block_meta)
{
    if (!prefix_meta || !block_meta) {
        return false;
    }
    if (prefix_meta->kind != W4_DB_RECORD_REQUEST_PREFIX ||
        block_meta->kind != W4_DB_RECORD_BLOCK_META) {
        return false;
    }
    return prefix_meta->last_result_segment != 0 &&
           block_meta->last_result_segment != 0 &&
           strncmp(prefix_meta->block_hash, block_meta->block_hash,
                   sizeof(prefix_meta->block_hash)) == 0 &&
           prefix_meta->hot_segment_id == block_meta->hot_segment_id &&
           prefix_meta->placement_node == block_meta->placement_node &&
           prefix_meta->placement_level == block_meta->placement_level &&
           prefix_meta->state == block_meta->state;
}

bool w4_db_group_covers_blocks(const struct w4_db_record *group_meta,
                               const struct w4_db_record *primary_block_meta,
                               const struct w4_db_record *aux_block_meta)
{
    if (!group_meta || !primary_block_meta || !aux_block_meta) {
        return false;
    }
    if (group_meta->kind != W4_DB_RECORD_PREFIX_GROUP ||
        primary_block_meta->kind != W4_DB_RECORD_BLOCK_META ||
        aux_block_meta->kind != W4_DB_RECORD_BLOCK_META) {
        return false;
    }
    return group_meta->member_count >= 2 &&
           group_meta->last_result_segment != 0 &&
           w4_db_record_has_member(group_meta, primary_block_meta->block_hash) &&
           w4_db_record_has_member(group_meta, aux_block_meta->block_hash);
}

const char *w4_kvcache_state_name(enum w4_kvcache_state state)
{
    switch (state) {
    case W4_KVCACHE_STATE_MISSING:
        return "missing";
    case W4_KVCACHE_STATE_FILLED:
        return "filled";
    case W4_KVCACHE_STATE_HOT:
        return "hot";
    case W4_KVCACHE_STATE_RELOADED:
        return "reloaded";
    default:
        return "unknown";
    }
}

int w4_db_service_init(struct w4_db_service *svc,
                       bool shmem_ready,
                       bool urma_ready,
                       bool block_ready)
{
    if (!svc) {
        return -1;
    }
    memset(svc, 0, sizeof(*svc));
    svc->shmem_ready = shmem_ready;
    svc->urma_ready = urma_ready;
    svc->block_ready = block_ready;
    if (!svc->shmem_ready || !svc->urma_ready || !svc->block_ready) {
        return -1;
    }
    return 0;
}

int w4_db_get_record(struct w4_db_service *svc, const char *key, struct w4_db_record *out)
{
    struct w4_db_record *rec = w4_db_find_record(svc, key);

    if (!rec || !out) {
        return -1;
    }
    memcpy(out, rec, sizeof(*out));
    return 0;
}

int w4_db_bootstrap_kvcache(struct w4_db_service *svc,
                            const struct w4_db_block_ctx *ctx,
                            struct w4_db_record *resolved_out)
{
    char prefix_key[96];
    char group_key[96];
    char block_key[96];

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    w4_db_build_prefix_key(ctx, prefix_key, sizeof(prefix_key));
    w4_db_build_group_key(ctx, group_key, sizeof(group_key));
    w4_db_build_block_key(ctx, block_key, sizeof(block_key));

    if (w4_db_put_prefix_group(svc,
                               group_key,
                               ctx->request_id,
                               ctx->group_id,
                               ctx->block_hash,
                               ctx->placement_node,
                               ctx->placement_level,
                               ctx->hot_segment_id,
                               W4_KVCACHE_STATE_FILLED,
                               0) != 0) {
        return -1;
    }
    if (w4_db_put_request_prefix(svc,
                                 prefix_key,
                                 ctx->request_id,
                                 ctx->prefix_group,
                                 ctx->group_id,
                                 ctx->block_hash) != 0) {
        return -1;
    }
    printf("[w4_guest] stage db_service_bootstrap=request_prefix_ok key=%s request=%s prefix=%s block=%s\n",
           prefix_key,
           ctx->request_id,
           ctx->prefix_group,
           ctx->block_hash);

    if (w4_db_put_block_meta(svc,
                             block_key,
                             ctx->request_id,
                             ctx->prefix_group,
                             ctx->group_id,
                             ctx->block_hash,
                             ctx->placement_node,
                             ctx->placement_level,
                             ctx->hot_segment_id,
                             W4_KVCACHE_STATE_FILLED) != 0) {
        return -1;
    }
    printf("[w4_guest] stage db_service_bootstrap=block_meta_ok key=%s placement_node=%u placement_level=%u hot_segment=0x%016" PRIx64 " state=%s\n",
           block_key,
           ctx->placement_node,
           ctx->placement_level,
           ctx->hot_segment_id,
           w4_kvcache_state_name(W4_KVCACHE_STATE_FILLED));

    if (w4_db_update_block_result(svc,
                                  block_key,
                                  ctx->result_segment_id,
                                  W4_KVCACHE_STATE_HOT) != 0) {
        return -1;
    }
    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    printf("[w4_guest] stage db_service_bootstrap=result_update_ok key=%s result_segment=0x%016" PRIx64 " state=%s\n",
           block_key,
           ctx->result_segment_id,
           w4_kvcache_state_name(W4_KVCACHE_STATE_HOT));

    if (w4_db_update_prefix_result(svc,
                                   prefix_key,
                                   ctx,
                                   resolved_out) != 0) {
        return -1;
    }
    printf("[w4_guest] stage db_service_bootstrap=prefix_result_ok key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s result_segment=0x%016" PRIx64 " version=%" PRIu64 "\n",
           prefix_key,
           resolved_out->block_hash,
           resolved_out->hot_segment_id,
           w4_kvcache_state_name(resolved_out->state),
           resolved_out->last_result_segment,
           resolved_out->version);

    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int w4_db_update_prefix_metadata(struct w4_db_service *svc,
                                 const struct w4_db_block_ctx *ctx,
                                 const struct w4_db_record *block_record,
                                 struct w4_db_record *resolved_out)
{
    char prefix_key[96];
    int rc;

    if (!svc || !ctx || !block_record || !resolved_out) {
        return -1;
    }

    w4_db_build_prefix_key(ctx, prefix_key, sizeof(prefix_key));
    rc = w4_db_update_prefix_result(svc, prefix_key, ctx, block_record);
    if (rc != 0) {
        return rc;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, block_record) != 0) {
        return -1;
    }
    if (w4_db_get_record(svc, prefix_key, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int w4_db_get_prefix_group_metadata(struct w4_db_service *svc,
                                    const struct w4_db_block_ctx *ctx,
                                    struct w4_db_record *resolved_out)
{
    char group_key[96];

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }
    w4_db_build_group_key(ctx, group_key, sizeof(group_key));
    return w4_db_get_record(svc, group_key, resolved_out);
}

int w4_db_apply_block_result(struct w4_db_service *svc,
                             const struct w4_db_block_ctx *ctx,
                             uint64_t result_segment_id,
                             enum w4_kvcache_state next_state,
                             struct w4_db_record *resolved_out)
{
    char block_key[96];
    struct w4_db_record current;
    int rc;

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    w4_db_build_block_key(ctx, block_key, sizeof(block_key));
    if (w4_db_get_record(svc, block_key, &current) != 0) {
        return -1;
    }
    if (current.placement_node != ctx->placement_node) {
        return 2;
    }
    rc = w4_db_update_block_result(svc, block_key, result_segment_id, next_state);
    if (rc != 0) {
        return rc;
    }
    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int w4_db_rebind_block_view(struct w4_db_service *svc,
                            const struct w4_db_block_ctx *ctx,
                            uint64_t hot_segment_id,
                            uint32_t placement_level,
                            struct w4_db_record *resolved_out)
{
    char block_key[96];
    struct w4_db_record current;
    int rc;

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    w4_db_build_block_key(ctx, block_key, sizeof(block_key));
    if (w4_db_get_record(svc, block_key, &current) != 0) {
        return -1;
    }
    if (current.placement_node != ctx->placement_node) {
        return 2;
    }
    rc = w4_db_update_block_view(svc, block_key, hot_segment_id, placement_level);
    if (rc != 0) {
        return rc;
    }
    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int w4_db_handoff_block_owner(struct w4_db_service *svc,
                              const struct w4_db_block_ctx *ctx,
                              uint32_t placement_node,
                              uint32_t placement_level,
                              uint64_t hot_segment_id,
                              struct w4_db_record *resolved_out)
{
    char block_key[96];
    struct w4_db_record current;
    int rc;

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    w4_db_build_block_key(ctx, block_key, sizeof(block_key));
    if (w4_db_get_record(svc, block_key, &current) != 0) {
        return -1;
    }
    if (current.placement_node != ctx->placement_node) {
        return 2;
    }
    rc = w4_db_update_block_owner(svc,
                                  block_key,
                                  placement_node,
                                  placement_level,
                                  hot_segment_id);
    if (rc != 0) {
        return rc;
    }
    if (w4_db_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (w4_db_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

static void w4_db_reset_remote_slots_for_publish(struct w4_db_cluster_runtime *rt)
{
    (void)rt;
}

int w4_db_cluster_fetch_record(struct w4_db_service *svc,
                               const char *key,
                               struct w4_db_record *resolved_out)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    long deadline;
    int i;
    int rc = -1;

    if (!svc || !key || !resolved_out) {
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0) {
        return -1;
    }
    if (w4_db_write_cluster_payload(svc, &rt->slots[rt->local_idx]) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=write_local_payload_failed\n");
        return -1;
    }
    w4_db_reset_remote_slots_for_publish(rt);

    deadline = w4_db_now_ms() + W4_DB_CLUSTER_WAIT_MS;
    while (w4_db_now_ms() < deadline) {
        for (i = 0; i < rt->node_count; ++i) {
            if (!rt->slots[i].region.addr) {
                if (i != rt->local_idx && w4_db_activate_remote_slot(rt, i) != 0) {
                    continue;
                }
                if (!rt->slots[i].region.addr) {
                    continue;
                }
            }
            if (w4_db_slot_find_record(&rt->slots[i], key, resolved_out)) {
                rc = 0;
                break;
            }
        }
        if (rc == 0) {
            break;
        }
        usleep(10000);
    }
    if (rc != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=key_not_found key=%s\n", key);
    }
    return rc;
}

int w4_db_publish_observe_cluster(struct w4_db_service *svc,
                                  const struct w4_db_record *local_record,
                                  struct w4_db_cluster_summary *summary)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_cluster_payload snapshot;
    struct w4_db_cluster_payload peer_snapshots[W4_DB_CLUSTER_MAX_NODES];
    struct w4_db_cluster_payload_header seen_header;
    bool peer_ready[W4_DB_CLUSTER_MAX_NODES] = { false };
    uint16_t ready_seq[W4_DB_CLUSTER_MAX_NODES] = { 0 };
    uint16_t publish_ready_seq[W4_DB_CLUSTER_MAX_NODES] = { 0 };
    uint16_t local_publish_seq;
    uint16_t observed_seq;
    int i;
    int rc = -1;

    if (summary) {
        memset(summary, 0, sizeof(*summary));
    }
    if (!svc || !local_record || !summary) {
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0) {
        goto out;
    }

    summary->active = true;
    summary->placement_coherent = true;
    summary->state_coherent = true;
    summary->prefix_state_ready = true;
    summary->prefix_view_ready = true;
    summary->node_count = (uint32_t)rt->node_count;
    summary->local_version = local_record->version;
    summary->peer_version_floor = local_record->version;
    summary->peer_result_floor = local_record->last_result_segment;
    summary->peer_prefix_version_floor = 0;
    summary->peer_prefix_result_floor = 0;
    summary->peer_record_count_floor = 0;
    summary->peer_prefix_count_floor = 0;
    summary->peer_block_count_floor = 0;
    summary->peer_group_count_floor = 0;

    if (w4_db_write_cluster_payload(svc, &rt->slots[rt->local_idx]) != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=write_local_payload_failed\n");
        goto out;
    }
    w4_db_reset_remote_slots_for_publish(rt);
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=remote_slots_reset seq=%u\n",
           rt->local_idx + 1,
           rt->publish_seq);
    if (!w4_db_try_read_stable_payload_region(&rt->slots[rt->local_idx],
                                              &peer_snapshots[rt->local_idx],
                                              NULL)) {
        printf("[w4_guest] gap db_service_cluster_stage=read_local_payload_failed\n");
        goto out;
    }
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=read_local_payload_ok seq=%u\n",
           rt->local_idx + 1,
           rt->publish_seq);
    peer_ready[rt->local_idx] = true;
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    memset(ready_seq, 0, sizeof(ready_seq));
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    if (w4_db_wait_until_cluster_barrier(rt->sockfd,
                                         rt->peers,
                                         rt->node_count,
                                         rt->local_idx,
                                         W4_DB_CLUSTER_MSG_READY,
                                         rt->observe_epoch,
                                         local_publish_seq,
                                         rt->metas,
                                         ready_seq,
                                         "ready_missing") != 0) {
        printf("[w4_guest] gap db_service_cluster_stage=payload_ready_timeout epoch=%u\n",
               rt->observe_epoch);
        goto out;
    }
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=ready_barrier_ok epoch=%u seq=%u\n",
           rt->local_idx + 1,
           rt->observe_epoch,
           local_publish_seq);
    memcpy(publish_ready_seq, ready_seq, sizeof(publish_ready_seq));

    for (i = 0; i < rt->node_count; ++i) {
        uint16_t owner_publish_seq = publish_ready_seq[i];

        if (i == rt->local_idx) {
            peer_ready[i] = true;
            continue;
        }
        if (w4_db_activate_remote_slot(rt, i) != 0) {
            printf("[w4_guest] gap db_service_cluster_stage=activate_remote_failed owner=node%d reader=node%d\n",
                   i + 1,
                   rt->local_idx + 1);
            goto out;
        }
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=remote_payload_read_wait expect_seq=%u mem_id=%" PRIu64 " map_osync=%d addr=%p\n",
               i + 1,
               rt->local_idx + 1,
               owner_publish_seq,
               rt->slots[i].mem_id,
               rt->slots[i].map_osync ? 1 : 0,
               rt->slots[i].region.addr);
        if (!w4_db_try_read_stable_payload_region(&rt->slots[i],
                                                  &peer_snapshots[i],
                                                  NULL) ||
            peer_snapshots[i].publish_done_seq < owner_publish_seq) {
            memset(&seen_header, 0, sizeof(seen_header));
            if (w4_db_wait_stable_payload_region_at_least(&rt->slots[i],
                                                          owner_publish_seq,
                                                          W4_DB_CLUSTER_WAIT_MS,
                                                          &snapshot,
                                                          &seen_header)) {
                peer_snapshots[i] = snapshot;
            } else {
                printf("[w4_guest] gap db_service_cluster_stage=payload_not_ready owner=node%d reader=node%d expect_seq=%u seen_seq=%u seen_done=%u magic=0x%08x version=%u count=%u\n",
                       i + 1,
                       rt->local_idx + 1,
                       owner_publish_seq,
                       seen_header.publish_seq,
                       seen_header.publish_done_seq,
                       seen_header.magic,
                       seen_header.version,
                       seen_header.record_count);
                printf("[w4_guest] gap db_service_cluster_stage=payload_not_ready owner=node%d reader=node%d\n",
                       i + 1,
                       rt->local_idx + 1);
                goto out;
            }
        }
        printf("[w4_guest] stage db_service_cluster_debug owner=node%d reader=node%d step=remote_payload_read_ok seq=%u expect_seq=%u\n",
               i + 1,
               rt->local_idx + 1,
               peer_snapshots[i].publish_done_seq,
               owner_publish_seq);
        fflush(stdout);
        peer_ready[i] = true;
    }

    observed_seq = (uint16_t)(rt->local_idx + 1);
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=observe_announce_begin epoch=%u seq=%u\n",
           rt->local_idx + 1,
           rt->observe_epoch,
           observed_seq);
    fflush(stdout);
    w4_db_broadcast_cluster_msg(rt->sockfd,
                                rt->peers,
                                rt->node_count,
                                rt->local_idx,
                                W4_DB_CLUSTER_MSG_OBSERVED,
                                rt->observe_epoch,
                                observed_seq,
                                rt->metas);
    printf("[w4_guest] stage db_service_cluster_debug owner=node%d step=observe_announce_done epoch=%u seq=%u\n",
           rt->local_idx + 1,
           rt->observe_epoch,
           observed_seq);
    fflush(stdout);

    for (i = 0; i < rt->node_count; ++i) {
        uint16_t r;
        uint32_t peer_prefix_count = 0;
        uint32_t peer_block_count = 0;
        uint32_t peer_group_count = 0;

        if (!peer_ready[i]) {
            printf("[w4_guest] gap db_service_cluster_stage=payload_not_ready owner=node%d\n",
                   i + 1);
            goto out;
        }
        snapshot = peer_snapshots[i];
        if (summary->peer_record_count_floor == 0 ||
            snapshot.record_count < summary->peer_record_count_floor) {
            summary->peer_record_count_floor = snapshot.record_count;
        }
        if (i != rt->local_idx) {
            summary->peers_observed += 1;
        }
        for (r = 0; r < snapshot.record_count; ++r) {
            struct w4_db_record *rec = &snapshot.records[r];

            if (!rec->in_use) {
                goto out;
            }
            if (rec->kind == W4_DB_RECORD_REQUEST_PREFIX) {
                peer_prefix_count += 1;
                if (summary->peer_prefix_version_floor == 0 ||
                    rec->version < summary->peer_prefix_version_floor) {
                    summary->peer_prefix_version_floor = rec->version;
                }
                if (summary->peer_prefix_result_floor == 0 ||
                    rec->last_result_segment < summary->peer_prefix_result_floor) {
                    summary->peer_prefix_result_floor = rec->last_result_segment;
                }
                if (rec->state != W4_KVCACHE_STATE_RELOADED) {
                    summary->prefix_state_ready = false;
                }
                if (rec->hot_segment_id == 0 || rec->last_result_segment == 0) {
                    summary->prefix_view_ready = false;
                }
                printf("[w4_guest] stage db_service_cluster_observe owner=node%d kind=request_prefix key=%s version=%" PRIu64 "\n",
                       i + 1,
                       rec->key,
                       rec->version);
            } else if (rec->kind == W4_DB_RECORD_PREFIX_GROUP) {
                peer_group_count += 1;
                printf("[w4_guest] stage db_service_cluster_observe owner=node%d kind=prefix_group key=%s group=%s members=%u state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       i + 1,
                       rec->key,
                       rec->group_id,
                       rec->member_count,
                       w4_kvcache_state_name(rec->state),
                       rec->version,
                       rec->last_result_segment);
            } else if (rec->kind == W4_DB_RECORD_BLOCK_META) {
                peer_block_count += 1;
                if (rec->version < summary->peer_version_floor) {
                    summary->peer_version_floor = rec->version;
                }
                if (rec->last_result_segment < summary->peer_result_floor) {
                    summary->peer_result_floor = rec->last_result_segment;
                }
                if (strncmp(rec->key, local_record->key, sizeof(rec->key)) == 0 &&
                    (rec->placement_node != local_record->placement_node ||
                     rec->placement_level != local_record->placement_level ||
                     rec->hot_segment_id != local_record->hot_segment_id)) {
                    summary->placement_coherent = false;
                }
                if (strncmp(rec->key, local_record->key, sizeof(rec->key)) == 0 &&
                    rec->state != local_record->state) {
                    summary->state_coherent = false;
                }
                printf("[w4_guest] stage db_service_cluster_observe owner=node%d kind=block_meta key=%s state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       i + 1,
                       rec->key,
                       w4_kvcache_state_name(rec->state),
                       rec->version,
                       rec->last_result_segment);
            } else if (rec->kind == W4_DB_RECORD_WEIGHT_TILE ||
                       rec->kind == W4_DB_RECORD_KVCACHE_OBJECT) {
                printf("[w4_guest] stage db_service_cluster_observe owner=node%d kind=%s key=%s offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " version=%" PRIu64 "\n",
                       i + 1,
                       w4_db_object_kind_name(rec->object_payload_kind),
                       rec->key,
                       rec->object_backing_offset,
                       rec->object_backing_len,
                       rec->object_payload_checksum,
                       rec->version);
            } else {
                goto out;
            }
        }
        if (summary->peer_prefix_count_floor == 0 ||
            peer_prefix_count < summary->peer_prefix_count_floor) {
            summary->peer_prefix_count_floor = peer_prefix_count;
        }
        if (summary->peer_block_count_floor == 0 ||
            peer_block_count < summary->peer_block_count_floor) {
            summary->peer_block_count_floor = peer_block_count;
        }
        if (summary->peer_group_count_floor == 0 ||
            peer_group_count < summary->peer_group_count_floor) {
            summary->peer_group_count_floor = peer_group_count;
        }
    }

    summary->ready = (summary->peers_observed == (uint32_t)(rt->node_count - 1));
    if (summary->ready) {
        printf("[w4_guest] stage db_service_cluster=metadata_visible nodes=%u peers=%u local_version=%" PRIu64 " peer_version_floor=%" PRIu64 " peer_prefix_version_floor=%" PRIu64 " peer_prefix_result_floor=0x%016" PRIx64 " peer_record_count_floor=%u peer_prefix_count_floor=%u peer_block_count_floor=%u peer_group_count_floor=%u prefix_state_ready=%s prefix_view_ready=%s\n",
               summary->node_count,
               summary->peers_observed,
               summary->local_version,
               summary->peer_version_floor,
               summary->peer_prefix_version_floor,
               summary->peer_prefix_result_floor,
               summary->peer_record_count_floor,
               summary->peer_prefix_count_floor,
               summary->peer_block_count_floor,
               summary->peer_group_count_floor,
               summary->prefix_state_ready ? "true" : "false",
               summary->prefix_view_ready ? "true" : "false");
    }
    rc = 0;

out:
    return rc;
}

int w4_db_obmm_service_v0_publish_resolve(struct w4_db_service *svc,
                                          uint32_t local_node,
                                          uint32_t remote_node,
                                          uint32_t cluster_node_count)
{
    struct w4_db_cluster_runtime *rt = &g_w4_db_cluster_runtime;
    struct w4_db_record local_weight;
    struct w4_db_record local_kvcache;
    struct w4_db_record remote_weight;
    struct w4_db_record remote_kvcache;
    struct w4_db_cluster_slot *local_slot;
    struct w4_db_cluster_slot *remote_slot;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long deadline;
    uint8_t *base;
    uint64_t weight_checksum;
    uint64_t kvcache_checksum;
    uint64_t remote_weight_checksum;
    uint64_t remote_kvcache_checksum;
    char local_weight_key[96];
    char local_kvcache_key[96];
    char remote_weight_key[96];
    char remote_kvcache_key[96];
    uint16_t last_seen_seq = 0;
    uint16_t last_seen_done_seq = 0;
    uint16_t last_seen_record_count = 0;
    bool saw_remote_snapshot = false;
    bool got_remote_weight = false;
    bool got_remote_kvcache = false;
    unsigned int relax_attempt = 0;

    if (!svc || cluster_node_count == 0 || local_node >= cluster_node_count ||
        remote_node >= cluster_node_count || local_node == remote_node) {
        return -1;
    }
    if (w4_db_cluster_runtime_init(rt) != 0) {
        return -1;
    }
    if ((uint32_t)rt->local_idx != local_node) {
        printf("[w4_guest] gap obmm_service_v0=local_node_mismatch expected=%u actual=%d\n",
               local_node + 1U,
               rt->local_idx + 1);
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if (!local_slot->region.addr || local_slot->region.len < W4_DB_OBMM_KVCACHE_OFFSET + W4_DB_OBMM_OBJECT_BYTES) {
        printf("[w4_guest] gap obmm_service_v0=local_region_too_small len=%zu\n",
               local_slot->region.len);
        return -1;
    }

    snprintf(local_weight_key,
             sizeof(local_weight_key),
             "weights/qwen3-0.6b/node%u/tile0",
             local_node + 1U);
    snprintf(local_kvcache_key,
             sizeof(local_kvcache_key),
             "kvcache/w4/node%u/block0",
             local_node + 1U);
    snprintf(remote_weight_key,
             sizeof(remote_weight_key),
             "weights/qwen3-0.6b/node%u/tile0",
             remote_node + 1U);
    snprintf(remote_kvcache_key,
             sizeof(remote_kvcache_key),
             "kvcache/w4/node%u/block0",
             remote_node + 1U);

    base = (uint8_t *)local_slot->region.addr;
    w4_db_fill_obmm_object_payload(base + W4_DB_OBMM_WEIGHT_OFFSET,
                                   W4_DB_OBMM_OBJECT_BYTES,
                                   local_node,
                                   W4_DB_OBMM_KIND_WEIGHT_TILE);
    w4_db_fill_obmm_object_payload(base + W4_DB_OBMM_KVCACHE_OFFSET,
                                   W4_DB_OBMM_OBJECT_BYTES,
                                   local_node,
                                   W4_DB_OBMM_KIND_KVCACHE_BLOCK);
    weight_checksum = w4_db_checksum_bytes(base + W4_DB_OBMM_WEIGHT_OFFSET,
                                           W4_DB_OBMM_OBJECT_BYTES);
    kvcache_checksum = w4_db_checksum_bytes(base + W4_DB_OBMM_KVCACHE_OFFSET,
                                            W4_DB_OBMM_OBJECT_BYTES);
    if (w4_db_update_region_range_at(local_slot,
                                     W4_DB_OBMM_WEIGHT_OFFSET,
                                     W4_DB_OBMM_OBJECT_BYTES,
                                     true) != 0 ||
        w4_db_update_region_range_at(local_slot,
                                     W4_DB_OBMM_KVCACHE_OFFSET,
                                     W4_DB_OBMM_OBJECT_BYTES,
                                     true) != 0) {
        printf("[w4_guest] gap obmm_service_v0=local_payload_publish_failed\n");
        return -1;
    }
    (void)msync(base + W4_DB_OBMM_WEIGHT_OFFSET, W4_DB_OBMM_OBJECT_BYTES, MS_SYNC);
    (void)msync(base + W4_DB_OBMM_KVCACHE_OFFSET, W4_DB_OBMM_OBJECT_BYTES, MS_SYNC);

    if (w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_WEIGHT_TILE,
                                     local_weight_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_WEIGHT_TILE,
                                     W4_DB_OBMM_WEIGHT_OFFSET,
                                     W4_DB_OBMM_OBJECT_BYTES,
                                     weight_checksum,
                                     &local_weight) != 0 ||
        w4_db_put_obmm_object_record(svc,
                                     W4_DB_RECORD_KVCACHE_OBJECT,
                                     local_kvcache_key,
                                     local_node,
                                     W4_DB_OBMM_KIND_KVCACHE_BLOCK,
                                     W4_DB_OBMM_KVCACHE_OFFSET,
                                     W4_DB_OBMM_OBJECT_BYTES,
                                     kvcache_checksum,
                                     &local_kvcache) != 0) {
        printf("[w4_guest] gap obmm_service_v0=metadata_put_failed\n");
        return -1;
    }
    printf("[w4_guest] stage obmm_service_v0_publish kind=weight_tile key=%s owner=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_weight.key,
           local_node + 1U,
           local_weight.object_backing_offset,
           local_weight.object_backing_len,
           local_weight.object_payload_checksum);
    printf("[w4_guest] stage obmm_service_v0_publish kind=kvcache_block key=%s owner=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_kvcache.key,
           local_node + 1U,
           local_kvcache.object_backing_offset,
           local_kvcache.object_backing_len,
           local_kvcache.object_payload_checksum);

    if (w4_db_write_cluster_payload(svc, local_slot) != 0) {
        printf("[w4_guest] gap obmm_service_v0=metadata_publish_failed\n");
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    object_epoch = rt->observe_epoch;
    w4_db_announce_cluster_msg(rt->sockfd,
                               rt->peers,
                               rt->node_count,
                               rt->local_idx,
                               W4_DB_CLUSTER_MSG_READY,
                               object_epoch,
                               local_publish_seq,
                               rt->metas);
    printf("[w4_guest] stage obmm_service_v0_local_ready_announced local=node%u epoch=%u seq=%u\n",
           local_node + 1U,
           object_epoch,
           local_publish_seq);
    if (w4_db_activate_remote_slot(rt, (int)remote_node) != 0) {
        printf("[w4_guest] gap obmm_service_v0=remote_slot_import_failed remote=node%u\n",
               remote_node + 1U);
        return -1;
    }
    remote_slot = &rt->slots[remote_node];
    deadline = w4_db_now_ms() + W4_DB_OBMM_SERVICE_WAIT_MS;
    while (w4_db_now_ms() < deadline) {
        struct w4_db_cluster_payload snapshot;
        struct w4_db_cluster_payload_header seen;

        memset(&snapshot, 0, sizeof(snapshot));
        memset(&seen, 0, sizeof(seen));
        if (w4_db_read_stable_payload_region(remote_slot, &snapshot, &seen)) {
            last_seen_seq = seen.publish_seq;
            last_seen_done_seq = seen.publish_done_seq;
            last_seen_record_count = seen.record_count;
            saw_remote_snapshot = true;
            got_remote_weight = w4_db_payload_snapshot_find_record(&snapshot,
                                                                   remote_weight_key,
                                                                   &remote_weight);
            got_remote_kvcache = w4_db_payload_snapshot_find_record(&snapshot,
                                                                    remote_kvcache_key,
                                                                    &remote_kvcache);
        }
        if (got_remote_weight && got_remote_kvcache) {
            break;
        }
        w4_db_cpu_relax_wait(&relax_attempt);
    }
    if (remote_weight.kind != W4_DB_RECORD_WEIGHT_TILE ||
        remote_kvcache.kind != W4_DB_RECORD_KVCACHE_OBJECT) {
        printf("[w4_guest] gap obmm_service_v0=remote_metadata_resolve_failed remote=node%u snapshot=%u seq=%u done=%u count=%u weight=%u kvcache=%u\n",
               remote_node + 1U,
               saw_remote_snapshot ? 1U : 0U,
               last_seen_seq,
               last_seen_done_seq,
               last_seen_record_count,
               got_remote_weight ? 1U : 0U,
               got_remote_kvcache ? 1U : 0U);
        return -1;
    }
    if (remote_weight.kind != W4_DB_RECORD_WEIGHT_TILE ||
        remote_kvcache.kind != W4_DB_RECORD_KVCACHE_OBJECT ||
        remote_weight.object_backing_len != W4_DB_OBMM_OBJECT_BYTES ||
        remote_kvcache.object_backing_len != W4_DB_OBMM_OBJECT_BYTES) {
        printf("[w4_guest] gap obmm_service_v0=remote_metadata_incoherent remote=node%u\n",
               remote_node + 1U);
        return -1;
    }
    if (!remote_slot->region.addr ||
        remote_weight.object_backing_offset + remote_weight.object_backing_len > remote_slot->region.len ||
        remote_kvcache.object_backing_offset + remote_kvcache.object_backing_len > remote_slot->region.len) {
        printf("[w4_guest] gap obmm_service_v0=remote_region_too_small remote=node%u\n",
               remote_node + 1U);
        return -1;
    }
    remote_weight_checksum =
        w4_db_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                 remote_weight.object_backing_offset,
                             remote_weight.object_backing_len);
    remote_kvcache_checksum =
        w4_db_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                 remote_kvcache.object_backing_offset,
                             remote_kvcache.object_backing_len);
    if (remote_weight_checksum != remote_weight.object_payload_checksum ||
        remote_kvcache_checksum != remote_kvcache.object_payload_checksum) {
        printf("[w4_guest] gap obmm_service_v0=remote_payload_checksum_mismatch remote=node%u weight=0x%016" PRIx64 "/0x%016" PRIx64 " kvcache=0x%016" PRIx64 "/0x%016" PRIx64 "\n",
               remote_node + 1U,
               remote_weight_checksum,
               remote_weight.object_payload_checksum,
               remote_kvcache_checksum,
               remote_kvcache.object_payload_checksum);
        return -1;
    }
    printf("[w4_guest] stage obmm_service_v0_resolve kind=weight_tile key=%s owner=node%u reader=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_weight.key,
           remote_node + 1U,
           local_node + 1U,
           remote_weight.object_backing_offset,
           remote_weight.object_backing_len,
           remote_weight_checksum);
    printf("[w4_guest] stage obmm_service_v0_resolve kind=kvcache_block key=%s owner=node%u reader=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_kvcache.key,
           remote_node + 1U,
           local_node + 1U,
           remote_kvcache.object_backing_offset,
           remote_kvcache.object_backing_len,
           remote_kvcache_checksum);
    printf("[w4_guest] stage obmm_service_v0=payload_backing_resolved local=node%u remote=node%u objects=2 bytes=%" PRIu64 " boundary_offsets=0,248,256,4088,4096 backing=obmm_pool metadata=db status=ok\n",
           local_node + 1U,
           remote_node + 1U,
           (uint64_t)W4_DB_OBMM_OBJECT_BYTES);
    return 0;
}
