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
#define WAIT_IFACE_MS  90000
#define SERVER_TIMEOUT_S 60
#define RPC_ECHO_TEXT_FMT "greeting from rpc client %s"
#define RPC_CRC_PAYLOAD_FMT "rpc crc payload from %s to %s over ub_link"

enum rpc_mode {
    RPC_MODE_UNKNOWN = 0,
    RPC_MODE_CLIENT,
    RPC_MODE_SERVER,
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

static bool get_env_value(const char *key, char *out, size_t out_len)
{
    const char *env = getenv(key);

    if (env != NULL && env[0] != '\0') {
        snprintf(out, out_len, "%s", env);
        return true;
    }
    return false;
}

static enum rpc_mode parse_rpc_mode(const char *value)
{
    if (strcmp(value, "client") == 0 || strcmp(value, "initiator") == 0 ||
        strcmp(value, "nodeA") == 0) {
        return RPC_MODE_CLIENT;
    }
    if (strcmp(value, "server") == 0 || strcmp(value, "responder") == 0 ||
        strcmp(value, "nodeB") == 0) {
        return RPC_MODE_SERVER;
    }
    return RPC_MODE_UNKNOWN;
}

static const char *rpc_mode_name(enum rpc_mode mode)
{
    switch (mode) {
    case RPC_MODE_CLIENT:
        return "client";
    case RPC_MODE_SERVER:
        return "server";
    default:
        return "unknown";
    }
}

static bool get_config_value(const char *env_key, const char *cmd_key,
                             char *out, size_t out_len)
{
    return get_env_value(env_key, out, out_len) ||
           cmdline_get_value(cmd_key, out, out_len);
}

static bool legacy_role_ipv4_defaults(const char *role,
                                      char *local, size_t local_len,
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

static bool resolve_local_ipv4(const char *legacy_role, char *local, size_t local_len)
{
    bool have_local = get_config_value("LINQU_UB_LOCAL_IP", "linqu_ipourma_ipv4",
                                       local, local_len);

    if (!have_local) {
        char default_local[INET_ADDRSTRLEN];

        if (!legacy_role_ipv4_defaults(legacy_role, default_local, sizeof(default_local),
                                       (char[INET_ADDRSTRLEN]){0}, INET_ADDRSTRLEN)) {
            return false;
        }
        snprintf(local, local_len, "%s", default_local);
        have_local = true;
    }

    if (!have_local) {
        return false;
    }

    if (inet_pton(AF_INET, local, &(struct in_addr){0}) != 1) {
        return false;
    }

    return true;
}

static bool resolve_peer_ipv4(const char *legacy_role, char *peer, size_t peer_len)
{
    bool have_peer = get_config_value("LINQU_UB_PEER_IP", "linqu_ipourma_peer_ipv4",
                                      peer, peer_len);

    if (!have_peer) {
        char default_peer[INET_ADDRSTRLEN];

        if (!legacy_role_ipv4_defaults(legacy_role,
                                       (char[INET_ADDRSTRLEN]){0}, INET_ADDRSTRLEN,
                                       default_peer, sizeof(default_peer))) {
            return false;
        }
        snprintf(peer, peer_len, "%s", default_peer);
        have_peer = true;
    }

    if (!have_peer || inet_pton(AF_INET, peer, &(struct in_addr){0}) != 1) {
        return false;
    }

    return true;
}

static bool get_rpc_mode(enum rpc_mode *mode_out, char *legacy_role, size_t legacy_role_len)
{
    char value[32];
    enum rpc_mode mode;

    if (get_config_value("LINQU_RPC_MODE", "linqu_rpc_mode", value, sizeof(value))) {
        mode = parse_rpc_mode(value);
        if (mode != RPC_MODE_UNKNOWN) {
            snprintf(legacy_role, legacy_role_len, "%s", value);
            *mode_out = mode;
            return true;
        }
    }

    if (!cmdline_get_value("linqu_urma_dp_role", legacy_role, legacy_role_len)) {
        return false;
    }
    mode = parse_rpc_mode(legacy_role);
    if (mode == RPC_MODE_UNKNOWN) {
        return false;
    }
    *mode_out = mode;
    return true;
}

static unsigned int get_expected_call_count(enum rpc_mode mode)
{
    char value[32];
    char *end;
    unsigned long parsed;
    const char *env = getenv("LINQU_RPC_EXPECT_CALLS");

    if ((env != NULL && env[0] != '\0' && snprintf(value, sizeof(value), "%s", env) > 0) ||
        cmdline_get_value("linqu_rpc_expected_calls", value, sizeof(value))) {
        errno = 0;
        parsed = strtoul(value, &end, 10);
        if (errno == 0 && end != value && *end == '\0' && parsed <= UINT_MAX) {
            return (unsigned int)parsed;
        }
    }

    if (mode == RPC_MODE_SERVER) {
        return 2;
    }
    return 0;
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

/* ---------- RPC server ------------------------------------------------- */

static int run_rpc_server(int rpc_sock, const char *local_ip,
                          unsigned int expected_calls)
{
    long start_ms = now_ms();
    unsigned int rpc_count = 0;

    printf("[ub_rpc] mode=server local=%s peer=<dynamic> expected_calls=%u started\n",
           local_ip, expected_calls);

    while (!g_alarm_fired) {
        char buf[1024];
        ssize_t n;
        int req_msg_id;
        char req_op[64];
        size_t req_plen;
        char req_payload[512];
        char rsp_buf[1024];
        char rsp_payload[512];
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        char peer_ip[INET_ADDRSTRLEN] = {0};
        int rsp_len;

        n = recvfrom(rpc_sock, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                     (struct sockaddr *)&src, &src_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);
                continue;
            }
            fprintf(stderr, "[ub_rpc] server recv error: %s\n", strerror(errno));
            return 1;
        }
        buf[n] = '\0';
        inet_ntop(AF_INET, &src.sin_addr, peer_ip, sizeof(peer_ip));

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
        } else {
            snprintf(rsp_payload, sizeof(rsp_payload), "UNKNOWN_OP");
        }

        rsp_len = rpc_build_response(rsp_buf, sizeof(rsp_buf),
                                     req_msg_id, "OK", rsp_payload);
        if (rsp_len > 0) {
            sendto(rpc_sock, rsp_buf, (size_t)rsp_len, 0,
                   (const struct sockaddr *)&src, src_len);
        }

        rpc_count++;
        printf("[RPC] server local=%s peer=%s handled op=%s msg_id=%d rpc_count=%u\n",
               local_ip, peer_ip, req_op, req_msg_id, rpc_count);
        if (expected_calls > 0 && rpc_count >= expected_calls) {
            break;
        }
    }

    if (g_alarm_fired) {
        fprintf(stderr, "[ub_rpc] fail: mode=server local=%s alarm timeout after %u calls\n",
                local_ip, rpc_count);
        return 1;
    }

    printf("[ub_rpc] mode=server local=%s exiting handled=%u\n",
           local_ip, rpc_count);
    return 0;
}

/* ---------- RPC client ------------------------------------------------- */

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

static int run_rpc_client(int rpc_sock, const struct sockaddr_in *peer_addr,
                          const char *local_ip, const char *peer_ip)
{
    int fail_count = 0;
    char rsp_payload[512];
    char echo_text[128];
    char crc_payload[192];
    char expected_crc[32];
    uint32_t expected_crc_value;

    snprintf(echo_text, sizeof(echo_text), RPC_ECHO_TEXT_FMT, local_ip);
    snprintf(crc_payload, sizeof(crc_payload), RPC_CRC_PAYLOAD_FMT, local_ip, peer_ip);

    printf("[ub_rpc] mode=client local=%s peer=%s started\n", local_ip, peer_ip);

    {
        const char *expected = echo_text;

        if (rpc_client_send_recv(rpc_sock, peer_addr, 1, "ECHO",
                                 expected, rsp_payload, sizeof(rsp_payload)) != 0) {
            fprintf(stderr, "[RPC] client local=%s peer=%s op=ECHO msg_id=1 status=FAIL result=\"timeout\"\n",
                    local_ip, peer_ip);
            fail_count++;
        } else if (strcmp(rsp_payload, expected) != 0) {
            fprintf(stderr,
                    "[RPC] client local=%s peer=%s op=ECHO msg_id=1 status=FAIL result=\"%s\" expected=\"%s\"\n",
                    local_ip, peer_ip, rsp_payload, expected);
            fail_count++;
        } else {
            printf("[RPC] client local=%s peer=%s op=ECHO msg_id=1 status=OK result=\"%s\" expected=\"%s\" verified=1\n",
                   local_ip, peer_ip, rsp_payload, expected);
        }
    }

    {
        expected_crc_value = crc32_ieee((const unsigned char *)crc_payload,
                                        strlen(crc_payload));
        snprintf(expected_crc, sizeof(expected_crc), "0x%08" PRIx32, expected_crc_value);

        if (rpc_client_send_recv(rpc_sock, peer_addr, 2, "CRC32",
                                 crc_payload, rsp_payload, sizeof(rsp_payload)) != 0) {
            fprintf(stderr, "[RPC] client local=%s peer=%s op=CRC32 msg_id=2 status=FAIL result=\"timeout\"\n",
                    local_ip, peer_ip);
            fail_count++;
        } else if (strcmp(rsp_payload, expected_crc) != 0) {
            fprintf(stderr,
                    "[RPC] client local=%s peer=%s op=CRC32 msg_id=2 status=FAIL result=\"%s\" expected=\"%s\"\n",
                    local_ip, peer_ip, rsp_payload, expected_crc);
            fail_count++;
        } else {
            printf("[RPC] client local=%s peer=%s op=CRC32 msg_id=2 status=OK payload=\"%s\" result=\"%s\" expected=\"%s\" verified=1\n",
                   local_ip, peer_ip, crc_payload, rsp_payload, expected_crc);
        }
    }

    printf("[ub_rpc] mode=client local=%s peer=%s completed_ops=2 failures=%d\n",
           local_ip, peer_ip, fail_count);

    return (fail_count > 0) ? 1 : 0;
}

/* ---------- main ------------------------------------------------------- */

int main(void)
{
    char role_hint[32] = "unknown";
    enum rpc_mode mode = RPC_MODE_UNKNOWN;
    char ifname[IFNAMSIZ] = {0};
    struct in_addr local_addr = {0};
    struct in_addr desired_local = {0};
    struct in_addr peer_addr = {0};
    unsigned int ifindex = 0;
    char my_ip[INET_ADDRSTRLEN] = {0};
    char peer_ip[INET_ADDRSTRLEN] = {0};
    int rpc_sock = -1;
    struct sockaddr_in peer_sin;
    struct sigaction sa;
    unsigned int expected_calls;
    int ret;

    if (!get_rpc_mode(&mode, role_hint, sizeof(role_hint))) {
        fprintf(stderr, "[ub_rpc] fail: unable to resolve rpc mode from LINQU_RPC_MODE/linqu_rpc_mode/linqu_urma_dp_role\n");
        return 1;
    }
    expected_calls = get_expected_call_count(mode);

    printf("[ub_rpc] start mode=%s role_hint=%s\n", rpc_mode_name(mode), role_hint);

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

    if (!resolve_local_ipv4(role_hint, my_ip, sizeof(my_ip))) {
        fprintf(stderr, "[ub_rpc] fail: missing local ip config for mode=%s role_hint=%s\n",
                rpc_mode_name(mode), role_hint);
        return 1;
    }
    if (mode == RPC_MODE_CLIENT &&
        !resolve_peer_ipv4(role_hint, peer_ip, sizeof(peer_ip))) {
        fprintf(stderr, "[ub_rpc] fail: missing peer ip config for mode=%s role_hint=%s\n",
                rpc_mode_name(mode), role_hint);
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

    if (mode == RPC_MODE_CLIENT) {
        if (inet_pton(AF_INET, peer_ip, &peer_addr) != 1) {
            fprintf(stderr, "[ub_rpc] fail: peer ip parse failed for %s\n", peer_ip);
            return 1;
        }
        install_static_arp(ifname, &peer_addr);
    }

    memset(&peer_sin, 0, sizeof(peer_sin));
    if (mode == RPC_MODE_CLIENT) {
        peer_sin.sin_family = AF_INET;
        peer_sin.sin_port = htons(RPC_PORT);
        peer_sin.sin_addr = peer_addr;
    }

    {
        char buf_local[INET_ADDRSTRLEN];
        printf("[ub_rpc] iface=%s ifindex=%u local=%s peer=%s\n",
               ifname, ifindex,
               inet_ntop(AF_INET, &local_addr, buf_local, sizeof(buf_local)),
               (mode == RPC_MODE_CLIENT) ? peer_ip : "<dynamic>");
    }

    /* create rpc socket */
    rpc_sock = create_udp_socket(ifname, (mode == RPC_MODE_SERVER) ? RPC_PORT : 0);
    if (rpc_sock < 0) {
        return 1;
    }

    if (mode == RPC_MODE_SERVER) {
        ret = run_rpc_server(rpc_sock, my_ip, expected_calls);
    } else {
        ret = run_rpc_client(rpc_sock, &peer_sin, my_ip, peer_ip);
    }

    close(rpc_sock);

    if (ret == 0) {
        printf("[ub_rpc] pass\n");
    } else {
        printf("[ub_rpc] fail\n");
    }

    return ret;
}
