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
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RPC_PORT       18557
#define SYNC_PORT      18559
#define WAIT_IFACE_MS  90000
#define SYNC_TIMEOUT_S 30
#define SERVER_TIMEOUT_S 60
#define RPC_ECHO_TEXT  "greeting from NodeA"
#define RPC_CRC_PAYLOAD "buffer from NodeA for CRC verification over ub_link"

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

/* ---------- helper: file read ------------------------------------------ */

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

/* ---------- helper: cmdline parse -------------------------------------- */

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

/* ---------- helper: interface discovery -------------------------------- */

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

/* ---------- helper: IPv4 address --------------------------------------- */

static bool set_ipv4_addr(const char *ifname, const char *addr_str)
{
    struct ifreq ifr;
    struct sockaddr_in *sin;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[ub_rpc] set_ipv4: socket failed: %s\n", strerror(errno));
        return false;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, addr_str, &sin->sin_addr) != 1) {
        fprintf(stderr, "[ub_rpc] set_ipv4: inet_pton failed for %s\n", addr_str);
        close(fd);
        return false;
    }

    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) {
        fprintf(stderr, "[ub_rpc] set_ipv4: SIOCSIFADDR failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    /* set netmask 255.255.255.0 */
    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    sin = (struct sockaddr_in *)&ifr.ifr_netmask;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) != 0) {
        fprintf(stderr, "[ub_rpc] set_ipv4: SIOCSIFNETMASK failed: %s\n", strerror(errno));
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

/* ---------- helper: static ARP ----------------------------------------- */

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
        fprintf(stderr, "[ub_rpc] warn: socket for SIOCSARP failed: %s\n", strerror(errno));
        return;
    }

    if (ioctl(fd, SIOCSARP, &req) != 0) {
        fprintf(stderr, "[ub_rpc] warn: SIOCSARP %s failed: %s\n",
                ifname, strerror(errno));
    } else {
        fprintf(stderr, "[ub_rpc] static arp installed for peer on %s\n", ifname);
    }

    close(fd);
}

/* ---------- helper: create UDP socket ---------------------------------- */

static int create_udp_socket(const char *ifname, int port)
{
    int sockfd;
    int one = 1;
    struct timeval tv;
    struct sockaddr_in bind_addr;
    int flags;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "[ub_rpc] fail: socket create failed: %s\n", strerror(errno));
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[ub_rpc] fail: SO_REUSEADDR failed: %s\n", strerror(errno));
        close(sockfd);
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_NO_CHECK, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[ub_rpc] warn: SO_NO_CHECK failed: %s\n", strerror(errno));
    }

    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    } else {
        fprintf(stderr, "[ub_rpc] warn: fcntl F_GETFL failed: %s\n", strerror(errno));
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) != 0) {
        fprintf(stderr, "[ub_rpc] warn: SO_BINDTODEVICE failed: %s\n", strerror(errno));
    }

    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        fprintf(stderr, "[ub_rpc] warn: SO_RCVTIMEO failed: %s\n", strerror(errno));
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons((uint16_t)port);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        fprintf(stderr, "[ub_rpc] fail: bind port %d failed: %s\n", port, strerror(errno));
        close(sockfd);
        return -1;
    }

    printf("[ub_rpc] bind ok port=%d\n", port);
    return sockfd;
}

/* ---------- RPC protocol helpers --------------------------------------- */

/* Build a request string: "RPC:<msg_id>:<op>:<payload_len>:<payload>" */
static int rpc_build_request(char *buf, size_t buf_len,
                             int msg_id, const char *op,
                             const char *payload)
{
    int n;

    n = snprintf(buf, buf_len, "RPC:%d:%s:%zu:%s",
                 msg_id, op, strlen(payload), payload);
    if (n < 0 || (size_t)n >= buf_len) {
        return -1;
    }
    return n;
}

/* Build a response string: "RPC_RSP:<msg_id>:<status>:<payload_len>:<payload>" */
static int rpc_build_response(char *buf, size_t buf_len,
                              int msg_id, const char *status,
                              const char *payload)
{
    int n;

    n = snprintf(buf, buf_len, "RPC_RSP:%d:%s:%zu:%s",
                 msg_id, status, strlen(payload), payload);
    if (n < 0 || (size_t)n >= buf_len) {
        return -1;
    }
    return n;
}

/* Parse a request. Returns 0 on success, -1 on parse error. */
static int rpc_parse_request(const char *pkt,
                             int *msg_id, char *op, size_t op_len,
                             size_t *payload_len,
                             char *payload, size_t payload_cap)
{
    int plen = 0;
    size_t hdr_consumed;
    const char *p;
    char tmp_op[64];
    int tmp_id;
    int tmp_plen;

    /* Expect: RPC:<msg_id>:<op>:<payload_len>:<payload> */
    if (strncmp(pkt, "RPC:", 4) != 0) {
        size_t pkt_len = strlen(pkt);
        if (pkt_len < 6 || strncmp(pkt + 2, "RPC:", 4) != 0) {
            return -1;
        }
        pkt += 2;
    }

    p = pkt + 4;

    /* parse msg_id */
    {
        char *end;
        long val;

        errno = 0;
        val = strtol(p, &end, 10);
        if (errno != 0 || end == p || *end != ':') {
            return -1;
        }
        tmp_id = (int)val;
        p = end + 1;
    }

    /* parse op */
    {
        size_t i = 0;
        while (*p != '\0' && *p != ':' && i < sizeof(tmp_op) - 1) {
            tmp_op[i++] = *p++;
        }
        tmp_op[i] = '\0';
        if (*p != ':') {
            return -1;
        }
        p++; /* skip ':' */
    }

    /* parse payload_len */
    {
        char *end;
        long val;

        errno = 0;
        val = strtol(p, &end, 10);
        if (errno != 0 || end == p || *end != ':') {
            return -1;
        }
        tmp_plen = (int)val;
        p = end + 1;
    }
    plen = tmp_plen;
    hdr_consumed = (size_t)(p - pkt);

    if (op_len > 0) {
        snprintf(op, op_len, "%s", tmp_op);
    }
    if (msg_id) {
        *msg_id = tmp_id;
    }
    if (payload_len) {
        *payload_len = (size_t)plen;
    }
    if (payload && payload_cap > 0) {
        size_t remaining = strlen(pkt) - hdr_consumed;
        size_t copy = (size_t)plen;
        if (copy > remaining) {
            copy = remaining;
        }
        if (copy >= payload_cap) {
            copy = payload_cap - 1;
        }
        memcpy(payload, p, copy);
        payload[copy] = '\0';
    }

    return 0;
}

/* Parse a response. Returns 0 on success, -1 on parse error. */
static int rpc_parse_response(const char *pkt,
                              int *msg_id, char *status, size_t status_len,
                              size_t *payload_len,
                              char *payload, size_t payload_cap)
{
    const char *p;
    char tmp_status[64];
    int tmp_id;
    int tmp_plen;

    /* Expect: RPC_RSP:<msg_id>:<status>:<payload_len>:<payload> */
    if (strncmp(pkt, "RPC_RSP:", 8) != 0) {
        size_t pkt_len = strlen(pkt);
        if (pkt_len < 10 || strncmp(pkt + 2, "RPC_RSP:", 8) != 0) {
            return -1;
        }
        pkt += 2;
    }

    p = pkt + 8;

    /* parse msg_id */
    {
        char *end;
        long val;

        errno = 0;
        val = strtol(p, &end, 10);
        if (errno != 0 || end == p || *end != ':') {
            return -1;
        }
        tmp_id = (int)val;
        p = end + 1;
    }

    /* parse status */
    {
        size_t i = 0;
        while (*p != '\0' && *p != ':' && i < sizeof(tmp_status) - 1) {
            tmp_status[i++] = *p++;
        }
        tmp_status[i] = '\0';
        if (*p != ':') {
            return -1;
        }
        p++; /* skip ':' */
    }

    /* parse payload_len */
    {
        char *end;
        long val;

        errno = 0;
        val = strtol(p, &end, 10);
        if (errno != 0 || end == p || *end != ':') {
            return -1;
        }
        tmp_plen = (int)val;
        p = end + 1;
    }

    if (status && status_len > 0) {
        snprintf(status, status_len, "%s", tmp_status);
    }
    if (msg_id) {
        *msg_id = tmp_id;
    }
    if (payload_len) {
        *payload_len = (size_t)tmp_plen;
    }
    if (payload && payload_cap > 0) {
        size_t remaining = strlen(p);
        size_t copy = (size_t)tmp_plen;
        if (copy > remaining) {
            copy = remaining;
        }
        if (copy >= payload_cap) {
            copy = payload_cap - 1;
        }
        memcpy(payload, p, copy);
        payload[copy] = '\0';
    }

    return 0;
}

/* ---------- compute helper --------------------------------------------- */

static bool compute_eval(const char *expr, long *result)
{
    long a = 0, b = 0;
    char op = '\0';
    const char *p;
    char *end;

    /* skip leading whitespace */
    p = expr;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    errno = 0;
    a = strtol(p, &end, 10);
    if (errno != 0 || end == p) {
        return false;
    }
    p = end;

    /* find operator */
    while (*p == ' ') {
        p++;
    }
    if (*p != '+' && *p != '-' && *p != '*' && *p != '/') {
        return false;
    }
    op = *p;
    p++;

    while (*p == ' ') {
        p++;
    }

    errno = 0;
    b = strtol(p, &end, 10);
    if (errno != 0 || end == p) {
        return false;
    }

    switch (op) {
    case '+':
        *result = a + b;
        break;
    case '-':
        *result = a - b;
        break;
    case '*':
        *result = a * b;
        break;
    case '/':
        if (b == 0) {
            return false;
        }
        *result = a / b;
        break;
    default:
        return false;
    }

    return true;
}

static uint32_t crc32_ieee(const unsigned char *data, size_t len)
{
    uint32_t crc = 0xffffffffU;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned int bit;

        crc ^= (uint32_t)data[i];
        for (bit = 0; bit < 8; bit++) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1) ^ 0xedb88320U;
            } else {
                crc >>= 1;
            }
        }
    }

    return crc ^ 0xffffffffU;
}

/* ---------- meminfo helper --------------------------------------------- */

static bool read_meminfo(char *out, size_t out_len)
{
    char buf[1024];
    char *saveptr = NULL;
    char *line;
    long memtotal = -1;
    long memfree = -1;

    if (!read_file("/proc/meminfo", buf, sizeof(buf))) {
        return false;
    }

    line = strtok_r(buf, "\n", &saveptr);
    while (line != NULL) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            memtotal = atol(line + 9);
        } else if (strncmp(line, "MemFree:", 8) == 0) {
            memfree = atol(line + 8);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (memtotal < 0 || memfree < 0) {
        return false;
    }

    snprintf(out, out_len, "MemTotal:%ld MemFree:%ld", memtotal, memfree);
    return true;
}

/* ---------- startup synchronization ------------------------------------ */

static bool do_sync_server(int sync_sock)
{
    /* nodeB: listen for RPC_SYNC_REQ, reply RPC_SYNC_ACK */
    long deadline = now_ms() + (long)SYNC_TIMEOUT_S * 1000L;
    char buf[256];

    printf("[ub_rpc] sync server waiting for RPC_SYNC_REQ on port %d\n", SYNC_PORT);

    while (!g_alarm_fired && now_ms() < deadline) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n;

        memset(&src, 0, sizeof(src));
        n = recvfrom(sync_sock, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);
                continue;
            }
            fprintf(stderr, "[ub_rpc] sync recvfrom failed: %s\n", strerror(errno));
            return false;
        }
        buf[n] = '\0';

        if (strcmp(buf, "RPC_SYNC_REQ") == 0 ||
            (n >= 14 && strncmp(buf + 2, "RPC_SYNC_REQ", 12) == 0)) {
            const char *rsp = "RPC_SYNC_ACK";
            sendto(sync_sock, rsp, strlen(rsp), 0,
                   (struct sockaddr *)&src, slen);
            printf("[ub_rpc] sync server received RPC_SYNC_REQ, sent RPC_SYNC_ACK\n");
            return true;
        }
    }

    fprintf(stderr, "[ub_rpc] fail: sync server timeout\n");
    return false;
}

static bool do_sync_client(int sync_sock,
                           const struct sockaddr_in *peer_addr)
{
    /* nodeA: send RPC_SYNC_REQ, wait for RPC_SYNC_ACK */
    long deadline = now_ms() + (long)SYNC_TIMEOUT_S * 1000L;
    char buf[256];
    const char *req = "RPC_SYNC_REQ";

    printf("[ub_rpc] sync client sending RPC_SYNC_REQ to port %d\n", SYNC_PORT);

    while (!g_alarm_fired && now_ms() < deadline) {
        ssize_t sn;
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n;

        sn = sendto(sync_sock, req, strlen(req), 0,
                    (const struct sockaddr *)peer_addr, sizeof(*peer_addr));
        if (sn < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[ub_rpc] sync sendto failed: %s\n", strerror(errno));
            return false;
        }

        memset(&src, 0, sizeof(src));
        n = recvfrom(sync_sock, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&src, &slen);
        if (n > 0) {
            buf[n] = '\0';
            if (strcmp(buf, "RPC_SYNC_ACK") == 0 ||
                (n >= 14 && strncmp(buf + 2, "RPC_SYNC_ACK", 12) == 0)) {
                printf("[ub_rpc] sync client received RPC_SYNC_ACK\n");
                return true;
            }
        }

        usleep(200000);
    }

    fprintf(stderr, "[ub_rpc] fail: sync client timeout\n");
    return false;
}

/* ---------- nonblocking recv with retry -------------------------------- */

static ssize_t recv_with_retry(int sockfd, char *buf, size_t buf_len,
                               long timeout_ms)
{
    long deadline = now_ms() + timeout_ms;

    while (!g_alarm_fired && now_ms() < deadline) {
        ssize_t n;

        n = recv(sockfd, buf, buf_len - 1, MSG_DONTWAIT);
        if (n > 0) {
            buf[n] = '\0';
            return n;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
        usleep(50000);
    }

    return 0; /* timeout */
}

/* ---------- RPC server (nodeB) ----------------------------------------- */

static int run_rpc_server(int rpc_sock,
                          const struct sockaddr_in *peer_addr)
{
    long start_ms = now_ms();
    unsigned int rpc_count = 0;
    bool shutdown_requested = false;

    printf("[ub_rpc] server started, waiting for requests\n");

    while (!g_alarm_fired && !shutdown_requested) {
        char buf[1024];
        ssize_t n;
        int req_msg_id;
        char req_op[64];
        size_t req_plen;
        char req_payload[512];
        char rsp_buf[1024];
        char rsp_payload[512];
        int rsp_len;

        n = recv_with_retry(rpc_sock, buf, sizeof(buf), 2000);
        if (n < 0) {
            fprintf(stderr, "[ub_rpc] server recv error: %s\n", strerror(errno));
            return 1;
        }
        if (n == 0) {
            /* timeout, loop again to check alarm */
            continue;
        }

        if (rpc_parse_request(buf, &req_msg_id, req_op, sizeof(req_op),
                              &req_plen, req_payload, sizeof(req_payload)) != 0) {
            fprintf(stderr, "[ub_rpc] server: failed to parse request: %s\n", buf);
            continue;
        }

        rsp_payload[0] = '\0';

        if (strcmp(req_op, "ECHO") == 0) {
            snprintf(rsp_payload, sizeof(rsp_payload), "%s", req_payload);
        } else if (strcmp(req_op, "CRC32") == 0) {
            uint32_t crc = crc32_ieee((const unsigned char *)req_payload,
                                      strlen(req_payload));

            snprintf(rsp_payload, sizeof(rsp_payload), "0x%08" PRIx32, crc);
        } else if (strcmp(req_op, "COMPUTE") == 0) {
            long result;
            if (compute_eval(req_payload, &result)) {
                snprintf(rsp_payload, sizeof(rsp_payload), "%ld", result);
            } else {
                snprintf(rsp_payload, sizeof(rsp_payload), "ERR");
            }
        } else if (strcmp(req_op, "STATUS") == 0) {
            long elapsed_s = (now_ms() - start_ms) / 1000L;
            snprintf(rsp_payload, sizeof(rsp_payload),
                     "rpcs:%u uptime:%lds", rpc_count, elapsed_s);
        } else if (strcmp(req_op, "MEMINFO") == 0) {
            if (!read_meminfo(rsp_payload, sizeof(rsp_payload))) {
                snprintf(rsp_payload, sizeof(rsp_payload), "ERR");
            }
        } else if (strcmp(req_op, "SHUTDOWN") == 0) {
            shutdown_requested = true;
            snprintf(rsp_payload, sizeof(rsp_payload), "OK");
        } else {
            snprintf(rsp_payload, sizeof(rsp_payload), "UNKNOWN_OP");
        }

        rsp_len = rpc_build_response(rsp_buf, sizeof(rsp_buf),
                                     req_msg_id, "OK", rsp_payload);
        if (rsp_len > 0) {
            sendto(rpc_sock, rsp_buf, (size_t)rsp_len, 0,
                   (const struct sockaddr *)peer_addr, sizeof(*peer_addr));
        }

        rpc_count++;
        printf("[RPC] server handled op=%s msg_id=%d\n", req_op, req_msg_id);
    }

    if (g_alarm_fired && !shutdown_requested) {
        fprintf(stderr, "[ub_rpc] fail: server alarm timeout\n");
        return 1;
    }

    printf("[ub_rpc] server exiting, handled %u rpcs\n", rpc_count);
    return 0;
}

/* ---------- RPC client (nodeA) ----------------------------------------- */

static int rpc_client_send_recv(int rpc_sock,
                                const struct sockaddr_in *peer_addr,
                                int msg_id, const char *op,
                                const char *payload,
                                char *rsp_payload, size_t rsp_payload_cap)
{
    char req_buf[1024];
    char rx_buf[1024];
    int req_len;
    long deadline;
    int retries = 0;
    const int max_retries = 10;

    req_len = rpc_build_request(req_buf, sizeof(req_buf), msg_id, op, payload);
    if (req_len < 0) {
        return -1;
    }

    deadline = now_ms() + 5000L;

    while (!g_alarm_fired && now_ms() < deadline && retries < max_retries) {
        ssize_t sn;
        ssize_t rn;

        sn = sendto(rpc_sock, req_buf, (size_t)req_len, 0,
                    (const struct sockaddr *)peer_addr, sizeof(*peer_addr));
        if (sn < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[ub_rpc] client sendto failed: %s\n", strerror(errno));
            return -1;
        }

        rn = recv_with_retry(rpc_sock, rx_buf, sizeof(rx_buf), 1000);
        if (rn > 0) {
            int rsp_msg_id;
            char rsp_status[64];
            size_t rsp_plen;

            if (rpc_parse_response(rx_buf, &rsp_msg_id,
                                   rsp_status, sizeof(rsp_status),
                                   &rsp_plen,
                                   rsp_payload, rsp_payload_cap) == 0) {
                if (rsp_msg_id == msg_id) {
                    if (strcmp(rsp_status, "OK") != 0) {
                        return -1;
                    }
                    return 0;
                }
            }
            /* wrong msg_id or parse failure, retry recv */
        }

        retries++;
    }

    return -1; /* timeout or max retries */
}

static int run_rpc_client(int rpc_sock,
                          const struct sockaddr_in *peer_addr)
{
    int fail_count = 0;
    char rsp_payload[512];
    char expected_crc[32];
    uint32_t expected_crc_value;

    printf("[ub_rpc] client started\n");

    /* msg_id=1: ECHO(RPC_ECHO_TEXT) */
    {
        const char *expected = RPC_ECHO_TEXT;

        if (rpc_client_send_recv(rpc_sock, peer_addr, 1, "ECHO",
                                 expected, rsp_payload, sizeof(rsp_payload)) != 0) {
            fprintf(stderr, "[RPC] client op=ECHO msg_id=1 status=FAIL result=\"timeout\"\n");
            fail_count++;
        } else if (strcmp(rsp_payload, expected) != 0) {
            fprintf(stderr, "[RPC] client op=ECHO msg_id=1 status=FAIL result=\"%s\" expected=\"%s\"\n",
                    rsp_payload, expected);
            fail_count++;
        } else {
            printf("[RPC] client op=ECHO msg_id=1 status=OK result=\"%s\" expected=\"%s\" verified=1\n",
                   rsp_payload, expected);
        }
    }

    /* msg_id=2: CRC32(RPC_CRC_PAYLOAD) */
    {
        expected_crc_value = crc32_ieee((const unsigned char *)RPC_CRC_PAYLOAD,
                                        strlen(RPC_CRC_PAYLOAD));
        snprintf(expected_crc, sizeof(expected_crc), "0x%08" PRIx32, expected_crc_value);

        if (rpc_client_send_recv(rpc_sock, peer_addr, 2, "CRC32",
                                 RPC_CRC_PAYLOAD, rsp_payload, sizeof(rsp_payload)) != 0) {
            fprintf(stderr, "[RPC] client op=CRC32 msg_id=2 status=FAIL result=\"timeout\"\n");
            fail_count++;
        } else if (strcmp(rsp_payload, expected_crc) != 0) {
            fprintf(stderr, "[RPC] client op=CRC32 msg_id=2 status=FAIL result=\"%s\" expected=\"%s\"\n",
                    rsp_payload, expected_crc);
            fail_count++;
        } else {
            printf("[RPC] client op=CRC32 msg_id=2 status=OK payload=\"%s\" result=\"%s\" expected=\"%s\" verified=1\n",
                   RPC_CRC_PAYLOAD, rsp_payload, expected_crc);
        }
    }

    /* msg_id=3: SHUTDOWN("") */
    {
        if (rpc_client_send_recv(rpc_sock, peer_addr, 3, "SHUTDOWN",
                                 "", rsp_payload, sizeof(rsp_payload)) != 0) {
            fprintf(stderr, "[RPC] client op=SHUTDOWN msg_id=3 status=FAIL result=\"timeout\"\n");
            fail_count++;
        } else if (strcmp(rsp_payload, "OK") != 0) {
            fprintf(stderr, "[RPC] client op=SHUTDOWN msg_id=3 status=FAIL result=\"%s\" expected=\"OK\"\n",
                    rsp_payload);
            fail_count++;
        } else {
            printf("[RPC] client op=SHUTDOWN msg_id=3 status=OK result=\"%s\"\n", rsp_payload);
        }
    }

    /* summary */
    printf("[ub_rpc] client completed %d ops, %d failures\n",
           3, fail_count);

    return (fail_count > 0) ? 1 : 0;
}

/* ---------- main ------------------------------------------------------- */

int main(void)
{
    char role[32] = "unknown";
    char ifname[IFNAMSIZ] = {0};
    struct in_addr local_addr = {0};
    struct in_addr desired_local = {0};
    struct in_addr peer_addr = {0};
    unsigned int ifindex = 0;
    char my_ip[INET_ADDRSTRLEN] = {0};
    char peer_ip[INET_ADDRSTRLEN] = {0};
    int rpc_sock = -1;
    int sync_sock = -1;
    struct sockaddr_in peer_sin;
    struct sigaction sa;
    int ret;

    if (!cmdline_get_value("linqu_urma_dp_role", role, sizeof(role))) {
        fprintf(stderr, "[ub_rpc] fail: no linqu_urma_dp_role in cmdline\n");
        return 1;
    }

    printf("[ub_rpc] start role=%s\n", role);

    /* setup alarm */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(SERVER_TIMEOUT_S + 30);

    /* wait for interface */
    if (!wait_iface_ready(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, "[ub_rpc] fail: ipourma iface not ready\n");
        return 1;
    }

    if (!resolve_ipv4_pair(role, my_ip, sizeof(my_ip), peer_ip, sizeof(peer_ip))) {
        fprintf(stderr, "[ub_rpc] fail: missing ip config for role '%s'\n", role);
        return 1;
    }

    inet_pton(AF_INET, my_ip, &desired_local);
    if (!get_local_ipv4(ifname, &local_addr) || local_addr.s_addr != desired_local.s_addr) {
        fprintf(stderr, "[ub_rpc] warn: bootstrap ipv4 missing or mismatched on %s, applying %s\n",
                ifname, my_ip);
        if (!set_ipv4_addr(ifname, my_ip)) {
            fprintf(stderr, "[ub_rpc] fail: set ipv4 %s on %s failed\n", my_ip, ifname);
            return 1;
        }
    }

    if (!get_local_ipv4(ifname, &local_addr)) {
        fprintf(stderr, "[ub_rpc] fail: get ipv4 addr on %s failed\n", ifname);
        return 1;
    }

    if (inet_pton(AF_INET, peer_ip, &peer_addr) != 1) {
        fprintf(stderr, "[ub_rpc] fail: peer ip parse failed for %s\n", peer_ip);
        return 1;
    }
    install_static_arp(ifname, &peer_addr);

    memset(&peer_sin, 0, sizeof(peer_sin));
    peer_sin.sin_family = AF_INET;
    peer_sin.sin_port = htons(RPC_PORT);
    peer_sin.sin_addr = peer_addr;

    {
        char buf_local[INET_ADDRSTRLEN];
        char buf_peer[INET_ADDRSTRLEN];
        printf("[ub_rpc] iface=%s ifindex=%u local=%s peer=%s\n",
               ifname, ifindex,
               inet_ntop(AF_INET, &local_addr, buf_local, sizeof(buf_local)),
               inet_ntop(AF_INET, &peer_addr, buf_peer, sizeof(buf_peer)));
    }

    /* create sync socket */
    sync_sock = create_udp_socket(ifname, SYNC_PORT);
    if (sync_sock < 0) {
        return 1;
    }

    /* create rpc socket */
    rpc_sock = create_udp_socket(ifname, RPC_PORT);
    if (rpc_sock < 0) {
        close(sync_sock);
        return 1;
    }

    /* startup synchronization */
    if (strcmp(role, "nodeB") == 0) {
        if (!do_sync_server(sync_sock)) {
            close(rpc_sock);
            close(sync_sock);
            return 1;
        }
        ret = run_rpc_server(rpc_sock, &peer_sin);
    } else {
        struct sockaddr_in sync_peer;

        memset(&sync_peer, 0, sizeof(sync_peer));
        sync_peer.sin_family = AF_INET;
        sync_peer.sin_port = htons(SYNC_PORT);
        sync_peer.sin_addr = peer_addr;

        if (!do_sync_client(sync_sock, &sync_peer)) {
            close(rpc_sock);
            close(sync_sock);
            return 1;
        }
        ret = run_rpc_client(rpc_sock, &peer_sin);
    }

    close(rpc_sock);
    close(sync_sock);

    if (ret == 0) {
        printf("[ub_rpc] pass\n");
    } else {
        printf("[ub_rpc] fail\n");
    }

    return ret;
}
