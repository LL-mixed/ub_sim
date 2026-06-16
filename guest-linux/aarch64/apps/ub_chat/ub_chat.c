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

#define CHAT_PORT       18556
#define SYNC_PORT       18559
#define CHAT_ROUNDS     5
#define RUN_TIMEOUT_S   30
#define WAIT_IFACE_MS   90000
#define CHAT_REQ_TEXT   "greeting from initiator"
#define CHAT_RSP_TEXT   "copy, greeting back from responder"

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

static bool env_get_value(const char *key, char *out, size_t out_len)
{
    const char *val = getenv(key);

    if (val == NULL || *val == '\0') {
        return false;
    }

    snprintf(out, out_len, "%s", val);
    return true;
}

static unsigned int resolve_timeout_s(void)
{
    char buf[32];
    unsigned long value;
    char *end = NULL;

    if (!env_get_value("LINQU_UB_TIMEOUT_S", buf, sizeof(buf)) &&
        !cmdline_get_value("linqu_ub_chat_timeout_s", buf, sizeof(buf))) {
        return RUN_TIMEOUT_S;
    }

    errno = 0;
    value = strtoul(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0') {
        return RUN_TIMEOUT_S;
    }
    if (value < 5) {
        value = 5;
    }
    if (value > 600) {
        value = 600;
    }
    return (unsigned int)value;
}

static unsigned int resolve_post_sync_settle_ms(void)
{
    char buf[32];
    unsigned long value;
    char *end = NULL;

    if (!env_get_value("LINQU_UB_POST_SYNC_SETTLE_MS", buf, sizeof(buf)) &&
        !cmdline_get_value("linqu_ub_chat_post_sync_settle_ms", buf, sizeof(buf))) {
        return 0;
    }

    errno = 0;
    value = strtoul(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0') {
        return 0;
    }
    if (value > 30000) {
        value = 30000;
    }
    return (unsigned int)value;
}

static bool resolve_shared_socket_mode(void)
{
    char buf[16];

    if (!env_get_value("LINQU_UB_CHAT_SHARED_SOCKET", buf, sizeof(buf)) &&
        !cmdline_get_value("linqu_ub_chat_shared_socket", buf, sizeof(buf))) {
        return false;
    }

    return strcmp(buf, "1") == 0 || strcasecmp(buf, "true") == 0 ||
           strcasecmp(buf, "yes") == 0;
}

static bool normalize_role(const char *in, char *out, size_t out_len)
{
    if (strcmp(in, "initiator") == 0 || strcmp(in, "nodeA") == 0) {
        snprintf(out, out_len, "%s", "initiator");
        return true;
    }
    if (strcmp(in, "responder") == 0 || strcmp(in, "nodeB") == 0) {
        snprintf(out, out_len, "%s", "responder");
        return true;
    }
    return false;
}

static bool role_is_initiator(const char *role)
{
    return strcmp(role, "initiator") == 0;
}

static bool role_default_ipv4_pair(const char *role, char *local, size_t local_len,
                                   char *peer, size_t peer_len)
{
    if (role_is_initiator(role)) {
        snprintf(local, local_len, "%s", "10.0.0.1");
        snprintf(peer, peer_len, "%s", "10.0.0.2");
        return true;
    }
    if (strcmp(role, "responder") == 0) {
        snprintf(local, local_len, "%s", "10.0.0.2");
        snprintf(peer, peer_len, "%s", "10.0.0.1");
        return true;
    }
    return false;
}

static bool resolve_ipv4_pair(const char *role, char *local, size_t local_len,
                              char *peer, size_t peer_len)
{
    bool have_local = env_get_value("LINQU_UB_LOCAL_IP", local, local_len) ||
                      cmdline_get_value("linqu_ipourma_ipv4", local, local_len);
    bool have_peer = env_get_value("LINQU_UB_PEER_IP", peer, peer_len) ||
                     cmdline_get_value("linqu_ipourma_peer_ipv4", peer, peer_len);

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
        fprintf(stderr, "[ub_chat] set_ipv4: socket failed: %s\n", strerror(errno));
        return false;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, addr_str, &sin->sin_addr) != 1) {
        fprintf(stderr, "[ub_chat] set_ipv4: inet_pton failed for %s\n", addr_str);
        close(fd);
        return false;
    }

    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) {
        fprintf(stderr, "[ub_chat] set_ipv4: SIOCSIFADDR failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    sin = (struct sockaddr_in *)&ifr.ifr_netmask;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) != 0) {
        fprintf(stderr, "[ub_chat] set_ipv4: SIOCSIFNETMASK failed: %s\n", strerror(errno));
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
        fprintf(stderr, "[ub_chat] warn: socket for SIOCSARP failed: %s\n", strerror(errno));
        return;
    }

    if (ioctl(fd, SIOCSARP, &req) != 0) {
        fprintf(stderr, "[ub_chat] warn: SIOCSARP %s failed: %s\n",
                ifname, strerror(errno));
    } else {
        fprintf(stderr, "[ub_chat] static arp installed for peer on %s\n", ifname);
    }

    close(fd);
}

static int create_chat_socket(const char *ifname)
{
    int sockfd;
    int one = 1;
    struct sockaddr_in bind_addr;
    int flags;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "[ub_chat] fail: socket create failed: %s\n", strerror(errno));
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[ub_chat] fail: SO_REUSEADDR failed: %s\n", strerror(errno));
        close(sockfd);
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_NO_CHECK, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[ub_chat] warn: SO_NO_CHECK failed: %s\n", strerror(errno));
    }

    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    } else {
        fprintf(stderr, "[ub_chat] warn: fcntl F_GETFL failed: %s\n", strerror(errno));
    }

    if (setsockopt(sockfd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[ub_chat] warn: IP_PKTINFO failed: %s\n", strerror(errno));
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) != 0) {
        fprintf(stderr, "[ub_chat] warn: SO_BINDTODEVICE failed: %s\n", strerror(errno));
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(CHAT_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        fprintf(stderr, "[ub_chat] fail: bind port %d failed: %s\n",
                CHAT_PORT, strerror(errno));
        close(sockfd);
        return -1;
    }

    fprintf(stderr, "[ub_chat] chat socket bind ok port=%d\n", CHAT_PORT);
    return sockfd;
}

static int create_sync_socket(const char *ifname)
{
    int sockfd;
    int one = 1;
    struct sockaddr_in bind_addr;
    int flags;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "[ub_chat] fail: sync socket create failed: %s\n", strerror(errno));
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[ub_chat] fail: sync SO_REUSEADDR failed: %s\n", strerror(errno));
        close(sockfd);
        return -1;
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_NO_CHECK, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[ub_chat] warn: sync SO_NO_CHECK failed: %s\n", strerror(errno));
    }

    flags = fcntl(sockfd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) != 0) {
        fprintf(stderr, "[ub_chat] warn: sync SO_BINDTODEVICE failed: %s\n", strerror(errno));
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(SYNC_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
        fprintf(stderr, "[ub_chat] fail: sync bind port %d failed: %s\n",
                SYNC_PORT, strerror(errno));
        close(sockfd);
        return -1;
    }

    fprintf(stderr, "[ub_chat] sync socket bind ok port=%d\n", SYNC_PORT);
    return sockfd;
}

static int do_startup_sync_initiator(int sync_fd,
                                     const struct sockaddr_in *peer_addr,
                                     const char *local_ip,
                                     const char *peer_ip,
                                     unsigned int timeout_s)
{
    long deadline = now_ms() + (long)(timeout_s * 1000L);
    const char *req = "SYNC_REQ";
    struct sockaddr_in sync_peer = *peer_addr;

    sync_peer.sin_port = htons(SYNC_PORT);

    while (!g_alarm_fired && now_ms() < deadline) {
        ssize_t sn;
        char buf[64];
        struct sockaddr_in from;
        socklen_t fromlen;
        ssize_t rn;

        sn = sendto(sync_fd, req, strlen(req), MSG_DONTWAIT,
                    (const struct sockaddr *)&sync_peer, sizeof(sync_peer));
        if (sn < 0) {
            fprintf(stderr, "[ub_chat] sync send ret=%zd errno=%d(%s)\n",
                    sn, errno, strerror(errno));
        } else {
            fprintf(stderr, "[ub_chat] sync send ok ret=%zd\n", sn);
        }

        usleep(50000);

        fromlen = sizeof(from);
        rn = recvfrom(sync_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                      (struct sockaddr *)&from, &fromlen);
        if (rn > 0) {
            buf[rn] = '\0';
            if (strncmp(buf, "SYNC_ACK", 8) == 0 ||
                (rn >= 10 && strncmp(buf + 2, "SYNC_ACK", 8) == 0)) {
                printf("[ub_chat] startup sync complete role=initiator local=%s peer=%s\n",
                       local_ip, peer_ip);
                return 0;
            }
        }

        usleep(200000);
    }

    fprintf(stderr, "[ub_chat] fail: startup sync timeout role=initiator local=%s peer=%s\n",
            local_ip, peer_ip);
    return -1;
}

static int do_startup_sync_responder(int sync_fd,
                                     const char *local_ip,
                                     const char *peer_ip,
                                     unsigned int timeout_s)
{
    long deadline = now_ms() + (long)(timeout_s * 1000L);

    while (!g_alarm_fired && now_ms() < deadline) {
        char buf[64];
        struct sockaddr_in from;
        socklen_t fromlen;
        ssize_t rn;

        fromlen = sizeof(from);
        rn = recvfrom(sync_fd, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                      (struct sockaddr *)&from, &fromlen);
        if (rn > 0) {
            buf[rn] = '\0';
            if (strncmp(buf, "SYNC_REQ", 8) == 0 ||
                (rn >= 10 && strncmp(buf + 2, "SYNC_REQ", 8) == 0)) {
                const char *ack = "SYNC_ACK";
                sendto(sync_fd, ack, strlen(ack), MSG_DONTWAIT,
                       (const struct sockaddr *)&from, sizeof(from));
                printf("[ub_chat] startup sync complete role=responder local=%s peer=%s\n",
                       local_ip, peer_ip);
                return 0;
            }
        }

        usleep(200000);
    }

    fprintf(stderr, "[ub_chat] fail: startup sync timeout role=responder local=%s peer=%s\n",
            local_ip, peer_ip);
    return -1;
}

static ssize_t recv_chat_msg(int sockfd, char *buf, size_t buflen,
                             struct sockaddr_in *from)
{
    struct iovec iov;
    struct msghdr msg;
    char cbuf[256];
    ssize_t n;
    socklen_t fromlen;

    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = buflen - 1;
    msg.msg_name = from;
    msg.msg_namelen = sizeof(*from);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    n = recvmsg(sockfd, &msg, MSG_DONTWAIT);
    if (n < 0) {
        return -1;
    }
    buf[n] = '\0';
    (void)fromlen;
    return n;
}

static char *find_chat_payload_start(char *buf, size_t len)
{
    size_t i;

    if (len >= 5 && strncmp(buf, "CHAT:", 5) == 0) {
        return buf;
    }

    for (i = 0; i + 5 <= len; i++) {
        if (memcmp(buf + i, "CHAT:", 5) == 0) {
            if (i != 0) {
                fprintf(stderr, "[ub_chat] recv raw adjusted by +%zu bytes\n", i);
            }
            return buf + i;
        }
    }

    return NULL;
}

static int wait_for_peer_chat(int sockfd, const char *local_role,
                              char *peer_text, size_t peer_text_len,
                              int *peer_seq, long *peer_ts_ms,
                              unsigned int timeout_s)
{
    long deadline = now_ms() + (long)(timeout_s * 1000L);

    while (!g_alarm_fired && now_ms() < deadline) {
        char buf[512];
        struct sockaddr_in from;
        ssize_t n;
        char peer_role[32];

        n = recv_chat_msg(sockfd, buf, sizeof(buf), &from);
        if (n > 0) {
            char *p;
            char *field;
            int fields = 0;
            char saved[sizeof(buf)];
            char *payload;

            memcpy(saved, buf, (size_t)n + 1);
            fprintf(stderr, "[ub_chat] recv raw len=%zd head=\"%.*s\"\n",
                    n, (int)((n < 64) ? n : 64), saved);

            if ((n >= 8 && strncmp(saved, "SYNC_REQ", 8) == 0) ||
                (n >= 8 && strncmp(saved, "SYNC_ACK", 8) == 0)) {
                fprintf(stderr, "[ub_chat] recv control packet ignored\n");
                continue;
            }

            payload = find_chat_payload_start(saved, (size_t)n);
            if (payload == NULL) {
                fprintf(stderr, "[ub_chat] recv parse skip: missing CHAT tag\n");
                continue;
            }
            p = payload;
            field = strsep(&p, ":");
            if (!field || strcmp(field, "CHAT") != 0) {
                fprintf(stderr, "[ub_chat] recv parse skip: invalid CHAT tag\n");
                continue;
            }
            fields++;

            field = strsep(&p, ":");
            if (!field) {
                fprintf(stderr, "[ub_chat] recv parse skip: missing role\n");
                continue;
            }
            snprintf(peer_role, sizeof(peer_role), "%s", field);
            fields++;

            if (strcmp(peer_role, local_role) == 0) {
                continue;
            }

            field = strsep(&p, ":");
            if (!field) {
                fprintf(stderr, "[ub_chat] recv parse skip: missing text\n");
                continue;
            }
            snprintf(peer_text, peer_text_len, "%s", field);
            fields++;

            field = strsep(&p, ":");
            if (!field) {
                fprintf(stderr, "[ub_chat] recv parse skip: missing seq\n");
                continue;
            }
            *peer_seq = atoi(field);
            fields++;

            field = strsep(&p, ":");
            if (!field) {
                fprintf(stderr, "[ub_chat] recv parse skip: missing timestamp\n");
                continue;
            }
            *peer_ts_ms = atol(field);
            fields++;

            if (fields >= 5) {
                return 0;
            }
        }

        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[ub_chat] recv error: %s\n", strerror(errno));
            return -1;
        }

        usleep(50000);
    }

    fprintf(stderr, "[ub_chat] fail: timeout waiting for peer chat\n");
    return -1;
}

static int run_initiator(int chat_fd, int sync_fd,
                         const struct sockaddr_in *peer_addr,
                         const char *role,
                         const char *local_ip,
                         const char *peer_ip,
                         unsigned int timeout_s)
{
    int i;
    unsigned int tx_count = 0;
    unsigned int rx_count = 0;
    unsigned int settle_ms = resolve_post_sync_settle_ms();
    long total_latency = 0;
    long min_latency = LONG_MAX;
    long max_latency = LONG_MIN;

    if (do_startup_sync_initiator(sync_fd, peer_addr, local_ip, peer_ip, timeout_s) != 0) {
        return 1;
    }
    if (settle_ms > 0) {
        fprintf(stderr, "[ub_chat] post-sync settle %u ms role=initiator local=%s peer=%s\n",
                settle_ms, local_ip, peer_ip);
        usleep((useconds_t)settle_ms * 1000U);
    }

    for (i = 0; i < CHAT_ROUNDS && !g_alarm_fired; i++) {
        char tx[256];
        char peer_text[128];
        int peer_seq;
        long peer_ts;
        long send_ts;
        long recv_ts;
        long latency;
        ssize_t sn;

        snprintf(tx, sizeof(tx), "CHAT:%s:%s:%d:%ld",
                 role, CHAT_REQ_TEXT, i, now_ms());

        send_ts = now_ms();
        fprintf(stderr, "[ub_chat] send seq=%d payload=\"%s\"\n", i, tx);
        sn = sendto(chat_fd, tx, strlen(tx), MSG_DONTWAIT,
                    (const struct sockaddr *)peer_addr, sizeof(*peer_addr));
        if (sn < 0) {
            fprintf(stderr, "[ub_chat] fail: send seq %d: %s\n", i, strerror(errno));
            return 1;
        }
        fprintf(stderr, "[ub_chat] send seq=%d ok len=%zd\n", i, sn);
        tx_count++;

        if (wait_for_peer_chat(chat_fd, role, peer_text, sizeof(peer_text),
                               &peer_seq, &peer_ts, timeout_s) != 0) {
            fprintf(stderr, "[ub_chat] fail: no reply for seq %d\n", i);
            return 1;
        }
        if (peer_seq != i) {
            fprintf(stderr, "[ub_chat] fail: reply seq mismatch got=%d expect=%d\n",
                    peer_seq, i);
            return 1;
        }
        if (strcmp(peer_text, CHAT_RSP_TEXT) != 0) {
            fprintf(stderr, "[ub_chat] fail: reply payload mismatch got=\"%s\" expect=\"%s\"\n",
                    peer_text, CHAT_RSP_TEXT);
            return 1;
        }
        recv_ts = now_ms();
        rx_count++;

        latency = recv_ts - send_ts;
        total_latency += latency;
        if (latency < min_latency) {
            min_latency = latency;
        }
        if (latency > max_latency) {
            max_latency = latency;
        }

        printf("[CHAT] %s seq=%d \"%s\" latency=%ldms\n",
               role, i, peer_text, latency);
    }

    if (tx_count > 0) {
        long avg_latency = total_latency / (long)tx_count;
        printf("[ub_chat] summary tx=%u rx=%u avg_latency=%ldms "
               "min_latency=%ldms max_latency=%ldms\n",
               tx_count, rx_count, avg_latency, min_latency, max_latency);
    }

    if (g_alarm_fired || tx_count < CHAT_ROUNDS || rx_count < CHAT_ROUNDS) {
        printf("[ub_chat] fail\n");
        return 1;
    }

    printf("[ub_chat] pass\n");
    return 0;
}

static int run_responder(int chat_fd, int sync_fd,
                         const struct sockaddr_in *peer_addr,
                         const char *role,
                         const char *local_ip,
                         const char *peer_ip,
                         unsigned int timeout_s)
{
    unsigned int tx_count = 0;
    unsigned int rx_count = 0;
    long total_latency = 0;
    long min_latency = LONG_MAX;
    long max_latency = LONG_MIN;
    int rounds = 0;
    unsigned int settle_ms = resolve_post_sync_settle_ms();

    if (do_startup_sync_responder(sync_fd, local_ip, peer_ip, timeout_s) != 0) {
        return 1;
    }
    if (settle_ms > 0) {
        fprintf(stderr, "[ub_chat] post-sync settle %u ms role=responder local=%s peer=%s\n",
                settle_ms, local_ip, peer_ip);
        usleep((useconds_t)settle_ms * 1000U);
    }

    while (rounds < CHAT_ROUNDS && !g_alarm_fired) {
        char peer_text[128];
        int peer_seq;
        long peer_ts;
        long recv_ts;
        long latency;
        char tx[256];
        ssize_t sn;

        if (wait_for_peer_chat(chat_fd, role, peer_text, sizeof(peer_text),
                               &peer_seq, &peer_ts, timeout_s) != 0) {
            fprintf(stderr, "[ub_chat] fail: responder timeout waiting msg local=%s peer=%s\n",
                    local_ip, peer_ip);
            return 1;
        }
        if (peer_seq != rounds) {
            fprintf(stderr, "[ub_chat] fail: responder req seq mismatch got=%d expect=%d local=%s peer=%s\n",
                    peer_seq, rounds, local_ip, peer_ip);
            return 1;
        }
        if (strcmp(peer_text, CHAT_REQ_TEXT) != 0) {
            fprintf(stderr, "[ub_chat] fail: responder req payload mismatch got=\"%s\" expect=\"%s\" local=%s peer=%s\n",
                    peer_text, CHAT_REQ_TEXT, local_ip, peer_ip);
            return 1;
        }
        recv_ts = now_ms();
        rx_count++;

        latency = recv_ts - peer_ts;
        total_latency += latency;
        if (latency < min_latency) {
            min_latency = latency;
        }
        if (latency > max_latency) {
            max_latency = latency;
        }

        printf("[CHAT] %s seq=%d \"%s\" latency=%ldms\n",
               role, peer_seq, peer_text, latency);

        snprintf(tx, sizeof(tx), "CHAT:%s:%s:%d:%ld",
                 role, CHAT_RSP_TEXT, peer_seq, now_ms());

        fprintf(stderr, "[ub_chat] send reply seq=%d payload=\"%s\"\n", peer_seq, tx);
        sn = sendto(chat_fd, tx, strlen(tx), MSG_DONTWAIT,
                    (const struct sockaddr *)peer_addr, sizeof(*peer_addr));
        if (sn < 0) {
            fprintf(stderr, "[ub_chat] fail: responder send reply seq %d local=%s peer=%s: %s\n",
                    peer_seq, local_ip, peer_ip, strerror(errno));
            return 1;
        }
        fprintf(stderr, "[ub_chat] send reply seq=%d ok len=%zd\n", peer_seq, sn);
        tx_count++;
        rounds++;
    }

    if (tx_count > 0) {
        long avg_latency = total_latency / (long)rx_count;
        printf("[ub_chat] summary tx=%u rx=%u avg_latency=%ldms "
               "min_latency=%ldms max_latency=%ldms\n",
               tx_count, rx_count, avg_latency, min_latency, max_latency);
    }

    if (g_alarm_fired || rounds < CHAT_ROUNDS) {
        printf("[ub_chat] fail\n");
        return 1;
    }

    printf("[ub_chat] pass\n");
    return 0;
}

#ifndef IP_PKTINFO
#define IP_PKTINFO 8
#endif

int main(void)
{
    char role[32] = "unknown";
    char normalized_role[32] = {0};
    char ifname[IFNAMSIZ] = {0};
    struct in_addr local_addr = {0};
    struct in_addr desired_local = {0};
    struct in_addr peer_addr = {0};
    struct sockaddr_in peer_sockaddr;
    unsigned int ifindex = 0;
    char my_ip[INET_ADDRSTRLEN] = {0};
    char peer_ip[INET_ADDRSTRLEN] = {0};
    int chat_fd = -1;
    int sync_fd = -1;
    int result;
    unsigned int timeout_s;
    bool shared_socket;
    bool close_chat_fd = true;
    struct sigaction sa;

    if (!env_get_value("LINQU_UB_ROLE", role, sizeof(role)) &&
        !cmdline_get_value("linqu_urma_dp_role", role, sizeof(role))) {
        fprintf(stderr, "[ub_chat] fail: missing linqu_urma_dp_role in cmdline\n");
        return 1;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (!normalize_role(role, normalized_role, sizeof(normalized_role))) {
        fprintf(stderr, "[ub_chat] fail: invalid role '%s', expected initiator/responder\n", role);
        return 1;
    }
    snprintf(role, sizeof(role), "%s", normalized_role);

    printf("[ub_chat] start role=%s\n", role);
    timeout_s = resolve_timeout_s();
    shared_socket = resolve_shared_socket_mode();

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(timeout_s);

    if (!wait_iface_ready(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, "[ub_chat] fail: ipourma iface not ready\n");
        return 1;
    }

    if (!resolve_ipv4_pair(role, my_ip, sizeof(my_ip), peer_ip, sizeof(peer_ip))) {
        fprintf(stderr, "[ub_chat] fail: missing ip config for role '%s'\n", role);
        return 1;
    }

    inet_pton(AF_INET, my_ip, &desired_local);
    if (!get_local_ipv4(ifname, &local_addr) || local_addr.s_addr != desired_local.s_addr) {
        fprintf(stderr, "[ub_chat] warn: bootstrap ipv4 missing or mismatched on %s, applying %s\n",
                ifname, my_ip);
        if (!set_ipv4_addr(ifname, my_ip)) {
            fprintf(stderr, "[ub_chat] fail: set ipv4 %s on %s failed\n", my_ip, ifname);
            return 1;
        }
    }

    if (!get_local_ipv4(ifname, &local_addr)) {
        fprintf(stderr, "[ub_chat] fail: get ipv4 addr on %s failed\n", ifname);
        return 1;
    }

    if (inet_pton(AF_INET, peer_ip, &peer_addr) != 1) {
        fprintf(stderr, "[ub_chat] fail: peer ip parse failed for %s\n", peer_ip);
        return 1;
    }
    install_static_arp(ifname, &peer_addr);

    {
        char local_buf[INET_ADDRSTRLEN];
        char peer_buf[INET_ADDRSTRLEN];
        printf("[ub_chat] iface=%s ifindex=%u local=%s peer=%s\n",
               ifname, ifindex,
               inet_ntop(AF_INET, &local_addr, local_buf, sizeof(local_buf)),
               inet_ntop(AF_INET, &peer_addr, peer_buf, sizeof(peer_buf)));
    }

    memset(&peer_sockaddr, 0, sizeof(peer_sockaddr));
    peer_sockaddr.sin_family = AF_INET;
    peer_sockaddr.sin_port = htons(shared_socket ? SYNC_PORT : CHAT_PORT);
    peer_sockaddr.sin_addr = peer_addr;

    sync_fd = create_sync_socket(ifname);
    if (sync_fd < 0) {
        return 1;
    }

    if (shared_socket) {
        chat_fd = sync_fd;
        close_chat_fd = false;
        fprintf(stderr, "[ub_chat] using shared sync/chat socket on port=%d\n", SYNC_PORT);
    } else {
        chat_fd = create_chat_socket(ifname);
        if (chat_fd < 0) {
            close(sync_fd);
            return 1;
        }
    }

    if (role_is_initiator(role)) {
        result = run_initiator(chat_fd, sync_fd, &peer_sockaddr, role, my_ip, peer_ip, timeout_s);
    } else {
        result = run_responder(chat_fd, sync_fd, &peer_sockaddr, role, my_ip, peer_ip, timeout_s);
    }

    close(sync_fd);
    if (close_chat_fd) {
        close(chat_fd);
    }
    return result;
}
