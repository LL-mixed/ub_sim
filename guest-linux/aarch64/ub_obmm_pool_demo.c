#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../kernel_ub/include/uapi/ub/obmm.h"

#define OBMM_PORT 18560
#define RUN_TIMEOUT_S 120
#define WAIT_IFACE_MS 90000
#define EXPORT_REGION_SIZE (2UL * 1024UL * 1024UL)
#define SLOT_TOUCH_SIZE 256UL
#define IMPORT_ALIGN (2UL * 1024UL * 1024UL)
#define MAX_NODES 8
#define MAX_WINDOWS 16
#define POOL_MAGIC 0x4f424d50U
#define POOL_VERSION 1U
#define MSG_HELLO 1U
#define MSG_READY 2U
#define MSG_ROUND_TURN 3U
#define MSG_ROUND_ACK 4U
#define MSG_ROUND_COMMIT 5U
#define OBMM_SIM_DEC_PRIV_MAGIC 0x53444950U
#define OBMM_SIM_DEC_PRIV_VER_1 1

struct obmm_sim_dec_import_priv_v1_user {
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint64_t remote_uba;
    uint32_t token_value;
    uint32_t flags;
};

struct obmm_demo_meta {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
};

struct mapped_region {
    int fd;
    void *addr;
    size_t len;
    uint64_t mem_id;
};

struct mem_window {
    uint64_t base_pa;
    uint64_t size_bytes;
    uint64_t decode;
    unsigned int mar;
    bool is_cacheable;
};

struct pool_slot {
    int owner_idx;
    bool is_local;
    bool map_osync;
    uint64_t mem_id;
    uint64_t local_pa;
    uint32_t export_cna;
    struct mapped_region region;
};

struct pool_msg {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint16_t src_idx;
    uint16_t dst_idx;
    uint16_t round_idx;
    uint16_t slot_count;
    struct obmm_demo_meta meta;
};

struct pool_slot_record {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved0;
    uint16_t data_owner_idx;
    uint16_t data_round_idx;
    uint64_t data_cookie;
    uint16_t ack_writer_idx;
    uint16_t ack_owner_idx;
    uint16_t ack_round_idx;
    uint64_t ack_cookie;
    uint16_t commit_owner_idx;
    uint16_t commit_round_idx;
    uint32_t reserved1;
    uint64_t commit_cookie;
};

static volatile sig_atomic_t g_alarm_fired;

static void alarm_handler(int signo)
{
    (void)signo;
    g_alarm_fired = 1;
}

static long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static bool read_file(const char *path, char *buf, size_t len)
{
    int fd;
    ssize_t n;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }

    buf[n] = '\0';
    return true;
}

static bool parse_hex_file_u64(const char *path, uint64_t *value_out)
{
    char buf[128];
    char *end = NULL;
    unsigned long long value;

    if (!read_file(path, buf, sizeof(buf))) {
        return false;
    }

    errno = 0;
    value = strtoull(buf, &end, 0);
    if (errno != 0 || end == buf) {
        return false;
    }

    *value_out = (uint64_t)value;
    return true;
}

static bool cmdline_get_value(const char *key, char *out, size_t out_len)
{
    char buf[4096];
    char *saveptr = NULL;
    char *tok;
    size_t key_len;

    if (!read_file("/proc/cmdline", buf, sizeof(buf))) {
        return false;
    }

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

static bool env_or_cmdline_value(const char *env_key, const char *cmd_key,
                                 char *out, size_t out_len)
{
    const char *env = getenv(env_key);

    if (env && env[0] != '\0') {
        snprintf(out, out_len, "%s", env);
        return true;
    }
    return cmdline_get_value(cmd_key, out, out_len);
}

static bool parse_node_ip_list(const char *csv, char ips[MAX_NODES][INET_ADDRSTRLEN],
                               int *count_out)
{
    char buf[512];
    char *saveptr = NULL;
    char *tok;
    int count = 0;

    snprintf(buf, sizeof(buf), "%s", csv);
    tok = strtok_r(buf, ",", &saveptr);
    while (tok != NULL) {
        while (*tok == ' ' || *tok == '\t') {
            tok++;
        }
        if (*tok != '\0') {
            if (count >= MAX_NODES) {
                return false;
            }
            snprintf(ips[count], INET_ADDRSTRLEN, "%s", tok);
            if (inet_pton(AF_INET, ips[count], &(struct in_addr){0}) != 1) {
                return false;
            }
            count++;
        }
        tok = strtok_r(NULL, ",", &saveptr);
    }

    if (count < 2) {
        return false;
    }
    *count_out = count;
    return true;
}

static bool resolve_pool_nodes(char local_ip[INET_ADDRSTRLEN],
                               char ips[MAX_NODES][INET_ADDRSTRLEN],
                               int *node_count_out, int *local_idx_out)
{
    char csv[512];
    int local_idx;
    int node_count;
    int i;

    if (!env_or_cmdline_value("LINQU_UB_LOCAL_IP", "linqu_ipourma_ipv4",
                              local_ip, INET_ADDRSTRLEN)) {
        return false;
    }

    if (!env_or_cmdline_value("LINQU_UB_ALL_IPS", "linqu_ipourma_all_ipv4",
                              csv, sizeof(csv))) {
        int max_nodes = 2;
        char count_buf[16];
        if (env_or_cmdline_value("LINQU_UB_NODE_COUNT", "linqu_ipourma_node_count",
                                 count_buf, sizeof(count_buf))) {
            max_nodes = atoi(count_buf);
            if (max_nodes < 2 || max_nodes > MAX_NODES) {
                return false;
            }
        }
        csv[0] = '\0';
        for (i = 1; i <= max_nodes; i++) {
            char ip[INET_ADDRSTRLEN];
            snprintf(ip, sizeof(ip), "10.0.0.%d", i);
            if (csv[0] != '\0') {
                if (snprintf(csv + strlen(csv), sizeof(csv) - strlen(csv), ",") >=
                    (int)(sizeof(csv) - strlen(csv))) {
                    return false;
                }
            }
            if (snprintf(csv + strlen(csv), sizeof(csv) - strlen(csv), "%s", ip) >=
                (int)(sizeof(csv) - strlen(csv))) {
                return false;
            }
        }
    }

    if (!parse_node_ip_list(csv, ips, &node_count)) {
        return false;
    }

    local_idx = -1;
    for (i = 0; i < node_count; i++) {
        if (strcmp(ips[i], local_ip) == 0) {
            local_idx = i;
            break;
        }
    }
    if (local_idx < 0) {
        return false;
    }

    *node_count_out = node_count;
    *local_idx_out = local_idx;
    return true;
}

static bool find_ipourma_iface(char *name, size_t name_len)
{
    FILE *fp;
    char line[512];

    fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        return false;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *colon;
        char *left;

        colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';

        left = line;
        while (*left == ' ' || *left == '\t') {
            left++;
        }

        if (strncmp(left, "ipourma", strlen("ipourma")) == 0) {
            size_t n = strcspn(left, " \t\r\n");
            if (n >= name_len) {
                n = name_len - 1;
            }
            memcpy(name, left, n);
            name[n] = '\0';
            fclose(fp);
            return true;
        }
    }

    fclose(fp);
    return false;
}

static bool iface_is_up(const char *ifname)
{
    struct ifreq ifr;
    int fd;

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
        close(fd);
        return false;
    }

    close(fd);
    return ((ifr.ifr_flags & IFF_UP) != 0);
}

static bool wait_iface_ready(char *ifname, size_t ifname_len, unsigned int *ifindex)
{
    long deadline = now_ms() + WAIT_IFACE_MS;

    while (now_ms() < deadline) {
        if (find_ipourma_iface(ifname, ifname_len)) {
            *ifindex = if_nametoindex(ifname);
            if (*ifindex > 0 && iface_is_up(ifname)) {
                return true;
            }
        }
        usleep(200000);
    }

    return false;
}

static bool set_ipv4_addr(const char *ifname, const char *addr_str)
{
    struct ifreq ifr;
    struct sockaddr_in *sin;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, addr_str, &sin->sin_addr) != 1) {
        close(fd);
        return false;
    }

    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) {
        close(fd);
        return false;
    }

    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    sin = (struct sockaddr_in *)&ifr.ifr_netmask;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) != 0) {
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

static bool get_local_ipv4(const char *ifname, struct in_addr *addr)
{
    struct ifreq ifr;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return false;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);

    if (ioctl(fd, SIOCGIFADDR, &ifr) != 0) {
        close(fd);
        return false;
    }

    close(fd);
    *addr = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    return (addr->s_addr != 0);
}

static void install_static_arp(const char *ifname, const struct in_addr *peer_addr)
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

static int create_udp_socket(const char *ifname)
{
    int sockfd;
    int one = 1;
    int buf_bytes = 1 << 20;
    struct sockaddr_in bind_addr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    (void)setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname));
    (void)setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &buf_bytes, sizeof(buf_bytes));
    (void)setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &buf_bytes, sizeof(buf_bytes));

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(OBMM_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        close(sockfd);
        return -1;
    }

    return sockfd;
}

static bool parse_windows(struct mem_window windows[MAX_WINDOWS], int *count_out)
{
    FILE *fp;
    char line[256];
    int count = 0;

    fp = fopen("/sys/bus/ub/devices/00001/mem_windows", "r");
    if (!fp) {
        fprintf(stderr, "[ub_obmm_pool] open mem_windows failed: %s\n", strerror(errno));
        return false;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long mar = 0;
        unsigned long long decode = 0;
        unsigned long long cc_base_mb = 0;
        unsigned long long cc_size_mb = 0;
        unsigned long long nc_base_mb = 0;
        unsigned long long nc_size_mb = 0;
        int matched;

        matched = sscanf(line,
                         "mar%llu decode=%llx cc_base_mb=%llx cc_size_mb=%llx nc_base_mb=%llx nc_size_mb=%llx",
                         &mar, &decode, &cc_base_mb, &cc_size_mb,
                         &nc_base_mb, &nc_size_mb);
        if (matched != 6) {
            continue;
        }
        if (count >= MAX_WINDOWS) {
            break;
        }
        windows[count].mar = (unsigned int)mar;
        windows[count].decode = (uint64_t)decode;
        if (nc_size_mb != 0) {
            windows[count].base_pa = ((uint64_t)nc_base_mb) << 20;
            windows[count].size_bytes = ((uint64_t)nc_size_mb) << 20;
            windows[count].is_cacheable = false;
        } else {
            windows[count].base_pa = ((uint64_t)cc_base_mb) << 20;
            windows[count].size_bytes = ((uint64_t)cc_size_mb) << 20;
            windows[count].is_cacheable = true;
        }
        fprintf(stderr,
                "[ub_obmm_pool] mem_window mar=%u decode=%#" PRIx64 " use=[%#" PRIx64 ",%#" PRIx64 "] cc_base_mb=%#llx cc_size_mb=%#llx nc_base_mb=%#llx nc_size_mb=%#llx\n",
                windows[count].mar, windows[count].decode,
                windows[count].base_pa, windows[count].size_bytes,
                cc_base_mb, cc_size_mb,
                nc_base_mb, nc_size_mb);
        count++;
    }

    fclose(fp);
    *count_out = count;
    return count > 0;
}

static uint64_t align_up_u64(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}

static bool allocate_import_pas(int import_count, uint64_t size_per_import,
                                uint64_t pas[MAX_NODES], bool map_osync[MAX_NODES])
{
    struct mem_window windows[MAX_WINDOWS];
    int window_count = 0;
    int import_idx = 0;
    int wi;

    if (import_count <= 0) {
        return true;
    }
    if (!parse_windows(windows, &window_count)) {
        return false;
    }

    for (wi = 0; wi < window_count && import_idx < import_count; wi++) {
        uint64_t cur = align_up_u64(windows[wi].base_pa, IMPORT_ALIGN);
        uint64_t end = windows[wi].base_pa + windows[wi].size_bytes;
        while (import_idx < import_count && cur + size_per_import <= end) {
            pas[import_idx++] = cur;
            map_osync[import_idx - 1] = !windows[wi].is_cacheable;
            cur = align_up_u64(cur + size_per_import, IMPORT_ALIGN);
        }
    }

    return import_idx == import_count;
}

static int open_obmm(void)
{
    return open("/dev/obmm", O_RDWR);
}

static int open_region_dev(uint64_t mem_id, bool map_osync)
{
    char path[128];
    snprintf(path, sizeof(path), "/dev/obmm_shmdev%" PRIu64, mem_id);
    return open(path, O_RDWR | (map_osync ? O_SYNC : 0));
}

static int map_region_device(uint64_t mem_id, size_t len, bool map_osync,
                             struct mapped_region *region)
{
    memset(region, 0, sizeof(*region));
    region->fd = -1;
    region->mem_id = mem_id;
    region->len = len;

    region->fd = open_region_dev(mem_id, map_osync);
    if (region->fd < 0) {
        fprintf(stderr, "[ub_obmm_pool] open shmdev%" PRIu64 " failed: %s\n",
                mem_id, strerror(errno));
        return -1;
    }

    region->addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, region->fd, 0);
    if (region->addr == MAP_FAILED) {
        fprintf(stderr, "[ub_obmm_pool] mmap shmdev%" PRIu64 " failed: %s\n",
                mem_id, strerror(errno));
        close(region->fd);
        region->fd = -1;
        region->addr = NULL;
        return -1;
    }

    fprintf(stderr, "[ub_obmm_pool] shmdev open/mmap -> ok mem_id=%" PRIu64 " mode=%s\n",
            mem_id, map_osync ? "osync" : "normal");
    return 0;
}

static void unmap_region_device(struct mapped_region *region)
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

static int do_export_region(int obmm_fd, struct obmm_demo_meta *meta)
{
    struct obmm_cmd_export cmd;
    memset(&cmd, 0, sizeof(cmd));

    cmd.length = 1;
    cmd.size[0] = EXPORT_REGION_SIZE;
    cmd.flags = OBMM_EXPORT_FLAG_ALLOW_MMAP;
    cmd.pxm_numa = 0;
    if (ioctl(obmm_fd, OBMM_CMD_EXPORT, &cmd) != 0) {
        fprintf(stderr, "[ub_obmm_pool] export failed: %s\n", strerror(errno));
        return -1;
    }

    meta->export_mem_id = cmd.mem_id;
    meta->remote_uba = cmd.uba;
    meta->size = EXPORT_REGION_SIZE;
    meta->token_id = cmd.tokenid;
    fprintf(stderr,
            "[ub_obmm_pool] export -> ok mem_id=%" PRIu64 " uba=%#" PRIx64 " token=%u\n",
            meta->export_mem_id, meta->remote_uba, meta->token_id);
    return 0;
}

static int do_unexport_region(int obmm_fd, uint64_t mem_id)
{
    struct obmm_cmd_unexport cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.mem_id = mem_id;
    if (ioctl(obmm_fd, OBMM_CMD_UNEXPORT, &cmd) != 0) {
        fprintf(stderr, "[ub_obmm_pool] unexport failed mem_id=%" PRIu64 ": %s\n",
                mem_id, strerror(errno));
        return -1;
    }

    fprintf(stderr, "[ub_obmm_pool] unexport -> ok mem_id=%" PRIu64 "\n", mem_id);
    return 0;
}

static int do_import_region(int obmm_fd, const struct obmm_demo_meta *meta,
                            int owner_idx, uint32_t local_cna, uint64_t local_pa,
                            uint64_t *import_mem_id)
{
    struct obmm_sim_dec_import_priv_v1_user priv;
    struct obmm_cmd_import cmd;

    memset(&priv, 0, sizeof(priv));
    priv.magic = OBMM_SIM_DEC_PRIV_MAGIC;
    priv.version = OBMM_SIM_DEC_PRIV_VER_1;
    priv.len = sizeof(priv);
    priv.remote_uba = meta->remote_uba;
    priv.token_value = 0;

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
        fprintf(stderr,
                "[ub_obmm_pool] import failed pa=%#" PRIx64 " uba=%#" PRIx64
                " token=%u scna=%#x dcna=%#x: %s\n",
                local_pa, meta->remote_uba, meta->token_id,
                local_cna, meta->export_cna, strerror(errno));
        return -1;
    }

    *import_mem_id = cmd.mem_id;
    fprintf(stderr,
            "[ub_obmm_pool] import -> ok owner=%d mem_id=%" PRIu64 " local_pa=%#" PRIx64
            " local_cna=%#x remote_cna=%#x\n",
            owner_idx + 1, *import_mem_id, local_pa, local_cna, meta->export_cna);
    return 0;
}

static int do_unimport_region(int obmm_fd, uint64_t mem_id)
{
    struct obmm_cmd_unimport cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.mem_id = mem_id;
    if (ioctl(obmm_fd, OBMM_CMD_UNIMPORT, &cmd) != 0) {
        fprintf(stderr, "[ub_obmm_pool] unimport failed mem_id=%" PRIu64 ": %s\n",
                mem_id, strerror(errno));
        return -1;
    }

    fprintf(stderr, "[ub_obmm_pool] unimport -> ok mem_id=%" PRIu64 "\n", mem_id);
    return 0;
}

static int send_msg(int sockfd, const struct sockaddr_in *peer,
                    const void *buf, size_t len)
{
    ssize_t n = sendto(sockfd, buf, len, 0,
                       (const struct sockaddr *)peer, sizeof(*peer));
    return (n == (ssize_t)len) ? 0 : -1;
}

static ssize_t recv_msg(int sockfd, void *buf, size_t len, struct sockaddr_in *from)
{
    socklen_t fromlen = sizeof(*from);
    return recvfrom(sockfd, buf, len, MSG_DONTWAIT,
                    (struct sockaddr *)from, &fromlen);
}

static void init_pool_msg(struct pool_msg *msg, uint16_t type, int src_idx, int dst_idx)
{
    memset(msg, 0, sizeof(*msg));
    msg->magic = POOL_MAGIC;
    msg->version = POOL_VERSION;
    msg->type = type;
    msg->src_idx = (uint16_t)src_idx;
    msg->dst_idx = (uint16_t)dst_idx;
}

static uint64_t round_cookie(int owner_idx, int slot_idx)
{
    return ((uint64_t)(owner_idx + 1) << 32) | (uint32_t)(slot_idx + 1);
}

static int write_slot_payload(struct pool_slot *slot, int owner_idx, int slot_idx)
{
    struct pool_slot_record rec;

    memset(&rec, 0, sizeof(rec));
    memcpy(&rec, slot->region.addr, sizeof(rec));
    rec.magic = POOL_MAGIC;
    rec.version = POOL_VERSION;
    rec.data_owner_idx = (uint16_t)owner_idx;
    rec.data_round_idx = (uint16_t)slot_idx;
    rec.data_cookie = round_cookie(owner_idx, slot_idx);
    if (sizeof(rec) > SLOT_TOUCH_SIZE) {
        return -1;
    }
    memset(slot->region.addr, 0, SLOT_TOUCH_SIZE);
    memcpy(slot->region.addr, &rec, sizeof(rec));
    __sync_synchronize();
    (void)msync(slot->region.addr, SLOT_TOUCH_SIZE, MS_SYNC);
    return 0;
}

static bool slot_matches_payload(const struct pool_slot *slot, int owner_idx, int slot_idx)
{
    const struct pool_slot_record *rec = slot->region.addr;
    return rec->magic == POOL_MAGIC &&
           rec->version == POOL_VERSION &&
           rec->data_owner_idx == (uint16_t)owner_idx &&
           rec->data_round_idx == (uint16_t)slot_idx &&
           rec->data_cookie == round_cookie(owner_idx, slot_idx);
}

static int wait_for_slot_payload(const struct pool_slot *slot, int owner_idx, int slot_idx,
                                 long timeout_ms)
{
    long deadline = now_ms() + timeout_ms;

    while (!g_alarm_fired && now_ms() < deadline) {
        if (slot_matches_payload(slot, owner_idx, slot_idx)) {
            return 0;
        }
        usleep(100000);
    }
    return -1;
}

static int broadcast_hello_until_all(int sockfd,
                                     struct sockaddr_in peers[MAX_NODES],
                                     int node_count, int local_idx,
                                     const struct obmm_demo_meta *local_meta,
                                     struct obmm_demo_meta metas[MAX_NODES],
                                     bool got_meta[MAX_NODES])
{
    struct pool_msg msg;
    long deadline = now_ms() + RUN_TIMEOUT_S * 1000L;

    metas[local_idx] = *local_meta;
    got_meta[local_idx] = true;

    while (!g_alarm_fired && now_ms() < deadline) {
        int i;
        struct sockaddr_in from;
        struct pool_msg rx;
        bool all = true;

        for (i = 0; i < node_count; i++) {
            if (!got_meta[i]) {
                all = false;
                break;
            }
        }
        if (all) {
            return 0;
        }

        for (i = 0; i < node_count; i++) {
            if (i == local_idx || got_meta[i]) {
                continue;
            }
            init_pool_msg(&msg, MSG_HELLO, local_idx, i);
            msg.meta = *local_meta;
            (void)send_msg(sockfd, &peers[i], &msg, sizeof(msg));
        }

        while (recv_msg(sockfd, &rx, sizeof(rx), &from) == (ssize_t)sizeof(rx)) {
            if (rx.magic != POOL_MAGIC || rx.version != POOL_VERSION) {
                continue;
            }
            if (rx.type == MSG_HELLO && rx.src_idx < node_count) {
                metas[rx.src_idx] = rx.meta;
                got_meta[rx.src_idx] = true;
            }
        }
        usleep(100000);
    }

    fprintf(stderr, "[ub_obmm_pool] fail: timeout waiting for all HELLO metadata\n");
    return -1;
}

static int import_all_peers(int obmm_fd, uint32_t local_cna,
                            int node_count, int local_idx,
                            const struct obmm_demo_meta metas[MAX_NODES],
                            struct pool_slot slots[MAX_NODES])
{
    uint64_t import_pas[MAX_NODES];
    bool import_osync[MAX_NODES];
    int import_count = node_count - 1;
    int i;
    int import_idx = 0;

    if (!allocate_import_pas(import_count, EXPORT_REGION_SIZE, import_pas, import_osync)) {
        fprintf(stderr, "[ub_obmm_pool] fail: unable to allocate import local_pa windows\n");
        return -1;
    }

    for (i = 0; i < node_count; i++) {
        uint64_t mem_id;

        if (i == local_idx) {
            continue;
        }
        slots[i].owner_idx = i;
        slots[i].is_local = false;
        slots[i].local_pa = import_pas[import_idx++];
        slots[i].map_osync = import_osync[import_idx - 1];
        slots[i].export_cna = metas[i].export_cna;
        if (do_import_region(obmm_fd, &metas[i], i, local_cna, slots[i].local_pa, &mem_id) != 0) {
            return -1;
        }
        slots[i].mem_id = mem_id;
        if (map_region_device(mem_id, SLOT_TOUCH_SIZE, slots[i].map_osync,
                              &slots[i].region) != 0) {
            return -1;
        }
    }

    fprintf(stderr, "[ub_obmm_pool] import_all -> ok remote_slots=%d\n", import_count);
    return 0;
}

static int wait_until_everyone_ready(int sockfd, struct sockaddr_in peers[MAX_NODES],
                                     int node_count, int local_idx)
{
    bool ready[MAX_NODES] = { false };
    struct pool_msg msg;
    long deadline = now_ms() + RUN_TIMEOUT_S * 1000L;

    ready[local_idx] = true;
    while (!g_alarm_fired && now_ms() < deadline) {
        int i;
        struct sockaddr_in from;
        struct pool_msg rx;
        bool all = true;

        for (i = 0; i < node_count; i++) {
            if (!ready[i]) {
                all = false;
                break;
            }
        }
        if (all) {
            return 0;
        }

        for (i = 0; i < node_count; i++) {
            if (i == local_idx || ready[i]) {
                continue;
            }
            init_pool_msg(&msg, MSG_READY, local_idx, i);
            (void)send_msg(sockfd, &peers[i], &msg, sizeof(msg));
        }

        while (recv_msg(sockfd, &rx, sizeof(rx), &from) == (ssize_t)sizeof(rx)) {
            if (rx.magic != POOL_MAGIC || rx.version != POOL_VERSION) {
                continue;
            }
            if (rx.type == MSG_READY && rx.src_idx < node_count) {
                ready[rx.src_idx] = true;
            }
        }
        usleep(100000);
    }

    fprintf(stderr, "[ub_obmm_pool] fail: timeout waiting for READY from all nodes\n");
    return -1;
}

static void send_round_turn(int sockfd, const struct sockaddr_in *peer,
                            int local_idx, int peer_idx, int round_idx)
{
    struct pool_msg msg;

    init_pool_msg(&msg, MSG_ROUND_TURN, local_idx, peer_idx);
    msg.round_idx = (uint16_t)round_idx;
    (void)send_msg(sockfd, peer, &msg, sizeof(msg));
}

static void send_round_ack(int sockfd, const struct sockaddr_in *peer,
                           int local_idx, int owner_idx, int round_idx)
{
    struct pool_msg msg;

    init_pool_msg(&msg, MSG_ROUND_ACK, local_idx, owner_idx);
    msg.round_idx = (uint16_t)round_idx;
    (void)send_msg(sockfd, peer, &msg, sizeof(msg));
}

static void broadcast_round_commit(int sockfd, struct sockaddr_in peers[MAX_NODES],
                                   int node_count, int local_idx, int round_idx)
{
    struct pool_msg msg;
    int attempt;
    int i;

    for (attempt = 0; attempt < 10; attempt++) {
        for (i = 0; i < node_count; i++) {
            if (i == local_idx) {
                continue;
            }
            init_pool_msg(&msg, MSG_ROUND_COMMIT, local_idx, i);
            msg.round_idx = (uint16_t)round_idx;
            (void)send_msg(sockfd, &peers[i], &msg, sizeof(msg));
        }
        usleep(20000);
    }
}

static int wait_for_round_msg(int sockfd, uint16_t type, int src_idx, int dst_idx,
                              int round_idx, long timeout_ms)
{
    long deadline = now_ms() + timeout_ms;

    while (!g_alarm_fired && now_ms() < deadline) {
        struct sockaddr_in from;
        struct pool_msg rx;

        while (recv_msg(sockfd, &rx, sizeof(rx), &from) == (ssize_t)sizeof(rx)) {
            if (rx.magic != POOL_MAGIC || rx.version != POOL_VERSION) {
                continue;
            }
            if (rx.type == type &&
                rx.src_idx == (uint16_t)src_idx &&
                rx.dst_idx == (uint16_t)dst_idx &&
                rx.round_idx == (uint16_t)round_idx) {
                return 0;
            }
        }
        usleep(10000);
    }

    return -1;
}

static int wait_for_all_round_acks(int sockfd, int node_count, int local_idx,
                                   int round_idx)
{
    bool got_ack[MAX_NODES] = { false };
    int pending = node_count - 1;
    long deadline = now_ms() + 10000;

    while (!g_alarm_fired && now_ms() < deadline) {
        struct sockaddr_in from;
        struct pool_msg rx;

        while (recv_msg(sockfd, &rx, sizeof(rx), &from) == (ssize_t)sizeof(rx)) {
            if (rx.magic != POOL_MAGIC || rx.version != POOL_VERSION) {
                continue;
            }
            if (rx.type != MSG_ROUND_ACK ||
                rx.dst_idx != (uint16_t)local_idx ||
                rx.round_idx != (uint16_t)round_idx ||
                rx.src_idx >= (uint16_t)node_count ||
                rx.src_idx == (uint16_t)local_idx) {
                continue;
            }
            if (!got_ack[rx.src_idx]) {
                got_ack[rx.src_idx] = true;
                pending--;
                fprintf(stderr,
                        "[ub_obmm_pool] round=%d owner=%d got ACK from node=%d\n",
                        round_idx + 1, round_idx + 1, rx.src_idx + 1);
            }
        }

        if (pending == 0) {
            return 0;
        }
        usleep(10000);
    }

    for (int i = 0; i < node_count; i++) {
        if (i == local_idx) {
            continue;
        }
        if (!got_ack[i]) {
            fprintf(stderr,
                    "[ub_obmm_pool] fail: round=%d timeout waiting ACK from node=%d\n",
                    round_idx + 1, i + 1);
            return -1;
        }
    }

    return -1;
}

static int do_rounds(int sockfd, struct sockaddr_in peers[MAX_NODES],
                     int node_count, int local_idx, struct pool_slot slots[MAX_NODES])
{
    int round_idx;

    for (round_idx = 0; round_idx < node_count; round_idx++) {
        int owner_slot = round_idx;
        int i;

        if (local_idx == round_idx) {
            if (write_slot_payload(&slots[owner_slot], round_idx, owner_slot) != 0) {
                fprintf(stderr, "[ub_obmm_pool] fail: round=%d write owner slot failed\n",
                        round_idx + 1);
                return -1;
            }
            fprintf(stderr, "[ub_obmm_pool] round owner=%d write_local -> ok slot=%d\n",
                    round_idx + 1, owner_slot + 1);
            if (wait_for_slot_payload(&slots[owner_slot], round_idx, owner_slot, 5000) != 0) {
                fprintf(stderr,
                        "[ub_obmm_pool] fail: round=%d verify owner=%d slot=%d timeout\n",
                        round_idx + 1, round_idx + 1, owner_slot + 1);
                return -1;
            }
            fprintf(stderr, "[ub_obmm_pool] round verify owner=%d -> ok slot=%d\n",
                    round_idx + 1, owner_slot + 1);
            for (i = 0; i < node_count; i++) {
                if (i == local_idx) {
                    continue;
                }
                send_round_turn(sockfd, &peers[i], local_idx, i, round_idx);
            }
            if (wait_for_all_round_acks(sockfd, node_count, local_idx, round_idx) != 0) {
                return -1;
            }
            broadcast_round_commit(sockfd, peers, node_count, local_idx, round_idx);
            fprintf(stderr, "[ub_obmm_pool] round=%d owner=%d commit -> ok\n",
                    round_idx + 1, round_idx + 1);
        } else {
            if (wait_for_round_msg(sockfd, MSG_ROUND_TURN, round_idx, local_idx,
                                   round_idx, 10000) != 0) {
                fprintf(stderr,
                        "[ub_obmm_pool] fail: round=%d node=%d timeout waiting TURN from owner=%d\n",
                        round_idx + 1, local_idx + 1, round_idx + 1);
                return -1;
            }
            if (wait_for_slot_payload(&slots[owner_slot], round_idx, owner_slot, 10000) != 0) {
                fprintf(stderr,
                        "[ub_obmm_pool] fail: round=%d verify owner=%d slot=%d timeout\n",
                        round_idx + 1, round_idx + 1, owner_slot + 1);
                return -1;
            }
            fprintf(stderr, "[ub_obmm_pool] round verify owner=%d -> ok slot=%d\n",
                    round_idx + 1, owner_slot + 1);
            send_round_ack(sockfd, &peers[round_idx], local_idx, round_idx, round_idx);
            fprintf(stderr, "[ub_obmm_pool] round=%d node=%d ACK -> owner=%d\n",
                    round_idx + 1, local_idx + 1, round_idx + 1);
            if (wait_for_round_msg(sockfd, MSG_ROUND_COMMIT, round_idx, local_idx,
                                   round_idx, 10000) != 0) {
                fprintf(stderr,
                        "[ub_obmm_pool] fail: round=%d node=%d timeout waiting COMMIT from owner=%d\n",
                        round_idx + 1, local_idx + 1, round_idx + 1);
                return -1;
            }
            fprintf(stderr, "[ub_obmm_pool] round=%d node=%d saw COMMIT from owner=%d\n",
                    round_idx + 1, local_idx + 1, round_idx + 1);
        }
    }

    fprintf(stderr, "[ub_obmm_pool] pool rounds -> ok count=%d\n", node_count);
    return 0;
}

static void cleanup_slots(int obmm_fd, int node_count, int local_idx,
                          struct pool_slot slots[MAX_NODES])
{
    int i;
    for (i = 0; i < node_count; i++) {
        if (slots[i].region.fd >= 0 || slots[i].region.addr) {
            unmap_region_device(&slots[i].region);
        }
    }
    for (i = 0; i < node_count; i++) {
        if (i == local_idx) {
            if (slots[i].mem_id != 0) {
                (void)do_unexport_region(obmm_fd, slots[i].mem_id);
            }
        } else {
            if (slots[i].mem_id != 0) {
                (void)do_unimport_region(obmm_fd, slots[i].mem_id);
            }
        }
    }
}

int main(void)
{
    char ifname[IFNAMSIZ];
    unsigned int ifindex = 0;
    char local_ip[INET_ADDRSTRLEN];
    char ips[MAX_NODES][INET_ADDRSTRLEN];
    int node_count = 0;
    int local_idx = -1;
    struct sockaddr_in peers[MAX_NODES];
    struct obmm_demo_meta metas[MAX_NODES];
    bool got_meta[MAX_NODES] = { false };
    struct pool_slot slots[MAX_NODES];
    struct obmm_demo_meta local_meta;
    struct in_addr local_addr;
    int sockfd = -1;
    int obmm_fd = -1;
    uint32_t local_cna = 0;
    uint64_t local_cna_u64 = 0;
    int i;
    int rc = 1;

    memset(slots, 0, sizeof(slots));
    memset(&local_meta, 0, sizeof(local_meta));

    signal(SIGALRM, alarm_handler);
    alarm(RUN_TIMEOUT_S);

    fprintf(stderr, "[ub_obmm_pool] start\n");

    if (!resolve_pool_nodes(local_ip, ips, &node_count, &local_idx)) {
        fprintf(stderr, "[ub_obmm_pool] fail: resolve pool nodes failed\n");
        return 1;
    }
    fprintf(stderr, "[ub_obmm_pool] node local=%d local_ip=%s node_count=%d\n",
            local_idx + 1, local_ip, node_count);

    if (!wait_iface_ready(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, "[ub_obmm_pool] fail: ipourma iface not ready\n");
        return 1;
    }
    if (!get_local_ipv4(ifname, &local_addr) || strcmp(inet_ntoa(local_addr), local_ip) != 0) {
        if (!set_ipv4_addr(ifname, local_ip)) {
            fprintf(stderr, "[ub_obmm_pool] fail: set local ip %s failed\n", local_ip);
            return 1;
        }
        if (!get_local_ipv4(ifname, &local_addr)) {
            fprintf(stderr, "[ub_obmm_pool] fail: unable to query local ip after set\n");
            return 1;
        }
    }
    for (i = 0; i < node_count; i++) {
        struct in_addr peer_addr;
        if (i == local_idx) {
            continue;
        }
        memset(&peers[i], 0, sizeof(peers[i]));
        peers[i].sin_family = AF_INET;
        peers[i].sin_port = htons(OBMM_PORT);
        inet_pton(AF_INET, ips[i], &peers[i].sin_addr);
        peer_addr = peers[i].sin_addr;
        install_static_arp(ifname, &peer_addr);
    }

    sockfd = create_udp_socket(ifname);
    if (sockfd < 0) {
        fprintf(stderr, "[ub_obmm_pool] fail: create socket failed\n");
        return 1;
    }

    obmm_fd = open_obmm();
    if (obmm_fd < 0) {
        fprintf(stderr, "[ub_obmm_pool] fail: open /dev/obmm failed: %s\n", strerror(errno));
        goto out;
    }

    if (!parse_hex_file_u64("/sys/bus/ub/devices/00001/primary_cna", &local_cna_u64)) {
        fprintf(stderr, "[ub_obmm_pool] fail: read primary_cna failed\n");
        goto out;
    }
    local_cna = (uint32_t)local_cna_u64;
    local_meta.export_cna = local_cna;
    if (do_export_region(obmm_fd, &local_meta) != 0) {
        goto out;
    }
    slots[local_idx].owner_idx = local_idx;
    slots[local_idx].is_local = true;
    slots[local_idx].mem_id = local_meta.export_mem_id;
    slots[local_idx].export_cna = local_cna;
    if (map_region_device(local_meta.export_mem_id, SLOT_TOUCH_SIZE, false,
                          &slots[local_idx].region) != 0) {
        goto out;
    }

    if (broadcast_hello_until_all(sockfd, peers, node_count, local_idx, &local_meta, metas, got_meta) != 0) {
        goto out;
    }
    fprintf(stderr, "[ub_obmm_pool] metadata exchange -> ok count=%d\n", node_count);

    if (import_all_peers(obmm_fd, local_cna, node_count, local_idx, metas, slots) != 0) {
        goto out;
    }
    if (wait_until_everyone_ready(sockfd, peers, node_count, local_idx) != 0) {
        goto out;
    }
    fprintf(stderr, "[ub_obmm_pool] pool ready -> ok nodes=%d\n", node_count);
    usleep(500000);

    if (do_rounds(sockfd, peers, node_count, local_idx, slots) != 0) {
        goto out;
    }

    fprintf(stderr, "[ub_obmm_pool] pass\n");
    rc = 0;

out:
    cleanup_slots(obmm_fd, node_count, local_idx, slots);
    if (obmm_fd >= 0) {
        close(obmm_fd);
    }
    if (sockfd >= 0) {
        close(sockfd);
    }
    return rc;
}
