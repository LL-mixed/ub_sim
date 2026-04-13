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
#define CHAT_REQ_TEXT   "greeting from NodeA"
#define CHAT_RSP_TEXT   "copy, greeting back from NodeB"

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

static int do_startup_sync_nodeA(int sync_fd,
                                 const struct sockaddr_in *peer_addr)
{
    long deadline = now_ms() + (long)(RUN_TIMEOUT_S * 1000L);
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
                printf("[ub_chat] startup sync complete (nodeA)\n");
                return 0;
            }
        }

        usleep(200000);
    }

    fprintf(stderr, "[ub_chat] fail: startup sync timeout (nodeA)\n");
    return -1;
}

static int do_startup_sync_nodeB(int sync_fd)
{
    long deadline = now_ms() + (long)(RUN_TIMEOUT_S * 1000L);

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
                printf("[ub_chat] startup sync complete (nodeB)\n");
                return 0;
            }
        }

        usleep(200000);
    }

    fprintf(stderr, "[ub_chat] fail: startup sync timeout (nodeB)\n");
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

static int wait_for_peer_chat(int sockfd, const char *local_role,
                              char *peer_text, size_t peer_text_len,
                              int *peer_seq, long *peer_ts_ms)
{
    long deadline = now_ms() + (long)(RUN_TIMEOUT_S * 1000L);

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
            char *payload = saved;

            memcpy(saved, buf, (size_t)n + 1);
            if (n >= 7 && strncmp(saved, "CHAT:", 5) != 0 &&
                strncmp(saved + 2, "CHAT:", 5) == 0) {
                payload = saved + 2;
            }

            p = payload;
            field = strsep(&p, ":");
            if (!field || strcmp(field, "CHAT") != 0) {
                continue;
            }
            fields++;

            field = strsep(&p, ":");
            if (!field) {
                continue;
            }
            snprintf(peer_role, sizeof(peer_role), "%s", field);
            fields++;

            if (strcmp(peer_role, local_role) == 0) {
                continue;
            }

            field = strsep(&p, ":");
            if (!field) {
                continue;
            }
            snprintf(peer_text, peer_text_len, "%s", field);
            fields++;

            field = strsep(&p, ":");
            if (!field) {
                continue;
            }
            *peer_seq = atoi(field);
            fields++;

            field = strsep(&p, ":");
            if (!field) {
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

static int run_nodeA(int chat_fd, int sync_fd,
                     const struct sockaddr_in *peer_addr,
                     const char *role)
{
    int i;
    unsigned int tx_count = 0;
    unsigned int rx_count = 0;
    long total_latency = 0;
    long min_latency = LONG_MAX;
    long max_latency = LONG_MIN;

    if (do_startup_sync_nodeA(sync_fd, peer_addr) != 0) {
        return 1;
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
        sn = sendto(chat_fd, tx, strlen(tx), MSG_DONTWAIT,
                    (const struct sockaddr *)peer_addr, sizeof(*peer_addr));
        if (sn < 0) {
            fprintf(stderr, "[ub_chat] fail: send seq %d: %s\n", i, strerror(errno));
            return 1;
        }
        tx_count++;

        if (wait_for_peer_chat(chat_fd, role, peer_text, sizeof(peer_text),
                               &peer_seq, &peer_ts) != 0) {
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

static int run_nodeB(int chat_fd, int sync_fd,
                     const struct sockaddr_in *peer_addr,
                     const char *role)
{
    unsigned int tx_count = 0;
    unsigned int rx_count = 0;
    long total_latency = 0;
    long min_latency = LONG_MAX;
    long max_latency = LONG_MIN;
    int rounds = 0;

    if (do_startup_sync_nodeB(sync_fd) != 0) {
        return 1;
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
                               &peer_seq, &peer_ts) != 0) {
            fprintf(stderr, "[ub_chat] fail: nodeB timeout waiting msg\n");
            return 1;
        }
        if (peer_seq != rounds) {
            fprintf(stderr, "[ub_chat] fail: nodeB req seq mismatch got=%d expect=%d\n",
                    peer_seq, rounds);
            return 1;
        }
        if (strcmp(peer_text, CHAT_REQ_TEXT) != 0) {
            fprintf(stderr, "[ub_chat] fail: nodeB req payload mismatch got=\"%s\" expect=\"%s\"\n",
                    peer_text, CHAT_REQ_TEXT);
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

        sn = sendto(chat_fd, tx, strlen(tx), MSG_DONTWAIT,
                    (const struct sockaddr *)peer_addr, sizeof(*peer_addr));
        if (sn < 0) {
            fprintf(stderr, "[ub_chat] fail: nodeB send reply seq %d: %s\n",
                    peer_seq, strerror(errno));
            return 1;
        }
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
    char ifname[IFNAMSIZ] = {0};
    struct in_addr local_addr = {0};
    struct in_addr peer_addr = {0};
    struct sockaddr_in peer_sockaddr;
    unsigned int ifindex = 0;
    const char *my_ip;
    const char *peer_ip;
    int chat_fd = -1;
    int sync_fd = -1;
    int result;
    struct sigaction sa;

    if (!cmdline_get_value("linqu_urma_dp_role", role, sizeof(role))) {
        fprintf(stderr, "[ub_chat] fail: missing linqu_urma_dp_role in cmdline\n");
        return 1;
    }

    printf("[ub_chat] start role=%s\n", role);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm(RUN_TIMEOUT_S);

    if (!wait_iface_ready(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, "[ub_chat] fail: ipourma iface not ready\n");
        return 1;
    }

    if (strcmp(role, "nodeA") == 0) {
        my_ip = "10.0.0.1";
        peer_ip = "10.0.0.2";
    } else if (strcmp(role, "nodeB") == 0) {
        my_ip = "10.0.0.2";
        peer_ip = "10.0.0.1";
    } else {
        fprintf(stderr, "[ub_chat] fail: unknown role '%s'\n", role);
        return 1;
    }

    if (!set_ipv4_addr(ifname, my_ip)) {
        fprintf(stderr, "[ub_chat] fail: set ipv4 %s on %s failed\n", my_ip, ifname);
        return 1;
    }

    if (!get_local_ipv4(ifname, &local_addr)) {
        fprintf(stderr, "[ub_chat] fail: get ipv4 addr on %s failed\n", ifname);
        return 1;
    }

    inet_pton(AF_INET, peer_ip, &peer_addr);
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
    peer_sockaddr.sin_port = htons(CHAT_PORT);
    peer_sockaddr.sin_addr = peer_addr;

    chat_fd = create_chat_socket(ifname);
    if (chat_fd < 0) {
        return 1;
    }

    sync_fd = create_sync_socket(ifname);
    if (sync_fd < 0) {
        close(chat_fd);
        return 1;
    }

    if (strcmp(role, "nodeA") == 0) {
        result = run_nodeA(chat_fd, sync_fd, &peer_sockaddr, role);
    } else {
        result = run_nodeB(chat_fd, sync_fd, &peer_sockaddr, role);
    }

    close(sync_fd);
    close(chat_fd);
    return result;
}
