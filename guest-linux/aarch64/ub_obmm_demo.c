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
#define RUN_TIMEOUT_S 40
#define WAIT_IFACE_MS 90000
#define EXPORT_REGION_SIZE (2UL * 1024UL * 1024UL)
#define DEMO_SIZE (256UL * 1024UL)
#define IMPORT_ALIGN 0x1000UL
#define DEMO_PAYLOAD_A "obmm-export-payload-from-nodeA"
#define DEMO_PAYLOAD_B "obmm-import-payload-from-nodeB"
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

static bool cmdline_get_value(const char *key, char *out, size_t out_len)
{
    char buf[2048];
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

static bool role_default_ipv4_pair(const char *role, char *local, size_t local_len,
                                   char *peer, size_t peer_len)
{
    if (strcmp(role, "nodeA") == 0) {
        snprintf(local, local_len, "%s", "10.0.0.1");
        snprintf(peer, peer_len, "%s", "10.0.0.2");
        return true;
    }
    if (strcmp(role, "nodeB") == 0) {
        snprintf(local, local_len, "%s", "10.0.0.2");
        snprintf(peer, peer_len, "%s", "10.0.0.1");
        return true;
    }
    return false;
}

static bool resolve_ipv4_pair(const char *role, char *local, size_t local_len,
                              char *peer, size_t peer_len)
{
    bool have_local = cmdline_get_value("linqu_ipourma_ipv4", local, local_len);
    bool have_peer = cmdline_get_value("linqu_ipourma_peer_ipv4", peer, peer_len);

    if (!have_local || !have_peer) {
        char default_local[INET_ADDRSTRLEN];
        char default_peer[INET_ADDRSTRLEN];

        if (!role_default_ipv4_pair(role, default_local, sizeof(default_local),
                                    default_peer, sizeof(default_peer))) {
            return false;
        }
        if (!have_local) {
            snprintf(local, local_len, "%s", default_local);
            have_local = true;
        }
        if (!have_peer) {
            snprintf(peer, peer_len, "%s", default_peer);
            have_peer = true;
        }
    }

    if (!have_local || !have_peer) {
        return false;
    }

    if (inet_pton(AF_INET, local, &(struct in_addr){0}) != 1 ||
        inet_pton(AF_INET, peer, &(struct in_addr){0}) != 1) {
        return false;
    }

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
    struct sockaddr_in bind_addr;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        return -1;
    }

    (void)setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    (void)setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname));

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

static bool parse_hex_file_u64(const char *path, uint64_t *value)
{
    char buf[256];
    char *end = NULL;
    unsigned long long v;

    if (!read_file(path, buf, sizeof(buf))) {
        return false;
    }

    errno = 0;
    v = strtoull(buf, &end, 0);
    if (errno != 0 || end == buf) {
        return false;
    }
    *value = (uint64_t)v;
    return true;
}

static bool find_import_window(uint64_t needed_size, uint64_t *local_pa_out,
                               uint64_t *window_size_out)
{
    FILE *fp;
    char line[256];
    unsigned long long mar = 0;
    unsigned long long decode = 0;
    unsigned long long cc_base_mb = 0;
    unsigned long long cc_size_mb = 0;
    unsigned long long nc_base_mb = 0;
    unsigned long long nc_size_mb = 0;

    fp = fopen("/sys/bus/ub/devices/00001/mem_windows", "r");
    if (!fp) {
        fprintf(stderr, "[ub_obmm] open mem_windows failed: %s\n", strerror(errno));
        return false;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        int matched;
        uint64_t base_pa;
        uint64_t size_bytes;

        matched = sscanf(line,
                         "mar%llu decode=%llx cc_base_mb=%llx cc_size_mb=%llx nc_base_mb=%llx nc_size_mb=%llx",
                         &mar, &decode, &cc_base_mb, &cc_size_mb,
                         &nc_base_mb, &nc_size_mb);
        if (matched != 6) {
            continue;
        }

        base_pa = ((uint64_t)cc_base_mb) << 20;
        size_bytes = ((uint64_t)cc_size_mb) << 20;
        fprintf(stderr,
                "[ub_obmm] mem_window mar=%llu decode=%#llx cc=[%#" PRIx64 ",%#" PRIx64 "] nc_base_mb=%#llx nc_size_mb=%#llx\n",
                mar, decode, base_pa, size_bytes, nc_base_mb, nc_size_mb);

        if (size_bytes < needed_size) {
            continue;
        }
        if ((base_pa & ((2ULL * 1024ULL * 1024ULL) - 1)) != 0) {
            continue;
        }

        fclose(fp);
        *local_pa_out = base_pa;
        *window_size_out = size_bytes;
        return true;
    }

    fclose(fp);
    return false;
}

static uint64_t align_up_u64(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}

static int open_obmm(void)
{
    return open("/dev/obmm", O_RDWR);
}

static int open_region_dev(uint64_t mem_id)
{
    char path[128];

    snprintf(path, sizeof(path), "/dev/obmm_shmdev%" PRIu64, mem_id);
    return open(path, O_RDWR);
}

static int map_region_device(uint64_t mem_id, size_t len, struct mapped_region *region)
{
    memset(region, 0, sizeof(*region));
    region->fd = -1;
    region->mem_id = mem_id;
    region->len = len;

    region->fd = open_region_dev(mem_id);
    if (region->fd < 0) {
        fprintf(stderr, "[ub_obmm] open shmdev%" PRIu64 " failed: %s\n",
                mem_id, strerror(errno));
        return -1;
    }

    region->addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, region->fd, 0);
    if (region->addr == MAP_FAILED) {
        fprintf(stderr, "[ub_obmm] mmap shmdev%" PRIu64 " failed: %s\n",
                mem_id, strerror(errno));
        close(region->fd);
        region->fd = -1;
        region->addr = NULL;
        return -1;
    }

    fprintf(stderr, "[ub_obmm] shmdev open/mmap -> ok mem_id=%" PRIu64 "\n", mem_id);
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

static int stamp_payload(struct mapped_region *region, const char *payload)
{
    size_t payload_len = strlen(payload) + 1;

    if (payload_len > region->len) {
        fprintf(stderr, "[ub_obmm] payload too large for shmdev%" PRIu64 "\n",
                region->mem_id);
        return -1;
    }

    memset(region->addr, 0, payload_len);
    memcpy(region->addr, payload, payload_len);
    __sync_synchronize();
    if (msync(region->addr, region->len, MS_SYNC) != 0) {
        if (errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP) {
            fprintf(stderr,
                    "[ub_obmm] msync shmdev%" PRIu64 " unsupported (%s), continue\n",
                    region->mem_id, strerror(errno));
            return 0;
        }
        fprintf(stderr, "[ub_obmm] msync shmdev%" PRIu64 " failed: %s\n",
                region->mem_id, strerror(errno));
        return -1;
    }
    return 0;
}

static bool region_matches_payload(const struct mapped_region *region, const char *payload)
{
    return memcmp(region->addr, payload, strlen(payload) + 1) == 0;
}

static int wait_for_payload(const struct mapped_region *region, const char *payload,
                            long timeout_ms, const char *label)
{
    long deadline = now_ms() + timeout_ms;

    while (!g_alarm_fired && now_ms() < deadline) {
        if (region_matches_payload(region, payload)) {
            fprintf(stderr, "[ub_obmm] %s -> ok payload=\"%s\"\n", label, payload);
            return 0;
        }
        usleep(100000);
    }

    fprintf(stderr, "[ub_obmm] fail: timeout waiting for %s payload=\"%s\"\n",
            label, payload);
    return -1;
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
        fprintf(stderr, "[ub_obmm] export failed: %s\n", strerror(errno));
        return -1;
    }

    meta->export_mem_id = cmd.mem_id;
    meta->remote_uba = cmd.uba;
    meta->size = EXPORT_REGION_SIZE;
    meta->token_id = cmd.tokenid;
    fprintf(stderr,
            "[ub_obmm] export -> ok mem_id=%" PRIu64 " uba=%#" PRIx64 " token=%u\n",
            meta->export_mem_id, meta->remote_uba, meta->token_id);
    return 0;
}

static int do_unexport_region(int obmm_fd, uint64_t mem_id)
{
    struct obmm_cmd_unexport cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.mem_id = mem_id;
    if (ioctl(obmm_fd, OBMM_CMD_UNEXPORT, &cmd) != 0) {
        fprintf(stderr, "[ub_obmm] unexport failed mem_id=%" PRIu64 ": %s\n",
                mem_id, strerror(errno));
        return -1;
    }

    fprintf(stderr, "[ub_obmm] unexport -> ok mem_id=%" PRIu64 "\n", mem_id);
    return 0;
}

static int do_import_region(int obmm_fd, const struct obmm_demo_meta *meta,
                            uint32_t local_cna, uint64_t local_pa,
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
                "[ub_obmm] import failed pa=%#" PRIx64 " uba=%#" PRIx64
                " token=%u scna=%#x dcna=%#x: %s\n",
                local_pa, meta->remote_uba, meta->token_id,
                local_cna, meta->export_cna, strerror(errno));
        return -1;
    }

    *import_mem_id = cmd.mem_id;
    fprintf(stderr,
            "[ub_obmm] import -> ok mem_id=%" PRIu64 " local_pa=%#" PRIx64
            " local_cna=%#x remote_cna=%#x\n",
            *import_mem_id, local_pa, local_cna, meta->export_cna);
    return 0;
}

static int do_unimport_region(int obmm_fd, uint64_t mem_id)
{
    struct obmm_cmd_unimport cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.mem_id = mem_id;
    if (ioctl(obmm_fd, OBMM_CMD_UNIMPORT, &cmd) != 0) {
        fprintf(stderr, "[ub_obmm] unimport failed mem_id=%" PRIu64 ": %s\n",
                mem_id, strerror(errno));
        return -1;
    }

    fprintf(stderr, "[ub_obmm] unimport -> ok mem_id=%" PRIu64 "\n", mem_id);
    return 0;
}

static int send_msg(int sockfd, const struct sockaddr_in *peer,
                    const void *buf, size_t len)
{
    ssize_t n = sendto(sockfd, buf, len, MSG_DONTWAIT,
                       (const struct sockaddr *)peer, sizeof(*peer));
    return (n == (ssize_t)len) ? 0 : -1;
}

static ssize_t recv_msg(int sockfd, void *buf, size_t len, struct sockaddr_in *from)
{
    socklen_t fromlen = sizeof(*from);
    return recvfrom(sockfd, buf, len, MSG_DONTWAIT,
                    (struct sockaddr *)from, &fromlen);
}

static int run_nodeA(int sockfd, const struct sockaddr_in *peer,
                     int obmm_fd, uint32_t local_cna)
{
    struct obmm_demo_meta meta;
    struct mapped_region region;
    long deadline = now_ms() + RUN_TIMEOUT_S * 1000L;
    char ack[64];
    struct sockaddr_in from;

    memset(&meta, 0, sizeof(meta));
    memset(&region, 0, sizeof(region));
    region.fd = -1;
    meta.export_cna = local_cna;

    if (do_export_region(obmm_fd, &meta) != 0) {
        return 1;
    }
    if (map_region_device(meta.export_mem_id, DEMO_SIZE, &region) != 0) {
        (void)do_unexport_region(obmm_fd, meta.export_mem_id);
        return 1;
    }
    while (!g_alarm_fired && now_ms() < deadline) {
        if (send_msg(sockfd, peer, &meta, sizeof(meta)) == 0) {
            ssize_t n;

            usleep(100000);
            n = recv_msg(sockfd, ack, sizeof(ack) - 1, &from);
            if (n > 0) {
                ack[n] = '\0';
                if (strcmp(ack, "IMPORT_READY") == 0) {
                    fprintf(stderr, "[ub_obmm] sync: nodeB import ready\n");
                    break;
                }
            }
        }
        usleep(200000);
    }

    if (g_alarm_fired || now_ms() >= deadline) {
        fprintf(stderr, "[ub_obmm] fail: timeout waiting for IMPORT_READY\n");
        unmap_region_device(&region);
        (void)do_unexport_region(obmm_fd, meta.export_mem_id);
        return 1;
    }

    if (stamp_payload(&region, DEMO_PAYLOAD_A) != 0) {
        unmap_region_device(&region);
        (void)do_unexport_region(obmm_fd, meta.export_mem_id);
        return 1;
    }

    if (send_msg(sockfd, peer, "WRITE_A_DONE", strlen("WRITE_A_DONE")) != 0) {
        fprintf(stderr, "[ub_obmm] fail: send WRITE_A_DONE failed\n");
        unmap_region_device(&region);
        (void)do_unexport_region(obmm_fd, meta.export_mem_id);
        return 1;
    }

    deadline = now_ms() + RUN_TIMEOUT_S * 1000L;
    while (!g_alarm_fired && now_ms() < deadline) {
        ssize_t n = recv_msg(sockfd, ack, sizeof(ack) - 1, &from);
        if (n > 0) {
            ack[n] = '\0';
            if (strcmp(ack, "IMPORT_OK") == 0) {
                fprintf(stderr, "[ub_obmm] sync: nodeB import acknowledged\n");
                break;
            }
        }
        usleep(200000);
    }

    if (g_alarm_fired || now_ms() >= deadline) {
        fprintf(stderr, "[ub_obmm] fail: timeout waiting for IMPORT_OK\n");
        unmap_region_device(&region);
        (void)do_unexport_region(obmm_fd, meta.export_mem_id);
        return 1;
    }

    if (wait_for_payload(&region, DEMO_PAYLOAD_B, 5000, "nodeA verify nodeB write") != 0) {
        unmap_region_device(&region);
        (void)do_unexport_region(obmm_fd, meta.export_mem_id);
        return 1;
    }

    if (send_msg(sockfd, peer, "WRITEBACK_OK", strlen("WRITEBACK_OK")) != 0) {
        fprintf(stderr, "[ub_obmm] fail: send WRITEBACK_OK failed\n");
        unmap_region_device(&region);
        (void)do_unexport_region(obmm_fd, meta.export_mem_id);
        return 1;
    }

    deadline = now_ms() + RUN_TIMEOUT_S * 1000L;
    while (!g_alarm_fired && now_ms() < deadline) {
        ssize_t n = recv_msg(sockfd, ack, sizeof(ack) - 1, &from);
        if (n > 0) {
            ack[n] = '\0';
            if (strcmp(ack, "UNIMPORT_OK") == 0) {
                fprintf(stderr, "[ub_obmm] sync: nodeB unimport acknowledged\n");
                break;
            }
        }
        usleep(200000);
    }

    if (g_alarm_fired || now_ms() >= deadline) {
        fprintf(stderr, "[ub_obmm] fail: timeout waiting for UNIMPORT_OK\n");
        unmap_region_device(&region);
        (void)do_unexport_region(obmm_fd, meta.export_mem_id);
        return 1;
    }

    unmap_region_device(&region);
    if (do_unexport_region(obmm_fd, meta.export_mem_id) != 0) {
        return 1;
    }

    fprintf(stderr, "[ub_obmm] pass\n");
    return 0;
}

static int run_nodeB(int sockfd, const struct sockaddr_in *peer, int obmm_fd,
                     uint32_t local_cna)
{
    struct obmm_demo_meta meta;
    struct mapped_region region;
    long deadline = now_ms() + RUN_TIMEOUT_S * 1000L;
    long writeback_deadline;
    char ack[64];
    struct sockaddr_in from;
    uint64_t local_pa = 0, window_size = 0, import_mem_id = 0;

    memset(&region, 0, sizeof(region));
    region.fd = -1;

    while (!g_alarm_fired && now_ms() < deadline) {
        ssize_t n = recv_msg(sockfd, &meta, sizeof(meta), &from);
        if (n == (ssize_t)sizeof(meta)) {
            break;
        }
        usleep(200000);
    }

    if (g_alarm_fired || now_ms() >= deadline) {
        fprintf(stderr, "[ub_obmm] fail: timeout waiting for export metadata\n");
        return 1;
    }

    if (!find_import_window(meta.size, &local_pa, &window_size)) {
        fprintf(stderr, "[ub_obmm] fail: unable to find import window\n");
        return 1;
    }
    local_pa = align_up_u64(local_pa, IMPORT_ALIGN);
    fprintf(stderr, "[ub_obmm] import local_pa=%#" PRIx64 " window_size=%#" PRIx64 "\n",
            local_pa, window_size);

    if (do_import_region(obmm_fd, &meta, local_cna, local_pa, &import_mem_id) != 0) {
        return 1;
    }
    if (map_region_device(import_mem_id, DEMO_SIZE, &region) != 0) {
        (void)do_unimport_region(obmm_fd, import_mem_id);
        return 1;
    }

    if (send_msg(sockfd, peer, "IMPORT_READY", strlen("IMPORT_READY")) != 0) {
        fprintf(stderr, "[ub_obmm] fail: send IMPORT_READY failed\n");
        unmap_region_device(&region);
        (void)do_unimport_region(obmm_fd, import_mem_id);
        return 1;
    }

    deadline = now_ms() + RUN_TIMEOUT_S * 1000L;
    while (!g_alarm_fired && now_ms() < deadline) {
        ssize_t n = recv_msg(sockfd, ack, sizeof(ack) - 1, &from);
        if (n > 0) {
            ack[n] = '\0';
            if (strcmp(ack, "WRITE_A_DONE") == 0) {
                fprintf(stderr, "[ub_obmm] sync: nodeA write published\n");
                break;
            }
        }
        usleep(200000);
    }

    if (g_alarm_fired || now_ms() >= deadline) {
        fprintf(stderr, "[ub_obmm] fail: timeout waiting for WRITE_A_DONE\n");
        unmap_region_device(&region);
        (void)do_unimport_region(obmm_fd, import_mem_id);
        return 1;
    }

    if (wait_for_payload(&region, DEMO_PAYLOAD_A, 5000, "nodeB verify nodeA write") != 0) {
        unmap_region_device(&region);
        (void)do_unimport_region(obmm_fd, import_mem_id);
        return 1;
    }

    if (stamp_payload(&region, DEMO_PAYLOAD_B) != 0) {
        unmap_region_device(&region);
        (void)do_unimport_region(obmm_fd, import_mem_id);
        return 1;
    }

    if (send_msg(sockfd, peer, "IMPORT_OK", strlen("IMPORT_OK")) != 0) {
        fprintf(stderr, "[ub_obmm] fail: send IMPORT_OK failed\n");
        unmap_region_device(&region);
        (void)do_unimport_region(obmm_fd, import_mem_id);
        return 1;
    }

    writeback_deadline = now_ms() + RUN_TIMEOUT_S * 1000L;
    while (!g_alarm_fired && now_ms() < writeback_deadline) {
        ssize_t n = recv_msg(sockfd, ack, sizeof(ack) - 1, &from);
        if (n > 0) {
            ack[n] = '\0';
            if (strcmp(ack, "WRITEBACK_OK") == 0) {
                fprintf(stderr, "[ub_obmm] sync: nodeA writeback acknowledged\n");
                break;
            }
        }
        usleep(200000);
    }

    if (g_alarm_fired || now_ms() >= writeback_deadline) {
        fprintf(stderr, "[ub_obmm] fail: timeout waiting for WRITEBACK_OK\n");
        unmap_region_device(&region);
        (void)do_unimport_region(obmm_fd, import_mem_id);
        return 1;
    }

    unmap_region_device(&region);
    if (do_unimport_region(obmm_fd, import_mem_id) != 0) {
        return 1;
    }

    if (send_msg(sockfd, peer, "UNIMPORT_OK", strlen("UNIMPORT_OK")) != 0) {
        fprintf(stderr, "[ub_obmm] fail: send UNIMPORT_OK failed\n");
        return 1;
    }

    fprintf(stderr, "[ub_obmm] pass\n");
    return 0;
}

int main(void)
{
    char role[32] = "unknown";
    char ifname[IFNAMSIZ] = {0};
    char my_ip[INET_ADDRSTRLEN] = {0};
    char peer_ip[INET_ADDRSTRLEN] = {0};
    struct in_addr local_addr = {0};
    struct in_addr desired_local = {0};
    struct in_addr peer_addr;
    struct sockaddr_in peer_sockaddr;
    unsigned int ifindex = 0;
    uint64_t local_cna = 0;
    int sockfd = -1;
    int obmm_fd = -1;
    int rc = 1;
    struct sigaction sa;

    if (!cmdline_get_value("linqu_urma_dp_role", role, sizeof(role))) {
        fprintf(stderr, "[ub_obmm] fail: missing linqu_urma_dp_role in cmdline\n");
        return 1;
    }

    if (!resolve_ipv4_pair(role, my_ip, sizeof(my_ip), peer_ip, sizeof(peer_ip))) {
        fprintf(stderr, "[ub_obmm] fail: missing ip config for role=%s\n", role);
        return 1;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigaction(SIGALRM, &sa, NULL);
    alarm(RUN_TIMEOUT_S);

    printf("[ub_obmm] start\n");
    printf("[ub_obmm] role=%s\n", role);

    if (!wait_iface_ready(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, "[ub_obmm] fail: ipourma iface not ready\n");
        goto out;
    }
    inet_pton(AF_INET, my_ip, &desired_local);
    if (!get_local_ipv4(ifname, &local_addr) || local_addr.s_addr != desired_local.s_addr) {
        fprintf(stderr, "[ub_obmm] warn: bootstrap ipv4 missing or mismatched on %s, applying %s\n",
                ifname, my_ip);
        if (!set_ipv4_addr(ifname, my_ip)) {
            fprintf(stderr, "[ub_obmm] fail: set_ipv4 failed\n");
            goto out;
        }
    }
    if (inet_pton(AF_INET, peer_ip, &peer_addr) != 1) {
        fprintf(stderr, "[ub_obmm] fail: inet_pton peer failed\n");
        goto out;
    }
    install_static_arp(ifname, &peer_addr);

    if (!parse_hex_file_u64("/sys/bus/ub/devices/00001/primary_cna", &local_cna)) {
        fprintf(stderr, "[ub_obmm] fail: read primary_cna failed\n");
        goto out;
    }

    sockfd = create_udp_socket(ifname);
    if (sockfd < 0) {
        fprintf(stderr, "[ub_obmm] fail: create socket failed\n");
        goto out;
    }

    memset(&peer_sockaddr, 0, sizeof(peer_sockaddr));
    peer_sockaddr.sin_family = AF_INET;
    peer_sockaddr.sin_port = htons(OBMM_PORT);
    peer_sockaddr.sin_addr = peer_addr;

    obmm_fd = open_obmm();
    if (obmm_fd < 0) {
        fprintf(stderr, "[ub_obmm] fail: open /dev/obmm failed: %s\n", strerror(errno));
        goto out;
    }

    if (strcmp(role, "nodeA") == 0) {
        rc = run_nodeA(sockfd, &peer_sockaddr, obmm_fd, (uint32_t)local_cna);
    } else {
        rc = run_nodeB(sockfd, &peer_sockaddr, obmm_fd, (uint32_t)local_cna);
    }

out:
    if (obmm_fd >= 0) {
        close(obmm_fd);
    }
    if (sockfd >= 0) {
        close(sockfd);
    }
    if (rc != 0) {
        printf("[ub_obmm] fail\n");
    }
    return rc;
}
