/*
 * ub_rdma_demo.c - Dual-node URMA RDMA resource lifecycle demo.
 *
 * Demonstrates the full URMA resource creation and binding pipeline
 * over a simulated UB network between two QEMU VMs (nodeA/nodeB).
 *
 * Steps:
 *   1. Query device attributes
 *   2. Create context
 *   3. Create + active JFC
 *   4. Create + active JFR
 *   5. (Optional) standalone Create + active JFS probe
 *   6. Create + active Jetty
 *   7. Register memory segment
 *   8. Exchange info with peer, import jetty
 *   9. Bind jetty
 *
 * Exit codes:
 *   0 - success or skip (no device)
 *   1 - failure
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
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
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../kernel_ub/include/uapi/linux/ummu_core.h"
#include "uburma_cmd_user_compat.h"

/* ---------- constants ---------- */

#define RDMA_PORT       18558
#define SYNC_PORT       18559
#define WAIT_IFACE_MS   90000
#define SYNC_TIMEOUT_MS 30000
#define DEMO_PAGE_SIZE  4096
#define RDMA_Q_DEPTH    64
#define RDMA_CQ_SHIFT   6
#define RDMA_MAX_PAYLOAD 256
#define UDMA_JFC_DB_VALID_OWNER_M 1
#define UDMA_JFC_DB_CI_IDX_M 0x003fffffU
#define UDMA_JFR_DB_PI_M 0x0000ffffU
#define UDMA_SRC_IDX_SHIFT 16
#define UDMA_MMAP_JETTY_DSQE_LOCAL 2
#define UDMA_DOORBELL_OFFSET_LOCAL 0x80
#define MAP_INDEX_SHIFT_LOCAL 4

enum {
    CQE_FOR_SEND_LOCAL = 0,
    CQE_FOR_RECEIVE_LOCAL = 1,
};

/* ---------- resource tracking ---------- */

struct rdma_resources {
    int fd;
    int ummu_fd;
    uint32_t jfc_id;
    uint32_t jfr_id;
    uint32_t jfs_id;
    uint32_t jetty_id;
    uint32_t ummu_tid;
    uint64_t jfc_handle;
    uint64_t jfr_handle;
    uint64_t jfs_handle;
    uint64_t jetty_handle;
    uint64_t tjetty_handle;
    uint32_t token_id;
    uint64_t token_id_handle;
    uint64_t seg_handle;
    bool ctx_created;
    bool ummu_tid_allocated;
    bool jfc_alloc;
    bool jfr_alloc;
    bool jfs_alloc;
    bool jetty_alloc;
    bool seg_registered;
    bool tjetty_imported;
    uint32_t local_hw_jetty_id;
    uint32_t bound_tpn;
    uint8_t peer_eid[UBCORE_EID_SIZE];
    uint32_t peer_jetty_id;
    uint32_t sq_pi;
    uint32_t rq_pi;
    uint32_t cq_ci;
    void *seg_buf;
    size_t seg_len;
    void *jfc_ucmd_buf;
    size_t jfc_ucmd_len;
    bool jfc_ucmd_is_mmap;
    void *jfc_db_buf;
    size_t jfc_db_len;
    bool jfc_db_is_mmap;
    void *jfr_buf;
    size_t jfr_buf_len;
    bool jfr_buf_is_mmap;
    void *jfr_idx_buf;
    size_t jfr_idx_buf_len;
    bool jfr_idx_buf_is_mmap;
    void *jfr_db_buf;
    size_t jfr_db_buf_len;
    bool jfr_db_buf_is_mmap;
    void *jfs_buf;
    void *jfs_db_buf;
    void *jetty_buf;
    size_t jetty_buf_len;
    bool jetty_buf_is_mmap;
    void *jetty_db_buf;
    void *jetty_dsqe_page;
    size_t jetty_dsqe_len;
    bool jetty_dsqe_is_mmap;
};

static struct rdma_resources g_res;

enum udma_sq_opcode_local {
    UDMA_OPC_SEND_LOCAL = 0x0,
};

struct udma_sqe_ctl_local {
    uint32_t sqe_bb_idx : 16;
    uint32_t place_odr : 2;
    uint32_t comp_order : 1;
    uint32_t fence : 1;
    uint32_t se : 1;
    uint32_t cqe : 1;
    uint32_t inline_en : 1;
    uint32_t rsv : 5;
    uint32_t token_en : 1;
    uint32_t rmt_jetty_type : 2;
    uint32_t owner : 1;
    uint32_t target_hint : 8;
    uint32_t opcode : 8;
    uint32_t rsv1 : 6;
    uint32_t inline_msg_len : 10;
    uint32_t tpn : 24;
    uint32_t sge_num : 8;
    uint32_t rmt_obj_id : 20;
    uint32_t rsv2 : 12;
    uint8_t rmt_eid[UBCORE_EID_SIZE];
    uint32_t rmt_token_value;
    uint32_t rsv3;
    uint32_t rmt_addr_l_or_token_id;
    uint32_t rmt_addr_h_or_token_value;
};

struct udma_normal_sge_local {
    uint32_t length;
    uint32_t token_id;
    uint64_t va;
};

struct udma_wqe_sge_local {
    uint32_t length;
    uint32_t token_id;
    uint64_t va;
};

struct udma_jfc_cqe_local {
    uint32_t s_r : 1;
    uint32_t is_jetty : 1;
    uint32_t owner : 1;
    uint32_t inline_en : 1;
    uint32_t opcode : 3;
    uint32_t fd : 1;
    uint32_t rsv : 8;
    uint32_t substatus : 8;
    uint32_t status : 8;
    uint32_t entry_idx : 16;
    uint32_t local_num_l : 16;
    uint32_t local_num_h : 4;
    uint32_t rmt_idx : 20;
    uint32_t rsv1 : 8;
    uint32_t tpn : 24;
    uint32_t rsv2 : 8;
    uint32_t byte_cnt;
    uint32_t user_data_l;
    uint32_t user_data_h;
    uint32_t rmt_eid[4];
    uint32_t data_l;
    uint32_t data_h;
    uint32_t inline_data[3];
};

struct uburma_cmd_unbind_jetty_local {
    struct {
        uint64_t jetty_handle;
        uint64_t tjetty_handle;
    } in;
};

struct uburma_cmd_unimport_jetty_local {
    struct {
        uint64_t handle;
    } in;
};

/* ---------- STEP_CHECK macro ---------- */

#define STEP_CHECK(step_num, name, expr) \
    do { \
        int ret = (expr); \
        if (ret < 0) { \
            fprintf(stderr, "[ub_rdma] step %d: %s failed: %d\n", \
                    step_num, name, ret); \
            cleanup_resources(&g_res); \
            fprintf(stderr, "[ub_rdma] fail\n"); \
            return 1; \
        } \
        printf("[ub_rdma] step %d: %s -> ok\n", step_num, name); \
    } while (0)

/* ---------- forward declarations ---------- */

static void cleanup_resources(struct rdma_resources *res);

/* ---------- helper: time ---------- */

static long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void local_wmb(void)
{
    __sync_synchronize();
}

static void reverse_bytes(const uint8_t *src, uint8_t *dst, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        dst[i] = src[len - i - 1];
    }
}

static uint32_t rdma_cq_valid_owner(uint32_t ci)
{
    return (ci >> RDMA_CQ_SHIFT) & UDMA_JFC_DB_VALID_OWNER_M;
}

static struct udma_jfc_cqe_local *rdma_get_cqe(struct rdma_resources *res, uint32_t ci)
{
    struct udma_jfc_cqe_local *ring = res->jfc_ucmd_buf;
    struct udma_jfc_cqe_local *cqe = &ring[ci & (RDMA_Q_DEPTH - 1)];

    if (!(cqe->owner ^ rdma_cq_valid_owner(ci))) {
        return NULL;
    }
    return cqe;
}

static void rdma_consume_cqe(struct rdma_resources *res)
{
    uint32_t *db = res->jfc_db_buf;

    res->cq_ci++;
    local_wmb();
    *db = res->cq_ci & UDMA_JFC_DB_CI_IDX_M;
}

static int rdma_poll_one_cqe(struct rdma_resources *res, long timeout_ms,
                             struct udma_jfc_cqe_local *out)
{
    long deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        struct udma_jfc_cqe_local *cqe = rdma_get_cqe(res, res->cq_ci);

        if (cqe != NULL) {
            memcpy(out, cqe, sizeof(*out));
            rdma_consume_cqe(res);
            return 0;
        }
        usleep(1000);
    }
    return -ETIMEDOUT;
}

static int rdma_wait_for_cqe(struct rdma_resources *res, long timeout_ms,
                             uint32_t expect_s_r,
                             struct udma_jfc_cqe_local *out)
{
    long deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        int ret = rdma_poll_one_cqe(res, 50, out);

        if (ret == -ETIMEDOUT) {
            continue;
        }
        if (ret < 0) {
            return ret;
        }
        if (out->status != 0) {
            fprintf(stderr,
                    "[ub_rdma] cqe error: s_r=%u opcode=%u status=%u substatus=%u local=%u remote=%u\n",
                    out->s_r, out->opcode, out->status, out->substatus,
                    (out->local_num_h << UDMA_SRC_IDX_SHIFT) | out->local_num_l,
                    out->rmt_idx);
            return -EIO;
        }
        if (out->s_r == expect_s_r) {
            return 0;
        }
    }
    return -ETIMEDOUT;
}

static int rdma_post_recv_one(struct rdma_resources *res, void *buf, uint32_t len)
{
    struct udma_wqe_sge_local *rq = res->jfr_buf;
    uint32_t *idx_ring = res->jfr_idx_buf;
    uint32_t *db = res->jfr_db_buf;
    uint32_t slot = res->rq_pi & (RDMA_Q_DEPTH - 1);

    rq[slot].length = len;
    rq[slot].token_id = 0;
    rq[slot].va = (uint64_t)(uintptr_t)buf;
    idx_ring[slot] = slot;
    res->rq_pi++;
    local_wmb();
    *db = res->rq_pi & UDMA_JFR_DB_PI_M;
    return 0;
}

static int rdma_post_send_one(struct rdma_resources *res, const void *buf, uint32_t len)
{
    struct udma_sqe_ctl_local *sqe;
    struct udma_normal_sge_local *sge;
    uint32_t *db = res->jetty_db_buf;
    uint32_t slot = res->sq_pi & (RDMA_Q_DEPTH - 1);

    if (len == 0 || len > RDMA_MAX_PAYLOAD) {
        return -EINVAL;
    }

    sqe = (struct udma_sqe_ctl_local *)((uint8_t *)res->jetty_buf + slot * 64);
    memset(sqe, 0, 64);
    sge = (struct udma_normal_sge_local *)(sqe + 1);

    sqe->sqe_bb_idx = res->sq_pi;
    sqe->cqe = 1;
    sqe->owner = ((res->sq_pi & RDMA_Q_DEPTH) == 0) ? 1 : 0;
    sqe->opcode = UDMA_OPC_SEND_LOCAL;
    sqe->tpn = res->bound_tpn;
    sqe->sge_num = 1;
    sqe->rmt_obj_id = res->peer_jetty_id;
    sqe->rmt_jetty_type = UBCORE_JETTY_USER;
    memcpy(sqe->rmt_eid, res->peer_eid, sizeof(res->peer_eid));

    sge->length = len;
    sge->token_id = 0;
    sge->va = (uint64_t)(uintptr_t)buf;

    res->sq_pi++;
    local_wmb();
    *db = res->sq_pi;
    return 0;
}

static int sync_datapath_ready(int udp_fd, const char *role,
                               const struct sockaddr_in *peer)
{
    static const char ready_msg[] = "RDMA_DEMO_READY";
    static const char ack_msg[] = "RDMA_DEMO_READY_ACK";
    char buf[64];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    ssize_t n;

    if (strcmp(role, "nodeA") == 0) {
        if (sendto(udp_fd, ready_msg, sizeof(ready_msg), 0,
                   (const struct sockaddr *)peer, sizeof(*peer)) < 0) {
            return -errno;
        }
        n = recvfrom(udp_fd, buf, sizeof(buf), 0,
                     (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            return -errno;
        }
        if ((size_t)n != sizeof(ack_msg) ||
            memcmp(buf, ack_msg, sizeof(ack_msg)) != 0) {
            return -EPROTO;
        }
    } else {
        n = recvfrom(udp_fd, buf, sizeof(buf), 0,
                     (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            return -errno;
        }
        if ((size_t)n != sizeof(ready_msg) ||
            memcmp(buf, ready_msg, sizeof(ready_msg)) != 0) {
            return -EPROTO;
        }
        if (sendto(udp_fd, ack_msg, sizeof(ack_msg), 0,
                   (const struct sockaddr *)peer, sizeof(*peer)) < 0) {
            return -errno;
        }
    }
    return 0;
}

/* ---------- helper: file read ---------- */

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

static int parse_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool read_eid_from_sysfs(uint8_t eid[UBCORE_EID_SIZE])
{
    char raw[256];
    char hex[UBCORE_EID_SIZE * 2];
    size_t i;
    size_t h = 0;

    if (!read_file("/sys/class/uburma/uburma0/device/eid", raw, sizeof(raw))) {
        return false;
    }

    for (i = 0; raw[i] != '\0' && h < sizeof(hex); i++) {
        if (isxdigit((unsigned char)raw[i])) {
            hex[h++] = raw[i];
        }
    }
    if (h < sizeof(hex)) {
        return false;
    }

    for (i = 0; i < UBCORE_EID_SIZE; i++) {
        int hi = parse_hex_nibble(hex[i * 2]);
        int lo = parse_hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        eid[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void *alloc_aligned_zero(size_t size)
{
    void *p = NULL;

    if (posix_memalign(&p, DEMO_PAGE_SIZE, size) != 0 || p == NULL) {
        return NULL;
    }
    memset(p, 0, size);
    return p;
}

static void free_ptr(void **p)
{
    if (*p != NULL) {
        free(*p);
        *p = NULL;
    }
}

/* ---------- helper: cmdline ---------- */

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

static bool cmdline_get_bool(const char *key)
{
    char val[32];

    if (!cmdline_get_value(key, val, sizeof(val))) {
        return false;
    }
    if (strcmp(val, "1") == 0 || strcmp(val, "true") == 0 ||
        strcmp(val, "yes") == 0 || strcmp(val, "on") == 0) {
        return true;
    }
    return false;
}

/* ---------- helper: network init (from urma_dp.c) ---------- */

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
    bool up;

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
    up = ((ifr.ifr_flags & IFF_UP) != 0);
    return up;
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
        fprintf(stderr, "[ub_rdma] set_ipv4: socket failed: %s\n", strerror(errno));
        return false;
    }
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, addr_str, &sin->sin_addr) != 1) {
        fprintf(stderr, "[ub_rdma] set_ipv4: inet_pton failed for %s\n", addr_str);
        close(fd);
        return false;
    }
    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) {
        fprintf(stderr, "[ub_rdma] set_ipv4: SIOCSIFADDR failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }
    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    sin = (struct sockaddr_in *)&ifr.ifr_netmask;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) != 0) {
        fprintf(stderr, "[ub_rdma] set_ipv4: SIOCSIFNETMASK failed: %s\n", strerror(errno));
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
    if (ioctl(fd, SIOCSARP, &req) != 0) {
        fprintf(stderr, "[ub_rdma] warn: SIOCSARP failed: %s\n", strerror(errno));
    }
    close(fd);
}

/* ---------- UDMA ucmd compat ---------- */

struct udma_create_jfc_ucmd_compat {
    uint64_t buf_addr;
    uint32_t buf_len;
    uint32_t mode;
    uint64_t db_addr;
    uint32_t is_hugepage : 1;
    uint32_t rsv : 31;
    uint32_t rsv1;
};

struct udma_create_jetty_ucmd_compat {
    uint64_t buf_addr;
    uint32_t buf_len;
    uint32_t jfr_id;
    uint64_t db_addr;
    uint64_t idx_addr;
    uint32_t idx_len;
    uint32_t sqe_bb_cnt;
    uint64_t jetty_addr;
    uint32_t pi_type : 1;
    uint32_t non_pin : 1;
    uint32_t is_hugepage : 1;
    uint32_t rsv : 29;
    uint32_t jetty_type;
    uint64_t jfr_sleep_buf;
    uint32_t jfs_id;
    uint32_t rsv1;
};

struct udma_create_jetty_resp_compat {
    uint64_t buf_addr;
};

enum {
    UDMA_KERNEL_STARS_JFC_TYPE = 2,
    UDMA_URMA_NORMAL_JETTY_TYPE = 3,
    UDMA_MMAP_KERNEL_BUF = 4,
    UDMA_TID_SHIFT_USER = 8,
    UBCORE_ACCESS_LOCAL_ONLY_USER = 0x1,
    UBURMA_REG_SEG_ACCESS_SHIFT = 5,
    UBURMA_REG_SEG_TOKEN_ID_VALID_SHIFT = 13,
};

static uint32_t make_register_seg_flag(void)
{
    return (UBCORE_ACCESS_LOCAL_ONLY_USER << UBURMA_REG_SEG_ACCESS_SHIFT) |
           (1u << UBURMA_REG_SEG_TOKEN_ID_VALID_SHIFT);
}

static void *map_udma_kernel_buf(int fd, size_t len)
{
    off_t offset = (off_t)UDMA_MMAP_KERNEL_BUF * DEMO_PAGE_SIZE;
    void *addr;

    addr = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset);
    if (addr == MAP_FAILED) {
        return NULL;
    }
    memset(addr, 0, len);
    return addr;
}

static void *map_udma_jetty_dsqe_page(int fd, uint32_t jetty_id)
{
    off_t offset = (off_t)(((uint64_t)jetty_id << MAP_INDEX_SHIFT_LOCAL) |
                           UDMA_MMAP_JETTY_DSQE_LOCAL) * DEMO_PAGE_SIZE;
    void *addr = mmap(NULL, DEMO_PAGE_SIZE, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, offset);

    if (addr == MAP_FAILED) {
        return NULL;
    }
    return addr;
}

static int ensure_runtime_buffers(struct rdma_resources *res)
{
    if (res->seg_buf == NULL) {
        res->seg_buf = alloc_aligned_zero(DEMO_PAGE_SIZE);
        if (res->seg_buf == NULL) {
            return -ENOMEM;
        }
        res->seg_len = DEMO_PAGE_SIZE;
    }
    if (res->jfc_ucmd_buf == NULL) {
        res->jfc_ucmd_buf = map_udma_kernel_buf(res->fd, DEMO_PAGE_SIZE);
        if (res->jfc_ucmd_buf == NULL) {
            return -ENOMEM;
        }
        res->jfc_ucmd_len = DEMO_PAGE_SIZE;
        res->jfc_ucmd_is_mmap = true;
    }
    if (res->jfc_db_buf == NULL) {
        res->jfc_db_buf = map_udma_kernel_buf(res->fd, DEMO_PAGE_SIZE);
        if (res->jfc_db_buf == NULL) {
            return -ENOMEM;
        }
        res->jfc_db_len = DEMO_PAGE_SIZE;
        res->jfc_db_is_mmap = true;
    }
    if (res->jfr_buf == NULL) {
        res->jfr_buf = map_udma_kernel_buf(res->fd, DEMO_PAGE_SIZE);
        if (res->jfr_buf == NULL) {
            return -ENOMEM;
        }
        res->jfr_buf_len = DEMO_PAGE_SIZE;
        res->jfr_buf_is_mmap = true;
    }
    if (res->jfr_idx_buf == NULL) {
        res->jfr_idx_buf = map_udma_kernel_buf(res->fd, DEMO_PAGE_SIZE);
        if (res->jfr_idx_buf == NULL) {
            return -ENOMEM;
        }
        res->jfr_idx_buf_len = DEMO_PAGE_SIZE;
        res->jfr_idx_buf_is_mmap = true;
    }
    if (res->jfr_db_buf == NULL) {
        res->jfr_db_buf = map_udma_kernel_buf(res->fd, DEMO_PAGE_SIZE);
        if (res->jfr_db_buf == NULL) {
            return -ENOMEM;
        }
        res->jfr_db_buf_len = DEMO_PAGE_SIZE;
        res->jfr_db_buf_is_mmap = true;
    }
    if (res->jfs_buf == NULL) {
        res->jfs_buf = alloc_aligned_zero(DEMO_PAGE_SIZE);
        if (res->jfs_buf == NULL) {
            return -ENOMEM;
        }
    }
    if (res->jfs_db_buf == NULL) {
        res->jfs_db_buf = alloc_aligned_zero(DEMO_PAGE_SIZE);
        if (res->jfs_db_buf == NULL) {
            return -ENOMEM;
        }
    }
    if (res->jetty_buf == NULL) {
        res->jetty_buf = map_udma_kernel_buf(res->fd, DEMO_PAGE_SIZE);
        if (res->jetty_buf == NULL) {
            return -ENOMEM;
        }
        res->jetty_buf_len = DEMO_PAGE_SIZE;
        res->jetty_buf_is_mmap = true;
    }
    if (res->jetty_db_buf == NULL) {
        res->jetty_db_buf = alloc_aligned_zero(DEMO_PAGE_SIZE);
        if (res->jetty_db_buf == NULL) {
            return -ENOMEM;
        }
    }
    return 0;
}

/* ---------- helper: ioctl wrapper (TLV) ---------- */

#define UBURMA_CMD_OUT_TYPE_INIT 0x80
#define UBURMA_TLV_MAX_ATTRS     UBURMA_CMD_OUT_TYPE_INIT
#define ARRAY_SIZE(x)            (sizeof(x) / sizeof((x)[0]))

struct uburma_cmd_attr {
    uint8_t type;
    uint8_t flag;
    uint16_t field_size;
    union {
        struct {
            uint32_t el_num : 12;
            uint32_t el_size : 12;
            uint32_t reserved : 8;
        } bs;
        uint32_t value;
    } attr_data;
    uint64_t data;
};

struct uburma_cmd_spec {
    uint8_t type;
    union {
        struct {
            uint8_t mandatory : 1;
        } bs;
        uint8_t value;
    } flag;
    uint16_t field_size;
    union {
        struct {
            uint32_t el_num : 12;
            uint32_t el_size : 12;
            uint32_t reserved : 8;
        } bs;
        uint32_t value;
    } attr_data;
    uint64_t data;
};

enum uburma_cmd_create_ctx_type {
    CREATE_CTX_IN_EID,
    CREATE_CTX_IN_EID_INDEX,
    CREATE_CTX_IN_UDATA,
    CREATE_CTX_IN_NUM,
    CREATE_CTX_OUT_ASYNC_FD = UBURMA_CMD_OUT_TYPE_INIT,
    CREATE_CTX_OUT_UDATA,
    CREATE_CTX_OUT_NUM,
};

enum uburma_cmd_alloc_token_id_type {
    ALLOC_TOKEN_ID_IN_UDATA,
    ALLOC_TOKEN_ID_IN_FLAG,
    ALLOC_TOKEN_ID_IN_NUM,
    ALLOC_TOKEN_ID_OUT_TOKEN_ID = UBURMA_CMD_OUT_TYPE_INIT,
    ALLOC_TOKEN_ID_OUT_HANDLE,
    ALLOC_TOKEN_ID_OUT_UDATA,
    ALLOC_TOKEN_ID_OUT_NUM,
};

enum uburma_cmd_free_token_id_type {
    FREE_TOKEN_ID_IN_HANDLE,
    FREE_TOKEN_ID_IN_TOKEN_ID,
    FREE_TOKEN_ID_IN_NUM,
};

enum uburma_cmd_register_seg_type {
    REGISTER_SEG_IN_VA,
    REGISTER_SEG_IN_LEN,
    REGISTER_SEG_IN_TOKEN_ID,
    REGISTER_SEG_IN_TOKEN_ID_HANDLE,
    REGISTER_SEG_IN_TOKEN,
    REGISTER_SEG_IN_FLAG,
    REGISTER_SEG_IN_UDATA,
    REGISTER_SEG_IN_NUM,
    REGISTER_SEG_OUT_TOKEN_ID = UBURMA_CMD_OUT_TYPE_INIT,
    REGISTER_SEG_OUT_HANDLE,
    REGISTER_SEG_OUT_UDATA,
    REGISTER_SEG_OUT_NUM,
};

enum uburma_cmd_unregister_seg_type {
    UNREGISTER_SEG_IN_HANDLE,
    UNREGISTER_SEG_IN_NUM,
};

enum uburma_cmd_alloc_jfs_type {
    ALLOC_JFS_IN_DEPTH,
    ALLOC_JFS_IN_FLAG,
    ALLOC_JFS_IN_TRANS_MODE,
    ALLOC_JFS_IN_PRIORITY,
    ALLOC_JFS_IN_MAX_SGE,
    ALLOC_JFS_IN_MAX_RSGE,
    ALLOC_JFS_IN_MAX_INLINE_DATA,
    ALLOC_JFS_IN_RNR_RETRY,
    ALLOC_JFS_IN_ERR_TIMEOUT,
    ALLOC_JFS_IN_JFC_ID,
    ALLOC_JFS_IN_JFC_HANDLE,
    ALLOC_JFS_IN_URMA_JFS,
    ALLOC_JFS_IN_UDATA,
    ALLOC_JFS_IN_NUM,
    ALLOC_JFS_OUT_ID = UBURMA_CMD_OUT_TYPE_INIT,
    ALLOC_JFS_OUT_DEPTH,
    ALLOC_JFS_OUT_MAX_SGE,
    ALLOC_JFS_OUT_MAX_RSGE,
    ALLOC_JFS_OUT_MAX_INLINE_DATA,
    ALLOC_JFS_OUT_HANDLE,
    ALLOC_JFS_OUT_UDATA,
    ALLOC_JFS_OUT_NUM,
};

enum uburma_cmd_active_jfs_type {
    ACTIVE_JFS_IN_HANDLE,
    ACTIVE_JFS_IN_DEPTH,
    ACTIVE_JFS_IN_FLAG,
    ACTIVE_JFS_IN_TRANS_MODE,
    ACTIVE_JFS_IN_PRIORITY,
    ACTIVE_JFS_IN_MAX_SGE,
    ACTIVE_JFS_IN_MAX_RSGE,
    ACTIVE_JFS_IN_MAX_INLINE_DATA,
    ACTIVE_JFS_IN_RNR_RETRY,
    ACTIVE_JFS_IN_ERR_TIMEOUT,
    ACTIVE_JFS_IN_JFC_ID,
    ACTIVE_JFS_IN_JFC_HANDLE,
    ACTIVE_JFS_IN_JFS_OPT,
    ACTIVE_JFS_IN_UDATA,
    ACTIVE_JFS_IN_NUM,
    ACTIVE_JFS_OUT_ID = UBURMA_CMD_OUT_TYPE_INIT,
    ACTIVE_JFS_OUT_DEPTH,
    ACTIVE_JFS_OUT_MAX_SGE,
    ACTIVE_JFS_OUT_MAX_RSGE,
    ACTIVE_JFS_OUT_MAX_INLINE_DATA,
    ACTIVE_JFS_OUT_HANDLE,
    ACTIVE_JFS_OUT_UDATA,
    ACTIVE_JFS_OUT_NUM,
};

enum uburma_cmd_alloc_jfr_type {
    ALLOC_JFR_IN_DEPTH,
    ALLOC_JFR_IN_FLAG,
    ALLOC_JFR_IN_TRANS_MODE,
    ALLOC_JFR_IN_MAX_SGE,
    ALLOC_JFR_IN_MIN_RNR_TIMER,
    ALLOC_JFR_IN_JFC_ID,
    ALLOC_JFR_IN_JFC_HANDLE,
    ALLOC_JFR_IN_TOKEN,
    ALLOC_JFR_IN_ID,
    ALLOC_JFR_IN_URMA_JFR,
    ALLOC_JFR_IN_UDATA,
    ALLOC_JFR_IN_NUM,
    ALLOC_JFR_OUT_ID = UBURMA_CMD_OUT_TYPE_INIT,
    ALLOC_JFR_OUT_DEPTH,
    ALLOC_JFR_OUT_HANDLE,
    ALLOC_JFR_OUT_MAX_SGE,
    ALLOC_JFR_OUT_UDATA,
    ALLOC_JFR_OUT_NUM,
};

enum uburma_cmd_active_jfr_type {
    ACTIVE_JFR_IN_HANDLE,
    ACTIVE_JFR_IN_DEPTH,
    ACTIVE_JFR_IN_FLAG,
    ACTIVE_JFR_IN_TRANS_MODE,
    ACTIVE_JFR_IN_MAX_SGE,
    ACTIVE_JFR_IN_MIN_RNR_TIMER,
    ACTIVE_JFR_IN_JFC_ID,
    ACTIVE_JFR_IN_JFC_HANDLE,
    ACTIVE_JFR_IN_TOKEN,
    ACTIVE_JFR_IN_JFR_OPT,
    ACTIVE_JFR_IN_UDATA,
    ACTIVE_JFR_IN_NUM,
    ACTIVE_JFR_OUT_ID = UBURMA_CMD_OUT_TYPE_INIT,
    ACTIVE_JFR_OUT_DEPTH,
    ACTIVE_JFR_OUT_HANDLE,
    ACTIVE_JFR_OUT_MAX_SGE,
    ACTIVE_JFR_OUT_UDATA,
    ACTIVE_JFR_OUT_NUM,
};

enum uburma_cmd_alloc_jfc_type {
    ALLOC_JFC_IN_DEPTH,
    ALLOC_JFC_IN_FLAG,
    ALLOC_JFC_IN_JFCE_FD,
    ALLOC_JFC_IN_URMA_JFC,
    ALLOC_JFC_IN_CEQN,
    ALLOC_JFC_IN_UDATA,
    ALLOC_JFC_IN_NUM,
    ALLOC_JFC_OUT_ID = UBURMA_CMD_OUT_TYPE_INIT,
    ALLOC_JFC_OUT_DEPTH,
    ALLOC_JFC_OUT_HANDLE,
    ALLOC_JFC_OUT_UDATA,
    ALLOC_JFC_OUT_NUM,
};

enum uburma_cmd_active_jfc_type {
    ACTIVE_JFC_IN_HANDLE,
    ACTIVE_JFC_IN_DEPTH,
    ACTIVE_JFC_IN_FLAG,
    ACTIVE_JFC_IN_CEQN,
    ACTIVE_JFC_IN_JFC_OPT,
    ACTIVE_JFC_IN_UDATA,
    ACTIVE_JFC_IN_NUM,
    ACTIVE_JFC_OUT_ID = UBURMA_CMD_OUT_TYPE_INIT,
    ACTIVE_JFC_OUT_DEPTH,
    ACTIVE_JFC_OUT_HANDLE,
    ACTIVE_JFC_OUT_UDATA,
    ACTIVE_JFC_OUT_NUM,
};

enum uburma_cmd_import_jetty_type {
    IMPORT_JETTY_IN_EID,
    IMPORT_JETTY_IN_ID,
    IMPORT_JETTY_IN_FLAG,
    IMPORT_JETTY_IN_TOKEN,
    IMPORT_JETTY_IN_TRANS_MODE,
    IMPORT_JETTY_IN_POLICY,
    IMPORT_JETTY_IN_TYPE,
    IMPORT_JETTY_IN_TP_TYPE,
    IMPORT_JETTY_IN_UDATA,
    IMPORT_JETTY_IN_NUM,
    IMPORT_JETTY_OUT_TPN = UBURMA_CMD_OUT_TYPE_INIT,
    IMPORT_JETTY_OUT_HANDLE,
    IMPORT_JETTY_OUT_UDATA,
    IMPORT_JETTY_OUT_NUM,
};

enum uburma_cmd_bind_jetty_type {
    BIND_JETTY_IN_JETTY_HANDLE,
    BIND_JETTY_IN_TJETTY_HANDLE,
    BIND_JETTY_IN_UDATA,
    BIND_JETTY_IN_NUM,
    BIND_JETTY_OUT_TPN = UBURMA_CMD_OUT_TYPE_INIT,
    BIND_JETTY_OUT_UDATA,
    BIND_JETTY_OUT_NUM,
};

enum uburma_cmd_unimport_jetty_type {
    UNIMPORT_JETTY_IN_HANDLE,
    UNIMPORT_JETTY_IN_NUM,
};

enum uburma_cmd_unadvise_jetty_type {
    UNADVISE_JETTY_IN_JETTY_HANDLE,
    UNADVISE_JETTY_IN_TJETTY_HANDLE,
    UNADVISE_JETTY_IN_NUM,
};

enum uburma_cmd_query_device_attr_type {
    QUERY_DEVICE_IN_DEV_NAME,
    QUERY_DEVICE_IN_NUM,
    QUERY_DEVICE_OUT_GUID = UBURMA_CMD_OUT_TYPE_INIT,
    QUERY_DEVICE_OUT_DEV_CAP_FEATURE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY_GRP,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY_IN_JETTY_GRP,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC_DEPTH,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_DEPTH,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_DEPTH,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_INLINE_LEN,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_SGE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_RSGE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_SGE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_MSG_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_READ_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_WRITE_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_CAS_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_SWAP_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_ADD_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_SUB_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_AND_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_OR_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_XOR_SIZE,
    QUERY_DEVICE_OUT_DEV_CAP_ATOMIC_FEAT,
    QUERY_DEVICE_OUT_DEV_CAP_TRANS_MODE,
    QUERY_DEVICE_OUT_DEV_CAP_SUB_TRANS_MODE_CAP,
    QUERY_DEVICE_OUT_DEV_CAP_CONGESTION_CTRL_ALG,
    QUERY_DEVICE_OUT_DEV_CAP_CEQ_CNT,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_TP_IN_TPG,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_EID_CNT,
    QUERY_DEVICE_OUT_DEV_CAP_PAGE_SIZE_CAP,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_OOR_CNT,
    QUERY_DEVICE_OUT_DEV_CAP_MN,
    QUERY_DEVICE_OUT_DEV_CAP_MAX_NETADDR_CN,
    QUERY_DEVICE_OUT_PORT_CNT,
    QUERY_DEVICE_OUT_PORT_ATTR_MAX_MTU,
    QUERY_DEVICE_OUT_PORT_ATTR_STATE,
    QUERY_DEVICE_OUT_PORT_ATTR_ACTIVE_WIDTH,
    QUERY_DEVICE_OUT_PORT_ATTR_ACTIVE_SPEED,
    QUERY_DEVICE_OUT_PORT_ATTR_ACTIVE_MTU,
    QUERY_DEVICE_OUT_RESERVED_JETTY_ID_MIN,
    QUERY_DEVICE_OUT_RESERVED_JETTY_ID_MAX,
    QUERY_DEVICE_OUT_DEV_CAP_RM_ORDER_CAP,
    QUERY_DEVICE_OUT_DEV_CAP_RC_ORDER_CAP,
    QUERY_DEVICE_OUT_DEV_CAP_RM_TP_CAP,
    QUERY_DEVICE_OUT_DEV_CAP_RC_TP_CAP,
    QUERY_DEVICE_OUT_DEV_CAP_UM_TP_CAP,
    QUERY_DEVICE_OUT_DEV_CAP_TP_FEATURE,
    QUERY_DEVICE_OUT_DEV_CAP_PRIORITY_INFO,
    QUERY_DEVICE_OUT_NUM,
};

enum uburma_cmd_alloc_jetty_type {
    ALLOC_JETTY_IN_CFG,
    ALLOC_JETTY_IN_NUM,
    ALLOC_JETTY_OUT_CFG = UBURMA_CMD_OUT_TYPE_INIT,
    ALLOC_JETTY_OUT_NUM,
};

enum uburma_cmd_active_jetty_type {
    ACTIVE_JETTY_IN_FLAG,
    ACTIVE_JETTY_IN_HANDLE,
    ACTIVE_JETTY_IN_SEND_JFC_HANDLE,
    ACTIVE_JETTY_IN_RECV_JFC_HANDLE,
    ACTIVE_JETTY_IN_URMA_JETTY,
    ACTIVE_JETTY_IN_JETTY_OPT,
    ACTIVE_JETTY_IN_UDATA,
    ACTIVE_JETTY_IN_NUM,
    ACTIVE_JETTY_OUT_JETTY_ID = UBURMA_CMD_OUT_TYPE_INIT,
    ACTIVE_JETTY_OUT_UDATA,
    ACTIVE_JETTY_OUT_NUM,
};

static inline void fill_spec(struct uburma_cmd_spec *spec,
                             uint16_t type, uint16_t field_size,
                             uint16_t el_num, uint16_t el_size,
                             uintptr_t data)
{
    spec->type = (uint8_t)type;
    spec->flag.value = 0;
    spec->flag.bs.mandatory = 1;
    spec->field_size = field_size;
    spec->attr_data.value = 0;
    spec->attr_data.bs.el_num = el_num;
    spec->attr_data.bs.el_size = el_size;
    spec->data = (uint64_t)data;
}

#define SPEC(spec, type, v) \
    fill_spec((spec), (type), sizeof(v), 1, 0, (uintptr_t)(&(v)))

#define SPEC_ARRAY(spec, type, v1, v2) \
    fill_spec((spec), (type), sizeof((v1)->v2), ARRAY_SIZE(v1), \
              sizeof((v1)[0]), (uintptr_t)(&((v1)->v2)))

static void uburma_create_ctx_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_create_ctx *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, CREATE_CTX_IN_EID, arg->in.eid);
    SPEC(s++, CREATE_CTX_IN_EID_INDEX, arg->in.eid_index);
    SPEC(s++, CREATE_CTX_IN_UDATA, arg->udata);
}

static void uburma_create_ctx_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_create_ctx *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, CREATE_CTX_OUT_ASYNC_FD, arg->out.async_fd);
    SPEC(s++, CREATE_CTX_OUT_UDATA, arg->udata);
}

static void uburma_alloc_token_id_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_token_id *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_TOKEN_ID_IN_FLAG, arg->flag);
    SPEC(s++, ALLOC_TOKEN_ID_IN_UDATA, arg->udata);
}

static void uburma_alloc_token_id_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_token_id *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_TOKEN_ID_OUT_TOKEN_ID, arg->out.token_id);
    SPEC(s++, ALLOC_TOKEN_ID_OUT_HANDLE, arg->out.handle);
    SPEC(s++, ALLOC_TOKEN_ID_OUT_UDATA, arg->udata);
}

static void uburma_free_token_id_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_free_token_id *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, FREE_TOKEN_ID_IN_HANDLE, arg->in.handle);
    SPEC(s++, FREE_TOKEN_ID_IN_TOKEN_ID, arg->in.token_id);
}

static void uburma_unregister_seg_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_unregister_seg *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, UNREGISTER_SEG_IN_HANDLE, arg->in.handle);
}

static void uburma_register_seg_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_register_seg *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, REGISTER_SEG_IN_VA, arg->in.va);
    SPEC(s++, REGISTER_SEG_IN_LEN, arg->in.len);
    SPEC(s++, REGISTER_SEG_IN_TOKEN_ID, arg->in.token_id);
    SPEC(s++, REGISTER_SEG_IN_TOKEN_ID_HANDLE, arg->in.token_id_handle);
    SPEC(s++, REGISTER_SEG_IN_TOKEN, arg->in.token);
    SPEC(s++, REGISTER_SEG_IN_FLAG, arg->in.flag);
    SPEC(s++, REGISTER_SEG_IN_UDATA, arg->udata);
}

static void uburma_register_seg_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_register_seg *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, REGISTER_SEG_OUT_TOKEN_ID, arg->out.token_id);
    SPEC(s++, REGISTER_SEG_OUT_HANDLE, arg->out.handle);
    SPEC(s++, REGISTER_SEG_OUT_UDATA, arg->udata);
}

static void uburma_alloc_jfc_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_jfc *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_JFC_IN_DEPTH, arg->in.depth);
    SPEC(s++, ALLOC_JFC_IN_FLAG, arg->in.flag);
    SPEC(s++, ALLOC_JFC_IN_JFCE_FD, arg->in.jfce_fd);
    SPEC(s++, ALLOC_JFC_IN_URMA_JFC, arg->in.urma_jfc);
    SPEC(s++, ALLOC_JFC_IN_CEQN, arg->in.ceqn);
    SPEC(s++, ALLOC_JFC_IN_UDATA, arg->udata);
}

static void uburma_alloc_jfc_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_jfc *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_JFC_OUT_DEPTH, arg->out.depth);
    SPEC(s++, ALLOC_JFC_OUT_ID, arg->out.id);
    SPEC(s++, ALLOC_JFC_OUT_HANDLE, arg->out.handle);
    /* Keep consistent with kernel fill_spec_out implementation. */
    SPEC(s++, ALLOC_JFC_IN_UDATA, arg->udata);
}

static void uburma_active_jfc_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_active_jfc *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ACTIVE_JFC_IN_HANDLE, arg->in.handle);
    SPEC(s++, ACTIVE_JFC_IN_DEPTH, arg->in.depth);
    SPEC(s++, ACTIVE_JFC_IN_FLAG, arg->in.flag);
    SPEC(s++, ACTIVE_JFC_IN_CEQN, arg->in.ceqn);
    SPEC(s++, ACTIVE_JFC_IN_JFC_OPT, arg->in.urma_jfc_opt);
    SPEC(s++, ACTIVE_JFC_IN_UDATA, arg->udata);
}

static void uburma_active_jfc_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_active_jfc *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ACTIVE_JFC_OUT_ID, arg->out.id);
    SPEC(s++, ACTIVE_JFC_OUT_DEPTH, arg->out.depth);
    SPEC(s++, ACTIVE_JFC_OUT_HANDLE, arg->out.handle);
    SPEC(s++, ACTIVE_JFC_OUT_UDATA, arg->udata);
}

static void uburma_alloc_jfr_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_jfr *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_JFR_IN_DEPTH, arg->in.depth);
    SPEC(s++, ALLOC_JFR_IN_FLAG, arg->in.flag);
    SPEC(s++, ALLOC_JFR_IN_TRANS_MODE, arg->in.trans_mode);
    SPEC(s++, ALLOC_JFR_IN_MAX_SGE, arg->in.max_sge);
    SPEC(s++, ALLOC_JFR_IN_MIN_RNR_TIMER, arg->in.min_rnr_timer);
    SPEC(s++, ALLOC_JFR_IN_JFC_ID, arg->in.jfc_id);
    SPEC(s++, ALLOC_JFR_IN_JFC_HANDLE, arg->in.jfc_handle);
    SPEC(s++, ALLOC_JFR_IN_TOKEN, arg->in.token);
    SPEC(s++, ALLOC_JFR_IN_ID, arg->in.id);
    SPEC(s++, ALLOC_JFR_IN_URMA_JFR, arg->in.urma_jfr);
    SPEC(s++, ALLOC_JFR_IN_UDATA, arg->udata);
}

static void uburma_alloc_jfr_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_jfr *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_JFR_OUT_ID, arg->out.id);
    SPEC(s++, ALLOC_JFR_OUT_DEPTH, arg->out.depth);
    SPEC(s++, ALLOC_JFR_OUT_HANDLE, arg->out.handle);
    SPEC(s++, ALLOC_JFR_OUT_MAX_SGE, arg->out.max_sge);
    SPEC(s++, ALLOC_JFR_OUT_UDATA, arg->udata);
}

static void uburma_active_jfr_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_active_jfr *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ACTIVE_JFR_IN_HANDLE, arg->in.handle);
    SPEC(s++, ACTIVE_JFR_IN_DEPTH, arg->in.depth);
    SPEC(s++, ACTIVE_JFR_IN_FLAG, arg->in.flag);
    SPEC(s++, ACTIVE_JFR_IN_TRANS_MODE, arg->in.trans_mode);
    SPEC(s++, ACTIVE_JFR_IN_MAX_SGE, arg->in.max_sge);
    SPEC(s++, ACTIVE_JFR_IN_MIN_RNR_TIMER, arg->in.min_rnr_timer);
    SPEC(s++, ACTIVE_JFR_IN_JFC_ID, arg->in.jfc_id);
    SPEC(s++, ACTIVE_JFR_IN_JFC_HANDLE, arg->in.jfc_handle);
    SPEC(s++, ACTIVE_JFR_IN_TOKEN, arg->in.token_value);
    SPEC(s++, ACTIVE_JFR_IN_JFR_OPT, arg->in.urma_jfr_opt);
    SPEC(s++, ACTIVE_JFR_IN_UDATA, arg->udata);
}

static void uburma_active_jfr_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_active_jfr *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ACTIVE_JFR_OUT_ID, arg->out.id);
    SPEC(s++, ACTIVE_JFR_OUT_DEPTH, arg->out.depth);
    SPEC(s++, ACTIVE_JFR_OUT_HANDLE, arg->out.handle);
    SPEC(s++, ACTIVE_JFR_OUT_MAX_SGE, arg->out.max_sge);
    SPEC(s++, ACTIVE_JFR_OUT_UDATA, arg->udata);
}

static void uburma_alloc_jfs_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_jfs *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_JFS_IN_DEPTH, arg->in.depth);
    SPEC(s++, ALLOC_JFS_IN_FLAG, arg->in.flag);
    SPEC(s++, ALLOC_JFS_IN_TRANS_MODE, arg->in.trans_mode);
    SPEC(s++, ALLOC_JFS_IN_PRIORITY, arg->in.priority);
    SPEC(s++, ALLOC_JFS_IN_MAX_SGE, arg->in.max_sge);
    SPEC(s++, ALLOC_JFS_IN_MAX_RSGE, arg->in.max_rsge);
    SPEC(s++, ALLOC_JFS_IN_MAX_INLINE_DATA, arg->in.max_inline_data);
    SPEC(s++, ALLOC_JFS_IN_RNR_RETRY, arg->in.rnr_retry);
    SPEC(s++, ALLOC_JFS_IN_ERR_TIMEOUT, arg->in.err_timeout);
    SPEC(s++, ALLOC_JFS_IN_JFC_ID, arg->in.jfc_id);
    SPEC(s++, ALLOC_JFS_IN_JFC_HANDLE, arg->in.jfc_handle);
    SPEC(s++, ALLOC_JFS_IN_URMA_JFS, arg->in.urma_jfs);
    SPEC(s++, ALLOC_JFS_IN_UDATA, arg->udata);
}

static void uburma_alloc_jfs_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_jfs *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_JFS_OUT_ID, arg->out.id);
    SPEC(s++, ALLOC_JFS_OUT_DEPTH, arg->out.depth);
    SPEC(s++, ALLOC_JFS_OUT_MAX_SGE, arg->out.max_sge);
    SPEC(s++, ALLOC_JFS_OUT_MAX_RSGE, arg->out.max_rsge);
    SPEC(s++, ALLOC_JFS_OUT_MAX_INLINE_DATA, arg->out.max_inline_data);
    SPEC(s++, ALLOC_JFS_OUT_HANDLE, arg->out.handle);
    SPEC(s++, ALLOC_JFS_OUT_UDATA, arg->udata);
}

static void uburma_active_jfs_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_active_jfs *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ACTIVE_JFS_IN_HANDLE, arg->in.handle);
    SPEC(s++, ACTIVE_JFS_IN_DEPTH, arg->in.depth);
    SPEC(s++, ACTIVE_JFS_IN_FLAG, arg->in.flag);
    SPEC(s++, ACTIVE_JFS_IN_TRANS_MODE, arg->in.trans_mode);
    SPEC(s++, ACTIVE_JFS_IN_PRIORITY, arg->in.priority);
    SPEC(s++, ACTIVE_JFS_IN_MAX_SGE, arg->in.max_sge);
    SPEC(s++, ACTIVE_JFS_IN_MAX_RSGE, arg->in.max_rsge);
    SPEC(s++, ACTIVE_JFS_IN_MAX_INLINE_DATA, arg->in.max_inline_data);
    SPEC(s++, ACTIVE_JFS_IN_RNR_RETRY, arg->in.rnr_retry);
    SPEC(s++, ACTIVE_JFS_IN_ERR_TIMEOUT, arg->in.err_timeout);
    SPEC(s++, ACTIVE_JFS_IN_JFC_ID, arg->in.jfc_id);
    SPEC(s++, ACTIVE_JFS_IN_JFC_HANDLE, arg->in.jfc_handle);
    SPEC(s++, ACTIVE_JFS_IN_JFS_OPT, arg->in.jfs_opt);
    SPEC(s++, ACTIVE_JFS_IN_UDATA, arg->udata);
}

static void uburma_active_jfs_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_active_jfs *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ACTIVE_JFS_OUT_ID, arg->out.id);
    SPEC(s++, ACTIVE_JFS_OUT_DEPTH, arg->out.depth);
    SPEC(s++, ACTIVE_JFS_OUT_MAX_SGE, arg->out.max_sge);
    SPEC(s++, ACTIVE_JFS_OUT_MAX_RSGE, arg->out.max_rsge);
    SPEC(s++, ACTIVE_JFS_OUT_MAX_INLINE_DATA, arg->out.max_inline_data);
    SPEC(s++, ACTIVE_JFS_OUT_HANDLE, arg->out.handle);
    SPEC(s++, ACTIVE_JFS_OUT_UDATA, arg->udata);
}

static void uburma_alloc_jetty_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_jetty *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_JETTY_IN_CFG, arg->create_jetty);
}

static void uburma_alloc_jetty_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_alloc_jetty *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ALLOC_JETTY_OUT_CFG, arg->create_jetty);
}

static void uburma_active_jetty_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_active_jetty *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ACTIVE_JETTY_IN_FLAG, arg->in.flag);
    SPEC(s++, ACTIVE_JETTY_IN_HANDLE, arg->in.handle);
    SPEC(s++, ACTIVE_JETTY_IN_SEND_JFC_HANDLE, arg->in.send_jfc_handle);
    SPEC(s++, ACTIVE_JETTY_IN_RECV_JFC_HANDLE, arg->in.recv_jfc_handle);
    SPEC(s++, ACTIVE_JETTY_IN_URMA_JETTY, arg->in.urma_jetty);
    SPEC(s++, ACTIVE_JETTY_IN_JETTY_OPT, arg->in.jetty_opt);
    SPEC(s++, ACTIVE_JETTY_IN_UDATA, arg->udata);
}

static void uburma_active_jetty_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_active_jetty *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, ACTIVE_JETTY_OUT_JETTY_ID, arg->out.jetty_id);
    SPEC(s++, ACTIVE_JETTY_OUT_UDATA, arg->udata);
}

static void uburma_import_jetty_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_import_jetty *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, IMPORT_JETTY_IN_EID, arg->in.eid);
    SPEC(s++, IMPORT_JETTY_IN_ID, arg->in.id);
    SPEC(s++, IMPORT_JETTY_IN_FLAG, arg->in.flag);
    SPEC(s++, IMPORT_JETTY_IN_TOKEN, arg->in.token);
    SPEC(s++, IMPORT_JETTY_IN_TRANS_MODE, arg->in.trans_mode);
    SPEC(s++, IMPORT_JETTY_IN_POLICY, arg->in.policy);
    SPEC(s++, IMPORT_JETTY_IN_TYPE, arg->in.type);
    SPEC(s++, IMPORT_JETTY_IN_TP_TYPE, arg->in.tp_type);
    SPEC(s++, IMPORT_JETTY_IN_UDATA, arg->udata);
}

static void uburma_import_jetty_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_import_jetty *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, IMPORT_JETTY_OUT_TPN, arg->out.tpn);
    SPEC(s++, IMPORT_JETTY_OUT_HANDLE, arg->out.handle);
    SPEC(s++, IMPORT_JETTY_OUT_UDATA, arg->udata);
}

static void uburma_bind_jetty_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_bind_jetty *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, BIND_JETTY_IN_JETTY_HANDLE, arg->in.jetty_handle);
    SPEC(s++, BIND_JETTY_IN_TJETTY_HANDLE, arg->in.tjetty_handle);
    SPEC(s++, BIND_JETTY_IN_UDATA, arg->udata);
}

static void uburma_bind_jetty_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_bind_jetty *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, BIND_JETTY_OUT_TPN, arg->out.tpn);
    SPEC(s++, BIND_JETTY_OUT_UDATA, arg->udata);
}

static void uburma_unimport_jetty_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_unimport_jetty_local *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, UNIMPORT_JETTY_IN_HANDLE, arg->in.handle);
}

static void uburma_unbind_jetty_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_unbind_jetty_local *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, UNADVISE_JETTY_IN_JETTY_HANDLE, arg->in.jetty_handle);
    SPEC(s++, UNADVISE_JETTY_IN_TJETTY_HANDLE, arg->in.tjetty_handle);
}

static void uburma_query_device_fill_spec_in(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_query_device_attr *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;
    SPEC(s++, QUERY_DEVICE_IN_DEV_NAME, arg->in.dev_name);
}

static void uburma_query_device_fill_spec_out(void *arg_addr, struct uburma_cmd_spec *spec)
{
    struct uburma_cmd_query_device_attr *arg = arg_addr;
    struct uburma_cmd_spec *s = spec;

    SPEC(s++, QUERY_DEVICE_OUT_GUID, arg->out.attr.guid);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_FEATURE, arg->out.attr.dev_cap.feature);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC, arg->out.attr.dev_cap.max_jfc);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS, arg->out.attr.dev_cap.max_jfs);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR, arg->out.attr.dev_cap.max_jfr);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY, arg->out.attr.dev_cap.max_jetty);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY_GRP, arg->out.attr.dev_cap.max_jetty_grp);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JETTY_IN_JETTY_GRP,
         arg->out.attr.dev_cap.max_jetty_in_jetty_grp);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFC_DEPTH, arg->out.attr.dev_cap.max_jfc_depth);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_DEPTH, arg->out.attr.dev_cap.max_jfs_depth);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_DEPTH, arg->out.attr.dev_cap.max_jfr_depth);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_INLINE_LEN,
         arg->out.attr.dev_cap.max_jfs_inline_len);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_SGE, arg->out.attr.dev_cap.max_jfs_sge);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFS_RSGE, arg->out.attr.dev_cap.max_jfs_rsge);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_JFR_SGE, arg->out.attr.dev_cap.max_jfr_sge);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_MSG_SIZE, arg->out.attr.dev_cap.max_msg_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_READ_SIZE, arg->out.attr.dev_cap.max_read_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_WRITE_SIZE, arg->out.attr.dev_cap.max_write_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_CAS_SIZE, arg->out.attr.dev_cap.max_cas_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_SWAP_SIZE, arg->out.attr.dev_cap.max_swap_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_ADD_SIZE,
         arg->out.attr.dev_cap.max_fetch_and_add_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_SUB_SIZE,
         arg->out.attr.dev_cap.max_fetch_and_sub_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_AND_SIZE,
         arg->out.attr.dev_cap.max_fetch_and_and_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_OR_SIZE,
         arg->out.attr.dev_cap.max_fetch_and_or_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_FETCH_AND_XOR_SIZE,
         arg->out.attr.dev_cap.max_fetch_and_xor_size);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_ATOMIC_FEAT, arg->out.attr.dev_cap.atomic_feat);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_TRANS_MODE, arg->out.attr.dev_cap.trans_mode);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_SUB_TRANS_MODE_CAP,
         arg->out.attr.dev_cap.sub_trans_mode_cap);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_CONGESTION_CTRL_ALG,
         arg->out.attr.dev_cap.congestion_ctrl_alg);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_CEQ_CNT, arg->out.attr.dev_cap.ceq_cnt);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_TP_IN_TPG, arg->out.attr.dev_cap.max_tp_in_tpg);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_EID_CNT, arg->out.attr.dev_cap.max_eid_cnt);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_PAGE_SIZE_CAP, arg->out.attr.dev_cap.page_size_cap);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_OOR_CNT, arg->out.attr.dev_cap.max_oor_cnt);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MN, arg->out.attr.dev_cap.mn);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_MAX_NETADDR_CN, arg->out.attr.dev_cap.max_netaddr_cnt);
    SPEC(s++, QUERY_DEVICE_OUT_PORT_CNT, arg->out.attr.port_cnt);
    SPEC(s++, QUERY_DEVICE_OUT_RESERVED_JETTY_ID_MIN, arg->out.attr.reserved_jetty_id_min);
    SPEC(s++, QUERY_DEVICE_OUT_RESERVED_JETTY_ID_MAX, arg->out.attr.reserved_jetty_id_max);
    SPEC_ARRAY(s++, QUERY_DEVICE_OUT_PORT_ATTR_MAX_MTU, arg->out.attr.port_attr, max_mtu);
    SPEC_ARRAY(s++, QUERY_DEVICE_OUT_PORT_ATTR_STATE, arg->out.attr.port_attr, state);
    SPEC_ARRAY(s++, QUERY_DEVICE_OUT_PORT_ATTR_ACTIVE_WIDTH,
               arg->out.attr.port_attr, active_width);
    SPEC_ARRAY(s++, QUERY_DEVICE_OUT_PORT_ATTR_ACTIVE_SPEED,
               arg->out.attr.port_attr, active_speed);
    SPEC_ARRAY(s++, QUERY_DEVICE_OUT_PORT_ATTR_ACTIVE_MTU, arg->out.attr.port_attr, active_mtu);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_RM_ORDER_CAP, arg->out.attr.dev_cap.rm_order_cap.value);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_RC_ORDER_CAP, arg->out.attr.dev_cap.rc_order_cap.value);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_RM_TP_CAP, arg->out.attr.dev_cap.rm_tp_cap.value);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_RC_TP_CAP, arg->out.attr.dev_cap.rc_tp_cap.value);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_UM_TP_CAP, arg->out.attr.dev_cap.um_tp_cap.value);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_TP_FEATURE, arg->out.attr.dev_cap.tp_feature.value);
    SPEC(s++, QUERY_DEVICE_OUT_DEV_CAP_PRIORITY_INFO, arg->out.attr.dev_cap.priority_info);
}

static size_t specs_to_attrs(struct uburma_cmd_attr *attrs,
                             const struct uburma_cmd_spec *specs,
                             size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        attrs[i].type = specs[i].type;
        attrs[i].flag = specs[i].flag.value;
        attrs[i].field_size = specs[i].field_size;
        attrs[i].attr_data.value = specs[i].attr_data.value;
        attrs[i].data = specs[i].data;
    }
    return count;
}

static int build_tlv_specs(uint32_t cmd, void *arg,
                           struct uburma_cmd_spec *in_specs, size_t *in_count,
                           struct uburma_cmd_spec *out_specs, size_t *out_count)
{
    *in_count = 0;
    *out_count = 0;

    switch (cmd) {
    case UBURMA_CMD_CREATE_CTX:
        uburma_create_ctx_fill_spec_in(arg, in_specs);
        uburma_create_ctx_fill_spec_out(arg, out_specs);
        *in_count = CREATE_CTX_IN_NUM;
        *out_count = CREATE_CTX_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_ALLOC_TOKEN_ID:
        uburma_alloc_token_id_fill_spec_in(arg, in_specs);
        uburma_alloc_token_id_fill_spec_out(arg, out_specs);
        *in_count = ALLOC_TOKEN_ID_IN_NUM;
        *out_count = ALLOC_TOKEN_ID_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_FREE_TOKEN_ID:
        uburma_free_token_id_fill_spec_in(arg, in_specs);
        *in_count = FREE_TOKEN_ID_IN_NUM;
        *out_count = 0;
        return 0;
    case UBURMA_CMD_UNREGISTER_SEG:
        uburma_unregister_seg_fill_spec_in(arg, in_specs);
        *in_count = UNREGISTER_SEG_IN_NUM;
        *out_count = 0;
        return 0;
    case UBURMA_CMD_ALLOC_JFC:
        uburma_alloc_jfc_fill_spec_in(arg, in_specs);
        uburma_alloc_jfc_fill_spec_out(arg, out_specs);
        *in_count = ALLOC_JFC_IN_NUM;
        *out_count = ALLOC_JFC_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_ACTIVE_JFC:
        uburma_active_jfc_fill_spec_in(arg, in_specs);
        uburma_active_jfc_fill_spec_out(arg, out_specs);
        *in_count = ACTIVE_JFC_IN_NUM;
        *out_count = ACTIVE_JFC_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_ALLOC_JFR:
        uburma_alloc_jfr_fill_spec_in(arg, in_specs);
        uburma_alloc_jfr_fill_spec_out(arg, out_specs);
        *in_count = ALLOC_JFR_IN_NUM;
        *out_count = ALLOC_JFR_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_ACTIVE_JFR:
        uburma_active_jfr_fill_spec_in(arg, in_specs);
        uburma_active_jfr_fill_spec_out(arg, out_specs);
        *in_count = ACTIVE_JFR_IN_NUM;
        *out_count = ACTIVE_JFR_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_ALLOC_JFS:
        uburma_alloc_jfs_fill_spec_in(arg, in_specs);
        uburma_alloc_jfs_fill_spec_out(arg, out_specs);
        *in_count = ALLOC_JFS_IN_NUM;
        *out_count = ALLOC_JFS_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_ACTIVE_JFS:
        uburma_active_jfs_fill_spec_in(arg, in_specs);
        uburma_active_jfs_fill_spec_out(arg, out_specs);
        *in_count = ACTIVE_JFS_IN_NUM;
        *out_count = ACTIVE_JFS_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_ALLOC_JETTY:
        uburma_alloc_jetty_fill_spec_in(arg, in_specs);
        uburma_alloc_jetty_fill_spec_out(arg, out_specs);
        *in_count = ALLOC_JETTY_IN_NUM;
        *out_count = ALLOC_JETTY_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_ACTIVE_JETTY:
        uburma_active_jetty_fill_spec_in(arg, in_specs);
        uburma_active_jetty_fill_spec_out(arg, out_specs);
        *in_count = ACTIVE_JETTY_IN_NUM;
        *out_count = ACTIVE_JETTY_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_REGISTER_SEG:
        uburma_register_seg_fill_spec_in(arg, in_specs);
        uburma_register_seg_fill_spec_out(arg, out_specs);
        *in_count = REGISTER_SEG_IN_NUM;
        *out_count = REGISTER_SEG_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_IMPORT_JETTY:
        uburma_import_jetty_fill_spec_in(arg, in_specs);
        uburma_import_jetty_fill_spec_out(arg, out_specs);
        *in_count = IMPORT_JETTY_IN_NUM;
        *out_count = IMPORT_JETTY_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_BIND_JETTY:
        uburma_bind_jetty_fill_spec_in(arg, in_specs);
        uburma_bind_jetty_fill_spec_out(arg, out_specs);
        *in_count = BIND_JETTY_IN_NUM;
        *out_count = BIND_JETTY_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    case UBURMA_CMD_UNIMPORT_JETTY:
        uburma_unimport_jetty_fill_spec_in(arg, in_specs);
        *in_count = UNIMPORT_JETTY_IN_NUM;
        *out_count = 0;
        return 0;
    case UBURMA_CMD_UNBIND_JETTY:
        uburma_unbind_jetty_fill_spec_in(arg, in_specs);
        *in_count = UNADVISE_JETTY_IN_NUM;
        *out_count = 0;
        return 0;
    case UBURMA_CMD_QUERY_DEV_ATTR:
        uburma_query_device_fill_spec_in(arg, in_specs);
        uburma_query_device_fill_spec_out(arg, out_specs);
        *in_count = QUERY_DEVICE_IN_NUM;
        *out_count = QUERY_DEVICE_OUT_NUM - UBURMA_CMD_OUT_TYPE_INIT;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static int ub_ioctl(int fd, uint32_t cmd, void *arg, size_t arg_size)
{
    struct uburma_cmd_spec in_specs[UBURMA_TLV_MAX_ATTRS];
    struct uburma_cmd_spec out_specs[UBURMA_TLV_MAX_ATTRS];
    struct uburma_cmd_attr attrs[UBURMA_TLV_MAX_ATTRS];
    struct uburma_cmd_hdr hdr;
    size_t in_count = 0;
    size_t out_count = 0;
    size_t total_count = 0;
    int ret;

    ret = build_tlv_specs(cmd, arg, in_specs, &in_count, out_specs, &out_count);
    if (ret == 0) {
        if (in_count + out_count > ARRAY_SIZE(attrs)) {
            return -E2BIG;
        }
        memset(attrs, 0, sizeof(attrs));
        total_count += specs_to_attrs(attrs + total_count, in_specs, in_count);
        total_count += specs_to_attrs(attrs + total_count, out_specs, out_count);

        memset(&hdr, 0, sizeof(hdr));
        hdr.command = cmd;
        hdr.args_len = (uint32_t)(total_count * sizeof(struct uburma_cmd_attr));
        hdr.args_addr = (uint64_t)(uintptr_t)attrs;
    } else {
        memset(&hdr, 0, sizeof(hdr));
        hdr.command = cmd;
        hdr.args_len = (uint32_t)arg_size;
        hdr.args_addr = (uint64_t)(uintptr_t)arg;
    }

    ret = ioctl(fd, UBURMA_CMD, &hdr);
    if (ret < 0) {
        return -errno;
    }
    return 0;
}

/* ---------- helper: udrv_priv zero fill ---------- */

static void zero_udrv_priv(struct uburma_cmd_udrv_priv *p)
{
    p->in_addr = 0;
    p->in_len = 0;
    p->out_addr = 0;
    p->out_len = 0;
}

/* ---------- helper: device discovery ---------- */

static int discover_uburma_device(char *dev_name, size_t dev_name_len)
{
    DIR *dir;
    struct dirent *ent;

    dir = opendir("/sys/class/uburma");
    if (!dir) {
        return -1;
    }
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        snprintf(dev_name, dev_name_len, "%s", ent->d_name);
        closedir(dir);
        return 0;
    }
    closedir(dir);
    return -1;
}

static int open_uburma_device(const char *dev_name)
{
    char path[256];
    int fd;
    int written;

    written = snprintf(path, sizeof(path), "/dev/uburma/%s", dev_name);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        fprintf(stderr, "[ub_rdma] device path too long for %s\n", dev_name);
        return -1;
    }
    fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[ub_rdma] open %s failed: %s\n", path, strerror(errno));
        return -1;
    }
    return fd;
}

static int open_ummu_tid_device(void)
{
    int fd;

    fd = open(TID_DEVICE_NAME, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[ub_rdma] open %s failed: %s\n",
                TID_DEVICE_NAME, strerror(errno));
        return -1;
    }
    return fd;
}

static int copy_dev_name(char *dst, size_t dst_size, const char *dev_name)
{
    int written;

    written = snprintf(dst, dst_size, "%s", dev_name);
    if (written < 0 || (size_t)written >= dst_size) {
        fprintf(stderr, "[ub_rdma] device name too long: %s\n", dev_name);
        return -1;
    }
    return 0;
}

static int alloc_ummu_tid(int fd, uint32_t *tid_out)
{
    struct ummu_tid_info info;

    memset(&info, 0, sizeof(info));
    info.mode = MAPT_MODE_TABLE;
    if (ioctl(fd, UMMU_IOCALLOC_TID, &info) < 0) {
        return -errno;
    }
    *tid_out = info.tid;
    return 0;
}

static int free_ummu_tid(int fd, uint32_t tid)
{
    struct ummu_tid_info info;

    memset(&info, 0, sizeof(info));
    info.tid = tid;
    if (ioctl(fd, UMMU_IOCFREE_TID, &info) < 0) {
        return -errno;
    }
    return 0;
}

/* ---------- helper: UDP info exchange ---------- */

struct peer_info {
    uint8_t eid[UBCORE_EID_SIZE];
    uint32_t jetty_id;
    uint32_t token;
};

static int udp_send_all(int fd, const void *buf, size_t len,
                        const struct sockaddr_in *dst)
{
    const char *p = (const char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = sendto(fd, p, remaining, 0,
                           (const struct sockaddr *)dst, sizeof(*dst));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

static int udp_recv_all(int fd, void *buf, size_t len,
                        const struct sockaddr_in *expected_src)
{
    (void)expected_src;
    char *p = (char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n;

        n = recvfrom(fd, p, remaining, 0,
                     (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += n;
        remaining -= (size_t)n;
    }
    return 0;
}

static int do_startup_sync(int udp_fd, const char *role,
                           const struct sockaddr_in *peer_addr)
{
    long deadline;
    char sync_buf[64];

    if (strcmp(role, "nodeA") == 0) {
        snprintf(sync_buf, sizeof(sync_buf), "RDMA_SYNC_REQ");
        if (udp_send_all(udp_fd, sync_buf, strlen(sync_buf), peer_addr) < 0) {
            fprintf(stderr, "[ub_rdma] sync: send REQ failed\n");
            return -1;
        }
        deadline = now_ms() + SYNC_TIMEOUT_MS;
        while (now_ms() < deadline) {
            char resp[64];
            ssize_t n;
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);

            n = recvfrom(udp_fd, resp, sizeof(resp) - 1, 0,
                         (struct sockaddr *)&src, &slen);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(100000);
                    continue;
                }
                return -1;
            }
            resp[n] = '\0';
            if (strncmp(resp, "RDMA_SYNC_ACK", 13) == 0 ||
                (n >= 15 && strncmp(resp + 2, "RDMA_SYNC_ACK", 13) == 0)) {
                printf("[ub_rdma] sync: peer acknowledged\n");
                return 0;
            }
        }
        fprintf(stderr, "[ub_rdma] sync: timeout waiting for ACK\n");
        return -1;
    } else {
        deadline = now_ms() + SYNC_TIMEOUT_MS;
        while (now_ms() < deadline) {
            ssize_t n;
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);

            n = recvfrom(udp_fd, sync_buf, sizeof(sync_buf) - 1, 0,
                         (struct sockaddr *)&src, &slen);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(100000);
                    continue;
                }
                return -1;
            }
            sync_buf[n] = '\0';
            if (strncmp(sync_buf, "RDMA_SYNC_REQ", 13) == 0 ||
                (n >= 15 && strncmp(sync_buf + 2, "RDMA_SYNC_REQ", 13) == 0)) {
                snprintf(sync_buf, sizeof(sync_buf), "RDMA_SYNC_ACK");
                if (udp_send_all(udp_fd, sync_buf, strlen(sync_buf),
                                 &src) < 0) {
                    fprintf(stderr, "[ub_rdma] sync: send ACK failed\n");
                    return -1;
                }
                printf("[ub_rdma] sync: sent ACK to peer\n");
                return 0;
            }
        }
        fprintf(stderr, "[ub_rdma] sync: timeout waiting for REQ\n");
        return -1;
    }
}

static int exchange_peer_info(int udp_fd, const char *role,
                              const struct sockaddr_in *peer_addr,
                              const struct peer_info *local,
                              struct peer_info *remote)
{
    if (strcmp(role, "nodeA") == 0) {
        if (udp_send_all(udp_fd, local, sizeof(*local), peer_addr) < 0) {
            fprintf(stderr, "[ub_rdma] exchange: send local info failed\n");
            return -1;
        }
        if (udp_recv_all(udp_fd, remote, sizeof(*remote), peer_addr) < 0) {
            fprintf(stderr, "[ub_rdma] exchange: recv remote info failed\n");
            return -1;
        }
    } else {
        if (udp_recv_all(udp_fd, remote, sizeof(*remote), peer_addr) < 0) {
            fprintf(stderr, "[ub_rdma] exchange: recv remote info failed\n");
            return -1;
        }
        if (udp_send_all(udp_fd, local, sizeof(*local), peer_addr) < 0) {
            fprintf(stderr, "[ub_rdma] exchange: send local info failed\n");
            return -1;
        }
    }
    return 0;
}

/* ---------- cleanup ---------- */

static void cleanup_resources(struct rdma_resources *res)
{
    if (res->fd >= 0) {
        if (res->seg_registered) {
            struct uburma_cmd_unregister_seg cmd;
            int ret;

            memset(&cmd, 0, sizeof(cmd));
            cmd.in.handle = res->seg_handle;
            ret = ub_ioctl(res->fd, UBURMA_CMD_UNREGISTER_SEG,
                           &cmd, sizeof(cmd));
            if (ret < 0) {
                fprintf(stderr, "[ub_rdma] cleanup: unregister_seg failed: %d\n", ret);
            } else {
                printf("[ub_rdma] cleanup: unregister_seg -> ok\n");
                res->seg_registered = false;
                res->seg_handle = 0;
            }
        }
        if (res->token_id_handle != 0) {
            struct uburma_cmd_free_token_id cmd;
            int ret;

            memset(&cmd, 0, sizeof(cmd));
            cmd.in.handle = res->token_id_handle;
            cmd.in.token_id = res->token_id;
            ret = ub_ioctl(res->fd, UBURMA_CMD_FREE_TOKEN_ID,
                           &cmd, sizeof(cmd));
            if (ret < 0) {
                fprintf(stderr, "[ub_rdma] cleanup: free_token_id failed: %d\n", ret);
            } else {
                printf("[ub_rdma] cleanup: free_token_id -> ok\n");
                res->token_id = 0;
                res->token_id_handle = 0;
            }
        }
        if (res->jetty_alloc) {
            /* no explicit free jetty ioctl in demo; resource freed by fd close */
        }
        if (res->jfs_alloc) {
            /* no explicit free jfs ioctl in demo; resource freed by fd close */
        }
        if (res->jfr_alloc) {
            /* no explicit free jfr ioctl in demo; resource freed by fd close */
        }
        if (res->jfc_alloc) {
            /* no explicit free jfc ioctl in demo; resource freed by fd close */
        }

        close(res->fd);
        res->fd = -1;
    }

    if (res->ummu_fd >= 0) {
        if (res->ummu_tid_allocated) {
            int ret = free_ummu_tid(res->ummu_fd, res->ummu_tid);

            if (ret < 0) {
                fprintf(stderr, "[ub_rdma] cleanup: free_ummu_tid failed: %d\n", ret);
            } else {
                printf("[ub_rdma] cleanup: free_ummu_tid -> ok\n");
                res->ummu_tid = 0;
                res->ummu_tid_allocated = false;
            }
        }
        close(res->ummu_fd);
        res->ummu_fd = -1;
    }

    free_ptr(&res->seg_buf);
    if (res->jfc_ucmd_is_mmap && res->jfc_ucmd_buf != NULL) {
        munmap(res->jfc_ucmd_buf, res->jfc_ucmd_len);
        res->jfc_ucmd_buf = NULL;
    } else {
        free_ptr(&res->jfc_ucmd_buf);
    }
    if (res->jfc_db_is_mmap && res->jfc_db_buf != NULL) {
        munmap(res->jfc_db_buf, res->jfc_db_len);
        res->jfc_db_buf = NULL;
    } else {
        free_ptr(&res->jfc_db_buf);
    }
    if (res->jfr_buf_is_mmap && res->jfr_buf != NULL) {
        munmap(res->jfr_buf, res->jfr_buf_len);
        res->jfr_buf = NULL;
    } else {
        free_ptr(&res->jfr_buf);
    }
    if (res->jfr_idx_buf_is_mmap && res->jfr_idx_buf != NULL) {
        munmap(res->jfr_idx_buf, res->jfr_idx_buf_len);
        res->jfr_idx_buf = NULL;
    } else {
        free_ptr(&res->jfr_idx_buf);
    }
    if (res->jfr_db_buf_is_mmap && res->jfr_db_buf != NULL) {
        munmap(res->jfr_db_buf, res->jfr_db_buf_len);
        res->jfr_db_buf = NULL;
    } else {
        free_ptr(&res->jfr_db_buf);
    }
    free_ptr(&res->jfs_buf);
    free_ptr(&res->jfs_db_buf);
    if (res->jetty_buf_is_mmap && res->jetty_buf != NULL) {
        munmap(res->jetty_buf, res->jetty_buf_len);
        res->jetty_buf = NULL;
    } else {
        free_ptr(&res->jetty_buf);
    }
    if (!res->jetty_dsqe_is_mmap) {
        free_ptr(&res->jetty_db_buf);
    } else {
        res->jetty_db_buf = NULL;
    }
    if (res->jetty_dsqe_is_mmap && res->jetty_dsqe_page != NULL) {
        munmap(res->jetty_dsqe_page, res->jetty_dsqe_len);
        res->jetty_dsqe_page = NULL;
    }
    res->jfc_ucmd_len = 0;
    res->jfc_db_len = 0;
    res->jfr_buf_len = 0;
    res->jfr_idx_buf_len = 0;
    res->jfr_db_buf_len = 0;
    res->jetty_buf_len = 0;
    res->jetty_dsqe_len = 0;
    res->jfc_ucmd_is_mmap = false;
    res->jfc_db_is_mmap = false;
    res->jfr_buf_is_mmap = false;
    res->jfr_idx_buf_is_mmap = false;
    res->jfr_db_buf_is_mmap = false;
    res->jetty_buf_is_mmap = false;
    res->jetty_dsqe_is_mmap = false;
    res->seg_len = 0;
}

/* ---------- main ---------- */

int main(void)
{
    char role[32] = "unknown";
    char ifname[IFNAMSIZ] = {0};
    struct in_addr local_addr = {0};
    struct in_addr desired_local = {0};
    struct in_addr peer_addr = {0};
    char dev_name[256];
    unsigned int ifindex = 0;
    char my_ip[INET_ADDRSTRLEN] = {0};
    char peer_ip[INET_ADDRSTRLEN] = {0};
    struct sockaddr_in sync_addr;
    struct sockaddr_in data_addr;
    int udp_fd;
    struct timeval tv;
    int one = 1;

    memset(&g_res, 0, sizeof(g_res));
    g_res.fd = -1;
    g_res.ummu_fd = -1;

    printf("[ub_rdma] start\n");

    /* --- parse role from cmdline --- */
    if (!cmdline_get_value("linqu_urma_dp_role", role, sizeof(role))) {
        fprintf(stderr, "[ub_rdma] fail: no linqu_urma_dp_role in cmdline\n");
        return 1;
    }
    printf("[ub_rdma] role=%s\n", role);

    if (!resolve_ipv4_pair(role, my_ip, sizeof(my_ip), peer_ip, sizeof(peer_ip))) {
        fprintf(stderr, "[ub_rdma] fail: missing ip config for role '%s'\n", role);
        return 1;
    }

    /* --- network init --- */
    if (!wait_iface_ready(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, "[ub_rdma] fail: ipourma iface not ready\n");
        return 1;
    }
    inet_pton(AF_INET, my_ip, &desired_local);
    if (!get_local_ipv4(ifname, &local_addr) || local_addr.s_addr != desired_local.s_addr) {
        fprintf(stderr, "[ub_rdma] warn: bootstrap ipv4 missing or mismatched on %s, applying %s\n",
                ifname, my_ip);
        if (!set_ipv4_addr(ifname, my_ip)) {
            fprintf(stderr, "[ub_rdma] fail: set ipv4 %s on %s failed\n", my_ip, ifname);
            return 1;
        }
    }
    if (!get_local_ipv4(ifname, &local_addr)) {
        fprintf(stderr, "[ub_rdma] fail: get ipv4 addr on %s failed\n", ifname);
        return 1;
    }
    if (inet_pton(AF_INET, peer_ip, &peer_addr) != 1) {
        fprintf(stderr, "[ub_rdma] fail: peer ip parse failed for %s\n", peer_ip);
        return 1;
    }
    install_static_arp(ifname, &peer_addr);

    {
        char buf[INET_ADDRSTRLEN];
        printf("[ub_rdma] iface=%s ifindex=%u local=%s peer=%s\n",
               ifname, ifindex,
               inet_ntop(AF_INET, &local_addr, buf, sizeof(buf)),
               inet_ntop(AF_INET, &peer_addr,
                         (char[INET_ADDRSTRLEN]){0}, INET_ADDRSTRLEN));
    }

    /* --- device discovery --- */
    if (discover_uburma_device(dev_name, sizeof(dev_name)) < 0) {
        printf("[ub_rdma] skip: no uburma device found\n");
        return 0;
    }
    printf("[ub_rdma] found device: %s\n", dev_name);

    g_res.fd = open_uburma_device(dev_name);
    if (g_res.fd < 0) {
        fprintf(stderr, "[ub_rdma] fail: cannot open uburma device\n");
        return 1;
    }
    g_res.ummu_fd = open_ummu_tid_device();
    if (g_res.ummu_fd < 0) {
        cleanup_resources(&g_res);
        fprintf(stderr, "[ub_rdma] fail\n");
        return 1;
    }

    /* --- step 1: query device attributes --- */
    {
        struct uburma_cmd_query_device_attr cmd;
        int ret;

        memset(&cmd, 0, sizeof(cmd));
        if (copy_dev_name(cmd.in.dev_name, sizeof(cmd.in.dev_name), dev_name) != 0) {
            cleanup_resources(&g_res);
            fprintf(stderr, "[ub_rdma] fail\n");
            return 1;
        }

        ret = ub_ioctl(g_res.fd, UBURMA_CMD_QUERY_DEV_ATTR, &cmd, sizeof(cmd));
        if (ret < 0) {
            fprintf(stderr, "[ub_rdma] step 1: query_dev_attr unavailable: %d\n",
                    ret);
            cleanup_resources(&g_res);
            fprintf(stderr, "[ub_rdma] fail\n");
            return 1;
        } else {
            printf("[ub_rdma] step 1: query_dev_attr -> ok\n");
            printf("[ub_rdma]   max_jfc=%u max_jfs=%u max_jfr=%u max_jetty=%u\n",
                   cmd.out.attr.dev_cap.max_jfc,
                   cmd.out.attr.dev_cap.max_jfs,
                   cmd.out.attr.dev_cap.max_jfr,
                   cmd.out.attr.dev_cap.max_jetty);
            printf("[ub_rdma]   max_jfc_depth=%u max_jfs_depth=%u max_jfr_depth=%u\n",
                   cmd.out.attr.dev_cap.max_jfc_depth,
                   cmd.out.attr.dev_cap.max_jfs_depth,
                   cmd.out.attr.dev_cap.max_jfr_depth);
            printf("[ub_rdma]   max_msg_size=%" PRIu64 " port_cnt=%u\n",
                   cmd.out.attr.dev_cap.max_msg_size,
                   cmd.out.attr.port_cnt);
        }
    }

    /* --- step 2: create context --- */
    {
        struct uburma_cmd_create_ctx cmd;
        uint8_t ctx_resp[256];

        memset(&cmd, 0, sizeof(cmd));
        memset(ctx_resp, 0, sizeof(ctx_resp));
        if (!read_eid_from_sysfs(cmd.in.eid)) {
            memset(cmd.in.eid, 0, sizeof(cmd.in.eid));
        }
        cmd.in.eid_index = 0;
        zero_udrv_priv(&cmd.udata);
        cmd.udata.out_addr = (uint64_t)(uintptr_t)ctx_resp;
        cmd.udata.out_len = sizeof(ctx_resp);

        STEP_CHECK(2, "create_ctx",
                    ub_ioctl(g_res.fd, UBURMA_CMD_CREATE_CTX,
                             &cmd, sizeof(cmd)));
        g_res.ctx_created = true;
    }
    {
        struct uburma_cmd_alloc_token_id cmd;
        uint32_t user_token_id;

        STEP_CHECK(2, "alloc_ummu_tid",
                    alloc_ummu_tid(g_res.ummu_fd, &g_res.ummu_tid));
        g_res.ummu_tid_allocated = true;
        user_token_id = g_res.ummu_tid << UDMA_TID_SHIFT_USER;

        memset(&cmd, 0, sizeof(cmd));
        zero_udrv_priv(&cmd.udata);
        cmd.flag.value = 0;
        cmd.udata.in_addr = (uint64_t)(uintptr_t)&user_token_id;
        cmd.udata.in_len = sizeof(user_token_id);

        STEP_CHECK(2, "alloc_token_id",
                    ub_ioctl(g_res.fd, UBURMA_CMD_ALLOC_TOKEN_ID,
                             &cmd, sizeof(cmd)));
        if (cmd.out.handle == 0) {
            fprintf(stderr, "[ub_rdma] step 2: alloc_token_id returned empty handle\n");
            cleanup_resources(&g_res);
            fprintf(stderr, "[ub_rdma] fail\n");
            return 1;
        }
        g_res.token_id = cmd.out.token_id;
        g_res.token_id_handle = cmd.out.handle;
    }
    if (ensure_runtime_buffers(&g_res) != 0) {
        fprintf(stderr, "[ub_rdma] fail: alloc runtime buffers failed\n");
        cleanup_resources(&g_res);
        return 1;
    }

    /* --- step 3: alloc + active JFC --- */
    {
        struct uburma_cmd_alloc_jfc cmd;

        memset(&cmd, 0, sizeof(cmd));
        cmd.in.depth = 64;
        cmd.in.flag = 0;
        cmd.in.jfce_fd = -1;
        cmd.in.urma_jfc = 0;
        cmd.in.ceqn = 0;
        zero_udrv_priv(&cmd.udata);

        STEP_CHECK(3, "alloc_jfc",
                    ub_ioctl(g_res.fd, UBURMA_CMD_ALLOC_JFC,
                             &cmd, sizeof(cmd)));
        g_res.jfc_id = cmd.out.id;
        g_res.jfc_handle = cmd.out.handle;
        g_res.jfc_alloc = true;
    }
    {
        struct uburma_cmd_active_jfc cmd;
        struct udma_create_jfc_ucmd_compat ucmd;
        uint8_t jfc_opt_blob[256];

        memset(&cmd, 0, sizeof(cmd));
        memset(&ucmd, 0, sizeof(ucmd));
        memset(jfc_opt_blob, 0, sizeof(jfc_opt_blob));
        cmd.in.handle = g_res.jfc_handle;
        cmd.in.depth = 64;
        cmd.in.flag = 0;
        cmd.in.ceqn = 0;
        cmd.in.urma_jfc_opt = (uint64_t)(uintptr_t)jfc_opt_blob;
        zero_udrv_priv(&cmd.udata);

        ucmd.buf_addr = (uint64_t)(uintptr_t)g_res.jfc_ucmd_buf;
        ucmd.buf_len = DEMO_PAGE_SIZE;
        ucmd.mode = UDMA_KERNEL_STARS_JFC_TYPE;
        ucmd.db_addr = (uint64_t)(uintptr_t)g_res.jfc_db_buf;
        ucmd.is_hugepage = 0;

        cmd.udata.in_addr = (uint64_t)(uintptr_t)&ucmd;
        cmd.udata.in_len = sizeof(ucmd);

        STEP_CHECK(3, "active_jfc",
                   ub_ioctl(g_res.fd, UBURMA_CMD_ACTIVE_JFC,
                            &cmd, sizeof(cmd)));
    }

    /* --- step 4: alloc + active JFR --- */
    {
        struct uburma_cmd_alloc_jfr cmd;

        memset(&cmd, 0, sizeof(cmd));
        cmd.in.depth = 64;
        cmd.in.flag = 0;
        cmd.in.trans_mode = UBCORE_TP_RC_USER;
        cmd.in.max_sge = 1;
        cmd.in.min_rnr_timer = 0;
        cmd.in.jfc_id = g_res.jfc_id;
        cmd.in.jfc_handle = g_res.jfc_handle;
        cmd.in.token = 0;
        cmd.in.id = 0;
        cmd.in.urma_jfr = 0;
        zero_udrv_priv(&cmd.udata);

        STEP_CHECK(4, "alloc_jfr",
                    ub_ioctl(g_res.fd, UBURMA_CMD_ALLOC_JFR,
                             &cmd, sizeof(cmd)));
        if (cmd.out.handle == 0) {
            fprintf(stderr, "[ub_rdma] step 4: alloc_jfr returned empty handle\n");
            cleanup_resources(&g_res);
            fprintf(stderr, "[ub_rdma] fail\n");
            return 1;
        }
        g_res.jfr_id = cmd.out.id;
        g_res.jfr_handle = cmd.out.handle;
        g_res.jfr_alloc = true;
    }
    {
        struct uburma_cmd_active_jfr cmd;
        struct udma_create_jetty_ucmd_compat ucmd;
        uint8_t jfr_opt_blob[256];

        memset(&cmd, 0, sizeof(cmd));
        memset(&ucmd, 0, sizeof(ucmd));
        memset(jfr_opt_blob, 0, sizeof(jfr_opt_blob));
        cmd.in.handle = g_res.jfr_handle;
        cmd.in.depth = 64;
        cmd.in.flag = 0;
        cmd.in.trans_mode = UBCORE_TP_RC_USER;
        cmd.in.max_sge = 1;
        cmd.in.min_rnr_timer = 0;
        cmd.in.jfc_id = g_res.jfc_id;
        cmd.in.jfc_handle = g_res.jfc_handle;
        cmd.in.token_value = 0;
        cmd.in.urma_jfr_opt = (uint64_t)(uintptr_t)jfr_opt_blob;
        zero_udrv_priv(&cmd.udata);

        ucmd.buf_addr = (uint64_t)(uintptr_t)g_res.jfr_buf;
        ucmd.buf_len = DEMO_PAGE_SIZE;
        ucmd.db_addr = (uint64_t)(uintptr_t)g_res.jfr_db_buf;
        ucmd.idx_addr = (uint64_t)(uintptr_t)g_res.jfr_idx_buf;
        ucmd.idx_len = DEMO_PAGE_SIZE;
        ucmd.jetty_addr = (uint64_t)(uintptr_t)g_res.jfr_buf;
        ucmd.non_pin = 0;
        ucmd.is_hugepage = 0;
        ucmd.jfr_sleep_buf = 0;

        cmd.udata.in_addr = (uint64_t)(uintptr_t)&ucmd;
        cmd.udata.in_len = sizeof(ucmd);

        STEP_CHECK(4, "active_jfr",
                    ub_ioctl(g_res.fd, UBURMA_CMD_ACTIVE_JFR,
                             &cmd, sizeof(cmd)));
    }

    /* --- step 5: alloc + active JFS (optional standalone probe) --- */
    {
        const bool run_standalone_jfs_probe =
            cmdline_get_bool("linqu_rdma_probe_standalone_jfs");
        const uint32_t jfs_modes[] = {
            UBCORE_TP_RM_USER,
            UBCORE_TP_UM_USER,
            UBCORE_TP_RC_USER,
        };
        const char *jfs_mode_names[] = {"RM", "UM", "RC"};
        bool jfs_ready = false;
        size_t mode_idx;

        if (!run_standalone_jfs_probe) {
            printf("[ub_rdma] step 5: standalone jfs probe disabled (set linqu_rdma_probe_standalone_jfs=1 to enable)\n");
        }

        for (mode_idx = 0; run_standalone_jfs_probe && mode_idx < ARRAY_SIZE(jfs_modes);
             mode_idx++) {
            struct uburma_cmd_alloc_jfs alloc_cmd;
            struct uburma_cmd_active_jfs active_cmd;
            struct udma_create_jetty_ucmd_compat ucmd;
            uint8_t jfs_opt_blob[256];
            int ret;

            memset(&alloc_cmd, 0, sizeof(alloc_cmd));
            alloc_cmd.in.depth = 64;
            alloc_cmd.in.flag = 0;
            alloc_cmd.in.trans_mode = jfs_modes[mode_idx];
            alloc_cmd.in.priority = 0;
            alloc_cmd.in.max_sge = 1;
            alloc_cmd.in.max_rsge = 0;
            alloc_cmd.in.max_inline_data = 0;
            alloc_cmd.in.rnr_retry = 0;
            alloc_cmd.in.err_timeout = 0;
            alloc_cmd.in.jfc_id = g_res.jfc_id;
            alloc_cmd.in.jfc_handle = g_res.jfc_handle;
            alloc_cmd.in.urma_jfs = 0;
            zero_udrv_priv(&alloc_cmd.udata);

            ret = ub_ioctl(g_res.fd, UBURMA_CMD_ALLOC_JFS,
                           &alloc_cmd, sizeof(alloc_cmd));
            if (ret < 0) {
                fprintf(stderr, "[ub_rdma] step 5: alloc_jfs(%s) failed: %d\n",
                        jfs_mode_names[mode_idx], ret);
                continue;
            }
            if (alloc_cmd.out.handle == 0) {
                fprintf(stderr,
                        "[ub_rdma] step 5: alloc_jfs(%s) returned empty handle\n",
                        jfs_mode_names[mode_idx]);
                continue;
            }

            g_res.jfs_id = alloc_cmd.out.id;
            g_res.jfs_handle = alloc_cmd.out.handle;
            g_res.jfs_alloc = true;

            memset(&active_cmd, 0, sizeof(active_cmd));
            memset(&ucmd, 0, sizeof(ucmd));
            memset(jfs_opt_blob, 0, sizeof(jfs_opt_blob));
            active_cmd.in.handle = g_res.jfs_handle;
            active_cmd.in.depth = 64;
            active_cmd.in.flag = 0;
            active_cmd.in.trans_mode = jfs_modes[mode_idx];
            active_cmd.in.priority = 0;
            active_cmd.in.max_sge = 1;
            active_cmd.in.max_rsge = 0;
            active_cmd.in.max_inline_data = 0;
            active_cmd.in.rnr_retry = 0;
            active_cmd.in.err_timeout = 0;
            active_cmd.in.jfc_id = g_res.jfc_id;
            active_cmd.in.jfc_handle = g_res.jfc_handle;
            active_cmd.in.jfs_opt = (uint64_t)(uintptr_t)jfs_opt_blob;
            zero_udrv_priv(&active_cmd.udata);

            ucmd.buf_addr = (uint64_t)(uintptr_t)g_res.jfs_buf;
            ucmd.buf_len = DEMO_PAGE_SIZE;
            ucmd.db_addr = (uint64_t)(uintptr_t)g_res.jfs_db_buf;
            ucmd.idx_addr = 0;
            ucmd.idx_len = 0;
            ucmd.sqe_bb_cnt = 1;
            ucmd.jetty_addr = (uint64_t)(uintptr_t)g_res.jfs_buf;
            ucmd.non_pin = 0;
            ucmd.is_hugepage = 0;
            ucmd.jfr_sleep_buf = 0;
            ucmd.jfs_id = 0;
            ucmd.jetty_type = UDMA_URMA_NORMAL_JETTY_TYPE;

            active_cmd.udata.in_addr = (uint64_t)(uintptr_t)&ucmd;
            active_cmd.udata.in_len = sizeof(ucmd);

            ret = ub_ioctl(g_res.fd, UBURMA_CMD_ACTIVE_JFS,
                           &active_cmd, sizeof(active_cmd));
            if (ret < 0) {
                fprintf(stderr, "[ub_rdma] step 5: active_jfs(%s) failed: %d\n",
                        jfs_mode_names[mode_idx], ret);
                g_res.jfs_alloc = false;
                g_res.jfs_id = 0;
                g_res.jfs_handle = 0;
                continue;
            }

            printf("[ub_rdma] step 5: standalone jfs active in %s mode\n",
                   jfs_mode_names[mode_idx]);
            jfs_ready = true;
            break;
        }

        if (run_standalone_jfs_probe && !jfs_ready) {
            fprintf(stderr,
                    "[ub_rdma] step 5: standalone jfs unavailable, continue with jetty path\n");
        }
    }

    /* --- step 6: alloc + active Jetty --- */
    {
        struct uburma_cmd_alloc_jetty cmd;

        memset(&cmd, 0, sizeof(cmd));
        cmd.create_jetty.in.id = 0;
        cmd.create_jetty.in.jetty_flag = 0;
        cmd.create_jetty.in.jfs_depth = 64;
        cmd.create_jetty.in.jfs_flag = 0;
        cmd.create_jetty.in.trans_mode = UBCORE_TP_RC_USER;
        cmd.create_jetty.in.priority = 0;
        cmd.create_jetty.in.max_send_sge = 1;
        cmd.create_jetty.in.max_send_rsge = 0;
        cmd.create_jetty.in.max_inline_data = 0;
        cmd.create_jetty.in.rnr_retry = 0;
        cmd.create_jetty.in.err_timeout = 0;
        cmd.create_jetty.in.send_jfc_id = g_res.jfc_id;
        cmd.create_jetty.in.send_jfc_handle = g_res.jfc_handle;
        cmd.create_jetty.in.jfr_depth = 64;
        cmd.create_jetty.in.jfr_flag = 0;
        cmd.create_jetty.in.max_recv_sge = 1;
        cmd.create_jetty.in.min_rnr_timer = 0;
        cmd.create_jetty.in.recv_jfc_id = g_res.jfc_id;
        cmd.create_jetty.in.recv_jfc_handle = g_res.jfc_handle;
        cmd.create_jetty.in.token = 0;
        cmd.create_jetty.in.jfr_id = g_res.jfr_id;
        cmd.create_jetty.in.jfr_handle = g_res.jfr_handle;
        cmd.create_jetty.in.jetty_grp_handle = 0;
        cmd.create_jetty.in.is_jetty_grp = 0;
        cmd.create_jetty.in.urma_jetty = 0;
        zero_udrv_priv(&cmd.create_jetty.udata);

        STEP_CHECK(6, "alloc_jetty",
                    ub_ioctl(g_res.fd, UBURMA_CMD_ALLOC_JETTY,
                             &cmd, sizeof(cmd)));
        g_res.jetty_id = cmd.create_jetty.out.id;
        g_res.jetty_handle = cmd.create_jetty.out.handle;
        g_res.jetty_alloc = true;
    }
    {
        struct uburma_cmd_active_jetty cmd;
        struct udma_create_jetty_ucmd_compat ucmd;
        struct udma_create_jetty_resp_compat jetty_resp;
        uint8_t jetty_opt_blob[256];

        memset(&cmd, 0, sizeof(cmd));
        memset(&ucmd, 0, sizeof(ucmd));
        memset(&jetty_resp, 0, sizeof(jetty_resp));
        memset(jetty_opt_blob, 0, sizeof(jetty_opt_blob));
        cmd.in.flag = 0;
        cmd.in.handle = g_res.jetty_handle;
        cmd.in.send_jfc_handle = g_res.jfc_handle;
        cmd.in.recv_jfc_handle = g_res.jfc_handle;
        cmd.in.urma_jetty = 0;
        cmd.in.jetty_opt = (uint64_t)(uintptr_t)jetty_opt_blob;
        zero_udrv_priv(&cmd.udata);

        ucmd.buf_addr = (uint64_t)(uintptr_t)g_res.jetty_buf;
        ucmd.buf_len = DEMO_PAGE_SIZE;
        ucmd.db_addr = (uint64_t)(uintptr_t)g_res.jetty_db_buf;
        ucmd.idx_addr = 0;
        ucmd.idx_len = 0;
        ucmd.sqe_bb_cnt = 1;
        ucmd.jetty_addr = (uint64_t)(uintptr_t)g_res.jetty_buf;
        ucmd.non_pin = 0;
        ucmd.is_hugepage = 0;
        ucmd.jfr_sleep_buf = 0;
        ucmd.jfs_id = 0;
        ucmd.jetty_type = UDMA_URMA_NORMAL_JETTY_TYPE;

        cmd.udata.in_addr = (uint64_t)(uintptr_t)&ucmd;
        cmd.udata.in_len = sizeof(ucmd);
        cmd.udata.out_addr = (uint64_t)(uintptr_t)&jetty_resp;
        cmd.udata.out_len = sizeof(jetty_resp);

        STEP_CHECK(6, "active_jetty",
                    ub_ioctl(g_res.fd, UBURMA_CMD_ACTIVE_JETTY,
                             &cmd, sizeof(cmd)));
        free_ptr(&g_res.jetty_db_buf);
        g_res.local_hw_jetty_id = cmd.out.jetty_id;
        g_res.jetty_dsqe_page = map_udma_jetty_dsqe_page(g_res.fd, g_res.local_hw_jetty_id);
        if (g_res.jetty_dsqe_page == NULL) {
            fprintf(stderr, "[ub_rdma] step 6: map jetty dsqe page failed: %s\n",
                    strerror(errno));
            cleanup_resources(&g_res);
            fprintf(stderr, "[ub_rdma] fail\n");
            return 1;
        }
        g_res.jetty_dsqe_len = DEMO_PAGE_SIZE;
        g_res.jetty_dsqe_is_mmap = true;
        g_res.jetty_db_buf = (uint8_t *)g_res.jetty_dsqe_page + UDMA_DOORBELL_OFFSET_LOCAL;
        printf("[ub_rdma] step 6: local_hw_jetty_id=%u db=%p\n",
               g_res.local_hw_jetty_id, g_res.jetty_db_buf);
    }

    /* --- step 7: register memory segment --- */
    {
        struct uburma_cmd_register_seg cmd;

        memset(&cmd, 0, sizeof(cmd));
        cmd.in.va = (uint64_t)(uintptr_t)g_res.seg_buf;
        cmd.in.len = g_res.seg_len;
        cmd.in.token_id = g_res.token_id;
        cmd.in.token_id_handle = g_res.token_id_handle;
        cmd.in.token = 0;
        cmd.in.flag = make_register_seg_flag();
        zero_udrv_priv(&cmd.udata);

        STEP_CHECK(7, "register_seg",
                    ub_ioctl(g_res.fd, UBURMA_CMD_REGISTER_SEG,
                             &cmd, sizeof(cmd)));
        g_res.token_id = cmd.out.token_id;
        g_res.seg_handle = cmd.out.handle;
        g_res.seg_registered = true;
    }

    /* --- step 8: UDP sync + info exchange + import jetty --- */
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        fprintf(stderr, "[ub_rdma] fail: udp socket create failed: %s\n",
                strerror(errno));
        cleanup_resources(&g_res);
        fprintf(stderr, "[ub_rdma] fail\n");
        return 1;
    }
    if (setsockopt(udp_fd, SOL_SOCKET, SO_NO_CHECK, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[ub_rdma] warn: sync SO_NO_CHECK failed: %s\n", strerror(errno));
    }

    memset(&sync_addr, 0, sizeof(sync_addr));
    sync_addr.sin_family = AF_INET;
    sync_addr.sin_port = htons(SYNC_PORT);
    sync_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(udp_fd, (struct sockaddr *)&sync_addr, sizeof(sync_addr)) != 0) {
        fprintf(stderr, "[ub_rdma] fail: bind sync port %d failed: %s\n",
                SYNC_PORT, strerror(errno));
        close(udp_fd);
        cleanup_resources(&g_res);
        fprintf(stderr, "[ub_rdma] fail\n");
        return 1;
    }

    memset(&data_addr, 0, sizeof(data_addr));
    data_addr.sin_family = AF_INET;
    data_addr.sin_port = htons(SYNC_PORT);
    data_addr.sin_addr = peer_addr;

    tv.tv_sec = SYNC_TIMEOUT_MS / 1000;
    tv.tv_usec = (SYNC_TIMEOUT_MS % 1000) * 1000;
    setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    printf("[ub_rdma] step 8: starting sync on port %d\n", SYNC_PORT);
    if (do_startup_sync(udp_fd, role, &data_addr) < 0) {
        fprintf(stderr, "[ub_rdma] step 8: startup sync failed\n");
        close(udp_fd);
        cleanup_resources(&g_res);
        fprintf(stderr, "[ub_rdma] fail\n");
        return 1;
    }

    /* prepare local peer info for exchange */
    {
        struct peer_info local_info;
        struct peer_info remote_info;
        struct sockaddr_in info_addr;

        memset(&local_info, 0, sizeof(local_info));
        memset(&remote_info, 0, sizeof(remote_info));

        if (!read_eid_from_sysfs(local_info.eid)) {
            memset(local_info.eid, 0, sizeof(local_info.eid));
        }
        /*
         * The device model routes incoming URMA payloads by the active hardware
         * jetty id returned from ACTIVE_JETTY, not the alloc-time logical id.
         */
        local_info.jetty_id = g_res.local_hw_jetty_id;
        local_info.token = g_res.token_id;

        /* rebind to data port for info exchange */
        close(udp_fd);
        udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_fd < 0) {
            fprintf(stderr, "[ub_rdma] fail: udp socket for data failed\n");
            cleanup_resources(&g_res);
            fprintf(stderr, "[ub_rdma] fail\n");
            return 1;
        }
        if (setsockopt(udp_fd, SOL_SOCKET, SO_NO_CHECK, &one, sizeof(one)) != 0) {
            fprintf(stderr, "[ub_rdma] warn: data SO_NO_CHECK failed: %s\n", strerror(errno));
        }
        setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        tv.tv_sec = SYNC_TIMEOUT_MS / 1000;
        tv.tv_usec = (SYNC_TIMEOUT_MS % 1000) * 1000;
        setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        memset(&info_addr, 0, sizeof(info_addr));
        info_addr.sin_family = AF_INET;
        info_addr.sin_port = htons(RDMA_PORT);
        info_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(udp_fd, (struct sockaddr *)&info_addr,
                 sizeof(info_addr)) != 0) {
            fprintf(stderr, "[ub_rdma] fail: bind data port %d failed: %s\n",
                    RDMA_PORT, strerror(errno));
            close(udp_fd);
            cleanup_resources(&g_res);
            fprintf(stderr, "[ub_rdma] fail\n");
            return 1;
        }

        memset(&data_addr, 0, sizeof(data_addr));
        data_addr.sin_family = AF_INET;
        data_addr.sin_port = htons(RDMA_PORT);
        data_addr.sin_addr = peer_addr;

        if (exchange_peer_info(udp_fd, role, &data_addr,
                               &local_info, &remote_info) < 0) {
            fprintf(stderr, "[ub_rdma] step 8: exchange peer info failed\n");
            close(udp_fd);
            cleanup_resources(&g_res);
            fprintf(stderr, "[ub_rdma] fail\n");
            return 1;
        }
        printf("[ub_rdma] step 8: exchange info -> ok\n");
        printf("[ub_rdma]   peer jetty_id=%u token=%u\n",
               remote_info.jetty_id, remote_info.token);
        memcpy(g_res.peer_eid, remote_info.eid, sizeof(g_res.peer_eid));
        reverse_bytes(remote_info.eid, g_res.peer_eid, sizeof(g_res.peer_eid));
        g_res.peer_jetty_id = remote_info.jetty_id;

        /* import peer jetty */
        {
            struct uburma_cmd_import_jetty cmd;

            memset(&cmd, 0, sizeof(cmd));
            memcpy(cmd.in.eid, remote_info.eid, UBCORE_EID_SIZE);
            cmd.in.id = remote_info.jetty_id;
            cmd.in.flag = 0;
            cmd.in.token = remote_info.token;
            cmd.in.trans_mode = UBCORE_TP_RC_USER;
            cmd.in.policy = UBCORE_JETTY_GRP_POLICY_RR_USER;
            cmd.in.type = UBCORE_JETTY_USER;
            cmd.in.tp_type = UBCORE_RTP_USER;
            zero_udrv_priv(&cmd.udata);

            STEP_CHECK(8, "import_jetty",
                        ub_ioctl(g_res.fd, UBURMA_CMD_IMPORT_JETTY,
                                 &cmd, sizeof(cmd)));
            g_res.tjetty_handle = cmd.out.handle;
            g_res.tjetty_imported = true;
            printf("[ub_rdma]   imported tjetty handle=0x%" PRIx64
                   " tpn=%u\n",
                   g_res.tjetty_handle, cmd.out.tpn);
        }

        /* --- step 9: bind jetty --- */
        {
            struct uburma_cmd_bind_jetty cmd;

            memset(&cmd, 0, sizeof(cmd));
            cmd.in.jetty_handle = g_res.jetty_handle;
            cmd.in.tjetty_handle = g_res.tjetty_handle;
            zero_udrv_priv(&cmd.udata);

            STEP_CHECK(9, "bind_jetty",
                        ub_ioctl(g_res.fd, UBURMA_CMD_BIND_JETTY,
                                 &cmd, sizeof(cmd)));
            g_res.bound_tpn = cmd.out.tpn;
            printf("[ub_rdma]   bound tpn=%u\n", cmd.out.tpn);
        }

        /* --- step 9.5: actual payload round-trip over bound jetty --- */
        {
            struct udma_jfc_cqe_local cqe;
            uint8_t *tx_buf = g_res.seg_buf;
            uint8_t *rx_buf = (uint8_t *)g_res.seg_buf + 2048;
            const char req_msg[] = "rdma request payload from NodeA";
            const char reply_msg[] = "rdma reply payload from NodeB";
            int ret;

            memset(rx_buf, 0, RDMA_MAX_PAYLOAD);
            ret = rdma_post_recv_one(&g_res, rx_buf, RDMA_MAX_PAYLOAD);
            if (ret < 0) {
                fprintf(stderr, "[ub_rdma] step 9.5: post_recv failed: %d\n", ret);
                close(udp_fd);
                cleanup_resources(&g_res);
                fprintf(stderr, "[ub_rdma] fail\n");
                return 1;
            }
            printf("[ub_rdma] step 9.5: post_recv -> ok\n");

            ret = sync_datapath_ready(udp_fd, role, &data_addr);
            if (ret < 0) {
                fprintf(stderr, "[ub_rdma] step 9.5: ready sync failed: %d\n", ret);
                close(udp_fd);
                cleanup_resources(&g_res);
                fprintf(stderr, "[ub_rdma] fail\n");
                return 1;
            }
            printf("[ub_rdma] step 9.5: ready_sync -> ok\n");

            if (strcmp(role, "nodeA") == 0) {
                memcpy(tx_buf, req_msg, sizeof(req_msg));
                ret = rdma_post_send_one(&g_res, tx_buf, sizeof(req_msg));
                if (ret < 0) {
                    fprintf(stderr, "[ub_rdma] step 9.5: send request failed: %d\n", ret);
                    close(udp_fd);
                    cleanup_resources(&g_res);
                    fprintf(stderr, "[ub_rdma] fail\n");
                    return 1;
                }
                ret = rdma_wait_for_cqe(&g_res, 5000, CQE_FOR_SEND_LOCAL, &cqe);
                if (ret < 0) {
                    fprintf(stderr, "[ub_rdma] step 9.5: wait request send cqe failed: %d\n", ret);
                    close(udp_fd);
                    cleanup_resources(&g_res);
                    fprintf(stderr, "[ub_rdma] fail\n");
                    return 1;
                }
                printf("[ub_rdma] step 9.5: send_request -> ok len=%u\n", cqe.byte_cnt);

                ret = rdma_wait_for_cqe(&g_res, 5000, CQE_FOR_RECEIVE_LOCAL, &cqe);
                if (ret < 0) {
                    fprintf(stderr, "[ub_rdma] step 9.5: wait reply recv cqe failed: %d\n", ret);
                    close(udp_fd);
                    cleanup_resources(&g_res);
                    fprintf(stderr, "[ub_rdma] fail\n");
                    return 1;
                }
                if (strcmp((char *)rx_buf, reply_msg) != 0) {
                    fprintf(stderr,
                            "[ub_rdma] step 9.5: reply payload mismatch got=\"%s\" expected=\"%s\"\n",
                            (char *)rx_buf, reply_msg);
                    close(udp_fd);
                    cleanup_resources(&g_res);
                    fprintf(stderr, "[ub_rdma] fail\n");
                    return 1;
                }
                printf("[ub_rdma] step 9.5: recv_reply -> ok payload=\"%s\"\n", rx_buf);
            } else {
                ret = rdma_wait_for_cqe(&g_res, 5000, CQE_FOR_RECEIVE_LOCAL, &cqe);
                if (ret < 0) {
                    fprintf(stderr, "[ub_rdma] step 9.5: wait request recv cqe failed: %d\n", ret);
                    close(udp_fd);
                    cleanup_resources(&g_res);
                    fprintf(stderr, "[ub_rdma] fail\n");
                    return 1;
                }
                if (strcmp((char *)rx_buf, req_msg) != 0) {
                    fprintf(stderr,
                            "[ub_rdma] step 9.5: request payload mismatch got=\"%s\" expected=\"%s\"\n",
                            (char *)rx_buf, req_msg);
                    close(udp_fd);
                    cleanup_resources(&g_res);
                    fprintf(stderr, "[ub_rdma] fail\n");
                    return 1;
                }
                printf("[ub_rdma] step 9.5: recv_request -> ok payload=\"%s\"\n", rx_buf);

                memcpy(tx_buf, reply_msg, sizeof(reply_msg));
                ret = rdma_post_send_one(&g_res, tx_buf, sizeof(reply_msg));
                if (ret < 0) {
                    fprintf(stderr, "[ub_rdma] step 9.5: send reply failed: %d\n", ret);
                    close(udp_fd);
                    cleanup_resources(&g_res);
                    fprintf(stderr, "[ub_rdma] fail\n");
                    return 1;
                }
                ret = rdma_wait_for_cqe(&g_res, 5000, CQE_FOR_SEND_LOCAL, &cqe);
                if (ret < 0) {
                    fprintf(stderr, "[ub_rdma] step 9.5: wait reply send cqe failed: %d\n", ret);
                    close(udp_fd);
                    cleanup_resources(&g_res);
                    fprintf(stderr, "[ub_rdma] fail\n");
                    return 1;
                }
                printf("[ub_rdma] step 9.5: send_reply -> ok len=%u\n", cqe.byte_cnt);
            }
        }

        close(udp_fd);
    }

    /* --- step 10: explicit unbind + unimport to avoid fd-close stall --- */
    {
        struct uburma_cmd_unbind_jetty_local cmd;

        memset(&cmd, 0, sizeof(cmd));
        cmd.in.jetty_handle = g_res.jetty_handle;
        cmd.in.tjetty_handle = g_res.tjetty_handle;
        STEP_CHECK(10, "unbind_jetty",
                    ub_ioctl(g_res.fd, UBURMA_CMD_UNBIND_JETTY,
                             &cmd, sizeof(cmd)));
    }
    {
        struct uburma_cmd_unimport_jetty_local cmd;
        int ret = 0;
        int attempt;

        memset(&cmd, 0, sizeof(cmd));
        cmd.in.handle = g_res.tjetty_handle;
        for (attempt = 0; attempt < 10; attempt++) {
            ret = ub_ioctl(g_res.fd, UBURMA_CMD_UNIMPORT_JETTY,
                           &cmd, sizeof(cmd));
            if (ret == 0) {
                break;
            }
            if (ret != -EBUSY && ret != -EAGAIN) {
                fprintf(stderr, "[ub_rdma] step 10: unimport_jetty failed: %d\n", ret);
                cleanup_resources(&g_res);
                fprintf(stderr, "[ub_rdma] fail\n");
                return 1;
            }
            usleep(100000);
        }
        if (ret == 0) {
            printf("[ub_rdma] step 10: unimport_jetty -> ok\n");
            g_res.tjetty_imported = false;
        } else {
            fprintf(stderr,
                    "[ub_rdma] step 10: unimport_jetty busy after retries (%d)\n",
                    ret);
            cleanup_resources(&g_res);
            fprintf(stderr, "[ub_rdma] fail\n");
            return 1;
        }
    }

    /* --- success --- */
    cleanup_resources(&g_res);
    printf("[ub_rdma] pass\n");
    return 0;
}
